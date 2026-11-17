#pragma once

#include "sampaudiostream/audio/ring_buffer.hpp"
#include "sampaudiostream/core/types.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace sampaudiostream {

struct TranscoderConfig {
    std::uint32_t target_sample_rate{48000};
    std::uint32_t target_bitrate_kbps{128};
    std::uint32_t initial_burst_ms{6000}; // Initial pre-buffer lead (~5-10s)
    std::uint64_t buffer_generation{0};   // Expected AudioStreamBuffer track generation (0 = any)
    bool pace_realtime{true};             // Emit at ~1.0x realtime for finite tracks and channels
    bool emit_silence_on_pause{false};    // True on shared radio channels, false on personal pause
    std::uint32_t hls_poll_interval_ms{2000};
    std::uint32_t hls_live_stall_timeout_ms{30000};
    std::uint32_t max_prefetch_mb{16};
    std::uint32_t no_audio_timeout_ms{20000};
};

class AudioTranscoder {
public:
    AudioTranscoder(
        const TrackMetadata& metadata,
        const TranscoderConfig& config,
        std::shared_ptr<AudioStreamBuffer> output_buffer);
    ~AudioTranscoder();

    AudioTranscoder(const AudioTranscoder&) = delete;
    AudioTranscoder& operator=(const AudioTranscoder&) = delete;

    // Runs synchronously on a worker thread until track finishes, errors, or cancel_flag is set
    bool Run(
        std::atomic<bool>& cancel_flag,
        std::atomic<bool>& pause_flag,
        std::atomic<float>& volume_percent,
        std::string& out_error,
        ErrorCode* out_error_code = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace sampaudiostream
