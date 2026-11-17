# Getting Started

This guide walks you through checking system requirements, installing **SampAudioStream**, opening the audio streaming port, verifying startup logs, configuring `config.json`, and resolving common issues.

---

## 1. Requirements

### Windows (Server Host)
* **No manual OS packages required:** the Windows binary links the MSVC C++ runtime statically (`/MT`) and uses Windows built-in `WINHTTP.dll`, `WS2_32.dll`, and `KERNEL32.dll`.
* `yt-dlp` and `quickjs-ng` (`qjs`) are downloaded and SHA-256 verified automatically on first server startup.
* **Minimum OS version:** **Windows 10 / 11** or **Windows Server 2016+** is recommended because current official prebuilt `yt-dlp` Windows binaries (`yt-dlp.exe` / `yt-dlp_x86.exe`) require Windows 10 or later (official `yt-dlp` releases dropped Windows 7 and 8). Direct audio URLs (HTTP MP3/OGG/WAV/FLAC/Shoutcast) still work on older Windows builds, whereas YouTube/platform extraction on legacy Windows requires placing an older or backported `yt-dlp` binary manually inside `scriptfiles/samp-audio-stream/bin/`.

### Linux (Server Host)
* **Required: 32-bit `libcurl` (`libcurl.so.4`).** Because the SA-MP plugin and open.mp component are 32-bit x86 binaries, 64-bit `libcurl` alone is not enough—without the 32-bit library, HTTPS extraction and stream proxying do not work. If it is missing, the server log prints the exact install command for your Linux distribution.

  | Distribution | Install Command |
  | :--- | :--- |
  | Debian 11 / 12, Ubuntu 20.04 / 22.04 | `sudo dpkg --add-architecture i386 && sudo apt update && sudo apt install libcurl4:i386` |
  | Debian 13+, Ubuntu 24.04+ | `sudo dpkg --add-architecture i386 && sudo apt update && sudo apt install libcurl4t64:i386` |
  | RHEL / Rocky / Alma / Fedora | `sudo dnf install libcurl.i686` |
  | Arch Linux | `sudo pacman -S lib32-curl` *(requires `[multilib]` enabled)* |
  | openSUSE | `sudo zypper install libcurl4-32bit` |

* **Check your Linux distribution:**
  ```bash
  lsb_release -a
  ```
  Or read `/etc/os-release`:
  ```bash
  cat /etc/os-release
  ```
* **Verify 32-bit `libcurl` is installed:**
  ```bash
  ldconfig -p | grep libcurl.so.4
  ```
  Example healthy output on a 64-bit Ubuntu/Debian system:
  ```text
  libcurl.so.4 (libc6,x86-64) => /lib/x86_64-linux-gnu/libcurl.so.4
  libcurl.so.4 (libc6) => /lib/i386-linux-gnu/libcurl.so.4
  ```
  Look for the `(libc6)` line **without** `x86-64`. If only `(libc6,x86-64)` appears, 32-bit `libcurl` is still missing.
* **Using Remote Desktop (RDP) on Ubuntu:** during `apt install`, Ubuntu may show a *"Daemons using outdated libraries"* dialog. Press `Space` to uncheck `xrdp` before continuing so your remote desktop session is not dropped.
* **CA certificates (`ca-certificates`):** normally preinstalled on Linux hosts; required for HTTPS validation.
* **Pure 32-bit x86 Linux kernels:** official prebuilt `yt-dlp` Linux binaries require a 64-bit kernel (`x86_64` / `arm64`). If your host runs a pure 32-bit x86 kernel, place a compatible `yt-dlp` binary manually in `scriptfiles/samp-audio-stream/bin/yt-dlp`.

---

## 2. Installation

### File Placement

1. Download `SampAudioStream-Windows.zip` or `SampAudioStream-Linux.zip` from [GitHub Releases](https://github.com/Walter-Correa/SampAudioStream/releases) matching your **server** OS.
2. Copy **only one** binary folder (`components/` for open.mp native mode **or** `plugins/` for SA-MP / open.mp legacy mode) plus `include/sampaudiostream.inc` into your server directory:

```text
[Server Root]/
  ├── components/                   (open.mp native component - Option A)
  │     └── sampaudiostream.dll     (or sampaudiostream.so on Linux)
  ├── plugins/                      (SA-MP 0.3.7 / open.mp legacy - Option B)
  │     └── sampaudiostream.dll     (or sampaudiostream.so on Linux)
  ├── pawno/include/                (SA-MP Pawn includes; or qawno/include/ on open.mp)
  │     └── sampaudiostream.inc
  └── scriptfiles/
        └── samp-audio-stream/      (Auto-created on first server startup)
              ├── config.json       (SA-MP only; open.mp uses server config.json)
              └── bin/
                    ├── yt-dlp-onedir/yt-dlp(.exe)
                    └── qjs(.exe)
```

* **No `chmod +x` needed on Linux:** `BinaryManager` automatically applies executable permissions (`chmod 0755`) to `yt-dlp` and `qjs` after downloading. Also extracts an isolated *onedir* `yt-dlp` directory (`yt-dlp-onedir/`) when available for ~1s faster cold-starts, falling back cleanly to a standalone binary if needed.

> [!IMPORTANT]
> **Never load both** `components/sampaudiostream.*` and `plugins/sampaudiostream.*` in the same server process. SampAudioStream rejects duplicate loading with:
> `SampAudioStream is already active in this process (rejected duplicate init from ...). Do not load both SA-MP plugin and open.mp component.`

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
  Place `sampaudiostream.dll` (or `sampaudiostream.so`) inside `components/`. open.mp detects and loads it automatically without adding `legacy_plugins`.
* **Option B — Legacy Plugin Mode:**
  Place the binary inside `plugins/` and register it in `config.json`:
  ```json
  {
    "pawn": {
      "legacy_plugins": ["sampaudiostream"]
    }
  }
  ```

### Include Order & Drop-In Hooking

Place `#include <sampaudiostream>` **after** `<a_samp>` or `<open.mp>` in your gamemode or filterscript (otherwise compilation stops with `#error Please include <a_samp> or <open.mp> BEFORE <sampaudiostream>.`):

```pawn
#include <open.mp> // or <a_samp>
#include <sampaudiostream>
```

By default, SampAudioStream transparently hooks `PlayAudioStreamForPlayer` and `StopAudioStreamForPlayer` at compile-time (ALS macro in `sampaudiostream.inc`) and runtime (AMX native table patching), giving existing calls YouTube/platform support plus optional `worldid`, `interiorid`, and `volume` parameters:

```pawn
PlayAudioStreamForPlayer(playerid, "https://www.youtube.com/watch?v=dQw4w9WgXcQ", 0.0, 0.0, 0.0, 50.0, false, 0, 0, 80.0);
```

Optional compile-time flags before `#include <sampaudiostream>`:
* `#define SA_DISABLE_NATIVE_HOOKS` — keeps `PlayAudioStreamForPlayer` / `StopAudioStreamForPlayer` unhooked while keeping all `SA_*` natives.
* `#define SA_DISABLE_DISCONNECT_HOOK` — disables the ALS `OnPlayerDisconnect` hook (`SA_Internal_PlayerDisconnect`).

---

## 3. Open the Audio Port (Firewall)

* **Why:** Each player's GTA client downloads audio over HTTP directly from your server on this TCP port (`http://<public_ip>:<http_port>/stream/<token>`). If this port is blocked, Pawn commands succeed without error, but **no player hears audio**.
* **Which port:** Check the port printed in `HTTP audio streaming server listening on IP:PORT` (default is `server_port + 1`, e.g. `7778` when your server runs on `7777`).

### Windows Firewall (PowerShell as Administrator)
```powershell
New-NetFirewallRule -DisplayName "SampAudioStream" -Direction Inbound -Protocol TCP -LocalPort 7778 -Action Allow
```

### Linux (`ufw`)
```bash
sudo ufw allow 7778/tcp
```

### Linux (`firewalld`)
```bash
sudo firewall-cmd --permanent --add-port=7778/tcp && sudo firewall-cmd --reload
```

### VPS / Cloud Security Groups
If your server runs on AWS, Oracle Cloud, Azure, DigitalOcean, OVH, Hetzner, or a game-hosting VPS, also add an inbound TCP allow rule for port `7778` (or your custom `http_port`) in your provider's network firewall / security group dashboard.

### External Connectivity & Local LAN Testing (`lan_aware_urls`)
* **Remote / Internet players:** if your server is behind a home router, NAT, or cloud firewall, **port-forward TCP `http_port`** (default `7778`) to the server machine and allow it in your VPS security group dashboard.
* **Local / LAN testing (`lan_aware_urls: true`, default):** if you connect from the same PC (`127.0.0.1`) or the same local subnet (`192.168.x.x`, `10.x.x.x`, `172.16–31.x.x`), SampAudioStream inspects the player's IP via `GetPlayerIp` and automatically builds the URL using `127.0.0.1` or the matching local NIC address instead of the external WAN `public_ip` (preventing NAT hairpinning issues during testing), while still sending `public_ip` to internet players. Set `lan_aware_urls` to `false` if a reverse proxy makes all players look local.
* **Quick port test:** from a **different network** (for example, your phone on 4G/5G with Wi-Fi disabled), open `http://<YOUR_PUBLIC_IP>:7778/` in a browser:
  * Receiving **`404 Not Found`** (or `400 Bad Request`) confirms the TCP port is open and SampAudioStream's HTTP server is reachable externally.
  * A browser timeout or *"connection refused"* means the port is still blocked by a firewall, VPS security group, or router port-forwarding rule.

---

## 4. First Startup: What the Log Should Show

When you start the server for the first time, watch for these lines (each category can also be silenced individually via `logging.*` / `log_*` config options):

1. **Plugin / Component & HTTP streaming server initialized:**
   * SA-MP:
     ```text
     [SampAudioStream] HTTP audio streaming server listening on 0.0.0.0:7778
       SampAudioStream v1.1.0 [SA-MP] loaded (config: scriptfiles/samp-audio-stream/config.json, HTTP port: 7778)
     ```
   * open.mp:
     ```text
     [SampAudioStream] [INFO] HTTP audio streaming server listening on 0.0.0.0:7778
     [SampAudioStream] [INFO] SampAudioStream v1.1.0 [open.mp] initialized (config: config.json [sampaudiostream], HTTP port: 7778)
     ```
2. **Public IPv4 auto-detected** *(omitted when `public_ip` is explicitly set in config)*:
   ```text
   [SampAudioStream] [INFO] Detected public server IPv4: 203.0.113.10
   ```
3. **Extractor & JS runtime downloaded on first boot** *(runs asynchronously in the background)*:
   ```text
   [SampAudioStream] [INFO] Downloading stream extractor (yt-dlp <tag>) onedir archive from ...
   [SampAudioStream] [INFO] yt-dlp <tag> downloaded and SHA-256 verified at scriptfiles/samp-audio-stream/bin/yt-dlp-onedir/yt-dlp(.exe)
   [SampAudioStream] [INFO] QuickJS-ng downloaded and SHA-256 verified at scriptfiles/samp-audio-stream/bin/qjs(.exe)
   [SampAudioStream] [INFO] Audio extractor subsystem ready.
   ```

The initial download takes a few seconds depending on network speed. Until it completes:
* `SA_IsExtractorReady()` returns `false`.
* Platform/search requests (YouTube, SoundCloud, Twitch, etc.) fail with error code `6` (`SA_ERR_EXTRACTOR_NOT_READY`), while direct MP3/WAV/OGG/FLAC HTTP URLs already work immediately.

---

## 5. Configuration

### Usually You Only Need to Change
* **`public_ip`**: set your server's public IPv4 or domain manually if auto-detection picks the wrong interface (NAT, multi-homed machines, or reverse proxies). Setting `public_ip` also skips external IP lookup requests on startup.
* **`http_port`**: `0` (default) automatically uses `server_port + 1` (e.g. `7778`). Set a fixed port (`1..65535`) if your host only forwards specific ports (a non-zero `http_port` fails initialization cleanly if already in use rather than picking an ephemeral port).
* **`lan_aware_urls`**: `true` (default) automatically gives `127.0.0.1` / LAN players a matching local server IP instead of the WAN `public_ip`.
* **`allow_private_network_urls`**: keep `false` (default) on public servers for SSRF protection; set `true` only if you intentionally stream audio from trusted LAN/private servers.

### Unified Configuration Reference

| SA-MP Key (`scriptfiles/samp-audio-stream/config.json`) | open.mp Key (`config.json` -> `"sampaudiostream"`) | Default | Range / Notes |
| :--- | :--- | :--- | :--- |
| `network.bind_ip` | `bind_ip` | `"0.0.0.0"` | Interface IP to bind HTTP streaming socket |
| `network.public_ip` | `public_ip` | `""` | Empty = auto-detect public IPv4 on startup |
| `network.http_port` | `http_port` | `0` | `0` (`server_port + 1`) or `1..65535` |
| `network.allow_private_network_urls` | `allow_private_network_urls` | `false` | Anti-SSRF block on private/loopback/metadata IPs |
| `network.lan_aware_urls` | `lan_aware_urls` | `true` | Serve local NIC/`127.0.0.1` URLs to LAN players |
| `audio.max_concurrent_streams` | `max_concurrent_streams` | `64` | `1..256` max concurrent stream sessions |
| `audio.mp3_bitrate_kbps` | `mp3_bitrate_kbps` | `128` | `64`, `96`, `128`, `160`, `192`, `256`, `320` |
| `audio.mp3_sample_rate` | `mp3_sample_rate` | `48000` | `32000`, `44100`, `48000` Hz |
| `audio.transcoder_burst_ms` | `transcoder_burst_ms` | `6000` | `2000..15000` ms initial burst lead buffer |
| `audio.spatial_update_interval_ms` | `spatial_update_interval_ms` | `200` | `50..5000` ms radius/attachment tick interval |
| `audio.player_priority_order` | `player_priority_order` | `"personal,channel,global"` | Overlap arbitration order across scopes |
| `extractor.bin_dir` | `bin_dir` | `"scriptfiles/samp-audio-stream/bin"` | Directory for `yt-dlp`, `qjs`, and extractor cache |
| `extractor.cookies_file` | `cookies_file` | `""` | Relative path to Netscape `cookies.txt` |
| `extractor.auto_download` | `auto_download_extractor` *(alias `auto_download`)* | `true` | Auto-download `yt-dlp` and `qjs` on startup |
| `extractor.auto_update` | `auto_update_extractor` *(alias `auto_update`)* | `true` | Background extractor updates on startup |
| `extractor.enable_js_runtime` | `enable_js_runtime` | `true` | Pass pinned QuickJS (`qjs`) runtime to `yt-dlp` |
| `performance.worker_threads` | `worker_threads` | `4` | `1..16` extraction & search worker threads |
| `performance.max_concurrent_transcoders` | `max_concurrent_transcoders` | `32` | `1..128` parallel active transcoder threads |
| `performance.cache_max_entries` | `cache_max_entries` | `256` | `16..4096` LRU cache entries per bucket |
| `performance.cache_ttl_seconds` | `cache_ttl_seconds` | `3600` | `60..86400` s (search TTL is `max(60, TTL/4)`) |
| `performance.max_search_results` | `max_search_results` | `10` | `1..50` max items returned by `SA_SearchAudioStream` |
| `performance.max_prefetch_mb` | `max_prefetch_mb` | `16` | `1..64` MB pull buffer cap per finite HTTP download *(max prefetch RAM = `max_prefetch_mb * active_transcoders`)* |
| `scripting.hook_native_audio` | `hook_native_audio` | `true` | Intercept vanilla `PlayAudioStreamForPlayer` / `Stop` |
| `scripting.debug_logging` | `debug` *(alias `debug_logging`)* | `false` | Detailed extractor/transcoder timing diagnostics |
| `scripting.pawn_text_encoding` | `pawn_text_encoding` | `"cp1252"` | `"cp1252"` *(GTA/SA-MP ANSI)* or `"utf8"` *(emojis stripped in CP1252)* |
| `logging.startup_banner` | `log_startup_banner` | `true` | Log loaded version and config summary |
| `logging.http_listen` | `log_http_listen` | `true` | Log HTTP streaming bind IP and port |
| `logging.public_ip_detected` | `log_public_ip_detected` | `true` | Log auto-detected public IPv4 |
| `logging.extractor_status` | `log_extractor_status` | `true` | Log `yt-dlp` / `QuickJS-ng` provisioning progress |

### Complete Config Examples
* **SA-MP:** `scriptfiles/samp-audio-stream/config.json` (auto-created on first launch):
  ```json
  {
      "network": {
          "bind_ip": "0.0.0.0",
          "public_ip": "",
          "http_port": 0,
          "allow_private_network_urls": false,
          "lan_aware_urls": true
      },
      "audio": {
          "max_concurrent_streams": 64,
          "mp3_bitrate_kbps": 128,
          "mp3_sample_rate": 48000,
          "transcoder_burst_ms": 6000,
          "spatial_update_interval_ms": 200,
          "player_priority_order": "personal,channel,global"
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
          "max_concurrent_transcoders": 32,
          "cache_max_entries": 256,
          "cache_ttl_seconds": 3600,
          "max_search_results": 10,
          "max_prefetch_mb": 16
      },
      "scripting": {
          "hook_native_audio": true,
          "debug_logging": false,
          "pawn_text_encoding": "cp1252"
      },
      "logging": {
          "startup_banner": true,
          "http_listen": true,
          "public_ip_detected": true,
          "extractor_status": true
      }
  }
  ```
* **open.mp:** stored under `"sampaudiostream"` inside the server's root `config.json` (`scriptfiles/samp-audio-stream/config.json` is **not** used on open.mp):
  ```json
  {
    "sampaudiostream": {
      "bind_ip": "0.0.0.0",
      "public_ip": "",
      "http_port": 0,
      "allow_private_network_urls": false,
      "lan_aware_urls": true,
      "max_concurrent_streams": 64,
      "mp3_bitrate_kbps": 128,
      "mp3_sample_rate": 48000,
      "transcoder_burst_ms": 6000,
      "spatial_update_interval_ms": 200,
      "player_priority_order": "personal,channel,global",
      "bin_dir": "scriptfiles/samp-audio-stream/bin",
      "cookies_file": "",
      "auto_download_extractor": true,
      "auto_update_extractor": true,
      "enable_js_runtime": true,
      "worker_threads": 4,
      "max_concurrent_transcoders": 32,
      "cache_max_entries": 256,
      "cache_ttl_seconds": 3600,
      "max_search_results": 10,
      "max_prefetch_mb": 16,
      "hook_native_audio": true,
      "debug_logging": false,
      "pawn_text_encoding": "cp1252",
      "log_startup_banner": true,
      "log_http_listen": true,
      "log_public_ip_detected": true,
      "log_extractor_status": true
    }
  }
  ```

---

## 6. Troubleshooting

| Symptom | Probable Cause | Solution |
| :--- | :--- | :--- |
| Log shows `32-bit libcurl (libcurl.so.4) not found` | Linux host is missing 32-bit `libcurl` | Run the exact install command printed in the log, or pick your distro in [1. Requirements](#1-requirements), then restart the server |
| Command accepted, nobody hears audio | Audio TCP port blocked, missing port-forward, or wrong `public_ip` | Forward/open TCP `http_port` ([3. Open the Audio Port](#3-open-the-audio-port-firewall)) and verify the URL passed to `OnAudioStreamReady` |
| Only you (on the server machine) hear audio | `http_port` is blocked externally / not port-forwarded | Keep `lan_aware_urls: true` for LAN testing, and open + port-forward TCP `http_port` externally so remote players can reach `public_ip:http_port` |
| Error code `6` (`SA_ERR_EXTRACTOR_NOT_READY`) | `yt-dlp` / `qjs` is still downloading or blocked | Wait a few seconds after startup and check the console log; ensure outbound HTTPS access is allowed |
| Error code `1` (`"URL not allowed"`) | Target resolves to loopback/private IP (anti-SSRF) | Use a public URL, or set `allow_private_network_urls` to `true` only for trusted internal audio servers |
| Error code `3` (`"DNS resolution failed for <host>"`) | Domain name typo, offline host, or server DNS outage | Verify hostname spelling and server DNS resolution (`nslookup <host>`) |
| YouTube fails with *"Sign in to confirm you're not a bot"* | Hosting/datacenter IP rate-limited by YouTube | Export browser cookies in Netscape format to `scriptfiles/samp-audio-stream/cookies.txt` (via a trusted *"Get cookies.txt LOCALLY"* browser extension from a logged-in session), upload it to the server, and set `"cookies_file": "scriptfiles/samp-audio-stream/cookies.txt"` |
| Log shows `Configured HTTP port N is unavailable` | Port is already bound by another process | Change `http_port` to an unused TCP port (or `0` for `server_port + 1`) and update firewall rules |
| Log shows `already active in this process` | Both `plugins/` and `components/` binaries were loaded | Remove the extra copy so only `plugins/` **or** `components/` is loaded (never both) |
| Pawn compile error on `sampaudiostream.inc` | Included before `<a_samp>` / `<open.mp>`, or wrong folder | Place `sampaudiostream.inc` in `pawno/include/` (or `qawno/include/`) and include it **after** `<a_samp>` or `<open.mp>` |

### Error Codes (`E_SA_ERROR_CODE`)

Quick summary of codes received in `OnAudioStreamError(playerid, streamid, error_code, error_message[])` — see [Complete API Reference](api-reference.md#e_sa_error_code) for full details:

* `SA_ERR_NONE` (`0`): No error.
* `SA_ERR_INVALID_URL` (`1`): Empty, malformed, or SSRF-blocked URL.
* `SA_ERR_EXTRACTION_FAILED` (`2`): `yt-dlp` or resolver could not find a playable stream.
* `SA_ERR_NETWORK_ERROR` (`3`): HTTP/TLS network failure, DNS lookup failure (`DNS resolution failed for ...`), timeout, or sustained live-stream outage.
* `SA_ERR_UNSUPPORTED_FORMAT` (`4`): Container or audio codec not supported.
* `SA_ERR_TRANSCODE_ERROR` (`5`): Malformed media, zero decoded audio within timeout (`20s`), or internal decoder/encoder failure.
* `SA_ERR_EXTRACTOR_NOT_READY` (`6`): `yt-dlp` / `qjs` is still downloading in the background.
* `SA_ERR_SEARCH_FAILED` (`7`): `SA_SearchAudioStream` execution or JSON parsing failure.
* `SA_ERR_STREAM_NOT_FOUND` (`8`): Invalid or already stopped `streamid`.
* `SA_ERR_INTERNAL_ERROR` (`9`): Worker queue timeout or unexpected internal failure.

---

## 7. FAQ

* **Do players need to install anything?**
  **No.** Audio streams to the standard GTA:SA / SA-MP / open.mp client over HTTP as a normal MP3 audio stream.
* **Why does `"Audio stream: http://..."` appear in player chat, and how can it be hidden?**
  That message is printed by the **GTA:SA SA-MP/open.mp game client itself** whenever a player receives `PlayAudioStreamForPlayer`. Players can toggle it on or off anytime by typing **`/audiomsg`** in their in-game chat (or disable it in SA-MP settings); there is no server-side packet to hide it while still playing audio.
* **How much RAM and CPU does it consume?**
  Each active transcoded stream or channel uses one transcoder slot (within `max_concurrent_transcoders`), one ~30 s circular MP3 output ring buffer (`~480 KB` at `128 kbps`, minimum `256 KB`), and—for finite HTTP files—an input pull buffer bounded by **`performance.max_prefetch_mb`** (`16 MB` default, upper bound `max_prefetch_mb * active_finite_transcoders`). Buffers are freed immediately when playback ends, stops, or the owner disconnects.
* **Can I keep using `PlayAudioStreamForPlayer` without changing my gamemode?**
  Yes. Including `<sampaudiostream>` automatically hooks `PlayAudioStreamForPlayer` and `StopAudioStreamForPlayer`. Define `#define SA_DISABLE_NATIVE_HOOKS` before `#include <sampaudiostream>` if you prefer keeping vanilla `PlayAudioStreamForPlayer` untouched and calling `SA_*` natives explicitly.
* **How do I update SampAudioStream?**
  Replace `plugins/sampaudiostream.*` (or `components/sampaudiostream.*`) and `include/sampaudiostream.inc` from the new release zip, then recompile your Pawn scripts. `yt-dlp` checks for updates automatically in the background when `auto_update` is `true`.
* **How do I uninstall SampAudioStream?**
  Remove `sampaudiostream.dll` / `.so` from `plugins/` or `components/`, remove its entry from `server.cfg` or `config.json`, remove `sampaudiostream.inc`, and delete `scriptfiles/samp-audio-stream/`.
