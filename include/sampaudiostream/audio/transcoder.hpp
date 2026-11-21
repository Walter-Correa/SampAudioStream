#pragma once

#include "sampaudiostream/audio/ring_buffer.hpp"
#include "sampaudiostream/core/types.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sampaudiostream {

struct TranscoderConfig {
    using ReResolveCallback = std::function<bool(
        const TrackMetadata& failed_meta,
        const std::atomic<bool>& cancel_flag,
        TrackMetadata& out_meta,
        std::string& out_error)>;

    std::uint32_t target_sample_rate{48000};
    std::uint32_t target_bitrate_kbps{128};
    std::uint32_t initial_burst_ms{6000}; // Initial pre-buffer lead (~5-10s)
    std::uint64_t buffer_generation{0};   // Expected AudioStreamBuffer track generation (0 = any)
    bool pace_realtime{true};             // Emit at ~1.0x realtime for finite tracks and channels
    bool emit_silence_on_pause{false};    // True on shared radio channels, false on personal pause
    bool normalize_volume{false};         // Optional sliding-RMS AGC + peak limiter before int16 encoding
    std::uint32_t hls_poll_interval_ms{2000};
    std::uint32_t hls_live_stall_timeout_ms{30000};
    std::uint32_t max_prefetch_mb{16};
    std::uint32_t no_audio_timeout_ms{20000};
    std::uint32_t stream_connect_timeout_ms{15000};
    std::uint32_t retry_backoff_first_ms{500};
    std::uint32_t retry_backoff_second_ms{1500};
    std::uint64_t start_offset_ms{0};
    std::string base_dir;
    ReResolveCallback re_resolve_callback;
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

// Probes a local audio file's duration in seconds using the plugin's built-in container/codec parsers.
bool ProbeLocalAudioFileDurationSeconds(
    const std::filesystem::path& file_path,
    std::int32_t& out_duration_seconds);

// Feeds synthetic PCM chunks directly through Mp3OutputPipeline for deterministic unit tests.
bool RunNormalizedPipelineForTesting(
    const TranscoderConfig& config,
    float volume_percent,
    int sample_rate,
    int channels,
    const std::vector<std::vector<std::int16_t>>& chunks,
    std::vector<std::uint8_t>& out_mp3_bytes,
    std::vector<std::int16_t>* out_pre_encode_pcm = nullptr,
    float* out_final_gain_db = nullptr);

} // namespace sampaudiostream
