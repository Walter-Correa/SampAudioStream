#include "sampaudiostream/core/config.hpp"

#include "sampaudiostream/core/logger.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>

namespace sampaudiostream {

namespace {

void ReadJsonUint(
    const nlohmann::json& obj,
    const char* key,
    const char* full_key,
    std::int64_t min_val,
    std::int64_t max_val,
    std::uint32_t default_val,
    std::uint32_t& out_val,
    bool allow_nearest_clamp = false) {
    if (!obj.contains(key)) return;
    if (!obj[key].is_number_integer()) {
        Logger::Instance().Warn(std::string("Config key '") + full_key + "' must be an integer; using default (" + std::to_string(default_val) + ").");
        out_val = default_val;
        return;
    }
    std::int64_t raw = obj[key].get<std::int64_t>();
    if (allow_nearest_clamp && raw >= 0) {
        out_val = (raw > 0xFFFFFFFFLL) ? 0xFFFFFFFFu : static_cast<std::uint32_t>(raw);
        return;
    }
    ConfigManager::ParseValidatedInt(full_key, raw, min_val, max_val, default_val, out_val);
}

void ApplyJsonToConfig(const nlohmann::json& j, PluginConfig& cfg) {
    PluginConfig def;
    if (j.contains("network") && j["network"].is_object()) {
        const auto& net = j["network"];
        if (net.contains("bind_ip") && net["bind_ip"].is_string()) cfg.bind_ip = net["bind_ip"].get<std::string>();
        if (net.contains("public_ip") && net["public_ip"].is_string()) cfg.public_ip = net["public_ip"].get<std::string>();
        if (net.contains("http_port")) {
            std::uint32_t port_val = def.http_port;
            if (!net["http_port"].is_number_integer()) {
                Logger::Instance().Warn("Config key 'network.http_port' must be an integer; using default (0).");
            } else {
                ConfigManager::ParseValidatedInt("network.http_port", net["http_port"].get<std::int64_t>(), 0, 65535, def.http_port, port_val);
            }
            cfg.http_port = static_cast<std::uint16_t>(port_val);
        }
        if (net.contains("allow_private_network_urls") && net["allow_private_network_urls"].is_boolean()) {
            cfg.allow_private_network_urls = net["allow_private_network_urls"].get<bool>();
        }
        if (net.contains("lan_aware_urls") && net["lan_aware_urls"].is_boolean()) {
            cfg.lan_aware_urls = net["lan_aware_urls"].get<bool>();
        }
    }

    if (j.contains("audio") && j["audio"].is_object()) {
        const auto& aud = j["audio"];
        ReadJsonUint(aud, "max_concurrent_streams", "audio.max_concurrent_streams", 1, 256, def.max_concurrent_streams, cfg.max_concurrent_streams);
        ReadJsonUint(aud, "mp3_bitrate_kbps", "audio.mp3_bitrate_kbps", 0, 320, def.mp3_bitrate_kbps, cfg.mp3_bitrate_kbps, true);
        ReadJsonUint(aud, "mp3_sample_rate", "audio.mp3_sample_rate", 0, 48000, def.mp3_sample_rate, cfg.mp3_sample_rate, true);
        ReadJsonUint(aud, "transcoder_burst_ms", "audio.transcoder_burst_ms", 2000, 15000, def.transcoder_burst_ms, cfg.transcoder_burst_ms);
        ReadJsonUint(aud, "spatial_update_interval_ms", "audio.spatial_update_interval_ms", 50, 5000, def.spatial_update_interval_ms, cfg.spatial_update_interval_ms);
        if (aud.contains("player_priority_order") && aud["player_priority_order"].is_string())
            cfg.player_priority_order = aud["player_priority_order"].get<std::string>();
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
        ReadJsonUint(perf, "worker_threads", "performance.worker_threads", 1, 16, def.worker_threads, cfg.worker_threads);
        ReadJsonUint(perf, "max_concurrent_transcoders", "performance.max_concurrent_transcoders", 1, 128, def.max_concurrent_transcoders, cfg.max_concurrent_transcoders);
        ReadJsonUint(perf, "cache_max_entries", "performance.cache_max_entries", 16, 4096, def.cache_max_entries, cfg.cache_max_entries);
        ReadJsonUint(perf, "cache_ttl_seconds", "performance.cache_ttl_seconds", 60, 86400, def.cache_ttl_seconds, cfg.cache_ttl_seconds);
        ReadJsonUint(perf, "max_search_results", "performance.max_search_results", 1, 50, def.max_search_results, cfg.max_search_results);
        ReadJsonUint(perf, "max_prefetch_mb", "performance.max_prefetch_mb", 1, 64, def.max_prefetch_mb, cfg.max_prefetch_mb);
    }

    if (j.contains("scripting") && j["scripting"].is_object()) {
        const auto& scr = j["scripting"];
        if (scr.contains("hook_native_audio") && scr["hook_native_audio"].is_boolean())
            cfg.hook_native_audio = scr["hook_native_audio"].get<bool>();
        if (scr.contains("debug_logging") && scr["debug_logging"].is_boolean())
            cfg.debug_logging = scr["debug_logging"].get<bool>();
        if (scr.contains("pawn_text_encoding") && scr["pawn_text_encoding"].is_string())
            cfg.pawn_text_encoding = scr["pawn_text_encoding"].get<std::string>();
    }

    if (j.contains("logging") && j["logging"].is_object()) {
        const auto& log = j["logging"];
        if (log.contains("startup_banner") && log["startup_banner"].is_boolean())
            cfg.log_startup_banner = log["startup_banner"].get<bool>();
        if (log.contains("http_listen") && log["http_listen"].is_boolean())
            cfg.log_http_listen = log["http_listen"].get<bool>();
        if (log.contains("public_ip_detected") && log["public_ip_detected"].is_boolean())
            cfg.log_public_ip_detected = log["public_ip_detected"].get<bool>();
        if (log.contains("extractor_status") && log["extractor_status"].is_boolean())
            cfg.log_extractor_status = log["extractor_status"].get<bool>();
    }
}

nlohmann::json ConfigToJson(const PluginConfig& cfg) {
    return nlohmann::json{
        {"network", {
            {"bind_ip", cfg.bind_ip},
            {"public_ip", cfg.public_ip},
            {"http_port", cfg.http_port},
            {"allow_private_network_urls", cfg.allow_private_network_urls},
            {"lan_aware_urls", cfg.lan_aware_urls}
        }},
        {"audio", {
            {"max_concurrent_streams", cfg.max_concurrent_streams},
            {"mp3_bitrate_kbps", cfg.mp3_bitrate_kbps},
            {"mp3_sample_rate", cfg.mp3_sample_rate},
            {"transcoder_burst_ms", cfg.transcoder_burst_ms},
            {"spatial_update_interval_ms", cfg.spatial_update_interval_ms},
            {"player_priority_order", cfg.player_priority_order}
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
            {"max_concurrent_transcoders", cfg.max_concurrent_transcoders},
            {"cache_max_entries", cfg.cache_max_entries},
            {"cache_ttl_seconds", cfg.cache_ttl_seconds},
            {"max_search_results", cfg.max_search_results},
            {"max_prefetch_mb", cfg.max_prefetch_mb}
        }},
        {"scripting", {
            {"hook_native_audio", cfg.hook_native_audio},
            {"debug_logging", cfg.debug_logging},
            {"pawn_text_encoding", cfg.pawn_text_encoding}
        }},
        {"logging", {
            {"startup_banner", cfg.log_startup_banner},
            {"http_listen", cfg.log_http_listen},
            {"public_ip_detected", cfg.log_public_ip_detected},
            {"extractor_status", cfg.log_extractor_status}
        }}
    };
}

} // namespace

bool ConfigManager::ParseValidatedInt(
    const char* key_name,
    std::int64_t raw_value,
    std::int64_t min_val,
    std::int64_t max_val,
    std::uint32_t default_val,
    std::uint32_t& out_val) {
    if (raw_value < 0) {
        Logger::Instance().Warn(
            std::string("Config key '") + key_name + "' cannot be negative (" +
            std::to_string(raw_value) + "); using default (" + std::to_string(default_val) + ").");
        out_val = default_val;
        return false;
    }
    if (raw_value < min_val || raw_value > max_val) {
        Logger::Instance().Warn(
            std::string("Config key '") + key_name + "' value " + std::to_string(raw_value) +
            " is out of range [" + std::to_string(min_val) + ".." + std::to_string(max_val) +
            "]; using default (" + std::to_string(default_val) + ").");
        out_val = default_val;
        return false;
    }
    out_val = static_cast<std::uint32_t>(raw_value);
    return true;
}

void ConfigManager::Sanitize(PluginConfig& cfg) {
    PluginConfig def;
    auto check_range = [](const char* key, std::uint32_t& val, std::uint32_t min_v, std::uint32_t max_v, std::uint32_t def_v) {
        if (val < min_v || val > max_v) {
            Logger::Instance().Warn(
                std::string("Config key '") + key + "' value " + std::to_string(val) +
                " is out of range [" + std::to_string(min_v) + ".." + std::to_string(max_v) +
                "]; using default (" + std::to_string(def_v) + ").");
            val = def_v;
        }
    };

    check_range("max_concurrent_streams", cfg.max_concurrent_streams, 1, 256, def.max_concurrent_streams);
    check_range("max_search_results", cfg.max_search_results, 1, 50, def.max_search_results);

    static constexpr std::uint32_t kValidBitrates[] = {64, 96, 128, 160, 192, 256, 320};
    std::uint32_t best_br = 128;
    std::uint32_t best_br_diff = 0xFFFFFFFFu;
    for (std::uint32_t vb : kValidBitrates) {
        std::uint32_t d = (cfg.mp3_bitrate_kbps > vb) ? (cfg.mp3_bitrate_kbps - vb) : (vb - cfg.mp3_bitrate_kbps);
        if (d < best_br_diff) {
            best_br_diff = d;
            best_br = vb;
        }
    }
    if (cfg.mp3_bitrate_kbps != best_br) {
        Logger::Instance().Warn(
            "Config key 'mp3_bitrate_kbps' value " + std::to_string(cfg.mp3_bitrate_kbps) +
            " snapped to nearest supported bitrate (" + std::to_string(best_br) + ").");
    }
    cfg.mp3_bitrate_kbps = best_br;

    static constexpr std::uint32_t kValidRates[] = {32000, 44100, 48000};
    std::uint32_t best_sr = 48000;
    std::uint32_t best_sr_diff = 0xFFFFFFFFu;
    for (std::uint32_t vr : kValidRates) {
        std::uint32_t d = (cfg.mp3_sample_rate > vr) ? (cfg.mp3_sample_rate - vr) : (vr - cfg.mp3_sample_rate);
        if (d < best_sr_diff) {
            best_sr_diff = d;
            best_sr = vr;
        }
    }
    if (cfg.mp3_sample_rate != best_sr) {
        Logger::Instance().Warn(
            "Config key 'mp3_sample_rate' value " + std::to_string(cfg.mp3_sample_rate) +
            " snapped to nearest supported sample rate (" + std::to_string(best_sr) + ").");
    }
    cfg.mp3_sample_rate = best_sr;

    check_range("transcoder_burst_ms", cfg.transcoder_burst_ms, 2000, 15000, def.transcoder_burst_ms);
    check_range("worker_threads", cfg.worker_threads, 1, 16, def.worker_threads);
    check_range("max_concurrent_transcoders", cfg.max_concurrent_transcoders, 1, 128, def.max_concurrent_transcoders);
    check_range("cache_max_entries", cfg.cache_max_entries, 16, 4096, def.cache_max_entries);
    check_range("cache_ttl_seconds", cfg.cache_ttl_seconds, 60, 86400, def.cache_ttl_seconds);
    check_range("max_prefetch_mb", cfg.max_prefetch_mb, 1, 64, def.max_prefetch_mb);
    check_range("spatial_update_interval_ms", cfg.spatial_update_interval_ms, 50, 5000, def.spatial_update_interval_ms);
    if (cfg.bind_ip.empty()) cfg.bind_ip = "0.0.0.0";
    if (cfg.base_dir.empty()) cfg.base_dir = "scriptfiles/samp-audio-stream";
    if (cfg.bin_dir.empty()) cfg.bin_dir = "scriptfiles/samp-audio-stream/bin";
    if (cfg.player_priority_order.empty()) cfg.player_priority_order = "personal,channel,global";

    for (char& ch : cfg.pawn_text_encoding) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    if (cfg.pawn_text_encoding != "utf8" && cfg.pawn_text_encoding != "cp1252") {
        Logger::Instance().Warn(
            "Config key 'pawn_text_encoding' value '" + cfg.pawn_text_encoding +
            "' is invalid; expected 'utf8' or 'cp1252'. Using default ('cp1252').");
        cfg.pawn_text_encoding = "cp1252";
    }
}

std::string ConvertTextForPawn(const std::string& utf8, const std::string& encoding) {
    if (encoding != "cp1252" || utf8.empty()) {
        return utf8;
    }

    auto is_ignored_invisible = [](std::uint32_t cp) -> bool {
        return (cp >= 0xFE00u && cp <= 0xFE0Fu) ||     // Variation selectors (incl. U+FE0F)
               (cp >= 0xE0100u && cp <= 0xE01EFu) || // Variation selectors supplement
               (cp >= 0x200Bu && cp <= 0x200Fu) ||   // Zero-width space/joiners/marks
               (cp >= 0x202Au && cp <= 0x202Eu) ||   // Bidi embeddings/overrides
               (cp >= 0x2060u && cp <= 0x206Fu) ||   // Word joiner & invisible chars
               cp == 0xFEFFu ||                      // BOM / ZWNBSP
               cp == 0x034Fu ||                      // Combining grapheme joiner
               cp == 0x20E3u ||                      // Combining enclosing keycap
               (cp >= 0x1F3FBu && cp <= 0x1F3FFu);   // Emoji skin tone modifiers
    };

    auto unicode_to_cp1252 = [](std::uint32_t cp, std::uint8_t& out_byte) -> bool {
        if (cp <= 0x7Fu) {
            out_byte = static_cast<std::uint8_t>(cp);
            return true;
        }
        if (cp >= 0xA0u && cp <= 0xFFu) {
            out_byte = static_cast<std::uint8_t>(cp);
            return true;
        }
        switch (cp) {
            case 0x20ACu: out_byte = 0x80u; return true;
            case 0x201Au: out_byte = 0x82u; return true;
            case 0x0192u: out_byte = 0x83u; return true;
            case 0x201Eu: out_byte = 0x84u; return true;
            case 0x2026u: out_byte = 0x85u; return true;
            case 0x2020u: out_byte = 0x86u; return true;
            case 0x2021u: out_byte = 0x87u; return true;
            case 0x02C6u: out_byte = 0x88u; return true;
            case 0x2030u: out_byte = 0x89u; return true;
            case 0x0160u: out_byte = 0x8Au; return true;
            case 0x2039u: out_byte = 0x8Bu; return true;
            case 0x0152u: out_byte = 0x8Cu; return true;
            case 0x017Du: out_byte = 0x8Eu; return true;
            case 0x2018u: out_byte = 0x91u; return true;
            case 0x2019u: out_byte = 0x92u; return true;
            case 0x201Cu: out_byte = 0x93u; return true;
            case 0x201Du: out_byte = 0x94u; return true;
            case 0x2022u: out_byte = 0x95u; return true;
            case 0x2013u: out_byte = 0x96u; return true;
            case 0x2014u: out_byte = 0x97u; return true;
            case 0x02DCu: out_byte = 0x98u; return true;
            case 0x2122u: out_byte = 0x99u; return true;
            case 0x0161u: out_byte = 0x9Au; return true;
            case 0x203Au: out_byte = 0x9Bu; return true;
            case 0x0153u: out_byte = 0x9Cu; return true;
            case 0x017Eu: out_byte = 0x9Eu; return true;
            case 0x0178u: out_byte = 0x9Fu; return true;
            default: return false;
        }
    };

    std::string out;
    out.reserve(utf8.size());
    std::size_t i = 0;
    bool stripped_symbol = false;
    bool trailing_emoji_space = false;
    while (i < utf8.size()) {
        std::uint8_t b0 = static_cast<std::uint8_t>(utf8[i]);
        std::uint32_t cp = 0;
        bool valid_seq = false;
        if (b0 < 0x80u) {
            cp = b0;
            valid_seq = true;
            ++i;
        } else if ((b0 & 0xE0u) == 0xC0u && i + 1 < utf8.size()) {
            std::uint8_t b1 = static_cast<std::uint8_t>(utf8[i + 1]);
            if ((b1 & 0xC0u) == 0x80u) {
                cp = ((b0 & 0x1Fu) << 6) | (b1 & 0x3Fu);
                valid_seq = true;
                i += 2;
            } else {
                ++i;
            }
        } else if ((b0 & 0xF0u) == 0xE0u && i + 2 < utf8.size()) {
            std::uint8_t b1 = static_cast<std::uint8_t>(utf8[i + 1]);
            std::uint8_t b2 = static_cast<std::uint8_t>(utf8[i + 2]);
            if ((b1 & 0xC0u) == 0x80u && (b2 & 0xC0u) == 0x80u) {
                cp = ((b0 & 0x0Fu) << 12) | ((b1 & 0x3Fu) << 6) | (b2 & 0x3Fu);
                valid_seq = true;
                i += 3;
            } else {
                ++i;
            }
        } else if ((b0 & 0xF8u) == 0xF0u && i + 3 < utf8.size()) {
            std::uint8_t b1 = static_cast<std::uint8_t>(utf8[i + 1]);
            std::uint8_t b2 = static_cast<std::uint8_t>(utf8[i + 2]);
            std::uint8_t b3 = static_cast<std::uint8_t>(utf8[i + 3]);
            if ((b1 & 0xC0u) == 0x80u && (b2 & 0xC0u) == 0x80u && (b3 & 0xC0u) == 0x80u) {
                cp = ((b0 & 0x07u) << 18) | ((b1 & 0x3Fu) << 12) | ((b2 & 0x3Fu) << 6) | (b3 & 0x3Fu);
                valid_seq = true;
                i += 4;
            } else {
                ++i;
            }
        } else {
            ++i;
        }

        if (!valid_seq || is_ignored_invisible(cp)) {
            continue;
        }

        std::uint8_t mapped = 0;
        if (unicode_to_cp1252(cp, mapped)) {
            if (mapped == ' ' && stripped_symbol && (out.empty() || out.back() == ' ')) {
                continue;
            }
            out.push_back(static_cast<char>(mapped));
            stripped_symbol = false;
            trailing_emoji_space = false;
        } else {
            stripped_symbol = true;
            if (!out.empty() && out.back() == ' ') {
                trailing_emoji_space = true;
            }
        }
    }
    if (stripped_symbol && trailing_emoji_space && !out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

ServerCfgInfo ConfigManager::LoadServerCfg(const std::string& path) {
    ServerCfgInfo info;
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        // Fallback for open.mp legacy plugin mode when server.cfg does not exist
        std::ifstream omp_ifs("config.json");
        if (omp_ifs.is_open()) {
            try {
                auto j = nlohmann::json::parse(omp_ifs);
                if (j.contains("network") && j["network"].is_object()) {
                    const auto& net = j["network"];
                    if (net.contains("port") && net["port"].is_number_integer()) {
                        long p = net["port"].get<long>();
                        if (p >= 1 && p <= 65535) {
                            info.port = static_cast<std::int32_t>(p);
                            info.has_port = true;
                        }
                    }
                    if (net.contains("bind") && net["bind"].is_string()) {
                        std::string b = net["bind"].get<std::string>();
                        if (!b.empty()) {
                            info.bind_ip = std::move(b);
                            info.has_bind = true;
                        }
                    }
                }
            } catch (...) {
            }
        }
        return info;
    }

    auto trim = [](std::string s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) s.erase(s.begin());
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
        return s;
    };

    std::string line;
    while (std::getline(ifs, line)) {
        line = trim(std::move(line));
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;

        std::istringstream iss(line);
        std::string key;
        if (!(iss >> key)) continue;
        std::string val;
        std::getline(iss, val);
        val = trim(std::move(val));
        if (val.empty()) continue;

        if (key == "port") {
            long p = std::strtol(val.c_str(), nullptr, 10);
            if (p >= 1 && p <= 65535) {
                info.port = static_cast<std::int32_t>(p);
                info.has_port = true;
            }
        } else if (key == "bind") {
            info.bind_ip = val;
            info.has_bind = true;
        }
    }
    return info;
}

PluginConfig ConfigManager::LoadOrCreate(const std::string& config_path) {
    PluginConfig config;
    std::filesystem::path path(config_path);

    if (path.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
    }

    if (!std::filesystem::exists(path)) {
        Sanitize(config);
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
    } catch (const std::exception& ex) {
        std::string err = "Malformed JSON in config file '" + config_path + "' (" + ex.what() + "); using defaults without overwriting file.";
        Logger::Instance().Error(err);
        std::cerr << "[SampAudioStream] [ERROR] " << err << "\n";
    } catch (...) {
        std::string err = "Unknown error parsing config file '" + config_path + "'; using defaults without overwriting file.";
        Logger::Instance().Error(err);
        std::cerr << "[SampAudioStream] [ERROR] " << err << "\n";
    }

    Sanitize(config);
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
