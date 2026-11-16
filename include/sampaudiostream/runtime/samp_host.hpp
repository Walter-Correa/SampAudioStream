#pragma once

#include "sampaudiostream/runtime/platform_host.hpp"

#include <amx/amx.h>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace sampaudiostream {

class AudioEngine;

class SampHost : public IPlatformHost {
public:
    using LogPrintfFn = void (*)(const char* format, ...);

    explicit SampHost(LogPrintfFn logprintf);
    ~SampHost() override = default;

    void SetEngine(AudioEngine* engine);
    void SetHookNativeAudio(bool enabled);

    void OnAmxLoad(AMX* amx);
    void OnAmxUnload(AMX* amx);
    void OnProcessTick();

    // Call original (unhooked) PlayAudioStreamForPlayer / StopAudioStreamForPlayer
    void SendPlayAudioStream(PlayerId player_id, const std::string& url, const Vector3& pos, float distance, bool use_pos) override;
    void SendStopAudioStream(PlayerId player_id) override;

    void Log(LogSeverity severity, const std::string& message) override;

    std::int32_t GetServerPort() const override;
    std::string GetServerBindIp() const override;

    bool IsPlayerConnected(PlayerId player_id) const override;
    std::vector<PlayerId> GetConnectedPlayers() const override;
    bool GetPlayerPos(PlayerId player_id, Vector3& out_pos) const override;
    std::int32_t GetPlayerVirtualWorld(PlayerId player_id) const override;
    std::int32_t GetPlayerInterior(PlayerId player_id) const override;
    std::int32_t GetPlayerVehicleId(PlayerId player_id) const override;
    bool GetVehiclePos(std::int32_t vehicle_id, Vector3& out_pos) const override;
    std::string GetPlayerIp(PlayerId player_id) const override;

    void DispatchPawnCallback(const std::string& name, const std::vector<CallbackArg>& args) override;

    AMX_NATIVE GetOriginalPlayAudioNative() const { return orig_play_audio_; }
    AMX_NATIVE GetOriginalStopAudioNative() const { return orig_stop_audio_; }
    AudioEngine* GetEngine() const { return engine_; }

private:
    AMX* GetPrimaryAmx() const;
    AMX_NATIVE FindNativeFunc(AMX* amx, const char* name) const;
    bool PatchNative(AMX* amx, const char* name, AMX_NATIVE replacement, AMX_NATIVE* out_original);
    void CaptureServerNatives(AMX* amx);

    LogPrintfFn logprintf_{nullptr};
    AudioEngine* engine_{nullptr};
    bool hook_native_audio_{true};

    mutable std::mutex amx_mutex_;
    std::vector<AMX*> loaded_amxs_;

    // Captured original SA-MP server native function pointers
    AMX_NATIVE orig_play_audio_{nullptr};
    AMX_NATIVE orig_stop_audio_{nullptr};
    AMX_NATIVE fn_is_player_connected_{nullptr};
    AMX_NATIVE fn_get_player_pos_{nullptr};
    AMX_NATIVE fn_get_player_vw_{nullptr};
    AMX_NATIVE fn_get_player_interior_{nullptr};
    AMX_NATIVE fn_get_player_vehicle_{nullptr};
    AMX_NATIVE fn_get_vehicle_pos_{nullptr};
    AMX_NATIVE fn_get_player_ip_{nullptr};
    AMX_NATIVE fn_get_server_var_int_{nullptr};
    AMX_NATIVE fn_get_server_var_str_{nullptr};
    AMX_NATIVE fn_get_player_pool_size_{nullptr};
};

} // namespace sampaudiostream
