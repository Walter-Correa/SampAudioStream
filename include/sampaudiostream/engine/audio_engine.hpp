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

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
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
    TrackMetadata metadata;      // Main-thread owned after initial construction
    StreamOptions options;
    std::atomic<StreamState> state{StreamState::Resolving};
    SourceType source_type{SourceType::ExtractorStream}; // Main-thread owned

    bool is_global{false};
    bool owner_is_loopback{false};
    std::atomic<bool> is_passthrough{false};
    bool remote_url_error_reported{false};
    PlayerId owner_player_id{INVALID_PLAYER_ID};
    std::unordered_set<PlayerId> active_listeners;

    std::shared_ptr<AudioStreamBuffer> buffer;
    std::uint64_t paused_offset{0};
    std::atomic<bool> cancel_flag{false};
    std::atomic<bool> pause_flag{false};
    std::atomic<bool> ready{false};
    std::atomic<float> volume_percent{100.0f};
    std::chrono::steady_clock::time_point started_at;
};

struct ChannelRunState {
    std::uint64_t generation{0};
    std::atomic<bool> cancel_flag{false};
};

enum class AudioSourcePriority : std::uint8_t {
    None = 0,
    Personal = 1,
    Channel = 2,
    Global = 3
};

struct PlayerArbitrationState {
    StreamId personal_stream_id{INVALID_STREAM_ID};
    ChannelId channel_id{INVALID_CHANNEL_ID};
    std::vector<StreamId> global_streams;

    AudioSourcePriority client_source_kind{AudioSourcePriority::None};
    std::uint64_t client_source_id{0};
    std::uint64_t client_generation{0};
    std::string client_url;
};

struct PlayerAudioCommand {
    PlayerId player_id{INVALID_PLAYER_ID};
    bool play{false};
    std::string url;
    Vector3 position{};
    float distance{0.0f};
    bool use_position{false};
};

struct SyncChannel {
    ChannelId id{INVALID_CHANNEL_ID};
    std::string name;
    std::string token;
    std::atomic<StreamState> state{StreamState::Idle};
    TrackMetadata current_track; // Main-thread owned
    std::deque<std::string> queue;
    std::unordered_set<PlayerId> listeners;
    std::shared_ptr<AudioStreamBuffer> buffer; // Lazily allocated on first PlayChannel
    std::uint64_t next_generation{0};
    std::shared_ptr<ChannelRunState> active_run;
    std::atomic<bool> pause_flag{false};
    std::atomic<float> volume_percent{100.0f};
};

struct PendingStreamCompletion {
    std::shared_ptr<ActiveStreamSession> session;
    std::chrono::steady_clock::time_point min_deadline;
    std::chrono::steady_clock::time_point max_deadline;
};

struct PendingChannelCompletion {
    std::shared_ptr<SyncChannel> channel;
    std::shared_ptr<ChannelRunState> run;
    std::chrono::steady_clock::time_point min_deadline;
    std::chrono::steady_clock::time_point max_deadline;
};

struct ActiveSearchState {
    SearchId id{INVALID_SEARCH_ID};
    std::shared_ptr<std::atomic<bool>> cancel_flag;
};

struct LocalIpv4Interface {
    std::string ip;
    std::uint32_t addr_host{0};
    std::uint32_t mask_host{0};
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
    void NotifyDisconnectHookActive();

    // Individual & Global Playback API
    StreamId PlayAudioStreamForPlayer(PlayerId player_id, const std::string& url_or_query, const StreamOptions& options);
    StreamId PlayAudioStream(const std::string& url_or_query, const StreamOptions& options);

    bool StopAudioStreamForPlayer(PlayerId player_id, bool send_stop = true);
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
    StreamState GetChannelState(ChannelId channel_id) const;
    StreamId GetPlayerStreamId(PlayerId player_id) const;
    bool IsExtractorReady() const;
    std::uint16_t GetHttpPort() const;
    std::string GetPublicBaseUrl() const;
    const PluginConfig& GetConfig() const { return config_; }

    // LAN interface discovery & per-player host IP resolution (exposed for unit tests)
    static std::vector<LocalIpv4Interface> DiscoverLocalIpv4Interfaces(const std::string& bind_ip = "");
    static std::string ResolveEndpointHostIp(
        const std::string& player_ip,
        const std::string& public_host_ip,
        const std::vector<LocalIpv4Interface>& local_interfaces,
        bool lan_aware_urls,
        const std::string& bind_ip = "");

private:
    std::string GenerateToken();
    std::string BuildEndpointUrl(const std::string& prefix, const std::string& token) const;
    std::string BuildEndpointUrlLocked(const std::string& prefix, const std::string& token) const;
    std::string BuildEndpointUrlForPlayerLocked(const std::string& prefix, const std::string& token, PlayerId player_id) const;
    bool MatchesSpatialFilter(PlayerId player_id, const StreamOptions& opts) const;

    std::size_t CalculateRingBufferBytes() const;
    std::size_t EffectiveStreamConcurrencyLimit() const;
    std::size_t CountResourceStreamsLocked(StreamId exclude_id = INVALID_STREAM_ID, ChannelId exclude_channel_id = INVALID_CHANNEL_ID) const;
    std::size_t CurrentStreamLoadLocked(StreamId exclude_id = INVALID_STREAM_ID, ChannelId exclude_channel_id = INVALID_CHANNEL_ID) const;
    bool PlayChannelLocked(
        ChannelId channel_id,
        const std::string& url_or_query,
        std::shared_ptr<SyncChannel>& out_ch,
        std::shared_ptr<ChannelRunState>& out_run);
    void HandleChannelTrackError(
        const std::shared_ptr<SyncChannel>& channel,
        const std::shared_ptr<ChannelRunState>& run,
        ErrorCode err_code,
        const std::string& err_msg);
    std::array<AudioSourcePriority, 3> ParsePriorityOrder() const;
    std::string BuildSessionPlayUrlLocked(const ActiveStreamSession& session, PlayerId player_id) const;
    bool MatchesSpatialFilterSnapshot(const PlayerSnapshot& snap, const StreamOptions& opts) const;
    void ReconcilePlayerLocked(PlayerId player_id, const PlayerSnapshot* snapshot, std::vector<PlayerAudioCommand>& out_cmds);
    void ApplyPlayerAudioCommands(const std::vector<PlayerAudioCommand>& cmds);
    void StartStreamWorker(std::shared_ptr<ActiveStreamSession> session);
    void StartChannelWorker(std::shared_ptr<SyncChannel> channel, std::shared_ptr<ChannelRunState> run, const std::string& url_or_query);
    void PreResolveNextChannelTrack(const std::shared_ptr<SyncChannel>& channel, const std::string& url_or_query);
    void ProbeIcyTitleAsync(const std::shared_ptr<ActiveStreamSession>& session);
    void AdvanceChannelOrEndLocked(
        const std::shared_ptr<SyncChannel>& channel,
        StreamState terminal_state,
        std::string& out_next_track,
        std::shared_ptr<ChannelRunState>& out_next_run,
        std::vector<PlayerAudioCommand>& out_cmds);
    void UpdateSpatialStreams(const std::vector<PlayerSnapshot>* precomputed_snapshots = nullptr);
    void CleanupExpiredPassthroughSessions(std::chrono::steady_clock::time_point now);
    void ProcessPendingCompletions(std::chrono::steady_clock::time_point now);
    void DetachSessionFromArbitrationLocked(const std::shared_ptr<ActiveStreamSession>& session, std::vector<PlayerAudioCommand>& out_cmds);
    void CancelPendingCompletionLocked(StreamId stream_id);
    void CancelPendingChannelCompletionLocked(ChannelId channel_id);
    void CheckDisconnectedPlayers();

    IPlatformHost& host_;
    PluginConfig config_;

    ThreadPool control_pool_;
    ThreadPool extraction_pool_;
    TranscodeScheduler transcode_scheduler_;
    BinaryManager binary_manager_;
    MetadataCache metadata_cache_;
    StreamResolver resolver_;
    HttpStreamingServer http_server_;

    std::vector<LocalIpv4Interface> local_interfaces_;
    std::string public_host_ip_;
    std::mutex public_ip_mutex_;
    bool public_ip_resolved_{false};
    std::atomic<bool> public_ip_failed_{false};
    std::atomic<bool> public_ip_error_logged_{false};
    std::condition_variable public_ip_cv_;
    std::atomic<bool> initialized_{false};
    std::atomic<bool> shutting_down_{false};
    std::shared_ptr<std::atomic<bool>> alive_token_;
    std::atomic<StreamId> next_stream_id_{1};
    std::atomic<ChannelId> next_channel_id_{1};
    std::atomic<SearchId> next_search_id_{1};
    std::atomic<std::uint64_t> token_counter_{1};

    mutable std::mutex state_mutex_;
    std::unordered_map<StreamId, std::shared_ptr<ActiveStreamSession>> streams_;
    std::unordered_map<PlayerId, StreamId> player_streams_;
    std::unordered_map<ChannelId, std::shared_ptr<SyncChannel>> channels_;
    std::unordered_map<PlayerId, ChannelId> player_channels_;
    std::unordered_map<PlayerId, PlayerArbitrationState> player_arbitration_;
    std::unordered_map<PlayerId, ActiveSearchState> active_searches_;
    std::vector<PendingStreamCompletion> pending_stream_completions_;
    std::vector<PendingChannelCompletion> pending_channel_completions_;

    std::chrono::steady_clock::time_point last_spatial_update_;
    std::chrono::steady_clock::time_point last_disconnect_poll_;
    std::array<AudioSourcePriority, 3> priority_order_{
        AudioSourcePriority::Personal,
        AudioSourcePriority::Channel,
        AudioSourcePriority::Global
    };
};

} // namespace sampaudiostream
