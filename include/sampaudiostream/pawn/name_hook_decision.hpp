#pragma once

#include "sampaudiostream/core/types.hpp"
#include "sampaudiostream/engine/audio_engine.hpp"

#include <string>

namespace sampaudiostream {

// Shared decision helper for Hooked_SetPlayerName so unit tests can verify return-value gating
// (only original return == 1 propagates OnPlayerNameChanged to AudioEngine; 0 / -1 / null do not).
template <typename AmxPtr, typename CellT, typename OrigFn>
inline CellT ExecuteSetPlayerNameHook(
    AudioEngine* engine,
    OrigFn orig_fn,
    AmxPtr amx,
    CellT* params,
    PlayerId player_id,
    const std::string& new_name) {
    if (!orig_fn) {
        return static_cast<CellT>(0);
    }
    const CellT ret = orig_fn(amx, params);
    if (ret == static_cast<CellT>(1) && engine) {
        engine->OnPlayerNameChanged(player_id, new_name);
    }
    return ret;
}

} // namespace sampaudiostream
