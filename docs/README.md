# SampAudioStream - Documentation Hub

Welcome to the technical documentation for **SampAudioStream** (SA-MP & open.mp). This hub covers installation, server configuration, spatial and synchronized playback, asynchronous music search, internal transcoding/caching architecture, and the complete Pawn API reference.

---

## Documentation Sections

| Section | Description |
| :--- | :--- |
| **[📖 Getting Started](getting-started.md)** | Requirements, installation for SA-MP & open.mp (Windows & Linux), `config.json` reference, port forwarding, cookies setup, and drop-in migration from `PlayAudioStreamForPlayer`. |
| **[🔊 Playback, Hooking & Spatial Audio](playback-and-spatial.md)** | Individual vs. global playback (`SA_PlayAudioStreamForPlayer` vs. `SA_PlayAudioStream`), radius time-synchronization for finite tracks, live stream pass-through, `worldid`/`interiorid` filtering, volume control, and 3D entity attachment. |
| **[🔎 Music Search & Synced Channels](search-and-channels.md)** | Asynchronous track search (`SA_SearchAudio`) for interactive in-game dialogs, plus synchronized multi-player radio channels with playback queues (`SA_CreateChannel`, `SA_QueueChannel`). |
| **[⚙️ Architecture, Transcoding & Caching](architecture-and-caching.md)** | Dual-runtime C++20 design, zero-setup extractor provisioning (`yt-dlp` + `quickjs-ng`), static in-memory MP3 transcoding codecs, HLS/MPEG-TS live handling, LRU RAM metadata cache, and 2 MB ring buffers. |
| **[📚 Complete API Reference](api-reference.md)** | Exhaustive technical dictionary covering all 27 `SA_*` natives, hooked standard natives, 6 callbacks, enumerations, and constants. |

---

## Quick Navigation Matrix

Find immediate guidance based on your goal:

* **I want to install SampAudioStream on my SA-MP or open.mp server:**
  * Read [Getting Started](getting-started.md#1-installation).
  * Place `sampaudiostream.dll`/`.so` in `plugins/` (SA-MP) or `components/` (open.mp), and copy `include/sampaudiostream.inc`.
* **I already use `PlayAudioStreamForPlayer` in my gamemode and want YouTube/web links to work immediately:**
  * Read [Drop-In Hooking](getting-started.md#4-drop-in-native-hooking).
  * Just include `#include <sampaudiostream>` (or enable `hook_native_audio: true` in `config.json`) — existing scripts gain YouTube/platform support plus optional `worldid`, `interiorid`, and `volume` parameters.
* **I want everyone entering a club or 3D zone to hear the current timestamp of a song:**
  * Read [Global Streams & Radius Synchronization](playback-and-spatial.md#2-global-spatial-streams--time-synchronization).
  * Use `SA_PlayAudioStream` with `usepos = true` and `distance > 0.0`.
* **I want players to type a song name and pick from a search results dialog:**
  * Read [Asynchronous Music Search](search-and-channels.md#1-asynchronous-music-search).
  * Call `SA_SearchAudio(playerid, query, max_results)` and populate a dialog inside `OnAudioSearchResultItem` / `OnAudioSearchResults`.
* **I want persistent server radio stations with shared queues:**
  * Read [Synchronized Radio Channels](search-and-channels.md#2-synchronized-radio-channels).
  * Use `SA_CreateChannel`, `SA_PlayChannel`, `SA_QueueChannel`, and `SA_JoinChannel`.
* **I want to understand memory usage, caching TTL, and network ports:**
  * Read [Architecture, Transcoding & Caching](architecture-and-caching.md#4-caching--memory-management).

---

## Compatibility & System Requirements

* **Game Platforms:**
  * **SA-MP:** `0.3.7-R1` through `0.3.7-R5` (x86 32-bit).
  * **open.mp:** Native Component (`components/`) or Legacy Plugin (`plugins/`), x86 (32-bit).
* **Operating Systems:**
  * **Windows:** Windows 7 / 8 / 10 / 11, Windows Server 2008 R2+ (32-bit x86). Zero external MSVC runtime dependencies (`/MT` static linking; relies only on standard Windows DLLs `WS2_32.dll`, `WINHTTP.dll`, `KERNEL32.dll`, `MSWSOCK.dll`).
  * **Linux:** Debian 11+, Ubuntu 20.04+ (32-bit x86). Statically linked `libstdc++` / `libgcc`; loads `libcurl.so.4` dynamically if present on the system.
* **Pawn Compilers:**
  * Official Pawn `3.2.3664` (legacy `pawno`).
  * Community Pawn Compiler `3.10.x` (`qawno` / `sampctl`).
* **Network Requirements:**
  * Outbound HTTPS access on the host server (to fetch extractor binaries on first boot and resolve media streams).
  * One open TCP port for client audio streaming (default: `server_port + 1`, e.g. `7778`, configurable via `network.http_port`).
