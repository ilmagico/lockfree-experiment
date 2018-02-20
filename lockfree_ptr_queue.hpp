#pragma once
#include <boost/optional.hpp>
#include <boost/lockfree/queue.hpp>
#include <memory>

template <typename T>
class LockfreePtrQueue
{    
    boost::lockfree::queue<T*> m_q;

public:
    LockfreePtrQueue(int size = 0) : m_q(size) { }
    ~LockfreePtrQueue() {
        while (pop());
    }

    boost::optional<std::unique_ptr<T>> pop_ptr() {
        T* ptr;
        if (m_q.pop(ptr)) {
            return std::unique_ptr<T>(ptr);
        } else {
            return boost::none;
        }
    }

    bool push_ptr(std::unique_ptr<T> elem) {
        if (m_q.push(elem.get())) {
            elem.release(); // release only if push successful
            return true;
        } else {
            // failed to push, unique_ptr's destructor will destroy the object
            return false;
        }
    }

    // same as pop_ptr(), but returns by value, which involves a move/copy
    boost::optional<T> pop() {
        auto ret = pop_ptr();
        if (ret) {
            return std::move(**ret);
        } else {
            return boost::none;
        }
    }

    // same as push_ptr(), but takes T by value, which involved a move/copy
    bool push(T elem) {
        return push_ptr(std::unique_ptr<T>(new T(std::move(elem))));
    }
};

