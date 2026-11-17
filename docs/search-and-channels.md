# Music Search & Synchronized Radio Channels

SampAudioStream lets players search for music by name directly in-game and enables server-wide synchronized radio stations with shared queues.

---

## 1. Asynchronous Music Search

Calling `SA_SearchAudioStream(playerid, query, max_results = 5)` dispatches a background search on the worker thread pool without stalling the SA-MP/open.mp server tick.

### Callback Sequence
1. `OnAudioStreamSearchResultItem(playerid, search_id, item_index, const title[], const artist[], duration, const video_id[])` is fired once for each result (`item_index` from `0` to `total_found - 1`).
2. `OnAudioStreamSearchResults(playerid, search_id, total_found)` is fired right after all items have been delivered (`total_found == 0` if no matches were found or if the search failed).

### Complete Interactive Dialog Example

```pawn
#define DIALOG_MUSIC_SEARCH (8801)
#define MAX_SEARCH_CACHE    (10)

enum E_SEARCH_ENTRY
{
    bool:seUsed,
    seTitle[128],
    seArtist[64],
    seDuration,
    seUrl[192]
};

new g_SearchCache[MAX_PLAYERS][MAX_SEARCH_CACHE][E_SEARCH_ENTRY];
new g_ActiveSearchID[MAX_PLAYERS] = { INVALID_AUDIO_SEARCH_ID, ... };

CMD:music(playerid, params[])
{
    if (isnull(params)) return SendClientMessage(playerid, -1, "Usage: /music <song name or URL>");

    if (strcmp(params, "http://", true, 7) == 0 || strcmp(params, "https://", true, 8) == 0)
    {
        SA_PlayAudioStreamForPlayer(playerid, params);
        return 1;
    }

    for (new i = 0; i < MAX_SEARCH_CACHE; i++) g_SearchCache[playerid][i][seUsed] = false;
    g_ActiveSearchID[playerid] = SA_SearchAudioStream(playerid, params, 8);
    SendClientMessage(playerid, 0x3498DBAA, "[Music] Searching...");
    return 1;
}

public OnAudioStreamSearchResultItem(playerid, search_id, item_index, const title[], const artist[], duration, const video_id[])
{
    if (search_id != g_ActiveSearchID[playerid] || item_index < 0 || item_index >= MAX_SEARCH_CACHE)
        return 1;

    g_SearchCache[playerid][item_index][seUsed] = true;
    format(g_SearchCache[playerid][item_index][seTitle], 128, "%s", title);
    format(g_SearchCache[playerid][item_index][seArtist], 64, "%s", artist);
    g_SearchCache[playerid][item_index][seDuration] = duration;
    format(g_SearchCache[playerid][item_index][seUrl], 192, "https://www.youtube.com/watch?v=%s", video_id);
    return 1;
}

public OnAudioStreamSearchResults(playerid, search_id, total_found)
{
    if (search_id != g_ActiveSearchID[playerid]) return 1;
    if (total_found <= 0) return SendClientMessage(playerid, 0xE74C3CAA, "[Music] No results found.");

    new body[1536], row[220];
    body = "Title\tArtist\tDuration\n";

    for (new i = 0; i < total_found && i < MAX_SEARCH_CACHE; i++)
    {
        if (!g_SearchCache[playerid][i][seUsed]) continue;
        new s = g_SearchCache[playerid][i][seDuration];
        format(row, sizeof(row), "%s\t%s\t%02d:%02d\n", g_SearchCache[playerid][i][seTitle], g_SearchCache[playerid][i][seArtist], s / 60, s % 60);
        strcat(body, row, sizeof(body));
    }

    ShowPlayerDialog(playerid, DIALOG_MUSIC_SEARCH, DIALOG_STYLE_TABLIST_HEADERS, "Music Search Results", body, "Play", "Cancel");
    return 1;
}

public OnDialogResponse(playerid, dialogid, response, listitem, inputtext[])
{
    if (dialogid == DIALOG_MUSIC_SEARCH && response)
    {
        if (listitem >= 0 && listitem < MAX_SEARCH_CACHE && g_SearchCache[playerid][listitem][seUsed])
        {
            SA_PlayAudioStreamForPlayer(playerid, g_SearchCache[playerid][listitem][seUrl]);
        }
        return 1;
    }
    return 0;
}
```

> [!TIP]
> You can also pass a plain song name directly to `SA_PlayAudioStreamForPlayer(playerid, "bohemian rhapsody")` or `SA_PlayAudioStream(...)` — the resolver automatically resolves `ytsearch1:<query>` and starts the top match immediately!

---

## 2. Synchronized Radio Channels

Channels allow you to create shared radio stations where all joined listeners hear the exact same audio stream synchronized in real time (`1x` live pacing).

### Lifecycle & Queueing

```pawn
new g_RadioChannel = INVALID_AUDIO_CHANNEL_ID;

public OnGameModeInit()
{
    g_RadioChannel = SA_CreateChannel("Server FM");
    SA_PlayChannel(g_RadioChannel, "https://www.youtube.com/watch?v=jfKfPfyJRdk");
    SA_QueueChannel(g_RadioChannel, "https://soundcloud.com/artist/track");
    return 1;
}

CMD:radio(playerid, params[])
{
    if (SA_IsPlayerInChannel(playerid, g_RadioChannel))
    {
        SA_LeaveChannel(playerid);
        SendClientMessage(playerid, -1, "[Radio] Left Server FM.");
    }
    else
    {
        SA_JoinChannel(playerid, g_RadioChannel);
        SendClientMessage(playerid, -1, "[Radio] Tuned into Server FM.");
    }
    return 1;
}
```

### Channel Behavior
* `SA_PlayChannel`: Clears/replaces current playback on the channel and immediately starts the given URL or search query.
* `SA_QueueChannel`: Appends a track to the channel's queue. When the active track finishes draining its lead buffer, listeners cleanly transition to the next track (with a brief audible gap while clients reconnect to the new track).
* `SA_SkipChannel`: Aborts the active track and advances immediately to the next queued track.
* `SA_JoinChannel` / `SA_LeaveChannel`: Joining a channel stops the player's individual stream or previous channel automatically. Calling `StopAudioStreamForPlayer` / `SA_StopAudioStreamForPlayer` also leaves any active channel.
