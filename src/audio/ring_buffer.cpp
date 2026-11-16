#include "sampaudiostream/audio/ring_buffer.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace sampaudiostream {

AudioStreamBuffer::AudioStreamBuffer(std::size_t capacity_bytes)
    : capacity_(capacity_bytes > 0 ? capacity_bytes : 1024 * 1024),
      buffer_(capacity_) {}

void AudioStreamBuffer::Write(const std::uint8_t* data, std::size_t size) {
    if (!data || size == 0) return;

    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) return;

    // If a single write exceeds capacity, keep only the trailing capacity_ bytes
    if (size >= capacity_) {
        const std::uint8_t* tail = data + (size - capacity_);
        std::memcpy(buffer_.data(), tail, capacity_);
        total_written_ += size;
        cv_data_.notify_all();
        return;
    }

    std::size_t write_pos = static_cast<std::size_t>(total_written_ % capacity_);
    std::size_t first_chunk = std::min(size, capacity_ - write_pos);
    std::memcpy(buffer_.data() + write_pos, data, first_chunk);

    if (size > first_chunk) {
        std::memcpy(buffer_.data(), data + first_chunk, size - first_chunk);
    }

    total_written_ += size;
    cv_data_.notify_all();
}

void AudioStreamBuffer::MarkEof() {
    std::lock_guard<std::mutex> lock(mutex_);
    eof_ = true;
    cv_data_.notify_all();
}

void AudioStreamBuffer::Close() {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    eof_ = true;
    cv_data_.notify_all();
}

void AudioStreamBuffer::Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    total_written_ = 0;
    eof_ = false;
    closed_ = false;
}

std::size_t AudioStreamBuffer::Read(std::uint64_t& stream_offset, std::uint8_t* out_data, std::size_t max_bytes, int timeout_ms) {
    if (!out_data || max_bytes == 0) return 0;

    std::unique_lock<std::mutex> lock(mutex_);
    if (stream_offset >= total_written_ && !eof_ && !closed_) {
        if (timeout_ms > 0) {
            cv_data_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this, stream_offset]() {
                return total_written_ > stream_offset || eof_ || closed_;
            });
        }
    }

    if (closed_ && stream_offset >= total_written_) {
        return 0;
    }

    std::uint64_t earliest = (total_written_ > capacity_) ? (total_written_ - capacity_) : 0;
    if (stream_offset < earliest) {
        stream_offset = earliest;
    }

    if (stream_offset >= total_written_) {
        return 0;
    }

    std::uint64_t available = total_written_ - stream_offset;
    std::size_t to_read = static_cast<std::size_t>(std::min<std::uint64_t>(available, max_bytes));

    std::size_t read_pos = static_cast<std::size_t>(stream_offset % capacity_);
    std::size_t first_chunk = std::min(to_read, capacity_ - read_pos);
    std::memcpy(out_data, buffer_.data() + read_pos, first_chunk);

    if (to_read > first_chunk) {
        std::memcpy(out_data + first_chunk, buffer_.data(), to_read - first_chunk);
    }

    stream_offset += to_read;
    return to_read;
}

std::uint64_t AudioStreamBuffer::TotalWritten() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return total_written_;
}

std::uint64_t AudioStreamBuffer::EarliestAvailableOffset() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return (total_written_ > capacity_) ? (total_written_ - capacity_) : 0;
}

bool AudioStreamBuffer::IsEof() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return eof_;
}

bool AudioStreamBuffer::IsClosed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
}

} // namespace sampaudiostream
