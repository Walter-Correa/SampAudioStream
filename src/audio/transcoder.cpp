#include "sampaudiostream/audio/transcoder.hpp"

#include "sampaudiostream/core/logger.hpp"
#include "sampaudiostream/core/thread_pool.hpp"
#include "sampaudiostream/network/http_client.hpp"

#include <aacdecoder_lib.h>
#include <lame.h>
#include <ogg/ogg.h>
#include <opus.h>
#include <opus_multistream.h>

#define DR_FLAC_IMPLEMENTATION
#include <dr_flac.h>
#define DR_MP3_IMPLEMENTATION
#include <dr_mp3.h>
#define DR_WAV_IMPLEMENTATION
#include <dr_wav.h>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4244 4245 4456 4457 4701)
#endif
#include <stb_vorbis.c>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <vector>

namespace sampaudiostream {

namespace {

std::string ExtractHostOnly(const std::string& url) {
    auto scheme_pos = url.find("://");
    std::size_t host_start = (scheme_pos != std::string::npos) ? (scheme_pos + 3) : 0;
    std::size_t host_end = url.find_first_of("/?#", host_start);
    std::string authority = (host_end == std::string::npos)
                                ? url.substr(host_start)
                                : url.substr(host_start, host_end - host_start);
    auto at_pos = authority.rfind('@');
    if (at_pos != std::string::npos) {
        authority = authority.substr(at_pos + 1);
    }
    if (!authority.empty() && authority.front() == '[') {
        auto close_bracket = authority.find(']');
        if (close_bracket != std::string::npos) {
            return authority.substr(1, close_bracket - 1);
        }
    }
    auto colon_pos = authority.find(':');
    return (colon_pos != std::string::npos) ? authority.substr(0, colon_pos) : authority;
}

std::string SanitizePrintablePreview(const std::string& raw, std::size_t max_bytes = 200) {
    const std::size_t n = std::min(raw.size(), max_bytes);
    std::string out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(raw[i]);
        out.push_back((c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '.');
    }
    return out;
}

// Bounded contiguous circular pull stream fed by HttpClient::StreamGet
class PullByteStream {
public:
    explicit PullByteStream(std::size_t max_buffer_bytes = 512 * 1024, const std::atomic<bool>* cancel_flag = nullptr)
        : max_capacity_(max_buffer_bytes > 0 ? max_buffer_bytes : 512 * 1024),
          ring_(max_capacity_),
          cancel_flag_(cancel_flag) {}

    bool Push(const std::uint8_t* data, std::size_t size, const std::atomic<bool>& cancel_flag) {
        if (!data || size == 0) return true;
        std::size_t offset = 0;
        while (offset < size) {
            std::unique_lock<std::mutex> lock(mutex_);
            while (size_ >= max_capacity_ && !closed_ && !IsCancelled(&cancel_flag)) {
                cv_space_.wait_for(lock, std::chrono::milliseconds(50));
            }
            if (closed_ || IsCancelled(&cancel_flag)) {
                return false;
            }
            std::size_t free_space = max_capacity_ - size_;
            std::size_t chunk = std::min(size - offset, free_space);
            std::size_t tail = (head_ + size_) % max_capacity_;
            std::size_t first = std::min(chunk, max_capacity_ - tail);
            std::memcpy(ring_.data() + tail, data + offset, first);
            if (chunk > first) {
                std::memcpy(ring_.data(), data + offset + first, chunk - first);
            }
            size_ += chunk;
            offset += chunk;
            cv_data_.notify_all();
        }
        return true;
    }

    void MarkEof() {
        std::lock_guard<std::mutex> lock(mutex_);
        eof_ = true;
        cv_data_.notify_all();
        cv_space_.notify_all();
    }

    void Close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        eof_ = true;
        cv_data_.notify_all();
        cv_space_.notify_all();
    }

    std::size_t ReadSome(std::uint8_t* out, std::size_t max_bytes) {
        if (!out || max_bytes == 0) return 0;
        std::unique_lock<std::mutex> lock(mutex_);
        while (size_ == 0 && !eof_ && !closed_ && !IsCancelled(cancel_flag_)) {
            cv_data_.wait_for(lock, std::chrono::milliseconds(50));
        }
        if (size_ == 0 || IsCancelled(cancel_flag_)) {
            return 0;
        }
        std::size_t n = std::min(max_bytes, size_);
        std::size_t first = std::min(n, max_capacity_ - head_);
        std::memcpy(out, ring_.data() + head_, first);
        if (n > first) {
            std::memcpy(out + first, ring_.data(), n - first);
        }
        head_ = (head_ + n) % max_capacity_;
        size_ -= n;
        total_consumed_ += n;
        cv_space_.notify_all();
        return n;
    }

    bool ReadExact(std::uint8_t* out, std::size_t count) {
        std::size_t total = 0;
        while (total < count) {
            std::size_t n = ReadSome(out + total, count - total);
            if (n == 0) return false;
            total += n;
        }
        return true;
    }

    bool Peek(std::vector<std::uint8_t>& out, std::size_t count) {
        std::unique_lock<std::mutex> lock(mutex_);
        while (size_ < count && !eof_ && !closed_ && !IsCancelled(cancel_flag_)) {
            cv_data_.wait_for(lock, std::chrono::milliseconds(50));
        }
        if (IsCancelled(cancel_flag_)) {
            out.clear();
            return false;
        }
        std::size_t avail = std::min(count, size_);
        out.resize(avail);
        if (avail > 0) {
            std::size_t first = std::min(avail, max_capacity_ - head_);
            std::memcpy(out.data(), ring_.data() + head_, first);
            if (avail > first) {
                std::memcpy(out.data() + first, ring_.data(), avail - first);
            }
        }
        return avail > 0;
    }

    bool Skip(std::uint64_t count) {
        std::array<std::uint8_t, 4096> scratch{};
        std::uint64_t remaining = count;
        while (remaining > 0) {
            std::size_t chunk = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, scratch.size()));
            if (!ReadExact(scratch.data(), chunk)) return false;
            remaining -= chunk;
        }
        return true;
    }

    std::uint64_t TotalConsumed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return total_consumed_;
    }

    void EnsureCapacity(std::size_t target_bytes) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (target_bytes <= max_capacity_) return;
        std::vector<std::uint8_t> new_ring(target_bytes);
        if (size_ > 0) {
            std::size_t first = std::min(size_, max_capacity_ - head_);
            std::memcpy(new_ring.data(), ring_.data() + head_, first);
            if (size_ > first) {
                std::memcpy(new_ring.data() + first, ring_.data(), size_ - first);
            }
        }
        ring_.swap(new_ring);
        head_ = 0;
        max_capacity_ = target_bytes;
        cv_space_.notify_all();
    }

    std::size_t Capacity() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return max_capacity_;
    }

    bool IsClosed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

private:
    bool IsCancelled(const std::atomic<bool>* flag) const {
        return (flag && flag->load()) || (cancel_flag_ && cancel_flag_->load());
    }

    std::size_t max_capacity_;
    std::vector<std::uint8_t> ring_;
    std::size_t head_{0};
    std::size_t size_{0};
    const std::atomic<bool>* cancel_flag_{nullptr};
    std::uint64_t total_consumed_{0};
    bool eof_{false};
    bool closed_{false};
    mutable std::mutex mutex_;
    std::condition_variable cv_data_;
    std::condition_variable cv_space_;
};

// Unified LAME MP3 encoder with resampling, volume scaling, pause handling, and optional 1x pacing
class Mp3OutputPipeline {
public:
    Mp3OutputPipeline(
        const TranscoderConfig& cfg,
        std::shared_ptr<AudioStreamBuffer> out_buf,
        std::atomic<bool>& cancel_flag,
        std::atomic<bool>& pause_flag,
        std::atomic<float>& volume_percent,
        std::chrono::steady_clock::time_point run_start,
        std::atomic<std::uint64_t>* external_pcm_frames = nullptr)
        : cfg_(cfg),
          out_buf_(std::move(out_buf)),
          cancel_flag_(cancel_flag),
          pause_flag_(pause_flag),
          volume_percent_(volume_percent),
          skip_remaining_seconds_(static_cast<double>(cfg_.start_offset_ms) / 1000.0),
          run_start_(run_start),
          start_time_(run_start),
          external_pcm_frames_(external_pcm_frames) {}

    ~Mp3OutputPipeline() {
        if (cancel_flag_.load()) {
            CloseWithoutFlush();
        } else {
            FlushAndClose();
        }
    }

    bool Init(int input_sample_rate, int input_channels) {
        int actual_rate = input_sample_rate > 0 ? input_sample_rate : 48000;
        int actual_channels = (input_channels == 1) ? 1 : 2;

        if (lame_ && input_rate_ == actual_rate) {
            return true;
        }

        if (lame_) {
            FlushEncoderOnly();
        } else {
            // Compute constant MP3 output sample rate once at pipeline startup
            int enc_rate = static_cast<int>(cfg_.target_sample_rate);
            if (!cfg_.pace_realtime && (actual_rate == 44100 || actual_rate == 48000 || actual_rate == 32000)) {
                enc_rate = actual_rate;
            }
            output_rate_ = enc_rate;
            start_time_ = std::chrono::steady_clock::now();
        }

        input_rate_ = actual_rate;
        input_channels_ = actual_channels;

        lame_ = lame_init();
        if (!lame_) return false;

        lame_set_in_samplerate(lame_, input_rate_);
        lame_set_out_samplerate(lame_, output_rate_);
        lame_set_num_channels(lame_, 2);
        lame_set_mode(lame_, JOINT_STEREO);
        lame_set_brate(lame_, static_cast<int>(cfg_.target_bitrate_kbps));
        lame_set_quality(lame_, 5); // Fast & clean quality
        lame_set_bWriteVbrTag(lame_, 0);

        if (lame_init_params(lame_) < 0) {
            lame_close(lame_);
            lame_ = nullptr;
            return false;
        }
        return true;
    }

    bool PushPcmInterleaved(const std::int16_t* samples, std::size_t frame_count, int sample_rate, int channels) {
        if (cancel_flag_.load()) return false;
        if (!samples || frame_count == 0 || channels <= 0 || sample_rate <= 0) return true;

        if (skip_remaining_seconds_ > 0.0) {
            const double chunk_seconds = static_cast<double>(frame_count) / static_cast<double>(sample_rate);
            if (chunk_seconds <= skip_remaining_seconds_) {
                skip_remaining_seconds_ -= chunk_seconds;
                decoded_only_frames_ += frame_count;
                if (external_pcm_frames_) {
                    external_pcm_frames_->store(total_pcm_frames_ + decoded_only_frames_, std::memory_order_release);
                }
                return true;
            }
            std::size_t skip_frames = static_cast<std::size_t>(
                std::floor(skip_remaining_seconds_ * static_cast<double>(sample_rate)));
            skip_frames = std::min(skip_frames, frame_count);
            skip_remaining_seconds_ = 0.0;
            decoded_only_frames_ += skip_frames;
            samples += skip_frames * static_cast<std::size_t>(channels);
            frame_count -= skip_frames;
            if (frame_count == 0) {
                if (external_pcm_frames_) {
                    external_pcm_frames_->store(total_pcm_frames_ + decoded_only_frames_, std::memory_order_release);
                }
                return true;
            }
        }

        if (!Init(sample_rate, channels)) return false;

        // Handle pause state
        if (pause_flag_.load() && !cancel_flag_.load()) {
            auto pause_started = std::chrono::steady_clock::now();
            while (pause_flag_.load() && !cancel_flag_.load()) {
                if (cfg_.emit_silence_on_pause && cfg_.pace_realtime) {
                    // Emit 20ms of silence on shared radio channels so listeners don't time out
                    std::size_t sil_frames = static_cast<std::size_t>(std::max(1, input_rate_ / 50));
                    std::vector<std::int16_t> silence(sil_frames * 2, 0);
                    EncodeStereo(silence.data(), sil_frames);
                    PaceOutput(sil_frames, input_rate_);
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(30));
                }
            }
            if (!cfg_.emit_silence_on_pause) {
                start_time_ += (std::chrono::steady_clock::now() - pause_started);
            }
        }
        if (cancel_flag_.load()) return false;

        // Convert mono/stereo/multichannel to stereo and apply volume scaling;
        // LAME's polyphase resampler converts input_rate_ -> output_rate_ internally.
        float vol = std::clamp(volume_percent_.load() / 100.0f, 0.0f, 2.0f);
        stereo_scratch_.resize(frame_count * 2);

        if (!cfg_.normalize_volume) {
            if (channels == 1) {
                for (std::size_t i = 0; i < frame_count; ++i) {
                    std::int16_t s = ScaleSample(samples[i], vol);
                    stereo_scratch_[i * 2] = s;
                    stereo_scratch_[i * 2 + 1] = s;
                }
            } else if (channels == 2) {
                for (std::size_t i = 0; i < frame_count; ++i) {
                    stereo_scratch_[i * 2] = ScaleSample(samples[i * 2], vol);
                    stereo_scratch_[i * 2 + 1] = ScaleSample(samples[i * 2 + 1], vol);
                }
            } else {
                for (std::size_t i = 0; i < frame_count; ++i) {
                    const std::int16_t* f = samples + i * static_cast<std::size_t>(channels);
                    float l = static_cast<float>(f[0]);
                    float r = static_cast<float>(f[1]);
                    if (channels == 3) {
                        l += 0.707f * static_cast<float>(f[1]);
                        r = static_cast<float>(f[2]) + 0.707f * static_cast<float>(f[1]);
                    } else if (channels >= 5) {
                        float fc = 0.707f * static_cast<float>(f[1]);
                        float fr = static_cast<float>(f[2]);
                        float rl = 0.707f * static_cast<float>(f[3]);
                        float rr = 0.707f * static_cast<float>(f[4]);
                        l = static_cast<float>(f[0]) + fc + rl;
                        r = fr + fc + rr;
                        if (channels >= 6) {
                            float lfe = 0.5f * static_cast<float>(f[5]);
                            l += lfe;
                            r += lfe;
                        }
                    } else if (channels == 4) {
                        l += 0.707f * static_cast<float>(f[2]);
                        r += 0.707f * static_cast<float>(f[3]);
                    }
                    stereo_scratch_[i * 2] = static_cast<std::int16_t>(std::clamp(l * vol, -32768.0f, 32767.0f));
                    stereo_scratch_[i * 2 + 1] = static_cast<std::int16_t>(std::clamp(r * vol, -32768.0f, 32767.0f));
                }
            }
        } else {
            ConvertAndNormalizeStereo(samples, frame_count, sample_rate, channels, vol);
        }

        if (capture_stereo_pcm_) {
            capture_stereo_pcm_->insert(capture_stereo_pcm_->end(), stereo_scratch_.begin(), stereo_scratch_.end());
        }

        if (!EncodeStereo(stereo_scratch_.data(), frame_count)) return false;
        total_pcm_frames_ += frame_count;
        if (external_pcm_frames_) {
            external_pcm_frames_->store(total_pcm_frames_ + decoded_only_frames_, std::memory_order_release);
        }
        if (!logged_first_pcm_) {
            logged_first_pcm_ = true;
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - run_start_).count();
            Logger::Instance().Debug(
                "First decoded PCM frame (" + std::to_string(sample_rate) + " Hz, " +
                std::to_string(channels) + " ch, " + std::to_string(frame_count) +
                " frames) at +" + std::to_string(ms) + " ms");
        }
        PaceOutput(frame_count, sample_rate);
        return true;
    }

    std::uint64_t TotalPcmFrames() const { return total_pcm_frames_ + decoded_only_frames_; }
    float CurrentNormGainDb() const { return norm_gain_db_; }
    void SetCaptureStereoPcmForTesting(std::vector<std::int16_t>* capture) { capture_stereo_pcm_ = capture; }

    void FlushAndClose() {
        if (cancel_flag_.load()) {
            CloseWithoutFlush();
        } else {
            FlushEncoderOnly();
        }
    }

    void CloseWithoutFlush() {
        if (!lame_) return;
        lame_close(lame_);
        lame_ = nullptr;
    }

private:
    void FlushEncoderOnly() {
        if (!lame_) return;
        if (!cancel_flag_.load()) {
            std::array<std::uint8_t, 8192> mp3_buf{};
            int flushed = lame_encode_flush(lame_, mp3_buf.data(), static_cast<int>(mp3_buf.size()));
            if (flushed > 0 && out_buf_) {
                out_buf_->Write(mp3_buf.data(), static_cast<std::size_t>(flushed), cfg_.buffer_generation);
            }
        }
        lame_close(lame_);
        lame_ = nullptr;
    }

    static std::int16_t ScaleSample(std::int16_t s, float vol) {
        if (std::abs(vol - 1.0f) < 0.001f) return s;
        float v = static_cast<float>(s) * vol;
        return static_cast<std::int16_t>(std::clamp(v, -32768.0f, 32767.0f));
    }

    void ConvertAndNormalizeStereo(
        const std::int16_t* samples,
        std::size_t frame_count,
        int sample_rate,
        int channels,
        float vol) {
        static constexpr float kNormTargetDb = -16.0f;
        static constexpr float kNormMinGainDb = -12.0f;
        static constexpr float kNormMaxGainDb = 9.0f;
        static constexpr float kNormSilenceDb = -50.0f;
        static constexpr double kNormWindowSeconds = 3.0;
        static constexpr double kNormFastPhaseSeconds = 3.0;
        static constexpr float kNormFastRateDbPerSec = 6.0f;
        static constexpr float kNormSlowRateDbPerSec = 1.0f;
        static constexpr float kLimiterCeiling = 0.98f * (32767.0f / 32768.0f);
        static constexpr float kLimiterReleaseSeconds = 0.200f;
        static constexpr float kInvInt16Scale = 1.0f / 32768.0f;

        const std::size_t sub_block_max = std::max<std::size_t>(1, static_cast<std::size_t>(sample_rate / 50));
        const std::size_t max_window_frames = std::max<std::size_t>(
            1, static_cast<std::size_t>(kNormWindowSeconds * static_cast<double>(sample_rate)));
        const float release_coeff = std::exp(
            -1.0f / (kLimiterReleaseSeconds * static_cast<float>(sample_rate)));

        for (std::size_t offset = 0; offset < frame_count; offset += sub_block_max) {
            const std::size_t sub_frames = std::min(sub_block_max, frame_count - offset);
            float_stereo_scratch_.resize(sub_frames * 2);

            double blk_sum_sq = 0.0;
            if (channels == 1) {
                for (std::size_t i = 0; i < sub_frames; ++i) {
                    const float s = static_cast<float>(samples[offset + i]) * kInvInt16Scale;
                    float_stereo_scratch_[i * 2] = s;
                    float_stereo_scratch_[i * 2 + 1] = s;
                    blk_sum_sq += static_cast<double>(s) * static_cast<double>(s);
                }
            } else if (channels == 2) {
                for (std::size_t i = 0; i < sub_frames; ++i) {
                    const std::size_t idx = (offset + i) * 2;
                    const float l = static_cast<float>(samples[idx]) * kInvInt16Scale;
                    const float r = static_cast<float>(samples[idx + 1]) * kInvInt16Scale;
                    float_stereo_scratch_[i * 2] = l;
                    float_stereo_scratch_[i * 2 + 1] = r;
                    blk_sum_sq += 0.5 * (static_cast<double>(l) * static_cast<double>(l) +
                                         static_cast<double>(r) * static_cast<double>(r));
                }
            } else {
                for (std::size_t i = 0; i < sub_frames; ++i) {
                    const std::int16_t* f = samples + (offset + i) * static_cast<std::size_t>(channels);
                    float l = static_cast<float>(f[0]);
                    float r = static_cast<float>(f[1]);
                    if (channels == 3) {
                        l += 0.707f * static_cast<float>(f[1]);
                        r = static_cast<float>(f[2]) + 0.707f * static_cast<float>(f[1]);
                    } else if (channels >= 5) {
                        float fc = 0.707f * static_cast<float>(f[1]);
                        float fr = static_cast<float>(f[2]);
                        float rl = 0.707f * static_cast<float>(f[3]);
                        float rr = 0.707f * static_cast<float>(f[4]);
                        l = static_cast<float>(f[0]) + fc + rl;
                        r = fr + fc + rr;
                        if (channels >= 6) {
                            float lfe = 0.5f * static_cast<float>(f[5]);
                            l += lfe;
                            r += lfe;
                        }
                    } else if (channels == 4) {
                        l += 0.707f * static_cast<float>(f[2]);
                        r += 0.707f * static_cast<float>(f[3]);
                    }
                    l *= kInvInt16Scale;
                    r *= kInvInt16Scale;
                    float_stereo_scratch_[i * 2] = l;
                    float_stereo_scratch_[i * 2 + 1] = r;
                    blk_sum_sq += 0.5 * (static_cast<double>(l) * static_cast<double>(l) +
                                         static_cast<double>(r) * static_cast<double>(r));
                }
            }

            const double blk_mean_sq = blk_sum_sq / static_cast<double>(sub_frames);
            const float blk_rms_db = (blk_mean_sq > 1e-12)
                ? (10.0f * std::log10(static_cast<float>(blk_mean_sq)))
                : -120.0f;

            norm_blocks_.push_back(RmsBlock{blk_sum_sq, sub_frames});
            norm_window_sum_sq_ += blk_sum_sq;
            norm_window_frames_ += sub_frames;
            while (norm_blocks_.size() > 1 &&
                   (norm_window_frames_ - norm_blocks_.front().frames) >= max_window_frames) {
                norm_window_sum_sq_ -= norm_blocks_.front().sum_sq;
                norm_window_frames_ -= norm_blocks_.front().frames;
                norm_blocks_.pop_front();
            }
            if (norm_window_sum_sq_ < 0.0) {
                norm_window_sum_sq_ = 0.0;
            }

            const double win_mean_sq = (norm_window_frames_ > 0)
                ? (norm_window_sum_sq_ / static_cast<double>(norm_window_frames_))
                : 0.0;
            const float win_rms_db = (win_mean_sq > 1e-12)
                ? (10.0f * std::log10(static_cast<float>(win_mean_sq)))
                : -120.0f;

            const double dt = static_cast<double>(sub_frames) / static_cast<double>(sample_rate);
            if (blk_rms_db >= kNormSilenceDb && win_rms_db >= kNormSilenceDb) {
                const float desired_db = std::clamp(kNormTargetDb - win_rms_db, kNormMinGainDb, kNormMaxGainDb);
                const float delta = desired_db - norm_gain_db_;
                // Drop gain rapidly (6 dB/s) in all phases to prevent loud transitions from pumping;
                // increase gain quickly only during the initial 3 s active-audio phase, then slowly (1 dB/s).
                const float max_rate = (delta < 0.0f)
                    ? kNormFastRateDbPerSec
                    : ((norm_elapsed_seconds_ < kNormFastPhaseSeconds)
                        ? kNormFastRateDbPerSec
                        : kNormSlowRateDbPerSec);
                const float max_step = max_rate * static_cast<float>(dt);
                if (std::abs(delta) <= max_step) {
                    norm_gain_db_ = desired_db;
                } else {
                    norm_gain_db_ += (delta > 0.0f) ? max_step : -max_step;
                }
                norm_elapsed_seconds_ += dt;
            }

            const float combined_gain = std::pow(10.0f, norm_gain_db_ / 20.0f) * vol;
            for (std::size_t i = 0; i < sub_frames; ++i) {
                float l = float_stereo_scratch_[i * 2] * combined_gain;
                float r = float_stereo_scratch_[i * 2 + 1] * combined_gain;
                const float peak = std::max(std::abs(l), std::abs(r));
                const float desired_lim = (peak > kLimiterCeiling && peak > 0.0f)
                    ? (kLimiterCeiling / peak)
                    : 1.0f;
                if (desired_lim < limiter_gain_) {
                    limiter_gain_ = desired_lim;
                } else {
                    limiter_gain_ = 1.0f - (1.0f - limiter_gain_) * release_coeff;
                    if (limiter_gain_ > 0.9999f) {
                        limiter_gain_ = 1.0f;
                    }
                }
                l *= limiter_gain_;
                r *= limiter_gain_;
                const std::size_t out_idx = (offset + i) * 2;
                stereo_scratch_[out_idx] = static_cast<std::int16_t>(
                    std::clamp(std::lrintf(l * 32768.0f), -32768L, 32767L));
                stereo_scratch_[out_idx + 1] = static_cast<std::int16_t>(
                    std::clamp(std::lrintf(r * 32768.0f), -32768L, 32767L));
            }
        }
    }

    bool EncodeStereo(const std::int16_t* interleaved, std::size_t frames) {
        if (frames == 0) return true;
        std::size_t needed = static_cast<std::size_t>(1.25 * static_cast<double>(frames) + 7200);
        if (mp3_out_.size() < needed) {
            mp3_out_.resize(needed);
        }
        int encoded = lame_encode_buffer_interleaved(
            lame_,
            const_cast<short*>(interleaved),
            static_cast<int>(frames),
            mp3_out_.data(),
            static_cast<int>(mp3_out_.size()));
        if (encoded < 0) return false;
        if (encoded > 0 && out_buf_ && !cancel_flag_.load()) {
            out_buf_->Write(mp3_out_.data(), static_cast<std::size_t>(encoded), cfg_.buffer_generation);
            total_encoded_bytes_ += static_cast<std::uint64_t>(encoded);
            if (!logged_first_write_) {
                logged_first_write_ = true;
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - run_start_).count();
                Logger::Instance().Debug(
                    "First ring buffer write (" + std::to_string(encoded) +
                    " bytes) at +" + std::to_string(ms) + " ms");
            }
        }
        return true;
    }

    void PaceOutput(std::size_t emitted_frames, int sample_rate) {
        if (!cfg_.pace_realtime || sample_rate <= 0) return;
        elapsed_audio_seconds_ += static_cast<double>(emitted_frames) / static_cast<double>(sample_rate);
        auto target_elapsed = std::chrono::duration<double>(elapsed_audio_seconds_);
        auto deadline = start_time_ + std::chrono::duration_cast<std::chrono::steady_clock::duration>(target_elapsed);
        auto burst = std::chrono::milliseconds(cfg_.initial_burst_ms > 0 ? cfg_.initial_burst_ms : 6000);
        auto lead_adjusted = deadline - burst;
        while (!cancel_flag_.load()) {
            auto now = std::chrono::steady_clock::now();
            if (now >= lead_adjusted) break;
            auto step = std::min<std::chrono::steady_clock::duration>(
                lead_adjusted - now,
                std::chrono::milliseconds(25));
            std::this_thread::sleep_for(step);
        }
    }

    TranscoderConfig cfg_;
    std::shared_ptr<AudioStreamBuffer> out_buf_;
    std::atomic<bool>& cancel_flag_;
    std::atomic<bool>& pause_flag_;
    std::atomic<float>& volume_percent_;

    lame_global_flags* lame_{nullptr};
    int input_rate_{48000};
    int input_channels_{2};
    int output_rate_{48000};

    std::vector<std::int16_t> stereo_scratch_;
    std::vector<float> float_stereo_scratch_;
    std::vector<std::uint8_t> mp3_out_;
    struct RmsBlock {
        double sum_sq{0.0};
        std::size_t frames{0};
    };
    std::deque<RmsBlock> norm_blocks_;
    double norm_window_sum_sq_{0.0};
    std::size_t norm_window_frames_{0};
    double norm_elapsed_seconds_{0.0};
    float norm_gain_db_{0.0f};
    float limiter_gain_{1.0f};
    std::vector<std::int16_t>* capture_stereo_pcm_{nullptr};
    std::uint64_t total_pcm_frames_{0};
    std::uint64_t decoded_only_frames_{0};
    double skip_remaining_seconds_{0.0};
    std::uint64_t total_encoded_bytes_{0};
    double elapsed_audio_seconds_{0.0};
    std::chrono::steady_clock::time_point run_start_;
    std::chrono::steady_clock::time_point start_time_;
    std::atomic<std::uint64_t>* external_pcm_frames_{nullptr};
    bool logged_first_pcm_{false};
    bool logged_first_write_{false};
};

// ---------------- Format Decoders ----------------

bool DecodeMp3Stream(PullByteStream& input, Mp3OutputPipeline& pipeline, std::atomic<bool>& cancel_flag) {
    struct Ctx {
        PullByteStream* in;
    } ctx{&input};

    auto on_read = [](void* pUserData, void* pBufferOut, size_t bytesToRead) -> size_t {
        auto* c = static_cast<Ctx*>(pUserData);
        return c->in->ReadSome(static_cast<std::uint8_t*>(pBufferOut), bytesToRead);
    };

    drmp3 mp3{};
    // Non-seekable continuous stream mode: dr_mp3 caches initial bytes cleanly without backward seek,
    // allowing raw MP3 streams without ID3v2 tags (e.g., Icecast/Shoutcast) to decode from frame 0.
    if (!drmp3_init(&mp3, on_read, nullptr, nullptr, nullptr, &ctx, nullptr)) {
        Logger::Instance().Debug("drmp3_init failed");
        return false;
    }

    std::array<drmp3_int16, 4096 * 2> pcm{};
    while (!cancel_flag.load()) {
        drmp3_uint64 read_frames = drmp3_read_pcm_frames_s16(&mp3, 4096, pcm.data());
        if (read_frames == 0) break;
        if (!pipeline.PushPcmInterleaved(pcm.data(), static_cast<std::size_t>(read_frames), static_cast<int>(mp3.sampleRate), static_cast<int>(mp3.channels))) {
            break;
        }
    }
    drmp3_uninit(&mp3);
    return true;
}

bool DecodeWavStream(PullByteStream& input, Mp3OutputPipeline& pipeline, std::atomic<bool>& cancel_flag) {
    struct Ctx {
        PullByteStream* in;
    } ctx{&input};

    auto on_read = [](void* pUserData, void* pBufferOut, size_t bytesToRead) -> size_t {
        return static_cast<Ctx*>(pUserData)->in->ReadSome(static_cast<std::uint8_t*>(pBufferOut), bytesToRead);
    };
    auto on_seek = [](void* pUserData, int offset, drwav_seek_origin origin) -> drwav_bool32 {
        auto* c = static_cast<Ctx*>(pUserData);
        if (origin == DRWAV_SEEK_CUR) {
            if (offset == 0) return DRWAV_TRUE;
            if (offset > 0) return c->in->Skip(static_cast<std::size_t>(offset)) ? DRWAV_TRUE : DRWAV_FALSE;
        } else if (origin == DRWAV_SEEK_SET && offset >= 0) {
            std::uint64_t cur = c->in->TotalConsumed();
            std::uint64_t target = static_cast<std::uint64_t>(offset);
            if (target == cur) return DRWAV_TRUE;
            if (target > cur) return c->in->Skip(static_cast<std::size_t>(target - cur)) ? DRWAV_TRUE : DRWAV_FALSE;
        }
        return DRWAV_FALSE;
    };

    drwav wav{};
    if (!drwav_init_ex(&wav, on_read, on_seek, nullptr, nullptr, &ctx, nullptr, DRWAV_SEQUENTIAL, nullptr)) {
        Logger::Instance().Error("drwav_init failed (consumed=" + std::to_string(input.TotalConsumed()) + ")");
        return false;
    }

    std::array<drwav_int16, 4096 * 2> pcm{};
    while (!cancel_flag.load()) {
        drwav_uint64 read_frames = drwav_read_pcm_frames_s16(&wav, 4096, pcm.data());
        if (read_frames == 0) break;
        if (!pipeline.PushPcmInterleaved(pcm.data(), static_cast<std::size_t>(read_frames), static_cast<int>(wav.sampleRate), static_cast<int>(wav.channels))) {
            break;
        }
    }
    drwav_uninit(&wav);
    return true;
}

bool DecodeFlacStream(PullByteStream& input, Mp3OutputPipeline& pipeline, std::atomic<bool>& cancel_flag) {
    struct Ctx {
        PullByteStream* in;
    } ctx{&input};

    auto on_read = [](void* pUserData, void* pBufferOut, size_t bytesToRead) -> size_t {
        return static_cast<Ctx*>(pUserData)->in->ReadSome(static_cast<std::uint8_t*>(pBufferOut), bytesToRead);
    };
    auto on_seek = [](void* pUserData, int offset, drflac_seek_origin origin) -> drflac_bool32 {
        auto* c = static_cast<Ctx*>(pUserData);
        if (origin == DRFLAC_SEEK_CUR) {
            if (offset == 0) return DRFLAC_TRUE;
            if (offset > 0) return c->in->Skip(static_cast<std::size_t>(offset)) ? DRFLAC_TRUE : DRFLAC_FALSE;
        } else if (origin == DRFLAC_SEEK_SET && offset >= 0) {
            std::uint64_t cur = c->in->TotalConsumed();
            std::uint64_t target = static_cast<std::uint64_t>(offset);
            if (target == cur) return DRFLAC_TRUE;
            if (target > cur) return c->in->Skip(static_cast<std::size_t>(target - cur)) ? DRFLAC_TRUE : DRFLAC_FALSE;
        }
        return DRFLAC_FALSE;
    };

    // Pass nullptr for onTell so dr_flac skips DRFLAC_SEEK_END + backwards SEEK_SET on non-seekable streams.
    drflac* flac = drflac_open(on_read, on_seek, nullptr, &ctx, nullptr);
    if (!flac) return false;

    std::vector<drflac_int16> pcm(4096 * flac->channels);
    while (!cancel_flag.load()) {
        drflac_uint64 read_frames = drflac_read_pcm_frames_s16(flac, 4096, pcm.data());
        if (read_frames == 0) break;
        if (!pipeline.PushPcmInterleaved(pcm.data(), static_cast<std::size_t>(read_frames), static_cast<int>(flac->sampleRate), static_cast<int>(flac->channels))) {
            break;
        }
    }
    drflac_close(flac);
    return true;
}

// Unified Opus decoder supporting mono, stereo, and multichannel (via opus_multistream_decoder)
class OpusStreamDecoder {
public:
    OpusStreamDecoder() = default;
    ~OpusStreamDecoder() {
        Destroy();
    }

    OpusStreamDecoder(const OpusStreamDecoder&) = delete;
    OpusStreamDecoder& operator=(const OpusStreamDecoder&) = delete;

    void Destroy() {
        if (dec_) {
            opus_decoder_destroy(dec_);
            dec_ = nullptr;
        }
        if (ms_dec_) {
            opus_multistream_decoder_destroy(ms_dec_);
            ms_dec_ = nullptr;
        }
    }

    bool InitFromOpusHead(const std::uint8_t* hdr, std::size_t len) {
        if (!hdr || len < 10 || std::memcmp(hdr, "OpusHead", 8) != 0) {
            return false;
        }
        Destroy();
        int ch = static_cast<int>(hdr[9]);
        if (ch <= 0 || ch > 8) ch = 2;
        channels_ = ch;

        int mapping_family = (len >= 19) ? static_cast<int>(hdr[18]) : 0;
        int err = 0;
        if (mapping_family == 0 || ch <= 2) {
            dec_ = opus_decoder_create(48000, ch == 1 ? 1 : 2, &err);
            channels_ = (ch == 1) ? 1 : 2;
            pcm_.resize(5760 * static_cast<std::size_t>(channels_));
            return dec_ != nullptr && err == OPUS_OK;
        }

        if (len >= static_cast<std::size_t>(21 + ch)) {
            int streams = static_cast<int>(hdr[19]);
            int coupled = static_cast<int>(hdr[20]);
            if (streams < 1 || streams > 255 || coupled < 0 || coupled > streams) {
                return false;
            }
            ms_dec_ = opus_multistream_decoder_create(48000, ch, streams, coupled, hdr + 21, &err);
            pcm_.resize(5760 * static_cast<std::size_t>(channels_));
            return ms_dec_ != nullptr && err == OPUS_OK;
        }
        return false;
    }

    bool InitBasic(int channels) {
        Destroy();
        int ch = (channels == 1) ? 1 : 2;
        channels_ = ch;
        int err = 0;
        dec_ = opus_decoder_create(48000, ch, &err);
        pcm_.resize(5760 * static_cast<std::size_t>(ch));
        return dec_ != nullptr && err == OPUS_OK;
    }

    bool IsInitialized() const {
        return dec_ != nullptr || ms_dec_ != nullptr;
    }

    bool DecodePacket(const std::uint8_t* pkt, std::size_t len, Mp3OutputPipeline& pipeline) {
        if (!IsInitialized() || !pkt || len == 0) return true;
        int frames = 0;
        if (ms_dec_) {
            frames = opus_multistream_decode(ms_dec_, pkt, static_cast<opus_int32>(len), pcm_.data(), 5760, 0);
        } else if (dec_) {
            frames = opus_decode(dec_, pkt, static_cast<opus_int32>(len), pcm_.data(), 5760, 0);
        }
        if (frames > 0) {
            return pipeline.PushPcmInterleaved(pcm_.data(), static_cast<std::size_t>(frames), 48000, channels_);
        }
        return true;
    }

private:
    OpusDecoder* dec_{nullptr};
    OpusMSDecoder* ms_dec_{nullptr};
    int channels_{2};
    std::vector<opus_int16> pcm_;
};

// Feed raw Ogg Vorbis container bytes directly to stb_vorbis pushdata (preserves Ogg framing required by stb_vorbis)
bool DecodeRawOggVorbis(
    PullByteStream& input,
    std::vector<std::uint8_t> buf,
    Mp3OutputPipeline& pipeline,
    std::atomic<bool>& cancel_flag) {
    constexpr std::size_t kMaxVorbisBufBytes = 4 * 1024 * 1024;
    buf.reserve(64 * 1024);
    stb_vorbis* v = nullptr;
    stb_vorbis_info info{};
    bool eof = false;

    auto read_more = [&]() -> bool {
        if (eof || buf.size() >= kMaxVorbisBufBytes) return false;
        std::array<std::uint8_t, 8192> chunk{};
        std::size_t n = input.ReadSome(chunk.data(), chunk.size());
        if (n == 0) {
            eof = true;
            return false;
        }
        buf.insert(buf.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(n));
        return true;
    };

    while (!v && !cancel_flag.load()) {
        int consumed = 0;
        int err = VORBIS__no_error;
        v = stb_vorbis_open_pushdata(buf.data(), static_cast<int>(buf.size()), &consumed, &err, nullptr);
        if (v) {
            if (consumed > 0) {
                buf.erase(buf.begin(), buf.begin() + std::min<std::size_t>(static_cast<std::size_t>(consumed), buf.size()));
            }
            info = stb_vorbis_get_info(v);
            break;
        }
        if (err == VORBIS_need_more_data) {
            if (!read_more()) break;
        } else {
            return false;
        }
    }
    if (!v) return false;

    std::vector<std::int16_t> inter;
    while (!cancel_flag.load()) {
        int channels_out = 0;
        float** outputs = nullptr;
        int samples = 0;
        int used = stb_vorbis_decode_frame_pushdata(
            v,
            buf.data(),
            static_cast<int>(buf.size()),
            &channels_out,
            &outputs,
            &samples);

        if (used == 0) {
            if (!read_more()) break;
            continue;
        }

        buf.erase(buf.begin(), buf.begin() + std::min<std::size_t>(static_cast<std::size_t>(used), buf.size()));

        if (samples > 0 && outputs && channels_out > 0) {
            inter.resize(static_cast<std::size_t>(samples) * static_cast<std::size_t>(channels_out));
            for (int i = 0; i < samples; ++i) {
                for (int c = 0; c < channels_out; ++c) {
                    float f = std::clamp(outputs[c][i], -1.0f, 1.0f);
                    inter[static_cast<std::size_t>(i * channels_out + c)] = static_cast<std::int16_t>(f * 32767.0f);
                }
            }
            if (!pipeline.PushPcmInterleaved(
                    inter.data(),
                    static_cast<std::size_t>(samples),
                    static_cast<int>(info.sample_rate),
                    channels_out)) {
                break;
            }
        }
    }

    stb_vorbis_close(v);
    return pipeline.TotalPcmFrames() > 0;
}

// OGG container (Opus via libogg + opus/multistream, Vorbis via raw stb_vorbis stream)
bool DecodeOggStream(PullByteStream& input, Mp3OutputPipeline& pipeline, std::atomic<bool>& cancel_flag) {
    std::vector<std::uint8_t> init_buf;
    init_buf.reserve(4096);
    while (init_buf.size() < 512 && !cancel_flag.load()) {
        std::array<std::uint8_t, 512> chunk{};
        std::size_t n = input.ReadSome(chunk.data(), chunk.size());
        if (n == 0) break;
        init_buf.insert(init_buf.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(n));
    }
    if (init_buf.empty()) return false;

    auto contains_seq = [&](const char* pat, std::size_t plen) {
        if (init_buf.size() < plen) return false;
        auto it = std::search(
            init_buf.begin(),
            init_buf.end(),
            reinterpret_cast<const std::uint8_t*>(pat),
            reinterpret_cast<const std::uint8_t*>(pat) + plen);
        return it != init_buf.end();
    };

    if (contains_seq("\x01vorbis", 7)) {
        return DecodeRawOggVorbis(input, std::move(init_buf), pipeline, cancel_flag);
    }

    ogg_sync_state oy;
    ogg_sync_init(&oy);
    ogg_stream_state os{};
    bool stream_inited = false;
    bool is_opus = false;
    int headers_seen = 0;
    OpusStreamDecoder opus;

    char* sync_init = ogg_sync_buffer(&oy, static_cast<long>(init_buf.size()));
    std::memcpy(sync_init, init_buf.data(), init_buf.size());
    ogg_sync_wrote(&oy, static_cast<long>(init_buf.size()));

    auto cleanup = [&]() {
        opus.Destroy();
        if (stream_inited) ogg_stream_clear(&os);
        ogg_sync_clear(&oy);
    };

    bool aborted = false;
    while (!cancel_flag.load() && !aborted) {
        ogg_page og{};
        while (ogg_sync_pageout(&oy, &og) == 1) {
            if (!stream_inited) {
                ogg_stream_init(&os, ogg_page_serialno(&og));
                stream_inited = true;
            }
            ogg_stream_pagein(&os, &og);
            ogg_packet op{};
            while (ogg_stream_packetout(&os, &op) == 1) {
                if (op.bytes < 0 || op.bytes > 1024 * 1024) {
                    aborted = true;
                    break;
                }
                if (headers_seen == 0) {
                    if (op.bytes >= 8 && std::memcmp(op.packet, "OpusHead", 8) == 0) {
                        is_opus = opus.InitFromOpusHead(op.packet, static_cast<std::size_t>(op.bytes));
                        headers_seen = 1;
                    }
                    continue;
                }

                if (is_opus) {
                    if (headers_seen == 1 && op.bytes >= 8 && std::memcmp(op.packet, "OpusTags", 8) == 0) {
                        headers_seen = 2;
                        continue;
                    }
                    if (!opus.DecodePacket(op.packet, static_cast<std::size_t>(op.bytes), pipeline)) {
                        aborted = true;
                        break;
                    }
                }
            }
            if (aborted) break;
        }
        if (aborted) break;

        char* buf = ogg_sync_buffer(&oy, 8192);
        std::size_t bytes = input.ReadSome(reinterpret_cast<std::uint8_t*>(buf), 8192);
        if (bytes == 0) break;
        ogg_sync_wrote(&oy, static_cast<long>(bytes));
    }

    cleanup();
    return pipeline.TotalPcmFrames() > 0;
}

// ---------------- WebM / Matroska Streaming Demuxer ----------------

bool ReadEbmlVint(PullByteStream& in, std::uint64_t& out_val, bool keep_marker, std::size_t& out_len) {
    std::uint8_t first = 0;
    if (!in.ReadExact(&first, 1)) return false;
    if (first == 0) return false;

    int len = 1;
    std::uint8_t mask = 0x80;
    while (len <= 8 && (first & mask) == 0) {
        mask >>= 1;
        len++;
    }
    if (len > 8) return false;

    std::uint64_t val = keep_marker ? first : (first & ~mask);
    for (int i = 1; i < len; ++i) {
        std::uint8_t b = 0;
        if (!in.ReadExact(&b, 1)) return false;
        val = (val << 8) | b;
    }
    out_val = val;
    out_len = static_cast<std::size_t>(len);
    return true;
}

bool ReadEbmlVintFromMem(const std::uint8_t* data, std::size_t avail, std::size_t& pos, std::uint64_t& out_val, bool keep_marker) {
    if (pos >= avail) return false;
    std::uint8_t first = data[pos++];
    if (first == 0) return false;
    int len = 1;
    std::uint8_t mask = 0x80;
    while (len <= 8 && (first & mask) == 0) {
        mask >>= 1;
        len++;
    }
    if (len > 8 || pos + static_cast<std::size_t>(len - 1) > avail) return false;
    std::uint64_t val = keep_marker ? first : (first & ~mask);
    for (int i = 1; i < len; ++i) {
        val = (val << 8) | data[pos++];
    }
    out_val = val;
    return true;
}

bool DecodeWebmStream(PullByteStream& input, Mp3OutputPipeline& pipeline, std::atomic<bool>& cancel_flag) {
    // WebM EBML Element IDs
    constexpr std::uint64_t ID_EBML = 0x1A45DFA3;
    constexpr std::uint64_t ID_SEGMENT = 0x18538067;
    constexpr std::uint64_t ID_TRACKS = 0x1654AE6B;
    constexpr std::uint64_t ID_TRACK_ENTRY = 0xAE;
    constexpr std::uint64_t ID_TRACK_NUMBER = 0xD7;
    constexpr std::uint64_t ID_TRACK_TYPE = 0x83;
    constexpr std::uint64_t ID_CODEC_ID = 0x86;
    constexpr std::uint64_t ID_CODEC_PRIVATE = 0x63A2;
    constexpr std::uint64_t ID_AUDIO = 0xE1;
    constexpr std::uint64_t ID_SAMPLING_FREQ = 0xB5;
    constexpr std::uint64_t ID_CHANNELS = 0x9F;
    constexpr std::uint64_t ID_CLUSTER = 0x1F43B675;
    constexpr std::uint64_t ID_BLOCK_GROUP = 0xA0;
    constexpr std::uint64_t ID_BLOCK = 0xA1;
    constexpr std::uint64_t ID_SIMPLE_BLOCK = 0xA3;

    struct WebmTrack {
        std::uint64_t number{0};
        std::uint64_t type{0};
        std::string codec_id;
        std::vector<std::uint8_t> codec_private;
        int sample_rate{48000};
        int channels{2};
    };

    WebmTrack audio_track;
    WebmTrack current_track;
    bool has_audio_track = false;
    bool unsupported_codec = false;

    OpusStreamDecoder opus;

    auto ensure_decoder = [&]() -> bool {
        if (!has_audio_track) return true;
        if (audio_track.codec_id != "A_OPUS") {
            if (!unsupported_codec) {
                unsupported_codec = true;
                Logger::Instance().Warn("Unsupported WebM audio codec '" + audio_track.codec_id + "' (only A_OPUS is supported).");
            }
            return false;
        }
        if (!opus.IsInitialized()) {
            if (!audio_track.codec_private.empty() &&
                opus.InitFromOpusHead(audio_track.codec_private.data(), audio_track.codec_private.size())) {
                return true;
            }
            return opus.InitBasic(audio_track.channels);
        }
        return true;
    };

    auto decode_packet = [&](const std::uint8_t* pkt, std::size_t len) -> bool {
        if (!ensure_decoder()) return false;
        if (opus.IsInitialized()) {
            return opus.DecodePacket(pkt, len, pipeline);
        }
        return true;
    };

    auto process_block = [&](const std::vector<std::uint8_t>& blk) -> bool {
        if (blk.size() < 4) return true;
        std::size_t pos = 0;
        std::uint64_t track_num = 0;
        if (!ReadEbmlVintFromMem(blk.data(), blk.size(), pos, track_num, false)) return true;
        if (has_audio_track && track_num != audio_track.number) return true;
        if (pos + 3 > blk.size()) return true;

        pos += 2; // relative timecode
        std::uint8_t flags = blk[pos++];
        int lacing = (flags >> 1) & 0x03;

        if (lacing == 0) {
            return decode_packet(blk.data() + pos, blk.size() - pos);
        }

        if (pos >= blk.size()) return true;
        int frame_count = static_cast<int>(blk[pos++]) + 1;
        std::vector<std::size_t> sizes(static_cast<std::size_t>(frame_count), 0);

        if (lacing == 1) { // Xiph lacing
            std::size_t sum = 0;
            for (int i = 0; i < frame_count - 1; ++i) {
                std::size_t sz = 0;
                while (pos < blk.size()) {
                    std::uint8_t b = blk[pos++];
                    sz += b;
                    if (b < 255) break;
                }
                sizes[static_cast<std::size_t>(i)] = sz;
                sum += sz;
            }
            if (pos + sum <= blk.size()) {
                sizes.back() = blk.size() - pos - sum;
            }
        } else if (lacing == 2) { // Fixed-size lacing
            std::size_t rem = blk.size() - pos;
            std::size_t each = rem / static_cast<std::size_t>(frame_count);
            std::fill(sizes.begin(), sizes.end(), each);
        } else if (lacing == 3) { // EBML lacing
            std::uint64_t first_sz = 0;
            if (!ReadEbmlVintFromMem(blk.data(), blk.size(), pos, first_sz, false)) return true;
            sizes[0] = static_cast<std::size_t>(first_sz);
            std::int64_t prev = static_cast<std::int64_t>(first_sz);
            std::size_t sum = sizes[0];
            for (int i = 1; i < frame_count - 1; ++i) {
                std::size_t start_pos = pos;
                std::uint64_t raw = 0;
                if (!ReadEbmlVintFromMem(blk.data(), blk.size(), pos, raw, false)) return true;
                std::size_t vlen = pos - start_pos;
                std::int64_t bias = (1LL << (vlen * 7 - 1)) - 1;
                prev += static_cast<std::int64_t>(raw) - bias;
                if (prev < 0) return true;
                sizes[static_cast<std::size_t>(i)] = static_cast<std::size_t>(prev);
                sum += sizes[static_cast<std::size_t>(i)];
            }
            if (pos + sum <= blk.size()) {
                sizes.back() = blk.size() - pos - sum;
            }
        }

        for (std::size_t sz : sizes) {
            if (pos + sz > blk.size()) break;
            if (!decode_packet(blk.data() + pos, sz)) return false;
            pos += sz;
        }
        return true;
    };

    auto parse_track_entry = [&](const std::uint8_t* data, std::size_t len) {
        current_track = WebmTrack{};
        std::size_t pos = 0;
        while (pos < len) {
            std::uint64_t id = 0, sz = 0;
            if (!ReadEbmlVintFromMem(data, len, pos, id, true)) break;
            if (!ReadEbmlVintFromMem(data, len, pos, sz, false)) break;
            if (pos + sz > len) break;

            if (id == ID_TRACK_NUMBER) {
                std::uint64_t v = 0;
                for (std::size_t i = 0; i < sz; ++i) v = (v << 8) | data[pos + i];
                current_track.number = v;
            } else if (id == ID_TRACK_TYPE) {
                std::uint64_t v = 0;
                for (std::size_t i = 0; i < sz; ++i) v = (v << 8) | data[pos + i];
                current_track.type = v;
            } else if (id == ID_CODEC_ID) {
                current_track.codec_id.assign(reinterpret_cast<const char*>(data + pos), static_cast<std::size_t>(sz));
            } else if (id == ID_CODEC_PRIVATE && sz <= 65536) {
                current_track.codec_private.assign(data + pos, data + pos + static_cast<std::size_t>(sz));
            } else if (id == ID_AUDIO) {
                std::size_t apos = pos;
                std::size_t aend = pos + static_cast<std::size_t>(sz);
                while (apos < aend) {
                    std::uint64_t aid = 0, asz = 0;
                    if (!ReadEbmlVintFromMem(data, aend, apos, aid, true)) break;
                    if (!ReadEbmlVintFromMem(data, aend, apos, asz, false)) break;
                    if (apos + asz > aend) break;
                    if (aid == ID_CHANNELS) {
                        std::uint64_t ch = 0;
                        for (std::size_t i = 0; i < asz; ++i) ch = (ch << 8) | data[apos + i];
                        current_track.channels = static_cast<int>(ch);
                    } else if (aid == ID_SAMPLING_FREQ && (asz == 4 || asz == 8)) {
                        if (asz == 4) {
                            std::uint32_t bits = 0;
                            for (int i = 0; i < 4; ++i) bits = (bits << 8) | data[apos + i];
                            float f = 0.0f;
                            std::memcpy(&f, &bits, 4);
                            current_track.sample_rate = static_cast<int>(f);
                        } else {
                            std::uint64_t bits = 0;
                            for (int i = 0; i < 8; ++i) bits = (bits << 8) | data[apos + i];
                            double d = 0.0;
                            std::memcpy(&d, &bits, 8);
                            current_track.sample_rate = static_cast<int>(d);
                        }
                    }
                    apos += static_cast<std::size_t>(asz);
                }
            }
            pos += static_cast<std::size_t>(sz);
        }
        // TrackType 2 = Audio
        if (current_track.type == 2 && !has_audio_track) {
            audio_track = current_track;
            has_audio_track = true;
        }
    };

    // Stream top-level & nested WebM master elements
    while (!cancel_flag.load()) {
        std::uint64_t id = 0, sz = 0;
        std::size_t id_len = 0, sz_len = 0;
        if (!ReadEbmlVint(input, id, true, id_len)) break;
        if (!ReadEbmlVint(input, sz, false, sz_len)) break;

        // Master container elements we descend into
        if (id == ID_SEGMENT || id == ID_TRACKS || id == ID_CLUSTER || id == ID_BLOCK_GROUP) {
            continue;
        }

        if (id == ID_EBML) {
            if (!input.Skip(sz)) break;
            continue;
        }

        if (id == ID_TRACK_ENTRY && sz < 1024 * 1024) {
            std::vector<std::uint8_t> buf(static_cast<std::size_t>(sz));
            if (!input.ReadExact(buf.data(), buf.size())) break;
            parse_track_entry(buf.data(), buf.size());
            if (has_audio_track && audio_track.codec_id != "A_OPUS") {
                Logger::Instance().Warn("Unsupported WebM audio codec '" + audio_track.codec_id + "' (only A_OPUS is supported).");
                return false;
            }
            continue;
        }

        if ((id == ID_SIMPLE_BLOCK || id == ID_BLOCK) && sz < 4 * 1024 * 1024) {
            std::vector<std::uint8_t> buf(static_cast<std::size_t>(sz));
            if (!input.ReadExact(buf.data(), buf.size())) break;
            if (!process_block(buf)) break;
            continue;
        }

        // Unknown / uninteresting leaf element (e.g. Timecode, Cues, Void, Tags)
        if (!input.Skip(sz)) break;
    }

    opus.Destroy();
    return !unsupported_codec && pipeline.TotalPcmFrames() > 0;
}

// ---------------- AAC (ADTS & MP4/M4A & MPEG-TS) via FDK-AAC ----------------

class FdkAacHelper {
public:
    explicit FdkAacHelper(TRANSPORT_TYPE transport)
        : transport_(transport),
          dec_(aacDecoder_Open(transport, 1)) {}
    ~FdkAacHelper() {
        if (dec_) aacDecoder_Close(dec_);
    }

    bool ConfigureRaw(const std::vector<std::uint8_t>& asc) {
        if (!dec_ || asc.empty()) return false;
        UCHAR* ptr = const_cast<UCHAR*>(asc.data());
        UINT len = static_cast<UINT>(asc.size());
        return aacDecoder_ConfigRaw(dec_, &ptr, &len) == AAC_DEC_OK;
    }

    bool FeedAndDecode(const std::uint8_t* data, std::size_t len, Mp3OutputPipeline& pipeline) {
        if (!dec_ || !data || len == 0) return true;
        UINT valid = static_cast<UINT>(len);
        UINT buf_size = static_cast<UINT>(len);
        UCHAR* ptr = const_cast<UCHAR*>(data);
        bool reset_attempted = false;

        while (valid > 0) {
            if (aacDecoder_Fill(dec_, &ptr, &buf_size, &valid) != AAC_DEC_OK) {
                break;
            }
            bool decode_error = false;
            while (true) {
                AAC_DECODER_ERROR err = aacDecoder_DecodeFrame(dec_, pcm_.data(), static_cast<INT>(pcm_.size()), 0);
                if (err == AAC_DEC_NOT_ENOUGH_BITS) break;
                if (err != AAC_DEC_OK) {
                    decode_error = true;
                    break;
                }
                CStreamInfo* info = aacDecoder_GetStreamInfo(dec_);
                if (info && info->sampleRate > 0 && info->numChannels > 0) {
                    if (!pipeline.PushPcmInterleaved(
                            pcm_.data(),
                            static_cast<std::size_t>(info->frameSize),
                            info->sampleRate,
                            info->numChannels)) {
                        return false;
                    }
                }
            }
            if (decode_error && !reset_attempted && transport_ != TT_MP4_RAW) {
                // Resynchronize after mid-stream ADTS/LATM discontinuity without losing subsequent audio
                reset_attempted = true;
                aacDecoder_Close(dec_);
                dec_ = aacDecoder_Open(transport_, 1);
                if (!dec_) break;
                UINT prev_rem = valid;
                ptr += (buf_size - valid);
                buf_size = prev_rem;
                continue;
            }
            if (valid > 0 && valid == buf_size) break; // prevent infinite loop on invalid bytes
            ptr += (buf_size - valid);
            buf_size = valid;
        }
        return true;
    }

private:
    TRANSPORT_TYPE transport_;
    HANDLE_AACDECODER dec_{nullptr};
    std::array<INT_PCM, 2048 * 8> pcm_{};
};

std::uint32_t ReadBe32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) |
           static_cast<std::uint32_t>(p[3]);
}

std::uint64_t ReadBe64(const std::uint8_t* p) {
    return (static_cast<std::uint64_t>(ReadBe32(p)) << 32) | ReadBe32(p + 4);
}

// Returns total size of an ID3v2 header at data[0..avail), or 0 if not a valid ID3v2 tag.
std::size_t ParseId3v2TagSize(const std::uint8_t* p, std::size_t avail) {
    if (!p || avail < 10) return 0;
    if (p[0] != 'I' || p[1] != 'D' || p[2] != '3' || p[3] >= 0xFF || p[4] >= 0xFF) return 0;
    if ((p[6] & 0x80u) || (p[7] & 0x80u) || (p[8] & 0x80u) || (p[9] & 0x80u)) return 0;
    std::size_t payload = (static_cast<std::size_t>(p[6] & 0x7Fu) << 21) |
                          (static_cast<std::size_t>(p[7] & 0x7Fu) << 14) |
                          (static_cast<std::size_t>(p[8] & 0x7Fu) << 7) |
                          static_cast<std::size_t>(p[9] & 0x7Fu);
    std::size_t footer = (p[5] & 0x10u) ? 10 : 0;
    return 10 + payload + footer;
}

bool DecodeAdtsStream(PullByteStream& input, Mp3OutputPipeline& pipeline, std::atomic<bool>& cancel_flag) {
    FdkAacHelper aac(TT_MP4_ADTS);
    std::vector<std::uint8_t> buf;
    buf.reserve(16384);
    std::array<std::uint8_t, 8192> chunk{};
    std::size_t pos = 0;
    bool eof = false;

    auto compact_and_refill = [&]() -> bool {
        if (pos > 0) {
            if (pos < buf.size()) {
                std::memmove(buf.data(), buf.data() + pos, buf.size() - pos);
            }
            buf.resize(buf.size() - pos);
            pos = 0;
        }
        if (eof) return !buf.empty();
        std::size_t n = input.ReadSome(chunk.data(), chunk.size());
        if (n == 0) {
            eof = true;
            return !buf.empty();
        }
        buf.insert(buf.end(), chunk.data(), chunk.data() + n);
        return true;
    };

    while (!cancel_flag.load()) {
        if (buf.size() - pos < 16 && !eof) {
            if (!compact_and_refill()) break;
        }
        if (pos >= buf.size()) {
            if (eof) break;
            continue;
        }

        const std::size_t avail = buf.size() - pos;
        const std::uint8_t* cur = buf.data() + pos;

        // Skip inline ID3v2 tags (present at the start of every RFC 8216 packed-audio HLS segment).
        if (avail >= 10 && cur[0] == 'I' && cur[1] == 'D' && cur[2] == '3') {
            std::size_t tag_size = ParseId3v2TagSize(cur, avail);
            if (tag_size > 0 && tag_size <= 2 * 1024 * 1024) {
                if (tag_size <= avail) {
                    pos += tag_size;
                } else {
                    std::size_t remaining_skip = tag_size - avail;
                    pos = buf.size();
                    if (!input.Skip(remaining_skip)) break;
                }
                continue;
            }
        }

        if (avail >= 7 && cur[0] == 0xFF && (cur[1] & 0xF6) == 0xF0) {
            std::size_t frame_len = (static_cast<std::size_t>(cur[3] & 0x03u) << 11) |
                                    (static_cast<std::size_t>(cur[4]) << 3) |
                                    (static_cast<std::size_t>(cur[5] & 0xE0u) >> 5);
            if (frame_len >= 7 && frame_len <= 8192) {
                while (buf.size() - pos < frame_len && !eof && !cancel_flag.load()) {
                    if (!compact_and_refill()) break;
                }
                if (buf.size() - pos >= frame_len) {
                    if (!aac.FeedAndDecode(buf.data() + pos, frame_len, pipeline)) {
                        return false;
                    }
                    pos += frame_len;
                    continue;
                }
                if (eof) break;
            }
        }

        // Advance past non-sync byte (or if < 7 bytes remain before non-EOF refill)
        if (avail < 7 && !eof) {
            if (!compact_and_refill()) break;
            continue;
        }
        ++pos;
    }
    return pipeline.TotalPcmFrames() > 0;
}

bool DecodeMp4Stream(PullByteStream& input, Mp3OutputPipeline& pipeline, std::atomic<bool>& cancel_flag) {
    constexpr std::uint32_t kMaxMp4SampleCount = 2000000;
    constexpr std::uint32_t kMaxMp4SampleBytes = 64 * 1024;
    constexpr int kMaxMp4BoxDepth = 16;

    FdkAacHelper aac(TT_MP4_RAW);
    bool configured = false;
    bool saw_moov_or_moof = false;
    bool malformed = false;
    std::vector<std::uint32_t> prog_sample_sizes;
    std::uint32_t prog_fixed_count = 0;
    std::uint32_t prog_fixed_size = 0;
    std::uint32_t frag_default_sample_size = 0;
    std::vector<std::uint32_t> frag_sample_sizes;

    auto extract_esds_asc = [](const std::uint8_t* data, std::size_t len) -> std::vector<std::uint8_t> {
        // Scan for 0x05 (DecoderSpecificInfo tag) inside esds
        for (std::size_t i = 0; i + 4 < len; ++i) {
            if (data[i] == 0x05) {
                std::size_t pos = i + 1;
                std::size_t dlen = 0;
                for (int k = 0; k < 4 && pos < len; ++k) {
                    std::uint8_t b = data[pos++];
                    dlen = (dlen << 7) | (b & 0x7F);
                    if ((b & 0x80) == 0) break;
                }
                if (dlen >= 2 && dlen <= 16 && pos + dlen <= len) {
                    return std::vector<std::uint8_t>(data + pos, data + pos + dlen);
                }
            }
        }
        return {};
    };

    std::function<void(const std::uint8_t*, std::size_t, int)> parse_moov_or_moof;
    parse_moov_or_moof = [&](const std::uint8_t* data, std::size_t len, int depth) {
        if (malformed || depth > kMaxMp4BoxDepth) return;
        std::size_t pos = 0;
        while (pos + 8 <= len && !malformed) {
            std::uint64_t sz = ReadBe32(data + pos);
            std::uint32_t fourcc = ReadBe32(data + pos + 4);
            std::size_t hdr = 8;
            if (sz == 1 && pos + 16 <= len) {
                sz = ReadBe64(data + pos + 8);
                hdr = 16;
            } else if (sz == 0) {
                sz = len - pos;
            }
            if (sz < hdr || pos + sz > len) break;

            const std::uint8_t* payload = data + pos + hdr;
            std::size_t plen = static_cast<std::size_t>(sz - hdr);

            // Container boxes
            if (fourcc == 0x7472616B || // trak
                fourcc == 0x6D646961 || // mdia
                fourcc == 0x6D696E66 || // minf
                fourcc == 0x7374626C || // stbl
                fourcc == 0x74726166) { // traf
                parse_moov_or_moof(payload, plen, depth + 1);
            } else if (fourcc == 0x73747364 && plen > 8) { // stsd
                // Sample entry starts at offset 8
                parse_moov_or_moof(payload + 8, plen - 8, depth + 1);
            } else if (fourcc == 0x6D703461 && plen > 28) { // mp4a
                parse_moov_or_moof(payload + 28, plen - 28, depth + 1);
            } else if (fourcc == 0x65736473 && plen > 4) { // esds
                auto asc = extract_esds_asc(payload + 4, plen - 4);
                if (!asc.empty() && !configured) {
                    configured = aac.ConfigureRaw(asc);
                }
            } else if (fourcc == 0x7374737A && plen >= 12) { // stsz
                std::uint32_t sample_size = ReadBe32(payload + 4);
                std::uint32_t count = ReadBe32(payload + 8);
                if (sample_size == 0) {
                    prog_fixed_count = 0;
                    prog_fixed_size = 0;
                    std::size_t max_entries = (plen - 12) / 4;
                    std::size_t safe_count = std::min<std::size_t>(count, max_entries);
                    if (safe_count > kMaxMp4SampleCount || prog_sample_sizes.size() + safe_count > kMaxMp4SampleCount) {
                        malformed = true;
                        break;
                    }
                    for (std::size_t i = 0; i < safe_count; ++i) {
                        std::uint32_t ssz = ReadBe32(payload + 12 + i * 4);
                        if (ssz > kMaxMp4SampleBytes) {
                            malformed = true;
                            break;
                        }
                        prog_sample_sizes.push_back(ssz);
                    }
                } else {
                    if (sample_size > kMaxMp4SampleBytes || count > kMaxMp4SampleCount) {
                        malformed = true;
                        break;
                    }
                    prog_sample_sizes.clear();
                    prog_fixed_count = count;
                    prog_fixed_size = sample_size;
                }
            } else if (fourcc == 0x74666864 && plen >= 8) { // tfhd
                std::uint32_t flags = ReadBe32(payload) & 0x00FFFFFFu;
                std::size_t off = 8; // version+flags (4) + track_ID (4)
                if (flags & 0x000001u) off += 8; // base-data-offset
                if (flags & 0x000002u) off += 4; // sample-description-index
                if (flags & 0x000008u) off += 4; // default-sample-duration
                if ((flags & 0x000010u) && off + 4 <= plen) { // default-sample-size
                    std::uint32_t dsz = ReadBe32(payload + off);
                    if (dsz <= kMaxMp4SampleBytes) {
                        frag_default_sample_size = dsz;
                    }
                }
            } else if (fourcc == 0x7472756E && plen >= 8) { // trun
                std::uint32_t flags = ReadBe32(payload) & 0x00FFFFFF;
                std::uint32_t count = ReadBe32(payload + 4);
                std::size_t off = 8;
                if (flags & 0x000001) off += 4; // data_offset
                if (flags & 0x000004) off += 4; // first_sample_flags
                bool has_dur = (flags & 0x000100) != 0;
                bool has_sz = (flags & 0x000200) != 0;
                bool has_fl = (flags & 0x000400) != 0;
                bool has_cto = (flags & 0x000800) != 0;
                if (!has_sz && frag_default_sample_size > 0) {
                    std::size_t safe_def = std::min<std::size_t>(count, kMaxMp4SampleCount - frag_sample_sizes.size());
                    if (count > kMaxMp4SampleCount) {
                        malformed = true;
                        break;
                    }
                    frag_sample_sizes.insert(frag_sample_sizes.end(), safe_def, frag_default_sample_size);
                    pos += static_cast<std::size_t>(sz);
                    continue;
                }
                std::size_t entry_stride = (has_dur ? 4 : 0) + (has_sz ? 4 : 0) + (has_fl ? 4 : 0) + (has_cto ? 4 : 0);
                std::size_t max_table = (plen >= 12) ? ((plen - 12) / 4) : 0;
                std::size_t max_entries = (entry_stride > 0 && off <= plen) ? ((plen - off) / entry_stride) : 0;
                std::size_t safe_count = std::min<std::size_t>({count, max_table, max_entries});
                if (frag_sample_sizes.size() + safe_count > kMaxMp4SampleCount) {
                    malformed = true;
                    break;
                }
                for (std::size_t i = 0; i < safe_count && off < plen; ++i) {
                    if (has_dur) off += 4;
                    std::uint32_t ssz = 0;
                    if (has_sz && off + 4 <= plen) {
                        ssz = ReadBe32(payload + off);
                        off += 4;
                        if (ssz > kMaxMp4SampleBytes) {
                            malformed = true;
                            break;
                        }
                    }
                    if (has_fl) off += 4;
                    if (has_cto) off += 4;
                    if (ssz > 0) frag_sample_sizes.push_back(ssz);
                }
            }
            pos += static_cast<std::size_t>(sz);
        }
    };

    std::vector<std::uint8_t> pkt;
    while (!cancel_flag.load() && !malformed) {
        std::array<std::uint8_t, 8> hdr{};
        if (!input.ReadExact(hdr.data(), 8)) break;

        std::uint64_t box_size = ReadBe32(hdr.data());
        std::uint32_t fourcc = ReadBe32(hdr.data() + 4);
        std::size_t hdr_len = 8;

        if (box_size == 1) {
            std::array<std::uint8_t, 8> ext{};
            if (!input.ReadExact(ext.data(), 8)) break;
            box_size = ReadBe64(ext.data());
            hdr_len = 16;
        }
        if (box_size > 0 && box_size < hdr_len) break;

        if (fourcc == 0x6D6F6F76 || fourcc == 0x6D6F6F66) { // moov or moof
            if (box_size > 16 * 1024 * 1024) {
                malformed = true;
                break;
            }
            std::vector<std::uint8_t> box(static_cast<std::size_t>(box_size - hdr_len));
            if (!input.ReadExact(box.data(), box.size())) break;
            saw_moov_or_moof = true;
            if (fourcc == 0x6D6F6F66) {
                frag_sample_sizes.clear();
                frag_default_sample_size = 0;
            }
            parse_moov_or_moof(box.data(), box.size(), 0);
            if (malformed) break;
        } else if (fourcc == 0x6D646174) { // mdat
            if (!saw_moov_or_moof && prog_sample_sizes.empty() && prog_fixed_count == 0 && frag_sample_sizes.empty()) {
                Logger::Instance().Error("MP4 stream has 'moov' box after 'mdat' (non-faststart progressive MP4 cannot be streamed).");
                return false;
            }
            if (!configured) {
                // Default AAC-LC 44.1kHz stereo ASC fallback if esds wasn't parsed
                aac.ConfigureRaw({0x12, 0x10});
                configured = true;
            }
            bool has_frag = !frag_sample_sizes.empty();
            bool has_prog_table = !has_frag && !prog_sample_sizes.empty();
            bool has_prog_fixed = !has_frag && !has_prog_table && prog_fixed_count > 0;
            if (has_frag || has_prog_table || has_prog_fixed) {
                std::uint64_t consumed = 0;
                std::uint64_t mdat_len = (box_size > hdr_len) ? (box_size - hdr_len) : UINT64_MAX;
                auto feed_sample = [&](std::uint32_t ssz) -> bool {
                    if (ssz > kMaxMp4SampleBytes) {
                        malformed = true;
                        return false;
                    }
                    if (cancel_flag.load() || consumed + ssz > mdat_len) return false;
                    pkt.resize(ssz);
                    if (!input.ReadExact(pkt.data(), pkt.size())) return false;
                    consumed += ssz;
                    return aac.FeedAndDecode(pkt.data(), pkt.size(), pipeline);
                };

                if (has_frag || has_prog_table) {
                    const auto& sizes = has_frag ? frag_sample_sizes : prog_sample_sizes;
                    for (std::uint32_t ssz : sizes) {
                        if (!feed_sample(ssz)) {
                            if (malformed) break;
                            break;
                        }
                    }
                } else {
                    for (std::uint32_t i = 0; i < prog_fixed_count; ++i) {
                        if (!feed_sample(prog_fixed_size)) {
                            break;
                        }
                    }
                }
                if (malformed) break;
                if (mdat_len != UINT64_MAX && consumed < mdat_len) {
                    input.Skip(mdat_len - consumed);
                }
            } else {
                // Unknown sample boundaries (skip mdat if moov appears at end)
                if (box_size > hdr_len) {
                    input.Skip(box_size - hdr_len);
                } else {
                    break;
                }
            }
        } else {
            if (box_size > hdr_len) {
                if (!input.Skip(box_size - hdr_len)) break;
            } else if (box_size == 0) {
                break;
            }
        }
    }
    if (malformed) {
        Logger::Instance().Error("Malformed MP4 container rejected safely.");
        return false;
    }
    return pipeline.TotalPcmFrames() > 0;
}

bool ReadNextTsPacket(PullByteStream& input, std::array<std::uint8_t, 188>& pkt, std::atomic<bool>& cancel_flag) {
    while (!cancel_flag.load()) {
        std::uint8_t b = 0;
        if (!input.ReadExact(&b, 1)) return false;
        if (b != 0x47) continue;
        pkt[0] = 0x47;
        return input.ReadExact(pkt.data() + 1, 187);
    }
    return false;
}

bool DecodeMpegTsStream(PullByteStream& input, Mp3OutputPipeline& pipeline, std::atomic<bool>& cancel_flag) {
    enum class TsAudioCodec { Unknown, AacAdts, AacLatm, MpegAudio };

    TsAudioCodec detected_codec = TsAudioCodec::Unknown;
    std::uint16_t pmt_pid = 0xFFFF;
    std::uint16_t audio_pid = 0xFFFF;
    int pmt_version = -1;
    bool force_pmt_reinit = false;

    std::unique_ptr<FdkAacHelper> aac;
    drmp3dec mp3_dec{};
    bool mp3_dec_inited = false;
    std::vector<std::uint8_t> mp3_pes_accum;
    std::array<drmp3_int16, 1152 * 2> mp3_pcm{};

    constexpr std::size_t kMaxPesBufferBytes = 512 * 1024;
    std::array<std::uint8_t, 188> pkt{};
    std::vector<std::uint8_t> pes_buf;
    bool awaiting_pusi = false;

    auto decode_mpeg_audio = [&](const std::uint8_t* data, std::size_t len) -> bool {
        if (!mp3_dec_inited) {
            drmp3dec_init(&mp3_dec);
            mp3_dec_inited = true;
        }
        mp3_pes_accum.insert(mp3_pes_accum.end(), data, data + len);
        std::size_t off = 0;
        while (off < mp3_pes_accum.size()) {
            drmp3dec_frame_info info{};
            int samples = drmp3dec_decode_frame(
                &mp3_dec,
                mp3_pes_accum.data() + off,
                static_cast<int>(mp3_pes_accum.size() - off),
                mp3_pcm.data(),
                &info);
            if (info.frame_bytes <= 0) {
                break;
            }
            off += static_cast<std::size_t>(info.frame_bytes);
            if (samples > 0 && info.sample_rate > 0 && info.channels > 0) {
                if (!pipeline.PushPcmInterleaved(mp3_pcm.data(), static_cast<std::size_t>(samples), info.sample_rate, info.channels)) {
                    return false;
                }
            }
        }
        if (off > 0) {
            mp3_pes_accum.erase(mp3_pes_accum.begin(), mp3_pes_accum.begin() + static_cast<std::ptrdiff_t>(off));
        }
        if (mp3_pes_accum.size() > 65536) {
            mp3_pes_accum.clear();
        }
        return true;
    };

    auto flush_pes = [&]() -> bool {
        if (pes_buf.size() < 9) {
            pes_buf.clear();
            return true;
        }
        if (pes_buf[0] == 0x00 && pes_buf[1] == 0x00 && pes_buf[2] == 0x01) {
            std::size_t hdr_len = 9 + pes_buf[8];
            if (hdr_len < pes_buf.size()) {
                const std::uint8_t* payload = pes_buf.data() + hdr_len;
                std::size_t plen = pes_buf.size() - hdr_len;

                if (detected_codec == TsAudioCodec::Unknown && plen >= 2) {
                    if (payload[0] == 0xFF && (payload[1] & 0xF6) == 0xF0) {
                        detected_codec = TsAudioCodec::AacAdts;
                    } else if (payload[0] == 0x56 && (payload[1] & 0xE0) == 0xE0) {
                        detected_codec = TsAudioCodec::AacLatm;
                    } else if (payload[0] == 0xFF && (payload[1] & 0xE0) == 0xE0) {
                        detected_codec = TsAudioCodec::MpegAudio;
                    }
                }

                if (detected_codec == TsAudioCodec::MpegAudio) {
                    return decode_mpeg_audio(payload, plen);
                }
                if (!aac) {
                    TRANSPORT_TYPE tt = (detected_codec == TsAudioCodec::AacLatm) ? TT_MP4_LOAS : TT_MP4_ADTS;
                    aac = std::make_unique<FdkAacHelper>(tt);
                }
                return aac->FeedAndDecode(payload, plen, pipeline);
            }
        }
        pes_buf.clear();
        return true;
    };

    while (!cancel_flag.load()) {
        if (!ReadNextTsPacket(input, pkt, cancel_flag)) break;

        bool pusi = (pkt[1] & 0x40) != 0;
        std::uint16_t pid = static_cast<std::uint16_t>(((pkt[1] & 0x1F) << 8) | pkt[2]);
        std::uint8_t afc = (pkt[3] >> 4) & 0x03;

        auto reset_stream_decoder = [&](bool require_pusi) -> bool {
            if (!pes_buf.empty()) {
                if (!flush_pes()) return false;
                pes_buf.clear();
            }
            mp3_pes_accum.clear();
            mp3_dec_inited = false;
            aac.reset();
            if (require_pusi) {
                awaiting_pusi = true;
            }
            return true;
        };

        std::size_t off = 4;
        if (afc == 2 || afc == 3) {
            std::uint8_t adapt_len = pkt[4];
            if (adapt_len >= 1 && (pkt[5] & 0x80) != 0 && audio_pid != 0xFFFF && pid == audio_pid) {
                if (!reset_stream_decoder(!pusi)) break;
            }
            off = 5 + adapt_len;
        }
        if (afc == 2 || afc == 0) continue;
        if (off >= 188) continue;

        // Parse PAT (PID 0x0000) to locate PMT PID
        if (pid == 0x0000 && pusi && off + 1 < 188) {
            std::size_t ptr = off + 1 + pkt[off]; // pointer_field
            if (ptr + 12 <= 188 && pkt[ptr] == 0x00) {
                std::size_t sec_len = static_cast<std::size_t>(((pkt[ptr + 1] & 0x0F) << 8) | pkt[ptr + 2]);
                std::size_t pos = ptr + 8;
                std::size_t end = (sec_len >= 4) ? std::min<std::size_t>(188, ptr + 3 + sec_len - 4) : 0;
                while (pos + 4 <= end) {
                    std::uint16_t prog = static_cast<std::uint16_t>((pkt[pos] << 8) | pkt[pos + 1]);
                    std::uint16_t p = static_cast<std::uint16_t>(((pkt[pos + 2] & 0x1F) << 8) | pkt[pos + 3]);
                    if (prog != 0) {
                        if (p != pmt_pid) {
                            if (pmt_pid != 0xFFFF) {
                                force_pmt_reinit = true;
                            }
                            pmt_pid = p;
                            pmt_version = -1;
                        }
                        break;
                    }
                    pos += 4;
                }
            }
            continue;
        }

        // Parse PMT to identify audio PID and stream_type (supports mid-stream PID/codec switches)
        if (pmt_pid != 0xFFFF && pid == pmt_pid && pusi && off + 1 < 188) {
            std::size_t ptr = off + 1 + pkt[off];
            if (ptr + 12 <= 188 && pkt[ptr] == 0x02) {
                std::size_t sec_len = static_cast<std::size_t>(((pkt[ptr + 1] & 0x0F) << 8) | pkt[ptr + 2]);
                int ver = static_cast<int>((pkt[ptr + 5] >> 1) & 0x1F);
                std::size_t prog_info_len = static_cast<std::size_t>(((pkt[ptr + 10] & 0x0F) << 8) | pkt[ptr + 11]);
                std::size_t pos = ptr + 12 + prog_info_len;
                std::size_t end = (sec_len >= 4) ? std::min<std::size_t>(188, ptr + 3 + sec_len - 4) : 0;
                std::uint16_t new_audio_pid = 0xFFFF;
                TsAudioCodec new_codec = TsAudioCodec::Unknown;
                while (pos + 5 <= end) {
                    std::uint8_t stype = pkt[pos];
                    std::uint16_t epid = static_cast<std::uint16_t>(((pkt[pos + 1] & 0x1F) << 8) | pkt[pos + 2]);
                    std::size_t es_info_len = static_cast<std::size_t>(((pkt[pos + 3] & 0x0F) << 8) | pkt[pos + 4]);
                    if (stype == 0x0F) {
                        new_audio_pid = epid;
                        new_codec = TsAudioCodec::AacAdts;
                        break;
                    } else if (stype == 0x11) {
                        new_audio_pid = epid;
                        new_codec = TsAudioCodec::AacLatm;
                        break;
                    } else if (stype == 0x03 || stype == 0x04) {
                        new_audio_pid = epid;
                        new_codec = TsAudioCodec::MpegAudio;
                        break;
                    }
                    pos += 5 + es_info_len;
                }
                if (new_audio_pid != 0xFFFF) {
                    const bool switched =
                        (audio_pid != 0xFFFF) &&
                        (force_pmt_reinit || new_audio_pid != audio_pid || new_codec != detected_codec ||
                         (pmt_version >= 0 && ver != pmt_version));
                    if (switched) {
                        Logger::Instance().Debug("MPEG-TS PMT changed mid-stream; resetting audio decoder state.");
                        if (!reset_stream_decoder(new_audio_pid != audio_pid)) break;
                    }
                    audio_pid = new_audio_pid;
                    detected_codec = new_codec;
                    pmt_version = ver;
                    force_pmt_reinit = false;
                }
            }
            continue;
        }

        // Fallback if PAT/PMT weren't present at start of live segment
        if (audio_pid == 0xFFFF && pusi && off + 4 < 188) {
            if (pkt[off] == 0x00 && pkt[off + 1] == 0x00 && pkt[off + 2] == 0x01 &&
                (pkt[off + 3] >= 0xC0 && pkt[off + 3] <= 0xDF)) {
                audio_pid = pid;
            }
        }

        if (pid == audio_pid) {
            if (pusi) {
                awaiting_pusi = false;
                if (!pes_buf.empty()) {
                    if (!flush_pes()) break;
                    pes_buf.clear();
                }
            }
            if (awaiting_pusi) continue;
            if (pes_buf.size() + (188 - off) > kMaxPesBufferBytes) {
                Logger::Instance().Warn("MPEG-TS PES buffer exceeded 512 KB without PUSI; discarding until next PUSI.");
                pes_buf.clear();
                awaiting_pusi = true;
                continue;
            }
            pes_buf.insert(pes_buf.end(), pkt.begin() + static_cast<std::ptrdiff_t>(off), pkt.end());
        }
    }
    flush_pes();
    return pipeline.TotalPcmFrames() > 0;
}

// ---------------- HLS (.m3u8) Segment Fetcher ----------------

bool IsHlsUrl(const TrackMetadata& meta) {
    auto has_m3u8 = [](const std::string& s) {
        std::string lower = s;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return lower.find(".m3u8") != std::string::npos;
    };
    return meta.container_ext == "m3u8" || has_m3u8(meta.direct_media_url);
}

std::string ResolveRelativeUrl(const std::string& base_url, const std::string& rel) {
    if (rel.rfind("http://", 0) == 0 || rel.rfind("https://", 0) == 0) {
        return rel;
    }
    if (rel.rfind("//", 0) == 0) {
        auto scheme_end = base_url.find("://");
        std::string scheme = (scheme_end != std::string::npos) ? base_url.substr(0, scheme_end) : "https";
        return scheme + ":" + rel;
    }
    auto scheme_end = base_url.find("://");
    auto path_start = (scheme_end != std::string::npos) ? base_url.find('/', scheme_end + 3) : std::string::npos;
    if (!rel.empty() && rel[0] == '/') {
        if (path_start != std::string::npos) {
            return base_url.substr(0, path_start) + rel;
        }
        return base_url + rel;
    }
    auto last_slash = base_url.rfind('/');
    if (last_slash != std::string::npos && (scheme_end == std::string::npos || last_slash > scheme_end + 2)) {
        return base_url.substr(0, last_slash + 1) + rel;
    }
    return base_url + "/" + rel;
}

struct HlsSegment {
    std::int64_t seq{0};
    std::string url;
    std::string map_url;
    bool discontinuity{false};
};

std::string ExtractHlsAttr(const std::string& line, const char* key) {
    std::string k = std::string(key) + "=";
    auto pos = line.find(k);
    while (pos != std::string::npos) {
        if (pos == 0 || line[pos - 1] == ',' || line[pos - 1] == ':' || line[pos - 1] == ' ') {
            break;
        }
        pos = line.find(k, pos + 1);
    }
    if (pos == std::string::npos) return "";
    pos += k.size();
    if (pos >= line.size()) return "";
    if (line[pos] == '"') {
        ++pos;
        auto end = line.find('"', pos);
        if (end == std::string::npos) return "";
        return line.substr(pos, end - pos);
    }
    auto end = line.find(',', pos);
    std::string val = (end == std::string::npos) ? line.substr(pos) : line.substr(pos, end - pos);
    while (!val.empty() && (val.back() == ' ' || val.back() == '\t' || val.back() == '\r')) val.pop_back();
    return val;
}

bool HlsCodecsHaveVideo(const std::string& codecs) {
    std::string lower = codecs;
    for (char& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return lower.find("avc") != std::string::npos ||
           lower.find("hev") != std::string::npos ||
           lower.find("hvc") != std::string::npos ||
           lower.find("vp0") != std::string::npos ||
           lower.find("vp8") != std::string::npos ||
           lower.find("vp9") != std::string::npos ||
           lower.find("av01") != std::string::npos ||
           lower.find("mp4v") != std::string::npos;
}

std::string StripUrlQueryPathOnly(const std::string& url) {
    auto q = url.find_first_of("?#");
    return (q == std::string::npos) ? url : url.substr(0, q);
}

void RunHlsDownloader(
    const TrackMetadata& meta,
    const TranscoderConfig& cfg,
    PullByteStream& pull_stream,
    const std::atomic<bool>& cancel_flag,
    std::string& out_error,
    std::chrono::steady_clock::time_point run_start) {
    std::string playlist_url = meta.direct_media_url;

    auto sleep_interruptible = [&](int total_ms) {
        for (int elapsed = 0; elapsed < total_ms && !cancel_flag.load() && !pull_stream.IsClosed(); elapsed += 100) {
            std::this_thread::sleep_for(std::chrono::milliseconds(std::min(100, total_ms - elapsed)));
        }
    };

    const int poll_interval_ms = static_cast<int>(cfg.hls_poll_interval_ms > 0 ? cfg.hls_poll_interval_ms : 2000U);
    const auto stall_timeout = std::chrono::milliseconds(cfg.hls_live_stall_timeout_ms > 0 ? cfg.hls_live_stall_timeout_ms : 30000U);

    int consecutive_failures = 0;
    auto last_success_at = std::chrono::steady_clock::now();
    auto last_segment_delivered_at = last_success_at;
    auto last_skip_warn_at = std::chrono::steady_clock::time_point{};
    std::optional<std::int64_t> last_seq_downloaded;
    std::optional<std::int64_t> last_media_sequence;
    std::optional<std::int64_t> last_discontinuity_sequence;
    std::size_t last_segment_count = 0;
    std::string last_pushed_map_url;
    bool logged_playlist_summary = false;
    bool logged_first_http = false;

    while (!cancel_flag.load() && !pull_stream.IsClosed()) {
        auto resp = HttpClient::Get(playlist_url, meta.http_headers, 10000, &cancel_flag);
        if (cancel_flag.load() || pull_stream.IsClosed()) {
            out_error.clear();
            break;
        }
        if (!resp.Ok()) {
            if (resp.error == "URL not allowed") {
                out_error = resp.error;
                break;
            }
            ++consecutive_failures;
            auto now = std::chrono::steady_clock::now();
            if (consecutive_failures >= 3 || (now - last_success_at) >= std::chrono::seconds(15)) {
                out_error = resp.error.empty() ? ("HLS HTTP " + std::to_string(resp.status_code)) : resp.error;
                break;
            }
            sleep_interruptible(1000);
            continue;
        }

        if (!logged_first_http) {
            logged_first_http = true;
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - run_start).count();
            Logger::Instance().Debug(
                "HLS first playlist response HTTP " + std::to_string(resp.status_code) +
                " (" + std::to_string(resp.body.size()) + " bytes) at +" + std::to_string(ms) + " ms");
        }

        // Parse media/master playlist (#EXT-X-STREAM-INF, #EXT-X-MEDIA, #EXT-X-MAP, #EXT-X-MEDIA-SEQUENCE, #EXT-X-DISCONTINUITY, #EXT-X-ENDLIST)
        struct MasterVariant {
            std::string url;
            std::uint64_t bandwidth{0};
            bool has_video{false};
        };
        std::istringstream iss(resp.body);
        std::string line;
        bool is_master = false;
        bool end_list = false;
        bool awaiting_variant_uri = false;
        MasterVariant pending_variant{};
        std::vector<MasterVariant> master_variants;
        std::string best_audio_media_uri;
        bool found_default_audio_media = false;
        std::int64_t media_sequence = 0;
        std::optional<std::int64_t> discontinuity_sequence;
        bool next_seg_discontinuity = false;
        std::string current_map_url;
        std::vector<HlsSegment> segments;

        while (std::getline(iss, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
                line.pop_back();
            }
            if (line.empty()) continue;
            if (line.rfind("#EXT-X-MEDIA:", 0) == 0) {
                if (ExtractHlsAttr(line, "TYPE") == "AUDIO") {
                    std::string uri = ExtractHlsAttr(line, "URI");
                    if (!uri.empty()) {
                        is_master = true;
                        bool is_def = (ExtractHlsAttr(line, "DEFAULT") == "YES");
                        if (best_audio_media_uri.empty() || (is_def && !found_default_audio_media)) {
                            best_audio_media_uri = ResolveRelativeUrl(playlist_url, uri);
                            if (is_def) found_default_audio_media = true;
                        }
                    }
                }
                continue;
            }
            if (line.rfind("#EXT-X-STREAM-INF", 0) == 0) {
                is_master = true;
                awaiting_variant_uri = true;
                pending_variant = MasterVariant{};
                std::string bw = ExtractHlsAttr(line, "BANDWIDTH");
                if (!bw.empty()) {
                    pending_variant.bandwidth = std::strtoull(bw.c_str(), nullptr, 10);
                }
                std::string codecs = ExtractHlsAttr(line, "CODECS");
                pending_variant.has_video = !codecs.empty() && HlsCodecsHaveVideo(codecs);
                continue;
            }
            if (line.rfind("#EXT-X-MAP:", 0) == 0) {
                std::string uri = ExtractHlsAttr(line, "URI");
                if (!uri.empty()) {
                    current_map_url = ResolveRelativeUrl(playlist_url, uri);
                }
                continue;
            }
            if (line.rfind("#EXT-X-ENDLIST", 0) == 0) {
                end_list = true;
                continue;
            }
            if (line.rfind("#EXT-X-MEDIA-SEQUENCE:", 0) == 0) {
                const char* p = line.c_str() + 22;
                while (*p == ' ' || *p == '\t') ++p;
                char* endptr = nullptr;
                long long val = std::strtoll(p, &endptr, 10);
                if (endptr != p && val >= 0) {
                    media_sequence = static_cast<std::int64_t>(val);
                }
                continue;
            }
            if (line.rfind("#EXT-X-DISCONTINUITY-SEQUENCE:", 0) == 0) {
                const char* p = line.c_str() + 30;
                while (*p == ' ' || *p == '\t') ++p;
                char* endptr = nullptr;
                long long val = std::strtoll(p, &endptr, 10);
                if (endptr != p && val >= 0) {
                    discontinuity_sequence = static_cast<std::int64_t>(val);
                }
                continue;
            }
            if (line == "#EXT-X-DISCONTINUITY") {
                next_seg_discontinuity = true;
                continue;
            }
            if (line[0] == '#') continue;

            if (awaiting_variant_uri) {
                pending_variant.url = ResolveRelativeUrl(playlist_url, line);
                if (!pending_variant.url.empty()) {
                    master_variants.push_back(std::move(pending_variant));
                }
                awaiting_variant_uri = false;
                continue;
            }
            if (is_master) {
                continue;
            }
            HlsSegment seg;
            seg.seq = media_sequence + static_cast<std::int64_t>(segments.size());
            seg.url = ResolveRelativeUrl(playlist_url, line);
            seg.map_url = current_map_url;
            seg.discontinuity = next_seg_discontinuity;
            next_seg_discontinuity = false;
            segments.push_back(std::move(seg));
        }

        if (is_master) {
            std::string chosen_variant;
            if (!best_audio_media_uri.empty()) {
                chosen_variant = best_audio_media_uri;
            } else if (!master_variants.empty()) {
                const MasterVariant* best = nullptr;
                for (const auto& v : master_variants) {
                    if (!v.has_video) {
                        if (!best || best->has_video || v.bandwidth > best->bandwidth) {
                            best = &v;
                        }
                    } else if (!best) {
                        best = &v;
                    } else if (best->has_video && v.bandwidth > 0 && (best->bandwidth == 0 || v.bandwidth < best->bandwidth)) {
                        best = &v;
                    }
                }
                if (best) chosen_variant = best->url;
            }
            if (!chosen_variant.empty()) {
                Logger::Instance().Debug(
                    "HLS master playlist resolved -> " + StripUrlQueryPathOnly(chosen_variant) +
                    " (variants=" + std::to_string(master_variants.size()) + ")");
                playlist_url = std::move(chosen_variant);
                continue;
            }
        }

        if (!logged_playlist_summary) {
            logged_playlist_summary = true;
            Logger::Instance().Debug(
                std::string("HLS media playlist parsed: mode=") + (end_list ? "VOD" : "LIVE") +
                ", segments=" + std::to_string(segments.size()) +
                ", MEDIA-SEQUENCE=" + std::to_string(media_sequence) +
                (current_map_url.empty() ? std::string() : ", has EXT-X-MAP"));
        }

        bool stale_response = false;
        if (!segments.empty()) {
            if (!last_seq_downloaded.has_value()) {
                // First media playlist read: live starts at the live edge (last 3 segments); VOD starts from index 0
                std::size_t start_idx = (!end_list && segments.size() > 3) ? (segments.size() - 3) : 0;
                last_seq_downloaded = segments[start_idx].seq - 1;
            } else if (!end_list) {
                const std::int64_t last = *last_seq_downloaded;
                const std::int64_t first_seq = segments.front().seq;
                const std::int64_t back_seq = segments.back().seq;
                const std::int64_t max_stale_lag = std::min<std::int64_t>(
                    std::max<std::int64_t>(static_cast<std::int64_t>(last_segment_count), 1), 10);

                // Differentiate transient CDN edge lag from a real encoder restart
                if (last_media_sequence.has_value() &&
                    media_sequence < *last_media_sequence &&
                    (*last_media_sequence - media_sequence) <= max_stale_lag &&
                    back_seq >= *last_media_sequence) {
                    stale_response = true;
                } else if (last_media_sequence.has_value() &&
                           media_sequence == *last_media_sequence &&
                           back_seq < last &&
                           (last - back_seq) <= std::min<std::int64_t>(max_stale_lag, 3) &&
                           back_seq >= *last_media_sequence) {
                    stale_response = true;
                }

                if (stale_response) {
                    Logger::Instance().Debug(
                        "Ignoring stale HLS live playlist response from CDN edge.");
                } else {
                    const bool seq_rewound =
                        (back_seq < last) ||
                        (last_media_sequence.has_value() && media_sequence < *last_media_sequence);
                    const bool seq_jumped_far = (first_seq > last) && ((first_seq - last) > 4);
                    const bool dseq_changed =
                        discontinuity_sequence.has_value() && last_discontinuity_sequence.has_value() &&
                        (*discontinuity_sequence != *last_discontinuity_sequence);
                    if (dseq_changed) {
                        Logger::Instance().Debug(
                            "HLS live discontinuity sequence incremented (" +
                            std::to_string(*last_discontinuity_sequence) + " -> " +
                            std::to_string(*discontinuity_sequence) + "); continuing sequentially.");
                    }
                    if (seq_rewound || seq_jumped_far) {
                        Logger::Instance().Warn(
                            "HLS live sequence rewind or jump detected; repositioning to live edge.");
                        std::size_t edge_idx = (segments.size() > 3) ? (segments.size() - 3) : 0;
                        if (!seq_rewound) {
                            while (edge_idx < segments.size() && segments[edge_idx].seq <= last) {
                                ++edge_idx;
                            }
                        }
                        if (edge_idx < segments.size()) {
                            last_seq_downloaded = segments[edge_idx].seq - 1;
                        } else {
                            last_seq_downloaded = back_seq;
                        }
                    }
                }
            }
            if (!stale_response) {
                last_media_sequence = media_sequence;
                last_segment_count = segments.size();
                if (discontinuity_sequence.has_value()) {
                    last_discontinuity_sequence = discontinuity_sequence;
                }
            }
        }

        bool fetched_any = false;
        bool fatal_error = false;
        for (const auto& seg : segments) {
            if (stale_response) {
                break;
            }
            if (cancel_flag.load() || pull_stream.IsClosed()) {
                out_error.clear();
                fatal_error = true;
                break;
            }
            if (last_seq_downloaded.has_value() && seg.seq <= *last_seq_downloaded) {
                continue;
            }
            if (seg.discontinuity) {
                Logger::Instance().Debug(
                    "HLS segment seq " + std::to_string(seg.seq) +
                    " marked with #EXT-X-DISCONTINUITY; continuing sequentially.");
            }

            fetched_any = true;

            if (!seg.map_url.empty() && seg.map_url != last_pushed_map_url) {
                bool map_ok = false;
                std::string map_err;
                std::size_t map_bytes = 0;
                for (int attempt = 0; attempt < 2; ++attempt) {
                    if (cancel_flag.load() || pull_stream.IsClosed()) {
                        out_error.clear();
                        fatal_error = true;
                        break;
                    }
                    map_err.clear();
                    map_bytes = 0;
                    map_ok = HttpClient::StreamGet(
                        seg.map_url,
                        meta.http_headers,
                        [&](const std::uint8_t* data, std::size_t size) {
                            map_bytes += size;
                            return pull_stream.Push(data, size, cancel_flag);
                        },
                        cancel_flag,
                        map_err);
                    if (map_ok || cancel_flag.load() || pull_stream.IsClosed() || map_err == "URL not allowed") {
                        break;
                    }
                }
                if (fatal_error || cancel_flag.load() || pull_stream.IsClosed()) {
                    out_error.clear();
                    fatal_error = true;
                    break;
                }
                if (!map_ok) {
                    out_error = map_err.empty() ? "HLS EXT-X-MAP init segment failed" : map_err;
                    fatal_error = true;
                    break;
                }
                last_pushed_map_url = seg.map_url;
                Logger::Instance().Debug(
                    "HLS EXT-X-MAP init segment fetched (" + std::to_string(map_bytes) +
                    " bytes): " + StripUrlQueryPathOnly(seg.map_url));
            }

            bool seg_ok = false;
            std::string seg_err;
            std::size_t seg_bytes = 0;
            // Up to 1 immediate retry for both live and VOD segments
            for (int attempt = 0; attempt < 2; ++attempt) {
                if (cancel_flag.load() || pull_stream.IsClosed()) {
                    out_error.clear();
                    fatal_error = true;
                    break;
                }
                seg_err.clear();
                seg_bytes = 0;
                seg_ok = HttpClient::StreamGet(
                    seg.url,
                    meta.http_headers,
                    [&](const std::uint8_t* data, std::size_t size) {
                        seg_bytes += size;
                        return pull_stream.Push(data, size, cancel_flag);
                    },
                    cancel_flag,
                    seg_err);
                if (seg_ok || cancel_flag.load() || pull_stream.IsClosed() || seg_err == "URL not allowed") {
                    break;
                }
            }

            if (cancel_flag.load() || pull_stream.IsClosed()) {
                out_error.clear();
                fatal_error = true;
                break;
            }
            if (seg_err == "URL not allowed") {
                out_error = seg_err;
                fatal_error = true;
                break;
            }

            if (seg_ok) {
                Logger::Instance().Debug(
                    "HLS segment seq " + std::to_string(seg.seq) + " received " +
                    std::to_string(seg_bytes) + " bytes");
                last_seq_downloaded = seg.seq;
                consecutive_failures = 0;
                auto now = std::chrono::steady_clock::now();
                last_success_at = now;
                last_segment_delivered_at = now;
                continue;
            }

            if (end_list) {
                // VOD HLS: after 1 retry, fail with error
                out_error = seg_err.empty() ? "HLS segment transfer failed" : seg_err;
                fatal_error = true;
                break;
            }

            // Live HLS: advance sequence so skipped segment is not retried on subsequent polls
            last_seq_downloaded = seg.seq;
            ++consecutive_failures;
            auto now = std::chrono::steady_clock::now();
            if (last_skip_warn_at == std::chrono::steady_clock::time_point{} ||
                (now - last_skip_warn_at) >= std::chrono::seconds(10)) {
                last_skip_warn_at = now;
                Logger::Instance().Warn(
                    "HLS live segment failed after retry; skipping segment (" +
                    (seg_err.empty() ? std::string("transfer failed") : seg_err) + ")");
            }
            if (consecutive_failures >= 3 || (now - last_success_at) >= std::chrono::seconds(15)) {
                out_error = seg_err.empty() ? "HLS segment transfer failed" : seg_err;
                fatal_error = true;
                break;
            }
        }

        if (fatal_error) {
            break;
        }
        if (end_list) {
            break;
        }
        if (!fetched_any) {
            auto now = std::chrono::steady_clock::now();
            if (consecutive_failures > 0 && (now - last_success_at) >= std::chrono::seconds(15)) {
                out_error = "HLS live stream stalled for 15s without new segments";
                break;
            }
            if (consecutive_failures == 0 && (now - last_segment_delivered_at) >= stall_timeout) {
                // Live stream ended or frozen without errors: finish cleanly as normal EOF
                out_error.clear();
                break;
            }
            sleep_interruptible(poll_interval_ms);
        }
    }
    pull_stream.MarkEof();
}

} // namespace

struct AudioTranscoder::Impl {
    TrackMetadata metadata;
    TranscoderConfig config;
    std::shared_ptr<AudioStreamBuffer> output_buffer;
};

AudioTranscoder::AudioTranscoder(
    const TrackMetadata& metadata,
    const TranscoderConfig& config,
    std::shared_ptr<AudioStreamBuffer> output_buffer)
    : impl_(std::make_unique<Impl>()) {
    impl_->metadata = metadata;
    impl_->config = config;
    impl_->output_buffer = std::move(output_buffer);
}

AudioTranscoder::~AudioTranscoder() = default;

bool AudioTranscoder::Run(
    std::atomic<bool>& cancel_flag,
    std::atomic<bool>& pause_flag,
    std::atomic<float>& volume_percent,
    std::string& out_error,
    ErrorCode* out_error_code) {
    const auto& meta = impl_->metadata;
    const auto& cfg = impl_->config;

    // Fast Passthrough path ONLY for live MP3 radios when volume is 100% and no pacing is needed
    bool likely_mp3 = (meta.container_ext == "mp3" || meta.codec == "mp3");
    if (likely_mp3 && meta.is_live && !cfg.pace_realtime && std::abs(volume_percent.load() - 100.0f) < 0.5f) {
        bool ok = HttpClient::StreamGet(
            meta.direct_media_url,
            meta.http_headers,
            [&](const std::uint8_t* data, std::size_t size) {
                while (pause_flag.load() && !cancel_flag.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                if (cancel_flag.load()) return false;
                impl_->output_buffer->Write(data, size, cfg.buffer_generation);
                return true;
            },
            cancel_flag,
            out_error);
        if (!cancel_flag.load()) {
            impl_->output_buffer->MarkEof(cfg.buffer_generation);
        }
        if (!ok && !cancel_flag.load()) {
            if (out_error.empty()) {
                out_error = "Live audio stream disconnected unexpectedly";
            }
            if (out_error_code) {
                *out_error_code = (out_error == "URL not allowed") ? ErrorCode::InvalidUrl : ErrorCode::NetworkError;
            }
            return false;
        }
        return true;
    }

    const auto run_start = std::chrono::steady_clock::now();
    TrackMetadata current_meta = impl_->metadata;
    bool re_resolved_across_passes = false;
    std::string final_dl_error;
    std::uint64_t final_pcm_frames = 0;
    bool final_timed_out_no_audio = false;
    bool ok = false;

    auto format_http_attempt_error = [](
        int status,
        const std::string& fallback_err,
        int attempt_idx,
        int max_attempts,
        std::uint64_t total_rx,
        std::int64_t expected_len,
        bool after_re_resolve) {
        if (status >= 400) {
            std::ostringstream oss;
            oss << "HTTP status " << status;
            if (after_re_resolve) {
                oss << " after re-resolve";
            }
            oss << " (attempt " << (attempt_idx + 1) << "/" << max_attempts << ", ";
            if (attempt_idx > 0) {
                oss << "Range resume at byte ";
            } else {
                oss << "byte ";
            }
            oss << total_rx;
            if (expected_len > 0) {
                oss << "/" << expected_len;
            }
            oss << ")";
            return oss.str();
        }
        if (after_re_resolve && !fallback_err.empty()) {
            return fallback_err + " after re-resolve";
        }
        return fallback_err;
    };

    for (int pipeline_pass = 0; pipeline_pass < 2 && !cancel_flag.load(); ++pipeline_pass) {
        std::atomic<bool> effective_cancel{cancel_flag.load()};
        std::atomic<bool> timed_out_no_audio{false};
        std::atomic<bool> watchdog_stop{false};
        std::atomic<bool> downloader_in_deliberate_wait{false};
        std::atomic<bool> restart_pipeline{false};
        std::atomic<std::uint64_t> decoded_pcm_frames{0};
        std::atomic<std::uint32_t> download_attempt_epoch{0};
        TrackMetadata restart_meta;

        struct DeliberateWaitGuard {
            std::atomic<bool>& flag;
            explicit DeliberateWaitGuard(std::atomic<bool>& f) : flag(f) {
                flag.store(true, std::memory_order_release);
            }
            ~DeliberateWaitGuard() {
                flag.store(false, std::memory_order_release);
            }
            DeliberateWaitGuard(const DeliberateWaitGuard&) = delete;
            DeliberateWaitGuard& operator=(const DeliberateWaitGuard&) = delete;
        };

        PullByteStream pull_stream(512 * 1024, &effective_cancel);
        std::string dl_error;
        bool hls = IsHlsUrl(current_meta);
        Logger::Instance().Debug(
            "Transcoder starting (hls=" + std::string(hls ? "true" : "false") +
            ", live=" + std::string(current_meta.is_live ? "true" : "false") +
            ", url=" + StripUrlQueryPathOnly(current_meta.direct_media_url) + ")");

        ManagedThread watchdog;
        watchdog.Start([&]() {
            std::uint64_t unpaused_zero_ms = 0;
            std::uint32_t seen_attempt_epoch = download_attempt_epoch.load(std::memory_order_acquire);
            auto last_check = std::chrono::steady_clock::now();
            bool prev_ext_cancel = false;
            while (!watchdog_stop.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
                if (cancel_flag.load()) {
                    if (!prev_ext_cancel) {
                        prev_ext_cancel = true;
                        effective_cancel.store(true);
                        pull_stream.Close();
                        HttpClient::PollCancellations();
                    }
                    break;
                }
                auto now = std::chrono::steady_clock::now();
                auto step_ms = static_cast<std::uint64_t>(std::max<long long>(
                    0, std::chrono::duration_cast<std::chrono::milliseconds>(now - last_check).count()));
                last_check = now;

                const std::uint32_t cur_epoch = download_attempt_epoch.load(std::memory_order_acquire);
                if (cur_epoch != seen_attempt_epoch) {
                    seen_attempt_epoch = cur_epoch;
                    unpaused_zero_ms = 0;
                }

                if (decoded_pcm_frames.load(std::memory_order_acquire) == 0 &&
                    !pause_flag.load() &&
                    !downloader_in_deliberate_wait.load(std::memory_order_acquire)) {
                    unpaused_zero_ms += step_ms;
                    if (cfg.no_audio_timeout_ms > 0 && unpaused_zero_ms >= cfg.no_audio_timeout_ms) {
                        Logger::Instance().Warn(
                            "Transcoder produced 0 decoded PCM frames within " +
                            std::to_string(cfg.no_audio_timeout_ms) + " ms; aborting stream.");
                        timed_out_no_audio.store(true);
                        effective_cancel.store(true);
                        pull_stream.Close();
                        HttpClient::PollCancellations();
                        break;
                    }
                }
            }
        }, 256 * 1024);

        ManagedThread downloader;
        struct PullStreamCloseGuard {
            PullByteStream& stream;
            std::atomic<bool>& wd_stop;
            ManagedThread& wd;
            ~PullStreamCloseGuard() {
                stream.Close();
                wd_stop.store(true);
                if (wd.Joinable()) wd.Join();
            }
        } close_guard{pull_stream, watchdog_stop, watchdog};

        if (!downloader.Start([&]() {
            if (hls) {
                RunHlsDownloader(current_meta, cfg, pull_stream, effective_cancel, dl_error, run_start);
            } else {
                constexpr int kMaxRangeRetries = 2;
                constexpr int kMaxTransientRetries = 2;
                TrackMetadata active_meta = current_meta;
                bool already_re_resolved = re_resolved_across_passes;
                bool early_403_same_url_retried = false;
                std::string last_ab_probe_url;
                std::uint64_t total_rx = 0;
                std::int64_t expected_len = -1;
                std::int64_t prev_expected_len = -1;
                bool logged_first_http = false;
                bool attempt_is_range = false;
                bool check_reresolved_length = false;
                int attempt = 0;
                int transient_retries = 0;

                auto wait_cancelable_ms = [&](std::int64_t wait_ms) -> bool {
                    if (wait_ms <= 0) return true;
                    DeliberateWaitGuard wait_guard(downloader_in_deliberate_wait);
                    const auto dl = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
                    while (!effective_cancel.load() && !pull_stream.IsClosed()) {
                        auto now_tp = std::chrono::steady_clock::now();
                        if (now_tp >= dl) return true;
                        auto rem = std::chrono::duration_cast<std::chrono::milliseconds>(dl - now_tp).count();
                        std::this_thread::sleep_for(std::chrono::milliseconds(std::min<long long>(rem, 50)));
                    }
                    return false;
                };

                while (!effective_cancel.load()) {
                    const int max_attempts = active_meta.is_live ? 1 : (1 + kMaxRangeRetries);
                    std::string attempt_err;
                    int resp_status = 0;
                    bool unsupported_resume = false;
                    bool reresolved_len_mismatch = false;
                    HttpClient::HeaderMap hdrs = active_meta.http_headers;
                    if (attempt_is_range) {
                        hdrs["Range"] = "bytes=" + std::to_string(total_rx) + "-";
                    }
                    HttpRequestDiagnostics http_diag;

                    const bool use_fresh_session = already_re_resolved || (transient_retries > 0);
                    const int connect_timeout = static_cast<int>(
                        cfg.stream_connect_timeout_ms > 0 ? cfg.stream_connect_timeout_ms : 15000);
                    bool dl_ok = HttpClient::StreamGet(
                        active_meta.direct_media_url,
                        hdrs,
                        [&](const std::uint8_t* data, std::size_t size) {
                            if (unsupported_resume || reresolved_len_mismatch) {
                                return false;
                            }
                            if (attempt_is_range && resp_status != 206) {
                                unsupported_resume = true;
                                return false;
                            }
                            total_rx += size;
                            return pull_stream.Push(data, size, effective_cancel);
                        },
                        effective_cancel,
                        attempt_err,
                        [&](int st, const std::string&, std::int64_t cl) {
                            resp_status = st;
                            if (!logged_first_http) {
                                logged_first_http = true;
                                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - run_start).count();
                                Logger::Instance().Debug(
                                    "First HTTP response HTTP " + std::to_string(st) +
                                    " (Content-Length=" + std::to_string(cl) + ") at +" +
                                    std::to_string(ms) + " ms");
                            }
                            if (attempt_is_range && st >= 200 && st < 300) {
                                if (st != 206) {
                                    unsupported_resume = true;
                                } else if (check_reresolved_length && prev_expected_len > 0 && cl > 0 &&
                                           total_rx + static_cast<std::uint64_t>(cl) != static_cast<std::uint64_t>(prev_expected_len)) {
                                    reresolved_len_mismatch = true;
                                }
                            } else if (!attempt_is_range && st >= 200 && st < 300 && cl > 0) {
                                expected_len = cl;
                                if (!active_meta.is_live) {
                                    std::uint64_t max_pf = static_cast<std::uint64_t>(
                                        std::clamp<std::uint32_t>(cfg.max_prefetch_mb, 1, 64)) * 1024ULL * 1024ULL;
                                    std::size_t desired_cap = static_cast<std::size_t>(
                                        std::min<std::uint64_t>(static_cast<std::uint64_t>(cl), max_pf));
                                    pull_stream.EnsureCapacity(desired_cap);
                                }
                            }
                        },
                        connect_timeout,
                        &http_diag,
                        use_fresh_session);

                    if (resp_status >= 400 && Logger::Instance().IsDebugEnabled()) {
                        const auto ref_tp = (active_meta.resolved_at != std::chrono::steady_clock::time_point{})
                                                ? active_meta.resolved_at
                                                : run_start;
                        const auto since_resolve_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - ref_tp).count();
                        const std::string req_host = ExtractHostOnly(active_meta.direct_media_url);
                        const std::string redir_info = http_diag.redirected
                            ? ("true (redirect_host=" + (http_diag.final_host.empty() ? std::string("unknown") : http_diag.final_host) + ")")
                            : std::string("false");
                        const std::string reused_str = http_diag.connection_reused_known
                            ? (http_diag.connection_reused ? "true" : "false")
                            : "unknown";
                        Logger::Instance().Debug(
                            "HTTP error details: status=" + std::to_string(resp_status) +
                            ", attempt=" + std::to_string(attempt + 1) + "/" + std::to_string(max_attempts) +
                            ", host=" + req_host +
                            ", since_resolve_ms=" + std::to_string(since_resolve_ms) +
                            ", redirected=" + redir_info +
                            ", reused_conn=" + reused_str +
                            ", had_cookie=" + std::string(http_diag.had_cookie_header ? "true" : "false") +
                            ", body_preview=\"" + SanitizePrintablePreview(http_diag.error_body_snippet, 200) + "\"");
                        if (!http_diag.sent_headers_summary.empty()) {
                            Logger::Instance().Debug(
                                "HTTP request headers sent: " + http_diag.sent_headers_summary);
                        }
                        if (active_meta.source_type == SourceType::ExtractorStream) {
                            const std::filesystem::path base_p(
                                cfg.base_dir.empty() ? std::string("scriptfiles/SampAudioStream") : cfg.base_dir);
                            const std::filesystem::path dump_path = base_p / "debug" / "last_rejected_media_request.txt";
                            std::error_code ec;
                            std::filesystem::create_directories(dump_path.parent_path(), ec);
                            std::ofstream ofs(dump_path, std::ios::trunc);
                            if (ofs.is_open()) {
                                const std::time_t now_t = std::time(nullptr);
                                std::tm tm_local{};
#ifdef _WIN32
                                localtime_s(&tm_local, &now_t);
#else
                                localtime_r(&now_t, &tm_local);
#endif
                                char time_buf[32]{};
                                std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_local);
                                ofs << "timestamp: " << time_buf << "\n"
                                    << "status: " << resp_status << "\n"
                                    << "format_id: " << (active_meta.format_id.empty() ? "none" : active_meta.format_id) << "\n"
                                    << "js_runtime: " << (active_meta.js_runtime_used.empty() ? "unknown" : active_meta.js_runtime_used) << "\n"
                                    << "url: " << active_meta.direct_media_url << "\n"
                                    << "headers:\n";
                                for (const auto& [hk, hv] : hdrs) {
                                    std::string kl = hk;
                                    for (char& ch : kl) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                                    if (kl == "cookie" || kl == "authorization") continue;
                                    ofs << "  " << hk << ": " << hv << "\n";
                                }
                                ofs.close();
                                Logger::Instance().Debug(
                                    "Saved rejected media request details to " + dump_path.string() +
                                    " (contains signed IP-bound URL; do not share publicly).");
                            }
                        }
                        // Defer A/B probe when we are about to wait 5 s and retry the exact same URL once on early 403.
                        const bool defer_ab_for_early_403 =
                            (resp_status == 403) &&
                            active_meta.source_type == SourceType::ExtractorStream &&
                            active_meta.available_at_epoch_ms <= 0 &&
                            !early_403_same_url_retried &&
                            attempt == 0 &&
                            total_rx == 0 &&
                            since_resolve_ms < 10000;
                        if (active_meta.source_type == SourceType::ExtractorStream &&
                            !already_re_resolved &&
                            !defer_ab_for_early_403 &&
                            last_ab_probe_url != active_meta.direct_media_url &&
                            !effective_cancel.load() &&
                            !pull_stream.IsClosed()) {
                            last_ab_probe_url = active_meta.direct_media_url;
                            HttpAbProbeResult ab = HttpClient::ProbeUrlSharedVsFreshForDebug(
                                active_meta.direct_media_url,
                                active_meta.http_headers,
                                &effective_cancel,
                                5000);
                            auto fmt_ab = [](int st, const std::string& e) -> std::string {
                                if (st > 0) return std::to_string(st);
                                return e.empty() ? std::string("0") : ("err(" + SanitizePrintablePreview(e, 60) + ")");
                            };
                            Logger::Instance().Debug(
                                "HTTP " + std::to_string(resp_status) +
                                " A/B: shared_session_no_headers=" +
                                fmt_ab(ab.shared_no_headers_status, ab.shared_no_headers_error) +
                                ", fresh_session_same_headers=" +
                                fmt_ab(ab.fresh_same_headers_status, ab.fresh_same_headers_error));
                        }
                    }

                    if (effective_cancel.load() || pull_stream.IsClosed()) {
                        dl_error.clear();
                        break;
                    }
                    if (attempt_err == "URL not allowed") {
                        dl_error = attempt_err;
                        break;
                    }

                    if (reresolved_len_mismatch || unsupported_resume) {
                        if (reresolved_len_mismatch && decoded_pcm_frames.load(std::memory_order_acquire) == 0) {
                            restart_meta = active_meta;
                            restart_pipeline.store(true);
                            pull_stream.Close();
                            break;
                        }
                        if (reresolved_len_mismatch) {
                            dl_error = "Media size mismatch after HTTP re-resolve";
                            break;
                        }
                    }

                    bool incomplete = (expected_len > 0 && total_rx < static_cast<std::uint64_t>(expected_len));
                    if (dl_ok && !incomplete) {
                        dl_error.clear();
                        break;
                    }

                    const auto ref_tp = (active_meta.resolved_at != std::chrono::steady_clock::time_point{})
                                            ? active_meta.resolved_at
                                            : run_start;
                    const auto since_resolve_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - ref_tp).count();
                    const bool can_early_403_same_url_retry =
                        (resp_status == 403) &&
                        active_meta.source_type == SourceType::ExtractorStream &&
                        active_meta.available_at_epoch_ms <= 0 &&
                        !early_403_same_url_retried &&
                        attempt == 0 &&
                        total_rx == 0 &&
                        since_resolve_ms < 10000;
                    if (can_early_403_same_url_retry) {
                        early_403_same_url_retried = true;
                        Logger::Instance().Warn(
                            "Early HTTP 403 without available_at (" + std::to_string(since_resolve_ms) +
                            " ms since resolve); waiting 5 s and retrying the same URL once");
                        if (!wait_cancelable_ms(5000)) {
                            dl_error.clear();
                            break;
                        }
                        continue;
                    }

                    const bool can_re_resolve =
                        (resp_status == 403 || resp_status == 410) &&
                        active_meta.source_type == SourceType::ExtractorStream &&
                        !already_re_resolved &&
                        static_cast<bool>(cfg.re_resolve_callback);
                    if (can_re_resolve) {
                        already_re_resolved = true;
                        re_resolved_across_passes = true;
                        const std::string log_id = active_meta.id.empty() ? active_meta.original_url : active_meta.id;
                        Logger::Instance().Warn(
                            "Media URL rejected with HTTP " + std::to_string(resp_status) +
                            "; re-resolving " + log_id + " once");
                        TrackMetadata new_meta;
                        std::string rr_err;
                        bool rr_ok = false;
                        {
                            DeliberateWaitGuard wait_guard(downloader_in_deliberate_wait);
                            rr_ok = cfg.re_resolve_callback(active_meta, effective_cancel, new_meta, rr_err);
                        }
                        if (!rr_ok) {
                            if (effective_cancel.load() || pull_stream.IsClosed()) {
                                dl_error.clear();
                            } else {
                                dl_error = format_http_attempt_error(
                                    resp_status,
                                    attempt_err,
                                    attempt,
                                    max_attempts,
                                    total_rx,
                                    expected_len,
                                    true);
                                if (!rr_err.empty()) {
                                    dl_error += " (" + rr_err + ")";
                                }
                            }
                            break;
                        }
                        if (new_meta.available_at_epoch_ms > 0) {
                            const auto now_epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch()).count();
                            const std::int64_t delta_ms = new_meta.available_at_epoch_ms - now_epoch_ms;
                            if (delta_ms > 60000) {
                                Logger::Instance().Warn(
                                    "Extractor reported implausible available_at (" + std::to_string(delta_ms) +
                                    " ms in future) after re-resolve; ignoring wait");
                            } else {
                                const std::int64_t wait_ms = (new_meta.available_at_epoch_ms + 300) - now_epoch_ms;
                                if (wait_ms > 0) {
                                    Logger::Instance().Debug(
                                        "Waiting " + std::to_string(wait_ms) +
                                        " ms until re-resolved URL available_at + 300 ms");
                                    if (!wait_cancelable_ms(wait_ms)) {
                                        dl_error.clear();
                                        break;
                                    }
                                }
                            }
                        }
                        early_403_same_url_retried = false;
                        if (total_rx == 0) {
                            active_meta = std::move(new_meta);
                            expected_len = -1;
                            prev_expected_len = -1;
                            attempt = 0;
                            transient_retries = 0;
                            attempt_is_range = false;
                            check_reresolved_length = false;
                            download_attempt_epoch.fetch_add(1, std::memory_order_release);
                            continue;
                        }
                        const bool same_format = (active_meta.format_id == new_meta.format_id);
                        if (same_format && !new_meta.is_live) {
                            prev_expected_len = expected_len;
                            active_meta = std::move(new_meta);
                            attempt = 1;
                            attempt_is_range = true;
                            check_reresolved_length = true;
                            continue;
                        }
                        if (decoded_pcm_frames.load(std::memory_order_acquire) == 0) {
                            restart_meta = std::move(new_meta);
                            restart_pipeline.store(true);
                            pull_stream.Close();
                            break;
                        }
                        dl_error = "Media format changed after HTTP " + std::to_string(resp_status) + " re-resolve";
                        break;
                    }

                    if (attempt_err.empty() && incomplete) {
                        attempt_err = "Network stream ended prematurely";
                    }
                    if (total_rx == 0 &&
                        http_diag.transient_transport_error &&
                        resp_status < 400 &&
                        transient_retries < kMaxTransientRetries) {
                        ++transient_retries;
                        download_attempt_epoch.fetch_add(1, std::memory_order_release);
                        const std::string reason = attempt_err.empty() ? std::string("transient transport error") : attempt_err;
                        Logger::Instance().Debug(
                            "Media download attempt failed before first byte (" + reason +
                            "); retrying same URL (" + std::to_string(transient_retries) + "/" +
                            std::to_string(kMaxTransientRetries) + ")");
                        const std::int64_t backoff_ms = static_cast<std::int64_t>(
                            (transient_retries == 1) ? cfg.retry_backoff_first_ms : cfg.retry_backoff_second_ms);
                        if (!wait_cancelable_ms(backoff_ms)) {
                            dl_error.clear();
                            break;
                        }
                        download_attempt_epoch.fetch_add(1, std::memory_order_release);
                        continue;
                    }
                    if (resp_status >= 400 || active_meta.is_live || expected_len <= 0 || total_rx == 0 || unsupported_resume || attempt + 1 >= max_attempts) {
                        dl_error = format_http_attempt_error(
                            resp_status,
                            attempt_err.empty() ? "Network stream interrupted" : attempt_err,
                            attempt,
                            max_attempts,
                            total_rx,
                            expected_len,
                            already_re_resolved);
                        if (transient_retries > 0 && resp_status < 400) {
                            dl_error += " (after " + std::to_string(transient_retries + 1) + " attempts)";
                        }
                        break;
                    }

                    Logger::Instance().Warn(
                        "Stream interrupted at byte " + std::to_string(total_rx) + "/" +
                        std::to_string(expected_len) + "; attempting Range resume (" +
                        std::to_string(attempt + 1) + "/" + std::to_string(kMaxRangeRetries) + ")");
                    for (int w = 0; w < 10 && !effective_cancel.load(); ++w) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(25));
                    }
                    ++attempt;
                    attempt_is_range = true;
                    check_reresolved_length = false;
                }
                pull_stream.MarkEof();
            }
        }, 256 * 1024)) {
            out_error = "Failed to spawn downloader thread";
            if (out_error_code) *out_error_code = ErrorCode::InternalError;
            if (!cancel_flag.load()) {
                impl_->output_buffer->MarkEof(cfg.buffer_generation);
            }
            return false;
        }

        Mp3OutputPipeline pipeline(cfg, impl_->output_buffer, effective_cancel, pause_flag, volume_percent, run_start, &decoded_pcm_frames);

        // Sniff magic bytes from stream head
        std::vector<std::uint8_t> magic;
        ok = false;
        try {
            if (pull_stream.Peek(magic, 16) && !magic.empty()) {
                if (magic.size() >= 4 && magic[0] == 0x1A && magic[1] == 0x45 && magic[2] == 0xDF && magic[3] == 0xA3) {
                    Logger::Instance().Debug("Detected container: WebM/EBML (Opus/Vorbis)");
                    ok = DecodeWebmStream(pull_stream, pipeline, effective_cancel);
                } else if (magic.size() >= 4 && std::memcmp(magic.data(), "OggS", 4) == 0) {
                    Logger::Instance().Debug("Detected container: Ogg");
                    ok = DecodeOggStream(pull_stream, pipeline, effective_cancel);
                } else if (magic.size() >= 8 && std::memcmp(magic.data() + 4, "ftyp", 4) == 0) {
                    Logger::Instance().Debug("Detected container: MP4/fMP4");
                    ok = DecodeMp4Stream(pull_stream, pipeline, effective_cancel);
                } else if (magic[0] == 0x47 && hls) {
                    Logger::Instance().Debug("Detected container: MPEG-TS");
                    ok = DecodeMpegTsStream(pull_stream, pipeline, effective_cancel);
                } else if (magic.size() >= 2 && magic[0] == 0xFF && ((magic[1] & 0xF6) == 0xF0)) {
                    Logger::Instance().Debug("Detected container: ADTS AAC");
                    ok = DecodeAdtsStream(pull_stream, pipeline, effective_cancel);
                } else if (magic.size() >= 10 && magic[0] == 'I' && magic[1] == 'D' && magic[2] == '3') {
                    std::size_t id3_len = ParseId3v2TagSize(magic.data(), magic.size());
                    bool is_id3_adts = false;
                    if (id3_len > 0 && id3_len <= 128 * 1024) {
                        std::vector<std::uint8_t> ext_peek;
                        if (pull_stream.Peek(ext_peek, id3_len + 16) && ext_peek.size() >= id3_len + 2) {
                            if (ext_peek[id3_len] == 0xFF && (ext_peek[id3_len + 1] & 0xF6) == 0xF0) {
                                is_id3_adts = true;
                            }
                        }
                    }
                    if (is_id3_adts) {
                        Logger::Instance().Debug("Detected container: ID3v2-wrapped ADTS AAC (HLS packed audio)");
                        ok = DecodeAdtsStream(pull_stream, pipeline, effective_cancel);
                    } else {
                        Logger::Instance().Debug("Detected container: MP3 (with ID3v2)");
                        ok = DecodeMp3Stream(pull_stream, pipeline, effective_cancel);
                    }
                } else if (magic.size() >= 4 && std::memcmp(magic.data(), "RIFF", 4) == 0) {
                    Logger::Instance().Debug("Detected container: WAV/RIFF");
                    ok = DecodeWavStream(pull_stream, pipeline, effective_cancel);
                } else if (magic.size() >= 4 && std::memcmp(magic.data(), "fLaC", 4) == 0) {
                    Logger::Instance().Debug("Detected container: FLAC");
                    ok = DecodeFlacStream(pull_stream, pipeline, effective_cancel);
                } else {
                    Logger::Instance().Debug("Detected container: MP3 (raw frames / fallback)");
                    ok = DecodeMp3Stream(pull_stream, pipeline, effective_cancel);
                }
            } else {
                out_error = dl_error.empty() ? "Empty or unreachable media stream" : dl_error;
            }
        } catch (const std::exception& ex) {
            ok = false;
            out_error = std::string("Decoder exception: ") + ex.what();
            Logger::Instance().Error("AudioTranscoder decoder exception: " + out_error);
        } catch (...) {
            ok = false;
            out_error = "Unknown decoder exception";
            Logger::Instance().Error("AudioTranscoder unknown decoder exception.");
        }

        watchdog_stop.store(true);
        pull_stream.Close();
        if (downloader.Joinable()) {
            downloader.Join();
        }
        if (watchdog.Joinable()) {
            watchdog.Join();
        }

        if (restart_pipeline.load() && !cancel_flag.load() && pipeline.TotalPcmFrames() == 0 && pipeline_pass == 0) {
            pipeline.CloseWithoutFlush();
            current_meta = std::move(restart_meta);
            re_resolved_across_passes = true;
            out_error.clear();
            continue;
        }

        if (cancel_flag.load()) {
            pipeline.CloseWithoutFlush();
        } else {
            pipeline.FlushAndClose();
            impl_->output_buffer->MarkEof(cfg.buffer_generation);
        }
        final_dl_error = dl_error;
        final_pcm_frames = pipeline.TotalPcmFrames();
        final_timed_out_no_audio = timed_out_no_audio.load();
        break;
    }

    if (!cancel_flag.load() && (final_timed_out_no_audio || !final_dl_error.empty() || !ok || final_pcm_frames == 0)) {
        ok = false;
        if (final_timed_out_no_audio) {
            out_error = "No decodable audio frames received within " +
                        std::to_string(std::max<std::uint32_t>(1, cfg.no_audio_timeout_ms / 1000)) + "s";
            if (out_error_code) *out_error_code = ErrorCode::TranscodeError;
        } else if (!final_dl_error.empty()) {
            // Surface transport interruption so truncated tracks do not silently fire OnAudioStreamFinished
            out_error = final_dl_error;
            if (out_error_code) {
                *out_error_code = (out_error == "URL not allowed") ? ErrorCode::InvalidUrl : ErrorCode::NetworkError;
            }
        } else {
            if (out_error.empty()) {
                out_error = "Unsupported codec or zero decoded audio samples (frames=" +
                            std::to_string(final_pcm_frames) + ")";
            }
            if (out_error_code) {
                *out_error_code = (out_error == "URL not allowed") ? ErrorCode::InvalidUrl : ErrorCode::TranscodeError;
            }
        }
    }
    return ok || cancel_flag.load();
}

bool RunNormalizedPipelineForTesting(
    const TranscoderConfig& config,
    float volume_percent,
    int sample_rate,
    int channels,
    const std::vector<std::vector<std::int16_t>>& chunks,
    std::vector<std::uint8_t>& out_mp3_bytes,
    std::vector<std::int16_t>* out_pre_encode_pcm,
    float* out_final_gain_db) {
    out_mp3_bytes.clear();
    if (out_pre_encode_pcm) {
        out_pre_encode_pcm->clear();
    }
    if (sample_rate <= 0 || channels <= 0) {
        return false;
    }

    auto out_buf = std::make_shared<AudioStreamBuffer>(2 * 1024 * 1024);
    std::atomic<bool> cancel{false};
    std::atomic<bool> pause{false};
    std::atomic<float> vol{volume_percent};
    const auto now = std::chrono::steady_clock::now();

    Mp3OutputPipeline pipeline(config, out_buf, cancel, pause, vol, now, nullptr);
    pipeline.SetCaptureStereoPcmForTesting(out_pre_encode_pcm);

    for (const auto& chunk : chunks) {
        if (chunk.empty()) continue;
        const std::size_t frames = chunk.size() / static_cast<std::size_t>(channels);
        if (frames == 0) continue;
        if (!pipeline.PushPcmInterleaved(chunk.data(), frames, sample_rate, channels)) {
            return false;
        }
    }
    pipeline.FlushAndClose();
    if (out_final_gain_db) {
        *out_final_gain_db = pipeline.CurrentNormGainDb();
    }
    out_buf->MarkEof(config.buffer_generation);

    const std::uint64_t total = out_buf->TotalWritten();
    out_mp3_bytes.resize(static_cast<std::size_t>(total));
    if (total > 0) {
        const std::size_t copied = out_buf->PeekAt(0, out_mp3_bytes.data(), out_mp3_bytes.size());
        out_mp3_bytes.resize(copied);
    }
    return true;
}

} // namespace sampaudiostream
