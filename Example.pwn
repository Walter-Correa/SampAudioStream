/*
 * SampAudioStream - Showcase Filterscript
 * Demonstrates:
 *  - YouTube / SoundCloud / Twitch / Direct audio playback via SA_* and hooked PlayAudioStreamForPlayer
 *  - Asynchronous search with interactive Dialog selection
 *  - 3D spatial / world / interior / vehicle-attached audio streams
 *  - Synchronized multi-player radio channels with queue
 *  - Real-time volume & pause/resume control
 */

#define FILTERSCRIPT

#include <a_samp>
#include <sampaudiostream>

#if !defined DIALOG_STYLE_TABLIST_HEADERS
    #define DIALOG_STYLE_TABLIST_HEADERS (5)
#endif

#define DIALOG_MUSIC_SEARCH (8801)
#define MAX_SEARCH_CACHE    (10)

enum E_SEARCH_ENTRY {
    bool:seUsed,
    seTitle[128],
    seArtist[64],
    seDuration,
    seVideoId[32]
};

static gSearchItems[MAX_PLAYERS][MAX_SEARCH_CACHE][E_SEARCH_ENTRY];
static gSearchId[MAX_PLAYERS] = { INVALID_AUDIO_SEARCH_ID, ... };
static gPlayerRadioChannel = INVALID_AUDIO_CHANNEL_ID;
static gLastGlobalStream[MAX_PLAYERS] = { INVALID_AUDIO_STREAM_ID, ... };

stock FormatTrackDuration(seconds, output[], maxlen = sizeof(output))
{
    if (seconds <= 0)
    {
        format(output, maxlen, "LIVE");
        return;
    }
    new mins = seconds / 60;
    new secs = seconds % 60;
    if (mins >= 60)
    {
        new hours = mins / 60;
        mins = mins % 60;
        format(output, maxlen, "%02d:%02d:%02d", hours, mins, secs);
    }
    else
    {
        format(output, maxlen, "%02d:%02d", mins, secs);
    }
}

public OnFilterScriptInit()
{
    print("=========================================");
    print("  SampAudioStream Demo Filterscript Loaded");
    print("=========================================");

    gPlayerRadioChannel = SA_CreateChannel("SampAudioStream FM");
    return 1;
}

public OnFilterScriptExit()
{
    if (gPlayerRadioChannel != INVALID_AUDIO_CHANNEL_ID)
    {
        SA_DestroyChannel(gPlayerRadioChannel);
        gPlayerRadioChannel = INVALID_AUDIO_CHANNEL_ID;
    }
    return 1;
}

public OnPlayerConnect(playerid)
{
    gSearchId[playerid] = INVALID_AUDIO_SEARCH_ID;
    gLastGlobalStream[playerid] = INVALID_AUDIO_STREAM_ID;
    for (new i = 0; i < MAX_SEARCH_CACHE; i++)
    {
        gSearchItems[playerid][i][seUsed] = false;
    }
    return 1;
}

public OnPlayerDisconnect(playerid, reason)
{
    SA_StopAudioStreamForPlayer(playerid);
    SA_CancelAudioStreamSearch(playerid);
    return 1;
}

// -----------------------------------------------------------------------------
// Commands (standard OnPlayerCommandText for universal compatibility)
// -----------------------------------------------------------------------------

public OnPlayerCommandText(playerid, cmdtext[])
{
    new cmd[32], params[256];
    new space = strfind(cmdtext, " ");
    if (space == -1)
    {
        format(cmd, sizeof(cmd), "%s", cmdtext);
        params[0] = '\0';
    }
    else
    {
        strmid(cmd, cmdtext, 0, space);
        strmid(params, cmdtext, space + 1, strlen(cmdtext));
    }

    // /play <url or song name>
    if (!strcmp(cmd, "/play", true))
    {
        if (!params[0])
            return SendClientMessage(playerid, -1, "{FFAA00}[Audio] {FFFFFF}Usage: /play <YouTube/SoundCloud/MP3 URL or song name>");

        SendClientMessage(playerid, -1, "{00CCFF}[Audio] {FFFFFF}Resolving audio, please wait...");
        // Demonstrates that standard PlayAudioStreamForPlayer works with YouTube + extended parameters
        PlayAudioStreamForPlayer(playerid, params, 0.0, 0.0, 0.0, 50.0, false, -1, -1, 100.0);
        return 1;
    }

    // /search <query>
    if (!strcmp(cmd, "/search", true))
    {
        if (!params[0])
            return SendClientMessage(playerid, -1, "{FFAA00}[Audio] {FFFFFF}Usage: /search <song title or artist>");

        for (new i = 0; i < MAX_SEARCH_CACHE; i++)
        {
            gSearchItems[playerid][i][seUsed] = false;
        }
        gSearchId[playerid] = SA_SearchAudioStream(playerid, params, MAX_SEARCH_CACHE);
        SendClientMessage(playerid, -1, "{00CCFF}[Audio] {FFFFFF}Searching tracks...");
        return 1;
    }

    // /stop
    if (!strcmp(cmd, "/stop", true))
    {
        SA_StopAudioStreamForPlayer(playerid);
        SendClientMessage(playerid, -1, "{00CCFF}[Audio] {FFFFFF}Playback stopped.");
        return 1;
    }

    // /pause
    if (!strcmp(cmd, "/pause", true))
    {
        SA_PauseAudioStreamForPlayer(playerid, true);
        SendClientMessage(playerid, -1, "{00CCFF}[Audio] {FFFFFF}Playback paused.");
        return 1;
    }

    // /resume
    if (!strcmp(cmd, "/resume", true))
    {
        SA_PauseAudioStreamForPlayer(playerid, false);
        SendClientMessage(playerid, -1, "{00CCFF}[Audio] {FFFFFF}Playback resumed.");
        return 1;
    }

    // /volume <0-200>
    if (!strcmp(cmd, "/volume", true))
    {
        if (!params[0])
            return SendClientMessage(playerid, -1, "{FFAA00}[Audio] {FFFFFF}Usage: /volume <0 - 200>");

        new Float:vol = floatstr(params);
        if (SA_SetPlayerAudioStreamVolume(playerid, vol))
        {
            new msg[96];
            format(msg, sizeof(msg), "{00CCFF}[Audio] {FFFFFF}Volume set to %.0f%%.", vol);
            SendClientMessage(playerid, -1, msg);
        }
        else
        {
            SendClientMessage(playerid, -1, "{FF4444}[Audio] {FFFFFF}No active personal stream found.");
        }
        return 1;
    }

    // /zone <url or song name> (Creates a 3D zone stream in the current world/interior; attaches to vehicle if driving)
    if (!strcmp(cmd, "/zone", true))
    {
        if (!params[0])
            return SendClientMessage(playerid, -1, "{FFAA00}[Audio] {FFFFFF}Usage: /zone <URL or song name>");

        if (gLastGlobalStream[playerid] != INVALID_AUDIO_STREAM_ID)
        {
            SA_StopAudioStream(gLastGlobalStream[playerid]);
        }

        new Float:x, Float:y, Float:z;
        GetPlayerPos(playerid, x, y, z);
        new vw = GetPlayerVirtualWorld(playerid);
        new interior = GetPlayerInterior(playerid);

        new sid = SA_PlayAudioStream(params, x, y, z, 35.0, true, vw, interior, 100.0);
        gLastGlobalStream[playerid] = sid;

        new vehid = GetPlayerVehicleID(playerid);
        if (vehid > 0 && sid != INVALID_AUDIO_STREAM_ID)
        {
            SA_AttachAudioStreamToVehicle(sid, vehid);
            SendClientMessage(playerid, -1, "{00CCFF}[Audio] {FFFFFF}3D stream attached to your vehicle!");
        }
        else
        {
            SendClientMessage(playerid, -1, "{00CCFF}[Audio] {FFFFFF}3D stream created at your position (World/Interior filtered)!");
        }
        return 1;
    }

    // /stopzone
    if (!strcmp(cmd, "/stopzone", true))
    {
        if (gLastGlobalStream[playerid] != INVALID_AUDIO_STREAM_ID)
        {
            SA_StopAudioStream(gLastGlobalStream[playerid]);
            gLastGlobalStream[playerid] = INVALID_AUDIO_STREAM_ID;
            SendClientMessage(playerid, -1, "{00CCFF}[Audio] {FFFFFF}3D zone stream stopped.");
        }
        return 1;
    }

    // /radio <join|leave|play <url/query>|queue <url/query>|skip>
    if (!strcmp(cmd, "/radio", true))
    {
        if (!strcmp(params, "join", true))
        {
            SA_JoinChannel(playerid, gPlayerRadioChannel);
            SendClientMessage(playerid, -1, "{00FF88}[Radio] {FFFFFF}You tuned into the Synchronized Radio.");
            return 1;
        }
        if (!strcmp(params, "leave", true))
        {
            SA_LeaveChannel(playerid);
            SendClientMessage(playerid, -1, "{00FF88}[Radio] {FFFFFF}You left the Radio.");
            return 1;
        }
        if (!strcmp(params, "skip", true))
        {
            SA_SkipChannel(gPlayerRadioChannel);
            SendClientMessage(playerid, -1, "{00FF88}[Radio] {FFFFFF}Skipping to the next queued track...");
            return 1;
        }
        if (!strcmp(params, "play ", true, 5))
        {
            SA_PlayChannel(gPlayerRadioChannel, params[5]);
            SendClientMessage(playerid, -1, "{00FF88}[Radio] {FFFFFF}Starting synchronized radio broadcast...");
            return 1;
        }
        if (!strcmp(params, "queue ", true, 6))
        {
            SA_QueueChannel(gPlayerRadioChannel, params[6]);
            SendClientMessage(playerid, -1, "{00FF88}[Radio] {FFFFFF}Track added to the radio queue!");
            return 1;
        }
        SendClientMessage(playerid, -1, "{FFAA00}[Radio] {FFFFFF}Usage: /radio <join | leave | play <url/query> | queue <url/query> | skip>");
        return 1;
    }

    return 0;
}

// -----------------------------------------------------------------------------
// Search Callbacks & Interactive Dialog
// -----------------------------------------------------------------------------

public OnAudioStreamSearchResults(playerid, search_id, total_found)
{
    if (search_id != gSearchId[playerid]) return 1;

    if (total_found <= 0)
    {
        SendClientMessage(playerid, -1, "{FF4444}[Audio] {FFFFFF}No tracks found for your search.");
        return 1;
    }

    new dialog_str[2048];
    format(dialog_str, sizeof(dialog_str), "Track\tArtist / Channel\tDuration\n");

    for (new i = 0; i < total_found && i < MAX_SEARCH_CACHE; i++)
    {
        if (!gSearchItems[playerid][i][seUsed]) continue;
        new dur_str[16], line[220];
        FormatTrackDuration(gSearchItems[playerid][i][seDuration], dur_str, sizeof(dur_str));
        format(line, sizeof(line), "%s\t%s\t%s\n",
            gSearchItems[playerid][i][seTitle],
            gSearchItems[playerid][i][seArtist],
            dur_str);
        strcat(dialog_str, line);
    }

    ShowPlayerDialog(playerid, DIALOG_MUSIC_SEARCH, DIALOG_STYLE_TABLIST_HEADERS, "Search Results - Select a track", dialog_str, "Play", "Close");
    return 1;
}

public OnAudioStreamSearchResultItem(playerid, search_id, item_index, const title[], const artist[], duration, const video_id[])
{
    if (search_id != gSearchId[playerid] || item_index < 0 || item_index >= MAX_SEARCH_CACHE)
        return 1;

    gSearchItems[playerid][item_index][seUsed] = true;
    format(gSearchItems[playerid][item_index][seTitle], 128, "%s", title);
    format(gSearchItems[playerid][item_index][seArtist], 64, "%s", artist);
    gSearchItems[playerid][item_index][seDuration] = duration;
    format(gSearchItems[playerid][item_index][seVideoId], 32, "%s", video_id);
    return 1;
}

public OnDialogResponse(playerid, dialogid, response, listitem, inputtext[])
{
    if (dialogid == DIALOG_MUSIC_SEARCH)
    {
        if (!response || listitem < 0 || listitem >= MAX_SEARCH_CACHE)
            return 1;

        if (!gSearchItems[playerid][listitem][seUsed])
            return 1;

        new url[128], msg[192];
        format(url, sizeof(url), "https://www.youtube.com/watch?v=%s", gSearchItems[playerid][listitem][seVideoId]);
        format(msg, sizeof(msg), "{00CCFF}[Audio] {FFFFFF}Loading: {00FF88}%s", gSearchItems[playerid][listitem][seTitle]);
        SendClientMessage(playerid, -1, msg);

        SA_PlayAudioStreamForPlayer(playerid, url);
        return 1;
    }
    return 0;
}

// -----------------------------------------------------------------------------
// Playback Lifecycle Callbacks
// -----------------------------------------------------------------------------

public OnAudioStreamReady(playerid, streamid, const title[], const artist[], duration, const url[])
{
    if (playerid != INVALID_PLAYER_ID)
    {
        new dur_str[16], msg[256];
        FormatTrackDuration(duration, dur_str, sizeof(dur_str));
        format(msg, sizeof(msg), "{00FF88}[Now Playing] {FFFFFF}%s - %s [%s]", title, artist, dur_str);
        SendClientMessage(playerid, -1, msg);
    }
    return 1;
}

public OnAudioStreamFinished(playerid, streamid)
{
    if (playerid != INVALID_PLAYER_ID)
    {
        SendClientMessage(playerid, -1, "{00CCFF}[Audio] {FFFFFF}Playback finished.");
    }
    return 1;
}

public OnAudioStreamError(playerid, streamid, E_SA_ERROR_CODE:error_code, const error_message[])
{
    if (playerid != INVALID_PLAYER_ID)
    {
        new msg[256];
        format(msg, sizeof(msg), "{FF4444}[Audio Error #%d] {FFFFFF}%s", _:error_code, error_message);
        SendClientMessage(playerid, -1, msg);
    }
    return 1;
}
