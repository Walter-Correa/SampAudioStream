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
// SA-MP / open.mp Pawn standard INVALID_PLAYER_ID constant (0xFFFF)
inline constexpr std::int32_t PAWN_INVALID_PLAYER_ID = 65535;

inline constexpr std::int32_t ToPawnPlayerId(PlayerId id) {
    return (id < 0 || id == PAWN_INVALID_PLAYER_ID) ? PAWN_INVALID_PLAYER_ID : id;
}

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
    InternalError = 9,
    AuthRequired = 10
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

enum class ChannelDestroyMode : std::int32_t {
    Manual = 0,
    OnDisconnect = 1,
    Timeout = 2
};

enum class ChannelDestroyReason : std::int32_t {
    Manual = 0,
    CreatorDisconnected = 1,
    TimeoutExpired = 2
};

enum class ChannelLoopMode : std::int32_t {
    Off = 0,
    Track = 1,
    Queue = 2
};

enum class AudioStreamSourceType : std::int32_t {
    None = 0,
    Personal = 1,
    Channel = 2,
    Global = 3,
    ChannelRange = 4
};

struct ChannelInfo {
    ChannelId id{INVALID_CHANNEL_ID};
    std::string name;
    PlayerId creator_player_id{INVALID_PLAYER_ID};
    std::string creator_name;
    bool creator_online{false};
    bool has_password{false};
    std::size_t listener_count{0};
    std::size_t queue_count{0};
    StreamState state{StreamState::Idle};
    ChannelDestroyMode destroy_mode{ChannelDestroyMode::Manual};
    std::int32_t destroy_timeout_seconds{0};
    std::int32_t destroy_remaining_seconds{0};
    std::string current_track_title;
    std::string current_track_artist;
    std::int32_t current_track_duration{0};
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
    bool notify_processing{true};
    bool process_direct_audio_files{false};
    bool stream_text{true};
};

struct TrackMetadata {
    std::string id;
    std::string original_url;
    std::string direct_media_url;
    std::string local_file_path; // Internal validated disk path for library:// playback (never logged or returned)
    std::string title;
    std::string artist;
    std::string extractor_name;
    std::string format_id;
    std::string codec;
    std::string container_ext;
    std::int32_t duration_seconds{0};
    bool is_live{false};
    bool title_is_placeholder{false};
    SourceType source_type{SourceType::DirectAudio};
    std::chrono::steady_clock::time_point resolved_at{};
    std::int64_t available_at_epoch_ms{0};
    std::string js_runtime_used;
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
