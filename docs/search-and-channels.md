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
* **Queue Manipulation & Loop Modes:** Insert at any position (`SA_InsertAudioStreamChannelQueue`), remove by index (`SA_RemoveAudioStreamChannelQueue`), import extractor playlists (`SA_QueueChannelAudioStreamPlaylist`), seek tracks (`SA_SeekChannelAudioStream`), configure `SA_SetAudioStreamChannelLoop` (`OFF`, `TRACK`, `QUEUE`), and handle `OnAudioStreamChannelTrackStart`, `OnAudioStreamChannelQueueEnd`, `OnAudioStreamChannelError`, `OnAudioStreamChannelPlaylistQueued`, and `OnAudioStreamChannelRestored`.

---

## 3. Channel Persistence

When `channels.persist_channels` is `true` (default), synchronized channels survive server restarts and GMX mode reloads with their original `channelid`:

* **Where & Format:** Each channel is saved atomically (`.tmp` write + replace) as `scriptfiles/SampAudioStream/channels/channel_<id>.json`, debounced at most once per second per channel on an async worker thread (flushed synchronously during `Shutdown()`).
* **What is Saved:** Channel `id`, UTF-8 `name`, salted SHA-256 `password_hash`/`password_salt` (no plaintext password in memory or on disk), creator metadata (`has_creator`, `creator_name_hex`), destruction policy/timeout, volume, pause state, `loop_mode`, `current_input` (`utf8_url` + `pawn_url_hex`), `was_playing`, `queue`, and fixed spatial configuration (`pos`, `distance`, `world_id`, `interior_id`, `auto_listen`). Player and vehicle attachments are **not** persisted across restarts or GMX because player/vehicle IDs are reassigned; only the last known position is preserved as a fixed 3D position.
* **Deleted When Genuinely Destroyed:** Channel JSON files are deleted **only** on real channel destruction (`SA_DestroyAudioStreamChannel`, genuine `OnDisconnect`, or expired `Timeout` / restore grace). Server shutdown disconnects and `OnGameModeExit` / GMX do **not** delete channels.
* **Boot Restoration & Extractor Readiness:** On startup (`Initialize()`), valid files are loaded smallest-ID-first up to `channels.max_channels` (excess files are skipped with a warning and kept untouched on disk), `OnAudioStreamChannelRestored` fires after the gamemode finishes loading (`OnGameModeInit`), `Timeout` countdowns restart cleanly in full, and `OnDisconnect` channels wait for `channels.channel_restore_grace_seconds` (`300s` default; `0` = delete on boot) before expiring. Channels with `was_playing = true` remain `Idle` with their queue intact until `yt-dlp` becomes ready (or report `SA_ERR_EXTRACTOR_NOT_READY` once via `OnAudioStreamChannelError` without dropping the queue if extractor provisioning fails).
* **GMX Behavior:** With `channels.persist_channels = true`, GMX stops personal/global streams and detaches any channel player/vehicle attachments (keeping the last known coordinates as a fixed 3D position) while keeping live channels, listeners, and active channel playback uninterrupted, then dispatches `OnAudioStreamChannelRestored` once the new gamemode initializes so scripts can rebind handles or re-attach entities without losing listeners. With `channels.persist_channels = false`, no channel files are read/written and GMX destroys all channels cleanly.
* **Script Best Practices:** When `channels.persist_channels` is `true`, do **not** call `SA_DestroyAudioStreamChannel` inside `OnFilterScriptExit` or `OnGameModeExit` (otherwise server shutdown or `/rcon reloadfs` deletes saved channel files from disk). Before creating fixed server-owned channels in `OnFilterScriptInit` / `OnGameModeInit`, scan existing channels (`SA_GetAudioStreamChannelCount` / `SA_GetAudioStreamChannelID` / `SA_GetAudioStreamChannelInfo`) and reuse a matching channel instead of creating duplicates (see `FS_SampAudioStream_Demo.pwn`).
