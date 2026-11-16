# Complete API Reference

Exhaustive technical reference for **`sampaudiostream.inc`** (`v1.0.0`), covering all **27 `SA_*` natives**, **2 hooked standard natives**, **6 callbacks**, **2 enums**, and constants.

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
native SA_PlayAudioStreamForPlayer(playerid, const url[], Float:posX = 0.0, Float:posY = 0.0, Float:posZ = 0.0, Float:distance = 50.0, bool:usepos = false, worldid = -1, interiorid = -1, Float:volume = 100.0);
```
Plays a URL (YouTube, platform link, direct audio, Shoutcast/Icecast) or search query for a single player. Automatically stops any previous personal stream or radio channel for that player.
* **Returns:** Valid `streamid` (`>= 1`), or `INVALID_AUDIO_STREAM_ID` (`-1`) on failure.

### `SA_PlayAudioStream`
```pawn
native SA_PlayAudioStream(const url[], Float:posX = 0.0, Float:posY = 0.0, Float:posZ = 0.0, Float:distance = 50.0, bool:usepos = false, worldid = -1, interiorid = -1, Float:volume = 100.0);
```
Starts a global / spatial stream. Any connected player matching `worldid`, `interiorid`, and 3D `distance` (when `usepos == true`) hears it automatically upon entering range and stops upon leaving range. Finite tracks advance at `1x` real-time so latecomers hear the current playback timestamp; live broadcasts stream directly in real time.
* **Returns:** Valid `streamid` (`>= 1`), or `INVALID_AUDIO_STREAM_ID` (`-1`) on failure.

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

### `SA_SetPlayerAudioVolume`
```pawn
native SA_SetPlayerAudioVolume(playerid, Float:volume);
```
Adjusts volume (`0.0` to `200.0`, clamped) for `playerid`'s active personal stream.
* **Returns:** `1` on success, `0` if no personal stream is active.

### `SA_SetAudioStreamVolume`
```pawn
native SA_SetAudioStreamVolume(streamid, Float:volume);
```
Adjusts volume (`0.0` to `200.0`, clamped) for any active stream by `streamid`.
* **Returns:** `1` on success, `0` if `streamid` was not found.

---

## 2. Spatial & Attachment Natives

### `SA_AttachAudioStreamToPlayer`
```pawn
native SA_AttachAudioStreamToPlayer(streamid, target_playerid);
```
Attaches an active stream to dynamically track `target_playerid`'s 3D position, Virtual World, and Interior every `spatial_update_interval_ms`. Enables `usepos` on the stream.
* **Returns:** `1` on success, `0` if the stream does not exist or `target_playerid` is not connected.

### `SA_AttachAudioStreamToVehicle`
```pawn
native SA_AttachAudioStreamToVehicle(streamid, vehicleid);
```
Attaches an active stream to dynamically track `vehicleid`'s 3D position every `spatial_update_interval_ms`. Enables `usepos` on the stream.
* **Returns:** `1` on success, `0` if the stream does not exist or `vehicleid <= 0`.

---

## 3. Asynchronous Music Search Natives

### `SA_SearchAudio`
```pawn
native SA_SearchAudio(playerid, const query[], max_results = 5);
```
Starts a non-blocking background search for `query` (clamped between `1` and `performance.max_search_results`). Cancels/supersedes any older pending search for `playerid`. Triggers `OnAudioSearchResultItem` per item followed by `OnAudioSearchResults`.
* **Returns:** Valid `search_id` (`>= 1`), or `INVALID_AUDIO_SEARCH_ID` (`-1`) if the query is empty or engine uninitialized.

### `SA_CancelAudioSearch`
```pawn
native SA_CancelAudioSearch(playerid);
```
Cancels any pending asynchronous search for `playerid` so its callbacks will not be delivered.
* **Returns:** `1` if a pending search was cancelled, `0` otherwise.

---

## 4. Synchronized Radio Channel Natives

### `SA_CreateChannel`
```pawn
native SA_CreateChannel(const name[]);
```
Creates a new persistent synchronized audio channel with a unique HTTP live endpoint.
* **Returns:** Valid `channelid` (`>= 1`), or `INVALID_AUDIO_CHANNEL_ID` (`-1`).

### `SA_DestroyChannel`
```pawn
native SA_DestroyChannel(channelid);
```
Stops playback, removes all listeners, and destroys `channelid`.
* **Returns:** `1` on success, `0` if not found.

### `SA_PlayChannel`
```pawn
native SA_PlayChannel(channelid, const url[]);
```
Immediately starts playing `url` (or search query) on `channelid`, replacing any currently playing track.

### `SA_QueueChannel`
```pawn
native SA_QueueChannel(channelid, const url[]);
```
Appends `url` (or search query) to `channelid`'s playback queue. If the channel is idle, starts playing immediately.

### `SA_SkipChannel`
```pawn
native SA_SkipChannel(channelid);
```
Skips the current track on `channelid` and immediately advances to the next queued track (or goes idle if the queue is empty).

### `SA_StopChannel`
```pawn
native SA_StopChannel(channelid);
```
Clears the queue and stops current playback on `channelid` (listeners remain subscribed to the channel).

### `SA_PauseChannel`
```pawn
native SA_PauseChannel(channelid, bool:pause);
```
Pauses (`true`) or resumes (`false`) playback on `channelid`.

### `SA_SetChannelVolume`
```pawn
native SA_SetChannelVolume(channelid, Float:volume);
```
Sets volume (`0.0` to `200.0`, clamped) on `channelid`.

### `SA_JoinChannel`
```pawn
native SA_JoinChannel(playerid, channelid);
```
Subscribes `playerid` to `channelid` (automatically stops any personal stream or previous channel). If `channelid` is currently playing, the player immediately starts listening at the live position.

### `SA_LeaveChannel`
```pawn
native SA_LeaveChannel(playerid);
```
Unsubscribes `playerid` from their current channel and stops audio playback.

### `SA_IsPlayerInChannel`
```pawn
native bool:SA_IsPlayerInChannel(playerid, channelid);
```
Returns `true` if `playerid` is currently subscribed to `channelid`.

### `SA_GetPlayerChannel`
```pawn
native SA_GetPlayerChannel(playerid);
```
Returns the `channelid` that `playerid` is currently subscribed to, or `INVALID_AUDIO_CHANNEL_ID` (`-1`).

---

## 5. Diagnostics & Metadata Natives

### `SA_GetPlayerStreamID`
```pawn
native SA_GetPlayerStreamID(playerid);
```
Returns the personal `streamid` currently owned by `playerid`, or `INVALID_AUDIO_STREAM_ID` (`-1`).

### `SA_GetStreamInfo`
```pawn
native SA_GetStreamInfo(streamid, title[], title_len = sizeof(title), artist[], artist_len = sizeof(artist), &duration, &E_SA_STREAM_STATE:stream_state);
```
Retrieves current metadata (`title`, `artist`, `duration` in seconds, and `stream_state`) for `streamid`.
* **Returns:** `1` if `streamid` exists, `0` otherwise.

### `SA_IsExtractorReady`
```pawn
native bool:SA_IsExtractorReady();
```
Returns `true` once `yt-dlp` (and `qjs` when `enable_js_runtime` is true) has been verified/downloaded and is ready to resolve external platform URLs and searches.

### `SA_GetHttpPort`
```pawn
native SA_GetHttpPort();
```
Returns the active TCP port bound by the embedded Asio HTTP streaming server (or `0` if not running).

---

## 6. Hooked Vanilla SA-MP Natives

When `SA_DISABLE_NATIVE_HOOKS` is not defined (and `hook_native_audio` is enabled in `config.json`), standard calls are transparently upgraded:

```pawn
PlayAudioStreamForPlayer(playerid, url[], Float:posX = 0.0, Float:posY = 0.0, Float:posZ = 0.0, Float:distance = 50.0, bool:usepos = false, worldid = -1, interiorid = -1, Float:volume = 100.0);
StopAudioStreamForPlayer(playerid);
```
* Direct `.mp3`/`.ogg`/`.wav`/`.flac`/`.aac`/`.pls`/`.m3u` URLs with default `worldid = -1`, `interiorid = -1`, and `volume = 100.0` take the native SA-MP fast path.
* Any YouTube/platform link, search query, or call using `worldid`/`interiorid`/`volume` routes transparently through `SampAudioStream`.

---

## 7. Callbacks

### `OnAudioStreamReady`
```pawn
forward OnAudioStreamReady(playerid, streamid, const title[], const artist[], duration, const url[]);
```
Fired as soon as stream metadata is resolved and playback is ready to begin (`playerid == INVALID_PLAYER_ID` for global streams; `url` is the resolved direct or local proxy endpoint).

### `OnAudioStreamStart`
```pawn
forward OnAudioStreamStart(playerid, streamid);
```
Fired when playback packets/URL dispatch starts (`playerid == INVALID_PLAYER_ID` for global streams).

### `OnAudioStreamFinished`
```pawn
forward OnAudioStreamFinished(playerid, streamid);
```
Fired when a non-cancelled transcoded stream completes playback naturally.

### `OnAudioStreamError`
```pawn
forward OnAudioStreamError(playerid, streamid, E_SA_ERROR_CODE:error_code, const error_message[]);
```
Fired if extraction, network fetching, or transcoding fails.

### `OnAudioSearchResults`
```pawn
forward OnAudioSearchResults(playerid, search_id, total_found);
```
Fired after all `OnAudioSearchResultItem` events for `search_id` have been dispatched (`total_found == 0` when nothing matched or on error).

### `OnAudioSearchResultItem`
```pawn
forward OnAudioSearchResultItem(playerid, search_id, item_index, const title[], const artist[], duration, const video_id[]);
```
Fired once per search result before `OnAudioSearchResults`. Build the YouTube URL as `"https://www.youtube.com/watch?v=" + video_id` or pass the URL/ID to `SA_PlayAudioStreamForPlayer`.

---

## 8. Constants & Enums

### Constants
```pawn
#define SAMPAUDIOSTREAM_VERSION_MAJOR  (1)
#define SAMPAUDIOSTREAM_VERSION_MINOR  (0)
#define SAMPAUDIOSTREAM_VERSION_PATCH  (0)
#define SAMPAUDIOSTREAM_VERSION_STRING "1.0.0"

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
