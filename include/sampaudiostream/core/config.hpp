#pragma once

#include <cstdint>
#include <string>

namespace sampaudiostream {

struct PluginConfig {
    std::string bind_ip{"0.0.0.0"};
    std::string public_ip{""}; // Auto-detected if empty
    std::uint16_t http_port{0}; // 0 = server_port + 1
    bool allow_private_network_urls{false};
    bool lan_aware_urls{true};
    std::uint32_t max_concurrent_streams{64};
    std::uint32_t max_search_results{10};
    std::uint32_t mp3_bitrate_kbps{128};
    std::uint32_t mp3_sample_rate{48000};
    std::uint32_t transcoder_burst_ms{6000};
    std::uint32_t max_prefetch_mb{16};
    std::uint32_t worker_threads{4};
    std::uint32_t max_concurrent_transcoders{32};
    std::uint32_t cache_max_entries{256};
    std::uint32_t cache_ttl_seconds{3600};
    std::uint32_t spatial_update_interval_ms{200};
    std::string base_dir{"scriptfiles/samp-audio-stream"};
    std::string bin_dir{"scriptfiles/samp-audio-stream/bin"};
    std::string cookies_file{""};
    bool auto_download_extractor{true};
    bool auto_update_extractor{true};
    bool enable_js_runtime{true};
    bool hook_native_audio{true};
    bool debug_logging{false};
    bool log_startup_banner{true};
    bool log_http_listen{true};
    bool log_public_ip_detected{true};
    bool log_extractor_status{true};
    std::string player_priority_order{"personal,channel,global"};
    std::string pawn_text_encoding{"cp1252"}; // "cp1252" (client default) or "utf8"
};

struct ServerCfgInfo {
    std::int32_t port{7777};
    std::string bind_ip{"0.0.0.0"};
    bool has_port{false};
    bool has_bind{false};
};

class ConfigManager {
public:
    static PluginConfig LoadOrCreate(const std::string& config_path);
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

} // namespace sampaudiostream
