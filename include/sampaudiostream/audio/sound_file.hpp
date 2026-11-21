#pragma once

#include <cstdint>
#include <string>

namespace sampaudiostream {

// Short sound effects from audio.sound_dir are served to the client as-is (no transcoding),
// so only formats the SA-MP client decodes by itself are accepted.
inline constexpr std::uintmax_t kMaxSoundFileBytes = 5u * 1024u * 1024u;

bool IsSupportedSoundFileExtension(const std::string& ext_lower);
const char* SoundFileContentType(const std::string& ext_lower);

// Computes the playback length of an in-memory MP3 (Layer III frames, CBR or VBR), WAV (RIFF data chunk)
// or Ogg Vorbis (last granule position) file. Returns false when the bytes are not a valid file of that type.
bool ProbeSoundDurationMs(const std::string& bytes, const std::string& ext_lower, std::int64_t& out_ms);

} // namespace sampaudiostream
