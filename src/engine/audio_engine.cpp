#include "sampaudiostream/engine/audio_engine.hpp"

#include "sampaudiostream/audio/transcoder.hpp"
#include "sampaudiostream/core/logger.hpp"
#include "sampaudiostream/network/http_client.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <bcrypt.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace sampaudiostream {

namespace {

float DistanceSq(const Vector3& a, const Vector3& b) {
    float dx = a.x - b.x;
    float dy = a.y - b.y;
    float dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

std::pair<std::chrono::steady_clock::time_point, std::chrono::steady_clock::time_point>
MakeCompletionDeadlines(std::chrono::steady_clock::time_point anchor, std::uint32_t lead_ms) {
    return {
        anchor + std::chrono::milliseconds(lead_ms + 2000),
        anchor + std::chrono::milliseconds(std::min<std::uint32_t>(lead_ms + 10000, 30000))
    };
}

bool StartsWithHttpPlain(const std::string& url) {
    if (url.size() < 7) return false;
    auto eq = [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == b;
    };
    return eq(url[0], 'h') && eq(url[1], 't') && eq(url[2], 't') &&
           eq(url[3], 'p') && url[4] == ':' && url[5] == '/' && url[6] == '/';
}

bool IsLoopbackIp(const std::string& ip) {
    if (ip == "::1" || ip == "localhost") return true;
    struct in_addr a{};
    if (::inet_pton(AF_INET, ip.c_str(), &a) == 1) {
        std::uint32_t h = ntohl(a.s_addr);
        return (h & 0xFF000000u) == 0x7F000000u;
    }
    return ip == "127.0.0.1";
}

bool ParseIpv4HostOrder(const std::string& ip, std::uint32_t& out_host) {
    struct in_addr a{};
    if (ip.empty() || ::inet_pton(AF_INET, ip.c_str(), &a) != 1) {
        return false;
    }
    out_host = ntohl(a.s_addr);
    return true;
}

bool IsRfc1918PrivateIpv4(std::uint32_t ip_host) {
    if ((ip_host & 0xFF000000u) == 0x0A000000u) return true; // 10.0.0.0/8
    if ((ip_host & 0xFFF00000u) == 0xAC100000u) return true; // 172.16.0.0/12
    if ((ip_host & 0xFFFF0000u) == 0xC0A80000u) return true; // 192.168.0.0/16
    return false;
}

bool ShouldUseDirectPassthroughParams(
    SourceType source_type,
    const std::string& direct_url,
    float volume_percent,
    bool is_global,
    bool is_live) {
    if (source_type != SourceType::DirectAudio) {
        return false;
    }
    if (!StartsWithHttpPlain(direct_url)) {
        // Never send HTTPS URLs directly to legacy GTA SA client
        return false;
    }
    if (std::abs(volume_percent - 100.0f) >= 0.5f) {
        return false;
    }
    if (is_global && !is_live) {
        return false;
    }
    return true;
}

bool ShouldUseDirectPassthrough(const ActiveStreamSession& session) {
    return ShouldUseDirectPassthroughParams(
        session.source_type,
        session.metadata.direct_media_url,
        session.volume_percent.load(),
        session.is_global,
        session.metadata.is_live);
}

} // namespace

AudioEngine::AudioEngine(IPlatformHost& host, const PluginConfig& config)
    : host_(host),
      config_(config),
      control_pool_(0),
      extraction_pool_(0),
      binary_manager_(config_),
      metadata_cache_(config_.cache_max_entries, config_.cache_ttl_seconds),
      resolver_(config_, binary_manager_, metadata_cache_),
      alive_token_(std::make_shared<std::atomic<bool>>(true)),
      last_spatial_update_(std::chrono::steady_clock::now()),
      last_disconnect_poll_(std::chrono::steady_clock::now()),
      priority_order_(ParsePriorityOrder()) {}

AudioEngine::~AudioEngine() {
    Shutdown();
}

bool AudioEngine::Initialize() {
    bool expected = false;
    if (!initialized_.compare_exchange_strong(expected, true)) {
        return true;
    }

    shutting_down_.store(false);
    alive_token_->store(true);
    public_ip_failed_.store(false);
    public_ip_error_logged_.store(false);
    resolver_.ResetCancel();

    Logger::Instance().SetDebugEnabled(config_.debug_logging);

    std::string http_support_err;
    if (!HttpClient::CheckRuntimeSupport(http_support_err)) {
        Logger::Instance().Error(http_support_err);
    }

    const std::size_t ext_workers = std::clamp<std::size_t>(
        config_.worker_threads > 0 ? config_.worker_threads : 4, 2, 16);
    const std::size_t max_transcoders = std::clamp<std::size_t>(
        config_.max_concurrent_transcoders > 0 ? config_.max_concurrent_transcoders : 32, 1, 128);

    control_pool_.Start(2, 512 * 1024);
    extraction_pool_.Start(ext_workers, 512 * 1024);
    transcode_scheduler_.Start(max_transcoders, 256 * 1024);

    std::uint16_t port = config_.http_port;
    bool auto_port = (port == 0);
    if (auto_port) {
        std::int32_t samp_port = host_.GetServerPort();
        port = static_cast<std::uint16_t>(samp_port > 0 ? samp_port + 1 : 7778);
    }

    std::string err;
    if (!http_server_.Start(config_.bind_ip, port, err, config_.log_http_listen)) {
        if (!auto_port) {
            Logger::Instance().Error(
                "Configured HTTP port " + std::to_string(port) + " is unavailable (" + err +
                "); failing initialization instead of falling back to an ephemeral port.");
            Shutdown();
            return false;
        }
        Logger::Instance().Warn(
            "Auto-selected HTTP port " + std::to_string(port) + " unavailable (" + err +
            "), trying ephemeral port...");
        if (!http_server_.Start(config_.bind_ip, 0, err, config_.log_http_listen)) {
            Logger::Instance().Error("Failed to start HTTP streaming server: " + err);
            Shutdown();
            return false;
        }
        Logger::Instance().Warn(
            "HTTP streaming server bound to ephemeral port " +
            std::to_string(http_server_.BoundPort()) +
            "; set 'http_port' explicitly in config if behind a firewall.");
    }

    local_interfaces_ = DiscoverLocalIpv4Interfaces(config_.bind_ip);

    auto apply_http_security = [this](const std::string& pub_ip) {
        HttpClient::SetSecurityPolicy(HttpSecurityPolicy{
            config_.allow_private_network_urls,
            config_.bind_ip,
            pub_ip,
            http_server_.BoundPort()});
    };

    // Determine public IP for clients
    if (!config_.public_ip.empty()) {
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            public_host_ip_ = config_.public_ip;
        }
        apply_http_security(config_.public_ip);
        {
            std::lock_guard<std::mutex> iplock(public_ip_mutex_);
            public_ip_resolved_ = true;
        }
        public_ip_cv_.notify_all();
    } else {
        std::string bind_ip = host_.GetServerBindIp();
        if (!bind_ip.empty() && bind_ip != "0.0.0.0" && bind_ip != "127.0.0.1") {
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                public_host_ip_ = bind_ip;
            }
            apply_http_security(bind_ip);
            {
                std::lock_guard<std::mutex> iplock(public_ip_mutex_);
                public_ip_resolved_ = true;
            }
            public_ip_cv_.notify_all();
        } else {
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                public_host_ip_.clear();
            }
            apply_http_security("");
            {
                std::lock_guard<std::mutex> iplock(public_ip_mutex_);
                public_ip_resolved_ = false;
            }
            auto alive = alive_token_;
            std::uint16_t bound_port = http_server_.BoundPort();
            extraction_pool_.Post([this, alive, bound_port]() {
                if (shutting_down_.load() || !alive->load()) return;
                std::string detected = HttpClient::DetectPublicIPv4();
                if (!alive->load() || shutting_down_.load()) return;
                if (!detected.empty() && detected != "127.0.0.1") {
                    {
                        std::lock_guard<std::mutex> lock(state_mutex_);
                        public_host_ip_ = detected;
                    }
                    HttpClient::SetSecurityPolicy(HttpSecurityPolicy{
                        config_.allow_private_network_urls,
                        config_.bind_ip,
                        detected,
                        bound_port});
                    if (config_.log_public_ip_detected) {
                        Logger::Instance().Info("Detected public server IPv4: " + detected);
                    }
                    {
                        std::lock_guard<std::mutex> iplock(public_ip_mutex_);
                        public_ip_resolved_ = true;
                    }
                    public_ip_cv_.notify_all();

                    MainThreadDispatcher::Instance().Post([this, alive]() {
                        if (!alive->load() || shutting_down_.load()) return;
                        auto snapshots = host_.GetConnectedPlayerSnapshots();
                        std::vector<PlayerAudioCommand> cmds;
                        {
                            std::lock_guard<std::mutex> lock(state_mutex_);
                            for (const auto& [pid, _] : player_arbitration_) {
                                const PlayerSnapshot* match = nullptr;
                                for (const auto& s : snapshots) {
                                    if (s.id == pid) {
                                        match = &s;
                                        break;
                                    }
                                }
                                ReconcilePlayerLocked(pid, match, cmds);
                            }
                        }
                        ApplyPlayerAudioCommands(cmds);
                    });
                } else {
                    public_ip_failed_.store(true);
                    if (!public_ip_error_logged_.exchange(true)) {
                        Logger::Instance().Error(
                            "Failed to detect public IPv4 address; remote players cannot receive proxied streams until 'public_ip' is configured.");
                    }
                    {
                        std::lock_guard<std::mutex> iplock(public_ip_mutex_);
                        public_ip_resolved_ = true;
                    }
                    public_ip_cv_.notify_all();

                    MainThreadDispatcher::Instance().Post([this, alive]() {
                        if (!alive->load() || shutting_down_.load()) return;
                        std::vector<std::pair<PlayerId, StreamId>> errs;
                        {
                            std::lock_guard<std::mutex> lock(state_mutex_);
                            if (!public_host_ip_.empty()) return;
                            for (const auto& [sid, session] : streams_) {
                                if (!session || session->cancel_flag.load() || session->remote_url_error_reported) continue;
                                if (ShouldUseDirectPassthrough(*session)) continue;
                                if (session->is_global ||
                                    BuildEndpointUrlForPlayerLocked("stream", session->token, session->owner_player_id).empty()) {
                                    session->remote_url_error_reported = true;
                                    errs.emplace_back(session->owner_player_id, session->id);
                                }
                            }
                        }
                        for (const auto& [pid, sid] : errs) {
                            host_.DispatchPawnCallback("OnAudioStreamError", {
                                pid,
                                sid,
                                static_cast<std::int32_t>(ErrorCode::NetworkError),
                                std::string("Public IPv4 detection failed; set 'public_ip' in SampAudioStream config.")
                            });
                        }
                    });
                }
            });
        }
    }

    // Provision yt-dlp & QuickJS asynchronously on the extraction pool
    binary_manager_.InitializeAsync(extraction_pool_);
    return true;
}

void AudioEngine::Shutdown() {
    bool expected_init = true;
    bool was_init = initialized_.compare_exchange_strong(expected_init, false);
    if (!was_init && shutting_down_.load()) {
        return;
    }

    // 1. Signal shutdown and invalidate callbacks
    shutting_down_.store(true);
    alive_token_->store(false);
    public_ip_cv_.notify_all();

    // 2. Cancel all sessions, channels, searches, extractor processes, and binary downloads
    binary_manager_.Cancel();
    resolver_.CancelAll();

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        for (auto& [id, session] : streams_) {
            session->cancel_flag.store(true);
            session->state.store(StreamState::Stopped);
            if (session->buffer) session->buffer->Close();
            http_server_.UnregisterEndpoint(session->token);
        }
        streams_.clear();
        player_streams_.clear();

        for (auto& [id, ch] : channels_) {
            if (ch->active_run) {
                ch->active_run->cancel_flag.store(true);
            }
            ch->state.store(StreamState::Stopped);
            if (ch->buffer) ch->buffer->Close();
            http_server_.UnregisterEndpoint(ch->token);
        }
        channels_.clear();
        player_channels_.clear();
        player_arbitration_.clear();
        for (auto& [pid, st] : active_searches_) {
            if (st.cancel_flag) st.cancel_flag->store(true);
        }
        active_searches_.clear();
        pending_stream_completions_.clear();
        pending_channel_completions_.clear();
    }
    resolver_.PollCancellations();
    HttpClient::PollCancellations();

    // 3. Join all transcoder threads before tearing down HTTP server or pools
    transcode_scheduler_.StopAndJoin();

    // 4. Stop HTTP streaming server
    http_server_.Stop();
    HttpClient::SetSecurityPolicy({});

    // 5. Stop extraction and control thread pools (discarding queued unstarted work, joining running)
    extraction_pool_.Stop();
    control_pool_.Stop();

    // 6. Clear pending main-thread callbacks
    MainThreadDispatcher::Instance().Clear();
}

void AudioEngine::Reset() {
    std::vector<std::shared_ptr<ActiveStreamSession>> old_streams;
    std::vector<std::shared_ptr<SyncChannel>> old_channels;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        for (auto& [id, session] : streams_) {
            session->cancel_flag.store(true);
            session->state.store(StreamState::Stopped);
            if (session->buffer) session->buffer->Close();
            http_server_.UnregisterEndpoint(session->token);
            old_streams.push_back(session);
        }
        streams_.clear();
        player_streams_.clear();

        for (auto& [id, ch] : channels_) {
            if (ch->active_run) {
                ch->active_run->cancel_flag.store(true);
            }
            ch->state.store(StreamState::Stopped);
            if (ch->buffer) ch->buffer->Close();
            http_server_.UnregisterEndpoint(ch->token);
            old_channels.push_back(ch);
        }
        channels_.clear();
        player_channels_.clear();
        player_arbitration_.clear();
        for (auto& [pid, st] : active_searches_) {
            if (st.cancel_flag) st.cancel_flag->store(true);
        }
        active_searches_.clear();
        pending_stream_completions_.clear();
        pending_channel_completions_.clear();
    }

    resolver_.PollCancellations();
    MainThreadDispatcher::Instance().Clear();
}

void AudioEngine::Tick() {
    Logger::Instance().Flush();
    MainThreadDispatcher::Instance().ProcessPending();
    resolver_.PollCancellations();
    HttpClient::PollCancellations();

    auto now = std::chrono::steady_clock::now();
    ProcessPendingCompletions(now);
    auto poll_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_disconnect_poll_).count();
    if (!host_.TracksDisconnectEvents()) {
        CheckDisconnectedPlayers();
        if (poll_elapsed_ms >= 1000) {
            last_disconnect_poll_ = now;
            CleanupExpiredPassthroughSessions(now);
        }
    } else if (poll_elapsed_ms >= 1000) {
        last_disconnect_poll_ = now;
        CheckDisconnectedPlayers();
        CleanupExpiredPassthroughSessions(now);
    }

    binary_manager_.MaybePeriodicUpdate(extraction_pool_, resolver_.ActiveProcessCount());

    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_spatial_update_).count();
    if (elapsed_ms >= static_cast<long long>(config_.spatial_update_interval_ms)) {
        last_spatial_update_ = now;
        UpdateSpatialStreams();
    }
}

void AudioEngine::DetachSessionFromArbitrationLocked(
    const std::shared_ptr<ActiveStreamSession>& session,
    std::vector<PlayerAudioCommand>& out_cmds) {
    if (!session) return;
    StreamId sid = session->id;
    if (!session->is_global && session->owner_player_id != INVALID_PLAYER_ID) {
        auto pit = player_streams_.find(session->owner_player_id);
        if (pit != player_streams_.end() && pit->second == sid) {
            player_streams_.erase(pit);
        }
        auto ait = player_arbitration_.find(session->owner_player_id);
        if (ait != player_arbitration_.end() && ait->second.personal_stream_id == sid) {
            ait->second.personal_stream_id = INVALID_STREAM_ID;
        }
        ReconcilePlayerLocked(session->owner_player_id, nullptr, out_cmds);
    } else {
        for (PlayerId pid : session->active_listeners) {
            auto ait = player_arbitration_.find(pid);
            if (ait != player_arbitration_.end()) {
                auto& g = ait->second.global_streams;
                g.erase(std::remove(g.begin(), g.end(), sid), g.end());
                ReconcilePlayerLocked(pid, nullptr, out_cmds);
            }
        }
        session->active_listeners.clear();
    }
}

void AudioEngine::CancelPendingCompletionLocked(StreamId stream_id) {
    pending_stream_completions_.erase(
        std::remove_if(pending_stream_completions_.begin(), pending_stream_completions_.end(),
            [stream_id](const PendingStreamCompletion& p) {
                return !p.session || p.session->id == stream_id;
            }),
        pending_stream_completions_.end());
}

void AudioEngine::CancelPendingChannelCompletionLocked(ChannelId channel_id) {
    pending_channel_completions_.erase(
        std::remove_if(pending_channel_completions_.begin(), pending_channel_completions_.end(),
            [channel_id](const PendingChannelCompletion& p) {
                return !p.channel || p.channel->id == channel_id;
            }),
        pending_channel_completions_.end());
}

void AudioEngine::ProcessPendingCompletions(std::chrono::steady_clock::time_point now) {
    struct FinishedStreamNotice {
        PlayerId player_id;
        StreamId stream_id;
    };
    struct ChannelStartWork {
        std::shared_ptr<SyncChannel> channel;
        std::shared_ptr<ChannelRunState> run;
        std::string next_track;
    };

    std::vector<FinishedStreamNotice> finished_notices;
    std::vector<std::string> tokens_to_unregister;
    std::vector<ChannelStartWork> channel_next_work;
    std::vector<PlayerAudioCommand> cmds;

    {
        std::lock_guard<std::mutex> lock(state_mutex_);

        for (auto it = pending_stream_completions_.begin(); it != pending_stream_completions_.end();) {
            auto session = it->session;
            if (!session || session->cancel_flag.load() || session->state.load() == StreamState::Stopped) {
                if (session) tokens_to_unregister.push_back(session->token);
                it = pending_stream_completions_.erase(it);
                continue;
            }
            if (session->pause_flag.load()) {
                ++it;
                continue;
            }
            if (now < it->min_deadline) {
                ++it;
                continue;
            }
            bool ready = (now >= it->max_deadline) ||
                         (http_server_.GetActiveConnectionCount(session->token) == 0);
            if (!ready) {
                ++it;
                continue;
            }

            tokens_to_unregister.push_back(session->token);
            session->state.store(StreamState::Finished);
            streams_.erase(session->id);
            DetachSessionFromArbitrationLocked(session, cmds);
            finished_notices.push_back({session->owner_player_id, session->id});
            it = pending_stream_completions_.erase(it);
        }

        for (auto it = pending_channel_completions_.begin(); it != pending_channel_completions_.end();) {
            auto ch = it->channel;
            auto run = it->run;
            if (!ch || !run || run->cancel_flag.load() || ch->active_run != run) {
                it = pending_channel_completions_.erase(it);
                continue;
            }
            if (ch->pause_flag.load()) {
                ++it;
                continue;
            }
            if (now < it->min_deadline) {
                ++it;
                continue;
            }
            bool ready = (now >= it->max_deadline) ||
                         (http_server_.GetActiveConnectionCount(ch->token) == 0);
            if (!ready) {
                ++it;
                continue;
            }

            std::string next_track;
            std::shared_ptr<ChannelRunState> next_run;
            AdvanceChannelOrEndLocked(ch, StreamState::Finished, next_track, next_run, cmds);
            if (next_run && !next_track.empty()) {
                channel_next_work.push_back({ch, next_run, std::move(next_track)});
            }
            it = pending_channel_completions_.erase(it);
        }
    }

    for (const auto& tok : tokens_to_unregister) {
        http_server_.UnregisterEndpoint(tok);
    }
    for (const auto& notice : finished_notices) {
        host_.DispatchPawnCallback("OnAudioStreamFinished", {notice.player_id, notice.stream_id});
    }
    ApplyPlayerAudioCommands(cmds);
    for (const auto& cw : channel_next_work) {
        StartChannelWorker(cw.channel, cw.run, cw.next_track);
    }
}

void AudioEngine::CleanupExpiredPassthroughSessions(std::chrono::steady_clock::time_point now) {
    std::vector<PlayerAudioCommand> cmds;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        for (auto it = streams_.begin(); it != streams_.end();) {
            const auto& session = it->second;
            if (session && session->is_passthrough.load() &&
                session->state.load() == StreamState::Playing &&
                session->metadata.duration_seconds > 0) {
                auto expire_after = std::chrono::seconds(session->metadata.duration_seconds + 15);
                if ((now - session->started_at) >= expire_after) {
                    session->state.store(StreamState::Finished);
                    DetachSessionFromArbitrationLocked(session, cmds);
                    it = streams_.erase(it);
                    continue;
                }
            }
            ++it;
        }
    }
    ApplyPlayerAudioCommands(cmds);
}

void AudioEngine::NotifyDisconnectHookActive() {
    host_.OnDisconnectHookObserved();
}

void AudioEngine::CheckDisconnectedPlayers() {
    std::vector<PlayerId> tracked;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (player_arbitration_.empty() && active_searches_.empty() && player_streams_.empty() && player_channels_.empty()) {
            return;
        }
        tracked.reserve(player_arbitration_.size() + active_searches_.size());
        for (const auto& [pid, _] : player_arbitration_) tracked.push_back(pid);
        for (const auto& [pid, _] : active_searches_) tracked.push_back(pid);
        for (const auto& [pid, _] : player_streams_) tracked.push_back(pid);
        for (const auto& [pid, _] : player_channels_) tracked.push_back(pid);
    }

    std::sort(tracked.begin(), tracked.end());
    tracked.erase(std::unique(tracked.begin(), tracked.end()), tracked.end());

    for (PlayerId pid : tracked) {
        if (!host_.IsPlayerConnected(pid)) {
            OnPlayerDisconnect(pid);
        }
    }
}

void AudioEngine::OnPlayerDisconnect(PlayerId player_id) {
    std::shared_ptr<ActiveStreamSession> old_personal;
    std::shared_ptr<AudioStreamBuffer> old_buf;
    bool search_cancelled = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto sit_search = active_searches_.find(player_id);
        if (sit_search != active_searches_.end()) {
            if (sit_search->second.cancel_flag) {
                sit_search->second.cancel_flag->store(true);
                search_cancelled = true;
            }
            active_searches_.erase(sit_search);
        }

        auto ps_it = player_streams_.find(player_id);
        if (ps_it != player_streams_.end()) {
            CancelPendingCompletionLocked(ps_it->second);
            auto sit = streams_.find(ps_it->second);
            if (sit != streams_.end()) {
                old_personal = sit->second;
                old_buf = old_personal->buffer;
                streams_.erase(sit);
            }
            player_streams_.erase(ps_it);
        }

        auto pc_it = player_channels_.find(player_id);
        if (pc_it != player_channels_.end()) {
            auto cit = channels_.find(pc_it->second);
            if (cit != channels_.end()) {
                cit->second->listeners.erase(player_id);
            }
            player_channels_.erase(pc_it);
        }

        for (auto& [id, session] : streams_) {
            session->active_listeners.erase(player_id);
        }
        for (auto& [id, ch] : channels_) {
            ch->listeners.erase(player_id);
        }
        player_arbitration_.erase(player_id);
    }

    if (old_personal) {
        old_personal->cancel_flag.store(true);
        old_personal->state.store(StreamState::Stopped);
        if (old_buf) old_buf->Close();
        http_server_.UnregisterEndpoint(old_personal->token);
        HttpClient::PollCancellations();
    }
    if (search_cancelled || old_personal) {
        resolver_.PollCancellations();
    }
}

std::string AudioEngine::GenerateToken() {
    std::array<std::uint8_t, 16> bytes{};
    bool ok = false;
#ifdef _WIN32
    ok = (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0);
#else
    ssize_t got = ::getrandom(bytes.data(), bytes.size(), 0);
    if (got == static_cast<ssize_t>(bytes.size())) {
        ok = true;
    } else {
        int fd = ::open("/dev/urandom", O_RDONLY);
        if (fd >= 0) {
            std::size_t off = 0;
            while (off < bytes.size()) {
                ssize_t r = ::read(fd, bytes.data() + off, bytes.size() - off);
                if (r <= 0) break;
                off += static_cast<std::size_t>(r);
            }
            ::close(fd);
            ok = (off == bytes.size());
        }
    }
#endif
    if (!ok) {
        std::uint64_t val = token_counter_.fetch_add(1);
        std::uint64_t now = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        std::uint64_t a = now ^ (val * 0x9e3779b97f4a7c15ULL);
        std::uint64_t b = (now >> 13) ^ (val * 0xbf58476d1ce4e5b9ULL);
        std::memcpy(bytes.data(), &a, 8);
        std::memcpy(bytes.data() + 8, &b, 8);
    }

    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (std::uint8_t b : bytes) {
        oss << std::setw(2) << static_cast<unsigned int>(b);
    }
    return oss.str();
}

std::string AudioEngine::BuildEndpointUrlLocked(const std::string& prefix, const std::string& token) const {
    const std::string& host_ip = public_host_ip_.empty() ? std::string("127.0.0.1") : public_host_ip_;
    return "http://" + host_ip + ":" + std::to_string(http_server_.BoundPort()) + "/" + prefix + "/" + token;
}

std::vector<LocalIpv4Interface> AudioEngine::DiscoverLocalIpv4Interfaces(const std::string& bind_ip) {
    std::vector<LocalIpv4Interface> result;
    const bool filter_bind = !bind_ip.empty() && bind_ip != "0.0.0.0" && bind_ip != "::";

#ifdef _WIN32
    ULONG buf_len = 16 * 1024;
    std::vector<std::uint8_t> buf(buf_len);
    auto* addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    ULONG ret = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, addrs, &buf_len);
    if (ret == ERROR_BUFFER_OVERFLOW) {
        buf.resize(buf_len);
        addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        ret = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, addrs, &buf_len);
    }
    if (ret == NO_ERROR) {
        for (auto* a = addrs; a != nullptr; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp) continue;
            for (auto* ua = a->FirstUnicastAddress; ua != nullptr; ua = ua->Next) {
                if (!ua->Address.lpSockaddr || ua->Address.lpSockaddr->sa_family != AF_INET) continue;
                const auto* sin = reinterpret_cast<const struct sockaddr_in*>(ua->Address.lpSockaddr);
                std::uint32_t addr_h = ntohl(sin->sin_addr.s_addr);
                if ((addr_h & 0xFF000000u) == 0x7F000000u || addr_h == 0) continue;
                char ip_buf[INET_ADDRSTRLEN]{};
                if (!::inet_ntop(AF_INET, &sin->sin_addr, ip_buf, sizeof(ip_buf))) continue;
                std::string ip_str(ip_buf);
                if (filter_bind && ip_str != bind_ip) continue;
                std::uint8_t prefix = ua->OnLinkPrefixLength;
                std::uint32_t mask_h = (prefix == 0) ? 0u : (prefix >= 32 ? 0xFFFFFFFFu : (0xFFFFFFFFu << (32 - prefix)));
                if (mask_h == 0) continue;
                result.push_back(LocalIpv4Interface{std::move(ip_str), addr_h, mask_h});
            }
        }
    }
#else
    struct ifaddrs* ifap = nullptr;
    if (::getifaddrs(&ifap) == 0 && ifap) {
        for (struct ifaddrs* ifa = ifap; ifa != nullptr; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr || !ifa->ifa_netmask || ifa->ifa_addr->sa_family != AF_INET) continue;
            const auto* sin = reinterpret_cast<const struct sockaddr_in*>(ifa->ifa_addr);
            const auto* smask = reinterpret_cast<const struct sockaddr_in*>(ifa->ifa_netmask);
            std::uint32_t addr_h = ntohl(sin->sin_addr.s_addr);
            std::uint32_t mask_h = ntohl(smask->sin_addr.s_addr);
            if ((addr_h & 0xFF000000u) == 0x7F000000u || addr_h == 0 || mask_h == 0) continue;
            char ip_buf[INET_ADDRSTRLEN]{};
            if (!::inet_ntop(AF_INET, &sin->sin_addr, ip_buf, sizeof(ip_buf))) continue;
            std::string ip_str(ip_buf);
            if (filter_bind && ip_str != bind_ip) continue;
            result.push_back(LocalIpv4Interface{std::move(ip_str), addr_h, mask_h});
        }
        ::freeifaddrs(ifap);
    }
#endif
    return result;
}

std::string AudioEngine::ResolveEndpointHostIp(
    const std::string& player_ip,
    const std::string& public_host_ip,
    const std::vector<LocalIpv4Interface>& local_interfaces,
    bool lan_aware_urls,
    const std::string& bind_ip) {
    const bool bind_allows_loopback = bind_ip.empty() || bind_ip == "0.0.0.0" || bind_ip == "::" || bind_ip == "127.0.0.1";
    if (!player_ip.empty() && IsLoopbackIp(player_ip)) {
        if (lan_aware_urls && bind_allows_loopback) {
            return "127.0.0.1";
        }
        return public_host_ip.empty() ? (bind_allows_loopback ? std::string("127.0.0.1") : bind_ip) : public_host_ip;
    }

    if (lan_aware_urls && !player_ip.empty()) {
        std::uint32_t pip_h = 0;
        if (ParseIpv4HostOrder(player_ip, pip_h) && IsRfc1918PrivateIpv4(pip_h)) {
            for (const auto& iface : local_interfaces) {
                if (!iface.ip.empty() && iface.mask_host != 0 &&
                    (pip_h & iface.mask_host) == (iface.addr_host & iface.mask_host)) {
                    return iface.ip;
                }
            }
        }
    }

    return public_host_ip;
}

std::string AudioEngine::BuildEndpointUrlForPlayerLocked(
    const std::string& prefix,
    const std::string& token,
    PlayerId player_id) const {
    std::string pip = (player_id != INVALID_PLAYER_ID) ? host_.GetPlayerIp(player_id) : "";
    std::string host_ip = ResolveEndpointHostIp(pip, public_host_ip_, local_interfaces_, config_.lan_aware_urls, config_.bind_ip);
    if (host_ip.empty()) {
        return "";
    }
    return "http://" + host_ip + ":" + std::to_string(http_server_.BoundPort()) + "/" + prefix + "/" + token;
}

std::string AudioEngine::BuildEndpointUrl(const std::string& prefix, const std::string& token) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return BuildEndpointUrlLocked(prefix, token);
}

bool AudioEngine::MatchesSpatialFilterSnapshot(const PlayerSnapshot& snap, const StreamOptions& opts) const {
    if (opts.world_id != -1 && snap.world_id != opts.world_id) {
        return false;
    }
    if (opts.interior_id != -1 && snap.interior_id != opts.interior_id) {
        return false;
    }
    if (opts.use_position && opts.distance > 0.0f) {
        float max_d = opts.distance;
        if (DistanceSq(snap.pos, opts.position) > (max_d * max_d)) {
            return false;
        }
    }
    return true;
}

std::array<AudioSourcePriority, 3> AudioEngine::ParsePriorityOrder() const {
    std::array<AudioSourcePriority, 3> order{
        AudioSourcePriority::Personal,
        AudioSourcePriority::Channel,
        AudioSourcePriority::Global
    };
    if (config_.player_priority_order.empty()) {
        return order;
    }

    std::array<AudioSourcePriority, 3> parsed{};
    std::size_t count = 0;
    auto add_unique = [&](AudioSourcePriority p) {
        for (std::size_t i = 0; i < count; ++i) {
            if (parsed[i] == p) return;
        }
        if (count < 3) parsed[count++] = p;
    };

    std::string lower;
    lower.reserve(config_.player_priority_order.size());
    for (char c : config_.player_priority_order) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }

    std::size_t pos = 0;
    while (pos <= lower.size() && count < 3) {
        std::size_t comma = lower.find(',', pos);
        std::string tok = (comma == std::string::npos) ? lower.substr(pos) : lower.substr(pos, comma - pos);
        while (!tok.empty() && tok.front() == ' ') tok.erase(tok.begin());
        while (!tok.empty() && tok.back() == ' ') tok.pop_back();
        if (tok == "personal") add_unique(AudioSourcePriority::Personal);
        else if (tok == "channel") add_unique(AudioSourcePriority::Channel);
        else if (tok == "global") add_unique(AudioSourcePriority::Global);
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }

    for (AudioSourcePriority def : order) {
        add_unique(def);
    }
    return parsed;
}

std::string AudioEngine::BuildSessionPlayUrlLocked(const ActiveStreamSession& session, PlayerId player_id) const {
    if (ShouldUseDirectPassthrough(session)) {
        return session.metadata.direct_media_url;
    }
    std::string url = BuildEndpointUrlForPlayerLocked("stream", session.token, player_id);
    if (url.empty()) {
        return "";
    }
    if (!session.is_global && session.paused_offset > 0) {
        url += "?resume=" + std::to_string(session.paused_offset);
    }
    return url;
}

void AudioEngine::ReconcilePlayerLocked(
    PlayerId player_id,
    const PlayerSnapshot* snapshot,
    std::vector<PlayerAudioCommand>& out_cmds) {
    if (player_id < 0) return;

    auto ait = player_arbitration_.find(player_id);
    if (ait == player_arbitration_.end()) return;
    PlayerArbitrationState& arb = ait->second;

    const auto& order = priority_order_;
    AudioSourcePriority desired_kind = AudioSourcePriority::None;
    std::uint64_t desired_id = 0;
    std::uint64_t desired_gen = 0;
    std::string desired_url;
    Vector3 desired_pos{};
    float desired_dist = 0.0f;
    bool desired_use_pos = false;

    for (AudioSourcePriority tier : order) {
        if (tier == AudioSourcePriority::Personal) {
            if (arb.personal_stream_id != INVALID_STREAM_ID) {
                auto sit = streams_.find(arb.personal_stream_id);
                if (sit == streams_.end() || sit->second->cancel_flag.load()) {
                    arb.personal_stream_id = INVALID_STREAM_ID;
                } else {
                    const auto& session = *sit->second;
                    StreamState st = session.state.load();
                    if (st == StreamState::Stopped || st == StreamState::Error || st == StreamState::Finished) {
                        arb.personal_stream_id = INVALID_STREAM_ID;
                    } else {
                        if (st == StreamState::Playing && !session.pause_flag.load()) {
                            bool passthrough_no_zone_filter =
                                session.is_passthrough.load() &&
                                session.options.world_id == -1 &&
                                session.options.interior_id == -1;
                            bool in_range = passthrough_no_zone_filter || (snapshot
                                ? MatchesSpatialFilterSnapshot(*snapshot, session.options)
                                : MatchesSpatialFilter(player_id, session.options));
                            if (in_range) {
                                std::string candidate_url = BuildSessionPlayUrlLocked(session, player_id);
                                if (!candidate_url.empty()) {
                                    desired_kind = AudioSourcePriority::Personal;
                                    desired_id = session.id;
                                    desired_gen = 1;
                                    desired_url = std::move(candidate_url);
                                    desired_pos = session.options.position;
                                    desired_dist = session.options.distance;
                                    desired_use_pos = session.options.use_position;
                                }
                            }
                        }
                        break; // Personal claims priority slot while resolving, buffering, playing, or paused
                    }
                }
            }
        } else if (tier == AudioSourcePriority::Channel) {
            if (arb.channel_id != INVALID_CHANNEL_ID) {
                auto cit = channels_.find(arb.channel_id);
                if (cit == channels_.end()) {
                    arb.channel_id = INVALID_CHANNEL_ID;
                } else {
                    const auto& ch = *cit->second;
                    StreamState st = ch.state.load();
                    if (st == StreamState::Resolving || st == StreamState::Buffering ||
                        st == StreamState::Playing || st == StreamState::Paused) {
                        if (st == StreamState::Playing || st == StreamState::Paused) {
                            std::string candidate_url = BuildEndpointUrlForPlayerLocked("channel", ch.token, player_id);
                            if (!candidate_url.empty()) {
                                desired_kind = AudioSourcePriority::Channel;
                                desired_id = ch.id;
                                desired_gen = ch.active_run ? ch.active_run->generation : ch.next_generation;
                                desired_url = std::move(candidate_url);
                            }
                        }
                        break;
                    }
                }
            }
        } else if (tier == AudioSourcePriority::Global) {
            for (auto it = arb.global_streams.begin(); it != arb.global_streams.end();) {
                auto sit = streams_.find(*it);
                if (sit == streams_.end() || sit->second->cancel_flag.load()) {
                    it = arb.global_streams.erase(it);
                    continue;
                }
                StreamState st = sit->second->state.load();
                if (st == StreamState::Stopped || st == StreamState::Error || st == StreamState::Finished) {
                    it = arb.global_streams.erase(it);
                    continue;
                }
                ++it;
            }
            for (StreamId gid : arb.global_streams) {
                auto sit = streams_.find(gid);
                if (sit == streams_.end()) continue;
                const auto& session = *sit->second;
                if (session.state.load() == StreamState::Playing && !session.pause_flag.load()) {
                    bool in_range = snapshot
                        ? MatchesSpatialFilterSnapshot(*snapshot, session.options)
                        : MatchesSpatialFilter(player_id, session.options);
                    if (in_range) {
                        std::string candidate_url = BuildSessionPlayUrlLocked(session, player_id);
                        if (!candidate_url.empty()) {
                            desired_kind = AudioSourcePriority::Global;
                            desired_id = session.id;
                            desired_gen = 1;
                            desired_url = std::move(candidate_url);
                            desired_pos = session.options.position;
                            desired_dist = session.options.distance;
                            desired_use_pos = session.options.use_position;
                            break;
                        }
                    }
                }
            }
            if (desired_kind != AudioSourcePriority::None) {
                break;
            }
        }
    }

    if (desired_kind == AudioSourcePriority::None) {
        if (arb.client_source_kind != AudioSourcePriority::None) {
            out_cmds.push_back(PlayerAudioCommand{player_id, false, "", {}, 0.0f, false});
            arb.client_source_kind = AudioSourcePriority::None;
            arb.client_source_id = 0;
            arb.client_generation = 0;
            arb.client_url.clear();
        }
        if (arb.personal_stream_id == INVALID_STREAM_ID &&
            arb.channel_id == INVALID_CHANNEL_ID &&
            arb.global_streams.empty()) {
            player_arbitration_.erase(ait);
        }
        return;
    }

    if (desired_kind != AudioSourcePriority::None &&
        desired_kind != AudioSourcePriority::Personal &&
        arb.personal_stream_id != INVALID_STREAM_ID) {
        auto sit = streams_.find(arb.personal_stream_id);
        if (sit != streams_.end() && sit->second->is_passthrough.load()) {
            sit->second->cancel_flag.store(true);
            sit->second->state.store(StreamState::Stopped);
            streams_.erase(sit);
            auto pit = player_streams_.find(player_id);
            if (pit != player_streams_.end() && pit->second == arb.personal_stream_id) {
                player_streams_.erase(pit);
            }
            arb.personal_stream_id = INVALID_STREAM_ID;
        }
    }

    if (desired_kind != arb.client_source_kind ||
        desired_id != arb.client_source_id ||
        desired_gen != arb.client_generation ||
        desired_url != arb.client_url) {
        out_cmds.push_back(PlayerAudioCommand{
            player_id, true, desired_url, desired_pos, desired_dist, desired_use_pos
        });
        arb.client_source_kind = desired_kind;
        arb.client_source_id = desired_id;
        arb.client_generation = desired_gen;
        arb.client_url = desired_url;
    }
}

void AudioEngine::ApplyPlayerAudioCommands(const std::vector<PlayerAudioCommand>& cmds) {
    for (const auto& cmd : cmds) {
        if (!host_.IsPlayerConnected(cmd.player_id)) {
            continue;
        }
        if (cmd.play) {
            host_.SendPlayAudioStream(cmd.player_id, cmd.url, cmd.position, cmd.distance, cmd.use_position);
        } else {
            host_.SendStopAudioStream(cmd.player_id);
        }
    }
}

bool AudioEngine::MatchesSpatialFilter(PlayerId player_id, const StreamOptions& opts) const {
    if (!host_.IsPlayerConnected(player_id)) {
        return false;
    }
    if (opts.world_id != -1 && host_.GetPlayerVirtualWorld(player_id) != opts.world_id) {
        return false;
    }
    if (opts.interior_id != -1 && host_.GetPlayerInterior(player_id) != opts.interior_id) {
        return false;
    }
    if (opts.use_position && opts.distance > 0.0f) {
        Vector3 ppos{};
        if (!host_.GetPlayerPos(player_id, ppos)) {
            return false;
        }
        float max_d = opts.distance;
        if (DistanceSq(ppos, opts.position) > (max_d * max_d)) {
            return false;
        }
    }
    return true;
}

std::size_t AudioEngine::EffectiveStreamConcurrencyLimit() const {
    return std::min<std::size_t>(
        std::max<std::uint32_t>(1, config_.max_concurrent_streams),
        std::max<std::uint32_t>(1, config_.max_concurrent_transcoders));
}

std::size_t AudioEngine::CountResourceStreamsLocked(StreamId exclude_id, ChannelId exclude_channel_id) const {
    std::size_t count = 0;
    for (const auto& [sid, session] : streams_) {
        if (sid == exclude_id || !session) continue;
        if (!session->is_passthrough.load()) {
            ++count;
        }
    }
    for (const auto& [cid, ch] : channels_) {
        if (cid == exclude_channel_id || !ch) continue;
        if (ch->active_run != nullptr) {
            ++count;
        }
    }
    return count;
}

std::size_t AudioEngine::CurrentStreamLoadLocked(StreamId exclude_id, ChannelId exclude_channel_id) const {
    std::size_t resource_count = CountResourceStreamsLocked(exclude_id, exclude_channel_id);
    std::size_t scheduler_load = transcode_scheduler_.LiveCount();

    if (scheduler_load > 0 && exclude_id != INVALID_STREAM_ID) {
        auto sit = streams_.find(exclude_id);
        if (sit != streams_.end() && sit->second && !sit->second->is_passthrough.load() && sit->second->buffer &&
            !sit->second->cancel_flag.load()) {
            bool is_pending_completion = std::any_of(
                pending_stream_completions_.begin(),
                pending_stream_completions_.end(),
                [exclude_id](const PendingStreamCompletion& p) {
                    return p.session && p.session->id == exclude_id;
                });
            if (!is_pending_completion) {
                --scheduler_load;
            }
        }
    }

    if (scheduler_load > 0 && exclude_channel_id != INVALID_CHANNEL_ID) {
        auto cit = channels_.find(exclude_channel_id);
        if (cit != channels_.end() && cit->second && cit->second->active_run &&
            !cit->second->active_run->cancel_flag.load()) {
            bool is_pending_completion = std::any_of(
                pending_channel_completions_.begin(),
                pending_channel_completions_.end(),
                [exclude_channel_id](const PendingChannelCompletion& p) {
                    return p.channel && p.channel->id == exclude_channel_id;
                });
            if (!is_pending_completion) {
                --scheduler_load;
            }
        }
    }

    return std::max(resource_count, scheduler_load);
}

StreamId AudioEngine::PlayAudioStreamForPlayer(PlayerId player_id, const std::string& url_or_query, const StreamOptions& options) {
    if (!initialized_.load() || shutting_down_.load() || !host_.IsPlayerConnected(player_id) || url_or_query.empty()) {
        return INVALID_STREAM_ID;
    }

    auto session = std::make_shared<ActiveStreamSession>();
    session->id = next_stream_id_.fetch_add(1);
    session->token = GenerateToken();
    session->original_input = url_or_query;
    session->options = options;
    session->is_global = false;
    session->owner_player_id = player_id;
    session->owner_is_loopback = IsLoopbackIp(host_.GetPlayerIp(player_id));
    session->volume_percent.store(std::clamp(options.volume, 0.0f, 200.0f));
    session->started_at = std::chrono::steady_clock::now();

    std::string sync_policy_err;
    const bool sync_http_shortcut =
        StartsWithHttpPlain(url_or_query) &&
        !StreamResolver::IsKnownPlatformUrl(url_or_query) &&
        std::abs(session->volume_percent.load() - 100.0f) < 0.5f &&
        options.world_id == -1 &&
        options.interior_id == -1 &&
        HttpClient::ValidateUrlSyntaxAndLiteralPolicy(url_or_query, sync_policy_err);

    const bool pre_passthrough = sync_http_shortcut ||
        (StartsWithHttpPlain(url_or_query) &&
         StreamResolver::IsKnownDirectAudioExtension(url_or_query) &&
         std::abs(session->volume_percent.load() - 100.0f) < 0.5f);
    session->is_passthrough.store(pre_passthrough);

    if (sync_http_shortcut) {
        session->source_type = SourceType::DirectAudio;
        session->metadata.original_url = url_or_query;
        session->metadata.direct_media_url = url_or_query;
        session->metadata.extractor_name = "direct";
        session->metadata.title.clear();
        session->metadata.is_live = true;
        session->ready.store(true);
        session->state.store(StreamState::Playing);
    }

    std::shared_ptr<ActiveStreamSession> old_personal;
    std::shared_ptr<AudioStreamBuffer> old_buf;
    std::vector<PlayerAudioCommand> cmds;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto prev_it = player_streams_.find(player_id);
        StreamId replacing_id = (prev_it != player_streams_.end()) ? prev_it->second : INVALID_STREAM_ID;
        std::size_t projected_size = CurrentStreamLoadLocked(replacing_id, INVALID_CHANNEL_ID) + (pre_passthrough ? 0 : 1);
        std::size_t limit = EffectiveStreamConcurrencyLimit();
        if (!pre_passthrough && projected_size > limit) {
            Logger::Instance().Warn(
                "Maximum concurrent streams/transcoders limit reached (" + std::to_string(limit) + ")");
            return INVALID_STREAM_ID;
        }
        if (prev_it != player_streams_.end()) {
            CancelPendingCompletionLocked(prev_it->second);
            auto sit = streams_.find(prev_it->second);
            if (sit != streams_.end()) {
                old_personal = sit->second;
                old_buf = old_personal->buffer;
                streams_.erase(sit);
            }
            player_streams_.erase(prev_it);
        }
        streams_[session->id] = session;
        player_streams_[player_id] = session->id;
        player_arbitration_[player_id].personal_stream_id = session->id;
        if (sync_http_shortcut) {
            session->active_listeners.insert(player_id);
        }
        ReconcilePlayerLocked(player_id, nullptr, cmds);
    }

    if (old_personal) {
        old_personal->cancel_flag.store(true);
        old_personal->state.store(StreamState::Stopped);
        if (old_buf) old_buf->Close();
        http_server_.UnregisterEndpoint(old_personal->token);
        HttpClient::PollCancellations();
    }
    ApplyPlayerAudioCommands(cmds);

    if (sync_http_shortcut) {
        auto alive = alive_token_;
        MainThreadDispatcher::Instance().Post([this, session, alive]() {
            if (!alive->load() || shutting_down_.load() || session->cancel_flag.load()) return;
            host_.DispatchPawnCallback("OnAudioStreamReady", {
                session->owner_player_id,
                session->id,
                ConvertTextForPawn(session->metadata.title, config_.pawn_text_encoding),
                ConvertTextForPawn(session->metadata.artist, config_.pawn_text_encoding),
                session->metadata.duration_seconds,
                session->metadata.direct_media_url
            });
            host_.DispatchPawnCallback("OnAudioStreamStart", {session->owner_player_id, session->id});
        });
        ProbeIcyTitleAsync(session);
        return session->id;
    }

    StartStreamWorker(session);
    return session->id;
}

StreamId AudioEngine::PlayAudioStream(const std::string& url_or_query, const StreamOptions& options) {
    if (!initialized_.load() || shutting_down_.load() || url_or_query.empty()) {
        return INVALID_STREAM_ID;
    }

    auto session = std::make_shared<ActiveStreamSession>();
    session->id = next_stream_id_.fetch_add(1);
    session->token = GenerateToken();
    session->original_input = url_or_query;
    session->options = options;
    session->is_global = true;
    session->owner_player_id = INVALID_PLAYER_ID;
    session->volume_percent.store(std::clamp(options.volume, 0.0f, 200.0f));
    session->started_at = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        std::size_t limit = EffectiveStreamConcurrencyLimit();
        if (CurrentStreamLoadLocked() >= limit) {
            Logger::Instance().Warn(
                "Maximum concurrent streams/transcoders limit reached (" + std::to_string(limit) + ")");
            return INVALID_STREAM_ID;
        }
        streams_[session->id] = session;
    }

    StartStreamWorker(session);
    return session->id;
}

std::size_t AudioEngine::CalculateRingBufferBytes() const {
    std::uint32_t kbps = std::clamp<std::uint32_t>(config_.mp3_bitrate_kbps, 64, 320);
    std::size_t bytes_per_sec = (static_cast<std::size_t>(kbps) * 1000) / 8;
    // ~30 seconds of encoded MP3 audio (at 128 kbps -> ~480 KB)
    return std::max<std::size_t>(256 * 1024, bytes_per_sec * 30);
}

void AudioEngine::ProbeIcyTitleAsync(const std::shared_ptr<ActiveStreamSession>& session) {
    if (!session) return;
    const std::string url = session->metadata.direct_media_url;
    if (url.empty() || StreamResolver::IsKnownDirectAudioExtension(url)) {
        return;
    }
    auto alive = alive_token_;
    extraction_pool_.Post([this, session, url, alive]() {
        if (session->cancel_flag.load() || shutting_down_.load() || !alive->load()) return;
        std::string err;
        std::string captured_title;
        // Brief small read with Icy-MetaData=1 just to extract StreamTitle if present
        std::vector<std::uint8_t> buf;
        std::size_t metaint = 0;
        HttpClient::StreamGet(
            url,
            {{"Icy-MetaData", "1"}},
            [&](const std::uint8_t* data, std::size_t size) {
                if (session->cancel_flag.load() || shutting_down_.load() || !alive->load()) return false;
                if (buf.size() < 24576) {
                    std::size_t take = std::min<std::size_t>(size, 24576 - buf.size());
                    buf.insert(buf.end(), data, data + take);
                }
                return buf.size() < 20480;
            },
            session->cancel_flag,
            err,
            nullptr,
            3000);
        (void)metaint;
        if (session->cancel_flag.load() || shutting_down_.load() || !alive->load() || buf.empty()) return;
        std::string text(reinterpret_cast<const char*>(buf.data()), buf.size());
        auto pos = text.find("StreamTitle='");
        if (pos != std::string::npos) {
            pos += 13;
            auto end = text.find("';", pos);
            if (end != std::string::npos && end > pos && (end - pos) <= 256) {
                captured_title = text.substr(pos, end - pos);
            }
        }
        if (!captured_title.empty()) {
            MainThreadDispatcher::Instance().Post([session, alive, captured_title]() {
                if (!alive->load() || session->cancel_flag.load()) return;
                session->metadata.title = captured_title;
            });
        }
    });
}

void AudioEngine::PreResolveNextChannelTrack(const std::shared_ptr<SyncChannel>& channel, const std::string& url_or_query) {
    if (!channel || url_or_query.empty() || shutting_down_.load()) return;
    auto run_snapshot = channel->active_run;
    auto alive = alive_token_;
    extraction_pool_.Post([this, channel, run_snapshot, url_or_query, alive]() {
        if (shutting_down_.load() || !alive->load()) return;
        if (run_snapshot && run_snapshot->cancel_flag.load()) return;
        Logger::Instance().Debug("Pre-resolving next queued track for channel " + std::to_string(channel->id));
        const std::atomic<bool>* cancel_ptr = run_snapshot ? &run_snapshot->cancel_flag : nullptr;
        resolver_.Resolve(url_or_query, cancel_ptr);
    });
}

void AudioEngine::StartStreamWorker(std::shared_ptr<ActiveStreamSession> session) {
    auto alive = alive_token_;
    auto queued_at = std::chrono::steady_clock::now();
    if (!extraction_pool_.Post([this, session, alive, queued_at]() {
        if (session->cancel_flag.load() || shutting_down_.load() || !alive->load()) {
            return;
        }
        auto wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - queued_at).count();
        Logger::Instance().Debug("Stream " + std::to_string(session->id) + " extraction_pool queue wait: " + std::to_string(wait_ms) + " ms");

        ResolveResult resolved = resolver_.Resolve(session->original_input, &session->cancel_flag);
        if (session->cancel_flag.load() || shutting_down_.load() || !alive->load()) {
            return;
        }

        if (!resolved.success) {
            session->state.store(StreamState::Error);
            ErrorCode err_code = resolved.error;
            std::string err_msg = resolved.error_message;
            MainThreadDispatcher::Instance().Post([this, session, alive, err_code, err_msg]() {
                if (!alive->load() || shutting_down_.load()) return;
                host_.DispatchPawnCallback("OnAudioStreamError", {
                    session->owner_player_id,
                    session->id,
                    static_cast<std::int32_t>(err_code),
                    ConvertTextForPawn(err_msg, config_.pawn_text_encoding)
                });
                StopAudioStream(session->id);
            });
            return;
        }

        TrackMetadata local_meta = std::move(resolved.metadata);
        SourceType local_src = resolved.source_type;

        bool use_direct_passthrough = ShouldUseDirectPassthroughParams(
            local_src,
            local_meta.direct_media_url,
            session->volume_percent.load(),
            session->is_global,
            local_meta.is_live);

        if (!use_direct_passthrough) {
            std::unique_lock<std::mutex> iplock(public_ip_mutex_);
            public_ip_cv_.wait_for(iplock, std::chrono::milliseconds(2500), [this, &session, &alive]() {
                return public_ip_resolved_ || session->cancel_flag.load() || shutting_down_.load() || !alive->load();
            });
            if (session->cancel_flag.load() || shutting_down_.load() || !alive->load()) {
                return;
            }
        }

        session->is_passthrough.store(use_direct_passthrough);
        if (use_direct_passthrough) {
            std::string play_url = local_meta.direct_media_url;
            MainThreadDispatcher::Instance().Post([this, session, alive, local_meta, local_src, play_url]() {
                if (!alive->load() || shutting_down_.load() || session->cancel_flag.load()) return;

                session->metadata = local_meta;
                session->source_type = local_src;
                session->ready.store(true);
                session->state.store(session->pause_flag.load() ? StreamState::Paused : StreamState::Playing);

                host_.DispatchPawnCallback("OnAudioStreamReady", {
                    session->owner_player_id,
                    session->id,
                    ConvertTextForPawn(session->metadata.title, config_.pawn_text_encoding),
                    ConvertTextForPawn(session->metadata.artist, config_.pawn_text_encoding),
                    session->metadata.duration_seconds,
                    play_url
                });

                if (session->is_global) {
                    auto snapshots = host_.GetConnectedPlayerSnapshots();
                    std::vector<PlayerAudioCommand> cmds;
                    {
                        std::lock_guard<std::mutex> lock(state_mutex_);
                        if (!session->cancel_flag.load()) {
                            for (const auto& snap : snapshots) {
                                if (MatchesSpatialFilterSnapshot(snap, session->options)) {
                                    session->active_listeners.insert(snap.id);
                                    auto& gvec = player_arbitration_[snap.id].global_streams;
                                    if (std::find(gvec.begin(), gvec.end(), session->id) == gvec.end()) {
                                        gvec.push_back(session->id);
                                    }
                                    ReconcilePlayerLocked(snap.id, &snap, cmds);
                                }
                            }
                        }
                    }
                    ApplyPlayerAudioCommands(cmds);
                    host_.DispatchPawnCallback("OnAudioStreamStart", {INVALID_PLAYER_ID, session->id});
                } else {
                    std::vector<PlayerAudioCommand> cmds;
                    {
                        std::lock_guard<std::mutex> lock(state_mutex_);
                        if (!session->cancel_flag.load() && host_.IsPlayerConnected(session->owner_player_id)) {
                            if (MatchesSpatialFilter(session->owner_player_id, session->options)) {
                                session->active_listeners.insert(session->owner_player_id);
                            }
                            player_arbitration_[session->owner_player_id].personal_stream_id = session->id;
                            ReconcilePlayerLocked(session->owner_player_id, nullptr, cmds);
                        }
                    }
                    ApplyPlayerAudioCommands(cmds);
                    host_.DispatchPawnCallback("OnAudioStreamStart", {session->owner_player_id, session->id});
                }
            });
            return;
        }

        // Proxy & Transcode path
        bool missing_personal_remote_url = false;
        bool warn_global_remote_url = false;
        std::shared_ptr<AudioStreamBuffer> buf;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (session->cancel_flag.load() || shutting_down_.load() || !alive->load()) {
                return;
            }
            if (public_host_ip_.empty() && public_ip_failed_.load()) {
                if (!session->is_global && !session->owner_is_loopback) {
                    missing_personal_remote_url = true;
                    session->remote_url_error_reported = true;
                    session->state.store(StreamState::Error);
                } else if (session->is_global && !session->remote_url_error_reported) {
                    session->remote_url_error_reported = true;
                    warn_global_remote_url = true;
                }
            }
            if (!missing_personal_remote_url) {
                buf = std::make_shared<AudioStreamBuffer>(CalculateRingBufferBytes());
                session->buffer = buf;
                session->state.store(session->pause_flag.load() ? StreamState::Paused : StreamState::Buffering);

                StreamEndpointInfo ep;
                ep.token = session->token;
                ep.title = local_meta.title;
                ep.artist = local_meta.artist;
                ep.bitrate_kbps = config_.mp3_bitrate_kbps;
                ep.is_live_channel = session->is_global;
                ep.buffer = buf;
                http_server_.RegisterEndpoint(ep);
            }
        }

        if (missing_personal_remote_url) {
            if (!public_ip_error_logged_.exchange(true)) {
                Logger::Instance().Error(
                    "Cannot deliver proxied stream URL to remote player: public IPv4 detection failed; configure 'public_ip'.");
            }
            MainThreadDispatcher::Instance().Post([this, session, alive]() {
                if (!alive->load() || shutting_down_.load()) return;
                host_.DispatchPawnCallback("OnAudioStreamError", {
                    session->owner_player_id,
                    session->id,
                    static_cast<std::int32_t>(ErrorCode::NetworkError),
                    std::string("Failed to generate stream URL for remote player; configure 'public_ip'.")
                });
                StopAudioStream(session->id);
            });
            return;
        }
        if (warn_global_remote_url) {
            MainThreadDispatcher::Instance().Post([this, session, alive]() {
                if (!alive->load() || shutting_down_.load()) return;
                host_.DispatchPawnCallback("OnAudioStreamError", {
                    INVALID_PLAYER_ID,
                    session->id,
                    static_cast<std::int32_t>(ErrorCode::NetworkError),
                    std::string("Public IPv4 detection failed; remote players cannot receive global stream until 'public_ip' is configured.")
                });
            });
        }

        TranscoderConfig tcfg;
        tcfg.target_sample_rate = config_.mp3_sample_rate;
        tcfg.target_bitrate_kbps = config_.mp3_bitrate_kbps;
        tcfg.initial_burst_ms = std::clamp<std::uint32_t>(config_.transcoder_burst_ms, 2000, 15000);
        tcfg.max_prefetch_mb = std::clamp<std::uint32_t>(config_.max_prefetch_mb, 1, 64);
        tcfg.pace_realtime = !local_meta.is_live;
        tcfg.emit_silence_on_pause = false;

        // Publish metadata and start client playback on the main thread before/alongside transcode
        MainThreadDispatcher::Instance().Post([this, session, alive, local_meta, local_src]() {
            if (!alive->load() || shutting_down_.load() || session->cancel_flag.load()) return;

            std::string endpoint_url;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                if (session->is_global) {
                    endpoint_url = public_host_ip_.empty() ? "" : BuildEndpointUrlLocked("stream", session->token);
                } else {
                    endpoint_url = BuildEndpointUrlForPlayerLocked("stream", session->token, session->owner_player_id);
                }
            }

            session->metadata = local_meta;
            session->source_type = local_src;
            session->ready.store(true);
            session->state.store(session->pause_flag.load() ? StreamState::Paused : StreamState::Playing);

            host_.DispatchPawnCallback("OnAudioStreamReady", {
                session->owner_player_id,
                session->id,
                ConvertTextForPawn(session->metadata.title, config_.pawn_text_encoding),
                ConvertTextForPawn(session->metadata.artist, config_.pawn_text_encoding),
                session->metadata.duration_seconds,
                endpoint_url
            });

            if (session->is_global) {
                auto snapshots = host_.GetConnectedPlayerSnapshots();
                std::vector<PlayerAudioCommand> cmds;
                {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    if (!session->cancel_flag.load()) {
                        for (const auto& snap : snapshots) {
                            if (MatchesSpatialFilterSnapshot(snap, session->options)) {
                                session->active_listeners.insert(snap.id);
                                auto& gvec = player_arbitration_[snap.id].global_streams;
                                if (std::find(gvec.begin(), gvec.end(), session->id) == gvec.end()) {
                                    gvec.push_back(session->id);
                                }
                                ReconcilePlayerLocked(snap.id, &snap, cmds);
                            }
                        }
                    }
                }
                ApplyPlayerAudioCommands(cmds);
                host_.DispatchPawnCallback("OnAudioStreamStart", {INVALID_PLAYER_ID, session->id});
            } else {
                std::vector<PlayerAudioCommand> cmds;
                {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    if (!session->cancel_flag.load() && host_.IsPlayerConnected(session->owner_player_id)) {
                        if (MatchesSpatialFilter(session->owner_player_id, session->options)) {
                            session->active_listeners.insert(session->owner_player_id);
                        }
                        player_arbitration_[session->owner_player_id].personal_stream_id = session->id;
                        ReconcilePlayerLocked(session->owner_player_id, nullptr, cmds);
                    }
                }
                ApplyPlayerAudioCommands(cmds);
                host_.DispatchPawnCallback("OnAudioStreamStart", {session->owner_player_id, session->id});
            }
        });

        // Schedule transcode on owned scheduler (never detached; always joined on Shutdown)
        HttpStreamingServer* http_srv_ptr = &http_server_;
        std::atomic<bool>* shutdown_ptr = &shutting_down_;
        auto enqueued_at = std::chrono::steady_clock::now();

        bool queued = transcode_scheduler_.Enqueue([this, session, alive, local_meta, tcfg, buf, http_srv_ptr, shutdown_ptr, enqueued_at]() {
            if (session->cancel_flag.load() || shutdown_ptr->load() || !alive->load()) {
                http_srv_ptr->UnregisterEndpoint(session->token);
                return;
            }

            if ((std::chrono::steady_clock::now() - enqueued_at) > std::chrono::seconds(5)) {
                Logger::Instance().Warn("Transcoder worker waited > 5s in queue for stream " + std::to_string(session->id));
                http_srv_ptr->UnregisterEndpoint(session->token);
                session->state.store(StreamState::Error);
                MainThreadDispatcher::Instance().Post([this, session, alive]() {
                    if (!alive->load() || shutting_down_.load() || session->cancel_flag.load()) return;
                    host_.DispatchPawnCallback("OnAudioStreamError", {
                        session->owner_player_id,
                        session->id,
                        static_cast<std::int32_t>(ErrorCode::InternalError),
                        ConvertTextForPawn("Transcoder queue wait timed out after 5s", config_.pawn_text_encoding)
                    });
                    StopAudioStream(session->id);
                });
                return;
            }

            AudioTranscoder transcoder(local_meta, tcfg, buf);
            std::string trans_err;
            ErrorCode err_code = ErrorCode::TranscodeError;
            bool ok = transcoder.Run(session->cancel_flag, session->pause_flag, session->volume_percent, trans_err, &err_code);

            if (!session->cancel_flag.load() && !shutdown_ptr->load() && alive->load()) {
                if (ok) {
                    auto eof_at = std::chrono::steady_clock::now();
                    std::uint32_t lead_ms = tcfg.pace_realtime ? tcfg.initial_burst_ms : 0;
                    auto [min_dl, max_dl] = MakeCompletionDeadlines(eof_at, lead_ms);
                    MainThreadDispatcher::Instance().Post([this, session, alive, min_dl, max_dl]() {
                        if (!alive->load() || shutting_down_.load() || session->cancel_flag.load()) {
                            http_server_.UnregisterEndpoint(session->token);
                            return;
                        }
                        std::lock_guard<std::mutex> lock(state_mutex_);
                        if (streams_.find(session->id) == streams_.end() ||
                            session->cancel_flag.load() ||
                            session->state.load() == StreamState::Stopped) {
                            http_server_.UnregisterEndpoint(session->token);
                            return;
                        }
                        pending_stream_completions_.push_back(PendingStreamCompletion{session, min_dl, max_dl});
                    });
                } else {
                    http_srv_ptr->UnregisterEndpoint(session->token);
                    session->state.store(StreamState::Error);
                    if (trans_err == "URL not allowed") {
                        err_code = ErrorCode::InvalidUrl;
                    }
                    MainThreadDispatcher::Instance().Post([this, session, alive, err_code, trans_err]() {
                        if (!alive->load() || shutting_down_.load()) return;
                        host_.DispatchPawnCallback("OnAudioStreamError", {
                            session->owner_player_id,
                            session->id,
                            static_cast<std::int32_t>(err_code),
                            ConvertTextForPawn(trans_err, config_.pawn_text_encoding)
                        });
                        StopAudioStream(session->id);
                    });
                }
            } else {
                http_srv_ptr->UnregisterEndpoint(session->token);
            }
        }, &session->cancel_flag);

        if (!queued) {
            http_server_.UnregisterEndpoint(session->token);
            session->state.store(StreamState::Error);
            MainThreadDispatcher::Instance().Post([this, session, alive]() {
                if (!alive->load() || shutting_down_.load()) return;
                host_.DispatchPawnCallback("OnAudioStreamError", {
                    session->owner_player_id,
                    session->id,
                    static_cast<std::int32_t>(ErrorCode::InternalError),
                    "Failed to schedule transcoder worker"
                });
                StopAudioStream(session->id);
            });
        }
    })) {
        session->state.store(StreamState::Error);
    }
}

bool AudioEngine::StopAudioStreamForPlayer(PlayerId player_id, bool send_stop) {
    std::shared_ptr<ActiveStreamSession> session;
    std::shared_ptr<AudioStreamBuffer> buf;
    std::vector<PlayerAudioCommand> cmds;
    bool untracked_stop = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = player_streams_.find(player_id);
        if (it == player_streams_.end()) {
            if (!send_stop) {
                return false;
            }
            auto ait = player_arbitration_.find(player_id);
            if (ait == player_arbitration_.end() || ait->second.client_source_kind == AudioSourcePriority::None) {
                untracked_stop = true;
            }
        } else {
            CancelPendingCompletionLocked(it->second);
            auto sit = streams_.find(it->second);
            if (sit != streams_.end()) {
                session = sit->second;
                buf = session->buffer;
                streams_.erase(sit);
            }
            player_streams_.erase(it);

            auto ait = player_arbitration_.find(player_id);
            if (ait != player_arbitration_.end()) {
                ait->second.personal_stream_id = INVALID_STREAM_ID;
                if (!send_stop && ait->second.client_source_kind == AudioSourcePriority::Personal) {
                    ait->second.client_source_kind = AudioSourcePriority::None;
                    ait->second.client_source_id = 0;
                    ait->second.client_generation = 0;
                    ait->second.client_url.clear();
                }
            }
            if (send_stop) {
                ReconcilePlayerLocked(player_id, nullptr, cmds);
            }
        }
    }

    if (untracked_stop) {
        if (send_stop && host_.IsPlayerConnected(player_id)) {
            host_.SendStopAudioStream(player_id);
        }
        return false;
    }

    if (session) {
        session->cancel_flag.store(true);
        session->state.store(StreamState::Stopped);
        if (buf) buf->Close();
        http_server_.UnregisterEndpoint(session->token);
        HttpClient::PollCancellations();
    }
    if (send_stop) {
        ApplyPlayerAudioCommands(cmds);
    }
    return true;
}

bool AudioEngine::StopAudioStream(StreamId stream_id) {
    std::shared_ptr<ActiveStreamSession> session;
    std::shared_ptr<AudioStreamBuffer> buf;
    std::vector<PlayerAudioCommand> cmds;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = streams_.find(stream_id);
        if (it == streams_.end()) {
            return false;
        }
        CancelPendingCompletionLocked(stream_id);
        session = it->second;
        buf = session->buffer;
        streams_.erase(it);

        session->cancel_flag.store(true);
        session->state.store(StreamState::Stopped);
        DetachSessionFromArbitrationLocked(session, cmds);
    }

    if (buf) buf->Close();
    http_server_.UnregisterEndpoint(session->token);
    HttpClient::PollCancellations();
    ApplyPlayerAudioCommands(cmds);
    return true;
}

bool AudioEngine::PauseAudioStreamForPlayer(PlayerId player_id, bool pause) {
    std::shared_ptr<ActiveStreamSession> session;
    std::vector<PlayerAudioCommand> cmds;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = player_streams_.find(player_id);
        if (it == player_streams_.end()) return false;
        auto sit = streams_.find(it->second);
        if (sit == streams_.end()) return false;
        session = sit->second;
        StreamState cur = session->state.load();
        if (cur == StreamState::Stopped || cur == StreamState::Error || cur == StreamState::Finished) {
            return false;
        }

        bool is_ready = session->ready.load();
        session->pause_flag.store(pause);
        if (pause) {
            session->state.store(StreamState::Paused);
            if (!ShouldUseDirectPassthrough(*session)) {
                session->paused_offset = http_server_.GetEndpointPlayedOffset(session->token);
            }
        } else {
            StreamState next_state = is_ready
                ? StreamState::Playing
                : (session->buffer ? StreamState::Buffering : StreamState::Resolving);
            session->state.store(next_state);
            if (is_ready) {
                std::uint32_t lead_ms = session->metadata.is_live
                    ? 0
                    : std::clamp<std::uint32_t>(config_.transcoder_burst_ms, 2000, 15000);
                auto [min_dl, max_dl] = MakeCompletionDeadlines(std::chrono::steady_clock::now(), lead_ms);
                for (auto& pending : pending_stream_completions_) {
                    if (pending.session == session) {
                        pending.min_deadline = min_dl;
                        pending.max_deadline = max_dl;
                    }
                }
            }
        }
        ReconcilePlayerLocked(player_id, nullptr, cmds);
    }

    ApplyPlayerAudioCommands(cmds);
    return true;
}

bool AudioEngine::SetPlayerAudioVolume(PlayerId player_id, float volume) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = player_streams_.find(player_id);
    if (it == player_streams_.end()) return false;
    auto sit = streams_.find(it->second);
    if (sit == streams_.end()) return false;
    sit->second->volume_percent.store(std::clamp(volume, 0.0f, 200.0f));
    return true;
}

bool AudioEngine::SetStreamVolume(StreamId stream_id, float volume) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = streams_.find(stream_id);
    if (it == streams_.end()) return false;
    it->second->volume_percent.store(std::clamp(volume, 0.0f, 200.0f));
    return true;
}

bool AudioEngine::AttachStreamToPlayer(StreamId stream_id, PlayerId target_player_id) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = streams_.find(stream_id);
    if (it == streams_.end()) return false;
    it->second->options.attach_player_id = target_player_id;
    it->second->options.attach_vehicle_id = 0;
    it->second->options.use_position = true;
    return true;
}

bool AudioEngine::AttachStreamToVehicle(StreamId stream_id, std::int32_t vehicle_id) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = streams_.find(stream_id);
    if (it == streams_.end()) return false;
    it->second->options.attach_vehicle_id = vehicle_id;
    it->second->options.attach_player_id = INVALID_PLAYER_ID;
    it->second->options.use_position = true;
    return true;
}

void AudioEngine::UpdateSpatialStreams(const std::vector<PlayerSnapshot>* precomputed_snapshots) {
    struct SpatialWorkItem {
        std::shared_ptr<ActiveStreamSession> session;
        PlayerId attach_player_id{INVALID_PLAYER_ID};
        std::int32_t attach_vehicle_id{0};
    };

    std::vector<SpatialWorkItem> active_spatial;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        active_spatial.reserve(streams_.size());
        for (const auto& [id, s] : streams_) {
            if (s->state.load() == StreamState::Playing && !s->cancel_flag.load() && !s->pause_flag.load() &&
                (s->is_global || s->options.world_id != -1 || s->options.interior_id != -1 || s->options.use_position)) {
                active_spatial.push_back(SpatialWorkItem{
                    s, s->options.attach_player_id, s->options.attach_vehicle_id
                });
            }
        }
    }

    if (active_spatial.empty()) return;

    std::vector<PlayerSnapshot> owned_snapshots;
    const std::vector<PlayerSnapshot>& snapshots = precomputed_snapshots
        ? *precomputed_snapshots
        : (owned_snapshots = host_.GetConnectedPlayerSnapshots());

    std::unordered_map<PlayerId, const PlayerSnapshot*> snapshot_by_id;
    snapshot_by_id.reserve(snapshots.size());
    for (const auto& snap : snapshots) {
        snapshot_by_id[snap.id] = &snap;
    }

    // Fetch dynamic attached positions outside state_mutex_
    struct UpdatedTransform {
        bool has_pos{false};
        Vector3 pos{};
        bool has_vw{false};
        std::int32_t world_id{-1};
        std::int32_t interior_id{-1};
    };
    std::vector<UpdatedTransform> transforms(active_spatial.size());
    for (std::size_t i = 0; i < active_spatial.size(); ++i) {
        if (active_spatial[i].attach_player_id != INVALID_PLAYER_ID) {
            auto it = snapshot_by_id.find(active_spatial[i].attach_player_id);
            if (it != snapshot_by_id.end()) {
                transforms[i].has_pos = true;
                transforms[i].pos = it->second->pos;
                transforms[i].has_vw = true;
                transforms[i].world_id = it->second->world_id;
                transforms[i].interior_id = it->second->interior_id;
            }
        } else if (active_spatial[i].attach_vehicle_id > 0) {
            Vector3 vpos{};
            if (host_.GetVehiclePos(active_spatial[i].attach_vehicle_id, vpos)) {
                transforms[i].has_pos = true;
                transforms[i].pos = vpos;
            }
        }
    }

    std::vector<PlayerAudioCommand> cmds;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        for (std::size_t i = 0; i < active_spatial.size(); ++i) {
            auto& session = active_spatial[i].session;
            if (session->cancel_flag.load() || session->pause_flag.load()) continue;
            if (session->state.load() != StreamState::Playing) continue;

            if (transforms[i].has_pos) {
                session->options.position = transforms[i].pos;
            }
            if (transforms[i].has_vw) {
                session->options.world_id = transforms[i].world_id;
                session->options.interior_id = transforms[i].interior_id;
            }

            if (session->is_global) {
                // Prune any listeners that disconnected before current snapshot
                for (auto lit = session->active_listeners.begin(); lit != session->active_listeners.end();) {
                    if (snapshot_by_id.find(*lit) == snapshot_by_id.end()) {
                        lit = session->active_listeners.erase(lit);
                    } else {
                        ++lit;
                    }
                }

                for (const auto& snap : snapshots) {
                    bool in_range = MatchesSpatialFilterSnapshot(snap, session->options);
                    bool was_listening = session->active_listeners.count(snap.id) > 0;

                    if (in_range && !was_listening) {
                        session->active_listeners.insert(snap.id);
                        auto& gvec = player_arbitration_[snap.id].global_streams;
                        if (std::find(gvec.begin(), gvec.end(), session->id) == gvec.end()) {
                            gvec.push_back(session->id);
                        }
                        ReconcilePlayerLocked(snap.id, &snap, cmds);
                    } else if (!in_range && was_listening) {
                        session->active_listeners.erase(snap.id);
                        auto ait = player_arbitration_.find(snap.id);
                        if (ait != player_arbitration_.end()) {
                            auto& gvec = ait->second.global_streams;
                            gvec.erase(std::remove(gvec.begin(), gvec.end(), session->id), gvec.end());
                        }
                        ReconcilePlayerLocked(snap.id, &snap, cmds);
                    }
                }
            } else if (session->owner_player_id != INVALID_PLAYER_ID) {
                auto it = snapshot_by_id.find(session->owner_player_id);
                if (it != snapshot_by_id.end()) {
                    bool in_range = MatchesSpatialFilterSnapshot(*it->second, session->options);
                    bool was_listening = session->active_listeners.count(session->owner_player_id) > 0;
                    if (in_range && !was_listening) {
                        session->active_listeners.insert(session->owner_player_id);
                    } else if (!in_range && was_listening) {
                        session->active_listeners.erase(session->owner_player_id);
                    }
                    ReconcilePlayerLocked(session->owner_player_id, it->second, cmds);
                }
            }
        }
    }

    ApplyPlayerAudioCommands(cmds);
}

SearchId AudioEngine::SearchAudio(PlayerId player_id, const std::string& query, std::uint32_t max_results) {
    if (!initialized_.load() || shutting_down_.load() || query.empty()) {
        return INVALID_SEARCH_ID;
    }

    SearchId sid = next_search_id_.fetch_add(1);
    auto search_cancel = std::make_shared<std::atomic<bool>>(false);
    bool cancelled_prev = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto prev_it = active_searches_.find(player_id);
        if (prev_it != active_searches_.end() && prev_it->second.cancel_flag) {
            prev_it->second.cancel_flag->store(true);
            cancelled_prev = true;
        }
        active_searches_[player_id] = ActiveSearchState{sid, search_cancel};
    }
    if (cancelled_prev) {
        resolver_.PollCancellations();
    }

    auto alive = alive_token_;
    if (!extraction_pool_.Post([this, player_id, sid, query, max_results, search_cancel, alive]() {
        if (shutting_down_.load() || !alive->load() || search_cancel->load()) return;

        SearchExecutionResult res = resolver_.Search(query, max_results, search_cancel.get());
        if (shutting_down_.load() || !alive->load() || search_cancel->load()) return;

        MainThreadDispatcher::Instance().Post([this, player_id, sid, search_cancel, res = std::move(res), alive]() {
            if (!alive->load() || shutting_down_.load() || search_cancel->load()) return;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                auto it = active_searches_.find(player_id);
                if (it == active_searches_.end() || it->second.id != sid) {
                    return; // Search was cancelled or superseded
                }
                active_searches_.erase(it);
            }

            if (!host_.IsPlayerConnected(player_id)) {
                return;
            }

            std::int32_t total = res.success ? static_cast<std::int32_t>(res.items.size()) : 0;
            if (res.success) {
                for (std::int32_t i = 0; i < total; ++i) {
                    const auto& item = res.items[static_cast<std::size_t>(i)];
                    host_.DispatchPawnCallback("OnAudioStreamSearchResultItem", {
                        player_id,
                        sid,
                        i,
                        ConvertTextForPawn(item.title, config_.pawn_text_encoding),
                        ConvertTextForPawn(item.artist, config_.pawn_text_encoding),
                        item.duration_seconds,
                        item.id
                    });
                }
            }
            host_.DispatchPawnCallback("OnAudioStreamSearchResults", {player_id, sid, total});
        });
    })) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = active_searches_.find(player_id);
        if (it != active_searches_.end() && it->second.id == sid) {
            active_searches_.erase(it);
        }
        return INVALID_SEARCH_ID;
    }

    return sid;
}

bool AudioEngine::CancelSearch(PlayerId player_id) {
    bool removed = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = active_searches_.find(player_id);
        if (it != active_searches_.end()) {
            if (it->second.cancel_flag) {
                it->second.cancel_flag->store(true);
            }
            active_searches_.erase(it);
            removed = true;
        }
    }
    if (removed) {
        resolver_.PollCancellations();
    }
    return removed;
}

ChannelId AudioEngine::CreateChannel(const std::string& name) {
    auto ch = std::make_shared<SyncChannel>();
    ch->id = next_channel_id_.fetch_add(1);
    ch->name = name;
    ch->token = "ch_" + GenerateToken();

    std::lock_guard<std::mutex> lock(state_mutex_);
    channels_[ch->id] = ch;
    return ch->id;
}

bool AudioEngine::DestroyChannel(ChannelId channel_id) {
    std::shared_ptr<SyncChannel> ch;
    std::shared_ptr<ChannelRunState> old_run;
    std::vector<PlayerAudioCommand> cmds;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channels_.find(channel_id);
        if (it == channels_.end()) return false;
        ch = it->second;

        CancelPendingChannelCompletionLocked(channel_id);
        old_run = ch->active_run;
        ch->active_run.reset();
        ch->state.store(StreamState::Stopped);
        channels_.erase(it);
        for (PlayerId pid : ch->listeners) {
            player_channels_.erase(pid);
            auto ait = player_arbitration_.find(pid);
            if (ait != player_arbitration_.end() && ait->second.channel_id == channel_id) {
                ait->second.channel_id = INVALID_CHANNEL_ID;
            }
            ReconcilePlayerLocked(pid, nullptr, cmds);
        }
        ch->listeners.clear();
    }

    if (old_run) old_run->cancel_flag.store(true);
    if (ch->buffer) ch->buffer->Close();
    http_server_.UnregisterEndpoint(ch->token);

    ApplyPlayerAudioCommands(cmds);
    return true;
}

bool AudioEngine::PlayChannelLocked(
    ChannelId channel_id,
    const std::string& url_or_query,
    std::shared_ptr<SyncChannel>& out_ch,
    std::shared_ptr<ChannelRunState>& out_run) {
    auto it = channels_.find(channel_id);
    if (it == channels_.end()) return false;
    auto& ch = it->second;

    std::size_t limit = EffectiveStreamConcurrencyLimit();
    if (CurrentStreamLoadLocked(INVALID_STREAM_ID, channel_id) + 1 > limit) {
        Logger::Instance().Warn(
            "Maximum concurrent streams/transcoders limit reached (" + std::to_string(limit) + ")");
        return false;
    }

    CancelPendingChannelCompletionLocked(channel_id);

    if (ch->active_run) {
        ch->active_run->cancel_flag.store(true);
    }

    if (!ch->buffer) {
        ch->buffer = std::make_shared<AudioStreamBuffer>(CalculateRingBufferBytes());
        StreamEndpointInfo ep;
        ep.token = ch->token;
        ep.title = ch->name;
        ep.bitrate_kbps = config_.mp3_bitrate_kbps;
        ep.is_live_channel = true;
        ep.live_burst_bytes = 48 * 1024;
        ep.buffer = ch->buffer;
        http_server_.RegisterEndpoint(ep);
        ch->next_generation = ch->buffer->CurrentGeneration();
    } else {
        ch->next_generation = ch->buffer->PrepareNextTrack();
    }

    out_run = std::make_shared<ChannelRunState>();
    out_run->generation = ch->next_generation;
    ch->active_run = out_run;
    ch->pause_flag.store(false);
    ch->state.store(StreamState::Resolving);
    out_ch = ch;
    return true;
}

bool AudioEngine::PlayChannel(ChannelId channel_id, const std::string& url_or_query) {
    std::shared_ptr<SyncChannel> ch;
    std::shared_ptr<ChannelRunState> run;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!PlayChannelLocked(channel_id, url_or_query, ch, run)) {
            return false;
        }
    }

    HttpClient::PollCancellations();
    StartChannelWorker(ch, run, url_or_query);
    return true;
}

void AudioEngine::AdvanceChannelOrEndLocked(
    const std::shared_ptr<SyncChannel>& channel,
    StreamState terminal_state,
    std::string& out_next_track,
    std::shared_ptr<ChannelRunState>& out_next_run,
    std::vector<PlayerAudioCommand>& out_cmds) {
    if (!channel->queue.empty()) {
        out_next_track = std::move(channel->queue.front());
        channel->queue.pop_front();
        channel->next_generation = channel->buffer->PrepareNextTrack();
        out_next_run = std::make_shared<ChannelRunState>();
        out_next_run->generation = channel->next_generation;
        channel->active_run = out_next_run;
        channel->state.store(StreamState::Resolving);
    } else {
        channel->state.store(terminal_state);
        channel->active_run.reset();
        for (PlayerId pid : channel->listeners) {
            ReconcilePlayerLocked(pid, nullptr, out_cmds);
        }
    }
}

void AudioEngine::HandleChannelTrackError(
    const std::shared_ptr<SyncChannel>& channel,
    const std::shared_ptr<ChannelRunState>& run,
    ErrorCode err_code,
    const std::string& err_msg) {
    if (!channel || !run || shutting_down_.load() || run->cancel_flag.load()) return;

    std::string next_track;
    std::shared_ptr<ChannelRunState> next_run;
    std::vector<PlayerAudioCommand> cmds;
    bool active_match = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (channel->active_run == run) {
            active_match = true;
            AdvanceChannelOrEndLocked(channel, StreamState::Error, next_track, next_run, cmds);
        }
    }
    if (!active_match) return;

    host_.DispatchPawnCallback("OnAudioStreamError", {
        INVALID_PLAYER_ID,
        static_cast<std::int32_t>(-channel->id - 1),
        static_cast<std::int32_t>(err_code),
        ConvertTextForPawn(err_msg, config_.pawn_text_encoding)
    });
    ApplyPlayerAudioCommands(cmds);
    if (next_run && !next_track.empty()) {
        StartChannelWorker(channel, next_run, next_track);
    }
}

void AudioEngine::StartChannelWorker(
    std::shared_ptr<SyncChannel> channel,
    std::shared_ptr<ChannelRunState> run,
    const std::string& url_or_query) {
    auto alive = alive_token_;
    auto queued_at = std::chrono::steady_clock::now();
    if (!extraction_pool_.Post([this, channel, run, url_or_query, alive, queued_at]() {
        if (run->cancel_flag.load() || shutting_down_.load() || !alive->load()) return;
        auto wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - queued_at).count();
        Logger::Instance().Debug("Channel " + std::to_string(channel->id) + " extraction_pool queue wait: " + std::to_string(wait_ms) + " ms");

        ResolveResult resolved = resolver_.Resolve(url_or_query, &run->cancel_flag);
        if (run->cancel_flag.load() || shutting_down_.load() || !alive->load()) return;

        if (!resolved.success) {
            Logger::Instance().Error(
                "Channel " + std::to_string(channel->id) + " resolve failed: " + resolved.error_message);
            ErrorCode err_code = resolved.error;
            std::string err_msg = resolved.error_message;
            MainThreadDispatcher::Instance().Post([this, channel, run, err_code, err_msg, alive]() {
                if (!alive->load() || shutting_down_.load() || run->cancel_flag.load()) return;
                HandleChannelTrackError(channel, run, err_code, err_msg);
            });
            return;
        }

        {
            std::string queued_next;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                if (channel->active_run == run && !channel->queue.empty()) {
                    queued_next = channel->queue.front();
                }
            }
            if (!queued_next.empty()) {
                PreResolveNextChannelTrack(channel, queued_next);
            }
        }

        TrackMetadata local_track = std::move(resolved.metadata);
        MainThreadDispatcher::Instance().Post([this, channel, run, local_track, alive]() {
            if (!alive->load() || shutting_down_.load() || run->cancel_flag.load()) return;
            std::vector<PlayerAudioCommand> cmds;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                if (channel->active_run != run) return;
                channel->current_track = local_track;
                channel->state.store(channel->pause_flag.load() ? StreamState::Paused : StreamState::Playing);
                for (PlayerId pid : channel->listeners) {
                    ReconcilePlayerLocked(pid, nullptr, cmds);
                }
            }
            http_server_.UpdateEndpointMetadata(channel->token, local_track.title, local_track.artist, channel->buffer);
            ApplyPlayerAudioCommands(cmds);
        });

        TranscoderConfig tcfg;
        tcfg.target_sample_rate = config_.mp3_sample_rate;
        tcfg.target_bitrate_kbps = config_.mp3_bitrate_kbps;
        tcfg.initial_burst_ms = std::clamp<std::uint32_t>(config_.transcoder_burst_ms, 2000, 15000);
        tcfg.max_prefetch_mb = std::clamp<std::uint32_t>(config_.max_prefetch_mb, 1, 64);
        tcfg.buffer_generation = run->generation;
        tcfg.pace_realtime = !local_track.is_live;
        tcfg.emit_silence_on_pause = true;

        std::atomic<bool>* shutdown_ptr = &shutting_down_;
        auto enqueued_at = std::chrono::steady_clock::now();
        bool queued = transcode_scheduler_.Enqueue([this, channel, run, local_track, tcfg, alive, shutdown_ptr, enqueued_at]() {
            if (run->cancel_flag.load() || shutdown_ptr->load() || !alive->load()) return;

            if ((std::chrono::steady_clock::now() - enqueued_at) > std::chrono::seconds(5)) {
                Logger::Instance().Warn("Channel " + std::to_string(channel->id) + " transcoder waited > 5s in queue.");
                MainThreadDispatcher::Instance().Post([this, channel, run, alive]() {
                    if (!alive->load() || shutting_down_.load() || run->cancel_flag.load()) return;
                    HandleChannelTrackError(channel, run, ErrorCode::InternalError, "Transcoder queue wait timed out after 5s");
                });
                return;
            }

            AudioTranscoder transcoder(local_track, tcfg, channel->buffer);
            std::string err;
            ErrorCode err_code = ErrorCode::TranscodeError;
            bool transcoder_ok = transcoder.Run(run->cancel_flag, channel->pause_flag, channel->volume_percent, err, &err_code);
            if (!transcoder_ok && !run->cancel_flag.load() && !shutdown_ptr->load() && alive->load()) {
                Logger::Instance().Error("Channel transcode failed: " + err);
                if (err == "URL not allowed") err_code = ErrorCode::InvalidUrl;
                MainThreadDispatcher::Instance().Post([this, channel, run, err_code, err, alive]() {
                    if (!alive->load() || shutting_down_.load() || run->cancel_flag.load()) return;
                    HandleChannelTrackError(channel, run, err_code, err);
                });
                return;
            }

            if (!run->cancel_flag.load() && !shutdown_ptr->load() && alive->load()) {
                auto eof_at = std::chrono::steady_clock::now();
                std::uint32_t lead_ms = tcfg.pace_realtime ? tcfg.initial_burst_ms : 0;
                auto [min_dl, max_dl] = MakeCompletionDeadlines(eof_at, lead_ms);
                MainThreadDispatcher::Instance().Post([this, channel, run, min_dl, max_dl, alive]() {
                    if (!alive->load() || shutting_down_.load() || run->cancel_flag.load()) return;
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    if (channel->active_run != run) return;
                    pending_channel_completions_.push_back(PendingChannelCompletion{channel, run, min_dl, max_dl});
                });
            }
        }, &run->cancel_flag);

        if (!queued) {
            Logger::Instance().Error("Failed to schedule channel transcoder worker for channel " + std::to_string(channel->id));
            MainThreadDispatcher::Instance().Post([this, channel, run, alive]() {
                if (!alive->load() || shutting_down_.load() || run->cancel_flag.load()) return;
                HandleChannelTrackError(channel, run, ErrorCode::InternalError, "Failed to schedule channel transcoder");
            });
        }
    })) {
        Logger::Instance().Error("Failed to schedule channel extraction worker for channel " + std::to_string(channel->id));
        MainThreadDispatcher::Instance().Post([this, channel, run, alive]() {
            if (!alive->load() || shutting_down_.load() || run->cancel_flag.load()) return;
            HandleChannelTrackError(channel, run, ErrorCode::InternalError, "Failed to schedule channel extraction worker");
        });
    }
}

bool AudioEngine::QueueChannel(ChannelId channel_id, const std::string& url_or_query) {
    std::shared_ptr<SyncChannel> ch_preresolve;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channels_.find(channel_id);
        if (it == channels_.end()) return false;
        if (!it->second->active_run &&
            CurrentStreamLoadLocked(INVALID_STREAM_ID, channel_id) >= EffectiveStreamConcurrencyLimit()) {
            Logger::Instance().Warn(
                "Maximum concurrent streams/transcoders limit reached (" +
                std::to_string(EffectiveStreamConcurrencyLimit()) + ")");
            return false;
        }
        bool should_preresolve = (it->second->active_run != nullptr && it->second->queue.empty());
        it->second->queue.push_back(url_or_query);
        if (should_preresolve) {
            ch_preresolve = it->second;
        }
    }
    if (ch_preresolve) {
        PreResolveNextChannelTrack(ch_preresolve, url_or_query);
    }
    return true;
}

bool AudioEngine::SkipChannel(ChannelId channel_id) {
    std::shared_ptr<SyncChannel> ch;
    std::shared_ptr<ChannelRunState> run;
    std::string next_track;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channels_.find(channel_id);
        if (it == channels_.end() || it->second->queue.empty()) return false;
        next_track = it->second->queue.front();
        if (!PlayChannelLocked(channel_id, next_track, ch, run)) {
            return false;
        }
        ch->queue.pop_front();
    }
    HttpClient::PollCancellations();
    StartChannelWorker(ch, run, next_track);
    return true;
}

bool AudioEngine::StopChannel(ChannelId channel_id) {
    std::shared_ptr<SyncChannel> ch;
    std::shared_ptr<ChannelRunState> old_run;
    std::vector<PlayerAudioCommand> cmds;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channels_.find(channel_id);
        if (it == channels_.end()) return false;
        ch = it->second;

        CancelPendingChannelCompletionLocked(channel_id);
        old_run = ch->active_run;
        ch->active_run.reset();
        ch->queue.clear();
        ch->state.store(StreamState::Stopped);
        for (PlayerId pid : ch->listeners) {
            ReconcilePlayerLocked(pid, nullptr, cmds);
        }
    }

    if (old_run) {
        old_run->cancel_flag.store(true);
        HttpClient::PollCancellations();
    }
    ApplyPlayerAudioCommands(cmds);
    return true;
}

bool AudioEngine::PauseChannel(ChannelId channel_id, bool pause) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = channels_.find(channel_id);
    if (it == channels_.end()) return false;
    auto& ch = *it->second;
    StreamState cur = ch.state.load();
    if (pause) {
        if (!ch.active_run || (cur != StreamState::Playing && cur != StreamState::Paused)) {
            return false;
        }
        ch.pause_flag.store(true);
        ch.state.store(StreamState::Paused);
    } else {
        if (!ch.active_run || cur != StreamState::Paused) {
            return false;
        }
        ch.pause_flag.store(false);
        ch.state.store(StreamState::Playing);
        std::uint32_t lead_ms = ch.current_track.is_live
            ? 0
            : std::clamp<std::uint32_t>(config_.transcoder_burst_ms, 2000, 15000);
        auto [min_dl, max_dl] = MakeCompletionDeadlines(std::chrono::steady_clock::now(), lead_ms);
        for (auto& pending : pending_channel_completions_) {
            if (pending.channel == it->second && pending.run == ch.active_run) {
                pending.min_deadline = min_dl;
                pending.max_deadline = max_dl;
            }
        }
    }
    return true;
}

bool AudioEngine::SetChannelVolume(ChannelId channel_id, float volume) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = channels_.find(channel_id);
    if (it == channels_.end()) return false;
    it->second->volume_percent.store(std::clamp(volume, 0.0f, 200.0f));
    return true;
}

bool AudioEngine::JoinChannel(PlayerId player_id, ChannelId channel_id) {
    if (!host_.IsPlayerConnected(player_id)) return false;

    std::vector<PlayerAudioCommand> cmds;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channels_.find(channel_id);
        if (it == channels_.end()) return false;

        auto prev_it = player_channels_.find(player_id);
        if (prev_it != player_channels_.end() && prev_it->second != channel_id) {
            auto old_cit = channels_.find(prev_it->second);
            if (old_cit != channels_.end()) {
                old_cit->second->listeners.erase(player_id);
            }
        }

        it->second->listeners.insert(player_id);
        player_channels_[player_id] = channel_id;
        player_arbitration_[player_id].channel_id = channel_id;
        ReconcilePlayerLocked(player_id, nullptr, cmds);
    }

    ApplyPlayerAudioCommands(cmds);
    return true;
}

bool AudioEngine::LeaveChannel(PlayerId player_id) {
    std::vector<PlayerAudioCommand> cmds;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = player_channels_.find(player_id);
        if (it == player_channels_.end()) return false;
        auto cit = channels_.find(it->second);
        if (cit != channels_.end()) {
            cit->second->listeners.erase(player_id);
        }
        player_channels_.erase(it);
        auto ait = player_arbitration_.find(player_id);
        if (ait != player_arbitration_.end()) {
            ait->second.channel_id = INVALID_CHANNEL_ID;
        }
        ReconcilePlayerLocked(player_id, nullptr, cmds);
    }
    ApplyPlayerAudioCommands(cmds);
    return true;
}

bool AudioEngine::IsPlayerInChannel(PlayerId player_id, ChannelId channel_id) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = player_channels_.find(player_id);
    return it != player_channels_.end() && it->second == channel_id;
}

ChannelId AudioEngine::GetPlayerChannelId(PlayerId player_id) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = player_channels_.find(player_id);
    return it != player_channels_.end() ? it->second : INVALID_CHANNEL_ID;
}

bool AudioEngine::GetStreamInfo(StreamId stream_id, TrackMetadata& out_meta, StreamState& out_state) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = streams_.find(stream_id);
    if (it == streams_.end()) return false;
    out_meta = it->second->metadata;
    out_meta.title = ConvertTextForPawn(out_meta.title, config_.pawn_text_encoding);
    out_meta.artist = ConvertTextForPawn(out_meta.artist, config_.pawn_text_encoding);
    out_state = it->second->state.load();
    return true;
}

StreamState AudioEngine::GetChannelState(ChannelId channel_id) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = channels_.find(channel_id);
    return (it != channels_.end()) ? it->second->state.load() : StreamState::Idle;
}

StreamId AudioEngine::GetPlayerStreamId(PlayerId player_id) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = player_streams_.find(player_id);
    return it != player_streams_.end() ? it->second : INVALID_STREAM_ID;
}

bool AudioEngine::IsExtractorReady() const {
    return binary_manager_.IsReady();
}

std::uint16_t AudioEngine::GetHttpPort() const {
    return http_server_.BoundPort();
}

std::string AudioEngine::GetPublicBaseUrl() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    const std::string& host_ip = public_host_ip_.empty() ? std::string("127.0.0.1") : public_host_ip_;
    return "http://" + host_ip + ":" + std::to_string(http_server_.BoundPort());
}

} // namespace sampaudiostream
