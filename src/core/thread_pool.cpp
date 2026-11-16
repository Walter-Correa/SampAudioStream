#include "sampaudiostream/core/thread_pool.hpp"

#include <algorithm>

namespace sampaudiostream {

ThreadPool::ThreadPool(std::size_t num_threads) {
    if (num_threads > 0) {
        Start(num_threads);
    }
}

ThreadPool::~ThreadPool() {
    Stop();
}

void ThreadPool::Start(std::size_t num_threads) {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;
    }

    const std::size_t count = std::max<std::size_t>(1, num_threads);
    workers_.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        workers_.emplace_back(&ThreadPool::WorkerLoop, this);
    }
}

void ThreadPool::Stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) {
        return;
    }

    cv_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();

    std::lock_guard<std::mutex> lock(mutex_);
    while (!tasks_.empty()) {
        tasks_.pop();
    }
}

void ThreadPool::Post(std::function<void()> task) {
    if (!running_.load()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_.push(std::move(task));
    }
    cv_.notify_one();
}

std::size_t ThreadPool::PendingTasks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tasks_.size();
}

void ThreadPool::WorkerLoop() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() {
                return !running_.load() || !tasks_.empty();
            });

            if (!running_.load() && tasks_.empty()) {
                return;
            }

            task = std::move(tasks_.front());
            tasks_.pop();
        }

        if (task) {
            try {
                task();
            } catch (...) {
                // Prevent worker thread termination on unhandled exceptions
            }
        }
    }
}

MainThreadDispatcher& MainThreadDispatcher::Instance() {
    static MainThreadDispatcher instance;
    return instance;
}

void MainThreadDispatcher::Post(std::function<void()> task) {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push(std::move(task));
}

void MainThreadDispatcher::ProcessPending() {
    std::queue<std::function<void()>> local;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            return;
        }
        std::swap(local, queue_);
    }

    while (!local.empty()) {
        auto task = std::move(local.front());
        local.pop();
        if (task) {
            try {
                task();
            } catch (...) {
                // Keep server tick loop resilient
            }
        }
    }
}

void MainThreadDispatcher::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!queue_.empty()) {
        queue_.pop();
    }
}

} // namespace sampaudiostream
