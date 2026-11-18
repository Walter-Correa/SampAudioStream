# Architecture, Transcoding & Caching

This document explains how **SampAudioStream** achieves high performance, minimal host dependencies (none on Windows, 32-bit `libcurl` on Linux), live/video compatibility, and predictable memory usage.

---

## 1. High-Level Pipeline

```text
  Pawn Script (SA_* / hooked PlayAudioStreamForPlayer)
       │
       ▼
  AudioEngine (Main-thread non-blocking API)
       │
       ├──► MetadataCache (In-memory LRU + TTL)
       │        ├── Track cache (default: 256 entries, 3600s TTL)
       │        └── Search cache (default: 256 entries, max(60, TTL/4) = 900s)
       │
       ├──► DirectAudio (HTTP/HTTPS, non-.m3u8) & volume == 100% & (!is_global || is_live) & !(process_direct_audio_files && !is_live && file_ext)
       │        └── Fast Pass-Through -> Client receives original URL directly (+ async ICY title probe)
       │
       └──► Proxy / Extractor Path (Worker Pool + Dedicated Transcoder Thread)
                ├── StreamResolver: direct http:// probe shortcut OR yt-dlp (--cache-dir + deno / quickjs-ng)
                ├── AudioTranscoder: HLS (TS / ID3-ADTS / fMP4) / WebM / Ogg / MP4 / WAV / FLAC / ADTS / MP3
                │                    -> LibMP3Lame (constant MP3 stream)
                ├── AudioStreamBuffer (~30s circular ring buffer in RAM; ~480 KB at 128 kbps, min 256 KB)
                └── HttpStreamingServer (Standalone Asio async TCP server, optional Icy-MetaData framing)
                         └── Serves http://<host_for_player>:<http_port>/stream/<token>
```

---

## 2. Minimal Host Dependencies & Self-Managed Extractor

Instead of requiring manual installation of `ffmpeg`, `python3`, `yt-dlp`, or `deno`/`node` on your server OS (only 32-bit `libcurl.so.4` is required on Linux hosts; Windows requires nothing), **SampAudioStream** automatically downloads and SHA-256-verifies its own pinned `deno` runtime in the background when supported by the host platform while using `quickjs-ng` immediately on startup and as fallback:

* **Zero external C/C++ runtime or `ffmpeg` binary required**:
  * All audio decoders and encoders (`opus`, `ogg`, `fdk-aac`, `mp3lame_static`, `dr_mp3`, `dr_wav`, `dr_flac`, `stb_vorbis`) are **statically compiled into `SampAudioStream.dll` / `SampAudioStream.so`**.
  * On Windows, the plugin uses static MSVC CRT (`/MT`), depending only on OS system DLLs (`WS2_32.dll`, `WINHTTP.dll`, `KERNEL32.dll`, `IPHLPAPI.dll`).
* **Self-managed `yt-dlp` (*onedir*) + `deno` (`v2.9.8`) + `quickjs-ng` (`qjs`)**:
  * To handle ever-changing platform signatures and throttling across YouTube and hundreds of other sites, [`BinaryManager`](../src/extractor/binary_manager.cpp) automatically downloads and SHA-256 verifies `yt-dlp` (preferring official *onedir* `.zip` archives into `yt-dlp-onedir/` for ~1s faster cold-starts, falling back to single-file binaries if needed) and `quickjs-ng` (`qjs`) first into `scriptfiles/SampAudioStream/bin/`, then asynchronously downloads and health-checks pinned **Deno `v2.9.8`** into `deno/` (`deno-cache/`) on supported 64-bit hosts when `extractor.js_runtime` is `"auto"` or `"deno"`. Persistent `--cache-dir` state lives in `yt-dlp-cache/` (the verified `youtube-sigfuncs/` subdirectory is automatically pruned by file age via `extractor.yt_dlp_cache_max_age_days` — `7` days default, `0` to disable — with an internal `64 MB` size cap protecting files newer than `1 hour`, executed once at startup and asynchronously during maintenance ticks when no `yt-dlp` subprocess is active; `deno/`, `deno-cache/`, and `challenge-solver/` JS scripts remain untouched).
  * **Automatic direct HTTP/HTTPS passthrough & probe:** Direct `http://` URLs and native-extension `https://` URLs (`.mp3`/`.ogg`/`.wav`/`.flac`/`.aac`/`.pls`/`.m3u`, excluding `.m3u8` and platform hosts) at `100.0%` volume start in passthrough without launching `yt-dlp`. Extensionless `https://` URLs run a lightweight HTTP/ICY/HLS probe (`ProbeDirectHttpAudio`): if an `audio/*` radio is detected, it routes directly in passthrough and caches that classification; HTML pages and `.m3u8` proceed to `yt-dlp` / HLS transcoding. Passing `.process_direct_audio_files = true` on `SA_PlayAudioStreamForPlayer` / `SA_PlayAudioStream` intentionally bypasses passthrough for direct music files so the transcoder manages playback, pause/resume, volume, distance updates, and `OnAudioStreamFinished`.
  * When `extractor.auto_update` is enabled in `config.json`, extractor updates happen cleanly in the background.

---

## 3. Supported Platforms, Formats & Live Streams

### Platforms
Any URL not detected as direct HTTP audio falls back to the extractor, supporting **YouTube** (videos, Shorts, YouTube Music, live streams), **SoundCloud**, **Twitch**, **Kick**, **Vimeo**, **Dailymotion**, **TikTok**, **Instagram**, **Facebook**, **X/Twitter**, **Reddit**, **Bandcamp**, **Mixcloud**, **Audiomack**, **Bilibili**, **Niconico**, **VK**, and hundreds of other public sites, in addition to **Shoutcast/Icecast** and direct `.mp3`/`.ogg`/`.wav`/`.flac`/`.aac`/`.m3u`/`.pls` URLs.

### Containers & Codecs Decoded Internally
* **Containers / Protocols:** HTTP(S) progressive streams, **HLS (`.m3u8`)** master & live media playlists (MPEG-TS `.ts`, RFC 8216 ID3v2-prefixed ADTS packed audio, and fMP4 `#EXT-X-MAP` + `moof`/`mdat`), WebM/Matroska (EBML), MP4/M4A/MOV (`moov`/`mdat` & fragmented `moof`/`traf`), Ogg, non-seekable WAV/FLAC/raw MP3, and ADTS AAC.
* **Audio Codecs:** Opus, Vorbis, AAC-LC / HE-AAC (`fdk-aac`), MP3, FLAC, PCM WAV (8/16/24/32-bit int & 32-bit float).
* **Output to GTA:SA Client:** Standard MP3 stream (`48000 Hz` / `128 kbps` by default) over HTTP/1.1 (`Content-Type: audio/mpeg`), with optional `Icy-MetaData: 1` (`icy-metaint: 16000`) live `StreamTitle` framing for clients that request Shoutcast metadata.

---

## 4. Caching & Memory Management

SampAudioStream **does not write downloaded MP3 music files to disk**. Everything stays cleanly in RAM with bounded lifetime and capacity:

1. **Metadata & Search Cache (`MetadataCache`)**:
   * Stores resolved track metadata + extracted direct media URLs (`3600s` / **1 hour** TTL by default) and `SA_SearchAudioStream` result lists (`900s` / **15 minutes** TTL by default, calculated as `max(60, cache_ttl_seconds / 4)`).
   * Eviction policy: **LRU** capped at `performance.cache_max_entries` (default `256` entries per bucket).
   * Repeated requests for the same URL or search query within TTL return without spawning `yt-dlp`.
2. **Pull Prefetch Buffer (`performance.max_prefetch_mb`) & MP3 Output Ring Buffer (`AudioStreamBuffer`)**:
   * Finite HTTP downloads expand their input pull buffer up to `min(Content-Length, max_prefetch_mb * 1 MB)` (`16 MB` default, clamped `1..64 MB`), allowing upstream CDNs to finish fast downloads without TCP idle drops while bound-capping total prefetch RAM to **`max_prefetch_mb * active_finite_transcoders`**.
   * Each active transcoded stream or channel also allocates a circular output MP3 ring buffer sized for **~30 seconds** (`~480 KB` at `128 kbps`, minimum `256 KB`) in RAM (`CalculateRingBufferBytes`).
   * A zero-decode watchdog aborts hung or non-audio streams cleanly if zero PCM frames are decoded within `20 seconds`.
   * Stopping a stream, disconnecting (`OnPlayerDisconnect`), or restarting the gamemode (GMX `Reset`) cancels and frees buffers immediately.
