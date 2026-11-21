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

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace sampaudiostream {

namespace {

enum class ConfigFieldType {
    String,
    Bool,
    Uint32,
    PortUint16
};

struct ConfigFieldSpec {
    const char* group;
    const char* key;
    const char* full_key;
    ConfigFieldType type;
    std::int64_t min_val{0};
    std::int64_t max_val{0};
    bool snap_nearest{false};
    std::string PluginConfig::* str_member{nullptr};
    bool PluginConfig::* bool_member{nullptr};
    std::uint32_t PluginConfig::* uint_member{nullptr};
    std::uint16_t PluginConfig::* port_member{nullptr};
};

constexpr ConfigFieldSpec StrSpec(const char* g, const char* k, const char* fk, std::string PluginConfig::* m) {
    return {g, k, fk, ConfigFieldType::String, 0, 0, false, m, nullptr, nullptr, nullptr};
}

constexpr ConfigFieldSpec BoolSpec(const char* g, const char* k, const char* fk, bool PluginConfig::* m) {
    return {g, k, fk, ConfigFieldType::Bool, 0, 0, false, nullptr, m, nullptr, nullptr};
}

constexpr ConfigFieldSpec UintSpec(const char* g, const char* k, const char* fk,
                                   std::int64_t min_v, std::int64_t max_v, bool snap,
                                   std::uint32_t PluginConfig::* m) {
    return {g, k, fk, ConfigFieldType::Uint32, min_v, max_v, snap, nullptr, nullptr, m, nullptr};
}

constexpr ConfigFieldSpec PortSpec(const char* g, const char* k, const char* fk, std::uint16_t PluginConfig::* m) {
    return {g, k, fk, ConfigFieldType::PortUint16, 0, 65535, false, nullptr, nullptr, nullptr, m};
}

// Single authoritative schema shared by SA-MP JSON, open.mp flat/dotted config, and Sanitize().
constexpr ConfigFieldSpec kConfigSchema[] = {
    // network
    StrSpec("network", "bind_ip", "network.bind_ip", &PluginConfig::bind_ip),
    StrSpec("network", "public_ip", "network.public_ip", &PluginConfig::public_ip),
    PortSpec("network", "http_port", "network.http_port", &PluginConfig::http_port),
    BoolSpec("network", "lan_aware_urls", "network.lan_aware_urls", &PluginConfig::lan_aware_urls),
    BoolSpec("network", "allow_private_network_urls", "network.allow_private_network_urls", &PluginConfig::allow_private_network_urls),
    UintSpec("network", "max_connections_per_ip", "network.max_connections_per_ip", 1, 4096, false, &PluginConfig::max_connections_per_ip),

    // audio
    UintSpec("audio", "mp3_bitrate_kbps", "audio.mp3_bitrate_kbps", 0, 320, true, &PluginConfig::mp3_bitrate_kbps),
    UintSpec("audio", "mp3_sample_rate", "audio.mp3_sample_rate", 0, 48000, true, &PluginConfig::mp3_sample_rate),
    UintSpec("audio", "transcoder_burst_ms", "audio.transcoder_burst_ms", 2000, 15000, false, &PluginConfig::transcoder_burst_ms),
    UintSpec("audio", "max_concurrent_streams", "audio.max_concurrent_streams", 1, 256, false, &PluginConfig::max_concurrent_streams),
    UintSpec("audio", "spatial_update_interval_ms", "audio.spatial_update_interval_ms", 50, 5000, false, &PluginConfig::spatial_update_interval_ms),
    StrSpec("audio", "player_priority_order", "audio.player_priority_order", &PluginConfig::player_priority_order),
    BoolSpec("audio", "normalize_volume", "audio.normalize_volume", &PluginConfig::normalize_volume),
    UintSpec("audio", "live_title_poll_seconds", "audio.live_title_poll_seconds", 0, 300, true, &PluginConfig::live_title_poll_seconds),
    StrSpec("audio", "music_dir", "audio.music_dir", &PluginConfig::music_dir),

    // channels
    UintSpec("channels", "max_channels", "channels.max_channels", 1, 1000, false, &PluginConfig::max_channels),
    UintSpec("channels", "max_queue_per_channel", "channels.max_queue_per_channel", 1, 1000, false, &PluginConfig::max_queue_per_channel),
    UintSpec("channels", "channel_restore_grace_seconds", "channels.channel_restore_grace_seconds", 0, 3600, false, &PluginConfig::channel_restore_grace_seconds),

    // extractor
    StrSpec("extractor", "bin_dir", "extractor.bin_dir", &PluginConfig::bin_dir),
    BoolSpec("extractor", "auto_download", "extractor.auto_download", &PluginConfig::auto_download_extractor),
    BoolSpec("extractor", "auto_update", "extractor.auto_update", &PluginConfig::auto_update_extractor),
    BoolSpec("extractor", "enable_js_runtime", "extractor.enable_js_runtime", &PluginConfig::enable_js_runtime),
    StrSpec("extractor", "js_runtime", "extractor.js_runtime", &PluginConfig::js_runtime),
    StrSpec("extractor", "cookies_file", "extractor.cookies_file", &PluginConfig::cookies_file),
    StrSpec("extractor", "cookies_from_browser", "extractor.cookies_from_browser", &PluginConfig::cookies_from_browser),
    UintSpec("extractor", "yt_dlp_cache_max_age_days", "extractor.yt_dlp_cache_max_age_days", 0, 90, false, &PluginConfig::yt_dlp_cache_max_age_days),
    UintSpec("extractor", "max_search_results", "extractor.max_search_results", 1, 50, false, &PluginConfig::max_search_results),

    // performance
    UintSpec("performance", "worker_threads", "performance.worker_threads", 1, 16, false, &PluginConfig::worker_threads),
    UintSpec("performance", "max_concurrent_transcoders", "performance.max_concurrent_transcoders", 1, 128, false, &PluginConfig::max_concurrent_transcoders),
    UintSpec("performance", "max_prefetch_mb", "performance.max_prefetch_mb", 1, 64, false, &PluginConfig::max_prefetch_mb),
    UintSpec("performance", "cache_max_entries", "performance.cache_max_entries", 16, 4096, false, &PluginConfig::cache_max_entries),
    UintSpec("performance", "cache_ttl_seconds", "performance.cache_ttl_seconds", 60, 86400, false, &PluginConfig::cache_ttl_seconds),

    // scripting
    BoolSpec("scripting", "hook_native_audio", "scripting.hook_native_audio", &PluginConfig::hook_native_audio),
    StrSpec("scripting", "pawn_text_encoding", "scripting.pawn_text_encoding", &PluginConfig::pawn_text_encoding),

    // logging
    BoolSpec("logging", "debug", "logging.debug", &PluginConfig::debug_logging),
    BoolSpec("logging", "startup_banner", "logging.startup_banner", &PluginConfig::log_startup_banner),
    BoolSpec("logging", "http_listen", "logging.http_listen", &PluginConfig::log_http_listen),
    BoolSpec("logging", "public_ip_detected", "logging.public_ip_detected", &PluginConfig::log_public_ip_detected),
    BoolSpec("logging", "extractor_status", "logging.extractor_status", &PluginConfig::log_extractor_status)
};

void ApplyValidatedRawInt(
    const ConfigFieldSpec& spec,
    std::int64_t raw,
    const PluginConfig& def,
    PluginConfig& cfg) {
    if (spec.type == ConfigFieldType::PortUint16) {
        std::uint32_t port_val = def.*(spec.port_member);
        ConfigManager::ParseValidatedInt(
            spec.full_key, raw, spec.min_val, spec.max_val,
            static_cast<std::uint32_t>(def.*(spec.port_member)), port_val);
        cfg.*(spec.port_member) = static_cast<std::uint16_t>(port_val);
        return;
    }
    const std::uint32_t def_val = def.*(spec.uint_member);
    if (spec.snap_nearest && raw >= 0) {
        cfg.*(spec.uint_member) = (raw > 0xFFFFFFFFLL) ? 0xFFFFFFFFu : static_cast<std::uint32_t>(raw);
        return;
    }
    ConfigManager::ParseValidatedInt(
        spec.full_key, raw, spec.min_val, spec.max_val, def_val, cfg.*(spec.uint_member));
}

void ApplyJsonToConfig(const nlohmann::json& j, PluginConfig& cfg) {
    if (!j.is_object()) {
        return;
    }
    const PluginConfig def;
    for (const auto& spec : kConfigSchema) {
        auto git = j.find(spec.group);
        if (git == j.end() || !git->is_object()) {
            continue;
        }
        auto kit = git->find(spec.key);
        if (kit == git->end()) {
            continue;
        }
        const nlohmann::json& val = *kit;
        switch (spec.type) {
            case ConfigFieldType::String:
                if (val.is_string()) {
                    cfg.*(spec.str_member) = val.get<std::string>();
                } else {
                    const std::string& def_str = def.*(spec.str_member);
                    Logger::Instance().Warn(
                        std::string("Config key '") + spec.full_key +
                        "' must be a string; using default ('" + def_str + "').");
                    cfg.*(spec.str_member) = def_str;
                }
                break;
            case ConfigFieldType::Bool:
                if (val.is_boolean()) {
                    cfg.*(spec.bool_member) = val.get<bool>();
                } else {
                    const bool def_bool = def.*(spec.bool_member);
                    Logger::Instance().Warn(
                        std::string("Config key '") + spec.full_key +
                        "' must be a boolean; using default (" + (def_bool ? "true" : "false") + ").");
                    cfg.*(spec.bool_member) = def_bool;
                }
                break;
            case ConfigFieldType::Uint32:
            case ConfigFieldType::PortUint16: {
                const std::uint32_t def_num = (spec.type == ConfigFieldType::PortUint16)
                    ? static_cast<std::uint32_t>(def.*(spec.port_member))
                    : def.*(spec.uint_member);
                if (!val.is_number_integer()) {
                    Logger::Instance().Warn(
                        std::string("Config key '") + spec.full_key +
                        "' must be an integer; using default (" + std::to_string(def_num) + ").");
                    if (spec.type == ConfigFieldType::PortUint16) {
                        cfg.*(spec.port_member) = static_cast<std::uint16_t>(def_num);
                    } else {
                        cfg.*(spec.uint_member) = def_num;
                    }
                    break;
                }
                ApplyValidatedRawInt(spec, val.get<std::int64_t>(), def, cfg);
                break;
            }
        }
    }
}

struct AllmanJsonNode {
    enum class Kind { Object, String, Int, Bool };
    Kind kind{Kind::Object};
    std::vector<std::pair<std::string, AllmanJsonNode>> children;
    std::string str_val;
    std::int64_t int_val{0};
    bool bool_val{false};

    static AllmanJsonNode MakeString(std::string s) {
        AllmanJsonNode n;
        n.kind = Kind::String;
        n.str_val = std::move(s);
        return n;
    }
    static AllmanJsonNode MakeInt(std::int64_t v) {
        AllmanJsonNode n;
        n.kind = Kind::Int;
        n.int_val = v;
        return n;
    }
    static AllmanJsonNode MakeBool(bool v) {
        AllmanJsonNode n;
        n.kind = Kind::Bool;
        n.bool_val = v;
        return n;
    }
};

void SerializeAllmanJson(std::ostream& os, const AllmanJsonNode& node, int indent_level) {
    if (node.kind == AllmanJsonNode::Kind::String) {
        os << nlohmann::json(node.str_val).dump();
        return;
    }
    if (node.kind == AllmanJsonNode::Kind::Int) {
        os << nlohmann::json(node.int_val).dump();
        return;
    }
    if (node.kind == AllmanJsonNode::Kind::Bool) {
        os << nlohmann::json(node.bool_val).dump();
        return;
    }

    const std::string pad(static_cast<std::size_t>(indent_level) * 4, ' ');
    const std::string child_pad(static_cast<std::size_t>(indent_level + 1) * 4, ' ');
    os << pad << "{\n";
    for (std::size_t i = 0; i < node.children.size(); ++i) {
        const auto& kv = node.children[i];
        const bool is_last = (i + 1 == node.children.size());
        if (kv.second.kind == AllmanJsonNode::Kind::Object) {
            os << child_pad << nlohmann::json(kv.first).dump() << ":\n";
            SerializeAllmanJson(os, kv.second, indent_level + 1);
            os << (is_last ? "\n" : ",\n");
        } else {
            os << child_pad << nlohmann::json(kv.first).dump() << ": ";
            SerializeAllmanJson(os, kv.second, indent_level + 1);
            os << (is_last ? "\n" : ",\n");
        }
    }
    os << pad << "}";
}

AllmanJsonNode BuildConfigTree(const PluginConfig& cfg) {
    AllmanJsonNode root;
    root.kind = AllmanJsonNode::Kind::Object;

    for (const auto& spec : kConfigSchema) {
        if (root.children.empty() || root.children.back().first != spec.group) {
            AllmanJsonNode grp;
            grp.kind = AllmanJsonNode::Kind::Object;
            root.children.emplace_back(spec.group, std::move(grp));
        }
        AllmanJsonNode& group_node = root.children.back().second;
        switch (spec.type) {
            case ConfigFieldType::String:
                group_node.children.emplace_back(spec.key, AllmanJsonNode::MakeString(cfg.*(spec.str_member)));
                break;
            case ConfigFieldType::Bool:
                group_node.children.emplace_back(spec.key, AllmanJsonNode::MakeBool(cfg.*(spec.bool_member)));
                break;
            case ConfigFieldType::Uint32:
                group_node.children.emplace_back(spec.key, AllmanJsonNode::MakeInt(static_cast<std::int64_t>(cfg.*(spec.uint_member))));
                break;
            case ConfigFieldType::PortUint16:
                group_node.children.emplace_back(spec.key, AllmanJsonNode::MakeInt(static_cast<std::int64_t>(cfg.*(spec.port_member))));
                break;
        }
    }
    return root;
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

PluginConfig ConfigManager::LoadFromOmp(const OmpConfigReader& reader) {
    PluginConfig cfg;
    const PluginConfig def;
    for (const auto& spec : kConfigSchema) {
        const std::string omp_key = std::string("sampaudiostream.") + spec.full_key;
        switch (spec.type) {
            case ConfigFieldType::String:
                if (reader.get_string) {
                    std::string val;
                    if (reader.get_string(omp_key.c_str(), val)) {
                        cfg.*(spec.str_member) = std::move(val);
                    }
                }
                break;
            case ConfigFieldType::Bool:
                if (reader.get_bool) {
                    bool val = false;
                    if (reader.get_bool(omp_key.c_str(), val)) {
                        cfg.*(spec.bool_member) = val;
                    }
                }
                break;
            case ConfigFieldType::Uint32:
            case ConfigFieldType::PortUint16:
                if (reader.get_int) {
                    std::int64_t raw = 0;
                    if (reader.get_int(omp_key.c_str(), raw)) {
                        ApplyValidatedRawInt(spec, raw, def, cfg);
                    }
                }
                break;
        }
    }
    Sanitize(cfg);
    return cfg;
}

void ConfigManager::ProvideOmpDefaults(const OmpConfigWriter& writer) {
    const PluginConfig def;
    for (const auto& spec : kConfigSchema) {
        const std::string omp_key = std::string("sampaudiostream.") + spec.full_key;
        switch (spec.type) {
            case ConfigFieldType::String:
                if (writer.set_string) {
                    writer.set_string(omp_key.c_str(), def.*(spec.str_member));
                }
                break;
            case ConfigFieldType::Bool:
                if (writer.set_bool) {
                    writer.set_bool(omp_key.c_str(), def.*(spec.bool_member));
                }
                break;
            case ConfigFieldType::Uint32:
                if (writer.set_int) {
                    writer.set_int(omp_key.c_str(), static_cast<std::int32_t>(def.*(spec.uint_member)));
                }
                break;
            case ConfigFieldType::PortUint16:
                if (writer.set_int) {
                    writer.set_int(omp_key.c_str(), static_cast<std::int32_t>(def.*(spec.port_member)));
                }
                break;
        }
    }
}

std::vector<ConfigSchemaFieldInfo> ConfigManager::GetUnifiedSchemaFields() {
    std::vector<ConfigSchemaFieldInfo> out;
    out.reserve(sizeof(kConfigSchema) / sizeof(kConfigSchema[0]));
    for (const auto& spec : kConfigSchema) {
        out.push_back(ConfigSchemaFieldInfo{spec.group, spec.key, spec.full_key});
    }
    return out;
}

void ConfigManager::Sanitize(PluginConfig& cfg) {
    const PluginConfig def;
    for (const auto& spec : kConfigSchema) {
        if (spec.type != ConfigFieldType::Uint32 || spec.snap_nearest) {
            continue;
        }
        std::uint32_t& val = cfg.*(spec.uint_member);
        const std::uint32_t min_v = static_cast<std::uint32_t>(spec.min_val);
        const std::uint32_t max_v = static_cast<std::uint32_t>(spec.max_val);
        const std::uint32_t def_v = def.*(spec.uint_member);
        if (val < min_v || val > max_v) {
            Logger::Instance().Warn(
                std::string("Config key '") + spec.full_key + "' value " + std::to_string(val) +
                " is out of range [" + std::to_string(min_v) + ".." + std::to_string(max_v) +
                "]; using default (" + std::to_string(def_v) + ").");
            val = def_v;
        }
    }

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
            "Config key 'audio.mp3_bitrate_kbps' value " + std::to_string(cfg.mp3_bitrate_kbps) +
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
            "Config key 'audio.mp3_sample_rate' value " + std::to_string(cfg.mp3_sample_rate) +
            " snapped to nearest supported sample rate (" + std::to_string(best_sr) + ").");
    }
    cfg.mp3_sample_rate = best_sr;

    if (cfg.bind_ip.empty()) cfg.bind_ip = "0.0.0.0";
    if (cfg.base_dir.empty()) cfg.base_dir = "scriptfiles/SampAudioStream";
    if (cfg.bin_dir.empty()) cfg.bin_dir = "scriptfiles/SampAudioStream/bin";
    if (cfg.player_priority_order.empty()) cfg.player_priority_order = "personal,channel,global";

    if (cfg.live_title_poll_seconds > 300) {
        Logger::Instance().Warn(
            "Config key 'audio.live_title_poll_seconds' value " +
            std::to_string(cfg.live_title_poll_seconds) +
            " exceeds maximum (300); clamped to 300.");
        cfg.live_title_poll_seconds = 300;
    } else if (cfg.live_title_poll_seconds > 0 && cfg.live_title_poll_seconds < 10) {
        Logger::Instance().Warn(
            "Config key 'audio.live_title_poll_seconds' value " +
            std::to_string(cfg.live_title_poll_seconds) +
            " is below minimum active interval (10); clamped to 10.");
        cfg.live_title_poll_seconds = 10;
    }

    for (char& ch : cfg.pawn_text_encoding) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    if (cfg.pawn_text_encoding != "utf8" && cfg.pawn_text_encoding != "cp1252") {
        Logger::Instance().Warn(
            "Config key 'scripting.pawn_text_encoding' value '" + cfg.pawn_text_encoding +
            "' is invalid; expected 'utf8' or 'cp1252'. Using default ('cp1252').");
        cfg.pawn_text_encoding = "cp1252";
    }

    for (char& ch : cfg.js_runtime) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    if (cfg.js_runtime != "auto" && cfg.js_runtime != "deno" && cfg.js_runtime != "quickjs") {
        Logger::Instance().Warn(
            "Config key 'extractor.js_runtime' value '" + cfg.js_runtime +
            "' is invalid; expected 'auto', 'deno', or 'quickjs'. Using default ('auto').");
        cfg.js_runtime = "auto";
    }

    for (char& ch : cfg.cookies_from_browser) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    if (cfg.cookies_from_browser != "auto" && cfg.cookies_from_browser != "off") {
        Logger::Instance().Warn(
            "Config key 'extractor.cookies_from_browser' value '" + cfg.cookies_from_browser +
            "' is invalid; expected 'auto' or 'off'. Using default ('auto').");
        cfg.cookies_from_browser = "auto";
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

namespace {

void AppendUtf8CodePoint(std::string& out, std::uint32_t cp) {
    if (cp <= 0x7Fu) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FFu) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0xFFFFu) {
        out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

bool IsStrictValidUtf8(const std::string& s) {
    std::size_t i = 0;
    while (i < s.size()) {
        std::uint8_t b0 = static_cast<std::uint8_t>(s[i]);
        if (b0 <= 0x7Fu) {
            ++i;
            continue;
        }
        std::uint32_t cp = 0;
        std::size_t len = 0;
        std::uint32_t min_cp = 0;
        if ((b0 & 0xE0u) == 0xC0u) {
            len = 2;
            cp = b0 & 0x1Fu;
            min_cp = 0x80u;
        } else if ((b0 & 0xF0u) == 0xE0u) {
            len = 3;
            cp = b0 & 0x0Fu;
            min_cp = 0x800u;
        } else if ((b0 & 0xF8u) == 0xF0u) {
            len = 4;
            cp = b0 & 0x07u;
            min_cp = 0x10000u;
        } else {
            return false;
        }
        if (i + len > s.size()) {
            return false;
        }
        for (std::size_t j = 1; j < len; ++j) {
            std::uint8_t bx = static_cast<std::uint8_t>(s[i + j]);
            if ((bx & 0xC0u) != 0x80u) {
                return false;
            }
            cp = (cp << 6) | (bx & 0x3Fu);
        }
        if (cp < min_cp || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) {
            return false;
        }
        i += len;
    }
    return true;
}

} // namespace

std::string ConvertTextFromPawn(const std::string& text, const std::string& encoding) {
    if (encoding != "cp1252" || text.empty()) {
        return text;
    }

    std::string out;
    out.reserve(text.size() * 2);
    for (unsigned char uc : text) {
        if (uc < 0x80u) {
            out.push_back(static_cast<char>(uc));
            continue;
        }
        if (uc >= 0xA0u) {
            AppendUtf8CodePoint(out, static_cast<std::uint32_t>(uc));
            continue;
        }
        std::uint32_t cp = 0;
        switch (uc) {
            case 0x80u: cp = 0x20ACu; break;
            case 0x82u: cp = 0x201Au; break;
            case 0x83u: cp = 0x0192u; break;
            case 0x84u: cp = 0x201Eu; break;
            case 0x85u: cp = 0x2026u; break;
            case 0x86u: cp = 0x2020u; break;
            case 0x87u: cp = 0x2021u; break;
            case 0x88u: cp = 0x02C6u; break;
            case 0x89u: cp = 0x2030u; break;
            case 0x8Au: cp = 0x0160u; break;
            case 0x8Bu: cp = 0x2039u; break;
            case 0x8Cu: cp = 0x0152u; break;
            case 0x8Eu: cp = 0x017Du; break;
            case 0x91u: cp = 0x2018u; break;
            case 0x92u: cp = 0x2019u; break;
            case 0x93u: cp = 0x201Cu; break;
            case 0x94u: cp = 0x201Du; break;
            case 0x95u: cp = 0x2022u; break;
            case 0x96u: cp = 0x2013u; break;
            case 0x97u: cp = 0x2014u; break;
            case 0x98u: cp = 0x02DCu; break;
            case 0x99u: cp = 0x2122u; break;
            case 0x9Au: cp = 0x0161u; break;
            case 0x9Bu: cp = 0x203Au; break;
            case 0x9Cu: cp = 0x0153u; break;
            case 0x9Eu: cp = 0x017Eu; break;
            case 0x9Fu: cp = 0x0178u; break;
            default:
                // Undefined CP1252 slots (0x81, 0x8D, 0x8F, 0x90, 0x9D) are stripped cleanly.
                continue;
        }
        AppendUtf8CodePoint(out, cp);
    }
    return out;
}

std::string NormalizeExternalTextToUtf8(const std::string& raw) {
    if (raw.empty() || IsStrictValidUtf8(raw)) {
        return raw;
    }
    return ConvertTextFromPawn(raw, "cp1252");
}

std::string TruncateUtf8Bytes(const std::string& utf8, std::size_t max_bytes) {
    if (utf8.empty() || max_bytes == 0) {
        return {};
    }
    std::string out;
    out.reserve(std::min(utf8.size(), max_bytes));
    for (std::size_t i = 0; i < utf8.size() && out.size() < max_bytes;) {
        const unsigned char b0 = static_cast<unsigned char>(utf8[i]);
        std::size_t seq_len = 1;
        if (b0 < 0x80u) {
            seq_len = 1;
        } else if ((b0 & 0xE0u) == 0xC0u && i + 1 < utf8.size() &&
                   (static_cast<unsigned char>(utf8[i + 1]) & 0xC0u) == 0x80u) {
            seq_len = 2;
        } else if ((b0 & 0xF0u) == 0xE0u && i + 2 < utf8.size() &&
                   (static_cast<unsigned char>(utf8[i + 1]) & 0xC0u) == 0x80u &&
                   (static_cast<unsigned char>(utf8[i + 2]) & 0xC0u) == 0x80u) {
            seq_len = 3;
        } else if ((b0 & 0xF8u) == 0xF0u && i + 3 < utf8.size() &&
                   (static_cast<unsigned char>(utf8[i + 1]) & 0xC0u) == 0x80u &&
                   (static_cast<unsigned char>(utf8[i + 2]) & 0xC0u) == 0x80u &&
                   (static_cast<unsigned char>(utf8[i + 3]) & 0xC0u) == 0x80u) {
            seq_len = 4;
        }
        if (out.size() + seq_len > max_bytes) {
            break;
        }
        out.append(utf8, i, seq_len);
        i += seq_len;
    }
    return out;
}

std::string SanitizeControlAndTrimUtf8(const std::string& utf8, std::size_t max_bytes) {
    if (utf8.empty() || max_bytes == 0) {
        return {};
    }
    std::string filtered;
    filtered.reserve(utf8.size());
    for (unsigned char ch : utf8) {
        if (ch >= 0x20u && ch != 0x7Fu) {
            filtered.push_back(static_cast<char>(ch));
        }
    }
    std::size_t first = 0;
    while (first < filtered.size() && filtered[first] == ' ') {
        ++first;
    }
    std::size_t last = filtered.size();
    while (last > first && filtered[last - 1] == ' ') {
        --last;
    }
    return TruncateUtf8Bytes(filtered.substr(first, last - first), max_bytes);
}

std::string CleanIcyStationNameUtf8(const std::string& raw) {
    if (raw.empty()) {
        return {};
    }
    return SanitizeControlAndTrimUtf8(NormalizeExternalTextToUtf8(raw), 128);
}

bool CanRepresentTextInPawnEncoding(const std::string& utf8, const std::string& encoding) {
    if (!IsStrictValidUtf8(utf8)) {
        return false;
    }
    if (encoding != "cp1252") {
        return true;
    }
    return ConvertTextFromPawn(ConvertTextForPawn(utf8, "cp1252"), "cp1252") == utf8;
}

std::filesystem::path Utf8ToPath(const std::string& utf8) {
#ifdef _WIN32
    if (utf8.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed > 0) {
        std::wstring wide(static_cast<std::size_t>(needed), L'\0');
        if (MultiByteToWideChar(
                CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), wide.data(), needed) == needed) {
            return std::filesystem::path(wide);
        }
    }
    return std::filesystem::path(utf8);
#else
    return std::filesystem::path(utf8);
#endif
}

std::string PathToUtf8(const std::filesystem::path& path) {
#ifdef _WIN32
    const std::wstring wide = path.wstring();
    if (wide.empty()) {
        return {};
    }
    const int needed = WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (needed > 0) {
        std::string utf8(static_cast<std::size_t>(needed), '\0');
        if (WideCharToMultiByte(
                CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), needed, nullptr, nullptr) == needed) {
            return utf8;
        }
    }
    return path.string();
#else
    return path.string();
#endif
}

bool IsSupportedAudioFileExtension(const std::string& lower_ext) {
    return lower_ext == ".mp3" || lower_ext == ".ogg" || lower_ext == ".opus" ||
           lower_ext == ".wav" || lower_ext == ".flac" || lower_ext == ".aac" ||
           lower_ext == ".m4a";
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
                if (j.contains("max_players") && j["max_players"].is_number_integer()) {
                    long mp = j["max_players"].get<long>();
                    if (mp >= 1 && mp <= 1000) {
                        info.max_players = static_cast<std::int32_t>(mp);
                        info.has_max_players = true;
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
        } else if (key == "maxplayers") {
            long mp = std::strtol(val.c_str(), nullptr, 10);
            if (mp >= 1 && mp <= 1000) {
                info.max_players = static_cast<std::int32_t>(mp);
                info.has_max_players = true;
            }
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

    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        return false;
    }

    const AllmanJsonNode tree = BuildConfigTree(config);
    SerializeAllmanJson(ofs, tree, 0);
    ofs << '\n';
    return true;
}

} // namespace sampaudiostream
