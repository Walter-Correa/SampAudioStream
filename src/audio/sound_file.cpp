#include "sampaudiostream/audio/sound_file.hpp"

#include <cstddef>
#include <cstring>

namespace sampaudiostream {

namespace {

std::uint32_t ReadLe32(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t ReadLe64(const unsigned char* p) {
    return static_cast<std::uint64_t>(ReadLe32(p)) |
           (static_cast<std::uint64_t>(ReadLe32(p + 4)) << 32);
}

struct Mp3FrameInfo {
    std::size_t length{0};
    std::uint32_t samples{0};
    std::uint32_t sample_rate{0};
};

bool ParseMp3Frame(const unsigned char* h, Mp3FrameInfo& out) {
    if (h[0] != 0xFF || (h[1] & 0xE0u) != 0xE0u) return false;
    const unsigned int version = (h[1] >> 3) & 0x03u; // 0=2.5, 1=reserved, 2=2, 3=1
    const unsigned int layer = (h[1] >> 1) & 0x03u;   // 1=Layer III
    const unsigned int br_idx = (h[2] >> 4) & 0x0Fu;
    const unsigned int sr_idx = (h[2] >> 2) & 0x03u;
    const unsigned int pad = (h[2] >> 1) & 0x01u;
    if (version == 1 || layer != 1 || br_idx == 0 || br_idx == 15 || sr_idx == 3) {
        return false;
    }
    static constexpr int kBitratesV1L3[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
    static constexpr int kBitratesV2L3[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
    static constexpr int kSampleRates[4][3] = {
        {11025, 12000, 8000},
        {0, 0, 0},
        {22050, 24000, 16000},
        {44100, 48000, 32000}
    };
    const int bitrate = (version == 3 ? kBitratesV1L3[br_idx] : kBitratesV2L3[br_idx]) * 1000;
    const int srate = kSampleRates[version][sr_idx];
    if (bitrate <= 0 || srate <= 0) return false;
    const int coef = (version == 3) ? 144 : 72;
    out.length = static_cast<std::size_t>((coef * bitrate) / srate + static_cast<int>(pad));
    out.samples = (version == 3) ? 1152u : 576u;
    out.sample_rate = static_cast<std::uint32_t>(srate);
    return out.length >= 24;
}

bool ProbeMp3DurationMs(const std::string& bytes, std::int64_t& out_ms) {
    const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
    const std::size_t n = bytes.size();
    std::size_t i = 0;
    if (n >= 10 && std::memcmp(data, "ID3", 3) == 0) {
        const std::size_t tag_size =
            (static_cast<std::size_t>(data[6] & 0x7F) << 21) |
            (static_cast<std::size_t>(data[7] & 0x7F) << 14) |
            (static_cast<std::size_t>(data[8] & 0x7F) << 7) |
            static_cast<std::size_t>(data[9] & 0x7F);
        i = 10 + tag_size + (((data[5] & 0x10u) != 0) ? 10u : 0u);
    }

    double seconds = 0.0;
    std::size_t frames = 0;
    bool in_sync = false;
    while (i + 4 <= n) {
        Mp3FrameInfo frame;
        if (ParseMp3Frame(data + i, frame) && i + frame.length <= n) {
            bool accept = in_sync;
            if (!accept) {
                // Resynchronizing: require the next header to also be valid (or the frame to end the file).
                Mp3FrameInfo next;
                const std::size_t next_pos = i + frame.length;
                accept = (next_pos == n) || (next_pos + 4 <= n && ParseMp3Frame(data + next_pos, next));
            }
            if (accept) {
                seconds += static_cast<double>(frame.samples) / static_cast<double>(frame.sample_rate);
                ++frames;
                i += frame.length;
                in_sync = true;
                continue;
            }
        }
        in_sync = false;
        ++i;
    }
    if (frames == 0) return false;
    out_ms = static_cast<std::int64_t>(seconds * 1000.0 + 0.5);
    return out_ms > 0;
}

bool ProbeWavDurationMs(const std::string& bytes, std::int64_t& out_ms) {
    const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
    const std::size_t n = bytes.size();
    if (n < 12 || std::memcmp(data, "RIFF", 4) != 0 || std::memcmp(data + 8, "WAVE", 4) != 0) {
        return false;
    }
    std::uint32_t byte_rate = 0;
    std::uint64_t data_size = 0;
    bool have_data = false;
    std::size_t pos = 12;
    while (pos + 8 <= n) {
        const unsigned char* id = data + pos;
        const std::uint64_t chunk_size = ReadLe32(data + pos + 4);
        const std::size_t body = pos + 8;
        if (std::memcmp(id, "fmt ", 4) == 0) {
            if (chunk_size < 16 || body + 16 > n) return false;
            byte_rate = ReadLe32(data + body + 8);
        } else if (std::memcmp(id, "data", 4) == 0) {
            const std::uint64_t available = n - body;
            data_size = (chunk_size < available) ? chunk_size : available;
            have_data = true;
            break;
        }
        pos = body + static_cast<std::size_t>(chunk_size) + static_cast<std::size_t>(chunk_size & 1u);
    }
    if (!have_data || byte_rate == 0) return false;
    out_ms = static_cast<std::int64_t>((data_size * 1000u) / byte_rate);
    return out_ms > 0;
}

bool ProbeOggVorbisDurationMs(const std::string& bytes, std::int64_t& out_ms) {
    const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
    const std::size_t n = bytes.size();
    if (n < 58 || std::memcmp(data, "OggS", 4) != 0) return false;

    static constexpr unsigned char kVorbisId[] = {0x01, 'v', 'o', 'r', 'b', 'i', 's'};
    const std::size_t search_end = (n < 512) ? n : 512;
    std::uint32_t sample_rate = 0;
    for (std::size_t i = 0; i + sizeof(kVorbisId) + 9 <= search_end; ++i) {
        if (std::memcmp(data + i, kVorbisId, sizeof(kVorbisId)) == 0) {
            sample_rate = ReadLe32(data + i + sizeof(kVorbisId) + 5);
            break;
        }
    }
    if (sample_rate == 0) return false;

    for (std::size_t i = n - 14; i > 0; --i) {
        if (data[i] == 'O' && std::memcmp(data + i, "OggS", 4) == 0 && data[i + 4] == 0) {
            const std::uint64_t granule = ReadLe64(data + i + 6);
            if (granule == ~0ULL) continue;
            out_ms = static_cast<std::int64_t>((granule * 1000u) / sample_rate);
            return out_ms > 0;
        }
    }
    return false;
}

} // namespace

bool IsSupportedSoundFileExtension(const std::string& ext_lower) {
    return ext_lower == ".mp3" || ext_lower == ".ogg" || ext_lower == ".wav";
}

const char* SoundFileContentType(const std::string& ext_lower) {
    if (ext_lower == ".ogg") return "audio/ogg";
    if (ext_lower == ".wav") return "audio/wav";
    return "audio/mpeg";
}

bool ProbeSoundDurationMs(const std::string& bytes, const std::string& ext_lower, std::int64_t& out_ms) {
    out_ms = 0;
    if (ext_lower == ".mp3") return ProbeMp3DurationMs(bytes, out_ms);
    if (ext_lower == ".wav") return ProbeWavDurationMs(bytes, out_ms);
    if (ext_lower == ".ogg") return ProbeOggVorbisDurationMs(bytes, out_ms);
    return false;
}

} // namespace sampaudiostream
