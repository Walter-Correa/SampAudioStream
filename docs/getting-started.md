# Getting Started

This guide walks you through checking system requirements, installing **SampAudioStream**, opening the audio streaming port, verifying startup logs, configuring `config.json`, and resolving common issues.

---

## 1. Requirements

### Windows (Server Host)
* **No manual OS packages required:** the Windows binary links the MSVC C++ runtime statically (`/MT`) and uses Windows built-in `WINHTTP.dll`, `WS2_32.dll`, and `KERNEL32.dll`.
* `yt-dlp`, `quickjs-ng` (`qjs`), and (on 64-bit x86/arm64 hosts when enabled) **Deno `v2.9.8`** are downloaded and SHA-256 verified automatically on startup. `yt-dlp` and `quickjs-ng` settle first so the extractor becomes ready right away (~2 MB for `qjs`), while Deno (~42.9 MB `.zip` / `~98.1 MB` [`93.5 MiB`] unpacked `bin/deno/` directory on Windows; ~42.0 MB `.zip` on Linux x64) downloads and health-checks asynchronously in the background into `scriptfiles/SampAudioStream/bin/deno/` (`deno-cache/`). Keep ~150 MB of free disk space under `scriptfiles/SampAudioStream/bin/` if Deno is enabled.
* **Minimum OS version:** **Windows 10 / 11** or **Windows Server 2016+** is recommended because current official prebuilt `yt-dlp` Windows binaries (`yt-dlp.exe` / `yt-dlp_x86.exe`) require Windows 10 or later (official `yt-dlp` releases dropped Windows 7 and 8). Direct audio URLs (HTTP MP3/OGG/WAV/FLAC/Shoutcast) still work on older Windows builds, whereas YouTube/platform extraction on legacy Windows requires placing an older or backported `yt-dlp` binary manually inside `scriptfiles/SampAudioStream/bin/`.

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
* **ZIP archive extractor for background Deno (`unzip` / `bsdtar` / `python3`):** on 64-bit Linux hosts, unpacking Deno (`deno-x86_64-unknown-linux-gnu.zip` or `aarch64`) requires `unzip`, `bsdtar`/`tar`, or `python3`. If none is installed (e.g. minimal Debian/Ubuntu containers), install `unzip` (`sudo apt install unzip` / `sudo dnf install unzip`); otherwise SampAudioStream logs a warning and continues using `quickjs-ng`.
* **Pure 32-bit x86 Linux kernels:** official prebuilt `yt-dlp` Linux binaries require a 64-bit kernel (`x86_64` / `arm64`). If your host runs a pure 32-bit x86 kernel, place a compatible `yt-dlp` binary manually in `scriptfiles/SampAudioStream/bin/yt-dlp`. (Deno is only available for 64-bit x86_64/arm64 hosts; 32-bit/armv7 hosts automatically use QuickJS.)

---

## 2. Installation

### File Placement

1. Download `SampAudioStream-Windows.zip` or `SampAudioStream-Linux.zip` from [GitHub Releases](https://github.com/Walter-Correa/SampAudioStream/releases) matching your **server** OS.
2. Copy **only one** binary folder (`components/` for open.mp native mode **or** `plugins/` for SA-MP / open.mp legacy mode) plus `include/SampAudioStream.inc` into your server directory:

```text
[Server Root]/
  ├── components/                   (open.mp native component - Option A)
  │     └── SampAudioStream.dll     (or SampAudioStream.so on Linux)
  ├── plugins/                      (SA-MP 0.3.7 / open.mp legacy - Option B)
  │     └── SampAudioStream.dll     (or SampAudioStream.so on Linux)
  ├── filterscripts/                (Optional interactive demo filterscript)
  │     ├── FS_SampAudioStream_Demo.pwn
  │     └── FS_SampAudioStream_Demo.amx
  ├── pawno/include/                (SA-MP Pawn includes; or qawno/include/ on open.mp)
  │     └── SampAudioStream.inc
  └── scriptfiles/
        └── SampAudioStream/      (Auto-created on first server startup)
              ├── config.json       (SA-MP only; open.mp uses server config.json)
              └── bin/
                    ├── yt-dlp-onedir/yt-dlp(.exe)
                    ├── qjs(.exe)
                    └── deno/deno(.exe)      (downloaded in background on supported 64-bit hosts)
```

* **No `chmod +x` needed on Linux:** `BinaryManager` automatically applies executable permissions (`chmod 0755`) to `yt-dlp` and `qjs` after downloading. Also extracts an isolated *onedir* `yt-dlp` directory (`yt-dlp-onedir/`) when available for ~1s faster cold-starts, falling back cleanly to a standalone binary if needed.

> [!IMPORTANT]
> **Never load both** `components/SampAudioStream.*` and `plugins/SampAudioStream.*` in the same server process. SampAudioStream rejects duplicate loading with:
> `SampAudioStream is already active in this process (rejected duplicate init from ...). Do not load both SA-MP plugin and open.mp component.`

### SA-MP (`server.cfg`)

* **Windows:**
  ```cfg
  plugins SampAudioStream.dll
  ```
* **Linux:**
  ```cfg
  plugins SampAudioStream.so
  ```

### open.mp (`config.json`)

* **Option A — Native open.mp Component (Recommended):**
  Place `SampAudioStream.dll` (or `SampAudioStream.so`) inside `components/`. open.mp detects and loads it automatically without adding `legacy_plugins`.
* **Option B — Legacy Plugin Mode:**
  Place the binary inside `plugins/` and register it in `config.json`:
  ```json
  {
    "pawn": {
      "legacy_plugins": ["SampAudioStream"]
    }
  }
  ```

### Include Order & Drop-In Hooking

Place `#include <SampAudioStream>` **after** `<a_samp>` or `<open.mp>` in your gamemode or filterscript (otherwise compilation stops with `#error Please include <a_samp> or <open.mp> BEFORE <SampAudioStream>.`):

```pawn
#include <open.mp> // or <a_samp>
#include <SampAudioStream>
```

By default, SampAudioStream transparently hooks `PlayAudioStreamForPlayer` and `StopAudioStreamForPlayer` at compile-time (ALS macro in `SampAudioStream.inc`) and runtime (AMX native table patching), giving existing calls YouTube/platform support plus optional `worldid`, `interiorid`, and `volume` parameters:

```pawn
PlayAudioStreamForPlayer(playerid, "https://www.youtube.com/watch?v=dQw4w9WgXcQ", 0.0, 0.0, 0.0, 50.0, false, 0, 0, 80.0);
```

Optional compile-time flags before `#include <SampAudioStream>`:
* `#define SA_DISABLE_NATIVE_HOOKS` — keeps `PlayAudioStreamForPlayer` / `StopAudioStreamForPlayer` unhooked while keeping all `SA_*` natives.
* `#define SA_DISABLE_CONNECT_HOOK` — disables the ALS `OnPlayerConnect` hook (`SA_Internal_PlayerConnect`).
* `#define SA_DISABLE_DISCONNECT_HOOK` — disables the ALS `OnPlayerDisconnect` hook (`SA_Internal_PlayerDisconnect`).
* `#define SA_DISABLE_GAMEMODE_EXIT_HOOK` — disables the ALS `OnGameModeExit` lifecycle hook (`SA_Internal_GameModeExit`).

---

## 3. Open the Audio Port (Firewall)

* **Why:** Each player's GTA client downloads audio over HTTP directly from your server on this TCP port (`http://<public_ip>:<http_port>/stream/<token>`). If this port is blocked, Pawn commands succeed without error, but **no player hears audio**.
* **Which ports:** the plugin uses the first free port of `network.http_port` (default `"7778-7785"`) and prints it in `HTTP audio streaming server listening on IP:PORT`. Open the **whole list** so audio keeps working when the first port is still busy (for example right after a quick server restart, the plugin moves on to the next port of the list).

### Windows Firewall (PowerShell as Administrator)
```powershell
New-NetFirewallRule -DisplayName "SampAudioStream" -Direction Inbound -Protocol TCP -LocalPort 7778-7785 -Action Allow
```

### Linux (`ufw`)
```bash
sudo ufw allow 7778:7785/tcp
```

### Linux (`firewalld`)
```bash
sudo firewall-cmd --permanent --add-port=7778-7785/tcp && sudo firewall-cmd --reload
```

### VPS / Cloud Security Groups
If your server runs on AWS, Oracle Cloud, Azure, DigitalOcean, OVH, Hetzner, or a game-hosting VPS, also add an inbound TCP allow rule for ports `7778-7785` (or every port of your custom `http_port` list) in your provider's network firewall / security group dashboard.

### External Connectivity & Local LAN Testing (`lan_aware_urls`)
* **Remote / Internet players:** if your server is behind a home router, NAT, or cloud firewall, **port-forward every TCP port of `http_port`** (default `7778-7785`) to the server machine and allow it in your VPS security group dashboard.
* **Local / LAN testing (`lan_aware_urls: true`, default):** if you connect from the same PC (`127.0.0.1`) or the same local subnet (`192.168.x.x`, `10.x.x.x`, `172.16–31.x.x`), SampAudioStream inspects the player's IP via `GetPlayerIp` and automatically builds the URL using `127.0.0.1` or the matching local NIC address instead of the external WAN `public_ip` (preventing NAT hairpinning issues during testing), while still sending `public_ip` to internet players. Set `lan_aware_urls` to `false` if a reverse proxy makes all players look local.
* **Quick port test:** from a **different network** (for example, your phone on 4G/5G with Wi-Fi disabled), open `http://<YOUR_PUBLIC_IP>:7778/` in a browser:
  * Receiving **`404 Not Found`** (or `400 Bad Request`) confirms the TCP port is open and SampAudioStream's HTTP server is reachable externally.
  * A browser timeout or *"connection refused"* means the port is still blocked by a firewall, VPS security group, or router port-forwarding rule.

---

## 4. First Startup: What the Log Should Show

When you start the server, SampAudioStream waits up to **1.5 seconds (`1500 ms`)** during initialization so the startup banner, extractor status, and auto-detected public IPv4 print cleanly grouped together before gamemode logs (when a first-time `yt-dlp`/`qjs` download is required, startup skips waiting and immediately reports background downloading). Each category can also be silenced individually via `logging.*` config options:

1. **Plugin / Component, Extractor & Public IPv4 grouped output:**
   * SA-MP:
     ```text
     [SampAudioStream] HTTP audio streaming server listening on 0.0.0.0:7778 (connection limit 264 for 100 player slots)
     [SampAudioStream] SampAudioStream v2.3.1 [SA-MP] loaded (config: scriptfiles/SampAudioStream/config.json, HTTP port: 7778)
     [SampAudioStream] Audio extractor subsystem ready.
     [SampAudioStream] Detected public server IPv4: 203.0.113.10
     ```
   * open.mp:
     ```text
     [SampAudioStream] HTTP audio streaming server listening on 0.0.0.0:7778 (connection limit 264 for 100 player slots)
     [SampAudioStream] SampAudioStream v2.3.1 [open.mp] initialized (config: config.json [sampaudiostream], HTTP port: 7778)
     [SampAudioStream] Audio extractor subsystem ready.
     [SampAudioStream] Detected public server IPv4: 203.0.113.10
     ```
2. **Slow network / First-time extractor download** *(if `yt-dlp` / `qjs` is downloading for the first time or a lookup exceeds `1.5s`)*:
   ```text
   [SampAudioStream] Audio extractor: downloading in background...
   [SampAudioStream] Public IP detection still running...
   [SampAudioStream] Downloading stream extractor (yt-dlp <tag>) onedir archive from ...
   [SampAudioStream] yt-dlp <tag> downloaded and SHA-256 verified at scriptfiles/SampAudioStream/bin/yt-dlp-onedir/yt-dlp(.exe)
   [SampAudioStream] QuickJS-ng downloaded and SHA-256 verified at scriptfiles/SampAudioStream/bin/qjs(.exe)
   [SampAudioStream] Audio extractor subsystem ready.
   ```

The initial download takes a few seconds depending on network speed. Until it completes:
* `SA_IsAudioStreamExtractorReady()` returns `false`.
* Platform/search requests (YouTube, SoundCloud, Twitch, etc.) fail with error code `6` (`SA_ERR_EXTRACTOR_NOT_READY`), while direct MP3/WAV/OGG/FLAC HTTP URLs already work immediately.

---

## 5. Configuration

### Usually You Only Need to Change
* **`network.public_ip`**: set your server's public IPv4 or domain manually if auto-detection picks the wrong interface (NAT, multi-homed machines, or reverse proxies). Setting `network.public_ip` also skips external IP lookup requests on startup.
* **`network.http_port`**: TCP ports for the audio HTTP server, as a string of single ports and ranges separated by commas, tried in order. Default `"7778-7785"`.
  * Examples: `"7778"` (one port), `"7778-7785"` (range), `"7778-7780,7785,7792"` (mixed). Spaces are ignored, duplicates are skipped, ranges must be ascending, ports go from `1` to `65535`, at most 64 ports.
  * The first free port of the list is used immediately, so the first port has priority whenever it is free. If every port is busy (typically the previous server process still releasing its socket after a quick restart), the list is retried every 250 ms for up to 3 s; only then does the plugin fall back to a random OS-assigned port and log a warning, since players behind a firewall cannot reach that port.
  * Open every port of the list in your firewall / port forwarding (see [Open the Audio Port](#3-open-the-audio-port-firewall)).
  * Legacy integer values still work: a number such as `7778` is a one-port list and `0` means the default list. An invalid list falls back to the default with a warning.
* **`network.lan_aware_urls`**: `true` (default) automatically gives `127.0.0.1` / LAN players a matching local server IP instead of the WAN `network.public_ip`.
* **`network.allow_private_network_urls`**: keep `false` (default) on public servers for SSRF protection; set `true` only if you intentionally stream audio from trusted LAN/private servers.

### Unified Configuration Reference

On **SA-MP**, these groups and keys live in `scriptfiles/SampAudioStream/config.json`; on **open.mp**, the exact same groups and keys live inside `"sampaudiostream"` in the server's root `config.json` (`sampaudiostream.<group>.<key>`, and `scriptfiles/SampAudioStream/config.json` is not used).

| Key (`group.key`) | Default | Range / Notes |
| :--- | :--- | :--- |
| `network.bind_ip` | `"0.0.0.0"` | Interface IP to bind HTTP streaming socket |
| `network.public_ip` | `""` | Empty = auto-detect public IPv4 on startup |
| `network.http_port` | `"7778-7785"` | Port list tried in order: single ports and ascending ranges separated by commas (e.g. `"7778-7780,7785"`), `1..65535`, up to 64 ports; first free port wins, retried up to 3 s, then an OS-assigned port |
| `network.lan_aware_urls` | `true` | Serve local NIC/`127.0.0.1` URLs to LAN players |
| `network.allow_private_network_urls` | `false` | Anti-SSRF block on private/loopback/metadata IPs |
| `network.max_connections_per_ip` | `64` | `1..4096` simultaneous open HTTP audio connections per IP *(raise on CGNAT/mobile/LAN-heavy servers; if your firewall has a per-IP rule such as `iptables connlimit`, set it >= this value)* |
| `audio.mp3_bitrate_kbps` | `128` | `64`, `96`, `128`, `160`, `192`, `256`, `320` |
| `audio.mp3_sample_rate` | `48000` | `32000`, `44100`, `48000` Hz |
| `audio.transcoder_burst_ms` | `6000` | `2000..15000` ms initial burst lead buffer |
| `audio.max_concurrent_streams` | `64` | `1..256` max concurrent stream sessions |
| `audio.spatial_update_interval_ms` | `200` | `50..5000` ms radius/attachment tick interval |
| `audio.player_priority_order` | `"personal,channel,global"` | Overlap arbitration order across scopes |
| `audio.normalize_volume` | `false` | Optional track loudness equalization (`-16 dBFS` RMS target with `[-12, +9] dB` AGC gain and peak limiter at `0.98`). Applies only to server-transcoded/proxied streams (global, channel, live, and transcoded personal streams); direct passthrough streams are not normalized. Minimal CPU cost (one multiply and sum-of-squares per sample) |
| `audio.live_title_poll_seconds` | `30` | `0` (disabled) or `10..300` s interval for polling Shoutcast/Icecast ICY `StreamTitle` updates on live radios. Each poll opens a short HTTP connection up to 64 KB (so the radio briefly sees one extra listener at that interval) |
| `audio.music_dir` | `"scriptfiles/SampAudioStream/music"` | Local server folder for depth-1 music folders and loose tracks playable via `library://` references *(created on startup if missing)* |
| `audio.sound_dir` | `"scriptfiles/SampAudioStream/sound"` | Local server folder for short sound effects (alerts, notifications, jingles, UI and gameplay sounds such as achievements or a radio switching off) played with `SA_PlayAudioStreamSoundForPlayer`. Files (`.mp3`, `.ogg`, `.wav`, up to 5 MB, loose or in depth-1 subfolders) are sent to the player as they are, without transcoding, so they start immediately and use no transcoder slot; this folder is never listed in the music library *(created on startup if missing; keep it separate from `audio.music_dir`)* |
| `audio.audio_file_label` | `"Audio File"` | Text shown after the file name in the client on-screen stream text for direct/local audio files played as personal or global sessions (e.g. `musicas.mp3 - Audio File`); extracted tracks show their platform (e.g. `YouTube`) and live radios their station name |
| `channels.max_channels` | `100` | `1..1000` max simultaneous synchronized channels |
| `channels.max_queue_per_channel` | `50` | `1..1000` max queued tracks per synchronized channel |
| `channels.channel_restore_grace_seconds` | `300` | `0..3600` s grace window after boot for restored `OnDisconnect` channels (`0` = remove on boot) |
| `extractor.bin_dir` | `"scriptfiles/SampAudioStream/bin"` | Directory for `yt-dlp`, `qjs`, and extractor cache |
| `extractor.auto_download` | `true` | Auto-download `yt-dlp` and `qjs` on startup |
| `extractor.auto_update` | `true` | Background extractor updates on startup |
| `extractor.enable_js_runtime` | `true` | Pass configured JS runtime (`deno` / `qjs`) to `yt-dlp` (`false` disables all JS runtimes) |
| `extractor.js_runtime` | `"auto"` | Preferred YouTube JS challenge runtime: `"auto"` (use Deno `v2.9.8` when installed & healthy, else QuickJS), `"deno"`, or `"quickjs"` |
| `extractor.cookies_file` | `""` | Relative path to Netscape `cookies.txt` (when empty, `scriptfiles/SampAudioStream/cookies.txt` is used automatically if present) |
| `extractor.cookies_from_browser` | `"auto"` | Automatic browser cookie fallback only after YouTube blocks the server IP (`"auto"` = Firefox first then Chromium-family; `"off"` = disable) |
| `extractor.yt_dlp_cache_max_age_days` | `7` | `1..90` days before pruning old `yt-dlp-cache` files (`0` = disabled) |
| `extractor.max_search_results` | `10` | `1..50` max items returned by `SA_SearchAudioStream` |
| `performance.worker_threads` | `4` | `1..16` extraction & search worker threads |
| `performance.max_concurrent_transcoders` | `32` | `1..128` parallel active transcoder threads |
| `performance.max_prefetch_mb` | `8` | `1..64` MB pull buffer cap per finite HTTP download *(max prefetch RAM = `performance.max_prefetch_mb * active_transcoders`; local library files always use a fixed `1 MB` buffer)* |
| `performance.cache_max_entries` | `256` | `16..4096` LRU cache entries per bucket |
| `performance.cache_ttl_seconds` | `3600` | `60..86400` s (search TTL is `max(60, TTL/4)`) |
| `scripting.hook_native_audio` | `true` | Intercept vanilla `PlayAudioStreamForPlayer` / `Stop` |
| `scripting.pawn_text_encoding` | `"cp1252"` | Bidirectional conversion for `"cp1252"` *(GTA/SA-MP ANSI: script inputs like search/URLs/channel names converted to UTF-8, outputs back to CP1252)* or `"utf8"` *(raw UTF-8 pass-through)* |
| `logging.debug` | `false` | Detailed extractor/transcoder timing diagnostics |
| `logging.startup_banner` | `true` | Log loaded version and config summary |
| `logging.http_listen` | `true` | Log HTTP streaming bind IP and port |
| `logging.public_ip_detected` | `true` | Log auto-detected public IPv4 |
| `logging.extractor_status` | `true` | Log `yt-dlp` / JS runtime (`QuickJS-ng` / `Deno`) provisioning progress |

> The total number of HTTP audio connections is managed automatically: `player slots × 2 + 64` (minimum 128, up to 2064), read from `maxplayers` (SA-MP) or `max_players` (open.mp). On Linux, the plugin raises the process open file limit by itself when needed; it only warns if the system hard limit is too low.

### Complete Config Examples
* **SA-MP:** `scriptfiles/SampAudioStream/config.json` (auto-created on first launch):
  ```json
  {
      "network":
      {
          "bind_ip": "0.0.0.0",
          "public_ip": "",
          "http_port": "7778-7785",
          "lan_aware_urls": true,
          "allow_private_network_urls": false,
          "max_connections_per_ip": 64
      },
      "audio":
      {
          "mp3_bitrate_kbps": 128,
          "mp3_sample_rate": 48000,
          "transcoder_burst_ms": 6000,
          "max_concurrent_streams": 64,
          "spatial_update_interval_ms": 200,
          "player_priority_order": "personal,channel,global",
          "normalize_volume": false,
          "live_title_poll_seconds": 30,
          "music_dir": "scriptfiles/SampAudioStream/music",
          "sound_dir": "scriptfiles/SampAudioStream/sound",
          "audio_file_label": "Audio File"
      },
      "channels":
      {
          "max_channels": 100,
          "max_queue_per_channel": 50,
          "channel_restore_grace_seconds": 300
      },
      "extractor":
      {
          "bin_dir": "scriptfiles/SampAudioStream/bin",
          "auto_download": true,
          "auto_update": true,
          "enable_js_runtime": true,
          "js_runtime": "auto",
          "cookies_file": "",
          "cookies_from_browser": "auto",
          "yt_dlp_cache_max_age_days": 7,
          "max_search_results": 10
      },
      "performance":
      {
          "worker_threads": 4,
          "max_concurrent_transcoders": 32,
          "max_prefetch_mb": 8,
          "cache_max_entries": 256,
          "cache_ttl_seconds": 3600
      },
      "scripting":
      {
          "hook_native_audio": true,
          "pawn_text_encoding": "cp1252"
      },
      "logging":
      {
          "debug": false,
          "startup_banner": true,
          "http_listen": true,
          "public_ip_detected": true,
          "extractor_status": true
      }
  }
  ```
* **open.mp:** stored under `"sampaudiostream"` inside the server's root `config.json` (`scriptfiles/SampAudioStream/config.json` is **not** used on open.mp):
  ```json
  {
      "sampaudiostream":
      {
          "network":
          {
              "bind_ip": "0.0.0.0",
              "public_ip": "",
              "http_port": "7778-7785",
              "lan_aware_urls": true,
              "allow_private_network_urls": false,
              "max_connections_per_ip": 64
          },
          "audio":
          {
              "mp3_bitrate_kbps": 128,
              "mp3_sample_rate": 48000,
              "transcoder_burst_ms": 6000,
              "max_concurrent_streams": 64,
              "spatial_update_interval_ms": 200,
              "player_priority_order": "personal,channel,global",
              "normalize_volume": false,
              "live_title_poll_seconds": 30,
              "music_dir": "scriptfiles/SampAudioStream/music",
              "sound_dir": "scriptfiles/SampAudioStream/sound",
              "audio_file_label": "Audio File"
          },
          "channels":
          {
              "max_channels": 100,
              "max_queue_per_channel": 50,
              "channel_restore_grace_seconds": 300
          },
          "extractor":
          {
              "bin_dir": "scriptfiles/SampAudioStream/bin",
              "auto_download": true,
              "auto_update": true,
              "enable_js_runtime": true,
              "js_runtime": "auto",
              "cookies_file": "",
              "cookies_from_browser": "auto",
              "yt_dlp_cache_max_age_days": 7,
              "max_search_results": 10
          },
          "performance":
          {
              "worker_threads": 4,
              "max_concurrent_transcoders": 32,
              "max_prefetch_mb": 8,
              "cache_max_entries": 256,
              "cache_ttl_seconds": 3600
          },
          "scripting":
          {
              "hook_native_audio": true,
              "pawn_text_encoding": "cp1252"
          },
          "logging":
          {
              "debug": false,
              "startup_banner": true,
              "http_listen": true,
              "public_ip_detected": true,
              "extractor_status": true
          }
      }
  }
  ```

---

## 6. Troubleshooting

| Symptom | Probable Cause | Solution |
| :--- | :--- | :--- |
| Log shows `32-bit libcurl (libcurl.so.4) not found` | Linux host is missing 32-bit `libcurl` | Run the exact install command printed in the log, or pick your distro in [1. Requirements](#1-requirements), then restart the server |
| Players hear nothing and no error appears *(log shows `Player <id> did not connect to the audio stream within 15 s (HTTP port <port>/TCP)`)* | The plugin's HTTP TCP port is blocked in the server firewall or `network.public_ip` is wrong | Open every TCP port of `network.http_port` (by default `7778-7785`) in your host firewall/control panel — e.g. `sudo ufw allow 7778:7785/tcp` or `sudo iptables -A INPUT -p tcp --dport 7778:7785 -j ACCEPT` followed by `sudo netfilter-persistent save` (TCP and UDP are separate ports, so another service using the same port on **UDP** such as sampvoice does **not** conflict) |
| Some players hear nothing on busy servers / mobile players *(log shows `HTTP audio connection ... rejected: per-IP limit reached` or `total connection limit reached`)* | The server hit `network.max_connections_per_ip` (CGNAT/mobile carriers, LAN houses), a firewall per-IP limit is lower, or non-player connections exhausted the automatic slot-derived total limit (`player slots × 2 + 64`) | Raise `network.max_connections_per_ip` (`1..4096`) in config and ensure any firewall per-IP cap (e.g. `iptables connlimit`) is **>= `network.max_connections_per_ip`**. If `total connection limit reached` appears, inspect incoming traffic for non-player connections. On Linux, if the startup log warns about the open file limit, raise the hard limit (`LimitNOFILE=` in systemd or `nofile` in `/etc/security/limits.conf`) |
| Only you (on the server machine) hear audio | `network.http_port` is blocked externally / not port-forwarded | Keep `network.lan_aware_urls: true` for LAN testing, and open + port-forward TCP `network.http_port` externally so remote players can reach `network.public_ip` |
| Error code `6` (`SA_ERR_EXTRACTOR_NOT_READY`) | `yt-dlp` / `qjs` is still downloading or blocked | Wait a few seconds after startup and check the console log; ensure outbound HTTPS access is allowed |
| Error code `1` (`"URL not allowed"`) | Target resolves to loopback/private IP (anti-SSRF) | Use a public URL, or set `allow_private_network_urls` to `true` only for trusted internal audio servers |
| Error code `3` (`"DNS resolution failed for <host>"`) | Domain name typo, offline host, or server DNS outage | Verify hostname spelling and server DNS resolution (`nslookup <host>`) |
| YouTube fails with *"Sign in to confirm you're not a bot"* (`SA_ERR_AUTH_REQUIRED`) | Datacenter/VPS IP challenged by YouTube and automatic cookie fallback found no valid cookies | Follow [the YouTube Cookies Guide](youtube-cookies.md) (sign in with a secondary account in Firefox on the server, or place exported `cookies.txt` at `scriptfiles/SampAudioStream/cookies.txt`) |
| Log shows `No port from network.http_port (...) is available` or `using an OS-assigned port` | Every port of the list is bound by another process (or still held by a server that did not finish closing) | Add free ports to `http_port` (e.g. `"7778-7785,7790-7795"`), open them in the firewall, and make sure the previous server process has exited before starting a new one |
| Log shows `already active in this process` | Both `plugins/` and `components/` binaries were loaded | Remove the extra copy so only `plugins/` **or** `components/` is loaded (never both) |
| Pawn compile error on `SampAudioStream.inc` | Included before `<a_samp>` / `<open.mp>`, or wrong folder | Place `SampAudioStream.inc` in `pawno/include/` (or `qawno/include/`) and include it **after** `<a_samp>` or `<open.mp>` |
| Platform links (e.g. YouTube) suddenly fail for everyone with error code `2` (`SA_ERR_EXTRACTION_FAILED`), while direct MP3/radio URLs still work | The platform changed and the installed `yt-dlp` is outdated | Wait for the automatic update (`extractor.auto_update`), or force it: stop the server, delete `scriptfiles/SampAudioStream/bin/`, and start again so the latest `yt-dlp` is downloaded |
| Saved channels come back after a restart even though you expected them gone | The channel was created with persistence enabled (`persistent = true`, default) | Destroy the channel explicitly with `SA_DestroyAudioStreamChannel` (this also deletes its saved file), or pass `persistent = false` to `SA_CreateAudioStreamChannel` for temporary channels |
| Want lower disk/RAM usage or Deno fails on an older host (`glibc` mismatch) | Deno `v2.9.8` uses ~98 MB on disk and may require newer glibc on legacy Linux distros | Set `"js_runtime": "quickjs"` under `"extractor"` in `scriptfiles/SampAudioStream/config.json` (or `"sampaudiostream"` -> `"extractor"` in open.mp `config.json`) and restart to force `qjs` (~1.8 MB) without downloading Deno |

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
* `SA_ERR_AUTH_REQUIRED` (`10`): YouTube blocked the server IP (*"Sign in to confirm you're not a bot"*) or requires adult-account age verification; see [the YouTube Cookies Guide](youtube-cookies.md).

### Debug Logging (`logging.debug`)

When `logging.debug` is `true`, **SampAudioStream** emits detailed timing, subprocess execution, `yt-dlp -v` solver diagnostics, and HTTP A/B diagnostics. If an extractor media URL returns HTTP `400` or higher while `logging.debug` is enabled, the plugin also overwrites `scriptfiles/SampAudioStream/debug/last_rejected_media_request.txt` with the rejected request details (timestamp, full URL, non-secret request headers excluding `Cookie` and `Authorization`, HTTP status, `format_id`, and active JS runtime).

> [!CAUTION]
> `scriptfiles/SampAudioStream/debug/last_rejected_media_request.txt` contains a signed media URL tied to the server's public IP address and remains valid for several hours. **Do not share this file publicly.**

---

## 7. FAQ

* **Do players need to install anything?**
  **No.** Audio streams to the standard GTA:SA / SA-MP / open.mp client over HTTP as a normal MP3 audio stream.
* **Why does `"Audio stream: http://..."` appear in player chat, and how can it be hidden?**
  That message is printed by the **GTA:SA SA-MP/open.mp game client itself** whenever a player receives `PlayAudioStreamForPlayer`. Players can toggle it on or off anytime by typing **`/audiomsg`** in their in-game chat (or disable it in SA-MP settings); there is no server-side packet to hide it while still playing audio.
* **How much RAM and CPU does it consume?**
  Each active transcoded stream or channel uses one transcoder slot (within `max_concurrent_transcoders`), one ~30 s circular MP3 output ring buffer (`~480 KB` at `128 kbps`, minimum `256 KB`), and—for finite HTTP files—an input pull buffer bounded by **`performance.max_prefetch_mb`** (`8 MB` default, upper bound `max_prefetch_mb * active_finite_transcoders`). Local music library files use a fixed input buffer of at most `1 MB` instead. Buffers are freed immediately when playback ends, stops, or the owner disconnects.
* **Can I keep using `PlayAudioStreamForPlayer` without changing my gamemode?**
  Yes. Including `<SampAudioStream>` automatically hooks `PlayAudioStreamForPlayer` and `StopAudioStreamForPlayer`. Define `#define SA_DISABLE_NATIVE_HOOKS` before `#include <SampAudioStream>` if you prefer keeping vanilla `PlayAudioStreamForPlayer` untouched and calling `SA_*` natives explicitly.
* **How do I update SampAudioStream?**
  Replace `plugins/SampAudioStream.*` (or `components/SampAudioStream.*`) and `include/SampAudioStream.inc` from the new release zip, then recompile your Pawn scripts. `yt-dlp` checks for updates automatically in the background when `auto_update` is `true`. Saved channels in `scriptfiles/SampAudioStream/channels/` are kept across updates.
* **How do I uninstall SampAudioStream?**
  Remove `SampAudioStream.dll` / `.so` from `plugins/` or `components/`, remove its entry from `server.cfg` or `config.json`, remove `SampAudioStream.inc`, and delete `scriptfiles/SampAudioStream/` (which contains saved channels in `channels/`, the `yt-dlp` and QuickJS binaries in `bin/`, and cached extractor/runtime data).
* **Is it legal to play YouTube music on my server?**
  That depends on your country and on the content. The plugin only relays audio; see [Important Notes](../README.md#important-notes). Server owners are responsible for the content they broadcast.
