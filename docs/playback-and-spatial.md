# Playback, Hooking & Spatial Audio

SampAudioStream supports both **per-player streams** and **world-wide/radius-based spatial streams**, with full compatibility for direct audio streams, live online radios, and extracted video/audio platforms.

---

## 1. Individual vs. Global Playback

| Feature | `SA_PlayAudioStreamForPlayer` | `SA_PlayAudioStream` |
| :--- | :--- | :--- |
| **Target Audience** | Single `playerid` | All matching players dynamically |
| **Radius Entry/Exit** | Starts/stops if owner leaves own filter | Any player entering the radius hears it automatically; leaving stops it |
| **Finite Track Time Sync** | Starts from beginning for owner | `1x` real-time pacing: latecomers hear the track at its current playback position |
| **Live Radio / Streams** | Direct pass-through (even for extensionless `http://` radios at `100.0` vol, with async ICY metadata probing; `OnAudioStreamFinished` does not fire on pass-through) or transcoded proxy | Direct real-time pass-through (HTTP radio at `100.0` vol) or live transcoded proxy (HLS / volume != `100.0`, with ICY `StreamTitle` framing when requested) |
| **Replaces Previous Audio** | Automatically stops player's prior personal stream or channel | Multiple global streams can run concurrently across the map |

* **LAN & Loopback Testing (`lan_aware_urls`)**: When enabled (default), local/LAN players automatically receive `127.0.0.1` or the server's matching private LAN IPv4 while remote players receive `public_ip`, without requiring manual config switching.
* **Playlist links:** a video link that also carries a playlist (`watch?v=...&list=...`) plays only that video. A playlist-only link (`youtube.com/playlist?list=...`) plays only its first entry; to play a whole playlist in sequence, use a synchronized channel with `SA_QueueChannelAudioStreamPlaylist`.

---

## 2. Global Spatial Streams & Time Synchronization

When using `SA_PlayAudioStream(url, posX, posY, posZ, distance, usepos, worldid, interiorid, volume)`:

1. Every `spatial_update_interval_ms` (default `200 ms`), the engine checks connected players via `MatchesSpatialFilter`:
   * `worldid == -1` or player's Virtual World matches `worldid`.
   * `interiorid == -1` or player's Interior matches `interiorid`.
   * If `usepos == true` and `distance > 0.0`, 3D distance $\le \text{distance}$.
2. **Entering the radius**: The player receives the stream URL and starts listening.
3. **Leaving the radius / changing world or interior**: The player receives `StopAudioStreamForPlayer` automatically.
4. **Finite songs vs. Live transmissions**:
   * **Finite audio/video tracks** (`!is_live` — YouTube videos, direct `.mp3`/`.ogg`/`.wav`/`.flac`/`.aac` files): the server paces transcoding at `1x` real-time and serves new listeners from the current live buffer offset. A player walking into a club 90 seconds into a song hears it around `01:30`, synchronized with everyone else inside.
   * **Live radios / live broadcasts** (`is_live == true` — Shoutcast/Icecast HTTP radio, `.pls`/`.m3u`, YouTube Live, Twitch, Kick): streamed directly in real time with no artificial pacing.

### Example: Nightclub 3D Zone Music

```pawn
new g_ClubStream = INVALID_AUDIO_STREAM_ID;

CMD:clubmusic(playerid, params[])
{
    if (isnull(params)) return SendClientMessage(playerid, -1, "Usage: /clubmusic <URL or song name>");

    if (g_ClubStream != INVALID_AUDIO_STREAM_ID)
    {
        SA_StopAudioStream(g_ClubStream);
    }

    new Float:x, Float:y, Float:z;
    GetPlayerPos(playerid, x, y, z);

    // Audible within 35.0m in the player's current world & interior at 85% volume
    g_ClubStream = SA_PlayAudioStream(
        params,
        x, y, z, 35.0, true,
        GetPlayerVirtualWorld(playerid),
        GetPlayerInterior(playerid),
        85.0
    );
    return 1;
}
```

---

## 3. Dynamic 3D Entity Attachment

You can attach any active stream (`streamid`) to follow a moving **player** or **vehicle**:

```pawn
// Boombox attached to the player who spawned it
new sid = SA_PlayAudioStream(url, 0.0, 0.0, 0.0, 25.0, true, -1, -1, 100.0);
SA_AttachAudioStreamToPlayer(sid, playerid);

// Car stereo attached to a vehicle
new vehid = GetPlayerVehicleID(playerid);
if (vehid > 0)
{
    new sid = SA_PlayAudioStream(url, 0.0, 0.0, 0.0, 30.0, true, -1, -1, 90.0);
    SA_AttachAudioStreamToVehicle(sid, vehid);
}
```

* When attached to a player (`SA_AttachAudioStreamToPlayer` or `SA_AttachAudioStreamChannelToPlayer`), playback tracks that player's position, Virtual World, and Interior every `spatial_update_interval_ms` tick interval (even if playback started with `usepos = false`).
* When attached to a vehicle (`SA_AttachAudioStreamToVehicle` or `SA_AttachAudioStreamChannelToVehicle`), playback tracks the vehicle's position every `spatial_update_interval_ms` tick interval so nearby players enter and leave hearing range dynamically as the vehicle moves.
* **Client-side 2D vs. 3D Attenuation:** Because the SA-MP client protocol fixes 3D sound coordinates at the moment `PlayAudioStreamForPlayer` is called and cannot move an active 3D sound afterward, **attached** sources always send `usepos = false` to the client — playing at **full volume (no distance attenuation) while inside server range and stopping cleanly outside range**. **Fixed-position** sources (`SA_SetAudioStreamPos`, `SA_SetAudioStreamChannelPos`, or `SA_PlayAudioStream` with `usepos = true`) continue sending `usepos = true` with client-side distance attenuation. Detaching or calling `SA_SetAudioStream*Pos` switches affected listeners back to `usepos = true` automatically.

---

## 4. Real-Time Volume, Distance, Position & Pause Control

* **Volume range**: `0.0` to `200.0` percent (`100.0` = unity gain, applied live on server-transcoded streams; passthrough `http://`/`https://` direct streams started at `100.0` cannot adjust volume afterward and return `0` — start with `volume != 100.0` or `.process_direct_audio_files = true` for direct files):
  * `SA_SetPlayerAudioStreamVolume(playerid, volume)`
  * `SA_SetAudioStreamVolume(streamid, volume)`
* **Dynamic Distance & Position**:
  * `SA_SetPlayerAudioStreamDistance(playerid, distance)` / `SA_SetAudioStreamDistance(streamid, distance)`
  * `SA_SetPlayerAudioStreamPos(playerid, posX, posY, posZ)` / `SA_SetAudioStreamPos(streamid, posX, posY, posZ)` (also automatically enables `usepos = true` and detaches from any player/vehicle)
  * Updates spatial filtering immediately across all listeners (stopping listeners outside the new radius, starting new ones inside it) and refreshes active clients cleanly. Transcoded streams reconnect seamlessly at the current MP3 frame boundary; native passthrough streams restart externally per SA-MP client behavior.
* **Pause / Resume**:
  * `SA_PauseAudioStreamForPlayer(playerid, bool:pause)`
  * When paused, server-transcoded streams resume seamlessly from their paused offset; passthrough `http://` streams stop on pause and restart externally on unpause.
* **Direct Audio Files (`process_direct_audio_files` parameter)**:
  * By default (`process_direct_audio_files = false`), `http://` and `https://` direct native audio links at `100.0` volume (`.mp3`, `.ogg`, `.wav`, `.flac`, `.aac`, `.pls`, `.m3u`, and probed Shoutcast/Icecast radios — excluding `.m3u8`/HLS and platform URLs) use zero-CPU native client passthrough — which means pause/resume, `0..200%` volume scaling, frame-accurate `SA_Set*Distance`, and `OnAudioStreamFinished` are unavailable for those sessions.
  * Passing `.process_direct_audio_files = true` in `SA_PlayAudioStreamForPlayer`, `SA_PlayAudioStream`, or `PlayAudioStreamForPlayer` intercepts direct music file links (`.mp3`, `.ogg`, `.wav`, `.flac`, `.aac`, `.m4a`, `.opus`) through `SampAudioStream`'s server pipeline for full pause/volume/distance/callback support, while live radio stations and `.pls`/`.m3u` playlists continue on passthrough.
