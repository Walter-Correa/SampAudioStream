#include "sampaudiostream/audio/ring_buffer.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace sampaudiostream {

AudioStreamBuffer::AudioStreamBuffer(std::size_t capacity_bytes)
    : capacity_(capacity_bytes > 0 ? capacity_bytes : 480 * 1024),
      buffer_(capacity_) {}

void AudioStreamBuffer::NotifyWaitersLocked(std::vector<NotifyCallback>& out) {
    if (waiters_.empty()) return;
    out.reserve(waiters_.size());
    for (auto& [id, cb] : waiters_) {
        if (cb) out.push_back(std::move(cb));
    }
    waiters_.clear();
}

AudioStreamBuffer::WaiterId AudioStreamBuffer::RegisterWaiter(NotifyCallback cb) {
    if (!cb) return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) {
        return 0;
    }
    WaiterId id = next_waiter_id_++;
    waiters_.emplace(id, std::move(cb));
    return id;
}

bool AudioStreamBuffer::RemoveWaiter(WaiterId id) {
    if (id == 0) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    return waiters_.erase(id) > 0;
}

void AudioStreamBuffer::Write(const std::uint8_t* data, std::size_t size) {
    Write(data, size, 0);
}

void AudioStreamBuffer::Write(const std::uint8_t* data, std::size_t size, std::uint64_t expected_generation) {
    if (!data || size == 0) return;

    std::vector<NotifyCallback> to_notify;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) return;
        if (expected_generation != 0 && expected_generation != generation_) return;

        // If a single write exceeds capacity, keep only the trailing capacity_ bytes
        // placed at the exact modular positions corresponding to [new_total - capacity_, new_total).
        if (size >= capacity_) {
            const std::uint8_t* tail = data + (size - capacity_);
            std::uint64_t new_total = total_written_ + size;
            std::uint64_t earliest = new_total - capacity_;
            std::size_t start_pos = static_cast<std::size_t>(earliest % capacity_);
            std::size_t first_chunk = capacity_ - start_pos;
            std::memcpy(buffer_.data() + start_pos, tail, first_chunk);
            if (first_chunk < capacity_) {
                std::memcpy(buffer_.data(), tail + first_chunk, capacity_ - first_chunk);
            }
            total_written_ = new_total;
        } else {
            std::size_t write_pos = static_cast<std::size_t>(total_written_ % capacity_);
            std::size_t first_chunk = std::min(size, capacity_ - write_pos);
            std::memcpy(buffer_.data() + write_pos, data, first_chunk);

            if (size > first_chunk) {
                std::memcpy(buffer_.data(), data + first_chunk, size - first_chunk);
            }

            total_written_ += size;
        }
        cv_data_.notify_all();
        NotifyWaitersLocked(to_notify);
    }

    for (auto& cb : to_notify) {
        cb();
    }
}

void AudioStreamBuffer::MarkEof() {
    MarkEof(0);
}

void AudioStreamBuffer::MarkEof(std::uint64_t expected_generation) {
    std::vector<NotifyCallback> to_notify;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (expected_generation != 0 && expected_generation != generation_) return;
        eof_ = true;
        cv_data_.notify_all();
        NotifyWaitersLocked(to_notify);
    }
    for (auto& cb : to_notify) {
        cb();
    }
}

std::uint64_t AudioStreamBuffer::PrepareNextTrack() {
    std::uint64_t new_gen = 0;
    std::vector<NotifyCallback> to_notify;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++generation_;
        generation_start_offset_ = total_written_;
        new_gen = generation_;
        if (!closed_) {
            eof_ = false;
        }
        cv_data_.notify_all();
        NotifyWaitersLocked(to_notify);
    }
    for (auto& cb : to_notify) {
        cb();
    }
    return new_gen;
}

std::uint64_t AudioStreamBuffer::CurrentGeneration() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation_;
}

std::uint64_t AudioStreamBuffer::CurrentGenerationStartOffset() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation_start_offset_;
}

void AudioStreamBuffer::Close() {
    std::vector<NotifyCallback> to_notify;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        eof_ = true;
        cv_data_.notify_all();
        NotifyWaitersLocked(to_notify);
    }
    for (auto& cb : to_notify) {
        cb();
    }
}

std::size_t AudioStreamBuffer::CopyRangeLocked(std::uint64_t offset, std::uint8_t* out_data, std::size_t max_bytes) const {
    if (offset >= total_written_ || max_bytes == 0 || !out_data) {
        return 0;
    }
    std::uint64_t available = total_written_ - offset;
    std::size_t to_read = static_cast<std::size_t>(std::min<std::uint64_t>(available, max_bytes));

    std::size_t read_pos = static_cast<std::size_t>(offset % capacity_);
    std::size_t first_chunk = std::min(to_read, capacity_ - read_pos);
    std::memcpy(out_data, buffer_.data() + read_pos, first_chunk);

    if (to_read > first_chunk) {
        std::memcpy(out_data + first_chunk, buffer_.data(), to_read - first_chunk);
    }
    return to_read;
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

    std::size_t to_read = CopyRangeLocked(stream_offset, out_data, max_bytes);
    stream_offset += to_read;
    return to_read;
}

std::size_t AudioStreamBuffer::PeekAt(std::uint64_t stream_offset, std::uint8_t* out_data, std::size_t max_bytes) const {
    if (!out_data || max_bytes == 0) return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    std::uint64_t earliest = (total_written_ > capacity_) ? (total_written_ - capacity_) : 0;
    if (stream_offset < earliest || stream_offset >= total_written_) {
        return 0;
    }
    return CopyRangeLocked(stream_offset, out_data, max_bytes);
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
