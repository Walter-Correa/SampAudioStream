#pragma once

#include "sampaudiostream/core/logger.hpp"
#include "sampaudiostream/core/types.hpp"

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace sampaudiostream {

using CallbackArg = std::variant<std::int32_t, float, std::string>;

struct PlayerSnapshot {
    PlayerId id{INVALID_PLAYER_ID};
    Vector3 pos{};
    std::int32_t world_id{0};
    std::int32_t interior_id{0};
};

class IPlatformHost {
public:
    virtual ~IPlatformHost() = default;

    virtual void Log(LogSeverity severity, const std::string& message) = 0;

    virtual std::int32_t GetServerPort() const = 0;
    virtual std::string GetServerBindIp() const = 0;

    virtual bool TracksDisconnectEvents() const { return false; }
    virtual bool UsesComponentLifecycleEvents() const { return false; }
    virtual bool TracksGameModeLifecycle() const { return false; }
    virtual void OnDisconnectHookObserved() {}

    virtual bool IsPlayerConnected(PlayerId player_id) const = 0;
    virtual std::vector<PlayerId> GetConnectedPlayers() const = 0;
    virtual bool GetPlayerPos(PlayerId player_id, Vector3& out_pos) const = 0;
    virtual std::int32_t GetPlayerVirtualWorld(PlayerId player_id) const = 0;
    virtual std::int32_t GetPlayerInterior(PlayerId player_id) const = 0;

    virtual std::vector<PlayerSnapshot> GetConnectedPlayerSnapshots() const {
        std::vector<PlayerSnapshot> out;
        auto ids = GetConnectedPlayers();
        out.reserve(ids.size());
        for (PlayerId pid : ids) {
            PlayerSnapshot snap;
            snap.id = pid;
            GetPlayerPos(pid, snap.pos);
            snap.world_id = GetPlayerVirtualWorld(pid);
            snap.interior_id = GetPlayerInterior(pid);
            out.push_back(snap);
        }
        return out;
    }
    virtual std::int32_t GetPlayerVehicleId(PlayerId player_id) const = 0;
    virtual bool GetVehiclePos(std::int32_t vehicle_id, Vector3& out_pos) const = 0;
    virtual std::string GetPlayerIp(PlayerId player_id) const = 0;
    virtual std::string GetPlayerName(PlayerId player_id) const { (void)player_id; return ""; }

    virtual void SendPlayAudioStream(PlayerId player_id, const std::string& url, const Vector3& pos, float distance, bool use_pos) = 0;
    virtual void SendStopAudioStream(PlayerId player_id) = 0;

    virtual void DispatchPawnCallback(const std::string& name, const std::vector<CallbackArg>& args) = 0;
};

} // namespace sampaudiostream
