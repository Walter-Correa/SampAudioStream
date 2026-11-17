#include "sampaudiostream/pawn/amx_natives.hpp"

#include "sampaudiostream/engine/audio_engine.hpp"
#include "sampaudiostream/extractor/resolver.hpp"

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
    return opts;
}

// SA_PlayAudioStreamForPlayer(playerid, const url[], Float:posX = 0.0, Float:posY = 0.0, Float:posZ = 0.0, Float:distance = 50.0, bool:usepos = false, worldid = -1, interiorid = -1, Float:volume = 100.0)
cell AMX_NATIVE_CALL n_SA_PlayAudioStreamForPlayer(AMX* amx, cell* params) {
    if (!g_engine) return INVALID_STREAM_ID;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 2) return INVALID_STREAM_ID;

    PlayerId player_id = static_cast<PlayerId>(params[1]);
    std::string url = ReadAmxString(amx, params[2]);
    StreamOptions opts = ParseStreamOptions(params, 2, argc);

    return static_cast<cell>(g_engine->PlayAudioStreamForPlayer(player_id, url, opts));
}

// SA_PlayAudioStream(const url[], Float:posX = 0.0, Float:posY = 0.0, Float:posZ = 0.0, Float:distance = 50.0, bool:usepos = false, worldid = -1, interiorid = -1, Float:volume = 100.0)
cell AMX_NATIVE_CALL n_SA_PlayAudioStream(AMX* amx, cell* params) {
    if (!g_engine) return INVALID_STREAM_ID;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 1) return INVALID_STREAM_ID;

    std::string url = ReadAmxString(amx, params[1]);
    StreamOptions opts = ParseStreamOptions(params, 1, argc);

    return static_cast<cell>(g_engine->PlayAudioStream(url, opts));
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
    std::string query = ReadAmxString(amx, params[2]);
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

// SA_CreateChannel(const name[])
cell AMX_NATIVE_CALL n_SA_CreateChannel(AMX* amx, cell* params) {
    if (!g_engine) return INVALID_CHANNEL_ID;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 1) return INVALID_CHANNEL_ID;
    return static_cast<cell>(g_engine->CreateChannel(ReadAmxString(amx, params[1])));
}

// SA_DestroyChannel(channelid)
cell AMX_NATIVE_CALL n_SA_DestroyChannel(AMX*, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 1) return 0;
    return g_engine->DestroyChannel(static_cast<ChannelId>(params[1])) ? 1 : 0;
}

// SA_PlayChannel(channelid, const url[])
cell AMX_NATIVE_CALL n_SA_PlayChannel(AMX* amx, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 2) return 0;
    return g_engine->PlayChannel(static_cast<ChannelId>(params[1]), ReadAmxString(amx, params[2])) ? 1 : 0;
}

// SA_QueueChannel(channelid, const url[])
cell AMX_NATIVE_CALL n_SA_QueueChannel(AMX* amx, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 2) return 0;
    return g_engine->QueueChannel(static_cast<ChannelId>(params[1]), ReadAmxString(amx, params[2])) ? 1 : 0;
}

// SA_SkipChannel(channelid)
cell AMX_NATIVE_CALL n_SA_SkipChannel(AMX*, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 1) return 0;
    return g_engine->SkipChannel(static_cast<ChannelId>(params[1])) ? 1 : 0;
}

// SA_StopChannel(channelid)
cell AMX_NATIVE_CALL n_SA_StopChannel(AMX*, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 1) return 0;
    return g_engine->StopChannel(static_cast<ChannelId>(params[1])) ? 1 : 0;
}

// SA_PauseChannel(channelid, bool:pause)
cell AMX_NATIVE_CALL n_SA_PauseChannel(AMX*, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 2) return 0;
    return g_engine->PauseChannel(static_cast<ChannelId>(params[1]), params[2] != 0) ? 1 : 0;
}

// SA_SetChannelVolume(channelid, Float:volume)
cell AMX_NATIVE_CALL n_SA_SetChannelVolume(AMX*, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 2) return 0;
    return g_engine->SetChannelVolume(static_cast<ChannelId>(params[1]), amx_ctof(params[2])) ? 1 : 0;
}

// SA_JoinChannel(playerid, channelid)
cell AMX_NATIVE_CALL n_SA_JoinChannel(AMX*, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 2) return 0;
    return g_engine->JoinChannel(static_cast<PlayerId>(params[1]), static_cast<ChannelId>(params[2])) ? 1 : 0;
}

// SA_LeaveChannel(playerid)
cell AMX_NATIVE_CALL n_SA_LeaveChannel(AMX*, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 1) return 0;
    return g_engine->LeaveChannel(static_cast<PlayerId>(params[1])) ? 1 : 0;
}

// SA_IsPlayerInChannel(playerid, channelid)
cell AMX_NATIVE_CALL n_SA_IsPlayerInChannel(AMX*, cell* params) {
    if (!g_engine) return 0;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 2) return 0;
    return g_engine->IsPlayerInChannel(static_cast<PlayerId>(params[1]), static_cast<ChannelId>(params[2])) ? 1 : 0;
}

// SA_GetPlayerChannel(playerid)
cell AMX_NATIVE_CALL n_SA_GetPlayerChannel(AMX*, cell* params) {
    if (!g_engine) return INVALID_CHANNEL_ID;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 1) return INVALID_CHANNEL_ID;
    return static_cast<cell>(g_engine->GetPlayerChannelId(static_cast<PlayerId>(params[1])));
}

// SA_GetPlayerStreamID(playerid)
cell AMX_NATIVE_CALL n_SA_GetPlayerStreamID(AMX*, cell* params) {
    if (!g_engine) return INVALID_STREAM_ID;
    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 1) return INVALID_STREAM_ID;
    return static_cast<cell>(g_engine->GetPlayerStreamId(static_cast<PlayerId>(params[1])));
}

// SA_GetStreamInfo(streamid, title[], title_len = sizeof(title), artist[], artist_len = sizeof(artist), &duration = 0, &state = 0)
cell AMX_NATIVE_CALL n_SA_GetStreamInfo(AMX* amx, cell* params) {
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

// SA_IsExtractorReady()
cell AMX_NATIVE_CALL n_SA_IsExtractorReady(AMX*, cell*) {
    return (g_engine && g_engine->IsExtractorReady()) ? 1 : 0;
}

// SA_GetHttpPort()
cell AMX_NATIVE_CALL n_SA_GetHttpPort(AMX*, cell*) {
    return g_engine ? static_cast<cell>(g_engine->GetHttpPort()) : 0;
}

// SA_Internal_PlayerDisconnect(playerid) - Internal runtime hook bridge
cell AMX_NATIVE_CALL n_SA_Internal_PlayerDisconnect(AMX*, cell* params) {
    if (!g_engine || params[0] < static_cast<cell>(sizeof(cell))) return 0;
    PlayerId player_id = static_cast<PlayerId>(params[1]);
    g_engine->NotifyDisconnectHookActive();
    g_engine->OnPlayerDisconnect(player_id);
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

namespace {

bool StartsWithHttpPlain(const std::string& url) {
    if (url.size() < 7) return false;
    auto eq = [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == b;
    };
    return eq(url[0], 'h') && eq(url[1], 't') && eq(url[2], 't') &&
           eq(url[3], 'p') && url[4] == ':' && url[5] == '/' && url[6] == '/';
}

} // namespace

cell AMX_NATIVE_CALL Hooked_PlayAudioStreamForPlayer(AMX* amx, cell* params) {
    if (!g_engine) {
        return g_orig_play_audio ? g_orig_play_audio(amx, params) : 0;
    }

    std::size_t argc = static_cast<std::size_t>(params[0]) / sizeof(cell);
    if (argc < 2) return 0;

    PlayerId player_id = static_cast<PlayerId>(params[1]);
    std::string url = ReadAmxString(amx, params[2]);
    StreamOptions opts = ParseStreamOptions(params, 2, argc);

    // Fast path: plain HTTP (never HTTPS) direct audio URL with default world/interior/volume
    bool plain_http_direct = StartsWithHttpPlain(url) && StreamResolver::IsKnownDirectAudioExtension(url);
    if (plain_http_direct &&
        opts.world_id == -1 &&
        opts.interior_id == -1 &&
        std::abs(opts.volume - 100.0f) < 0.5f &&
        g_orig_play_audio) {
        g_engine->StopAudioStreamForPlayer(player_id, false);
        return g_orig_play_audio(amx, params);
    }

    StreamId sid = g_engine->PlayAudioStreamForPlayer(player_id, url, opts);
    if (sid == INVALID_STREAM_ID && StartsWithHttpPlain(url) && g_orig_play_audio) {
        g_engine->StopAudioStreamForPlayer(player_id, false);
        return g_orig_play_audio(amx, params);
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

const std::vector<AMX_NATIVE_INFO>& GetPluginNatives() {
    static const std::vector<AMX_NATIVE_INFO> natives = {
        {"SA_PlayAudioStreamForPlayer", n_SA_PlayAudioStreamForPlayer},
        {"SA_PlayAudioStream", n_SA_PlayAudioStream},
        {"SA_StopAudioStreamForPlayer", n_SA_StopAudioStreamForPlayer},
        {"SA_StopAudioStream", n_SA_StopAudioStream},
        {"SA_PauseAudioStreamForPlayer", n_SA_PauseAudioStreamForPlayer},
        {"SA_SetPlayerAudioStreamVolume", n_SA_SetPlayerAudioStreamVolume},
        {"SA_SetAudioStreamVolume", n_SA_SetAudioStreamVolume},
        {"SA_AttachAudioStreamToPlayer", n_SA_AttachAudioStreamToPlayer},
        {"SA_AttachAudioStreamToVehicle", n_SA_AttachAudioStreamToVehicle},
        {"SA_SearchAudioStream", n_SA_SearchAudioStream},
        {"SA_CancelAudioStreamSearch", n_SA_CancelAudioStreamSearch},
        {"SA_CreateChannel", n_SA_CreateChannel},
        {"SA_DestroyChannel", n_SA_DestroyChannel},
        {"SA_PlayChannel", n_SA_PlayChannel},
        {"SA_QueueChannel", n_SA_QueueChannel},
        {"SA_SkipChannel", n_SA_SkipChannel},
        {"SA_StopChannel", n_SA_StopChannel},
        {"SA_PauseChannel", n_SA_PauseChannel},
        {"SA_SetChannelVolume", n_SA_SetChannelVolume},
        {"SA_JoinChannel", n_SA_JoinChannel},
        {"SA_LeaveChannel", n_SA_LeaveChannel},
        {"SA_IsPlayerInChannel", n_SA_IsPlayerInChannel},
        {"SA_GetPlayerChannel", n_SA_GetPlayerChannel},
        {"SA_GetPlayerStreamID", n_SA_GetPlayerStreamID},
        {"SA_GetStreamInfo", n_SA_GetStreamInfo},
        {"SA_IsExtractorReady", n_SA_IsExtractorReady},
        {"SA_GetHttpPort", n_SA_GetHttpPort},
        {"SA_Internal_PlayerDisconnect", n_SA_Internal_PlayerDisconnect},
        {nullptr, nullptr}
    };
    return natives;
}

} // namespace sampaudiostream
