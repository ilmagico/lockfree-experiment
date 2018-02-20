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
#include <queue>

#include <signal.h>

#include <boost/lockfree/queue.hpp>
#include <boost/program_options.hpp>

#include "lockfree_ptr_queue.hpp"


// This can be used to disable tracing entirely at compile-time,
// although there's little use for this now that tracing is a command
// line option, and timing difference between compile-time and
// run-time disabling is not measurable.
#define DBG_TRACE

// Test Payload to push/pop in the queue
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



// if DBG_TRACE is enabled, it can still be disabled at runtime
static bool trace_enabled;

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
    // if this returns true, it means the constructor will
    // print the message immediately, as well as when it gets popped
    static constexpr bool immediate = false;
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
    static constexpr bool immediate = true;
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
    static constexpr bool immediate = true;
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
    if (trace_enabled) {
        traceq.push(Event(args...));
    } else if (Event::immediate) {
        // constructor has side effects, call it
        Event(args...);
    }
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
class LockfreeQueue
{
    boost::lockfree::queue<T> m_q;
    WaitFlag m_flag;

public:
    LockfreeQueue() : m_q(0) { }

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

// plain old queue + mutex, for comparison
template <typename T>
class LockingQueue
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


std::atomic<bool> quit;

void exit_handler(int s){
    //printf("Caught signal %d\n", s);
    TRACE(MessageFmtNow, "Caught signal ", s);
    quit = true;
}

struct Params {
    int num_threads;
    int num_produces;
    int sleep_us;
    int interval_us;
};

template <template<class> typename Queue>
void run_test(const Params& p)
{
    Queue<Payload> q;

    auto start_ts = std::chrono::steady_clock::now();

    std::thread consumer([p,&q]() {
        // 'tis the consumer thread
        auto interval = std::chrono::microseconds(p.sleep_us);
        int cnt = 0;
        while (!quit) {
            // wait for data
            TRACE(EmptyQueue, cnt);
            if (p.sleep_us > 0) {
                std::this_thread::sleep_for(interval);
            }
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
    for (int i = 0; i < p.num_threads; i++) {
        producers.emplace_back([p,&q] () {
            std::random_device r;
            std::default_random_engine rnd(r());
            std::uniform_int_distribution<int> data_dist(1, 10000);
            std::uniform_int_distribution<int> interval_dist(1, p.interval_us);
            for (int k = 0; !quit && k < p.num_produces; k++) {
                // randomly sleep for some time
                if (p.interval_us > 0) {
                    auto intv = std::chrono::microseconds(interval_dist(rnd));
                    std::this_thread::sleep_for(intv);
                }
                // produce some random data
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
}

int main(int argc, char** argv)
{
    Params p;

    namespace po = boost::program_options;

    std::string help_text = "Usage: lockfree-experiment [options]\n\n";
    po::options_description options_desc("General options");
    options_desc.add_options()
        ("help,h", "Print help")
        ("trace,t", "Enable tracing all operations in a memory buffer, "
         "which is dumped on stdout at the end of the test.")
        ("locking,l", "Disable lockfree queue and use a standard "
         "std::queue + std::mutex instead, for comparison.")
        ("num-treads,n", po::value<int>(&p.num_threads)->default_value(50),
         "How many producer threads to spawn (consumer is only one)")
        ("num-produces,p", po::value<int>(&p.num_produces)->default_value(200000),
         "How many element producers should push to the queue (each)")
        ("sleep,s", po::value<int>(&p.sleep_us)->default_value(0),
         "Microseconds consumer will sleep after emptying the queue "
         "to reduce cpu usage, zero disables sleeping")
        ("interval,i", po::value<int>(&p.interval_us)->default_value(10),
         "Maximum amount of microseconds producer will sleep after pushing "
         "one element to the queue. Value is randomized between 0 and this.")
        ;

    po::variables_map opts;
    po::store(po::command_line_parser(argc, argv)
            .options(options_desc).run(), opts);

    if (opts.count("help")) {
        std::cout << help_text << options_desc << "\n";
        return 0;
    }
    po::notify(opts);

    if (opts.count("trace") > 0) {
        trace_enabled = true;
    } else {
        trace_enabled = false;
    }

    struct sigaction sigact;

    sigact.sa_handler = exit_handler;
    sigemptyset(&sigact.sa_mask);
    sigact.sa_flags = SA_RESETHAND;

    sigaction(SIGINT, &sigact, NULL);
    sigaction(SIGTERM, &sigact, NULL);

    if (opts.count("locking") > 0) {
        TRACE(MessageNow, "Using Locking queue");
        run_test<LockingQueue>(p);
    } else {
        TRACE(MessageNow, "Using Lock-free queue");
        run_test<LockfreeQueue>(p);
    }

    dump_trace();

    return 0;
}
