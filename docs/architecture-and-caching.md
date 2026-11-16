# Architecture, Transcoding & Caching

This document explains how **SampAudioStream** achieves high performance, zero manual host setup, live/video compatibility, and predictable memory usage.

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
       ├──► DirectAudio & volume == 100% & (!is_global || is_live)
       │        └── Fast Pass-Through -> Client receives original URL directly
       │
       └──► Proxy / Extractor Path (Worker Pool + Dedicated Transcoder Thread)
                ├── StreamResolver: direct HTTP probe or yt-dlp (+ quickjs-ng)
                ├── AudioTranscoder: HLS / MPEG-TS / WebM / Ogg / MP4 / WAV / FLAC / ADTS / MP3
                │                    -> LibMP3Lame (constant MP3 stream)
                ├── AudioStreamBuffer (2 MB circular ring buffer in RAM)
                └── HttpStreamingServer (Standalone Asio async TCP server)
                         └── Serves http://<public_ip>:<http_port>/stream/<token>
```

---

## 2. Zero Manual Server Setup vs. Self-Managed Extractor

Instead of requiring manual installation of external tools (`ffmpeg`, `python3`, `yt-dlp`, or `deno`/`node`) on your server OS:

* **Zero external C/C++ runtime or `ffmpeg` binary required**:
  * All audio decoders and encoders (`opus`, `ogg`, `fdk-aac`, `mp3lame_static`, `dr_mp3`, `dr_wav`, `dr_flac`, `stb_vorbis`) are **statically compiled into `sampaudiostream.dll` / `sampaudiostream.so`**.
  * On Windows, the plugin uses static MSVC CRT (`/MT`), depending only on OS system DLLs (`WS2_32.dll`, `WINHTTP.dll`, `KERNEL32.dll`, `MSWSOCK.dll`).
* **Self-managed `yt-dlp` + `quickjs-ng` (`qjs`)**:
  * To handle ever-changing platform signatures and throttling across YouTube and hundreds of other sites, [`BinaryManager`](file:///d:/SERVIDOR%20GTA%20MULTIPLAYER/Projetos/SampAudioStream/SampAudioStream/src/extractor/binary_manager.cpp) automatically downloads and unpacks `yt-dlp` and the tiny `quickjs-ng` (`qjs`) JS runtime into `scriptfiles/samp-audio-stream/bin/` asynchronously on first boot.
  * When `auto_update` is enabled in `config.json`, extractor updates happen cleanly in the background.

---

## 3. Supported Platforms, Formats & Live Streams

### Platforms
Any URL not detected as direct HTTP audio falls back to the extractor, supporting **YouTube** (videos, Shorts, YouTube Music, live streams), **SoundCloud**, **Twitch**, **Kick**, **Vimeo**, **Dailymotion**, **TikTok**, **Instagram**, **Facebook**, **X/Twitter**, **Reddit**, **Bandcamp**, **Mixcloud**, **Audiomack**, **Bilibili**, **Niconico**, **VK**, and hundreds of other public sites, in addition to **Shoutcast/Icecast** and direct `.mp3`/`.ogg`/`.wav`/`.flac`/`.aac`/`.m3u`/`.pls` URLs.

### Containers & Codecs Decoded Internally
* **Containers / Protocols:** HTTP(S) progressive streams, **HLS (`.m3u8`)** master & live media playlists, **MPEG-TS (`.ts`)**, WebM/Matroska (EBML), MP4/M4A/MOV (`moov`/`mdat` & fragmented `moof`/`traf`), Ogg, WAV, FLAC, ADTS, MP3 (ID3v2 + raw frames).
* **Audio Codecs:** Opus, Vorbis, AAC-LC / HE-AAC (`fdk-aac`), MP3, FLAC, PCM WAV (8/16/24/32-bit int & 32-bit float).
* **Output to GTA:SA Client:** Standard MP3 stream (`48000 Hz` / `128 kbps` by default) over HTTP/1.1 (`Content-Type: audio/mpeg`).

---

## 4. Caching & Memory Management

SampAudioStream **does not write downloaded MP3 music files to disk**. Everything stays cleanly in RAM with bounded lifetime and capacity:

1. **Metadata & Search Cache (`MetadataCache`)**:
   * Stores resolved track metadata + extracted direct media URLs (`3600s` / **1 hour** TTL by default) and `SA_SearchAudio` result lists (`900s` / **15 minutes** TTL by default, calculated as `max(60, cache_ttl_seconds / 4)`).
   * Eviction policy: **LRU** capped at `performance.cache_max_entries` (default `256` entries per bucket).
   * Repeated requests for the same URL or search query within TTL return without spawning `yt-dlp`.
2. **Active Stream Ring Buffer (`AudioStreamBuffer`)**:
   * Each active transcoded stream or channel allocates a **2 MB (`2 * 1024 * 1024` bytes) circular buffer** in RAM.
   * Old audio bytes beyond the 2 MB sliding window are overwritten in place.
   * When playback ends, the server allows up to ~3 seconds (`30 × 100 ms`) for HTTP clients to drain remaining buffered bytes, unregisters the endpoint, and immediately frees all associated memory.
   * Stopping a stream, disconnecting (`OnPlayerDisconnect`), or restarting the gamemode (GMX `Reset`) cancels and frees buffers immediately.
