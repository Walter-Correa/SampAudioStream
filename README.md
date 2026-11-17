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
| **Synchronized Radio Channels** | ❌ None | ✅ `SA_CreateChannel`, `SA_QueueChannel`, `SA_SkipChannel`, and shared live HTTP endpoints |
| **Caching & Memory Footprint** | Uncached client HTTP requests | ✅ In-memory LRU+TTL metadata/search cache + ~30s sliding ring buffers (zero MP3 disk bloat) |

---

## Documentation Hub

Modular guides and exhaustive API documentation are available in the [`docs/`](docs/) directory:

| Guide | Description |
| :--- | :--- |
| **[📖 Getting Started](docs/getting-started.md)** | Requirements, installation for SA-MP & open.mp, firewall setup, first startup log, `config.json` reference, troubleshooting, and FAQ. |
| **[🔊 Playback, Hooking & Spatial Audio](docs/playback-and-spatial.md)** | `SA_PlayAudioStreamForPlayer` vs. `SA_PlayAudioStream`, radius time sync, live streams, volume, pause, and 3D entity attachment. |
| **[🔎 Music Search & Synced Channels](docs/search-and-channels.md)** | Asynchronous `SA_SearchAudioStream` dialog workflows and synchronized multi-player radio stations with queues. |
| **[⚙️ Architecture, Transcoding & Caching](docs/architecture-and-caching.md)** | Internal C++20 worker pool, embedded codecs, HLS/MPEG-TS live decoding, LRU RAM cache, and ~30s ring buffers. |
| **[📚 Complete API Reference](docs/api-reference.md)** | Exhaustive reference for all 27 `SA_*` natives, hooked vanilla natives, 6 callbacks, enums, constants, and helpers. |

---

## Quick Start & Installation

Follow these 5 steps to install SampAudioStream and play your first track:

1. **Download the release archive for your server OS:** download `SampAudioStream-Windows.zip` or `SampAudioStream-Linux.zip` from [GitHub Releases](https://github.com/Walter-Correa/SampAudioStream/releases) (matching the **server** host OS). Verify the archive contains `plugins/`, `components/`, and `include/sampaudiostream.inc`. ([Full details](docs/getting-started.md#2-installation))
2. **Check host dependencies:** **Windows** requires nothing extra; **Linux** requires 32-bit `libcurl` (`libcurl.so.4`). If `libcurl` is missing on Linux, the server log prints the exact install command for your distribution. ([Distribution commands](docs/getting-started.md#1-requirements))
3. **Copy the binary folder and include file:** copy **only one** binary folder (`plugins/` **or** `components/` — never both) plus `include/sampaudiostream.inc` into `pawno/include/` (SA-MP) or `qawno/include/` (open.mp). ([Installation guide](docs/getting-started.md#2-installation))

   | Server Runtime | Binary Folder | Config Edit Required? |
   | :--- | :--- | :--- |
   | SA-MP 0.3.7 | `plugins/` | Yes, add `plugins sampaudiostream.dll` (or `.so`) in `server.cfg` |
   | open.mp (recommended) | `components/` | No (loaded automatically) |
   | open.mp legacy mode | `plugins/` | Yes, add `"sampaudiostream"` under `pawn.legacy_plugins` in `config.json` |

   > [!IMPORTANT]
   > Copy **only one** binary folder (`plugins/` **or** `components/`). Loading both in the same server process is rejected.

4. **Open the streaming TCP port:** allow inbound TCP traffic on `server_port + 1` (default `7778` when your server runs on `7777`, or your custom `http_port`) in your OS firewall and VPS security group (note: LAN players automatically receive local/loopback URLs when `lan_aware_urls` is `true`, while remote players require port-forwarding). Start the server and confirm `HTTP audio streaming server listening on ...` appears in the console log. ([Firewall & verification steps](docs/getting-started.md#3-open-the-audio-port-firewall))
5. **Include `<sampaudiostream>` in your Pawn script and compile:** place `#include <sampaudiostream>` **after** `<open.mp>` or `<a_samp>`, then recompile and start playing audio. Confirm `yt-dlp ... downloaded and SHA-256 verified` and `QuickJS-ng downloaded and SHA-256 verified` appear on first boot. ([First startup log](docs/getting-started.md#4-first-startup-what-the-log-should-show))

```pawn
#include <open.mp> // Or <a_samp>
#include <sampaudiostream>

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

See [`Example.pwn`](Example.pwn) for a full showcase filterscript including asynchronous dialog search (`/search`), boombox/vehicle attachment, volume/pause controls, and synchronized radio channels.

---

## Complete API Reference

All 27 `SA_*` natives, hooked vanilla functions, 6 callbacks, constants, and enums are documented with signatures and examples in **[`docs/api-reference.md`](docs/api-reference.md)**:

* **[Playback & Control](docs/api-reference.md#1-playback--control-natives):** `SA_PlayAudioStreamForPlayer`, `SA_PlayAudioStream`, `SA_StopAudioStreamForPlayer`, `SA_StopAudioStream`, `SA_PauseAudioStreamForPlayer`, `SA_SetPlayerAudioStreamVolume`, `SA_SetAudioStreamVolume`
* **[Spatial & Entity Attachment](docs/api-reference.md#2-spatial--attachment-natives):** `SA_AttachAudioStreamToPlayer`, `SA_AttachAudioStreamToVehicle`
* **[Asynchronous Music Search](docs/api-reference.md#3-asynchronous-music-search-natives):** `SA_SearchAudioStream`, `SA_CancelAudioStreamSearch`
* **[Synchronized Radio Channels](docs/api-reference.md#4-synchronized-radio-channel-natives):** `SA_CreateChannel`, `SA_DestroyChannel`, `SA_PlayChannel`, `SA_QueueChannel`, `SA_SkipChannel`, `SA_StopChannel`, `SA_PauseChannel`, `SA_SetChannelVolume`, `SA_JoinChannel`, `SA_LeaveChannel`, `SA_IsPlayerInChannel`, `SA_GetPlayerChannel`
* **[Diagnostics & Metadata](docs/api-reference.md#5-diagnostics--metadata-natives):** `SA_GetPlayerStreamID`, `SA_GetStreamInfo`, `SA_IsExtractorReady`, `SA_GetHttpPort`
* **[Hooked Vanilla Natives](docs/api-reference.md#6-hooked-vanilla-sa-mp-natives):** `PlayAudioStreamForPlayer`, `StopAudioStreamForPlayer`
* **[Callbacks](docs/api-reference.md#7-callbacks):** `OnAudioStreamReady`, `OnAudioStreamStart`, `OnAudioStreamFinished`, `OnAudioStreamError`, `OnAudioStreamSearchResults`, `OnAudioStreamSearchResultItem`

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
    - Self-managed asynchronous provisioning of `yt-dlp` and `quickjs-ng` (`qjs`) into `scriptfiles/samp-audio-stream/bin/`.
  - **Broad Media & Live Stream Support**:
    - Plays YouTube videos & lives, SoundCloud, Twitch, Kick, Vimeo, TikTok, HLS (`.m3u8` master & live playlists with built-in MPEG-TS demuxer), WebM/Matroska, MP4/M4A, Ogg, WAV, FLAC, ADTS, MP3, `.pls`/`.m3u`, and Shoutcast/Icecast HTTP radios.
  - **Transparent Native Hooking + Rich `SA_*` API**:
    - Hooks `PlayAudioStreamForPlayer` and `StopAudioStreamForPlayer` (both ALS in `sampaudiostream.inc` and AMX native table patching in C++ across SA-MP and open.mp).
    - Adds `worldid`, `interiorid`, and `volume` (`0.0`–`200.0`) controls.
    - Global `SA_PlayAudioStream` with `1x` real-time pacing on finite tracks so players entering the radius hear the current timestamp, while live broadcasts stream directly in real time.
    - Dynamic 3D tracking via `SA_AttachAudioStreamToPlayer` and `SA_AttachAudioStreamToVehicle`.
  - **Asynchronous Search & Synced Radio Channels**:
    - Non-blocking `SA_SearchAudioStream` returning metadata items via `OnAudioStreamSearchResultItem` and `OnAudioStreamSearchResults`.
    - Shared multi-player channels (`SA_CreateChannel`, `SA_PlayChannel`, `SA_QueueChannel`, `SA_SkipChannel`, `SA_JoinChannel`) with real-time synchronized HTTP endpoints.
  - **Bounded Memory & Caching**:
    - In-memory LRU+TTL cache for extracted track metadata (`3600s` default) and search results (`900s` default), plus ~30s circular ring buffers (~480 KB at 128 kbps, min 256 KB) freed immediately on completion or stop.

</details>

---

## Author & Credits

* [@Walter-Correa](https://github.com/Walter-Correa) - Concept and development of **SampAudioStream**, with the assistance of **Artificial Intelligence**.

---

## License

This project is licensed under the **Mozilla Public License 2.0 (MPL-2.0)**. See the [LICENSE](LICENSE) file for complete legal terms.
