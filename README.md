# SampAudioStream

[![GitHub release](https://img.shields.io/github/v/release/Walter-Correa/SampAudioStream?include_prereleases&color=blue)](https://github.com/Walter-Correa/SampAudioStream/releases)
[![License: MPL 2.0](https://img.shields.io/badge/License-MPL%202.0-brightgreen.svg)](https://opensource.org/licenses/MPL-2.0)
[![Platform](https://img.shields.io/badge/platform-SA--MP%20%7C%20open.mp-orange.svg)](https://open.mp)

**SampAudioStream** is a high-performance, asynchronous C++20 universal audio streaming plugin and native component engineered specifically for **San Andreas Multiplayer (SA-MP)** and **open.mp**.

It allows Pawn developers to play audio from **YouTube, SoundCloud, Twitch, Kick, Vimeo, TikTok**, and hundreds of other video/audio platforms—as well as **live broadcasts, Shoutcast/Icecast online radios, HLS streams, and direct MP3/OGG/WAV/FLAC links**—with **no installation on Windows** and **a single package on Linux** (32-bit `libcurl`), **zero `ffmpeg` dependency**, built-in **in-memory MP3 transcoding**, **spatial 3D/world/interior filtering**, **time-synchronized radius playback**, **asynchronous in-game music search**, and **synchronized multi-player radio channels**.

---

## Extending Standard SA-MP / open.mp AudioStream

| Feature / Capability | Standard `PlayAudioStreamForPlayer` | SampAudioStream |
| :--- | :--- | :--- |
| **Platform Architecture** | Built-in server native | ✅ Unified dual-runtime binary: native **open.mp Component** + **SA-MP Plugin** (32-bit `x86`) |
| **Server Dependencies** | None | ✅ Windows: none · Linux: 32-bit `libcurl` only (the server log prints the exact install command); static CRT (`/MT` & `-static-libstdc++`), embedded codecs, auto-provisioned `yt-dlp` + `quickjs-ng` |
| **Transcoding Engine** | Client plays direct HTTP audio only | ✅ Embedded static C++ pipeline (`libopus`, `libogg`, `fdk-aac`, `mp3lame`, `dr_libs`, `stb_vorbis`) + ICY `StreamTitle` framing |
| **Video, Audio & Live Sites** | Direct `.mp3`/`.ogg`/Shoutcast only | ✅ YouTube (videos & lives incl. RFC 8216 ADTS/fMP4), SoundCloud, Twitch, Kick, Vimeo, TikTok, HLS (`.m3u8` / MPEG-TS), extensionless Shoutcast/Icecast & more |
| **Drop-In Compatibility** | Native behavior | ✅ Transparently hooks `PlayAudioStreamForPlayer` & `StopAudioStreamForPlayer`, LAN-aware local URLs, CP1252 Pawn encoding default + full `SA_*` API |
| **World / Interior / Volume** | Global `usepos` only; no server volume scaling | ✅ Native `worldid`, `interiorid`, and `volume` (`0.0`–`200.0`) parameters |
| **Radius Time Synchronization** | Static URL starts from beginning per player | ✅ `SA_PlayAudioStream` paces finite tracks at `1x` real-time so latecomers entering the radius hear the current timestamp |
| **In-Game Music Search** | ❌ None | ✅ Non-blocking `SA_SearchAudioStream` returning structured results via callbacks for interactive Pawn dialogs |
| **Dynamic 3D Attachment** | Static coordinates only | ✅ `SA_AttachAudioStreamToPlayer` & `SA_AttachAudioStreamToVehicle` |
| **Synchronized Radio Channels** | ❌ None | ✅ `SA_CreateAudioStreamChannel`, `SA_QueueChannelAudioStream`, `SA_SkipChannelAudioStream`, 3D positional/vehicle/player channels, queue manipulation & loop modes, elapsed/listener queries, creator ownership, passwords, destroy policies, and atomic JSON disk persistence |
| **Caching & Memory Footprint** | Uncached client HTTP requests | ✅ In-memory LRU+TTL metadata/search cache + ~30s sliding ring buffers (zero MP3 disk bloat) |

---

## Documentation Hub

Modular guides and exhaustive API documentation are available in the [`docs/`](docs/) directory:

| Guide | Description |
| :--- | :--- |
| **[📖 Getting Started](docs/getting-started.md)** | Requirements, installation for SA-MP & open.mp, firewall setup, first startup log, `config.json` reference, troubleshooting, and FAQ. |
| **[🔊 Playback, Hooking & Spatial Audio](docs/playback-and-spatial.md)** | `SA_PlayAudioStreamForPlayer` vs. `SA_PlayAudioStream`, radius time sync, live streams, volume, pause, and 3D entity attachment. |
| **[🔎 Music Search & Synced Channels](docs/search-and-channels.md)** | Asynchronous `SA_SearchAudioStream` dialog workflows and synchronized multi-player radio stations with passwords, creator lifecycle, and queues. |
| **[⚙️ Architecture, Transcoding & Caching](docs/architecture-and-caching.md)** | Internal C++20 worker pool, embedded codecs, HLS/MPEG-TS live decoding, LRU RAM cache, and ~30s ring buffers. |
| **[📚 Complete API Reference](docs/api-reference.md)** | Exhaustive reference for all 81 `SA_*` natives, hooked vanilla natives, 19 callbacks, enums, constants, and helpers (plus optional `audio.normalize_volume` equalization for transcoded/proxy streams and live broadcasts, excluding direct passthrough). |
| **[⚠️ Important Notes](#important-notes)** | Extractor (`yt-dlp`) dependency notice, server owner content responsibility, and non-affiliation disclaimer. |

---

## Important Notes

* **Platform Extractor Dependency (`yt-dlp`):** Platform streams (YouTube, SoundCloud, Twitch, etc.) are resolved by `yt-dlp`, which the plugin downloads and keeps updated automatically. When a platform changes its internals, extraction may fail temporarily until a new `yt-dlp` release is published and picked up by the automatic update. Direct audio URLs and internet radios do not depend on `yt-dlp`. See [Troubleshooting](docs/getting-started.md#6-troubleshooting) for how to force an update and [the YouTube Cookies Guide](docs/youtube-cookies.md) if YouTube challenges your VPS IP with *"Sign in to confirm you're not a bot"*.
* **Content Responsibility:** SampAudioStream does not host, store, or distribute any media. It only relays audio that the server owner (or players, through your scripts) chooses to play. Server owners are solely responsible for ensuring they have the rights to broadcast the content played on their servers and for complying with the terms of service of the platforms they stream from.
* **No Affiliation:** SampAudioStream is an independent community project and is not affiliated with, endorsed by, or sponsored by YouTube, Google, SoundCloud, Twitch, Take-Two Interactive, Rockstar Games, the SA-MP team, or the open.mp team. All trademarks belong to their respective owners.

---

## Quick Start & Installation

Follow these 5 steps to install SampAudioStream and play your first track:

1. **Download the release archive for your server OS:** download `SampAudioStream-Windows.zip` or `SampAudioStream-Linux.zip` from [GitHub Releases](https://github.com/Walter-Correa/SampAudioStream/releases) (matching the **server** host OS). Verify the archive contains `plugins/`, `components/`, `include/SampAudioStream.inc`, and `filterscripts/FS_SampAudioStream_Demo.{pwn,amx}`. ([Full details](docs/getting-started.md#2-installation))
2. **Check host dependencies:** **Windows** requires nothing extra; **Linux** requires 32-bit `libcurl` (`libcurl.so.4`). If `libcurl` is missing on Linux, the server log prints the exact install command for your distribution. ([Distribution commands](docs/getting-started.md#1-requirements))
3. **Copy the binary folder and include file:** copy **only one** binary folder (`plugins/` **or** `components/` — never both) plus `include/SampAudioStream.inc` into `pawno/include/` (SA-MP) or `qawno/include/` (open.mp). ([Installation guide](docs/getting-started.md#2-installation))

   | Server Runtime | Binary Folder | Config Edit Required? |
   | :--- | :--- | :--- |
   | SA-MP 0.3.7 | `plugins/` | Yes, add `plugins SampAudioStream.dll` (or `.so`) in `server.cfg` |
   | open.mp (recommended) | `components/` | No (loaded automatically) |
   | open.mp legacy mode | `plugins/` | Yes, add `"SampAudioStream"` under `pawn.legacy_plugins` in `config.json` |

   > [!IMPORTANT]
   > Copy **only one** binary folder (`plugins/` **or** `components/`). Loading both in the same server process is rejected.

4. **Open the streaming TCP port:** allow inbound TCP traffic on `server_port + 1` (default `7778` when your server runs on `7777`, or your custom `http_port`) in your OS firewall and VPS security group (note: LAN players automatically receive local/loopback URLs when `lan_aware_urls` is `true`, while remote players require port-forwarding). Start the server and confirm `HTTP audio streaming server listening on ...` appears in the console log. ([Firewall & verification steps](docs/getting-started.md#3-open-the-audio-port-firewall))
5. **Include `<SampAudioStream>` in your Pawn script and compile:** place `#include <SampAudioStream>` **after** `<open.mp>` or `<a_samp>`, then recompile and start playing audio. Confirm `yt-dlp ... downloaded and SHA-256 verified` and `QuickJS-ng downloaded and SHA-256 verified` appear on first boot. ([First startup log](docs/getting-started.md#4-first-startup-what-the-log-should-show))

```pawn
#include <open.mp> // Or <a_samp>
#include <SampAudioStream>

CMD:play(playerid, params[])
{
    if (isnull(params)) return SendClientMessage(playerid, -1, "Usage: /play <URL or song name>");

    // Works with YouTube, SoundCloud, Twitch, direct MP3, radios, or plain search text
    SA_PlayAudioStreamForPlayer(playerid, params, 0.0, 0.0, 0.0, 50.0, false, -1, -1, 90.0);
    return 1;
}

CMD:zone(playerid, params[])
{
    if (isnull(params)) return SendClientMessage(playerid, -1, "Usage: /zone <URL or song name>");

    new Float:x, Float:y, Float:z;
    GetPlayerPos(playerid, x, y, z);

    // Everyone entering the 35m radius hears the song synchronized at its current timestamp
    SA_PlayAudioStream(params, x, y, z, 35.0, true, GetPlayerVirtualWorld(playerid), GetPlayerInterior(playerid), 85.0);
    return 1;
}
```

See [`FS_SampAudioStream_Demo.pwn`](FS_SampAudioStream_Demo.pwn) for a complete plug-and-play English showcase filterscript (`/audiomenu` / `/sa`) demonstrating all 83 `SA_*` natives, hooked vanilla functions, player mute/source tracking, global pause, stream listing/stats, playlist import, seeking, queue item moving, listener skip voting, previous-track history, live radio text inspection/overrides, boombox/vehicle attachment, volume/position/distance controls, local music library browsing, channel playlist mode, sound effects, on-screen stream text toggle, persistent or temporary channels, and synchronized radio channels.

---

## Complete API Reference

All 73 `SA_*` natives, hooked vanilla functions, 19 callbacks, constants, and enums are documented with signatures and examples in **[`docs/api-reference.md`](docs/api-reference.md)**:

* **[Playback & Control](docs/api-reference.md#1-playback--control-natives):** `SA_PlayAudioStreamForPlayer`, `SA_PlayAudioStream`, `SA_StopAudioStreamForPlayer`, `SA_StopAudioStream`, `SA_PauseAudioStreamForPlayer`, `SA_PauseAudioStream`, `SA_SeekAudioStream`, `SA_SetPlayerAudioStreamMuted`, `SA_IsPlayerAudioStreamMuted`, `SA_GetPlayerAudioStreamSource`, `SA_SetPlayerAudioStreamVolume`, `SA_SetAudioStreamVolume`, `SA_SetPlayerAudioStreamDistance`, `SA_SetAudioStreamDistance`, `SA_SetPlayerAudioStreamPos`, `SA_SetAudioStreamPos`
* **[Spatial & Entity Attachment](docs/api-reference.md#2-spatial--attachment-natives):** `SA_AttachAudioStreamToPlayer`, `SA_AttachAudioStreamToVehicle`
* **[Asynchronous Music Search](docs/api-reference.md#3-asynchronous-music-search-natives):** `SA_SearchAudioStream`, `SA_CancelAudioStreamSearch`
* **[Synchronized Radio Channels](docs/api-reference.md#4-synchronized-radio-channel-natives):** `SA_CreateAudioStreamChannel`, `SA_DestroyAudioStreamChannel`, `SA_SetAudioStreamChannelName`, `SA_GetAudioStreamChannelLiveTitle`, `SA_SetAudioStreamChannelLiveTitle`, `SA_PlayChannelAudioStream`, `SA_QueueChannelAudioStream`, `SA_QueueChannelAudioStreamPlaylist`, `SA_SeekChannelAudioStream`, `SA_RemoveAudioStreamChannelQueue`, `SA_InsertAudioStreamChannelQueue`, `SA_MoveAudioStreamChannelQueue`, `SA_ShuffleAudioStreamChannelQueue`, `SA_SkipChannelAudioStream`, `SA_StopChannelAudioStream`, `SA_PauseChannelAudioStream`, `SA_SetChannelAudioStreamVolume`, `SA_SetAudioStreamChannelLoop`, `SA_GetAudioStreamChannelLoop`, `SA_SetAudioStreamChannelPos`, `SA_ClearAudioStreamChannelPos`, `SA_AttachAudioStreamChannelToPlayer`, `SA_AttachAudioStreamChannelToVehicle`, `SA_JoinAudioStreamChannel`, `SA_LeaveAudioStreamChannel`, `SA_IsPlayerInAudioStreamChannel`, `SA_GetPlayerAudioStreamChannel`, `SA_SetAudioStreamChannelPass`, `SA_CheckAudioStreamChannelPass`, `SA_SetAudioStreamChannelCreator`, `SA_SetAudioStreamChannelDestroy`, `SA_GetAudioStreamChannelDestroy`, `SA_GetAudioStreamChannelCount`, `SA_GetAudioStreamChannelID`, `SA_GetAudioStreamChannelInfo`, `SA_GetAudioStreamChannelTrack`, `SA_GetAudioStreamChannelElapsed`, `SA_GetAudioStreamChannelListener`, `SA_GetAudioStreamChannelQueue`, `SA_GetAudioStreamChannelQueueLabel`, `SA_FindAudioStreamChannelQueue`, `SA_ClearAudioStreamChannelQueue`
* **[Diagnostics & Metadata](docs/api-reference.md#5-diagnostics--metadata-natives):** `SA_GetPlayerAudioStreamID`, `SA_GetAudioStreamCount`, `SA_GetAudioStreamID`, `SA_GetAudioStreamOwner`, `SA_GetAudioStreamElapsed`, `SA_GetAudioStreamURL`, `SA_GetAudioStreamStats`, `SA_GetAudioStreamInfo`, `SA_GetAudioStreamLiveTitle`, `SA_SetAudioStreamLiveTitle`, `SA_IsAudioStreamExtractorReady`, `SA_GetAudioStreamHttpPort`
* **[Hooked Vanilla Natives](docs/api-reference.md#6-hooked-vanilla-sa-mp-natives):** `PlayAudioStreamForPlayer`, `StopAudioStreamForPlayer`
* **[Callbacks](docs/api-reference.md#7-callbacks):** `OnAudioStreamProcessing`, `OnAudioStreamWaiting`, `OnAudioStreamReady`, `OnAudioStreamStart`, `OnAudioStreamFinished`, `OnAudioStreamError`, `OnAudioStreamLiveTitleChange`, `OnPlayerAudioStreamSourceChange`, `OnPlayerAudioStreamNoConnect`, `OnAudioStreamSearchResults`, `OnAudioStreamSearchResultItem`, `OnAudioStreamChannelDestroyed`, `OnAudioStreamChannelTrackWaiting`, `OnAudioStreamChannelTrackStart`, `OnAudioStreamChannelQueueEnd`, `OnAudioStreamChannelError`, `OnAudioStreamChannelRestored`, `OnAudioStreamChannelPlaylistQueued`, `OnAudioStreamChannelLiveTitleChange`

---

## Building from Source

### Prerequisites
* **CMake 3.20+** and **Git** (dependencies `nlohmann_json`, `asio`, `ogg`, `opus`, `fdk-aac`, `mp3lame_static`, `dr_libs`, and `stb` are fetched automatically via CMake `FetchContent`)
* **Windows:** Visual Studio 2022 (Desktop C++ workload)
* **Linux:** GCC 10+ / Clang with `gcc-multilib g++-multilib` (for `-m32` x86 builds)
* **SDKs in `deps/`:**
  ```bash
  git clone --depth 1 --recurse-submodules --shallow-submodules https://github.com/openmultiplayer/open.mp-sdk.git deps/open.mp-sdk
  git clone --depth 1 https://github.com/maddinat0r/samp-plugin-sdk.git deps/samp-plugin-sdk
  ```

### Compiling on Windows (1-Click Script)
Run the automated MSVC build script:
```cmd
build_msvc.bat
```
*(Automatically configures CMake for `Win32`, compiles Release binaries, runs `core_tests`, and builds `dist/SampAudioStream-Windows.zip`.)*

### Compiling on Linux
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=cmake/linux-x86.cmake
cmake --build build --parallel
ctest --test-dir build -R core_tests --output-on-failure
```

---

## Version history

<details>
<summary>Show the version history</summary>

### 2.3.0 — 21/11/2026
- **Sound Effects Folder (`audio.sound_dir`) & `SA_PlayAudioStreamSoundForPlayer`**:
  - New `audio.sound_dir` folder (`"scriptfiles/SampAudioStream/sound"`, created on startup) for short sound effects such as alerts, notifications, jingles and gameplay sounds, kept apart from the music library (a startup warning is logged when it overlaps `audio.music_dir`).
  - New `SA_PlayAudioStreamSoundForPlayer(playerid, sound[], posX, posY, posZ, distance, usepos)` serves `.mp3`, `.ogg` and `.wav` files up to 5 MB as-is from memory through the new `/f/<code>` route (no transcoder, immediate start), plays them above every other source (`SA_SOURCE_SOUND`) and then resumes what the player was hearing: channels and radios continue live and interrupted transcoded personal streams resume where they were cut.
- **Playlist Continues After Out-of-List Tracks**:
  - In channel playlist mode, a track started with `SA_PlayChannelAudioStream` (such as a live radio played over the list) now continues into the playlist when it ends, is skipped or fails, starting after the item that was playing before it (or from the first item); previously the channel stopped. `LOOP_TRACK` repeats the out-of-list track and `LOOP_QUEUE` wraps past the last item.
- **Large Channel Queues**:
  - New `SA_FindAudioStreamChannelQueue(channelid, item[], start_index)` finds a URL in a channel queue inside the plugin and returns its index (or `-1`), so scripts can check or highlight queued tracks with one call instead of reading every entry.
  - Playlist imports allow more time for big lists (`60s` up to 200 items plus `10s` per further 100 items, `140s` at the 1000-item maximum).
- **Demo Filterscript Updated to 2.2.0/2.3.0 Features (`FS_SampAudioStream_Demo.pwn`)**:
  - Added a Music Library menu (refresh, folders, tracks, private/global/channel playback and whole-folder import via `library://`), channel playlist mode (toggle, highlighted current item, play any item), a sound effect player, an on-screen stream text toggle (`stream_text`), a persistent/temporary step in channel creation and the `SA_SOURCE_SOUND` label.
  - Fixed the dialog range check that ignored the live radio text, global live text and queue item dialogs.
  - The channel queue, library folder and library track lists are paged (only the visible page is read), queued tracks are marked through `SA_FindAudioStreamChannelQueue`, whole-folder imports request up to 1000 items.
  - Compiles without warnings: the large dialog buffers are `static` (estimated stack use drops from about 26 KB to 10 KB, inside the default 16 KB) and `<open.mp>` is included directly when available, falling back to `<a_samp>` on legacy SA-MP toolchains.

### 2.2.0 — 20/11/2026
- **Cleaner On-Screen Text & Preserved Apostrophes**:
  - Single-player and global sessions no longer emit an HTTP `icy-name:` header (preventing duplicated station/title text in the client); synchronized channels continue sending `icy-name:` as the channel name.
  - Inline `StreamTitle` preserves apostrophes (`'` is no longer stripped; only `\r`, `\n`, `\0`, and `';` sequences are sanitized) and formats live radio stream updates as `"<song> - <station>"` when a station name is available and not already included.
- **Short Endpoint URLs (`/s/` & `/c/`)**:
  - Replaced legacy 32-hex `/stream/<token>` and `/channel/ch_<token>` routes with compact 10-character alphanumeric CSPRNG codes under `/s/<code>` (with optional `/<version>` when options update and `?resume=<offset>`) and `/c/<code>`.
- **Channel Playlist Mode**:
  - Added non-destructive channel playlist mode (`SA_SetAudioStreamChannelPlaylist`, `SA_IsAudioStreamChannelPlaylist`, `SA_GetAudioStreamChannelQueuePos`, `SA_PlayAudioStreamChannelQueue`) with persisted mode/position, loop-aware wraparound, pre-resolution of the next track, and support for out-of-list `SA_PlayChannelAudioStream(url)` overrides without mutating the list.
- **Local Music Library (`audio.music_dir` & `library://`)**:
  - Added on-demand scanning of 1-level subfolders and loose root files in `audio.music_dir` (`"scriptfiles/SampAudioStream/music"`, configurable), exposed via `SA_RefreshAudioLibrary`, `SA_GetAudioLibraryFolderCount`, `SA_GetAudioLibraryFolder`, and `SA_GetAudioLibraryTrack`, and playable/importable across all stream and channel natives using `library://<folder>/<file>`, `library:///<file>`, `library://<folder>`, or `library://` with strict path containment and transcoded finite playback.
- **Direct File Titles & Optional On-Screen Stream Text (`bool:stream_text = true`)**:
  - Direct audio files now report their file name (from the URL path) as the track title instead of `"Direct Audio Stream"`; live radios with a file-like link still switch to their `icy-name` when available.
  - `SA_PlayAudioStreamForPlayer`, `SA_PlayAudioStream` and hooked `PlayAudioStreamForPlayer` gained a trailing `bool:stream_text = true` parameter; `false` hides the on-screen stream text for server-processed playback (useful for short sound effects).
- **Live Radio Station Names (`icy-name`)**:
  - Direct live radios now report the station's `icy-name` as the track title (instead of `"Direct Audio Stream"` / `"Radio Stream"`) once it is known from the link probe or the first ICY poll.
- **Per-Channel Persistence**:
  - `SA_CreateAudioStreamChannel` gained a trailing `bool:persistent = true` parameter. Persistent channels are saved and restored as before; non-persistent channels live only in memory, are never written to disk, and are destroyed on GMX. The global `channels.persist_channels` option was removed (old config files keep loading; the key is ignored).
- **Single-Track Playlist Link Handling**:
  - Playlist-only YouTube links passed to single-track playback (`SA_PlayAudioStreamForPlayer`, `SA_PlayAudioStream`, hooked `PlayAudioStreamForPlayer`, channel tracks) now resolve only their first entry instead of extracting the whole playlist.
- **Automatic HTTP Connection Limits (`network.max_http_connections` removed)**:
  - The total number of simultaneous HTTP audio connections is now derived from the server's player slots (`player slots × 2 + 64`, minimum `128`, up to `2064`), read from `maxplayers` (SA-MP `server.cfg`) or `max_players` (open.mp `config.json`). Existing config files that still contain `network.max_http_connections` keep loading without warnings; the key is simply ignored.
  - On Linux, the plugin now raises the process open file descriptor soft limit (`RLIMIT_NOFILE`) by itself, only up to what the connection limit needs, and warns only when the system hard limit is too low.
  - The startup listening log now shows the computed limit and player slots, and the total connection limit warning no longer points to a config key.

### 2.1.0 — 19/11/2026
- **Live Internet Radio Text & Script Overrides (`73 SA_*` Natives, `19` Callbacks)**:
  - Added periodic ICY `StreamTitle` polling (`audio.live_title_poll_seconds`, default `30s`, `0` to disable or `10..300s`, capped at 2 concurrent `64 KB` probes) for direct Shoutcast/Icecast radio streams (including extension mounts like `.mp3`), while permanently skipping finite files (`Content-Length`), HLS playlists, and `yt-dlp` extractors.
  - Added `OnAudioStreamLiveTitleChange(playerid, streamid, const title[])` and `OnAudioStreamChannelLiveTitleChange(channelid, const title[])`, plus `SA_GetAudioStreamLiveTitle`, `SA_SetAudioStreamLiveTitle`, `SA_GetAudioStreamChannelLiveTitle`, and `SA_SetAudioStreamChannelLiveTitle` to read, filter, or customize outgoing HUD radio text separately from channel names and track metadata without re-firing callbacks.
- **Queue Display Labels, Item Moving & One-Time Shuffle**:
  - Added optional `label[]` support to `SA_QueueChannelAudioStream` and `SA_InsertAudioStreamChannelQueue` plus automatic entry-title labeling for `SA_QueueChannelAudioStreamPlaylist`, persisted across restarts/loops and readable with `SA_GetAudioStreamChannelQueueLabel`.
  - Added `SA_ShuffleAudioStreamChannelQueue(channelid)` and `SA_MoveAudioStreamChannelQueue(channelid, from_index, to_index)` (preserving raw UTF-8/Pawn labels losslessly and pre-resolving queue index `0` when changed).
- **Audio Normalization, Client No-Connect Feedback & Playlist Error Clarity**:
  - Added optional sliding-RMS AGC loudness normalization (`audio.normalize_volume`, default `false`) with peak limiting for server-transcoded/proxied streams and channels.
  - Added `OnPlayerAudioStreamNoConnect` handling/notifications and improved `OnAudioStreamChannelPlaylistQueued` error reporting when queues or concurrent transcoder limits block playback.
- **Interactive Demo Filterscript Enhancements (`FS_SampAudioStream_Demo.pwn`)**:
  - Added live radio text announcements (`Now playing...`), live text inspection/editing dialogs, queue item action submenu (`Move to Top`, `Move Up`, `Move Down`, `Remove`), listener majority skip voting (`Vote to Skip Track`), and per-channel in-memory previous track history (`Previous Track`).

### 2.0.0 — 18/11/2026
- **Breaking API & Storage Standardization (`AudioStream` Naming + `scriptfiles/SampAudioStream/`)**:
  - Standardized all Pawn natives and callbacks to consistent `AudioStream` / `ChannelAudioStream` naming (`SA_SetAudioStreamVolume`, `SA_SearchAudioStream`, `SA_CancelAudioStreamSearch`, `SA_CreateAudioStreamChannel`, `SA_DestroyAudioStreamChannel`, `SA_PlayChannelAudioStream`, `SA_QueueChannelAudioStream`, `SA_SkipChannelAudioStream`, `SA_StopChannelAudioStream`, `SA_PauseChannelAudioStream`, `SA_SetChannelAudioStreamVolume`, `SA_JoinAudioStreamChannel`, `SA_LeaveAudioStreamChannel`, `SA_IsPlayerInAudioStreamChannel`, `SA_GetPlayerAudioStreamChannel`, `SA_GetPlayerAudioStreamID`, `SA_GetAudioStreamInfo`, `OnAudioStreamSearchResults`, and `OnAudioStreamSearchResultItem`).
  - Uses `scriptfiles/SampAudioStream/` as the default runtime directory for binaries, extractor caches, configuration, and persisted channels.
  - Added background Deno `v2.9.8` provisioning (`extractor.js_runtime`: `"auto"` / `"deno"` / `"quickjs"`) making Deno the preferred JS challenge runtime with `quickjs-ng` as automatic fallback, plus single automatic extractor re-resolution and debug diagnostics on HTTP 403/410 media errors.
- **Complete Synchronized Channel Management**:
  - Added optional creator tracking (`creator_playerid`, storing creator name at creation time + `SA_SetAudioStreamChannelCreator`), channel passwords (`SA_SetAudioStreamChannelPass`, `SA_CheckAudioStreamChannelPass`, password check in `SA_JoinAudioStreamChannel`), channel destruction policies (`SA_CHANNEL_DESTROY_MANUAL`, `SA_CHANNEL_DESTROY_ON_DISCONNECT`, `SA_CHANNEL_DESTROY_ON_TIMEOUT` whose countdown resets/stays paused whenever the creator is online; `SA_SetAudioStreamChannelDestroy`, `SA_GetAudioStreamChannelDestroy`, `OnAudioStreamChannelDestroyed`), and full introspection/queue natives (`SA_GetAudioStreamChannelCount`, `SA_GetAudioStreamChannelID`, `SA_GetAudioStreamChannelInfo`, `SA_GetAudioStreamChannelTrack`, `SA_GetAudioStreamChannelQueue`, `SA_ClearAudioStreamChannelQueue`).
  - Added channel track lifecycle callbacks (`OnAudioStreamChannelTrackStart`, `OnAudioStreamChannelQueueEnd`, `OnAudioStreamChannelError`) and persistence restore callback (`OnAudioStreamChannelRestored`).
  - Added configurable channel and queue capacity limits (`channels.max_channels`, `channels.max_queue_per_channel`).
  - Added spatial 3D channel positioning and entity attachment (`SA_SetAudioStreamChannelPos`, `SA_ClearAudioStreamChannelPos`, `SA_AttachAudioStreamChannelToPlayer`, `SA_AttachAudioStreamChannelToVehicle`), with attached sources playing without distance attenuation.
  - Added channel queue editing (`SA_InsertAudioStreamChannelQueue`, `SA_RemoveAudioStreamChannelQueue`) and loop modes (`SA_SetAudioStreamChannelLoop`, `SA_GetAudioStreamChannelLoop` supporting `OFF`/`TRACK`/`QUEUE`).
  - Added channel elapsed playback query (`SA_GetAudioStreamChannelElapsed`), indexed listener lookup (`SA_GetAudioStreamChannelListener`), and channel renaming (`SA_SetAudioStreamChannelName`).
  - Added atomic JSON channel persistence across restarts and GMX (`channels.persist_channels`, `channels.channel_restore_grace_seconds`).
- **Dynamic Range, Position, Search & Encoding Control**:
  - Added `SA_SetPlayerAudioStreamDistance`, `SA_SetAudioStreamDistance`, `SA_SetPlayerAudioStreamPos`, and `SA_SetAudioStreamPos` with immediate spatial listener evaluation and MP3-frame-aligned playback resumption (`?resume=`) for transcoded streams (avoiding mid-frame glitches), plus clean parameter re-dispatch for passthrough streams.
  - Enhanced `SA_AttachAudioStreamToPlayer` and `SA_AttachAudioStreamToVehicle` to immediately fetch the target entity's current position (plus `worldid`/`interiorid` for players), enforce `usepos = true` automatically, and refresh active playback/listeners even when started with `usepos = false`.
  - Added bidirectional `cp1252` ↔ UTF-8 text conversion for channel names, URLs/queries, queue items, and track metadata, plus error reporting (`error_code`, `error_message[]`, and `SA_ERR_SEARCH_FAILED`) in `OnAudioStreamSearchResults`.
- **Processing Callback & Per-Call Direct-File Transcoding**:
  - Added `OnAudioStreamProcessing(playerid, streamid, const url[], bool:from_cache)` alongside the `bool:notify_processing = true` parameter on `SA_PlayAudioStreamForPlayer`, `SA_PlayAudioStream`, and the `PlayAudioStreamForPlayer` hook.
  - Added `bool:process_direct_audio_files = false` to `SA_PlayAudioStreamForPlayer`, `SA_PlayAudioStream`, and `PlayAudioStreamForPlayer` so direct music file links (`.mp3`, `.ogg`, `.wav`, `.flac`, `.aac`, `.m4a`, `.opus`) can be selectively transcoded for full pause, volume, distance, and `OnAudioStreamFinished` support while keeping live radio streams and `.pls`/`.m3u` playlists in zero-slot passthrough.
- **Automatic Direct HTTP/HTTPS Passthrough, Cache Pruning & Lifecycle Reliability**:
  - Enabled automatic passthrough detection for `https://` native direct-audio URLs and probed `audio/*` radios (preserving `SourceType::DirectAudio` across `MetadataCache` hits) while always transcoding platform URLs, `.m3u8` (HLS) playlists, and `.m4a`/`.opus` containers.
  - Added automatic age-based `yt-dlp-cache/youtube-sigfuncs` pruning (`extractor.yt_dlp_cache_max_age_days`, default `7` days, `0` = off, plus internal `64 MB` cap protecting `< 1h` files) at startup and asynchronously during idle maintenance ticks, keeping `challenge-solver` scripts and other subdirs untouched.
  - Added automatic creator reconnection & rename tracking across `OnPlayerConnect`, `SetPlayerName` (AMX hook on SA-MP; `PlayerChangeEventHandler` on open.mp), and disconnect snapshot so `SA_CHANNEL_DESTROY_ON_TIMEOUT` resets/pauses reliably when returning; dispatched `OnAudioStreamChannelDestroyed` asynchronously on the next tick; enforced creator requirement for automatic channel destroy policies; grouped startup extractor/public-IP readiness logs with the banner (<= `1.5s`); and packaged both `.pwn` and compiled `-d0` `.amx` demo filterscripts under `filterscripts/`.
  - Fixed Pawn `INVALID_PLAYER_ID` callback mapping (`-1` -> `65535` across all callbacks including global streams and channel errors) and fixed `HttpStreamingServer::Stop()` partial-bind cleanup hangs when an explicit port is already in use.
  - Added `available_at` respect (non-blocking `Tick()` deferral + watchdog exclusion) alongside `OnAudioStreamWaiting(playerid, streamid, wait_ms)` and `OnAudioStreamChannelTrackWaiting(channelid, wait_ms)` callbacks to notify scripts when platform playback waits for media availability.
  - Renamed the plugin/component binaries and Pawn include to `SampAudioStream.dll` / `SampAudioStream.so` and `SampAudioStream.inc`.
  - Added automatic YouTube anti-bot cookie fallback (`scriptfiles/SampAudioStream/cookies.txt` + browser discovery controlled by `extractor.cookies_from_browser`, Firefox-first then Chromium family) and `SA_ERR_AUTH_REQUIRED` (`10`), with step-by-step instructions in [`docs/youtube-cookies.md`](docs/youtube-cookies.md).
- **Extended Stream Controls, Channel Persistence, Seeking & Diagnostics (`66 SA_*` Natives, 16 Callbacks)**:
  - Standardized diagnostics and queue-item naming (`SA_RemoveAudioStreamChannelQueue`, `SA_InsertAudioStreamChannelQueue`, `SA_IsAudioStreamExtractorReady`, `SA_GetAudioStreamHttpPort`).
  - Added per-player muting (`SA_SetPlayerAudioStreamMuted`, `SA_IsPlayerAudioStreamMuted`), active source inspection and change notifications (`SA_GetPlayerAudioStreamSource`, `OnPlayerAudioStreamSourceChange`), global/personal stream pause (`SA_PauseAudioStream`), active stream listing/metrics (`SA_GetAudioStreamCount`, `SA_GetAudioStreamID`, `SA_GetAudioStreamOwner`, `SA_GetAudioStreamElapsed`, `SA_GetAudioStreamURL`, `SA_GetAudioStreamStats`), channel playlist importing (`SA_QueueChannelAudioStreamPlaylist`, `OnAudioStreamChannelPlaylistQueued`), and transcoded track seeking (`SA_SeekAudioStream`, `SA_SeekChannelAudioStream`).

### 1.1.0 — 17/11/2026
- **Live HLS & MPEG-TS Overhaul**:
  - Sequence-based HLS live tracking (`#EXT-X-MEDIA-SEQUENCE`) starting at the live edge (last 3 segments) to play at live time without repeating DVR segments (> 64 entries) or rotating-token URLs, while keeping full playback from index 0 for VOD (`#EXT-X-ENDLIST`).
  - Tolerates isolated failing live segments (1 immediate retry, then skip with throttled `Warn` logs) while failing cleanly on sustained outages (3 consecutive failures or 15s without success), and finishes cleanly as normal EOF after 30s on stalled error-free live playlists.
  - Ignores transient stale CDN edge playlists without repeating audio, handles `#EXT-X-DISCONTINUITY` / `#EXT-X-DISCONTINUITY-SEQUENCE` sequentially without skipping or replaying segments, and resets MPEG-TS / FDK-AAC decoder state cleanly on mid-stream PAT/PMT/PID/codec changes.
- **Security & Container Hardening**:
  - Added SSRF protection blocking loopback, private LAN, link-local/cloud metadata, CGNAT, multicast, IPv6 local targets, and self-loopback requests to the plugin's own HTTP server across requests and HTTP redirects (plus Linux `CURLOPT_OPENSOCKETFUNCTION` peer validation), configurable via `network.allow_private_network_urls`.
  - Hardened WebM/EBML, MP4/M4A, FLAC, Ogg, WAV, and MPEG-TS parsers against malformed headers, non-faststart MP4 layouts, integer overflows, and unbounded allocations; stopped misclassifying early decoder/container failures as network interruptions.
- **Streaming, Concurrency & Playback Reliability**:
  - Added HTTP `Range`-resume recovery on interrupted finite downloads, bounded HTTP slow-writer disconnects, ring-buffer track generation isolation, and deterministic EOF drain cleanup without stale playback callbacks.
  - Preserved pause states across pre-ready extraction and synchronized radio channel transitions, and enforced configurable arbitration (`audio.player_priority_order`).
- **Configuration, Pawn Bridge & Runtime Stability**:
  - Added `audio.transcoder_burst_ms`, `performance.max_concurrent_transcoders`, `network.allow_private_network_urls`, `audio.player_priority_order`, and `scripting.pawn_text_encoding` (`utf8` / `cp1252`), with strict validation/clamping and `server.cfg` port/bind detection on SA-MP.
  - Added single-instance runtime guard (`RuntimeInstanceGuard`), `SA_Internal_PlayerDisconnect` ALS hook, full native parameter forwarding across AMX/open.mp, and require-before-include compiler guard in `sampaudiostream.inc`.
- **Build, Linux x86 Compatibility & Release Pipeline**:
  - Updated Linux 32-bit CI builds to `debian:bullseye` (`cmake/linux-x86.cmake`) verifying `glibc <= 2.31`, static `libstdc++`/`libgcc`, strict 7-symbol exports (`exports.map`), static MSVC `/MT` verification on Windows, and streamlined release archives.

### 1.0.0 — 14/11/2026
- Initial public release of **SampAudioStream**:
  - **Unified Dual-Runtime Binary**: Exports both open.mp Native Component (`ComponentEntryPoint`) and legacy SA-MP Plugin (`Supports`, `Load`, `Unload`, `AmxLoad`, `AmxUnload`, `ProcessTick`) across Windows and Linux (`32-bit x86`).
  - **Zero Manual Host Dependencies**:
    - Statically linked MSVC runtime (`/MT`) on Windows and static `libgcc`/`libstdc++` with strict symbol version script (`exports.map`) on Linux.
    - Embedded static C++ audio codecs (`libopus`, `libogg`, `fdk-aac`, `mp3lame_static`, `dr_libs`, `stb_vorbis`) eliminating any external `ffmpeg` requirement.
    - Self-managed asynchronous provisioning of `yt-dlp` and `quickjs-ng` (`qjs`) into `scriptfiles/SampAudioStream/bin/`.
  - **Broad Media & Live Stream Support**:
    - Plays YouTube videos & lives, SoundCloud, Twitch, Kick, Vimeo, TikTok, HLS (`.m3u8` master & live playlists with built-in MPEG-TS demuxer), WebM/Matroska, MP4/M4A, Ogg, WAV, FLAC, ADTS, MP3, `.pls`/`.m3u`, and Shoutcast/Icecast HTTP radios.
  - **Transparent Native Hooking + Rich `SA_*` API**:
    - Hooks `PlayAudioStreamForPlayer` and `StopAudioStreamForPlayer` (both ALS in `sampaudiostream.inc` and AMX native table patching in C++ across SA-MP and open.mp).
    - Adds `worldid`, `interiorid`, and `volume` (`0.0`–`200.0`) controls.
    - Global `SA_PlayAudioStream` with `1x` real-time pacing on finite tracks so players entering the radius hear the current timestamp, while live broadcasts stream directly in real time.
    - Dynamic 3D tracking via `SA_AttachAudioStreamToPlayer` and `SA_AttachAudioStreamToVehicle`.
  - **Asynchronous Search & Synced Radio Channels**:
    - Non-blocking `SA_SearchAudioStream` returning metadata items via `OnAudioStreamSearchResultItem` and `OnAudioStreamSearchResults`.
    - Shared multi-player channels (`SA_CreateAudioStreamChannel`, `SA_PlayChannelAudioStream`, `SA_QueueChannelAudioStream`, `SA_SkipChannelAudioStream`, `SA_JoinAudioStreamChannel`) with real-time synchronized HTTP endpoints.
  - **Bounded Memory & Caching**:
    - In-memory LRU+TTL cache for extracted track metadata (`3600s` default) and search results (`900s` default), plus ~30s circular ring buffers (~480 KB at 128 kbps, min 256 KB) freed immediately on completion or stop.

</details>

---

## Author & Credits

* [@Walter-Correa](https://github.com/Walter-Correa) - Concept and development of **SampAudioStream**, with the assistance of **Artificial Intelligence**.

---

## License

This project is licensed under the **Mozilla Public License 2.0 (MPL-2.0)**. See the [LICENSE](LICENSE) file for complete legal terms.
