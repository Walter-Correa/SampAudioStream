#include "sampaudiostream/pawn/amx_natives.hpp"

#include "sampaudiostream/core/config.hpp"
#include "sampaudiostream/engine/audio_engine.hpp"
#include "sampaudiostream/extractor/resolver.hpp"
#include "sampaudiostream/pawn/name_hook_decision.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sampaudiostream {

namespace {

AudioEngine* g_engine = nullptr;
AMX_NATIVE g_orig_play_audio = nullptr;
AMX_NATIVE g_orig_stop_audio = nullptr;
AMX_NATIVE g_orig_set_player_name = nullptr;

std::string ReadAmxString(AMX* amx, cell param_addr) {
    cell* addr = nullptr;
    if (amx_GetAddr(amx, param_addr, &addr) != AMX_ERR_NONE || !addr) {
        return "";
    }
    int len = 0;
    amx_StrLen(addr, &len);
    if (len <= 0) {
        return "";
    }
    std::string result(static_cast<std::size_t>(len) + 1, '\0');
    amx_GetString(result.data(), addr, 0, result.size());
    if (!result.empty() && result.back() == '\0') {
        result.pop_back();
    }
    // Trim trailing nulls if any
    auto nul = result.find('\0');
    if (nul != std::string::npos) {
        result.resize(nul);
    }
    return result;
}

void WriteAmxString(AMX* amx, cell param_addr, const std::string& str, int max_len) {
    if (max_len <= 0) return;
    cell* addr = nullptr;
    if (amx_GetAddr(amx, param_addr, &addr) == AMX_ERR_NONE && addr) {
        amx_SetString(addr, str.c_str(), 0, 0, static_cast<std::size_t>(max_len));
    }
}

void WriteAmxCell(AMX* amx, cell param_addr, cell value) {
    cell* addr = nullptr;
    if (amx_GetAddr(amx, param_addr, &addr) == AMX_ERR_NONE && addr) {
        *addr = value;
    }
}

StreamOptions ParseStreamOptions(cell* params, std::size_t start_idx, std::size_t argc) {
    StreamOptions opts;
    if (argc >= start_idx + 1) opts.position.x = amx_ctof(params[start_idx + 1]);
    if (argc >= start_idx + 2) opts.position.y = amx_ctof(params[start_idx + 2]);
    if (argc >= start_idx + 3) opts.position.z = amx_ctof(params[start_idx + 3]);
    if (argc >= start_idx + 4) opts.distance = amx_ctof(params[start_idx + 4]);
    if (argc >= start_idx + 5) opts.use_position = (params[start_idx + 5] != 0);
    if (argc >= start_idx + 6) opts.world_id = static_cast<std::int32_t>(params[start_idx + 6]);
    if (argc >= start_idx + 7) opts.interior_id = static_cast<std::int32_t>(params[start_idx + 7]);
    if (argc >= start_idx + 8) opts.volume = amx_ctof(params[start_idx + 8]);
    if (argc >= start_idx + 9) opts.notify_processing = (params[start_idx + 9] != 0);
    if (argc >= start_idx + 10) opts.process_direct_audio_files = (params[start_idx + 10] != 0);
    if (argc >= start_idx + 11) opts.stream_text = (params[start_idx + 11] != 0);
    return opts;
}

// SA_PlayAudioStreamForPlayer(playerid, const url[], Float:posX = 0.0, Float:posY = 0.0, Float:posZ = 0.0, Float:distance = 50.0, bool:usepos = false, worldid = -1, interiorid = -1, Float:volume = 100.0, bool:notify_processing = true, bool:process_direct_audio_files = false, bool:stream_text = true)
cell AMX_NATIVE_CALL n_SA_PlayAudioStreamForPlayer(AMX* amx, cell* params) {
    if (!g_engine) return INVALID_STREAM_ID;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 2) return INVALID_STREAM_ID;

    PlayerId player_id = static_cast<PlayerId>(params[1]);
    std::string raw_url = ReadAmxString(amx, params[2]);
    std::string url = ConvertTextFromPawn(raw_url, g_engine->GetPawnTextEncoding());
    StreamOptions opts = ParseStreamOptions(params, 2, argc);

    return static_cast<cell>(g_engine->PlayAudioStreamForPlayer(player_id, url, opts, raw_url));
}

// SA_PlayAudioStream(const url[], Float:posX = 0.0, Float:posY = 0.0, Float:posZ = 0.0, Float:distance = 50.0, bool:usepos = false, worldid = -1, interiorid = -1, Float:volume = 100.0, bool:notify_processing = true, bool:process_direct_audio_files = false, bool:stream_text = true)
cell AMX_NATIVE_CALL n_SA_PlayAudioStream(AMX* amx, cell* params) {
    if (!g_engine) return INVALID_STREAM_ID;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 1) return INVALID_STREAM_ID;

    std::string raw_url = ReadAmxString(amx, params[1]);
    std::string url = ConvertTextFromPawn(raw_url, g_engine->GetPawnTextEncoding());
    StreamOptions opts = ParseStreamOptions(params, 1, argc);

    return static_cast<cell>(g_engine->PlayAudioStream(url, opts, raw_url));
}

// SA_PlayAudioStreamSoundForPlayer(playerid, const sound[], Float:posX = 0.0, Float:posY = 0.0, Float:posZ = 0.0, Float:distance = 50.0, bool:usepos = false)
cell AMX_NATIVE_CALL n_SA_PlayAudioStreamSoundForPlayer(AMX* amx, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 2) return 0;

    PlayerId player_id = static_cast<PlayerId>(params[1]);
    std::string sound = ConvertTextFromPawn(ReadAmxString(amx, params[2]), g_engine->GetPawnTextEncoding());
    Vector3 position{};
    float distance = 50.0f;
    bool use_position = false;
    if (argc >= 3) position.x = amx_ctof(params[3]);
    if (argc >= 4) position.y = amx_ctof(params[4]);
    if (argc >= 5) position.z = amx_ctof(params[5]);
    if (argc >= 6) distance = amx_ctof(params[6]);
    if (argc >= 7) use_position = (params[7] != 0);
    return g_engine->PlayAudioStreamSoundForPlayer(player_id, sound, position, distance, use_position) ? 1 : 0;
}

// SA_StopAudioStreamForPlayer(playerid)
cell AMX_NATIVE_CALL n_SA_StopAudioStreamForPlayer(AMX*, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 1) return 0;
    return g_engine->StopAudioStreamForPlayer(static_cast<PlayerId>(params[1])) ? 1 : 0;
}

// SA_StopAudioStream(streamid)
cell AMX_NATIVE_CALL n_SA_StopAudioStream(AMX*, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 1) return 0;
    return g_engine->StopAudioStream(static_cast<StreamId>(params[1])) ? 1 : 0;
}

// SA_PauseAudioStreamForPlayer(playerid, bool:pause)
cell AMX_NATIVE_CALL n_SA_PauseAudioStreamForPlayer(AMX*, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 2) return 0;
    return g_engine->PauseAudioStreamForPlayer(static_cast<PlayerId>(params[1]), params[2] != 0) ? 1 : 0;
}

// SA_PauseAudioStream(streamid, bool:pause)
cell AMX_NATIVE_CALL n_SA_PauseAudioStream(AMX*, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 2) return 0;
    return g_engine->PauseAudioStream(static_cast<StreamId>(params[1]), params[2] != 0) ? 1 : 0;
}

// SA_SeekAudioStream(streamid, seconds)
cell AMX_NATIVE_CALL n_SA_SeekAudioStream(AMX*, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 2) return 0;
    return g_engine->SeekAudioStream(static_cast<StreamId>(params[1]), static_cast<std::int32_t>(params[2])) ? 1 : 0;
}

// SA_SetPlayerAudioStreamMuted(playerid, bool:mute, bool:include_personal = false)
cell AMX_NATIVE_CALL n_SA_SetPlayerAudioStreamMuted(AMX*, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 2) return 0;
    bool include_personal = (argc >= 3) ? (params[3] != 0) : false;
    return g_engine->SetPlayerAudioStreamMuted(
        static_cast<PlayerId>(params[1]),
        params[2] != 0,
        include_personal) ? 1 : 0;
}

// SA_SetPlayerAudioStreamGroupMuted(playerid, group, bool:mute)
cell AMX_NATIVE_CALL n_SA_SetPlayerAudioStreamGroupMuted(AMX*, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 3) return 0;
    return g_engine->SetPlayerRangeGroupMuted(
        static_cast<PlayerId>(params[1]),
        static_cast<std::int32_t>(params[2]),
        params[3] != 0) ? 1 : 0;
}

// SA_IsPlayerAudioStreamGroupMuted(playerid, group)
cell AMX_NATIVE_CALL n_SA_IsPlayerAudioStreamGroupMuted(AMX*, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 2) return 0;
    return g_engine->IsPlayerRangeGroupMuted(
        static_cast<PlayerId>(params[1]),
        static_cast<std::int32_t>(params[2])) ? 1 : 0;
}

// SA_IsPlayerAudioStreamMuted(playerid)
cell AMX_NATIVE_CALL n_SA_IsPlayerAudioStreamMuted(AMX*, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 1) return 0;
    return g_engine->IsPlayerAudioStreamMuted(static_cast<PlayerId>(params[1])) ? 1 : 0;
}

// SA_GetPlayerAudioStreamSource(playerid, &E_SA_AUDIO_STREAM_SOURCE:source_type, &source_id)
cell AMX_NATIVE_CALL n_SA_GetPlayerAudioStreamSource(AMX* amx, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 3) return 0;
    AudioStreamSourceType src_type = AudioStreamSourceType::None;
    std::int32_t src_id = -1;
    bool ok = g_engine->GetPlayerAudioStreamSource(static_cast<PlayerId>(params[1]), src_type, src_id);
    WriteAmxCell(amx, params[2], static_cast<cell>(src_type));
    WriteAmxCell(amx, params[3], static_cast<cell>(src_id));
    return ok ? 1 : 0;
}

// SA_SetPlayerAudioStreamVolume(playerid, Float:volume)
cell AMX_NATIVE_CALL n_SA_SetPlayerAudioStreamVolume(AMX*, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 2) return 0;
    return g_engine->SetPlayerAudioVolume(static_cast<PlayerId>(params[1]), amx_ctof(params[2])) ? 1 : 0;
}

// SA_SetAudioStreamVolume(streamid, Float:volume)
cell AMX_NATIVE_CALL n_SA_SetAudioStreamVolume(AMX*, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 2) return 0;
    return g_engine->SetStreamVolume(static_cast<StreamId>(params[1]), amx_ctof(params[2])) ? 1 : 0;
}

// SA_SetPlayerAudioStreamDistance(playerid, Float:distance)
cell AMX_NATIVE_CALL n_SA_SetPlayerAudioStreamDistance(AMX*, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 2) return 0;
    return g_engine->SetPlayerAudioStreamDistance(static_cast<PlayerId>(params[1]), amx_ctof(params[2])) ? 1 : 0;
}

// SA_SetAudioStreamDistance(streamid, Float:distance)
cell AMX_NATIVE_CALL n_SA_SetAudioStreamDistance(AMX*, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 2) return 0;
    return g_engine->SetAudioStreamDistance(static_cast<StreamId>(params[1]), amx_ctof(params[2])) ? 1 : 0;
}

// SA_SetPlayerAudioStreamPos(playerid, Float:posX, Float:posY, Float:posZ)
cell AMX_NATIVE_CALL n_SA_SetPlayerAudioStreamPos(AMX*, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 4) return 0;
    return g_engine->SetPlayerAudioStreamPos(
        static_cast<PlayerId>(params[1]),
        amx_ctof(params[2]),
        amx_ctof(params[3]),
        amx_ctof(params[4])) ? 1 : 0;
}

// SA_SetAudioStreamPos(streamid, Float:posX, Float:posY, Float:posZ)
cell AMX_NATIVE_CALL n_SA_SetAudioStreamPos(AMX*, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 4) return 0;
    return g_engine->SetAudioStreamPos(
        static_cast<StreamId>(params[1]),
        amx_ctof(params[2]),
        amx_ctof(params[3]),
        amx_ctof(params[4])) ? 1 : 0;
}

// SA_AttachAudioStreamToPlayer(streamid, target_playerid)
cell AMX_NATIVE_CALL n_SA_AttachAudioStreamToPlayer(AMX*, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 2) return 0;
    return g_engine->AttachStreamToPlayer(static_cast<StreamId>(params[1]), static_cast<PlayerId>(params[2])) ? 1 : 0;
}

// SA_AttachAudioStreamToVehicle(streamid, vehicleid)
cell AMX_NATIVE_CALL n_SA_AttachAudioStreamToVehicle(AMX*, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 2) return 0;
    return g_engine->AttachStreamToVehicle(static_cast<StreamId>(params[1]), static_cast<std::int32_t>(params[2])) ? 1 : 0;
}

// SA_SearchAudioStream(playerid, const query[], max_results = 5)
cell AMX_NATIVE_CALL n_SA_SearchAudioStream(AMX* amx, cell* params) {
    if (!g_engine) return INVALID_SEARCH_ID;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 2) return INVALID_SEARCH_ID;
    PlayerId pid = static_cast<PlayerId>(params[1]);
    std::string query = ConvertTextFromPawn(ReadAmxString(amx, params[2]), g_engine->GetPawnTextEncoding());
    std::uint32_t max_res = (argc >= 3) ? static_cast<std::uint32_t>(std::max<cell>(1, params[3])) : 5;
    return static_cast<cell>(g_engine->SearchAudio(pid, query, max_res));
}

// SA_CancelAudioStreamSearch(playerid)
cell AMX_NATIVE_CALL n_SA_CancelAudioStreamSearch(AMX*, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 1) return 0;
    return g_engine->CancelSearch(static_cast<PlayerId>(params[1])) ? 1 : 0;
}

// SA_CreateAudioStreamChannel(const name[], creator_playerid = INVALID_PLAYER_ID, const password[] = "", E_SA_CHANNEL_DESTROY_MODE:destroy_mode = SA_CHANNEL_DESTROY_MANUAL, destroy_timeout_sec = 0, bool:persistent = true)
cell AMX_NATIVE_CALL n_SA_CreateAudioStreamChannel(AMX* amx, cell* params) {
    if (!g_engine) return INVALID_CHANNEL_ID;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 1) return INVALID_CHANNEL_ID;

    std::string name = ConvertTextFromPawn(ReadAmxString(amx, params[1]), g_engine->GetPawnTextEncoding());
    PlayerId creator_pid = (argc >= 2) ? static_cast<PlayerId>(params[2]) : PAWN_INVALID_PLAYER_ID;
    std::string password = (argc >= 3) ? ReadAmxString(amx, params[3]) : std::string();
    std::int32_t mode_raw = (argc >= 4) ? static_cast<std::int32_t>(params[4]) : 0;
    if (mode_raw < 0 || mode_raw > 2) mode_raw = 0;
    std::int32_t timeout_sec = (argc >= 5) ? static_cast<std::int32_t>(params[5]) : 0;
    bool persistent = (argc >= 6) ? (params[6] != 0) : true;

    return static_cast<cell>(g_engine->CreateChannel(
        name,
        creator_pid,
        password,
        static_cast<ChannelDestroyMode>(mode_raw),
        timeout_sec,
        persistent));
}

// SA_DestroyAudioStreamChannel(channelid)
cell AMX_NATIVE_CALL n_SA_DestroyAudioStreamChannel(AMX*, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 1) return 0;
    return g_engine->DestroyChannel(static_cast<ChannelId>(params[1])) ? 1 : 0;
}

// SA_PlayChannelAudioStream(channelid, const url[])
cell AMX_NATIVE_CALL n_SA_PlayChannelAudioStream(AMX* amx, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 2) return 0;
    std::string raw_url = ReadAmxString(amx, params[2]);
    std::string url = ConvertTextFromPawn(raw_url, g_engine->GetPawnTextEncoding());
    return g_engine->PlayChannel(static_cast<ChannelId>(params[1]), url, raw_url) ? 1 : 0;
}

// SA_QueueChannelAudioStream(channelid, const url[], const label[] = "")
cell AMX_NATIVE_CALL n_SA_QueueChannelAudioStream(AMX* amx, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 2) return 0;
    std::string raw_url = ReadAmxString(amx, params[2]);
    std::string url = ConvertTextFromPawn(raw_url, g_engine->GetPawnTextEncoding());
    std::string raw_label;
    std::string utf8_label;
    if (argc >= 3) {
        raw_label = ReadAmxString(amx, params[3]);
        utf8_label = ConvertTextFromPawn(raw_label, g_engine->GetPawnTextEncoding());
    }
    return g_engine->QueueChannel(
        static_cast<ChannelId>(params[1]),
        url,
        raw_url,
        utf8_label) ? 1 : 0;
}

// SA_SkipChannelAudioStream(channelid)
cell AMX_NATIVE_CALL n_SA_SkipChannelAudioStream(AMX*, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 1) return 0;
    return g_engine->SkipChannel(static_cast<ChannelId>(params[1])) ? 1 : 0;
}

// SA_StopChannelAudioStream(channelid)
cell AMX_NATIVE_CALL n_SA_StopChannelAudioStream(AMX*, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 1) return 0;
    return g_engine->StopChannel(static_cast<ChannelId>(params[1])) ? 1 : 0;
}

// SA_PauseChannelAudioStream(channelid, bool:pause)
cell AMX_NATIVE_CALL n_SA_PauseChannelAudioStream(AMX*, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 2) return 0;
    return g_engine->PauseChannel(static_cast<ChannelId>(params[1]), params[2] != 0) ? 1 : 0;
}

// SA_SetChannelAudioStreamVolume(channelid, Float:volume)
cell AMX_NATIVE_CALL n_SA_SetChannelAudioStreamVolume(AMX*, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 2) return 0;
    return g_engine->SetChannelVolume(static_cast<ChannelId>(params[1]), amx_ctof(params[2])) ? 1 : 0;
}

// SA_JoinAudioStreamChannel(playerid, channelid, const password[] = "", bool:ignore_password = false)
cell AMX_NATIVE_CALL n_SA_JoinAudioStreamChannel(AMX* amx, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 2) return 0;
    std::string password = (argc >= 3) ? ReadAmxString(amx, params[3]) : std::string();
    const bool ignore_password = (argc >= 4) && params[4] != 0;
    return g_engine->JoinChannel(
        static_cast<PlayerId>(params[1]), static_cast<ChannelId>(params[2]), password, ignore_password) ? 1 : 0;
}

// SA_LeaveAudioStreamChannel(playerid)
cell AMX_NATIVE_CALL n_SA_LeaveAudioStreamChannel(AMX*, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 1) return 0;
    return g_engine->LeaveChannel(static_cast<PlayerId>(params[1])) ? 1 : 0;
}

// SA_IsPlayerInAudioStreamChannel(playerid, channelid)
cell AMX_NATIVE_CALL n_SA_IsPlayerInAudioStreamChannel(AMX*, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 2) return 0;
    return g_engine->IsPlayerInChannel(static_cast<PlayerId>(params[1]), static_cast<ChannelId>(params[2])) ? 1 : 0;
}

// SA_GetPlayerAudioStreamChannel(playerid)
cell AMX_NATIVE_CALL n_SA_GetPlayerAudioStreamChannel(AMX*, cell* params) {
    if (!g_engine) return INVALID_CHANNEL_ID;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 1) return INVALID_CHANNEL_ID;
    return static_cast<cell>(g_engine->GetPlayerChannelId(static_cast<PlayerId>(params[1])));
}

// SA_SetAudioStreamChannelPass(channelid, const password[])
cell AMX_NATIVE_CALL n_SA_SetAudioStreamChannelPass(AMX* amx, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 2) return 0;
    return g_engine->SetChannelPassword(static_cast<ChannelId>(params[1]), ReadAmxString(amx, params[2])) ? 1 : 0;
}

// SA_CheckAudioStreamChannelPass(channelid, const password[])
cell AMX_NATIVE_CALL n_SA_CheckAudioStreamChannelPass(AMX* amx, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 2) return 0;
    return g_engine->VerifyChannelPassword(static_cast<ChannelId>(params[1]), ReadAmxString(amx, params[2])) ? 1 : 0;
}

// SA_SetAudioStreamChannelCreator(channelid, playerid)
cell AMX_NATIVE_CALL n_SA_SetAudioStreamChannelCreator(AMX*, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 2) return 0;
    return g_engine->SetChannelCreator(static_cast<ChannelId>(params[1]), static_cast<PlayerId>(params[2])) ? 1 : 0;
}

// SA_SetAudioStreamChannelDestroy(channelid, E_SA_CHANNEL_DESTROY_MODE:mode, timeout_seconds = 0)
cell AMX_NATIVE_CALL n_SA_SetAudioStreamChannelDestroy(AMX*, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 2) return 0;
    std::int32_t mode_raw = static_cast<std::int32_t>(params[2]);
    if (mode_raw < 0 || mode_raw > 2) return 0;
    std::int32_t timeout_sec = (argc >= 3) ? static_cast<std::int32_t>(params[3]) : 0;
    if (timeout_sec < 0) return 0;
    return g_engine->SetChannelDestroyPolicy(
        static_cast<ChannelId>(params[1]),
        static_cast<ChannelDestroyMode>(mode_raw),
        timeout_sec) ? 1 : 0;
}

// SA_GetAudioStreamChannelDestroy(channelid, &E_SA_CHANNEL_DESTROY_MODE:mode, &timeout_sec, &remaining_sec, &bool:creator_online)
cell AMX_NATIVE_CALL n_SA_GetAudioStreamChannelDestroy(AMX* amx, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 1) return 0;
    ChannelInfo info;
    if (!g_engine->GetChannelInfo(static_cast<ChannelId>(params[1]), info)) return 0;
    if (argc >= 2) WriteAmxCell(amx, params[2], static_cast<cell>(info.destroy_mode));
    if (argc >= 3) WriteAmxCell(amx, params[3], static_cast<cell>(info.destroy_timeout_seconds));
    if (argc >= 4) WriteAmxCell(amx, params[4], static_cast<cell>(info.destroy_remaining_seconds));
    if (argc >= 5) WriteAmxCell(amx, params[5], info.creator_online ? 1 : 0);
    return 1;
}

// SA_SetAudioStreamChannelIdleSuspend(channelid, bool:enable, grace_seconds = 30)
cell AMX_NATIVE_CALL n_SA_SetAudioStreamChannelIdleSuspend(AMX*, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 2) return 0;
    const std::int32_t grace = (argc >= 3) ? static_cast<std::int32_t>(params[3]) : 30;
    return g_engine->SetChannelIdleSuspend(static_cast<ChannelId>(params[1]), params[2] != 0, grace) ? 1 : 0;
}

// SA_GetAudioStreamChannelIdleSuspend(channelid, &bool:enable, &grace_seconds, &bool:suspended)
cell AMX_NATIVE_CALL n_SA_GetAudioStreamChannelIdleSuspend(AMX* amx, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 1) return 0;
    bool enable = false;
    bool suspended = false;
    std::int32_t grace = 0;
    if (!g_engine->GetChannelIdleSuspend(static_cast<ChannelId>(params[1]), enable, grace, suspended)) return 0;
    if (argc >= 2) WriteAmxCell(amx, params[2], enable ? 1 : 0);
    if (argc >= 3) WriteAmxCell(amx, params[3], static_cast<cell>(grace));
    if (argc >= 4) WriteAmxCell(amx, params[4], suspended ? 1 : 0);
    return 1;
}

// SA_GetAudioStreamChannelCount()
cell AMX_NATIVE_CALL n_SA_GetAudioStreamChannelCount(AMX*, cell*) {
    return g_engine ? static_cast<cell>(g_engine->GetChannelCount()) : 0;
}

// SA_GetAudioStreamChannelID(index)
cell AMX_NATIVE_CALL n_SA_GetAudioStreamChannelID(AMX*, cell* params) {
    if (!g_engine) return INVALID_CHANNEL_ID;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 1 || params[1] < 0) return INVALID_CHANNEL_ID;
    return static_cast<cell>(g_engine->GetChannelIdAt(static_cast<std::size_t>(params[1])));
}

// SA_GetAudioStreamChannelInfo(channelid, name[], name_len, creator_name[], creator_len, &listeners, &state, &bool:has_password, &queue_count)
cell AMX_NATIVE_CALL n_SA_GetAudioStreamChannelInfo(AMX* amx, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 1) return 0;
    ChannelInfo info;
    if (!g_engine->GetChannelInfo(static_cast<ChannelId>(params[1]), info)) return 0;
    if (argc >= 3) WriteAmxString(amx, params[2], info.name, static_cast<int>(params[3]));
    if (argc >= 5) WriteAmxString(amx, params[4], info.creator_name, static_cast<int>(params[5]));
    if (argc >= 6) WriteAmxCell(amx, params[6], static_cast<cell>(info.listener_count));
    if (argc >= 7) WriteAmxCell(amx, params[7], static_cast<cell>(info.state));
    if (argc >= 8) WriteAmxCell(amx, params[8], info.has_password ? 1 : 0);
    if (argc >= 9) WriteAmxCell(amx, params[9], static_cast<cell>(info.queue_count));
    return 1;
}

// SA_GetAudioStreamChannelTrack(channelid, title[], title_len, artist[], artist_len, &duration)
cell AMX_NATIVE_CALL n_SA_GetAudioStreamChannelTrack(AMX* amx, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 1) return 0;
    ChannelInfo info;
    if (!g_engine->GetChannelInfo(static_cast<ChannelId>(params[1]), info)) return 0;
    if (argc >= 3) WriteAmxString(amx, params[2], info.current_track_title, static_cast<int>(params[3]));
    if (argc >= 5) WriteAmxString(amx, params[4], info.current_track_artist, static_cast<int>(params[5]));
    if (argc >= 6) WriteAmxCell(amx, params[6], static_cast<cell>(info.current_track_duration));
    return 1;
}

// SA_GetAudioStreamChannelQueue(channelid, index, item[], item_len)
cell AMX_NATIVE_CALL n_SA_GetAudioStreamChannelQueue(AMX* amx, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 4 || params[2] < 0) return 0;
    std::string item;
    if (!g_engine->GetChannelQueueItem(static_cast<ChannelId>(params[1]), static_cast<std::size_t>(params[2]), item)) {
        return 0;
    }
    WriteAmxString(amx, params[3], item, static_cast<int>(params[4]));
    return 1;
}

// SA_GetAudioStreamChannelQueueLabel(channelid, index, label[], label_len, bool:original = false)
cell AMX_NATIVE_CALL n_SA_GetAudioStreamChannelQueueLabel(AMX* amx, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 4 || params[2] < 0) return 0;
    std::string label;
    const bool original = (argc >= 5) && params[5] != 0;
    if (!g_engine->GetChannelQueueItemLabel(static_cast<ChannelId>(params[1]), static_cast<std::size_t>(params[2]), label, original)) {
        return 0;
    }
    WriteAmxString(amx, params[3], label, static_cast<int>(params[4]));
    return 1;
}

// SA_SetAudioStreamChannelQueueLabel(channelid, index, const label[])
cell AMX_NATIVE_CALL n_SA_SetAudioStreamChannelQueueLabel(AMX* amx, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 3 || params[2] < 0) return 0;
    std::string utf8 = ConvertTextFromPawn(ReadAmxString(amx, params[3]), g_engine->GetPawnTextEncoding());
    return g_engine->SetChannelQueueItemLabel(
        static_cast<ChannelId>(params[1]), static_cast<std::size_t>(params[2]), utf8) ? 1 : 0;
}

// SA_SetAudioStreamChannelDescription(channelid, const description[])
cell AMX_NATIVE_CALL n_SA_SetAudioStreamChannelDescription(AMX* amx, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 2) return 0;
    std::string utf8 = ConvertTextFromPawn(ReadAmxString(amx, params[2]), g_engine->GetPawnTextEncoding());
    return g_engine->SetChannelDescription(static_cast<ChannelId>(params[1]), utf8) ? 1 : 0;
}

// SA_GetAudioStreamChannelDescription(channelid, description[], description_len)
cell AMX_NATIVE_CALL n_SA_GetAudioStreamChannelDescription(AMX* amx, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 3) return 0;
    std::string description;
    if (!g_engine->GetChannelDescription(static_cast<ChannelId>(params[1]), description)) return 0;
    WriteAmxString(amx, params[2], description, static_cast<int>(params[3]));
    return 1;
}

// SA_SetAudioStreamChannelQueueDescription(channelid, index, const description[])
cell AMX_NATIVE_CALL n_SA_SetAudioStreamChannelQueueDescription(AMX* amx, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 3 || params[2] < 0) return 0;
    std::string utf8 = ConvertTextFromPawn(ReadAmxString(amx, params[3]), g_engine->GetPawnTextEncoding());
    return g_engine->SetChannelQueueItemDescription(
        static_cast<ChannelId>(params[1]), static_cast<std::size_t>(params[2]), utf8) ? 1 : 0;
}

// SA_GetAudioStreamChannelQueueDescription(channelid, index, description[], description_len)
cell AMX_NATIVE_CALL n_SA_GetAudioStreamChannelQueueDescription(AMX* amx, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 4 || params[2] < 0) return 0;
    std::string description;
    if (!g_engine->GetChannelQueueItemDescription(
            static_cast<ChannelId>(params[1]), static_cast<std::size_t>(params[2]), description)) {
        return 0;
    }
    WriteAmxString(amx, params[3], description, static_cast<int>(params[4]));
    return 1;
}

// SA_GetAudioStreamChannelTrackDescription(channelid, description[], description_len)
cell AMX_NATIVE_CALL n_SA_GetAudioStreamChannelTrackDescription(AMX* amx, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 3) return 0;
    std::string description;
    if (!g_engine->GetChannelTrackDescription(static_cast<ChannelId>(params[1]), description)) return 0;
    WriteAmxString(amx, params[2], description, static_cast<int>(params[3]));
    return 1;
}

// SA_FindAudioStreamChannelQueue(channelid, const item[], start_index = 0)
cell AMX_NATIVE_CALL n_SA_FindAudioStreamChannelQueue(AMX* amx, cell* params) {
    if (!g_engine) return -1;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 2) return -1;
    const std::string raw_item = ReadAmxString(amx, params[2]);
    const std::int32_t start_index = (argc >= 3) ? static_cast<std::int32_t>(params[3]) : 0;
    return static_cast<cell>(g_engine->FindChannelQueueItem(static_cast<ChannelId>(params[1]), raw_item, start_index));
}

// SA_ClearAudioStreamChannelQueue(channelid)
cell AMX_NATIVE_CALL n_SA_ClearAudioStreamChannelQueue(AMX*, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 1) return 0;
    return g_engine->ClearChannelQueue(static_cast<ChannelId>(params[1])) ? 1 : 0;
}

// SA_SetAudioStreamChannelPos(channelid, Float:x, Float:y, Float:z, Float:distance = 50.0, worldid = -1, interiorid = -1, bool:auto_listen = true)
cell AMX_NATIVE_CALL n_SA_SetAudioStreamChannelPos(AMX*, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 4) return 0;
    float x = amx_ctof(params[2]);
    float y = amx_ctof(params[3]);
    float z = amx_ctof(params[4]);
    float dist = (argc >= 5) ? amx_ctof(params[5]) : 50.0f;
    std::int32_t world_id = (argc >= 6) ? static_cast<std::int32_t>(params[6]) : -1;
    std::int32_t interior_id = (argc >= 7) ? static_cast<std::int32_t>(params[7]) : -1;
    bool auto_listen = (argc >= 8) ? (params[8] != 0) : true;
    return g_engine->SetChannelPos(
        static_cast<ChannelId>(params[1]), x, y, z, dist, world_id, interior_id, auto_listen) ? 1 : 0;
}

// SA_ClearAudioStreamChannelPos(channelid)
cell AMX_NATIVE_CALL n_SA_ClearAudioStreamChannelPos(AMX*, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 1) return 0;
    return g_engine->ClearChannelPos(static_cast<ChannelId>(params[1])) ? 1 : 0;
}

// SA_SetAudioStreamChannelGroup(channelid, group)
cell AMX_NATIVE_CALL n_SA_SetAudioStreamChannelGroup(AMX*, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 2) return 0;
    return g_engine->SetChannelRangeGroup(
        static_cast<ChannelId>(params[1]),
        static_cast<std::int32_t>(params[2])) ? 1 : 0;
}

// SA_GetAudioStreamChannelGroup(channelid)
cell AMX_NATIVE_CALL n_SA_GetAudioStreamChannelGroup(AMX*, cell* params) {
    if (!g_engine) return -1;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 1) return -1;
    return static_cast<cell>(g_engine->GetChannelRangeGroup(static_cast<ChannelId>(params[1])));
}

// SA_AttachAudioStreamChannelToPlayer(channelid, target_playerid, Float:distance = 0.0)
cell AMX_NATIVE_CALL n_SA_AttachAudioStreamChannelToPlayer(AMX*, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 2) return 0;
    const float distance = (argc >= 3) ? amx_ctof(params[3]) : 0.0f;
    return g_engine->AttachChannelToPlayer(
        static_cast<ChannelId>(params[1]),
        static_cast<PlayerId>(params[2]),
        distance) ? 1 : 0;
}

// SA_AttachAudioStreamChannelToVehicle(channelid, vehicleid, Float:distance = 0.0)
cell AMX_NATIVE_CALL n_SA_AttachAudioStreamChannelToVehicle(AMX*, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 2) return 0;
    const float distance = (argc >= 3) ? amx_ctof(params[3]) : 0.0f;
    return g_engine->AttachChannelToVehicle(
        static_cast<ChannelId>(params[1]),
        static_cast<std::int32_t>(params[2]),
        distance) ? 1 : 0;
}

// SA_RemoveAudioStreamChannelQueue(channelid, index)
cell AMX_NATIVE_CALL n_SA_RemoveAudioStreamChannelQueue(AMX*, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 2 || params[2] < 0) return 0;
    return g_engine->RemoveChannelQueueItem(
        static_cast<ChannelId>(params[1]),
        static_cast<std::size_t>(params[2])) ? 1 : 0;
}

// SA_InsertAudioStreamChannelQueue(channelid, index, const url[], const label[] = "")
cell AMX_NATIVE_CALL n_SA_InsertAudioStreamChannelQueue(AMX* amx, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 3 || params[2] < 0) return 0;
    std::string raw_url = ReadAmxString(amx, params[3]);
    std::string url = ConvertTextFromPawn(raw_url, g_engine->GetPawnTextEncoding());
    std::string raw_label;
    std::string utf8_label;
    if (argc >= 4) {
        raw_label = ReadAmxString(amx, params[4]);
        utf8_label = ConvertTextFromPawn(raw_label, g_engine->GetPawnTextEncoding());
    }
    return g_engine->InsertChannelQueueItem(
        static_cast<ChannelId>(params[1]),
        static_cast<std::size_t>(params[2]),
        url,
        raw_url,
        utf8_label) ? 1 : 0;
}

// SA_ReplaceAudioStreamChannelQueue(channelid, index, const url[], const label[] = "", bool:keep_description = false)
cell AMX_NATIVE_CALL n_SA_ReplaceAudioStreamChannelQueue(AMX* amx, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 3 || params[2] < 0) return 0;
    std::string raw_url = ReadAmxString(amx, params[3]);
    std::string url = ConvertTextFromPawn(raw_url, g_engine->GetPawnTextEncoding());
    std::string utf8_label;
    if (argc >= 4) {
        utf8_label = ConvertTextFromPawn(ReadAmxString(amx, params[4]), g_engine->GetPawnTextEncoding());
    }
    const bool keep_description = (argc >= 5) && params[5] != 0;
    return g_engine->ReplaceChannelQueueItem(
        static_cast<ChannelId>(params[1]),
        static_cast<std::size_t>(params[2]),
        url,
        raw_url,
        utf8_label,
        keep_description) ? 1 : 0;
}

// SA_MoveAudioStreamChannelQueue(channelid, from_index, to_index)
cell AMX_NATIVE_CALL n_SA_MoveAudioStreamChannelQueue(AMX*, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 3 || params[2] < 0 || params[3] < 0) return 0;
    return g_engine->MoveChannelQueueItem(
        static_cast<ChannelId>(params[1]),
        static_cast<std::size_t>(params[2]),
        static_cast<std::size_t>(params[3])) ? 1 : 0;
}

// SA_ShuffleAudioStreamChannelQueue(channelid)
cell AMX_NATIVE_CALL n_SA_ShuffleAudioStreamChannelQueue(AMX*, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 1) return 0;
    return g_engine->ShuffleChannelQueue(static_cast<ChannelId>(params[1])) ? 1 : 0;
}

// SA_QueueChannelAudioStreamPlaylist(channelid, const url[], max_items = 25)
cell AMX_NATIVE_CALL n_SA_QueueChannelAudioStreamPlaylist(AMX* amx, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 2) return 0;
    std::string raw_url = ReadAmxString(amx, params[2]);
    std::string url = ConvertTextFromPawn(raw_url, g_engine->GetPawnTextEncoding());
    std::int32_t max_items = (argc >= 3) ? static_cast<std::int32_t>(params[3]) : 25;
    return g_engine->QueueChannelPlaylist(
        static_cast<ChannelId>(params[1]),
        url,
        max_items) ? 1 : 0;
}

// SA_SeekChannelAudioStream(channelid, seconds)
cell AMX_NATIVE_CALL n_SA_SeekChannelAudioStream(AMX*, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 2) return 0;
    return g_engine->SeekChannelAudioStream(
        static_cast<ChannelId>(params[1]),
        static_cast<std::int32_t>(params[2])) ? 1 : 0;
}

// SA_SetAudioStreamChannelLoop(channelid, E_SA_CHANNEL_LOOP_MODE:mode)
cell AMX_NATIVE_CALL n_SA_SetAudioStreamChannelLoop(AMX*, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 2) return 0;
    std::int32_t mode_raw = static_cast<std::int32_t>(params[2]);
    if (mode_raw < 0 || mode_raw > 2) return 0;
    return g_engine->SetChannelLoopMode(
        static_cast<ChannelId>(params[1]),
        static_cast<ChannelLoopMode>(mode_raw)) ? 1 : 0;
}

// SA_GetAudioStreamChannelLoop(channelid)
cell AMX_NATIVE_CALL n_SA_GetAudioStreamChannelLoop(AMX*, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 1) return 0;
    return static_cast<cell>(g_engine->GetChannelLoopMode(static_cast<ChannelId>(params[1])));
}

// SA_GetAudioStreamChannelElapsed(channelid)
cell AMX_NATIVE_CALL n_SA_GetAudioStreamChannelElapsed(AMX*, cell* params) {
    if (!g_engine) return -1;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 1) return -1;
    return static_cast<cell>(g_engine->GetChannelElapsedSeconds(static_cast<ChannelId>(params[1])));
}

// SA_GetAudioStreamChannelListener(channelid, index)
cell AMX_NATIVE_CALL n_SA_GetAudioStreamChannelListener(AMX*, cell* params) {
    if (!g_engine) return PAWN_INVALID_PLAYER_ID;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 2 || params[2] < 0) return PAWN_INVALID_PLAYER_ID;
    PlayerId pid = g_engine->GetChannelListenerAt(
        static_cast<ChannelId>(params[1]),
        static_cast<std::size_t>(params[2]));
    return static_cast<cell>(ToPawnPlayerId(pid));
}

// SA_SetAudioStreamChannelName(channelid, const name[])
cell AMX_NATIVE_CALL n_SA_SetAudioStreamChannelName(AMX* amx, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 2) return 0;
    std::string utf8_name = ConvertTextFromPawn(ReadAmxString(amx, params[2]), g_engine->GetPawnTextEncoding());
    return g_engine->SetChannelName(static_cast<ChannelId>(params[1]), utf8_name) ? 1 : 0;
}

// SA_GetAudioStreamChannelLiveTitle(channelid, title[], title_len = sizeof(title))
cell AMX_NATIVE_CALL n_SA_GetAudioStreamChannelLiveTitle(AMX* amx, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 3) return 0;
    std::string title;
    if (!g_engine->GetChannelLiveTitle(static_cast<ChannelId>(params[1]), title)) {
        return 0;
    }
    WriteAmxString(amx, params[2], title, static_cast<int>(params[3]));
    return 1;
}

// SA_SetAudioStreamChannelLiveTitle(channelid, const title[])
cell AMX_NATIVE_CALL n_SA_SetAudioStreamChannelLiveTitle(AMX* amx, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 2) return 0;
    std::string utf8_title = ConvertTextFromPawn(ReadAmxString(amx, params[2]), g_engine->GetPawnTextEncoding());
    return g_engine->SetChannelLiveTitle(static_cast<ChannelId>(params[1]), utf8_title) ? 1 : 0;
}

// SA_SetAudioStreamChannelPlaylist(channelid, bool:enable)
cell AMX_NATIVE_CALL n_SA_SetAudioStreamChannelPlaylist(AMX*, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 2) return 0;
    return g_engine->SetChannelPlaylistMode(
        static_cast<ChannelId>(params[1]),
        params[2] != 0) ? 1 : 0;
}

// SA_IsAudioStreamChannelPlaylist(channelid)
cell AMX_NATIVE_CALL n_SA_IsAudioStreamChannelPlaylist(AMX*, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 1) return 0;
    return g_engine->IsChannelPlaylistMode(static_cast<ChannelId>(params[1])) ? 1 : 0;
}

// SA_GetAudioStreamChannelQueuePos(channelid)
cell AMX_NATIVE_CALL n_SA_GetAudioStreamChannelQueuePos(AMX*, cell* params) {
    if (!g_engine) return -1;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 1) return -1;
    return static_cast<cell>(g_engine->GetChannelQueuePos(static_cast<ChannelId>(params[1])));
}

// SA_PlayAudioStreamChannelQueue(channelid, index)
cell AMX_NATIVE_CALL n_SA_PlayAudioStreamChannelQueue(AMX*, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 2 || params[2] < 0) return 0;
    return g_engine->PlayChannelQueueItem(
        static_cast<ChannelId>(params[1]),
        static_cast<std::size_t>(params[2])) ? 1 : 0;
}

// SA_RefreshAudioLibrary()
cell AMX_NATIVE_CALL n_SA_RefreshAudioLibrary(AMX*, cell*) {
    return g_engine ? static_cast<cell>(g_engine->RefreshAudioLibrary()) : 0;
}

// SA_GetAudioLibraryFolderCount()
cell AMX_NATIVE_CALL n_SA_GetAudioLibraryFolderCount(AMX*, cell*) {
    return g_engine ? static_cast<cell>(g_engine->GetAudioLibraryFolderCount()) : 0;
}

// SA_GetAudioLibraryFolder(index, name[], name_len = sizeof(name), &track_count)
cell AMX_NATIVE_CALL n_SA_GetAudioLibraryFolder(AMX* amx, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 4 || params[1] < 0) return 0;
    std::string name;
    std::int32_t track_count = 0;
    if (!g_engine->GetAudioLibraryFolder(static_cast<std::size_t>(params[1]), name, track_count)) {
        return 0;
    }
    WriteAmxString(amx, params[2], name, static_cast<int>(params[3]));
    WriteAmxCell(amx, params[4], static_cast<cell>(track_count));
    return 1;
}

// SA_GetAudioLibraryTrack(folder_index, track_index, name[], name_len = sizeof(name))
cell AMX_NATIVE_CALL n_SA_GetAudioLibraryTrack(AMX* amx, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 4 || params[1] < 0 || params[2] < 0) return 0;
    std::string name;
    if (!g_engine->GetAudioLibraryTrack(
            static_cast<std::size_t>(params[1]),
            static_cast<std::size_t>(params[2]),
            name)) {
        return 0;
    }
    WriteAmxString(amx, params[3], name, static_cast<int>(params[4]));
    return 1;
}

// SA_GetAudioStreamLiveTitle(streamid, title[], title_len = sizeof(title))
cell AMX_NATIVE_CALL n_SA_GetAudioStreamLiveTitle(AMX* amx, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 3) return 0;
    std::string title;
    if (!g_engine->GetStreamLiveTitle(static_cast<StreamId>(params[1]), title)) {
        return 0;
    }
    WriteAmxString(amx, params[2], title, static_cast<int>(params[3]));
    return 1;
}

// SA_SetAudioStreamLiveTitle(streamid, const title[])
cell AMX_NATIVE_CALL n_SA_SetAudioStreamLiveTitle(AMX* amx, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 2) return 0;
    std::string utf8_title = ConvertTextFromPawn(ReadAmxString(amx, params[2]), g_engine->GetPawnTextEncoding());
    return g_engine->SetStreamLiveTitle(static_cast<StreamId>(params[1]), utf8_title) ? 1 : 0;
}

// SA_GetPlayerAudioStreamID(playerid)
cell AMX_NATIVE_CALL n_SA_GetPlayerAudioStreamID(AMX*, cell* params) {
    if (!g_engine) return INVALID_STREAM_ID;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 1) return INVALID_STREAM_ID;
    return static_cast<cell>(g_engine->GetPlayerStreamId(static_cast<PlayerId>(params[1])));
}

// SA_GetAudioStreamInfo(streamid, title[], title_len = sizeof(title), artist[], artist_len = sizeof(artist), &duration = 0, &state = 0)
cell AMX_NATIVE_CALL n_SA_GetAudioStreamInfo(AMX* amx, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 1) return 0;

    TrackMetadata meta;
    StreamState state = StreamState::Idle;
    if (!g_engine->GetStreamInfo(static_cast<StreamId>(params[1]), meta, state)) {
        return 0;
    }

    if (argc >= 3) WriteAmxString(amx, params[2], meta.title, static_cast<int>(params[3]));
    if (argc >= 5) WriteAmxString(amx, params[4], meta.artist, static_cast<int>(params[5]));
    if (argc >= 6) WriteAmxCell(amx, params[6], static_cast<cell>(meta.duration_seconds));
    if (argc >= 7) WriteAmxCell(amx, params[7], static_cast<cell>(state));
    return 1;
}

// SA_GetAudioStreamCount()
cell AMX_NATIVE_CALL n_SA_GetAudioStreamCount(AMX*, cell*) {
    return g_engine ? static_cast<cell>(g_engine->GetStreamCount()) : 0;
}

// SA_GetAudioStreamID(index)
cell AMX_NATIVE_CALL n_SA_GetAudioStreamID(AMX*, cell* params) {
    if (!g_engine) return INVALID_STREAM_ID;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 1 || params[1] < 0) return INVALID_STREAM_ID;
    return static_cast<cell>(g_engine->GetStreamIdAt(static_cast<std::size_t>(params[1])));
}

// SA_GetAudioStreamOwner(streamid)
cell AMX_NATIVE_CALL n_SA_GetAudioStreamOwner(AMX*, cell* params) {
    if (!g_engine) return PAWN_INVALID_PLAYER_ID;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 1) return PAWN_INVALID_PLAYER_ID;
    return static_cast<cell>(ToPawnPlayerId(g_engine->GetStreamOwner(static_cast<StreamId>(params[1]))));
}

// SA_GetAudioStreamElapsed(streamid)
cell AMX_NATIVE_CALL n_SA_GetAudioStreamElapsed(AMX*, cell* params) {
    if (!g_engine) return -1;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 1) return -1;
    return static_cast<cell>(g_engine->GetStreamElapsedSeconds(static_cast<StreamId>(params[1])));
}

// SA_GetAudioStreamURL(streamid, url[], url_len = sizeof(url))
cell AMX_NATIVE_CALL n_SA_GetAudioStreamURL(AMX* amx, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 3) return 0;
    std::string url;
    if (!g_engine->GetStreamUrl(static_cast<StreamId>(params[1]), url)) {
        return 0;
    }
    WriteAmxString(amx, params[2], url, static_cast<int>(params[3]));
    return 1;
}

// SA_GetAudioStreamStats(&active_streams, &active_channels, &active_transcoders, &active_listeners)
cell AMX_NATIVE_CALL n_SA_GetAudioStreamStats(AMX* amx, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 4) return 0;
    AudioEngineStats stats = g_engine->GetStats();
    WriteAmxCell(amx, params[1], static_cast<cell>(stats.active_streams));
    WriteAmxCell(amx, params[2], static_cast<cell>(stats.active_channels));
    WriteAmxCell(amx, params[3], static_cast<cell>(stats.active_transcoders));
    WriteAmxCell(amx, params[4], static_cast<cell>(stats.active_listeners));
    return 1;
}

// SA_IsAudioStreamExtractorReady()
cell AMX_NATIVE_CALL n_SA_IsAudioStreamExtractorReady(AMX*, cell*) {
    return (g_engine && g_engine->IsExtractorReady()) ? 1 : 0;
}

// SA_GetAudioStreamHttpPort()
cell AMX_NATIVE_CALL n_SA_GetAudioStreamHttpPort(AMX*, cell*) {
    return g_engine ? static_cast<cell>(g_engine->GetHttpPort()) : 0;
}

// SA_Internal_PlayerConnect(playerid) - Internal runtime hook bridge
cell AMX_NATIVE_CALL n_SA_Internal_PlayerConnect(AMX*, cell* params) {
    if (!g_engine || params[0] < static_cast<cell>(sizeof(cell))) return 0;
    if (!g_engine->UsesComponentLifecycleEvents()) {
        PlayerId player_id = static_cast<PlayerId>(params[1]);
        g_engine->OnPlayerConnect(player_id);
    }
    return 1;
}

// SA_Internal_PlayerDisconnect(playerid) - Internal runtime hook bridge
cell AMX_NATIVE_CALL n_SA_Internal_PlayerDisconnect(AMX*, cell* params) {
    if (!g_engine || params[0] < static_cast<cell>(sizeof(cell))) return 0;
    if (!g_engine->UsesComponentLifecycleEvents()) {
        PlayerId player_id = static_cast<PlayerId>(params[1]);
        g_engine->NotifyDisconnectHookActive();
        g_engine->OnPlayerDisconnect(player_id);
    }
    return 1;
}

// SA_Internal_GameModeExit() - Internal runtime lifecycle bridge
cell AMX_NATIVE_CALL n_SA_Internal_GameModeExit(AMX*, cell*) {
    if (g_engine) {
        g_engine->NotifyLifecycleStopping();
    }
    return 1;
}

} // namespace

void SetActiveAudioEngine(AudioEngine* engine) {
    g_engine = engine;
}

AudioEngine* GetActiveAudioEngine() {
    return g_engine;
}

void SetOriginalAudioNatives(AMX_NATIVE play_orig, AMX_NATIVE stop_orig) {
    g_orig_play_audio = play_orig;
    g_orig_stop_audio = stop_orig;
}

AMX_NATIVE GetOriginalPlayAudioNative() {
    return g_orig_play_audio;
}

AMX_NATIVE GetOriginalStopAudioNative() {
    return g_orig_stop_audio;
}

void SetOriginalSetPlayerNameNative(AMX_NATIVE set_name_orig) {
    g_orig_set_player_name = set_name_orig;
}

AMX_NATIVE GetOriginalSetPlayerNameNative() {
    return g_orig_set_player_name;
}

namespace {

bool StartsWithHttpPlain(const std::string& url) {
    if (url.size() < 7) return false;
    auto eq = [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == b;
    };
    return eq(url[0], 'h') && eq(url[1], 't') && eq(url[2], 't') &&
           eq(url[3], 'p') && url[4] == ':' && url[5] == '/' && url[6] == '/';
}

bool StartsWithHttpOrHttps(const std::string& url) {
    if (StartsWithHttpPlain(url)) return true;
    if (url.size() < 8) return false;
    auto eq = [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == b;
    };
    return eq(url[0], 'h') && eq(url[1], 't') && eq(url[2], 't') &&
           eq(url[3], 'p') && eq(url[4], 's') && url[5] == ':' &&
           url[6] == '/' && url[7] == '/';
}

} // namespace

cell AMX_NATIVE_CALL Hooked_PlayAudioStreamForPlayer(AMX* amx, cell* params) {
    if (!g_engine) {
        return g_orig_play_audio ? g_orig_play_audio(amx, params) : 0;
    }

    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 2) return 0;

    PlayerId player_id = static_cast<PlayerId>(params[1]);
    std::string raw_url = ReadAmxString(amx, params[2]);
    std::string url = ConvertTextFromPawn(raw_url, g_engine->GetPawnTextEncoding());
    StreamOptions opts = ParseStreamOptions(params, 2, argc);

    // Fast path: HTTP or HTTPS direct audio URL with known native extension and default world/interior/volume
    // Unless process_direct_audio_files is true for a direct music file (.mp3/.ogg/.wav/.flac/.aac/.m4a/.opus)
    const bool intercept_direct_file =
        opts.process_direct_audio_files && StreamResolver::IsDirectAudioFileUrl(url);
    bool direct_passthrough = !intercept_direct_file &&
                              StartsWithHttpOrHttps(url) &&
                              StreamResolver::IsKnownDirectAudioExtension(url) &&
                              !StreamResolver::IsKnownPlatformUrl(url) &&
                              !StreamResolver::IsHlsPlaylistUrl(url);
    if (direct_passthrough &&
        opts.world_id == -1 &&
        opts.interior_id == -1 &&
        std::abs(opts.volume - 100.0f) < 0.5f &&
        g_orig_play_audio) {
        if (g_engine->DeferNativePassthroughForSound(player_id, url, opts)) {
            return 1;
        }
        g_engine->StopAudioStreamForPlayer(player_id, false);
        cell ret = g_orig_play_audio(amx, params);
        if (ret != 0) {
            g_engine->RegisterNativePassthroughSession(player_id, url, opts);
        }
        return ret;
    }

    StreamId sid = g_engine->PlayAudioStreamForPlayer(player_id, url, opts, raw_url);
    if (sid == INVALID_STREAM_ID && !intercept_direct_file && !StreamResolver::IsHlsPlaylistUrl(url) &&
        !StreamResolver::IsKnownPlatformUrl(url) &&
        (StartsWithHttpPlain(url) || (StartsWithHttpOrHttps(url) && StreamResolver::IsKnownDirectAudioExtension(url))) &&
        g_orig_play_audio) {
        if (g_engine->DeferNativePassthroughForSound(player_id, url, opts)) {
            return 1;
        }
        g_engine->StopAudioStreamForPlayer(player_id, false);
        cell ret = g_orig_play_audio(amx, params);
        if (ret != 0) {
            g_engine->RegisterNativePassthroughSession(player_id, url, opts);
        }
        return ret;
    }
    return (sid != INVALID_STREAM_ID) ? 1 : 0;
}

cell AMX_NATIVE_CALL Hooked_StopAudioStreamForPlayer(AMX* amx, cell* params) {
    if (!g_engine) {
        return g_orig_stop_audio ? g_orig_stop_audio(amx, params) : 0;
    }
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 1) return 0;

    PlayerId player_id = static_cast<PlayerId>(params[1]);
    g_engine->StopAudioStreamForPlayer(player_id);
    g_engine->LeaveChannel(player_id);
    return 1;
}

cell AMX_NATIVE_CALL Hooked_SetPlayerName(AMX* amx, cell* params) {
    if (!g_orig_set_player_name) {
        return 0;
    }
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 2) {
        return g_orig_set_player_name(amx, params);
    }

    PlayerId player_id = static_cast<PlayerId>(params[1]);
    std::string new_name = ReadAmxString(amx, params[2]);
    return ExecuteSetPlayerNameHook(
        g_engine,
        g_orig_set_player_name,
        amx,
        params,
        player_id,
        new_name);
}

const std::vector<AMX_NATIVE_INFO>& GetPluginNatives() {
    static const std::vector<AMX_NATIVE_INFO> natives = {
        {"SA_PlayAudioStreamForPlayer", n_SA_PlayAudioStreamForPlayer},
        {"SA_PlayAudioStream", n_SA_PlayAudioStream},
        {"SA_PlayAudioStreamSoundForPlayer", n_SA_PlayAudioStreamSoundForPlayer},
        {"SA_StopAudioStreamForPlayer", n_SA_StopAudioStreamForPlayer},
        {"SA_StopAudioStream", n_SA_StopAudioStream},
        {"SA_PauseAudioStreamForPlayer", n_SA_PauseAudioStreamForPlayer},
        {"SA_PauseAudioStream", n_SA_PauseAudioStream},
        {"SA_SeekAudioStream", n_SA_SeekAudioStream},
        {"SA_SetPlayerAudioStreamMuted", n_SA_SetPlayerAudioStreamMuted},
        {"SA_IsPlayerAudioStreamMuted", n_SA_IsPlayerAudioStreamMuted},
        {"SA_SetPlayerAudioStreamGroupMuted", n_SA_SetPlayerAudioStreamGroupMuted},
        {"SA_IsPlayerAudioStreamGroupMuted", n_SA_IsPlayerAudioStreamGroupMuted},
        {"SA_GetPlayerAudioStreamSource", n_SA_GetPlayerAudioStreamSource},
        {"SA_SetPlayerAudioStreamVolume", n_SA_SetPlayerAudioStreamVolume},
        {"SA_SetAudioStreamVolume", n_SA_SetAudioStreamVolume},
        {"SA_SetPlayerAudioStreamDistance", n_SA_SetPlayerAudioStreamDistance},
        {"SA_SetAudioStreamDistance", n_SA_SetAudioStreamDistance},
        {"SA_SetPlayerAudioStreamPos", n_SA_SetPlayerAudioStreamPos},
        {"SA_SetAudioStreamPos", n_SA_SetAudioStreamPos},
        {"SA_AttachAudioStreamToPlayer", n_SA_AttachAudioStreamToPlayer},
        {"SA_AttachAudioStreamToVehicle", n_SA_AttachAudioStreamToVehicle},
        {"SA_SearchAudioStream", n_SA_SearchAudioStream},
        {"SA_CancelAudioStreamSearch", n_SA_CancelAudioStreamSearch},
        {"SA_CreateAudioStreamChannel", n_SA_CreateAudioStreamChannel},
        {"SA_DestroyAudioStreamChannel", n_SA_DestroyAudioStreamChannel},
        {"SA_PlayChannelAudioStream", n_SA_PlayChannelAudioStream},
        {"SA_QueueChannelAudioStream", n_SA_QueueChannelAudioStream},
        {"SA_SkipChannelAudioStream", n_SA_SkipChannelAudioStream},
        {"SA_StopChannelAudioStream", n_SA_StopChannelAudioStream},
        {"SA_PauseChannelAudioStream", n_SA_PauseChannelAudioStream},
        {"SA_SetChannelAudioStreamVolume", n_SA_SetChannelAudioStreamVolume},
        {"SA_JoinAudioStreamChannel", n_SA_JoinAudioStreamChannel},
        {"SA_LeaveAudioStreamChannel", n_SA_LeaveAudioStreamChannel},
        {"SA_IsPlayerInAudioStreamChannel", n_SA_IsPlayerInAudioStreamChannel},
        {"SA_GetPlayerAudioStreamChannel", n_SA_GetPlayerAudioStreamChannel},
        {"SA_SetAudioStreamChannelPass", n_SA_SetAudioStreamChannelPass},
        {"SA_CheckAudioStreamChannelPass", n_SA_CheckAudioStreamChannelPass},
        {"SA_SetAudioStreamChannelCreator", n_SA_SetAudioStreamChannelCreator},
        {"SA_SetAudioStreamChannelDestroy", n_SA_SetAudioStreamChannelDestroy},
        {"SA_GetAudioStreamChannelDestroy", n_SA_GetAudioStreamChannelDestroy},
        {"SA_SetAudioStreamChannelIdleSuspend", n_SA_SetAudioStreamChannelIdleSuspend},
        {"SA_GetAudioStreamChannelIdleSuspend", n_SA_GetAudioStreamChannelIdleSuspend},
        {"SA_GetAudioStreamChannelCount", n_SA_GetAudioStreamChannelCount},
        {"SA_GetAudioStreamChannelID", n_SA_GetAudioStreamChannelID},
        {"SA_GetAudioStreamChannelInfo", n_SA_GetAudioStreamChannelInfo},
        {"SA_GetAudioStreamChannelTrack", n_SA_GetAudioStreamChannelTrack},
        {"SA_GetAudioStreamChannelQueue", n_SA_GetAudioStreamChannelQueue},
        {"SA_GetAudioStreamChannelQueueLabel", n_SA_GetAudioStreamChannelQueueLabel},
        {"SA_SetAudioStreamChannelQueueLabel", n_SA_SetAudioStreamChannelQueueLabel},
        {"SA_SetAudioStreamChannelDescription", n_SA_SetAudioStreamChannelDescription},
        {"SA_GetAudioStreamChannelDescription", n_SA_GetAudioStreamChannelDescription},
        {"SA_SetAudioStreamChannelQueueDescription", n_SA_SetAudioStreamChannelQueueDescription},
        {"SA_GetAudioStreamChannelQueueDescription", n_SA_GetAudioStreamChannelQueueDescription},
        {"SA_GetAudioStreamChannelTrackDescription", n_SA_GetAudioStreamChannelTrackDescription},
        {"SA_ReplaceAudioStreamChannelQueue", n_SA_ReplaceAudioStreamChannelQueue},
        {"SA_FindAudioStreamChannelQueue", n_SA_FindAudioStreamChannelQueue},
        {"SA_ClearAudioStreamChannelQueue", n_SA_ClearAudioStreamChannelQueue},
        {"SA_SetAudioStreamChannelPos", n_SA_SetAudioStreamChannelPos},
        {"SA_ClearAudioStreamChannelPos", n_SA_ClearAudioStreamChannelPos},
        {"SA_SetAudioStreamChannelGroup", n_SA_SetAudioStreamChannelGroup},
        {"SA_GetAudioStreamChannelGroup", n_SA_GetAudioStreamChannelGroup},
        {"SA_AttachAudioStreamChannelToPlayer", n_SA_AttachAudioStreamChannelToPlayer},
        {"SA_AttachAudioStreamChannelToVehicle", n_SA_AttachAudioStreamChannelToVehicle},
        {"SA_RemoveAudioStreamChannelQueue", n_SA_RemoveAudioStreamChannelQueue},
        {"SA_InsertAudioStreamChannelQueue", n_SA_InsertAudioStreamChannelQueue},
        {"SA_MoveAudioStreamChannelQueue", n_SA_MoveAudioStreamChannelQueue},
        {"SA_ShuffleAudioStreamChannelQueue", n_SA_ShuffleAudioStreamChannelQueue},
        {"SA_QueueChannelAudioStreamPlaylist", n_SA_QueueChannelAudioStreamPlaylist},
        {"SA_SeekChannelAudioStream", n_SA_SeekChannelAudioStream},
        {"SA_SetAudioStreamChannelLoop", n_SA_SetAudioStreamChannelLoop},
        {"SA_GetAudioStreamChannelLoop", n_SA_GetAudioStreamChannelLoop},
        {"SA_GetAudioStreamChannelElapsed", n_SA_GetAudioStreamChannelElapsed},
        {"SA_GetAudioStreamChannelListener", n_SA_GetAudioStreamChannelListener},
        {"SA_SetAudioStreamChannelName", n_SA_SetAudioStreamChannelName},
        {"SA_GetAudioStreamChannelLiveTitle", n_SA_GetAudioStreamChannelLiveTitle},
        {"SA_SetAudioStreamChannelLiveTitle", n_SA_SetAudioStreamChannelLiveTitle},
        {"SA_SetAudioStreamChannelPlaylist", n_SA_SetAudioStreamChannelPlaylist},
        {"SA_IsAudioStreamChannelPlaylist", n_SA_IsAudioStreamChannelPlaylist},
        {"SA_GetAudioStreamChannelQueuePos", n_SA_GetAudioStreamChannelQueuePos},
        {"SA_PlayAudioStreamChannelQueue", n_SA_PlayAudioStreamChannelQueue},
        {"SA_RefreshAudioLibrary", n_SA_RefreshAudioLibrary},
        {"SA_GetAudioLibraryFolderCount", n_SA_GetAudioLibraryFolderCount},
        {"SA_GetAudioLibraryFolder", n_SA_GetAudioLibraryFolder},
        {"SA_GetAudioLibraryTrack", n_SA_GetAudioLibraryTrack},
        {"SA_GetPlayerAudioStreamID", n_SA_GetPlayerAudioStreamID},
        {"SA_GetAudioStreamCount", n_SA_GetAudioStreamCount},
        {"SA_GetAudioStreamID", n_SA_GetAudioStreamID},
        {"SA_GetAudioStreamOwner", n_SA_GetAudioStreamOwner},
        {"SA_GetAudioStreamElapsed", n_SA_GetAudioStreamElapsed},
        {"SA_GetAudioStreamURL", n_SA_GetAudioStreamURL},
        {"SA_GetAudioStreamStats", n_SA_GetAudioStreamStats},
        {"SA_GetAudioStreamInfo", n_SA_GetAudioStreamInfo},
        {"SA_GetAudioStreamLiveTitle", n_SA_GetAudioStreamLiveTitle},
        {"SA_SetAudioStreamLiveTitle", n_SA_SetAudioStreamLiveTitle},
        {"SA_IsAudioStreamExtractorReady", n_SA_IsAudioStreamExtractorReady},
        {"SA_GetAudioStreamHttpPort", n_SA_GetAudioStreamHttpPort},
        {"SA_Internal_PlayerConnect", n_SA_Internal_PlayerConnect},
        {"SA_Internal_PlayerDisconnect", n_SA_Internal_PlayerDisconnect},
        {"SA_Internal_GameModeExit", n_SA_Internal_GameModeExit},
        {nullptr, nullptr}
    };
    return natives;
}

} // namespace sampaudiostream
