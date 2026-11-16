#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace sampaudiostream {

class ThreadPool {
public:
    explicit ThreadPool(std::size_t num_threads = 4);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void Start(std::size_t num_threads = 4);
    void Stop();

    void Post(std::function<void()> task);
    std::size_t PendingTasks() const;

private:
    void WorkerLoop();

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> running_{false};
};

class MainThreadDispatcher {
public:
    static MainThreadDispatcher& Instance();

    void Post(std::function<void()> task);
    void ProcessPending();
    void Clear();

private:
    MainThreadDispatcher() = default;

    std::mutex mutex_;
    std::queue<std::function<void()>> queue_;
};

} // namespace sampaudiostream
