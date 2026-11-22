/*
 * SampAudioStream - Universal Audio Streaming Plugin/Component for SA-MP & open.mp
 * Version: 2.3.2
 * Repository: https://github.com/Walter-Correa/SampAudioStream
 * Documentation: docs/api-reference.md
 *
 * Include <a_samp> or <open.mp> BEFORE <SampAudioStream> so native hooking
 * cleanly overrides PlayAudioStreamForPlayer and StopAudioStreamForPlayer.
 */

#if defined _SampAudioStream_included
    #endinput
#endif
#define _SampAudioStream_included

#if !defined _samp_included && !defined _INC_open_mp
    #error Please include <a_samp> or <open.mp> BEFORE <SampAudioStream>.
#endif

#define SAMPAUDIOSTREAM_VERSION_MAJOR  (2)
#define SAMPAUDIOSTREAM_VERSION_MINOR  (3)
#define SAMPAUDIOSTREAM_VERSION_PATCH  (2)
#define SAMPAUDIOSTREAM_VERSION_STRING "2.3.2"

// -----------------------------------------------------------------------------
// Constants & Enums
// -----------------------------------------------------------------------------

#if !defined INVALID_AUDIO_STREAM_ID
    #define INVALID_AUDIO_STREAM_ID (-1)
#endif

#if !defined INVALID_AUDIO_CHANNEL_ID
    #define INVALID_AUDIO_CHANNEL_ID (-1)
#endif

#if !defined INVALID_AUDIO_SEARCH_ID
    #define INVALID_AUDIO_SEARCH_ID (-1)
#endif

enum E_SA_ERROR_CODE {
    SA_ERR_NONE = 0,
    SA_ERR_INVALID_URL = 1,
    SA_ERR_EXTRACTION_FAILED = 2,
    SA_ERR_NETWORK_ERROR = 3,
    SA_ERR_UNSUPPORTED_FORMAT = 4,
    SA_ERR_TRANSCODE_ERROR = 5,
    SA_ERR_EXTRACTOR_NOT_READY = 6,
    SA_ERR_SEARCH_FAILED = 7,
    SA_ERR_STREAM_NOT_FOUND = 8,
    SA_ERR_INTERNAL_ERROR = 9,
    SA_ERR_AUTH_REQUIRED = 10
};

enum E_SA_STREAM_STATE {
    SA_STATE_IDLE = 0,
    SA_STATE_RESOLVING = 1,
    SA_STATE_BUFFERING = 2,
    SA_STATE_PLAYING = 3,
    SA_STATE_PAUSED = 4,
    SA_STATE_STOPPED = 5,
    SA_STATE_FINISHED = 6,
    SA_STATE_ERROR = 7,
    SA_STATE_SUSPENDED = 8
};

enum E_SA_CHANNEL_DESTROY_MODE {
    SA_CHANNEL_DESTROY_MANUAL = 0,
    SA_CHANNEL_DESTROY_ON_DISCONNECT = 1,
    SA_CHANNEL_DESTROY_ON_TIMEOUT = 2
};

enum E_SA_CHANNEL_DESTROY_REASON {
    SA_CH_DESTROY_REASON_MANUAL = 0,
    SA_CH_DESTROY_REASON_CREATOR = 1,
    SA_CH_DESTROY_REASON_TIMEOUT = 2
};

enum E_SA_CHANNEL_LOOP_MODE {
    SA_CHANNEL_LOOP_OFF = 0,
    SA_CHANNEL_LOOP_TRACK = 1,
    SA_CHANNEL_LOOP_QUEUE = 2
};

enum E_SA_AUDIO_STREAM_SOURCE {
    SA_SOURCE_NONE = 0,
    SA_SOURCE_PERSONAL = 1,
    SA_SOURCE_CHANNEL = 2,
    SA_SOURCE_GLOBAL = 3,
    SA_SOURCE_CHANNEL_RANGE = 4,
    SA_SOURCE_SOUND = 5
};

// -----------------------------------------------------------------------------
// Core Playback Natives (SA_* API)
// -----------------------------------------------------------------------------

native SA_PlayAudioStreamForPlayer(playerid, const url[], Float:posX = 0.0, Float:posY = 0.0, Float:posZ = 0.0, Float:distance = 50.0, bool:usepos = false, worldid = -1, interiorid = -1, Float:volume = 100.0, bool:notify_processing = true, bool:process_direct_audio_files = false, bool:stream_text = true);
native SA_PlayAudioStream(const url[], Float:posX = 0.0, Float:posY = 0.0, Float:posZ = 0.0, Float:distance = 50.0, bool:usepos = false, worldid = -1, interiorid = -1, Float:volume = 100.0, bool:notify_processing = true, bool:process_direct_audio_files = false, bool:stream_text = true);

native bool:SA_PlayAudioStreamSoundForPlayer(playerid, const sound[], Float:posX = 0.0, Float:posY = 0.0, Float:posZ = 0.0, Float:distance = 50.0, bool:usepos = false);

native SA_StopAudioStreamForPlayer(playerid);
native SA_StopAudioStream(streamid);

native SA_PauseAudioStreamForPlayer(playerid, bool:pause);
native SA_PauseAudioStream(streamid, bool:pause);
native SA_SeekAudioStream(streamid, seconds);
native SA_SetPlayerAudioStreamMuted(playerid, bool:mute, bool:include_personal = false);
native bool:SA_IsPlayerAudioStreamMuted(playerid);
native SA_SetPlayerAudioStreamGroupMuted(playerid, group, bool:mute);
native bool:SA_IsPlayerAudioStreamGroupMuted(playerid, group);
native SA_GetPlayerAudioStreamSource(playerid, &E_SA_AUDIO_STREAM_SOURCE:source_type, &source_id);
native SA_SetPlayerAudioStreamVolume(playerid, Float:volume);
native SA_SetAudioStreamVolume(streamid, Float:volume);
native SA_SetPlayerAudioStreamDistance(playerid, Float:distance);
native SA_SetAudioStreamDistance(streamid, Float:distance);
native SA_SetPlayerAudioStreamPos(playerid, Float:posX, Float:posY, Float:posZ);
native SA_SetAudioStreamPos(streamid, Float:posX, Float:posY, Float:posZ);

native SA_AttachAudioStreamToPlayer(streamid, target_playerid);
native SA_AttachAudioStreamToVehicle(streamid, vehicleid);

// -----------------------------------------------------------------------------
// Asynchronous Search Natives
// -----------------------------------------------------------------------------

native SA_SearchAudioStream(playerid, const query[], max_results = 5);
native SA_CancelAudioStreamSearch(playerid);

// -----------------------------------------------------------------------------
// Synchronized Channel / Radio Natives
// -----------------------------------------------------------------------------

native SA_CreateAudioStreamChannel(const name[], creator_playerid = INVALID_PLAYER_ID, const password[] = "", E_SA_CHANNEL_DESTROY_MODE:destroy_mode = SA_CHANNEL_DESTROY_MANUAL, destroy_timeout_sec = 0, bool:persistent = true);
native SA_DestroyAudioStreamChannel(channelid);
native SA_PlayChannelAudioStream(channelid, const url[]);
native SA_QueueChannelAudioStream(channelid, const url[], const label[] = "");
native SA_SkipChannelAudioStream(channelid);
native SA_StopChannelAudioStream(channelid);
native SA_PauseChannelAudioStream(channelid, bool:pause);
native SA_SetChannelAudioStreamVolume(channelid, Float:volume);
native SA_JoinAudioStreamChannel(playerid, channelid, const password[] = "", bool:ignore_password = false);
native SA_LeaveAudioStreamChannel(playerid);
native bool:SA_IsPlayerInAudioStreamChannel(playerid, channelid);
native SA_GetPlayerAudioStreamChannel(playerid);
native SA_SetAudioStreamChannelPass(channelid, const password[]);
native bool:SA_CheckAudioStreamChannelPass(channelid, const password[]);
native SA_SetAudioStreamChannelCreator(channelid, playerid);
native SA_SetAudioStreamChannelDestroy(channelid, E_SA_CHANNEL_DESTROY_MODE:destroy_mode, destroy_timeout_sec = 0);
native SA_GetAudioStreamChannelDestroy(channelid, &E_SA_CHANNEL_DESTROY_MODE:destroy_mode, &destroy_timeout_sec, &destroy_remaining_sec, &bool:creator_online);
native SA_SetAudioStreamChannelIdleSuspend(channelid, bool:enable, grace_seconds = 30);
native SA_GetAudioStreamChannelIdleSuspend(channelid, &bool:enable, &grace_seconds, &bool:suspended);
native SA_GetAudioStreamChannelCount();
native SA_GetAudioStreamChannelID(index);
native SA_GetAudioStreamChannelInfo(channelid, name[], name_len = sizeof(name), creator_name[], creator_len = sizeof(creator_name), &listener_count, &E_SA_STREAM_STATE:channel_state, &bool:has_password, &queue_count);
native SA_GetAudioStreamChannelTrack(channelid, title[], title_len = sizeof(title), artist[], artist_len = sizeof(artist), &duration);
native SA_GetAudioStreamChannelQueue(channelid, index, item[], item_len = sizeof(item));
native SA_GetAudioStreamChannelQueueLabel(channelid, index, label[], label_len = sizeof(label));
native SA_SetAudioStreamChannelDescription(channelid, const description[]);
native SA_GetAudioStreamChannelDescription(channelid, description[], description_len = sizeof(description));
native SA_SetAudioStreamChannelQueueDescription(channelid, index, const description[]);
native SA_GetAudioStreamChannelQueueDescription(channelid, index, description[], description_len = sizeof(description));
native SA_GetAudioStreamChannelTrackDescription(channelid, description[], description_len = sizeof(description));
native SA_FindAudioStreamChannelQueue(channelid, const item[], start_index = 0);
native SA_ClearAudioStreamChannelQueue(channelid);
native SA_SetAudioStreamChannelPos(channelid, Float:x, Float:y, Float:z, Float:distance = 50.0, worldid = -1, interiorid = -1, bool:auto_listen = true);
native SA_ClearAudioStreamChannelPos(channelid);
native SA_SetAudioStreamChannelGroup(channelid, group);
native SA_GetAudioStreamChannelGroup(channelid);
native SA_AttachAudioStreamChannelToPlayer(channelid, target_playerid, Float:distance = 0.0);
native SA_AttachAudioStreamChannelToVehicle(channelid, vehicleid, Float:distance = 0.0);
native SA_RemoveAudioStreamChannelQueue(channelid, index);
native SA_InsertAudioStreamChannelQueue(channelid, index, const url[], const label[] = "");
native SA_ReplaceAudioStreamChannelQueue(channelid, index, const url[], const label[] = "", bool:keep_description = false);
native SA_MoveAudioStreamChannelQueue(channelid, from_index, to_index);
native SA_ShuffleAudioStreamChannelQueue(channelid);
native SA_QueueChannelAudioStreamPlaylist(channelid, const url[], max_items = 25);
native SA_SeekChannelAudioStream(channelid, seconds);
native SA_SetAudioStreamChannelLoop(channelid, E_SA_CHANNEL_LOOP_MODE:mode);
native E_SA_CHANNEL_LOOP_MODE:SA_GetAudioStreamChannelLoop(channelid);
native SA_GetAudioStreamChannelElapsed(channelid);
native SA_GetAudioStreamChannelListener(channelid, index);
native SA_SetAudioStreamChannelName(channelid, const name[]);
native SA_GetAudioStreamChannelLiveTitle(channelid, title[], title_len = sizeof(title));
native SA_SetAudioStreamChannelLiveTitle(channelid, const title[]);
native SA_SetAudioStreamChannelPlaylist(channelid, bool:enable);
native bool:SA_IsAudioStreamChannelPlaylist(channelid);
native SA_GetAudioStreamChannelQueuePos(channelid);
native SA_PlayAudioStreamChannelQueue(channelid, index);

// -----------------------------------------------------------------------------
// Local Audio Library Natives
// -----------------------------------------------------------------------------

native SA_RefreshAudioLibrary();
native SA_GetAudioLibraryFolderCount();
native SA_GetAudioLibraryFolder(index, name[], name_len = sizeof(name), &track_count);
native SA_GetAudioLibraryTrack(folder_index, track_index, name[], name_len = sizeof(name));

// -----------------------------------------------------------------------------
// Diagnostics & Metadata Natives
// -----------------------------------------------------------------------------

native SA_GetPlayerAudioStreamID(playerid);
native SA_GetAudioStreamCount();
native SA_GetAudioStreamID(index);
native SA_GetAudioStreamOwner(streamid);
native SA_GetAudioStreamElapsed(streamid);
native SA_GetAudioStreamURL(streamid, url[], url_len = sizeof(url));
native SA_GetAudioStreamStats(&active_streams, &active_channels, &active_transcoders, &active_listeners);
native SA_GetAudioStreamInfo(streamid, title[], title_len = sizeof(title), artist[], artist_len = sizeof(artist), &duration, &E_SA_STREAM_STATE:stream_state);
native SA_GetAudioStreamLiveTitle(streamid, title[], title_len = sizeof(title));
native SA_SetAudioStreamLiveTitle(streamid, const title[]);
native bool:SA_IsAudioStreamExtractorReady();
native SA_GetAudioStreamHttpPort();

// -----------------------------------------------------------------------------
// Callbacks
// -----------------------------------------------------------------------------

forward OnAudioStreamProcessing(playerid, streamid, const url[], bool:from_cache);
forward OnAudioStreamWaiting(playerid, streamid, wait_ms);
forward OnAudioStreamReady(playerid, streamid, const title[], const artist[], duration, const url[]);
forward OnAudioStreamStart(playerid, streamid);
forward OnAudioStreamFinished(playerid, streamid);
forward OnAudioStreamError(playerid, streamid, E_SA_ERROR_CODE:error_code, const error_message[]);
forward OnAudioStreamLiveTitleChange(playerid, streamid, const title[]);
forward OnPlayerAudioStreamSourceChange(playerid, E_SA_AUDIO_STREAM_SOURCE:source_type, source_id);
forward OnPlayerAudioStreamNoConnect(playerid, E_SA_AUDIO_STREAM_SOURCE:source_type, source_id);

forward OnAudioStreamSearchResults(playerid, search_id, total_found, E_SA_ERROR_CODE:error_code, const error_message[]);
forward OnAudioStreamSearchResultItem(playerid, search_id, item_index, const title[], const artist[], duration, const video_id[]);
forward OnAudioStreamChannelDestroyed(channelid, E_SA_CHANNEL_DESTROY_REASON:reason);
forward OnAudioStreamChannelTrackWaiting(channelid, wait_ms);
forward OnAudioStreamChannelTrackStart(channelid, const title[], const artist[], duration, const url[]);
forward OnAudioStreamChannelQueueEnd(channelid);
forward OnAudioStreamChannelError(channelid, E_SA_ERROR_CODE:error_code, const error_message[]);
forward OnAudioStreamChannelRestored(channelid, const name[], const creator_name[]);
forward OnAudioStreamChannelPlaylistQueued(channelid, added, E_SA_ERROR_CODE:error_code, const error_message[]);
forward OnAudioStreamChannelLiveTitleChange(channelid, const title[]);

// -----------------------------------------------------------------------------
// Internal Runtime Bridge (do not call directly)
// -----------------------------------------------------------------------------

// Internal connect/disconnect/gamemode-exit notification bridges used by ALS lifecycle hooks.
native SA_Internal_PlayerConnect(playerid);
native SA_Internal_PlayerDisconnect(playerid);
native SA_Internal_GameModeExit();

// Forces the Pawn compiler to retain all host natives required by SampHost in
// the AMX native table even when hooked or unused directly in script code.
#if defined __Pawn && __Pawn >= 0x030A
    #pragma warning push
    #pragma warning disable 234
#endif
forward SA_Internal_ImportNatives();
public SA_Internal_ImportNatives()
{
    new Float:f, s[16];
    PlayAudioStreamForPlayer(0, "");
    StopAudioStreamForPlayer(0);
    IsPlayerConnected(0);
    GetPlayerPos(0, f, f, f);
    GetPlayerVirtualWorld(0);
    GetPlayerInterior(0);
    GetPlayerVehicleID(0);
    GetVehiclePos(0, f, f, f);
    GetPlayerIp(0, s, sizeof s);
    GetPlayerName(0, s, sizeof s);
    SetPlayerName(0, "");
    GetServerVarAsInt("port");
    GetServerVarAsString("bind", s, sizeof s);
#if defined GetPlayerPoolSize
    GetPlayerPoolSize();
#endif
    return 0;
}
#if defined __Pawn && __Pawn >= 0x030A
    #pragma warning pop
#endif

// -----------------------------------------------------------------------------
// Transparent Native Hooking (PlayAudioStreamForPlayer & StopAudioStreamForPlayer)
// Allows calling standard SA-MP natives with YouTube links AND extended optional params
// -----------------------------------------------------------------------------

#if !defined SA_DISABLE_NATIVE_HOOKS

stock bool:SA_Hook_PlayAudioStream(playerid, const url[], Float:posX = 0.0, Float:posY = 0.0, Float:posZ = 0.0, Float:distance = 50.0, bool:usepos = false, worldid = -1, interiorid = -1, Float:volume = 100.0, bool:notify_processing = true, bool:process_direct_audio_files = false, bool:stream_text = true)
{
    return (SA_PlayAudioStreamForPlayer(playerid, url, posX, posY, posZ, distance, usepos, worldid, interiorid, volume, notify_processing, process_direct_audio_files, stream_text) != INVALID_AUDIO_STREAM_ID);
}

#if defined _ALS_PlayAudioStreamForPlayer
    #undef PlayAudioStreamForPlayer
#else
    #define _ALS_PlayAudioStreamForPlayer
#endif
#define PlayAudioStreamForPlayer SA_Hook_PlayAudioStream

stock SA_Hook_StopAudioStream(playerid)
{
    SA_StopAudioStreamForPlayer(playerid);
    SA_LeaveAudioStreamChannel(playerid);
    return 1;
}

#if defined _ALS_StopAudioStreamForPlayer
    #undef StopAudioStreamForPlayer
#else
    #define _ALS_StopAudioStreamForPlayer
#endif
#define StopAudioStreamForPlayer SA_Hook_StopAudioStream

#endif // !SA_DISABLE_NATIVE_HOOKS

// -----------------------------------------------------------------------------
// Connect & Disconnect Lifecycle Hooks
// Define SA_DISABLE_CONNECT_HOOK / SA_DISABLE_DISCONNECT_HOOK before including
// <SampAudioStream> to disable either ALS hook.
// -----------------------------------------------------------------------------

#if !defined SA_DISABLE_CONNECT_HOOK

public OnPlayerConnect(playerid)
{
    SA_Internal_PlayerConnect(playerid);
    if (funcidx("SA_OnPlayerConnect") != -1)
    {
        return CallLocalFunction("SA_OnPlayerConnect", "i", playerid);
    }
    return 1;
}

#if defined _ALS_OnPlayerConnect
    #undef OnPlayerConnect
#else
    #define _ALS_OnPlayerConnect
#endif
#define OnPlayerConnect SA_OnPlayerConnect
forward SA_OnPlayerConnect(playerid);

#endif // !SA_DISABLE_CONNECT_HOOK

#if !defined SA_DISABLE_DISCONNECT_HOOK

public OnPlayerDisconnect(playerid, reason)
{
    SA_Internal_PlayerDisconnect(playerid);
    if (funcidx("SA_OnPlayerDisconnect") != -1)
    {
        return CallLocalFunction("SA_OnPlayerDisconnect", "ii", playerid, reason);
    }
    return 1;
}

#if defined _ALS_OnPlayerDisconnect
    #undef OnPlayerDisconnect
#else
    #define _ALS_OnPlayerDisconnect
#endif
#define OnPlayerDisconnect SA_OnPlayerDisconnect
forward SA_OnPlayerDisconnect(playerid, reason);

#endif // !SA_DISABLE_DISCONNECT_HOOK

#if !defined SA_DISABLE_GAMEMODE_EXIT_HOOK

public OnGameModeExit()
{
    SA_Internal_GameModeExit();
    if (funcidx("SA_OnGameModeExit") != -1)
    {
        return CallLocalFunction("SA_OnGameModeExit", "");
    }
    return 1;
}

#if defined _ALS_OnGameModeExit
    #undef OnGameModeExit
#else
    #define _ALS_OnGameModeExit
#endif
#define OnGameModeExit SA_OnGameModeExit
forward SA_OnGameModeExit();

#endif // !SA_DISABLE_GAMEMODE_EXIT_HOOK
