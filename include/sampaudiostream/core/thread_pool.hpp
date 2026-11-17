#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <vector>

namespace sampaudiostream {

class ManagedThread {
public:
    ManagedThread();
    ~ManagedThread();

    ManagedThread(const ManagedThread&) = delete;
    ManagedThread& operator=(const ManagedThread&) = delete;
    ManagedThread(ManagedThread&& other) noexcept;
    ManagedThread& operator=(ManagedThread&& other) noexcept;

    bool Start(std::function<void()> fn, std::size_t stack_size_bytes = 0);
    bool Joinable() const;
    void Join();

private:
#ifdef _WIN32
    void* handle_{nullptr};
#else
    bool started_{false};
    unsigned long pthread_handle_{0};
    struct Impl;
    std::unique_ptr<Impl> impl_;
#endif
};

class ThreadPool {
public:
    explicit ThreadPool(std::size_t num_threads = 4, std::size_t stack_size_bytes = 0);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void Start(std::size_t num_threads = 4, std::size_t stack_size_bytes = 0);
    // Discards all queued (unstarted) tasks and joins only workers currently running.
    void Stop();

    bool Post(std::function<void()> task);
    std::size_t PendingTasks() const;

private:
    void WorkerLoop();

    std::vector<ManagedThread> workers_;
    std::queue<std::function<void()>> tasks_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> running_{false};
};

// Owns transcoder threads with bounded concurrency, queued overflow, and deterministic join on shutdown.
class TranscodeScheduler {
public:
    TranscodeScheduler() = default;
    ~TranscodeScheduler();

    TranscodeScheduler(const TranscodeScheduler&) = delete;
    TranscodeScheduler& operator=(const TranscodeScheduler&) = delete;

    void Start(std::size_t max_concurrent = 32, std::size_t stack_size_bytes = 256 * 1024);
    void StopAndJoin();

    // Enqueues a transcode job. Returns false if stopped or if spawning a needed worker thread fails.
    // Optional cancel_flag is kept alive by the job closure while queued or running.
    bool Enqueue(std::function<void()> job, const std::atomic<bool>* cancel_flag = nullptr);
    std::size_t ActiveCount() const;
    std::size_t QueuedCount() const;
    std::size_t LiveCount() const;

private:
    struct QueuedJob {
        std::function<void()> fn;
        const std::atomic<bool>* cancel_flag{nullptr};
    };

    struct ActiveJobInfo {
        std::uint64_t id{0};
        const std::atomic<bool>* cancel_flag{nullptr};
    };

    void WorkerLoop();

    std::size_t max_concurrent_{32};
    std::size_t stack_size_bytes_{256 * 1024};
    std::vector<ManagedThread> workers_;
    std::deque<QueuedJob> jobs_;
    std::vector<ActiveJobInfo> active_jobs_info_;
    std::uint64_t next_active_id_{0};
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> running_{false};
    std::atomic<std::size_t> active_jobs_{0};
    std::size_t idle_workers_{0};
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
