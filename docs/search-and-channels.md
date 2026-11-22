# Music Search & Synchronized Radio Channels

SampAudioStream lets players search for music by name directly in-game and enables server-wide synchronized radio stations with shared queues.

---

## 1. Asynchronous Music Search

Calling `SA_SearchAudioStream(playerid, query, max_results = 5)` dispatches a background search on the worker thread pool without stalling the SA-MP/open.mp server tick.

### Callback Sequence
1. `OnAudioStreamSearchResultItem(playerid, search_id, item_index, const title[], const artist[], duration, const video_id[])` is fired once for each result (`item_index` from `0` to `total_found - 1`).
2. `OnAudioStreamSearchResults(playerid, search_id, total_found, E_SA_ERROR_CODE:error_code, const error_message[])` is fired right after all items have been delivered (`error_code == SA_ERR_NONE` on success, or non-zero with `total_found == 0` on error).

### Complete Interactive Dialog Example

```pawn
#define DIALOG_MUSIC_SEARCH (8801)
#define MAX_SEARCH_CACHE    (10)

enum E_SEARCH_ENTRY
{
    bool:seUsed,
    seTitle[128],
    seArtist[64],
    seDuration,
    seUrl[192]
};

new g_SearchCache[MAX_PLAYERS][MAX_SEARCH_CACHE][E_SEARCH_ENTRY];
new g_ActiveSearchID[MAX_PLAYERS] = { INVALID_AUDIO_SEARCH_ID, ... };

CMD:music(playerid, params[])
{
    if (isnull(params)) return SendClientMessage(playerid, -1, "Usage: /music <song name or URL>");

    if (strcmp(params, "http://", true, 7) == 0 || strcmp(params, "https://", true, 8) == 0)
    {
        SA_PlayAudioStreamForPlayer(playerid, params);
        return 1;
    }

    for (new i = 0; i < MAX_SEARCH_CACHE; i++) g_SearchCache[playerid][i][seUsed] = false;
    g_ActiveSearchID[playerid] = SA_SearchAudioStream(playerid, params, 8);
    SendClientMessage(playerid, 0x3498DBAA, "[Music] Searching...");
    return 1;
}

public OnAudioStreamSearchResultItem(playerid, search_id, item_index, const title[], const artist[], duration, const video_id[])
{
    if (search_id != g_ActiveSearchID[playerid] || item_index < 0 || item_index >= MAX_SEARCH_CACHE)
        return 1;

    g_SearchCache[playerid][item_index][seUsed] = true;
    format(g_SearchCache[playerid][item_index][seTitle], 128, "%s", title);
    format(g_SearchCache[playerid][item_index][seArtist], 64, "%s", artist);
    g_SearchCache[playerid][item_index][seDuration] = duration;
    format(g_SearchCache[playerid][item_index][seUrl], 192, "https://www.youtube.com/watch?v=%s", video_id);
    return 1;
}

public OnAudioStreamSearchResults(playerid, search_id, total_found, E_SA_ERROR_CODE:error_code, const error_message[])
{
    if (search_id != g_ActiveSearchID[playerid]) return 1;
    if (error_code != SA_ERR_NONE)
    {
        new err[144];
        format(err, sizeof(err), "[Music] Search failed (%d): %s", _:error_code, error_message);
        return SendClientMessage(playerid, 0xE74C3CAA, err);
    }
    if (total_found <= 0) return SendClientMessage(playerid, 0xE74C3CAA, "[Music] No results found.");

    new body[1536], row[220];
    body = "Title\tArtist\tDuration\n";

    for (new i = 0; i < total_found && i < MAX_SEARCH_CACHE; i++)
    {
        if (!g_SearchCache[playerid][i][seUsed]) continue;
        new s = g_SearchCache[playerid][i][seDuration];
        format(row, sizeof(row), "%s\t%s\t%02d:%02d\n", g_SearchCache[playerid][i][seTitle], g_SearchCache[playerid][i][seArtist], s / 60, s % 60);
        strcat(body, row, sizeof(body));
    }

    ShowPlayerDialog(playerid, DIALOG_MUSIC_SEARCH, DIALOG_STYLE_TABLIST_HEADERS, "Music Search Results", body, "Play", "Cancel");
    return 1;
}

public OnDialogResponse(playerid, dialogid, response, listitem, inputtext[])
{
    if (dialogid == DIALOG_MUSIC_SEARCH && response)
    {
        if (listitem >= 0 && listitem < MAX_SEARCH_CACHE && g_SearchCache[playerid][listitem][seUsed])
        {
            SA_PlayAudioStreamForPlayer(playerid, g_SearchCache[playerid][listitem][seUrl]);
        }
        return 1;
    }
    return 0;
}
```

> [!TIP]
> You can also pass a plain song name directly to `SA_PlayAudioStreamForPlayer(playerid, "bohemian rhapsody")` or `SA_PlayAudioStream(...)` — the resolver automatically resolves `ytsearch1:<query>` and starts the top match immediately!

---

## 2. Synchronized Radio Channels

Channels allow you to create shared radio stations where all joined listeners hear the exact same audio stream synchronized in real time (`1x` live pacing).

### Lifecycle & Queueing

```pawn
new g_RadioChannel = INVALID_AUDIO_CHANNEL_ID;

public OnGameModeInit()
{
    g_RadioChannel = SA_CreateAudioStreamChannel("Server FM");
    SA_PlayChannelAudioStream(g_RadioChannel, "https://www.youtube.com/watch?v=jfKfPfyJRdk");
    SA_QueueChannelAudioStream(g_RadioChannel, "https://soundcloud.com/artist/track");
    return 1;
}

CMD:radio(playerid, params[])
{
    if (SA_IsPlayerInAudioStreamChannel(playerid, g_RadioChannel))
    {
        SA_LeaveAudioStreamChannel(playerid);
        SendClientMessage(playerid, -1, "[Radio] Left Server FM.");
    }
    else
    {
        SA_JoinAudioStreamChannel(playerid, g_RadioChannel);
        SendClientMessage(playerid, -1, "[Radio] Tuned into Server FM.");
    }
    return 1;
}
```

### Channel Lifecycle, Passwords & Queue Behavior
* `SA_PlayChannelAudioStream`: Clears/replaces current playback on the channel and immediately starts the given URL or search query.
* `SA_QueueChannelAudioStream`: Appends a track to the channel's queue. When the active track finishes draining its lead buffer, listeners cleanly transition to the next track (with a brief audible gap while clients reconnect to the new track).
* `SA_SkipChannelAudioStream`: Aborts the active track and advances immediately to the next queued track.
* `SA_JoinAudioStreamChannel` / `SA_LeaveAudioStreamChannel`: Joining a channel stops the player's individual stream or previous channel automatically; if the channel has a password set (`SA_SetAudioStreamChannelPass`), the matching password must be supplied or join fails. Calling `StopAudioStreamForPlayer` / `SA_StopAudioStreamForPlayer` also leaves any active channel.
* **Creator Tracking & Auto-Destruction:** Pass `creator_playerid`, optional `password`, `E_SA_CHANNEL_DESTROY_MODE` (`SA_CHANNEL_DESTROY_MANUAL`, `SA_CHANNEL_DESTROY_ON_DISCONNECT`, or `SA_CHANNEL_DESTROY_ON_TIMEOUT`), and `destroy_timeout_sec` to `SA_CreateAudioStreamChannel` (or update later via `SA_SetAudioStreamChannelDestroy` / `SA_SetAudioStreamChannelCreator`).
  * Automatic destruction modes (`OnDisconnect` / `Timeout`) require a connected creator (server-owned channels without a creator remain `Manual`).
  * Online creators keep ownership across `SetPlayerName` changes (updating the stored creator name), and disconnect snapshots the final name as a safety net. When an offline creator reconnects (`OnPlayerConnect`) or completes login/tag rename (`SetPlayerName`) matching their stored name (case-insensitive), `Timeout` resets to full `destroy_timeout_sec` and pauses again. Servers with account systems should call `SA_SetAudioStreamChannelCreator` after account login if another player could adopt an offline creator's name.
  * When `Timeout` expires offline or creator disconnects in `OnDisconnect` mode, `OnAudioStreamChannelDestroyed` arrives on the next server tick.
* **Dynamic Channel Menus:** Enumerate all active channels with `SA_GetAudioStreamChannelCount`, `SA_GetAudioStreamChannelID`, `SA_GetAudioStreamChannelInfo`, `SA_GetAudioStreamChannelTrack`, `SA_GetAudioStreamChannelElapsed`, `SA_GetAudioStreamChannelListener`, `SA_GetAudioStreamChannelQueue`, and `SA_GetAudioStreamChannelDestroy`.
* **Spatial Channels & Mobile Sound Cars:** Call `SA_SetAudioStreamChannelPos(channelid, x, y, z, distance, worldid, interiorid, auto_listen)` for fixed-position clubs/bars/venues (with client-side 3D attenuation), or attach dynamically with `SA_AttachAudioStreamChannelToVehicle` / `SA_AttachAudioStreamChannelToPlayer` (which play at full 2D volume inside server range and silence outside range because SA-MP clients cannot move active 3D sound origins; reset via `SA_ClearAudioStreamChannelPos`). Use `auto_listen = false` for private/password-restricted areas so only subscribed players inside the zone hear audio.
* **Queue Manipulation, Labels, Shuffle & Loop Modes:** Optionally pass a friendly `label` to `SA_QueueChannelAudioStream` / `SA_InsertAudioStreamChannelQueue` and read it with `SA_GetAudioStreamChannelQueueLabel`, attach free-text descriptions to the channel (`SA_SetAudioStreamChannelDescription` / `SA_GetAudioStreamChannelDescription`) and to queue entries (`SA_SetAudioStreamChannelQueueDescription` / `SA_GetAudioStreamChannelQueueDescription`, with the playing track's description in `SA_GetAudioStreamChannelTrackDescription`), or find an entry by URL with `SA_FindAudioStreamChannelQueue` (one native call, useful to mark already-queued tracks in big lists; playlist imports via `SA_QueueChannelAudioStreamPlaylist` populate labels automatically from entry titles, and labels follow the item into `current_input` and `SA_CHANNEL_LOOP_QUEUE`). You can also remove by index (`SA_RemoveAudioStreamChannelQueue`), reorder queue items without losing UTF-8 labels (`SA_MoveAudioStreamChannelQueue`), perform a one-time random shuffle of pending queue items (`SA_ShuffleAudioStreamChannelQueue`), seek tracks (`SA_SeekChannelAudioStream`), configure `SA_SetAudioStreamChannelLoop` (`OFF`, `TRACK`, `QUEUE`), and handle `OnAudioStreamChannelTrackStart`, `OnAudioStreamChannelQueueEnd`, `OnAudioStreamChannelError`, `OnAudioStreamChannelPlaylistQueued`, and `OnAudioStreamChannelRestored`.
* **Live Internet Radio Text & Station Titles:** On direct live radio streams, the track title starts as `"Direct Audio Stream"` / `"Radio Stream"` unless the station's `icy-name` was found during link probing, and is updated once from `icy-name` on the first ICY poll without re-firing `TrackStart`; ongoing song updates arrive via `OnAudioStreamChannelLiveTitleChange` / `OnAudioStreamLiveTitleChange` without altering `live_title` or channel names.

### Channel Playlist Mode
Enable non-destructive playlist mode per channel with `SA_SetAudioStreamChannelPlaylist(channelid, true)` (`SA_IsAudioStreamChannelPlaylist` / `SA_GetAudioStreamChannelQueuePos` / `SA_PlayAudioStreamChannelQueue`). Switching **on** keeps any active queued track at index `0`; switching **off** removes entries before `playlist_pos` and reverts to consuming FIFO order:

| Operation | Behavior in Playlist Mode |
| :--- | :--- |
| `Queue` / `Insert` | Appends/inserts in list (`playlist_pos` shifts if inserted before it); starts immediately if channel is stopped without a current item |
| Natural end (`LOOP_TRACK`) | Repeats current item at the same `playlist_pos` |
| Natural end (`LOOP_QUEUE`) | Advances to `playlist_pos + 1`, wrapping back to `0` after the last item |
| Natural end (`LOOP_OFF`) | Advances to `playlist_pos + 1`; after the last item, stops with `playlist_pos = -1`, preserves list, and fires `OnAudioStreamChannelQueueEnd` |
| `SA_SkipChannelAudioStream` | Jumps to next item (wrapping on `LOOP_QUEUE`, or stopping without `QueueEnd` on `LOOP_OFF`); during an out-of-list track it continues the list like a natural end |
| `SA_PlayAudioStreamChannelQueue(i)` | Jumps to item `i` without altering list order (`playlist_pos = i`) |
| `SA_PlayChannelAudioStream(url)` | Plays out-of-list (`playlist_pos = -1`) without modifying the list. When it ends (or is skipped or fails) the list continues after the item that was playing before it, or from the first item if none was; `LOOP_TRACK` repeats the out-of-list track, `LOOP_QUEUE` wraps past the last item, and the channel stops (without `OnAudioStreamChannelQueueEnd`) when nothing is left |
| `Remove(i)` | Decrements `playlist_pos` when `i < playlist_pos`; stops and plays the replacement item at `i` (or stops if none) when `i == playlist_pos`; leaves playback untouched when `i > playlist_pos` |
| `Move(from, to)` / `Shuffle` | Reorders the list and keeps `playlist_pos` pinned to the currently playing track |
| `Clear` | Clears the list, sets `playlist_pos = -1`, and stops playback if the active track came from the list |
| `Stop` | Stops playback while preserving both the list and `playlist_pos` |
| Playlist Import | Appends entries to the list (auto-starting from the first imported item if stopped without a current item) |
| Track Error | Skips to the next item (never loops the failing track); 5 consecutive errors still stop the channel |

### Local Music Library (`audio.music_dir` & `library://`)
Place audio files directly in `audio.music_dir` (default `"scriptfiles/SampAudioStream/music"`) or inside **depth-1 subfolders** (deeper subfolders and empty folders are ignored). Call `SA_RefreshAudioLibrary()` to index up to `200` folders and `1000` tracks per folder (sorted case-insensitively, filtered to `.mp3`, `.ogg`, `.opus`, `.wav`, `.flac`, `.aac`, `.m4a` and representable in `scripting.pawn_text_encoding`), then enumerate with `SA_GetAudioLibraryFolderCount`, `SA_GetAudioLibraryFolder`, and `SA_GetAudioLibraryTrack`:
* **Single tracks:** pass `library://<folder>/<file>` (or `library:///<file>` for loose root files) to any URL-accepting native (`SA_PlayAudioStreamForPlayer`, `SA_PlayAudioStream`, `PlayAudioStreamForPlayer`, `SA_PlayChannelAudioStream`, `SA_QueueChannelAudioStream`, `SA_InsertAudioStreamChannelQueue`).
* **Folder playlist import:** pass `library://<folder>` (or `library://` for the root folder) to `SA_QueueChannelAudioStreamPlaylist` using file names without their audio extension as queue labels.
* **Security & Transcoding:** Paths are strictly canonicalized inside `audio.music_dir` (rejecting `..`, `\`, `:`, drive letters, control characters, symlinks escaping `music_dir`, invalid extensions, and non-`library://` disk paths such as `file://`, `C:\...`, `/etc/...` with `SA_ERR_INVALID_URL`). Local files always stream through the server transcoder as finite tracks with known duration, seek, pause, volume, AGC, and `OnAudioStreamFinished`.

---

## 3. Channel Persistence

Synchronized channels created with `persistent = true` (the default parameter on `SA_CreateAudioStreamChannel`) survive server restarts and GMX mode reloads with their original `channelid`, while channels created with `persistent = false` live strictly in RAM, never touch disk, and are destroyed automatically on GMX:

* **Where & Format:** Each persistent channel is saved atomically (`.tmp` write + replace) as `scriptfiles/SampAudioStream/channels/channel_<id>.json`, debounced at most once per second per channel on an async worker thread (flushed synchronously during `Shutdown()`).
* **What is Saved:** Channel `id`, `persistent` flag (`true`), UTF-8 `name`, salted SHA-256 `password_hash`/`password_salt` (no plaintext password in memory or on disk), creator metadata (`has_creator`, `creator_name_hex`), destruction policy/timeout, volume, pause state, `loop_mode`, channel description (`description_hex`, `pawn_description_hex`), `current_input` (`utf8_url`, `pawn_url_hex`, `label_hex`, `pawn_label_hex`, `description_hex`, `pawn_description_hex`), `was_playing`, idle suspension (`idle_suspend`, `idle_suspend_seconds`, `resume_offset_seconds`), `queue` (including labels and descriptions), and fixed spatial configuration (`pos`, `distance`, `world_id`, `interior_id`, `auto_listen`). Player and vehicle attachments are **not** persisted across restarts or GMX because player/vehicle IDs are reassigned; only the last known position is preserved as a fixed 3D position.
* **Deleted When Genuinely Destroyed:** Channel JSON files are deleted **only** on real channel destruction (`SA_DestroyAudioStreamChannel`, genuine `OnDisconnect`, or expired `Timeout` / restore grace). Server shutdown disconnects and `OnGameModeExit` / GMX do **not** delete persistent channel files. Non-persistent channels (`persistent = false`) never write or delete any file on disk.
* **Boot Restoration & Extractor Readiness:** On startup (`Initialize()`), valid files in `scriptfiles/SampAudioStream/channels/` are loaded smallest-ID-first as persistent channels up to `channels.max_channels` (excess files are skipped with a warning and kept untouched on disk, and their IDs remain reserved so new channels never collide with them). `OnAudioStreamChannelRestored` fires after the gamemode finishes loading (`OnGameModeInit`), `Timeout` countdowns restart cleanly in full, and `OnDisconnect` channels wait for `channels.channel_restore_grace_seconds` (`300s` default; `0` = delete on boot) before expiring. Channels with `was_playing = true` remain `Idle` with their queue intact until `yt-dlp` becomes ready (or report `SA_ERR_EXTRACTOR_NOT_READY` once via `OnAudioStreamChannelError` without dropping the queue if extractor provisioning fails).
* **GMX Behavior:** On GMX, personal and global streams stop. Persistent channels (`persistent = true`) stay alive with their listeners and active playback uninterrupted (detaching any player/vehicle attachments while keeping the last known coordinates as a fixed 3D position), then receive `OnAudioStreamChannelRestored` once the new gamemode initializes so scripts can rebind handles or re-attach entities without losing listeners. Non-persistent channels (`persistent = false`) are destroyed immediately on GMX and dispatch `OnAudioStreamChannelDestroyed`.
* **Script Best Practices:** Do **not** call `SA_DestroyAudioStreamChannel` on persistent channels inside `OnFilterScriptExit` or `OnGameModeExit` (otherwise server shutdown or `/rcon reloadfs` deletes saved channel files from disk). Before creating fixed server-owned **persistent** channels in `OnFilterScriptInit` / `OnGameModeInit`, scan existing channels (`SA_GetAudioStreamChannelCount` / `SA_GetAudioStreamChannelID` / `SA_GetAudioStreamChannelInfo`) and reuse a matching channel instead of creating duplicates (see `FS_SampAudioStream_Demo.pwn`). For temporary player channels (such as personal boomboxes or short-lived car audio), pass `persistent = false` instead.
