# SampAudioStream

[![GitHub release](https://img.shields.io/github/v/release/Walter-Correa/SampAudioStream?include_prereleases&color=blue)](https://github.com/Walter-Correa/SampAudioStream/releases)
[![License: MPL 2.0](https://img.shields.io/badge/License-MPL%202.0-brightgreen.svg)](https://opensource.org/licenses/MPL-2.0)
[![Platform](https://img.shields.io/badge/platform-SA--MP%20%7C%20open.mp-orange.svg)](https://open.mp)

**SampAudioStream** is a high-performance, asynchronous C++20 universal audio streaming plugin and native component engineered specifically for **San Andreas Multiplayer (SA-MP)** and **open.mp**.

It allows Pawn developers to play audio from **YouTube, SoundCloud, Twitch, Kick, Vimeo, TikTok**, and hundreds of other video/audio platforms—as well as **live broadcasts, Shoutcast/Icecast online radios, HLS streams, and direct MP3/OGG/WAV/FLAC links**—with **zero manual server installation** beyond placing the plugin/component binary, **zero `ffmpeg` dependency**, built-in **in-memory MP3 transcoding**, **spatial 3D/world/interior filtering**, **time-synchronized radius playback**, **asynchronous in-game music search**, and **synchronized multi-player radio channels**.

---

## Extending Standard SA-MP / open.mp AudioStream

| Feature / Capability | Standard `PlayAudioStreamForPlayer` | SampAudioStream |
| :--- | :--- | :--- |
| **Platform Architecture** | Built-in server native | ✅ Unified dual-runtime binary: native **open.mp Component** + **SA-MP Plugin** (32-bit `x86`) |
| **Server Dependencies** | None | ✅ **Zero manual OS setup**: static CRT (`/MT` & `-static-libstdc++`), embedded codecs, auto-provisioned `yt-dlp` + `quickjs-ng` |
| **Transcoding Engine** | Client plays direct HTTP audio only | ✅ Embedded static C++ pipeline (`libopus`, `libogg`, `fdk-aac`, `mp3lame`, `dr_libs`, `stb_vorbis`) |
| **Video, Audio & Live Sites** | Direct `.mp3`/`.ogg`/Shoutcast only | ✅ YouTube (videos & lives), SoundCloud, Twitch, Kick, Vimeo, TikTok, HLS (`.m3u8` / MPEG-TS), Shoutcast/Icecast & more |
| **Drop-In Compatibility** | Native behavior | ✅ Transparently hooks `PlayAudioStreamForPlayer` & `StopAudioStreamForPlayer` + full `SA_*` API |
| **World / Interior / Volume** | Global `usepos` only; no server volume scaling | ✅ Native `worldid`, `interiorid`, and `volume` (`0.0`–`200.0`) parameters |
| **Radius Time Synchronization** | Static URL starts from beginning per player | ✅ `SA_PlayAudioStream` paces finite tracks at `1x` real-time so latecomers entering the radius hear the current timestamp |
| **In-Game Music Search** | ❌ None | ✅ Non-blocking `SA_SearchAudio` returning structured results via callbacks for interactive Pawn dialogs |
| **Dynamic 3D Attachment** | Static coordinates only | ✅ `SA_AttachAudioStreamToPlayer` & `SA_AttachAudioStreamToVehicle` |
| **Synchronized Radio Channels** | ❌ None | ✅ `SA_CreateChannel`, `SA_QueueChannel`, `SA_SkipChannel`, and shared live HTTP endpoints |
| **Caching & Memory Footprint** | Uncached client HTTP requests | ✅ In-memory LRU+TTL metadata/search cache + 2 MB sliding ring buffers (zero MP3 disk bloat) |

---

## Documentation Hub

Modular guides and exhaustive API documentation are available in the [`docs/`](docs/) directory:

| Guide | Description |
| :--- | :--- |
| **[📖 Getting Started](docs/getting-started.md)** | Step-by-step installation for SA-MP & open.mp, `config.json` setup, HTTP ports, and drop-in migration. |
| **[🔊 Playback, Hooking & Spatial Audio](docs/playback-and-spatial.md)** | `SA_PlayAudioStreamForPlayer` vs. `SA_PlayAudioStream`, radius time sync, live streams, volume, pause, and 3D entity attachment. |
| **[🔎 Music Search & Synced Channels](docs/search-and-channels.md)** | Asynchronous `SA_SearchAudio` dialog workflows and synchronized multi-player radio stations with queues. |
| **[⚙️ Architecture, Transcoding & Caching](docs/architecture-and-caching.md)** | Internal C++20 worker pool, embedded codecs, HLS/MPEG-TS live decoding, LRU RAM cache, and 2 MB ring buffers. |
| **[📚 Complete API Reference](docs/api-reference.md)** | Exhaustive reference for all 27 `SA_*` natives, hooked vanilla natives, 6 callbacks, enums, constants, and helpers. |

---

## Quick Start & Installation

### 1. File Placement

```text
[Server Root]/
  ├── components/                   (open.mp native component)
  │     └── sampaudiostream.dll     (or sampaudiostream.so on Linux)
  ├── plugins/                      (SA-MP 0.3.7 / open.mp legacy)
  │     └── sampaudiostream.dll     (or sampaudiostream.so on Linux)
  ├── pawno/include/                (or qawno/include/)
  │     └── sampaudiostream.inc
  └── scriptfiles/
        └── samp-audio-stream/      (Auto-created on first launch: config.json + bin/)
```

### 2. Server Configuration

Install **either** the SA-MP plugin in `plugins/` **or** the open.mp component in `components/` (do not load both simultaneously).

* **SA-MP (`server.cfg`):**
  Windows:
  ```cfg
  plugins sampaudiostream.dll
  ```
  Linux:
  ```cfg
  plugins sampaudiostream.so
  ```

* **open.mp (`config.json`):**
  * **Option A — Native open.mp Component (Recommended):**
    Place `sampaudiostream.dll` (or `sampaudiostream.so`) inside `components/`. open.mp loads it automatically.
  * **Option B — Legacy Plugin Mode:**
    Place the binary inside `plugins/` and register it in `config.json`:
    ```json
    {
      "pawn": {
        "legacy_plugins": ["sampaudiostream"]
      }
    }
    ```

> [!TIP]
> Make sure TCP port `server_port + 1` (e.g. `7778` if your server is on `7777`, or your custom `network.http_port` in `scriptfiles/samp-audio-stream/config.json`) is open for client HTTP audio streaming.

### 3. Include in Your Script

```pawn
#include <open.mp> // Or <a_samp>
#include <sampaudiostream>
```

### 4. Basic Example

Play YouTube/platform links, search interactively, or spawn a 3D radius zone where everyone entering hears the song at its current timestamp:

```pawn
CMD:play(playerid, params[])
{
    if (isnull(params)) return SendClientMessage(playerid, -1, "Usage: /play <URL or song name>");

    // Works with YouTube, SoundCloud, Twitch, direct MP3, radios, or plain search text!
    SA_PlayAudioStreamForPlayer(playerid, params, 0.0, 0.0, 0.0, 50.0, false, -1, -1, 90.0);
    return 1;
}

CMD:zone(playerid, params[])
{
    if (isnull(params)) return SendClientMessage(playerid, -1, "Usage: /zone <URL or song name>");

    new Float:x, Float:y, Float:z;
    GetPlayerPos(playerid, x, y, z);

    // Everyone entering the 35m radius in this world & interior hears the song at its current time
    SA_PlayAudioStream(params, x, y, z, 35.0, true, GetPlayerVirtualWorld(playerid), GetPlayerInterior(playerid), 85.0);
    return 1;
}

// Vanilla PlayAudioStreamForPlayer is transparently hooked as well:
CMD:legacy(playerid, params[])
{
    PlayAudioStreamForPlayer(playerid, "https://www.youtube.com/watch?v=dQw4w9WgXcQ");
    return 1;
}
```

See [`Example.pwn`](Example.pwn) for a full showcase filterscript including asynchronous dialog search (`/search`), boombox/vehicle attachment, volume/pause controls, and synchronized radio channels.

---

## Complete API Reference

All 27 `SA_*` natives, hooked vanilla functions, 6 callbacks, constants, and enums are documented with signatures and examples in **[`docs/api-reference.md`](docs/api-reference.md)**:

* **[Playback & Control](docs/api-reference.md#1-playback--control-natives):** `SA_PlayAudioStreamForPlayer`, `SA_PlayAudioStream`, `SA_StopAudioStreamForPlayer`, `SA_StopAudioStream`, `SA_PauseAudioStreamForPlayer`, `SA_SetPlayerAudioVolume`, `SA_SetAudioStreamVolume`
* **[Spatial & Entity Attachment](docs/api-reference.md#2-spatial--attachment-natives):** `SA_AttachAudioStreamToPlayer`, `SA_AttachAudioStreamToVehicle`
* **[Asynchronous Music Search](docs/api-reference.md#3-asynchronous-music-search-natives):** `SA_SearchAudio`, `SA_CancelAudioSearch`
* **[Synchronized Radio Channels](docs/api-reference.md#4-synchronized-radio-channel-natives):** `SA_CreateChannel`, `SA_DestroyChannel`, `SA_PlayChannel`, `SA_QueueChannel`, `SA_SkipChannel`, `SA_StopChannel`, `SA_PauseChannel`, `SA_SetChannelVolume`, `SA_JoinChannel`, `SA_LeaveChannel`, `SA_IsPlayerInChannel`, `SA_GetPlayerChannel`
* **[Diagnostics & Metadata](docs/api-reference.md#5-diagnostics--metadata-natives):** `SA_GetPlayerStreamID`, `SA_GetStreamInfo`, `SA_IsExtractorReady`, `SA_GetHttpPort`
* **[Hooked Vanilla Natives](docs/api-reference.md#6-hooked-vanilla-sa-mp-natives):** `PlayAudioStreamForPlayer`, `StopAudioStreamForPlayer`
* **[Callbacks](docs/api-reference.md#7-callbacks):** `OnAudioStreamReady`, `OnAudioStreamStart`, `OnAudioStreamFinished`, `OnAudioStreamError`, `OnAudioSearchResults`, `OnAudioSearchResultItem`

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
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build -R core_tests --output-on-failure
```

---

## Version history

<details>
<summary>Show the version history</summary>

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
    - Non-blocking `SA_SearchAudio` returning metadata items via `OnAudioSearchResultItem` and `OnAudioSearchResults`.
    - Shared multi-player channels (`SA_CreateChannel`, `SA_PlayChannel`, `SA_QueueChannel`, `SA_SkipChannel`, `SA_JoinChannel`) with real-time synchronized HTTP endpoints.
  - **Bounded Memory & Caching**:
    - In-memory LRU+TTL cache for extracted track metadata (`3600s` default) and search results (`900s` default), plus 2 MB circular ring buffers freed immediately on completion or stop.

</details>

---

## Author & Credits

* [@Walter-Correa](https://github.com/Walter-Correa) - Concept and development of **SampAudioStream**, with the assistance of **Artificial Intelligence**.

---

## License

This project is licensed under the **Mozilla Public License 2.0 (MPL-2.0)**. See the [LICENSE](LICENSE) file for complete legal terms.
