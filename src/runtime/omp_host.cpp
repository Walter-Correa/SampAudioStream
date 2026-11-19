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
        core_->getPlayers().getPlayerChangeDispatcher().addEventHandler(this);
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
    Logger::Instance().SetSink([this](LogSeverity sev, const std::string& msg) {
        this->Log(sev, msg);
    });
    if (core_) {
        IConfig& omp_cfg = core_->getConfig();
        OmpConfigReader reader;
        reader.get_string = [&omp_cfg](const char* key, std::string& out) -> bool {
            StringView sv = omp_cfg.getString(key);
            if (sv.empty()) {
                return false;
            }
            out.assign(sv.data(), sv.size());
            return true;
        };
        reader.get_int = [&omp_cfg](const char* key, std::int64_t& out) -> bool {
            if (auto* v = omp_cfg.getInt(key)) {
                out = static_cast<std::int64_t>(*v);
                return true;
            }
            return false;
        };
        reader.get_bool = [&omp_cfg](const char* key, bool& out) -> bool {
            if (auto* v = omp_cfg.getBool(key)) {
                out = *v;
                return true;
            }
            return false;
        };
        cfg = ConfigManager::LoadFromOmp(reader);
    } else {
        ConfigManager::Sanitize(cfg);
    }

    hook_native_audio_ = cfg.hook_native_audio;

    engine_ = std::make_unique<AudioEngine>(*this, cfg);
    SetActiveAudioEngine(engine_.get());

    if (engine_->Initialize()) {
        engine_->WaitForStartupReadinessAndLog([this, &cfg]() {
            if (cfg.log_startup_banner) {
                Logger::Instance().Info(
                    "SampAudioStream v2.1.0 [open.mp] initialized (config: config.json [sampaudiostream], HTTP port: " +
                    std::to_string(engine_->GetHttpPort()) + ")");
            }
        });
    } else {
        Logger::Instance().Error("Failed to initialize AudioEngine on open.mp.");
        Logger::Instance().Flush();
    }
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
        OmpConfigWriter writer;
        writer.set_string = [&config](const char* key, const std::string& val) {
            config.setString(key, val);
        };
        writer.set_int = [&config](const char* key, std::int32_t val) {
            config.setInt(key, static_cast<int>(val));
        };
        writer.set_bool = [&config](const char* key, bool val) {
            config.setBool(key, val);
        };
        ConfigManager::ProvideOmpDefaults(writer);
    }
}

void OmpHost::free() {
    if (core_) {
        core_->getEventDispatcher().removeEventHandler(this);
        core_->getPlayers().getPlayerConnectDispatcher().removeEventHandler(this);
        core_->getPlayers().getPlayerChangeDispatcher().removeEventHandler(this);
    }
    if (pawn_) {
        pawn_->getEventDispatcher().removeEventHandler(this);
    }
    if (instance_acquired_) {
        SetActiveAudioEngine(nullptr);
    }
    if (engine_) {
        engine_->NotifyLifecycleStopping();
        engine_->Shutdown();
        engine_.reset();
    }
    {
        std::lock_guard<std::mutex> lock(scripts_mutex_);
        scripts_.clear();
        pending_patch_scripts_.clear();
        has_pending_patches_.store(false, std::memory_order_release);
        main_script_ = nullptr;
        gamemode_loaded_pending_tick_ = false;
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
    {
        std::lock_guard<std::mutex> lock(scripts_mutex_);
        gamemode_loaded_pending_tick_ = false;
    }
    if (engine_) {
        engine_->NotifyLifecycleStopping();
        engine_->Reset();
    }
}

void OmpHost::onTick(Microseconds, TimePoint) {
    // Scripts loaded/reloaded dynamically (loadfs/reloadfs/gmx) receive open.mp
    // natives after onAmxLoad() returns; apply deferred hook patches on the next tick.
    PatchPendingScripts();
    bool notify_gm_loaded = false;
    {
        std::lock_guard<std::mutex> lock(scripts_mutex_);
        if (gamemode_loaded_pending_tick_ && main_script_ && main_script_->IsLoaded()) {
            gamemode_loaded_pending_tick_ = false;
            notify_gm_loaded = true;
        }
    }
    if (notify_gm_loaded && engine_) {
        engine_->OnGameModeLoaded();
    }
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
    int gm_idx = -1;
    const bool is_main = (pawn_ && pawn_->mainScript() == &script) ||
                         (script.FindPublic("OnGameModeInit", &gm_idx) == AMX_ERR_NONE) ||
                         (script.FindPublic("OnGameModeExit", &gm_idx) == AMX_ERR_NONE);
    {
        std::lock_guard<std::mutex> lock(scripts_mutex_);
        if (std::find(scripts_.begin(), scripts_.end(), &script) != scripts_.end()) {
            if (is_main) {
                main_script_ = &script;
                gamemode_loaded_pending_tick_ = true;
            }
            return;
        }
        scripts_.push_back(&script);
        if (is_main) {
            main_script_ = &script;
            gamemode_loaded_pending_tick_ = true;
        }
        if (hook_native_audio_) {
            // Defer patching PlayAudioStreamForPlayer/StopAudioStreamForPlayer until after
            // open.mp's pawn_natives::AmxLoad() populates the AMX native table slots.
            // Calls made without SampAudioStream.inc before the first tick hit the original
            // open.mp native; scripts including SampAudioStream.inc use ALS (SA_Hook_*).
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
    bool main_unloading = false;
    {
        std::lock_guard<std::mutex> lock(scripts_mutex_);
        scripts_.erase(std::remove(scripts_.begin(), scripts_.end(), &script), scripts_.end());
        pending_patch_scripts_.erase(
            std::remove(pending_patch_scripts_.begin(), pending_patch_scripts_.end(), &script),
            pending_patch_scripts_.end());
        has_pending_patches_.store(!pending_patch_scripts_.empty(), std::memory_order_release);
        if (&script == main_script_ || (pawn_ && pawn_->mainScript() == &script) || scripts_.empty()) {
            main_script_ = nullptr;
            gamemode_loaded_pending_tick_ = false;
            main_unloading = true;
        }
    }
    if (main_unloading && engine_) {
        engine_->NotifyLifecycleStopping();
    }
}

void OmpHost::onPlayerConnect(IPlayer& player) {
    if (engine_) {
        engine_->OnPlayerConnect(static_cast<PlayerId>(player.getID()));
    }
}

void OmpHost::onPlayerDisconnect(IPlayer& player, PeerDisconnectReason) {
    if (engine_) {
        engine_->OnPlayerDisconnect(static_cast<PlayerId>(player.getID()));
    }
}

void OmpHost::onPlayerNameChange(IPlayer& player, StringView) {
    if (engine_) {
        StringView sv = player.getName();
        engine_->OnPlayerNameChanged(
            static_cast<PlayerId>(player.getID()),
            std::string(sv.data(), sv.length()));
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

std::string OmpHost::GetPlayerName(PlayerId player_id) const {
    if (!core_) return "";
    IPlayer* player = core_->getPlayers().get(player_id);
    if (!player) return "";
    StringView sv = player->getName();
    return std::string(sv.data(), sv.length());
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
        if (!script || !script->IsLoaded()) continue;
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
