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
    bool pace_realtime{false}; // True for shared radio channels, false for individual streams
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
    bool Run(std::atomic<bool>& cancel_flag, std::atomic<bool>& pause_flag, std::atomic<float>& volume_percent, std::string& out_error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace sampaudiostream
