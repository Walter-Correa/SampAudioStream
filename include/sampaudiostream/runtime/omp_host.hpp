#pragma once

#include "sampaudiostream/runtime/platform_host.hpp"

#include <sdk.hpp>
#include <Server/Components/Pawn/pawn.hpp>
#include <Server/Components/Vehicles/vehicles.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace sampaudiostream {

class AudioEngine;

class OmpHost final : public IComponent,
                      public CoreEventHandler,
                      public PawnEventHandler,
                      public PlayerConnectEventHandler,
                      public PlayerChangeEventHandler,
                      public IPlatformHost {
public:
    PROVIDE_UID(0x53415F415544494FULL);

    OmpHost() = default;
    ~OmpHost() override = default;

    // IComponent
    StringView componentName() const override { return "SampAudioStream"; }
    SemanticVersion componentVersion() const override { return SemanticVersion(2, 1, 0, 0); }
    void onLoad(ICore* c) override;
    void onInit(IComponentList* components) override;
    void onReady() override;
    void onFree(IComponent* component) override;
    void provideConfiguration(ILogger& logger, IEarlyConfig& config, bool defaults) override;
    void free() override;
    void reset() override;

    // CoreEventHandler
    void onTick(Microseconds elapsed, TimePoint now) override;

    // PawnEventHandler
    void onAmxLoad(IPawnScript& script) override;
    void onAmxUnload(IPawnScript& script) override;

    // PlayerConnectEventHandler
    void onPlayerConnect(IPlayer& player) override;
    void onPlayerDisconnect(IPlayer& player, PeerDisconnectReason reason) override;

    // PlayerChangeEventHandler
    void onPlayerNameChange(IPlayer& player, StringView oldName) override;

    // IPlatformHost
    void Log(LogSeverity severity, const std::string& message) override;
    std::int32_t GetServerPort() const override;
    std::string GetServerBindIp() const override;

    bool TracksDisconnectEvents() const override { return true; }
    bool UsesComponentLifecycleEvents() const override { return true; }
    bool TracksGameModeLifecycle() const override { return true; }
    bool IsPlayerConnected(PlayerId player_id) const override;
    std::vector<PlayerId> GetConnectedPlayers() const override;
    std::vector<PlayerSnapshot> GetConnectedPlayerSnapshots() const override;
    bool GetPlayerPos(PlayerId player_id, sampaudiostream::Vector3& out_pos) const override;
    std::int32_t GetPlayerVirtualWorld(PlayerId player_id) const override;
    std::int32_t GetPlayerInterior(PlayerId player_id) const override;
    std::int32_t GetPlayerVehicleId(PlayerId player_id) const override;
    bool GetVehiclePos(std::int32_t vehicle_id, sampaudiostream::Vector3& out_pos) const override;
    std::string GetPlayerIp(PlayerId player_id) const override;
    std::string GetPlayerName(PlayerId player_id) const override;

    void SendPlayAudioStream(PlayerId player_id, const std::string& url, const sampaudiostream::Vector3& pos, float distance, bool use_pos) override;
    void SendStopAudioStream(PlayerId player_id) override;
    void DispatchPawnCallback(const std::string& name, const std::vector<CallbackArg>& args) override;

    AudioEngine* GetEngine() const { return engine_.get(); }
    AMX_NATIVE GetOriginalPlayAudioNative() const { return orig_play_audio_; }
    AMX_NATIVE GetOriginalStopAudioNative() const { return orig_stop_audio_; }
    ICore* GetCore() const { return core_; }

private:
    bool PatchScriptNative(IPawnScript& script, const char* name, AMX_NATIVE replacement, AMX_NATIVE* out_original);
    void PatchPendingScripts();

    ICore* core_{nullptr};
    IPawnComponent* pawn_{nullptr};
    IVehiclesComponent* vehicles_{nullptr};
    std::unique_ptr<AudioEngine> engine_;

    mutable std::mutex scripts_mutex_;
    std::vector<IPawnScript*> scripts_;
    std::vector<IPawnScript*> pending_patch_scripts_;
    std::atomic<bool> has_pending_patches_{false};

    AMX_NATIVE orig_play_audio_{nullptr};
    AMX_NATIVE orig_stop_audio_{nullptr};
    bool hook_native_audio_{true};
    bool instance_acquired_{false};
    IPawnScript* main_script_{nullptr};
    bool gamemode_loaded_pending_tick_{false};
};

} // namespace sampaudiostream
