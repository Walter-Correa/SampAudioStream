#include "sampaudiostream/runtime/samp_host.hpp"

#include "sampaudiostream/engine/audio_engine.hpp"
#include "sampaudiostream/pawn/amx_natives.hpp"

#include <cstdint>
#include <cstring>

namespace sampaudiostream {

namespace {

AMX_FUNCSTUBNT* GetNativeEntry(AMX* amx, int index) {
    auto* hdr = reinterpret_cast<AMX_HEADER*>(amx->base);
    auto* table = reinterpret_cast<unsigned char*>(amx->base + hdr->natives);
    return reinterpret_cast<AMX_FUNCSTUBNT*>(table + static_cast<std::size_t>(index) * static_cast<std::size_t>(hdr->defsize));
}

} // namespace

SampHost::SampHost(LogPrintfFn logprintf)
    : logprintf_(logprintf) {}

void SampHost::SetEngine(AudioEngine* engine) {
    engine_ = engine;
}

void SampHost::SetHookNativeAudio(bool enabled) {
    hook_native_audio_ = enabled;
}

AMX* SampHost::GetPrimaryAmx() const {
    std::lock_guard<std::mutex> lock(amx_mutex_);
    return loaded_amxs_.empty() ? nullptr : loaded_amxs_.front();
}

AMX_NATIVE SampHost::FindNativeFunc(AMX* amx, const char* name) const {
#if INTPTR_MAX == INT32_MAX
    if (!amx || !name) return nullptr;
    int idx = -1;
    if (amx_FindNative(amx, name, &idx) != AMX_ERR_NONE || idx < 0) {
        return nullptr;
    }
    auto* hdr = reinterpret_cast<AMX_HEADER*>(amx->base);
    if (hdr->defsize != sizeof(AMX_FUNCSTUBNT)) {
        auto* stub = reinterpret_cast<AMX_FUNCSTUB*>(
            amx->base + hdr->natives + static_cast<std::size_t>(idx) * static_cast<std::size_t>(hdr->defsize));
        return reinterpret_cast<AMX_NATIVE>(static_cast<std::uintptr_t>(stub->address));
    }
    AMX_FUNCSTUBNT* entry = GetNativeEntry(amx, idx);
    return reinterpret_cast<AMX_NATIVE>(static_cast<std::uintptr_t>(entry->address));
#else
    (void)amx;
    (void)name;
    return nullptr;
#endif
}

bool SampHost::PatchNative(AMX* amx, const char* name, AMX_NATIVE replacement, AMX_NATIVE* out_original) {
#if INTPTR_MAX == INT32_MAX
    if (!amx || !name || !replacement) return false;
    int idx = -1;
    if (amx_FindNative(amx, name, &idx) != AMX_ERR_NONE || idx < 0) {
        return false;
    }
    auto* hdr = reinterpret_cast<AMX_HEADER*>(amx->base);
    if (hdr->defsize != sizeof(AMX_FUNCSTUBNT)) {
        auto* stub = reinterpret_cast<AMX_FUNCSTUB*>(
            amx->base + hdr->natives + static_cast<std::size_t>(idx) * static_cast<std::size_t>(hdr->defsize));
        auto orig = reinterpret_cast<AMX_NATIVE>(static_cast<std::uintptr_t>(stub->address));
        if (orig && orig != replacement && out_original && !*out_original) {
            *out_original = orig;
        }
        stub->address = static_cast<ucell>(reinterpret_cast<std::uintptr_t>(replacement));
        return true;
    }

    AMX_FUNCSTUBNT* entry = GetNativeEntry(amx, idx);
    auto orig = reinterpret_cast<AMX_NATIVE>(static_cast<std::uintptr_t>(entry->address));
    if (orig && orig != replacement && out_original && !*out_original) {
        *out_original = orig;
    }
    entry->address = static_cast<ucell>(reinterpret_cast<std::uintptr_t>(replacement));
    return true;
#else
    (void)amx;
    (void)name;
    (void)replacement;
    (void)out_original;
    return false;
#endif
}

void SampHost::CaptureServerNatives(AMX* amx) {
    if (!fn_is_player_connected_) fn_is_player_connected_ = FindNativeFunc(amx, "IsPlayerConnected");
    if (!fn_get_player_pos_) fn_get_player_pos_ = FindNativeFunc(amx, "GetPlayerPos");
    if (!fn_get_player_vw_) fn_get_player_vw_ = FindNativeFunc(amx, "GetPlayerVirtualWorld");
    if (!fn_get_player_interior_) fn_get_player_interior_ = FindNativeFunc(amx, "GetPlayerInterior");
    if (!fn_get_player_vehicle_) fn_get_player_vehicle_ = FindNativeFunc(amx, "GetPlayerVehicleID");
    if (!fn_get_vehicle_pos_) fn_get_vehicle_pos_ = FindNativeFunc(amx, "GetVehiclePos");
    if (!fn_get_player_ip_) fn_get_player_ip_ = FindNativeFunc(amx, "GetPlayerIp");
    if (!fn_get_server_var_int_) fn_get_server_var_int_ = FindNativeFunc(amx, "GetServerVarAsInt");
    if (!fn_get_server_var_str_) fn_get_server_var_str_ = FindNativeFunc(amx, "GetServerVarAsString");
    if (!fn_get_player_pool_size_) fn_get_player_pool_size_ = FindNativeFunc(amx, "GetPlayerPoolSize");

    if (!orig_play_audio_) {
        AMX_NATIVE p = FindNativeFunc(amx, "PlayAudioStreamForPlayer");
        if (p && p != Hooked_PlayAudioStreamForPlayer) orig_play_audio_ = p;
    }
    if (!orig_stop_audio_) {
        AMX_NATIVE s = FindNativeFunc(amx, "StopAudioStreamForPlayer");
        if (s && s != Hooked_StopAudioStreamForPlayer) orig_stop_audio_ = s;
    }
}

void SampHost::OnAmxLoad(AMX* amx) {
    if (!amx) return;

    {
        std::lock_guard<std::mutex> lock(amx_mutex_);
        loaded_amxs_.push_back(amx);
    }

    CaptureServerNatives(amx);
    SetOriginalAudioNatives(orig_play_audio_, orig_stop_audio_);

    // Register SA_* natives
    const auto& natives = GetPluginNatives();
    amx_Register(amx, const_cast<AMX_NATIVE_INFO*>(natives.data()), -1);

    // Hook standard PlayAudioStreamForPlayer / StopAudioStreamForPlayer
    if (hook_native_audio_) {
        PatchNative(amx, "PlayAudioStreamForPlayer", Hooked_PlayAudioStreamForPlayer, &orig_play_audio_);
        PatchNative(amx, "StopAudioStreamForPlayer", Hooked_StopAudioStreamForPlayer, &orig_stop_audio_);
        SetOriginalAudioNatives(orig_play_audio_, orig_stop_audio_);
    }
}

void SampHost::OnAmxUnload(AMX* amx) {
    std::lock_guard<std::mutex> lock(amx_mutex_);
    loaded_amxs_.erase(
        std::remove(loaded_amxs_.begin(), loaded_amxs_.end(), amx),
        loaded_amxs_.end());
}

void SampHost::OnProcessTick() {
    if (engine_) {
        engine_->Tick();
    }
}

void SampHost::Log(LogSeverity severity, const std::string& message) {
    if (!logprintf_) return;
    const char* tag = "INFO";
    switch (severity) {
        case LogSeverity::Debug: tag = "DEBUG"; break;
        case LogSeverity::Info: tag = "INFO"; break;
        case LogSeverity::Warning: tag = "WARN"; break;
        case LogSeverity::Error: tag = "ERROR"; break;
    }
    logprintf_("[SampAudioStream] [%s] %s", tag, message.c_str());
}

std::int32_t SampHost::GetServerPort() const {
    AMX* amx = GetPrimaryAmx();
    if (!amx || !fn_get_server_var_int_) return 7777;

    cell amx_addr = 0;
    cell* phys_addr = nullptr;
    if (amx_Allot(amx, 8, &amx_addr, &phys_addr) != AMX_ERR_NONE) return 7777;
    amx_SetString(phys_addr, "port", 0, 0, 8);
    cell params[2] = {sizeof(cell), amx_addr};
    cell ret = fn_get_server_var_int_(amx, params);
    amx_Release(amx, amx_addr);
    return ret > 0 ? static_cast<std::int32_t>(ret) : 7777;
}

std::string SampHost::GetServerBindIp() const {
    AMX* amx = GetPrimaryAmx();
    if (!amx || !fn_get_server_var_str_) return "0.0.0.0";

    cell var_addr = 0, out_addr = 0;
    cell* var_phys = nullptr;
    cell* out_phys = nullptr;
    if (amx_Allot(amx, 8, &var_addr, &var_phys) != AMX_ERR_NONE) return "0.0.0.0";
    if (amx_Allot(amx, 64, &out_addr, &out_phys) != AMX_ERR_NONE) {
        amx_Release(amx, var_addr);
        return "0.0.0.0";
    }
    amx_SetString(var_phys, "bind", 0, 0, 8);
    cell params[4] = {3 * sizeof(cell), var_addr, out_addr, 64};
    fn_get_server_var_str_(amx, params);
    char buf[64]{};
    amx_GetString(buf, out_phys, 0, sizeof(buf));
    amx_Release(amx, var_addr);
    return std::string(buf);
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
