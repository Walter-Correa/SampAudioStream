#include "sampaudiostream/runtime/samp_host.hpp"

#include "sampaudiostream/engine/audio_engine.hpp"
#include "sampaudiostream/pawn/amx_natives.hpp"
#include "sampaudiostream/runtime/amx_patch.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace sampaudiostream {

SampHost::SampHost(LogPrintfFn logprintf)
    : logprintf_(logprintf),
      server_cfg_(ConfigManager::LoadServerCfg("server.cfg")) {}

void SampHost::SetEngine(AudioEngine* engine) {
    engine_ = engine;
}

void SampHost::SetHookNativeAudio(bool enabled) {
    hook_native_audio_ = enabled;
}

AMX* SampHost::GetPrimaryAmx() const {
    std::lock_guard<std::mutex> lock(amx_mutex_);
    if (gamemode_amx_) return gamemode_amx_;
    return loaded_amxs_.empty() ? nullptr : loaded_amxs_.front();
}

AMX_NATIVE SampHost::FindNativeFunc(AMX* amx, const char* name) const {
    return FindAmxNativeFunc(amx, name);
}

bool SampHost::PatchNative(AMX* amx, const char* name, AMX_NATIVE replacement, AMX_NATIVE* out_original) {
    return PatchAmxNative(amx, name, replacement, out_original);
}

void SampHost::CaptureServerNatives(AMX* amx) {
    if (!fn_is_player_connected_) fn_is_player_connected_ = FindNativeFunc(amx, "IsPlayerConnected");
    if (!fn_get_player_pos_) fn_get_player_pos_ = FindNativeFunc(amx, "GetPlayerPos");
    if (!fn_get_player_vw_) fn_get_player_vw_ = FindNativeFunc(amx, "GetPlayerVirtualWorld");
    if (!fn_get_player_interior_) fn_get_player_interior_ = FindNativeFunc(amx, "GetPlayerInterior");
    if (!fn_get_player_vehicle_) fn_get_player_vehicle_ = FindNativeFunc(amx, "GetPlayerVehicleID");
    if (!fn_get_vehicle_pos_) fn_get_vehicle_pos_ = FindNativeFunc(amx, "GetVehiclePos");
    if (!fn_get_player_ip_) fn_get_player_ip_ = FindNativeFunc(amx, "GetPlayerIp");
    if (!fn_get_player_name_) fn_get_player_name_ = FindNativeFunc(amx, "GetPlayerName");
    if (!fn_get_server_var_int_) {
        fn_get_server_var_int_ = FindNativeFunc(amx, "GetServerVarAsInt");
        if (!fn_get_server_var_int_) fn_get_server_var_int_ = FindNativeFunc(amx, "GetConsoleVarAsInt");
    }
    if (!fn_get_server_var_str_) {
        fn_get_server_var_str_ = FindNativeFunc(amx, "GetServerVarAsString");
        if (!fn_get_server_var_str_) fn_get_server_var_str_ = FindNativeFunc(amx, "GetConsoleVarAsString");
    }
    if (!fn_get_player_pool_size_) fn_get_player_pool_size_ = FindNativeFunc(amx, "GetPlayerPoolSize");

    if (!orig_play_audio_) {
        AMX_NATIVE p = FindNativeFunc(amx, "PlayAudioStreamForPlayer");
        if (p && p != Hooked_PlayAudioStreamForPlayer) orig_play_audio_ = p;
    }
    if (!orig_stop_audio_) {
        AMX_NATIVE s = FindNativeFunc(amx, "StopAudioStreamForPlayer");
        if (s && s != Hooked_StopAudioStreamForPlayer) orig_stop_audio_ = s;
    }
    if (!orig_set_player_name_) {
        AMX_NATIVE sn = FindNativeFunc(amx, "SetPlayerName");
        if (sn && sn != Hooked_SetPlayerName) orig_set_player_name_ = sn;
    }
}

void SampHost::OnAmxLoad(AMX* amx) {
    if (!amx) return;

    int gm_idx = -1;
    bool is_gamemode = (amx_FindPublic(amx, "OnGameModeInit", &gm_idx) == AMX_ERR_NONE ||
                        amx_FindPublic(amx, "OnGameModeExit", &gm_idx) == AMX_ERR_NONE);

    {
        std::lock_guard<std::mutex> lock(amx_mutex_);
        loaded_amxs_.push_back(amx);
        if (is_gamemode) {
            gamemode_amx_ = amx;
        }
    }

    CaptureServerNatives(amx);
    SetOriginalAudioNatives(orig_play_audio_, orig_stop_audio_);
    SetOriginalSetPlayerNameNative(orig_set_player_name_);

    // Register SA_* natives
    const auto& natives = GetPluginNatives();
    amx_Register(amx, const_cast<AMX_NATIVE_INFO*>(natives.data()), -1);

    // Track SetPlayerName across all scripts (for channel creator rename/reconnection tracking)
    PatchNative(amx, "SetPlayerName", Hooked_SetPlayerName, &orig_set_player_name_);
    SetOriginalSetPlayerNameNative(orig_set_player_name_);

    // Hook standard PlayAudioStreamForPlayer / StopAudioStreamForPlayer
    if (hook_native_audio_) {
        PatchNative(amx, "PlayAudioStreamForPlayer", Hooked_PlayAudioStreamForPlayer, &orig_play_audio_);
        PatchNative(amx, "StopAudioStreamForPlayer", Hooked_StopAudioStreamForPlayer, &orig_stop_audio_);
        SetOriginalAudioNatives(orig_play_audio_, orig_stop_audio_);
    }

    if (is_gamemode) {
        {
            std::lock_guard<std::mutex> lock(amx_mutex_);
            gamemode_loaded_pending_tick_ = true;
        }
        auto log_missing_native = [this](AMX_NATIVE fn, const char* name) {
            if (!fn) {
                Log(LogSeverity::Error,
                    std::string("Required SA-MP native '") + name +
                    "' was not found in the gamemode AMX native table. "
                    "Please recompile your gamemode with the updated SampAudioStream.inc included after <a_samp>.");
            }
        };
        log_missing_native(orig_play_audio_, "PlayAudioStreamForPlayer");
        log_missing_native(orig_stop_audio_, "StopAudioStreamForPlayer");
        log_missing_native(fn_is_player_connected_, "IsPlayerConnected");
    }
}

void SampHost::OnAmxUnload(AMX* amx) {
    bool reset_engine = false;
    {
        std::lock_guard<std::mutex> lock(amx_mutex_);
        loaded_amxs_.erase(
            std::remove(loaded_amxs_.begin(), loaded_amxs_.end(), amx),
            loaded_amxs_.end());
        if (amx == gamemode_amx_ || loaded_amxs_.empty()) {
            if (gamemode_amx_ == amx) {
                gamemode_amx_ = nullptr;
                reset_engine = true;
            } else if (loaded_amxs_.empty()) {
                reset_engine = true;
            }
        }
    }

    if (reset_engine) {
        disconnect_event_seen_.store(false);
        {
            std::lock_guard<std::mutex> lock(amx_mutex_);
            gamemode_loaded_pending_tick_ = false;
        }
        if (engine_) {
            engine_->NotifyLifecycleStopping();
            engine_->Reset();
        }
    }
}

void SampHost::OnProcessTick() {
    bool notify_gm_loaded = false;
    {
        std::lock_guard<std::mutex> lock(amx_mutex_);
        if (gamemode_loaded_pending_tick_ && gamemode_amx_) {
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

void SampHost::Log(LogSeverity severity, const std::string& message) {
    if (!logprintf_) return;
    if (severity == LogSeverity::Info) {
        logprintf_("[SampAudioStream] %s", message.c_str());
        return;
    }
    const char* tag = "INFO";
    switch (severity) {
        case LogSeverity::Debug: tag = "DEBUG"; break;
        case LogSeverity::Info: break;
        case LogSeverity::Warning: tag = "WARN"; break;
        case LogSeverity::Error: tag = "ERROR"; break;
    }
    logprintf_("[SampAudioStream] [%s] %s", tag, message.c_str());
}

std::int32_t SampHost::ReadServerVarInt(const char* name, std::int32_t fallback) const {
    AMX* amx = GetPrimaryAmx();
    if (!amx || !fn_get_server_var_int_) {
        return fallback;
    }

    cell amx_addr = 0;
    cell* phys_addr = nullptr;
    if (amx_Allot(amx, 16, &amx_addr, &phys_addr) != AMX_ERR_NONE) {
        return fallback;
    }
    amx_SetString(phys_addr, name, 0, 0, 16);
    cell params[2] = {sizeof(cell), amx_addr};
    cell ret = fn_get_server_var_int_(amx, params);
    amx_Release(amx, amx_addr);
    return ret > 0 ? static_cast<std::int32_t>(ret) : fallback;
}

std::int32_t SampHost::GetServerPort() const {
    const std::int32_t fallback = server_cfg_.port > 0 ? server_cfg_.port : 7777;
    return ReadServerVarInt("port", fallback);
}

std::int32_t SampHost::GetMaxPlayers() const {
    const std::int32_t fallback = server_cfg_.has_max_players ? server_cfg_.max_players : 0;
    return ReadServerVarInt("maxplayers", fallback);
}

std::string SampHost::GetServerBindIp() const {
    AMX* amx = GetPrimaryAmx();
    if (!amx || !fn_get_server_var_str_) {
        return server_cfg_.bind_ip.empty() ? "0.0.0.0" : server_cfg_.bind_ip;
    }

    cell var_addr = 0, out_addr = 0;
    cell* var_phys = nullptr;
    cell* out_phys = nullptr;
    if (amx_Allot(amx, 8, &var_addr, &var_phys) != AMX_ERR_NONE) {
        return server_cfg_.bind_ip.empty() ? "0.0.0.0" : server_cfg_.bind_ip;
    }
    if (amx_Allot(amx, 64, &out_addr, &out_phys) != AMX_ERR_NONE) {
        amx_Release(amx, var_addr);
        return server_cfg_.bind_ip.empty() ? "0.0.0.0" : server_cfg_.bind_ip;
    }
    amx_SetString(var_phys, "bind", 0, 0, 8);
    cell params[4] = {3 * sizeof(cell), var_addr, out_addr, 64};
    fn_get_server_var_str_(amx, params);
    char buf[64]{};
    amx_GetString(buf, out_phys, 0, sizeof(buf));
    amx_Release(amx, var_addr);
    return buf[0] != '\0' ? std::string(buf) : (server_cfg_.bind_ip.empty() ? "0.0.0.0" : server_cfg_.bind_ip);
}

bool SampHost::IsPlayerConnected(PlayerId player_id) const {
    AMX* amx = GetPrimaryAmx();
    if (!amx || !fn_is_player_connected_ || player_id < 0) return false;
    cell params[2] = {sizeof(cell), static_cast<cell>(player_id)};
    return fn_is_player_connected_(amx, params) != 0;
}

std::vector<PlayerId> SampHost::GetConnectedPlayers() const {
    std::vector<PlayerId> result;
    AMX* amx = GetPrimaryAmx();
    if (!amx || !fn_is_player_connected_) return result;

    std::int32_t max_id = 999;
    if (fn_get_player_pool_size_) {
        cell p[1] = {0};
        max_id = static_cast<std::int32_t>(fn_get_player_pool_size_(amx, p));
        if (max_id < 0) max_id = 0;
        if (max_id > 1000) max_id = 1000;
    }
    for (std::int32_t pid = 0; pid <= max_id; ++pid) {
        cell params[2] = {sizeof(cell), static_cast<cell>(pid)};
        if (fn_is_player_connected_(amx, params) != 0) {
            result.push_back(pid);
        }
    }
    return result;
}

std::vector<PlayerSnapshot> SampHost::GetConnectedPlayerSnapshots() const {
    std::vector<PlayerSnapshot> result;
    AMX* amx = GetPrimaryAmx();
    if (!amx || !fn_is_player_connected_) return result;

    std::int32_t max_id = 999;
    if (fn_get_player_pool_size_) {
        cell p[1] = {0};
        max_id = static_cast<std::int32_t>(fn_get_player_pool_size_(amx, p));
        if (max_id < 0) max_id = 0;
        if (max_id > 1000) max_id = 1000;
    }

    cell pos_addr = 0;
    cell* pos_phys = nullptr;
    bool has_pos_alloc = fn_get_player_pos_ && (amx_Allot(amx, 3, &pos_addr, &pos_phys) == AMX_ERR_NONE);

    for (std::int32_t pid = 0; pid <= max_id; ++pid) {
        cell c_params[2] = {sizeof(cell), static_cast<cell>(pid)};
        if (fn_is_player_connected_(amx, c_params) == 0) {
            continue;
        }
        PlayerSnapshot snap;
        snap.id = pid;
        if (has_pos_alloc) {
            cell p_params[5] = {
                4 * sizeof(cell),
                static_cast<cell>(pid),
                pos_addr,
                pos_addr + static_cast<cell>(sizeof(cell)),
                pos_addr + static_cast<cell>(2 * sizeof(cell))
            };
            if (fn_get_player_pos_(amx, p_params) != 0) {
                snap.pos = {amx_ctof(pos_phys[0]), amx_ctof(pos_phys[1]), amx_ctof(pos_phys[2])};
            }
        }
        if (fn_get_player_vw_) {
            snap.world_id = static_cast<std::int32_t>(fn_get_player_vw_(amx, c_params));
        }
        if (fn_get_player_interior_) {
            snap.interior_id = static_cast<std::int32_t>(fn_get_player_interior_(amx, c_params));
        }
        result.push_back(snap);
    }

    if (has_pos_alloc) {
        amx_Release(amx, pos_addr);
    }
    return result;
}

bool SampHost::GetPlayerPos(PlayerId player_id, Vector3& out_pos) const {
    AMX* amx = GetPrimaryAmx();
    if (!amx || !fn_get_player_pos_) return false;

    cell amx_addr = 0;
    cell* phys = nullptr;
    if (amx_Allot(amx, 3, &amx_addr, &phys) != AMX_ERR_NONE) return false;

    cell params[5] = {
        4 * sizeof(cell),
        static_cast<cell>(player_id),
        amx_addr,
        amx_addr + static_cast<cell>(sizeof(cell)),
        amx_addr + static_cast<cell>(2 * sizeof(cell))
    };
    cell ret = fn_get_player_pos_(amx, params);
    out_pos.x = amx_ctof(phys[0]);
    out_pos.y = amx_ctof(phys[1]);
    out_pos.z = amx_ctof(phys[2]);
    amx_Release(amx, amx_addr);
    return ret != 0;
}

std::int32_t SampHost::GetPlayerVirtualWorld(PlayerId player_id) const {
    AMX* amx = GetPrimaryAmx();
    if (!amx || !fn_get_player_vw_) return 0;
    cell params[2] = {sizeof(cell), static_cast<cell>(player_id)};
    return static_cast<std::int32_t>(fn_get_player_vw_(amx, params));
}

std::int32_t SampHost::GetPlayerInterior(PlayerId player_id) const {
    AMX* amx = GetPrimaryAmx();
    if (!amx || !fn_get_player_interior_) return 0;
    cell params[2] = {sizeof(cell), static_cast<cell>(player_id)};
    return static_cast<std::int32_t>(fn_get_player_interior_(amx, params));
}

std::int32_t SampHost::GetPlayerVehicleId(PlayerId player_id) const {
    AMX* amx = GetPrimaryAmx();
    if (!amx || !fn_get_player_vehicle_) return 0;
    cell params[2] = {sizeof(cell), static_cast<cell>(player_id)};
    return static_cast<std::int32_t>(fn_get_player_vehicle_(amx, params));
}

bool SampHost::GetVehiclePos(std::int32_t vehicle_id, Vector3& out_pos) const {
    AMX* amx = GetPrimaryAmx();
    if (!amx || !fn_get_vehicle_pos_ || vehicle_id <= 0) return false;

    cell amx_addr = 0;
    cell* phys = nullptr;
    if (amx_Allot(amx, 3, &amx_addr, &phys) != AMX_ERR_NONE) return false;

    cell params[5] = {
        4 * sizeof(cell),
        static_cast<cell>(vehicle_id),
        amx_addr,
        amx_addr + static_cast<cell>(sizeof(cell)),
        amx_addr + static_cast<cell>(2 * sizeof(cell))
    };
    cell ret = fn_get_vehicle_pos_(amx, params);
    out_pos.x = amx_ctof(phys[0]);
    out_pos.y = amx_ctof(phys[1]);
    out_pos.z = amx_ctof(phys[2]);
    amx_Release(amx, amx_addr);
    return ret != 0;
}

std::string SampHost::GetPlayerIp(PlayerId player_id) const {
    AMX* amx = GetPrimaryAmx();
    if (!amx || !fn_get_player_ip_) return "";

    cell amx_addr = 0;
    cell* phys = nullptr;
    if (amx_Allot(amx, 32, &amx_addr, &phys) != AMX_ERR_NONE) return "";

    cell params[4] = {3 * sizeof(cell), static_cast<cell>(player_id), amx_addr, 32};
    fn_get_player_ip_(amx, params);
    char buf[32]{};
    amx_GetString(buf, phys, 0, sizeof(buf));
    amx_Release(amx, amx_addr);
    return std::string(buf);
}

std::string SampHost::GetPlayerName(PlayerId player_id) const {
    AMX* amx = GetPrimaryAmx();
    if (!amx || !fn_get_player_name_) return "";

    cell amx_addr = 0;
    cell* phys = nullptr;
    if (amx_Allot(amx, 32, &amx_addr, &phys) != AMX_ERR_NONE) return "";

    cell params[4] = {3 * sizeof(cell), static_cast<cell>(player_id), amx_addr, 32};
    fn_get_player_name_(amx, params);
    char buf[32]{};
    amx_GetString(buf, phys, 0, sizeof(buf));
    amx_Release(amx, amx_addr);
    return std::string(buf);
}

void SampHost::SendPlayAudioStream(PlayerId player_id, const std::string& url, const Vector3& pos, float distance, bool use_pos) {
    AMX* amx = GetPrimaryAmx();
    if (!amx || !orig_play_audio_) return;

    cell str_cells = static_cast<cell>(url.size()) + 1;
    cell amx_addr = 0;
    cell* phys = nullptr;
    if (amx_Allot(amx, str_cells, &amx_addr, &phys) != AMX_ERR_NONE) return;
    amx_SetString(phys, url.c_str(), 0, 0, static_cast<std::size_t>(str_cells));

    cell params[8] = {
        7 * sizeof(cell),
        static_cast<cell>(player_id),
        amx_addr,
        amx_ftoc(pos.x),
        amx_ftoc(pos.y),
        amx_ftoc(pos.z),
        amx_ftoc(distance),
        use_pos ? 1 : 0
    };
    orig_play_audio_(amx, params);
    amx_Release(amx, amx_addr);
}

void SampHost::SendStopAudioStream(PlayerId player_id) {
    AMX* amx = GetPrimaryAmx();
    if (!amx || !orig_stop_audio_) return;
    cell params[2] = {sizeof(cell), static_cast<cell>(player_id)};
    orig_stop_audio_(amx, params);
}

void SampHost::DispatchPawnCallback(const std::string& name, const std::vector<CallbackArg>& args) {
    std::vector<AMX*> snapshot;
    {
        std::lock_guard<std::mutex> lock(amx_mutex_);
        snapshot = loaded_amxs_;
    }

    for (AMX* amx : snapshot) {
        int idx = -1;
        if (amx_FindPublic(amx, name.c_str(), &idx) != AMX_ERR_NONE || idx < 0) {
            continue;
        }

        cell reset_hea = amx->hea;
        // Pawn pushes arguments in reverse order
        for (auto it = args.rbegin(); it != args.rend(); ++it) {
            if (std::holds_alternative<std::int32_t>(*it)) {
                amx_Push(amx, static_cast<cell>(std::get<std::int32_t>(*it)));
            } else if (std::holds_alternative<float>(*it)) {
                float f = std::get<float>(*it);
                amx_Push(amx, amx_ftoc(f));
            } else if (std::holds_alternative<std::string>(*it)) {
                const auto& s = std::get<std::string>(*it);
                amx_PushString(amx, nullptr, nullptr, s.c_str(), 0, 0);
            }
        }

        cell ret = 0;
        amx_Exec(amx, &ret, idx);
        amx_Release(amx, reset_hea);
    }
}

} // namespace sampaudiostream
