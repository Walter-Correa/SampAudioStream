/*
 * =============================================================================
 * SampAudioStream - Full Showcase & Interactive Demo Filterscript (English)
 * Plug-and-Play: Requires ONLY <a_samp> (or <open.mp>) and <SampAudioStream>
 *
 * Compilation:
 *   pawncc FS_SampAudioStream_Demo.pwn -;+ -(+ -d0 -O1
 *
 * Commands:
 *   /audiomenu  or  /sa   - Open the interactive SampAudioStream Control Center
 *
 * Demonstrates ALL 66 SA_* natives, hooked PlayAudioStreamForPlayer / Stop,
 * and ALL 16 plugin callbacks cleanly from in-game menus (no shadow arrays).
 *
 * Persistence & Permissions:
 *  - Reuses the persistent server radio channel ("SampAudioStream FM") across restarts/reloads;
 *    channels created in Manual mode also stay saved on disk until explicitly destroyed.
 *  - Never destroys channels in OnFilterScriptExit, keeping channel persistence intact.
 *  - Players cannot control another player's stream or channel; server-owned streams and
 *    channels ("Server" creator) are free for all. Channel ownership compares player name
 *    (as stored by the plugin); adapt CanPlayerManageChannel to your account system in production.
 * =============================================================================
 */

#include <a_samp>
#include <SampAudioStream>

// -----------------------------------------------------------------------------
// Configuration & Dialog IDs (high offset to avoid gamemode dialog collisions)
// -----------------------------------------------------------------------------

#define DEMO_DLG_BASE              (18400)
#define DEMO_MAX_SEARCH_ITEMS      (10)
#define DEMO_MAX_TRACKED_GLOBALS   (64)
#define DEMO_MAX_LISTED_CHANNELS   (64)
#define DEMO_MAX_QUEUE_VIEW_ITEMS  (10)
#define DEMO_SERVER_RADIO_NAME     "SampAudioStream FM"

enum
{
    DLG_SA_MAIN = DEMO_DLG_BASE,
    DLG_SA_PERSONAL_MENU,
    DLG_SA_PERSONAL_PLAY,
    DLG_SA_PERSONAL_VOL,
    DLG_SA_PERSONAL_DIST,
    DLG_SA_GLOBAL_MENU,
    DLG_SA_GLOBAL_PLAY_3D,
    DLG_SA_GLOBAL_PLAY_2D,
    DLG_SA_GLOBAL_STOP_ID,
    DLG_SA_GLOBAL_DIST,
    DLG_SA_GLOBAL_VOL,
    DLG_SA_GLOBAL_ATTACH_PLY,
    DLG_SA_GLOBAL_INFO_ID,
    DLG_SA_SEARCH_INPUT,
    DLG_SA_SEARCH_RESULTS,
    DLG_SA_SEARCH_ACTION,
    DLG_SA_CH_LIST,
    DLG_SA_CH_CREATE_NAME,
    DLG_SA_CH_CREATE_PASS,
    DLG_SA_CH_CREATE_MODE,
    DLG_SA_CH_CREATE_TIME,
    DLG_SA_CH_JOIN_PASS,
    DLG_SA_CH_MENU,
    DLG_SA_CH_PLAY,
    DLG_SA_CH_QUEUE,
    DLG_SA_CH_QUEUE_VIEW,
    DLG_SA_CH_VOLUME,
    DLG_SA_CH_SET_PASS,
    DLG_SA_CH_DESTROY_MENU,
    DLG_SA_CH_DESTROY_TIME,
    DLG_SA_CH_RENAME,
    DLG_SA_CH_POS_MENU,
    DLG_SA_CH_POS_SET,
    DLG_SA_CH_QUEUE_INS0,
    DLG_SA_CH_QUEUE_REM,
    DLG_SA_CH_PLAYLIST,
    DLG_SA_CH_SEEK,
    DLG_SA_CH_LISTENERS,
    DLG_SA_GLOBAL_PAUSE_ID,
    DLG_SA_GLOBAL_SEEK_ID,
    DLG_SA_GLOBAL_SEEK_SEC,
    DLG_SA_VANILLA_PLAY,
    DLG_SA_DIAGNOSTICS
}

// -----------------------------------------------------------------------------
// Per-Player State
// -----------------------------------------------------------------------------

static gSearchId[MAX_PLAYERS];
static gSearchTotal[MAX_PLAYERS];
static gSearchSelIdx[MAX_PLAYERS];
static gSearchTitle[MAX_PLAYERS][DEMO_MAX_SEARCH_ITEMS][64 char];
static gSearchArtist[MAX_PLAYERS][DEMO_MAX_SEARCH_ITEMS][48 char];
static gSearchDur[MAX_PLAYERS][DEMO_MAX_SEARCH_ITEMS];
static gSearchVid[MAX_PLAYERS][DEMO_MAX_SEARCH_ITEMS][24 char];

static gLastGlobalSid[MAX_PLAYERS];
static gSeekTargetSid[MAX_PLAYERS];
static bool:gOptNotify[MAX_PLAYERS];
static bool:gOptProcessFiles[MAX_PLAYERS];
static gSelChannelId[MAX_PLAYERS];
static bool:gSelChanAutoListen[MAX_PLAYERS];

// Last channel list & queue snapshot shown to each player (race-condition guard)
static gChListIds[MAX_PLAYERS][DEMO_MAX_LISTED_CHANNELS];
static gChListCount[MAX_PLAYERS];
static gQueueViewHash[MAX_PLAYERS][DEMO_MAX_QUEUE_VIEW_ITEMS];
static gQueueViewCount[MAX_PLAYERS];

// Global streams created through this filterscript and their creator playerid
static gTrackedGlobalSid[DEMO_MAX_TRACKED_GLOBALS];
static gTrackedGlobalOwner[DEMO_MAX_TRACKED_GLOBALS];

// Temporary channel creation wizard state per player
static gNewChName[MAX_PLAYERS][48];
static gNewChPass[MAX_PLAYERS][32];

// Official server channel created on startup
static gServerRadioId = INVALID_AUDIO_CHANNEL_ID;

// -----------------------------------------------------------------------------
// Internal Helpers
// -----------------------------------------------------------------------------

static stock SanitizeDialogField(str[])
{
    for (new i = 0; str[i] != EOS; i++)
    {
        if (str[i] == '\t' || str[i] == '\r' || str[i] == '\n')
        {
            str[i] = ' ';
        }
    }
}

static stock bool:IsReservedServerRadioName(const name[])
{
    new trimmed[48], start = 0, len = strlen(name);
    while (start < len && name[start] <= ' ')
    {
        start++;
    }
    while (len > start && name[len - 1] <= ' ')
    {
        len--;
    }
    new out_len = 0;
    for (new i = start; i < len && out_len < sizeof(trimmed) - 1; i++)
    {
        trimmed[out_len++] = name[i];
    }
    trimmed[out_len] = EOS;
    return (out_len > 0 && !strcmp(trimmed, DEMO_SERVER_RADIO_NAME, true));
}

static stock FormatSec(seconds, out[], len = sizeof(out), bool:live_if_zero = true)
{
    if (seconds <= 0)
    {
        format(out, len, live_if_zero ? "LIVE" : "--:--");
        return;
    }
    new mins = seconds / 60;
    new secs = seconds % 60;
    format(out, len, "%02d:%02d", mins, secs);
}

static stock GetStateLabel(E_SA_STREAM_STATE:st, out[], len = sizeof(out))
{
    switch (st)
    {
        case SA_STATE_IDLE:      format(out, len, "Idle");
        case SA_STATE_RESOLVING: format(out, len, "Resolving");
        case SA_STATE_BUFFERING: format(out, len, "Buffering");
        case SA_STATE_PLAYING:   format(out, len, "Playing");
        case SA_STATE_PAUSED:    format(out, len, "Paused");
        case SA_STATE_STOPPED:   format(out, len, "Stopped");
        case SA_STATE_FINISHED:  format(out, len, "Finished");
        case SA_STATE_ERROR:     format(out, len, "Error");
        default:                 format(out, len, "Unknown");
    }
}

static stock GetDestroyLabel(E_SA_CHANNEL_DESTROY_MODE:mode, out[], len = sizeof(out))
{
    switch (mode)
    {
        case SA_CHANNEL_DESTROY_ON_DISCONNECT: format(out, len, "On Disconnect");
        case SA_CHANNEL_DESTROY_ON_TIMEOUT:    format(out, len, "Offline Timeout");
        default:                               format(out, len, "Manual");
    }
}

static stock GetSourceTypeLabel(E_SA_AUDIO_STREAM_SOURCE:stype, out[], len = sizeof(out))
{
    switch (stype)
    {
        case SA_SOURCE_PERSONAL:      format(out, len, "Personal");
        case SA_SOURCE_CHANNEL:       format(out, len, "Channel");
        case SA_SOURCE_GLOBAL:        format(out, len, "Global");
        case SA_SOURCE_CHANNEL_RANGE: format(out, len, "Channel 3D");
        default:                      format(out, len, "None");
    }
}

static stock HashStringDjb2(const str[])
{
    new h = 5381;
    for (new i = 0; str[i] != EOS; i++)
    {
        h = ((h << 5) + h) + str[i];
    }
    return h;
}

static stock ResetTrackedGlobals()
{
    for (new i = 0; i < DEMO_MAX_TRACKED_GLOBALS; i++)
    {
        gTrackedGlobalSid[i] = INVALID_AUDIO_STREAM_ID;
        gTrackedGlobalOwner[i] = INVALID_PLAYER_ID;
    }
}

static stock bool:TrackGlobalStream(streamid, ownerid)
{
    if (streamid == INVALID_AUDIO_STREAM_ID || ownerid == INVALID_PLAYER_ID)
    {
        return false;
    }
    new free_slot = -1;
    new title[8], artist[8], dur, E_SA_STREAM_STATE:st;
    for (new i = 0; i < DEMO_MAX_TRACKED_GLOBALS; i++)
    {
        if (gTrackedGlobalSid[i] == streamid)
        {
            gTrackedGlobalOwner[i] = ownerid;
            return true;
        }
        if (gTrackedGlobalSid[i] == INVALID_AUDIO_STREAM_ID)
        {
            if (free_slot == -1) free_slot = i;
        }
        else if (free_slot == -1 && !SA_GetAudioStreamInfo(gTrackedGlobalSid[i], title, sizeof(title), artist, sizeof(artist), dur, st))
        {
            // Reclaim stale slot whose stream no longer exists
            free_slot = i;
        }
    }
    if (free_slot != -1)
    {
        gTrackedGlobalSid[free_slot] = streamid;
        gTrackedGlobalOwner[free_slot] = ownerid;
        return true;
    }
    return false;
}

static stock UntrackGlobalStream(streamid)
{
    if (streamid == INVALID_AUDIO_STREAM_ID)
    {
        return INVALID_PLAYER_ID;
    }
    for (new i = 0; i < DEMO_MAX_TRACKED_GLOBALS; i++)
    {
        if (gTrackedGlobalSid[i] == streamid)
        {
            new owner = gTrackedGlobalOwner[i];
            gTrackedGlobalSid[i] = INVALID_AUDIO_STREAM_ID;
            gTrackedGlobalOwner[i] = INVALID_PLAYER_ID;
            return owner;
        }
    }
    return INVALID_PLAYER_ID;
}

static stock GetTrackedGlobalOwner(streamid)
{
    if (streamid == INVALID_AUDIO_STREAM_ID)
    {
        return INVALID_PLAYER_ID;
    }
    for (new i = 0; i < DEMO_MAX_TRACKED_GLOBALS; i++)
    {
        if (gTrackedGlobalSid[i] == streamid)
        {
            return gTrackedGlobalOwner[i];
        }
    }
    return INVALID_PLAYER_ID;
}

static stock UntrackGlobalsForPlayer(playerid)
{
    if (playerid == INVALID_PLAYER_ID)
    {
        return;
    }
    for (new i = 0; i < DEMO_MAX_TRACKED_GLOBALS; i++)
    {
        if (gTrackedGlobalOwner[i] == playerid)
        {
            gTrackedGlobalSid[i] = INVALID_AUDIO_STREAM_ID;
            gTrackedGlobalOwner[i] = INVALID_PLAYER_ID;
        }
    }
}

static stock ClearGlobalSidForAll(streamid)
{
    if (streamid == INVALID_AUDIO_STREAM_ID)
    {
        return;
    }
    for (new p = 0; p < MAX_PLAYERS; p++)
    {
        if (gLastGlobalSid[p] == streamid)
        {
            gLastGlobalSid[p] = INVALID_AUDIO_STREAM_ID;
        }
    }
}

static stock bool:CanPlayerControlStream(playerid, sid)
{
    if (sid == INVALID_AUDIO_STREAM_ID)
    {
        return true;
    }
    new owner = SA_GetAudioStreamOwner(sid);
    if (owner != INVALID_PLAYER_ID)
    {
        return (owner == playerid);
    }
    new tracked_owner = GetTrackedGlobalOwner(sid);
    if (tracked_owner != INVALID_PLAYER_ID)
    {
        return (tracked_owner == playerid);
    }
    return true;
}

static stock bool:EnsureStreamControlPermission(playerid, sid)
{
    if (!CanPlayerControlStream(playerid, sid))
    {
        new msg[80];
        format(msg, sizeof(msg), "[Stream] Stream #%d belongs to another player.", sid);
        SendClientMessage(playerid, 0xFF5555FF, msg);
        return false;
    }
    return true;
}

static stock ResetPlayerDemoState(playerid)
{
    gSearchId[playerid] = INVALID_AUDIO_SEARCH_ID;
    gSearchTotal[playerid] = 0;
    gSearchSelIdx[playerid] = 0;
    gLastGlobalSid[playerid] = INVALID_AUDIO_STREAM_ID;
    gSeekTargetSid[playerid] = INVALID_AUDIO_STREAM_ID;
    gOptNotify[playerid] = true;
    gOptProcessFiles[playerid] = false;
    gSelChannelId[playerid] = gServerRadioId;
    gChListCount[playerid] = 0;
    gQueueViewCount[playerid] = 0;
    gNewChName[playerid][0] = EOS;
    gNewChPass[playerid][0] = EOS;
}

// -----------------------------------------------------------------------------
// Dialog Builders
// -----------------------------------------------------------------------------

static stock ShowSaMainMenu(playerid)
{
    new sid = SA_GetPlayerAudioStreamID(playerid);
    new cid = SA_GetPlayerAudioStreamChannel(playerid);
    new ch_count = SA_GetAudioStreamChannelCount();
    new bool:muted = SA_IsPlayerAudioStreamMuted(playerid);
    new E_SA_AUDIO_STREAM_SOURCE:src_type, src_id, src_lbl[16];
    SA_GetPlayerAudioStreamSource(playerid, src_type, src_id);
    GetSourceTypeLabel(src_type, src_lbl, sizeof(src_lbl));
    new body[1024];

    format(body, sizeof(body),
        "{00FF88}1. Personal Playback Controls {AAAAAA}(Active Stream: %d)\n\
{00CCFF}2. Global & 3D Spatial Streams {AAAAAA}(Last Created: %d)\n\
{FFCC00}3. Search Music by Name {AAAAAA}(Async YouTube Search)\n\
{FF88FF}4. Synchronized Radio Channels {AAAAAA}(Channels: %d | Joined: %d)\n\
{FFFFFF}5. Vanilla Hook Demo {AAAAAA}(Hooked PlayAudioStreamForPlayer)\n\
{FFFFFF}6. Toggle: OnAudioStreamProcessing Callback [%s{FFFFFF}]\n\
{FFFFFF}7. Toggle: Process Direct Audio Files [%s{FFFFFF}]\n\
{FFFFFF}8. Toggle: Mute Streams (SA_SetPlayerAudioStreamMuted) [%s{FFFFFF}]\n\
{CCCCCC}9. Plugin Diagnostics & Stats {AAAAAA}(Heard: %s #%d)",
        sid,
        gLastGlobalSid[playerid],
        ch_count,
        cid,
        gOptNotify[playerid] ? "{00FF88}ON" : "{FF5555}OFF",
        gOptProcessFiles[playerid] ? "{00FF88}TRUE" : "{FFAA00}FALSE",
        muted ? "{FF5555}MUTED" : "{00FF88}UNMUTED",
        src_lbl,
        src_id
    );

    new title[64];
    format(title, sizeof(title), "{00CCFF}SampAudioStream v%s - Control Center", SAMPAUDIOSTREAM_VERSION_STRING);
    ShowPlayerDialog(playerid, DLG_SA_MAIN, DIALOG_STYLE_LIST,
        title, body, "Select", "Close");
}

static stock ShowPersonalMenu(playerid)
{
    new sid = SA_GetPlayerAudioStreamID(playerid);
    new t[64], a[48], dur, E_SA_STREAM_STATE:st = SA_STATE_IDLE;
    new bool:is_paused = (sid != INVALID_AUDIO_STREAM_ID &&
        SA_GetAudioStreamInfo(sid, t, sizeof(t), a, sizeof(a), dur, st) &&
        st == SA_STATE_PAUSED);
    new header[768];
    format(header, sizeof(header),
        "Play URL or Query {AAAAAA}(SA_PlayAudioStreamForPlayer)\n\
%s Personal Stream {AAAAAA}(SA_PauseAudioStreamForPlayer)\n\
Set Personal Volume 0-200%% {AAAAAA}(SA_SetPlayerAudioStreamVolume)\n\
Set Personal Range Distance {AAAAAA}(SA_SetPlayerAudioStreamDistance)\n\
Move Personal Stream Here (3D) {AAAAAA}(SA_SetPlayerAudioStreamPos)\n\
Stop Personal Stream {AAAAAA}(SA_StopAudioStreamForPlayer)\n\
Inspect Personal Stream Info {AAAAAA}(SA_GetAudioStreamInfo) [ID: %d]",
        is_paused ? "Resume" : "Pause",
        sid
    );
    ShowPlayerDialog(playerid, DLG_SA_PERSONAL_MENU, DIALOG_STYLE_LIST,
        "{00FF88}Personal Playback Controls", header, "Select", "Back");
}

static stock ShowGlobalMenu(playerid)
{
    new body[1024];
    format(body, sizeof(body),
        "Play Global 3D Stream Here {AAAAAA}(SA_PlayAudioStream usepos=true)\n\
Play Global 2D Stream {AAAAAA}(SA_PlayAudioStream usepos=false)\n\
Move Global Stream Here (3D) {AAAAAA}(SA_SetAudioStreamPos) [ID: %d]\n\
Set Global Stream Distance {AAAAAA}(SA_SetAudioStreamDistance) [ID: %d]\n\
Set Global Stream Volume {AAAAAA}(SA_SetAudioStreamVolume) [ID: %d]\n\
Attach Global Stream to My Vehicle {AAAAAA}(SA_AttachAudioStreamToVehicle)\n\
Attach Global Stream to a Player {AAAAAA}(SA_AttachAudioStreamToPlayer)\n\
Pause / Resume Any Stream by ID {AAAAAA}(SA_PauseAudioStream)\n\
Seek Stream to Seconds by ID {AAAAAA}(SA_SeekAudioStream)\n\
Inspect Stream Info by ID {AAAAAA}(SA_GetAudioStreamInfo)\n\
Stop Global Stream by ID {AAAAAA}(SA_StopAudioStream)",
        gLastGlobalSid[playerid],
        gLastGlobalSid[playerid],
        gLastGlobalSid[playerid]
    );
    ShowPlayerDialog(playerid, DLG_SA_GLOBAL_MENU, DIALOG_STYLE_LIST,
        "{00CCFF}Global & 3D Spatial Controls", body, "Select", "Back");
}

static stock ShowChannelListMenu(playerid)
{
    new body[2048];
    new active_cid = SA_GetPlayerAudioStreamChannel(playerid);
    new total = SA_GetAudioStreamChannelCount();

    format(body, sizeof(body), "{00FF88}+ Create New Synchronized Channel {AAAAAA}(SA_CreateAudioStreamChannel)\n");
    if (active_cid != INVALID_AUDIO_CHANNEL_ID)
    {
        strcat(body, "{FFAA00}- Leave Current Channel {AAAAAA}(SA_LeaveAudioStreamChannel)\n");
    }
    else
    {
        strcat(body, "{666666}- Not currently tuned into any channel\n");
    }

    gChListCount[playerid] = 0;
    for (new i = 0; i < total; i++)
    {
        if (gChListCount[playerid] >= DEMO_MAX_LISTED_CHANNELS) break;
        new cid = SA_GetAudioStreamChannelID(i);
        if (cid == INVALID_AUDIO_CHANNEL_ID) continue;

        new cname[40], ccreator[32], listeners, E_SA_STREAM_STATE:st, bool:locked, qcount;
        if (!SA_GetAudioStreamChannelInfo(cid, cname, sizeof(cname), ccreator, sizeof(ccreator), listeners, st, locked, qcount))
        {
            continue;
        }
        SanitizeDialogField(cname);
        SanitizeDialogField(ccreator);

        new st_lbl[16], line[224];
        GetStateLabel(st, st_lbl, sizeof(st_lbl));
        new bool:joined = SA_IsPlayerInAudioStreamChannel(playerid, cid);

        format(line, sizeof(line), "%s[#%d] %s {AAAAAA}| By: %s | Listeners: %d | %s%s{AAAAAA} | Q:%d | %s\n",
            joined ? "{00FF88}" : "{FFFFFF}",
            cid,
            cname,
            ccreator,
            listeners,
            locked ? "{FFAA00}[PASS]" : "[OPEN]",
            joined ? " {00FF88}[TUNED]" : "",
            qcount,
            st_lbl
        );
        if (strlen(body) + strlen(line) >= sizeof(body)) break;
        strcat(body, line);
        gChListIds[playerid][gChListCount[playerid]++] = cid;
    }

    ShowPlayerDialog(playerid, DLG_SA_CH_LIST, DIALOG_STYLE_LIST,
        "{FF88FF}Synchronized Radio Channels", body, "Select", "Back");
}

static stock ShowChannelManageMenu(playerid, channelid)
{
    new cname[40], ccreator[32], listeners, E_SA_STREAM_STATE:st, bool:locked, qcount;
    if (!SA_GetAudioStreamChannelInfo(channelid, cname, sizeof(cname), ccreator, sizeof(ccreator), listeners, st, locked, qcount))
    {
        SendClientMessage(playerid, 0xFFAA00FF, "[Channel] That channel no longer exists.");
        ShowChannelListMenu(playerid);
        return;
    }

    gSelChannelId[playerid] = channelid;
    new bool:joined = SA_IsPlayerInAudioStreamChannel(playerid, channelid);
    new E_SA_CHANNEL_DESTROY_MODE:dmode, dtimeout, drem, bool:cre_online;
    SA_GetAudioStreamChannelDestroy(channelid, dmode, dtimeout, drem, cre_online);

    new track_t[48], track_a[36], track_dur;
    SA_GetAudioStreamChannelTrack(channelid, track_t, sizeof(track_t), track_a, sizeof(track_a), track_dur);
    SanitizeDialogField(cname);
    SanitizeDialogField(ccreator);
    SanitizeDialogField(track_t);
    SanitizeDialogField(track_a);
    new elapsed = SA_GetAudioStreamChannelElapsed(channelid);
    new E_SA_CHANNEL_LOOP_MODE:lmode = SA_GetAudioStreamChannelLoop(channelid);
    new loop_lbl[12];
    switch (lmode)
    {
        case SA_CHANNEL_LOOP_TRACK: format(loop_lbl, sizeof(loop_lbl), "Track");
        case SA_CHANNEL_LOOP_QUEUE: format(loop_lbl, sizeof(loop_lbl), "Queue");
        default: format(loop_lbl, sizeof(loop_lbl), "Off");
    }

    new st_lbl[16], d_lbl[20], timeout_note[48];
    GetStateLabel(st, st_lbl, sizeof(st_lbl));
    GetDestroyLabel(dmode, d_lbl, sizeof(d_lbl));

    if (dmode == SA_CHANNEL_DESTROY_ON_TIMEOUT)
    {
        format(timeout_note, sizeof(timeout_note), cre_online ? " (%ds | Reset: Owner Online)" : " (%ds left)", drem);
    }
    else
    {
        timeout_note[0] = EOS;
    }

    new title[96], body[2048];
    format(title, sizeof(title), "{FF88FF}Channel #%d: %s", channelid, cname);
    format(body, sizeof(body),
        "%s This Channel {AAAAAA}(%s%s)\n\
Play Audio Stream Now {AAAAAA}(SA_PlayChannelAudioStream)\n\
Add URL / Query to Queue {AAAAAA}(SA_QueueChannelAudioStream) [Queued: %d]\n\
Skip to Next Track {AAAAAA}(SA_SkipChannelAudioStream)\n\
%s Channel Audio Stream {AAAAAA}(SA_PauseChannelAudioStream)\n\
Set Channel Stream Volume {AAAAAA}(SA_SetChannelAudioStreamVolume)\n\
Queue Actions: View / Play-Next / Remove / Clear {AAAAAA}([%d])\n\
Cycle Loop Mode {AAAAAA}(SA_SetAudioStreamChannelLoop: %s)\n\
Spatial Position / Attach Vehicle or Player {AAAAAA}(SA_SetAudioStreamChannelPos)\n\
View Subscribed Listeners {AAAAAA}(SA_GetAudioStreamChannelListener: %d)\n\
Rename Channel {AAAAAA}(SA_SetAudioStreamChannelName)\n\
%s Channel Password {AAAAAA}(SA_SetAudioStreamChannelPass)\n\
Configure Auto-Destroy Policy {AAAAAA}(%s%s)\n\
Reclaim Creator Ownership Here {AAAAAA}(By: %s [%s])\n\
{FF5555}Stop Stream & Clear Queue {AAAAAA}(SA_StopChannelAudioStream)\n\
{FF5555}Destroy Channel {AAAAAA}(SA_DestroyAudioStreamChannel)\n\
{CCCCCC}--- %s (%ds) | Listeners: %d | Track: %s%s%s ---",
        joined ? "Leave" : "Join",
        joined ? "SA_LeaveAudioStreamChannel" : "SA_JoinAudioStreamChannel",
        (!joined && locked) ? " - Password Required" : "",
        qcount,
        (st == SA_STATE_PAUSED) ? "Resume" : "Pause",
        qcount,
        loop_lbl,
        listeners,
        locked ? "Change / Remove" : "Set",
        d_lbl,
        timeout_note,
        ccreator,
        cre_online ? "Online" : "Offline",
        st_lbl,
        (elapsed < 0) ? 0 : elapsed,
        listeners,
        (track_a[0] != EOS) ? track_a : "",
        (track_a[0] != EOS && track_t[0] != EOS) ? " - " : "",
        (track_t[0] != EOS) ? track_t : "Nothing playing"
    );

    ShowPlayerDialog(playerid, DLG_SA_CH_MENU, DIALOG_STYLE_LIST, title, body, "Select", "Back");
}

static stock bool:CanPlayerManageChannel(playerid, const creator_name[])
{
    if (creator_name[0] == EOS)
    {
        return false;
    }
    if (!strcmp(creator_name, "Server", true))
    {
        return true;
    }
    new pname[MAX_PLAYER_NAME + 1];
    GetPlayerName(playerid, pname, sizeof(pname));
    return (pname[0] != EOS && !strcmp(pname, creator_name, true));
}

static stock bool:EnsureChannelManagePermission(playerid, channelid, bool:return_to_menu = true)
{
    new cname[40], ccreator[32], listeners, E_SA_STREAM_STATE:st, bool:locked, qcount;
    if (!SA_GetAudioStreamChannelInfo(channelid, cname, sizeof(cname), ccreator, sizeof(ccreator), listeners, st, locked, qcount))
    {
        SendClientMessage(playerid, 0xFFAA00FF, "[Channel] That channel no longer exists.");
        ShowChannelListMenu(playerid);
        return false;
    }
    if (!CanPlayerManageChannel(playerid, ccreator))
    {
        new msg[128];
        SanitizeDialogField(ccreator);
        format(msg, sizeof(msg), "[Channel] This channel belongs to %s; only its creator can do that.", ccreator);
        SendClientMessage(playerid, 0xFF5555FF, msg);
        if (return_to_menu)
        {
            ShowChannelManageMenu(playerid, channelid);
        }
        return false;
    }
    return true;
}

static stock bool:EnsureSelectedChannelExists(playerid, channelid)
{
    new cname[40], ccreator[32], listeners, E_SA_STREAM_STATE:st, bool:locked, qcount;
    if (!SA_GetAudioStreamChannelInfo(channelid, cname, sizeof(cname), ccreator, sizeof(ccreator), listeners, st, locked, qcount))
    {
        SendClientMessage(playerid, 0xFFAA00FF, "[Channel] That channel no longer exists.");
        ShowChannelListMenu(playerid);
        return false;
    }
    return true;
}

static stock ShowChannelQueueDialog(playerid, channelid)
{
    new cname[40], ccreator[32], listeners, E_SA_STREAM_STATE:st, bool:locked, qcount;
    if (!SA_GetAudioStreamChannelInfo(channelid, cname, sizeof(cname), ccreator, sizeof(ccreator), listeners, st, locked, qcount))
    {
        ShowChannelListMenu(playerid);
        return;
    }

    new body[2048], item[96], line[128];
    format(body, sizeof(body),
        "{00FF88}+ Insert URL at Index 0 (Play Next) {AAAAAA}(SA_InsertAudioStreamChannelQueue)\n\
{FF8888}- Remove Queue Item by Index {AAAAAA}(SA_RemoveAudioStreamChannelQueue)\n\
{00CCFF}+ Import Playlist URL {AAAAAA}(SA_QueueChannelAudioStreamPlaylist)\n\
{FFCC00}> Seek Current Track {AAAAAA}(SA_SeekChannelAudioStream)\n\
{FF5555}x Clear All Queued Tracks {AAAAAA}(SA_ClearAudioStreamChannelQueue) [%d]\n",
        qcount);
    gQueueViewCount[playerid] = 0;
    for (new i = 0; i < qcount && i < DEMO_MAX_QUEUE_VIEW_ITEMS; i++)
    {
        if (SA_GetAudioStreamChannelQueue(channelid, i, item, sizeof(item)))
        {
            SanitizeDialogField(item);
            format(line, sizeof(line), "{FFCC00}#%d: {FFFFFF}%s\n", i, item);
            if (strlen(body) + strlen(line) >= sizeof(body)) break;
            strcat(body, line);
            gQueueViewHash[playerid][gQueueViewCount[playerid]] = HashStringDjb2(item);
            gQueueViewCount[playerid]++;
        }
    }

    ShowPlayerDialog(playerid, DLG_SA_CH_QUEUE_VIEW, DIALOG_STYLE_LIST,
        "{FF88FF}Channel Queue Actions", body, "Run", "Back");
}

static stock ShowChannelListenersDialog(playerid, channelid)
{
    new cname[40], ccreator[32], listeners, E_SA_STREAM_STATE:st, bool:locked, qcount;
    if (!SA_GetAudioStreamChannelInfo(channelid, cname, sizeof(cname), ccreator, sizeof(ccreator), listeners, st, locked, qcount))
    {
        ShowChannelListMenu(playerid);
        return;
    }
    SanitizeDialogField(cname);

    new body[1536], line[96], pname[32];
    format(body, sizeof(body), "{FFFFFF}Subscribed listeners on #%d (%s): %d\n\n", channelid, cname, listeners);
    for (new i = 0; i < listeners && i < 15; i++)
    {
        new lpid = SA_GetAudioStreamChannelListener(channelid, i);
        if (lpid != INVALID_PLAYER_ID)
        {
            GetPlayerName(lpid, pname, sizeof(pname));
            SanitizeDialogField(pname);
            format(line, sizeof(line), "{FFCC00}#%d: {FFFFFF}%s (ID %d)\n", i + 1, pname, lpid);
            strcat(body, line);
        }
    }
    if (listeners == 0)
    {
        strcat(body, "{AAAAAA}(No explicitly joined listeners; range-only auto_listen players excluded)");
    }
    ShowPlayerDialog(playerid, DLG_SA_CH_LISTENERS, DIALOG_STYLE_MSGBOX,
        "{FF88FF}Channel Subscribers", body, "Back", "");
}

static stock ShowDiagnosticsDialog(playerid)
{
    new sid = SA_GetPlayerAudioStreamID(playerid);
    new cid = SA_GetPlayerAudioStreamChannel(playerid);
    new bool:ready = SA_IsAudioStreamExtractorReady();
    new port = SA_GetAudioStreamHttpPort();
    new st_streams, st_channels, st_transcoders, st_listeners;
    SA_GetAudioStreamStats(st_streams, st_channels, st_transcoders, st_listeners);

    new E_SA_AUDIO_STREAM_SOURCE:src_type, src_id, src_lbl[16];
    SA_GetPlayerAudioStreamSource(playerid, src_type, src_id);
    GetSourceTypeLabel(src_type, src_lbl, sizeof(src_lbl));

    new ptitle[64], partist[48], pdur, E_SA_STREAM_STATE:pst, st_label[16], pelapsed = -1, purl[64];
    if (sid != INVALID_AUDIO_STREAM_ID && SA_GetAudioStreamInfo(sid, ptitle, sizeof(ptitle), partist, sizeof(partist), pdur, pst))
    {
        GetStateLabel(pst, st_label, sizeof(st_label));
        pelapsed = SA_GetAudioStreamElapsed(sid);
        SA_GetAudioStreamURL(sid, purl, sizeof(purl));
    }
    else
    {
        format(ptitle, sizeof(ptitle), "None");
        format(partist, sizeof(partist), "-");
        format(st_label, sizeof(st_label), "Idle");
        format(purl, sizeof(purl), "-");
        pdur = 0;
    }

    new gtitle[64], gartist[48], gdur, E_SA_STREAM_STATE:gst, gst_label[16];
    if (gLastGlobalSid[playerid] != INVALID_AUDIO_STREAM_ID &&
        SA_GetAudioStreamInfo(gLastGlobalSid[playerid], gtitle, sizeof(gtitle), gartist, sizeof(gartist), gdur, gst))
    {
        GetStateLabel(gst, gst_label, sizeof(gst_label));
    }
    else
    {
        format(gtitle, sizeof(gtitle), "None");
        format(gartist, sizeof(gartist), "-");
        format(gst_label, sizeof(gst_label), "Idle");
        gdur = 0;
    }

    new first_sid = (SA_GetAudioStreamCount() > 0) ? SA_GetAudioStreamID(0) : INVALID_AUDIO_STREAM_ID;
    new first_owner = (first_sid != INVALID_AUDIO_STREAM_ID) ? SA_GetAudioStreamOwner(first_sid) : INVALID_PLAYER_ID;
    SanitizeDialogField(ptitle);
    SanitizeDialogField(partist);
    SanitizeDialogField(purl);
    SanitizeDialogField(gtitle);
    SanitizeDialogField(gartist);

    new info[1536];
    format(info, sizeof(info),
        "{00CCFF}SampAudioStream Version:\t{FFFFFF}v%s\n\
{00CCFF}Extractor Ready (yt-dlp + JS runtime):\t%s\n\
{00CCFF}HTTP Streaming Port:\t{FFFFFF}%d\n\
{00CCFF}Stats (Strm/Ch/Trans/Lst):\t{FFFFFF}%d / %d / %d / %d\n\
{00CCFF}Your Muted / Heard Source:\t{FFFFFF}%s / %s (#%d)\n\
{00CCFF}Active Stream #0 / Owner:\t{FFFFFF}%d / %d | Joined Ch: #%d\n\
{00FF88}Personal Stream ID:\t{FFFFFF}%d (%s | %ds)\n\
{00FF88}Personal Track / URL:\t{FFFFFF}%s - %s (%ds) [%s]\n\
{FFCC00}Last Global Stream ID:\t{FFFFFF}%d (%s)\n\
{FFCC00}Global Track:\t{FFFFFF}%s - %s (%ds)",
        SAMPAUDIOSTREAM_VERSION_STRING,
        ready ? "{00FF88}YES" : "{FFAA00}Downloading...",
        port,
        st_streams, st_channels, st_transcoders, st_listeners,
        SA_IsPlayerAudioStreamMuted(playerid) ? "YES" : "NO",
        src_lbl, src_id,
        first_sid, first_owner, cid,
        sid, st_label, (pelapsed < 0) ? 0 : pelapsed,
        partist, ptitle, pdur, purl,
        gLastGlobalSid[playerid], gst_label,
        gartist, gtitle, gdur
    );

    ShowPlayerDialog(playerid, DLG_SA_DIAGNOSTICS, DIALOG_STYLE_TABLIST,
        "{00CCFF}Plugin Diagnostics", info, "Refresh", "Back");
}

// -----------------------------------------------------------------------------
// FilterScript Lifecycle & Commands
// -----------------------------------------------------------------------------

public OnFilterScriptInit()
{
    print("============================================================");
    printf("  FS_SampAudioStream_Demo loaded (Include v%s)", SAMPAUDIOSTREAM_VERSION_STRING);
    print("  Commands: /audiomenu or /sa");
    print("============================================================");

    new keep_cid = INVALID_AUDIO_CHANNEL_ID;
    new total = SA_GetAudioStreamChannelCount();
    new cname[40], ccreator[32], listeners, E_SA_STREAM_STATE:st, bool:locked, qcount;

    for (new i = 0; i < total; i++)
    {
        new cid = SA_GetAudioStreamChannelID(i);
        if (cid == INVALID_AUDIO_CHANNEL_ID) continue;
        if (!SA_GetAudioStreamChannelInfo(cid, cname, sizeof(cname), ccreator, sizeof(ccreator), listeners, st, locked, qcount))
        {
            continue;
        }
        if (cname[0] != EOS && ccreator[0] != EOS &&
            !strcmp(cname, DEMO_SERVER_RADIO_NAME, false) && !strcmp(ccreator, "Server", false))
        {
            if (keep_cid == INVALID_AUDIO_CHANNEL_ID || cid < keep_cid)
            {
                keep_cid = cid;
            }
        }
    }

    if (keep_cid != INVALID_AUDIO_CHANNEL_ID)
    {
        new removed = 0;
        for (new i = SA_GetAudioStreamChannelCount() - 1; i >= 0; i--)
        {
            new cid = SA_GetAudioStreamChannelID(i);
            if (cid == INVALID_AUDIO_CHANNEL_ID || cid == keep_cid) continue;
            if (!SA_GetAudioStreamChannelInfo(cid, cname, sizeof(cname), ccreator, sizeof(ccreator), listeners, st, locked, qcount))
            {
                continue;
            }
            if (cname[0] != EOS && ccreator[0] != EOS &&
                !strcmp(cname, DEMO_SERVER_RADIO_NAME, false) && !strcmp(ccreator, "Server", false))
            {
                if (SA_DestroyAudioStreamChannel(cid))
                {
                    removed++;
                }
            }
        }
        if (removed > 0)
        {
            printf("[SampAudioStream] Removed %d duplicate '%s' server channel(s); kept #%d.",
                removed, DEMO_SERVER_RADIO_NAME, keep_cid);
        }
        gServerRadioId = keep_cid;
        printf("[SampAudioStream] Reusing existing '%s' channel #%d.", DEMO_SERVER_RADIO_NAME, gServerRadioId);
    }
    else
    {
        gServerRadioId = SA_CreateAudioStreamChannel(DEMO_SERVER_RADIO_NAME);
    }

    for (new p = 0; p < MAX_PLAYERS; p++)
    {
        ResetPlayerDemoState(p);
    }
    ResetTrackedGlobals();
    return 1;
}

public OnFilterScriptExit()
{
    gServerRadioId = INVALID_AUDIO_CHANNEL_ID;
    ResetTrackedGlobals();
    for (new p = 0; p < MAX_PLAYERS; p++)
    {
        ResetPlayerDemoState(p);
    }
    return 1;
}

public OnPlayerConnect(playerid)
{
    ResetPlayerDemoState(playerid);
    SendClientMessage(playerid, 0x00CCFFFF,
        "[SampAudioStream] Demo FS ready! Type /audiomenu or /sa to open the control menu.");
    return 1;
}

public OnPlayerDisconnect(playerid, reason)
{
    if (gSearchId[playerid] != INVALID_AUDIO_SEARCH_ID)
    {
        SA_CancelAudioStreamSearch(playerid);
    }
    UntrackGlobalsForPlayer(playerid);
    ResetPlayerDemoState(playerid);
    return 1;
}

public OnPlayerCommandText(playerid, cmdtext[])
{
    if (!strcmp(cmdtext, "/audiomenu", true) || !strcmp(cmdtext, "/sa", true))
    {
        ShowSaMainMenu(playerid);
        return 1;
    }
    return 0;
}

// -----------------------------------------------------------------------------
// Dialog Interaction Handler
// -----------------------------------------------------------------------------

public OnDialogResponse(playerid, dialogid, response, listitem, inputtext[])
{
    if (dialogid < DEMO_DLG_BASE || dialogid > DLG_SA_DIAGNOSTICS)
    {
        return 0;
    }

    switch (dialogid)
    {
        case DLG_SA_MAIN:
        {
            if (!response) return 1;
            switch (listitem)
            {
                case 0: ShowPersonalMenu(playerid);
                case 1: ShowGlobalMenu(playerid);
                case 2:
                {
                    if (gSearchId[playerid] != INVALID_AUDIO_SEARCH_ID)
                    {
                        SA_CancelAudioStreamSearch(playerid);
                        gSearchId[playerid] = INVALID_AUDIO_SEARCH_ID;
                        SendClientMessage(playerid, 0xFFAA00FF,
                            "[Search] Previous pending search cancelled.");
                    }
                    ShowPlayerDialog(playerid, DLG_SA_SEARCH_INPUT, DIALOG_STYLE_INPUT,
                        "{FFCC00}Search Music by Name",
                        "{FFFFFF}Enter a song name, artist, or keyword to search:\n\n{AAAAAA}Example: Daft Punk Around the World",
                        "Search", "Back");
                }
                case 3: ShowChannelListMenu(playerid);
                case 4:
                {
                    ShowPlayerDialog(playerid, DLG_SA_VANILLA_PLAY, DIALOG_STYLE_INPUT,
                        "{FFFFFF}Hooked PlayAudioStreamForPlayer Demo",
                        "{FFFFFF}Calls standard PlayAudioStreamForPlayer with extended plugin volume=80.0:\n\n{AAAAAA}Enter a YouTube URL, direct MP3/radio link, or song name:",
                        "Play", "Back");
                }
                case 5:
                {
                    gOptNotify[playerid] = !gOptNotify[playerid];
                    SendClientMessage(playerid, 0x00CCFFFF,
                        gOptNotify[playerid] ?
                        "[Options] OnAudioStreamProcessing callback ENABLED for your plays." :
                        "[Options] OnAudioStreamProcessing callback DISABLED (.notify_processing = false).");
                    ShowSaMainMenu(playerid);
                }
                case 6:
                {
                    gOptProcessFiles[playerid] = !gOptProcessFiles[playerid];
                    SendClientMessage(playerid, 0x00CCFFFF,
                        gOptProcessFiles[playerid] ?
                        "[Options] Direct audio files (.mp3/.ogg/.wav/.flac/.aac/.m4a/.opus) will be TRANSCODED by plugin." :
                        "[Options] Direct audio files use zero-slot passthrough (default).");
                    ShowSaMainMenu(playerid);
                }
                case 7:
                {
                    new bool:next_mute = !SA_IsPlayerAudioStreamMuted(playerid);
                    SA_SetPlayerAudioStreamMuted(playerid, next_mute, false);
                    SendClientMessage(playerid, 0x00CCFFFF,
                        next_mute ?
                        "[Mute] Channel/global/range audio streams MUTED for you." :
                        "[Mute] Audio streams UNMUTED.");
                    ShowSaMainMenu(playerid);
                }
                case 8: ShowDiagnosticsDialog(playerid);
            }
            return 1;
        }

        // ---------------- Personal Controls ----------------
        case DLG_SA_PERSONAL_MENU:
        {
            if (!response)
            {
                ShowSaMainMenu(playerid);
                return 1;
            }
            switch (listitem)
            {
                case 0:
                {
                    ShowPlayerDialog(playerid, DLG_SA_PERSONAL_PLAY, DIALOG_STYLE_INPUT,
                        "{00FF88}Personal Play (SA_PlayAudioStreamForPlayer)",
                        "{FFFFFF}Enter a URL (YouTube, SoundCloud, Twitch, MP3, radio) or song query:",
                        "Play", "Back");
                }
                case 1:
                {
                    new sid = SA_GetPlayerAudioStreamID(playerid);
                    new t[64], a[48], dur, E_SA_STREAM_STATE:st = SA_STATE_IDLE;
                    if (sid == INVALID_AUDIO_STREAM_ID ||
                        !SA_GetAudioStreamInfo(sid, t, sizeof(t), a, sizeof(a), dur, st))
                    {
                        SendClientMessage(playerid, 0xFF5555FF, "[Personal] No active personal stream to pause/resume.");
                    }
                    else
                    {
                        new bool:next_pause = (st != SA_STATE_PAUSED);
                        if (!SA_PauseAudioStreamForPlayer(playerid, next_pause))
                        {
                            SendClientMessage(playerid, 0xFF5555FF, "[Personal] No active personal stream to pause/resume.");
                        }
                        else
                        {
                            SendClientMessage(playerid, 0x00FF88FF,
                                next_pause ? "[Personal] Stream paused." : "[Personal] Stream resumed.");
                        }
                    }
                    ShowPersonalMenu(playerid);
                }
                case 2:
                {
                    ShowPlayerDialog(playerid, DLG_SA_PERSONAL_VOL, DIALOG_STYLE_INPUT,
                        "{00FF88}Set Personal Volume",
                        "{FFFFFF}Enter volume percentage (0.0 to 200.0; direct files need Process Direct Audio Files enabled):",
                        "Apply", "Back");
                }
                case 3:
                {
                    ShowPlayerDialog(playerid, DLG_SA_PERSONAL_DIST, DIALOG_STYLE_INPUT,
                        "{00FF88}Set Personal Hearing Distance",
                        "{FFFFFF}Enter new radius distance in meters (e.g. 35.0):",
                        "Apply", "Back");
                }
                case 4:
                {
                    new Float:x, Float:y, Float:z;
                    GetPlayerPos(playerid, x, y, z);
                    if (SA_SetPlayerAudioStreamPos(playerid, x, y, z))
                    {
                        SendClientMessage(playerid, 0x00FF88FF,
                            "[Personal] Personal stream moved here (usepos=true enabled).");
                    }
                    else
                    {
                        SendClientMessage(playerid, 0xFF5555FF, "[Personal] No active personal stream found.");
                    }
                    ShowPersonalMenu(playerid);
                }
                case 5:
                {
                    if (SA_StopAudioStreamForPlayer(playerid))
                    {
                        SendClientMessage(playerid, 0x00FF88FF, "[Personal] Personal stream stopped.");
                    }
                    else
                    {
                        SendClientMessage(playerid, 0xFFAA00FF, "[Personal] No active personal stream was running.");
                    }
                    ShowPersonalMenu(playerid);
                }
                case 6: ShowDiagnosticsDialog(playerid);
            }
            return 1;
        }

        case DLG_SA_PERSONAL_PLAY:
        {
            if (!response || inputtext[0] == EOS)
            {
                ShowPersonalMenu(playerid);
                return 1;
            }
            new sid = SA_PlayAudioStreamForPlayer(
                playerid,
                inputtext,
                .notify_processing = gOptNotify[playerid],
                .process_direct_audio_files = gOptProcessFiles[playerid]
            );
            if (sid == INVALID_AUDIO_STREAM_ID)
            {
                SendClientMessage(playerid, 0xFF5555FF, "[Personal] Failed to start stream.");
            }
            else
            {
                new msg[96];
                format(msg, sizeof(msg), "[Personal] Request queued (Stream ID: %d).", sid);
                SendClientMessage(playerid, 0x00FF88FF, msg);
            }
            ShowPersonalMenu(playerid);
            return 1;
        }

        case DLG_SA_PERSONAL_VOL:
        {
            if (!response)
            {
                ShowPersonalMenu(playerid);
                return 1;
            }
            new Float:vol = floatstr(inputtext);
            if (SA_SetPlayerAudioStreamVolume(playerid, vol))
            {
                new msg[96];
                format(msg, sizeof(msg), "[Personal] Volume set to %.1f%%.", vol);
                SendClientMessage(playerid, 0x00FF88FF, msg);
            }
            else
            {
                SendClientMessage(playerid, 0xFF5555FF, "[Personal] Failed: no active transcoded stream (passthrough streams do not support volume changes).");
            }
            ShowPersonalMenu(playerid);
            return 1;
        }

        case DLG_SA_PERSONAL_DIST:
        {
            if (!response)
            {
                ShowPersonalMenu(playerid);
                return 1;
            }
            new Float:dist = floatstr(inputtext);
            if (SA_SetPlayerAudioStreamDistance(playerid, dist))
            {
                new msg[96];
                format(msg, sizeof(msg), "[Personal] Distance updated to %.1fm.", dist);
                SendClientMessage(playerid, 0x00FF88FF, msg);
            }
            else
            {
                SendClientMessage(playerid, 0xFF5555FF, "[Personal] Failed (invalid distance or no active stream).");
            }
            ShowPersonalMenu(playerid);
            return 1;
        }

        // ---------------- Global / Spatial Controls ----------------
        case DLG_SA_GLOBAL_MENU:
        {
            if (!response)
            {
                ShowSaMainMenu(playerid);
                return 1;
            }
            switch (listitem)
            {
                case 0:
                {
                    ShowPlayerDialog(playerid, DLG_SA_GLOBAL_PLAY_3D, DIALOG_STYLE_INPUT,
                        "{00CCFF}Play Global 3D Stream Here",
                        "{FFFFFF}Starts a 45m spatial zone stream at your position/VW/interior (latecomers sync to live timestamp):\n\n{AAAAAA}Enter URL or search query:",
                        "Start", "Back");
                }
                case 1:
                {
                    ShowPlayerDialog(playerid, DLG_SA_GLOBAL_PLAY_2D, DIALOG_STYLE_INPUT,
                        "{00CCFF}Play Global 2D Stream",
                        "{FFFFFF}Starts a map-wide stream (usepos=false) matching your Virtual World/Interior:\n\n{AAAAAA}Enter URL or search query:",
                        "Start", "Back");
                }
                case 2:
                {
                    new Float:x, Float:y, Float:z;
                    GetPlayerPos(playerid, x, y, z);
                    if (gLastGlobalSid[playerid] != INVALID_AUDIO_STREAM_ID)
                    {
                        if (!EnsureStreamControlPermission(playerid, gLastGlobalSid[playerid]))
                        {
                            ShowGlobalMenu(playerid);
                            return 1;
                        }
                        if (SA_SetAudioStreamPos(gLastGlobalSid[playerid], x, y, z))
                        {
                            new msg[96];
                            format(msg, sizeof(msg), "[Global] Stream #%d moved here (usepos=true enabled).", gLastGlobalSid[playerid]);
                            SendClientMessage(playerid, 0x00FF88FF, msg);
                        }
                        else
                        {
                            SendClientMessage(playerid, 0xFF5555FF, "[Global] No active global stream found.");
                        }
                    }
                    else
                    {
                        SendClientMessage(playerid, 0xFF5555FF, "[Global] No active global stream found.");
                    }
                    ShowGlobalMenu(playerid);
                }
                case 3:
                {
                    if (gLastGlobalSid[playerid] != INVALID_AUDIO_STREAM_ID &&
                        !EnsureStreamControlPermission(playerid, gLastGlobalSid[playerid]))
                    {
                        ShowGlobalMenu(playerid);
                        return 1;
                    }
                    ShowPlayerDialog(playerid, DLG_SA_GLOBAL_DIST, DIALOG_STYLE_INPUT,
                        "{00CCFF}Set Global Stream Distance",
                        "{FFFFFF}Enter new radius in meters for your last global stream:",
                        "Apply", "Back");
                }
                case 4:
                {
                    if (gLastGlobalSid[playerid] != INVALID_AUDIO_STREAM_ID &&
                        !EnsureStreamControlPermission(playerid, gLastGlobalSid[playerid]))
                    {
                        ShowGlobalMenu(playerid);
                        return 1;
                    }
                    ShowPlayerDialog(playerid, DLG_SA_GLOBAL_VOL, DIALOG_STYLE_INPUT,
                        "{00CCFF}Set Global Stream Volume",
                        "{FFFFFF}Enter volume percentage (0.0 - 200.0) for your last global stream:",
                        "Apply", "Back");
                }
                case 5:
                {
                    new vehid = GetPlayerVehicleID(playerid);
                    if (vehid == 0)
                    {
                        SendClientMessage(playerid, 0xFFAA00FF, "[Global] Enter a vehicle first to attach a stream to it.");
                    }
                    else if (gLastGlobalSid[playerid] != INVALID_AUDIO_STREAM_ID)
                    {
                        if (!EnsureStreamControlPermission(playerid, gLastGlobalSid[playerid]))
                        {
                            ShowGlobalMenu(playerid);
                            return 1;
                        }
                        if (SA_AttachAudioStreamToVehicle(gLastGlobalSid[playerid], vehid))
                        {
                            new msg[128];
                            format(msg, sizeof(msg),
                                "[Global] Stream #%d attached to vehicle %d (2D full volume inside range; the client cannot move 3D audio).",
                                gLastGlobalSid[playerid], vehid);
                            SendClientMessage(playerid, 0x00FF88FF, msg);
                        }
                        else
                        {
                            SendClientMessage(playerid, 0xFF5555FF, "[Global] Failed to attach (no active stream or invalid vehicle).");
                        }
                    }
                    else
                    {
                        SendClientMessage(playerid, 0xFF5555FF, "[Global] Failed to attach (no active stream or invalid vehicle).");
                    }
                    ShowGlobalMenu(playerid);
                }
                case 6:
                {
                    if (gLastGlobalSid[playerid] != INVALID_AUDIO_STREAM_ID &&
                        !EnsureStreamControlPermission(playerid, gLastGlobalSid[playerid]))
                    {
                        ShowGlobalMenu(playerid);
                        return 1;
                    }
                    ShowPlayerDialog(playerid, DLG_SA_GLOBAL_ATTACH_PLY, DIALOG_STYLE_INPUT,
                        "{00CCFF}Attach Stream to Player",
                        "{FFFFFF}Enter target Player ID to attach your last global stream to:",
                        "Attach", "Back");
                }
                case 7:
                {
                    ShowPlayerDialog(playerid, DLG_SA_GLOBAL_PAUSE_ID, DIALOG_STYLE_INPUT,
                        "{00CCFF}Pause / Resume Stream by ID",
                        "{FFFFFF}Enter Stream ID and '1' or '0' separated by space (e.g. '2 1' to pause, '2 0' to resume):",
                        "Apply", "Back");
                }
                case 8:
                {
                    ShowPlayerDialog(playerid, DLG_SA_GLOBAL_SEEK_ID, DIALOG_STYLE_INPUT,
                        "{00CCFF}Seek Stream by ID",
                        "{FFFFFF}Enter Stream ID to seek (transcoded, non-live):",
                        "Next", "Back");
                }
                case 9:
                {
                    ShowPlayerDialog(playerid, DLG_SA_GLOBAL_INFO_ID, DIALOG_STYLE_INPUT,
                        "{00CCFF}Inspect Stream Info",
                        "{FFFFFF}Enter any Stream ID to query metadata and state via SA_GetAudioStreamInfo:",
                        "Inspect", "Back");
                }
                case 10:
                {
                    ShowPlayerDialog(playerid, DLG_SA_GLOBAL_STOP_ID, DIALOG_STYLE_INPUT,
                        "{00CCFF}Stop Stream by ID",
                        "{FFFFFF}Enter Stream ID to stop via SA_StopAudioStream:",
                        "Stop", "Back");
                }
            }
            return 1;
        }

        case DLG_SA_GLOBAL_PLAY_3D:
        {
            if (!response || inputtext[0] == EOS)
            {
                ShowGlobalMenu(playerid);
                return 1;
            }
            new Float:x, Float:y, Float:z;
            GetPlayerPos(playerid, x, y, z);
            new sid = SA_PlayAudioStream(
                inputtext, x, y, z, 45.0, true,
                GetPlayerVirtualWorld(playerid),
                GetPlayerInterior(playerid),
                100.0,
                gOptNotify[playerid],
                gOptProcessFiles[playerid]
            );
            if (sid != INVALID_AUDIO_STREAM_ID)
            {
                if (!TrackGlobalStream(sid, playerid))
                {
                    SA_StopAudioStream(sid);
                    SendClientMessage(playerid, 0xFF5555FF,
                        "[Global] Too many active global streams in this demo; stop one first.");
                }
                else
                {
                    gLastGlobalSid[playerid] = sid;
                    new msg[96];
                    format(msg, sizeof(msg), "[Global 3D] Stream #%d started at your position (45m radius).", sid);
                    SendClientMessage(playerid, 0x00FF88FF, msg);
                }
            }
            else
            {
                SendClientMessage(playerid, 0xFF5555FF, "[Global 3D] Failed to start global stream.");
            }
            ShowGlobalMenu(playerid);
            return 1;
        }

        case DLG_SA_GLOBAL_PLAY_2D:
        {
            if (!response || inputtext[0] == EOS)
            {
                ShowGlobalMenu(playerid);
                return 1;
            }
            new sid = SA_PlayAudioStream(
                inputtext, 0.0, 0.0, 0.0, 50.0, false,
                GetPlayerVirtualWorld(playerid),
                GetPlayerInterior(playerid),
                100.0,
                gOptNotify[playerid],
                gOptProcessFiles[playerid]
            );
            if (sid != INVALID_AUDIO_STREAM_ID)
            {
                if (!TrackGlobalStream(sid, playerid))
                {
                    SA_StopAudioStream(sid);
                    SendClientMessage(playerid, 0xFF5555FF,
                        "[Global] Too many active global streams in this demo; stop one first.");
                }
                else
                {
                    gLastGlobalSid[playerid] = sid;
                    new msg[96];
                    format(msg, sizeof(msg), "[Global 2D] Stream #%d started map-wide in your VW/Interior.", sid);
                    SendClientMessage(playerid, 0x00FF88FF, msg);
                }
            }
            else
            {
                SendClientMessage(playerid, 0xFF5555FF, "[Global 2D] Failed to start global stream.");
            }
            ShowGlobalMenu(playerid);
            return 1;
        }

        case DLG_SA_GLOBAL_DIST:
        {
            if (!response)
            {
                ShowGlobalMenu(playerid);
                return 1;
            }
            if (gLastGlobalSid[playerid] != INVALID_AUDIO_STREAM_ID &&
                !EnsureStreamControlPermission(playerid, gLastGlobalSid[playerid]))
            {
                ShowGlobalMenu(playerid);
                return 1;
            }
            new Float:dist = floatstr(inputtext);
            if (gLastGlobalSid[playerid] != INVALID_AUDIO_STREAM_ID &&
                SA_SetAudioStreamDistance(gLastGlobalSid[playerid], dist))
            {
                new msg[96];
                format(msg, sizeof(msg), "[Global] Stream #%d distance updated to %.1fm.", gLastGlobalSid[playerid], dist);
                SendClientMessage(playerid, 0x00FF88FF, msg);
            }
            else
            {
                SendClientMessage(playerid, 0xFF5555FF, "[Global] Failed to update distance.");
            }
            ShowGlobalMenu(playerid);
            return 1;
        }

        case DLG_SA_GLOBAL_VOL:
        {
            if (!response)
            {
                ShowGlobalMenu(playerid);
                return 1;
            }
            if (gLastGlobalSid[playerid] != INVALID_AUDIO_STREAM_ID &&
                !EnsureStreamControlPermission(playerid, gLastGlobalSid[playerid]))
            {
                ShowGlobalMenu(playerid);
                return 1;
            }
            new Float:vol = floatstr(inputtext);
            if (gLastGlobalSid[playerid] != INVALID_AUDIO_STREAM_ID &&
                SA_SetAudioStreamVolume(gLastGlobalSid[playerid], vol))
            {
                new msg[96];
                format(msg, sizeof(msg), "[Global] Stream #%d volume set to %.1f%%.", gLastGlobalSid[playerid], vol);
                SendClientMessage(playerid, 0x00FF88FF, msg);
            }
            else
            {
                SendClientMessage(playerid, 0xFF5555FF, "[Global] Failed: no active transcoded stream (passthrough streams do not support volume changes).");
            }
            ShowGlobalMenu(playerid);
            return 1;
        }

        case DLG_SA_GLOBAL_ATTACH_PLY:
        {
            if (!response)
            {
                ShowGlobalMenu(playerid);
                return 1;
            }
            if (gLastGlobalSid[playerid] != INVALID_AUDIO_STREAM_ID &&
                !EnsureStreamControlPermission(playerid, gLastGlobalSid[playerid]))
            {
                ShowGlobalMenu(playerid);
                return 1;
            }
            new targetid = strval(inputtext);
            if (gLastGlobalSid[playerid] != INVALID_AUDIO_STREAM_ID &&
                SA_AttachAudioStreamToPlayer(gLastGlobalSid[playerid], targetid))
            {
                new msg[128];
                format(msg, sizeof(msg),
                    "[Global] Stream #%d attached to player %d (2D full volume inside range; the client cannot move 3D audio).",
                    gLastGlobalSid[playerid], targetid);
                SendClientMessage(playerid, 0x00FF88FF, msg);
            }
            else
            {
                SendClientMessage(playerid, 0xFF5555FF, "[Global] Failed to attach (invalid player or inactive stream).");
            }
            ShowGlobalMenu(playerid);
            return 1;
        }

        case DLG_SA_GLOBAL_INFO_ID:
        {
            if (!response)
            {
                ShowGlobalMenu(playerid);
                return 1;
            }
            new sid = strval(inputtext);
            new t[64], a[48], dur, E_SA_STREAM_STATE:st, st_lbl[16], msg[180];
            if (SA_GetAudioStreamInfo(sid, t, sizeof(t), a, sizeof(a), dur, st))
            {
                GetStateLabel(st, st_lbl, sizeof(st_lbl));
                format(msg, sizeof(msg), "[Stream #%d] %s - %s | %ds | State: %s", sid, a, t, dur, st_lbl);
                SendClientMessage(playerid, 0x00CCFFFF, msg);
            }
            else
            {
                SendClientMessage(playerid, 0xFF5555FF, "[Diagnostics] Stream ID not found.");
            }
            ShowGlobalMenu(playerid);
            return 1;
        }

        case DLG_SA_GLOBAL_STOP_ID:
        {
            if (!response)
            {
                ShowGlobalMenu(playerid);
                return 1;
            }
            new sid = strval(inputtext);
            if (!EnsureStreamControlPermission(playerid, sid))
            {
                ShowGlobalMenu(playerid);
                return 1;
            }
            if (SA_StopAudioStream(sid))
            {
                UntrackGlobalStream(sid);
                ClearGlobalSidForAll(sid);
                new msg[80];
                format(msg, sizeof(msg), "[Global] Stream #%d stopped.", sid);
                SendClientMessage(playerid, 0x00FF88FF, msg);
            }
            else
            {
                SendClientMessage(playerid, 0xFF5555FF, "[Global] Stream ID not found.");
            }
            ShowGlobalMenu(playerid);
            return 1;
        }

        // ---------------- Music Search ----------------
        case DLG_SA_SEARCH_INPUT:
        {
            if (!response || inputtext[0] == EOS)
            {
                ShowSaMainMenu(playerid);
                return 1;
            }
            gSearchTotal[playerid] = 0;
            gSearchId[playerid] = SA_SearchAudioStream(playerid, inputtext, 8);
            if (gSearchId[playerid] == INVALID_AUDIO_SEARCH_ID)
            {
                SendClientMessage(playerid, 0xFF5555FF, "[Search] Failed to start search (extractor may still be downloading).");
                ShowSaMainMenu(playerid);
            }
            else
            {
                SendClientMessage(playerid, 0x00CCFFFF, "[Search] Searching... Results dialog will open automatically.");
            }
            return 1;
        }

        case DLG_SA_SEARCH_RESULTS:
        {
            if (!response)
            {
                ShowSaMainMenu(playerid);
                return 1;
            }
            if (listitem < 0 || listitem >= gSearchTotal[playerid])
            {
                return 1;
            }
            gSearchSelIdx[playerid] = listitem;

            new body[512];
            format(body, sizeof(body),
                "Play For Me Only {AAAAAA}(SA_PlayAudioStreamForPlayer)\n\
Play Global 3D Here {AAAAAA}(SA_PlayAudioStream 45m)\n\
Queue on Selected Channel #%d {AAAAAA}(SA_QueueChannelAudioStream)\n\
Play Now on Selected Channel #%d {AAAAAA}(SA_PlayChannelAudioStream)",
                gSelChannelId[playerid],
                gSelChannelId[playerid]
            );
            ShowPlayerDialog(playerid, DLG_SA_SEARCH_ACTION, DIALOG_STYLE_LIST,
                "{FFCC00}Result Action", body, "Run", "Back");
            return 1;
        }

        case DLG_SA_SEARCH_ACTION:
        {
            if (!response)
            {
                ShowSaMainMenu(playerid);
                return 1;
            }
            new idx = gSearchSelIdx[playerid];
            if (idx < 0 || idx >= gSearchTotal[playerid])
            {
                ShowSaMainMenu(playerid);
                return 1;
            }
            new vid[24], url[96];
            strunpack(vid, gSearchVid[playerid][idx]);
            format(url, sizeof(url), "https://www.youtube.com/watch?v=%s", vid);

            switch (listitem)
            {
                case 0:
                {
                    SA_PlayAudioStreamForPlayer(playerid, url, .notify_processing = gOptNotify[playerid]);
                    SendClientMessage(playerid, 0x00FF88FF, "[Search] Starting selected track privately...");
                }
                case 1:
                {
                    new Float:x, Float:y, Float:z;
                    GetPlayerPos(playerid, x, y, z);
                    new sid = SA_PlayAudioStream(url, x, y, z, 45.0, true,
                        GetPlayerVirtualWorld(playerid), GetPlayerInterior(playerid), 100.0, gOptNotify[playerid]);
                    if (sid != INVALID_AUDIO_STREAM_ID)
                    {
                        if (!TrackGlobalStream(sid, playerid))
                        {
                            SA_StopAudioStream(sid);
                            SendClientMessage(playerid, 0xFF5555FF,
                                "[Global] Too many active global streams in this demo; stop one first.");
                        }
                        else
                        {
                            gLastGlobalSid[playerid] = sid;
                            SendClientMessage(playerid, 0x00FF88FF, "[Search] Starting selected track globally in 45m radius...");
                        }
                    }
                    else
                    {
                        SendClientMessage(playerid, 0xFF5555FF, "[Search] Failed to start global stream.");
                    }
                }
                case 2:
                {
                    if (SA_QueueChannelAudioStream(gSelChannelId[playerid], url))
                    {
                        SendClientMessage(playerid, 0x00FF88FF, "[Search] Track added to channel queue.");
                    }
                    else
                    {
                        SendClientMessage(playerid, 0xFF5555FF, "[Search] Target channel unavailable.");
                    }
                }
                case 3:
                {
                    new target_cid = gSelChannelId[playerid];
                    new cname[40], ccreator[32], listeners, E_SA_STREAM_STATE:st, bool:locked, qcount;
                    if (!SA_GetAudioStreamChannelInfo(target_cid, cname, sizeof(cname), ccreator, sizeof(ccreator), listeners, st, locked, qcount))
                    {
                        SendClientMessage(playerid, 0xFF5555FF, "[Search] Target channel unavailable.");
                        return 1;
                    }
                    if (!CanPlayerManageChannel(playerid, ccreator))
                    {
                        new msg[128];
                        SanitizeDialogField(ccreator);
                        format(msg, sizeof(msg), "[Channel] This channel belongs to %s; only its creator can do that.", ccreator);
                        SendClientMessage(playerid, 0xFF5555FF, msg);
                        return 1;
                    }
                    if (SA_PlayChannelAudioStream(target_cid, url))
                    {
                        SendClientMessage(playerid, 0x00FF88FF, "[Search] Playing track immediately on channel.");
                    }
                    else
                    {
                        SendClientMessage(playerid, 0xFF5555FF, "[Search] Target channel unavailable.");
                    }
                }
            }
            return 1;
        }

        // ---------------- Synchronized Channels ----------------
        case DLG_SA_CH_LIST:
        {
            if (!response)
            {
                ShowSaMainMenu(playerid);
                return 1;
            }
            if (listitem == 0)
            {
                ShowPlayerDialog(playerid, DLG_SA_CH_CREATE_NAME, DIALOG_STYLE_INPUT,
                    "{00FF88}Create Channel (1/3) - Name",
                    "{FFFFFF}Enter a name for the new synchronized channel:",
                    "Next", "Back");
                return 1;
            }
            if (listitem == 1)
            {
                if (SA_LeaveAudioStreamChannel(playerid))
                {
                    SendClientMessage(playerid, 0x00FF88FF, "[Channel] Left synchronized channel.");
                }
                ShowChannelListMenu(playerid);
                return 1;
            }

            new idx = listitem - 2;
            if (idx >= 0 && idx < gChListCount[playerid])
            {
                new cid = gChListIds[playerid][idx];
                if (EnsureSelectedChannelExists(playerid, cid))
                {
                    ShowChannelManageMenu(playerid, cid);
                }
            }
            else
            {
                ShowChannelListMenu(playerid);
            }
            return 1;
        }

        case DLG_SA_CH_CREATE_NAME:
        {
            if (!response || inputtext[0] == EOS)
            {
                ShowChannelListMenu(playerid);
                return 1;
            }
            if (IsReservedServerRadioName(inputtext))
            {
                SendClientMessage(playerid, 0xFF5555FF,
                    "[Channel] That name is reserved for the official server radio.");
                ShowPlayerDialog(playerid, DLG_SA_CH_CREATE_NAME, DIALOG_STYLE_INPUT,
                    "{00FF88}Create Channel (1/3) - Name",
                    "{FFFFFF}Enter a name for the new synchronized channel:",
                    "Next", "Back");
                return 1;
            }
            format(gNewChName[playerid], 48, "%s", inputtext);
            ShowPlayerDialog(playerid, DLG_SA_CH_CREATE_PASS, DIALOG_STYLE_PASSWORD,
                "{00FF88}Create Channel (2/3) - Password",
                "{FFFFFF}Enter an optional password for your channel:\n\n{AAAAAA}Leave blank to create an open (public) channel.",
                "Next", "Back");
            return 1;
        }

        case DLG_SA_CH_CREATE_PASS:
        {
            if (!response)
            {
                ShowChannelListMenu(playerid);
                return 1;
            }
            format(gNewChPass[playerid], 32, "%s", inputtext);
            ShowPlayerDialog(playerid, DLG_SA_CH_CREATE_MODE, DIALOG_STYLE_LIST,
                "{00FF88}Create Channel (3/3) - Auto-Destroy Policy",
                "Manual {AAAAAA}(Never expires automatically)\n\
On Creator Disconnect {AAAAAA}(Destroyed when you leave)\n\
Offline Timeout (60s) {AAAAAA}(Timer resets whenever you are online)\n\
Offline Timeout (Custom seconds)",
                "Create", "Back");
            return 1;
        }

        case DLG_SA_CH_CREATE_MODE:
        {
            if (!response)
            {
                ShowChannelListMenu(playerid);
                return 1;
            }
            if (listitem == 3)
            {
                ShowPlayerDialog(playerid, DLG_SA_CH_CREATE_TIME, DIALOG_STYLE_INPUT,
                    "{00FF88}Custom Offline Timeout",
                    "{FFFFFF}Enter timeout in seconds (only counts down while you are disconnected):",
                    "Create", "Back");
                return 1;
            }

            new E_SA_CHANNEL_DESTROY_MODE:mode = SA_CHANNEL_DESTROY_MANUAL;
            new timeout_sec = 0;
            if (listitem == 1) mode = SA_CHANNEL_DESTROY_ON_DISCONNECT;
            else if (listitem == 2) { mode = SA_CHANNEL_DESTROY_ON_TIMEOUT; timeout_sec = 60; }

            new cid = SA_CreateAudioStreamChannel(gNewChName[playerid], playerid, gNewChPass[playerid], mode, timeout_sec);
            if (cid == INVALID_AUDIO_CHANNEL_ID)
            {
                SendClientMessage(playerid, 0xFF5555FF, "[Channel] Failed to create channel.");
                ShowChannelListMenu(playerid);
            }
            else
            {
                new msg[112];
                format(msg, sizeof(msg), "[Channel] Created channel #%d (%s).", cid, gNewChName[playerid]);
                SendClientMessage(playerid, 0x00FF88FF, msg);
                ShowChannelManageMenu(playerid, cid);
            }
            return 1;
        }

        case DLG_SA_CH_CREATE_TIME:
        {
            if (!response)
            {
                ShowChannelListMenu(playerid);
                return 1;
            }
            new timeout_sec = strval(inputtext);
            if (timeout_sec <= 0) timeout_sec = 30;
            new cid = SA_CreateAudioStreamChannel(gNewChName[playerid], playerid, gNewChPass[playerid],
                SA_CHANNEL_DESTROY_ON_TIMEOUT, timeout_sec);
            if (cid == INVALID_AUDIO_CHANNEL_ID)
            {
                SendClientMessage(playerid, 0xFF5555FF, "[Channel] Failed to create channel.");
                ShowChannelListMenu(playerid);
            }
            else
            {
                new msg[128];
                format(msg, sizeof(msg), "[Channel] Created #%d (%s) with %ds offline timeout.",
                    cid, gNewChName[playerid], timeout_sec);
                SendClientMessage(playerid, 0x00FF88FF, msg);
                ShowChannelManageMenu(playerid, cid);
            }
            return 1;
        }

        case DLG_SA_CH_JOIN_PASS:
        {
            new cid = gSelChannelId[playerid];
            if (!EnsureSelectedChannelExists(playerid, cid)) return 1;
            if (!response)
            {
                ShowChannelManageMenu(playerid, cid);
                return 1;
            }
            if (!SA_CheckAudioStreamChannelPass(cid, inputtext) || !SA_JoinAudioStreamChannel(playerid, cid, inputtext))
            {
                SendClientMessage(playerid, 0xFF5555FF, "[Channel] Incorrect password! Cannot join channel.");
            }
            else
            {
                SendClientMessage(playerid, 0x00FF88FF, "[Channel] Password accepted! Joined channel.");
            }
            ShowChannelManageMenu(playerid, cid);
            return 1;
        }

        case DLG_SA_CH_MENU:
        {
            if (!response)
            {
                ShowChannelListMenu(playerid);
                return 1;
            }
            new cid = gSelChannelId[playerid];
            new cname[40], ccreator[32], listeners, E_SA_STREAM_STATE:st, bool:locked, qcount;
            if (!SA_GetAudioStreamChannelInfo(cid, cname, sizeof(cname), ccreator, sizeof(ccreator), listeners, st, locked, qcount))
            {
                SendClientMessage(playerid, 0xFFAA00FF, "[Channel] That channel no longer exists.");
                ShowChannelListMenu(playerid);
                return 1;
            }

            switch (listitem)
            {
                case 0:
                {
                    if (SA_IsPlayerInAudioStreamChannel(playerid, cid))
                    {
                        SA_LeaveAudioStreamChannel(playerid);
                        SendClientMessage(playerid, 0x00FF88FF, "[Channel] Left channel.");
                        ShowChannelManageMenu(playerid, cid);
                    }
                    else if (locked)
                    {
                        ShowPlayerDialog(playerid, DLG_SA_CH_JOIN_PASS, DIALOG_STYLE_PASSWORD,
                            "{FFAA00}Channel Password Required",
                            "{FFFFFF}This channel is password-protected.\nEnter the channel password to tune in:",
                            "Join", "Back");
                    }
                    else
                    {
                        if (SA_JoinAudioStreamChannel(playerid, cid))
                        {
                            SendClientMessage(playerid, 0x00FF88FF, "[Channel] Joined synchronized channel.");
                        }
                        ShowChannelManageMenu(playerid, cid);
                    }
                }
                case 1:
                {
                    if (!EnsureChannelManagePermission(playerid, cid)) return 1;
                    ShowPlayerDialog(playerid, DLG_SA_CH_PLAY, DIALOG_STYLE_INPUT,
                        "{FF88FF}Play Audio Stream on Channel",
                        "{FFFFFF}Enter a YouTube URL, direct MP3, radio URL, or song query:",
                        "Play", "Back");
                }
                case 2:
                {
                    ShowPlayerDialog(playerid, DLG_SA_CH_QUEUE, DIALOG_STYLE_INPUT,
                        "{FF88FF}Queue Audio Stream on Channel",
                        "{FFFFFF}Enter a URL or song query to append to this channel's queue:",
                        "Queue", "Back");
                }
                case 3:
                {
                    if (!EnsureChannelManagePermission(playerid, cid)) return 1;
                    if (SA_SkipChannelAudioStream(cid))
                    {
                        SendClientMessage(playerid, 0x00FF88FF, "[Channel] Skipped current track (advanced queue or stopped if queue empty).");
                    }
                    else
                    {
                        SendClientMessage(playerid, 0xFFAA00FF, "[Channel] Channel is not currently playing anything.");
                    }
                    ShowChannelManageMenu(playerid, cid);
                }
                case 4:
                {
                    if (!EnsureChannelManagePermission(playerid, cid)) return 1;
                    new bool:next_pause = (st != SA_STATE_PAUSED);
                    if (SA_PauseChannelAudioStream(cid, next_pause))
                    {
                        SendClientMessage(playerid, 0x00FF88FF,
                            next_pause ? "[Channel] Channel audio stream paused." : "[Channel] Channel audio stream resumed.");
                    }
                    else
                    {
                        SendClientMessage(playerid, 0xFFAA00FF, "[Channel] Start playback first before pausing/resuming.");
                    }
                    ShowChannelManageMenu(playerid, cid);
                }
                case 5:
                {
                    if (!EnsureChannelManagePermission(playerid, cid)) return 1;
                    ShowPlayerDialog(playerid, DLG_SA_CH_VOLUME, DIALOG_STYLE_INPUT,
                        "{FF88FF}Set Channel Audio Stream Volume",
                        "{FFFFFF}Enter channel volume percentage (0.0 to 200.0):",
                        "Apply", "Back");
                }
                case 6:
                {
                    if (!EnsureChannelManagePermission(playerid, cid)) return 1;
                    ShowChannelQueueDialog(playerid, cid);
                }
                case 7:
                {
                    if (!EnsureChannelManagePermission(playerid, cid)) return 1;
                    new E_SA_CHANNEL_LOOP_MODE:cur = SA_GetAudioStreamChannelLoop(cid);
                    new E_SA_CHANNEL_LOOP_MODE:next = E_SA_CHANNEL_LOOP_MODE:((_:cur + 1) % 3);
                    if (SA_SetAudioStreamChannelLoop(cid, next))
                    {
                        new msg[64];
                        format(msg, sizeof(msg), "[Channel] Loop mode changed to %s.",
                            (next == SA_CHANNEL_LOOP_TRACK) ? "Track" :
                            ((next == SA_CHANNEL_LOOP_QUEUE) ? "Queue" : "Off"));
                        SendClientMessage(playerid, 0x00FF88FF, msg);
                    }
                    ShowChannelManageMenu(playerid, cid);
                }
                case 8:
                {
                    if (!EnsureChannelManagePermission(playerid, cid)) return 1;
                    ShowPlayerDialog(playerid, DLG_SA_CH_POS_MENU, DIALOG_STYLE_LIST,
                        "{FF88FF}Channel Spatial & Entity Attachment",
                        "Set Public Spatial Zone Here (auto_listen = true)\n\
Set Private Spatial Zone Here (auto_listen = false)\n\
Attach Channel to My Vehicle (Sound Car)\n\
Attach Channel to Me\n\
Clear Spatial Mode (Global Channel Again)",
                        "Select", "Back");
                }
                case 9: ShowChannelListenersDialog(playerid, cid);
                case 10:
                {
                    if (!EnsureChannelManagePermission(playerid, cid)) return 1;
                    ShowPlayerDialog(playerid, DLG_SA_CH_RENAME, DIALOG_STYLE_INPUT,
                        "{FF88FF}Rename Synchronized Channel",
                        "{FFFFFF}Enter the new name for this channel:",
                        "Save", "Back");
                }
                case 11:
                {
                    if (!EnsureChannelManagePermission(playerid, cid)) return 1;
                    ShowPlayerDialog(playerid, DLG_SA_CH_SET_PASS, DIALOG_STYLE_PASSWORD,
                        "{FFAA00}Set / Remove Channel Password",
                        "{FFFFFF}Enter a new password for this channel:\n\n{AAAAAA}Leave blank to remove the password and make it public.",
                        "Save", "Back");
                }
                case 12:
                {
                    if (!EnsureChannelManagePermission(playerid, cid)) return 1;
                    ShowPlayerDialog(playerid, DLG_SA_CH_DESTROY_MENU, DIALOG_STYLE_LIST,
                        "{FF88FF}Configure Channel Auto-Destroy",
                        "Manual {AAAAAA}(Never auto-destroyed)\n\
On Creator Disconnect {AAAAAA}(Destroyed immediately when creator leaves)\n\
Offline Timeout (60s) {AAAAAA}(Counts down only when creator is offline)\n\
Offline Timeout (Custom seconds)",
                        "Apply", "Back");
                }
                case 13:
                {
                    if (!EnsureChannelManagePermission(playerid, cid)) return 1;
                    if (SA_SetAudioStreamChannelCreator(cid, playerid))
                    {
                        SendClientMessage(playerid, 0x00FF88FF,
                            "[Channel] Creator ownership updated to you (any offline timer has been reset).");
                    }
                    ShowChannelManageMenu(playerid, cid);
                }
                case 14:
                {
                    if (!EnsureChannelManagePermission(playerid, cid)) return 1;
                    SA_StopChannelAudioStream(cid);
                    SendClientMessage(playerid, 0x00FF88FF, "[Channel] Channel stream stopped and queue cleared.");
                    ShowChannelManageMenu(playerid, cid);
                }
                case 15:
                {
                    if (!EnsureChannelManagePermission(playerid, cid)) return 1;
                    SA_DestroyAudioStreamChannel(cid);
                    SendClientMessage(playerid, 0xFFAA00FF, "[Channel] Channel destroyed.");
                    ShowChannelListMenu(playerid);
                }
                default: ShowChannelManageMenu(playerid, cid);
            }
            return 1;
        }

        case DLG_SA_CH_PLAY:
        {
            new cid = gSelChannelId[playerid];
            if (!response || inputtext[0] == EOS)
            {
                if (!EnsureSelectedChannelExists(playerid, cid)) return 1;
                ShowChannelManageMenu(playerid, cid);
                return 1;
            }
            if (!EnsureChannelManagePermission(playerid, cid)) return 1;
            if (SA_PlayChannelAudioStream(cid, inputtext))
            {
                SendClientMessage(playerid, 0x00FF88FF, "[Channel] Broadcast started!");
            }
            else
            {
                SendClientMessage(playerid, 0xFF5555FF, "[Channel] Failed to start broadcast.");
            }
            ShowChannelManageMenu(playerid, cid);
            return 1;
        }

        case DLG_SA_CH_QUEUE:
        {
            new cid = gSelChannelId[playerid];
            if (!EnsureSelectedChannelExists(playerid, cid)) return 1;
            if (!response || inputtext[0] == EOS)
            {
                ShowChannelManageMenu(playerid, cid);
                return 1;
            }
            if (SA_QueueChannelAudioStream(cid, inputtext))
            {
                SendClientMessage(playerid, 0x00FF88FF, "[Channel] Added to playback queue!");
            }
            else
            {
                SendClientMessage(playerid, 0xFF5555FF, "[Channel] Failed to queue URL.");
            }
            ShowChannelManageMenu(playerid, cid);
            return 1;
        }

        case DLG_SA_CH_QUEUE_VIEW:
        {
            new cid = gSelChannelId[playerid];
            if (!response)
            {
                if (!EnsureSelectedChannelExists(playerid, cid)) return 1;
                ShowChannelManageMenu(playerid, cid);
                return 1;
            }
            if (!EnsureChannelManagePermission(playerid, cid)) return 1;
            switch (listitem)
            {
                case 0:
                {
                    ShowPlayerDialog(playerid, DLG_SA_CH_QUEUE_INS0, DIALOG_STYLE_INPUT,
                        "{00FF88}Insert at Queue Index 0",
                        "{FFFFFF}Enter a URL or query to insert at the front of the queue (plays next):",
                        "Insert", "Back");
                }
                case 1:
                {
                    ShowPlayerDialog(playerid, DLG_SA_CH_QUEUE_REM, DIALOG_STYLE_INPUT,
                        "{FF8888}Remove Queue Item",
                        "{FFFFFF}Enter the 0-based queue index to remove (e.g. 0):",
                        "Remove", "Back");
                }
                case 2:
                {
                    ShowPlayerDialog(playerid, DLG_SA_CH_PLAYLIST, DIALOG_STYLE_INPUT,
                        "{00CCFF}Import Channel Playlist",
                        "{FFFFFF}Enter a playlist URL (max 25 items; requires extractor ready):",
                        "Import", "Back");
                }
                case 3:
                {
                    ShowPlayerDialog(playerid, DLG_SA_CH_SEEK, DIALOG_STYLE_INPUT,
                        "{FFCC00}Seek Channel Track",
                        "{FFFFFF}Enter target position in seconds (e.g. 30):",
                        "Seek", "Back");
                }
                case 4:
                {
                    if (SA_ClearAudioStreamChannelQueue(cid))
                    {
                        SendClientMessage(playerid, 0x00FF88FF, "[Channel] Playback queue cleared.");
                    }
                    ShowChannelManageMenu(playerid, cid);
                }
                default:
                {
                    new idx = listitem - 5;
                    new cur_item[96];
                    if (idx < 0 || idx >= gQueueViewCount[playerid] ||
                        !SA_GetAudioStreamChannelQueue(cid, idx, cur_item, sizeof(cur_item)))
                    {
                        SendClientMessage(playerid, 0xFFAA00FF,
                            "[Channel] Queue changed before selection; refreshing list.");
                        ShowChannelQueueDialog(playerid, cid);
                        return 1;
                    }
                    SanitizeDialogField(cur_item);
                    if (HashStringDjb2(cur_item) != gQueueViewHash[playerid][idx])
                    {
                        SendClientMessage(playerid, 0xFFAA00FF,
                            "[Channel] Queue changed before selection; refreshing list.");
                        ShowChannelQueueDialog(playerid, cid);
                        return 1;
                    }
                    if (SA_RemoveAudioStreamChannelQueue(cid, idx))
                    {
                        new msg[64];
                        format(msg, sizeof(msg), "[Channel] Removed queue item #%d.", idx);
                        SendClientMessage(playerid, 0x00FF88FF, msg);
                    }
                    ShowChannelQueueDialog(playerid, cid);
                }
            }
            return 1;
        }

        case DLG_SA_CH_QUEUE_INS0:
        {
            new cid = gSelChannelId[playerid];
            if (!response || inputtext[0] == EOS)
            {
                if (!EnsureSelectedChannelExists(playerid, cid)) return 1;
                ShowChannelQueueDialog(playerid, cid);
                return 1;
            }
            if (!EnsureChannelManagePermission(playerid, cid)) return 1;
            if (SA_InsertAudioStreamChannelQueue(cid, 0, inputtext))
            {
                SendClientMessage(playerid, 0x00FF88FF, "[Channel] Inserted track at index 0 (Play Next).");
            }
            else
            {
                SendClientMessage(playerid, 0xFF5555FF, "[Channel] Failed to insert item (queue full or invalid channel).");
            }
            ShowChannelQueueDialog(playerid, cid);
            return 1;
        }

        case DLG_SA_CH_QUEUE_REM:
        {
            new cid = gSelChannelId[playerid];
            if (!response || inputtext[0] == EOS)
            {
                if (!EnsureSelectedChannelExists(playerid, cid)) return 1;
                ShowChannelQueueDialog(playerid, cid);
                return 1;
            }
            if (!EnsureChannelManagePermission(playerid, cid)) return 1;
            new idx = strval(inputtext);
            if (SA_RemoveAudioStreamChannelQueue(cid, idx))
            {
                SendClientMessage(playerid, 0x00FF88FF, "[Channel] Queue item removed.");
            }
            else
            {
                SendClientMessage(playerid, 0xFF5555FF, "[Channel] Invalid queue index.");
            }
            ShowChannelQueueDialog(playerid, cid);
            return 1;
        }

        case DLG_SA_CH_PLAYLIST:
        {
            new cid = gSelChannelId[playerid];
            if (!response || inputtext[0] == EOS)
            {
                if (!EnsureSelectedChannelExists(playerid, cid)) return 1;
                ShowChannelQueueDialog(playerid, cid);
                return 1;
            }
            if (!EnsureChannelManagePermission(playerid, cid)) return 1;
            if (SA_QueueChannelAudioStreamPlaylist(cid, inputtext, 25))
            {
                SendClientMessage(playerid, 0x00CCFFFF, "[Channel] Playlist import started in background...");
            }
            else
            {
                SendClientMessage(playerid, 0xFF5555FF, "[Channel] Failed to start playlist import.");
            }
            ShowChannelQueueDialog(playerid, cid);
            return 1;
        }

        case DLG_SA_CH_SEEK:
        {
            new cid = gSelChannelId[playerid];
            if (!response || inputtext[0] == EOS)
            {
                if (!EnsureSelectedChannelExists(playerid, cid)) return 1;
                ShowChannelQueueDialog(playerid, cid);
                return 1;
            }
            if (!EnsureChannelManagePermission(playerid, cid)) return 1;
            new sec = strval(inputtext);
            if (SA_SeekChannelAudioStream(cid, sec))
            {
                SendClientMessage(playerid, 0x00FF88FF, "[Channel] Track seeked!");
            }
            else
            {
                SendClientMessage(playerid, 0xFF5555FF, "[Channel] Seek failed (track must be Playing/Paused, non-live, known duration).");
            }
            ShowChannelManageMenu(playerid, cid);
            return 1;
        }

        case DLG_SA_CH_POS_MENU:
        {
            new cid = gSelChannelId[playerid];
            if (!response)
            {
                if (!EnsureSelectedChannelExists(playerid, cid)) return 1;
                ShowChannelManageMenu(playerid, cid);
                return 1;
            }
            if (!EnsureChannelManagePermission(playerid, cid)) return 1;
            switch (listitem)
            {
                case 0, 1:
                {
                    gSelChanAutoListen[playerid] = (listitem == 0);
                    ShowPlayerDialog(playerid, DLG_SA_CH_POS_SET, DIALOG_STYLE_INPUT,
                        "{FF88FF}Set Channel Spatial Radius",
                        "{FFFFFF}Enter the audible 3D radius at your position (default 50.0):",
                        "Apply", "Back");
                    return 1;
                }
                case 2:
                {
                    new vid = GetPlayerVehicleID(playerid);
                    if (vid > 0 && SA_AttachAudioStreamChannelToVehicle(cid, vid))
                    {
                        SendClientMessage(playerid, 0x00FF88FF,
                            "[Channel] Channel attached to your current vehicle (2D full volume inside range; the client cannot move 3D audio).");
                    }
                    else
                    {
                        SendClientMessage(playerid, 0xFF5555FF, "[Channel] Enter a valid vehicle first.");
                    }
                }
                case 3:
                {
                    if (SA_AttachAudioStreamChannelToPlayer(cid, playerid))
                    {
                        SendClientMessage(playerid, 0x00FF88FF,
                            "[Channel] Channel attached to your player position (2D full volume inside range; the client cannot move 3D audio).");
                    }
                    else
                    {
                        SendClientMessage(playerid, 0xFF5555FF, "[Channel] Failed to attach channel to player.");
                    }
                }
                case 4:
                {
                    if (SA_ClearAudioStreamChannelPos(cid))
                    {
                        SendClientMessage(playerid, 0x00FF88FF, "[Channel] Cleared spatial mode; subscribers now hear globally.");
                    }
                }
            }
            ShowChannelManageMenu(playerid, cid);
            return 1;
        }

        case DLG_SA_CH_POS_SET:
        {
            new cid = gSelChannelId[playerid];
            if (!response)
            {
                if (!EnsureSelectedChannelExists(playerid, cid)) return 1;
                ShowChannelManageMenu(playerid, cid);
                return 1;
            }
            if (!EnsureChannelManagePermission(playerid, cid)) return 1;
            new Float:dist = (inputtext[0] == EOS) ? 50.0 : floatstr(inputtext);
            new Float:x, Float:y, Float:z;
            GetPlayerPos(playerid, x, y, z);
            if (SA_SetAudioStreamChannelPos(cid, x, y, z, dist,
                GetPlayerVirtualWorld(playerid), GetPlayerInterior(playerid), gSelChanAutoListen[playerid]))
            {
                SendClientMessage(playerid, 0x00FF88FF, gSelChanAutoListen[playerid]
                    ? "[Channel] Public 3D spatial zone configured (auto_listen enabled)."
                    : "[Channel] Private 3D spatial zone configured (subscribed listeners in range only).");
            }
            else
            {
                SendClientMessage(playerid, 0xFF5555FF, "[Channel] Invalid radius or channel.");
            }
            ShowChannelManageMenu(playerid, cid);
            return 1;
        }

        case DLG_SA_CH_RENAME:
        {
            new cid = gSelChannelId[playerid];
            if (!response || inputtext[0] == EOS)
            {
                if (!EnsureSelectedChannelExists(playerid, cid)) return 1;
                ShowChannelManageMenu(playerid, cid);
                return 1;
            }
            if (!EnsureChannelManagePermission(playerid, cid)) return 1;
            if (IsReservedServerRadioName(inputtext))
            {
                SendClientMessage(playerid, 0xFF5555FF,
                    "[Channel] That name is reserved for the official server radio.");
                ShowChannelManageMenu(playerid, cid);
                return 1;
            }
            if (SA_SetAudioStreamChannelName(cid, inputtext))
            {
                SendClientMessage(playerid, 0x00FF88FF, "[Channel] Channel renamed.");
            }
            else
            {
                SendClientMessage(playerid, 0xFF5555FF, "[Channel] Failed to rename channel.");
            }
            ShowChannelManageMenu(playerid, cid);
            return 1;
        }

        case DLG_SA_CH_LISTENERS:
        {
            ShowChannelManageMenu(playerid, gSelChannelId[playerid]);
            return 1;
        }

        case DLG_SA_CH_VOLUME:
        {
            new cid = gSelChannelId[playerid];
            if (!response)
            {
                if (!EnsureSelectedChannelExists(playerid, cid)) return 1;
                ShowChannelManageMenu(playerid, cid);
                return 1;
            }
            if (!EnsureChannelManagePermission(playerid, cid)) return 1;
            new Float:vol = floatstr(inputtext);
            if (SA_SetChannelAudioStreamVolume(cid, vol))
            {
                new msg[80];
                format(msg, sizeof(msg), "[Channel] Volume set to %.1f%%.", vol);
                SendClientMessage(playerid, 0x00FF88FF, msg);
            }
            ShowChannelManageMenu(playerid, cid);
            return 1;
        }

        case DLG_SA_CH_SET_PASS:
        {
            new cid = gSelChannelId[playerid];
            if (!response)
            {
                ShowChannelManageMenu(playerid, cid);
                return 1;
            }
            if (!EnsureChannelManagePermission(playerid, cid)) return 1;
            if (SA_SetAudioStreamChannelPass(cid, inputtext))
            {
                SendClientMessage(playerid, 0x00FF88FF,
                    (inputtext[0] == EOS) ?
                    "[Channel] Password removed (channel is now open)." :
                    "[Channel] Channel password updated.");
            }
            ShowChannelManageMenu(playerid, cid);
            return 1;
        }

        case DLG_SA_CH_DESTROY_MENU:
        {
            new cid = gSelChannelId[playerid];
            if (!response)
            {
                ShowChannelManageMenu(playerid, cid);
                return 1;
            }
            if (!EnsureChannelManagePermission(playerid, cid)) return 1;
            if (listitem == 3)
            {
                ShowPlayerDialog(playerid, DLG_SA_CH_DESTROY_TIME, DIALOG_STYLE_INPUT,
                    "{FF88FF}Custom Offline Timeout",
                    "{FFFFFF}Enter timeout seconds (only counts down while creator is offline):",
                    "Apply", "Back");
                return 1;
            }
            new E_SA_CHANNEL_DESTROY_MODE:mode = SA_CHANNEL_DESTROY_MANUAL;
            new timeout_sec = 0;
            if (listitem == 1) mode = SA_CHANNEL_DESTROY_ON_DISCONNECT;
            else if (listitem == 2) { mode = SA_CHANNEL_DESTROY_ON_TIMEOUT; timeout_sec = 60; }

            if (SA_SetAudioStreamChannelDestroy(cid, mode, timeout_sec))
            {
                SendClientMessage(playerid, 0x00FF88FF, "[Channel] Auto-destroy policy updated.");
            }
            ShowChannelManageMenu(playerid, cid);
            return 1;
        }

        case DLG_SA_CH_DESTROY_TIME:
        {
            new cid = gSelChannelId[playerid];
            if (!response)
            {
                ShowChannelManageMenu(playerid, cid);
                return 1;
            }
            if (!EnsureChannelManagePermission(playerid, cid)) return 1;
            new timeout_sec = strval(inputtext);
            if (timeout_sec <= 0) timeout_sec = 30;
            if (SA_SetAudioStreamChannelDestroy(cid, SA_CHANNEL_DESTROY_ON_TIMEOUT, timeout_sec))
            {
                new msg[96];
                format(msg, sizeof(msg), "[Channel] Offline timeout set to %ds.", timeout_sec);
                SendClientMessage(playerid, 0x00FF88FF, msg);
            }
            ShowChannelManageMenu(playerid, cid);
            return 1;
        }

        // ---------------- Vanilla Hook & Diagnostics ----------------
        case DLG_SA_VANILLA_PLAY:
        {
            if (!response || inputtext[0] == EOS)
            {
                ShowSaMainMenu(playerid);
                return 1;
            }
            PlayAudioStreamForPlayer(
                playerid,
                inputtext,
                0.0, 0.0, 0.0, 50.0, false,
                -1, -1, 80.0,
                gOptNotify[playerid],
                gOptProcessFiles[playerid]
            );
            SendClientMessage(playerid, 0x00FF88FF,
                "[Hook] Called hooked PlayAudioStreamForPlayer(..., volume = 80.0).");
            ShowSaMainMenu(playerid);
            return 1;
        }

        case DLG_SA_GLOBAL_PAUSE_ID:
        {
            if (!response || inputtext[0] == EOS)
            {
                ShowGlobalMenu(playerid);
                return 1;
            }
            new sid = strval(inputtext), bool:p = true;
            if (!EnsureStreamControlPermission(playerid, sid))
            {
                ShowGlobalMenu(playerid);
                return 1;
            }
            new sp = strfind(inputtext, " ");
            if (sp != -1 && inputtext[sp + 1] == '0') p = false;
            if (SA_PauseAudioStream(sid, p))
            {
                SendClientMessage(playerid, 0x00FF88FF, p ? "[Stream] Stream paused." : "[Stream] Stream resumed.");
            }
            else
            {
                SendClientMessage(playerid, 0xFF5555FF, "[Stream] Pause/resume failed.");
            }
            ShowGlobalMenu(playerid);
            return 1;
        }

        case DLG_SA_GLOBAL_SEEK_ID:
        {
            if (!response || inputtext[0] == EOS)
            {
                ShowGlobalMenu(playerid);
                return 1;
            }
            new sid = strval(inputtext);
            if (!EnsureStreamControlPermission(playerid, sid))
            {
                ShowGlobalMenu(playerid);
                return 1;
            }
            gSeekTargetSid[playerid] = sid;
            ShowPlayerDialog(playerid, DLG_SA_GLOBAL_SEEK_SEC, DIALOG_STYLE_INPUT,
                "{00CCFF}Seek Stream",
                "{FFFFFF}Enter target position in seconds:",
                "Seek", "Back");
            return 1;
        }

        case DLG_SA_GLOBAL_SEEK_SEC:
        {
            if (!response || inputtext[0] == EOS)
            {
                ShowGlobalMenu(playerid);
                return 1;
            }
            if (!EnsureStreamControlPermission(playerid, gSeekTargetSid[playerid]))
            {
                ShowGlobalMenu(playerid);
                return 1;
            }
            new sec = strval(inputtext);
            if (SA_SeekAudioStream(gSeekTargetSid[playerid], sec))
            {
                SendClientMessage(playerid, 0x00FF88FF, "[Stream] Seek applied!");
            }
            else
            {
                SendClientMessage(playerid, 0xFF5555FF, "[Stream] Seek failed (must be Playing/Paused, transcoded, non-live, valid seconds).");
            }
            ShowGlobalMenu(playerid);
            return 1;
        }

        case DLG_SA_DIAGNOSTICS:
        {
            if (response)
            {
                ShowDiagnosticsDialog(playerid);
            }
            else
            {
                ShowSaMainMenu(playerid);
            }
            return 1;
        }
    }
    return 0;
}

// -----------------------------------------------------------------------------
// All 16 SampAudioStream Plugin Callbacks
// -----------------------------------------------------------------------------

public OnAudioStreamProcessing(playerid, streamid, const url[], bool:from_cache)
{
    if (playerid != INVALID_PLAYER_ID && IsPlayerConnected(playerid))
    {
        new msg[144];
        format(msg, sizeof(msg),
            "{FFCC00}[Audio] {FFFFFF}%s stream #%d: %s",
            from_cache ? "Fast-starting cached" : "Processing",
            streamid,
            url
        );
        SendClientMessage(playerid, -1, msg);
    }
    return 1;
}

public OnAudioStreamWaiting(playerid, streamid, wait_ms)
{
    if (playerid != INVALID_PLAYER_ID && IsPlayerConnected(playerid))
    {
        new sec = (wait_ms > 0) ? ((wait_ms + 999) / 1000) : 0;
        new msg[96];
        format(msg, sizeof(msg), "{FFCC00}[Audio] {FFFFFF}Stream #%d starting in ~%ds...", streamid, sec);
        SendClientMessage(playerid, -1, msg);
    }
    return 1;
}

public OnAudioStreamReady(playerid, streamid, const title[], const artist[], duration, const url[])
{
    if (playerid != INVALID_PLAYER_ID && IsPlayerConnected(playerid))
    {
        new dur_str[16], msg[144];
        FormatSec(duration, dur_str, sizeof(dur_str));
        format(msg, sizeof(msg), "{00FF88}[Audio Ready] {FFFFFF}#%d: %s - %s (%s)",
            streamid,
            (artist[0] != EOS) ? artist : "Unknown",
            (title[0] != EOS) ? title : "Audio Stream",
            dur_str
        );
        SendClientMessage(playerid, -1, msg);
    }
    return 1;
}

public OnAudioStreamStart(playerid, streamid)
{
    if (playerid != INVALID_PLAYER_ID && IsPlayerConnected(playerid))
    {
        new msg[80];
        format(msg, sizeof(msg), "{00CCFF}[Audio] {FFFFFF}Playback started (Stream #%d).", streamid);
        SendClientMessage(playerid, -1, msg);
    }
    return 1;
}

public OnAudioStreamFinished(playerid, streamid)
{
    new tracked_owner = UntrackGlobalStream(streamid);
    if (playerid != INVALID_PLAYER_ID)
    {
        if (IsPlayerConnected(playerid))
        {
            new msg[80];
            format(msg, sizeof(msg), "{AAAAAA}[Audio] {FFFFFF}Stream #%d finished playing.", streamid);
            SendClientMessage(playerid, -1, msg);
        }
    }
    else
    {
        new msg[88];
        format(msg, sizeof(msg), "{AAAAAA}[Audio] {FFFFFF}Global stream #%d finished playing.", streamid);
        for (new p = 0; p < MAX_PLAYERS; p++)
        {
            if (IsPlayerConnected(p) && (gLastGlobalSid[p] == streamid || p == tracked_owner))
            {
                SendClientMessage(p, -1, msg);
            }
        }
    }
    ClearGlobalSidForAll(streamid);
    return 1;
}

public OnAudioStreamError(playerid, streamid, E_SA_ERROR_CODE:error_code, const error_message[])
{
    new tracked_owner = GetTrackedGlobalOwner(streamid);
    if (playerid != INVALID_PLAYER_ID)
    {
        if (IsPlayerConnected(playerid))
        {
            new msg[144];
            format(msg, sizeof(msg), "{FF5555}[Audio Error #%d] {FFFFFF}(Code %d): %s",
                streamid, _:error_code, error_message);
            SendClientMessage(playerid, -1, msg);
        }
    }
    else
    {
        new msg[144];
        format(msg, sizeof(msg), "{FF5555}[Audio Error #%d] {FFFFFF}(Code %d): %s",
            streamid, _:error_code, error_message);
        for (new p = 0; p < MAX_PLAYERS; p++)
        {
            if (IsPlayerConnected(p) && (gLastGlobalSid[p] == streamid || p == tracked_owner))
            {
                SendClientMessage(p, -1, msg);
            }
        }
    }

    // Non-fatal warnings (e.g., public IPv4 lookup failure) leave the stream active;
    // only untrack and clear gLastGlobalSid once the stream has actually ended/stopped.
    new t[8], a[8], dur, E_SA_STREAM_STATE:st;
    new bool:still_active = (SA_GetAudioStreamInfo(streamid, t, sizeof(t), a, sizeof(a), dur, st) &&
        st != SA_STATE_STOPPED && st != SA_STATE_ERROR && st != SA_STATE_FINISHED);
    if (!still_active)
    {
        UntrackGlobalStream(streamid);
        ClearGlobalSidForAll(streamid);
    }
    return 1;
}

public OnAudioStreamSearchResultItem(playerid, search_id, item_index, const title[], const artist[], duration, const video_id[])
{
    if (!IsPlayerConnected(playerid) || search_id != gSearchId[playerid])
    {
        return 1;
    }
    if (item_index >= 0 && item_index < DEMO_MAX_SEARCH_ITEMS)
    {
        new t[64], a[48], v[24];
        format(t, sizeof(t), "%s", title);
        format(a, sizeof(a), "%s", artist);
        format(v, sizeof(v), "%s", video_id);
        SanitizeDialogField(t);
        SanitizeDialogField(a);
        SanitizeDialogField(v);
        strpack(gSearchTitle[playerid][item_index], t);
        strpack(gSearchArtist[playerid][item_index], a);
        gSearchDur[playerid][item_index] = duration;
        strpack(gSearchVid[playerid][item_index], v);
        if (item_index + 1 > gSearchTotal[playerid])
        {
            gSearchTotal[playerid] = item_index + 1;
        }
    }
    return 1;
}

public OnAudioStreamSearchResults(playerid, search_id, total_found, E_SA_ERROR_CODE:error_code, const error_message[])
{
    if (!IsPlayerConnected(playerid) || search_id != gSearchId[playerid])
    {
        return 1;
    }
    gSearchId[playerid] = INVALID_AUDIO_SEARCH_ID;

    if (error_code != SA_ERR_NONE)
    {
        new msg[144];
        format(msg, sizeof(msg), "{FF5555}[Search Error] {FFFFFF}(Code %d): %s", _:error_code, error_message);
        SendClientMessage(playerid, -1, msg);
        return 1;
    }

    if (total_found <= 0 || gSearchTotal[playerid] <= 0)
    {
        SendClientMessage(playerid, 0xFFAA00FF, "[Search] No results found for that query.");
        return 1;
    }

    new body[2048], line[160], dur_str[16], t[64], a[48];
    format(body, sizeof(body), "Title\tArtist\tDuration\n");
    for (new i = 0; i < gSearchTotal[playerid]; i++)
    {
        strunpack(t, gSearchTitle[playerid][i]);
        strunpack(a, gSearchArtist[playerid][i]);
        FormatSec(gSearchDur[playerid][i], dur_str, sizeof(dur_str), false);
        format(line, sizeof(line), "%s\t%s\t%s\n", t, a, dur_str);
        strcat(body, line);
    }

    ShowPlayerDialog(playerid, DLG_SA_SEARCH_RESULTS, DIALOG_STYLE_TABLIST_HEADERS,
        "{FFCC00}Search Results - Pick a Track", body, "Select", "Close");
    return 1;
}

public OnAudioStreamChannelDestroyed(channelid, E_SA_CHANNEL_DESTROY_REASON:reason)
{
    if (channelid == gServerRadioId)
    {
        gServerRadioId = INVALID_AUDIO_CHANNEL_ID;
    }
    for (new i = 0; i < MAX_PLAYERS; i++)
    {
        if (gSelChannelId[i] == channelid)
        {
            gSelChannelId[i] = gServerRadioId;
        }
    }

    new reason_str[32];
    switch (reason)
    {
        case SA_CH_DESTROY_REASON_CREATOR: format(reason_str, sizeof(reason_str), "creator disconnected");
        case SA_CH_DESTROY_REASON_TIMEOUT: format(reason_str, sizeof(reason_str), "offline timeout expired");
        default:                           format(reason_str, sizeof(reason_str), "closed");
    }
    printf("[SampAudioStream] Channel #%d destroyed (%s).", channelid, reason_str);
    return 1;
}

public OnAudioStreamChannelTrackWaiting(channelid, wait_ms)
{
    new sec = (wait_ms > 0) ? ((wait_ms + 999) / 1000) : 0;
    new msg[96];
    format(msg, sizeof(msg), "{FFCC00}[Channel #%d] {FFFFFF}Track starting in ~%ds...", channelid, sec);
    for (new i = 0; i < MAX_PLAYERS; i++)
    {
        if (IsPlayerConnected(i) && SA_IsPlayerInAudioStreamChannel(i, channelid))
        {
            SendClientMessage(i, -1, msg);
        }
    }
    printf("[SampAudioStream] Channel #%d waiting ~%ds (%d ms) for media availability.", channelid, sec, wait_ms);
    return 1;
}

public OnAudioStreamChannelTrackStart(channelid, const title[], const artist[], duration, const url[])
{
    printf("[SampAudioStream] Channel #%d track started: %s - %s (%ds) [%s]",
        channelid, artist, title, duration, url);
    return 1;
}

public OnAudioStreamChannelQueueEnd(channelid)
{
    printf("[SampAudioStream] Channel #%d queue ended naturally.", channelid);
    return 1;
}

public OnAudioStreamChannelError(channelid, E_SA_ERROR_CODE:error_code, const error_message[])
{
    printf("[SampAudioStream] Channel #%d error (%d): %s", channelid, _:error_code, error_message);
    return 1;
}

public OnAudioStreamChannelRestored(channelid, const name[], const creator_name[])
{
    if (gServerRadioId == INVALID_AUDIO_CHANNEL_ID &&
        name[0] != EOS &&
        creator_name[0] != EOS &&
        !strcmp(name, DEMO_SERVER_RADIO_NAME, false) &&
        !strcmp(creator_name, "Server", false))
    {
        gServerRadioId = channelid;
    }
    printf("[SampAudioStream] Channel #%d restored: '%s' (Creator: %s)", channelid, name, creator_name);
    return 1;
}

public OnPlayerAudioStreamSourceChange(playerid, E_SA_AUDIO_STREAM_SOURCE:source_type, source_id)
{
    if (IsPlayerConnected(playerid))
    {
        new lbl[16], msg[96];
        GetSourceTypeLabel(source_type, lbl, sizeof(lbl));
        format(msg, sizeof(msg), "{CCCCCC}[Source] Now hearing: %s (ID: %d)", lbl, source_id);
        SendClientMessage(playerid, -1, msg);
    }
    return 1;
}

public OnAudioStreamChannelPlaylistQueued(channelid, added, E_SA_ERROR_CODE:error_code, const error_message[])
{
    printf("[SampAudioStream] Channel #%d playlist import finished: added=%d err=%d (%s)",
        channelid, added, _:error_code, error_message);
    return 1;
}
