#pragma once

#include <amx/amx.h>
#include <vector>

namespace sampaudiostream {

class AudioEngine;

void SetActiveAudioEngine(AudioEngine* engine);
AudioEngine* GetActiveAudioEngine();

void SetOriginalAudioNatives(AMX_NATIVE play_orig, AMX_NATIVE stop_orig);
AMX_NATIVE GetOriginalPlayAudioNative();
AMX_NATIVE GetOriginalStopAudioNative();

const std::vector<AMX_NATIVE_INFO>& GetPluginNatives();

cell AMX_NATIVE_CALL Hooked_PlayAudioStreamForPlayer(AMX* amx, cell* params);
cell AMX_NATIVE_CALL Hooked_StopAudioStreamForPlayer(AMX* amx, cell* params);

} // namespace sampaudiostream
