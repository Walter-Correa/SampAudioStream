#pragma once

#include <cstdint>
#include <string>

namespace sampaudiostream {

struct PluginConfig {
    std::string bind_ip{"0.0.0.0"};
    std::string public_ip{""}; // Auto-detected if empty
    std::uint16_t http_port{0}; // 0 = server_port + 1
    std::uint32_t max_concurrent_streams{64};
    std::uint32_t max_search_results{10};
    std::uint32_t mp3_bitrate_kbps{128};
    std::uint32_t mp3_sample_rate{48000};
    std::uint32_t worker_threads{4};
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
};

class ConfigManager {
public:
    static PluginConfig LoadOrCreate(const std::string& config_path);
    static bool Save(const PluginConfig& config, const std::string& config_path);
};

} // namespace sampaudiostream
