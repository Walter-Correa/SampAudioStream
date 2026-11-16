# Getting Started

This guide walks you through installing **SampAudioStream**, configuring `config.json`, opening the streaming TCP port, and upgrading existing `PlayAudioStreamForPlayer` calls.

---

## 1. Installation

### File Placement

Extract the release archive matching your server platform and OS into your server root:

```text
[Server Root]/
  ├── components/                   (open.mp native component)
  │     └── sampaudiostream.dll     (or sampaudiostream.so on Linux)
  ├── plugins/                      (SA-MP 0.3.7 / open.mp legacy)
  │     └── sampaudiostream.dll     (or sampaudiostream.so on Linux)
  ├── pawno/include/                (or qawno/include/)
  │     └── sampaudiostream.inc
  └── scriptfiles/
        └── samp-audio-stream/      (Auto-created on first server startup)
              ├── config.json
              └── bin/
                    ├── yt-dlp(.exe)
                    └── qjs(.exe)
```

> [!IMPORTANT]
> Install the binary in **either** `components/` (open.mp native mode) **or** `plugins/` (SA-MP / legacy mode) — never load both copies simultaneously.

### SA-MP (`server.cfg`)

* **Windows:**
  ```cfg
  plugins sampaudiostream.dll
  ```
* **Linux:**
  ```cfg
  plugins sampaudiostream.so
  ```

### open.mp (`config.json`)

* **Option A — Native open.mp Component (Recommended):**
  Place `sampaudiostream.dll` (or `sampaudiostream.so`) inside `components/`. open.mp detects and loads it automatically without requiring `legacy_plugins`.
  Optionally, configure overrides directly inside your server `config.json` under the `sampaudiostream` key:
  ```json
  {
    "sampaudiostream": {
      "http_port": 7778,
      "public_ip": "",
      "hook_native_audio": true,
      "debug": false
    }
  }
  ```
* **Option B — Legacy Plugin Mode:**
  Place the binary in `plugins/` and register it in `config.json`:
  ```json
  {
    "pawn": {
      "legacy_plugins": ["sampaudiostream"]
    }
  }
  ```

---

## 2. First Startup & Zero-Setup Extractor

On first launch, SampAudioStream automatically:
1. Generates `scriptfiles/samp-audio-stream/config.json` with sensible defaults.
2. Starts the embedded non-blocking Asio HTTP server (default TCP port: `server_port + 1`, e.g., `7778` if your server runs on `7777`).
3. Detects your server's public IPv4 address if `public_ip` is left blank (so external players connect to the right endpoint).
4. Downloads and extracts `yt-dlp` and the lightweight `quickjs-ng` (`qjs`) JS runtime into `scriptfiles/samp-audio-stream/bin/` asynchronously in the background—never blocking server startup or player connections.

Check readiness from Pawn with `SA_IsExtractorReady()` or watch the server log for ready status.

---

## 3. Configuration Reference (`scriptfiles/samp-audio-stream/config.json`)

```json
{
    "network": {
        "bind_ip": "0.0.0.0",
        "public_ip": "",
        "http_port": 0
    },
    "audio": {
        "max_concurrent_streams": 64,
        "mp3_bitrate_kbps": 128,
        "mp3_sample_rate": 48000,
        "spatial_update_interval_ms": 200
    },
    "extractor": {
        "bin_dir": "scriptfiles/samp-audio-stream/bin",
        "cookies_file": "",
        "auto_download": true,
        "auto_update": true,
        "enable_js_runtime": true
    },
    "performance": {
        "worker_threads": 4,
        "cache_max_entries": 256,
        "cache_ttl_seconds": 3600,
        "max_search_results": 10
    },
    "scripting": {
        "hook_native_audio": true,
        "debug_logging": false
    }
}
```

### Key Options
* **`network.http_port`**: `0` automatically picks `server_port + 1` (falls back to an ephemeral port if occupied). Make sure this TCP port is allowed through your firewall/VPS security group.
* **`network.public_ip`**: Leave empty for auto-detection, or set your server's public IP/domain explicitly (especially if behind NAT or reverse proxy).
* **`audio.mp3_bitrate_kbps` / `mp3_sample_rate`**: Output MP3 quality served to GTA clients (`128` kbps / `48000` Hz recommended).
* **`extractor.cookies_file`**: Optional path to a Netscape-format `cookies.txt` file if you need authenticated extraction on rate-limited hosts.
* **`performance.cache_max_entries` / `cache_ttl_seconds`**: Controls LRU RAM cache capacity and expiration (`3600` seconds = 1 hour for tracks, `900` seconds = 15 minutes for searches).

---

## 4. Drop-In Native Hooking

Include the header in your gamemode or filterscript:

```pawn
#include <a_samp> // or <open.mp>
#include <sampaudiostream>
```

By default, **SampAudioStream** hooks `PlayAudioStreamForPlayer` and `StopAudioStreamForPlayer` at both compile-time (ALS macro in `sampaudiostream.inc`) and runtime (AMX native table patching in C++):

* Calling standard direct audio (`http://radio.example.com/stream.mp3`) with default parameters works identically to vanilla SA-MP (direct pass-through).
* Calling `PlayAudioStreamForPlayer` with a **YouTube / SoundCloud / Twitch / etc. URL** or **extended parameters** routes automatically through SampAudioStream:

```pawn
// Vanilla call - now works with YouTube URLs AND extended world/interior/volume args!
PlayAudioStreamForPlayer(playerid, "https://www.youtube.com/watch?v=dQw4w9WgXcQ", 0.0, 0.0, 0.0, 50.0, false, 0, 0, 80.0);
```

To disable compile-time ALS macro hooking (while keeping `SA_*` natives):
```pawn
#define SA_DISABLE_NATIVE_HOOKS
#include <sampaudiostream>
```
