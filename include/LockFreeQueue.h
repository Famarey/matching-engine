#pragma once

#include <atomic>
#include <vector>
#include <cstddef>
#include "Order.h"

namespace matching_engine {

template<typename T, size_t Capacity>
class LockFreeQueue {
public:
    LockFreeQueue() : head_(0), tail_(0) {
        buffer_.resize(Capacity);
    }

    bool push(const T& value) {
        size_t tail = tail_.load(std::memory_order_relaxed);
        size_t next_tail = (tail + 1) % Capacity;

        if (next_tail == head_.load(std::memory_order_acquire)) {
            return false;
        }

        buffer_[tail] = value;
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }

    bool pop(T& value) {
        size_t head = head_.load(std::memory_order_relaxed);

        if (head == tail_.load(std::memory_order_acquire)) {
            return false;
        }

        value = buffer_[head];
        head_.store((head + 1) % Capacity, std::memory_order_release);
        return true;
    }

    bool empty() const {
        return head_.load(std::memory_order_acquire) == 
               tail_.load(std::memory_order_acquire);
    }

    size_t size() const {
        size_t head = head_.load(std::memory_order_acquire);
        size_t tail = tail_.load(std::memory_order_acquire);
        return (tail >= head) ? (tail - head) : (Capacity - head + tail);
    }

private:
    alignas(64) std::atomic<size_t> head_;
    alignas(64) std::atomic<size_t> tail_;
    std::vector<T> buffer_;
};

using OrderQueue = LockFreeQueue<Order, 65536>;

}
