#include "sampaudiostream/runtime/omp_host.hpp"

#include "sampaudiostream/core/config.hpp"
#include "sampaudiostream/engine/audio_engine.hpp"
#include "sampaudiostream/pawn/amx_natives.hpp"

#include <algorithm>
#include <cstring>

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

    pawn_ = components->queryComponent<IPawnComponent>();
    vehicles_ = components->queryComponent<IVehiclesComponent>();

    if (pawn_) {
        pAMXFunctions = const_cast<void*>(reinterpret_cast<const void*>(pawn_->getAmxFunctions().data()));
        pawn_->getEventDispatcher().addEventHandler(this);
    }

    PluginConfig cfg = ConfigManager::LoadOrCreate("scriptfiles/samp-audio-stream/config.json");
    if (core_) {
        IConfig& omp_cfg = core_->getConfig();
        if (auto* p = omp_cfg.getInt("sampaudiostream.http_port"); p && *p > 0) {
            cfg.http_port = static_cast<std::uint16_t>(*p);
        }
        StringView pub_ip = omp_cfg.getString("sampaudiostream.public_ip");
        if (!pub_ip.empty()) {
            cfg.public_ip = std::string(pub_ip.data(), pub_ip.size());
        }
        if (auto* h = omp_cfg.getBool("sampaudiostream.hook_native_audio")) {
            cfg.hook_native_audio = *h;
        }
        if (auto* dbg = omp_cfg.getBool("sampaudiostream.debug")) {
            cfg.debug_logging = *dbg;
        }
    }

    hook_native_audio_ = cfg.hook_native_audio;
    Logger::Instance().SetSink([this](LogSeverity sev, const std::string& msg) {
        this->Log(sev, msg);
    });

    engine_ = std::make_unique<AudioEngine>(*this, cfg);
    SetActiveAudioEngine(engine_.get());
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
        config.setInt("sampaudiostream.http_port", 0);
        config.setString("sampaudiostream.public_ip", "");
        config.setBool("sampaudiostream.hook_native_audio", true);
        config.setBool("sampaudiostream.debug", false);
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
    SetActiveAudioEngine(nullptr);
    engine_.reset();
    delete this;
}

void OmpHost::reset() {
    if (engine_) {
        engine_->Reset();
    }
}

void OmpHost::onTick(Microseconds, TimePoint) {
    if (engine_) {
        engine_->Tick();
    }
}

bool OmpHost::PatchScriptNative(IPawnScript& script, const char* name, AMX_NATIVE replacement, AMX_NATIVE* out_original) {
    if (!name || !replacement) return false;

    int idx = -1;
    if (script.FindNative(name, &idx) != AMX_ERR_NONE || idx < 0) {
        return false;
    }

    AMX_NATIVE_INFO info{};
    if (script.GetNativeByIndex(idx, &info) == AMX_ERR_NONE &&
        info.func && info.func != replacement && out_original && !*out_original) {
        *out_original = info.func;
    }

    script.Register(name, replacement);
    return true;
}

void OmpHost::onAmxLoad(IPawnScript& script) {
    {
        std::lock_guard<std::mutex> lock(scripts_mutex_);
        if (std::find(scripts_.begin(), scripts_.end(), &script) == scripts_.end()) {
            scripts_.push_back(&script);
        }
    }

    const auto& natives = GetPluginNatives();
    if (!natives.empty()) {
        script.Register(natives.data(), static_cast<int>(natives.size() - 1));
    }

    if (hook_native_audio_) {
        PatchScriptNative(script, "PlayAudioStreamForPlayer", Hooked_PlayAudioStreamForPlayer, &orig_play_audio_);
        PatchScriptNative(script, "StopAudioStreamForPlayer", Hooked_StopAudioStreamForPlayer, &orig_stop_audio_);
        SetOriginalAudioNatives(orig_play_audio_, orig_stop_audio_);
    }
}

void OmpHost::onAmxUnload(IPawnScript& script) {
    std::lock_guard<std::mutex> lock(scripts_mutex_);
    scripts_.erase(std::remove(scripts_.begin(), scripts_.end(), &script), scripts_.end());
}

void OmpHost::onPlayerDisconnect(IPlayer& player, PeerDisconnectReason) {
    if (engine_) {
        engine_->OnPlayerDisconnect(static_cast<PlayerId>(player.getID()));
    }
}

void OmpHost::Log(LogSeverity severity, const std::string& message) {
    if (!core_) return;
    ::LogLevel lvl = ::LogLevel::Message;
    switch (severity) {
        case LogSeverity::Debug: lvl = ::LogLevel::Debug; break;
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
