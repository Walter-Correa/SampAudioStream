#include "sampaudiostream/audio/transcoder.hpp"

#include "sampaudiostream/core/logger.hpp"
#include "sampaudiostream/network/http_client.hpp"

#include <aacdecoder_lib.h>
#include <lame.h>
#include <ogg/ogg.h>
#include <opus.h>

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
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace sampaudiostream {

namespace {

// Bounded thread-safe pull stream fed by HttpClient::StreamGet
class PullByteStream {
public:
    explicit PullByteStream(std::size_t max_buffer_bytes = 1024 * 1024)
        : max_capacity_(max_buffer_bytes) {}

    bool Push(const std::uint8_t* data, std::size_t size, const std::atomic<bool>& cancel_flag) {
        if (!data || size == 0) return true;
        std::unique_lock<std::mutex> lock(mutex_);
        cv_space_.wait(lock, [this, &cancel_flag]() {
            return queue_.size() < max_capacity_ || closed_ || cancel_flag.load();
        });
        if (closed_ || cancel_flag.load()) {
            return false;
        }
        queue_.insert(queue_.end(), data, data + size);
        cv_data_.notify_all();
        return true;
    }

    void MarkEof() {
        std::lock_guard<std::mutex> lock(mutex_);
        eof_ = true;
        cv_data_.notify_all();
    }

    void Close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        eof_ = true;
        cv_data_.notify_all();
        cv_space_.notify_all();
    }

    std::size_t ReadSome(std::uint8_t* out, std::size_t max_bytes) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_data_.wait(lock, [this]() {
            return !queue_.empty() || eof_ || closed_;
        });
        if (queue_.empty()) {
            return 0;
        }
        std::size_t n = std::min(max_bytes, queue_.size());
        std::copy_n(queue_.begin(), n, out);
        queue_.erase(queue_.begin(), queue_.begin() + static_cast<std::ptrdiff_t>(n));
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
        cv_data_.wait(lock, [this, count]() {
            return queue_.size() >= count || eof_ || closed_;
        });
        std::size_t avail = std::min(count, queue_.size());
        out.resize(avail);
        std::copy_n(queue_.begin(), avail, out.begin());
        return avail > 0;
    }

    bool Skip(std::size_t count) {
        std::array<std::uint8_t, 4096> scratch{};
        std::size_t remaining = count;
        while (remaining > 0) {
            std::size_t chunk = std::min(remaining, scratch.size());
            if (!ReadExact(scratch.data(), chunk)) return false;
            remaining -= chunk;
        }
        return true;
    }

    std::uint64_t TotalConsumed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return total_consumed_;
    }

private:
    std::size_t max_capacity_;
    std::deque<std::uint8_t> queue_;
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
        std::atomic<float>& volume_percent)
        : cfg_(cfg),
          out_buf_(std::move(out_buf)),
          cancel_flag_(cancel_flag),
          pause_flag_(pause_flag),
          volume_percent_(volume_percent),
          start_time_(std::chrono::steady_clock::now()) {}

    ~Mp3OutputPipeline() {
        FlushAndClose();
    }

    bool Init(int input_sample_rate, int input_channels) {
        if (lame_) return true;

        input_rate_ = input_sample_rate > 0 ? input_sample_rate : 48000;
        input_channels_ = (input_channels == 1) ? 1 : 2;

        // If pace_realtime (shared radio channel), normalize all tracks to cfg_.target_sample_rate
        // Otherwise use source rate if it's a valid MPEG sample rate to avoid resampling overhead
        int enc_rate = static_cast<int>(cfg_.target_sample_rate);
        if (!cfg_.pace_realtime && (input_rate_ == 44100 || input_rate_ == 48000 || input_rate_ == 32000)) {
            enc_rate = input_rate_;
        }
        output_rate_ = enc_rate;

        lame_ = lame_init();
        if (!lame_) return false;

        lame_set_in_samplerate(lame_, output_rate_);
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
        start_time_ = std::chrono::steady_clock::now();
        return true;
    }

    bool PushPcmInterleaved(const std::int16_t* samples, std::size_t frame_count, int sample_rate, int channels) {
        if (cancel_flag_.load()) return false;
        if (!Init(sample_rate, channels)) return false;

        // Handle pause state
        while (pause_flag_.load() && !cancel_flag_.load()) {
            if (cfg_.pace_realtime) {
                // Emit 20ms of silence on live radio channels so listeners don't time out
                std::vector<std::int16_t> silence(static_cast<std::size_t>(output_rate_ / 50) * 2, 0);
                EncodeStereo(silence.data(), silence.size() / 2);
                PaceOutput(silence.size() / 2);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                start_time_ = std::chrono::steady_clock::now();
                samples_emitted_ = 0;
            }
        }
        if (cancel_flag_.load()) return false;

        // Convert to stereo and apply volume + optional linear resample
        float vol = std::clamp(volume_percent_.load() / 100.0f, 0.0f, 2.0f);

        if (sample_rate == output_rate_) {
            stereo_scratch_.resize(frame_count * 2);
            if (channels == 1) {
                for (std::size_t i = 0; i < frame_count; ++i) {
                    std::int16_t s = ScaleSample(samples[i], vol);
                    stereo_scratch_[i * 2] = s;
                    stereo_scratch_[i * 2 + 1] = s;
                }
            } else {
                for (std::size_t i = 0; i < frame_count; ++i) {
                    stereo_scratch_[i * 2] = ScaleSample(samples[i * channels], vol);
                    stereo_scratch_[i * 2 + 1] = ScaleSample(samples[i * channels + 1], vol);
                }
            }
            if (!EncodeStereo(stereo_scratch_.data(), frame_count)) return false;
            PaceOutput(frame_count);
        } else {
            double ratio = static_cast<double>(sample_rate) / static_cast<double>(output_rate_);
            std::size_t out_frames = static_cast<std::size_t>(std::floor(static_cast<double>(frame_count) / ratio));
            if (out_frames == 0) return true;
            stereo_scratch_.resize(out_frames * 2);
            for (std::size_t i = 0; i < out_frames; ++i) {
                double src_pos = static_cast<double>(i) * ratio;
                std::size_t idx0 = static_cast<std::size_t>(src_pos);
                std::size_t idx1 = std::min(idx0 + 1, frame_count - 1);
                double frac = src_pos - static_cast<double>(idx0);

                auto interp = [&](int ch) -> std::int16_t {
                    std::size_t c_idx = static_cast<std::size_t>(ch < channels ? ch : 0);
                    double s0 = samples[idx0 * channels + c_idx];
                    double s1 = samples[idx1 * channels + c_idx];
                    double mixed = (s0 + (s1 - s0) * frac) * vol;
                    return static_cast<std::int16_t>(std::clamp(mixed, -32768.0, 32767.0));
                };

                stereo_scratch_[i * 2] = interp(0);
                stereo_scratch_[i * 2 + 1] = (channels >= 2) ? interp(1) : stereo_scratch_[i * 2];
            }
            if (!EncodeStereo(stereo_scratch_.data(), out_frames)) return false;
            PaceOutput(out_frames);
        }
        return true;
    }

    void FlushAndClose() {
        if (!lame_) return;
        std::array<std::uint8_t, 8192> mp3_buf{};
        int flushed = lame_encode_flush(lame_, mp3_buf.data(), static_cast<int>(mp3_buf.size()));
        if (flushed > 0 && out_buf_) {
            out_buf_->Write(mp3_buf.data(), static_cast<std::size_t>(flushed));
        }
        lame_close(lame_);
        lame_ = nullptr;
    }

private:
    static std::int16_t ScaleSample(std::int16_t s, float vol) {
        if (std::abs(vol - 1.0f) < 0.001f) return s;
        float v = static_cast<float>(s) * vol;
        return static_cast<std::int16_t>(std::clamp(v, -32768.0f, 32767.0f));
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
        if (encoded > 0 && out_buf_) {
            out_buf_->Write(mp3_out_.data(), static_cast<std::size_t>(encoded));
        }
        return true;
    }

    void PaceOutput(std::size_t emitted_frames) {
        if (!cfg_.pace_realtime || output_rate_ <= 0) return;
        samples_emitted_ += emitted_frames;
        auto target_elapsed = std::chrono::duration<double>(
            static_cast<double>(samples_emitted_) / static_cast<double>(output_rate_));
        auto deadline = start_time_ + std::chrono::duration_cast<std::chrono::steady_clock::duration>(target_elapsed);
        // Allow up to 1.5s pre-buffer lead so network jitter never causes stuttering
        auto lead_adjusted = deadline - std::chrono::milliseconds(1500);
        auto now = std::chrono::steady_clock::now();
        if (lead_adjusted > now) {
            std::this_thread::sleep_until(lead_adjusted);
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
    std::vector<std::uint8_t> mp3_out_;
    std::uint64_t samples_emitted_{0};
    std::chrono::steady_clock::time_point start_time_;
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
    auto on_seek = [](void*, int, drmp3_seek_origin) -> drmp3_bool32 {
        return DRMP3_FALSE;
    };
    auto on_tell = [](void* pUserData, drmp3_int64* pCursor) -> drmp3_bool32 {
        if (pCursor) *pCursor = static_cast<drmp3_int64>(static_cast<Ctx*>(pUserData)->in->TotalConsumed());
        return DRMP3_TRUE;
    };

    drmp3 mp3{};
    if (!drmp3_init(&mp3, on_read, on_seek, on_tell, nullptr, &ctx, nullptr)) {
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
    auto on_seek = [](void*, int, drwav_seek_origin) -> drwav_bool32 { return DRWAV_FALSE; };
    auto on_tell = [](void* pUserData, drwav_int64* pCursor) -> drwav_bool32 {
        if (pCursor) *pCursor = static_cast<drwav_int64>(static_cast<Ctx*>(pUserData)->in->TotalConsumed());
        return DRWAV_TRUE;
    };

    drwav wav{};
    if (!drwav_init(&wav, on_read, on_seek, on_tell, &ctx, nullptr)) {
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
    auto on_seek = [](void*, int, drflac_seek_origin) -> drflac_bool32 { return DRFLAC_FALSE; };
    auto on_tell = [](void* pUserData, drflac_int64* pCursor) -> drflac_bool32 {
        if (pCursor) *pCursor = static_cast<drflac_int64>(static_cast<Ctx*>(pUserData)->in->TotalConsumed());
        return DRFLAC_TRUE;
    };

    drflac* flac = drflac_open(on_read, on_seek, on_tell, &ctx, nullptr);
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

// OGG container (Opus & Vorbis)
bool DecodeOggStream(PullByteStream& input, Mp3OutputPipeline& pipeline, std::atomic<bool>& cancel_flag) {
    ogg_sync_state oy;
    ogg_sync_init(&oy);
    ogg_stream_state os{};
    bool stream_inited = false;
    bool is_opus = false;
    bool is_vorbis = false;
    int headers_seen = 0;

    OpusDecoder* opus_dec = nullptr;
    int opus_channels = 2;

    std::vector<std::uint8_t> vorbis_header_buf;
    stb_vorbis* vorbis_dec = nullptr;
    stb_vorbis_info vorbis_info{};

    std::array<std::int16_t, 5760 * 2> pcm{};

    auto cleanup = [&]() {
        if (opus_dec) opus_decoder_destroy(opus_dec);
        if (vorbis_dec) stb_vorbis_close(vorbis_dec);
        if (stream_inited) ogg_stream_clear(&os);
        ogg_sync_clear(&oy);
    };

    while (!cancel_flag.load()) {
        ogg_page og{};
        while (ogg_sync_pageout(&oy, &og) == 1) {
            if (!stream_inited) {
                ogg_stream_init(&os, ogg_page_serialno(&og));
                stream_inited = true;
            }
            ogg_stream_pagein(&os, &og);
            ogg_packet op{};
            while (ogg_stream_packetout(&os, &op) == 1) {
                if (headers_seen == 0) {
                    if (op.bytes >= 8 && std::memcmp(op.packet, "OpusHead", 8) == 0) {
                        is_opus = true;
                        opus_channels = (op.bytes >= 10 && op.packet[9] == 1) ? 1 : 2;
                        int err = 0;
                        opus_dec = opus_decoder_create(48000, opus_channels, &err);
                        headers_seen = 1;
                    } else if (op.bytes >= 7 && std::memcmp(op.packet, "\x01vorbis", 7) == 0) {
                        is_vorbis = true;
                        vorbis_header_buf.insert(vorbis_header_buf.end(), op.packet, op.packet + op.bytes);
                        headers_seen = 1;
                    }
                    continue;
                }

                if (is_opus) {
                    if (headers_seen == 1 && op.bytes >= 8 && std::memcmp(op.packet, "OpusTags", 8) == 0) {
                        headers_seen = 2;
                        continue;
                    }
                    if (opus_dec) {
                        int frames = opus_decode(opus_dec, op.packet, static_cast<opus_int32>(op.bytes), pcm.data(), 5760, 0);
                        if (frames > 0) {
                            if (!pipeline.PushPcmInterleaved(pcm.data(), static_cast<std::size_t>(frames), 48000, opus_channels)) {
                                cleanup();
                                return true;
                            }
                        }
                    }
                } else if (is_vorbis) {
                    if (headers_seen < 3) {
                        vorbis_header_buf.insert(vorbis_header_buf.end(), op.packet, op.packet + op.bytes);
                        headers_seen++;
                        if (headers_seen == 3) {
                            // stb_vorbis_open_pushdata needs the 3 headers
                            int consumed = 0;
                            int err = 0;
                            vorbis_dec = stb_vorbis_open_pushdata(
                                vorbis_header_buf.data(),
                                static_cast<int>(vorbis_header_buf.size()),
                                &consumed,
                                &err,
                                nullptr);
                            if (vorbis_dec) {
                                vorbis_info = stb_vorbis_get_info(vorbis_dec);
                            }
                        }
                        continue;
                    }
                    if (vorbis_dec) {
                        int channels_out = 0;
                        float** outputs = nullptr;
                        int samples = 0;
                        stb_vorbis_decode_frame_pushdata(
                            vorbis_dec,
                            op.packet,
                            static_cast<int>(op.bytes),
                            &channels_out,
                            &outputs,
                            &samples);
                        if (samples > 0 && outputs) {
                            int ch = std::min(channels_out, 2);
                            std::vector<std::int16_t> inter(static_cast<std::size_t>(samples * ch));
                            for (int i = 0; i < samples; ++i) {
                                for (int c = 0; c < ch; ++c) {
                                    float v = std::clamp(outputs[c][i], -1.0f, 1.0f);
                                    inter[static_cast<std::size_t>(i * ch + c)] = static_cast<std::int16_t>(v * 32767.0f);
                                }
                            }
                            if (!pipeline.PushPcmInterleaved(inter.data(), static_cast<std::size_t>(samples), static_cast<int>(vorbis_info.sample_rate), ch)) {
                                cleanup();
                                return true;
                            }
                        }
                    }
                }
            }
        }

        char* buf = ogg_sync_buffer(&oy, 8192);
        std::size_t bytes = input.ReadSome(reinterpret_cast<std::uint8_t*>(buf), 8192);
        if (bytes == 0) break;
        ogg_sync_wrote(&oy, static_cast<long>(bytes));
    }

    cleanup();
    return true;
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

    OpusDecoder* opus_dec = nullptr;
    std::array<std::int16_t, 5760 * 2> pcm{};

    auto ensure_decoder = [&]() {
        if (has_audio_track && !opus_dec && audio_track.codec_id == "A_OPUS") {
            int err = 0;
            int ch = (audio_track.channels == 1) ? 1 : 2;
            opus_dec = opus_decoder_create(48000, ch, &err);
        }
    };

    auto decode_packet = [&](const std::uint8_t* pkt, std::size_t len) -> bool {
        ensure_decoder();
        if (opus_dec && audio_track.codec_id == "A_OPUS") {
            int ch = (audio_track.channels == 1) ? 1 : 2;
            int frames = opus_decode(opus_dec, pkt, static_cast<opus_int32>(len), pcm.data(), 5760, 0);
            if (frames > 0) {
                return pipeline.PushPcmInterleaved(pcm.data(), static_cast<std::size_t>(frames), 48000, ch);
            }
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
                current_track.codec_id.assign(reinterpret_cast<const char*>(data + pos), sz);
            } else if (id == ID_CODEC_PRIVATE) {
                current_track.codec_private.assign(data + pos, data + pos + sz);
            } else if (id == ID_AUDIO) {
                std::size_t apos = pos;
                std::size_t aend = pos + sz;
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
                    apos += asz;
                }
            }
            pos += sz;
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
            if (!input.Skip(static_cast<std::size_t>(sz))) break;
            continue;
        }

        if (id == ID_TRACK_ENTRY && sz < 1024 * 1024) {
            std::vector<std::uint8_t> buf(static_cast<std::size_t>(sz));
            if (!input.ReadExact(buf.data(), buf.size())) break;
            parse_track_entry(buf.data(), buf.size());
            continue;
        }

        if ((id == ID_SIMPLE_BLOCK || id == ID_BLOCK) && sz < 4 * 1024 * 1024) {
            std::vector<std::uint8_t> buf(static_cast<std::size_t>(sz));
            if (!input.ReadExact(buf.data(), buf.size())) break;
            if (!process_block(buf)) break;
            continue;
        }

        // Unknown / uninteresting leaf element (e.g. Timecode, Cues, Void, Tags)
        if (!input.Skip(static_cast<std::size_t>(sz))) break;
    }

    if (opus_dec) opus_decoder_destroy(opus_dec);
    return true;
}

// ---------------- AAC (ADTS & MP4/M4A & MPEG-TS) via FDK-AAC ----------------

class FdkAacHelper {
public:
    explicit FdkAacHelper(TRANSPORT_TYPE transport) {
        dec_ = aacDecoder_Open(transport, 1);
    }
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

        while (valid > 0) {
            if (aacDecoder_Fill(dec_, &ptr, &buf_size, &valid) != AAC_DEC_OK) {
                break;
            }
            while (true) {
                AAC_DECODER_ERROR err = aacDecoder_DecodeFrame(dec_, pcm_.data(), static_cast<INT>(pcm_.size()), 0);
                if (err == AAC_DEC_NOT_ENOUGH_BITS) break;
                if (err != AAC_DEC_OK) break;
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
            if (valid > 0 && valid == buf_size) break; // prevent infinite loop on invalid bytes
            ptr += (buf_size - valid);
            buf_size = valid;
        }
        return true;
    }

private:
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

bool DecodeAdtsStream(PullByteStream& input, Mp3OutputPipeline& pipeline, std::atomic<bool>& cancel_flag) {
    FdkAacHelper aac(TT_MP4_ADTS);
    std::array<std::uint8_t, 8192> buf{};
    while (!cancel_flag.load()) {
        std::size_t n = input.ReadSome(buf.data(), buf.size());
        if (n == 0) break;
        if (!aac.FeedAndDecode(buf.data(), n, pipeline)) break;
    }
    return true;
}

bool DecodeMp4Stream(PullByteStream& input, Mp3OutputPipeline& pipeline, std::atomic<bool>& cancel_flag) {
    FdkAacHelper aac(TT_MP4_RAW);
    bool configured = false;
    std::vector<std::uint32_t> prog_sample_sizes;
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

    std::function<void(const std::uint8_t*, std::size_t)> parse_moov_or_moof;
    parse_moov_or_moof = [&](const std::uint8_t* data, std::size_t len) {
        std::size_t pos = 0;
        while (pos + 8 <= len) {
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
                parse_moov_or_moof(payload, plen);
            } else if (fourcc == 0x73747364 && plen > 8) { // stsd
                // Sample entry starts at offset 8
                parse_moov_or_moof(payload + 8, plen - 8);
            } else if (fourcc == 0x6D703461 && plen > 28) { // mp4a
                parse_moov_or_moof(payload + 28, plen - 28);
            } else if (fourcc == 0x65736473 && plen > 4) { // esds
                auto asc = extract_esds_asc(payload + 4, plen - 4);
                if (!asc.empty() && !configured) {
                    configured = aac.ConfigureRaw(asc);
                }
            } else if (fourcc == 0x7374737A && plen >= 12) { // stsz
                std::uint32_t sample_size = ReadBe32(payload + 4);
                std::uint32_t count = ReadBe32(payload + 8);
                if (sample_size == 0) {
                    for (std::uint32_t i = 0; i < count && 12 + (i + 1) * 4 <= plen; ++i) {
                        prog_sample_sizes.push_back(ReadBe32(payload + 12 + i * 4));
                    }
                } else {
                    prog_sample_sizes.assign(count, sample_size);
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
                for (std::uint32_t i = 0; i < count && off < plen; ++i) {
                    if (has_dur) off += 4;
                    std::uint32_t ssz = 0;
                    if (has_sz && off + 4 <= plen) {
                        ssz = ReadBe32(payload + off);
                        off += 4;
                    }
                    if (has_fl) off += 4;
                    if (has_cto) off += 4;
                    if (ssz > 0) frag_sample_sizes.push_back(ssz);
                }
            }
            pos += static_cast<std::size_t>(sz);
        }
    };

    while (!cancel_flag.load()) {
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
            if (box_size > 16 * 1024 * 1024) break;
            std::vector<std::uint8_t> box(static_cast<std::size_t>(box_size - hdr_len));
            if (!input.ReadExact(box.data(), box.size())) break;
            if (fourcc == 0x6D6F6F66) frag_sample_sizes.clear();
            parse_moov_or_moof(box.data(), box.size());
        } else if (fourcc == 0x6D646174) { // mdat
            if (!configured) {
                // Default AAC-LC 44.1kHz stereo ASC fallback if esds wasn't parsed
                aac.ConfigureRaw({0x12, 0x10});
                configured = true;
            }
            const auto& sizes = !frag_sample_sizes.empty() ? frag_sample_sizes : prog_sample_sizes;
            if (!sizes.empty()) {
                std::size_t consumed = 0;
                std::size_t mdat_len = (box_size > hdr_len) ? static_cast<std::size_t>(box_size - hdr_len) : SIZE_MAX;
                for (std::uint32_t ssz : sizes) {
                    if (cancel_flag.load() || consumed + ssz > mdat_len) break;
                    std::vector<std::uint8_t> pkt(ssz);
                    if (!input.ReadExact(pkt.data(), pkt.size())) break;
                    consumed += ssz;
                    if (!aac.FeedAndDecode(pkt.data(), pkt.size(), pipeline)) return true;
                }
                if (mdat_len != SIZE_MAX && consumed < mdat_len) {
                    input.Skip(mdat_len - consumed);
                }
            } else {
                // Unknown sample boundaries (skip mdat if moov appears at end)
                if (box_size > hdr_len) {
                    input.Skip(static_cast<std::size_t>(box_size - hdr_len));
                } else {
                    break;
                }
            }
        } else {
            if (box_size > hdr_len) {
                if (!input.Skip(static_cast<std::size_t>(box_size - hdr_len))) break;
            } else if (box_size == 0) {
                break;
            }
        }
    }
    return true;
}

bool DecodeMpegTsStream(PullByteStream& input, Mp3OutputPipeline& pipeline, std::atomic<bool>& cancel_flag) {
    FdkAacHelper aac(TT_MP4_ADTS);
    std::uint16_t audio_pid = 0xFFFF;
    std::array<std::uint8_t, 188> pkt{};
    std::vector<std::uint8_t> pes_buf;

    auto flush_pes = [&]() -> bool {
        if (pes_buf.size() < 9) {
            pes_buf.clear();
            return true;
        }
        if (pes_buf[0] == 0x00 && pes_buf[1] == 0x00 && pes_buf[2] == 0x01) {
            std::size_t hdr_len = 9 + pes_buf[8];
            if (hdr_len < pes_buf.size()) {
                return aac.FeedAndDecode(pes_buf.data() + hdr_len, pes_buf.size() - hdr_len, pipeline);
            }
        }
        pes_buf.clear();
        return true;
    };

    while (!cancel_flag.load()) {
        if (!input.ReadExact(pkt.data(), pkt.size())) break;
        if (pkt[0] != 0x47) continue;

        bool pusi = (pkt[1] & 0x40) != 0;
        std::uint16_t pid = static_cast<std::uint16_t>(((pkt[1] & 0x1F) << 8) | pkt[2]);
        std::uint8_t afc = (pkt[3] >> 4) & 0x03;

        std::size_t off = 4;
        if (afc == 2 || afc == 0) continue;
        if (afc == 3) {
            std::uint8_t adapt_len = pkt[4];
            off = 5 + adapt_len;
        }
        if (off >= 188) continue;

        // Auto-detect first audio PES stream (stream_id 0xC0..0xDF or ADTS inside PES)
        if (audio_pid == 0xFFFF && pusi && off + 4 < 188) {
            if (pkt[off] == 0x00 && pkt[off + 1] == 0x00 && pkt[off + 2] == 0x01 &&
                (pkt[off + 3] >= 0xC0 && pkt[off + 3] <= 0xDF)) {
                audio_pid = pid;
            }
        }

        if (pid == audio_pid) {
            if (pusi && !pes_buf.empty()) {
                if (!flush_pes()) break;
                pes_buf.clear();
            }
            pes_buf.insert(pes_buf.end(), pkt.begin() + static_cast<std::ptrdiff_t>(off), pkt.end());
        }
    }
    flush_pes();
    return true;
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

void RunHlsDownloader(
    const TrackMetadata& meta,
    PullByteStream& pull_stream,
    const std::atomic<bool>& cancel_flag,
    std::string& out_error) {
    std::string playlist_url = meta.direct_media_url;
    std::deque<std::string> seen_segments;
    auto has_seen = [&](const std::string& seg) {
        return std::find(seen_segments.begin(), seen_segments.end(), seg) != seen_segments.end();
    };

    while (!cancel_flag.load()) {
        auto resp = HttpClient::Get(playlist_url, meta.http_headers, 10000);
        if (!resp.Ok()) {
            out_error = resp.error.empty() ? ("HLS HTTP " + std::to_string(resp.status_code)) : resp.error;
            break;
        }

        // Handle master playlist if #EXT-X-STREAM-INF appears
        std::istringstream iss(resp.body);
        std::string line;
        bool is_master = false;
        bool end_list = false;
        std::vector<std::string> segments;
        std::string sub_playlist;

        while (std::getline(iss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            if (line.rfind("#EXT-X-STREAM-INF", 0) == 0) {
                is_master = true;
                continue;
            }
            if (line.rfind("#EXT-X-ENDLIST", 0) == 0) {
                end_list = true;
                continue;
            }
            if (line[0] == '#') continue;

            if (is_master) {
                sub_playlist = ResolveRelativeUrl(playlist_url, line);
                break;
            }
            segments.push_back(ResolveRelativeUrl(playlist_url, line));
        }

        if (is_master && !sub_playlist.empty()) {
            playlist_url = sub_playlist;
            continue;
        }

        bool fetched_any = false;
        for (const auto& seg_url : segments) {
            if (cancel_flag.load()) break;
            if (has_seen(seg_url)) continue;

            seen_segments.push_back(seg_url);
            if (seen_segments.size() > 64) seen_segments.pop_front();
            fetched_any = true;

            std::string seg_err;
            HttpClient::StreamGet(
                seg_url,
                meta.http_headers,
                [&](const std::uint8_t* data, std::size_t size) {
                    return pull_stream.Push(data, size, cancel_flag);
                },
                cancel_flag,
                seg_err);
        }

        if (end_list) {
            break;
        }
        if (!fetched_any) {
            for (int i = 0; i < 20 && !cancel_flag.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
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
    std::string& out_error) {
    const auto& meta = impl_->metadata;
    const auto& cfg = impl_->config;

    // Fast Passthrough path for direct MP3 when volume is 100% and no 1x channel pacing is needed
    bool likely_mp3 = (meta.container_ext == "mp3" || meta.codec == "mp3");
    if (likely_mp3 && !cfg.pace_realtime && std::abs(volume_percent.load() - 100.0f) < 0.5f) {
        bool ok = HttpClient::StreamGet(
            meta.direct_media_url,
            meta.http_headers,
            [&](const std::uint8_t* data, std::size_t size) {
                while (pause_flag.load() && !cancel_flag.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                if (cancel_flag.load()) return false;
                impl_->output_buffer->Write(data, size);
                return true;
            },
            cancel_flag,
            out_error);
        impl_->output_buffer->MarkEof();
        return ok;
    }

    PullByteStream pull_stream(1024 * 1024);
    std::string dl_error;
    bool hls = IsHlsUrl(meta);

    std::thread downloader([&]() {
        if (hls) {
            RunHlsDownloader(meta, pull_stream, cancel_flag, dl_error);
        } else {
            HttpClient::StreamGet(
                meta.direct_media_url,
                meta.http_headers,
                [&](const std::uint8_t* data, std::size_t size) {
                    return pull_stream.Push(data, size, cancel_flag);
                },
                cancel_flag,
                dl_error);
            pull_stream.MarkEof();
        }
    });

    Mp3OutputPipeline pipeline(cfg, impl_->output_buffer, cancel_flag, pause_flag, volume_percent);

    // Sniff magic bytes from stream head
    std::vector<std::uint8_t> magic;
    bool ok = false;
    if (pull_stream.Peek(magic, 16) && !magic.empty()) {
        if (magic.size() >= 4 && magic[0] == 0x1A && magic[1] == 0x45 && magic[2] == 0xDF && magic[3] == 0xA3) {
            ok = DecodeWebmStream(pull_stream, pipeline, cancel_flag);
        } else if (magic.size() >= 4 && std::memcmp(magic.data(), "OggS", 4) == 0) {
            ok = DecodeOggStream(pull_stream, pipeline, cancel_flag);
        } else if (magic.size() >= 8 && std::memcmp(magic.data() + 4, "ftyp", 4) == 0) {
            ok = DecodeMp4Stream(pull_stream, pipeline, cancel_flag);
        } else if (magic[0] == 0x47 && hls) {
            ok = DecodeMpegTsStream(pull_stream, pipeline, cancel_flag);
        } else if (magic.size() >= 2 && magic[0] == 0xFF && ((magic[1] & 0xF6) == 0xF0)) {
            ok = DecodeAdtsStream(pull_stream, pipeline, cancel_flag);
        } else if (magic.size() >= 4 && std::memcmp(magic.data(), "RIFF", 4) == 0) {
            ok = DecodeWavStream(pull_stream, pipeline, cancel_flag);
        } else if (magic.size() >= 4 && std::memcmp(magic.data(), "fLaC", 4) == 0) {
            ok = DecodeFlacStream(pull_stream, pipeline, cancel_flag);
        } else {
            // MP3 (ID3 or MPEG sync word) or fallback
            ok = DecodeMp3Stream(pull_stream, pipeline, cancel_flag);
        }
    } else {
        out_error = dl_error.empty() ? "Empty or unreachable media stream" : dl_error;
    }

    pull_stream.Close();
    if (downloader.joinable()) {
        downloader.join();
    }

    pipeline.FlushAndClose();
    impl_->output_buffer->MarkEof();

    if (!ok && out_error.empty() && !cancel_flag.load()) {
        out_error = dl_error.empty() ? "Unsupported or corrupted audio stream" : dl_error;
    }
    return ok || cancel_flag.load();
}

} // namespace sampaudiostream
