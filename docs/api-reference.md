# Complete API Reference

Exhaustive technical reference for **`SampAudioStream.inc`** (`v2.3.2`), covering all **89 `SA_*` natives**, **2 hooked standard natives**, **19 callbacks**, **6 enums**, and constants.

---

## Table of Contents

1. [Playback & Control Natives](#1-playback--control-natives)
2. [Spatial & Attachment Natives](#2-spatial--attachment-natives)
3. [Asynchronous Music Search Natives](#3-asynchronous-music-search-natives)
4. [Synchronized Radio Channel Natives](#4-synchronized-radio-channel-natives)
5. [Diagnostics & Metadata Natives](#5-diagnostics--metadata-natives)
6. [Hooked Vanilla SA-MP Natives](#6-hooked-vanilla-sa-mp-natives)
7. [Callbacks](#7-callbacks)
8. [Constants & Enums](#8-constants--enums)

---

## 1. Playback & Control Natives

### `SA_PlayAudioStreamForPlayer`
```pawn
native SA_PlayAudioStreamForPlayer(playerid, const url[], Float:posX = 0.0, Float:posY = 0.0, Float:posZ = 0.0, Float:distance = 50.0, bool:usepos = false, worldid = -1, interiorid = -1, Float:volume = 100.0, bool:notify_processing = true, bool:process_direct_audio_files = false, bool:stream_text = true);
```
Plays a URL (YouTube, platform link, direct audio, Shoutcast/Icecast) or search query for a single player. Automatically stops any previous personal stream or radio channel for that player. When `notify_processing` is `true`, fires `OnAudioStreamProcessing` right away before resolution/buffering starts. When `process_direct_audio_files` is `true`, direct music files (`.mp3`/`.ogg`/`.wav`/`.flac`/`.aac`/`.m4a`/`.opus` on `http://` or `https://`) bypass native passthrough and are server-transcoded (enabling pause/resume, live volume scaling, frame-aligned `SA_Set*Distance`, and `OnAudioStreamFinished`); live radio streams and `.pls`/`.m3u` playlists remain on passthrough. Passing `stream_text = false` omits `icy-name`, `icy-metaint`, and inline ICY metadata frames from server-processed endpoint responses (so the SA-MP/open.mp client displays no bottom-corner text) and disables ICY live-title polling for the session, while callbacks and query natives (`OnAudioStreamReady`, `SA_GetAudioStreamInfo`) still report metadata normally. For passthrough playback the upstream server controls client display text directly, so combine `.process_direct_audio_files = true, .stream_text = false` when hiding text on direct audio files.
* **Returns:** Valid `streamid` (`>= 1`), or `INVALID_AUDIO_STREAM_ID` (`-1`) on failure.

### `SA_PlayAudioStream`
```pawn
native SA_PlayAudioStream(const url[], Float:posX = 0.0, Float:posY = 0.0, Float:posZ = 0.0, Float:distance = 50.0, bool:usepos = false, worldid = -1, interiorid = -1, Float:volume = 100.0, bool:notify_processing = true, bool:process_direct_audio_files = false, bool:stream_text = true);
```
Starts a global / spatial stream. Any connected player matching `worldid`, `interiorid`, and 3D `distance` (when `usepos == true`) hears it automatically upon entering range and stops upon leaving range. Finite tracks advance at `1x` real-time so latecomers hear the current playback timestamp; live broadcasts stream directly in real time. When `notify_processing` is `true`, fires `OnAudioStreamProcessing` with `playerid == INVALID_PLAYER_ID`. Setting `process_direct_audio_files` to `true` ensures even spatial direct music files on plain `http://` are transcoded server-side rather than handed raw to the client (note: non-live `SA_PlayAudioStream` sessions already transcode for timeline synchronization unless streaming live). Passing `stream_text = false` omits `icy-name`, `icy-metaint`, and inline ICY frames on server-processed playback and disables ICY live-title polling while keeping callbacks and `SA_GetAudioStreamInfo` unchanged (passthrough playback ignores this flag because the remote server controls what the client displays).
* **Returns:** Valid `streamid` (`>= 1`), or `INVALID_AUDIO_STREAM_ID` (`-1`) on failure.

### `SA_PlayAudioStreamSoundForPlayer`
```pawn
native bool:SA_PlayAudioStreamSoundForPlayer(playerid, const sound[], Float:posX = 0.0, Float:posY = 0.0, Float:posZ = 0.0, Float:distance = 50.0, bool:usepos = false);
```
Plays a short sound effect (alert, notification, jingle, UI or gameplay sound such as an achievement) from `audio.sound_dir` for a single player. `sound` is a path relative to that folder (`"radio_off.mp3"` or one subfolder level such as `"alerts/siren.mp3"`; `\` is accepted as separator), validated with the same strict containment rules as the music library.
* **No transcoding:** the file is read once into a shared in-memory cache of up to `32 MB` (re-read when it changes on disk; the least recently used files leave the cache first) and served as-is from `/f/<code>` with `Content-Length`, so it starts immediately and uses no transcoder slot. Only formats the client decodes by itself are accepted: `.mp3`, `.ogg` (Vorbis) and `.wav`, up to `5 MB`. Volume follows the file itself (no volume parameter) and no on-screen stream text is sent.
* **Plays above everything, then resumes:** the sound takes priority over personal, channel, range and global sources (`OnPlayerAudioStreamSourceChange` reports `SA_SOURCE_SOUND`). It ends `500 ms` after the file's own length, counted from the client's download request (or the length plus `5 s` when the client never requests it), and the player goes back to the source it had before: a channel or radio continues live and an interrupted transcoded personal stream resumes where it was cut. A new sound for the same player replaces the current one. A direct link sent through the hooked `PlayAudioStreamForPlayer` while a sound plays waits for the sound to end, and a transcoded personal track does not count as finished while the player cannot hear it. Sound files stay out of the music library, and `SA_StopAudioStreamForPlayer` does not cut a playing sound.
* Respects `SA_SetPlayerAudioStreamMuted(playerid, true, true)` (returns `false` while personal audio is muted).
* **Returns:** `true` when the sound was sent, `false` if the player is not connected, the file is missing, unsupported, larger than `5 MB` or invalid (a warning with the reason is logged), or the plugin is not ready.

### `SA_StopAudioStreamForPlayer`
```pawn
native SA_StopAudioStreamForPlayer(playerid);
```
Stops the personal audio stream currently playing for `playerid`.
* **Returns:** `1` if a personal stream was stopped, `0` otherwise.

### `SA_StopAudioStream`
```pawn
native SA_StopAudioStream(streamid);
```
Stops and destroys any active stream (personal or global) by `streamid` for all listeners.
* **Returns:** `1` on success, `0` if `streamid` was not found.

### `SA_PauseAudioStreamForPlayer`
```pawn
native SA_PauseAudioStreamForPlayer(playerid, bool:pause);
```
Pauses (`true`) or resumes (`false`) the personal audio stream owned by `playerid`.
* **Returns:** `1` on success, `0` if no personal stream is active for `playerid`.

### `SA_PauseAudioStream`
```pawn
native SA_PauseAudioStream(streamid, bool:pause);
```
Pauses (`true`) or resumes (`false`) any active stream (`personal` or `global`) by `streamid`.
* **Transcoded global/personal:** Pausing snapshots `paused_offset` and stops active listeners; unpausing reconciles all in-range listeners using `?resume=paused_offset` and refreshes completion deadlines. Players entering range while paused stay silent until unpaused.
* **Passthrough streams:** Pausing stops playback for active listeners; unpausing re-dispatches the direct stream URL (live streams resume at live position; direct files restart from the beginning).
* **Returns:** `1` on success (including pausing an already-paused stream), `0` if `streamid` does not exist or has already stopped/errored/finished.

### `SA_SeekAudioStream`
```pawn
native SA_SeekAudioStream(streamid, seconds);
```
Seeks a playing or paused server-transcoded non-live stream (`personal` or `global`) with known duration (`0 <= seconds < duration`) to `seconds` by replacing the active transcoder run with a new buffer starting at `seconds` (keeping loop/pause state intact, resetting elapsed time to `seconds`, and advancing `options_version` without cancelling the session or emitting `OnPlayerAudioStreamSourceChange`). Counts as an in-place transcoder replacement so it succeeds even when the concurrency limit is full.
* **Returns:** `1` on success, `0` if `streamid` does not exist, is passthrough, is live, has unknown duration, is out of bounds, or is not `Playing`/`Paused`.

### `SA_SetPlayerAudioStreamMuted`
```pawn
native SA_SetPlayerAudioStreamMuted(playerid, bool:mute, bool:include_personal = false);
```
Mutes (`true`) or unmutes (`false`) audio stream delivery for `playerid` without removing channel subscriptions, range listener entries, or active sessions.
* When `include_personal == false` (default), channel, range-channel, and global streams are muted while personal streams for `playerid` continue playing.
* When `include_personal == true`, personal streams are also silenced (without cancelling passthrough sessions): transcoded personal streams resume at `resync_offset` when unmuted, whereas passthrough personal streams re-dispatch the direct link (live radios resume live; direct files restart from the beginning).
* Mute state lasts until disconnect (preserved across GMX) and is not saved to disk.
* **Returns:** `1` on success, `0` if `playerid` is not connected.

### `SA_IsPlayerAudioStreamMuted`
```pawn
native bool:SA_IsPlayerAudioStreamMuted(playerid);
```
Returns `true` if `playerid` is connected and currently muted via `SA_SetPlayerAudioStreamMuted`, or `false` otherwise.

### `SA_SetPlayerAudioStreamGroupMuted`
```pawn
native SA_SetPlayerAudioStreamGroupMuted(playerid, group, bool:mute);
```
Mutes (`true`) or unmutes (`false`) range listening of channel group `group` (`1..31`, see `SA_SetAudioStreamChannelGroup`) for `playerid` (`2.3.1`). While muted, positioned `auto_listen` channels of that group are not heard just by being inside their range, so another source (or silence) takes over; channels the player joined with `SA_JoinAudioStreamChannel` keep playing normally. Typical use: separate "hear boomboxes" and "hear car speakers" options without muting everything with `SA_SetPlayerAudioStreamMuted`. Lasts until disconnect (kept across GMX) and is not saved to disk.
* **Returns:** `1` on success, `0` if `playerid` is not connected or `group` is outside `1..31`.

### `SA_IsPlayerAudioStreamGroupMuted`
```pawn
native bool:SA_IsPlayerAudioStreamGroupMuted(playerid, group);
```
Returns `true` if `playerid` is connected and has range group `group` muted, or `false` otherwise.

### `SA_GetPlayerAudioStreamSource`
```pawn
native SA_GetPlayerAudioStreamSource(playerid, &E_SA_AUDIO_STREAM_SOURCE:source_type, &source_id);
```
Retrieves the audio source currently delivered to `playerid`'s client (`SA_SOURCE_NONE`, `SA_SOURCE_PERSONAL`, `SA_SOURCE_CHANNEL`, `SA_SOURCE_GLOBAL`, `SA_SOURCE_CHANNEL_RANGE`, or `SA_SOURCE_SOUND`, with decoded `streamid` or `channelid` in `source_id`, or `-1` when none or for a sound).
* **Returns:** `1` if `playerid` is currently receiving a source, `0` otherwise (`source_type = SA_SOURCE_NONE`, `source_id = -1`).

### `SA_SetPlayerAudioStreamVolume`
```pawn
native SA_SetPlayerAudioStreamVolume(playerid, Float:volume);
```
Adjusts volume (`0.0` to `200.0`, clamped) for `playerid`'s active server-transcoded personal stream. Direct passthrough streams (`http://`/`https://` direct audio or live radio started at `100.0` volume without `process_direct_audio_files = true`) do not support runtime volume changes (`0` returned); start with `volume != 100.0` or `.process_direct_audio_files = true` for direct files to enable transcoded volume scaling.
* **Returns:** `1` on success, `0` if no personal stream is active or the stream is running in direct passthrough.

### `SA_SetAudioStreamVolume`
```pawn
native SA_SetAudioStreamVolume(streamid, Float:volume);
```
Adjusts volume (`0.0` to `200.0`, clamped) for any active server-transcoded stream by `streamid` (passthrough `http://`/`https://` streams started at `100.0` vol cannot be scaled after startup and return `0`; start with `volume != 100.0` or `.process_direct_audio_files = true` for direct files).
* **Returns:** `1` on success, `0` if `streamid` was not found or is running in direct passthrough.

### `SA_SetPlayerAudioStreamDistance`
```pawn
native SA_SetPlayerAudioStreamDistance(playerid, Float:distance);
```
Updates the playback radius (`distance >= 0.0`) for `playerid`'s active personal stream (including direct/native `PlayAudioStreamForPlayer` streams) and re-applies the stored URL and original parameters (`posX, posY, posZ, usepos`) to the client if actively playing. For server-transcoded streams, reconnects at the current playback offset aligned to an MP3 frame boundary.
* **Returns:** `1` on success, `0` if no personal stream is active or `distance < 0.0`.

### `SA_SetAudioStreamDistance`
```pawn
native SA_SetAudioStreamDistance(streamid, Float:distance);
```
Updates `distance >= 0.0` for any active stream (`streamid`), immediately adding/removing listeners matching the new radius and updating `distance` for all active listeners (with MP3-frame-aligned playback resumption for transcoded streams).
* **Returns:** `1` on success, `0` if `streamid` was not found or `distance < 0.0`.

### `SA_SetPlayerAudioStreamPos`
```pawn
native SA_SetPlayerAudioStreamPos(playerid, Float:posX, Float:posY, Float:posZ);
```
Sets a new 3D position (`posX, posY, posZ`) for `playerid`'s active personal stream, automatically forces `usepos = true`, clears any active player/vehicle attachment on that stream, and immediately applies spatial filtering / client update (with MP3-frame-aligned playback resumption for transcoded streams).
* **Returns:** `1` on success, `0` if no active personal stream was found or coordinates are invalid.

### `SA_SetAudioStreamPos`
```pawn
native SA_SetAudioStreamPos(streamid, Float:posX, Float:posY, Float:posZ);
```
Moves any active stream (`streamid`) to `(posX, posY, posZ)`, automatically forces `usepos = true`, clears any active attachment, and immediately adds/removes listeners and updates active clients (with MP3-frame-aligned playback resumption for transcoded streams).
* **Returns:** `1` on success, `0` if `streamid` was not found or coordinates are invalid.

---

## 2. Spatial & Attachment Natives

### `SA_AttachAudioStreamToPlayer`
```pawn
native SA_AttachAudioStreamToPlayer(streamid, target_playerid);
```
Attaches an active stream to dynamically track `target_playerid`'s 3D position, Virtual World, and Interior every `spatial_update_interval_ms` (even if started with `usepos = false`). Because the SA-MP client cannot move 3D audio coordinates after `PlayAudioStreamForPlayer` starts, attached sources send `usepos = false` to the client and play at full volume (without distance attenuation) inside range and silence outside range, while fixed positions (`SA_SetAudioStreamPos`) retain 3D attenuation.
* **Returns:** `1` on success, `0` if the stream does not exist or `target_playerid` is not connected.

### `SA_AttachAudioStreamToVehicle`
```pawn
native SA_AttachAudioStreamToVehicle(streamid, vehicleid);
```
Attaches an active stream to dynamically track `vehicleid`'s 3D position every `spatial_update_interval_ms` (even if started with `usepos = false`). Because the SA-MP client cannot move 3D audio coordinates after `PlayAudioStreamForPlayer` starts, attached sources send `usepos = false` to the client and play at full volume (without distance attenuation) inside range and silence outside range, while fixed positions (`SA_SetAudioStreamPos`) retain 3D attenuation. The stream also follows the vehicle's virtual world and interior (`2.3.1`; the interior is read on open.mp, while SA-MP 0.3.7 has no `GetVehicleInterior` and any interior matches), so players in other worlds near the same coordinates do not hear it. The range comes from the stream's `distance` (play options or `SA_SetAudioStreamDistance`); attaching does not re-send the stream to listeners.
* **Returns:** `1` on success, `0` if the stream does not exist or `vehicleid` has no valid position.

---

## 3. Asynchronous Music Search Natives

### `SA_SearchAudioStream`
```pawn
native SA_SearchAudioStream(playerid, const query[], max_results = 5);
```
Starts a non-blocking background search for `query` (clamped between `1` and `extractor.max_search_results`). Cancels/supersedes any older pending search for `playerid`. Triggers `OnAudioStreamSearchResultItem` per item followed by `OnAudioStreamSearchResults`.
* **Returns:** Valid `search_id` (`>= 1`), or `INVALID_AUDIO_SEARCH_ID` (`-1`) if the query is empty or engine uninitialized.

### `SA_CancelAudioStreamSearch`
```pawn
native SA_CancelAudioStreamSearch(playerid);
```
Cancels any pending asynchronous search for `playerid` so its callbacks will not be delivered.
* **Returns:** `1` if a pending search was cancelled, `0` otherwise.

---

## 4. Synchronized Radio Channel Natives

### `SA_CreateAudioStreamChannel`
```pawn
native SA_CreateAudioStreamChannel(const name[], creator_playerid = INVALID_PLAYER_ID, const password[] = "", E_SA_CHANNEL_DESTROY_MODE:destroy_mode = SA_CHANNEL_DESTROY_MANUAL, destroy_timeout_sec = 0, bool:persistent = true);
```
Creates a new synchronized audio channel with a unique HTTP live endpoint. If `creator_playerid` is a connected player, their name is stored as the creator; otherwise `"Server"` is stored. Optional password protection (`""` = public) and automatic destruction mode (`Manual`, `OnDisconnect`, or `Timeout` when creator is offline) can be configured on creation. Automatic destruction modes (`ON_DISCONNECT` and `ON_TIMEOUT`) require a valid connected creator; server-owned channels (`INVALID_PLAYER_ID`) stay in `SA_CHANNEL_DESTROY_MANUAL`.
* **`persistent`:** `true` (default) = saved to disk (`scriptfiles/SampAudioStream/channels/`), restored across server restarts, and kept alive across GMX (`OnAudioStreamChannelRestored`); `false` = in-memory only, never written to disk, not restored after restart, and destroyed on GMX (`OnAudioStreamChannelDestroyed`).
* **Returns:** Valid `channelid` (`>= 1`), or `INVALID_AUDIO_CHANNEL_ID` (`-1`).

```pawn
// Temporary boombox channel for a player: lives only in RAM and disappears on GMX/restart
new boombox_ch = SA_CreateAudioStreamChannel("Boombox", playerid, "", SA_CHANNEL_DESTROY_ON_DISCONNECT, 0, .persistent = false);
```

### `SA_DestroyAudioStreamChannel`
```pawn
native SA_DestroyAudioStreamChannel(channelid);
```
Stops playback, removes all listeners, destroys `channelid`, and dispatches `OnAudioStreamChannelDestroyed(channelid, SA_CH_DESTROY_REASON_MANUAL)`.
* **Returns:** `1` on success, `0` if not found.

### `SA_PlayChannelAudioStream`
```pawn
native SA_PlayChannelAudioStream(channelid, const url[]);
```
Immediately starts playing `url` (or search query) on `channelid`, replacing any currently playing track.

### `SA_QueueChannelAudioStream`
```pawn
native SA_QueueChannelAudioStream(channelid, const url[], const label[] = "");
```
Appends `url` (or search query) to `channelid`'s playback queue, optionally associating a friendly display `label` (tabs/CR/LF replaced with spaces, truncated cleanly on UTF-8 codepoint boundaries to 128 bytes; persisted with the channel). If the channel is idle, starts playing immediately.

### `SA_SkipChannelAudioStream`
```pawn
native SA_SkipChannelAudioStream(channelid);
```
Skips the current track on `channelid` and immediately advances to the next queued track (or goes idle if the queue is empty).

### `SA_StopChannelAudioStream`
```pawn
native SA_StopChannelAudioStream(channelid);
```
Clears the queue and stops current playback on `channelid` (listeners remain subscribed to the channel).

### `SA_PauseChannelAudioStream`
```pawn
native SA_PauseChannelAudioStream(channelid, bool:pause);
```
Pauses (`true`) or resumes (`false`) playback on `channelid`.

### `SA_SetChannelAudioStreamVolume`
```pawn
native SA_SetChannelAudioStreamVolume(channelid, Float:volume);
```
Sets volume (`0.0` to `200.0`, clamped) on `channelid`.

### `SA_JoinAudioStreamChannel`
```pawn
native SA_JoinAudioStreamChannel(playerid, channelid, const password[] = "", bool:ignore_password = false);
```
Subscribes `playerid` to `channelid` (automatically stops any personal stream or previous channel). Returns `0` if the channel is password-protected and `password` does not match. Passing `ignore_password = true` (`2.3.2`) joins without checking the password, so the script can let the creator or its admins in without typing it; the plugin does not decide who may skip it. If `channelid` is currently playing, the player immediately starts listening at the live position. If the player was already hearing this same channel through its range (positioned channel with `auto_listen`), the stream keeps playing without a restart and only `OnPlayerAudioStreamSourceChange` fires.

### `SA_LeaveAudioStreamChannel`
```pawn
native SA_LeaveAudioStreamChannel(playerid);
```
Unsubscribes `playerid` from their current channel and stops audio playback. If the channel is positioned with `auto_listen` and the player is inside its range, they keep hearing it through the range without a restart (only `OnPlayerAudioStreamSourceChange` fires).

### `SA_IsPlayerInAudioStreamChannel`
```pawn
native bool:SA_IsPlayerInAudioStreamChannel(playerid, channelid);
```
Returns `true` if `playerid` is currently subscribed to `channelid`.

### `SA_GetPlayerAudioStreamChannel`
```pawn
native SA_GetPlayerAudioStreamChannel(playerid);
```
Returns the `channelid` that `playerid` is currently subscribed to, or `INVALID_AUDIO_CHANNEL_ID` (`-1`).

### `SA_SetAudioStreamChannelPass`
```pawn
native SA_SetAudioStreamChannelPass(channelid, const password[]);
```
Sets or updates the password for `channelid` (pass `""` to make it public).

### `SA_CheckAudioStreamChannelPass`
```pawn
native bool:SA_CheckAudioStreamChannelPass(channelid, const password[]);
```
Returns `true` if `channelid` has no password or `password` matches.

### `SA_SetAudioStreamChannelCreator`
```pawn
native SA_SetAudioStreamChannelCreator(channelid, playerid);
```
Assigns/reclaims ownership of `channelid` to a connected `playerid` (updating the stored creator name and immediately resetting any active `SA_CHANNEL_DESTROY_ON_TIMEOUT` countdown) or passes `INVALID_PLAYER_ID` to release ownership (reverting any `ON_DISCONNECT` / `ON_TIMEOUT` policy back to `SA_CHANNEL_DESTROY_MANUAL`).

### `SA_SetAudioStreamChannelDestroy`
```pawn
native SA_SetAudioStreamChannelDestroy(channelid, E_SA_CHANNEL_DESTROY_MODE:destroy_mode, destroy_timeout_sec = 0);
```
Updates the automatic destruction policy of `channelid` (`Manual`, `OnDisconnect`, or `Timeout` seconds active only while the creator is offline). Enabling `SA_CHANNEL_DESTROY_ON_DISCONNECT` or `SA_CHANNEL_DESTROY_ON_TIMEOUT` requires an assigned creator; if the channel has no creator, returns `0`, logs a warning, and keeps `SA_CHANNEL_DESTROY_MANUAL`.

### `SA_GetAudioStreamChannelDestroy`
```pawn
native SA_GetAudioStreamChannelDestroy(channelid, &E_SA_CHANNEL_DESTROY_MODE:destroy_mode, &destroy_timeout_sec, &destroy_remaining_sec, &bool:creator_online);
```
Reads `channelid`'s destroy policy, configured timeout, remaining countdown seconds (always equal to full `destroy_timeout_sec` while `creator_online == true`, never counting down while online), and whether the creator is currently online.

### `SA_SetAudioStreamChannelIdleSuspend`
```pawn
native SA_SetAudioStreamChannelIdleSuspend(channelid, bool:enable, grace_seconds = 30);
```
Enables or disables idle suspension for `channelid`. When enabled and the channel has had no joined listener (`SA_JoinAudioStreamChannel`) for `grace_seconds` (`0..3600`), the plugin stops the playback and frees its transcoder slot, remembering the track (`current_input`, playlist position) and the second it reached. The next joined listener resumes it on the following tick at that same track and second (live radios simply reconnect); a manual pause is kept. While suspended:
* `SA_GetAudioStreamChannelInfo` reports `SA_STATE_SUSPENDED`, and the track title, artist and duration stay readable through `SA_GetAudioStreamChannelTrack`.
* `SA_GetAudioStreamChannelElapsed` returns the saved second.
* `SA_SkipChannelAudioStream` moves the saved position to the next item without starting playback (at the end of the list without `SA_CHANNEL_LOOP_QUEUE` the channel stops), and `SA_SeekChannelAudioStream` moves the saved second.
* Queued tracks do not auto-start; `SA_PlayChannelAudioStream` / `SA_PlayAudioStreamChannelQueue` start playback right away and the channel is suspended again after the grace time if nobody joins.
* `SA_StopChannelAudioStream` clears the suspension.

Positioned channels (`SA_SetAudioStreamChannelPos` / attachments) are never suspended because anyone in range hears them. Persistent channels save the setting (`idle_suspend`, `idle_suspend_seconds`) and the saved second (`resume_offset_seconds`); after a restart, a channel that was playing comes back suspended and only starts for its first listener. Disabling the option on a suspended channel resumes it immediately.
* **Returns:** `1` on success, `0` if `channelid` does not exist.

### `SA_GetAudioStreamChannelIdleSuspend`
```pawn
native SA_GetAudioStreamChannelIdleSuspend(channelid, &bool:enable, &grace_seconds, &bool:suspended);
```
Reads whether idle suspension is enabled, its grace time and whether the channel is suspended right now.
* **Returns:** `1` if the channel exists, `0` otherwise.

### `SA_GetAudioStreamChannelCount`
```pawn
native SA_GetAudioStreamChannelCount();
```
Returns the number of currently existing synchronized channels.

### `SA_GetAudioStreamChannelID`
```pawn
native SA_GetAudioStreamChannelID(index);
```
Returns the `channelid` at `0`-based `index` (sorted ascending), or `INVALID_AUDIO_CHANNEL_ID` (`-1`) if out of range. Useful for populating dynamic channel menus together with `SA_GetAudioStreamChannelCount`.

### `SA_GetAudioStreamChannelInfo`
```pawn
native SA_GetAudioStreamChannelInfo(channelid, name[], name_len = sizeof(name), creator_name[], creator_len = sizeof(creator_name), &listener_count, &E_SA_STREAM_STATE:channel_state, &bool:has_password, &queue_count);
```
Retrieves `channelid`'s name, stored creator name, active listener count, stream state, password lock status, and queued track count.
* **Returns:** `1` if `channelid` exists, `0` otherwise.

### `SA_GetAudioStreamChannelTrack`
```pawn
native SA_GetAudioStreamChannelTrack(channelid, title[], title_len = sizeof(title), artist[], artist_len = sizeof(artist), &duration);
```
Retrieves metadata (`title`, `artist`, `duration` in seconds) of the track currently playing on `channelid`. For direct live radios, `title` starts as a generic placeholder (`"Direct Audio Stream"` / `"Radio Stream"`) unless the station name was found while probing the link; after the first ICY poll it is replaced by the station's `icy-name` header when available (no callback is re-fired; read it from `OnAudioStreamChannelLiveTitleChange` / `OnAudioStreamLiveTitleChange`).

### `SA_GetAudioStreamChannelQueue`
```pawn
native SA_GetAudioStreamChannelQueue(channelid, index, item[], item_len = sizeof(item));
```
Reads the `0`-based `index` entry from `channelid`'s queued URL/query list.
* **Returns:** `1` if found, `0` if invalid channel or out of range.

### `SA_GetAudioStreamChannelQueueLabel`
```pawn
native SA_GetAudioStreamChannelQueueLabel(channelid, index, label[], label_len = sizeof(label));
```
Reads the friendly display label for the `0`-based `index` queue entry on `channelid` (for example, `Artist - Title` from scripts or the entry title populated by `SA_QueueChannelAudioStreamPlaylist`). `label` may be empty (`""`) if no label was supplied or if the channel was restored from an older JSON without `label_hex`.
* **Returns:** `1` if the channel and index are valid (even when `label` is empty), `0` if `channelid` does not exist or `index` is out of range.

### `SA_SetAudioStreamChannelDescription`
```pawn
native SA_SetAudioStreamChannelDescription(channelid, const description[]);
```
Sets a free-text description for `channelid` (for example, what the channel plays or its rules). Control characters are removed, outer spaces are trimmed and the text is cut to `256` UTF-8 bytes without splitting a character. Pass `""` to clear it. Persistent channels save it (`description_hex` / `pawn_description_hex`) and restore it after restarts and GMX.
* **Returns:** `1` on success, `0` if `channelid` does not exist.

### `SA_GetAudioStreamChannelDescription`
```pawn
native SA_GetAudioStreamChannelDescription(channelid, description[], description_len = sizeof(description));
```
Reads the description set with `SA_SetAudioStreamChannelDescription`. `description` is empty (`""`) when none was set or the channel was restored from a file saved before 2.3.2.
* **Returns:** `1` if the channel exists (even when the description is empty), `0` otherwise.

### `SA_SetAudioStreamChannelQueueDescription`
```pawn
native SA_SetAudioStreamChannelQueueDescription(channelid, index, const description[]);
```
Sets a free-text description for the `0`-based `index` queue entry (same cleanup and `256`-byte limit as the channel description; `""` clears it). The description belongs to the entry: it follows it through `SA_MoveAudioStreamChannelQueue`, `SA_ShuffleAudioStreamChannelQueue`, removals of other entries, `SA_CHANNEL_LOOP_QUEUE` and playlist mode, and is saved with persistent channels. Entries added by `SA_QueueChannelAudioStream`, `SA_InsertAudioStreamChannelQueue` or `SA_QueueChannelAudioStreamPlaylist` start without a description. Changing the entry that is playing also updates `SA_GetAudioStreamChannelTrackDescription`.
* **Returns:** `1` on success, `0` if `channelid` does not exist or `index` is out of range.
* **Example:**
```pawn
new idx = SA_FindAudioStreamChannelQueue(ch, "library://Rock/song.mp3");
if (idx != -1) SA_SetAudioStreamChannelQueueDescription(ch, idx, "Requested by the event winner");
```

### `SA_GetAudioStreamChannelQueueDescription`
```pawn
native SA_GetAudioStreamChannelQueueDescription(channelid, index, description[], description_len = sizeof(description));
```
Reads the description of the `0`-based `index` queue entry. `description` may be empty (`""`).
* **Returns:** `1` if the channel and index are valid (even when the description is empty), `0` if `channelid` does not exist or `index` is out of range.

### `SA_GetAudioStreamChannelTrackDescription`
```pawn
native SA_GetAudioStreamChannelTrackDescription(channelid, description[], description_len = sizeof(description));
```
Reads the description of the track `channelid` is playing: the description of the queue entry it was started from (also when the entry already left the queue outside playlist mode). It is empty when nothing is playing, the track was started directly with `SA_PlayChannelAudioStream`, or the entry has no description. Useful inside `OnAudioStreamChannelTrackStart`.
* **Returns:** `1` if the channel exists (even when the description is empty), `0` otherwise.

### `SA_FindAudioStreamChannelQueue`
```pawn
native SA_FindAudioStreamChannelQueue(channelid, const item[], start_index = 0);
```
Searches `channelid`'s queue for an entry whose URL/query is exactly `item` (case-sensitive, the same text `SA_GetAudioStreamChannelQueue` returns), starting at the `0`-based `start_index` (negative values start at `0`). The comparison runs inside the plugin, so checking whether a track is already queued costs a single native call instead of reading every entry from Pawn, which keeps large queues (`channels.max_queue_per_channel` up to `1000`) cheap to inspect.
* **Returns:** the index of the first match at or after `start_index`, or `-1` if there is none, `item` is empty or `channelid` does not exist.
* **Example:**
```pawn
if (SA_FindAudioStreamChannelQueue(ch, "library://Rock/song.mp3") != -1) {
    // Already in the list: highlight it instead of adding it again
}
```

### `SA_ClearAudioStreamChannelQueue`
```pawn
native SA_ClearAudioStreamChannelQueue(channelid);
```
Clears all pending queued tracks on `channelid` without interrupting the track currently playing.

### `SA_SetAudioStreamChannelPos`
```pawn
native SA_SetAudioStreamChannelPos(channelid, Float:x, Float:y, Float:z, Float:distance = 50.0, worldid = -1, interiorid = -1, bool:auto_listen = true);
```
Configures 3D positional playback and world/interior filtering on `channelid` (client receives `usepos = true` for distance attenuation).
* **`auto_listen = true`** (e.g. public club/bar): any player within `distance` hears the channel automatically without `SA_JoinAudioStreamChannel` (competing in the `global` arbitration tier in order of range entry), while explicitly joined players stay in the `channel` tier inside range.
* **`auto_listen = false`** (restricted/password-protected area): only explicitly subscribed players (`SA_JoinAudioStreamChannel`) **and** within range hear audio; players out of range remain silently subscribed. Setting a channel password only restricts `SA_JoinAudioStreamChannel`, so use `auto_listen = false` to restrict physical access to an area.
* **Returns:** `0` if the channel does not exist, any coordinate/distance is non-finite, or `distance <= 0.0`.

### `SA_SetAudioStreamChannelGroup`
```pawn
native SA_SetAudioStreamChannelGroup(channelid, group);
```
Assigns `channelid` to range group `group` (`0` = no group, the default; `1..31`) (`2.3.1`). Players who muted that group with `SA_SetPlayerAudioStreamGroupMuted` stop hearing the channel through its range; joined listeners are not affected. The group is saved with persistent channels.
* **Returns:** `1` on success, `0` if the channel does not exist or `group` is outside `0..31`.

### `SA_GetAudioStreamChannelGroup`
```pawn
native SA_GetAudioStreamChannelGroup(channelid);
```
* **Returns:** the channel's range group (`0..31`), or `-1` if `channelid` does not exist.

### `SA_ClearAudioStreamChannelPos`
```pawn
native SA_ClearAudioStreamChannelPos(channelid);
```
Removes spatial positioning from `channelid`. Range-only listeners stop hearing audio, and explicitly joined subscribers resume hearing the channel globally anywhere on the map.

### `SA_AttachAudioStreamChannelToPlayer`
```pawn
native SA_AttachAudioStreamChannelToPlayer(channelid, target_playerid, Float:distance = 0.0);
```
Dynamically attaches `channelid`'s position (and `worldid`/`interiorid`) to `target_playerid`. Because the SA-MP client cannot move 3D audio coordinates after `PlayAudioStreamForPlayer` starts, attached channels send `usepos = false` to the client (playing at full volume inside range and stopping outside range), whereas fixed-position channels (`SA_SetAudioStreamChannelPos`) use `usepos = true` with distance attenuation. If `target_playerid` disconnects, attachment clears and playback stays fixed (`usepos = true`) at their last known coordinates. Player attachments do not survive server restarts or GMX (the last fixed position is kept/persisted instead).
* **`distance`** (`2.3.1`): a value above `0.0` sets the hearing range in the same update as the attachment, so players already hearing the channel receive a single re-dispatch (calling `SA_SetAudioStreamChannelPos` and then attaching sends two updates in a row, and the SA-MP client can play both streams at once when the second arrives while the first is still connecting). `0.0` (default) keeps the current range (`50.0` unless `SA_SetAudioStreamChannelPos` set another).
* **Returns:** `0` if the channel or target does not exist, or `distance` is not finite.

### `SA_AttachAudioStreamChannelToVehicle`
```pawn
native SA_AttachAudioStreamChannelToVehicle(channelid, vehicleid, Float:distance = 0.0);
```
Dynamically attaches `channelid`'s position, virtual world and interior to `vehicleid` (ideal for mobile sound cars; world and interior tracking since `2.3.1`, interior on open.mp only). Because the SA-MP client cannot move 3D audio coordinates after `PlayAudioStreamForPlayer` starts, attached channels send `usepos = false` to the client (playing at full volume inside range and stopping outside range), whereas fixed-position channels (`SA_SetAudioStreamChannelPos`) use `usepos = true` with distance attenuation. If the vehicle ceases to exist, attachment clears and playback stays fixed (`usepos = true`) at its last position. Vehicle attachments are not persisted across server restarts or GMX because vehicle IDs are reassigned on reload; the last fixed position is kept and scripts can re-attach inside `OnAudioStreamChannelRestored`.
* **`distance`** (`2.3.1`): a value above `0.0` sets the hearing range in the same update as the attachment, so players already hearing the channel receive a single re-dispatch (calling `SA_SetAudioStreamChannelPos` and then attaching sends two updates in a row, and the SA-MP client can play both streams at once when the second arrives while the first is still connecting). `0.0` (default) keeps the current range (`50.0` unless `SA_SetAudioStreamChannelPos` set another).
* **Returns:** `0` if the channel or target does not exist, or `distance` is not finite.

### `SA_RemoveAudioStreamChannelQueue`
```pawn
native SA_RemoveAudioStreamChannelQueue(channelid, index);
```
Removes the `0`-based `index` entry from `channelid`'s queue. Returns `0` if the channel or index is invalid.

### `SA_InsertAudioStreamChannelQueue`
```pawn
native SA_InsertAudioStreamChannelQueue(channelid, index, const url[], const label[] = "");
```
Inserts `url` (with optional friendly display `label`) at `0`-based `index` (`0` = play next, `queue_count` = append at end; values above `queue_count` return `0`). Respects `channels.max_queue_per_channel`, starts immediately if the channel is stopped, and pre-resolves into cache if inserted at index `0` while active.

### `SA_MoveAudioStreamChannelQueue`
```pawn
native SA_MoveAudioStreamChannelQueue(channelid, from_index, to_index);
```
Moves the queue entry at `from_index` to `to_index` (both `0`-based) while preserving both its URL and original UTF-8/Pawn display label intact without charset re-conversion (keeping non-cp1252 characters such as Japanese or emoji from imported playlists undamaged). Marks the channel dirty for persistence and pre-resolves the new first item if `channelid` has an active run and queue index `0` changed.
* **Returns:** `1` on success (including valid `from_index == to_index` no-op), `0` if `channelid` is invalid or either index is negative / `>= queue_count`.

### `SA_ShuffleAudioStreamChannelQueue`
```pawn
native SA_ShuffleAudioStreamChannelQueue(channelid);
```
Performs a one-time in-place random shuffle (`std::shuffle` seeded with `std::random_device`) on the queued items of `channelid` without interrupting or changing the currently playing track, and marks the channel dirty for persistence. If the channel is actively playing and index `0` changed after shuffling, pre-resolves the new next track asynchronously into cache.
* **Returns:** `1` if `channelid` exists (even if the queue has `0` or `1` items, which is a no-op), `0` if `channelid` is invalid.

### `SA_QueueChannelAudioStreamPlaylist`
```pawn
native SA_QueueChannelAudioStreamPlaylist(channelid, const url[], max_items = 25);
```
Asynchronously imports up to `clamp(max_items, 1, channels.max_queue_per_channel)` tracks from playlist URL `url` into `channelid` on `extraction_pool_` (one import per channel at a time, `60s` timeout up to 200 items plus `10s` per further 100 items, cancelled if the channel is destroyed). If `channelid` is stopped/idle, the first item begins playing immediately and the rest are queued; if the concurrent transcoder limit is reached when trying to start the first track, nothing is queued and `OnAudioStreamChannelPlaylistQueued` fires with `added = 0`, `SA_ERR_INTERNAL_ERROR`, and `"Maximum concurrent transcoders reached"`. Fires `OnAudioStreamChannelPlaylistQueued` on completion.
* **Returns:** `1` if the import was scheduled, `0` if `channelid` is invalid, `url` is not an HTTP(S) URL, the extractor is not ready, or an import is already pending for `channelid`.

### `SA_SeekChannelAudioStream`
```pawn
native SA_SeekChannelAudioStream(channelid, seconds);
```
Seeks the currently playing or paused non-live track with known duration (`0 <= seconds < duration`) on `channelid` to `seconds`, advancing the buffer generation with the same track, loop mode, queue, and pause state, resetting elapsed time to `seconds`, and re-firing `OnAudioStreamChannelTrackStart` (without firing `OnAudioStreamChannelQueueEnd`). Counts as an in-place transcoder replacement under the concurrency limit. Seek position is not persisted across restarts.
* **Returns:** `1` on success, `0` if `channelid` does not exist, is not playing/paused, is live, has unknown duration, or `seconds` is out of range.

### `SA_SetAudioStreamChannelLoop`
```pawn
native SA_SetAudioStreamChannelLoop(channelid, E_SA_CHANNEL_LOOP_MODE:mode);
```
Sets loop behavior on natural track completion (`SA_CHANNEL_LOOP_OFF`, `SA_CHANNEL_LOOP_TRACK`, or `SA_CHANNEL_LOOP_QUEUE`). Track failures do **not** re-queue via loop (preventing broken-link loops); 5 consecutive failures without a successful `TrackStart` automatically stop the channel.

### `SA_GetAudioStreamChannelLoop`
```pawn
native E_SA_CHANNEL_LOOP_MODE:SA_GetAudioStreamChannelLoop(channelid);
```
Returns the current `E_SA_CHANNEL_LOOP_MODE` of `channelid` (`SA_CHANNEL_LOOP_OFF` if not found).

### `SA_GetAudioStreamChannelElapsed`
```pawn
native SA_GetAudioStreamChannelElapsed(channelid);
```
Returns elapsed playback seconds for the currently active track on `channelid` (excluding paused duration, frozen while paused, counting continuously for live streams), or `-1` if the channel does not exist or is not currently playing/paused.

### `SA_GetAudioStreamChannelListener`
```pawn
native SA_GetAudioStreamChannelListener(channelid, index);
```
Returns the `playerid` of the `0`-based `index` explicitly subscribed (`SA_JoinAudioStreamChannel`) listener in ascending `playerid` order, or `INVALID_PLAYER_ID` if out of range. Range-only `auto_listen` listeners are not included in this list or in `listener_count`.

### `SA_SetAudioStreamChannelName`
```pawn
native SA_SetAudioStreamChannelName(channelid, const name[]);
```
Renames `channelid` (reflected in `SA_GetAudioStreamChannelInfo`, `icy-name` for new HTTP connections, and persisted state). Returns `0` if `name` is empty or `channelid` does not exist.

### `SA_SetAudioStreamChannelPlaylist`
```pawn
native SA_SetAudioStreamChannelPlaylist(channelid, bool:enable);
```
Enables or disables persistent playlist mode on `channelid` (`false` by default = FIFO queue consumed as tracks play).
* Turning **on** preserves the currently active queued track (if any) at index `0` followed by the pending queue (`pos = 0`).
* Turning **off** discards items before the current playback position and reverts to classic queue mode (`pos = -1`).
* **Returns:** `1` if `channelid` exists, `0` otherwise.

### `SA_IsAudioStreamChannelPlaylist`
```pawn
native bool:SA_IsAudioStreamChannelPlaylist(channelid);
```
Returns `true` if playlist mode is currently enabled on `channelid`, `false` if disabled or invalid channel.

### `SA_GetAudioStreamChannelQueuePos`
```pawn
native SA_GetAudioStreamChannelQueuePos(channelid);
```
Returns the `0`-based index of the item currently playing from the playlist, or `-1` if playlist mode is disabled, stopped after natural end, or playing an out-of-list track started via `SA_PlayChannelAudioStream`.

### `SA_PlayAudioStreamChannelQueue`
```pawn
native SA_PlayAudioStreamChannelQueue(channelid, index);
```
Starts playback of item `index` (`0 .. queue_count - 1`) without reordering the list (`playlist_pos = index` in playlist mode; consumes that item in classic queue mode).
* **Returns:** `1` on success, `0` if `channelid` or `index` is invalid or stream limit is reached.

```pawn
SA_SetAudioStreamChannelPlaylist(ch, true);
SA_QueueChannelAudioStream(ch, "library://Rock/A.ogg", "Track A");
SA_QueueChannelAudioStream(ch, "library://Rock/B.mp3", "Track B");
SA_PlayAudioStreamChannelQueue(ch, 1); // Jump directly to index 1 ("Track B")
```

### `SA_GetAudioStreamChannelLiveTitle`
```pawn
native SA_GetAudioStreamChannelLiveTitle(channelid, title[], title_len = sizeof(title));
```
Copies the current live radio ICY `live_title` for `channelid` (converted using `scripting.pawn_text_encoding`) into `title`. This value is separate from both the channel name (`SA_GetAudioStreamChannelInfo`) and the track title/artist (`SA_GetAudioStreamChannelTrack`) and never alters either. May be empty if nothing has been received yet, the source is not a live internet radio, or a new track just started.
* **Returns:** `1` if `channelid` exists, `0` otherwise.

### `SA_SetAudioStreamChannelLiveTitle`
```pawn
native SA_SetAudioStreamChannelLiveTitle(channelid, const title[]);
```
Replaces or clears the stored `live_title` of the active track on `channelid` and updates the embedded HTTP server's outgoing ICY `StreamTitle` frame so connected clients display it cleanly (for example, after filtering out a station prefix inside `OnAudioStreamChannelLiveTitleChange`). Passing `""` clears `live_title` so the bottom-corner HUD reverts to the regular track title.
* Stripped of control characters and truncated cleanly to 256 UTF-8 bytes without splitting multibyte characters.
* Does **not** modify the channel name, the track's metadata title, or the internal last-received radio title used for poll deduplication, and does **not** re-fire `OnAudioStreamChannelLiveTitleChange`. Remains active for the current track (even finite tracks) until the next track begins.
* **Returns:** `1` on success, `0` if `channelid` does not exist or has no active playing/resolving track.

```pawn
public OnAudioStreamChannelLiveTitleChange(channelid, const title[])
{
    // Strip fixed station prefix or ignore station-only jingles
    if (!strcmp(title, "Radio Example FM", true))
    {
        SA_SetAudioStreamChannelLiveTitle(channelid, "");
        return 1;
    }
    if (!strncmp(title, "Radio Example - ", 16, true))
    {
        SA_SetAudioStreamChannelLiveTitle(channelid, title[16]);
    }
    return 1;
}
```

---

## 5. Diagnostics & Metadata Natives

### `SA_GetPlayerAudioStreamID`
```pawn
native SA_GetPlayerAudioStreamID(playerid);
```
Returns the personal `streamid` currently owned by `playerid`, or `INVALID_AUDIO_STREAM_ID` (`-1`).

### `SA_GetAudioStreamCount`
```pawn
native SA_GetAudioStreamCount();
```
Returns the number of currently active streams (both personal and global).

### `SA_GetAudioStreamID`
```pawn
native SA_GetAudioStreamID(index);
```
Returns the `streamid` at `0`-based `index` (sorted in ascending ID order), or `INVALID_AUDIO_STREAM_ID` (`-1`) if out of range.

### `SA_GetAudioStreamOwner`
```pawn
native SA_GetAudioStreamOwner(streamid);
```
Returns the owning `playerid` for a personal stream, or `INVALID_PLAYER_ID` if `streamid` is a global stream or does not exist.

### `SA_GetAudioStreamElapsed`
```pawn
native SA_GetAudioStreamElapsed(streamid);
```
Returns elapsed playback seconds for `streamid` (frozen while paused, adjusted after `SA_SeekAudioStream`), or `-1` if `streamid` does not exist or is not currently `Playing`/`Paused`.

### `SA_GetAudioStreamURL`
```pawn
native SA_GetAudioStreamURL(streamid, url[], url_len = sizeof(url));
```
Copies the original Pawn input URL or search query originally passed when creating `streamid` into `url`.
* **Returns:** `1` on success, `0` if `streamid` was not found.

### `SA_GetAudioStreamStats`
```pawn
native SA_GetAudioStreamStats(&active_streams, &active_channels, &active_transcoders, &active_listeners);
```
Reads runtime plugin counters: active stream count, active channel count, live transcoder job count, and currently listening player count (`SA_SOURCE_NONE` excluded). Always returns `1`.

### `SA_GetAudioStreamInfo`
```pawn
native SA_GetAudioStreamInfo(streamid, title[], title_len = sizeof(title), artist[], artist_len = sizeof(artist), &duration, &E_SA_STREAM_STATE:stream_state);
```
Retrieves current metadata (`title`, `artist`, `duration` in seconds, and `stream_state`) for `streamid`. For direct live radios, `title` starts as a generic placeholder (`"Direct Audio Stream"` / `"Radio Stream"`) unless the station name was found while probing the link; after the first ICY poll it is replaced by the station's `icy-name` header when available (no callback is re-fired; read it from `OnAudioStreamChannelLiveTitleChange` / `OnAudioStreamLiveTitleChange`).
* **Returns:** `1` if `streamid` exists, `0` otherwise.

### `SA_GetAudioStreamLiveTitle`
```pawn
native SA_GetAudioStreamLiveTitle(streamid, title[], title_len = sizeof(title));
```
Copies the current live radio `live_title` for personal or global `streamid` (converted via `scripting.pawn_text_encoding`) into `title`. Separate from `SA_GetAudioStreamInfo`'s track `title` and `artist` (never alters them). May be empty before the first ICY title arrives or for non-radio streams.
* **Returns:** `1` if `streamid` exists, `0` otherwise.

### `SA_SetAudioStreamLiveTitle`
```pawn
native SA_SetAudioStreamLiveTitle(streamid, const title[]);
```
Replaces or clears the stored `live_title` on active `streamid` and updates the embedded HTTP server's outgoing ICY `StreamTitle` without firing `OnAudioStreamLiveTitleChange`, altering `SA_GetAudioStreamInfo`, or changing the poll deduplication state. Passing `""` clears `live_title`.
* **Passthrough note:** For direct passthrough streams (where the SA-MP/open.mp client connects directly to the upstream radio server), the upstream station controls what the client draws in the bottom-corner HUD; this native still updates the stored `live_title` returned by `SA_GetAudioStreamLiveTitle`.
* **Returns:** `1` on success, `0` if `streamid` does not exist or has already stopped/finished/errored.

### `SA_IsAudioStreamExtractorReady`
```pawn
native bool:SA_IsAudioStreamExtractorReady();
```
Returns `true` once `yt-dlp` (and `quickjs-ng` `qjs` when `extractor.enable_js_runtime` is `true`) has been verified/downloaded and is ready to resolve external platform URLs and searches. On supported 64-bit hosts when `extractor.js_runtime` allows Deno, pinned Deno is downloaded and health-checked afterward in the background without delaying this readiness flag.

### `SA_GetAudioStreamHttpPort`
```pawn
native SA_GetAudioStreamHttpPort();
```
Returns the active TCP port bound by the embedded Asio HTTP streaming server (or `0` if not running).

### `SA_RefreshAudioLibrary`
```pawn
native SA_RefreshAudioLibrary();
```
Rescans `audio.music_dir` (depth-1 subfolders plus loose root files) in memory, filters supported extensions (`.mp3`, `.ogg`, `.opus`, `.wav`, `.flac`, `.aac`, `.m4a`) and filenames representable in `scripting.pawn_text_encoding`, invalidates cached `library://` entries, and enforces up to `200` folders and `1000` tracks per folder.
* **Returns:** Total non-empty folder count stored in memory.

### `SA_GetAudioLibraryFolderCount`
```pawn
native SA_GetAudioLibraryFolderCount();
```
Returns the number of non-empty library folders currently cached in memory (requires `SA_RefreshAudioLibrary()` first).

### `SA_GetAudioLibraryFolder`
```pawn
native SA_GetAudioLibraryFolder(index, name[], name_len = sizeof(name), &track_count);
```
Copies the name of folder `index` into `name` (`""` denotes loose audio files directly inside `audio.music_dir`) and writes its track count to `track_count`.
* **Returns:** `1` on success, `0` if `index` is out of range.

### `SA_GetAudioLibraryTrack`
```pawn
native SA_GetAudioLibraryTrack(folder_index, track_index, name[], name_len = sizeof(name));
```
Copies the filename (with extension) at `track_index` inside folder `folder_index` into `name`.
* **Returns:** `1` on success, `0` if either index is out of range.

```pawn
new folders = SA_RefreshAudioLibrary();
for (new f = 0; f < folders; f++)
{
    new fname[64], tcount;
    SA_GetAudioLibraryFolder(f, fname, sizeof(fname), tcount);
    for (new t = 0; t < tcount; t++)
    {
        new tname[128], ref[200];
        SA_GetAudioLibraryTrack(f, t, tname, sizeof(tname));
        format(ref, sizeof(ref), fname[0] ? "library://%s/%s" : "library:///%s", fname, tname);
    }
}
```

---

## 6. Hooked Vanilla SA-MP Natives

When `SA_DISABLE_NATIVE_HOOKS` is not defined (and `scripting.hook_native_audio` is enabled in `config.json`), standard audio calls are transparently upgraded:

```pawn
PlayAudioStreamForPlayer(playerid, url[], Float:posX = 0.0, Float:posY = 0.0, Float:posZ = 0.0, Float:distance = 50.0, bool:usepos = false, worldid = -1, interiorid = -1, Float:volume = 100.0, bool:notify_processing = true, bool:process_direct_audio_files = false, bool:stream_text = true);
StopAudioStreamForPlayer(playerid);
```
* `SA_DISABLE_NATIVE_HOOKS` and `hook_native_audio` apply only to `PlayAudioStreamForPlayer` and `StopAudioStreamForPlayer`.
* Direct `http://` or `https://` native audio URLs (`.mp3`/`.ogg`/`.wav`/`.flac`/`.aac`/`.pls`/`.m3u` or probed `audio/*` radios) with default `worldid = -1`, `interiorid = -1`, `volume = 100.0`, and `process_direct_audio_files = false` take the native SA-MP passthrough path by default (extensionless `https://` URLs are probed once and cached before deciding passthrough vs. `yt-dlp`).
* Passing `.process_direct_audio_files = true` (or `process_direct_audio_files = true`) intercepts direct music file links (`.mp3`, `.ogg`, `.wav`, `.flac`, `.aac`, `.m4a`, `.opus`) through `SampAudioStream`'s transcoder pipeline — unlocking pause/resume, live volume scaling, smooth distance updates, `OnAudioStreamProcessing`, and `OnAudioStreamFinished` — while live radio streams and `.pls`/`.m3u` playlists remain on passthrough.
* Passing `.stream_text = false` hides the bottom-corner client text (omits `icy-name`, `icy-metaint`, and inline ICY metadata frames) and skips ICY live-title polling on server-processed playback, while callbacks/query natives continue receiving metadata. For passthrough playback the remote server controls the text, so use `.process_direct_audio_files = true, .stream_text = false` on direct sound-effect files:
```pawn
// Short sound effect: processed by the plugin so it releases the player when it ends, without any on-screen text
PlayAudioStreamForPlayer(playerid, "http://example.com/sounds/click.mp3", .process_direct_audio_files = true, .stream_text = false);
```
* Any YouTube/platform link, `.m3u8` (HLS) playlist, `.m4a`/`.opus` container, search query, or call using `worldid`/`interiorid`/`volume` always routes through `SampAudioStream`.
* **`StopAudioStreamForPlayer` vs. `SA_StopAudioStreamForPlayer`:** Vanilla hooked `StopAudioStreamForPlayer(playerid)` stops the player's personal stream **and** leaves any synchronized radio channel (`SA_LeaveAudioStreamChannel`) so the player stops hearing audio (both with and without `SampAudioStream.inc`), whereas `SA_StopAudioStreamForPlayer(playerid)` stops only the personal stream. Note that **global spatial streams** (`SA_PlayAudioStream`) within range continue to be re-applied by periodic spatial updates: to silence a global stream for a player, stop the stream (`SA_StopAudioStream`), change its filter, or move the player out of range.

### `SetPlayerName` Creator Synchronization
```pawn
SetPlayerName(playerid, const name[]);
```
`SetPlayerName` is **always** observed—regardless of `SA_DISABLE_NATIVE_HOOKS` or `hook_native_audio`—without altering its behavior or return value (`SA_DISABLE_NATIVE_HOOKS` and `hook_native_audio` apply only to `PlayAudioStreamForPlayer` / `StopAudioStreamForPlayer`). On SA-MP this uses an unconditional AMX native-table hook across all loaded scripts; on open.mp it uses `PlayerChangeEventHandler::onPlayerNameChange`. This keeps online channel creators' stored names up to date (for example clan tags applied after login or admin renames) and recognizes returning creators as soon as their name matches. *(Limitation: C/C++ plugins that mutate a player's name in memory without calling `SetPlayerName` or open.mp APIs are captured when the player disconnects.)*

---

## 7. Callbacks

### `OnAudioStreamProcessing`
```pawn
forward OnAudioStreamProcessing(playerid, streamid, const url[], bool:from_cache);
```
Fired immediately when `SA_PlayAudioStreamForPlayer` or `SA_PlayAudioStream` accepts a stream and `notify_processing == true` (before `OnAudioStreamWaiting` / `OnAudioStreamReady`). `playerid` is `INVALID_PLAYER_ID` for global streams, `url` is the original input passed by the caller, and `from_cache` is `true` if the track metadata is already cached or routed synchronously without launching `yt-dlp`.

### `OnAudioStreamWaiting`
```pawn
forward OnAudioStreamWaiting(playerid, streamid, wait_ms);
```
Fired only when a platform (for example, YouTube with a pre-roll ad) holds media availability in the future (`available_at` within the 60 s cap), notifying the script of the remaining wait after resolution completes.
* **`wait_ms`** is the remaining time (rounded up) until the plugin releases audio playback; the player starts hearing audio slightly later once the client connects and fills its playback buffer (~1–2 s). `wait_ms` may be `0` when the wait elapsed just before the callback was dispatched; the ready/start callback follows immediately.
* **Guaranteed callback order:** `OnAudioStreamProcessing` → `OnAudioStreamWaiting` → `OnAudioStreamReady` → `OnAudioStreamStart`.
* Fired at most once per stream start; never fired when `available_at` is absent, already in the past, exceeds 60 s (ignored), or during internal downloader 403 re-resolution after `OnAudioStreamReady` has already fired.
* If the stream is stopped or replaced during the wait, no further callbacks arrive (`OnAudioStreamReady` / `OnAudioStreamStart`), so scripts should also clear any status indicator in their own stop/replace flow.

```pawn
public OnAudioStreamWaiting(playerid, streamid, wait_ms)
{
    if (playerid != INVALID_PLAYER_ID)
    {
        new msg[64];
        format(msg, sizeof(msg), "Starting in ~%d s...", (wait_ms + 999) / 1000);
        SendClientMessage(playerid, -1, msg);
    }
    return 1;
}
```

### `OnAudioStreamReady`
```pawn
forward OnAudioStreamReady(playerid, streamid, const title[], const artist[], duration, const url[]);
```
Fired as soon as stream metadata is resolved and playback is ready to begin (`playerid == INVALID_PLAYER_ID` for global streams; `url` is the resolved direct or local proxy endpoint). For direct live radios, `title` starts as a generic placeholder (`"Direct Audio Stream"` / `"Radio Stream"`) unless the station name was found while probing the link; after the first ICY poll it is replaced by the station's `icy-name` header when available (no callback is re-fired; read it from `OnAudioStreamChannelLiveTitleChange` / `OnAudioStreamLiveTitleChange`).

### `OnAudioStreamStart`
```pawn
forward OnAudioStreamStart(playerid, streamid);
```
Fired when playback packets/URL dispatch starts (`playerid == INVALID_PLAYER_ID` for global streams).

### `OnAudioStreamFinished`
```pawn
forward OnAudioStreamFinished(playerid, streamid);
```
Fired when a non-cancelled server-transcoded stream completes playback naturally. **Note:** Direct passthrough streams (`http://` or `https://` direct audio URLs played at `volume == 100.0`, including live radios/playlists or direct music files when `process_direct_audio_files` is `false`) are handed directly to the GTA client without server-side transcoder completion tracking, so `OnAudioStreamFinished` does not fire for passthrough sessions.

### `OnAudioStreamError`
```pawn
forward OnAudioStreamError(playerid, streamid, E_SA_ERROR_CODE:error_code, const error_message[]);
```
Fired if extraction, network fetching, remote URL generation, or transcoding fails on an individual or global stream. *(Synchronized channel errors use `OnAudioStreamChannelError`, and search errors are reported directly in `OnAudioStreamSearchResults`.)*

### `OnPlayerAudioStreamSourceChange`
```pawn
forward OnPlayerAudioStreamSourceChange(playerid, E_SA_AUDIO_STREAM_SOURCE:source_type, source_id);
```
Fired after arbitration updates a connected player's client source type (`SA_SOURCE_*`) or `source_id` (including transitions to `SA_SOURCE_NONE` / `-1` on stop, mute, stream completion, or leaving spatial range). Not fired for same-source updates such as track changes inside the same channel, distance adjustments, or seeking. *Note: Near the edge of a spatial radius, transitions follow `spatial_update_interval_ms`, so scripts can debounce rapid edge crossings if desired.*

### `OnPlayerAudioStreamNoConnect`
```pawn
forward OnPlayerAudioStreamNoConnect(playerid, E_SA_AUDIO_STREAM_SOURCE:source_type, source_id);
```
Fired on the main thread when a connected player does not open an HTTP connection to the plugin's streaming server within `connect_warn_timeout_ms` (~15 s by default) for a server-transcoded/proxied stream or synchronized channel (`SA_SOURCE_PERSONAL`, `SA_SOURCE_CHANNEL`, `SA_SOURCE_GLOBAL`, `SA_SOURCE_CHANNEL_RANGE`).
* **Never fires for direct passthrough streams:** When the GTA client connects directly to an upstream HTTP(S) origin, the plugin does not observe client HTTP connections. Also suppressed if the player disconnects or playback is stopped, paused, or replaced before timeout.
* **Limitation:** This callback only reports that the client did not open a connection. If the client connects but the audio is inaudible on the player's side (for example, muted in-game), the plugin cannot detect it.

```pawn
public OnPlayerAudioStreamNoConnect(playerid, E_SA_AUDIO_STREAM_SOURCE:source_type, source_id)
{
    #pragma unused source_type, source_id
    SendClientMessage(playerid, 0xFFAA00FF,
        "[Audio] Could not hear audio? Make sure your GTA SA radio volume is above 0 (Options > Audio).");
    return 1;
}
```

### `OnAudioStreamSearchResults`
```pawn
forward OnAudioStreamSearchResults(playerid, search_id, total_found, E_SA_ERROR_CODE:error_code, const error_message[]);
```
Fired after all `OnAudioStreamSearchResultItem` events for `search_id` have been dispatched. On success, `error_code` is `SA_ERR_NONE` and `error_message` is `""` (`total_found == 0` simply means no matches). On failure, `total_found` is `0`, `error_code` indicates the failure (e.g. `SA_ERR_EXTRACTOR_NOT_READY` or `SA_ERR_SEARCH_FAILED`), and `error_message` describes why it failed.

### `OnAudioStreamSearchResultItem`
```pawn
forward OnAudioStreamSearchResultItem(playerid, search_id, item_index, const title[], const artist[], duration, const video_id[]);
```
Fired once per search result before `OnAudioStreamSearchResults`. Build the YouTube URL as `"https://www.youtube.com/watch?v=" + video_id` or pass the URL/ID to `SA_PlayAudioStreamForPlayer`.

### `OnAudioStreamChannelDestroyed`
```pawn
forward OnAudioStreamChannelDestroyed(channelid, E_SA_CHANNEL_DESTROY_REASON:reason);
```
Fired asynchronously on the **next server tick** (after `SA_DestroyAudioStreamChannel` or the triggering lifecycle hook/event returns) whenever a synchronized radio channel is genuinely destroyed (manually via `SA_DestroyAudioStreamChannel`, automatically on creator disconnect / boot grace expiration, or upon offline creator timeout expiration). During GMX (`OnGameModeExit` / `Reset`), non-persistent channels (`persistent = false`) are destroyed immediately and notify remaining loaded filterscripts directly, whereas persistent channels (`persistent = true`) stay alive and receive `OnAudioStreamChannelRestored`.

### `OnAudioStreamChannelTrackWaiting`
```pawn
forward OnAudioStreamChannelTrackWaiting(channelid, wait_ms);
```
Fired only when a platform (for example, YouTube with a pre-roll ad) holds media availability in the future (`available_at` within the 60 s cap) for a track starting on `channelid` (including pre-resolved tracks that still have remaining wait).
* **`wait_ms`** is the remaining time (rounded up) until the plugin releases the channel track; listeners hear audio slightly later once the client connects and fills its playback buffer (~1–2 s). `wait_ms` may be `0` when the wait elapsed just before the callback was dispatched; the ready/start callback follows immediately.
* **Guaranteed callback order:** `OnAudioStreamChannelTrackWaiting` → `OnAudioStreamChannelTrackStart`.
* Fired at most once per track start; never fired when `available_at` is absent, already in the past, or exceeds 60 s (ignored).
* If playback on the channel is stopped, skipped, replaced, or destroyed during the wait, `OnAudioStreamChannelTrackStart` will not arrive, so scripts should also clear any status message in their own stop/skip/destroy flow.

```pawn
public OnAudioStreamChannelTrackWaiting(channelid, wait_ms)
{
    new msg[64];
    format(msg, sizeof(msg), "Channel track starting in ~%d s...", (wait_ms + 999) / 1000);
    for (new i = 0; i < MAX_PLAYERS; i++)
    {
        if (IsPlayerConnected(i) && SA_IsPlayerInAudioStreamChannel(i, channelid))
        {
            SendClientMessage(i, -1, msg);
        }
    }
    return 1;
}
```

### `OnAudioStreamChannelTrackStart`
```pawn
forward OnAudioStreamChannelTrackStart(channelid, const title[], const artist[], duration, const url[]);
```
Fired when a track on `channelid` finishes resolving and starts playback (on initial play, queue advance, skip, loop repeat, and boot restoration). `url` matches the track's Pawn input string. For direct live radios, `title` starts as a generic placeholder (`"Direct Audio Stream"` / `"Radio Stream"`) unless the station name was found while probing the link; after the first ICY poll it is replaced by the station's `icy-name` header when available (no callback is re-fired; read it from `OnAudioStreamChannelLiveTitleChange` / `OnAudioStreamLiveTitleChange`).

### `OnAudioStreamChannelQueueEnd`
```pawn
forward OnAudioStreamChannelQueueEnd(channelid);
```
Fired when `channelid`'s current track finishes naturally, its queue is empty, and `loop_mode` is `SA_CHANNEL_LOOP_OFF`. Not fired on `SA_StopChannelAudioStream`, manual skips on an empty queue, errors, or channel destruction.

### `OnAudioStreamChannelError`
```pawn
forward OnAudioStreamChannelError(channelid, E_SA_ERROR_CODE:error_code, const error_message[]);
```
Fired when a track on `channelid` fails to resolve or transcode (or when a restored channel cannot resume playback because `SA_ERR_EXTRACTOR_NOT_READY`), and when 5 consecutive failures automatically stop the channel (`SA_ERR_INTERNAL_ERROR`).

### `OnAudioStreamChannelRestored`
```pawn
forward OnAudioStreamChannelRestored(channelid, const name[], const creator_name[]);
```
Fired on the first tick after the gamemode loads (`OnGameModeInit` completes) for each persistent channel (`persistent = true`) restored from disk on server startup or kept alive across GMX.

### `OnAudioStreamChannelPlaylistQueued`
```pawn
forward OnAudioStreamChannelPlaylistQueued(channelid, added, E_SA_ERROR_CODE:error_code, const error_message[]);
```
Fired on the main thread when a `SA_QueueChannelAudioStreamPlaylist` operation finishes. On success, `added > 0` indicates how many tracks were started/queued, `error_code == SA_ERR_NONE`, and `error_message` is empty. If extraction fails, the queue was already full (`"Channel queue is full"`), or a stopped channel could not start its first track because the concurrency limit was reached (`"Maximum concurrent transcoders reached"`), `added == 0` and `error_code` / `error_message` (`SA_ERR_INTERNAL_ERROR` for queue/limit failures) describe the reason. Not fired if `channelid` is destroyed while extraction is running.

### `OnAudioStreamLiveTitleChange`
```pawn
forward OnAudioStreamLiveTitleChange(playerid, streamid, const title[]);
```
Fired when a live internet radio (Shoutcast/Icecast) played via `streamid` reports a new ICY `StreamTitle` (`playerid` is the owner for personal streams, or `INVALID_PLAYER_ID` / `65535` for global streams).

### `OnAudioStreamChannelLiveTitleChange`
```pawn
forward OnAudioStreamChannelLiveTitleChange(channelid, const title[]);
```
Fired when a live internet radio (Shoutcast/Icecast) playing on synchronized channel `channelid` reports a new ICY `StreamTitle`.

**Live Radio Text Notes (both callbacks):**
* **Separate metadata:** Live radio text is kept completely separate from the channel name and the track title/artist metadata, and never overwrites either.
* **Eligible sources only:** Fires **only** for direct internet radios (Shoutcast/Icecast streams emitting ICY `StreamTitle`, including mount URLs ending in extensions like `.mp3`). Never fires for finite audio files, YouTube (including YouTube Live), other `yt-dlp` platforms, or `.m3u8` HLS streams.
* **Update latency:** Title updates can arrive up to `audio.live_title_poll_seconds` (default `30s`) after the station switches songs.
* **Raw content:** `title` is the `StreamTitle` text sent by the station (control characters removed, truncated cleanly to 256 UTF-8 bytes). Many radios send `Artist - Title`, while some include station names, ads, or jingles; the plugin does **not** attempt to split artist/song/station because stations follow no uniform format. Your gamemode can ignore or rewrite undesirable strings using `SA_SetAudioStreamChannelLiveTitle` / `SA_SetAudioStreamLiveTitle`.

```pawn
public OnAudioStreamChannelLiveTitleChange(channelid, const title[])
{
    new msg[144];
    format(msg, sizeof(msg), "[Radio] Now playing: %s", title);
    for (new i = 0; i < MAX_PLAYERS; i++)
    {
        if (IsPlayerConnected(i) && SA_IsPlayerInAudioStreamChannel(i, channelid))
        {
            SendClientMessage(i, 0x00CCFFFF, msg);
        }
    }
    return 1;
}
```

---

## 8. Constants & Enums

### Constants
```pawn
#define SAMPAUDIOSTREAM_VERSION_MAJOR  (2)
#define SAMPAUDIOSTREAM_VERSION_MINOR  (3)
#define SAMPAUDIOSTREAM_VERSION_PATCH  (1)
#define SAMPAUDIOSTREAM_VERSION_STRING "2.3.1"

#define INVALID_AUDIO_STREAM_ID  (-1)
#define INVALID_AUDIO_CHANNEL_ID (-1)
#define INVALID_AUDIO_SEARCH_ID  (-1)
```

### `E_SA_ERROR_CODE`
| Value | Meaning |
| :--- | :--- |
| `SA_ERR_NONE` (`0`) | No error |
| `SA_ERR_INVALID_URL` (`1`) | Empty or invalid URL/query |
| `SA_ERR_EXTRACTION_FAILED` (`2`) | Extractor returned no playable stream |
| `SA_ERR_NETWORK_ERROR` (`3`) | HTTP / network failure |
| `SA_ERR_UNSUPPORTED_FORMAT` (`4`) | Unsupported codec or container |
| `SA_ERR_TRANSCODE_ERROR` (`5`) | Decoder/encoder pipeline error |
| `SA_ERR_EXTRACTOR_NOT_READY` (`6`) | Extractor binaries still downloading |
| `SA_ERR_SEARCH_FAILED` (`7`) | Search execution or JSON parse failure |
| `SA_ERR_STREAM_NOT_FOUND` (`8`) | Stream handle not found |
| `SA_ERR_INTERNAL_ERROR` (`9`) | Unexpected internal error |
| `SA_ERR_AUTH_REQUIRED` (`10`) | YouTube blocked the server IP or requires sign-in / age verification |

### `E_SA_STREAM_STATE`
| Value | Meaning |
| :--- | :--- |
| `SA_STATE_IDLE` (`0`) | Created, not started |
| `SA_STATE_RESOLVING` (`1`) | Extracting metadata / media URL |
| `SA_STATE_BUFFERING` (`2`) | Filling initial MP3 ring buffer |
| `SA_STATE_PLAYING` (`3`) | Actively streaming |
| `SA_STATE_PAUSED` (`4`) | Paused |
| `SA_STATE_STOPPED` (`5`) | Manually stopped |
| `SA_STATE_FINISHED` (`6`) | Completed naturally |
| `SA_STATE_ERROR` (`7`) | Terminated with an error |
| `SA_STATE_SUSPENDED` (`8`) | Channel stopped by idle suspension, waiting for its next listener (`SA_SetAudioStreamChannelIdleSuspend`) |

### `E_SA_CHANNEL_DESTROY_MODE`
| Value | Meaning |
| :--- | :--- |
| `SA_CHANNEL_DESTROY_MANUAL` (`0`) | Remains alive until destroyed manually |
| `SA_CHANNEL_DESTROY_ON_DISCONNECT` (`1`) | Destroyed automatically when creator disconnects *(requires creator)* |
| `SA_CHANNEL_DESTROY_ON_TIMEOUT` (`2`) | Countdown runs in seconds only while creator is offline; resets and pauses if creator reconnects or changes name to match *(matched case-insensitively by the name saved at disconnect via `OnPlayerConnect` & `SetPlayerName`; requires creator)* |

> [!NOTE]
> Returning creators are identified by **name** (case-insensitive match against the creator name active when they disconnected). Online creators keep ownership across renames (updating stored `creator_name`). If another player connects or renames to the same name while the original creator is offline, they reclaim ownership—account-based gamemodes should call `SA_SetAudioStreamChannelCreator` after account login to rebind ownership explicitly.

### `E_SA_CHANNEL_DESTROY_REASON`
| Value | Meaning |
| :--- | :--- |
| `SA_CH_DESTROY_REASON_MANUAL` (`0`) | Destroyed explicitly by `SA_DestroyAudioStreamChannel` |
| `SA_CH_DESTROY_REASON_CREATOR` (`1`) | Destroyed because creator disconnected |
| `SA_CH_DESTROY_REASON_TIMEOUT` (`2`) | Destroyed because creator offline timeout expired |

### `E_SA_CHANNEL_LOOP_MODE`
| Value | Meaning |
| :--- | :--- |
| `SA_CHANNEL_LOOP_OFF` (`0`) | Advance linearly through queue and end when empty |
| `SA_CHANNEL_LOOP_TRACK` (`1`) | Repeat the current track indefinitely on natural completion |
| `SA_CHANNEL_LOOP_QUEUE` (`2`) | Move finished track to the end of the queue and advance continuously |

### `E_SA_AUDIO_STREAM_SOURCE`
| Value | Meaning |
| :--- | :--- |
| `SA_SOURCE_NONE` (`0`) | Player is not currently receiving any audio stream (`source_id = -1`) |
| `SA_SOURCE_PERSONAL` (`1`) | Player is listening to their own personal stream (`source_id = streamid`) |
| `SA_SOURCE_CHANNEL` (`2`) | Player is listening to a joined synchronized channel (`source_id = channelid`) |
| `SA_SOURCE_GLOBAL` (`3`) | Player is listening to a global stream (`source_id = streamid`) |
| `SA_SOURCE_CHANNEL_RANGE` (`4`) | Player is hearing a spatial channel via `auto_listen` range (`source_id = channelid`) |
| `SA_SOURCE_SOUND` (`5`) | Player is hearing a short sound from `SA_PlayAudioStreamSoundForPlayer` (`source_id = -1`); the previous source resumes when it ends |
