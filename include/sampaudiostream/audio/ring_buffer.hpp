#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <vector>

namespace sampaudiostream {

class AudioStreamBuffer {
public:
    explicit AudioStreamBuffer(std::size_t capacity_bytes = 2 * 1024 * 1024);

    void Write(const std::uint8_t* data, std::size_t size);
    void MarkEof();
    void Close();
    void Reset();

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
    bool eof_{false};
    bool closed_{false};

    mutable std::mutex mutex_;
    std::condition_variable cv_data_;
};

} // namespace sampaudiostream
