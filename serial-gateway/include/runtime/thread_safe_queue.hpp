#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>

namespace sg {

template <typename T>
class ThreadSafeQueue {
public:
    explicit ThreadSafeQueue(std::size_t capacity) : capacity_(capacity) {}

    bool push(T item) {
        std::unique_lock<std::mutex> lock(mutex_);
        cond_not_full_.wait(lock, [this]() { return stopped_ || queue_.size() < capacity_; });
        if (stopped_) {
            return false;
        }
        queue_.push_back(std::move(item));
        cond_not_empty_.notify_one();
        return true;
    }

    bool pop(T& out, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!cond_not_empty_.wait_for(lock, timeout, [this]() { return stopped_ || !queue_.empty(); })) {
            return false;
        }
        if (queue_.empty()) {
            return false;
        }
        out = std::move(queue_.front());
        queue_.pop_front();
        cond_not_full_.notify_one();
        return true;
    }

    void stop() {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;
        cond_not_empty_.notify_all();
        cond_not_full_.notify_all();
    }

private:
    std::size_t capacity_;
    std::deque<T> queue_;
    std::mutex mutex_;
    std::condition_variable cond_not_empty_;
    std::condition_variable cond_not_full_;
    bool stopped_ = false;
};

} // namespace sg
