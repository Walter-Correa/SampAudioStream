#pragma once

#include "sampaudiostream/audio/ring_buffer.hpp"
#include "sampaudiostream/core/config.hpp"
#include "sampaudiostream/core/thread_pool.hpp"
#include "sampaudiostream/core/types.hpp"
#include "sampaudiostream/extractor/binary_manager.hpp"
#include "sampaudiostream/extractor/metadata_cache.hpp"
#include "sampaudiostream/extractor/resolver.hpp"
#include "sampaudiostream/network/http_server.hpp"
#include "sampaudiostream/runtime/platform_host.hpp"

#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sampaudiostream {

struct ActiveStreamSession {
    StreamId id{INVALID_STREAM_ID};
    std::string token;
    std::string original_input;
    TrackMetadata metadata;
    StreamOptions options;
    StreamState state{StreamState::Resolving};
    SourceType source_type{SourceType::ExtractorStream};

    bool is_global{false};
    PlayerId owner_player_id{INVALID_PLAYER_ID};
    std::unordered_set<PlayerId> active_listeners;

    std::shared_ptr<AudioStreamBuffer> buffer;
    std::atomic<bool> cancel_flag{false};
    std::atomic<bool> pause_flag{false};
    std::atomic<float> volume_percent{100.0f};
    std::chrono::steady_clock::time_point started_at;
};

struct SyncChannel {
    ChannelId id{INVALID_CHANNEL_ID};
    std::string name;
    std::string token;
    StreamState state{StreamState::Idle};
    TrackMetadata current_track;
    std::deque<std::string> queue;
    std::unordered_set<PlayerId> listeners;
    std::shared_ptr<AudioStreamBuffer> buffer;
    std::atomic<bool> cancel_flag{false};
    std::atomic<bool> pause_flag{false};
    std::atomic<float> volume_percent{100.0f};
};

class AudioEngine {
public:
    AudioEngine(IPlatformHost& host, const PluginConfig& config);
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    bool Initialize();
    void Shutdown();
    void Reset(); // Called on GMX / mode restart
    void Tick();  // Called from main server tick

    // Player lifecycle
    void OnPlayerDisconnect(PlayerId player_id);

    // Individual & Global Playback API
    StreamId PlayAudioStreamForPlayer(PlayerId player_id, const std::string& url_or_query, const StreamOptions& options);
    StreamId PlayAudioStream(const std::string& url_or_query, const StreamOptions& options);

    bool StopAudioStreamForPlayer(PlayerId player_id);
    bool StopAudioStream(StreamId stream_id);

    bool PauseAudioStreamForPlayer(PlayerId player_id, bool pause);
    bool SetPlayerAudioVolume(PlayerId player_id, float volume);
    bool SetStreamVolume(StreamId stream_id, float volume);
    bool AttachStreamToPlayer(StreamId stream_id, PlayerId target_player_id);
    bool AttachStreamToVehicle(StreamId stream_id, std::int32_t vehicle_id);

    // Search API
    SearchId SearchAudio(PlayerId player_id, const std::string& query, std::uint32_t max_results);
    bool CancelSearch(PlayerId player_id);

    // Synchronized Channel API
    ChannelId CreateChannel(const std::string& name);
    bool DestroyChannel(ChannelId channel_id);
    bool PlayChannel(ChannelId channel_id, const std::string& url_or_query);
    bool QueueChannel(ChannelId channel_id, const std::string& url_or_query);
    bool SkipChannel(ChannelId channel_id);
    bool StopChannel(ChannelId channel_id);
    bool PauseChannel(ChannelId channel_id, bool pause);
    bool SetChannelVolume(ChannelId channel_id, float volume);
    bool JoinChannel(PlayerId player_id, ChannelId channel_id);
    bool LeaveChannel(PlayerId player_id);
    bool IsPlayerInChannel(PlayerId player_id, ChannelId channel_id) const;
    ChannelId GetPlayerChannelId(PlayerId player_id) const;

    // Diagnostics & Queries
    bool GetStreamInfo(StreamId stream_id, TrackMetadata& out_meta, StreamState& out_state) const;
    StreamId GetPlayerStreamId(PlayerId player_id) const;
    bool IsExtractorReady() const;
    std::uint16_t GetHttpPort() const;
    std::string GetPublicBaseUrl() const;
    const PluginConfig& GetConfig() const { return config_; }

private:
    std::string GenerateToken();
    std::string BuildEndpointUrl(const std::string& prefix, const std::string& token) const;
    bool MatchesSpatialFilter(PlayerId player_id, const StreamOptions& opts) const;

    void StartStreamWorker(std::shared_ptr<ActiveStreamSession> session);
    void StartChannelWorker(std::shared_ptr<SyncChannel> channel, const std::string& url_or_query);
    void UpdateSpatialStreams();

    IPlatformHost& host_;
    PluginConfig config_;

    ThreadPool pool_;
    BinaryManager binary_manager_;
    MetadataCache metadata_cache_;
    StreamResolver resolver_;
    HttpStreamingServer http_server_;

    std::string public_host_ip_;
    std::atomic<bool> initialized_{false};
    std::atomic<StreamId> next_stream_id_{1};
    std::atomic<ChannelId> next_channel_id_{1};
    std::atomic<SearchId> next_search_id_{1};
    std::atomic<std::uint64_t> token_counter_{1};

    mutable std::mutex state_mutex_;
    std::unordered_map<StreamId, std::shared_ptr<ActiveStreamSession>> streams_;
    std::unordered_map<PlayerId, StreamId> player_streams_;
    std::unordered_map<ChannelId, std::shared_ptr<SyncChannel>> channels_;
    std::unordered_map<PlayerId, ChannelId> player_channels_;
    std::unordered_map<PlayerId, SearchId> active_searches_;

    std::chrono::steady_clock::time_point last_spatial_update_;
};

} // namespace sampaudiostream
