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
 * Demonstrates ALL 83 SA_* natives, hooked PlayAudioStreamForPlayer / Stop,
 * and ALL 19 plugin callbacks cleanly from in-game menus (no shadow arrays).
 *
 * Server Folders (plugin config):
 *  - audio.music_dir (scriptfiles/SampAudioStream/music): local music library browsed in
 *    "Music Library" and played through library:// references.
 *  - audio.sound_dir (scriptfiles/SampAudioStream/sound): short sound effects (.mp3/.ogg/.wav up to 5 MB)
 *    played from "Personal Playback Controls > Play Sound Effect".
 *
 * Persistence & Permissions:
 *  - Reuses the persistent server radio channel ("SampAudioStream FM") across restarts/reloads;
 *    channels created as "Persistent" (persistent = true) stay saved on disk until explicitly destroyed,
 *    while "Temporary" channels live only in memory and are destroyed on GMX/restart.
 *  - Never destroys channels in OnFilterScriptExit, keeping channel persistence intact.
 *  - Players cannot control another player's stream or channel; server-owned streams and
 *    channels ("Server" creator) are free for all. Channel ownership compares player name
 *    (as stored by the plugin); adapt CanPlayerManageChannel to your account system in production.
 * =============================================================================
 */

// open.mp toolchains: include <open.mp> directly (its <a_samp> wrapper only adds a deprecation warning).
// Legacy SA-MP toolchains have no <open.mp> and fall back to <a_samp>.
#tryinclude <open.mp>
#if !defined _INC_open_mp
    #include <a_samp>
#endif
#include <SampAudioStream>

// -----------------------------------------------------------------------------
// Configuration & Dialog IDs (high offset to avoid gamemode dialog collisions)
// -----------------------------------------------------------------------------

#define DEMO_DLG_BASE              (18400)
#define DEMO_MAX_SEARCH_ITEMS      (10)
#define DEMO_MAX_TRACKED_GLOBALS   (64)
#define DEMO_MAX_LISTED_CHANNELS   (64)
#define DEMO_MAX_QUEUE_VIEW_ITEMS  (10)
#define DEMO_MAX_HIST_CHANNELS     (16)
#define DEMO_HIST_DEPTH            (5)
#define DEMO_MAX_LIBRARY_FOLDERS   (40)
#define DEMO_MAX_LIBRARY_TRACKS    (40)
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
    DLG_SA_DIAGNOSTICS,
    DLG_SA_CH_LIVE_VIEW,
    DLG_SA_CH_LIVE_SET,
    DLG_SA_GLOBAL_LIVE_SET_ID,
    DLG_SA_CH_QUEUE_ITEM,
    DLG_SA_CH_CREATE_PERSIST,
    DLG_SA_LIB_FOLDERS,
    DLG_SA_LIB_FOLDER,
    DLG_SA_LIB_TRACK,
    DLG_SA_SOUND_INPUT,
    DLG_SA_SOUND_MODE,
    DLG_SA_LAST
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
static bool:gOptStreamText[MAX_PLAYERS];
static gSelChannelId[MAX_PLAYERS];
static bool:gSelChanAutoListen[MAX_PLAYERS];

// Last channel list & queue snapshot shown to each player (race-condition guard)
static gChListIds[MAX_PLAYERS][DEMO_MAX_LISTED_CHANNELS];
static gChListCount[MAX_PLAYERS];
static gQueueViewHash[MAX_PLAYERS][DEMO_MAX_QUEUE_VIEW_ITEMS];
static gQueueViewCount[MAX_PLAYERS];
static gQueueViewPage[MAX_PLAYERS];
static gQueueViewStart[MAX_PLAYERS];
static gQueueSelIdx[MAX_PLAYERS];
static gQueueSelHash[MAX_PLAYERS];
static gVoteCid[MAX_PLAYERS];
static gVoteSerial[MAX_PLAYERS];

// Shared per-channel track history & vote-skip serial state (in-memory only; resets on FS reload/restart)
static gHistCid[DEMO_MAX_HIST_CHANNELS];
static gTrackSerial[DEMO_MAX_HIST_CHANNELS];
static gTrackSerialCounter = 0;
static gHistUrl[DEMO_MAX_HIST_CHANNELS][DEMO_HIST_DEPTH][256 char];
static gHistLabel[DEMO_MAX_HIST_CHANNELS][DEMO_HIST_DEPTH][128 char];
static gHistCount[DEMO_MAX_HIST_CHANNELS];
static gCurUrl[DEMO_MAX_HIST_CHANNELS][256 char];
static gCurLabel[DEMO_MAX_HIST_CHANNELS][128 char];
static bool:gHistGoingBack[DEMO_MAX_HIST_CHANNELS];

// Global streams created through this filterscript and their creator playerid
static gTrackedGlobalSid[DEMO_MAX_TRACKED_GLOBALS];
static gTrackedGlobalOwner[DEMO_MAX_TRACKED_GLOBALS];

// Temporary channel creation wizard state per player
static gNewChName[MAX_PLAYERS][48];
static gNewChPass[MAX_PLAYERS][32];
static E_SA_CHANNEL_DESTROY_MODE:gNewChMode[MAX_PLAYERS];
static gNewChTimeout[MAX_PLAYERS];

// Music library browser & sound effect selection per player
static gLibFolder[MAX_PLAYERS];
static gLibTrack[MAX_PLAYERS];
static gLibFolderPage[MAX_PLAYERS];
static gLibTrackPage[MAX_PLAYERS];
static gLibViewCount[MAX_PLAYERS];

// "Next/Previous Page" rows shown after the items of the open paged list (bit 1 = next, bit 2 = previous)
static gPageNav[MAX_PLAYERS];
static gSoundName[MAX_PLAYERS][64];

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
        case SA_SOURCE_SOUND:         format(out, len, "Sound");
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

static stock SendTruncatedLiveMessage(playerid, const prefix[], const raw_text[], color = -1)
{
    new clean[280];
    format(clean, sizeof(clean), "%s", raw_text);
    SanitizeDialogField(clean);

    new msg[145];
    new plen = strlen(prefix);
    new tlen = strlen(clean);
    if (plen + tlen <= 144)
    {
        format(msg, sizeof(msg), "%s%s", prefix, clean);
    }
    else if (plen < 141)
    {
        new keep = 144 - plen - 3;
        clean[keep] = EOS;
        format(msg, sizeof(msg), "%s%s...", prefix, clean);
    }
    else
    {
        format(msg, sizeof(msg), "%s", prefix);
        msg[144] = EOS;
    }
    SendClientMessage(playerid, color, msg);
}

static stock ResetHistSlot(slot)
{
    if (slot < 0 || slot >= DEMO_MAX_HIST_CHANNELS) return;
    gHistCid[slot] = INVALID_AUDIO_CHANNEL_ID;
    gTrackSerial[slot] = 0;
    gHistCount[slot] = 0;
    gHistGoingBack[slot] = false;
    gCurUrl[slot]{0} = EOS;
    gCurLabel[slot]{0} = EOS;
    for (new i = 0; i < DEMO_HIST_DEPTH; i++)
    {
        gHistUrl[slot][i]{0} = EOS;
        gHistLabel[slot][i]{0} = EOS;
    }
}

static stock InitHistSlots()
{
    for (new i = 0; i < DEMO_MAX_HIST_CHANNELS; i++)
    {
        ResetHistSlot(i);
    }
}

static stock FindHistSlot(cid)
{
    if (cid == INVALID_AUDIO_CHANNEL_ID) return -1;
    for (new i = 0; i < DEMO_MAX_HIST_CHANNELS; i++)
    {
        if (gHistCid[i] == cid) return i;
    }
    return -1;
}

static stock FindOrAllocHistSlot(cid)
{
    if (cid == INVALID_AUDIO_CHANNEL_ID) return -1;
    new existing = FindHistSlot(cid);
    if (existing != -1) return existing;

    for (new i = 0; i < DEMO_MAX_HIST_CHANNELS; i++)
    {
        if (gHistCid[i] == INVALID_AUDIO_CHANNEL_ID)
        {
            ResetHistSlot(i);
            gHistCid[i] = cid;
            return i;
        }
    }

    new cname[8], ccreator[8], listeners, E_SA_STREAM_STATE:st, bool:locked, qcount;
    for (new i = 0; i < DEMO_MAX_HIST_CHANNELS; i++)
    {
        if (!SA_GetAudioStreamChannelInfo(gHistCid[i], cname, sizeof(cname), ccreator, sizeof(ccreator), listeners, st, locked, qcount))
        {
            ResetHistSlot(i);
            gHistCid[i] = cid;
            return i;
        }
    }
    return -1;
}

static stock FreeHistSlot(cid)
{
    new slot = FindHistSlot(cid);
    if (slot != -1)
    {
        ResetHistSlot(slot);
    }
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
    gOptStreamText[playerid] = true;
    gSelChannelId[playerid] = gServerRadioId;
    gChListCount[playerid] = 0;
    gQueueViewCount[playerid] = 0;
    gQueueViewPage[playerid] = 0;
    gQueueViewStart[playerid] = 0;
    gQueueSelIdx[playerid] = -1;
    gQueueSelHash[playerid] = 0;
    gVoteCid[playerid] = INVALID_AUDIO_CHANNEL_ID;
    gVoteSerial[playerid] = 0;
    gNewChName[playerid][0] = EOS;
    gNewChPass[playerid][0] = EOS;
    gNewChMode[playerid] = SA_CHANNEL_DESTROY_MANUAL;
    gNewChTimeout[playerid] = 0;
    gLibFolder[playerid] = -1;
    gLibTrack[playerid] = -1;
    gLibFolderPage[playerid] = 0;
    gLibTrackPage[playerid] = 0;
    gLibViewCount[playerid] = 0;
    gPageNav[playerid] = 0;
    gSoundName[playerid][0] = EOS;
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
    static body[1024];

    format(body, sizeof(body),
        "{00FF88}1. Personal Playback Controls {AAAAAA}(Active Stream: %d)\n\
{00CCFF}2. Global & 3D Spatial Streams {AAAAAA}(Last Created: %d)\n\
{FFCC00}3. Search Music by Name {AAAAAA}(Async YouTube Search)\n\
{FF88FF}4. Synchronized Radio Channels {AAAAAA}(Channels: %d | Joined: %d)\n\
{00CCFF}5. Music Library {AAAAAA}(audio.music_dir | Folders: %d)\n\
{FFFFFF}6. Vanilla Hook Demo {AAAAAA}(Hooked PlayAudioStreamForPlayer)\n\
{FFFFFF}7. Toggle: OnAudioStreamProcessing Callback [%s{FFFFFF}]\n\
{FFFFFF}8. Toggle: Process Direct Audio Files [%s{FFFFFF}]\n\
{FFFFFF}9. Toggle: On-Screen Stream Text (stream_text) [%s{FFFFFF}]\n\
{FFFFFF}10. Toggle: Mute Streams (SA_SetPlayerAudioStreamMuted) [%s{FFFFFF}]\n\
{CCCCCC}11. Plugin Diagnostics & Stats {AAAAAA}(Heard: %s #%d)",
        sid,
        gLastGlobalSid[playerid],
        ch_count,
        cid,
        SA_GetAudioLibraryFolderCount(),
        gOptNotify[playerid] ? "{00FF88}ON" : "{FF5555}OFF",
        gOptProcessFiles[playerid] ? "{00FF88}TRUE" : "{FFAA00}FALSE",
        gOptStreamText[playerid] ? "{00FF88}SHOWN" : "{FFAA00}HIDDEN",
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
    static header[768];
    format(header, sizeof(header),
        "Play URL or Query {AAAAAA}(SA_PlayAudioStreamForPlayer)\n\
%s Personal Stream {AAAAAA}(SA_PauseAudioStreamForPlayer)\n\
Set Personal Volume 0-200%% {AAAAAA}(SA_SetPlayerAudioStreamVolume)\n\
Set Personal Range Distance {AAAAAA}(SA_SetPlayerAudioStreamDistance)\n\
Move Personal Stream Here (3D) {AAAAAA}(SA_SetPlayerAudioStreamPos)\n\
Stop Personal Stream {AAAAAA}(SA_StopAudioStreamForPlayer)\n\
Inspect Personal Stream Info {AAAAAA}(SA_GetAudioStreamInfo) [ID: %d]\n\
{00CCFF}Play Sound Effect {AAAAAA}(SA_PlayAudioStreamSoundForPlayer | audio.sound_dir)",
        is_paused ? "Resume" : "Pause",
        sid
    );
    ShowPlayerDialog(playerid, DLG_SA_PERSONAL_MENU, DIALOG_STYLE_LIST,
        "{00FF88}Personal Playback Controls", header, "Select", "Back");
}

static stock ShowGlobalMenu(playerid)
{
    static body[1024];
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
Stop Global Stream by ID {AAAAAA}(SA_StopAudioStream)\n\
Set Live Radio Text by ID {AAAAAA}(SA_SetAudioStreamLiveTitle)",
        gLastGlobalSid[playerid],
        gLastGlobalSid[playerid],
        gLastGlobalSid[playerid]
    );
    ShowPlayerDialog(playerid, DLG_SA_GLOBAL_MENU, DIALOG_STYLE_LIST,
        "{00CCFF}Global & 3D Spatial Controls", body, "Select", "Back");
}

static stock ShowChannelListMenu(playerid)
{
    static body[2048];
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

    new hslot = FindHistSlot(channelid);
    new hist_cnt = (hslot != -1) ? gHistCount[hslot] : 0;
    new bool:playlist_on = SA_IsAudioStreamChannelPlaylist(channelid);

    new title[96];
    static body[2048];
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
Radio Now Playing {AAAAAA}(SA_GetAudioStreamChannelLiveTitle)\n\
Set Radio Text {AAAAAA}(SA_SetAudioStreamChannelLiveTitle)\n\
Vote to Skip Track {AAAAAA}(listeners vote)\n\
Previous Track {AAAAAA}(history: %d)\n\
Toggle Playlist Mode {AAAAAA}(SA_SetAudioStreamChannelPlaylist: %s | Now: #%d)\n\
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
        hist_cnt,
        playlist_on ? "ON" : "OFF",
        SA_GetAudioStreamChannelQueuePos(channelid),
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

// Paged lists show a fixed number of items, then optional "Next Page" / "Previous Page" rows.
// Only one page is built per dialog, so large queues and libraries cost the same as small ones.
static stock AppendPageNav(playerid, body[], len, bool:has_next, bool:has_prev)
{
    gPageNav[playerid] = (has_next ? 1 : 0) | (has_prev ? 2 : 0);
    if (has_next) strcat(body, "{00CCFF}Next Page >>\n", len);
    if (has_prev) strcat(body, "{00CCFF}<< Previous Page\n", len);
}

// row: clicked row counted from the first row after the page items. Returns 1 (next), -1 (previous) or 0.
static stock GetPageNavAction(playerid, row)
{
    if (row < 0) return 0;
    if (gPageNav[playerid] & 1)
    {
        if (row == 0) return 1;
        row--;
    }
    if ((gPageNav[playerid] & 2) && row == 0) return -1;
    return 0;
}

static stock bool:GetChannelQueueDisplayText(channelid, index, out[], len = sizeof(out))
{
    if (SA_GetAudioStreamChannelQueueLabel(channelid, index, out, len) && out[0] != EOS)
    {
        SanitizeDialogField(out);
        return true;
    }
    if (SA_GetAudioStreamChannelQueue(channelid, index, out, len))
    {
        SanitizeDialogField(out);
        return true;
    }
    out[0] = EOS;
    return false;
}

static stock ShowChannelQueueDialog(playerid, channelid)
{
    new cname[40], ccreator[32], listeners, E_SA_STREAM_STATE:st, bool:locked, qcount;
    if (!SA_GetAudioStreamChannelInfo(channelid, cname, sizeof(cname), ccreator, sizeof(ccreator), listeners, st, locked, qcount))
    {
        ShowChannelListMenu(playerid);
        return;
    }

    new bool:playlist_on = SA_IsAudioStreamChannelPlaylist(channelid);
    new playlist_pos = playlist_on ? SA_GetAudioStreamChannelQueuePos(channelid) : -1;
    static body[3072];
    new item[132], line[160];
    format(body, sizeof(body),
        "{00FF88}+ Insert URL at Index 0 (Play Next) {AAAAAA}(SA_InsertAudioStreamChannelQueue)\n\
{FF8888}- Remove Queue Item by Index {AAAAAA}(SA_RemoveAudioStreamChannelQueue)\n\
{00CCFF}+ Import Playlist URL {AAAAAA}(SA_QueueChannelAudioStreamPlaylist)\n\
{FFCC00}> Seek Current Track {AAAAAA}(SA_SeekChannelAudioStream)\n\
{FF5555}x Clear All Queued Tracks {AAAAAA}(SA_ClearAudioStreamChannelQueue) [%d]\n\
{00CCFF}~ Shuffle Queue {AAAAAA}(SA_ShuffleAudioStreamChannelQueue) [%d]\n",
        qcount, qcount);
    new pages = (qcount > 0) ? (((qcount - 1) / DEMO_MAX_QUEUE_VIEW_ITEMS) + 1) : 1;
    new page = gQueueViewPage[playerid];
    if (page < 0) page = (playlist_pos >= 0) ? (playlist_pos / DEMO_MAX_QUEUE_VIEW_ITEMS) : 0; // open on the current item
    if (page >= pages) page = pages - 1;
    gQueueViewPage[playerid] = page;
    new start = page * DEMO_MAX_QUEUE_VIEW_ITEMS;
    gQueueViewStart[playerid] = start;
    gQueueViewCount[playerid] = 0;
    for (new i = start; i < qcount && gQueueViewCount[playerid] < DEMO_MAX_QUEUE_VIEW_ITEMS; i++)
    {
        if (!GetChannelQueueDisplayText(channelid, i, item, sizeof(item))) break;
        if (i == playlist_pos)
        {
            format(line, sizeof(line), "{EEDD82}> #%d: %s\n", i, item);
        }
        else
        {
            format(line, sizeof(line), "{FFCC00}#%d: {FFFFFF}%s\n", i, item);
        }
        if (strlen(body) + strlen(line) >= sizeof(body)) break;
        strcat(body, line);
        gQueueViewHash[playerid][gQueueViewCount[playerid]] = HashStringDjb2(item);
        gQueueViewCount[playerid]++;
    }
    AppendPageNav(playerid, body, sizeof(body), (start + gQueueViewCount[playerid] < qcount), (page > 0));

    new title[96];
    if (pages > 1)
    {
        format(title, sizeof(title), "%s (page %d/%d)",
            playlist_on ? "{FF88FF}Channel Playlist Actions" : "{FF88FF}Channel Queue Actions", page + 1, pages);
    }
    else
    {
        format(title, sizeof(title), "%s",
            playlist_on ? "{FF88FF}Channel Playlist Actions (current item highlighted)" : "{FF88FF}Channel Queue Actions");
    }
    ShowPlayerDialog(playerid, DLG_SA_CH_QUEUE_VIEW, DIALOG_STYLE_LIST, title, body, "Run", "Back");
}

static stock ShowChannelQueueItemDialog(playerid, channelid, idx)
{
    new title[96], item[132];
    if (GetChannelQueueDisplayText(channelid, idx, item, sizeof(item)))
    {
        format(title, sizeof(title), "{00CCFF}Item #%d: %.48s", idx, item);
    }
    else
    {
        format(title, sizeof(title), "{00CCFF}Queue Item #%d Actions", idx);
    }
    ShowPlayerDialog(playerid, DLG_SA_CH_QUEUE_ITEM, DIALOG_STYLE_LIST, title,
        "Move to Top\nMove Up\nMove Down\n{FF8888}Remove\n{00FF88}Play This Item Now {AAAAAA}(SA_PlayAudioStreamChannelQueue)", "Apply", "Back");
}

static stock HandleChannelVoteSkip(playerid, cid)
{
    new cname[40], ccreator[32], listeners, E_SA_STREAM_STATE:st, bool:locked, qcount;
    if (!SA_GetAudioStreamChannelInfo(cid, cname, sizeof(cname), ccreator, sizeof(ccreator), listeners, st, locked, qcount))
    {
        SendClientMessage(playerid, 0xFFAA00FF, "[Channel] That channel no longer exists.");
        ShowChannelListMenu(playerid);
        return;
    }

    new bool:is_listener = false;
    for (new i = 0; i < listeners; i++)
    {
        if (SA_GetAudioStreamChannelListener(cid, i) == playerid)
        {
            is_listener = true;
            break;
        }
    }
    if (!is_listener)
    {
        SendClientMessage(playerid, 0xFFAA00FF, "[Channel] Only listeners of this channel can vote.");
        ShowChannelManageMenu(playerid, cid);
        return;
    }

    if (st != SA_STATE_PLAYING)
    {
        SendClientMessage(playerid, 0xFFAA00FF, "[Channel] Nothing is playing right now.");
        ShowChannelManageMenu(playerid, cid);
        return;
    }

    new slot = FindOrAllocHistSlot(cid);
    if (slot == -1)
    {
        SendClientMessage(playerid, 0xFF5555FF, "[Channel] Voting is unavailable for this channel.");
        ShowChannelManageMenu(playerid, cid);
        return;
    }
    if (gTrackSerial[slot] == 0)
    {
        if (++gTrackSerialCounter == 0) gTrackSerialCounter = 1;
        gTrackSerial[slot] = gTrackSerialCounter;
    }

    if (gVoteCid[playerid] == cid && gVoteSerial[playerid] == gTrackSerial[slot])
    {
        SendClientMessage(playerid, 0xFFAA00FF, "[Channel] You already voted to skip this track.");
        ShowChannelManageMenu(playerid, cid);
        return;
    }

    gVoteCid[playerid] = cid;
    gVoteSerial[playerid] = gTrackSerial[slot];

    new votes = 0;
    for (new i = 0; i < listeners; i++)
    {
        new lp = SA_GetAudioStreamChannelListener(cid, i);
        if (lp != INVALID_PLAYER_ID && IsPlayerConnected(lp) &&
            gVoteCid[lp] == cid && gVoteSerial[lp] == gTrackSerial[slot])
        {
            votes++;
        }
    }
    new needed = (listeners / 2) + 1;
    if (needed < 1) needed = 1;

    new pname[MAX_PLAYER_NAME + 1], msg[144];
    GetPlayerName(playerid, pname, sizeof(pname));
    format(msg, sizeof(msg), "[Channel] %s voted to skip (%d/%d).", pname, votes, needed);
    for (new i = 0; i < listeners; i++)
    {
        new lp = SA_GetAudioStreamChannelListener(cid, i);
        if (lp != INVALID_PLAYER_ID && IsPlayerConnected(lp))
        {
            SendClientMessage(lp, 0x00CCFFFF, msg);
        }
    }

    if (votes >= needed)
    {
        SA_SkipChannelAudioStream(cid);
        if (++gTrackSerialCounter == 0) gTrackSerialCounter = 1;
        gTrackSerial[slot] = gTrackSerialCounter;
        for (new i = 0; i < listeners; i++)
        {
            new lp = SA_GetAudioStreamChannelListener(cid, i);
            if (lp != INVALID_PLAYER_ID && IsPlayerConnected(lp))
            {
                SendClientMessage(lp, 0x00FF88FF, "[Channel] Vote passed: skipping track.");
            }
        }
    }
    ShowChannelManageMenu(playerid, cid);
}

static stock HandleChannelPreviousTrack(playerid, cid)
{
    if (!EnsureChannelManagePermission(playerid, cid)) return;

    new cname[40], ccreator[32], listeners, E_SA_STREAM_STATE:st, bool:locked, qcount;
    if (!SA_GetAudioStreamChannelInfo(cid, cname, sizeof(cname), ccreator, sizeof(ccreator), listeners, st, locked, qcount))
    {
        SendClientMessage(playerid, 0xFFAA00FF, "[Channel] That channel no longer exists.");
        ShowChannelListMenu(playerid);
        return;
    }

    if (st != SA_STATE_PLAYING)
    {
        SendClientMessage(playerid, 0xFFAA00FF, "[Channel] Nothing is playing right now.");
        ShowChannelManageMenu(playerid, cid);
        return;
    }

    new slot = FindHistSlot(cid);
    if (slot == -1 || gHistCount[slot] <= 0)
    {
        SendClientMessage(playerid, 0xFFAA00FF, "[Channel] No previous track in history.");
        ShowChannelManageMenu(playerid, cid);
        return;
    }

    new cur_u[256], cur_l[128], prev_u[256], prev_l[128];
    new bool:inserted_cur = false;
    if (gCurUrl[slot]{0} != EOS)
    {
        strunpack(cur_u, gCurUrl[slot]);
        strunpack(cur_l, gCurLabel[slot]);
        if (!SA_InsertAudioStreamChannelQueue(cid, 0, cur_u, cur_l))
        {
            SendClientMessage(playerid, 0xFF5555FF, "[Channel] Queue is full.");
            ShowChannelManageMenu(playerid, cid);
            return;
        }
        inserted_cur = true;
    }

    new top = gHistCount[slot] - 1;
    strunpack(prev_u, gHistUrl[slot][top]);
    strunpack(prev_l, gHistLabel[slot][top]);
    if (!SA_InsertAudioStreamChannelQueue(cid, 0, prev_u, prev_l))
    {
        if (inserted_cur)
        {
            SA_RemoveAudioStreamChannelQueue(cid, 0);
        }
        SendClientMessage(playerid, 0xFF5555FF, "[Channel] Queue is full.");
        ShowChannelManageMenu(playerid, cid);
        return;
    }

    gHistCount[slot] = top;
    gHistUrl[slot][top]{0} = EOS;
    gHistLabel[slot][top]{0} = EOS;
    gHistGoingBack[slot] = true;
    SA_SkipChannelAudioStream(cid);
    SendClientMessage(playerid, 0x00FF88FF, "[Channel] Going back to the previous track.");
    ShowChannelManageMenu(playerid, cid);
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

    static body[1536];
    new line[96], pname[32];
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

static stock ShowChannelPersistDialog(playerid)
{
    ShowPlayerDialog(playerid, DLG_SA_CH_CREATE_PERSIST, DIALOG_STYLE_LIST,
        "{00FF88}Create Channel (4/4) - Persistence",
        "Persistent {AAAAAA}(persistent = true: saved to disk and restored after restarts)\n\
Temporary {AAAAAA}(persistent = false: memory only, destroyed on GMX / restart)",
        "Create", "Back");
}

static stock bool:GetLibraryFolderRef(folder_index, out[], len = sizeof(out))
{
    new folder[64], tracks;
    if (!SA_GetAudioLibraryFolder(folder_index, folder, sizeof(folder), tracks))
    {
        return false;
    }
    if (folder[0] == EOS)
    {
        format(out, len, "library://");
    }
    else
    {
        format(out, len, "library://%s", folder);
    }
    return true;
}

static stock bool:GetLibraryTrackRef(folder_index, track_index, out[], len, name[], name_len)
{
    new folder[64], tracks, track[128];
    if (!SA_GetAudioLibraryFolder(folder_index, folder, sizeof(folder), tracks) ||
        !SA_GetAudioLibraryTrack(folder_index, track_index, track, sizeof(track)))
    {
        return false;
    }
    if (folder[0] == EOS)
    {
        format(out, len, "library:///%s", track);
    }
    else
    {
        format(out, len, "library://%s/%s", folder, track);
    }
    format(name, name_len, "%s", track);
    return true;
}

static stock ShowLibraryFolderList(playerid)
{
    new count = SA_GetAudioLibraryFolderCount();
    static body[4096];
    new line[112], name[64], tracks;
    format(body, sizeof(body), "{00FF88}Refresh Library {AAAAAA}(SA_RefreshAudioLibrary | %d folders cached)\n", count);
    if (count == 0)
    {
        strcat(body, "{666666}(Empty: add audio files to audio.music_dir, then Refresh)\n");
    }
    new pages = (count > 0) ? (((count - 1) / DEMO_MAX_LIBRARY_FOLDERS) + 1) : 1;
    new page = gLibFolderPage[playerid];
    if (page < 0) page = 0;
    if (page >= pages) page = pages - 1;
    gLibFolderPage[playerid] = page;
    new start = page * DEMO_MAX_LIBRARY_FOLDERS;
    gLibViewCount[playerid] = 0;
    for (new i = start; i < count && gLibViewCount[playerid] < DEMO_MAX_LIBRARY_FOLDERS; i++)
    {
        if (!SA_GetAudioLibraryFolder(i, name, sizeof(name), tracks)) break;
        SanitizeDialogField(name);
        format(line, sizeof(line), "{FFFFFF}%.48s {AAAAAA}(%d tracks)\n", (name[0] == EOS) ? "(root files)" : name, tracks);
        if (strlen(body) + strlen(line) >= sizeof(body)) break;
        strcat(body, line);
        gLibViewCount[playerid]++;
    }
    AppendPageNav(playerid, body, sizeof(body), (start + gLibViewCount[playerid] < count), (page > 0));

    new title[80];
    if (pages > 1)
    {
        format(title, sizeof(title), "{00CCFF}Music Library (SA_GetAudioLibraryFolder) - page %d/%d", page + 1, pages);
    }
    else
    {
        format(title, sizeof(title), "{00CCFF}Music Library (SA_GetAudioLibraryFolder)");
    }
    ShowPlayerDialog(playerid, DLG_SA_LIB_FOLDERS, DIALOG_STYLE_LIST, title, body, "Open", "Back");
}

static stock ShowLibraryFolderMenu(playerid)
{
    new fidx = gLibFolder[playerid];
    new name[64], tracks;
    if (fidx < 0 || !SA_GetAudioLibraryFolder(fidx, name, sizeof(name), tracks))
    {
        SendClientMessage(playerid, 0xFFAA00FF, "[Library] The library changed; pick the folder again.");
        ShowLibraryFolderList(playerid);
        return;
    }
    new raw_name[64];
    format(raw_name, sizeof(raw_name), "%s", name);
    SanitizeDialogField(name);

    new cid = gSelChannelId[playerid];
    static body[4096];
    new line[160], track[128], ref[256];
    format(body, sizeof(body),
        "{00CCFF}+ Import Whole Folder into Channel #%d {AAAAAA}(SA_QueueChannelAudioStreamPlaylist; queued tracks in {EEDD82}yellow{AAAAAA})\n",
        cid);
    new pages = (tracks > 0) ? (((tracks - 1) / DEMO_MAX_LIBRARY_TRACKS) + 1) : 1;
    new page = gLibTrackPage[playerid];
    if (page < 0) page = 0;
    if (page >= pages) page = pages - 1;
    gLibTrackPage[playerid] = page;
    new start = page * DEMO_MAX_LIBRARY_TRACKS;
    gLibViewCount[playerid] = 0;
    for (new i = start; i < tracks && gLibViewCount[playerid] < DEMO_MAX_LIBRARY_TRACKS; i++)
    {
        if (!SA_GetAudioLibraryTrack(fidx, i, track, sizeof(track))) break;
        if (raw_name[0] == EOS)
        {
            format(ref, sizeof(ref), "library:///%s", track);
        }
        else
        {
            format(ref, sizeof(ref), "library://%s/%s", raw_name, track);
        }
        // One native call per track: the plugin searches the whole channel queue in C++.
        new queued = SA_FindAudioStreamChannelQueue(cid, ref);
        SanitizeDialogField(track);
        if (queued != -1)
        {
            format(line, sizeof(line), "{EEDD82}%.56s {AAAAAA}(queued #%d)\n", track, queued);
        }
        else
        {
            format(line, sizeof(line), "{FFFFFF}%.56s\n", track);
        }
        if (strlen(body) + strlen(line) >= sizeof(body)) break;
        strcat(body, line);
        gLibViewCount[playerid]++;
    }
    AppendPageNav(playerid, body, sizeof(body), (start + gLibViewCount[playerid] < tracks), (page > 0));

    new title[96];
    if (pages > 1)
    {
        format(title, sizeof(title), "{00CCFF}%.40s (%d tracks, page %d/%d)",
            (name[0] == EOS) ? "(root files)" : name, tracks, page + 1, pages);
    }
    else
    {
        format(title, sizeof(title), "{00CCFF}%.40s (%d tracks)", (name[0] == EOS) ? "(root files)" : name, tracks);
    }
    ShowPlayerDialog(playerid, DLG_SA_LIB_FOLDER, DIALOG_STYLE_LIST, title, body, "Select", "Back");
}

static stock ShowLibraryTrackMenu(playerid)
{
    new ref[256], track[128];
    if (!GetLibraryTrackRef(gLibFolder[playerid], gLibTrack[playerid], ref, sizeof(ref), track, sizeof(track)))
    {
        SendClientMessage(playerid, 0xFFAA00FF, "[Library] The library changed; pick the track again.");
        ShowLibraryFolderList(playerid);
        return;
    }
    SanitizeDialogField(track);
    new queued = SA_FindAudioStreamChannelQueue(gSelChannelId[playerid], ref);
    new queued_note[48];
    if (queued != -1)
    {
        format(queued_note, sizeof(queued_note), " {EEDD82}[already queued at #%d]", queued);
    }
    new title[96];
    static body[512];
    format(title, sizeof(title), "{00CCFF}Track: %.60s", track);
    format(body, sizeof(body),
        "Play Privately {AAAAAA}(SA_PlayAudioStreamForPlayer)\n\
Play Global 3D Here {AAAAAA}(SA_PlayAudioStream, 45m)\n\
Add to Channel #%d Queue {AAAAAA}(SA_QueueChannelAudioStream)%s\n\
Play Now on Channel #%d {AAAAAA}(SA_PlayChannelAudioStream)",
        gSelChannelId[playerid], queued_note, gSelChannelId[playerid]);
    ShowPlayerDialog(playerid, DLG_SA_LIB_TRACK, DIALOG_STYLE_LIST, title, body, "Run", "Back");
}

static stock ShowSoundInputDialog(playerid)
{
    ShowPlayerDialog(playerid, DLG_SA_SOUND_INPUT, DIALOG_STYLE_INPUT,
        "{00CCFF}Play Sound Effect (SA_PlayAudioStreamSoundForPlayer)",
        "{FFFFFF}Enter a file name inside audio.sound_dir (scriptfiles/SampAudioStream/sound):\n\n\
{AAAAAA}Examples: beep.mp3  or  alerts/siren.wav  (.mp3, .ogg, .wav up to 5 MB)\n\
{AAAAAA}The sound plays over whatever you hear, then your previous audio resumes.",
        "Next", "Back");
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

    static info[1536];
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
    InitHistSlots();
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
        gServerRadioId = SA_CreateAudioStreamChannel(DEMO_SERVER_RADIO_NAME, .persistent = true);
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
    InitHistSlots();
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
    if (dialogid < DEMO_DLG_BASE || dialogid >= DLG_SA_LAST)
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
                    gLibFolderPage[playerid] = 0;
                    ShowLibraryFolderList(playerid);
                }
                case 5:
                {
                    ShowPlayerDialog(playerid, DLG_SA_VANILLA_PLAY, DIALOG_STYLE_INPUT,
                        "{FFFFFF}Hooked PlayAudioStreamForPlayer Demo",
                        "{FFFFFF}Calls standard PlayAudioStreamForPlayer with extended plugin volume=80.0:\n\n{AAAAAA}Enter a YouTube URL, direct MP3/radio link, or song name:",
                        "Play", "Back");
                }
                case 6:
                {
                    gOptNotify[playerid] = !gOptNotify[playerid];
                    SendClientMessage(playerid, 0x00CCFFFF,
                        gOptNotify[playerid] ?
                        "[Options] OnAudioStreamProcessing callback ENABLED for your plays." :
                        "[Options] OnAudioStreamProcessing callback DISABLED (.notify_processing = false).");
                    ShowSaMainMenu(playerid);
                }
                case 7:
                {
                    gOptProcessFiles[playerid] = !gOptProcessFiles[playerid];
                    SendClientMessage(playerid, 0x00CCFFFF,
                        gOptProcessFiles[playerid] ?
                        "[Options] Direct audio files (.mp3/.ogg/.wav/.flac/.aac/.m4a/.opus) will be TRANSCODED by plugin." :
                        "[Options] Direct audio files use zero-slot passthrough (default).");
                    ShowSaMainMenu(playerid);
                }
                case 8:
                {
                    gOptStreamText[playerid] = !gOptStreamText[playerid];
                    SendClientMessage(playerid, 0x00CCFFFF,
                        gOptStreamText[playerid] ?
                        "[Options] On-screen stream text SHOWN for your plays (.stream_text = true)." :
                        "[Options] On-screen stream text HIDDEN for server-processed plays (.stream_text = false).");
                    if (!gOptStreamText[playerid] && !gOptProcessFiles[playerid])
                    {
                        SendClientMessage(playerid, 0xAAAAAAFF,
                            "[Options] Direct links on passthrough still show the radio's own text; enable Process Direct Audio Files to hide it.");
                    }
                    ShowSaMainMenu(playerid);
                }
                case 9:
                {
                    new bool:next_mute = !SA_IsPlayerAudioStreamMuted(playerid);
                    SA_SetPlayerAudioStreamMuted(playerid, next_mute, false);
                    SendClientMessage(playerid, 0x00CCFFFF,
                        next_mute ?
                        "[Mute] Channel/global/range audio streams MUTED for you." :
                        "[Mute] Audio streams UNMUTED.");
                    ShowSaMainMenu(playerid);
                }
                case 10: ShowDiagnosticsDialog(playerid);
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
                case 7: ShowSoundInputDialog(playerid);
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
                .process_direct_audio_files = gOptProcessFiles[playerid],
                .stream_text = gOptStreamText[playerid]
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
                case 11:
                {
                    ShowPlayerDialog(playerid, DLG_SA_GLOBAL_LIVE_SET_ID, DIALOG_STYLE_INPUT,
                        "{00CCFF}Set Live Radio Text by ID",
                        "{FFFFFF}Enter Stream ID and text separated by space (e.g. '2 My Radio Text', or '2' to clear):",
                        "Apply", "Back");
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
                gOptProcessFiles[playerid],
                gOptStreamText[playerid]
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
                gOptProcessFiles[playerid],
                gOptStreamText[playerid]
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

                new live[280];
                if (SA_GetAudioStreamLiveTitle(sid, live, sizeof(live)) && live[0] != EOS)
                {
                    SendTruncatedLiveMessage(playerid, "Live radio text: ", live, 0x00CCFFFF);
                }
                else
                {
                    SendClientMessage(playerid, 0xAAAAAAFF, "Live radio text: (none)");
                }
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

            static body[640];
            format(body, sizeof(body),
                "Play For Me Only {AAAAAA}(SA_PlayAudioStreamForPlayer)\n\
Play Global 3D Here {AAAAAA}(SA_PlayAudioStream 45m)\n\
Queue on Selected Channel #%d {AAAAAA}(SA_QueueChannelAudioStream)\n\
Insert at Top of Selected Channel #%d {AAAAAA}(SA_InsertAudioStreamChannelQueue)\n\
Play Now on Selected Channel #%d {AAAAAA}(SA_PlayChannelAudioStream)",
                gSelChannelId[playerid],
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
            new vid[24], title[64], artist[48], label[120], url[96];
            strunpack(vid, gSearchVid[playerid][idx]);
            strunpack(title, gSearchTitle[playerid][idx]);
            strunpack(artist, gSearchArtist[playerid][idx]);
            if (artist[0] != EOS)
            {
                format(label, sizeof(label), "%s - %s", artist, title);
            }
            else
            {
                format(label, sizeof(label), "%s", title);
            }
            format(url, sizeof(url), "https://www.youtube.com/watch?v=%s", vid);

            switch (listitem)
            {
                case 0:
                {
                    SA_PlayAudioStreamForPlayer(playerid, url, .notify_processing = gOptNotify[playerid],
                        .stream_text = gOptStreamText[playerid]);
                    SendClientMessage(playerid, 0x00FF88FF, "[Search] Starting selected track privately...");
                }
                case 1:
                {
                    new Float:x, Float:y, Float:z;
                    GetPlayerPos(playerid, x, y, z);
                    new sid = SA_PlayAudioStream(url, x, y, z, 45.0, true,
                        GetPlayerVirtualWorld(playerid), GetPlayerInterior(playerid), 100.0, gOptNotify[playerid],
                        .stream_text = gOptStreamText[playerid]);
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
                    if (SA_QueueChannelAudioStream(gSelChannelId[playerid], url, label))
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
                    if (SA_InsertAudioStreamChannelQueue(target_cid, 0, url, label))
                    {
                        SendClientMessage(playerid, 0x00FF88FF, "[Search] Track inserted at front of channel queue.");
                    }
                    else
                    {
                        SendClientMessage(playerid, 0xFF5555FF, "[Search] Target channel unavailable or queue full.");
                    }
                }
                case 4:
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
                    "{00FF88}Create Channel (1/4) - Name",
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
                    "{00FF88}Create Channel (1/4) - Name",
                    "{FFFFFF}Enter a name for the new synchronized channel:",
                    "Next", "Back");
                return 1;
            }
            format(gNewChName[playerid], 48, "%s", inputtext);
            ShowPlayerDialog(playerid, DLG_SA_CH_CREATE_PASS, DIALOG_STYLE_PASSWORD,
                "{00FF88}Create Channel (2/4) - Password",
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
                "{00FF88}Create Channel (3/4) - Auto-Destroy Policy",
                "Manual {AAAAAA}(Never expires automatically)\n\
On Creator Disconnect {AAAAAA}(Destroyed when you leave)\n\
Offline Timeout (60s) {AAAAAA}(Timer resets whenever you are online)\n\
Offline Timeout (Custom seconds)",
                "Next", "Back");
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
                    "Next", "Back");
                return 1;
            }

            gNewChMode[playerid] = SA_CHANNEL_DESTROY_MANUAL;
            gNewChTimeout[playerid] = 0;
            if (listitem == 1) gNewChMode[playerid] = SA_CHANNEL_DESTROY_ON_DISCONNECT;
            else if (listitem == 2) { gNewChMode[playerid] = SA_CHANNEL_DESTROY_ON_TIMEOUT; gNewChTimeout[playerid] = 60; }
            ShowChannelPersistDialog(playerid);
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
            gNewChMode[playerid] = SA_CHANNEL_DESTROY_ON_TIMEOUT;
            gNewChTimeout[playerid] = timeout_sec;
            ShowChannelPersistDialog(playerid);
            return 1;
        }

        case DLG_SA_CH_CREATE_PERSIST:
        {
            if (!response)
            {
                ShowChannelListMenu(playerid);
                return 1;
            }
            new bool:persistent = (listitem == 0);
            new cid = SA_CreateAudioStreamChannel(gNewChName[playerid], playerid, gNewChPass[playerid],
                gNewChMode[playerid], gNewChTimeout[playerid], persistent);
            if (cid == INVALID_AUDIO_CHANNEL_ID)
            {
                SendClientMessage(playerid, 0xFF5555FF, "[Channel] Failed to create channel.");
                ShowChannelListMenu(playerid);
            }
            else
            {
                new msg[144], d_lbl[20];
                GetDestroyLabel(gNewChMode[playerid], d_lbl, sizeof(d_lbl));
                format(msg, sizeof(msg), "[Channel] Created #%d (%s) | %s | %s.",
                    cid, gNewChName[playerid], d_lbl,
                    persistent ? "Persistent (saved to disk)" : "Temporary (memory only)");
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
                    gQueueViewPage[playerid] = -1;
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
                case 16:
                {
                    new live[280], box[384], title[64];
                    format(title, sizeof(title), "{00CCFF}Channel #%d Radio Now Playing", cid);
                    if (SA_GetAudioStreamChannelLiveTitle(cid, live, sizeof(live)) && live[0] != EOS)
                    {
                        SanitizeDialogField(live);
                        format(box, sizeof(box), "{FFFFFF}%s", live);
                    }
                    else
                    {
                        format(box, sizeof(box), "{AAAAAA}No live radio text yet.");
                    }
                    ShowPlayerDialog(playerid, DLG_SA_CH_LIVE_VIEW, DIALOG_STYLE_MSGBOX, title, box, "Back", "");
                }
                case 17:
                {
                    if (!EnsureChannelManagePermission(playerid, cid)) return 1;
                    ShowPlayerDialog(playerid, DLG_SA_CH_LIVE_SET, DIALOG_STYLE_INPUT,
                        "{00CCFF}Set Channel Live Radio Text",
                        "{FFFFFF}Enter custom live radio text for this channel (leave blank to clear):",
                        "Save", "Back");
                }
                case 18: HandleChannelVoteSkip(playerid, cid);
                case 19: HandleChannelPreviousTrack(playerid, cid);
                case 20:
                {
                    if (!EnsureChannelManagePermission(playerid, cid)) return 1;
                    new bool:enable = !SA_IsAudioStreamChannelPlaylist(cid);
                    if (SA_SetAudioStreamChannelPlaylist(cid, enable))
                    {
                        SendClientMessage(playerid, 0x00FF88FF,
                            enable ?
                            "[Channel] Playlist mode ON: items stay in the list after playing and Skip advances the position." :
                            "[Channel] Playlist mode OFF: classic queue (items are consumed as they play).");
                    }
                    ShowChannelManageMenu(playerid, cid);
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
                case 5:
                {
                    if (SA_ShuffleAudioStreamChannelQueue(cid))
                    {
                        SendClientMessage(playerid, 0x00FF88FF, "[Channel] Queue shuffled.");
                    }
                    ShowChannelQueueDialog(playerid, cid);
                }
                default:
                {
                    new idx = listitem - 6;
                    if (idx >= gQueueViewCount[playerid])
                    {
                        gQueueViewPage[playerid] += GetPageNavAction(playerid, idx - gQueueViewCount[playerid]);
                        ShowChannelQueueDialog(playerid, cid);
                        return 1;
                    }
                    new real_idx = gQueueViewStart[playerid] + idx;
                    new cur_item[132];
                    if (idx < 0 || !GetChannelQueueDisplayText(cid, real_idx, cur_item, sizeof(cur_item)))
                    {
                        SendClientMessage(playerid, 0xFFAA00FF,
                            "[Channel] Queue changed before selection; refreshing list.");
                        ShowChannelQueueDialog(playerid, cid);
                        return 1;
                    }
                    if (HashStringDjb2(cur_item) != gQueueViewHash[playerid][idx])
                    {
                        SendClientMessage(playerid, 0xFFAA00FF,
                            "[Channel] Queue changed before selection; refreshing list.");
                        ShowChannelQueueDialog(playerid, cid);
                        return 1;
                    }
                    gQueueSelIdx[playerid] = real_idx;
                    gQueueSelHash[playerid] = gQueueViewHash[playerid][idx];
                    ShowChannelQueueItemDialog(playerid, cid, real_idx);
                }
            }
            return 1;
        }

        case DLG_SA_CH_QUEUE_ITEM:
        {
            new cid = gSelChannelId[playerid];
            if (!EnsureChannelManagePermission(playerid, cid)) return 1;
            if (!response)
            {
                ShowChannelQueueDialog(playerid, cid);
                return 1;
            }
            new idx = gQueueSelIdx[playerid];
            new cur_item[132];
            if (idx < 0 || !GetChannelQueueDisplayText(cid, idx, cur_item, sizeof(cur_item)) ||
                HashStringDjb2(cur_item) != gQueueSelHash[playerid])
            {
                SendClientMessage(playerid, 0xFFAA00FF,
                    "[Channel] Queue changed before selection; refreshing list.");
                ShowChannelQueueDialog(playerid, cid);
                return 1;
            }

            new cname[40], ccreator[32], listeners, E_SA_STREAM_STATE:st, bool:locked, qcount;
            if (!SA_GetAudioStreamChannelInfo(cid, cname, sizeof(cname), ccreator, sizeof(ccreator), listeners, st, locked, qcount))
            {
                ShowChannelListMenu(playerid);
                return 1;
            }

            new msg[80];
            switch (listitem)
            {
                case 0:
                {
                    if (idx == 0)
                    {
                        SendClientMessage(playerid, 0xFFAA00FF, "[Channel] Item is already at the top.");
                    }
                    else if (SA_MoveAudioStreamChannelQueue(cid, idx, 0))
                    {
                        format(msg, sizeof(msg), "[Channel] Moved queue item #%d to #0.", idx);
                        SendClientMessage(playerid, 0x00FF88FF, msg);
                    }
                }
                case 1:
                {
                    if (idx <= 0)
                    {
                        SendClientMessage(playerid, 0xFFAA00FF, "[Channel] Item is already at the top.");
                    }
                    else if (SA_MoveAudioStreamChannelQueue(cid, idx, idx - 1))
                    {
                        format(msg, sizeof(msg), "[Channel] Moved queue item #%d to #%d.", idx, idx - 1);
                        SendClientMessage(playerid, 0x00FF88FF, msg);
                    }
                }
                case 2:
                {
                    if (idx + 1 >= qcount)
                    {
                        SendClientMessage(playerid, 0xFFAA00FF, "[Channel] Item is already at the bottom.");
                    }
                    else if (SA_MoveAudioStreamChannelQueue(cid, idx, idx + 1))
                    {
                        format(msg, sizeof(msg), "[Channel] Moved queue item #%d to #%d.", idx, idx + 1);
                        SendClientMessage(playerid, 0x00FF88FF, msg);
                    }
                }
                case 3:
                {
                    if (SA_RemoveAudioStreamChannelQueue(cid, idx))
                    {
                        format(msg, sizeof(msg), "[Channel] Removed queue item #%d.", idx);
                        SendClientMessage(playerid, 0x00FF88FF, msg);
                    }
                }
                case 4:
                {
                    if (SA_PlayAudioStreamChannelQueue(cid, idx))
                    {
                        format(msg, sizeof(msg), SA_IsAudioStreamChannelPlaylist(cid) ?
                            "[Channel] Playing playlist item #%d now." :
                            "[Channel] Playing queue item #%d now (removed from the queue).", idx);
                        SendClientMessage(playerid, 0x00FF88FF, msg);
                    }
                    else
                    {
                        SendClientMessage(playerid, 0xFF5555FF, "[Channel] Could not start that item (stream limit reached?).");
                    }
                }
            }
            ShowChannelQueueDialog(playerid, cid);
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
                gOptProcessFiles[playerid],
                gOptStreamText[playerid]
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

        case DLG_SA_CH_LIVE_VIEW:
        {
            new cid = gSelChannelId[playerid];
            if (!EnsureSelectedChannelExists(playerid, cid)) return 1;
            ShowChannelManageMenu(playerid, cid);
            return 1;
        }

        case DLG_SA_CH_LIVE_SET:
        {
            new cid = gSelChannelId[playerid];
            if (!response)
            {
                if (!EnsureSelectedChannelExists(playerid, cid)) return 1;
                ShowChannelManageMenu(playerid, cid);
                return 1;
            }
            if (!EnsureChannelManagePermission(playerid, cid)) return 1;
            if (SA_SetAudioStreamChannelLiveTitle(cid, inputtext))
            {
                SendClientMessage(playerid, 0x00FF88FF, "[Channel] Radio text updated.");
            }
            else
            {
                SendClientMessage(playerid, 0xFF5555FF, "[Channel] Failed to update radio text (no active track playing).");
            }
            ShowChannelManageMenu(playerid, cid);
            return 1;
        }

        // ---------------- Music Library ----------------
        case DLG_SA_LIB_FOLDERS:
        {
            if (!response)
            {
                ShowSaMainMenu(playerid);
                return 1;
            }
            if (listitem == 0)
            {
                new folders = SA_RefreshAudioLibrary();
                new msg[96];
                format(msg, sizeof(msg), "[Library] Rescanned audio.music_dir: %d folder(s) with audio files.", folders);
                SendClientMessage(playerid, 0x00FF88FF, msg);
                ShowLibraryFolderList(playerid);
                return 1;
            }
            new row = listitem - 1;
            if (row >= gLibViewCount[playerid])
            {
                gLibFolderPage[playerid] += GetPageNavAction(playerid, row - gLibViewCount[playerid]);
                ShowLibraryFolderList(playerid);
                return 1;
            }
            new idx = gLibFolderPage[playerid] * DEMO_MAX_LIBRARY_FOLDERS + row;
            new name[64], tracks;
            if (!SA_GetAudioLibraryFolder(idx, name, sizeof(name), tracks))
            {
                ShowLibraryFolderList(playerid);
                return 1;
            }
            gLibFolder[playerid] = idx;
            gLibTrackPage[playerid] = 0;
            ShowLibraryFolderMenu(playerid);
            return 1;
        }

        case DLG_SA_LIB_FOLDER:
        {
            if (!response)
            {
                ShowLibraryFolderList(playerid);
                return 1;
            }
            if (listitem == 0)
            {
                new cid = gSelChannelId[playerid];
                if (!EnsureSelectedChannelExists(playerid, cid)) return 1;
                if (!EnsureChannelManagePermission(playerid, cid, false))
                {
                    ShowLibraryFolderMenu(playerid);
                    return 1;
                }
                new ref[96];
                if (!GetLibraryFolderRef(gLibFolder[playerid], ref, sizeof(ref)))
                {
                    ShowLibraryFolderList(playerid);
                    return 1;
                }
                if (SA_QueueChannelAudioStreamPlaylist(cid, ref, 1000)) // clamped to channels.max_queue_per_channel
                {
                    SendClientMessage(playerid, 0x00FF88FF,
                        "[Library] Folder import scheduled; OnAudioStreamChannelPlaylistQueued reports the result.");
                }
                else
                {
                    SendClientMessage(playerid, 0xFF5555FF, "[Library] Import refused (channel busy importing or unavailable).");
                }
                ShowLibraryFolderMenu(playerid);
                return 1;
            }
            new row = listitem - 1;
            if (row >= gLibViewCount[playerid])
            {
                gLibTrackPage[playerid] += GetPageNavAction(playerid, row - gLibViewCount[playerid]);
                ShowLibraryFolderMenu(playerid);
                return 1;
            }
            gLibTrack[playerid] = gLibTrackPage[playerid] * DEMO_MAX_LIBRARY_TRACKS + row;
            ShowLibraryTrackMenu(playerid);
            return 1;
        }

        case DLG_SA_LIB_TRACK:
        {
            if (!response)
            {
                ShowLibraryFolderMenu(playerid);
                return 1;
            }
            new ref[256], track[128];
            if (!GetLibraryTrackRef(gLibFolder[playerid], gLibTrack[playerid], ref, sizeof(ref), track, sizeof(track)))
            {
                SendClientMessage(playerid, 0xFFAA00FF, "[Library] The library changed; pick the track again.");
                ShowLibraryFolderList(playerid);
                return 1;
            }
            new cid = gSelChannelId[playerid];
            switch (listitem)
            {
                case 0:
                {
                    new sid = SA_PlayAudioStreamForPlayer(playerid, ref,
                        .notify_processing = gOptNotify[playerid],
                        .stream_text = gOptStreamText[playerid]);
                    SendClientMessage(playerid, (sid != INVALID_AUDIO_STREAM_ID) ? 0x00FF88FF : 0xFF5555FF,
                        (sid != INVALID_AUDIO_STREAM_ID) ? "[Library] Playing the track privately." : "[Library] Failed to start the track.");
                }
                case 1:
                {
                    new Float:x, Float:y, Float:z;
                    GetPlayerPos(playerid, x, y, z);
                    new sid = SA_PlayAudioStream(ref, x, y, z, 45.0, true,
                        GetPlayerVirtualWorld(playerid), GetPlayerInterior(playerid), 100.0, gOptNotify[playerid],
                        .stream_text = gOptStreamText[playerid]);
                    if (sid == INVALID_AUDIO_STREAM_ID)
                    {
                        SendClientMessage(playerid, 0xFF5555FF, "[Library] Failed to start global stream.");
                    }
                    else if (!TrackGlobalStream(sid, playerid))
                    {
                        SA_StopAudioStream(sid);
                        SendClientMessage(playerid, 0xFF5555FF,
                            "[Global] Too many active global streams in this demo; stop one first.");
                    }
                    else
                    {
                        gLastGlobalSid[playerid] = sid;
                        SendClientMessage(playerid, 0x00FF88FF, "[Library] Playing the track globally in a 45m radius.");
                    }
                }
                case 2:
                {
                    if (SA_QueueChannelAudioStream(cid, ref, track))
                    {
                        SendClientMessage(playerid, 0x00FF88FF, "[Library] Track added to the channel queue.");
                    }
                    else
                    {
                        SendClientMessage(playerid, 0xFF5555FF, "[Library] Selected channel unavailable or queue full.");
                    }
                }
                case 3:
                {
                    if (!EnsureSelectedChannelExists(playerid, cid)) return 1;
                    if (!EnsureChannelManagePermission(playerid, cid, false))
                    {
                        ShowLibraryTrackMenu(playerid);
                        return 1;
                    }
                    SendClientMessage(playerid, SA_PlayChannelAudioStream(cid, ref) ? 0x00FF88FF : 0xFF5555FF,
                        "[Library] Channel playback requested.");
                }
            }
            ShowLibraryTrackMenu(playerid);
            return 1;
        }

        // ---------------- Sound Effects ----------------
        case DLG_SA_SOUND_INPUT:
        {
            if (!response || inputtext[0] == EOS)
            {
                ShowPersonalMenu(playerid);
                return 1;
            }
            format(gSoundName[playerid], sizeof(gSoundName[]), "%s", inputtext);
            ShowPlayerDialog(playerid, DLG_SA_SOUND_MODE, DIALOG_STYLE_LIST,
                "{00CCFF}Sound Effect Position",
                "2D {AAAAAA}(Heard anywhere, like an interface sound)\n\
3D at My Position {AAAAAA}(usepos = true, 30m)",
                "Play", "Back");
            return 1;
        }

        case DLG_SA_SOUND_MODE:
        {
            if (!response)
            {
                ShowSoundInputDialog(playerid);
                return 1;
            }
            new bool:ok;
            if (listitem == 1)
            {
                new Float:x, Float:y, Float:z;
                GetPlayerPos(playerid, x, y, z);
                ok = SA_PlayAudioStreamSoundForPlayer(playerid, gSoundName[playerid], x, y, z, 30.0, true);
            }
            else
            {
                ok = SA_PlayAudioStreamSoundForPlayer(playerid, gSoundName[playerid]);
            }
            if (ok)
            {
                SendClientMessage(playerid, 0x00FF88FF,
                    "[Sound] Playing over your current audio; it resumes by itself when the sound ends.");
            }
            else
            {
                SendClientMessage(playerid, 0xFF5555FF,
                    "[Sound] Not played: file missing, unsupported or over 5 MB, or your personal audio is muted (see server log).");
            }
            ShowPersonalMenu(playerid);
            return 1;
        }

        case DLG_SA_GLOBAL_LIVE_SET_ID:
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
            new text_pos = strfind(inputtext, " ");
            if (text_pos != -1)
            {
                while (inputtext[text_pos] == ' ') text_pos++;
                if (SA_SetAudioStreamLiveTitle(sid, inputtext[text_pos]))
                {
                    SendClientMessage(playerid, 0x00FF88FF, "[Stream] Live radio text updated.");
                }
                else
                {
                    SendClientMessage(playerid, 0xFF5555FF, "[Stream] Failed to update live radio text (inactive stream).");
                }
            }
            else
            {
                if (SA_SetAudioStreamLiveTitle(sid, ""))
                {
                    SendClientMessage(playerid, 0x00FF88FF, "[Stream] Live radio text cleared.");
                }
                else
                {
                    SendClientMessage(playerid, 0xFF5555FF, "[Stream] Failed to update live radio text (inactive stream).");
                }
            }
            ShowGlobalMenu(playerid);
            return 1;
        }
    }
    return 0;
}

// -----------------------------------------------------------------------------
// All 19 SampAudioStream Plugin Callbacks
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

    static body[2048];
    new line[160], dur_str[16], t[64], a[48];
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
    FreeHistSlot(channelid);
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

    new slot = FindOrAllocHistSlot(channelid);
    if (slot == -1) return 1;

    if (++gTrackSerialCounter == 0) gTrackSerialCounter = 1;
    gTrackSerial[slot] = gTrackSerialCounter;

    new new_lbl[128];
    if (artist[0] != EOS && title[0] != EOS)
    {
        format(new_lbl, sizeof(new_lbl), "%s - %s", artist, title);
    }
    else
    {
        format(new_lbl, sizeof(new_lbl), "%s", title);
    }

    if (gHistGoingBack[slot])
    {
        gHistGoingBack[slot] = false;
    }
    else if (gCurUrl[slot]{0} != EOS)
    {
        new cur_u[256];
        strunpack(cur_u, gCurUrl[slot]);
        if (strcmp(cur_u, url, false) != 0)
        {
            if (gHistCount[slot] >= DEMO_HIST_DEPTH)
            {
                for (new i = 1; i < DEMO_HIST_DEPTH; i++)
                {
                    new tmp_u[256], tmp_l[128];
                    strunpack(tmp_u, gHistUrl[slot][i]);
                    strunpack(tmp_l, gHistLabel[slot][i]);
                    strpack(gHistUrl[slot][i - 1], tmp_u);
                    strpack(gHistLabel[slot][i - 1], tmp_l);
                }
                gHistCount[slot] = DEMO_HIST_DEPTH - 1;
            }
            new dst = gHistCount[slot]++;
            new cur_l[128];
            strunpack(cur_l, gCurLabel[slot]);
            strpack(gHistUrl[slot][dst], cur_u);
            strpack(gHistLabel[slot][dst], cur_l);
        }
    }

    if (strlen(url) >= 255)
    {
        gCurUrl[slot]{0} = EOS;
    }
    else
    {
        strpack(gCurUrl[slot], url);
    }
    strpack(gCurLabel[slot], new_lbl);
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

public OnPlayerAudioStreamNoConnect(playerid, E_SA_AUDIO_STREAM_SOURCE:source_type, source_id)
{
    #pragma unused source_type, source_id
    if (IsPlayerConnected(playerid))
    {
        SendClientMessage(playerid, 0xFFAA00FF,
            "[Audio] Your game did not connect to the audio stream. Check that your radio volume is above 0 (Options > Audio).");
    }
    return 1;
}

public OnAudioStreamChannelPlaylistQueued(channelid, added, E_SA_ERROR_CODE:error_code, const error_message[])
{
    printf("[SampAudioStream] Channel #%d playlist import finished: added=%d err=%d (%s)",
        channelid, added, _:error_code, error_message);
    return 1;
}

public OnAudioStreamLiveTitleChange(playerid, streamid, const title[])
{
    if (playerid != INVALID_PLAYER_ID && IsPlayerConnected(playerid))
    {
        if (gOptNotify[playerid])
        {
            SendTruncatedLiveMessage(playerid, "{00CCFF}[Radio] {FFFFFF}Now playing: ", title);
        }
    }
    else
    {
        printf("[SampAudioStream] Global stream #%d radio text: %s", streamid, title);
    }
    return 1;
}

public OnAudioStreamChannelLiveTitleChange(channelid, const title[])
{
    new cname[40], ccreator[32], listeners, E_SA_STREAM_STATE:st, bool:locked, qcount;
    if (!SA_GetAudioStreamChannelInfo(channelid, cname, sizeof(cname), ccreator, sizeof(ccreator), listeners, st, locked, qcount))
    {
        return 1;
    }
    SanitizeDialogField(cname);

    new prefix[96];
    format(prefix, sizeof(prefix), "{00CCFF}[Radio] {FFFFFF}Now playing on %s: ", cname);
    for (new i = 0; i < listeners; i++)
    {
        new lp = SA_GetAudioStreamChannelListener(channelid, i);
        if (lp != INVALID_PLAYER_ID && IsPlayerConnected(lp) && gOptNotify[lp])
        {
            SendTruncatedLiveMessage(lp, prefix, title);
        }
    }
    return 1;
}
