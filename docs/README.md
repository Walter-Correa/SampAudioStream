# SampAudioStream - Documentation Hub

Welcome to the technical documentation for **SampAudioStream** (SA-MP & open.mp). This hub covers installation, server configuration, spatial and synchronized playback, asynchronous music search, internal transcoding/caching architecture, and the complete Pawn API reference.

> **New here?** [Getting Started](getting-started.md) → [Requirements](getting-started.md#1-requirements) → [First Startup Log](getting-started.md#4-first-startup-what-the-log-should-show).

---

## Documentation Sections

| Section | Description |
| :--- | :--- |
| **[📖 Getting Started](getting-started.md)** | Requirements, installation for SA-MP & open.mp (Windows & Linux), `config.json` reference, port forwarding, cookies setup, and drop-in migration from `PlayAudioStreamForPlayer`. |
| **[🔊 Playback, Hooking & Spatial Audio](playback-and-spatial.md)** | Individual vs. global playback (`SA_PlayAudioStreamForPlayer` vs. `SA_PlayAudioStream`), radius time-synchronization for finite tracks, live stream pass-through, `worldid`/`interiorid` filtering, volume control, and 3D entity attachment. |
| **[🔎 Music Search & Synced Channels](search-and-channels.md)** | Asynchronous track search (`SA_SearchAudioStream`) for interactive in-game dialogs, plus synchronized multi-player radio channels with playback queues, passwords, creator tracking, and auto-destroy policies (`SA_CreateAudioStreamChannel`, `SA_QueueChannelAudioStream`). |
| **[⚙️ Architecture, Transcoding & Caching](architecture-and-caching.md)** | Dual-runtime C++20 design, self-managed extractor provisioning (`yt-dlp` + `quickjs-ng`), static in-memory MP3 transcoding codecs, HLS/MPEG-TS live handling, LRU RAM metadata cache, and ~30s ring buffers (~480 KB at 128 kbps). |
| **[📚 Complete API Reference](api-reference.md)** | Exhaustive technical dictionary covering all 91 `SA_*` natives, hooked standard natives, 20 callbacks, enumerations, and constants. |
| **[🍪 YouTube Anti-Bot & Cookies Guide](youtube-cookies.md)** | Automatic `cookies.txt` / browser cookie fallback (`extractor.cookies_from_browser`), Firefox setup, headless VPS `cookies.txt` export, and troubleshooting `SA_ERR_AUTH_REQUIRED`. |
| **[⚠️ Important Notes](../README.md#important-notes)** | Extractor (`yt-dlp`) dependency disclaimer, server owner content responsibility, and non-affiliation notice. |

---

## Quick Navigation Matrix

Find immediate guidance based on your goal:

* **I want to install SampAudioStream on my SA-MP or open.mp server:**
  * Read [Getting Started](getting-started.md#2-installation).
  * Place `SampAudioStream.dll`/`.so` in `plugins/` (SA-MP) or `components/` (open.mp), and copy `include/SampAudioStream.inc`.
* **I already use `PlayAudioStreamForPlayer` in my gamemode and want YouTube/web links to work immediately:**
  * Read [Include Order & Drop-In Hooking](getting-started.md#include-order--drop-in-hooking).
  * Just include `#include <SampAudioStream>` (or enable `scripting.hook_native_audio: true` in `config.json`) — existing scripts gain YouTube/platform support plus optional `worldid`, `interiorid`, and `volume` parameters.
* **I want everyone entering a club or 3D zone to hear the current timestamp of a song:**
  * Read [Global Streams & Radius Synchronization](playback-and-spatial.md#2-global-spatial-streams--time-synchronization).
  * Use `SA_PlayAudioStream` with `usepos = true` and `distance > 0.0`.
* **I want players to type a song name and pick from a search results dialog:**
  * Read [Asynchronous Music Search](search-and-channels.md#1-asynchronous-music-search).
  * Call `SA_SearchAudioStream(playerid, query, max_results)` and populate a dialog inside `OnAudioStreamSearchResultItem` / `OnAudioStreamSearchResults`.
* **I want persistent server radio stations with shared queues:**
  * Read [Synchronized Radio Channels](search-and-channels.md#2-synchronized-radio-channels).
  * Use `SA_CreateAudioStreamChannel`, `SA_PlayChannelAudioStream`, `SA_QueueChannelAudioStream`, and `SA_JoinAudioStreamChannel`.
* **I want to understand memory usage, caching TTL, and network ports:**
  * Read [Architecture, Transcoding & Caching](architecture-and-caching.md#4-caching--memory-management).

---

## Compatibility & System Requirements

* **Game Platforms:**
  * **SA-MP:** `0.3.7-R1` through `0.3.7-R5` (x86 32-bit).
  * **open.mp:** Native Component (`components/`) or Legacy Plugin (`plugins/`), x86 (32-bit).
* **Operating Systems:**
  * **Windows:** Windows 10 / 11, Windows Server 2016+ recommended for official prebuilt `yt-dlp` binaries (static `/MT` C++ binary uses only `WS2_32.dll`, `WINHTTP.dll`, `KERNEL32.dll`; older Windows versions can still run direct audio streams, or platform extraction with a legacy/backported `yt-dlp` binary).
  * **Linux:** Debian 11+, Ubuntu 20.04+ (32-bit x86 / multilib). Statically linked `libstdc++` / `libgcc`; **requires** the 32-bit `libcurl.so.4` (see [Getting Started → Requirements](getting-started.md#1-requirements)).
* **Pawn Compilers:**
  * Official Pawn `3.2.3664` (legacy `pawno`).
  * Community Pawn Compiler `3.10.x` (`qawno` / `sampctl`).
* **Network Requirements:**
  * Outbound HTTPS access on the host server (to fetch extractor binaries on first boot and resolve media streams).
  * Open TCP ports for client audio streaming (default list `7778-7785`, configurable via `network.http_port`; the first free port is used).
