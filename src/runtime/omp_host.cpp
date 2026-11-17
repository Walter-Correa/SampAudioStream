#include "sampaudiostream/runtime/omp_host.hpp"

#include "sampaudiostream/core/config.hpp"
#include "sampaudiostream/engine/audio_engine.hpp"
#include "sampaudiostream/pawn/amx_natives.hpp"
#include "sampaudiostream/runtime/amx_patch.hpp"
#include "sampaudiostream/runtime/instance_guard.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>

extern void* pAMXFunctions;

namespace sampaudiostream {

void OmpHost::onLoad(ICore* c) {
    core_ = c;
    if (core_) {
        core_->getEventDispatcher().addEventHandler(this);
        core_->getPlayers().getPlayerConnectDispatcher().addEventHandler(this);
    }
}

void OmpHost::onInit(IComponentList* components) {
    if (!components) return;

    std::string guard_err;
    if (!RuntimeInstanceGuard::Acquire("open.mp Component", guard_err)) {
        if (core_) {
            core_->logLn(::LogLevel::Error, "[SampAudioStream] %s", guard_err.c_str());
        }
        return;
    }
    instance_acquired_ = true;

    pawn_ = components->queryComponent<IPawnComponent>();
    vehicles_ = components->queryComponent<IVehiclesComponent>();

    if (pawn_) {
        pAMXFunctions = const_cast<void*>(reinterpret_cast<const void*>(pawn_->getAmxFunctions().data()));
        pawn_->getEventDispatcher().addEventHandler(this);
    }

    PluginConfig cfg;
    PluginConfig def;
    Logger::Instance().SetSink([this](LogSeverity sev, const std::string& msg) {
        this->Log(sev, msg);
    });
    if (core_) {
        IConfig& omp_cfg = core_->getConfig();
        auto read_str = [&omp_cfg](const char* key, std::string& dst) {
            StringView sv = omp_cfg.getString(key);
            if (!sv.empty()) dst.assign(sv.data(), sv.size());
        };
        auto read_uint = [&omp_cfg](const char* key, std::uint32_t& dst, std::int64_t min_v, std::int64_t max_v, std::uint32_t def_v, bool nearest_clamp = false) {
            if (auto* v = omp_cfg.getInt(key)) {
                if (nearest_clamp && *v >= 0) {
                    dst = static_cast<std::uint32_t>(*v);
                } else {
                    ConfigManager::ParseValidatedInt(key, static_cast<std::int64_t>(*v), min_v, max_v, def_v, dst);
                }
            }
        };
        auto read_bool = [&omp_cfg](const char* key, bool& dst) {
            if (auto* v = omp_cfg.getBool(key)) dst = *v;
        };

        read_str("sampaudiostream.bind_ip", cfg.bind_ip);
        read_str("sampaudiostream.public_ip", cfg.public_ip);
        if (auto* p = omp_cfg.getInt("sampaudiostream.http_port")) {
            std::uint32_t port_val = def.http_port;
            ConfigManager::ParseValidatedInt("sampaudiostream.http_port", static_cast<std::int64_t>(*p), 0, 65535, def.http_port, port_val);
            cfg.http_port = static_cast<std::uint16_t>(port_val);
        }
        read_bool("sampaudiostream.allow_private_network_urls", cfg.allow_private_network_urls);
        read_bool("sampaudiostream.lan_aware_urls", cfg.lan_aware_urls);
        read_uint("sampaudiostream.max_concurrent_streams", cfg.max_concurrent_streams, 1, 256, def.max_concurrent_streams);
        read_uint("sampaudiostream.mp3_bitrate_kbps", cfg.mp3_bitrate_kbps, 0, 320, def.mp3_bitrate_kbps, true);
        read_uint("sampaudiostream.mp3_sample_rate", cfg.mp3_sample_rate, 0, 48000, def.mp3_sample_rate, true);
        read_uint("sampaudiostream.transcoder_burst_ms", cfg.transcoder_burst_ms, 2000, 15000, def.transcoder_burst_ms);
        read_uint("sampaudiostream.max_prefetch_mb", cfg.max_prefetch_mb, 1, 64, def.max_prefetch_mb);
        read_uint("sampaudiostream.spatial_update_interval_ms", cfg.spatial_update_interval_ms, 50, 5000, def.spatial_update_interval_ms);
        read_str("sampaudiostream.player_priority_order", cfg.player_priority_order);
        read_str("sampaudiostream.bin_dir", cfg.bin_dir);
        read_str("sampaudiostream.cookies_file", cfg.cookies_file);
        read_bool("sampaudiostream.auto_download", cfg.auto_download_extractor);
        read_bool("sampaudiostream.auto_download_extractor", cfg.auto_download_extractor);
        read_bool("sampaudiostream.auto_update", cfg.auto_update_extractor);
        read_bool("sampaudiostream.auto_update_extractor", cfg.auto_update_extractor);
        read_bool("sampaudiostream.enable_js_runtime", cfg.enable_js_runtime);
        read_uint("sampaudiostream.worker_threads", cfg.worker_threads, 1, 16, def.worker_threads);
        read_uint("sampaudiostream.max_concurrent_transcoders", cfg.max_concurrent_transcoders, 1, 128, def.max_concurrent_transcoders);
        read_uint("sampaudiostream.cache_max_entries", cfg.cache_max_entries, 16, 4096, def.cache_max_entries);
        read_uint("sampaudiostream.cache_ttl_seconds", cfg.cache_ttl_seconds, 60, 86400, def.cache_ttl_seconds);
        read_uint("sampaudiostream.max_search_results", cfg.max_search_results, 1, 50, def.max_search_results);
        read_bool("sampaudiostream.hook_native_audio", cfg.hook_native_audio);
        read_bool("sampaudiostream.debug_logging", cfg.debug_logging);
        read_bool("sampaudiostream.debug", cfg.debug_logging);
        read_bool("sampaudiostream.log_startup_banner", cfg.log_startup_banner);
        read_bool("sampaudiostream.log_http_listen", cfg.log_http_listen);
        read_bool("sampaudiostream.log_public_ip_detected", cfg.log_public_ip_detected);
        read_bool("sampaudiostream.log_extractor_status", cfg.log_extractor_status);
        read_str("sampaudiostream.pawn_text_encoding", cfg.pawn_text_encoding);
    }
    ConfigManager::Sanitize(cfg);

    hook_native_audio_ = cfg.hook_native_audio;

    engine_ = std::make_unique<AudioEngine>(*this, cfg);
    SetActiveAudioEngine(engine_.get());

    if (engine_->Initialize()) {
        if (cfg.log_startup_banner) {
            Logger::Instance().Info(
                "SampAudioStream v1.1.0 [open.mp] initialized (config: config.json [sampaudiostream], HTTP port: " +
                std::to_string(engine_->GetHttpPort()) + ")");
        }
    } else {
        Logger::Instance().Error("Failed to initialize AudioEngine on open.mp.");
    }
    Logger::Instance().Flush();
}

void OmpHost::onReady() {
    if (engine_) {
        engine_->Initialize();
    }
    if (pawn_) {
        if (IPawnScript* main_scr = pawn_->mainScript()) {
            onAmxLoad(*main_scr);
        }
        for (IPawnScript* side : pawn_->sideScripts()) {
            if (side) onAmxLoad(*side);
        }
    }
    // Scripts already loaded by startup reached the end of PawnManager::openAMX(),
    // so server natives are already bound and can be patched right away.
    PatchPendingScripts();
}

void OmpHost::onFree(IComponent* component) {
    if (component == pawn_) {
        pawn_ = nullptr;
    } else if (component == vehicles_) {
        vehicles_ = nullptr;
    }
}

void OmpHost::provideConfiguration(ILogger&, IEarlyConfig& config, bool defaults) {
    if (defaults) {
        PluginConfig def;
        config.setString("sampaudiostream.bind_ip", def.bind_ip);
        config.setString("sampaudiostream.public_ip", def.public_ip);
        config.setInt("sampaudiostream.http_port", def.http_port);
        config.setBool("sampaudiostream.allow_private_network_urls", def.allow_private_network_urls);
        config.setBool("sampaudiostream.lan_aware_urls", def.lan_aware_urls);
        config.setInt("sampaudiostream.max_concurrent_streams", static_cast<int>(def.max_concurrent_streams));
        config.setInt("sampaudiostream.mp3_bitrate_kbps", static_cast<int>(def.mp3_bitrate_kbps));
        config.setInt("sampaudiostream.mp3_sample_rate", static_cast<int>(def.mp3_sample_rate));
        config.setInt("sampaudiostream.transcoder_burst_ms", static_cast<int>(def.transcoder_burst_ms));
        config.setInt("sampaudiostream.max_prefetch_mb", static_cast<int>(def.max_prefetch_mb));
        config.setInt("sampaudiostream.spatial_update_interval_ms", static_cast<int>(def.spatial_update_interval_ms));
        config.setString("sampaudiostream.player_priority_order", def.player_priority_order);
        config.setString("sampaudiostream.bin_dir", def.bin_dir);
        config.setString("sampaudiostream.cookies_file", def.cookies_file);
        config.setBool("sampaudiostream.auto_download_extractor", def.auto_download_extractor);
        config.setBool("sampaudiostream.auto_update_extractor", def.auto_update_extractor);
        config.setBool("sampaudiostream.enable_js_runtime", def.enable_js_runtime);
        config.setInt("sampaudiostream.worker_threads", static_cast<int>(def.worker_threads));
        config.setInt("sampaudiostream.max_concurrent_transcoders", static_cast<int>(def.max_concurrent_transcoders));
        config.setInt("sampaudiostream.cache_max_entries", static_cast<int>(def.cache_max_entries));
        config.setInt("sampaudiostream.cache_ttl_seconds", static_cast<int>(def.cache_ttl_seconds));
        config.setInt("sampaudiostream.max_search_results", static_cast<int>(def.max_search_results));
        config.setBool("sampaudiostream.hook_native_audio", def.hook_native_audio);
        config.setBool("sampaudiostream.debug", def.debug_logging);
        config.setBool("sampaudiostream.log_startup_banner", def.log_startup_banner);
        config.setBool("sampaudiostream.log_http_listen", def.log_http_listen);
        config.setBool("sampaudiostream.log_public_ip_detected", def.log_public_ip_detected);
        config.setBool("sampaudiostream.log_extractor_status", def.log_extractor_status);
        config.setString("sampaudiostream.pawn_text_encoding", def.pawn_text_encoding);
    }
}

void OmpHost::free() {
    if (core_) {
        core_->getEventDispatcher().removeEventHandler(this);
        core_->getPlayers().getPlayerConnectDispatcher().removeEventHandler(this);
    }
    if (pawn_) {
        pawn_->getEventDispatcher().removeEventHandler(this);
    }
    if (instance_acquired_) {
        SetActiveAudioEngine(nullptr);
    }
    if (engine_) {
        engine_->Shutdown();
        engine_.reset();
    }
    {
        std::lock_guard<std::mutex> lock(scripts_mutex_);
        scripts_.clear();
        pending_patch_scripts_.clear();
        has_pending_patches_.store(false, std::memory_order_release);
    }
    if (instance_acquired_) {
        Logger::Instance().Flush();
        Logger::Instance().SetSink(nullptr);
        RuntimeInstanceGuard::Release();
        instance_acquired_ = false;
    }
    delete this;
}

void OmpHost::reset() {
    if (engine_) {
        engine_->Reset();
    }
}

void OmpHost::onTick(Microseconds, TimePoint) {
    // Scripts loaded/reloaded dynamically (loadfs/reloadfs/gmx) receive open.mp
    // natives after onAmxLoad() returns; apply deferred hook patches on the next tick.
    PatchPendingScripts();
    if (engine_) {
        engine_->Tick();
    }
}

bool OmpHost::PatchScriptNative(IPawnScript& script, const char* name, AMX_NATIVE replacement, AMX_NATIVE* out_original) {
    if (!name || !replacement) return false;
    AMX* amx = script.GetAMX();
    if (!amx) return false;
    return PatchAmxNative(amx, name, replacement, out_original);
}

void OmpHost::PatchPendingScripts() {
    if (!hook_native_audio_ || !has_pending_patches_.load(std::memory_order_acquire)) {
        return;
    }

    std::vector<IPawnScript*> to_patch;
    {
        std::lock_guard<std::mutex> lock(scripts_mutex_);
        if (pending_patch_scripts_.empty()) {
            has_pending_patches_.store(false, std::memory_order_release);
            return;
        }
        to_patch.swap(pending_patch_scripts_);
        has_pending_patches_.store(false, std::memory_order_release);
    }

    for (IPawnScript* script : to_patch) {
        if (!script || !script->IsLoaded()) continue;
        PatchScriptNative(*script, "PlayAudioStreamForPlayer", Hooked_PlayAudioStreamForPlayer, &orig_play_audio_);
        PatchScriptNative(*script, "StopAudioStreamForPlayer", Hooked_StopAudioStreamForPlayer, &orig_stop_audio_);
    }
    SetOriginalAudioNatives(orig_play_audio_, orig_stop_audio_);
}

void OmpHost::onAmxLoad(IPawnScript& script) {
    {
        std::lock_guard<std::mutex> lock(scripts_mutex_);
        if (std::find(scripts_.begin(), scripts_.end(), &script) != scripts_.end()) {
            return;
        }
        scripts_.push_back(&script);
        if (hook_native_audio_) {
            // Defer patching PlayAudioStreamForPlayer/StopAudioStreamForPlayer until after
            // open.mp's pawn_natives::AmxLoad() populates the AMX native table slots.
            // Calls made without sampaudiostream.inc before the first tick hit the original
            // open.mp native; scripts including sampaudiostream.inc use ALS (SA_Hook_*).
            pending_patch_scripts_.push_back(&script);
            has_pending_patches_.store(true, std::memory_order_release);
        }
    }

    const auto& natives = GetPluginNatives();
    if (!natives.empty()) {
        script.Register(natives.data(), static_cast<int>(natives.size() - 1));
    }
}

void OmpHost::onAmxUnload(IPawnScript& script) {
    std::lock_guard<std::mutex> lock(scripts_mutex_);
    scripts_.erase(std::remove(scripts_.begin(), scripts_.end(), &script), scripts_.end());
    pending_patch_scripts_.erase(
        std::remove(pending_patch_scripts_.begin(), pending_patch_scripts_.end(), &script),
        pending_patch_scripts_.end());
    has_pending_patches_.store(!pending_patch_scripts_.empty(), std::memory_order_release);
}

void OmpHost::onPlayerDisconnect(IPlayer& player, PeerDisconnectReason) {
    if (engine_) {
        engine_->OnPlayerDisconnect(static_cast<PlayerId>(player.getID()));
    }
}

void OmpHost::Log(LogSeverity severity, const std::string& message) {
    if (!core_) return;
    if (severity == LogSeverity::Debug) {
        // Logger already gates Debug messages behind debug_logging; emit at Message level so open.mp prints them to log.txt/console.
        core_->logLn(::LogLevel::Message, "[SampAudioStream] [Debug] %s", message.c_str());
        return;
    }
    ::LogLevel lvl = ::LogLevel::Message;
    switch (severity) {
        case LogSeverity::Debug: lvl = ::LogLevel::Message; break;
        case LogSeverity::Info: lvl = ::LogLevel::Message; break;
        case LogSeverity::Warning: lvl = ::LogLevel::Warning; break;
        case LogSeverity::Error: lvl = ::LogLevel::Error; break;
    }
    core_->logLn(lvl, "[SampAudioStream] %s", message.c_str());
}

std::int32_t OmpHost::GetServerPort() const {
    if (!core_) return 7777;
    if (int* port = core_->getConfig().getInt("network.port")) {
        return *port;
    }
    return 7777;
}

std::string OmpHost::GetServerBindIp() const {
    if (!core_) return "0.0.0.0";
    StringView bind = core_->getConfig().getString("network.bind");
    return bind.empty() ? "0.0.0.0" : std::string(bind.data(), bind.size());
}

bool OmpHost::IsPlayerConnected(PlayerId player_id) const {
    if (!core_ || player_id < 0) return false;
    return core_->getPlayers().get(player_id) != nullptr;
}

std::vector<PlayerId> OmpHost::GetConnectedPlayers() const {
    std::vector<PlayerId> out;
    if (!core_) return out;
    for (IPlayer* player : core_->getPlayers().entries()) {
        if (player) {
            out.push_back(static_cast<PlayerId>(player->getID()));
        }
    }
    return out;
}

std::vector<PlayerSnapshot> OmpHost::GetConnectedPlayerSnapshots() const {
    std::vector<PlayerSnapshot> out;
    if (!core_) return out;
    const auto& entries = core_->getPlayers().entries();
    out.reserve(entries.size());
    for (IPlayer* player : entries) {
        if (!player) continue;
        PlayerSnapshot snap;
        snap.id = static_cast<PlayerId>(player->getID());
        auto pos = player->getPosition();
        snap.pos = {pos.x, pos.y, pos.z};
        snap.world_id = player->getVirtualWorld();
        snap.interior_id = static_cast<std::int32_t>(player->getInterior());
        out.push_back(snap);
    }
    return out;
}

bool OmpHost::GetPlayerPos(PlayerId player_id, sampaudiostream::Vector3& out_pos) const {
    if (!core_) return false;
    IPlayer* player = core_->getPlayers().get(player_id);
    if (!player) return false;
    auto pos = player->getPosition();
    out_pos.x = pos.x;
    out_pos.y = pos.y;
    out_pos.z = pos.z;
    return true;
}

std::int32_t OmpHost::GetPlayerVirtualWorld(PlayerId player_id) const {
    if (!core_) return 0;
    IPlayer* player = core_->getPlayers().get(player_id);
    return player ? player->getVirtualWorld() : 0;
}

std::int32_t OmpHost::GetPlayerInterior(PlayerId player_id) const {
    if (!core_) return 0;
    IPlayer* player = core_->getPlayers().get(player_id);
    return player ? static_cast<std::int32_t>(player->getInterior()) : 0;
}

std::int32_t OmpHost::GetPlayerVehicleId(PlayerId player_id) const {
    if (!core_) return 0;
    IPlayer* player = core_->getPlayers().get(player_id);
    if (!player) return 0;
    IPlayerVehicleData* vdata = queryExtension<IPlayerVehicleData>(player);
    if (!vdata) return 0;
    IVehicle* veh = vdata->getVehicle();
    return veh ? veh->getID() : 0;
}

bool OmpHost::GetVehiclePos(std::int32_t vehicle_id, sampaudiostream::Vector3& out_pos) const {
    if (!vehicles_ || vehicle_id <= 0) return false;
    IVehicle* veh = vehicles_->get(vehicle_id);
    if (!veh) return false;
    auto pos = veh->getPosition();
    out_pos.x = pos.x;
    out_pos.y = pos.y;
    out_pos.z = pos.z;
    return true;
}

std::string OmpHost::GetPlayerIp(PlayerId player_id) const {
    if (!core_) return "";
    IPlayer* player = core_->getPlayers().get(player_id);
    if (!player) return "";
    PeerAddress::AddressString addr_str;
    if (PeerAddress::ToString(player->getNetworkData().networkID.address, addr_str)) {
        return std::string(addr_str.data(), addr_str.length());
    }
    return "";
}

void OmpHost::SendPlayAudioStream(PlayerId player_id, const std::string& url, const sampaudiostream::Vector3& pos, float distance, bool use_pos) {
    if (!core_) return;
    IPlayer* player = core_->getPlayers().get(player_id);
    if (!player) return;
    player->playAudio(StringView(url), use_pos, ::Vector3(pos.x, pos.y, pos.z), distance);
}

void OmpHost::SendStopAudioStream(PlayerId player_id) {
    if (!core_) return;
    IPlayer* player = core_->getPlayers().get(player_id);
    if (!player) return;
    player->stopAudio();
}

void OmpHost::DispatchPawnCallback(const std::string& name, const std::vector<CallbackArg>& args) {
    std::vector<IPawnScript*> snapshot;
    {
        std::lock_guard<std::mutex> lock(scripts_mutex_);
        snapshot = scripts_;
    }

    for (IPawnScript* script : snapshot) {
        if (!script) continue;
        int idx = -1;
        if (script->FindPublic(name.c_str(), &idx) != AMX_ERR_NONE || idx < 0) {
            continue;
        }

        cell reset_hea = script->GetHEA();
        for (auto it = args.rbegin(); it != args.rend(); ++it) {
            if (std::holds_alternative<std::int32_t>(*it)) {
                script->Push(static_cast<cell>(std::get<std::int32_t>(*it)));
            } else if (std::holds_alternative<float>(*it)) {
                float f = std::get<float>(*it);
                script->Push(amx_ftoc(f));
            } else if (std::holds_alternative<std::string>(*it)) {
                const auto& s = std::get<std::string>(*it);
                script->PushString(nullptr, nullptr, StringView(s), false, false);
            }
        }

        cell ret = 0;
        script->Exec(&ret, idx);
        script->Release(reset_hea);
    }
}

} // namespace sampaudiostream
