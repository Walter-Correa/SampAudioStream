#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace sampaudiostream {

struct PluginConfig {
    std::string bind_ip{"0.0.0.0"};
    std::string public_ip{""}; // Auto-detected if empty
    std::uint16_t http_port{0}; // 0 = server_port + 1
    bool allow_private_network_urls{false};
    bool lan_aware_urls{true};
    std::uint32_t max_connections_per_ip{64};
    std::uint32_t max_concurrent_streams{64};
    std::uint32_t max_queue_per_channel{50};
    std::uint32_t max_channels{100};
    std::uint32_t channel_restore_grace_seconds{300};
    std::uint32_t max_search_results{10};
    std::uint32_t mp3_bitrate_kbps{128};
    std::uint32_t mp3_sample_rate{48000};
    std::uint32_t transcoder_burst_ms{6000};
    std::uint32_t max_prefetch_mb{8};
    std::uint32_t worker_threads{4};
    std::uint32_t max_concurrent_transcoders{32};
    std::uint32_t cache_max_entries{256};
    std::uint32_t cache_ttl_seconds{3600};
    std::uint32_t spatial_update_interval_ms{200};
    std::string base_dir{"scriptfiles/SampAudioStream"};
    std::string bin_dir{"scriptfiles/SampAudioStream/bin"};
    std::string cookies_file{""};
    std::string cookies_from_browser{"auto"}; // "auto" or "off"
    bool auto_download_extractor{true};
    bool auto_update_extractor{true};
    bool enable_js_runtime{true};
    std::string js_runtime{"auto"}; // "auto", "deno", or "quickjs"
    std::uint32_t yt_dlp_cache_max_age_days{7};
    bool hook_native_audio{true};
    bool debug_logging{false};
    bool log_startup_banner{true};
    bool log_http_listen{true};
    bool log_public_ip_detected{true};
    bool log_extractor_status{true};
    std::string player_priority_order{"personal,channel,global"};
    bool normalize_volume{false};
    std::uint32_t live_title_poll_seconds{30};
    std::string music_dir{"scriptfiles/SampAudioStream/music"};
    std::string sound_dir{"scriptfiles/SampAudioStream/sound"};
    std::string audio_file_label{"Audio File"};
    std::string pawn_text_encoding{"cp1252"}; // "cp1252" (client default) or "utf8"
};

struct ServerCfgInfo {
    std::int32_t port{7777};
    std::string bind_ip{"0.0.0.0"};
    std::int32_t max_players{0};
    bool has_port{false};
    bool has_bind{false};
    bool has_max_players{false};
};

struct OmpConfigReader {
    std::function<bool(const char* key, std::string& out)> get_string;
    std::function<bool(const char* key, std::int64_t& out)> get_int;
    std::function<bool(const char* key, bool& out)> get_bool;
};

struct OmpConfigWriter {
    std::function<void(const char* key, const std::string& val)> set_string;
    std::function<void(const char* key, std::int32_t val)> set_int;
    std::function<void(const char* key, bool val)> set_bool;
};

struct ConfigSchemaFieldInfo {
    std::string group;
    std::string key;
    std::string full_key; // "<group>.<key>"
};

class ConfigManager {
public:
    static PluginConfig LoadOrCreate(const std::string& config_path);
    static PluginConfig LoadFromOmp(const OmpConfigReader& reader);
    static void ProvideOmpDefaults(const OmpConfigWriter& writer);
    static std::vector<ConfigSchemaFieldInfo> GetUnifiedSchemaFields();
    static bool Save(const PluginConfig& config, const std::string& config_path);
    static void Sanitize(PluginConfig& config);
    static bool ParseValidatedInt(
        const char* key_name,
        std::int64_t raw_value,
        std::int64_t min_val,
        std::int64_t max_val,
        std::uint32_t default_val,
        std::uint32_t& out_val);
    static ServerCfgInfo LoadServerCfg(const std::string& path = "server.cfg");
};

// Converts UTF-8 metadata/messages for Pawn AMX output ("utf8" pass-through or "cp1252").
std::string ConvertTextForPawn(const std::string& utf8, const std::string& encoding);

// Converts Pawn AMX input to UTF-8 ("utf8" pass-through or "cp1252").
// Undefined CP1252 bytes (0x81, 0x8D, 0x8F, 0x90, 0x9D) are stripped cleanly.
std::string ConvertTextFromPawn(const std::string& text, const std::string& encoding);

// Normalizes external ICY/HTTP strings to UTF-8 (pass-through if already valid UTF-8,
// otherwise decodes from CP1252).
std::string NormalizeExternalTextToUtf8(const std::string& raw);

// Truncates valid UTF-8 to at most max_bytes without splitting a multi-byte UTF-8 sequence.
std::string TruncateUtf8Bytes(const std::string& utf8, std::size_t max_bytes);

// Removes ASCII control characters (< 0x20 and 0x7F), trims outer spaces, and truncates to max_bytes.
std::string SanitizeControlAndTrimUtf8(const std::string& utf8, std::size_t max_bytes);

// Normalizes an HTTP icy-name header value to cleaned UTF-8 truncated to at most 128 bytes.
std::string CleanIcyStationNameUtf8(const std::string& raw);

// Returns true if utf8 is strictly valid UTF-8 and losslessly representable in encoding ("utf8" or "cp1252").
bool CanRepresentTextInPawnEncoding(const std::string& utf8, const std::string& encoding);

// Portable UTF-8 <-> std::filesystem::path conversions (wide UTF-16 on Windows, byte pass-through on POSIX).
std::filesystem::path Utf8ToPath(const std::string& utf8);
std::string PathToUtf8(const std::filesystem::path& path);

// Returns true if lower_ext (including leading '.', e.g. ".mp3") is supported for local playback/direct files.
bool IsSupportedAudioFileExtension(const std::string& lower_ext);

} // namespace sampaudiostream
