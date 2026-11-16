#include "sampaudiostream/engine/audio_engine.hpp"

#include "sampaudiostream/audio/transcoder.hpp"
#include "sampaudiostream/core/logger.hpp"
#include "sampaudiostream/network/http_client.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <thread>

namespace sampaudiostream {

namespace {

float DistanceSq(const Vector3& a, const Vector3& b) {
    float dx = a.x - b.x;
    float dy = a.y - b.y;
    float dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

bool ShouldUseDirectPassthrough(const ActiveStreamSession& session) {
    if (session.source_type != SourceType::DirectAudio) {
        return false;
    }
    if (std::abs(session.volume_percent.load() - 100.0f) >= 0.5f) {
        return false;
    }
    if (session.is_global && !session.metadata.is_live) {
        return false;
    }
    return true;
}

} // namespace

AudioEngine::AudioEngine(IPlatformHost& host, const PluginConfig& config)
    : host_(host),
      config_(config),
      pool_(config.worker_threads > 0 ? config.worker_threads : 4),
      binary_manager_(config_),
      metadata_cache_(config_.cache_max_entries, config_.cache_ttl_seconds),
      resolver_(config_, binary_manager_, metadata_cache_),
      last_spatial_update_(std::chrono::steady_clock::now()) {}

AudioEngine::~AudioEngine() {
    Shutdown();
}

bool AudioEngine::Initialize() {
    bool expected = false;
    if (!initialized_.compare_exchange_strong(expected, true)) {
        return true;
    }

    Logger::Instance().SetDebugEnabled(config_.debug_logging);

    std::uint16_t port = config_.http_port;
    if (port == 0) {
        std::int32_t samp_port = host_.GetServerPort();
        port = static_cast<std::uint16_t>(samp_port > 0 ? samp_port + 1 : 7778);
    }

    std::string err;
    if (!http_server_.Start(config_.bind_ip, port, err)) {
        Logger::Instance().Warn("Failed to bind HTTP server on port " + std::to_string(port) + " (" + err + "), trying ephemeral port...");
        if (!http_server_.Start(config_.bind_ip, 0, err)) {
            Logger::Instance().Error("Failed to start HTTP streaming server: " + err);
            initialized_.store(false);
            return false;
        }
    }

    // Determine public IP for clients
    if (!config_.public_ip.empty()) {
        public_host_ip_ = config_.public_ip;
    } else {
        std::string bind_ip = host_.GetServerBindIp();
        if (!bind_ip.empty() && bind_ip != "0.0.0.0") {
            public_host_ip_ = bind_ip;
        } else {
            public_host_ip_ = "127.0.0.1";
            pool_.Post([this]() {
                std::string detected = HttpClient::DetectPublicIPv4();
                if (!detected.empty() && detected != "127.0.0.1") {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    public_host_ip_ = detected;
                    Logger::Instance().Info("Detected public server IPv4: " + public_host_ip_);
                }
            });
        }
    }

    // Provision yt-dlp & QuickJS asynchronously
    binary_manager_.InitializeAsync(pool_);
    return true;
}

void AudioEngine::Shutdown() {
    bool expected = true;
    if (!initialized_.compare_exchange_strong(expected, false)) {
        return;
    }

    Reset();
    http_server_.Stop();
    pool_.Stop();
    MainThreadDispatcher::Instance().Clear();
}

void AudioEngine::Reset() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    for (auto& [id, session] : streams_) {
        session->cancel_flag.store(true);
        if (session->buffer) session->buffer->Close();
        http_server_.UnregisterEndpoint(session->token);
    }
    streams_.clear();
    player_streams_.clear();

    for (auto& [id, ch] : channels_) {
        ch->cancel_flag.store(true);
        if (ch->buffer) ch->buffer->Close();
        http_server_.UnregisterEndpoint(ch->token);
    }
    channels_.clear();
    player_channels_.clear();
    active_searches_.clear();
}

void AudioEngine::Tick() {
    Logger::Instance().Flush();
    MainThreadDispatcher::Instance().ProcessPending();

    auto now = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_spatial_update_).count();
    if (elapsed_ms >= static_cast<long long>(config_.spatial_update_interval_ms)) {
        last_spatial_update_ = now;
        UpdateSpatialStreams();
    }
}

void AudioEngine::OnPlayerDisconnect(PlayerId player_id) {
    StopAudioStreamForPlayer(player_id);
    LeaveChannel(player_id);
    CancelSearch(player_id);
}

std::string AudioEngine::GenerateToken() {
    std::uint64_t val = token_counter_.fetch_add(1);
    std::uint64_t now = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    std::ostringstream oss;
    oss << std::hex << std::setw(8) << std::setfill('0') << (now & 0xFFFFFFFFULL) << "_" << val;
    return oss.str();
}

std::string AudioEngine::BuildEndpointUrl(const std::string& prefix, const std::string& token) const {
    std::string ip;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        ip = public_host_ip_;
    }
    return "http://" + ip + ":" + std::to_string(http_server_.BoundPort()) + "/" + prefix + "/" + token + ".mp3";
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

StreamId AudioEngine::PlayAudioStreamForPlayer(PlayerId player_id, const std::string& url_or_query, const StreamOptions& options) {
    if (!initialized_.load() || !host_.IsPlayerConnected(player_id) || url_or_query.empty()) {
        return INVALID_STREAM_ID;
    }

    // Stop previous personal stream / leave radio channel
    StopAudioStreamForPlayer(player_id);
    LeaveChannel(player_id);

    auto session = std::make_shared<ActiveStreamSession>();
    session->id = next_stream_id_.fetch_add(1);
    session->token = GenerateToken();
    session->original_input = url_or_query;
    session->options = options;
    session->is_global = false;
    session->owner_player_id = player_id;
    session->volume_percent.store(std::clamp(options.volume, 0.0f, 200.0f));
    session->started_at = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (streams_.size() >= config_.max_concurrent_streams) {
            Logger::Instance().Warn("Maximum concurrent streams limit reached (" + std::to_string(config_.max_concurrent_streams) + ")");
            return INVALID_STREAM_ID;
        }
        streams_[session->id] = session;
        player_streams_[player_id] = session->id;
    }

    StartStreamWorker(session);
    return session->id;
}

StreamId AudioEngine::PlayAudioStream(const std::string& url_or_query, const StreamOptions& options) {
    if (!initialized_.load() || url_or_query.empty()) {
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
        if (streams_.size() >= config_.max_concurrent_streams) {
            Logger::Instance().Warn("Maximum concurrent streams limit reached.");
            return INVALID_STREAM_ID;
        }
        streams_[session->id] = session;
    }

    StartStreamWorker(session);
    return session->id;
}

void AudioEngine::StartStreamWorker(std::shared_ptr<ActiveStreamSession> session) {
    pool_.Post([this, session]() {
        if (session->cancel_flag.load() || !initialized_.load()) {
            return;
        }

        ResolveResult resolved = resolver_.Resolve(session->original_input);
        if (session->cancel_flag.load() || !initialized_.load()) {
            return;
        }

        if (!resolved.success) {
            session->state = StreamState::Error;
            ErrorCode err_code = resolved.error;
            std::string err_msg = resolved.error_message;
            MainThreadDispatcher::Instance().Post([this, session, err_code, err_msg]() {
                host_.DispatchPawnCallback("OnAudioStreamError", {
                    session->owner_player_id,
                    session->id,
                    static_cast<std::int32_t>(err_code),
                    err_msg
                });
                StopAudioStream(session->id);
            });
            return;
        }

        session->metadata = std::move(resolved.metadata);
        session->source_type = resolved.source_type;

        bool use_direct_passthrough = ShouldUseDirectPassthrough(*session);

        if (use_direct_passthrough) {
            session->state = StreamState::Playing;
            std::string play_url = session->metadata.direct_media_url;

            MainThreadDispatcher::Instance().Post([this, session, play_url]() {
                if (session->cancel_flag.load()) return;

                host_.DispatchPawnCallback("OnAudioStreamReady", {
                    session->owner_player_id,
                    session->id,
                    session->metadata.title,
                    session->metadata.artist,
                    session->metadata.duration_seconds,
                    play_url
                });

                if (session->is_global) {
                    for (PlayerId pid : host_.GetConnectedPlayers()) {
                        if (MatchesSpatialFilter(pid, session->options)) {
                            session->active_listeners.insert(pid);
                            host_.SendPlayAudioStream(pid, play_url, session->options.position, session->options.distance, session->options.use_position);
                        }
                    }
                    host_.DispatchPawnCallback("OnAudioStreamStart", {INVALID_PLAYER_ID, session->id});
                } else {
                    if (MatchesSpatialFilter(session->owner_player_id, session->options)) {
                        session->active_listeners.insert(session->owner_player_id);
                        host_.SendPlayAudioStream(
                            session->owner_player_id,
                            play_url,
                            session->options.position,
                            session->options.distance,
                            session->options.use_position);
                    }
                    host_.DispatchPawnCallback("OnAudioStreamStart", {session->owner_player_id, session->id});
                }
            });
            return;
        }

        // Proxy & Transcode path
        session->buffer = std::make_shared<AudioStreamBuffer>(2 * 1024 * 1024);
        session->state = StreamState::Buffering;

        StreamEndpointInfo ep;
        ep.token = session->token;
        ep.title = session->metadata.title;
        ep.artist = session->metadata.artist;
        ep.bitrate_kbps = config_.mp3_bitrate_kbps;
        ep.is_live_channel = session->is_global;
        ep.buffer = session->buffer;
        http_server_.RegisterEndpoint(ep);

        std::string endpoint_url = BuildEndpointUrl("stream", session->token);

        // Spawn dedicated transcoder thread so control_pool_ is never blocked by long tracks
        std::thread([this, session, endpoint_url]() {
            TranscoderConfig tcfg;
            tcfg.target_sample_rate = config_.mp3_sample_rate;
            tcfg.target_bitrate_kbps = config_.mp3_bitrate_kbps;
            tcfg.pace_realtime = (session->is_global && !session->metadata.is_live);

            AudioTranscoder transcoder(session->metadata, tcfg, session->buffer);

            // Notify Pawn & start client playback as soon as transcoder is ready
            session->state = StreamState::Playing;
            MainThreadDispatcher::Instance().Post([this, session, endpoint_url]() {
                if (session->cancel_flag.load()) return;

                host_.DispatchPawnCallback("OnAudioStreamReady", {
                    session->owner_player_id,
                    session->id,
                    session->metadata.title,
                    session->metadata.artist,
                    session->metadata.duration_seconds,
                    endpoint_url
                });

                if (session->is_global) {
                    for (PlayerId pid : host_.GetConnectedPlayers()) {
                        if (MatchesSpatialFilter(pid, session->options)) {
                            session->active_listeners.insert(pid);
                            host_.SendPlayAudioStream(pid, endpoint_url, session->options.position, session->options.distance, session->options.use_position);
                        }
                    }
                    host_.DispatchPawnCallback("OnAudioStreamStart", {INVALID_PLAYER_ID, session->id});
                } else {
                    if (MatchesSpatialFilter(session->owner_player_id, session->options)) {
                        session->active_listeners.insert(session->owner_player_id);
                        host_.SendPlayAudioStream(
                            session->owner_player_id,
                            endpoint_url,
                            session->options.position,
                            session->options.distance,
                            session->options.use_position);
                    }
                    host_.DispatchPawnCallback("OnAudioStreamStart", {session->owner_player_id, session->id});
                }
            });

            std::string trans_err;
            bool ok = transcoder.Run(session->cancel_flag, session->pause_flag, session->volume_percent, trans_err);

            if (!session->cancel_flag.load()) {
                if (ok) {
                    session->state = StreamState::Finished;
                    MainThreadDispatcher::Instance().Post([this, session]() {
                        host_.DispatchPawnCallback("OnAudioStreamFinished", {session->owner_player_id, session->id});
                    });
                } else {
                    session->state = StreamState::Error;
                    MainThreadDispatcher::Instance().Post([this, session, trans_err]() {
                        host_.DispatchPawnCallback("OnAudioStreamError", {
                            session->owner_player_id,
                            session->id,
                            static_cast<std::int32_t>(ErrorCode::TranscodeError),
                            trans_err
                        });
                    });
                }
            }

            // Give HTTP clients a moment to drain the remaining ring buffer before cleanup
            for (int i = 0; i < 30 && !session->cancel_flag.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            http_server_.UnregisterEndpoint(session->token);
        }).detach();
    });
}

bool AudioEngine::StopAudioStreamForPlayer(PlayerId player_id) {
    std::shared_ptr<ActiveStreamSession> session;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = player_streams_.find(player_id);
        if (it == player_streams_.end()) {
            host_.SendStopAudioStream(player_id);
            return false;
        }
        auto sit = streams_.find(it->second);
        if (sit != streams_.end()) {
            session = sit->second;
            streams_.erase(sit);
        }
        player_streams_.erase(it);
    }

    if (session) {
        session->cancel_flag.store(true);
        session->state = StreamState::Stopped;
        if (session->buffer) session->buffer->Close();
        http_server_.UnregisterEndpoint(session->token);
    }
    host_.SendStopAudioStream(player_id);
    return true;
}

bool AudioEngine::StopAudioStream(StreamId stream_id) {
    std::shared_ptr<ActiveStreamSession> session;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = streams_.find(stream_id);
        if (it == streams_.end()) {
            return false;
        }
        session = it->second;
        streams_.erase(it);
        if (!session->is_global && session->owner_player_id != INVALID_PLAYER_ID) {
            auto pit = player_streams_.find(session->owner_player_id);
            if (pit != player_streams_.end() && pit->second == stream_id) {
                player_streams_.erase(pit);
            }
        }
    }

    session->cancel_flag.store(true);
    session->state = StreamState::Stopped;
    if (session->buffer) session->buffer->Close();
    http_server_.UnregisterEndpoint(session->token);

    for (PlayerId pid : session->active_listeners) {
        if (host_.IsPlayerConnected(pid)) {
            host_.SendStopAudioStream(pid);
        }
    }
    session->active_listeners.clear();
    return true;
}

bool AudioEngine::PauseAudioStreamForPlayer(PlayerId player_id, bool pause) {
    std::shared_ptr<ActiveStreamSession> session;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = player_streams_.find(player_id);
        if (it == player_streams_.end()) return false;
        auto sit = streams_.find(it->second);
        if (sit == streams_.end()) return false;
        session = sit->second;
    }

    session->pause_flag.store(pause);
    session->state = pause ? StreamState::Paused : StreamState::Playing;
    if (pause) {
        host_.SendStopAudioStream(player_id);
    } else {
        std::string url = (session->source_type == SourceType::DirectAudio &&
                           std::abs(session->volume_percent.load() - 100.0f) < 0.5f)
                              ? session->metadata.direct_media_url
                              : BuildEndpointUrl("stream", session->token);
        if (MatchesSpatialFilter(player_id, session->options)) {
            host_.SendPlayAudioStream(
                player_id,
                url,
                session->options.position,
                session->options.distance,
                session->options.use_position);
        }
    }
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

void AudioEngine::UpdateSpatialStreams() {
    std::vector<std::shared_ptr<ActiveStreamSession>> snapshot;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        snapshot.reserve(streams_.size());
        for (const auto& [id, s] : streams_) {
            if (s->state == StreamState::Playing && (s->is_global || s->options.world_id != -1 || s->options.interior_id != -1 || s->options.use_position)) {
                snapshot.push_back(s);
            }
        }
    }

    if (snapshot.empty()) return;

    auto connected = host_.GetConnectedPlayers();

    for (auto& session : snapshot) {
        if (session->cancel_flag.load() || session->pause_flag.load()) continue;

        // Update attached entity position if configured
        if (session->options.attach_player_id != INVALID_PLAYER_ID) {
            Vector3 ppos{};
            if (host_.GetPlayerPos(session->options.attach_player_id, ppos)) {
                session->options.position = ppos;
                session->options.world_id = host_.GetPlayerVirtualWorld(session->options.attach_player_id);
                session->options.interior_id = host_.GetPlayerInterior(session->options.attach_player_id);
            }
        } else if (session->options.attach_vehicle_id > 0) {
            Vector3 vpos{};
            if (host_.GetVehiclePos(session->options.attach_vehicle_id, vpos)) {
                session->options.position = vpos;
            }
        }

        std::string play_url = ShouldUseDirectPassthrough(*session)
                                   ? session->metadata.direct_media_url
                                   : BuildEndpointUrl("stream", session->token);

        if (session->is_global) {
            for (PlayerId pid : connected) {
                bool in_range = MatchesSpatialFilter(pid, session->options);
                bool was_listening = session->active_listeners.count(pid) > 0;

                if (in_range && !was_listening) {
                    session->active_listeners.insert(pid);
                    host_.SendPlayAudioStream(pid, play_url, session->options.position, session->options.distance, session->options.use_position);
                } else if (!in_range && was_listening) {
                    session->active_listeners.erase(pid);
                    host_.SendStopAudioStream(pid);
                }
            }
        } else if (session->owner_player_id != INVALID_PLAYER_ID) {
            PlayerId pid = session->owner_player_id;
            bool in_range = MatchesSpatialFilter(pid, session->options);
            bool was_listening = session->active_listeners.count(pid) > 0;
            if (!in_range && was_listening) {
                session->active_listeners.erase(pid);
                host_.SendStopAudioStream(pid);
            } else if (in_range && !was_listening) {
                session->active_listeners.insert(pid);
                host_.SendPlayAudioStream(pid, play_url, session->options.position, session->options.distance, session->options.use_position);
            }
        }
    }
}

SearchId AudioEngine::SearchAudio(PlayerId player_id, const std::string& query, std::uint32_t max_results) {
    if (!initialized_.load() || query.empty()) {
        return INVALID_SEARCH_ID;
    }

    SearchId sid = next_search_id_.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        active_searches_[player_id] = sid;
    }

    pool_.Post([this, player_id, sid, query, max_results]() {
        SearchExecutionResult res = resolver_.Search(query, max_results);

        MainThreadDispatcher::Instance().Post([this, player_id, sid, res = std::move(res)]() {
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                auto it = active_searches_.find(player_id);
                if (it == active_searches_.end() || it->second != sid) {
                    return; // Search was cancelled or superseded
                }
                active_searches_.erase(it);
            }

            if (player_id != INVALID_PLAYER_ID && !host_.IsPlayerConnected(player_id)) {
                return;
            }

            std::int32_t total = res.success ? static_cast<std::int32_t>(res.items.size()) : 0;
            host_.DispatchPawnCallback("OnAudioSearchResults", {player_id, sid, total});

            if (res.success) {
                for (std::size_t i = 0; i < res.items.size(); ++i) {
                    const auto& item = res.items[i];
                    host_.DispatchPawnCallback("OnAudioSearchResultItem", {
                        player_id,
                        sid,
                        static_cast<std::int32_t>(i),
                        item.title,
                        item.artist,
                        item.duration_seconds,
                        item.id
                    });
                }
            }
        });
    });

    return sid;
}

bool AudioEngine::CancelSearch(PlayerId player_id) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return active_searches_.erase(player_id) > 0;
}

ChannelId AudioEngine::CreateChannel(const std::string& name) {
    auto ch = std::make_shared<SyncChannel>();
    ch->id = next_channel_id_.fetch_add(1);
    ch->name = name;
    ch->token = "ch_" + GenerateToken();
    ch->buffer = std::make_shared<AudioStreamBuffer>(2 * 1024 * 1024);

    StreamEndpointInfo ep;
    ep.token = ch->token;
    ep.title = name;
    ep.bitrate_kbps = config_.mp3_bitrate_kbps;
    ep.is_live_channel = true;
    ep.live_burst_bytes = 48 * 1024;
    ep.buffer = ch->buffer;
    http_server_.RegisterEndpoint(ep);

    std::lock_guard<std::mutex> lock(state_mutex_);
    channels_[ch->id] = ch;
    return ch->id;
}

bool AudioEngine::DestroyChannel(ChannelId channel_id) {
    std::shared_ptr<SyncChannel> ch;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channels_.find(channel_id);
        if (it == channels_.end()) return false;
        ch = it->second;
        channels_.erase(it);
        for (PlayerId pid : ch->listeners) {
            player_channels_.erase(pid);
        }
    }

    ch->cancel_flag.store(true);
    if (ch->buffer) ch->buffer->Close();
    http_server_.UnregisterEndpoint(ch->token);

    for (PlayerId pid : ch->listeners) {
        if (host_.IsPlayerConnected(pid)) {
            host_.SendStopAudioStream(pid);
        }
    }
    return true;
}

bool AudioEngine::PlayChannel(ChannelId channel_id, const std::string& url_or_query) {
    std::shared_ptr<SyncChannel> ch;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channels_.find(channel_id);
        if (it == channels_.end()) return false;
        ch = it->second;
        ch->cancel_flag.store(true);
    }

    // Small yield for any existing channel transcoder to notice cancel
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    ch->cancel_flag.store(false);
    ch->pause_flag.store(false);
    ch->buffer->Reset();
    ch->state = StreamState::Resolving;

    StartChannelWorker(ch, url_or_query);
    return true;
}

void AudioEngine::StartChannelWorker(std::shared_ptr<SyncChannel> channel, const std::string& url_or_query) {
    pool_.Post([this, channel, url_or_query]() {
        if (channel->cancel_flag.load() || !initialized_.load()) return;

        ResolveResult resolved = resolver_.Resolve(url_or_query);
        if (channel->cancel_flag.load() || !initialized_.load()) return;

        if (!resolved.success) {
            channel->state = StreamState::Error;
            return;
        }

        channel->current_track = std::move(resolved.metadata);
        channel->state = StreamState::Playing;
        http_server_.UpdateEndpointMetadata(channel->token, channel->current_track.title, channel->current_track.artist, channel->buffer);

        std::string url = BuildEndpointUrl("channel", channel->token);
        MainThreadDispatcher::Instance().Post([this, channel, url]() {
            if (channel->cancel_flag.load()) return;
            std::vector<PlayerId> listeners_copy;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                listeners_copy.assign(channel->listeners.begin(), channel->listeners.end());
            }
            for (PlayerId pid : listeners_copy) {
                if (host_.IsPlayerConnected(pid)) {
                    host_.SendPlayAudioStream(pid, url, Vector3{}, 0.0f, false);
                }
            }
        });

        std::thread([this, channel]() {
            TranscoderConfig tcfg;
            tcfg.target_sample_rate = config_.mp3_sample_rate;
            tcfg.target_bitrate_kbps = config_.mp3_bitrate_kbps;
            tcfg.pace_realtime = true;

            AudioTranscoder transcoder(channel->current_track, tcfg, channel->buffer);
            std::string err;
            transcoder.Run(channel->cancel_flag, channel->pause_flag, channel->volume_percent, err);

            if (!channel->cancel_flag.load() && initialized_.load()) {
                std::string next_track;
                {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    if (!channel->queue.empty()) {
                        next_track = std::move(channel->queue.front());
                        channel->queue.pop_front();
                    }
                }
                if (!next_track.empty()) {
                    channel->buffer->Reset();
                    StartChannelWorker(channel, next_track);
                } else {
                    channel->state = StreamState::Finished;
                }
            }
        }).detach();
    });
}

bool AudioEngine::QueueChannel(ChannelId channel_id, const std::string& url_or_query) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = channels_.find(channel_id);
    if (it == channels_.end()) return false;
    it->second->queue.push_back(url_or_query);
    return true;
}

bool AudioEngine::SkipChannel(ChannelId channel_id) {
    std::string next_track;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channels_.find(channel_id);
        if (it == channels_.end() || it->second->queue.empty()) return false;
        next_track = std::move(it->second->queue.front());
        it->second->queue.pop_front();
    }
    return PlayChannel(channel_id, next_track);
}

bool AudioEngine::StopChannel(ChannelId channel_id) {
    std::shared_ptr<SyncChannel> ch;
    std::vector<PlayerId> listeners_copy;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channels_.find(channel_id);
        if (it == channels_.end()) return false;
        ch = it->second;
        ch->queue.clear();
        listeners_copy.assign(ch->listeners.begin(), ch->listeners.end());
    }

    ch->cancel_flag.store(true);
    ch->state = StreamState::Stopped;
    for (PlayerId pid : listeners_copy) {
        if (host_.IsPlayerConnected(pid)) {
            host_.SendStopAudioStream(pid);
        }
    }
    return true;
}

bool AudioEngine::PauseChannel(ChannelId channel_id, bool pause) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = channels_.find(channel_id);
    if (it == channels_.end()) return false;
    it->second->pause_flag.store(pause);
    it->second->state = pause ? StreamState::Paused : StreamState::Playing;
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

    StopAudioStreamForPlayer(player_id);
    LeaveChannel(player_id);

    std::shared_ptr<SyncChannel> ch;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channels_.find(channel_id);
        if (it == channels_.end()) return false;
        ch = it->second;
        ch->listeners.insert(player_id);
        player_channels_[player_id] = channel_id;
    }

    if (ch->state == StreamState::Playing || ch->state == StreamState::Paused) {
        host_.SendPlayAudioStream(player_id, BuildEndpointUrl("channel", ch->token), Vector3{}, 0.0f, false);
    }
    return true;
}

bool AudioEngine::LeaveChannel(PlayerId player_id) {
    std::shared_ptr<SyncChannel> ch;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = player_channels_.find(player_id);
        if (it == player_channels_.end()) return false;
        auto cit = channels_.find(it->second);
        if (cit != channels_.end()) {
            cit->second->listeners.erase(player_id);
        }
        player_channels_.erase(it);
    }
    if (host_.IsPlayerConnected(player_id)) {
        host_.SendStopAudioStream(player_id);
    }
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
    out_state = it->second->state;
    return true;
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
    return "http://" + public_host_ip_ + ":" + std::to_string(http_server_.BoundPort());
}

} // namespace sampaudiostream
