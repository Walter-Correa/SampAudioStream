#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace sampaudiostream {

using StreamId = std::int32_t;
using ChannelId = std::int32_t;
using SearchId = std::int32_t;
using PlayerId = std::int32_t;

inline constexpr StreamId INVALID_STREAM_ID = -1;
inline constexpr ChannelId INVALID_CHANNEL_ID = -1;
inline constexpr SearchId INVALID_SEARCH_ID = -1;
inline constexpr PlayerId INVALID_PLAYER_ID = -1;

enum class ErrorCode : std::int32_t {
    None = 0,
    InvalidUrl = 1,
    ExtractionFailed = 2,
    NetworkError = 3,
    UnsupportedFormat = 4,
    TranscodeError = 5,
    ExtractorNotReady = 6,
    SearchFailed = 7,
    StreamNotFound = 8,
    InternalError = 9
};

enum class StreamState : std::int32_t {
    Idle = 0,
    Resolving = 1,
    Buffering = 2,
    Playing = 3,
    Paused = 4,
    Stopped = 5,
    Finished = 6,
    Error = 7
};

enum class SourceType : std::uint8_t {
    DirectAudio = 0,    // Direct MP3/OGG/WAV/Shoutcast stream playable by BASS directly
    ExtractorStream = 1 // YouTube, SoundCloud, Twitch, etc. resolved via extractor
};

struct Vector3 {
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
};

struct StreamOptions {
    Vector3 position{};
    float distance{50.0f};
    bool use_position{false};
    std::int32_t world_id{-1};
    std::int32_t interior_id{-1};
    float volume{100.0f};
    std::int32_t attach_player_id{INVALID_PLAYER_ID};
    std::int32_t attach_vehicle_id{0};
};

struct TrackMetadata {
    std::string original_url;
    std::string direct_media_url;
    std::string title;
    std::string artist;
    std::string extractor_name;
    std::string codec;
    std::string container_ext;
    std::int32_t duration_seconds{0};
    bool is_live{false};
    std::unordered_map<std::string, std::string> http_headers;
};

struct SearchResultItem {
    std::string id;
    std::string title;
    std::string artist;
    std::string url;
    std::int32_t duration_seconds{0};
    bool is_live{false};
};

struct SearchResponse {
    SearchId search_id{INVALID_SEARCH_ID};
    PlayerId player_id{INVALID_PLAYER_ID};
    std::string query;
    ErrorCode error{ErrorCode::None};
    std::string error_message;
    std::vector<SearchResultItem> items;
};

} // namespace sampaudiostream
