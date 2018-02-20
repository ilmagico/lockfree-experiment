#include <iostream>
#include <sstream>
#include <vector>
#include <random>
#include <atomic>
#include <thread>
#include <mutex>
#include <chrono>
#include <condition_variable>
#include <cassert>

#include <boost/lockfree/queue.hpp>

#include "lockfree_ptr_queue.hpp"


//#define USE_LOCKFREE
#define THROTTLE_CONSUMER 100

// High contention scenario
//static constexpr int NUM_THREADS = 500;
//static constexpr int NUM_PRODUCES = 20000;
// Low contention
static constexpr int NUM_THREADS = 50;
static constexpr int NUM_PRODUCES = 200000;

struct Payload
{
    int stuff[4];
};

template <typename Gen> Payload make_payload(Gen&& g)
{
    Payload ret;
    for (auto& x : ret.stuff) {
        x = g();
    }
    return ret;
}


//#define DBG_TRACE

// Taken from: https://stackoverflow.com/a/44886145
// replaced slow divide loop with modulo operator
template<std::intmax_t resolution>
std::ostream &operator<<(std::ostream &stream,
        const std::chrono::duration<std::intmax_t,
            std::ratio<std::intmax_t(1), resolution>> &duration)
{
    const std::intmax_t ticks = duration.count();
    stream << (ticks / resolution) << '.' << (ticks % resolution);
    return stream;
}

template<typename Clock, typename Duration>
std::ostream &operator<<(
    std::ostream &stream,
    const std::chrono::time_point<Clock, Duration> &timepoint)
{
    typename Duration::duration ago = timepoint.time_since_epoch();
    return stream << ago;
}


#ifdef DBG_TRACE

#include <boost/variant.hpp>

class TraceEventBase
{
public:
    typedef std::chrono::high_resolution_clock clock;
    clock::time_point timestamp;
    TraceEventBase() : timestamp(clock::now()) { }
};

template <typename OStream, typename TraceEv>
auto operator<<(OStream& os, const TraceEv& ev)
    -> decltype(os << ev.message())
{
    return os << ev.message();
}

class MessageEvent : public TraceEventBase {
    const char* m_msg;
public:
    MessageEvent(const char* msg) : m_msg(msg) { }
    const char* message() const {
        return m_msg;
    }
};

// Just like MessageEvent, but also prints the message now
class MessageNowEvent : public MessageEvent {
public:
    MessageNowEvent(const char* msg)
        : MessageEvent(msg)
    {
        std::cerr << *this << std::endl;
    }
};


template <typename OStream>
OStream& print_all(OStream& os) { return os; }

template <typename OStream, typename FirstArg, typename... Args>
OStream& print_all(OStream& os, FirstArg&& arg1, Args&&... args)
{
    os << std::forward<FirstArg>(arg1);
    return print_all(os, args...);
}


class MessageFmtEvent : public TraceEventBase
{
    std::string m_msg;
public:
    template <typename... Args>
    MessageFmtEvent(Args&&... args) {
        std::ostringstream os;
        print_all(os, args...);
        m_msg = os.str();
    }
    const std::string message() const {
        return m_msg;
    }
};


// Just like MessageEvent, but also prints the message now
class MessageFmtNowEvent : public MessageFmtEvent {
public:
    template <typename... Args>
    MessageFmtNowEvent(Args&&... args) : MessageFmtEvent(args...)
    {
        std::cerr << *this << std::endl;
    }
};


class EmptyQueueEvent : public TraceEventBase
{
public:
    int npopped;
    EmptyQueueEvent(int npop) : npopped(npop) { }
    std::string message() const {
        std::ostringstream os;
        os << *this;
        return os.str();
    }
    template <typename OStream>
    friend OStream& operator<<(OStream& os, EmptyQueueEvent& ev) {
        return os << "popped " << ev.npopped << " elements";
    }
};

typedef boost::variant<
            MessageEvent,
            MessageNowEvent,
            MessageFmtEvent,
            MessageFmtNowEvent,
            EmptyQueueEvent
        >
        event_t;

LockfreePtrQueue<event_t> traceq;

template <typename Event = MessageEvent, typename... Args>
void trace(Args&&... args)
{
    traceq.push(Event(args...));
}

class trace_get_message : public boost::static_visitor<>
{
public:
    typedef std::string result_type;

    template <typename T>
    result_type operator()(T& event) const
    {
        return event.message();
    }
};

class trace_get_timestamp : public boost::static_visitor<>
{
public:
    typedef TraceEventBase::clock::time_point result_type;

    template <typename T>
    result_type operator()(T& event) const
    {
        return event.timestamp;
    }
};

class trace_print : public boost::static_visitor<>
{
public:
    typedef TraceEventBase::clock::time_point time_point;
    time_point m_start;

    trace_print(time_point start) : m_start(start) { }

    template <typename T>
    void operator()(T& event) const
    {
        std::cout << (event.timestamp - m_start) << ": " << event << std::endl;
    }
};

void dump_trace()
{
    using clock = TraceEventBase::clock;
    boost::optional<clock::time_point> start_ts;
    while (auto ev = traceq.pop()) {
        clock::time_point ts = boost::apply_visitor(trace_get_timestamp(), *ev);
        if (!start_ts) {
            // use first timestamp as start time
            // TODO: print full starting time as YY:mm:DD MM:HH:SS or something
            start_ts = ts;
        }
        boost::apply_visitor(trace_print(*start_ts), *ev);
    }
}

#else

// stub out whole hierarchy, so types still exists but do nothing
class TraceEventBase { };
class MessageEvent : public TraceEventBase { };
class MessageNowEvent : public MessageEvent { };
class MessageFmtEvent : public TraceEventBase { };
class MessageFmtNowEvent : public MessageFmtEvent { };
class EmptyQueueEvent : public TraceEventBase { };

template <typename Event, typename... Args>
void trace(Args&&...) { }
void dump_trace() { }

#endif

#define TRACE(EventType, ...) trace<EventType##Event>(__VA_ARGS__)


#ifdef USE_LOCKFREE

class WaitFlag
{
    std::atomic<bool> m_flag;
 
    // we're wait-free and lock free, unless we really have to wait
    // in that case, we need a mutex and condition variable
    std::mutex m_mtx;
    std::condition_variable m_event;
public:
    WaitFlag(bool value = false) : m_flag(value) { }

    // wait until 
    void wait()
    {
        if (!m_flag.load(std::memory_order_acquire)) {
            std::unique_lock<std::mutex> lock(m_mtx);
            TRACE(Message, "sleeping");
            while (!m_flag.load(std::memory_order_acquire)) {
                TRACE(Message, "zzzz");
                m_event.wait(lock);
                TRACE(Message, "yawn");
            };
            TRACE(Message, "woke up");
        }
        //TRACE(MessageFmt, "C: woke up! wakeup_flag=", wakeup_flag);
        m_flag.store(false, std::memory_order_release);
        TRACE(Message, "reset flag");
    }

    void set()
    {
        bool flag = false;
        //TRACE(MessageFmt, "wakeup_flag = ", (int)wakeup_flag);
        if (m_flag.compare_exchange_strong(flag, true)) {
            TRACE(Message, "Set wakeup flag");
            // we just changed it from false to true, notify thread
            // via condition variable as well
            {
                std::unique_lock<std::mutex> lock(m_mtx);
                // do nothing. really. but we need to acquire the lock
                // even for an infinitesimal time to avoid a race condition
            }
            TRACE(Message, "waking consumer up!");
            m_event.notify_one();
        }
    }
};


template <typename T>
class Queue
{
    boost::lockfree::queue<T> m_q;
    WaitFlag m_flag;

public:
    Queue() : m_q(0) { }

    bool push(const T& x) {
        bool ret = m_q.push(x);
        m_flag.set();
        return ret;
    }

    bool pop(T& x) {
        return m_q.pop(x);
    }

    bool empty() {
        return m_q.empty();
    }

    void wait() {
        m_flag.wait();
    }

    void wakeup() {
        m_flag.set();
    }

};

#else

#include <queue>

// plain old queue + mutex, for comparison
template <typename T>
class Queue
{
    std::mutex m_mtx;
    std::condition_variable m_event;
    std::queue<T> m_q;
    bool m_flag;

public:

    bool push(const T& x) {
        {
            std::unique_lock<std::mutex> lock(m_mtx);
            m_q.push(x);
            if (!m_flag) {
                m_flag = true;
                lock.unlock(); // unlock as early as possible
                TRACE(Message, "set flag, notify after push");
                m_event.notify_one();
            }
        }
        return true;
    }

    bool pop(T& x) {
        std::unique_lock<std::mutex> lock(m_mtx);
        if (!m_q.empty()) {
            x = m_q.front();
            m_q.pop();
            return true;
        }
        return false;
    }

    bool empty() {
        std::unique_lock<std::mutex> lock(m_mtx);
        return m_q.empty();
    }

    void wait() {
        TRACE(Message, "sleeping");
        {
            std::unique_lock<std::mutex> lock(m_mtx);
            while (!m_flag) {
                TRACE(Message, "zzzz");
                m_event.wait(lock);
                TRACE(Message, "yawn");
            }
            m_flag = false;
        }
        TRACE(Message, "woke up");
    }

    void wakeup() {
        std::unique_lock<std::mutex> lock(m_mtx);
        if (!m_flag) {
            m_flag = true;
            lock.unlock(); // unlock as early as possible
            TRACE(Message, "waking consumer up!");
            m_event.notify_one();
        }
    }
};

#endif


#include <signal.h>
#include <stdlib.h>
#include <stdio.h>

std::atomic<bool> quit;

void exit_handler(int s){
    //printf("Caught signal %d\n", s);
    TRACE(MessageFmtNow, "Caught signal ", s);
    quit = true;
}

int main(void)
{
    struct sigaction sigact;

    sigact.sa_handler = exit_handler;
    sigemptyset(&sigact.sa_mask);
    sigact.sa_flags = SA_RESETHAND;

    sigaction(SIGINT, &sigact, NULL);
    sigaction(SIGTERM, &sigact, NULL);

    Queue<Payload> q;

    auto start_ts = std::chrono::steady_clock::now();

    std::thread consumer([&q]() {
        // 'tis the consumer thread
        int cnt = 0;
        while (!quit) {
            // wait for data
            TRACE(EmptyQueue, cnt);
            #ifdef THROTTLE_CONSUMER
            auto interval = std::chrono::milliseconds(THROTTLE_CONSUMER);
            std::this_thread::sleep_for(interval);
            #endif
            q.wait();
            cnt = 0;
            Payload elem;
            while (q.pop(elem) && !quit) {
                TRACE(Message, "<- pop elem");
                cnt++;
           }
        }
    });

    std::vector<std::thread> producers;
    for (int i = 0; i < NUM_THREADS; i++) {
        producers.emplace_back([&q] () {
            for (int k = 0; !quit && k < NUM_PRODUCES; k++) {
            //while (!quit) {
                std::random_device r;
                std::default_random_engine rnd(r());
                // randomly sleep for some time
                std::uniform_int_distribution<int> dist(1, 100);
                auto interval = std::chrono::nanoseconds(dist(rnd));
                std::this_thread::sleep_for(interval);
                // produce some random data
                std::uniform_int_distribution<int> data_dist(1, 100);
                //int data = data_dist(rnd);
                Payload data = make_payload([&]() { return data_dist(rnd); });
                TRACE(Message, "-> push elem");
                q.push(data);
            }
        });
    }

    for (auto& t : producers) {
        t.join();
    }

    if (!q.empty()) {
        TRACE(MessageNow, "Queue not empty yet, wait a bit");
        //std::cerr << "Queue not empty yet, wait a bit" << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    // reset signals
    sigaction(SIGINT, NULL, NULL);
    sigaction(SIGTERM, NULL, NULL);

    TRACE(Message, "*** Main: QUIT ***");
    quit = true;
    q.wakeup();
    consumer.join();
    auto end_ts = std::chrono::steady_clock::now();
    TRACE(MessageFmtNow, "Total time: ", end_ts - start_ts);

    if (!q.empty()) {
        TRACE(MessageNow, "QUEUE STILL NOT EMPTY! CONSUMER STARVED?");
    }

    dump_trace();
}
