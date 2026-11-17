#include "sampaudiostream/core/thread_pool.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <process.h>
#include <windows.h>
#else
#include <limits.h>
#include <pthread.h>
#endif

namespace sampaudiostream {

namespace {

struct ThreadStartContext {
    std::function<void()> fn;
};

#ifdef _WIN32
unsigned __stdcall WinThreadTrampoline(void* arg) {
    auto* ctx = static_cast<ThreadStartContext*>(arg);
    try {
        if (ctx && ctx->fn) {
            ctx->fn();
        }
    } catch (...) {
    }
    delete ctx;
    return 0;
}
#else
void* PosixThreadTrampoline(void* arg) {
    auto* ctx = static_cast<ThreadStartContext*>(arg);
    try {
        if (ctx && ctx->fn) {
            ctx->fn();
        }
    } catch (...) {
    }
    delete ctx;
    return nullptr;
}
#endif

} // namespace

#ifndef _WIN32
struct ManagedThread::Impl {
    pthread_t thread{};
    bool joinable{false};
};
#endif

ManagedThread::ManagedThread() = default;

ManagedThread::~ManagedThread() {
    Join();
}

ManagedThread::ManagedThread(ManagedThread&& other) noexcept {
#ifdef _WIN32
    handle_ = other.handle_;
    other.handle_ = nullptr;
#else
    started_ = other.started_;
    pthread_handle_ = other.pthread_handle_;
    impl_ = std::move(other.impl_);
    other.started_ = false;
#endif
}

ManagedThread& ManagedThread::operator=(ManagedThread&& other) noexcept {
    if (this != &other) {
        Join();
#ifdef _WIN32
        handle_ = other.handle_;
        other.handle_ = nullptr;
#else
        started_ = other.started_;
        pthread_handle_ = other.pthread_handle_;
        impl_ = std::move(other.impl_);
        other.started_ = false;
#endif
    }
    return *this;
}

bool ManagedThread::Start(std::function<void()> fn, std::size_t stack_size_bytes) {
    Join();
    if (!fn) return false;

    auto* ctx = new (std::nothrow) ThreadStartContext{std::move(fn)};
    if (!ctx) return false;

#ifdef _WIN32
    uintptr_t h = _beginthreadex(
        nullptr,
        static_cast<unsigned>(stack_size_bytes),
        &WinThreadTrampoline,
        ctx,
        0,
        nullptr);
    if (h == 0) {
        delete ctx;
        return false;
    }
    handle_ = reinterpret_cast<void*>(h);
    return true;
#else
    if (!impl_) {
        impl_ = std::make_unique<Impl>();
    }
    pthread_attr_t attr;
    bool has_attr = (pthread_attr_init(&attr) == 0);
    if (has_attr && stack_size_bytes > 0) {
#ifdef PTHREAD_STACK_MIN
        std::size_t min_stack = static_cast<std::size_t>(PTHREAD_STACK_MIN);
#else
        std::size_t min_stack = 64 * 1024;
#endif
        std::size_t target = std::max<std::size_t>(stack_size_bytes, min_stack);
        pthread_attr_setstacksize(&attr, target);
    }

    int rc = pthread_create(&impl_->thread, has_attr ? &attr : nullptr, &PosixThreadTrampoline, ctx);
    if (has_attr) {
        pthread_attr_destroy(&attr);
    }
    if (rc != 0) {
        delete ctx;
        impl_->joinable = false;
        started_ = false;
        return false;
    }
    impl_->joinable = true;
    started_ = true;
    return true;
#endif
}

bool ManagedThread::Joinable() const {
#ifdef _WIN32
    return handle_ != nullptr;
#else
    return impl_ && impl_->joinable;
#endif
}

void ManagedThread::Join() {
#ifdef _WIN32
    if (handle_) {
        WaitForSingleObject(static_cast<HANDLE>(handle_), INFINITE);
        CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
    }
#else
    if (impl_ && impl_->joinable) {
        pthread_join(impl_->thread, nullptr);
        impl_->joinable = false;
        started_ = false;
    }
#endif
}

// ---------------- ThreadPool ----------------

ThreadPool::ThreadPool(std::size_t num_threads, std::size_t stack_size_bytes) {
    if (num_threads > 0) {
        Start(num_threads, stack_size_bytes);
    }
}

ThreadPool::~ThreadPool() {
    Stop();
}

void ThreadPool::Start(std::size_t num_threads, std::size_t stack_size_bytes) {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;
    }

    const std::size_t count = std::max<std::size_t>(1, num_threads);
    workers_.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        ManagedThread t;
        if (t.Start([this]() { WorkerLoop(); }, stack_size_bytes)) {
            workers_.push_back(std::move(t));
        }
    }
}

void ThreadPool::Stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!tasks_.empty()) {
            tasks_.pop();
        }
    }

    cv_.notify_all();
    for (auto& worker : workers_) {
        worker.Join();
    }
    workers_.clear();
}

bool ThreadPool::Post(std::function<void()> task) {
    if (!running_.load() || !task) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_.load() || workers_.empty()) {
            return false;
        }
        tasks_.push(std::move(task));
    }
    cv_.notify_one();
    return true;
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

            if (!running_.load()) {
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

// ---------------- TranscodeScheduler ----------------

TranscodeScheduler::~TranscodeScheduler() {
    StopAndJoin();
}

void TranscodeScheduler::Start(std::size_t max_concurrent, std::size_t stack_size_bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    max_concurrent_ = std::max<std::size_t>(1, max_concurrent);
    stack_size_bytes_ = stack_size_bytes;
    running_.store(true);
}

void TranscodeScheduler::StopAndJoin() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) {
        return;
    }

    std::vector<ManagedThread> to_join;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        jobs_.clear();
        to_join = std::move(workers_);
        workers_.clear();
    }

    cv_.notify_all();
    for (auto& w : to_join) {
        w.Join();
    }
}

bool TranscodeScheduler::Enqueue(std::function<void()> job, const std::atomic<bool>* cancel_flag) {
    if (!running_.load() || !job) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_.load()) {
        return false;
    }

    // Spawn an on-demand worker if none are idle and we're below the concurrency limit
    if (idle_workers_ == 0 && workers_.size() < max_concurrent_) {
        ManagedThread t;
        if (!t.Start([this]() { WorkerLoop(); }, stack_size_bytes_)) {
            if (workers_.empty()) {
                return false;
            }
            // Existing workers can still pick it up from the queue
        } else {
            workers_.push_back(std::move(t));
        }
    }

    jobs_.push_back(QueuedJob{std::move(job), cancel_flag});
    cv_.notify_one();
    return true;
}

std::size_t TranscodeScheduler::ActiveCount() const {
    return active_jobs_.load();
}

std::size_t TranscodeScheduler::QueuedCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return jobs_.size();
}

std::size_t TranscodeScheduler::LiveCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t count = 0;
    for (const auto& q : jobs_) {
        if (!q.cancel_flag || !q.cancel_flag->load()) {
            ++count;
        }
    }
    for (const auto& a : active_jobs_info_) {
        if (!a.cancel_flag || !a.cancel_flag->load()) {
            ++count;
        }
    }
    return count;
}

void TranscodeScheduler::WorkerLoop() {
    while (true) {
        QueuedJob entry;
        std::uint64_t active_id = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ++idle_workers_;
            cv_.wait(lock, [this]() {
                return !running_.load() || !jobs_.empty();
            });
            --idle_workers_;

            if (!running_.load()) {
                return;
            }

            entry = std::move(jobs_.front());
            jobs_.pop_front();
            if (entry.fn) {
                active_id = ++next_active_id_;
                active_jobs_info_.push_back(ActiveJobInfo{active_id, entry.cancel_flag});
            }
        }

        if (entry.fn) {
            active_jobs_.fetch_add(1);
            // ActiveCleanup runs before `entry` leaves scope so its captured shared_ptr keeps cancel_flag alive.
            struct ActiveCleanup {
                TranscodeScheduler* self;
                std::uint64_t id;
                ~ActiveCleanup() {
                    std::lock_guard<std::mutex> lock(self->mutex_);
                    auto& v = self->active_jobs_info_;
                    v.erase(std::remove_if(v.begin(), v.end(), [this](const ActiveJobInfo& a) {
                        return a.id == id;
                    }), v.end());
                    self->active_jobs_.fetch_sub(1);
                }
            } cleanup{this, active_id};
            try {
                entry.fn();
            } catch (...) {
            }
        }
    }
}

// ---------------- MainThreadDispatcher ----------------

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
