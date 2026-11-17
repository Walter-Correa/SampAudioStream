#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace sampaudiostream {

class AudioStreamBuffer {
public:
    using WaiterId = std::uint64_t;
    using NotifyCallback = std::function<void()>;

    explicit AudioStreamBuffer(std::size_t capacity_bytes = 480 * 1024);

    void Write(const std::uint8_t* data, std::size_t size);
    void Write(const std::uint8_t* data, std::size_t size, std::uint64_t expected_generation);
    void MarkEof();
    void MarkEof(std::uint64_t expected_generation);
    std::uint64_t PrepareNextTrack();
    void Close();

    std::uint64_t CurrentGeneration() const;
    std::uint64_t CurrentGenerationStartOffset() const;

    // Register a one-shot callback invoked when new data, EOF, or close occurs.
    WaiterId RegisterWaiter(NotifyCallback cb);
    bool RemoveWaiter(WaiterId id);

    // Reads up to max_bytes starting from monotonic stream_offset.
    // Returns bytes read (0 on EOF or closed, or updates stream_offset if overwritten).
    // If stream_offset fell behind the sliding window, it snaps forward to the oldest available byte.
    std::size_t Read(std::uint64_t& stream_offset, std::uint8_t* out_data, std::size_t max_bytes, int timeout_ms = 50);

    std::uint64_t TotalWritten() const;
    std::uint64_t EarliestAvailableOffset() const;
    bool IsEof() const;
    bool IsClosed() const;

private:
    std::size_t capacity_;
    std::vector<std::uint8_t> buffer_;
    std::uint64_t total_written_{0};
    std::uint64_t generation_{1};
    std::uint64_t generation_start_offset_{0};
    bool eof_{false};
    bool closed_{false};

    mutable std::mutex mutex_;
    std::condition_variable cv_data_;
    WaiterId next_waiter_id_{1};
    std::unordered_map<WaiterId, NotifyCallback> waiters_;
    void NotifyWaitersLocked(std::vector<NotifyCallback>& out);
};

} // namespace sampaudiostream
