#pragma once

#include "sampaudiostream/audio/ring_buffer.hpp"
#include "sampaudiostream/core/config.hpp"
#include "sampaudiostream/core/thread_pool.hpp"
#include "sampaudiostream/core/types.hpp"
#include "sampaudiostream/extractor/binary_manager.hpp"
#include "sampaudiostream/extractor/metadata_cache.hpp"
#include "sampaudiostream/extractor/resolver.hpp"
#include "sampaudiostream/network/http_client.hpp"
#include "sampaudiostream/network/http_server.hpp"
#include "sampaudiostream/runtime/platform_host.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sampaudiostream {

struct StreamRunState {
    std::atomic<bool> cancel_flag{false};
    std::uint64_t start_offset_ms{0};
};

struct LiveTitlePollState {
    bool in_flight{false};
    bool stopped_permanently{false};
    std::int32_t consecutive_no_metadata{0};
    std::string last_received_title;
    std::chrono::steady_clock::time_point next_poll_at{};
};

struct ActiveStreamSession {
    StreamId id{INVALID_STREAM_ID};
    std::string token;
    std::string original_input;
    std::string original_input_pawn;
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
    std::shared_ptr<StreamRunState> active_run;
    std::uint64_t paused_offset{0};
    std::uint64_t resync_offset{0};
    std::uint64_t options_version{1};
    std::atomic<bool> cancel_flag{false};
    std::atomic<bool> pause_flag{false};
    std::atomic<bool> ready{false};
    std::atomic<float> volume_percent{100.0f};
    std::chrono::steady_clock::time_point started_at;
    std::chrono::steady_clock::time_point track_started_at{};
    std::chrono::steady_clock::time_point paused_since{};
    std::chrono::steady_clock::duration paused_accumulated{};
    std::chrono::seconds seek_base_offset{0};
    std::string station_name;
    std::string live_title;
    LiveTitlePollState live_poll{};
};

struct ChannelRunState {
    std::uint64_t generation{0};
    std::atomic<bool> cancel_flag{false};
    std::int32_t start_offset_seconds{0};
};

enum class AudioSourcePriority : std::uint8_t {
    None = 0,
    Personal = 1,
    Channel = 2,
    Global = 3,
    Sound = 4 // Short sound effect from audio.sound_dir: plays above every tier, then the previous source resumes
};

struct PlayerSoundState {
    std::uint64_t play_id{0};
    std::string token;
    std::string url;
    Vector3 position{};
    float distance{50.0f};
    bool use_position{false};
    std::int64_t duration_ms{0};
    bool fetched{false};
    std::chrono::steady_clock::time_point started_at{};
    std::chrono::steady_clock::time_point expires_at{};
};

struct CachedSoundFile {
    std::shared_ptr<const std::string> data;
    std::string content_type;
    std::int64_t duration_ms{0};
    std::uintmax_t size{0};
    std::filesystem::file_time_type mtime{};
    std::uint64_t last_use{0};
};

struct GlobalAudioSourceRef {
    bool is_channel{false};
    std::int32_t id{0};
    bool operator==(const GlobalAudioSourceRef& o) const {
        return is_channel == o.is_channel && id == o.id;
    }
};

struct PlayerArbitrationState {
    StreamId personal_stream_id{INVALID_STREAM_ID};
    ChannelId channel_id{INVALID_CHANNEL_ID};
    std::vector<GlobalAudioSourceRef> global_sources;
    bool muted{false};
    bool mute_include_personal{false};
    std::uint32_t muted_range_groups{0}; // bit N set: channels in group N are not heard through their range
    std::chrono::steady_clock::time_point personal_muted_since{};

    AudioSourcePriority client_source_kind{AudioSourcePriority::None};
    std::uint64_t client_source_id{0};
    std::uint64_t client_generation{0};
    std::uint64_t client_options_version{0};
    float client_distance{0.0f};
    std::string client_url;
};

struct PlayerAudioCommand {
    PlayerId player_id{INVALID_PLAYER_ID};
    bool play{false};
    std::string url;
    Vector3 position{};
    float distance{0.0f};
    bool use_position{false};
    bool source_changed{false};
    AudioStreamSourceType new_source_type{AudioStreamSourceType::None};
    std::int32_t new_source_id{-1};
    bool send{true}; // false: only the source changed, the client keeps the stream it already plays
};

struct ChannelQueueEntry {
    std::string utf8_url;
    std::string pawn_url;
    std::string utf8_label;
    std::string pawn_label;
    std::string utf8_description;
    std::string pawn_description;
    // Label the entry had before the first rename; restored when the label is set to "".
    bool custom_label{false};
    std::string utf8_original_label;
    std::string pawn_original_label;
};

struct SyncChannel {
    ChannelId id{INVALID_CHANNEL_ID};
    std::string name;
    std::string utf8_description;
    std::string pawn_description;
    std::string token;
    PlayerId creator_player_id{INVALID_PLAYER_ID};
    std::string creator_name;
    bool has_creator{false};
    bool creator_connected{false};
    std::string password_salt;
    std::string password_hash;
    ChannelDestroyMode destroy_mode{ChannelDestroyMode::Manual};
    std::int32_t destroy_timeout_seconds{0};
    std::chrono::steady_clock::time_point creator_disconnected_at{};
    ChannelLoopMode loop_mode{ChannelLoopMode::Off};
    bool playlist_mode{false};
    std::int32_t playlist_pos{-1};
    // Last list item that was playing when an out-of-list track started; the playlist continues after it.
    std::int32_t playlist_resume_pos{-1};
    bool current_from_queue{false};
    std::atomic<StreamState> state{StreamState::Idle};
    TrackMetadata current_track; // Main-thread owned
    ChannelQueueEntry current_input;
    std::deque<ChannelQueueEntry> queue;
    std::set<PlayerId> listeners;
    std::unordered_set<PlayerId> range_listeners;
    StreamOptions spatial{};
    bool auto_listen{true};
    std::int32_t range_group{0}; // 0 = no group; 1..31 can be muted per player for range listening
    std::uint64_t spatial_version{1};
    std::shared_ptr<AudioStreamBuffer> buffer; // Lazily allocated on first PlayChannel
    std::uint64_t next_generation{0};
    std::shared_ptr<ChannelRunState> active_run;
    std::atomic<bool> pause_flag{false};
    std::atomic<float> volume_percent{100.0f};
    std::chrono::steady_clock::time_point track_started_at{};
    std::chrono::steady_clock::time_point paused_since{};
    std::chrono::steady_clock::duration paused_accumulated{};
    std::chrono::seconds seek_base_offset{0};
    std::string station_name;
    std::string live_title;
    LiveTitlePollState live_poll{};
    std::int32_t consecutive_errors{0};
    bool playlist_import_in_flight{false};
    // Next track waiting to be pre-resolved close to the end of the current one (main thread decides when).
    std::string preresolve_pending_url;
    // Idle suspension: stop playback while nobody is joined and resume at the same point for the next listener.
    bool idle_suspend{false};
    std::int32_t idle_suspend_seconds{30};
    bool idle_suspended{false};
    std::int32_t resume_offset_seconds{0};
    std::chrono::steady_clock::time_point idle_since{};
    std::shared_ptr<std::atomic<bool>> playlist_cancel{std::make_shared<std::atomic<bool>>(false)};

    // Persistence & boot restore tracking
    bool persistent{true};
    bool restore_wanted{false};
    bool restore_retry_blocked{false};
    std::chrono::steady_clock::time_point restore_grace_deadline{};
    bool persist_dirty{false};
    bool persist_write_in_flight{false};
    bool persist_disk_error_logged{false};
    std::uint64_t persist_revision{0};
    std::chrono::steady_clock::time_point last_persist_flush{};
    std::chrono::steady_clock::time_point earliest_persist_due{};
    std::shared_ptr<std::atomic<bool>> persist_cancelled{std::make_shared<std::atomic<bool>>(false)};
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

struct PendingDelayedStreamStart {
    std::shared_ptr<ActiveStreamSession> session;
    TrackMetadata metadata;
    SourceType source_type{SourceType::DirectAudio};
    std::chrono::steady_clock::time_point release_at;
};

struct PendingDelayedChannelStart {
    std::shared_ptr<SyncChannel> channel;
    std::shared_ptr<ChannelRunState> run;
    TrackMetadata track;
    std::chrono::steady_clock::time_point release_at;
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

struct AudioEngineStats {
    std::int32_t active_streams{0};
    std::int32_t active_channels{0};
    std::int32_t active_transcoders{0};
    std::int32_t active_listeners{0};
};

class AudioEngine {
public:
    AudioEngine(IPlatformHost& host, const PluginConfig& config);
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    bool Initialize();
    void WaitForStartupReadinessAndLog(
        const std::function<void()>& emit_banner,
        bool flush_before_banner = false,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(1500));
    void Shutdown();
    void Reset(); // Called on GMX / mode restart
    void Tick();  // Called from main server tick

    // Player lifecycle
    void OnPlayerConnect(PlayerId player_id);
    void OnPlayerDisconnect(PlayerId player_id);
    void OnPlayerNameChanged(PlayerId player_id, const std::string& new_name);
    void NotifyDisconnectHookActive();
    void NotifyLifecycleStopping();
    void OnGameModeLoaded();
    bool IsLifecycleStopping() const { return lifecycle_stopping_.load(); }
    bool UsesComponentLifecycleEvents() const { return host_.UsesComponentLifecycleEvents(); }

    // Individual & Global Playback API
    StreamId PlayAudioStreamForPlayer(
        PlayerId player_id,
        const std::string& url_or_query,
        const StreamOptions& options,
        const std::string& original_input_pawn = "");
    StreamId PlayAudioStream(
        const std::string& url_or_query,
        const StreamOptions& options,
        const std::string& original_input_pawn = "");

    // Plays a file from audio.sound_dir as-is (no transcoding) above every other source, then resumes the previous one.
    bool PlayAudioStreamSoundForPlayer(
        PlayerId player_id,
        const std::string& sound,
        const Vector3& position = {},
        float distance = 50.0f,
        bool use_position = false);

    bool StopAudioStreamForPlayer(PlayerId player_id, bool send_stop = true);
    bool StopAudioStream(StreamId stream_id);

    bool PauseAudioStreamForPlayer(PlayerId player_id, bool pause);
    bool PauseAudioStream(StreamId stream_id, bool pause);
    bool SetPlayerAudioStreamMuted(PlayerId player_id, bool mute, bool include_personal = false);
    bool IsPlayerAudioStreamMuted(PlayerId player_id) const;
    // Range groups (1..31): a muted group stops the player from hearing those channels through their range;
    // channels the player joined are not affected.
    bool SetChannelRangeGroup(ChannelId channel_id, std::int32_t group);
    std::int32_t GetChannelRangeGroup(ChannelId channel_id) const;
    bool SetPlayerRangeGroupMuted(PlayerId player_id, std::int32_t group, bool mute);
    bool IsPlayerRangeGroupMuted(PlayerId player_id, std::int32_t group) const;
    bool GetPlayerAudioStreamSource(PlayerId player_id, AudioStreamSourceType& out_type, std::int32_t& out_source_id) const;
    bool SeekAudioStream(StreamId stream_id, std::int32_t seconds);
    bool SetPlayerAudioVolume(PlayerId player_id, float volume);
    bool SetStreamVolume(StreamId stream_id, float volume);
    bool SetPlayerAudioStreamDistance(PlayerId player_id, float distance);
    bool SetAudioStreamDistance(StreamId stream_id, float distance);
    bool SetPlayerAudioStreamPos(PlayerId player_id, float x, float y, float z);
    bool SetAudioStreamPos(StreamId stream_id, float x, float y, float z);
    void RegisterNativePassthroughSession(PlayerId player_id, const std::string& url, const StreamOptions& options);
    // While a sound plays for player_id, registers the native passthrough link without sending it, so it starts
    // when the sound ends. Returns false (doing nothing) when no sound is active.
    bool DeferNativePassthroughForSound(PlayerId player_id, const std::string& url, const StreamOptions& options);
    bool HasActivePlayerSound(PlayerId player_id) const;
    bool AttachStreamToPlayer(StreamId stream_id, PlayerId target_player_id);
    bool AttachStreamToVehicle(StreamId stream_id, std::int32_t vehicle_id);

    // Search API
    SearchId SearchAudio(PlayerId player_id, const std::string& query, std::uint32_t max_results);
    bool CancelSearch(PlayerId player_id);

    // Synchronized Channel API
    ChannelId CreateChannel(
        const std::string& name,
        PlayerId creator_player_id = INVALID_PLAYER_ID,
        const std::string& password = "",
        ChannelDestroyMode destroy_mode = ChannelDestroyMode::Manual,
        std::int32_t destroy_timeout_seconds = 0,
        bool persistent = true);
    bool DestroyChannel(ChannelId channel_id);
    bool PlayChannel(ChannelId channel_id, const std::string& url_or_query, const std::string& original_input_pawn = "");
    bool QueueChannel(
        ChannelId channel_id,
        const std::string& url_or_query,
        const std::string& original_input_pawn = "",
        const std::string& utf8_label = "");
    bool QueueChannelPlaylist(ChannelId channel_id, const std::string& url, std::int32_t max_items = 25);
    bool SeekChannelAudioStream(ChannelId channel_id, std::int32_t seconds);
    bool SkipChannel(ChannelId channel_id);
    bool StopChannel(ChannelId channel_id);
    bool PauseChannel(ChannelId channel_id, bool pause);
    bool SetChannelVolume(ChannelId channel_id, float volume);
    bool JoinChannel(PlayerId player_id, ChannelId channel_id, const std::string& password = "", bool ignore_password = false);
    bool LeaveChannel(PlayerId player_id);
    bool IsPlayerInChannel(PlayerId player_id, ChannelId channel_id) const;
    ChannelId GetPlayerChannelId(PlayerId player_id) const;
    bool SetChannelPassword(ChannelId channel_id, const std::string& password);
    bool VerifyChannelPassword(ChannelId channel_id, const std::string& password) const;
    bool SetChannelCreator(ChannelId channel_id, PlayerId creator_player_id);
    bool SetChannelDestroyPolicy(ChannelId channel_id, ChannelDestroyMode destroy_mode, std::int32_t timeout_seconds = 0);
    bool SetChannelIdleSuspend(ChannelId channel_id, bool enable, std::int32_t grace_seconds);
    bool GetChannelIdleSuspend(ChannelId channel_id, bool& out_enable, std::int32_t& out_grace_seconds, bool& out_suspended) const;
    bool ClearChannelQueue(ChannelId channel_id);
    bool SetChannelPos(
        ChannelId channel_id,
        float x,
        float y,
        float z,
        float distance = 50.0f,
        std::int32_t world_id = -1,
        std::int32_t interior_id = -1,
        bool auto_listen = true);
    bool ClearChannelPos(ChannelId channel_id);
    // distance > 0 sets the hearing range in the same update as the attachment (one client re-dispatch);
    // 0 keeps the current range. Non-finite distances are rejected.
    bool AttachChannelToPlayer(ChannelId channel_id, PlayerId target_player_id, float distance = 0.0f);
    bool AttachChannelToVehicle(ChannelId channel_id, std::int32_t vehicle_id, float distance = 0.0f);
    bool RemoveChannelQueueItem(ChannelId channel_id, std::size_t index);
    bool InsertChannelQueueItem(
        ChannelId channel_id,
        std::size_t index,
        const std::string& url_or_query,
        const std::string& original_input_pawn = "",
        const std::string& utf8_label = "");
    bool MoveChannelQueueItem(ChannelId channel_id, std::size_t from_index, std::size_t to_index);
    // Replaces the entry at index in place (new URL and label; the description is cleared unless keep_description);
    // the playing entry restarts with the new track.
    bool ReplaceChannelQueueItem(
        ChannelId channel_id,
        std::size_t index,
        const std::string& url_or_query,
        const std::string& original_input_pawn,
        const std::string& utf8_label,
        bool keep_description = false,
        bool keep_label = false);
    bool ShuffleChannelQueue(ChannelId channel_id);
    bool GetChannelLiveTitle(ChannelId channel_id, std::string& out_title) const;
    bool GetStreamLiveTitle(StreamId stream_id, std::string& out_title) const;
    bool SetChannelLiveTitle(ChannelId channel_id, const std::string& utf8_title);
    bool SetStreamLiveTitle(StreamId stream_id, const std::string& utf8_title);
    bool SetChannelLoopMode(ChannelId channel_id, ChannelLoopMode mode);
    ChannelLoopMode GetChannelLoopMode(ChannelId channel_id) const;
    bool SetChannelPlaylistMode(ChannelId channel_id, bool enable);
    bool IsChannelPlaylistMode(ChannelId channel_id) const;
    std::int32_t GetChannelQueuePos(ChannelId channel_id) const;
    bool PlayChannelQueueItem(ChannelId channel_id, std::int32_t index);
    std::int32_t RefreshAudioLibrary();
    std::int32_t GetAudioLibraryFolderCount() const;
    bool GetAudioLibraryFolder(std::int32_t index, std::string& out_pawn_name, std::int32_t& out_track_count) const;
    bool GetAudioLibraryTrack(std::int32_t folder_index, std::int32_t track_index, std::string& out_pawn_name) const;
    std::int32_t GetChannelElapsedSeconds(ChannelId channel_id) const;
    PlayerId GetChannelListenerAt(ChannelId channel_id, std::size_t index) const;
    bool SetChannelName(ChannelId channel_id, const std::string& utf8_name);
    std::size_t GetChannelCount() const;
    ChannelId GetChannelIdAt(std::size_t index) const;
    bool GetChannelInfo(ChannelId channel_id, ChannelInfo& out_info) const;
    bool GetChannelQueueItem(ChannelId channel_id, std::size_t index, std::string& out_item) const;
    bool GetChannelQueueItemLabel(ChannelId channel_id, std::size_t index, std::string& out_label, bool original = false) const;
    bool SetChannelQueueItemLabel(ChannelId channel_id, std::size_t index, const std::string& utf8_label);
    bool SetChannelDescription(ChannelId channel_id, const std::string& utf8_description);
    bool GetChannelDescription(ChannelId channel_id, std::string& out_description) const;
    bool SetChannelQueueItemDescription(ChannelId channel_id, std::size_t index, const std::string& utf8_description);
    bool GetChannelQueueItemDescription(ChannelId channel_id, std::size_t index, std::string& out_description) const;
    // Description of the track the channel is playing (it follows the queue entry it came from).
    bool GetChannelTrackDescription(ChannelId channel_id, std::string& out_description) const;
    // Index of the first queue entry at or after start_index whose Pawn-encoded URL equals pawn_item, or -1.
    std::int32_t FindChannelQueueItem(ChannelId channel_id, const std::string& pawn_item, std::int32_t start_index) const;

    // Diagnostics & Queries
    bool GetStreamInfo(StreamId stream_id, TrackMetadata& out_meta, StreamState& out_state) const;
    std::size_t GetStreamCount() const;
    StreamId GetStreamIdAt(std::size_t index) const;
    PlayerId GetStreamOwner(StreamId stream_id) const;
    std::int32_t GetStreamElapsedSeconds(StreamId stream_id) const;
    bool GetStreamUrl(StreamId stream_id, std::string& out_url) const;
    AudioEngineStats GetStats() const;
    StreamState GetChannelState(ChannelId channel_id) const;
    StreamId GetPlayerStreamId(PlayerId player_id) const;
    bool IsExtractorReady() const;
    std::uint16_t GetHttpPort() const;
    std::string GetPublicBaseUrl() const;
    const PluginConfig& GetConfig() const { return config_; }
    const std::string& GetPawnTextEncoding() const { return config_.pawn_text_encoding; }
    StreamResolver& ResolverForTesting() { return resolver_; }
    BinaryManager& BinariesForTesting() { return binary_manager_; }
    void SetConnectTimeoutWarningMsForTesting(std::int64_t timeout_ms) {
        connect_warn_timeout_ms_.store(timeout_ms, std::memory_order_relaxed);
    }
    void PollLiveTitlesAtForTesting(std::chrono::steady_clock::time_point now) {
        PollLiveTitles(now);
    }
    void FlushDirtyChannelsForTesting(bool sync_all = true, bool bypass_debounce = true) {
        FlushDirtyChannels(sync_all, bypass_debounce);
    }

    // LAN interface discovery & per-player host IP resolution (exposed for unit tests)
    static std::vector<LocalIpv4Interface> DiscoverLocalIpv4Interfaces(const std::string& bind_ip = "");
    static std::string ResolveEndpointHostIp(
        const std::string& player_ip,
        const std::string& public_host_ip,
        const std::vector<LocalIpv4Interface>& local_interfaces,
        bool lan_aware_urls,
        const std::string& bind_ip = "");

private:
    static std::string GenerateEndpointCode();
    std::string GenerateUniqueEndpointCodeLocked() const;
    std::string GetNextChannelTrackUrlLocked(const SyncChannel& ch) const;
    static std::int32_t NextPlaylistIndexAfterOutOfListLocked(const SyncChannel& ch);
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
        const std::string& original_input_pawn,
        std::shared_ptr<SyncChannel>& out_ch,
        std::shared_ptr<ChannelRunState>& out_run,
        const std::string& utf8_label = "");
    void HandleChannelTrackError(
        const std::shared_ptr<SyncChannel>& channel,
        const std::shared_ptr<ChannelRunState>& run,
        ErrorCode err_code,
        const std::string& err_msg);
    std::array<AudioSourcePriority, 3> ParsePriorityOrder() const;
    bool ShouldUseDirectPassthrough(const ActiveStreamSession& session) const;
    std::string BuildSessionPlayUrlLocked(const ActiveStreamSession& session, PlayerId player_id) const;
    bool MatchesSpatialFilterSnapshot(const PlayerSnapshot& snap, const StreamOptions& opts) const;
    void ReconcilePlayerLocked(PlayerId player_id, const PlayerSnapshot* snapshot, std::vector<PlayerAudioCommand>& out_cmds);
    void ApplyPlayerAudioCommands(const std::vector<PlayerAudioCommand>& cmds);
    void StartStreamWorker(std::shared_ptr<ActiveStreamSession> session);
    void StartStreamTranscoderWorker(
        std::shared_ptr<ActiveStreamSession> session,
        std::shared_ptr<StreamRunState> run,
        TrackMetadata local_meta,
        std::shared_ptr<AudioStreamBuffer> buf,
        std::uint64_t start_offset_ms);
    void StartChannelWorker(std::shared_ptr<SyncChannel> channel, std::shared_ptr<ChannelRunState> run, const std::string& url_or_query);
    void PreResolveNextChannelTrack(const std::shared_ptr<SyncChannel>& channel, const std::string& url_or_query);
    void DispatchPreResolveNextChannelTrack(const std::shared_ptr<SyncChannel>& channel, const std::string& url_or_query);
    // Entries added without a label (pasted links) get the real track title as soon as it is resolved.
    void FillQueueLabelsFromMetadata(ChannelId channel_id, const std::string& url_or_query);
    void ProcessPendingPreResolves(std::chrono::steady_clock::time_point now);
    void PollLiveTitles(std::chrono::steady_clock::time_point now);
    void LaunchSessionLiveTitlePoll(
        const std::shared_ptr<ActiveStreamSession>& session,
        const std::shared_ptr<StreamRunState>& run_snapshot,
        const std::string& url,
        const HttpClient::HeaderMap& headers);
    void LaunchChannelLiveTitlePoll(
        const std::shared_ptr<SyncChannel>& channel,
        const std::shared_ptr<ChannelRunState>& run_snapshot,
        const std::string& url,
        const HttpClient::HeaderMap& headers);
    void DispatchAudioCallback(const std::string& name, std::vector<CallbackArg> args);
    void DispatchProcessingNotification(PlayerId player_id, StreamId stream_id, const std::string& original_url, bool from_cache);
    bool RefreshActiveSessionSpatialLocked(
        const std::shared_ptr<ActiveStreamSession>& session,
        const std::vector<PlayerSnapshot>* snapshots,
        std::vector<PlayerAudioCommand>& out_cmds);
    void RefreshChannelSpatialLocked(
        const std::shared_ptr<SyncChannel>& channel,
        const std::vector<PlayerSnapshot>* snapshots,
        std::vector<PlayerAudioCommand>& out_cmds);
    bool IsRangeGroupMutedLocked(PlayerId player_id, std::int32_t group) const;
    void DetachChannelRangeListenersLocked(
        const std::shared_ptr<SyncChannel>& channel,
        std::vector<PlayerAudioCommand>& out_cmds);
    void AdvanceChannelOrEndLocked(
        const std::shared_ptr<SyncChannel>& channel,
        StreamState terminal_state,
        std::string& out_next_track,
        std::shared_ptr<ChannelRunState>& out_next_run,
        std::vector<PlayerAudioCommand>& out_cmds,
        bool& out_queue_ended);
    void UpdateSpatialStreams(const std::vector<PlayerSnapshot>* precomputed_snapshots = nullptr);
    bool LoadSoundFile(
        const std::string& sound,
        std::shared_ptr<const std::string>& out_data,
        std::string& out_content_type,
        std::int64_t& out_duration_ms,
        std::string& out_error);
    void UpdatePlayerSounds(std::chrono::steady_clock::time_point now);
    void RegisterNativePassthroughSessionImpl(
        PlayerId player_id,
        const std::string& url,
        const StreamOptions& options,
        bool deferred);
    bool IsPersonalAudioSilencedLocked(PlayerId player_id) const;
    void ExtendPendingPersonalCompletionLocked(PlayerId player_id, std::chrono::steady_clock::duration delay);
    void CleanupExpiredPassthroughSessions(std::chrono::steady_clock::time_point now);
    void CleanupExpiredChannels(std::chrono::steady_clock::time_point now);
    void CheckUnconnectedClients(std::chrono::steady_clock::time_point now);
    bool DestroyChannelInternal(ChannelId channel_id, ChannelDestroyReason reason);
    std::int32_t ComputeChannelRemainingTimeoutSecondsLocked(const SyncChannel& ch, std::chrono::steady_clock::time_point now) const;
    void ProcessPendingCompletions(std::chrono::steady_clock::time_point now);
    void ProcessDelayedStarts(std::chrono::steady_clock::time_point now);
    void CompleteResolvedStreamStart(
        const std::shared_ptr<ActiveStreamSession>& session,
        TrackMetadata local_meta,
        SourceType local_src,
        bool run_main_inline);
    void CompleteResolvedChannelTrackStart(
        const std::shared_ptr<SyncChannel>& channel,
        const std::shared_ptr<ChannelRunState>& run,
        TrackMetadata local_track);
    void DetachSessionFromArbitrationLocked(const std::shared_ptr<ActiveStreamSession>& session, std::vector<PlayerAudioCommand>& out_cmds);
    void CancelPendingCompletionLocked(StreamId stream_id);
    void CancelPendingChannelCompletionLocked(ChannelId channel_id);
    void CheckDisconnectedPlayers();
    void MatchCreatorLocked(PlayerId player_id, const std::string& player_name, std::vector<std::string>& out_debug_logs);
    void MatchCreator(PlayerId player_id, const std::string& player_name);
    void AddSortedChannelIdLocked(ChannelId id);
    void RemoveSortedChannelIdLocked(ChannelId id);
    void AddStreamSessionLocked(const std::shared_ptr<ActiveStreamSession>& session);
    std::shared_ptr<ActiveStreamSession> RemoveStreamSessionLocked(StreamId stream_id);
    void ClearStreamSessionsLocked();
    bool ReResolveExtractorTrack(
        const TrackMetadata& failed_meta,
        const std::atomic<bool>& cancel_flag,
        const std::shared_ptr<std::atomic<bool>>& alive,
        TrackMetadata& out_meta,
        std::string& out_error);
    static void DecodeClientSource(
        AudioSourcePriority kind,
        std::uint64_t raw_id,
        AudioStreamSourceType& out_type,
        std::int32_t& out_id);

    // Persistence helpers
    struct ChannelPersistSnapshot {
        ChannelId id{INVALID_CHANNEL_ID};
        std::string name;
        std::string utf8_description;
        std::string pawn_description;
        std::string password_salt;
        std::string password_hash;
        bool has_creator{false};
        std::string creator_name;
        ChannelDestroyMode destroy_mode{ChannelDestroyMode::Manual};
        std::int32_t destroy_timeout_seconds{0};
        float volume_percent{100.0f};
        bool paused{false};
        ChannelLoopMode loop_mode{ChannelLoopMode::Off};
        bool playlist_mode{false};
        std::int32_t playlist_pos{-1};
        bool current_from_queue{false};
        ChannelQueueEntry current_input;
        bool was_playing{false};
        std::vector<ChannelQueueEntry> queue;
        StreamOptions spatial{};
        bool auto_listen{true};
        std::int32_t range_group{0};
        bool idle_suspend{false};
        std::int32_t idle_suspend_seconds{30};
        std::int32_t resume_offset_seconds{0};
        bool persistent{true};
        std::uint64_t revision{0};
        std::shared_ptr<std::atomic<bool>> cancelled;
    };

    std::string GetChannelsDir() const;
    static ChannelPersistSnapshot CaptureChannelPersistSnapshotLocked(const SyncChannel& ch);
    void MarkChannelDirtyLocked(SyncChannel& ch);
    void FlushDirtyChannels(bool sync_all, bool bypass_debounce);
    bool WriteChannelPersistFile(const ChannelPersistSnapshot& snap, std::string& out_error) const;
    void DeleteChannelPersistFile(ChannelId id);
    void LoadPersistedChannelsOnInit();
    void ResumeRestoredChannelsInTick();
    void SuspendIdleChannelsInTick();
    void DispatchPendingRestoredCallbacks();

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
    std::mutex startup_log_mutex_;
    bool startup_wait_active_{false};
    bool public_ip_auto_detect_{false};
    bool startup_extractor_logged_{false};
    bool startup_public_ip_logged_{false};
    std::atomic<bool> initialized_{false};
    std::atomic<bool> shutting_down_{false};
    std::atomic<bool> lifecycle_stopping_{false};
    bool gamemode_loaded_{false};
    bool has_dirty_channels_{false};
    bool pending_restore_notifications_{false};
    std::vector<ChannelId> pending_restored_channel_ids_;
    std::mutex persist_io_mutex_;
    std::shared_ptr<std::atomic<bool>> alive_token_;
    std::atomic<StreamId> next_stream_id_{1};
    std::atomic<ChannelId> next_channel_id_{1};
    std::atomic<SearchId> next_search_id_{1};
    std::atomic<std::uint64_t> token_counter_{1};

    mutable std::mutex state_mutex_;
    std::unordered_map<StreamId, std::shared_ptr<ActiveStreamSession>> streams_;
    std::vector<StreamId> sorted_stream_ids_;
    std::unordered_map<PlayerId, StreamId> player_streams_;
    std::map<ChannelId, std::shared_ptr<SyncChannel>> channels_;
    std::vector<ChannelId> sorted_channel_ids_;
    std::unordered_map<PlayerId, ChannelId> player_channels_;
    std::unordered_map<PlayerId, PlayerArbitrationState> player_arbitration_;
    std::unordered_map<PlayerId, ActiveSearchState> active_searches_;
    std::vector<PendingStreamCompletion> pending_stream_completions_;
    std::vector<PendingChannelCompletion> pending_channel_completions_;
    struct PendingClientConnectCheck {
        std::string endpoint_token;
        AudioSourcePriority source_kind{AudioSourcePriority::None};
        std::uint64_t source_id{0};
        std::chrono::steady_clock::time_point delivered_at;
    };

    std::vector<PendingDelayedStreamStart> pending_delayed_stream_starts_;
    std::vector<PendingDelayedChannelStart> pending_delayed_channel_starts_;
    std::unordered_map<PlayerId, PendingClientConnectCheck> pending_connect_checks_;
    std::atomic<std::int64_t> connect_warn_timeout_ms_{15000};
    bool no_connect_warned_once_{false};
    std::chrono::steady_clock::time_point last_no_connect_warn_{};

    std::chrono::steady_clock::time_point last_spatial_update_;
    std::chrono::steady_clock::time_point last_disconnect_poll_;
    std::chrono::steady_clock::time_point last_idle_suspend_check_{};
    std::chrono::steady_clock::time_point last_preresolve_check_{};
    std::array<AudioSourcePriority, 3> priority_order_{
        AudioSourcePriority::Personal,
        AudioSourcePriority::Channel,
        AudioSourcePriority::Global
    };
    std::mt19937 queue_shuffle_rng_;
    std::atomic<std::int32_t> active_live_title_polls_{0};
    mutable std::mutex library_mutex_;
    std::vector<AudioLibraryFolderData> library_folders_;
    std::unordered_map<PlayerId, PlayerSoundState> player_sounds_; // Guarded by state_mutex_
    std::uint64_t next_sound_play_id_{1};
    std::mutex sound_cache_mutex_;
    std::unordered_map<std::string, CachedSoundFile> sound_cache_;
    std::uint64_t sound_cache_bytes_{0};
    std::uint64_t sound_cache_use_counter_{0};
};

enum class IcyPollResult {
    Title,       // Marker found and cleaned title is non-empty
    EmptyTitle,  // Marker found but cleaned title is empty
    NotLive,     // 2xx HTTP response with Content-Length > 0 (finite file)
    NoMetadata,  // Live response without StreamTitle within 64 KB
    Failed       // Network error, HTTP >= 400, deadline expired, or cancelled
};

bool ExtractIcyStreamTitle(const std::string& raw_bytes, std::string& out_title_utf8);
IcyPollResult FetchIcyStreamTitle(
    const std::string& url,
    const HttpClient::HeaderMap& headers,
    const std::atomic<bool>& cancel_flag,
    std::string& out_title_utf8,
    std::string& out_station_name_utf8);
bool IsLiveTitlePollCandidate(const TrackMetadata& meta, std::uint32_t live_title_poll_seconds = 30);

} // namespace sampaudiostream
