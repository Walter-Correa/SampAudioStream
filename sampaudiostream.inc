/*
 * SampAudioStream - Universal Audio Streaming Plugin/Component for SA-MP & open.mp
 * Version: 1.1.0
 *
 * Features:
 *  - YouTube, SoundCloud, Twitch, Vimeo, TikTok, Direct MP3/OGG/WAV/FLAC & HLS streams
 *  - Zero external server setup (self-managed extractor & in-memory MP3 transcoder)
 *  - Transparent hooking of PlayAudioStreamForPlayer & StopAudioStreamForPlayer
 *  - Extended parameters: worldid, interiorid, volume, 3D spatial tracking
 *  - Asynchronous music search with structured callbacks for interactive dialogs
 *  - Synchronized multi-player radio channels with queue support
 *
 * Include Order:
 *  Include <a_samp> or <open.mp> BEFORE <sampaudiostream> so native hooking
 *  cleanly overrides PlayAudioStreamForPlayer and StopAudioStreamForPlayer.
 */

#if defined _sampaudiostream_included
    #endinput
#endif
#define _sampaudiostream_included

#if !defined _samp_included && !defined _INC_open_mp
    #error Please include <a_samp> or <open.mp> BEFORE <sampaudiostream>.
#endif

#define SAMPAUDIOSTREAM_VERSION_MAJOR  (1)
#define SAMPAUDIOSTREAM_VERSION_MINOR  (1)
#define SAMPAUDIOSTREAM_VERSION_PATCH  (0)
#define SAMPAUDIOSTREAM_VERSION_STRING "1.1.0"

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
    SA_ERR_INTERNAL_ERROR = 9
};

enum E_SA_STREAM_STATE {
    SA_STATE_IDLE = 0,
    SA_STATE_RESOLVING = 1,
    SA_STATE_BUFFERING = 2,
    SA_STATE_PLAYING = 3,
    SA_STATE_PAUSED = 4,
    SA_STATE_STOPPED = 5,
    SA_STATE_FINISHED = 6,
    SA_STATE_ERROR = 7
};

// -----------------------------------------------------------------------------
// Core Playback Natives (SA_* API)
// -----------------------------------------------------------------------------

native SA_PlayAudioStreamForPlayer(playerid, const url[], Float:posX = 0.0, Float:posY = 0.0, Float:posZ = 0.0, Float:distance = 50.0, bool:usepos = false, worldid = -1, interiorid = -1, Float:volume = 100.0);
native SA_PlayAudioStream(const url[], Float:posX = 0.0, Float:posY = 0.0, Float:posZ = 0.0, Float:distance = 50.0, bool:usepos = false, worldid = -1, interiorid = -1, Float:volume = 100.0);

native SA_StopAudioStreamForPlayer(playerid);
native SA_StopAudioStream(streamid);

native SA_PauseAudioStreamForPlayer(playerid, bool:pause);
native SA_SetPlayerAudioStreamVolume(playerid, Float:volume);
native SA_SetAudioStreamVolume(streamid, Float:volume);

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

native SA_CreateChannel(const name[]);
native SA_DestroyChannel(channelid);
native SA_PlayChannel(channelid, const url[]);
native SA_QueueChannel(channelid, const url[]);
native SA_SkipChannel(channelid);
native SA_StopChannel(channelid);
// Pausing a channel while a track is completing or transitioning preserves the
// paused state into the next queued track until resumed with pause=false.
native SA_PauseChannel(channelid, bool:pause);
native SA_SetChannelVolume(channelid, Float:volume);
native SA_JoinChannel(playerid, channelid);
native SA_LeaveChannel(playerid);
native bool:SA_IsPlayerInChannel(playerid, channelid);
native SA_GetPlayerChannel(playerid);

// -----------------------------------------------------------------------------
// Diagnostics & Metadata Natives
// -----------------------------------------------------------------------------

native SA_GetPlayerStreamID(playerid);
native SA_GetStreamInfo(streamid, title[], title_len = sizeof(title), artist[], artist_len = sizeof(artist), &duration, &E_SA_STREAM_STATE:stream_state);
native bool:SA_IsExtractorReady();
native SA_GetHttpPort();

// -----------------------------------------------------------------------------
// Callbacks
// -----------------------------------------------------------------------------

// Notes:
//  - OnAudioStreamReady receives the resolved stream URL for personal streams,
//    or the full public stream URL (empty if the public IP is not known yet)
//    for global streams.
//  - OnAudioStreamFinished only fires for server-transcoded streams. Direct HTTP
//    passthrough streams (plain http:// direct audio at 100.0% volume) are played
//    directly by the client without server-side completion tracking.
//  - On synchronized radio channels (SA_QueueChannel / SA_SkipChannel), listeners
//    cleanly reconnect on track changes, causing a brief audible transition gap.
//  - For synchronized channel errors, OnAudioStreamError is dispatched with
//    playerid = INVALID_PLAYER_ID and streamid = (-channelid - 1).
forward OnAudioStreamReady(playerid, streamid, const title[], const artist[], duration, const url[]);
forward OnAudioStreamStart(playerid, streamid);
forward OnAudioStreamFinished(playerid, streamid);
forward OnAudioStreamError(playerid, streamid, E_SA_ERROR_CODE:error_code, const error_message[]);

forward OnAudioStreamSearchResults(playerid, search_id, total_found);
forward OnAudioStreamSearchResultItem(playerid, search_id, item_index, const title[], const artist[], duration, const video_id[]);

// -----------------------------------------------------------------------------
// Internal Runtime Bridge (do not call directly)
// -----------------------------------------------------------------------------

// Internal disconnect notification bridge used by the ALS OnPlayerDisconnect hook.
native SA_Internal_PlayerDisconnect(playerid);

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

stock bool:SA_Hook_PlayAudioStream(playerid, const url[], Float:posX = 0.0, Float:posY = 0.0, Float:posZ = 0.0, Float:distance = 50.0, bool:usepos = false, worldid = -1, interiorid = -1, Float:volume = 100.0)
{
    return (SA_PlayAudioStreamForPlayer(playerid, url, posX, posY, posZ, distance, usepos, worldid, interiorid, volume) != INVALID_AUDIO_STREAM_ID);
}

#if defined _ALS_PlayAudioStreamForPlayer
    #undef PlayAudioStreamForPlayer
#else
    #define _ALS_PlayAudioStreamForPlayer
#endif
#define PlayAudioStreamForPlayer SA_Hook_PlayAudioStream

stock SA_Hook_StopAudioStream(playerid)
{
    return SA_StopAudioStreamForPlayer(playerid);
}

#if defined _ALS_StopAudioStreamForPlayer
    #undef StopAudioStreamForPlayer
#else
    #define _ALS_StopAudioStreamForPlayer
#endif
#define StopAudioStreamForPlayer SA_Hook_StopAudioStream

#endif // !SA_DISABLE_NATIVE_HOOKS

// -----------------------------------------------------------------------------
// Disconnect Lifecycle Hook (reduces SA-MP player connection polling overhead)
// Define SA_DISABLE_DISCONNECT_HOOK before including <sampaudiostream> to disable.
// -----------------------------------------------------------------------------

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
