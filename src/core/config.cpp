#include "sampaudiostream/core/config.hpp"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace sampaudiostream {

namespace {

void ApplyJsonToConfig(const nlohmann::json& j, PluginConfig& cfg) {
    if (j.contains("network") && j["network"].is_object()) {
        const auto& net = j["network"];
        if (net.contains("bind_ip") && net["bind_ip"].is_string()) cfg.bind_ip = net["bind_ip"].get<std::string>();
        if (net.contains("public_ip") && net["public_ip"].is_string()) cfg.public_ip = net["public_ip"].get<std::string>();
        if (net.contains("http_port") && net["http_port"].is_number_unsigned()) cfg.http_port = net["http_port"].get<std::uint16_t>();
    }

    if (j.contains("audio") && j["audio"].is_object()) {
        const auto& aud = j["audio"];
        if (aud.contains("max_concurrent_streams") && aud["max_concurrent_streams"].is_number_unsigned())
            cfg.max_concurrent_streams = aud["max_concurrent_streams"].get<std::uint32_t>();
        if (aud.contains("mp3_bitrate_kbps") && aud["mp3_bitrate_kbps"].is_number_unsigned())
            cfg.mp3_bitrate_kbps = aud["mp3_bitrate_kbps"].get<std::uint32_t>();
        if (aud.contains("mp3_sample_rate") && aud["mp3_sample_rate"].is_number_unsigned())
            cfg.mp3_sample_rate = aud["mp3_sample_rate"].get<std::uint32_t>();
        if (aud.contains("spatial_update_interval_ms") && aud["spatial_update_interval_ms"].is_number_unsigned())
            cfg.spatial_update_interval_ms = aud["spatial_update_interval_ms"].get<std::uint32_t>();
    }

    if (j.contains("extractor") && j["extractor"].is_object()) {
        const auto& ext = j["extractor"];
        if (ext.contains("bin_dir") && ext["bin_dir"].is_string()) cfg.bin_dir = ext["bin_dir"].get<std::string>();
        if (ext.contains("cookies_file") && ext["cookies_file"].is_string()) cfg.cookies_file = ext["cookies_file"].get<std::string>();
        if (ext.contains("auto_download") && ext["auto_download"].is_boolean()) cfg.auto_download_extractor = ext["auto_download"].get<bool>();
        if (ext.contains("auto_update") && ext["auto_update"].is_boolean()) cfg.auto_update_extractor = ext["auto_update"].get<bool>();
        if (ext.contains("enable_js_runtime") && ext["enable_js_runtime"].is_boolean()) cfg.enable_js_runtime = ext["enable_js_runtime"].get<bool>();
    }

    if (j.contains("performance") && j["performance"].is_object()) {
        const auto& perf = j["performance"];
        if (perf.contains("worker_threads") && perf["worker_threads"].is_number_unsigned())
            cfg.worker_threads = perf["worker_threads"].get<std::uint32_t>();
        if (perf.contains("cache_max_entries") && perf["cache_max_entries"].is_number_unsigned())
            cfg.cache_max_entries = perf["cache_max_entries"].get<std::uint32_t>();
        if (perf.contains("cache_ttl_seconds") && perf["cache_ttl_seconds"].is_number_unsigned())
            cfg.cache_ttl_seconds = perf["cache_ttl_seconds"].get<std::uint32_t>();
        if (perf.contains("max_search_results") && perf["max_search_results"].is_number_unsigned())
            cfg.max_search_results = perf["max_search_results"].get<std::uint32_t>();
    }

    if (j.contains("scripting") && j["scripting"].is_object()) {
        const auto& scr = j["scripting"];
        if (scr.contains("hook_native_audio") && scr["hook_native_audio"].is_boolean())
            cfg.hook_native_audio = scr["hook_native_audio"].get<bool>();
        if (scr.contains("debug_logging") && scr["debug_logging"].is_boolean())
            cfg.debug_logging = scr["debug_logging"].get<bool>();
    }
}

nlohmann::json ConfigToJson(const PluginConfig& cfg) {
    return nlohmann::json{
        {"network", {
            {"bind_ip", cfg.bind_ip},
            {"public_ip", cfg.public_ip},
            {"http_port", cfg.http_port}
        }},
        {"audio", {
            {"max_concurrent_streams", cfg.max_concurrent_streams},
            {"mp3_bitrate_kbps", cfg.mp3_bitrate_kbps},
            {"mp3_sample_rate", cfg.mp3_sample_rate},
            {"spatial_update_interval_ms", cfg.spatial_update_interval_ms}
        }},
        {"extractor", {
            {"bin_dir", cfg.bin_dir},
            {"cookies_file", cfg.cookies_file},
            {"auto_download", cfg.auto_download_extractor},
            {"auto_update", cfg.auto_update_extractor},
            {"enable_js_runtime", cfg.enable_js_runtime}
        }},
        {"performance", {
            {"worker_threads", cfg.worker_threads},
            {"cache_max_entries", cfg.cache_max_entries},
            {"cache_ttl_seconds", cfg.cache_ttl_seconds},
            {"max_search_results", cfg.max_search_results}
        }},
        {"scripting", {
            {"hook_native_audio", cfg.hook_native_audio},
            {"debug_logging", cfg.debug_logging}
        }}
    };
}

} // namespace

PluginConfig ConfigManager::LoadOrCreate(const std::string& config_path) {
    PluginConfig config;
    std::filesystem::path path(config_path);

    if (path.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
    }

    if (!std::filesystem::exists(path)) {
        Save(config, config_path);
        return config;
    }

    try {
        std::ifstream ifs(path);
        if (ifs.is_open()) {
            nlohmann::json j;
            ifs >> j;
            ApplyJsonToConfig(j, config);
        }
    } catch (...) {
        // Fallback to defaults if malformed JSON
    }

    return config;
}

bool ConfigManager::Save(const PluginConfig& config, const std::string& config_path) {
    std::filesystem::path path(config_path);
    if (path.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
    }

    std::ofstream ofs(path, std::ios::trunc);
    if (!ofs.is_open()) {
        return false;
    }

    ofs << ConfigToJson(config).dump(4) << '\n';
    return true;
}

} // namespace sampaudiostream
