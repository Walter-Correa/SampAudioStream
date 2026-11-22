#include "sampaudiostream/engine/audio_engine.hpp"

#include "sampaudiostream/audio/sound_file.hpp"
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
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
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

std::int32_t ComputeRemainingWaitMs(std::chrono::steady_clock::time_point release_at) {
    const auto rem = release_at - std::chrono::steady_clock::now();
    if (rem <= std::chrono::steady_clock::duration::zero()) {
        return 0;
    }
    const auto ceil_ms = std::chrono::ceil<std::chrono::milliseconds>(rem).count();
    return static_cast<std::int32_t>(
        std::min<std::int64_t>(ceil_ms, std::numeric_limits<std::int32_t>::max()));
}

bool ComputeAvailableAtReleaseTime(
    const TrackMetadata& meta,
    std::chrono::steady_clock::time_point& out_release_at) {
    if (meta.available_at_epoch_ms <= 0) {
        return false;
    }
    const auto now_epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const std::int64_t delta_ms = meta.available_at_epoch_ms - now_epoch_ms;
    if (delta_ms > 60000) {
        Logger::Instance().Warn(
            "Extractor reported implausible available_at (" + std::to_string(delta_ms) +
            " ms in future); ignoring wait");
        return false;
    }
    const std::int64_t wait_ms = (meta.available_at_epoch_ms + 300) - now_epoch_ms;
    if (wait_ms <= 0) {
        return false;
    }
    out_release_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
    return true;
}

bool EqualsIgnoreCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::string BytesToHex(const std::string& bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (unsigned char c : bytes) {
        out.push_back(kHex[(c >> 4) & 0x0Fu]);
        out.push_back(kHex[c & 0x0Fu]);
    }
    return out;
}

std::string HexToBytes(const std::string& hex) {
    if (hex.size() % 2 != 0) return "";
    auto hex_nibble = [](char c, unsigned char& out) -> bool {
        if (c >= '0' && c <= '9') { out = static_cast<unsigned char>(c - '0'); return true; }
        if (c >= 'a' && c <= 'f') { out = static_cast<unsigned char>(10 + (c - 'a')); return true; }
        if (c >= 'A' && c <= 'F') { out = static_cast<unsigned char>(10 + (c - 'A')); return true; }
        return false;
    };
    std::string out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        unsigned char hi = 0, lo = 0;
        if (!hex_nibble(hex[i], hi) || !hex_nibble(hex[i + 1], lo)) {
            return "";
        }
        out.push_back(static_cast<char>((hi << 4) | lo));
    }
    return out;
}

std::string FormatUtcIso8601Now() {
    std::time_t now_t = std::time(nullptr);
    std::tm tm_utc{};
#ifdef _WIN32
    gmtime_s(&tm_utc, &now_t);
#else
    gmtime_r(&now_t, &tm_utc);
#endif
    char buf[32]{};
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
    return std::string(buf);
}

bool AtomicReplaceFile(const std::filesystem::path& tmp_path, const std::filesystem::path& final_path, std::string& out_err) {
#ifdef _WIN32
    if (::MoveFileExW(
            tmp_path.wstring().c_str(),
            final_path.wstring().c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
        return true;
    }
    DWORD err_code = ::GetLastError();
    std::error_code ec;
    std::filesystem::rename(tmp_path, final_path, ec);
    if (!ec) return true;
    out_err = "MoveFileExW failed (" + std::to_string(err_code) + "), rename failed: " + DescribeSystemError(ec);
    return false;
#else
    std::error_code ec;
    std::filesystem::rename(tmp_path, final_path, ec);
    if (!ec) return true;
    out_err = DescribeSystemError(ec);
    return false;
#endif
}

bool ParseChannelFileId(const std::filesystem::path& p, ChannelId& out_id) {
    const std::string fname = p.filename().string();
    if (fname.size() <= 13 || fname.rfind("channel_", 0) != 0 ||
        fname.compare(fname.size() - 5, 5, ".json") != 0) {
        return false;
    }
    const std::string num_str = fname.substr(8, fname.size() - 13);
    if (num_str.empty()) return false;
    for (char c : num_str) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    try {
        long val = std::stol(num_str);
        if (val <= 0) return false;
        out_id = static_cast<ChannelId>(val);
        return true;
    } catch (...) {
        return false;
    }
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

bool StartsWithHttpOrHttps(const std::string& url) {
    if (StartsWithHttpPlain(url)) return true;
    if (url.size() < 8) return false;
    auto eq = [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == b;
    };
    return eq(url[0], 'h') && eq(url[1], 't') && eq(url[2], 't') &&
           eq(url[3], 'p') && eq(url[4], 's') && url[5] == ':' &&
           url[6] == '/' && url[7] == '/';
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
    bool is_live,
    bool process_direct_audio_files) {
    if (source_type != SourceType::DirectAudio) {
        return false;
    }
    if (!StartsWithHttpOrHttps(direct_url)) {
        return false;
    }
    if (StreamResolver::IsHlsPlaylistUrl(direct_url)) {
        // Legacy GTA SA BASS does not support HLS (.m3u8) playlists natively
        return false;
    }
    if (!StreamResolver::IsKnownDirectAudioExtension(direct_url) &&
        StreamResolver::IsDirectAudioFileUrl(direct_url)) {
        // Container extensions not supported natively by GTA SA BASS (e.g. .m4a, .opus) must be transcoded
        return false;
    }
    if (process_direct_audio_files && !is_live && StreamResolver::IsDirectAudioFileUrl(direct_url)) {
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

// Replaces tab/CR/LF control characters with spaces and truncates to at most 128 UTF-8 bytes
// without splitting a multi-byte UTF-8 sequence.
std::string SanitizeQueueLabelUtf8(const std::string& raw) {
    if (raw.empty()) {
        return {};
    }
    std::string cleaned;
    cleaned.reserve(raw.size());
    for (char ch : raw) {
        if (ch == '\t' || ch == '\r' || ch == '\n') {
            cleaned.push_back(' ');
        } else {
            cleaned.push_back(ch);
        }
    }
    return TruncateUtf8Bytes(cleaned, 128);
}

// Removes control characters, trims outer spaces and truncates to at most 256 UTF-8 bytes.
std::string SanitizeDescriptionUtf8(const std::string& raw) {
    return raw.empty() ? std::string() : SanitizeControlAndTrimUtf8(raw, 256);
}

std::string DescriptionForPawn(const std::string& utf8, const std::string& pawn, const std::string& encoding) {
    if (!pawn.empty() || utf8.empty()) {
        return pawn;
    }
    return ConvertTextForPawn(utf8, encoding);
}

void CopyQueueEntryDescription(ChannelQueueEntry& dst, const ChannelQueueEntry& src) {
    dst.utf8_description = src.utf8_description;
    dst.pawn_description = src.pawn_description;
}

// Removes control characters (< 0x20), trims outer spaces, and truncates to <= 256 UTF-8 bytes.
std::string CleanLiveTitleUtf8(const std::string& utf8) {
    return SanitizeControlAndTrimUtf8(utf8, 256);
}

// Second part of the client on-screen text for sessions: the platform of extracted tracks, or the
// configured audio file label for direct/library files.
std::string SessionSourceLabel(const TrackMetadata& meta, const std::string& audio_file_label) {
    std::string key = meta.extractor_name;
    std::string lower = key;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower.empty() || lower == "direct" || lower == "library" || lower == "generic") {
        return audio_file_label;
    }
    static const std::pair<const char*, const char*> kPlatforms[] = {
        {"youtube", "YouTube"}, {"soundcloud", "SoundCloud"}, {"twitch", "Twitch"}, {"vimeo", "Vimeo"},
        {"tiktok", "TikTok"}, {"kick", "Kick"}, {"bandcamp", "Bandcamp"}, {"mixcloud", "Mixcloud"},
        {"dailymotion", "Dailymotion"}, {"facebook", "Facebook"}, {"instagram", "Instagram"},
        {"twitter", "X"}, {"spotify", "Spotify"}, {"deezer", "Deezer"}, {"audiomack", "Audiomack"},
    };
    for (const auto& [needle, label] : kPlatforms) {
        if (lower.find(needle) != std::string::npos) {
            return label;
        }
    }
    const auto colon = key.find(':');
    return (colon == std::string::npos) ? key : key.substr(0, colon);
}

bool IsGenericRadioTrackTitle(const TrackMetadata& meta) {
    return meta.title_is_placeholder ||
           meta.title.empty() ||
           meta.title == "Direct Audio Stream" ||
           meta.title == "Radio Stream";
}

} // namespace

bool AudioEngine::ShouldUseDirectPassthrough(const ActiveStreamSession& session) const {
    return session.is_passthrough.load();
}

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
      priority_order_(ParsePriorityOrder()),
      queue_shuffle_rng_(std::random_device{}()) {}

AudioEngine::~AudioEngine() {
    Shutdown();
}

bool AudioEngine::Initialize() {
    bool expected = false;
    if (!initialized_.compare_exchange_strong(expected, true)) {
        return true;
    }

    shutting_down_.store(false);
    lifecycle_stopping_.store(false);
    alive_token_->store(true);
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        gamemode_loaded_ = !host_.TracksGameModeLifecycle();
        has_dirty_channels_ = false;
        pending_restore_notifications_ = false;
        pending_restored_channel_ids_.clear();
    }
    public_ip_failed_.store(false);
    public_ip_error_logged_.store(false);
    resolver_.ResetCancel();
    HttpClient::Initialize();

    Logger::Instance().SetDebugEnabled(config_.debug_logging);

    std::string http_support_err;
    if (!HttpClient::CheckRuntimeSupport(http_support_err)) {
        Logger::Instance().Error(http_support_err);
    }

    if (!config_.music_dir.empty()) {
        std::error_code dir_ec;
        std::filesystem::create_directories(Utf8ToPath(config_.music_dir), dir_ec);
        if (dir_ec) {
            Logger::Instance().Warn("Failed to create audio.music_dir: " + DescribeSystemError(dir_ec));
        }
    }
    if (!config_.sound_dir.empty()) {
        std::error_code dir_ec;
        std::filesystem::create_directories(Utf8ToPath(config_.sound_dir), dir_ec);
        if (dir_ec) {
            Logger::Instance().Warn("Failed to create audio.sound_dir: " + DescribeSystemError(dir_ec));
        }
        if (!config_.music_dir.empty()) {
            std::error_code canon_ec;
            const auto sound_path = std::filesystem::weakly_canonical(Utf8ToPath(config_.sound_dir), canon_ec);
            const auto music_path = std::filesystem::weakly_canonical(Utf8ToPath(config_.music_dir), canon_ec);
            auto same_or_inside = [](const std::filesystem::path& p, const std::filesystem::path& base) {
                const std::filesystem::path rel = p.lexically_relative(base);
                return !rel.empty() && *rel.begin() != "..";
            };
            if (!canon_ec && (same_or_inside(sound_path, music_path) || same_or_inside(music_path, sound_path))) {
                Logger::Instance().Warn(
                    "audio.sound_dir and audio.music_dir should be separate folders: "
                    "files inside the music folder are listed in the music library.");
            }
        }
    }

    const std::size_t ext_workers = std::clamp<std::size_t>(
        config_.worker_threads > 0 ? config_.worker_threads : 4, 2, 16);
    const std::size_t max_transcoders = std::clamp<std::size_t>(
        config_.max_concurrent_transcoders > 0 ? config_.max_concurrent_transcoders : 32, 1, 128);

    control_pool_.Start(2, 512 * 1024);
    extraction_pool_.Start(ext_workers, 512 * 1024);
    transcode_scheduler_.Start(max_transcoders, 256 * 1024);

    std::vector<std::uint16_t> http_ports;
    {
        std::string port_err;
        if (!ParseHttpPortList(config_.http_port, http_ports, port_err)) {
            ParseHttpPortList(kDefaultHttpPortList, http_ports, port_err);
        }
    }

    const std::int32_t max_players = host_.GetMaxPlayers();
    http_server_.SetConnectionLimits(
        ComputeHttpConnectionLimit(max_players),
        config_.max_connections_per_ip > 0 ? config_.max_connections_per_ip : 64,
        max_players);

    // Ports are tried in list order, so the first free one wins without delay. When every port is busy
    // (typically the previous server process still releasing its socket after a quick restart), the list is
    // retried every 250 ms for up to 3 s before falling back to an OS-assigned port.
    std::string err;
    std::string first_err;
    bool http_started = false;
    const auto port_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
    for (bool first_pass = true; !http_started; first_pass = false) {
        for (std::uint16_t candidate : http_ports) {
            if (http_server_.Start(config_.bind_ip, candidate, err, config_.log_http_listen)) {
                http_started = true;
                if (candidate != http_ports.front()) {
                    Logger::Instance().Warn(
                        "HTTP port " + std::to_string(http_ports.front()) + " was unavailable (" + first_err +
                        "); using port " + std::to_string(candidate) + " from network.http_port.");
                } else if (!first_pass) {
                    Logger::Instance().Info(
                        "HTTP port " + std::to_string(candidate) + " became available after retrying (" + first_err + ").");
                }
                break;
            }
            if (first_err.empty()) {
                first_err = err;
            }
        }
        if (http_started || std::chrono::steady_clock::now() >= port_deadline) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    if (!http_started) {
        Logger::Instance().Warn(
            "No port from network.http_port ('" + config_.http_port + "') is available (" + first_err +
            "); using an OS-assigned port instead.");
        if (!http_server_.Start(config_.bind_ip, 0, err, config_.log_http_listen)) {
            Logger::Instance().Error("Failed to start HTTP streaming server: " + err);
            Shutdown();
            return false;
        }
        Logger::Instance().Warn(
            "HTTP streaming server bound to OS-assigned port " +
            std::to_string(http_server_.BoundPort()) +
            "; players behind a firewall cannot reach it until a port from network.http_port is free and open.");
    }

    local_interfaces_ = DiscoverLocalIpv4Interfaces(config_.bind_ip);
    resolver_.MaybePruneYtDlpCache(true);

    LoadPersistedChannelsOnInit();

    {
        std::lock_guard<std::mutex> slk(startup_log_mutex_);
        startup_wait_active_ = true;
        public_ip_auto_detect_ = false;
        startup_extractor_logged_ = false;
        startup_public_ip_logged_ = false;
    }

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
            {
                std::lock_guard<std::mutex> slk(startup_log_mutex_);
                public_ip_auto_detect_ = true;
            }
            auto alive = alive_token_;
            std::uint16_t bound_port = http_server_.BoundPort();
            extraction_pool_.Post([this, alive, bound_port]() {
                if (shutting_down_.load() || !alive->load()) {
                    {
                        std::lock_guard<std::mutex> iplock(public_ip_mutex_);
                        public_ip_resolved_ = true;
                    }
                    public_ip_cv_.notify_all();
                    return;
                }
                std::string detected = HttpClient::DetectPublicIPv4();
                if (!alive->load() || shutting_down_.load()) {
                    {
                        std::lock_guard<std::mutex> iplock(public_ip_mutex_);
                        public_ip_resolved_ = true;
                    }
                    public_ip_cv_.notify_all();
                    return;
                }
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
                    bool should_log_now = false;
                    {
                        std::lock_guard<std::mutex> slk(startup_log_mutex_);
                        if (!startup_wait_active_ && !startup_public_ip_logged_ && config_.log_public_ip_detected) {
                            startup_public_ip_logged_ = true;
                            should_log_now = true;
                        }
                    }
                    if (should_log_now) {
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
                        std::lock_guard<std::mutex> slk(startup_log_mutex_);
                        startup_public_ip_logged_ = true;
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
                                    BuildEndpointUrlForPlayerLocked("s", session->token, session->owner_player_id).empty()) {
                                    session->remote_url_error_reported = true;
                                    errs.emplace_back(session->owner_player_id, session->id);
                                }
                            }
                        }
                        for (const auto& [pid, sid] : errs) {
                            DispatchAudioCallback("OnAudioStreamError", {
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
    {
        auto alive = alive_token_;
        binary_manager_.InitializeAsync(extraction_pool_, [this, alive](bool ready) {
            if (!alive->load() || shutting_down_.load()) {
                return;
            }
            bool should_log_now = false;
            {
                std::lock_guard<std::mutex> slk(startup_log_mutex_);
                if (!startup_wait_active_ && ready && !startup_extractor_logged_ && config_.log_extractor_status) {
                    startup_extractor_logged_ = true;
                    should_log_now = true;
                }
            }
            if (should_log_now) {
                Logger::Instance().Info(
                    "Audio extractor subsystem ready (JS runtime: " +
                    binary_manager_.GetActiveJsRuntimeLabel() + ").");
            }
        });
    }
    return true;
}

void AudioEngine::WaitForStartupReadinessAndLog(
    const std::function<void()>& emit_banner,
    bool flush_before_banner,
    std::chrono::milliseconds timeout) {
    if (!initialized_.load() || shutting_down_.load()) {
        if (emit_banner) {
            emit_banner();
        }
        Logger::Instance().Flush();
        return;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::max(timeout, std::chrono::milliseconds(0));

    // Skip blocking for first-time extractor downloads (which can take seconds/minutes).
    const bool ext_needs_download = binary_manager_.NeedsInitialDownload();
    if (!ext_needs_download) {
        auto now = std::chrono::steady_clock::now();
        if (now < deadline) {
            auto rem = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
            binary_manager_.WaitUntilSettled(rem);
        }
    }

    bool ip_auto = false;
    {
        std::lock_guard<std::mutex> slk(startup_log_mutex_);
        ip_auto = public_ip_auto_detect_;
    }
    if (ip_auto) {
        auto now = std::chrono::steady_clock::now();
        std::unique_lock<std::mutex> iplock(public_ip_mutex_);
        if (!public_ip_resolved_ && now < deadline) {
            public_ip_cv_.wait_until(iplock, deadline, [this]() {
                return public_ip_resolved_ || shutting_down_.load();
            });
        }
    }

    bool log_ext_ready = false;
    bool log_ext_downloading = false;
    bool log_ext_pending = false;
    bool log_ip_ready = false;
    bool log_ip_pending = false;
    std::string detected_ip;

    {
        std::lock_guard<std::mutex> slk(startup_log_mutex_);
        startup_wait_active_ = false;

        const bool ext_settled = binary_manager_.IsSettled();
        const bool ext_ready = binary_manager_.IsReady();
        if (ext_ready && !startup_extractor_logged_) {
            startup_extractor_logged_ = true;
            log_ext_ready = config_.log_extractor_status;
        } else if (!ext_settled && config_.log_extractor_status) {
            if (ext_needs_download) {
                log_ext_downloading = true;
            } else if (config_.auto_download_extractor) {
                log_ext_pending = true;
            }
        }

        if (public_ip_auto_detect_) {
            bool ip_settled = false;
            {
                std::lock_guard<std::mutex> iplock(public_ip_mutex_);
                ip_settled = public_ip_resolved_;
            }
            if (ip_settled) {
                if (!startup_public_ip_logged_) {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    if (!public_host_ip_.empty()) {
                        detected_ip = public_host_ip_;
                        startup_public_ip_logged_ = true;
                        log_ip_ready = config_.log_public_ip_detected;
                    }
                }
            } else if (config_.log_public_ip_detected) {
                log_ip_pending = true;
            }
        }
    }

    if (flush_before_banner) {
        Logger::Instance().Flush();
    }
    if (emit_banner) {
        emit_banner();
    }
    if (log_ext_ready) {
        Logger::Instance().Info(
            "Audio extractor subsystem ready (JS runtime: " +
            binary_manager_.GetActiveJsRuntimeLabel() + ").");
    } else if (log_ext_downloading) {
        Logger::Instance().Info("Audio extractor: downloading in background...");
    } else if (log_ext_pending) {
        Logger::Instance().Info("Audio extractor: initializing in background...");
    }
    if (log_ip_ready) {
        Logger::Instance().Info("Detected public server IPv4: " + detected_ip);
    } else if (log_ip_pending) {
        Logger::Instance().Info("Public IP detection still running...");
    }
    Logger::Instance().Flush();
}

void AudioEngine::Shutdown() {
    bool expected_init = true;
    bool was_init = initialized_.compare_exchange_strong(expected_init, false);
    if (!was_init && shutting_down_.load()) {
        return;
    }

    // 1. Signal lifecycle stopping & shutdown and flush pending channel state before stopping pools
    lifecycle_stopping_.store(true);
    shutting_down_.store(true);
    alive_token_->store(false);
    FlushDirtyChannels(/*sync_all=*/true, /*bypass_debounce=*/true);
    {
        std::lock_guard<std::mutex> slk(startup_log_mutex_);
        startup_wait_active_ = false;
    }
    public_ip_cv_.notify_all();

    // 2. Cancel all sessions, channels, searches, extractor processes, and binary downloads
    binary_manager_.Cancel();
    resolver_.CancelAll();

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        for (auto& [id, session] : streams_) {
            session->cancel_flag.store(true);
            if (session->active_run) {
                session->active_run->cancel_flag.store(true);
            }
            session->state.store(StreamState::Stopped);
            if (session->buffer) session->buffer->Close();
            http_server_.UnregisterEndpoint(session->token);
        }
        ClearStreamSessionsLocked();
        player_streams_.clear();

        for (auto& [id, ch] : channels_) {
            if (ch->active_run) {
                ch->active_run->cancel_flag.store(true);
            }
            if (ch->playlist_cancel) {
                ch->playlist_cancel->store(true);
            }
            ch->state.store(StreamState::Stopped);
            if (ch->buffer) ch->buffer->Close();
            http_server_.UnregisterEndpoint(ch->token);
        }
        channels_.clear();
        sorted_channel_ids_.clear();
        player_channels_.clear();
        player_arbitration_.clear();
        for (const auto& [pid, snd] : player_sounds_) {
            (void)pid;
            http_server_.UnregisterFileEndpoint(snd.token);
        }
        player_sounds_.clear();
        for (auto& [pid, st] : active_searches_) {
            if (st.cancel_flag) st.cancel_flag->store(true);
        }
        active_searches_.clear();
        pending_stream_completions_.clear();
        pending_channel_completions_.clear();
        pending_delayed_stream_starts_.clear();
        pending_delayed_channel_starts_.clear();
        pending_connect_checks_.clear();
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

    // 6. Stop async HTTP cancellation closer thread once worker pools are stopped
    HttpClient::Shutdown();

    // 7. Clear pending main-thread callbacks
    MainThreadDispatcher::Instance().Clear();
}

void AudioEngine::Reset() {
    std::vector<std::shared_ptr<ActiveStreamSession>> old_streams;
    std::vector<std::shared_ptr<SyncChannel>> old_channels;
    std::vector<ChannelId> destroyed_channel_ids;
    std::vector<PlayerAudioCommand> cmds;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        for (auto& [id, session] : streams_) {
            session->cancel_flag.store(true);
            if (session->active_run) {
                session->active_run->cancel_flag.store(true);
            }
            session->state.store(StreamState::Stopped);
            if (session->buffer) session->buffer->Close();
            http_server_.UnregisterEndpoint(session->token);
            old_streams.push_back(session);
        }
        ClearStreamSessionsLocked();
        player_streams_.clear();
        pending_stream_completions_.clear();
        pending_delayed_stream_starts_.clear();
        pending_connect_checks_.clear();

        for (auto& [pid, st] : active_searches_) {
            if (st.cancel_flag) st.cancel_flag->store(true);
        }
        active_searches_.clear();

        std::unordered_set<PlayerId> affected_set;
        affected_set.reserve(player_arbitration_.size());

        std::unordered_set<ChannelId> destroyed_set;
        for (ChannelId cid : sorted_channel_ids_) {
            auto cit = channels_.find(cid);
            if (cit == channels_.end() || !cit->second || !cit->second->persistent) {
                destroyed_channel_ids.push_back(cid);
                destroyed_set.insert(cid);
            }
        }

        for (ChannelId cid : destroyed_channel_ids) {
            auto cit = channels_.find(cid);
            if (cit != channels_.end() && cit->second) {
                auto& ch = cit->second;
                if (ch->persist_cancelled) {
                    ch->persist_cancelled->store(true);
                }
                if (ch->active_run) {
                    ch->active_run->cancel_flag.store(true);
                }
                if (ch->playlist_cancel) {
                    ch->playlist_cancel->store(true);
                }
                ch->state.store(StreamState::Stopped);
                if (ch->buffer) ch->buffer->Close();
                http_server_.UnregisterEndpoint(ch->token);
                for (PlayerId pid : ch->listeners) {
                    player_channels_.erase(pid);
                    affected_set.insert(pid);
                }
                for (PlayerId pid : ch->range_listeners) {
                    affected_set.insert(pid);
                }
                old_channels.push_back(ch);
                channels_.erase(cit);
            }
            RemoveSortedChannelIdLocked(cid);
        }

        if (!destroyed_set.empty()) {
            pending_channel_completions_.erase(
                std::remove_if(pending_channel_completions_.begin(), pending_channel_completions_.end(),
                    [&](const PendingChannelCompletion& item) {
                        return !item.channel || destroyed_set.count(item.channel->id) > 0;
                    }),
                pending_channel_completions_.end());
            pending_delayed_channel_starts_.erase(
                std::remove_if(pending_delayed_channel_starts_.begin(), pending_delayed_channel_starts_.end(),
                    [&](const PendingDelayedChannelStart& item) {
                        return !item.channel || destroyed_set.count(item.channel->id) > 0;
                    }),
                pending_delayed_channel_starts_.end());
        }

        for (const auto& [pid, snd] : player_sounds_) {
            http_server_.UnregisterFileEndpoint(snd.token);
            affected_set.insert(pid);
        }
        player_sounds_.clear();

        for (auto& [pid, arb] : player_arbitration_) {
            bool changed = (arb.personal_stream_id != INVALID_STREAM_ID);
            arb.personal_stream_id = INVALID_STREAM_ID;
            if (arb.channel_id != INVALID_CHANNEL_ID && destroyed_set.count(arb.channel_id) > 0) {
                arb.channel_id = INVALID_CHANNEL_ID;
                changed = true;
            }
            auto before_sz = arb.global_sources.size();
            arb.global_sources.erase(
                std::remove_if(arb.global_sources.begin(), arb.global_sources.end(),
                    [](const GlobalAudioSourceRef& ref) { return !ref.is_channel; }),
                arb.global_sources.end());
            if (arb.global_sources.size() != before_sz) {
                changed = true;
            }
            if (changed) {
                affected_set.insert(pid);
            }
        }

        for (auto& [cid, ch] : channels_) {
            (void)cid;
            if (!ch) continue;
            if (ch->spatial.attach_player_id != INVALID_PLAYER_ID || ch->spatial.attach_vehicle_id > 0) {
                // Vehicles and player IDs do not survive GMX; keep last coordinates as fixed position
                ch->spatial.attach_player_id = INVALID_PLAYER_ID;
                ch->spatial.attach_vehicle_id = 0;
                ch->spatial.use_position = true;
                ++ch->spatial_version;
                MarkChannelDirtyLocked(*ch);
                for (PlayerId pid : ch->listeners) {
                    affected_set.insert(pid);
                }
                for (PlayerId pid : ch->range_listeners) {
                    affected_set.insert(pid);
                }
            }
        }

        for (PlayerId pid : affected_set) {
            ReconcilePlayerLocked(pid, nullptr, cmds);
        }

        // Keep connected players' mute flags across GMX; prune idle unmuted entries.
        for (auto it = player_arbitration_.begin(); it != player_arbitration_.end();) {
            if (!it->second.muted &&
                it->second.muted_range_groups == 0 &&
                it->second.personal_stream_id == INVALID_STREAM_ID &&
                it->second.channel_id == INVALID_CHANNEL_ID &&
                it->second.global_sources.empty() &&
                it->second.client_source_kind == AudioSourcePriority::None) {
                it = player_arbitration_.erase(it);
            } else {
                ++it;
            }
        }

        pending_restored_channel_ids_ = sorted_channel_ids_;
        pending_restore_notifications_ = !pending_restored_channel_ids_.empty();
        if (host_.TracksGameModeLifecycle()) {
            gamemode_loaded_ = false;
        }
    }

    resolver_.PollCancellations();
    if (pending_restored_channel_ids_.empty()) {
        MainThreadDispatcher::Instance().Clear();
    }
    ApplyPlayerAudioCommands(cmds);
    for (ChannelId cid : destroyed_channel_ids) {
        host_.DispatchPawnCallback("OnAudioStreamChannelDestroyed", {
            cid,
            static_cast<std::int32_t>(ChannelDestroyReason::Manual)
        });
    }
}

void AudioEngine::NotifyLifecycleStopping() {
    lifecycle_stopping_.store(true);
}

void AudioEngine::OnGameModeLoaded() {
    lifecycle_stopping_.store(false);
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        gamemode_loaded_ = true;
    }
    DispatchPendingRestoredCallbacks();
}

void AudioEngine::DispatchPendingRestoredCallbacks() {
    struct RestoredNotice {
        ChannelId id;
        std::string name;
        std::string creator_name;
    };
    std::vector<RestoredNotice> notices;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!gamemode_loaded_ || !pending_restore_notifications_) {
            return;
        }
        pending_restore_notifications_ = false;
        std::vector<ChannelId> ids;
        ids.swap(pending_restored_channel_ids_);
        notices.reserve(ids.size());
        for (ChannelId cid : ids) {
            auto it = channels_.find(cid);
            if (it == channels_.end() || !it->second) continue;
            notices.push_back(RestoredNotice{
                it->second->id,
                ConvertTextForPawn(it->second->name, config_.pawn_text_encoding),
                it->second->creator_name
            });
        }
    }
    for (const auto& n : notices) {
        host_.DispatchPawnCallback("OnAudioStreamChannelRestored", {
            n.id,
            n.name,
            n.creator_name
        });
    }
}

void AudioEngine::Tick() {
    Logger::Instance().Flush();
    if (!host_.TracksGameModeLifecycle()) {
        bool need_restore = false;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (!gamemode_loaded_) gamemode_loaded_ = true;
            need_restore = pending_restore_notifications_;
        }
        if (need_restore) {
            DispatchPendingRestoredCallbacks();
        }
    }
    MainThreadDispatcher::Instance().ProcessPending();
    resolver_.PollCancellations();
    HttpClient::PollCancellations();

    ResumeRestoredChannelsInTick();

    auto now = std::chrono::steady_clock::now();
    ProcessDelayedStarts(now);
    ProcessPendingCompletions(now);
    UpdatePlayerSounds(now);
    auto poll_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_disconnect_poll_).count();
    if (!host_.TracksDisconnectEvents()) {
        CheckDisconnectedPlayers();
    }
    const std::int64_t warn_timeout_ms = std::max<std::int64_t>(
        1, connect_warn_timeout_ms_.load(std::memory_order_relaxed));
    bool ran_unconnected_check = false;
    if (poll_elapsed_ms >= 1000) {
        last_disconnect_poll_ = now;
        if (host_.TracksDisconnectEvents()) {
            CheckDisconnectedPlayers();
        }
        CleanupExpiredPassthroughSessions(now);
        CleanupExpiredChannels(now);
        CheckUnconnectedClients(now);
        ran_unconnected_check = true;
        resolver_.MaybeScheduleCachePrune(extraction_pool_);
    }
    if (!ran_unconnected_check && warn_timeout_ms < 1000) {
        CheckUnconnectedClients(now);
    }
    if (has_dirty_channels_) {
        FlushDirtyChannels(/*sync_all=*/false, /*bypass_debounce=*/false);
    }

    binary_manager_.MaybePeriodicUpdate(extraction_pool_, resolver_.ActiveProcessCount());
    PollLiveTitles(now);

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
                auto& g = ait->second.global_sources;
                GlobalAudioSourceRef ref{false, sid};
                g.erase(std::remove(g.begin(), g.end(), ref), g.end());
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
    pending_delayed_stream_starts_.erase(
        std::remove_if(pending_delayed_stream_starts_.begin(), pending_delayed_stream_starts_.end(),
            [stream_id](const PendingDelayedStreamStart& p) {
                return !p.session || p.session->id == stream_id;
            }),
        pending_delayed_stream_starts_.end());
}

void AudioEngine::CancelPendingChannelCompletionLocked(ChannelId channel_id) {
    pending_channel_completions_.erase(
        std::remove_if(pending_channel_completions_.begin(), pending_channel_completions_.end(),
            [channel_id](const PendingChannelCompletion& p) {
                return !p.channel || p.channel->id == channel_id;
            }),
        pending_channel_completions_.end());
    pending_delayed_channel_starts_.erase(
        std::remove_if(pending_delayed_channel_starts_.begin(), pending_delayed_channel_starts_.end(),
            [channel_id](const PendingDelayedChannelStart& p) {
                return !p.channel || p.channel->id == channel_id;
            }),
        pending_delayed_channel_starts_.end());
}

void AudioEngine::ProcessDelayedStarts(std::chrono::steady_clock::time_point now) {
    std::vector<PendingDelayedStreamStart> due_streams;
    std::vector<PendingDelayedChannelStart> due_channels;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (shutting_down_.load()) {
            pending_delayed_stream_starts_.clear();
            pending_delayed_channel_starts_.clear();
            return;
        }
        for (auto it = pending_delayed_stream_starts_.begin(); it != pending_delayed_stream_starts_.end();) {
            const auto& s = it->session;
            if (!s || s->cancel_flag.load() || s->state.load() == StreamState::Stopped ||
                streams_.find(s->id) == streams_.end()) {
                it = pending_delayed_stream_starts_.erase(it);
                continue;
            }
            if (now >= it->release_at) {
                due_streams.push_back(std::move(*it));
                it = pending_delayed_stream_starts_.erase(it);
                continue;
            }
            ++it;
        }
        for (auto it = pending_delayed_channel_starts_.begin(); it != pending_delayed_channel_starts_.end();) {
            const auto& c = it->channel;
            const auto& r = it->run;
            if (!c || !r || r->cancel_flag.load() || c->active_run != r ||
                channels_.find(c->id) == channels_.end()) {
                it = pending_delayed_channel_starts_.erase(it);
                continue;
            }
            if (now >= it->release_at) {
                due_channels.push_back(std::move(*it));
                it = pending_delayed_channel_starts_.erase(it);
                continue;
            }
            ++it;
        }
    }

    for (auto& ds : due_streams) {
        CompleteResolvedStreamStart(ds.session, std::move(ds.metadata), ds.source_type, true);
    }
    for (auto& dc : due_channels) {
        CompleteResolvedChannelTrackStart(dc.channel, dc.run, std::move(dc.track));
    }
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
    std::vector<ChannelId> queue_ended_channels;
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
            if (!session->is_global && IsPersonalAudioSilencedLocked(session->owner_player_id)) {
                // The owner hears a sound or muted personal audio: the rest of the track is still due.
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
            RemoveStreamSessionLocked(session->id);
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
            bool queue_ended = false;
            AdvanceChannelOrEndLocked(ch, StreamState::Finished, next_track, next_run, cmds, queue_ended);
            if (next_run && !next_track.empty()) {
                channel_next_work.push_back({ch, next_run, std::move(next_track)});
            } else if (queue_ended) {
                queue_ended_channels.push_back(ch->id);
            }
            it = pending_channel_completions_.erase(it);
        }
    }

    for (const auto& tok : tokens_to_unregister) {
        http_server_.UnregisterEndpoint(tok);
    }
    for (const auto& notice : finished_notices) {
        DispatchAudioCallback("OnAudioStreamFinished", {notice.player_id, notice.stream_id});
    }
    ApplyPlayerAudioCommands(cmds);
    for (ChannelId cid : queue_ended_channels) {
        host_.DispatchPawnCallback("OnAudioStreamChannelQueueEnd", {cid});
    }
    for (const auto& cw : channel_next_work) {
        StartChannelWorker(cw.channel, cw.run, cw.next_track);
    }
}

void AudioEngine::CleanupExpiredPassthroughSessions(std::chrono::steady_clock::time_point now) {
    std::vector<PlayerAudioCommand> cmds;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        std::vector<std::shared_ptr<ActiveStreamSession>> expired;
        for (const auto& [sid, session] : streams_) {
            (void)sid;
            if (session && session->is_passthrough.load() &&
                session->state.load() == StreamState::Playing &&
                session->metadata.duration_seconds > 0) {
                auto expire_after = std::chrono::seconds(session->metadata.duration_seconds + 15);
                if ((now - session->started_at) >= expire_after) {
                    expired.push_back(session);
                }
            }
        }
        for (const auto& session : expired) {
            session->state.store(StreamState::Finished);
            RemoveStreamSessionLocked(session->id);
            DetachSessionFromArbitrationLocked(session, cmds);
        }
    }
    ApplyPlayerAudioCommands(cmds);
}

void AudioEngine::CheckUnconnectedClients(std::chrono::steady_clock::time_point now) {
    const std::int64_t timeout_ms = std::max<std::int64_t>(
        1, connect_warn_timeout_ms_.load(std::memory_order_relaxed));

    struct ExpiredConnectCheck {
        PlayerId player_id{INVALID_PLAYER_ID};
        AudioSourcePriority source_kind{AudioSourcePriority::None};
        std::uint64_t source_id{0};
    };
    std::vector<ExpiredConnectCheck> expired;
    bool emit_warn = false;
    PlayerId warn_pid = INVALID_PLAYER_ID;
    std::uint16_t warn_port = 0;

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        for (auto it = pending_connect_checks_.begin(); it != pending_connect_checks_.end();) {
            const PlayerId pid = it->first;
            auto& check = it->second;

            if (check.endpoint_token.empty() || !host_.IsPlayerConnected(pid)) {
                it = pending_connect_checks_.erase(it);
                continue;
            }
            if (http_server_.GetEndpointTotalConnectionCount(check.endpoint_token) > 0) {
                it = pending_connect_checks_.erase(it);
                continue;
            }

            auto ait = player_arbitration_.find(pid);
            if (ait == player_arbitration_.end() ||
                ait->second.client_source_kind != check.source_kind ||
                ait->second.client_source_id != check.source_id) {
                it = pending_connect_checks_.erase(it);
                continue;
            }

            bool source_active = false;
            bool source_paused = false;
            if (check.source_kind == AudioSourcePriority::Personal ||
                (check.source_kind == AudioSourcePriority::Global && (check.source_id & (1ULL << 62)) == 0)) {
                auto sit = streams_.find(static_cast<StreamId>(check.source_id));
                if (sit != streams_.end() && sit->second &&
                    !sit->second->cancel_flag.load() &&
                    !sit->second->is_passthrough.load() &&
                    sit->second->state.load() == StreamState::Playing) {
                    source_active = true;
                    source_paused = sit->second->pause_flag.load();
                }
            } else if (check.source_kind == AudioSourcePriority::Channel ||
                       (check.source_kind == AudioSourcePriority::Global && (check.source_id & (1ULL << 62)) != 0)) {
                const ChannelId cid = static_cast<ChannelId>(check.source_id & ~(1ULL << 62));
                auto cit = channels_.find(cid);
                if (cit != channels_.end() && cit->second) {
                    const StreamState cst = cit->second->state.load();
                    if (cst != StreamState::Idle &&
                        cst != StreamState::Stopped &&
                        cst != StreamState::Error &&
                        cst != StreamState::Finished) {
                        source_active = true;
                        source_paused = (cst == StreamState::Paused || cit->second->pause_flag.load());
                    }
                }
            }

            if (!source_active) {
                it = pending_connect_checks_.erase(it);
                continue;
            }
            if (source_paused) {
                check.delivered_at = now;
                ++it;
                continue;
            }

            const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - check.delivered_at).count();
            if (elapsed_ms >= timeout_ms) {
                expired.push_back(ExpiredConnectCheck{pid, check.source_kind, check.source_id});
                if (!no_connect_warned_once_ || (now - last_no_connect_warn_) >= std::chrono::minutes(10)) {
                    no_connect_warned_once_ = true;
                    last_no_connect_warn_ = now;
                    emit_warn = true;
                    warn_pid = pid;
                    warn_port = http_server_.BoundPort();
                }
                it = pending_connect_checks_.erase(it);
                continue;
            }
            ++it;
        }
    }

    if (emit_warn) {
        Logger::Instance().Warn(
            "Player " + std::to_string(warn_pid) +
            " did not connect to the audio stream within 15 s (HTTP port " +
            std::to_string(warn_port) + "/TCP).\n"
            "If players hear nothing, make sure TCP port " +
            std::to_string(warn_port) +
            " is open in the server firewall (iptables/ufw/hosting panel) and that 'network.public_ip' is correct.\n"
            "If only some players cannot hear anything, the cause is usually on the player's side rather than the server firewall (common causes: GTA SA radio volume set to 0 in Options > Audio, or the player's local firewall/antivirus).");
    }

    for (const auto& item : expired) {
        if (!host_.IsPlayerConnected(item.player_id)) {
            continue;
        }
        AudioStreamSourceType src_type = AudioStreamSourceType::None;
        std::int32_t src_id = -1;
        DecodeClientSource(item.source_kind, item.source_id, src_type, src_id);
        DispatchAudioCallback("OnPlayerAudioStreamNoConnect", {
            item.player_id,
            static_cast<std::int32_t>(src_type),
            src_id
        });
    }
}

void AudioEngine::NotifyDisconnectHookActive() {
    host_.OnDisconnectHookObserved();
}

void AudioEngine::CheckDisconnectedPlayers() {
    std::vector<PlayerId> tracked;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        tracked.reserve(player_arbitration_.size() + active_searches_.size() + channels_.size());
        for (const auto& [pid, _] : player_arbitration_) tracked.push_back(pid);
        for (const auto& [pid, _] : active_searches_) tracked.push_back(pid);
        for (const auto& [pid, _] : player_streams_) tracked.push_back(pid);
        for (const auto& [pid, _] : player_channels_) tracked.push_back(pid);
        for (const auto& [cid, ch] : channels_) {
            (void)cid;
            if (ch && ch->creator_connected && ch->creator_player_id != INVALID_PLAYER_ID) {
                tracked.push_back(ch->creator_player_id);
            }
            if (ch && ch->spatial.attach_player_id != INVALID_PLAYER_ID) {
                tracked.push_back(ch->spatial.attach_player_id);
            }
        }
        if (tracked.empty()) {
            return;
        }
    }

    std::sort(tracked.begin(), tracked.end());
    tracked.erase(std::unique(tracked.begin(), tracked.end()), tracked.end());

    for (PlayerId pid : tracked) {
        if (!host_.IsPlayerConnected(pid)) {
            OnPlayerDisconnect(pid);
        }
    }
}

void AudioEngine::MatchCreatorLocked(
    PlayerId player_id,
    const std::string& player_name,
    std::vector<std::string>& out_debug_logs) {
    if (player_id < 0 || player_id == PAWN_INVALID_PLAYER_ID || player_name.empty()) {
        return;
    }

    for (auto& [cid, ch] : channels_) {
        if (!ch || !ch->has_creator) {
            continue;
        }
        // (a) Online creator renamed -> keep ownership linked by playerid, update stored name only.
        if (ch->creator_connected && ch->creator_player_id == player_id) {
            ch->creator_name = player_name;
            continue;
        }
        // (b) Offline creator reconnected / obtained matching name -> restore ownership & reset countdown.
        if (!ch->creator_connected &&
            ch->creator_player_id == INVALID_PLAYER_ID &&
            !ch->creator_name.empty() &&
            EqualsIgnoreCase(ch->creator_name, player_name)) {
            ch->creator_player_id = player_id;
            ch->creator_connected = true;
            ch->creator_disconnected_at = std::chrono::steady_clock::time_point{};
            ch->restore_grace_deadline = std::chrono::steady_clock::time_point{};
            out_debug_logs.push_back(
                "Channel " + std::to_string(cid) + " creator '" + ch->creator_name +
                "' returned as player " + std::to_string(player_id) + "; destroy countdown reset");
        }
    }
}

void AudioEngine::MatchCreator(PlayerId player_id, const std::string& player_name) {
    if (shutting_down_.load() || player_id < 0 || player_id == PAWN_INVALID_PLAYER_ID || player_name.empty()) {
        return;
    }
    std::vector<std::string> debug_logs;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        MatchCreatorLocked(player_id, player_name, debug_logs);
    }
    for (const auto& msg : debug_logs) {
        Logger::Instance().Debug(msg);
    }
}

void AudioEngine::OnPlayerConnect(PlayerId player_id) {
    if (shutting_down_.load() || player_id < 0 || player_id == PAWN_INVALID_PLAYER_ID) {
        return;
    }
    const std::string player_name = host_.GetPlayerName(player_id);
    MatchCreator(player_id, player_name);
}

void AudioEngine::OnPlayerNameChanged(PlayerId player_id, const std::string& new_name) {
    MatchCreator(player_id, new_name);
}

void AudioEngine::OnPlayerDisconnect(PlayerId player_id) {
    std::shared_ptr<ActiveStreamSession> old_personal;
    std::shared_ptr<AudioStreamBuffer> old_buf;
    bool search_cancelled = false;
    std::vector<std::pair<ChannelId, ChannelDestroyReason>> channels_to_destroy;
    const std::string disconnect_name = host_.GetPlayerName(player_id);
    auto now = std::chrono::steady_clock::now();
    const bool stopping_lifecycle = lifecycle_stopping_.load();
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
            old_personal = RemoveStreamSessionLocked(ps_it->second);
            if (old_personal) {
                old_buf = old_personal->buffer;
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
            ch->range_listeners.erase(player_id);
            if (ch->spatial.attach_player_id == player_id) {
                ch->spatial.attach_player_id = INVALID_PLAYER_ID;
                ch->spatial.use_position = true;
                ++ch->spatial_version;
                MarkChannelDirtyLocked(*ch);
            }
            if (ch->creator_connected && ch->creator_player_id == player_id) {
                if (!disconnect_name.empty()) {
                    ch->creator_name = disconnect_name;
                    MarkChannelDirtyLocked(*ch);
                }
                ch->creator_connected = false;
                ch->creator_player_id = INVALID_PLAYER_ID;
                if (!stopping_lifecycle) {
                    if (ch->destroy_mode == ChannelDestroyMode::OnDisconnect) {
                        channels_to_destroy.emplace_back(id, ChannelDestroyReason::CreatorDisconnected);
                    } else if (ch->destroy_mode == ChannelDestroyMode::Timeout) {
                        if (ch->destroy_timeout_seconds <= 0) {
                            channels_to_destroy.emplace_back(id, ChannelDestroyReason::TimeoutExpired);
                        } else {
                            ch->creator_disconnected_at = now;
                        }
                    }
                }
            }
        }
        player_arbitration_.erase(player_id);
        auto snd_it = player_sounds_.find(player_id);
        if (snd_it != player_sounds_.end()) {
            http_server_.UnregisterFileEndpoint(snd_it->second.token);
            player_sounds_.erase(snd_it);
        }
    }

    if (old_personal) {
        old_personal->cancel_flag.store(true);
        if (old_personal->active_run) {
            old_personal->active_run->cancel_flag.store(true);
        }
        old_personal->state.store(StreamState::Stopped);
        if (old_buf) old_buf->Close();
        http_server_.UnregisterEndpoint(old_personal->token);
        HttpClient::PollCancellations();
    }
    if (search_cancelled || old_personal) {
        resolver_.PollCancellations();
    }
    for (const auto& [cid, reason] : channels_to_destroy) {
        DestroyChannelInternal(cid, reason);
    }
}

std::string AudioEngine::GenerateEndpointCode() {
    static constexpr char kAlphabet[] =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    static constexpr std::size_t kCodeLen = 10;
    static constexpr std::uint8_t kRejectThreshold = static_cast<std::uint8_t>((256 / 62) * 62); // 248

    auto fill_csprng = [](std::uint8_t* dst, std::size_t len) -> bool {
#ifdef _WIN32
        return BCryptGenRandom(nullptr, dst, static_cast<ULONG>(len), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
        std::size_t filled = 0;
        while (filled < len) {
            ssize_t got = ::getrandom(dst + filled, len - filled, 0);
            if (got > 0) {
                filled += static_cast<std::size_t>(got);
                continue;
            }
            if (got < 0 && errno == EINTR) {
                continue;
            }
            int fd = ::open("/dev/urandom", O_RDONLY);
            if (fd < 0) return false;
            while (filled < len) {
                ssize_t r = ::read(fd, dst + filled, len - filled);
                if (r > 0) {
                    filled += static_cast<std::size_t>(r);
                } else if (r < 0 && errno == EINTR) {
                    continue;
                } else {
                    break;
                }
            }
            ::close(fd);
            return filled == len;
        }
        return true;
#endif
    };

    std::string out;
    out.reserve(kCodeLen);
    std::array<std::uint8_t, 32> buf{};
    for (int round = 0; round < 8 && out.size() < kCodeLen; ++round) {
        if (!fill_csprng(buf.data(), buf.size())) {
            break;
        }
        for (std::uint8_t b : buf) {
            if (b < kRejectThreshold) {
                out.push_back(kAlphabet[b % 62]);
                if (out.size() == kCodeLen) {
                    return out;
                }
            }
        }
    }

    static std::atomic<std::uint64_t> fallback_counter{1};
    while (out.size() < kCodeLen) {
        const std::uint64_t v = fallback_counter.fetch_add(1) ^
            static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        out.push_back(kAlphabet[(v ^ (v >> 17)) % 62]);
    }
    return out;
}

std::string AudioEngine::GenerateUniqueEndpointCodeLocked() const {
    for (int attempt = 0; attempt < 64; ++attempt) {
        std::string code = GenerateEndpointCode();
        bool collision = false;
        for (const auto& [sid, sess] : streams_) {
            (void)sid;
            if (sess && sess->token == code) {
                collision = true;
                break;
            }
        }
        if (!collision) {
            for (const auto& [cid, ch] : channels_) {
                (void)cid;
                if (ch && ch->token == code) {
                    collision = true;
                    break;
                }
            }
        }
        if (!collision) {
            return code;
        }
    }
    return GenerateEndpointCode();
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
    std::string url = BuildEndpointUrlForPlayerLocked("s", session.token, player_id);
    if (url.empty()) {
        return "";
    }
    if (session.options_version > 1) {
        url += "/" + std::to_string(session.options_version);
    }
    std::uint64_t resume_off = session.pause_flag.load() ? session.paused_offset : session.resync_offset;
    if (resume_off > 0) {
        url += "?resume=" + std::to_string(resume_off);
    }
    return url;
}

void AudioEngine::DecodeClientSource(
    AudioSourcePriority kind,
    std::uint64_t raw_id,
    AudioStreamSourceType& out_type,
    std::int32_t& out_id) {
    switch (kind) {
        case AudioSourcePriority::Personal:
            out_type = AudioStreamSourceType::Personal;
            out_id = static_cast<std::int32_t>(raw_id);
            break;
        case AudioSourcePriority::Channel:
            out_type = AudioStreamSourceType::Channel;
            out_id = static_cast<std::int32_t>(raw_id);
            break;
        case AudioSourcePriority::Global:
            if ((raw_id & (1ULL << 62)) != 0) {
                out_type = AudioStreamSourceType::ChannelRange;
                out_id = static_cast<std::int32_t>(raw_id & ~(1ULL << 62));
            } else {
                out_type = AudioStreamSourceType::Global;
                out_id = static_cast<std::int32_t>(raw_id);
            }
            break;
        case AudioSourcePriority::Sound:
            out_type = AudioStreamSourceType::Sound;
            out_id = -1;
            break;
        case AudioSourcePriority::None:
        default:
            out_type = AudioStreamSourceType::None;
            out_id = -1;
            break;
    }
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
    std::uint64_t desired_opt_ver = 0;
    std::string desired_url;
    std::string desired_token;
    bool desired_paused = false;
    Vector3 desired_pos{};
    float desired_dist = 0.0f;
    bool desired_use_pos = false;

    const bool personal_muted = arb.muted && arb.mute_include_personal;
    const bool non_personal_muted = arb.muted;

    // Attached sources follow moving entities via server range checks and play 2D on the client
    // because SA-MP's PlayAudioStreamForPlayer fixes 3D coordinates at call time.
    auto effective_client_use_pos = [](const StreamOptions& opts) -> bool {
        bool attached = (opts.attach_player_id != INVALID_PLAYER_ID || opts.attach_vehicle_id > 0);
        return opts.use_position && !attached;
    };

    auto sound_it = player_sounds_.find(player_id);
    if (sound_it != player_sounds_.end() && !personal_muted) {
        const PlayerSoundState& snd = sound_it->second;
        desired_kind = AudioSourcePriority::Sound;
        desired_id = snd.play_id;
        desired_gen = 1;
        desired_opt_ver = 1;
        desired_url = snd.url;
        desired_pos = snd.position;
        desired_dist = snd.distance;
        desired_use_pos = snd.use_position;
    }

    for (AudioSourcePriority tier : order) {
        if (desired_kind == AudioSourcePriority::Sound) {
            break;
        }
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
                    } else if (!personal_muted) {
                        if (st == StreamState::Playing && !session.pause_flag.load()) {
                            bool passthrough_no_zone_filter =
                                session.is_passthrough.load() &&
                                session.options.world_id == -1 &&
                                session.options.interior_id == -1 &&
                                !session.options.use_position &&
                                session.options.attach_player_id == INVALID_PLAYER_ID &&
                                session.options.attach_vehicle_id <= 0;
                            bool in_range = passthrough_no_zone_filter || (snapshot
                                ? MatchesSpatialFilterSnapshot(*snapshot, session.options)
                                : MatchesSpatialFilter(player_id, session.options));
                            if (in_range) {
                                std::string candidate_url = BuildSessionPlayUrlLocked(session, player_id);
                                if (!candidate_url.empty()) {
                                    desired_kind = AudioSourcePriority::Personal;
                                    desired_id = session.id;
                                    desired_gen = 1;
                                    desired_opt_ver = session.options_version;
                                    desired_url = std::move(candidate_url);
                                    desired_token = ShouldUseDirectPassthrough(session) ? std::string() : session.token;
                                    desired_paused = session.pause_flag.load();
                                    desired_pos = session.options.position;
                                    desired_dist = session.options.distance;
                                    desired_use_pos = effective_client_use_pos(session.options);
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
                } else if (!non_personal_muted) {
                    const auto& ch = *cit->second;
                    StreamState st = ch.state.load();
                    if (st == StreamState::Resolving || st == StreamState::Buffering ||
                        st == StreamState::Playing || st == StreamState::Paused) {
                        bool in_range = !ch.spatial.use_position || (snapshot
                            ? MatchesSpatialFilterSnapshot(*snapshot, ch.spatial)
                            : MatchesSpatialFilter(player_id, ch.spatial));
                        if (in_range && (st == StreamState::Playing || st == StreamState::Paused)) {
                            std::string candidate_url = BuildEndpointUrlForPlayerLocked("c", ch.token, player_id);
                            if (!candidate_url.empty()) {
                                desired_kind = AudioSourcePriority::Channel;
                                desired_id = ch.id;
                                desired_gen = ch.active_run ? ch.active_run->generation : ch.next_generation;
                                desired_opt_ver = ch.spatial_version;
                                desired_url = std::move(candidate_url);
                                desired_token = ch.token;
                                desired_paused = (st == StreamState::Paused || ch.pause_flag.load());
                                desired_pos = ch.spatial.position;
                                desired_dist = ch.spatial.distance;
                                desired_use_pos = effective_client_use_pos(ch.spatial);
                            }
                        } else if (in_range &&
                                   (st == StreamState::Resolving || st == StreamState::Buffering) &&
                                   arb.client_source_kind == AudioSourcePriority::Channel &&
                                   arb.client_source_id == static_cast<std::uint64_t>(ch.id)) {
                            // Preserve active channel attachment across track switches / seeks while resolving.
                            desired_kind = arb.client_source_kind;
                            desired_id = arb.client_source_id;
                            desired_gen = arb.client_generation;
                            desired_opt_ver = arb.client_options_version;
                            desired_dist = arb.client_distance;
                            desired_url = arb.client_url;
                            desired_pos = ch.spatial.position;
                            desired_use_pos = effective_client_use_pos(ch.spatial);
                        }
                        break;
                    }
                }
            }
        } else if (tier == AudioSourcePriority::Global) {
            for (auto it = arb.global_sources.begin(); it != arb.global_sources.end();) {
                if (!it->is_channel) {
                    auto sit = streams_.find(it->id);
                    if (sit == streams_.end() || sit->second->cancel_flag.load()) {
                        it = arb.global_sources.erase(it);
                        continue;
                    }
                    StreamState st = sit->second->state.load();
                    if (st == StreamState::Stopped || st == StreamState::Error || st == StreamState::Finished) {
                        it = arb.global_sources.erase(it);
                        continue;
                    }
                } else {
                    auto cit = channels_.find(it->id);
                    if (cit == channels_.end() || !cit->second->spatial.use_position || !cit->second->auto_listen) {
                        it = arb.global_sources.erase(it);
                        continue;
                    }
                    StreamState st = cit->second->state.load();
                    if (st == StreamState::Idle || st == StreamState::Stopped ||
                        st == StreamState::Error || st == StreamState::Finished) {
                        it = arb.global_sources.erase(it);
                        continue;
                    }
                }
                ++it;
            }
            if (!non_personal_muted) {
                for (const auto& gsrc : arb.global_sources) {
                    if (!gsrc.is_channel) {
                        auto sit = streams_.find(gsrc.id);
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
                                    desired_opt_ver = session.options_version;
                                    desired_url = std::move(candidate_url);
                                    desired_token = ShouldUseDirectPassthrough(session) ? std::string() : session.token;
                                    desired_paused = session.pause_flag.load();
                                    desired_pos = session.options.position;
                                    desired_dist = session.options.distance;
                                    desired_use_pos = effective_client_use_pos(session.options);
                                    break;
                                }
                            }
                        }
                    } else {
                        auto cit = channels_.find(gsrc.id);
                        if (cit == channels_.end()) continue;
                        const auto& ch = *cit->second;
                        if (IsRangeGroupMutedLocked(player_id, ch.range_group)) continue;
                        StreamState st = ch.state.load();
                        if (ch.spatial.use_position && ch.auto_listen && !ch.pause_flag.load()) {
                            const bool in_range = snapshot
                                ? MatchesSpatialFilterSnapshot(*snapshot, ch.spatial)
                                : MatchesSpatialFilter(player_id, ch.spatial);
                            if (in_range && st == StreamState::Playing) {
                                std::string candidate_url = BuildEndpointUrlForPlayerLocked("c", ch.token, player_id);
                                if (!candidate_url.empty()) {
                                    desired_kind = AudioSourcePriority::Global;
                                    desired_id = static_cast<std::uint64_t>(ch.id) | (1ULL << 62);
                                    desired_gen = ch.active_run ? ch.active_run->generation : ch.next_generation;
                                    desired_opt_ver = ch.spatial_version;
                                    desired_url = std::move(candidate_url);
                                    desired_token = ch.token;
                                    desired_paused = ch.pause_flag.load();
                                    desired_pos = ch.spatial.position;
                                    desired_dist = ch.spatial.distance;
                                    desired_use_pos = effective_client_use_pos(ch.spatial);
                                    break;
                                }
                            } else if (in_range &&
                                       (st == StreamState::Resolving || st == StreamState::Buffering) &&
                                       arb.client_source_kind == AudioSourcePriority::Global &&
                                       arb.client_source_id == (static_cast<std::uint64_t>(ch.id) | (1ULL << 62))) {
                                desired_kind = arb.client_source_kind;
                                desired_id = arb.client_source_id;
                                desired_gen = arb.client_generation;
                                desired_opt_ver = arb.client_options_version;
                                desired_dist = arb.client_distance;
                                desired_url = arb.client_url;
                                desired_pos = ch.spatial.position;
                                desired_use_pos = effective_client_use_pos(ch.spatial);
                                break;
                            }
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
        pending_connect_checks_.erase(player_id);
        if (arb.client_source_kind != AudioSourcePriority::None) {
            PlayerAudioCommand cmd{player_id, false, "", {}, 0.0f, false, true, AudioStreamSourceType::None, -1};
            out_cmds.push_back(std::move(cmd));
            arb.client_source_kind = AudioSourcePriority::None;
            arb.client_source_id = 0;
            arb.client_generation = 0;
            arb.client_options_version = 0;
            arb.client_distance = 0.0f;
            arb.client_url.clear();
        }
        if (!arb.muted &&
            arb.muted_range_groups == 0 &&
            arb.personal_stream_id == INVALID_STREAM_ID &&
            arb.channel_id == INVALID_CHANNEL_ID &&
            arb.global_sources.empty()) {
            player_arbitration_.erase(ait);
        }
        return;
    }

    if (desired_kind != AudioSourcePriority::None &&
        desired_kind != AudioSourcePriority::Personal &&
        desired_kind != AudioSourcePriority::Sound &&
        arb.personal_stream_id != INVALID_STREAM_ID) {
        auto sit = streams_.find(arb.personal_stream_id);
        if (sit != streams_.end() && sit->second->is_passthrough.load()) {
            sit->second->cancel_flag.store(true);
            if (sit->second->active_run) {
                sit->second->active_run->cancel_flag.store(true);
            }
            sit->second->state.store(StreamState::Stopped);
            RemoveStreamSessionLocked(arb.personal_stream_id);
            auto pit = player_streams_.find(player_id);
            if (pit != player_streams_.end() && pit->second == arb.personal_stream_id) {
                player_streams_.erase(pit);
            }
            arb.personal_stream_id = INVALID_STREAM_ID;
        }
    }

    const bool identity_changed =
        (desired_kind != arb.client_source_kind || desired_id != arb.client_source_id);

    // Joining or leaving a positioned channel while inside its range only moves the player between the
    // channel and range tiers of the same channel. The client already plays exactly this stream (same URL,
    // generation, options and distance), so re-sending it would only restart the audio.
    auto channel_of_source = [](AudioSourcePriority kind, std::uint64_t id, std::uint64_t& out_channel) -> bool {
        if (kind == AudioSourcePriority::Channel) {
            out_channel = id;
            return true;
        }
        if (kind == AudioSourcePriority::Global && (id & (1ULL << 62)) != 0) {
            out_channel = id & ~(1ULL << 62);
            return true;
        }
        return false;
    };
    std::uint64_t desired_channel = 0;
    std::uint64_t client_channel = 0;
    if (identity_changed &&
        channel_of_source(desired_kind, desired_id, desired_channel) &&
        channel_of_source(arb.client_source_kind, arb.client_source_id, client_channel) &&
        desired_channel == client_channel &&
        desired_gen == arb.client_generation &&
        desired_opt_ver == arb.client_options_version &&
        desired_url == arb.client_url &&
        std::abs(desired_dist - arb.client_distance) <= 0.001f) {
        PlayerAudioCommand cmd{player_id, true, desired_url, desired_pos, desired_dist, desired_use_pos};
        cmd.send = false;
        cmd.source_changed = true;
        DecodeClientSource(desired_kind, desired_id, cmd.new_source_type, cmd.new_source_id);
        out_cmds.push_back(std::move(cmd));
        auto pit = pending_connect_checks_.find(player_id);
        if (pit != pending_connect_checks_.end() &&
            pit->second.source_kind == arb.client_source_kind &&
            pit->second.source_id == arb.client_source_id) {
            pit->second.source_kind = desired_kind;
            pit->second.source_id = desired_id;
        }
        arb.client_source_kind = desired_kind;
        arb.client_source_id = desired_id;
        return;
    }

    if (identity_changed ||
        desired_gen != arb.client_generation ||
        desired_opt_ver != arb.client_options_version ||
        desired_url != arb.client_url ||
        std::abs(desired_dist - arb.client_distance) > 0.001f) {
        PlayerAudioCommand cmd{
            player_id, true, desired_url, desired_pos, desired_dist, desired_use_pos
        };
        if (identity_changed) {
            cmd.source_changed = true;
            DecodeClientSource(desired_kind, desired_id, cmd.new_source_type, cmd.new_source_id);
        }
        out_cmds.push_back(std::move(cmd));
        arb.client_source_kind = desired_kind;
        arb.client_source_id = desired_id;
        arb.client_generation = desired_gen;
        arb.client_options_version = desired_opt_ver;
        arb.client_distance = desired_dist;
        arb.client_url = desired_url;
        if (!desired_token.empty() && !desired_paused) {
            pending_connect_checks_[player_id] = PendingClientConnectCheck{
                desired_token,
                desired_kind,
                desired_id,
                std::chrono::steady_clock::now()
            };
        } else {
            pending_connect_checks_.erase(player_id);
        }
    }
}

void AudioEngine::ApplyPlayerAudioCommands(const std::vector<PlayerAudioCommand>& cmds) {
    for (const auto& cmd : cmds) {
        if (!cmd.send || !host_.IsPlayerConnected(cmd.player_id)) {
            continue;
        }
        if (cmd.play) {
            host_.SendPlayAudioStream(cmd.player_id, cmd.url, cmd.position, cmd.distance, cmd.use_position);
        } else {
            host_.SendStopAudioStream(cmd.player_id);
        }
    }
    if (shutting_down_.load()) {
        return;
    }
    auto alive = alive_token_;
    for (const auto& cmd : cmds) {
        if (cmd.source_changed && host_.IsPlayerConnected(cmd.player_id)) {
            MainThreadDispatcher::Instance().Post([this, alive,
                                                   pid = cmd.player_id,
                                                   stype = cmd.new_source_type,
                                                   sid = cmd.new_source_id]() {
                if (!alive->load() || shutting_down_.load() || !host_.IsPlayerConnected(pid)) {
                    return;
                }
                DispatchAudioCallback("OnPlayerAudioStreamSourceChange", {
                    pid,
                    static_cast<std::int32_t>(stype),
                    sid
                });
            });
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
            !sit->second->cancel_flag.load() &&
            (!sit->second->active_run || !sit->second->active_run->cancel_flag.load())) {
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

StreamId AudioEngine::PlayAudioStreamForPlayer(
    PlayerId player_id,
    const std::string& url_or_query,
    const StreamOptions& options,
    const std::string& original_input_pawn) {
    if (!initialized_.load() || shutting_down_.load() || !host_.IsPlayerConnected(player_id) || url_or_query.empty()) {
        return INVALID_STREAM_ID;
    }

    auto session = std::make_shared<ActiveStreamSession>();
    session->id = next_stream_id_.fetch_add(1);
    session->original_input = url_or_query;
    session->original_input_pawn = original_input_pawn;
    session->options = options;
    session->is_global = false;
    session->owner_player_id = player_id;
    session->owner_is_loopback = IsLoopbackIp(host_.GetPlayerIp(player_id));
    session->volume_percent.store(std::clamp(options.volume, 0.0f, 200.0f));
    session->started_at = std::chrono::steady_clock::now();

    const bool force_process_file = options.process_direct_audio_files &&
                                    StreamResolver::IsDirectAudioFileUrl(url_or_query);
    const bool is_http_plain = StartsWithHttpPlain(url_or_query);
    const bool is_http_or_https = is_http_plain || StartsWithHttpOrHttps(url_or_query);
    const bool is_native_ext = StreamResolver::IsKnownDirectAudioExtension(url_or_query);
    const bool is_hls = StreamResolver::IsHlsPlaylistUrl(url_or_query);
    auto cached_meta = metadata_cache_.GetTrack(url_or_query);
    const bool cached_direct_radio =
        cached_meta.has_value() &&
        cached_meta->extractor_name == "direct" &&
        !StreamResolver::IsHlsPlaylistUrl(cached_meta->direct_media_url) &&
        (!StreamResolver::IsDirectAudioFileUrl(cached_meta->direct_media_url) ||
         StreamResolver::IsKnownDirectAudioExtension(cached_meta->direct_media_url));

    std::string sync_policy_err;
    const bool sync_http_shortcut =
        !force_process_file &&
        !is_hls &&
        (is_http_plain || (is_http_or_https && (is_native_ext || cached_direct_radio))) &&
        !StreamResolver::IsKnownPlatformUrl(url_or_query) &&
        std::abs(session->volume_percent.load() - 100.0f) < 0.5f &&
        options.world_id == -1 &&
        options.interior_id == -1 &&
        HttpClient::ValidateUrlSyntaxAndLiteralPolicy(url_or_query, sync_policy_err);

    const bool pre_passthrough = sync_http_shortcut ||
        (!force_process_file &&
         !is_hls &&
         is_http_or_https &&
         is_native_ext &&
         std::abs(session->volume_percent.load() - 100.0f) < 0.5f);
    session->is_passthrough.store(pre_passthrough);

    if (sync_http_shortcut) {
        session->source_type = SourceType::DirectAudio;
        session->metadata.original_url = url_or_query;
        session->metadata.direct_media_url = url_or_query;
        session->metadata.extractor_name = "direct";
        session->metadata.title.clear();
        session->metadata.is_live = true;
        session->track_started_at = session->started_at;
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
            old_personal = RemoveStreamSessionLocked(prev_it->second);
            if (old_personal) {
                old_buf = old_personal->buffer;
            }
            player_streams_.erase(prev_it);
        }
        session->token = GenerateUniqueEndpointCodeLocked();
        AddStreamSessionLocked(session);
        player_streams_[player_id] = session->id;
        player_arbitration_[player_id].personal_stream_id = session->id;
        if (sync_http_shortcut) {
            session->active_listeners.insert(player_id);
        }
        ReconcilePlayerLocked(player_id, nullptr, cmds);
    }

    if (old_personal) {
        old_personal->cancel_flag.store(true);
        if (old_personal->active_run) {
            old_personal->active_run->cancel_flag.store(true);
        }
        old_personal->state.store(StreamState::Stopped);
        if (old_buf) old_buf->Close();
        http_server_.UnregisterEndpoint(old_personal->token);
        HttpClient::PollCancellations();
    }
    if (options.notify_processing) {
        bool from_cache = sync_http_shortcut || metadata_cache_.GetTrack(url_or_query).has_value();
        const std::string processing_url = session->original_input_pawn.empty()
            ? ConvertTextForPawn(url_or_query, config_.pawn_text_encoding)
            : session->original_input_pawn;
        DispatchProcessingNotification(player_id, session->id, processing_url, from_cache);
    }

    if (sync_http_shortcut) {
        auto alive = alive_token_;
        MainThreadDispatcher::Instance().Post([this, session, alive]() {
            if (!alive->load() || shutting_down_.load() || session->cancel_flag.load()) return;
            DispatchAudioCallback("OnAudioStreamReady", {
                session->owner_player_id,
                session->id,
                ConvertTextForPawn(session->metadata.title, config_.pawn_text_encoding),
                ConvertTextForPawn(session->metadata.artist, config_.pawn_text_encoding),
                session->metadata.duration_seconds,
                session->metadata.direct_media_url
            });
            DispatchAudioCallback("OnAudioStreamStart", {session->owner_player_id, session->id});
        });
        ApplyPlayerAudioCommands(cmds);
        PollLiveTitles(std::chrono::steady_clock::now());
        return session->id;
    }

    ApplyPlayerAudioCommands(cmds);
    StartStreamWorker(session);
    return session->id;
}

void AudioEngine::RegisterNativePassthroughSession(PlayerId player_id, const std::string& url, const StreamOptions& options) {
    RegisterNativePassthroughSessionImpl(player_id, url, options, false);
}

bool AudioEngine::HasActivePlayerSound(PlayerId player_id) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return player_sounds_.find(player_id) != player_sounds_.end();
}

bool AudioEngine::DeferNativePassthroughForSound(PlayerId player_id, const std::string& url, const StreamOptions& options) {
    if (!initialized_.load() || shutting_down_.load() || player_id < 0 || url.empty()) {
        return false;
    }
    if (!HasActivePlayerSound(player_id)) {
        return false;
    }
    RegisterNativePassthroughSessionImpl(player_id, url, options, true);
    return true;
}

void AudioEngine::RegisterNativePassthroughSessionImpl(
    PlayerId player_id,
    const std::string& url,
    const StreamOptions& options,
    bool deferred) {
    if (!initialized_.load() || shutting_down_.load() || player_id < 0 || url.empty()) {
        return;
    }

    auto session = std::make_shared<ActiveStreamSession>();
    session->id = next_stream_id_.fetch_add(1);
    session->original_input = url;
    session->options = options;
    session->is_global = false;
    session->owner_player_id = player_id;
    session->owner_is_loopback = IsLoopbackIp(host_.GetPlayerIp(player_id));
    session->volume_percent.store(std::clamp(options.volume, 0.0f, 200.0f));
    session->started_at = std::chrono::steady_clock::now();
    session->track_started_at = session->started_at;
    session->source_type = SourceType::DirectAudio;
    session->metadata.original_url = url;
    session->metadata.direct_media_url = url;
    session->metadata.extractor_name = "direct";
    session->metadata.is_live = true;
    session->is_passthrough.store(true);
    session->ready.store(true);
    session->state.store(StreamState::Playing);
    session->active_listeners.insert(player_id);

    std::shared_ptr<ActiveStreamSession> old_personal;
    std::shared_ptr<AudioStreamBuffer> old_buf;
    bool source_changed = false;
    std::vector<PlayerAudioCommand> cmds;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto prev_it = player_streams_.find(player_id);
        if (prev_it != player_streams_.end()) {
            CancelPendingCompletionLocked(prev_it->second);
            old_personal = RemoveStreamSessionLocked(prev_it->second);
            if (old_personal) {
                old_buf = old_personal->buffer;
            }
            player_streams_.erase(prev_it);
        }
        session->token = GenerateUniqueEndpointCodeLocked();
        AddStreamSessionLocked(session);
        player_streams_[player_id] = session->id;
        auto& arb = player_arbitration_[player_id];
        arb.personal_stream_id = session->id;
        if (deferred) {
            // The client keeps playing the sound; reconciliation delivers this link when the sound ends.
            ReconcilePlayerLocked(player_id, nullptr, cmds);
        } else {
            source_changed = (arb.client_source_kind != AudioSourcePriority::Personal ||
                              arb.client_source_id != static_cast<std::uint64_t>(session->id));
            arb.client_source_kind = AudioSourcePriority::Personal;
            arb.client_source_id = session->id;
            arb.client_generation = 1;
            arb.client_options_version = session->options_version;
            arb.client_distance = session->options.distance;
            arb.client_url = url;
        }
    }
    ApplyPlayerAudioCommands(cmds);

    if (old_personal) {
        old_personal->cancel_flag.store(true);
        if (old_personal->active_run) {
            old_personal->active_run->cancel_flag.store(true);
        }
        old_personal->state.store(StreamState::Stopped);
        if (old_buf) old_buf->Close();
        http_server_.UnregisterEndpoint(old_personal->token);
        HttpClient::PollCancellations();
    }
    if (source_changed && !shutting_down_.load() && host_.IsPlayerConnected(player_id)) {
        auto alive = alive_token_;
        MainThreadDispatcher::Instance().Post([this, alive, player_id, sid = session->id]() {
            if (!alive->load() || shutting_down_.load() || !host_.IsPlayerConnected(player_id)) return;
            DispatchAudioCallback("OnPlayerAudioStreamSourceChange", {
                player_id,
                static_cast<std::int32_t>(AudioStreamSourceType::Personal),
                sid
            });
        });
    }
    PollLiveTitles(std::chrono::steady_clock::now());
}

void AudioEngine::DispatchAudioCallback(const std::string& name, std::vector<CallbackArg> args) {
    if (!args.empty()) {
        if (auto* pid = std::get_if<std::int32_t>(&args[0])) {
            *pid = ToPawnPlayerId(*pid);
        }
    }
    host_.DispatchPawnCallback(name, args);
}

void AudioEngine::DispatchProcessingNotification(
    PlayerId player_id,
    StreamId stream_id,
    const std::string& original_url,
    bool from_cache) {
    auto alive = alive_token_;
    const std::string echo_url = original_url;
    MainThreadDispatcher::Instance().Post([this, alive, player_id, stream_id, echo_url, from_cache]() {
        if (!alive->load() || shutting_down_.load()) return;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            auto it = streams_.find(stream_id);
            if (it == streams_.end() || !it->second || it->second->cancel_flag.load()) {
                return;
            }
        }
        DispatchAudioCallback("OnAudioStreamProcessing", {
            player_id,
            stream_id,
            echo_url,
            static_cast<std::int32_t>(from_cache ? 1 : 0)
        });
    });
}

StreamId AudioEngine::PlayAudioStream(
    const std::string& url_or_query,
    const StreamOptions& options,
    const std::string& original_input_pawn) {
    if (!initialized_.load() || shutting_down_.load() || url_or_query.empty()) {
        return INVALID_STREAM_ID;
    }

    auto session = std::make_shared<ActiveStreamSession>();
    session->id = next_stream_id_.fetch_add(1);
    session->original_input = url_or_query;
    session->original_input_pawn = original_input_pawn;
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
        session->token = GenerateUniqueEndpointCodeLocked();
        AddStreamSessionLocked(session);
    }

    if (options.notify_processing) {
        bool from_cache = metadata_cache_.GetTrack(url_or_query).has_value();
        const std::string processing_url = session->original_input_pawn.empty()
            ? ConvertTextForPawn(url_or_query, config_.pawn_text_encoding)
            : session->original_input_pawn;
        DispatchProcessingNotification(INVALID_PLAYER_ID, session->id, processing_url, from_cache);
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

bool ExtractIcyStreamTitle(const std::string& raw_bytes, std::string& out_title_utf8) {
    out_title_utf8.clear();
    static constexpr const char kMarker[] = "StreamTitle='";
    static constexpr std::size_t kMarkerLen = sizeof(kMarker) - 1;
    const std::size_t pos = raw_bytes.find(kMarker);
    if (pos == std::string::npos) {
        return false;
    }
    const std::size_t val_start = pos + kMarkerLen;
    const std::size_t val_end = raw_bytes.find("';", val_start);
    if (val_end == std::string::npos) {
        return false;
    }
    const std::size_t raw_len = val_end - val_start;
    if (raw_len > 1024) {
        return false;
    }
    const std::string utf8 = NormalizeExternalTextToUtf8(raw_bytes.substr(val_start, raw_len));
    out_title_utf8 = CleanLiveTitleUtf8(utf8);
    return true;
}

IcyPollResult FetchIcyStreamTitle(
    const std::string& url,
    const HttpClient::HeaderMap& headers,
    const std::atomic<bool>& cancel_flag,
    std::string& out_title_utf8,
    std::string& out_station_name_utf8) {
    out_title_utf8.clear();
    out_station_name_utf8.clear();
    if (cancel_flag.load() || url.empty()) {
        return IcyPollResult::Failed;
    }

    HttpClient::HeaderMap poll_headers;
    for (const auto& kv : headers) {
        std::string lower_k = kv.first;
        std::transform(lower_k.begin(), lower_k.end(), lower_k.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower_k != "icy-metadata") {
            poll_headers[kv.first] = kv.second;
        }
    }
    poll_headers["Icy-MetaData"] = "1";

    static constexpr std::size_t kMaxProbeBytes = 64 * 1024;
    static constexpr auto kTotalDeadline = std::chrono::seconds(8);
    const auto deadline = std::chrono::steady_clock::now() + kTotalDeadline;

    std::string raw_buf;
    std::string candidate_station_name;
    bool not_live = false;
    bool found_marker = false;
    bool timed_out = false;
    bool http_error = false;

    auto on_headers = [&](int status, const std::string&, std::int64_t content_len, const HttpClient::HeaderMap& resp_headers) {
        if (status >= 400 || status < 200 || status >= 300) {
            http_error = true;
        } else if (content_len > 0) {
            not_live = true;
        } else {
            candidate_station_name = CleanIcyStationNameUtf8(
                HttpClient::FindHeaderIgnoreCase(resp_headers, "icy-name"));
        }
    };

    auto on_chunk = [&](const std::uint8_t* data, std::size_t size) -> bool {
        if (cancel_flag.load() || http_error || not_live) {
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            timed_out = true;
            return false;
        }
        if (data && size > 0 && raw_buf.size() < kMaxProbeBytes) {
            const std::size_t take = std::min<std::size_t>(size, kMaxProbeBytes - raw_buf.size());
            raw_buf.append(reinterpret_cast<const char*>(data), take);
        }
        if (ExtractIcyStreamTitle(raw_buf, out_title_utf8)) {
            found_marker = true;
            return false;
        }
        if (raw_buf.size() >= kMaxProbeBytes) {
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            timed_out = true;
            return false;
        }
        return true;
    };

    std::string err;
    const bool ok = HttpClient::StreamGet(
        url,
        poll_headers,
        on_chunk,
        cancel_flag,
        err,
        on_headers,
        3000);

    if (cancel_flag.load()) {
        out_title_utf8.clear();
        return IcyPollResult::Failed;
    }
    if (not_live) {
        out_title_utf8.clear();
        return IcyPollResult::NotLive;
    }
    if (found_marker) {
        out_station_name_utf8 = std::move(candidate_station_name);
        return out_title_utf8.empty() ? IcyPollResult::EmptyTitle : IcyPollResult::Title;
    }
    if (http_error || timed_out) {
        out_title_utf8.clear();
        return IcyPollResult::Failed;
    }
    if (raw_buf.size() >= kMaxProbeBytes) {
        out_title_utf8.clear();
        out_station_name_utf8 = std::move(candidate_station_name);
        return IcyPollResult::NoMetadata;
    }
    if (!ok) {
        out_title_utf8.clear();
        return IcyPollResult::Failed;
    }
    out_station_name_utf8 = std::move(candidate_station_name);
    return IcyPollResult::NoMetadata;
}

bool IsLiveTitlePollCandidate(const TrackMetadata& meta, std::uint32_t live_title_poll_seconds) {
    if (live_title_poll_seconds == 0) {
        return false;
    }
    if (meta.extractor_name != "direct") {
        return false;
    }
    if (!StartsWithHttpOrHttps(meta.direct_media_url)) {
        return false;
    }
    if (StreamResolver::IsHlsPlaylistUrl(meta.direct_media_url)) {
        return false;
    }
    return true;
}

void AudioEngine::LaunchSessionLiveTitlePoll(
    const std::shared_ptr<ActiveStreamSession>& session,
    const std::shared_ptr<StreamRunState>& run_snapshot,
    const std::string& url,
    const HttpClient::HeaderMap& headers) {
    if (!session) return;
    auto alive = alive_token_;
    const StreamId sid = session->id;
    active_live_title_polls_.fetch_add(1, std::memory_order_acq_rel);
    if (!extraction_pool_.Post([this, session, run_snapshot, sid, url, headers, alive]() {
        struct PollCounterGuard {
            std::atomic<std::int32_t>& ctr;
            ~PollCounterGuard() { ctr.fetch_sub(1, std::memory_order_acq_rel); }
        } guard{active_live_title_polls_};

        if (shutting_down_.load() || !alive->load() || session->cancel_flag.load() ||
            (run_snapshot && run_snapshot->cancel_flag.load())) {
            MainThreadDispatcher::Instance().Post([this, session, run_snapshot, alive]() {
                if (!alive->load() || shutting_down_.load()) return;
                std::lock_guard<std::mutex> lock(state_mutex_);
                if (session->active_run == run_snapshot) {
                    session->live_poll.in_flight = false;
                }
            });
            return;
        }

        std::string fetched_title;
        std::string fetched_station_name;
        const IcyPollResult res = FetchIcyStreamTitle(
            url, headers, session->cancel_flag, fetched_title, fetched_station_name);

        MainThreadDispatcher::Instance().Post([this, session, run_snapshot, sid, res, fetched_title = std::move(fetched_title), fetched_station_name = std::move(fetched_station_name), alive]() mutable {
            if (!alive->load() || shutting_down_.load()) return;

            bool fire_cb = false;
            bool update_ep = false;
            bool update_title_ep = false;
            bool update_station_ep = false;
            PlayerId owner_pid = INVALID_PLAYER_ID;
            std::string ep_token;
            std::string new_title;
            std::string new_station_name;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                auto sit = streams_.find(sid);
                if (sit == streams_.end() || sit->second != session || session->cancel_flag.load() ||
                    session->active_run != run_snapshot || (run_snapshot && run_snapshot->cancel_flag.load())) {
                    session->live_poll.in_flight = false;
                    return;
                }

                session->live_poll.in_flight = false;
                session->live_poll.next_poll_at = std::chrono::steady_clock::now() +
                    std::chrono::seconds(config_.live_title_poll_seconds);

                const bool is_2xx = (res == IcyPollResult::Title ||
                                     res == IcyPollResult::EmptyTitle ||
                                     res == IcyPollResult::NoMetadata);
                if (is_2xx && !fetched_station_name.empty()) {
                    if (session->station_name != fetched_station_name &&
                        (session->station_name.empty() || IsGenericRadioTrackTitle(session->metadata))) {
                        session->station_name = fetched_station_name;
                        if (!session->is_passthrough.load() && !session->token.empty()) {
                            update_station_ep = true;
                            ep_token = session->token;
                            new_station_name = fetched_station_name;
                        }
                    }
                    if (IsGenericRadioTrackTitle(session->metadata)) {
                        session->metadata.title = fetched_station_name;
                        session->metadata.title_is_placeholder = false;
                        if (!session->is_passthrough.load() && !session->token.empty()) {
                            update_title_ep = true;
                            ep_token = session->token;
                            new_station_name = fetched_station_name;
                        }
                    }
                }

                switch (res) {
                    case IcyPollResult::NotLive:
                        session->live_poll.stopped_permanently = true;
                        break;
                    case IcyPollResult::NoMetadata:
                        ++session->live_poll.consecutive_no_metadata;
                        if (session->live_poll.consecutive_no_metadata >= 2) {
                            session->live_poll.stopped_permanently = true;
                        }
                        break;
                    case IcyPollResult::EmptyTitle:
                        session->live_poll.consecutive_no_metadata = 0;
                        break;
                    case IcyPollResult::Failed:
                        Logger::Instance().Debug("Live title poll failed for stream " + std::to_string(sid));
                        break;
                    case IcyPollResult::Title:
                        session->live_poll.consecutive_no_metadata = 0;
                        if (fetched_title != session->live_poll.last_received_title) {
                            session->live_poll.last_received_title = fetched_title;
                            session->live_title = fetched_title;
                            new_title = fetched_title;
                            owner_pid = session->is_global ? INVALID_PLAYER_ID : session->owner_player_id;
                            update_ep = !session->is_passthrough.load() && !session->token.empty();
                            ep_token = session->token;
                            fire_cb = true;
                        }
                        break;
                }
            }

            if (update_station_ep) {
                http_server_.UpdateEndpointStationName(ep_token, new_station_name);
            }
            if (update_title_ep) {
                http_server_.UpdateEndpointTitle(ep_token, new_station_name);
            }
            if (fire_cb) {
                Logger::Instance().Debug("Live title updated for stream " + std::to_string(sid));
                if (update_ep) {
                    http_server_.UpdateEndpointLiveTitle(ep_token, new_title);
                }
                DispatchAudioCallback("OnAudioStreamLiveTitleChange", {
                    owner_pid,
                    sid,
                    ConvertTextForPawn(new_title, config_.pawn_text_encoding)
                });
            }
        });
    })) {
        active_live_title_polls_.fetch_sub(1, std::memory_order_acq_rel);
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (session->active_run == run_snapshot) {
            session->live_poll.in_flight = false;
        }
    }
}

void AudioEngine::LaunchChannelLiveTitlePoll(
    const std::shared_ptr<SyncChannel>& channel,
    const std::shared_ptr<ChannelRunState>& run_snapshot,
    const std::string& url,
    const HttpClient::HeaderMap& headers) {
    if (!channel || !run_snapshot) return;
    auto alive = alive_token_;
    const ChannelId cid = channel->id;
    active_live_title_polls_.fetch_add(1, std::memory_order_acq_rel);
    if (!extraction_pool_.Post([this, channel, run_snapshot, cid, url, headers, alive]() {
        struct PollCounterGuard {
            std::atomic<std::int32_t>& ctr;
            ~PollCounterGuard() { ctr.fetch_sub(1, std::memory_order_acq_rel); }
        } guard{active_live_title_polls_};

        if (shutting_down_.load() || !alive->load() || run_snapshot->cancel_flag.load()) {
            MainThreadDispatcher::Instance().Post([this, channel, run_snapshot, alive]() {
                if (!alive->load() || shutting_down_.load()) return;
                std::lock_guard<std::mutex> lock(state_mutex_);
                if (channel->active_run == run_snapshot) {
                    channel->live_poll.in_flight = false;
                }
            });
            return;
        }

        std::string fetched_title;
        std::string fetched_station_name;
        const IcyPollResult res = FetchIcyStreamTitle(
            url, headers, run_snapshot->cancel_flag, fetched_title, fetched_station_name);

        MainThreadDispatcher::Instance().Post([this, channel, run_snapshot, cid, res, fetched_title = std::move(fetched_title), fetched_station_name = std::move(fetched_station_name), alive]() mutable {
            if (!alive->load() || shutting_down_.load()) return;

            bool fire_cb = false;
            bool update_title_ep = false;
            std::string ep_token;
            std::string new_title;
            std::string new_station_name;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                auto cit = channels_.find(cid);
                if (cit == channels_.end() || cit->second != channel ||
                    channel->active_run != run_snapshot || run_snapshot->cancel_flag.load()) {
                    return;
                }

                channel->live_poll.in_flight = false;
                channel->live_poll.next_poll_at = std::chrono::steady_clock::now() +
                    std::chrono::seconds(config_.live_title_poll_seconds);

                const bool is_2xx = (res == IcyPollResult::Title ||
                                     res == IcyPollResult::EmptyTitle ||
                                     res == IcyPollResult::NoMetadata);
                if (is_2xx && !fetched_station_name.empty() &&
                    IsGenericRadioTrackTitle(channel->current_track)) {
                    channel->current_track.title = fetched_station_name;
                    channel->current_track.title_is_placeholder = false;
                    if (!channel->token.empty()) {
                        update_title_ep = true;
                        ep_token = channel->token;
                        new_station_name = fetched_station_name;
                    }
                }

                switch (res) {
                    case IcyPollResult::NotLive:
                        channel->live_poll.stopped_permanently = true;
                        break;
                    case IcyPollResult::NoMetadata:
                        ++channel->live_poll.consecutive_no_metadata;
                        if (channel->live_poll.consecutive_no_metadata >= 2) {
                            channel->live_poll.stopped_permanently = true;
                        }
                        break;
                    case IcyPollResult::EmptyTitle:
                        channel->live_poll.consecutive_no_metadata = 0;
                        break;
                    case IcyPollResult::Failed:
                        Logger::Instance().Debug("Live title poll failed for channel " + std::to_string(cid));
                        break;
                    case IcyPollResult::Title:
                        channel->live_poll.consecutive_no_metadata = 0;
                        if (fetched_title != channel->live_poll.last_received_title) {
                            channel->live_poll.last_received_title = fetched_title;
                            channel->live_title = fetched_title;
                            new_title = fetched_title;
                            ep_token = channel->token;
                            fire_cb = true;
                        }
                        break;
                }
            }

            if (update_title_ep) {
                http_server_.UpdateEndpointTitle(ep_token, new_station_name);
            }
            if (fire_cb) {
                Logger::Instance().Debug("Live title updated for channel " + std::to_string(cid));
                http_server_.UpdateEndpointLiveTitle(ep_token, new_title);
                host_.DispatchPawnCallback("OnAudioStreamChannelLiveTitleChange", {
                    cid,
                    ConvertTextForPawn(new_title, config_.pawn_text_encoding)
                });
            }
        });
    })) {
        active_live_title_polls_.fetch_sub(1, std::memory_order_acq_rel);
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (channel->active_run == run_snapshot) {
            channel->live_poll.in_flight = false;
        }
    }
}

void AudioEngine::PollLiveTitles(std::chrono::steady_clock::time_point now) {
    if (shutting_down_.load() || config_.live_title_poll_seconds == 0) {
        return;
    }
    if (active_live_title_polls_.load(std::memory_order_acquire) >= 2) {
        return;
    }

    struct SessionPollJob {
        std::shared_ptr<ActiveStreamSession> session;
        std::shared_ptr<StreamRunState> run;
        std::string url;
        HttpClient::HeaderMap headers;
    };
    struct ChannelPollJob {
        std::shared_ptr<SyncChannel> channel;
        std::shared_ptr<ChannelRunState> run;
        std::string url;
        HttpClient::HeaderMap headers;
    };
    std::vector<SessionPollJob> s_jobs;
    std::vector<ChannelPollJob> c_jobs;

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        std::int32_t available = 2 - active_live_title_polls_.load(std::memory_order_acquire);
        if (available <= 0) {
            return;
        }

        for (StreamId sid : sorted_stream_ids_) {
            if (available <= 0) break;
            auto it = streams_.find(sid);
            if (it == streams_.end() || !it->second) continue;
            auto& s = it->second;
            if (s->cancel_flag.load() || !s->ready.load() || s->pause_flag.load()) continue;
            if (s->state.load() != StreamState::Playing) continue;
            if (s->live_poll.in_flight || s->live_poll.stopped_permanently) continue;
            if (!s->options.stream_text) continue;
            if (!IsLiveTitlePollCandidate(s->metadata, config_.live_title_poll_seconds)) continue;
            if (s->live_poll.next_poll_at != std::chrono::steady_clock::time_point{} &&
                now < s->live_poll.next_poll_at) {
                continue;
            }
            s->live_poll.in_flight = true;
            s_jobs.push_back(SessionPollJob{s, s->active_run, s->metadata.direct_media_url, s->metadata.http_headers});
            --available;
        }

        for (ChannelId cid : sorted_channel_ids_) {
            if (available <= 0) break;
            auto it = channels_.find(cid);
            if (it == channels_.end() || !it->second) continue;
            auto& ch = it->second;
            if (!ch->active_run || ch->active_run->cancel_flag.load() || ch->pause_flag.load()) continue;
            if (ch->state.load() != StreamState::Playing) continue;
            if (ch->live_poll.in_flight || ch->live_poll.stopped_permanently) continue;
            if (!IsLiveTitlePollCandidate(ch->current_track, config_.live_title_poll_seconds)) continue;
            if (ch->live_poll.next_poll_at != std::chrono::steady_clock::time_point{} &&
                now < ch->live_poll.next_poll_at) {
                continue;
            }
            ch->live_poll.in_flight = true;
            c_jobs.push_back(ChannelPollJob{ch, ch->active_run, ch->current_track.direct_media_url, ch->current_track.http_headers});
            --available;
        }
    }

    for (const auto& job : s_jobs) {
        if (active_live_title_polls_.load(std::memory_order_acquire) >= 2) {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (job.session->active_run == job.run) {
                job.session->live_poll.in_flight = false;
            }
            continue;
        }
        LaunchSessionLiveTitlePoll(job.session, job.run, job.url, job.headers);
    }
    for (const auto& job : c_jobs) {
        if (active_live_title_polls_.load(std::memory_order_acquire) >= 2) {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (job.channel->active_run == job.run) {
                job.channel->live_poll.in_flight = false;
            }
            continue;
        }
        LaunchChannelLiveTitlePoll(job.channel, job.run, job.url, job.headers);
    }
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
                DispatchAudioCallback("OnAudioStreamError", {
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
            local_meta.is_live,
            session->options.process_direct_audio_files);

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
                {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    auto now_tp = std::chrono::steady_clock::now();
                    session->track_started_at = now_tp;
                    session->paused_accumulated = std::chrono::steady_clock::duration::zero();
                    session->paused_since = session->pause_flag.load() ? now_tp : std::chrono::steady_clock::time_point{};
                }
                session->ready.store(true);
                session->state.store(session->pause_flag.load() ? StreamState::Paused : StreamState::Playing);

                DispatchAudioCallback("OnAudioStreamReady", {
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
                                    auto& gvec = player_arbitration_[snap.id].global_sources;
                                    GlobalAudioSourceRef ref{false, session->id};
                                    if (std::find(gvec.begin(), gvec.end(), ref) == gvec.end()) {
                                        gvec.push_back(ref);
                                    }
                                    ReconcilePlayerLocked(snap.id, &snap, cmds);
                                }
                            }
                        }
                    }
                    ApplyPlayerAudioCommands(cmds);
                    DispatchAudioCallback("OnAudioStreamStart", {INVALID_PLAYER_ID, session->id});
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
                    DispatchAudioCallback("OnAudioStreamStart", {session->owner_player_id, session->id});
                }
                PollLiveTitles(std::chrono::steady_clock::now());
            });
            return;
        }

        // Proxy & Transcode path: defer start if available_at is in the future (without blocking pool threads).
        std::chrono::steady_clock::time_point release_at;
        if (ComputeAvailableAtReleaseTime(local_meta, release_at)) {
            bool queued_delay = false;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                if (!shutting_down_.load() && alive->load() && !session->cancel_flag.load() &&
                    session->state.load() != StreamState::Stopped &&
                    streams_.find(session->id) != streams_.end()) {
                    pending_delayed_stream_starts_.push_back(PendingDelayedStreamStart{
                        session,
                        std::move(local_meta),
                        local_src,
                        release_at
                    });
                    queued_delay = true;
                }
            }
            if (queued_delay) {
                const StreamId sid = session->id;
                const PlayerId wait_pid = session->is_global ? INVALID_PLAYER_ID : session->owner_player_id;
                MainThreadDispatcher::Instance().Post([this, session, alive, sid, wait_pid, release_at]() {
                    if (!alive->load() || shutting_down_.load()) return;
                    std::int32_t wait_ms = 0;
                    {
                        std::lock_guard<std::mutex> lock(state_mutex_);
                        if (shutting_down_.load() || !session || session->cancel_flag.load() ||
                            session->state.load() == StreamState::Stopped ||
                            streams_.find(sid) == streams_.end()) {
                            return;
                        }
                        const bool still_pending = std::any_of(
                            pending_delayed_stream_starts_.begin(),
                            pending_delayed_stream_starts_.end(),
                            [sid](const PendingDelayedStreamStart& p) {
                                return p.session && p.session->id == sid;
                            });
                        if (!still_pending) return;
                        wait_ms = ComputeRemainingWaitMs(release_at);
                    }
                    Logger::Instance().Debug(
                        "Stream " + std::to_string(sid) + " waiting " + std::to_string(wait_ms) +
                        " ms for media availability");
                    DispatchAudioCallback("OnAudioStreamWaiting", {wait_pid, sid, wait_ms});
                });
            }
            return;
        }

        CompleteResolvedStreamStart(session, std::move(local_meta), local_src, false);
    })) {
        session->state.store(StreamState::Error);
    }
}

void AudioEngine::CompleteResolvedStreamStart(
    const std::shared_ptr<ActiveStreamSession>& session,
    TrackMetadata local_meta,
    SourceType local_src,
    bool run_main_inline) {
    auto alive = alive_token_;
    if (!session || shutting_down_.load() || !alive->load() || session->cancel_flag.load()) {
        return;
    }

    auto dispatch_main = [&](std::function<void()> fn) {
        if (run_main_inline) {
            fn();
        } else {
            MainThreadDispatcher::Instance().Post(std::move(fn));
        }
    };

    bool missing_personal_remote_url = false;
    bool warn_global_remote_url = false;
    std::shared_ptr<AudioStreamBuffer> buf;
    std::shared_ptr<StreamRunState> run;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (shutting_down_.load() || !alive->load() || session->cancel_flag.load() ||
            session->state.load() == StreamState::Stopped ||
            streams_.find(session->id) == streams_.end()) {
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
            run = std::make_shared<StreamRunState>();
            session->buffer = buf;
            session->active_run = run;
            session->state.store(session->pause_flag.load() ? StreamState::Paused : StreamState::Buffering);

            if (local_src == SourceType::DirectAudio && local_meta.is_live &&
                !local_meta.title_is_placeholder && !local_meta.title.empty()) {
                session->station_name = local_meta.title;
            }
            StreamEndpointInfo ep;
            ep.token = session->token;
            ep.title = local_meta.title;
            ep.artist = local_meta.artist;
            ep.station_name = session->station_name;
            ep.source_label = SessionSourceLabel(local_meta, config_.audio_file_label);
            ep.bitrate_kbps = config_.mp3_bitrate_kbps;
            ep.is_live_channel = session->is_global;
            ep.is_channel = false;
            ep.send_icy_headers = session->options.stream_text;
            ep.buffer = buf;
            http_server_.RegisterEndpoint(ep);
        }
    }

    if (missing_personal_remote_url) {
        if (!public_ip_error_logged_.exchange(true)) {
            Logger::Instance().Error(
                "Cannot deliver proxied stream URL to remote player: public IPv4 detection failed; configure 'public_ip'.");
        }
        dispatch_main([this, session, alive]() {
            if (!alive->load() || shutting_down_.load()) return;
            DispatchAudioCallback("OnAudioStreamError", {
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
        dispatch_main([this, session, alive]() {
            if (!alive->load() || shutting_down_.load()) return;
            DispatchAudioCallback("OnAudioStreamError", {
                INVALID_PLAYER_ID,
                session->id,
                static_cast<std::int32_t>(ErrorCode::NetworkError),
                std::string("Public IPv4 detection failed; remote players cannot receive global stream until 'public_ip' is configured.")
            });
        });
    }

    dispatch_main([this, session, alive, local_meta, local_src]() {
        if (!alive->load() || shutting_down_.load() || session->cancel_flag.load()) return;

        std::string endpoint_url;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (session->cancel_flag.load() || streams_.find(session->id) == streams_.end()) return;
            if (session->is_global) {
                endpoint_url = public_host_ip_.empty() ? "" : BuildEndpointUrlLocked("s", session->token);
            } else {
                endpoint_url = BuildEndpointUrlForPlayerLocked("s", session->token, session->owner_player_id);
            }
            auto now_tp = std::chrono::steady_clock::now();
            session->track_started_at = now_tp;
            session->paused_accumulated = std::chrono::steady_clock::duration::zero();
            session->paused_since = session->pause_flag.load() ? now_tp : std::chrono::steady_clock::time_point{};
            session->metadata = local_meta;
            session->source_type = local_src;
        }

        session->ready.store(true);
        session->state.store(session->pause_flag.load() ? StreamState::Paused : StreamState::Playing);

        DispatchAudioCallback("OnAudioStreamReady", {
            session->owner_player_id,
            session->id,
            ConvertTextForPawn(local_meta.title, config_.pawn_text_encoding),
            ConvertTextForPawn(local_meta.artist, config_.pawn_text_encoding),
            local_meta.duration_seconds,
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
                            auto& gvec = player_arbitration_[snap.id].global_sources;
                            GlobalAudioSourceRef ref{false, session->id};
                            if (std::find(gvec.begin(), gvec.end(), ref) == gvec.end()) {
                                gvec.push_back(ref);
                            }
                            ReconcilePlayerLocked(snap.id, &snap, cmds);
                        }
                    }
                }
            }
            ApplyPlayerAudioCommands(cmds);
            DispatchAudioCallback("OnAudioStreamStart", {INVALID_PLAYER_ID, session->id});
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
            DispatchAudioCallback("OnAudioStreamStart", {session->owner_player_id, session->id});
        }
        PollLiveTitles(std::chrono::steady_clock::now());
    });

    StartStreamTranscoderWorker(session, run, std::move(local_meta), buf, 0);
}

void AudioEngine::StartStreamTranscoderWorker(
    std::shared_ptr<ActiveStreamSession> session,
    std::shared_ptr<StreamRunState> run,
    TrackMetadata local_meta,
    std::shared_ptr<AudioStreamBuffer> buf,
    std::uint64_t start_offset_ms) {
    if (!session || !run || !buf) return;

    auto alive = alive_token_;

    TranscoderConfig tcfg;
    tcfg.target_sample_rate = config_.mp3_sample_rate;
    tcfg.target_bitrate_kbps = config_.mp3_bitrate_kbps;
    tcfg.initial_burst_ms = std::clamp<std::uint32_t>(config_.transcoder_burst_ms, 2000, 15000);
    tcfg.max_prefetch_mb = std::clamp<std::uint32_t>(config_.max_prefetch_mb, 1, 64);
    tcfg.pace_realtime = !local_meta.is_live;
    tcfg.emit_silence_on_pause = false;
    tcfg.normalize_volume = config_.normalize_volume;
    tcfg.start_offset_ms = start_offset_ms;
    tcfg.base_dir = config_.base_dir;
    tcfg.re_resolve_callback = [this, alive](
                                   const TrackMetadata& failed_meta,
                                   const std::atomic<bool>& dl_cancel,
                                   TrackMetadata& out_meta,
                                   std::string& out_error) {
        return ReResolveExtractorTrack(failed_meta, dl_cancel, alive, out_meta, out_error);
    };

    HttpStreamingServer* http_srv_ptr = &http_server_;
    std::atomic<bool>* shutdown_ptr = &shutting_down_;
    auto enqueued_at = std::chrono::steady_clock::now();

    bool queued = transcode_scheduler_.Enqueue([this, session, run, alive, local_meta, tcfg, buf, http_srv_ptr, shutdown_ptr, enqueued_at]() {
        if (run->cancel_flag.load() || session->cancel_flag.load() || shutdown_ptr->load() || !alive->load()) {
            if (session->cancel_flag.load()) {
                http_srv_ptr->UnregisterEndpoint(session->token);
            }
            return;
        }

        if ((std::chrono::steady_clock::now() - enqueued_at) > std::chrono::seconds(5)) {
            Logger::Instance().Error(
                "Stream " + std::to_string(session->id) + " (player " +
                std::to_string(session->owner_player_id) + ") transcode failed: Transcoder queue wait timed out after 5s");
            http_srv_ptr->UnregisterEndpoint(session->token);
            session->state.store(StreamState::Error);
            MainThreadDispatcher::Instance().Post([this, session, run, alive]() {
                if (!alive->load() || shutting_down_.load() || session->cancel_flag.load() || run->cancel_flag.load()) return;
                DispatchAudioCallback("OnAudioStreamError", {
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
        bool ok = transcoder.Run(run->cancel_flag, session->pause_flag, session->volume_percent, trans_err, &err_code);

        if (!run->cancel_flag.load() && !session->cancel_flag.load() && !shutdown_ptr->load() && alive->load()) {
            if (ok) {
                auto eof_at = std::chrono::steady_clock::now();
                std::uint32_t lead_ms = tcfg.pace_realtime ? tcfg.initial_burst_ms : 0;
                auto [min_dl, max_dl] = MakeCompletionDeadlines(eof_at, lead_ms);
                MainThreadDispatcher::Instance().Post([this, session, run, alive, min_dl, max_dl]() {
                    if (!alive->load() || shutting_down_.load() || session->cancel_flag.load() || run->cancel_flag.load()) {
                        if (session->cancel_flag.load()) {
                            http_server_.UnregisterEndpoint(session->token);
                        }
                        return;
                    }
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    if (streams_.find(session->id) == streams_.end() ||
                        session->active_run != run ||
                        session->cancel_flag.load() ||
                        session->state.load() == StreamState::Stopped) {
                        if (session->cancel_flag.load()) {
                            http_server_.UnregisterEndpoint(session->token);
                        }
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
                Logger::Instance().Error(
                    "Stream " + std::to_string(session->id) + " (player " +
                    std::to_string(session->owner_player_id) + ") transcode failed: " + trans_err);
                MainThreadDispatcher::Instance().Post([this, session, run, alive, err_code, trans_err]() {
                    if (!alive->load() || shutting_down_.load() || session->cancel_flag.load() || run->cancel_flag.load()) return;
                    DispatchAudioCallback("OnAudioStreamError", {
                        session->owner_player_id,
                        session->id,
                        static_cast<std::int32_t>(err_code),
                        ConvertTextForPawn(trans_err, config_.pawn_text_encoding)
                    });
                    StopAudioStream(session->id);
                });
            }
        } else if (session->cancel_flag.load()) {
            http_srv_ptr->UnregisterEndpoint(session->token);
        }
    }, &run->cancel_flag);

    if (!queued) {
        if (!run->cancel_flag.load() && !session->cancel_flag.load() && !shutting_down_.load() && alive->load()) {
            Logger::Instance().Error(
                "Stream " + std::to_string(session->id) + " (player " +
                std::to_string(session->owner_player_id) + ") transcode failed: Failed to schedule transcoder worker");
        }
        http_server_.UnregisterEndpoint(session->token);
        session->state.store(StreamState::Error);
        MainThreadDispatcher::Instance().Post([this, session, run, alive]() {
            if (!alive->load() || shutting_down_.load() || session->cancel_flag.load() || run->cancel_flag.load()) return;
            DispatchAudioCallback("OnAudioStreamError", {
                session->owner_player_id,
                session->id,
                static_cast<std::int32_t>(ErrorCode::InternalError),
                "Failed to schedule transcoder worker"
            });
            StopAudioStream(session->id);
        });
    }
}

bool AudioEngine::ReResolveExtractorTrack(
    const TrackMetadata& failed_meta,
    const std::atomic<bool>& cancel_flag,
    const std::shared_ptr<std::atomic<bool>>& alive,
    TrackMetadata& out_meta,
    std::string& out_error) {
    if (failed_meta.original_url.empty()) {
        out_error = "Missing original URL";
        return false;
    }
    if (cancel_flag.load() || shutting_down_.load() || !alive || !alive->load()) {
        out_error = "Cancelled";
        return false;
    }

    resolver_.InvalidateCachedTrack(failed_meta.original_url);

    struct ReResolveWaitState {
        std::mutex mtx;
        std::condition_variable cv;
        bool done{false};
        std::atomic<bool> combined_cancel{false};
        ResolveResult result;
    };
    auto wait_state = std::make_shared<ReResolveWaitState>();
    const std::string target_url = failed_meta.original_url;

    if (!extraction_pool_.Post([this, wait_state, target_url, alive]() {
            if (!wait_state->combined_cancel.load() && !shutting_down_.load() && alive->load()) {
                wait_state->result = resolver_.Resolve(target_url, &wait_state->combined_cancel);
            } else {
                wait_state->result.success = false;
                wait_state->result.error_message = "Cancelled";
            }
            {
                std::lock_guard<std::mutex> lk(wait_state->mtx);
                wait_state->done = true;
            }
            wait_state->cv.notify_all();
        })) {
        out_error = "Extractor thread pool stopped";
        return false;
    }

    std::unique_lock<std::mutex> lk(wait_state->mtx);
    while (!wait_state->done) {
        if (cancel_flag.load() || shutting_down_.load() || !alive->load()) {
            wait_state->combined_cancel.store(true);
            out_error = "Cancelled";
            return false;
        }
        wait_state->cv.wait_for(lk, std::chrono::milliseconds(50));
    }

    if (!wait_state->result.success) {
        out_error = wait_state->result.error_message.empty() ? "Re-resolve failed" : wait_state->result.error_message;
        return false;
    }
    out_meta = std::move(wait_state->result.metadata);
    out_meta.source_type = wait_state->result.source_type;
    return true;
}

bool AudioEngine::LoadSoundFile(
    const std::string& sound,
    std::shared_ptr<const std::string>& out_data,
    std::string& out_content_type,
    std::int64_t& out_duration_ms,
    std::string& out_error) {
    std::filesystem::path real_path;
    std::string ext_lower;
    ErrorCode err_code = ErrorCode::None;
    if (!StreamResolver::ParseAndResolveSoundFile(sound, config_.sound_dir, real_path, ext_lower, err_code, out_error)) {
        return false;
    }
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(real_path, ec);
    if (ec) {
        out_error = "Sound file cannot be read";
        return false;
    }
    if (size == 0 || size > kMaxSoundFileBytes) {
        out_error = "Sound file is empty or larger than 5 MB";
        return false;
    }
    const std::filesystem::file_time_type mtime = std::filesystem::last_write_time(real_path, ec);
    const std::string key = PathToUtf8(real_path);
    {
        std::lock_guard<std::mutex> lock(sound_cache_mutex_);
        auto it = sound_cache_.find(key);
        if (it != sound_cache_.end() && it->second.size == size && it->second.mtime == mtime && it->second.data) {
            it->second.last_use = ++sound_cache_use_counter_;
            out_data = it->second.data;
            out_content_type = it->second.content_type;
            out_duration_ms = it->second.duration_ms;
            return true;
        }
    }

    std::string bytes(static_cast<std::size_t>(size), '\0');
    std::ifstream ifs(real_path, std::ios::binary);
    if (!ifs || !ifs.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) {
        out_error = "Sound file cannot be read";
        return false;
    }
    std::int64_t duration_ms = 0;
    if (!ProbeSoundDurationMs(bytes, ext_lower, duration_ms)) {
        out_error = "Sound file is not a valid " + ext_lower.substr(1) + " file";
        return false;
    }

    CachedSoundFile entry;
    entry.data = std::make_shared<const std::string>(std::move(bytes));
    entry.content_type = SoundFileContentType(ext_lower);
    entry.duration_ms = duration_ms;
    entry.size = size;
    entry.mtime = mtime;
    out_data = entry.data;
    out_content_type = entry.content_type;
    out_duration_ms = duration_ms;
    std::lock_guard<std::mutex> lock(sound_cache_mutex_);
    auto existing = sound_cache_.find(key);
    if (existing != sound_cache_.end()) {
        sound_cache_bytes_ -= std::min<std::uint64_t>(sound_cache_bytes_, existing->second.size);
        sound_cache_.erase(existing);
    }
    if (size > kMaxSoundCacheBytes) {
        return true; // Played from the loaded copy, too large to keep cached.
    }
    while (!sound_cache_.empty() && sound_cache_bytes_ + size > kMaxSoundCacheBytes) {
        auto oldest = sound_cache_.begin();
        for (auto cit = sound_cache_.begin(); cit != sound_cache_.end(); ++cit) {
            if (cit->second.last_use < oldest->second.last_use) {
                oldest = cit;
            }
        }
        sound_cache_bytes_ -= std::min<std::uint64_t>(sound_cache_bytes_, oldest->second.size);
        sound_cache_.erase(oldest);
    }
    entry.last_use = ++sound_cache_use_counter_;
    sound_cache_bytes_ += size;
    sound_cache_[key] = std::move(entry);
    return true;
}

bool AudioEngine::PlayAudioStreamSoundForPlayer(
    PlayerId player_id,
    const std::string& sound,
    const Vector3& position,
    float distance,
    bool use_position) {
    if (!initialized_.load() || shutting_down_.load() || player_id < 0 || !host_.IsPlayerConnected(player_id)) {
        return false;
    }

    std::shared_ptr<const std::string> data;
    std::string content_type;
    std::int64_t duration_ms = 0;
    std::string err;
    if (!LoadSoundFile(sound, data, content_type, duration_ms, err)) {
        Logger::Instance().Warn("Sound '" + sound + "' cannot be played: " + err);
        return false;
    }

    std::vector<PlayerAudioCommand> cmds;
    std::string replaced_token;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto existing_arb = player_arbitration_.find(player_id);
        if (existing_arb != player_arbitration_.end() &&
            existing_arb->second.muted && existing_arb->second.mute_include_personal) {
            return false;
        }
        const std::string token = GenerateUniqueEndpointCodeLocked();
        const std::string url = BuildEndpointUrlForPlayerLocked("f", token, player_id);
        if (url.empty()) {
            return false;
        }
        auto& arb = player_arbitration_[player_id];

        // Keep the interrupted personal session's position so it resumes where the sound cut in.
        if (arb.client_source_kind == AudioSourcePriority::Personal && arb.personal_stream_id != INVALID_STREAM_ID) {
            auto sit = streams_.find(arb.personal_stream_id);
            if (sit != streams_.end() && sit->second && !ShouldUseDirectPassthrough(*sit->second) && sit->second->buffer) {
                auto& s = sit->second;
                const std::uint64_t earliest = s->buffer->EarliestAvailableOffset();
                const std::uint64_t written = s->buffer->TotalWritten();
                const std::uint64_t played = http_server_.GetEndpointPlayedOffset(s->token);
                s->resync_offset = std::clamp(std::max(played, s->resync_offset), earliest, written);
            }
        }

        auto old = player_sounds_.find(player_id);
        if (old != player_sounds_.end()) {
            replaced_token = old->second.token;
        }
        PlayerSoundState& st = player_sounds_[player_id];
        st.play_id = next_sound_play_id_++;
        st.token = token;
        st.url = url;
        st.position = position;
        st.distance = distance;
        st.use_position = use_position;
        st.duration_ms = duration_ms;
        st.fetched = false;
        if (replaced_token.empty()) {
            st.started_at = std::chrono::steady_clock::now();
        }
        // Fallback when the client never requests the file (audio streams disabled in its settings).
        st.expires_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(duration_ms + 5000);
        http_server_.RegisterFileEndpoint(FileEndpointInfo{token, data, content_type});
        ReconcilePlayerLocked(player_id, nullptr, cmds);
    }
    if (!replaced_token.empty()) {
        http_server_.UnregisterFileEndpoint(replaced_token);
    }
    ApplyPlayerAudioCommands(cmds);
    Logger::Instance().Debug(
        "Playing sound '" + sound + "' for player " + std::to_string(player_id) +
        " (" + std::to_string(duration_ms) + " ms)");
    return true;
}

void AudioEngine::UpdatePlayerSounds(std::chrono::steady_clock::time_point now) {
    std::vector<PlayerAudioCommand> cmds;
    std::vector<std::string> released_tokens;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (player_sounds_.empty()) {
            return;
        }
        std::vector<PlayerId> expired;
        for (auto& [pid, st] : player_sounds_) {
            if (!st.fetched) {
                std::chrono::steady_clock::time_point requested_at{};
                if (http_server_.GetFileEndpointFirstRequestTime(st.token, requested_at)) {
                    st.fetched = true;
                    // The client plays the complete file right after downloading it; a short margin covers its start-up.
                    st.expires_at = requested_at + std::chrono::milliseconds(st.duration_ms + 500);
                }
            }
            if (now >= st.expires_at) {
                expired.push_back(pid);
            }
        }
        for (PlayerId pid : expired) {
            auto it = player_sounds_.find(pid);
            if (it == player_sounds_.end()) continue;
            released_tokens.push_back(it->second.token);
            const auto silenced_for = now - it->second.started_at;
            player_sounds_.erase(it);
            ExtendPendingPersonalCompletionLocked(pid, silenced_for);
            ReconcilePlayerLocked(pid, nullptr, cmds);
        }
    }
    for (const auto& token : released_tokens) {
        http_server_.UnregisterFileEndpoint(token);
    }
    ApplyPlayerAudioCommands(cmds);
}

bool AudioEngine::IsPersonalAudioSilencedLocked(PlayerId player_id) const {
    if (player_id < 0) {
        return false;
    }
    if (player_sounds_.find(player_id) != player_sounds_.end()) {
        return true;
    }
    auto ait = player_arbitration_.find(player_id);
    return ait != player_arbitration_.end() && ait->second.muted && ait->second.mute_include_personal;
}

void AudioEngine::ExtendPendingPersonalCompletionLocked(PlayerId player_id, std::chrono::steady_clock::duration delay) {
    if (player_id < 0 || delay <= std::chrono::steady_clock::duration::zero() || IsPersonalAudioSilencedLocked(player_id)) {
        return;
    }
    for (auto& pending : pending_stream_completions_) {
        if (pending.session && !pending.session->is_global && pending.session->owner_player_id == player_id) {
            pending.min_deadline += delay;
            pending.max_deadline += delay;
        }
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
            session = RemoveStreamSessionLocked(it->second);
            if (session) {
                buf = session->buffer;
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
        if (session->active_run) {
            session->active_run->cancel_flag.store(true);
        }
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
        session = RemoveStreamSessionLocked(stream_id);
        if (!session) {
            return false;
        }
        CancelPendingCompletionLocked(stream_id);
        buf = session->buffer;

        session->cancel_flag.store(true);
        if (session->active_run) {
            session->active_run->cancel_flag.store(true);
        }
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
    StreamId sid = INVALID_STREAM_ID;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = player_streams_.find(player_id);
        if (it == player_streams_.end()) return false;
        sid = it->second;
    }
    return PauseAudioStream(sid, pause);
}

bool AudioEngine::PauseAudioStream(StreamId stream_id, bool pause) {
    bool fetch_global_snapshots = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto sit = streams_.find(stream_id);
        if (sit == streams_.end() || !sit->second) return false;
        const auto& session = *sit->second;
        StreamState cur = session.state.load();
        if (cur == StreamState::Stopped || cur == StreamState::Error || cur == StreamState::Finished ||
            session.cancel_flag.load()) {
            return false;
        }
        if (session.pause_flag.load() == pause) {
            return true;
        }
        if (!pause && session.is_global && session.ready.load()) {
            fetch_global_snapshots = true;
        }
    }

    std::vector<PlayerSnapshot> snapshots;
    if (fetch_global_snapshots) {
        snapshots = host_.GetConnectedPlayerSnapshots();
    }

    std::vector<PlayerAudioCommand> cmds;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto sit = streams_.find(stream_id);
        if (sit == streams_.end() || !sit->second) return false;
        auto& session = sit->second;
        StreamState cur = session->state.load();
        if (cur == StreamState::Stopped || cur == StreamState::Error || cur == StreamState::Finished ||
            session->cancel_flag.load()) {
            return false;
        }
        bool was_paused = session->pause_flag.load();
        if (was_paused == pause) {
            return true;
        }

        auto now = std::chrono::steady_clock::now();
        bool is_ready = session->ready.load();
        session->pause_flag.store(pause);
        if (pause) {
            if (cur == StreamState::Playing &&
                session->track_started_at != std::chrono::steady_clock::time_point{} &&
                session->paused_since == std::chrono::steady_clock::time_point{}) {
                session->paused_since = now;
            }
            session->state.store(StreamState::Paused);
            if (!ShouldUseDirectPassthrough(*session)) {
                session->paused_offset = http_server_.GetEndpointPlayedOffset(session->token);
            }
            if (session->is_global) {
                std::vector<PlayerId> listeners(session->active_listeners.begin(), session->active_listeners.end());
                for (PlayerId pid : listeners) {
                    ReconcilePlayerLocked(pid, nullptr, cmds);
                }
            } else {
                ReconcilePlayerLocked(session->owner_player_id, nullptr, cmds);
            }
        } else {
            if (session->paused_since != std::chrono::steady_clock::time_point{}) {
                if (now > session->paused_since) {
                    session->paused_accumulated += (now - session->paused_since);
                }
                session->paused_since = std::chrono::steady_clock::time_point{};
            }
            if (!ShouldUseDirectPassthrough(*session)) {
                session->resync_offset = session->paused_offset;
            }
            StreamState next_state = is_ready
                ? StreamState::Playing
                : (session->buffer ? StreamState::Buffering : StreamState::Resolving);
            session->state.store(next_state);
            if (is_ready) {
                std::uint32_t lead_ms = session->metadata.is_live
                    ? 0
                    : std::clamp<std::uint32_t>(config_.transcoder_burst_ms, 2000, 15000);
                auto [min_dl, max_dl] = MakeCompletionDeadlines(now, lead_ms);
                for (auto& pending : pending_stream_completions_) {
                    if (pending.session == session) {
                        pending.min_deadline = min_dl;
                        pending.max_deadline = max_dl;
                    }
                }
            }
            if (session->is_global) {
                if (fetch_global_snapshots) {
                    std::unordered_map<PlayerId, const PlayerSnapshot*> by_id;
                    by_id.reserve(snapshots.size());
                    for (const auto& snap : snapshots) {
                        by_id[snap.id] = &snap;
                    }
                    for (auto lit = session->active_listeners.begin(); lit != session->active_listeners.end();) {
                        if (by_id.find(*lit) == by_id.end()) {
                            PlayerId old_pid = *lit;
                            lit = session->active_listeners.erase(lit);
                            auto ait = player_arbitration_.find(old_pid);
                            if (ait != player_arbitration_.end()) {
                                auto& gvec = ait->second.global_sources;
                                GlobalAudioSourceRef ref{false, session->id};
                                gvec.erase(std::remove(gvec.begin(), gvec.end(), ref), gvec.end());
                            }
                            ReconcilePlayerLocked(old_pid, nullptr, cmds);
                        } else {
                            ++lit;
                        }
                    }
                    for (const auto& snap : snapshots) {
                        bool in_range = MatchesSpatialFilterSnapshot(snap, session->options);
                        bool was_listening = session->active_listeners.count(snap.id) > 0;
                        if (in_range && !was_listening) {
                            session->active_listeners.insert(snap.id);
                            auto& gvec = player_arbitration_[snap.id].global_sources;
                            GlobalAudioSourceRef ref{false, session->id};
                            if (std::find(gvec.begin(), gvec.end(), ref) == gvec.end()) {
                                gvec.push_back(ref);
                            }
                        } else if (!in_range && was_listening) {
                            session->active_listeners.erase(snap.id);
                            auto ait = player_arbitration_.find(snap.id);
                            if (ait != player_arbitration_.end()) {
                                auto& gvec = ait->second.global_sources;
                                GlobalAudioSourceRef ref{false, session->id};
                                gvec.erase(std::remove(gvec.begin(), gvec.end(), ref), gvec.end());
                            }
                        }
                        if (in_range || was_listening) {
                            ReconcilePlayerLocked(snap.id, &snap, cmds);
                        }
                    }
                } else {
                    std::vector<PlayerId> listeners(session->active_listeners.begin(), session->active_listeners.end());
                    for (PlayerId pid : listeners) {
                        ReconcilePlayerLocked(pid, nullptr, cmds);
                    }
                }
            } else {
                ReconcilePlayerLocked(session->owner_player_id, nullptr, cmds);
            }
        }
    }

    ApplyPlayerAudioCommands(cmds);
    return true;
}

bool AudioEngine::SetPlayerAudioVolume(PlayerId player_id, float volume) {
    bool is_passthrough = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = player_streams_.find(player_id);
        if (it == player_streams_.end()) return false;
        auto sit = streams_.find(it->second);
        if (sit == streams_.end() || !sit->second) return false;
        if (sit->second->is_passthrough.load()) {
            is_passthrough = true;
        } else {
            sit->second->volume_percent.store(std::clamp(volume, 0.0f, 200.0f));
        }
    }
    if (is_passthrough) {
        Logger::Instance().Debug("volume control requires a transcoded stream");
        return false;
    }
    return true;
}

bool AudioEngine::SetStreamVolume(StreamId stream_id, float volume) {
    bool is_passthrough = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = streams_.find(stream_id);
        if (it == streams_.end() || !it->second) return false;
        if (it->second->is_passthrough.load()) {
            is_passthrough = true;
        } else {
            it->second->volume_percent.store(std::clamp(volume, 0.0f, 200.0f));
        }
    }
    if (is_passthrough) {
        Logger::Instance().Debug("volume control requires a transcoded stream");
        return false;
    }
    return true;
}

bool AudioEngine::RefreshActiveSessionSpatialLocked(
    const std::shared_ptr<ActiveStreamSession>& session,
    const std::vector<PlayerSnapshot>* snapshots,
    std::vector<PlayerAudioCommand>& out_cmds) {
    if (!session || session->cancel_flag.load()) return false;
    StreamState cur = session->state.load();
    if (cur == StreamState::Stopped || cur == StreamState::Error || cur == StreamState::Finished) {
        return false;
    }

    ++session->options_version;

    // Not actively playing yet -> updated spatial config applies automatically when ready/resumed
    if (cur != StreamState::Playing || session->pause_flag.load()) {
        return true;
    }

    if (!ShouldUseDirectPassthrough(*session) && session->buffer) {
        std::uint64_t earliest = session->buffer->EarliestAvailableOffset();
        std::uint64_t written = session->buffer->TotalWritten();
        std::uint64_t played = http_server_.GetEndpointPlayedOffset(session->token);
        std::uint64_t target = std::max(played, session->resync_offset);
        if (target == 0 && written > 0) {
            if (session->is_global) {
                std::uint64_t bps = static_cast<std::uint64_t>(std::clamp<std::uint32_t>(config_.mp3_bitrate_kbps, 64, 320)) * 125ULL;
                std::uint64_t live_lag = bps * 2ULL;
                target = (written > live_lag) ? (written - live_lag) : earliest;
            } else {
                target = earliest;
            }
        }
        session->resync_offset = std::clamp(target, earliest, written);
    }

    if (session->is_global) {
        if (snapshots) {
            std::unordered_map<PlayerId, const PlayerSnapshot*> by_id;
            by_id.reserve(snapshots->size());
            for (const auto& snap : *snapshots) {
                by_id[snap.id] = &snap;
            }

            for (auto lit = session->active_listeners.begin(); lit != session->active_listeners.end();) {
                if (by_id.find(*lit) == by_id.end()) {
                    PlayerId old_pid = *lit;
                    lit = session->active_listeners.erase(lit);
                    auto ait = player_arbitration_.find(old_pid);
                    if (ait != player_arbitration_.end()) {
                        auto& gvec = ait->second.global_sources;
                        GlobalAudioSourceRef ref{false, session->id};
                        gvec.erase(std::remove(gvec.begin(), gvec.end(), ref), gvec.end());
                    }
                    ReconcilePlayerLocked(old_pid, nullptr, out_cmds);
                } else {
                    ++lit;
                }
            }

            for (const auto& snap : *snapshots) {
                bool in_range = MatchesSpatialFilterSnapshot(snap, session->options);
                bool was_listening = session->active_listeners.count(snap.id) > 0;
                if (in_range && !was_listening) {
                    session->active_listeners.insert(snap.id);
                    auto& gvec = player_arbitration_[snap.id].global_sources;
                    GlobalAudioSourceRef ref{false, session->id};
                    if (std::find(gvec.begin(), gvec.end(), ref) == gvec.end()) {
                        gvec.push_back(ref);
                    }
                } else if (!in_range && was_listening) {
                    session->active_listeners.erase(snap.id);
                    auto ait = player_arbitration_.find(snap.id);
                    if (ait != player_arbitration_.end()) {
                        auto& gvec = ait->second.global_sources;
                        GlobalAudioSourceRef ref{false, session->id};
                        gvec.erase(std::remove(gvec.begin(), gvec.end(), ref), gvec.end());
                    }
                }
                if (in_range || was_listening) {
                    ReconcilePlayerLocked(snap.id, &snap, out_cmds);
                }
            }
        } else {
            std::vector<PlayerId> listeners(session->active_listeners.begin(), session->active_listeners.end());
            for (PlayerId pid : listeners) {
                ReconcilePlayerLocked(pid, nullptr, out_cmds);
            }
        }
    } else if (session->owner_player_id != INVALID_PLAYER_ID) {
        bool in_range = (session->is_passthrough.load() &&
                         session->options.world_id == -1 &&
                         session->options.interior_id == -1 &&
                         !session->options.use_position &&
                         session->options.attach_player_id == INVALID_PLAYER_ID &&
                         session->options.attach_vehicle_id <= 0) ||
                        MatchesSpatialFilter(session->owner_player_id, session->options);
        if (in_range) {
            session->active_listeners.insert(session->owner_player_id);
        } else {
            session->active_listeners.erase(session->owner_player_id);
        }
        ReconcilePlayerLocked(session->owner_player_id, nullptr, out_cmds);
    }
    return true;
}

bool AudioEngine::SetPlayerAudioStreamDistance(PlayerId player_id, float distance) {
    if (!std::isfinite(distance) || distance < 0.0f) {
        return false;
    }

    std::vector<PlayerAudioCommand> cmds;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = player_streams_.find(player_id);
        if (it == player_streams_.end()) return false;
        auto sit = streams_.find(it->second);
        if (sit == streams_.end() || !sit->second) return false;
        StreamState cur = sit->second->state.load();
        if (cur == StreamState::Stopped || cur == StreamState::Error || cur == StreamState::Finished ||
            sit->second->cancel_flag.load()) {
            return false;
        }
        sit->second->options.distance = distance;
        RefreshActiveSessionSpatialLocked(sit->second, nullptr, cmds);
    }
    ApplyPlayerAudioCommands(cmds);
    return true;
}

bool AudioEngine::SetAudioStreamDistance(StreamId stream_id, float distance) {
    if (!std::isfinite(distance) || distance < 0.0f) {
        return false;
    }

    std::vector<PlayerSnapshot> snapshots = host_.GetConnectedPlayerSnapshots();
    std::vector<PlayerAudioCommand> cmds;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = streams_.find(stream_id);
        if (it == streams_.end() || !it->second) return false;
        StreamState cur = it->second->state.load();
        if (cur == StreamState::Stopped || cur == StreamState::Error || cur == StreamState::Finished ||
            it->second->cancel_flag.load()) {
            return false;
        }
        it->second->options.distance = distance;
        RefreshActiveSessionSpatialLocked(it->second, &snapshots, cmds);
    }
    ApplyPlayerAudioCommands(cmds);
    return true;
}

bool AudioEngine::SetPlayerAudioStreamPos(PlayerId player_id, float x, float y, float z) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
        return false;
    }

    std::vector<PlayerAudioCommand> cmds;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = player_streams_.find(player_id);
        if (it == player_streams_.end()) return false;
        auto sit = streams_.find(it->second);
        if (sit == streams_.end() || !sit->second) return false;
        StreamState cur = sit->second->state.load();
        if (cur == StreamState::Stopped || cur == StreamState::Error || cur == StreamState::Finished ||
            sit->second->cancel_flag.load()) {
            return false;
        }
        sit->second->options.position = Vector3{x, y, z};
        sit->second->options.use_position = true;
        sit->second->options.attach_player_id = INVALID_PLAYER_ID;
        sit->second->options.attach_vehicle_id = 0;
        RefreshActiveSessionSpatialLocked(sit->second, nullptr, cmds);
    }
    ApplyPlayerAudioCommands(cmds);
    return true;
}

bool AudioEngine::SetAudioStreamPos(StreamId stream_id, float x, float y, float z) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
        return false;
    }

    std::vector<PlayerSnapshot> snapshots = host_.GetConnectedPlayerSnapshots();
    std::vector<PlayerAudioCommand> cmds;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = streams_.find(stream_id);
        if (it == streams_.end() || !it->second) return false;
        StreamState cur = it->second->state.load();
        if (cur == StreamState::Stopped || cur == StreamState::Error || cur == StreamState::Finished ||
            it->second->cancel_flag.load()) {
            return false;
        }
        it->second->options.position = Vector3{x, y, z};
        it->second->options.use_position = true;
        it->second->options.attach_player_id = INVALID_PLAYER_ID;
        it->second->options.attach_vehicle_id = 0;
        RefreshActiveSessionSpatialLocked(it->second, &snapshots, cmds);
    }
    ApplyPlayerAudioCommands(cmds);
    return true;
}

bool AudioEngine::AttachStreamToPlayer(StreamId stream_id, PlayerId target_player_id) {
    if (target_player_id < 0 || target_player_id == PAWN_INVALID_PLAYER_ID || !host_.IsPlayerConnected(target_player_id)) {
        return false;
    }
    Vector3 target_pos{};
    if (!host_.GetPlayerPos(target_player_id, target_pos)) {
        return false;
    }
    std::int32_t target_world = host_.GetPlayerVirtualWorld(target_player_id);
    std::int32_t target_interior = host_.GetPlayerInterior(target_player_id);
    std::vector<PlayerSnapshot> snapshots = host_.GetConnectedPlayerSnapshots();

    std::vector<PlayerAudioCommand> cmds;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = streams_.find(stream_id);
        if (it == streams_.end() || !it->second) return false;
        StreamState cur = it->second->state.load();
        if (cur == StreamState::Stopped || cur == StreamState::Error || cur == StreamState::Finished ||
            it->second->cancel_flag.load()) {
            return false;
        }
        it->second->options.attach_player_id = target_player_id;
        it->second->options.attach_vehicle_id = 0;
        it->second->options.use_position = true;
        it->second->options.position = target_pos;
        it->second->options.world_id = target_world;
        it->second->options.interior_id = target_interior;
        RefreshActiveSessionSpatialLocked(it->second, it->second->is_global ? &snapshots : nullptr, cmds);
    }
    ApplyPlayerAudioCommands(cmds);
    return true;
}

bool AudioEngine::AttachStreamToVehicle(StreamId stream_id, std::int32_t vehicle_id) {
    Vector3 veh_pos{};
    if (vehicle_id <= 0 || !host_.GetVehiclePos(vehicle_id, veh_pos)) {
        return false;
    }
    std::int32_t veh_world = -1;
    std::int32_t veh_interior = -1;
    const bool has_veh_vw = host_.GetVehicleWorldInterior(vehicle_id, veh_world, veh_interior);
    std::vector<PlayerSnapshot> snapshots = host_.GetConnectedPlayerSnapshots();

    std::vector<PlayerAudioCommand> cmds;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = streams_.find(stream_id);
        if (it == streams_.end() || !it->second) return false;
        StreamState cur = it->second->state.load();
        if (cur == StreamState::Stopped || cur == StreamState::Error || cur == StreamState::Finished ||
            it->second->cancel_flag.load()) {
            return false;
        }
        it->second->options.attach_vehicle_id = vehicle_id;
        it->second->options.attach_player_id = INVALID_PLAYER_ID;
        it->second->options.use_position = true;
        it->second->options.position = veh_pos;
        if (has_veh_vw) {
            it->second->options.world_id = veh_world;
            it->second->options.interior_id = veh_interior;
        }
        RefreshActiveSessionSpatialLocked(it->second, it->second->is_global ? &snapshots : nullptr, cmds);
    }
    ApplyPlayerAudioCommands(cmds);
    return true;
}

void AudioEngine::UpdateSpatialStreams(const std::vector<PlayerSnapshot>* precomputed_snapshots) {
    struct SpatialWorkItem {
        std::shared_ptr<ActiveStreamSession> session;
        PlayerId attach_player_id{INVALID_PLAYER_ID};
        std::int32_t attach_vehicle_id{0};
    };
    struct ChannelSpatialWorkItem {
        std::shared_ptr<SyncChannel> channel;
        PlayerId attach_player_id{INVALID_PLAYER_ID};
        std::int32_t attach_vehicle_id{0};
    };

    std::vector<SpatialWorkItem> active_spatial;
    std::vector<ChannelSpatialWorkItem> active_spatial_channels;
    bool allow_vehicle_detach = true;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        allow_vehicle_detach = gamemode_loaded_;
        active_spatial.reserve(streams_.size());
        for (const auto& [id, s] : streams_) {
            if (s->state.load() == StreamState::Playing && !s->cancel_flag.load() && !s->pause_flag.load() &&
                (s->is_global || s->options.world_id != -1 || s->options.interior_id != -1 || s->options.use_position)) {
                active_spatial.push_back(SpatialWorkItem{
                    s, s->options.attach_player_id, s->options.attach_vehicle_id
                });
            }
        }
        for (const auto& [cid, ch] : channels_) {
            if (!ch || !ch->spatial.use_position) continue;
            StreamState st = ch->state.load();
            bool has_attach = (ch->spatial.attach_player_id != INVALID_PLAYER_ID || ch->spatial.attach_vehicle_id > 0);
            if (st == StreamState::Playing || st == StreamState::Paused || has_attach || !ch->range_listeners.empty()) {
                active_spatial_channels.push_back(ChannelSpatialWorkItem{
                    ch, ch->spatial.attach_player_id, ch->spatial.attach_vehicle_id
                });
            }
        }
    }

    if (active_spatial.empty() && active_spatial_channels.empty()) return;

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
        bool detach_player{false};
        bool detach_vehicle{false};
    };
    auto resolve_transform = [&](PlayerId attach_pid, std::int32_t attach_vid) {
        UpdatedTransform tr;
        if (attach_pid != INVALID_PLAYER_ID) {
            auto it = snapshot_by_id.find(attach_pid);
            if (it != snapshot_by_id.end()) {
                tr.has_pos = true;
                tr.pos = it->second->pos;
                tr.has_vw = true;
                tr.world_id = it->second->world_id;
                tr.interior_id = it->second->interior_id;
            } else {
                tr.detach_player = true;
            }
        } else if (attach_vid > 0) {
            Vector3 vpos{};
            if (host_.GetVehiclePos(attach_vid, vpos)) {
                tr.has_pos = true;
                tr.pos = vpos;
                std::int32_t vw = -1;
                std::int32_t vi = -1;
                if (host_.GetVehicleWorldInterior(attach_vid, vw, vi)) {
                    tr.has_vw = true;
                    tr.world_id = vw;
                    tr.interior_id = vi;
                }
            } else if (allow_vehicle_detach) {
                tr.detach_vehicle = true;
            }
        }
        return tr;
    };

    std::vector<UpdatedTransform> transforms(active_spatial.size());
    for (std::size_t i = 0; i < active_spatial.size(); ++i) {
        transforms[i] = resolve_transform(active_spatial[i].attach_player_id, active_spatial[i].attach_vehicle_id);
    }
    std::vector<UpdatedTransform> ch_transforms(active_spatial_channels.size());
    for (std::size_t i = 0; i < active_spatial_channels.size(); ++i) {
        ch_transforms[i] = resolve_transform(
            active_spatial_channels[i].attach_player_id,
            active_spatial_channels[i].attach_vehicle_id);
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
                        auto& gvec = player_arbitration_[snap.id].global_sources;
                        GlobalAudioSourceRef ref{false, session->id};
                        if (std::find(gvec.begin(), gvec.end(), ref) == gvec.end()) {
                            gvec.push_back(ref);
                        }
                        ReconcilePlayerLocked(snap.id, &snap, cmds);
                    } else if (!in_range && was_listening) {
                        session->active_listeners.erase(snap.id);
                        auto ait = player_arbitration_.find(snap.id);
                        if (ait != player_arbitration_.end()) {
                            auto& gvec = ait->second.global_sources;
                            GlobalAudioSourceRef ref{false, session->id};
                            gvec.erase(std::remove(gvec.begin(), gvec.end(), ref), gvec.end());
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

        for (std::size_t i = 0; i < active_spatial_channels.size(); ++i) {
            auto& ch = active_spatial_channels[i].channel;
            if (!ch || channels_.find(ch->id) == channels_.end()) continue;

            bool detached = false;
            if (ch_transforms[i].detach_player && ch->spatial.attach_player_id != INVALID_PLAYER_ID) {
                ch->spatial.attach_player_id = INVALID_PLAYER_ID;
                MarkChannelDirtyLocked(*ch);
                detached = true;
            }
            if (ch_transforms[i].detach_vehicle && ch->spatial.attach_vehicle_id > 0) {
                ch->spatial.attach_vehicle_id = 0;
                MarkChannelDirtyLocked(*ch);
                detached = true;
            }
            if (detached) {
                ++ch->spatial_version;
            }
            if (ch_transforms[i].has_pos) {
                ch->spatial.position = ch_transforms[i].pos;
            }
            if (ch_transforms[i].has_vw) {
                ch->spatial.world_id = ch_transforms[i].world_id;
                ch->spatial.interior_id = ch_transforms[i].interior_id;
            }

            RefreshChannelSpatialLocked(ch, &snapshots, cmds);
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
            ErrorCode err_code = ErrorCode::None;
            std::string err_msg;
            if (res.success) {
                for (std::int32_t i = 0; i < total; ++i) {
                    const auto& item = res.items[static_cast<std::size_t>(i)];
                    DispatchAudioCallback("OnAudioStreamSearchResultItem", {
                        player_id,
                        sid,
                        i,
                        ConvertTextForPawn(item.title, config_.pawn_text_encoding),
                        ConvertTextForPawn(item.artist, config_.pawn_text_encoding),
                        item.duration_seconds,
                        item.id
                    });
                }
            } else {
                err_code = (res.error != ErrorCode::None) ? res.error : ErrorCode::SearchFailed;
                std::string raw_err = res.error_message.empty() ? "Search failed" : res.error_message;
                err_msg = ConvertTextForPawn(raw_err, config_.pawn_text_encoding);
            }
            DispatchAudioCallback("OnAudioStreamSearchResults", {
                player_id,
                sid,
                total,
                static_cast<std::int32_t>(err_code),
                err_msg
            });
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

std::int32_t AudioEngine::ComputeChannelRemainingTimeoutSecondsLocked(
    const SyncChannel& ch,
    std::chrono::steady_clock::time_point now) const {
    if (ch.destroy_mode != ChannelDestroyMode::Timeout) {
        return 0;
    }
    std::int32_t timeout_sec = std::max<std::int32_t>(0, ch.destroy_timeout_seconds);
    if (ch.creator_connected || ch.creator_disconnected_at == std::chrono::steady_clock::time_point{}) {
        return timeout_sec;
    }
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - ch.creator_disconnected_at).count();
    if (elapsed >= timeout_sec) {
        return 0;
    }
    return static_cast<std::int32_t>(timeout_sec - elapsed);
}

void AudioEngine::CleanupExpiredChannels(std::chrono::steady_clock::time_point now) {
    std::vector<std::pair<ChannelId, ChannelDestroyReason>> expired;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        for (const auto& [cid, ch] : channels_) {
            if (!ch || ch->creator_connected) {
                continue;
            }
            if (ch->destroy_mode == ChannelDestroyMode::Timeout) {
                if (ch->creator_disconnected_at == std::chrono::steady_clock::time_point{}) {
                    continue;
                }
                std::int32_t timeout_sec = std::max<std::int32_t>(0, ch->destroy_timeout_seconds);
                if ((now - ch->creator_disconnected_at) >= std::chrono::seconds(timeout_sec)) {
                    expired.emplace_back(cid, ChannelDestroyReason::TimeoutExpired);
                }
            } else if (ch->destroy_mode == ChannelDestroyMode::OnDisconnect &&
                       ch->restore_grace_deadline != std::chrono::steady_clock::time_point{} &&
                       now >= ch->restore_grace_deadline) {
                expired.emplace_back(cid, ChannelDestroyReason::CreatorDisconnected);
            }
        }
    }
    for (const auto& [cid, reason] : expired) {
        DestroyChannelInternal(cid, reason);
    }
}

ChannelId AudioEngine::CreateChannel(
    const std::string& name,
    PlayerId creator_player_id,
    const std::string& password,
    ChannelDestroyMode destroy_mode,
    std::int32_t destroy_timeout_seconds,
    bool persistent) {
    auto ch = std::make_shared<SyncChannel>();
    ch->name = name;
    ch->persistent = persistent;
    if (!password.empty()) {
        ch->password_salt = GenerateEndpointCode() + GenerateEndpointCode();
        ch->password_hash = ComputeStringSha256Hex(ch->password_salt + password);
    }
    ch->destroy_mode = destroy_mode;
    ch->destroy_timeout_seconds = std::max<std::int32_t>(0, destroy_timeout_seconds);

    bool warn_no_creator = false;
    if (creator_player_id >= 0 && creator_player_id != PAWN_INVALID_PLAYER_ID &&
        host_.IsPlayerConnected(creator_player_id)) {
        ch->has_creator = true;
        ch->creator_player_id = creator_player_id;
        ch->creator_name = host_.GetPlayerName(creator_player_id);
        if (ch->creator_name.empty()) {
            ch->creator_name = "Player #" + std::to_string(creator_player_id);
        }
        ch->creator_connected = true;
    } else {
        ch->has_creator = false;
        ch->creator_player_id = INVALID_PLAYER_ID;
        ch->creator_name = "Server";
        ch->creator_connected = false;
        if (destroy_mode != ChannelDestroyMode::Manual) {
            warn_no_creator = true;
            ch->destroy_mode = ChannelDestroyMode::Manual;
            ch->destroy_timeout_seconds = 0;
        }
    }

    bool limit_reached = false;
    std::size_t max_ch = std::max<std::uint32_t>(1, config_.max_channels);
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (channels_.size() >= max_ch) {
            limit_reached = true;
        } else {
            ch->id = next_channel_id_.fetch_add(1);
            ch->token = GenerateUniqueEndpointCodeLocked();
            channels_[ch->id] = ch;
            AddSortedChannelIdLocked(ch->id);
            MarkChannelDirtyLocked(*ch);
        }
    }
    if (limit_reached) {
        Logger::Instance().Warn("Maximum channel limit reached (" + std::to_string(max_ch) + ")");
        return INVALID_CHANNEL_ID;
    }
    if (warn_no_creator) {
        Logger::Instance().Warn(
            "Channel " + std::to_string(ch->id) +
            " created without a creator; channel has no creator; use SA_SetAudioStreamChannelCreator first (defaulting destroy policy to Manual).");
    }
    return ch->id;
}

bool AudioEngine::DestroyChannel(ChannelId channel_id) {
    return DestroyChannelInternal(channel_id, ChannelDestroyReason::Manual);
}

bool AudioEngine::DestroyChannelInternal(ChannelId channel_id, ChannelDestroyReason reason) {
    std::shared_ptr<SyncChannel> ch;
    std::shared_ptr<ChannelRunState> old_run;
    std::vector<PlayerAudioCommand> cmds;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channels_.find(channel_id);
        if (it == channels_.end()) return false;
        ch = it->second;

        if (ch->persist_cancelled) {
            ch->persist_cancelled->store(true);
        }
        if (ch->playlist_cancel) {
            ch->playlist_cancel->store(true);
        }
        ch->persist_dirty = false;
        CancelPendingChannelCompletionLocked(channel_id);
        old_run = ch->active_run;
        ch->active_run.reset();
        ch->state.store(StreamState::Stopped);
        DetachChannelRangeListenersLocked(ch, cmds);
        channels_.erase(it);
        RemoveSortedChannelIdLocked(channel_id);
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
    if (ch->persistent) {
        DeleteChannelPersistFile(channel_id);
    }

    ApplyPlayerAudioCommands(cmds);
    if (!shutting_down_.load() && alive_token_ && alive_token_->load()) {
        auto alive = alive_token_;
        MainThreadDispatcher::Instance().Post([this, alive, channel_id, reason]() {
            if (!alive->load() || shutting_down_.load()) {
                return;
            }
            host_.DispatchPawnCallback("OnAudioStreamChannelDestroyed", {
                channel_id,
                static_cast<std::int32_t>(reason)
            });
        });
    }
    return true;
}

bool AudioEngine::PlayChannelLocked(
    ChannelId channel_id,
    const std::string& url_or_query,
    const std::string& original_input_pawn,
    std::shared_ptr<SyncChannel>& out_ch,
    std::shared_ptr<ChannelRunState>& out_run,
    const std::string& utf8_label) {
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
        ep.station_name = ch->name;
        ep.bitrate_kbps = config_.mp3_bitrate_kbps;
        ep.is_live_channel = true;
        ep.is_channel = true;
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
    const std::string clean_utf8_label = SanitizeQueueLabelUtf8(utf8_label);
    const std::string clean_pawn_label = clean_utf8_label.empty()
        ? std::string()
        : ConvertTextForPawn(clean_utf8_label, config_.pawn_text_encoding);
    ch->current_input = ChannelQueueEntry{url_or_query, original_input_pawn, clean_utf8_label, clean_pawn_label};
    ch->live_title.clear();
    ch->live_poll = LiveTitlePollState{};
    ch->restore_wanted = false;
    ch->restore_retry_blocked = false;
    ch->track_started_at = std::chrono::steady_clock::time_point{};
    ch->paused_since = std::chrono::steady_clock::time_point{};
    ch->paused_accumulated = std::chrono::steady_clock::duration::zero();
    ch->seek_base_offset = std::chrono::seconds::zero();
    ch->pause_flag.store(false);
    ch->state.store(StreamState::Resolving);
    MarkChannelDirtyLocked(*ch);
    out_ch = ch;
    return true;
}

bool AudioEngine::PlayChannel(
    ChannelId channel_id,
    const std::string& url_or_query,
    const std::string& original_input_pawn) {
    if (url_or_query.empty()) return false;
    std::shared_ptr<SyncChannel> ch;
    std::shared_ptr<ChannelRunState> run;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        std::int32_t resume_pos = -1;
        auto cur = channels_.find(channel_id);
        if (cur != channels_.end() && cur->second && cur->second->playlist_mode) {
            const SyncChannel& prev = *cur->second;
            if (prev.active_run && prev.current_from_queue && prev.playlist_pos >= 0) {
                resume_pos = prev.playlist_pos;
            } else if (prev.active_run && !prev.current_from_queue) {
                resume_pos = prev.playlist_resume_pos;
            }
        }
        if (!PlayChannelLocked(channel_id, url_or_query, original_input_pawn, ch, run)) {
            return false;
        }
        ch->playlist_pos = -1;
        ch->playlist_resume_pos = resume_pos;
        ch->current_from_queue = false;
        ch->consecutive_errors = 0;
        MarkChannelDirtyLocked(*ch);
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
    std::vector<PlayerAudioCommand>& out_cmds,
    bool& out_queue_ended) {
    out_queue_ended = false;
    const bool natural_end = (terminal_state == StreamState::Finished);

    auto start_track_from_entry = [&](const ChannelQueueEntry& entry, std::int32_t new_pos, bool from_queue) {
        channel->current_input = entry;
        out_next_track = entry.utf8_url;
        channel->playlist_pos = new_pos;
        channel->current_from_queue = from_queue;
        channel->next_generation = channel->buffer->PrepareNextTrack();
        out_next_run = std::make_shared<ChannelRunState>();
        out_next_run->generation = channel->next_generation;
        channel->active_run = out_next_run;
        channel->live_title.clear();
        channel->live_poll = LiveTitlePollState{};
        channel->track_started_at = std::chrono::steady_clock::time_point{};
        channel->paused_since = std::chrono::steady_clock::time_point{};
        channel->paused_accumulated = std::chrono::steady_clock::duration::zero();
        channel->seek_base_offset = std::chrono::seconds::zero();
        channel->state.store(StreamState::Resolving);
        MarkChannelDirtyLocked(*channel);
    };

    auto stop_playback = [&](bool emit_queue_end, bool reset_pos_and_from_queue) {
        channel->state.store(terminal_state);
        channel->active_run.reset();
        channel->current_track = TrackMetadata{};
        channel->current_input = ChannelQueueEntry{};
        if (reset_pos_and_from_queue) {
            channel->playlist_pos = -1;
            channel->current_from_queue = false;
        }
        channel->live_title.clear();
        channel->live_poll = LiveTitlePollState{};
        channel->track_started_at = std::chrono::steady_clock::time_point{};
        channel->paused_since = std::chrono::steady_clock::time_point{};
        channel->paused_accumulated = std::chrono::steady_clock::duration::zero();
        channel->seek_base_offset = std::chrono::seconds::zero();
        DetachChannelRangeListenersLocked(channel, out_cmds);
        for (PlayerId pid : channel->listeners) {
            ReconcilePlayerLocked(pid, nullptr, out_cmds);
        }
        MarkChannelDirtyLocked(*channel);
        if (emit_queue_end) {
            out_queue_ended = true;
        }
    };

    if (channel->playlist_mode) {
        if (!channel->current_from_queue) {
            // Out-of-list PlayChannel track: repeat it on LOOP_TRACK, otherwise continue the playlist after
            // the item that was playing before it (or from the first item).
            if (natural_end && channel->loop_mode == ChannelLoopMode::Track && !channel->current_input.utf8_url.empty()) {
                start_track_from_entry(channel->current_input, -1, false);
                return;
            }
            const std::int32_t next_pos = NextPlaylistIndexAfterOutOfListLocked(*channel);
            if (next_pos >= 0) {
                channel->playlist_resume_pos = -1;
                start_track_from_entry(channel->queue[static_cast<std::size_t>(next_pos)], next_pos, true);
                return;
            }
            stop_playback(false, true);
            return;
        }
        if (natural_end && channel->loop_mode == ChannelLoopMode::Track && !channel->current_input.utf8_url.empty()) {
            start_track_from_entry(channel->current_input, channel->playlist_pos, true);
            return;
        }
        if (channel->queue.empty()) {
            stop_playback(natural_end && channel->loop_mode == ChannelLoopMode::Off, true);
            return;
        }
        const std::int32_t q_size = static_cast<std::int32_t>(channel->queue.size());
        std::int32_t next_pos = (channel->playlist_pos >= -1 && channel->playlist_pos < q_size)
            ? (channel->playlist_pos + 1)
            : 0;
        if (next_pos < q_size) {
            start_track_from_entry(channel->queue[static_cast<std::size_t>(next_pos)], next_pos, true);
            return;
        }
        if (channel->loop_mode == ChannelLoopMode::Queue) {
            start_track_from_entry(channel->queue.front(), 0, true);
            return;
        }
        // Reached end of playlist in LoopOff (or error on last track in LoopTrack/Off): keep list, pos = -1.
        stop_playback(natural_end && channel->loop_mode == ChannelLoopMode::Off, true);
        return;
    }

    if (natural_end && channel->loop_mode == ChannelLoopMode::Track && !channel->current_input.utf8_url.empty()) {
        start_track_from_entry(channel->current_input, -1, channel->current_from_queue);
        return;
    }

    if (natural_end && channel->loop_mode == ChannelLoopMode::Queue && !channel->current_input.utf8_url.empty()) {
        channel->queue.push_back(channel->current_input);
    }

    if (!channel->queue.empty()) {
        ChannelQueueEntry next_entry = channel->queue.front();
        channel->queue.pop_front();
        start_track_from_entry(next_entry, -1, true);
    } else {
        stop_playback(natural_end && channel->loop_mode == ChannelLoopMode::Off, true);
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
    bool hit_error_cap = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (channel->active_run == run) {
            active_match = true;
            ++channel->consecutive_errors;
            if (channel->consecutive_errors >= 5) {
                hit_error_cap = true;
                channel->state.store(StreamState::Stopped);
                channel->active_run.reset();
                channel->current_track = TrackMetadata{};
                channel->current_input = ChannelQueueEntry{};
                channel->playlist_pos = -1;
                channel->current_from_queue = false;
                channel->live_title.clear();
                channel->live_poll = LiveTitlePollState{};
                channel->track_started_at = std::chrono::steady_clock::time_point{};
                channel->paused_since = std::chrono::steady_clock::time_point{};
                channel->paused_accumulated = std::chrono::steady_clock::duration::zero();
                channel->seek_base_offset = std::chrono::seconds::zero();
                DetachChannelRangeListenersLocked(channel, cmds);
                for (PlayerId pid : channel->listeners) {
                    ReconcilePlayerLocked(pid, nullptr, cmds);
                }
                MarkChannelDirtyLocked(*channel);
            } else {
                bool dummy_ended = false;
                AdvanceChannelOrEndLocked(channel, StreamState::Error, next_track, next_run, cmds, dummy_ended);
            }
        }
    }
    if (!active_match) return;

    host_.DispatchPawnCallback("OnAudioStreamChannelError", {
        channel->id,
        static_cast<std::int32_t>(err_code),
        ConvertTextForPawn(err_msg, config_.pawn_text_encoding)
    });
    if (hit_error_cap) {
        host_.DispatchPawnCallback("OnAudioStreamChannelError", {
            channel->id,
            static_cast<std::int32_t>(ErrorCode::InternalError),
            std::string("Too many consecutive track failures; channel stopped")
        });
    }
    ApplyPlayerAudioCommands(cmds);
    if (!hit_error_cap && next_run && !next_track.empty()) {
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
                if (channel->active_run == run) {
                    queued_next = GetNextChannelTrackUrlLocked(*channel);
                }
            }
            if (!queued_next.empty() && queued_next != url_or_query) {
                PreResolveNextChannelTrack(channel, queued_next);
            }
        }

        TrackMetadata local_track = std::move(resolved.metadata);
        std::chrono::steady_clock::time_point release_at;
        if (ComputeAvailableAtReleaseTime(local_track, release_at)) {
            bool queued_delay = false;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                if (!shutting_down_.load() && alive->load() && !run->cancel_flag.load() &&
                    channel->active_run == run && channels_.find(channel->id) != channels_.end()) {
                    pending_delayed_channel_starts_.push_back(PendingDelayedChannelStart{
                        channel,
                        run,
                        std::move(local_track),
                        release_at
                    });
                    queued_delay = true;
                }
            }
            if (queued_delay) {
                const ChannelId cid = channel->id;
                MainThreadDispatcher::Instance().Post([this, channel, run, alive, cid, release_at]() {
                    if (!alive->load() || shutting_down_.load() || !run || run->cancel_flag.load()) return;
                    std::int32_t wait_ms = 0;
                    {
                        std::lock_guard<std::mutex> lock(state_mutex_);
                        if (shutting_down_.load() || !channel || !run || run->cancel_flag.load() ||
                            channel->active_run != run || channels_.find(cid) == channels_.end()) {
                            return;
                        }
                        const bool still_pending = std::any_of(
                            pending_delayed_channel_starts_.begin(),
                            pending_delayed_channel_starts_.end(),
                            [cid, &run](const PendingDelayedChannelStart& p) {
                                return p.channel && p.channel->id == cid && p.run == run;
                            });
                        if (!still_pending) return;
                        wait_ms = ComputeRemainingWaitMs(release_at);
                    }
                    Logger::Instance().Debug(
                        "Channel " + std::to_string(cid) + " waiting " + std::to_string(wait_ms) +
                        " ms for track availability");
                    host_.DispatchPawnCallback("OnAudioStreamChannelTrackWaiting", {cid, wait_ms});
                });
            }
            return;
        }

        MainThreadDispatcher::Instance().Post([this, channel, run, local_track = std::move(local_track), alive]() mutable {
            if (!alive->load() || shutting_down_.load() || run->cancel_flag.load()) return;
            CompleteResolvedChannelTrackStart(channel, run, std::move(local_track));
        });
    })) {
        Logger::Instance().Error("Failed to schedule channel extraction worker for channel " + std::to_string(channel->id));
        MainThreadDispatcher::Instance().Post([this, channel, run, alive]() {
            if (!alive->load() || shutting_down_.load() || run->cancel_flag.load()) return;
            HandleChannelTrackError(channel, run, ErrorCode::InternalError, "Failed to schedule channel extraction worker");
        });
    }
}

void AudioEngine::CompleteResolvedChannelTrackStart(
    const std::shared_ptr<SyncChannel>& channel,
    const std::shared_ptr<ChannelRunState>& run,
    TrackMetadata local_track) {
    if (!channel || !run || shutting_down_.load() || !alive_token_->load() || run->cancel_flag.load()) {
        return;
    }

    auto alive = alive_token_;
    std::vector<PlayerSnapshot> snapshots;
    bool fetched_snaps = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (channel->active_run != run || channels_.find(channel->id) == channels_.end()) return;
        fetched_snaps = channel->spatial.use_position && channel->auto_listen;
    }
    if (fetched_snaps) {
        snapshots = host_.GetConnectedPlayerSnapshots();
    }

    std::vector<PlayerAudioCommand> cmds;
    std::string cb_title;
    std::string cb_artist;
    std::string cb_url;
    std::string icy_title;
    std::int32_t cb_duration = 0;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (channel->active_run != run || channels_.find(channel->id) == channels_.end()) return;
        channel->current_track = local_track;
        channel->consecutive_errors = 0;
        auto start_now = std::chrono::steady_clock::now();
        channel->seek_base_offset = std::chrono::seconds(std::max<std::int32_t>(0, run->start_offset_seconds));
        channel->track_started_at = start_now;
        channel->paused_accumulated = std::chrono::steady_clock::duration::zero();
        channel->paused_since = channel->pause_flag.load() ? start_now : std::chrono::steady_clock::time_point{};
        channel->state.store(channel->pause_flag.load() ? StreamState::Paused : StreamState::Playing);
        if (channel->spatial.use_position) {
            RefreshChannelSpatialLocked(channel, fetched_snaps ? &snapshots : nullptr, cmds);
        } else {
            for (PlayerId pid : channel->listeners) {
                ReconcilePlayerLocked(pid, nullptr, cmds);
            }
        }
        cb_title = ConvertTextForPawn(local_track.title, config_.pawn_text_encoding);
        cb_artist = ConvertTextForPawn(local_track.artist, config_.pawn_text_encoding);
        cb_duration = local_track.duration_seconds;
        cb_url = channel->current_input.pawn_url.empty()
            ? ConvertTextForPawn(channel->current_input.utf8_url, config_.pawn_text_encoding)
            : channel->current_input.pawn_url;
        icy_title = local_track.title.empty() ? channel->name : local_track.title;
    }
    http_server_.UpdateEndpointMetadata(channel->token, icy_title, local_track.artist, channel->buffer);
    std::string preserved_live_title;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (channel->active_run == run) {
            preserved_live_title = channel->live_title;
        }
    }
    if (!preserved_live_title.empty()) {
        http_server_.UpdateEndpointLiveTitle(channel->token, preserved_live_title);
    }
    ApplyPlayerAudioCommands(cmds);
    host_.DispatchPawnCallback("OnAudioStreamChannelTrackStart", {
        channel->id,
        cb_title,
        cb_artist,
        cb_duration,
        cb_url
    });
    PollLiveTitles(std::chrono::steady_clock::now());

    if (run->cancel_flag.load() || shutting_down_.load() || !alive->load()) return;

    TranscoderConfig tcfg;
    tcfg.target_sample_rate = config_.mp3_sample_rate;
    tcfg.target_bitrate_kbps = config_.mp3_bitrate_kbps;
    tcfg.initial_burst_ms = std::clamp<std::uint32_t>(config_.transcoder_burst_ms, 2000, 15000);
    tcfg.max_prefetch_mb = std::clamp<std::uint32_t>(config_.max_prefetch_mb, 1, 64);
    tcfg.buffer_generation = run->generation;
    tcfg.pace_realtime = !local_track.is_live;
    tcfg.emit_silence_on_pause = true;
    tcfg.normalize_volume = config_.normalize_volume;
    tcfg.start_offset_ms = static_cast<std::uint64_t>(std::max<std::int32_t>(0, run->start_offset_seconds)) * 1000ULL;
    tcfg.base_dir = config_.base_dir;
    tcfg.re_resolve_callback = [this, alive](
                                   const TrackMetadata& failed_meta,
                                   const std::atomic<bool>& dl_cancel,
                                   TrackMetadata& out_meta,
                                   std::string& out_error) {
        return ReResolveExtractorTrack(failed_meta, dl_cancel, alive, out_meta, out_error);
    };

    std::atomic<bool>* shutdown_ptr = &shutting_down_;
    auto enqueued_at = std::chrono::steady_clock::now();
    bool queued = transcode_scheduler_.Enqueue([this, channel, run, local_track = std::move(local_track), tcfg, alive, shutdown_ptr, enqueued_at]() {
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
        HandleChannelTrackError(channel, run, ErrorCode::InternalError, "Failed to schedule channel transcoder");
    }
}

bool AudioEngine::QueueChannel(
    ChannelId channel_id,
    const std::string& url_or_query,
    const std::string& original_input_pawn,
    const std::string& utf8_label) {
    if (url_or_query.empty()) return false;
    const std::string clean_utf8_label = SanitizeQueueLabelUtf8(utf8_label);
    const std::string clean_pawn_label = clean_utf8_label.empty()
        ? std::string()
        : ConvertTextForPawn(clean_utf8_label, config_.pawn_text_encoding);
    std::shared_ptr<SyncChannel> ch_start;
    std::shared_ptr<ChannelRunState> run_start;
    std::shared_ptr<SyncChannel> ch_preresolve;
    std::string preresolve_url;
    bool queue_limit_hit = false;
    const std::size_t max_q = std::max<std::uint32_t>(1, config_.max_queue_per_channel);
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channels_.find(channel_id);
        if (it == channels_.end()) return false;
        auto& ch = *it->second;
        if (ch.playlist_mode) {
            if (ch.queue.size() >= max_q) {
                queue_limit_hit = true;
            } else {
                const std::string prev_next = GetNextChannelTrackUrlLocked(ch);
                ch.queue.push_back(
                    ChannelQueueEntry{url_or_query, original_input_pawn, clean_utf8_label, clean_pawn_label});
                if (!ch.active_run && (!ch.restore_wanted || ch.restore_retry_blocked)) {
                    const std::int32_t new_pos = static_cast<std::int32_t>(ch.queue.size() - 1);
                    if (!PlayChannelLocked(channel_id, url_or_query, original_input_pawn, ch_start, run_start,
                                           clean_utf8_label)) {
                        ch.queue.pop_back();
                        return false;
                    }
                    ch_start->playlist_pos = new_pos;
                    ch_start->current_from_queue = true;
                    ch_start->consecutive_errors = 0;
                    MarkChannelDirtyLocked(*ch_start);
                } else {
                    MarkChannelDirtyLocked(ch);
                    const std::string new_next = GetNextChannelTrackUrlLocked(ch);
                    if (!new_next.empty() && new_next != prev_next) {
                        ch_preresolve = it->second;
                        preresolve_url = new_next;
                    }
                }
            }
        } else if (!ch.active_run && (!ch.restore_wanted || ch.restore_retry_blocked)) {
            if (!PlayChannelLocked(channel_id, url_or_query, original_input_pawn, ch_start, run_start,
                                   clean_utf8_label)) {
                return false;
            }
            ch_start->playlist_pos = -1;
            ch_start->current_from_queue = true;
            ch_start->consecutive_errors = 0;
            MarkChannelDirtyLocked(*ch_start);
        } else if (ch.queue.size() >= max_q) {
            queue_limit_hit = true;
        } else {
            bool should_preresolve = ch.queue.empty() && ch.active_run != nullptr;
            ch.queue.push_back(
                ChannelQueueEntry{url_or_query, original_input_pawn, clean_utf8_label, clean_pawn_label});
            MarkChannelDirtyLocked(ch);
            if (should_preresolve) {
                ch_preresolve = it->second;
                preresolve_url = url_or_query;
            }
        }
    }
    if (queue_limit_hit) {
        Logger::Instance().Warn(
            "Maximum queue size reached for channel " + std::to_string(channel_id) +
            " (" + std::to_string(max_q) + ")");
        return false;
    }
    if (ch_start && run_start) {
        HttpClient::PollCancellations();
        StartChannelWorker(ch_start, run_start, url_or_query);
    } else if (ch_preresolve && !preresolve_url.empty()) {
        PreResolveNextChannelTrack(ch_preresolve, preresolve_url);
    }
    return true;
}

bool AudioEngine::SkipChannel(ChannelId channel_id) {
    std::shared_ptr<SyncChannel> ch;
    std::shared_ptr<ChannelRunState> run;
    std::shared_ptr<ChannelRunState> stopped_run;
    std::string next_track;
    std::vector<PlayerAudioCommand> cmds;
    bool stopped_only = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channels_.find(channel_id);
        if (it == channels_.end()) return false;
        auto& target = it->second;

        auto stop_active = [&]() {
            CancelPendingChannelCompletionLocked(channel_id);
            stopped_run = target->active_run;
            target->active_run.reset();
            target->current_track = TrackMetadata{};
            target->current_input = ChannelQueueEntry{};
            target->playlist_pos = -1;
            target->current_from_queue = false;
            target->live_title.clear();
            target->live_poll = LiveTitlePollState{};
            target->track_started_at = std::chrono::steady_clock::time_point{};
            target->paused_since = std::chrono::steady_clock::time_point{};
            target->paused_accumulated = std::chrono::steady_clock::duration::zero();
            target->seek_base_offset = std::chrono::seconds::zero();
            target->state.store(StreamState::Stopped);
            DetachChannelRangeListenersLocked(target, cmds);
            for (PlayerId pid : target->listeners) {
                ReconcilePlayerLocked(pid, nullptr, cmds);
            }
            MarkChannelDirtyLocked(*target);
            stopped_only = true;
        };

        if (target->playlist_mode) {
            if (!target->active_run) {
                return false;
            }
            std::int32_t next_idx = -1;
            if (!target->current_from_queue) {
                next_idx = NextPlaylistIndexAfterOutOfListLocked(*target);
            } else if (!target->queue.empty()) {
                const std::int32_t q_size = static_cast<std::int32_t>(target->queue.size());
                const std::int32_t cand = (target->playlist_pos >= -1 && target->playlist_pos < q_size)
                    ? (target->playlist_pos + 1)
                    : 0;
                if (cand < q_size) {
                    next_idx = cand;
                } else if (target->loop_mode == ChannelLoopMode::Queue) {
                    next_idx = 0;
                }
            }
            if (next_idx < 0) {
                stop_active();
            } else {
                const auto& entry = target->queue[static_cast<std::size_t>(next_idx)];
                next_track = entry.utf8_url;
                std::string next_pawn = entry.pawn_url;
                std::string next_utf8_label = entry.utf8_label;
                if (!PlayChannelLocked(channel_id, next_track, next_pawn, ch, run, next_utf8_label)) {
                    return false;
                }
                CopyQueueEntryDescription(ch->current_input, entry);
                ch->playlist_pos = next_idx;
                ch->playlist_resume_pos = -1;
                ch->current_from_queue = true;
                ch->consecutive_errors = 0;
                MarkChannelDirtyLocked(*ch);
            }
        } else if (target->queue.empty()) {
            if (!target->active_run) {
                return false;
            }
            stop_active();
        } else {
            next_track = target->queue.front().utf8_url;
            std::string next_pawn = target->queue.front().pawn_url;
            std::string next_utf8_label = target->queue.front().utf8_label;
            if (!PlayChannelLocked(channel_id, next_track, next_pawn, ch, run, next_utf8_label)) {
                return false;
            }
            CopyQueueEntryDescription(ch->current_input, ch->queue.front());
            ch->playlist_pos = -1;
            ch->current_from_queue = true;
            ch->consecutive_errors = 0;
            ch->queue.pop_front();
            MarkChannelDirtyLocked(*ch);
        }
    }
    if (stopped_only) {
        if (stopped_run) {
            stopped_run->cancel_flag.store(true);
            HttpClient::PollCancellations();
        }
        ApplyPlayerAudioCommands(cmds);
        return true;
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
        if (!ch->playlist_mode) {
            ch->queue.clear();
            ch->playlist_pos = -1;
            ch->current_from_queue = false;
        }
        ch->current_track = TrackMetadata{};
        ch->current_input = ChannelQueueEntry{};
        ch->live_title.clear();
        ch->live_poll = LiveTitlePollState{};
        ch->track_started_at = std::chrono::steady_clock::time_point{};
        ch->paused_since = std::chrono::steady_clock::time_point{};
        ch->paused_accumulated = std::chrono::steady_clock::duration::zero();
        ch->seek_base_offset = std::chrono::seconds::zero();
        ch->consecutive_errors = 0;
        ch->restore_wanted = false;
        ch->restore_retry_blocked = false;
        ch->pause_flag.store(false);
        ch->state.store(StreamState::Stopped);
        DetachChannelRangeListenersLocked(ch, cmds);
        for (PlayerId pid : ch->listeners) {
            ReconcilePlayerLocked(pid, nullptr, cmds);
        }
        MarkChannelDirtyLocked(*ch);
    }

    if (old_run) {
        old_run->cancel_flag.store(true);
        HttpClient::PollCancellations();
    }
    ApplyPlayerAudioCommands(cmds);
    return true;
}

bool AudioEngine::PauseChannel(ChannelId channel_id, bool pause) {
    bool fetch_snapshots = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channels_.find(channel_id);
        if (it == channels_.end()) return false;
        const auto& ch = *it->second;
        StreamState cur = ch.state.load();
        if (pause) {
            if (!ch.active_run || (cur != StreamState::Playing && cur != StreamState::Paused)) {
                return false;
            }
        } else {
            if (!ch.active_run || cur != StreamState::Paused) {
                return false;
            }
            fetch_snapshots = ch.spatial.use_position && ch.auto_listen;
        }
    }

    std::vector<PlayerSnapshot> snapshots;
    if (fetch_snapshots) {
        snapshots = host_.GetConnectedPlayerSnapshots();
    }

    std::vector<PlayerAudioCommand> cmds;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channels_.find(channel_id);
        if (it == channels_.end()) return false;
        auto& ch = *it->second;
        StreamState cur = ch.state.load();
        auto now = std::chrono::steady_clock::now();
        if (pause) {
            if (!ch.active_run || (cur != StreamState::Playing && cur != StreamState::Paused)) {
                return false;
            }
            if (cur == StreamState::Playing && ch.paused_since == std::chrono::steady_clock::time_point{}) {
                ch.paused_since = now;
            }
            ch.pause_flag.store(true);
            ch.state.store(StreamState::Paused);
            if (ch.spatial.use_position) {
                RefreshChannelSpatialLocked(it->second, nullptr, cmds);
            }
            MarkChannelDirtyLocked(ch);
        } else {
            if (!ch.active_run || cur != StreamState::Paused) {
                return false;
            }
            if (ch.paused_since != std::chrono::steady_clock::time_point{}) {
                if (now > ch.paused_since) {
                    ch.paused_accumulated += (now - ch.paused_since);
                }
                ch.paused_since = std::chrono::steady_clock::time_point{};
            }
            ch.pause_flag.store(false);
            ch.state.store(StreamState::Playing);
            std::uint32_t lead_ms = ch.current_track.is_live
                ? 0
                : std::clamp<std::uint32_t>(config_.transcoder_burst_ms, 2000, 15000);
            auto [min_dl, max_dl] = MakeCompletionDeadlines(now, lead_ms);
            for (auto& pending : pending_channel_completions_) {
                if (pending.channel == it->second && pending.run == ch.active_run) {
                    pending.min_deadline = min_dl;
                    pending.max_deadline = max_dl;
                }
            }
            if (ch.spatial.use_position) {
                RefreshChannelSpatialLocked(it->second, fetch_snapshots ? &snapshots : nullptr, cmds);
            }
            MarkChannelDirtyLocked(ch);
        }
    }
    ApplyPlayerAudioCommands(cmds);
    return true;
}

bool AudioEngine::SetChannelVolume(ChannelId channel_id, float volume) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = channels_.find(channel_id);
    if (it == channels_.end()) return false;
    it->second->volume_percent.store(std::clamp(volume, 0.0f, 200.0f));
    MarkChannelDirtyLocked(*it->second);
    return true;
}

bool AudioEngine::JoinChannel(PlayerId player_id, ChannelId channel_id, const std::string& password) {
    if (!host_.IsPlayerConnected(player_id)) return false;

    std::vector<PlayerAudioCommand> cmds;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channels_.find(channel_id);
        if (it == channels_.end()) return false;
        if (!it->second->password_hash.empty() &&
            ComputeStringSha256Hex(it->second->password_salt + password) != it->second->password_hash) {
            return false;
        }

        auto prev_it = player_channels_.find(player_id);
        if (prev_it != player_channels_.end() && prev_it->second != channel_id) {
            auto old_cit = channels_.find(prev_it->second);
            if (old_cit != channels_.end()) {
                old_cit->second->listeners.erase(player_id);
            }
        }

        it->second->range_listeners.erase(player_id);
        {
            auto& g = player_arbitration_[player_id].global_sources;
            GlobalAudioSourceRef ref{true, channel_id};
            g.erase(std::remove(g.begin(), g.end(), ref), g.end());
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
    std::vector<PlayerSnapshot> snapshots;
    bool need_spatial = false;
    std::vector<PlayerAudioCommand> cmds;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = player_channels_.find(player_id);
        if (it == player_channels_.end()) return false;
        for (const auto& [cid, ch] : channels_) {
            (void)cid;
            if (ch && ch->spatial.use_position && ch->auto_listen && ch->state.load() == StreamState::Playing) {
                need_spatial = true;
                break;
            }
        }
    }
    if (need_spatial) {
        snapshots = host_.GetConnectedPlayerSnapshots();
    }

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
        if (cit != channels_.end() && cit->second && cit->second->spatial.use_position && cit->second->auto_listen) {
            RefreshChannelSpatialLocked(cit->second, need_spatial ? &snapshots : nullptr, cmds);
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

bool AudioEngine::SetChannelPassword(ChannelId channel_id, const std::string& password) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = channels_.find(channel_id);
    if (it == channels_.end()) return false;
    if (password.empty()) {
        it->second->password_salt.clear();
        it->second->password_hash.clear();
    } else {
        it->second->password_salt = GenerateEndpointCode() + GenerateEndpointCode();
        it->second->password_hash = ComputeStringSha256Hex(it->second->password_salt + password);
    }
    MarkChannelDirtyLocked(*it->second);
    return true;
}

bool AudioEngine::VerifyChannelPassword(ChannelId channel_id, const std::string& password) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = channels_.find(channel_id);
    if (it == channels_.end()) return false;
    if (it->second->password_hash.empty()) return true;
    return ComputeStringSha256Hex(it->second->password_salt + password) == it->second->password_hash;
}

bool AudioEngine::SetChannelCreator(ChannelId channel_id, PlayerId creator_player_id) {
    bool cleared_to_manual = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channels_.find(channel_id);
        if (it == channels_.end()) return false;
        auto& ch = *it->second;

        if (creator_player_id >= 0 && creator_player_id != PAWN_INVALID_PLAYER_ID &&
            host_.IsPlayerConnected(creator_player_id)) {
            ch.has_creator = true;
            ch.creator_player_id = creator_player_id;
            std::string pname = host_.GetPlayerName(creator_player_id);
            if (!pname.empty()) {
                ch.creator_name = std::move(pname);
            } else if (ch.creator_name.empty()) {
                ch.creator_name = "Player #" + std::to_string(creator_player_id);
            }
            ch.creator_connected = true;
            ch.creator_disconnected_at = std::chrono::steady_clock::time_point{};
            ch.restore_grace_deadline = std::chrono::steady_clock::time_point{};
        } else {
            ch.has_creator = false;
            ch.creator_player_id = INVALID_PLAYER_ID;
            ch.creator_name = "Server";
            ch.creator_connected = false;
            ch.creator_disconnected_at = std::chrono::steady_clock::time_point{};
            ch.restore_grace_deadline = std::chrono::steady_clock::time_point{};
            if (ch.destroy_mode != ChannelDestroyMode::Manual) {
                ch.destroy_mode = ChannelDestroyMode::Manual;
                ch.destroy_timeout_seconds = 0;
                cleared_to_manual = true;
            }
        }
        MarkChannelDirtyLocked(ch);
    }
    if (cleared_to_manual) {
        Logger::Instance().Warn(
            "Creator removed from channel " + std::to_string(channel_id) +
            "; reverting destroy policy to Manual.");
    }
    return true;
}

bool AudioEngine::SetChannelDestroyPolicy(
    ChannelId channel_id,
    ChannelDestroyMode destroy_mode,
    std::int32_t timeout_seconds) {
    bool rejected_no_creator = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channels_.find(channel_id);
        if (it == channels_.end()) return false;
        auto& ch = *it->second;
        if (destroy_mode != ChannelDestroyMode::Manual && !ch.has_creator) {
            rejected_no_creator = true;
        } else {
            ch.destroy_mode = destroy_mode;
            ch.destroy_timeout_seconds = std::max<std::int32_t>(0, timeout_seconds);
            if (destroy_mode == ChannelDestroyMode::Timeout) {
                ch.restore_grace_deadline = std::chrono::steady_clock::time_point{};
                if (ch.creator_connected) {
                    ch.creator_disconnected_at = std::chrono::steady_clock::time_point{};
                } else {
                    ch.creator_disconnected_at = std::chrono::steady_clock::now();
                }
            } else if (destroy_mode == ChannelDestroyMode::OnDisconnect) {
                ch.creator_disconnected_at = std::chrono::steady_clock::time_point{};
                ch.restore_grace_deadline = ch.creator_connected
                    ? std::chrono::steady_clock::time_point{}
                    : (std::chrono::steady_clock::now() +
                       std::chrono::seconds(std::clamp<std::uint32_t>(config_.channel_restore_grace_seconds, 0, 3600)));
            } else {
                ch.creator_disconnected_at = std::chrono::steady_clock::time_point{};
                ch.restore_grace_deadline = std::chrono::steady_clock::time_point{};
            }
            MarkChannelDirtyLocked(ch);
        }
    }
    if (rejected_no_creator) {
        Logger::Instance().Warn(
            "Channel " + std::to_string(channel_id) +
            ": channel has no creator; use SA_SetAudioStreamChannelCreator first.");
        return false;
    }
    return true;
}

bool AudioEngine::ClearChannelQueue(ChannelId channel_id) {
    std::shared_ptr<ChannelRunState> stopped_run;
    std::vector<PlayerAudioCommand> cmds;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channels_.find(channel_id);
        if (it == channels_.end() || !it->second) return false;
        auto& ch = *it->second;
        ch.queue.clear();
        if (ch.playlist_mode && ch.current_from_queue && ch.active_run != nullptr) {
            CancelPendingChannelCompletionLocked(channel_id);
            stopped_run = ch.active_run;
            ch.active_run.reset();
            ch.current_track = TrackMetadata{};
            ch.current_input = ChannelQueueEntry{};
            ch.live_title.clear();
            ch.live_poll = LiveTitlePollState{};
            ch.track_started_at = std::chrono::steady_clock::time_point{};
            ch.paused_since = std::chrono::steady_clock::time_point{};
            ch.paused_accumulated = std::chrono::steady_clock::duration::zero();
            ch.seek_base_offset = std::chrono::seconds::zero();
            ch.pause_flag.store(false);
            ch.state.store(StreamState::Stopped);
            DetachChannelRangeListenersLocked(it->second, cmds);
            for (PlayerId pid : ch.listeners) {
                ReconcilePlayerLocked(pid, nullptr, cmds);
            }
        }
        ch.playlist_pos = -1;
        ch.playlist_resume_pos = -1;
        if (!ch.playlist_mode || stopped_run != nullptr || !ch.active_run) {
            ch.current_from_queue = false;
        }
        MarkChannelDirtyLocked(ch);
    }
    if (stopped_run) {
        stopped_run->cancel_flag.store(true);
        HttpClient::PollCancellations();
    }
    ApplyPlayerAudioCommands(cmds);
    return true;
}

void AudioEngine::DetachChannelRangeListenersLocked(
    const std::shared_ptr<SyncChannel>& channel,
    std::vector<PlayerAudioCommand>& out_cmds) {
    if (!channel || channel->range_listeners.empty()) return;
    const GlobalAudioSourceRef ref{true, channel->id};
    for (PlayerId pid : channel->range_listeners) {
        auto ait = player_arbitration_.find(pid);
        if (ait != player_arbitration_.end()) {
            auto& g = ait->second.global_sources;
            g.erase(std::remove(g.begin(), g.end(), ref), g.end());
            ReconcilePlayerLocked(pid, nullptr, out_cmds);
        }
    }
    channel->range_listeners.clear();
}

void AudioEngine::RefreshChannelSpatialLocked(
    const std::shared_ptr<SyncChannel>& channel,
    const std::vector<PlayerSnapshot>* snapshots,
    std::vector<PlayerAudioCommand>& out_cmds) {
    if (!channel) return;

    std::unordered_map<PlayerId, const PlayerSnapshot*> by_id;
    if (snapshots) {
        by_id.reserve(snapshots->size());
        for (const auto& snap : *snapshots) {
            by_id[snap.id] = &snap;
        }
    }

    for (PlayerId pid : channel->listeners) {
        const PlayerSnapshot* snap_ptr = nullptr;
        if (snapshots) {
            auto bit = by_id.find(pid);
            if (bit != by_id.end()) snap_ptr = bit->second;
        }
        ReconcilePlayerLocked(pid, snap_ptr, out_cmds);
    }

    StreamState st = channel->state.load();
    bool allow_auto = channel->spatial.use_position &&
                      channel->auto_listen &&
                      st == StreamState::Playing &&
                      !channel->pause_flag.load();
    if (!allow_auto) {
        DetachChannelRangeListenersLocked(channel, out_cmds);
        return;
    }

    const GlobalAudioSourceRef ref{true, channel->id};
    if (snapshots) {
        for (auto lit = channel->range_listeners.begin(); lit != channel->range_listeners.end();) {
            if (by_id.find(*lit) == by_id.end() || channel->listeners.count(*lit) > 0) {
                PlayerId old_pid = *lit;
                lit = channel->range_listeners.erase(lit);
                auto ait = player_arbitration_.find(old_pid);
                if (ait != player_arbitration_.end()) {
                    auto& g = ait->second.global_sources;
                    g.erase(std::remove(g.begin(), g.end(), ref), g.end());
                }
                ReconcilePlayerLocked(old_pid, nullptr, out_cmds);
            } else {
                ++lit;
            }
        }

        for (const auto& snap : *snapshots) {
            if (channel->listeners.count(snap.id) > 0) continue;
            bool in_range = MatchesSpatialFilterSnapshot(snap, channel->spatial) &&
                            !IsRangeGroupMutedLocked(snap.id, channel->range_group);
            bool was_range = channel->range_listeners.count(snap.id) > 0;
            if (in_range && !was_range) {
                channel->range_listeners.insert(snap.id);
                auto& g = player_arbitration_[snap.id].global_sources;
                if (std::find(g.begin(), g.end(), ref) == g.end()) {
                    g.push_back(ref);
                }
            } else if (!in_range && was_range) {
                channel->range_listeners.erase(snap.id);
                auto ait = player_arbitration_.find(snap.id);
                if (ait != player_arbitration_.end()) {
                    auto& g = ait->second.global_sources;
                    g.erase(std::remove(g.begin(), g.end(), ref), g.end());
                }
            }
            if (in_range || was_range) {
                ReconcilePlayerLocked(snap.id, &snap, out_cmds);
            }
        }
    } else {
        std::vector<PlayerId> cur_range(channel->range_listeners.begin(), channel->range_listeners.end());
        for (PlayerId pid : cur_range) {
            ReconcilePlayerLocked(pid, nullptr, out_cmds);
        }
    }
}

bool AudioEngine::SetChannelPos(
    ChannelId channel_id,
    float x,
    float y,
    float z,
    float distance,
    std::int32_t world_id,
    std::int32_t interior_id,
    bool auto_listen) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) ||
        !std::isfinite(distance) || distance <= 0.0f) {
        return false;
    }

    std::vector<PlayerSnapshot> snapshots = host_.GetConnectedPlayerSnapshots();
    std::vector<PlayerAudioCommand> cmds;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channels_.find(channel_id);
        if (it == channels_.end() || !it->second) return false;
        auto& ch = *it->second;
        ch.spatial.use_position = true;
        ch.spatial.position = Vector3{x, y, z};
        ch.spatial.distance = distance;
        ch.spatial.world_id = world_id;
        ch.spatial.interior_id = interior_id;
        ch.spatial.attach_player_id = INVALID_PLAYER_ID;
        ch.spatial.attach_vehicle_id = 0;
        ch.auto_listen = auto_listen;
        ++ch.spatial_version;
        RefreshChannelSpatialLocked(it->second, &snapshots, cmds);
        MarkChannelDirtyLocked(ch);
    }
    ApplyPlayerAudioCommands(cmds);
    return true;
}

bool AudioEngine::ClearChannelPos(ChannelId channel_id) {
    std::vector<PlayerAudioCommand> cmds;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channels_.find(channel_id);
        if (it == channels_.end() || !it->second) return false;
        auto& ch = *it->second;
        ch.spatial.use_position = false;
        ch.spatial.position = Vector3{};
        ch.spatial.distance = 50.0f;
        ch.spatial.world_id = -1;
        ch.spatial.interior_id = -1;
        ch.spatial.attach_player_id = INVALID_PLAYER_ID;
        ch.spatial.attach_vehicle_id = 0;
        ch.auto_listen = true;
        ++ch.spatial_version;
        DetachChannelRangeListenersLocked(it->second, cmds);
        for (PlayerId pid : ch.listeners) {
            ReconcilePlayerLocked(pid, nullptr, cmds);
        }
        MarkChannelDirtyLocked(ch);
    }
    ApplyPlayerAudioCommands(cmds);
    return true;
}

bool AudioEngine::AttachChannelToPlayer(ChannelId channel_id, PlayerId target_player_id, float distance) {
    if (!std::isfinite(distance)) {
        return false;
    }
    if (target_player_id < 0 || target_player_id == PAWN_INVALID_PLAYER_ID ||
        !host_.IsPlayerConnected(target_player_id)) {
        return false;
    }
    Vector3 target_pos{};
    if (!host_.GetPlayerPos(target_player_id, target_pos)) {
        return false;
    }
    std::int32_t target_world = host_.GetPlayerVirtualWorld(target_player_id);
    std::int32_t target_interior = host_.GetPlayerInterior(target_player_id);
    std::vector<PlayerSnapshot> snapshots = host_.GetConnectedPlayerSnapshots();

    std::vector<PlayerAudioCommand> cmds;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channels_.find(channel_id);
        if (it == channels_.end() || !it->second) return false;
        auto& ch = *it->second;
        if (distance > 0.0f) {
            ch.spatial.distance = distance;
        } else if (!std::isfinite(ch.spatial.distance) || ch.spatial.distance <= 0.0f) {
            ch.spatial.distance = 50.0f;
        }
        ch.spatial.use_position = true;
        ch.spatial.position = target_pos;
        ch.spatial.world_id = target_world;
        ch.spatial.interior_id = target_interior;
        ch.spatial.attach_player_id = target_player_id;
        ch.spatial.attach_vehicle_id = 0;
        ++ch.spatial_version;
        RefreshChannelSpatialLocked(it->second, &snapshots, cmds);
        MarkChannelDirtyLocked(ch);
    }
    ApplyPlayerAudioCommands(cmds);
    return true;
}

bool AudioEngine::AttachChannelToVehicle(ChannelId channel_id, std::int32_t vehicle_id, float distance) {
    if (!std::isfinite(distance)) {
        return false;
    }
    Vector3 veh_pos{};
    if (vehicle_id <= 0 || !host_.GetVehiclePos(vehicle_id, veh_pos)) {
        return false;
    }
    std::int32_t veh_world = -1;
    std::int32_t veh_interior = -1;
    const bool has_veh_vw = host_.GetVehicleWorldInterior(vehicle_id, veh_world, veh_interior);
    std::vector<PlayerSnapshot> snapshots = host_.GetConnectedPlayerSnapshots();

    std::vector<PlayerAudioCommand> cmds;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channels_.find(channel_id);
        if (it == channels_.end() || !it->second) return false;
        auto& ch = *it->second;
        if (distance > 0.0f) {
            ch.spatial.distance = distance;
        } else if (!std::isfinite(ch.spatial.distance) || ch.spatial.distance <= 0.0f) {
            ch.spatial.distance = 50.0f;
        }
        ch.spatial.use_position = true;
        ch.spatial.position = veh_pos;
        if (has_veh_vw) {
            ch.spatial.world_id = veh_world;
            ch.spatial.interior_id = veh_interior;
        }
        ch.spatial.attach_vehicle_id = vehicle_id;
        ch.spatial.attach_player_id = INVALID_PLAYER_ID;
        ++ch.spatial_version;
        RefreshChannelSpatialLocked(it->second, &snapshots, cmds);
        MarkChannelDirtyLocked(ch);
    }
    ApplyPlayerAudioCommands(cmds);
    return true;
}

bool AudioEngine::RemoveChannelQueueItem(ChannelId channel_id, std::size_t index) {
    std::shared_ptr<SyncChannel> ch_start;
    std::shared_ptr<ChannelRunState> run_start;
    std::string start_url;
    std::shared_ptr<ChannelRunState> stopped_run;
    std::vector<PlayerAudioCommand> cmds;
    std::shared_ptr<SyncChannel> ch_preresolve;
    std::string preresolve_url;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channels_.find(channel_id);
        if (it == channels_.end() || !it->second || index >= it->second->queue.size()) {
            return false;
        }
        auto& ch = *it->second;
        const std::string prev_next = GetNextChannelTrackUrlLocked(ch);
        const std::int32_t idx_i32 = static_cast<std::int32_t>(index);
        ch.queue.erase(ch.queue.begin() + static_cast<std::ptrdiff_t>(index));
        if (idx_i32 <= ch.playlist_resume_pos) {
            --ch.playlist_resume_pos;
        }

        if (ch.playlist_mode) {
            if (idx_i32 < ch.playlist_pos) {
                --ch.playlist_pos;
            } else if (idx_i32 == ch.playlist_pos) {
                if (ch.current_from_queue && ch.active_run != nullptr) {
                    if (index < ch.queue.size()) {
                        const auto& next_entry = ch.queue[index];
                        start_url = next_entry.utf8_url;
                        if (!PlayChannelLocked(channel_id, next_entry.utf8_url, next_entry.pawn_url,
                                               ch_start, run_start, next_entry.utf8_label)) {
                            CancelPendingChannelCompletionLocked(channel_id);
                            stopped_run = ch.active_run;
                            ch.active_run.reset();
                            ch.current_track = TrackMetadata{};
                            ch.current_input = ChannelQueueEntry{};
                            ch.playlist_pos = -1;
                            ch.current_from_queue = false;
                            ch.state.store(StreamState::Stopped);
                            DetachChannelRangeListenersLocked(it->second, cmds);
                            for (PlayerId pid : ch.listeners) {
                                ReconcilePlayerLocked(pid, nullptr, cmds);
                            }
                        } else {
                            CopyQueueEntryDescription(ch_start->current_input, next_entry);
                            ch_start->playlist_pos = idx_i32;
                            ch_start->current_from_queue = true;
                            ch_start->consecutive_errors = 0;
                        }
                    } else {
                        CancelPendingChannelCompletionLocked(channel_id);
                        stopped_run = ch.active_run;
                        ch.active_run.reset();
                        ch.current_track = TrackMetadata{};
                        ch.current_input = ChannelQueueEntry{};
                        ch.playlist_pos = -1;
                        ch.current_from_queue = false;
                        ch.live_title.clear();
                        ch.live_poll = LiveTitlePollState{};
                        ch.track_started_at = std::chrono::steady_clock::time_point{};
                        ch.paused_since = std::chrono::steady_clock::time_point{};
                        ch.paused_accumulated = std::chrono::steady_clock::duration::zero();
                        ch.seek_base_offset = std::chrono::seconds::zero();
                        ch.pause_flag.store(false);
                        ch.state.store(StreamState::Stopped);
                        DetachChannelRangeListenersLocked(it->second, cmds);
                        for (PlayerId pid : ch.listeners) {
                            ReconcilePlayerLocked(pid, nullptr, cmds);
                        }
                    }
                } else {
                    ch.playlist_pos = -1;
                }
            } else {
                const std::string new_next = GetNextChannelTrackUrlLocked(ch);
                if (!new_next.empty() && new_next != prev_next) {
                    ch_preresolve = it->second;
                    preresolve_url = new_next;
                }
            }
        } else {
            const std::string new_next = GetNextChannelTrackUrlLocked(ch);
            if (!new_next.empty() && new_next != prev_next) {
                ch_preresolve = it->second;
                preresolve_url = new_next;
            }
        }
        MarkChannelDirtyLocked(ch);
    }
    if (stopped_run) {
        stopped_run->cancel_flag.store(true);
        HttpClient::PollCancellations();
    }
    ApplyPlayerAudioCommands(cmds);
    if (ch_start && run_start && !start_url.empty()) {
        HttpClient::PollCancellations();
        StartChannelWorker(ch_start, run_start, start_url);
    } else if (ch_preresolve && !preresolve_url.empty()) {
        PreResolveNextChannelTrack(ch_preresolve, preresolve_url);
    }
    return true;
}

bool AudioEngine::InsertChannelQueueItem(
    ChannelId channel_id,
    std::size_t index,
    const std::string& url_or_query,
    const std::string& original_input_pawn,
    const std::string& utf8_label) {
    if (url_or_query.empty()) return false;
    const std::string clean_utf8_label = SanitizeQueueLabelUtf8(utf8_label);
    const std::string clean_pawn_label = clean_utf8_label.empty()
        ? std::string()
        : ConvertTextForPawn(clean_utf8_label, config_.pawn_text_encoding);
    std::shared_ptr<SyncChannel> ch_start;
    std::shared_ptr<ChannelRunState> run_start;
    std::shared_ptr<SyncChannel> ch_preresolve;
    std::string preresolve_url;
    bool queue_limit_hit = false;
    const std::size_t max_q = std::max<std::uint32_t>(1, config_.max_queue_per_channel);
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channels_.find(channel_id);
        if (it == channels_.end() || !it->second) return false;
        auto& ch = *it->second;
        if (index > ch.queue.size()) return false;

        if (ch.playlist_mode) {
            if (ch.queue.size() >= max_q) {
                queue_limit_hit = true;
            } else {
                const std::string prev_next = GetNextChannelTrackUrlLocked(ch);
                ch.queue.insert(
                    ch.queue.begin() + static_cast<std::ptrdiff_t>(index),
                    ChannelQueueEntry{url_or_query, original_input_pawn, clean_utf8_label, clean_pawn_label});
                if (ch.playlist_pos >= 0 && static_cast<std::int32_t>(index) <= ch.playlist_pos) {
                    ++ch.playlist_pos;
                }
                if (ch.playlist_resume_pos >= 0 && static_cast<std::int32_t>(index) <= ch.playlist_resume_pos) {
                    ++ch.playlist_resume_pos;
                }
                if (!ch.active_run && (!ch.restore_wanted || ch.restore_retry_blocked)) {
                    if (!PlayChannelLocked(channel_id, url_or_query, original_input_pawn, ch_start, run_start,
                                           clean_utf8_label)) {
                        ch.queue.erase(ch.queue.begin() + static_cast<std::ptrdiff_t>(index));
                        return false;
                    }
                    ch_start->playlist_pos = static_cast<std::int32_t>(index);
                    ch_start->current_from_queue = true;
                    ch_start->consecutive_errors = 0;
                } else {
                    const std::string new_next = GetNextChannelTrackUrlLocked(ch);
                    if (!new_next.empty() && new_next != prev_next) {
                        ch_preresolve = it->second;
                        preresolve_url = new_next;
                    }
                }
                MarkChannelDirtyLocked(ch);
            }
        } else if (!ch.active_run && (!ch.restore_wanted || ch.restore_retry_blocked)) {
            if (!PlayChannelLocked(channel_id, url_or_query, original_input_pawn, ch_start, run_start,
                                   clean_utf8_label)) {
                return false;
            }
            ch_start->playlist_pos = -1;
            ch_start->current_from_queue = true;
            ch_start->consecutive_errors = 0;
            MarkChannelDirtyLocked(*ch_start);
        } else if (ch.queue.size() >= max_q) {
            queue_limit_hit = true;
        } else {
            ch.queue.insert(
                ch.queue.begin() + static_cast<std::ptrdiff_t>(index),
                ChannelQueueEntry{url_or_query, original_input_pawn, clean_utf8_label, clean_pawn_label});
            MarkChannelDirtyLocked(ch);
            if (index == 0 && ch.active_run != nullptr) {
                ch_preresolve = it->second;
                preresolve_url = url_or_query;
            }
        }
    }
    if (queue_limit_hit) {
        Logger::Instance().Warn(
            "Maximum queue size reached for channel " + std::to_string(channel_id) +
            " (" + std::to_string(max_q) + ")");
        return false;
    }
    if (ch_start && run_start) {
        HttpClient::PollCancellations();
        StartChannelWorker(ch_start, run_start, url_or_query);
    } else if (ch_preresolve && !preresolve_url.empty()) {
        PreResolveNextChannelTrack(ch_preresolve, preresolve_url);
    }
    return true;
}

bool AudioEngine::ShuffleChannelQueue(ChannelId channel_id) {
    std::shared_ptr<SyncChannel> ch_preresolve;
    std::string preresolve_url;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channels_.find(channel_id);
        if (it == channels_.end() || !it->second) return false;
        auto& ch = *it->second;
        if (ch.queue.size() <= 1) {
            return true;
        }
        const std::string prev_next = GetNextChannelTrackUrlLocked(ch);
        if (ch.playlist_mode && ch.playlist_pos >= 0 &&
            static_cast<std::size_t>(ch.playlist_pos) < ch.queue.size()) {
            const std::size_t n = ch.queue.size();
            std::vector<std::size_t> perm(n);
            for (std::size_t k = 0; k < n; ++k) perm[k] = k;
            std::shuffle(perm.begin(), perm.end(), queue_shuffle_rng_);
            std::deque<ChannelQueueEntry> reordered;
            reordered.resize(n);
            const std::size_t old_pos = static_cast<std::size_t>(ch.playlist_pos);
            std::int32_t new_pos = ch.playlist_pos;
            for (std::size_t k = 0; k < n; ++k) {
                reordered[k] = std::move(ch.queue[perm[k]]);
                if (perm[k] == old_pos) {
                    new_pos = static_cast<std::int32_t>(k);
                }
            }
            ch.queue = std::move(reordered);
            ch.playlist_pos = new_pos;
        } else {
            std::shuffle(ch.queue.begin(), ch.queue.end(), queue_shuffle_rng_);
        }
        ch.playlist_resume_pos = -1;
        MarkChannelDirtyLocked(ch);
        const std::string new_next = GetNextChannelTrackUrlLocked(ch);
        if (!new_next.empty() && new_next != prev_next) {
            ch_preresolve = it->second;
            preresolve_url = new_next;
        }
    }
    if (ch_preresolve && !preresolve_url.empty()) {
        PreResolveNextChannelTrack(ch_preresolve, preresolve_url);
    }
    return true;
}

bool AudioEngine::MoveChannelQueueItem(ChannelId channel_id, std::size_t from_index, std::size_t to_index) {
    std::shared_ptr<SyncChannel> ch_preresolve;
    std::string preresolve_url;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channels_.find(channel_id);
        if (it == channels_.end() || !it->second) return false;
        auto& ch = *it->second;
        if (from_index >= ch.queue.size() || to_index >= ch.queue.size()) return false;
        if (from_index == to_index) return true;

        const std::string prev_next = GetNextChannelTrackUrlLocked(ch);
        ChannelQueueEntry item = std::move(ch.queue[from_index]);
        ch.queue.erase(ch.queue.begin() + static_cast<std::ptrdiff_t>(from_index));
        ch.queue.insert(ch.queue.begin() + static_cast<std::ptrdiff_t>(to_index), std::move(item));

        if (ch.playlist_mode && ch.playlist_pos >= 0) {
            const std::size_t pos = static_cast<std::size_t>(ch.playlist_pos);
            if (pos == from_index) {
                ch.playlist_pos = static_cast<std::int32_t>(to_index);
            } else if (from_index < pos && to_index >= pos) {
                --ch.playlist_pos;
            } else if (from_index > pos && to_index <= pos) {
                ++ch.playlist_pos;
            }
        }
        if (ch.playlist_resume_pos >= 0) {
            const std::size_t rpos = static_cast<std::size_t>(ch.playlist_resume_pos);
            if (rpos == from_index) {
                ch.playlist_resume_pos = static_cast<std::int32_t>(to_index);
            } else if (from_index < rpos && to_index >= rpos) {
                --ch.playlist_resume_pos;
            } else if (from_index > rpos && to_index <= rpos) {
                ++ch.playlist_resume_pos;
            }
        }
        MarkChannelDirtyLocked(ch);

        const std::string new_next = GetNextChannelTrackUrlLocked(ch);
        if (!new_next.empty() && new_next != prev_next) {
            ch_preresolve = it->second;
            preresolve_url = new_next;
        }
    }
    if (ch_preresolve && !preresolve_url.empty()) {
        PreResolveNextChannelTrack(ch_preresolve, preresolve_url);
    }
    return true;
}

bool AudioEngine::GetChannelLiveTitle(ChannelId channel_id, std::string& out_title) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = channels_.find(channel_id);
    if (it == channels_.end() || !it->second) return false;
    out_title = ConvertTextForPawn(it->second->live_title, config_.pawn_text_encoding);
    return true;
}

bool AudioEngine::GetStreamLiveTitle(StreamId stream_id, std::string& out_title) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = streams_.find(stream_id);
    if (it == streams_.end() || !it->second) return false;
    out_title = ConvertTextForPawn(it->second->live_title, config_.pawn_text_encoding);
    return true;
}

bool AudioEngine::SetChannelLiveTitle(ChannelId channel_id, const std::string& utf8_title) {
    const std::string clean = CleanLiveTitleUtf8(utf8_title);
    std::string token;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channels_.find(channel_id);
        if (it == channels_.end() || !it->second || !it->second->active_run) return false;
        it->second->live_title = clean;
        token = it->second->token;
    }
    if (!token.empty()) {
        http_server_.UpdateEndpointLiveTitle(token, clean);
    }
    return true;
}

bool AudioEngine::SetStreamLiveTitle(StreamId stream_id, const std::string& utf8_title) {
    const std::string clean = CleanLiveTitleUtf8(utf8_title);
    std::string token;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = streams_.find(stream_id);
        if (it == streams_.end() || !it->second) return false;
        const auto& session = it->second;
        const StreamState st = session->state.load();
        if (session->cancel_flag.load() ||
            st == StreamState::Finished ||
            st == StreamState::Stopped ||
            st == StreamState::Error) {
            return false;
        }
        session->live_title = clean;
        token = session->token;
    }
    if (!token.empty()) {
        http_server_.UpdateEndpointLiveTitle(token, clean);
    }
    return true;
}

std::int32_t AudioEngine::NextPlaylistIndexAfterOutOfListLocked(const SyncChannel& ch) {
    const std::int32_t q_size = static_cast<std::int32_t>(ch.queue.size());
    if (q_size <= 0) {
        return -1;
    }
    const std::int32_t resume = std::clamp<std::int32_t>(ch.playlist_resume_pos, -1, q_size - 1);
    const std::int32_t next = resume + 1;
    if (next < q_size) {
        return next;
    }
    return (ch.loop_mode == ChannelLoopMode::Queue) ? 0 : -1;
}

std::string AudioEngine::GetNextChannelTrackUrlLocked(const SyncChannel& ch) const {
    if (!ch.active_run) {
        return "";
    }
    if (ch.playlist_mode) {
        if (!ch.current_from_queue) {
            if (ch.loop_mode == ChannelLoopMode::Track) {
                return ch.current_input.utf8_url;
            }
            const std::int32_t next_pos = NextPlaylistIndexAfterOutOfListLocked(ch);
            return (next_pos >= 0) ? ch.queue[static_cast<std::size_t>(next_pos)].utf8_url : std::string();
        }
        if (ch.loop_mode == ChannelLoopMode::Track) {
            return ch.current_input.utf8_url;
        }
        if (ch.playlist_pos < 0 || ch.queue.empty()) {
            return "";
        }
        const std::size_t next_idx = static_cast<std::size_t>(ch.playlist_pos) + 1;
        if (next_idx < ch.queue.size()) {
            return ch.queue[next_idx].utf8_url;
        }
        if (ch.loop_mode == ChannelLoopMode::Queue) {
            return ch.queue.front().utf8_url;
        }
        return "";
    }
    if (ch.loop_mode == ChannelLoopMode::Track) {
        return ch.current_input.utf8_url;
    }
    if (!ch.queue.empty()) {
        return ch.queue.front().utf8_url;
    }
    if (ch.loop_mode == ChannelLoopMode::Queue) {
        return ch.current_input.utf8_url;
    }
    return "";
}

bool AudioEngine::SetChannelLoopMode(ChannelId channel_id, ChannelLoopMode mode) {
    const auto raw = static_cast<std::int32_t>(mode);
    if (raw < 0 || raw > 2) return false;
    std::shared_ptr<SyncChannel> ch_preresolve;
    std::string preresolve_url;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channels_.find(channel_id);
        if (it == channels_.end() || !it->second) return false;
        auto& ch = *it->second;
        const std::string prev_next = GetNextChannelTrackUrlLocked(ch);
        ch.loop_mode = mode;
        MarkChannelDirtyLocked(ch);
        const std::string new_next = GetNextChannelTrackUrlLocked(ch);
        if (!new_next.empty() && new_next != prev_next && new_next != ch.current_input.utf8_url) {
            ch_preresolve = it->second;
            preresolve_url = new_next;
        }
    }
    if (ch_preresolve && !preresolve_url.empty()) {
        PreResolveNextChannelTrack(ch_preresolve, preresolve_url);
    }
    return true;
}

ChannelLoopMode AudioEngine::GetChannelLoopMode(ChannelId channel_id) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = channels_.find(channel_id);
    return (it != channels_.end() && it->second) ? it->second->loop_mode : ChannelLoopMode::Off;
}

bool AudioEngine::SetChannelPlaylistMode(ChannelId channel_id, bool enable) {
    std::shared_ptr<SyncChannel> ch_preresolve;
    std::string preresolve_url;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channels_.find(channel_id);
        if (it == channels_.end() || !it->second) return false;
        auto& ch = *it->second;
        if (ch.playlist_mode == enable) {
            return true;
        }
        const std::string prev_next = GetNextChannelTrackUrlLocked(ch);
        ch.playlist_resume_pos = -1;
        if (enable) {
            ch.playlist_mode = true;
            if (ch.active_run && ch.current_from_queue && !ch.current_input.utf8_url.empty()) {
                ch.queue.push_front(ch.current_input);
                ch.playlist_pos = 0;
            } else {
                ch.playlist_pos = -1;
            }
        } else {
            ch.playlist_mode = false;
            if (ch.playlist_pos >= 0 && !ch.queue.empty()) {
                const std::size_t q_size = ch.queue.size();
                const std::size_t pos_sz = static_cast<std::size_t>(ch.playlist_pos);
                const std::size_t erase_count = ch.active_run
                    ? std::min<std::size_t>(pos_sz + 1, q_size)
                    : std::min<std::size_t>(pos_sz, q_size);
                if (erase_count > 0) {
                    ch.queue.erase(ch.queue.begin(), ch.queue.begin() + static_cast<std::ptrdiff_t>(erase_count));
                }
            }
            ch.playlist_pos = -1;
        }
        MarkChannelDirtyLocked(ch);
        const std::string new_next = GetNextChannelTrackUrlLocked(ch);
        if (!new_next.empty() && new_next != prev_next) {
            ch_preresolve = it->second;
            preresolve_url = new_next;
        }
    }
    if (ch_preresolve && !preresolve_url.empty()) {
        PreResolveNextChannelTrack(ch_preresolve, preresolve_url);
    }
    return true;
}

bool AudioEngine::IsChannelPlaylistMode(ChannelId channel_id) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = channels_.find(channel_id);
    return (it != channels_.end() && it->second) ? it->second->playlist_mode : false;
}

std::int32_t AudioEngine::GetChannelQueuePos(ChannelId channel_id) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = channels_.find(channel_id);
    return (it != channels_.end() && it->second && it->second->playlist_mode)
        ? it->second->playlist_pos
        : -1;
}

bool AudioEngine::PlayChannelQueueItem(ChannelId channel_id, std::int32_t index) {
    if (index < 0) return false;
    std::shared_ptr<SyncChannel> ch;
    std::shared_ptr<ChannelRunState> run;
    std::string target_url;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channels_.find(channel_id);
        if (it == channels_.end() || !it->second) return false;
        auto& target = *it->second;
        const std::size_t idx = static_cast<std::size_t>(index);
        if (idx >= target.queue.size()) return false;

        const ChannelQueueEntry entry = target.queue[idx];
        target_url = entry.utf8_url;
        if (!PlayChannelLocked(channel_id, entry.utf8_url, entry.pawn_url, ch, run, entry.utf8_label)) {
            return false;
        }
        CopyQueueEntryDescription(ch->current_input, entry);
        ch->consecutive_errors = 0;
        ch->current_from_queue = true;
        ch->playlist_resume_pos = -1;
        if (ch->playlist_mode) {
            ch->playlist_pos = index;
        } else {
            ch->queue.erase(ch->queue.begin() + static_cast<std::ptrdiff_t>(idx));
            ch->playlist_pos = -1;
        }
        MarkChannelDirtyLocked(*ch);
    }

    HttpClient::PollCancellations();
    StartChannelWorker(ch, run, target_url);
    return true;
}

std::int32_t AudioEngine::GetChannelElapsedSeconds(ChannelId channel_id) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = channels_.find(channel_id);
    if (it == channels_.end() || !it->second) return -1;
    const auto& ch = *it->second;
    StreamState st = ch.state.load();
    const bool seek_repositioning =
        ((st == StreamState::Resolving || st == StreamState::Buffering) &&
         ch.track_started_at != std::chrono::steady_clock::time_point{});
    if ((st != StreamState::Playing && st != StreamState::Paused && !seek_repositioning) ||
        ch.track_started_at == std::chrono::steady_clock::time_point{}) {
        return -1;
    }
    auto end_tp = ((st == StreamState::Paused || ch.pause_flag.load()) &&
                   ch.paused_since != std::chrono::steady_clock::time_point{})
        ? ch.paused_since
        : std::chrono::steady_clock::now();
    auto elapsed = ch.seek_base_offset + (end_tp - ch.track_started_at - ch.paused_accumulated);
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
    return static_cast<std::int32_t>(secs < 0 ? 0 : secs);
}

PlayerId AudioEngine::GetChannelListenerAt(ChannelId channel_id, std::size_t index) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = channels_.find(channel_id);
    if (it == channels_.end() || !it->second || index >= it->second->listeners.size()) {
        return INVALID_PLAYER_ID;
    }
    std::vector<PlayerId> sorted_listeners(it->second->listeners.begin(), it->second->listeners.end());
    std::sort(sorted_listeners.begin(), sorted_listeners.end());
    return sorted_listeners[index];
}

bool AudioEngine::SetChannelName(ChannelId channel_id, const std::string& utf8_name) {
    if (utf8_name.empty()) return false;
    std::string token;
    std::string station_name;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channels_.find(channel_id);
        if (it == channels_.end() || !it->second) return false;
        auto& ch = *it->second;
        ch.name = utf8_name;
        token = ch.token;
        station_name = ch.name;
        MarkChannelDirtyLocked(ch);
    }
    http_server_.UpdateEndpointStationName(token, station_name);
    return true;
}

std::size_t AudioEngine::GetChannelCount() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return channels_.size();
}

void AudioEngine::AddSortedChannelIdLocked(ChannelId id) {
    auto it = std::lower_bound(sorted_channel_ids_.begin(), sorted_channel_ids_.end(), id);
    if (it == sorted_channel_ids_.end() || *it != id) {
        sorted_channel_ids_.insert(it, id);
    }
}

void AudioEngine::RemoveSortedChannelIdLocked(ChannelId id) {
    auto it = std::lower_bound(sorted_channel_ids_.begin(), sorted_channel_ids_.end(), id);
    if (it != sorted_channel_ids_.end() && *it == id) {
        sorted_channel_ids_.erase(it);
    }
}

void AudioEngine::AddStreamSessionLocked(const std::shared_ptr<ActiveStreamSession>& session) {
    if (!session) return;
    streams_[session->id] = session;
    auto it = std::lower_bound(sorted_stream_ids_.begin(), sorted_stream_ids_.end(), session->id);
    if (it == sorted_stream_ids_.end() || *it != session->id) {
        sorted_stream_ids_.insert(it, session->id);
    }
}

std::shared_ptr<ActiveStreamSession> AudioEngine::RemoveStreamSessionLocked(StreamId stream_id) {
    auto sit = streams_.find(stream_id);
    if (sit == streams_.end()) {
        return nullptr;
    }
    auto session = sit->second;
    streams_.erase(sit);
    auto it = std::lower_bound(sorted_stream_ids_.begin(), sorted_stream_ids_.end(), stream_id);
    if (it != sorted_stream_ids_.end() && *it == stream_id) {
        sorted_stream_ids_.erase(it);
    }
    return session;
}

void AudioEngine::ClearStreamSessionsLocked() {
    streams_.clear();
    sorted_stream_ids_.clear();
}

bool AudioEngine::SetPlayerAudioStreamMuted(PlayerId player_id, bool mute, bool include_personal) {
    if (!initialized_.load() || shutting_down_.load() || player_id < 0 || !host_.IsPlayerConnected(player_id)) {
        return false;
    }

    std::vector<PlayerAudioCommand> cmds;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto& arb = player_arbitration_[player_id];
        const bool was_personal_muted = arb.muted && arb.mute_include_personal;
        const bool now_personal_muted = mute && include_personal;
        if (!was_personal_muted && now_personal_muted) {
            arb.personal_muted_since = std::chrono::steady_clock::now();
        }
        if (mute && include_personal && arb.personal_stream_id != INVALID_STREAM_ID) {
            auto sit = streams_.find(arb.personal_stream_id);
            if (sit != streams_.end() && sit->second && !ShouldUseDirectPassthrough(*sit->second) && sit->second->buffer) {
                auto& s = sit->second;
                std::uint64_t earliest = s->buffer->EarliestAvailableOffset();
                std::uint64_t written = s->buffer->TotalWritten();
                std::uint64_t played = http_server_.GetEndpointPlayedOffset(s->token);
                std::uint64_t target = std::max(played, s->resync_offset);
                s->resync_offset = std::clamp(target, earliest, written);
            }
        }
        arb.muted = mute;
        arb.mute_include_personal = mute ? include_personal : false;
        if (was_personal_muted && !now_personal_muted) {
            ExtendPendingPersonalCompletionLocked(player_id, std::chrono::steady_clock::now() - arb.personal_muted_since);
        }
        ReconcilePlayerLocked(player_id, nullptr, cmds);
    }
    ApplyPlayerAudioCommands(cmds);
    return true;
}

bool AudioEngine::IsPlayerAudioStreamMuted(PlayerId player_id) const {
    if (player_id < 0 || !host_.IsPlayerConnected(player_id)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = player_arbitration_.find(player_id);
    return it != player_arbitration_.end() && it->second.muted;
}

bool AudioEngine::IsRangeGroupMutedLocked(PlayerId player_id, std::int32_t group) const {
    if (group <= 0 || group > 31) {
        return false;
    }
    auto it = player_arbitration_.find(player_id);
    return it != player_arbitration_.end() && (it->second.muted_range_groups & (1u << group)) != 0;
}

bool AudioEngine::SetChannelRangeGroup(ChannelId channel_id, std::int32_t group) {
    if (group < 0 || group > 31) {
        return false;
    }
    std::vector<PlayerSnapshot> snapshots = host_.GetConnectedPlayerSnapshots();
    std::vector<PlayerAudioCommand> cmds;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channels_.find(channel_id);
        if (it == channels_.end() || !it->second) return false;
        if (it->second->range_group != group) {
            it->second->range_group = group;
            MarkChannelDirtyLocked(*it->second);
            if (it->second->spatial.use_position && it->second->auto_listen) {
                RefreshChannelSpatialLocked(it->second, &snapshots, cmds);
            }
        }
    }
    ApplyPlayerAudioCommands(cmds);
    return true;
}

std::int32_t AudioEngine::GetChannelRangeGroup(ChannelId channel_id) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = channels_.find(channel_id);
    if (it == channels_.end() || !it->second) return -1;
    return it->second->range_group;
}

bool AudioEngine::SetPlayerRangeGroupMuted(PlayerId player_id, std::int32_t group, bool mute) {
    if (!initialized_.load() || shutting_down_.load() || group <= 0 || group > 31 ||
        player_id < 0 || !host_.IsPlayerConnected(player_id)) {
        return false;
    }
    std::vector<PlayerSnapshot> snapshots = host_.GetConnectedPlayerSnapshots();
    std::vector<PlayerAudioCommand> cmds;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto& arb = player_arbitration_[player_id];
        const std::uint32_t bit = 1u << group;
        const std::uint32_t before = arb.muted_range_groups;
        if (mute) arb.muted_range_groups |= bit;
        else arb.muted_range_groups &= ~bit;
        if (arb.muted_range_groups != before) {
            for (const auto& [cid, ch] : channels_) {
                (void)cid;
                if (ch && ch->range_group == group && ch->spatial.use_position && ch->auto_listen) {
                    RefreshChannelSpatialLocked(ch, &snapshots, cmds);
                }
            }
        }
        ReconcilePlayerLocked(player_id, nullptr, cmds);
    }
    ApplyPlayerAudioCommands(cmds);
    return true;
}

bool AudioEngine::IsPlayerRangeGroupMuted(PlayerId player_id, std::int32_t group) const {
    if (player_id < 0 || !host_.IsPlayerConnected(player_id)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(state_mutex_);
    return IsRangeGroupMutedLocked(player_id, group);
}

bool AudioEngine::GetPlayerAudioStreamSource(
    PlayerId player_id,
    AudioStreamSourceType& out_type,
    std::int32_t& out_source_id) const {
    out_type = AudioStreamSourceType::None;
    out_source_id = -1;
    if (!initialized_.load() || shutting_down_.load() || player_id < 0 || !host_.IsPlayerConnected(player_id)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = player_arbitration_.find(player_id);
    if (it == player_arbitration_.end() || it->second.client_source_kind == AudioSourcePriority::None) {
        return false;
    }
    DecodeClientSource(it->second.client_source_kind, it->second.client_source_id, out_type, out_source_id);
    return out_type != AudioStreamSourceType::None;
}

std::size_t AudioEngine::GetStreamCount() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return sorted_stream_ids_.size();
}

StreamId AudioEngine::GetStreamIdAt(std::size_t index) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (index >= sorted_stream_ids_.size()) return INVALID_STREAM_ID;
    return sorted_stream_ids_[index];
}

PlayerId AudioEngine::GetStreamOwner(StreamId stream_id) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = streams_.find(stream_id);
    if (it == streams_.end() || !it->second || it->second->is_global) {
        return INVALID_PLAYER_ID;
    }
    return it->second->owner_player_id;
}

std::int32_t AudioEngine::GetStreamElapsedSeconds(StreamId stream_id) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = streams_.find(stream_id);
    if (it == streams_.end() || !it->second) return -1;
    const auto& s = *it->second;
    StreamState st = s.state.load();
    if ((st != StreamState::Playing && st != StreamState::Paused) ||
        s.track_started_at == std::chrono::steady_clock::time_point{}) {
        return -1;
    }
    auto end_tp = (st == StreamState::Paused && s.paused_since != std::chrono::steady_clock::time_point{})
        ? s.paused_since
        : std::chrono::steady_clock::now();
    auto elapsed = s.seek_base_offset + (end_tp - s.track_started_at - s.paused_accumulated);
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
    return static_cast<std::int32_t>(secs < 0 ? 0 : secs);
}

bool AudioEngine::GetStreamUrl(StreamId stream_id, std::string& out_url) const {
    out_url.clear();
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = streams_.find(stream_id);
    if (it == streams_.end() || !it->second) return false;
    const auto& s = *it->second;
    out_url = s.original_input_pawn.empty()
        ? ConvertTextForPawn(s.original_input, config_.pawn_text_encoding)
        : s.original_input_pawn;
    return true;
}

AudioEngineStats AudioEngine::GetStats() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    AudioEngineStats stats;
    stats.active_streams = static_cast<std::int32_t>(streams_.size());
    stats.active_channels = static_cast<std::int32_t>(channels_.size());
    stats.active_transcoders = static_cast<std::int32_t>(transcode_scheduler_.LiveCount());
    std::int32_t listeners = 0;
    for (const auto& [pid, arb] : player_arbitration_) {
        (void)pid;
        if (arb.client_source_kind != AudioSourcePriority::None) {
            ++listeners;
        }
    }
    stats.active_listeners = listeners;
    return stats;
}

bool AudioEngine::SeekAudioStream(StreamId stream_id, std::int32_t seconds) {
    if (!initialized_.load() || shutting_down_.load() || seconds < 0) {
        return false;
    }

    std::shared_ptr<ActiveStreamSession> session;
    std::shared_ptr<StreamRunState> new_run;
    std::shared_ptr<AudioStreamBuffer> old_buf;
    std::shared_ptr<AudioStreamBuffer> new_buf;
    TrackMetadata local_meta;
    std::string tok;
    std::vector<PlayerAudioCommand> cmds;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = streams_.find(stream_id);
        if (it == streams_.end() || !it->second) return false;
        session = it->second;
        if (session->cancel_flag.load() || !session->ready.load() || session->is_passthrough.load()) {
            return false;
        }
        StreamState cur = session->state.load();
        if (cur != StreamState::Playing && cur != StreamState::Paused) {
            return false;
        }
        if (session->metadata.is_live || session->metadata.duration_seconds <= 0 ||
            seconds >= session->metadata.duration_seconds) {
            return false;
        }

        // Cancel the previous transcode run so Seek replaces it in place without consuming an extra concurrency slot.
        if (session->active_run) {
            session->active_run->cancel_flag.store(true);
        }

        CancelPendingCompletionLocked(stream_id);
        old_buf = session->buffer;
        new_buf = std::make_shared<AudioStreamBuffer>(CalculateRingBufferBytes());
        new_run = std::make_shared<StreamRunState>();
        new_run->start_offset_ms = static_cast<std::uint64_t>(seconds) * 1000ULL;

        session->buffer = new_buf;
        session->active_run = new_run;
        session->paused_offset = 0;
        session->resync_offset = 0;
        ++session->options_version;

        auto now = std::chrono::steady_clock::now();
        session->seek_base_offset = std::chrono::seconds(seconds);
        session->track_started_at = now;
        session->paused_accumulated = std::chrono::steady_clock::duration::zero();
        session->paused_since = session->pause_flag.load() ? now : std::chrono::steady_clock::time_point{};

        local_meta = session->metadata;
        tok = session->token;

        if (!session->pause_flag.load()) {
            if (session->is_global) {
                std::vector<PlayerId> listeners(session->active_listeners.begin(), session->active_listeners.end());
                for (PlayerId pid : listeners) {
                    ReconcilePlayerLocked(pid, nullptr, cmds);
                }
            } else {
                ReconcilePlayerLocked(session->owner_player_id, nullptr, cmds);
            }
        }
    }

    if (old_buf) {
        old_buf->Close();
    }
    http_server_.UpdateEndpointMetadata(tok, local_meta.title, local_meta.artist, new_buf);
    HttpClient::PollCancellations();
    ApplyPlayerAudioCommands(cmds);
    StartStreamTranscoderWorker(session, new_run, local_meta, new_buf, new_run->start_offset_ms);
    return true;
}

bool AudioEngine::QueueChannelPlaylist(ChannelId channel_id, const std::string& url, std::int32_t max_items) {
    const bool is_library = StreamResolver::IsLibraryUrl(url);
    if (!initialized_.load() || shutting_down_.load() || url.empty() ||
        (!is_library && (!StreamResolver::LooksLikeUrl(url) || !binary_manager_.IsReady()))) {
        return false;
    }

    std::shared_ptr<SyncChannel> ch;
    std::shared_ptr<std::atomic<bool>> pl_cancel;
    std::uint32_t clamped_max = 1;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channels_.find(channel_id);
        if (it == channels_.end() || !it->second || it->second->playlist_import_in_flight) {
            return false;
        }
        ch = it->second;
        ch->playlist_import_in_flight = true;
        pl_cancel = std::make_shared<std::atomic<bool>>(false);
        ch->playlist_cancel = pl_cancel;
        const std::uint32_t max_q = std::max<std::uint32_t>(1, config_.max_queue_per_channel);
        clamped_max = static_cast<std::uint32_t>(
            std::clamp<std::int32_t>(max_items, 1, static_cast<std::int32_t>(max_q)));
    }

    auto alive = alive_token_;
    if (!extraction_pool_.Post([this, channel_id, ch, pl_cancel, url, clamped_max, alive]() {
        if (shutting_down_.load() || !alive->load() || pl_cancel->load()) {
            return;
        }

        PlaylistExtractionResult pl = resolver_.ExtractPlaylist(url, clamped_max, pl_cancel.get());
        if (shutting_down_.load() || !alive->load() || pl_cancel->load()) {
            return;
        }

        MainThreadDispatcher::Instance().Post([this, channel_id, pl_cancel, pl = std::move(pl), alive]() {
            if (shutting_down_.load() || !alive->load() || pl_cancel->load()) {
                return;
            }

            std::shared_ptr<SyncChannel> ch_start;
            std::shared_ptr<ChannelRunState> run_start;
            std::string first_started_url;
            std::shared_ptr<SyncChannel> ch_preresolve;
            std::string preresolve_url;
            std::int32_t added = 0;
            bool queue_full = false;
            bool start_limit_reached = false;
            bool channel_missing = false;
            const std::size_t max_q = std::max<std::uint32_t>(1, config_.max_queue_per_channel);

            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                auto it = channels_.find(channel_id);
                if (it == channels_.end() || !it->second) {
                    channel_missing = true;
                } else {
                    auto& target = *it->second;
                    target.playlist_import_in_flight = false;
                    if (pl.success && !pl.urls.empty()) {
                        if (target.playlist_mode) {
                            const bool want_auto_start =
                                (!target.active_run && (!target.restore_wanted || target.restore_retry_blocked));
                            const std::string prev_next = GetNextChannelTrackUrlLocked(target);
                            const std::size_t first_inserted_idx = target.queue.size();
                            for (std::size_t idx = 0; idx < pl.urls.size(); ++idx) {
                                if (target.queue.size() >= max_q) {
                                    queue_full = true;
                                    break;
                                }
                                const std::string& item_url = pl.urls[idx];
                                const std::string pawn_url = ConvertTextForPawn(item_url, config_.pawn_text_encoding);
                                const std::string raw_title = (idx < pl.titles.size()) ? pl.titles[idx] : std::string();
                                const std::string item_utf8_label = SanitizeQueueLabelUtf8(raw_title);
                                const std::string item_pawn_label = item_utf8_label.empty()
                                    ? std::string()
                                    : ConvertTextForPawn(item_utf8_label, config_.pawn_text_encoding);
                                target.queue.push_back(
                                    ChannelQueueEntry{item_url, pawn_url, item_utf8_label, item_pawn_label});
                                ++added;
                            }
                            if (want_auto_start && added > 0) {
                                const auto& first_entry = target.queue[first_inserted_idx];
                                if (!PlayChannelLocked(channel_id, first_entry.utf8_url, first_entry.pawn_url,
                                                       ch_start, run_start, first_entry.utf8_label)) {
                                    start_limit_reached = true;
                                } else {
                                    target.playlist_pos = static_cast<std::int32_t>(first_inserted_idx);
                                    target.current_from_queue = true;
                                    ch_start->consecutive_errors = 0;
                                    first_started_url = first_entry.utf8_url;
                                }
                            }
                            if (added > 0) {
                                MarkChannelDirtyLocked(target);
                                if (!want_auto_start) {
                                    const std::string new_next = GetNextChannelTrackUrlLocked(target);
                                    if (!new_next.empty() && new_next != prev_next) {
                                        ch_preresolve = it->second;
                                        preresolve_url = new_next;
                                    }
                                }
                            }
                        } else {
                            for (std::size_t idx = 0; idx < pl.urls.size(); ++idx) {
                                const std::string& item_url = pl.urls[idx];
                                const std::string pawn_url = ConvertTextForPawn(item_url, config_.pawn_text_encoding);
                                const std::string raw_title = (idx < pl.titles.size()) ? pl.titles[idx] : std::string();
                                const std::string item_utf8_label = SanitizeQueueLabelUtf8(raw_title);
                                const std::string item_pawn_label = item_utf8_label.empty()
                                    ? std::string()
                                    : ConvertTextForPawn(item_utf8_label, config_.pawn_text_encoding);
                                if (!target.active_run && (!target.restore_wanted || target.restore_retry_blocked)) {
                                    if (!PlayChannelLocked(channel_id, item_url, pawn_url, ch_start, run_start,
                                                           item_utf8_label)) {
                                        start_limit_reached = true;
                                        break;
                                    }
                                    ch_start->playlist_pos = -1;
                                    ch_start->current_from_queue = true;
                                    ch_start->consecutive_errors = 0;
                                    first_started_url = item_url;
                                    ++added;
                                } else if (target.queue.size() < max_q) {
                                    const bool should_pre = target.queue.empty() && target.active_run != nullptr &&
                                                            preresolve_url.empty();
                                    target.queue.push_back(
                                        ChannelQueueEntry{item_url, pawn_url, item_utf8_label, item_pawn_label});
                                    if (should_pre) {
                                        ch_preresolve = it->second;
                                        preresolve_url = item_url;
                                    }
                                    ++added;
                                } else {
                                    queue_full = true;
                                    break;
                                }
                            }
                            if (added > 0) {
                                MarkChannelDirtyLocked(target);
                            }
                        }
                    }
                }
            }

            if (channel_missing) {
                return;
            }

            if (ch_start && run_start && !first_started_url.empty()) {
                HttpClient::PollCancellations();
                StartChannelWorker(ch_start, run_start, first_started_url);
            } else if (ch_preresolve && !preresolve_url.empty()) {
                PreResolveNextChannelTrack(ch_preresolve, preresolve_url);
            }

            ErrorCode cb_err = ErrorCode::None;
            std::string cb_msg;
            if (!pl.success || pl.urls.empty()) {
                cb_err = (pl.error == ErrorCode::None) ? ErrorCode::ExtractionFailed : pl.error;
                cb_msg = ConvertTextForPawn(
                    pl.error_message.empty() ? std::string("Playlist extraction failed") : pl.error_message,
                    config_.pawn_text_encoding);
            } else if (start_limit_reached) {
                cb_err = ErrorCode::InternalError;
                cb_msg = "Maximum concurrent transcoders reached";
            } else if (added == 0 && queue_full) {
                cb_err = ErrorCode::InternalError;
                cb_msg = "Channel queue is full";
            }

            host_.DispatchPawnCallback("OnAudioStreamChannelPlaylistQueued", {
                channel_id,
                added,
                static_cast<std::int32_t>(cb_err),
                cb_msg
            });
        });
    })) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channels_.find(channel_id);
        if (it != channels_.end() && it->second) {
            it->second->playlist_import_in_flight = false;
        }
        return false;
    }

    return true;
}

bool AudioEngine::SeekChannelAudioStream(ChannelId channel_id, std::int32_t seconds) {
    if (!initialized_.load() || shutting_down_.load() || seconds < 0) {
        return false;
    }

    std::shared_ptr<SyncChannel> ch;
    std::shared_ptr<ChannelRunState> new_run;
    std::string track_url;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channels_.find(channel_id);
        if (it == channels_.end() || !it->second) return false;
        ch = it->second;
        StreamState cur = ch->state.load();
        if (!ch->active_run || (cur != StreamState::Playing && cur != StreamState::Paused)) {
            return false;
        }
        if (ch->current_track.is_live || ch->current_track.duration_seconds <= 0 ||
            seconds >= ch->current_track.duration_seconds ||
            ch->current_input.utf8_url.empty() || !ch->buffer) {
            return false;
        }

        // Cancel current run so Seek replaces it in place without consuming an extra concurrency slot.
        ch->active_run->cancel_flag.store(true);

        CancelPendingChannelCompletionLocked(channel_id);
        const bool was_paused = ch->pause_flag.load();
        ch->next_generation = ch->buffer->PrepareNextTrack();
        new_run = std::make_shared<ChannelRunState>();
        new_run->generation = ch->next_generation;
        new_run->start_offset_seconds = seconds;
        ch->active_run = new_run;

        auto now = std::chrono::steady_clock::now();
        ch->seek_base_offset = std::chrono::seconds(seconds);
        ch->track_started_at = now;
        ch->paused_accumulated = std::chrono::steady_clock::duration::zero();
        ch->paused_since = was_paused ? now : std::chrono::steady_clock::time_point{};
        ch->pause_flag.store(was_paused);
        ch->state.store(was_paused ? StreamState::Paused : StreamState::Playing);
        track_url = ch->current_input.utf8_url;
        MarkChannelDirtyLocked(*ch);
    }

    HttpClient::PollCancellations();
    StartChannelWorker(ch, new_run, track_url);
    return true;
}

ChannelId AudioEngine::GetChannelIdAt(std::size_t index) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (index >= sorted_channel_ids_.size()) return INVALID_CHANNEL_ID;
    return sorted_channel_ids_[index];
}

bool AudioEngine::GetChannelInfo(ChannelId channel_id, ChannelInfo& out_info) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = channels_.find(channel_id);
    if (it == channels_.end() || !it->second) return false;
    const auto& ch = *it->second;
    out_info.id = ch.id;
    out_info.name = ConvertTextForPawn(ch.name, config_.pawn_text_encoding);
    out_info.creator_player_id = ch.creator_player_id;
    out_info.creator_name = ch.creator_name;
    out_info.creator_online = ch.creator_connected;
    out_info.has_password = !ch.password_hash.empty();
    out_info.listener_count = ch.listeners.size();
    out_info.queue_count = ch.queue.size();
    out_info.state = ch.state.load();
    out_info.destroy_mode = ch.destroy_mode;
    out_info.destroy_timeout_seconds = ch.destroy_timeout_seconds;
    out_info.destroy_remaining_seconds = ComputeChannelRemainingTimeoutSecondsLocked(ch, std::chrono::steady_clock::now());
    out_info.current_track_title = ConvertTextForPawn(ch.current_track.title, config_.pawn_text_encoding);
    out_info.current_track_artist = ConvertTextForPawn(ch.current_track.artist, config_.pawn_text_encoding);
    out_info.current_track_duration = ch.current_track.duration_seconds;
    return true;
}

bool AudioEngine::GetChannelQueueItem(ChannelId channel_id, std::size_t index, std::string& out_item) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = channels_.find(channel_id);
    if (it == channels_.end() || !it->second || index >= it->second->queue.size()) return false;
    const auto& entry = it->second->queue[index];
    out_item = entry.pawn_url.empty()
        ? ConvertTextForPawn(entry.utf8_url, config_.pawn_text_encoding)
        : entry.pawn_url;
    return true;
}

bool AudioEngine::GetChannelQueueItemLabel(ChannelId channel_id, std::size_t index, std::string& out_label) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = channels_.find(channel_id);
    if (it == channels_.end() || !it->second || index >= it->second->queue.size()) return false;
    const auto& entry = it->second->queue[index];
    if (entry.utf8_label.empty() && entry.pawn_label.empty()) {
        out_label.clear();
        return true;
    }
    out_label = entry.pawn_label.empty()
        ? ConvertTextForPawn(entry.utf8_label, config_.pawn_text_encoding)
        : entry.pawn_label;
    return true;
}

bool AudioEngine::SetChannelDescription(ChannelId channel_id, const std::string& utf8_description) {
    const std::string clean = SanitizeDescriptionUtf8(utf8_description);
    const std::string pawn = clean.empty() ? std::string() : ConvertTextForPawn(clean, config_.pawn_text_encoding);
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = channels_.find(channel_id);
    if (it == channels_.end() || !it->second) return false;
    auto& ch = *it->second;
    if (ch.utf8_description == clean && ch.pawn_description == pawn) return true;
    ch.utf8_description = clean;
    ch.pawn_description = pawn;
    MarkChannelDirtyLocked(ch);
    return true;
}

bool AudioEngine::GetChannelDescription(ChannelId channel_id, std::string& out_description) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = channels_.find(channel_id);
    if (it == channels_.end() || !it->second) return false;
    out_description = DescriptionForPawn(
        it->second->utf8_description, it->second->pawn_description, config_.pawn_text_encoding);
    return true;
}

bool AudioEngine::SetChannelQueueItemDescription(
    ChannelId channel_id,
    std::size_t index,
    const std::string& utf8_description) {
    const std::string clean = SanitizeDescriptionUtf8(utf8_description);
    const std::string pawn = clean.empty() ? std::string() : ConvertTextForPawn(clean, config_.pawn_text_encoding);
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = channels_.find(channel_id);
    if (it == channels_.end() || !it->second || index >= it->second->queue.size()) return false;
    auto& ch = *it->second;
    auto& entry = ch.queue[index];
    if (entry.utf8_description == clean && entry.pawn_description == pawn) return true;
    entry.utf8_description = clean;
    entry.pawn_description = pawn;
    if (ch.playlist_mode && ch.current_from_queue && ch.playlist_pos == static_cast<std::int32_t>(index)) {
        CopyQueueEntryDescription(ch.current_input, entry);
    }
    MarkChannelDirtyLocked(ch);
    return true;
}

bool AudioEngine::GetChannelQueueItemDescription(
    ChannelId channel_id,
    std::size_t index,
    std::string& out_description) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = channels_.find(channel_id);
    if (it == channels_.end() || !it->second || index >= it->second->queue.size()) return false;
    const auto& entry = it->second->queue[index];
    out_description = DescriptionForPawn(entry.utf8_description, entry.pawn_description, config_.pawn_text_encoding);
    return true;
}

bool AudioEngine::GetChannelTrackDescription(ChannelId channel_id, std::string& out_description) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = channels_.find(channel_id);
    if (it == channels_.end() || !it->second) return false;
    const auto& ch = *it->second;
    out_description = ch.active_run
        ? DescriptionForPawn(ch.current_input.utf8_description, ch.current_input.pawn_description,
                             config_.pawn_text_encoding)
        : std::string();
    return true;
}

std::int32_t AudioEngine::FindChannelQueueItem(
    ChannelId channel_id,
    const std::string& pawn_item,
    std::int32_t start_index) const {
    if (pawn_item.empty()) return -1;
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = channels_.find(channel_id);
    if (it == channels_.end() || !it->second) return -1;
    const auto& queue = it->second->queue;
    const std::size_t start = (start_index > 0) ? static_cast<std::size_t>(start_index) : 0u;
    for (std::size_t i = start; i < queue.size(); ++i) {
        const auto& entry = queue[i];
        const bool match = entry.pawn_url.empty()
            ? (ConvertTextForPawn(entry.utf8_url, config_.pawn_text_encoding) == pawn_item)
            : (entry.pawn_url == pawn_item);
        if (match) return static_cast<std::int32_t>(i);
    }
    return -1;
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

std::string AudioEngine::GetChannelsDir() const {
    std::filesystem::path base = config_.base_dir.empty()
        ? std::filesystem::path("scriptfiles/SampAudioStream")
        : std::filesystem::path(config_.base_dir);
    return (base / "channels").string();
}

AudioEngine::ChannelPersistSnapshot AudioEngine::CaptureChannelPersistSnapshotLocked(const SyncChannel& ch) {
    ChannelPersistSnapshot snap;
    snap.id = ch.id;
    snap.name = ch.name;
    snap.utf8_description = ch.utf8_description;
    snap.pawn_description = ch.pawn_description;
    snap.password_salt = ch.password_salt;
    snap.password_hash = ch.password_hash;
    snap.has_creator = ch.has_creator;
    snap.creator_name = ch.creator_name;
    snap.destroy_mode = ch.destroy_mode;
    snap.destroy_timeout_seconds = ch.destroy_timeout_seconds;
    snap.volume_percent = ch.volume_percent.load();
    snap.paused = ch.pause_flag.load();
    snap.loop_mode = ch.loop_mode;
    snap.playlist_mode = ch.playlist_mode;
    snap.playlist_pos = ch.playlist_pos;
    snap.current_from_queue = ch.current_from_queue;
    snap.current_input = ch.current_input;
    StreamState st = ch.state.load();
    snap.was_playing = ch.restore_wanted ||
                       (ch.active_run != nullptr &&
                        (st == StreamState::Resolving || st == StreamState::Buffering ||
                         st == StreamState::Playing || st == StreamState::Paused));
    snap.queue.assign(ch.queue.begin(), ch.queue.end());
    snap.spatial = ch.spatial;
    snap.spatial.attach_player_id = INVALID_PLAYER_ID; // Player/vehicle IDs do not survive server restarts
    snap.spatial.attach_vehicle_id = 0;
    snap.auto_listen = ch.auto_listen;
    snap.range_group = ch.range_group;
    snap.persistent = ch.persistent;
    snap.revision = ch.persist_revision;
    snap.cancelled = ch.persist_cancelled;
    return snap;
}

void AudioEngine::MarkChannelDirtyLocked(SyncChannel& ch) {
    if (!ch.persistent) return;
    if (ch.persist_cancelled && ch.persist_cancelled->load()) return;
    ++ch.persist_revision;
    ch.persist_dirty = true;
    auto now = std::chrono::steady_clock::now();
    if (ch.last_persist_flush == std::chrono::steady_clock::time_point{} ||
        (now - ch.last_persist_flush) >= std::chrono::seconds(1)) {
        ch.earliest_persist_due = now;
    } else {
        ch.earliest_persist_due = ch.last_persist_flush + std::chrono::seconds(1);
    }
    has_dirty_channels_ = true;
}

bool AudioEngine::WriteChannelPersistFile(const ChannelPersistSnapshot& snap, std::string& out_error) const {
    const std::filesystem::path dir(GetChannelsDir());
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        out_error = "Failed to create channels directory '" + dir.string() + "': " + DescribeSystemError(ec);
        return false;
    }

    nlohmann::json q_arr = nlohmann::json::array();
    for (const auto& entry : snap.queue) {
        q_arr.push_back(nlohmann::json{
            {"utf8_url", entry.utf8_url},
            {"pawn_url_hex", BytesToHex(entry.pawn_url)},
            {"label_hex", BytesToHex(entry.utf8_label)},
            {"pawn_label_hex", BytesToHex(entry.pawn_label)},
            {"description_hex", BytesToHex(entry.utf8_description)},
            {"pawn_description_hex", BytesToHex(entry.pawn_description)}
        });
    }

    nlohmann::json doc = {
        {"format_version", 1},
        {"persistent", snap.persistent},
        {"id", snap.id},
        {"name", snap.name},
        {"description_hex", BytesToHex(snap.utf8_description)},
        {"pawn_description_hex", BytesToHex(snap.pawn_description)},
        {"password_salt", snap.password_salt},
        {"password_hash", snap.password_hash},
        {"has_creator", snap.has_creator},
        {"creator_name_hex", BytesToHex(snap.creator_name)},
        {"destroy_mode", static_cast<std::int32_t>(snap.destroy_mode)},
        {"destroy_timeout_seconds", snap.destroy_timeout_seconds},
        {"volume", snap.volume_percent},
        {"paused", snap.paused},
        {"loop_mode", static_cast<std::int32_t>(snap.loop_mode)},
        {"playlist_mode", snap.playlist_mode},
        {"playlist_pos", snap.playlist_pos},
        {"current_from_queue", snap.current_from_queue},
        {"current_input", {
            {"utf8_url", snap.current_input.utf8_url},
            {"pawn_url_hex", BytesToHex(snap.current_input.pawn_url)},
            {"label_hex", BytesToHex(snap.current_input.utf8_label)},
            {"pawn_label_hex", BytesToHex(snap.current_input.pawn_label)},
            {"description_hex", BytesToHex(snap.current_input.utf8_description)},
            {"pawn_description_hex", BytesToHex(snap.current_input.pawn_description)}
        }},
        {"was_playing", snap.was_playing},
        {"queue", std::move(q_arr)},
        {"spatial", {
            {"use_position", snap.spatial.use_position},
            {"x", snap.spatial.position.x},
            {"y", snap.spatial.position.y},
            {"z", snap.spatial.position.z},
            {"distance", snap.spatial.distance},
            {"world_id", snap.spatial.world_id},
            {"interior_id", snap.spatial.interior_id},
            {"auto_listen", snap.auto_listen},
            {"range_group", snap.range_group}
        }},
        {"saved_at", FormatUtcIso8601Now()}
    };

    const std::filesystem::path final_path = dir / ("channel_" + std::to_string(snap.id) + ".json");
    const std::filesystem::path tmp_path = dir / ("channel_" + std::to_string(snap.id) + ".json.tmp");
    {
        std::ofstream ofs(tmp_path, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) {
            out_error = "Unable to open temp file '" + tmp_path.string() + "'";
            return false;
        }
        const std::string payload = doc.dump(2);
        ofs.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        ofs.put('\n');
        ofs.flush();
        if (!ofs.good()) {
            out_error = "Write failed for temp file '" + tmp_path.string() + "'";
            return false;
        }
    }

    if (!AtomicReplaceFile(tmp_path, final_path, out_error)) {
        std::filesystem::remove(tmp_path, ec);
        return false;
    }
    return true;
}

void AudioEngine::DeleteChannelPersistFile(ChannelId id) {
    const std::filesystem::path dir(GetChannelsDir());
    const std::filesystem::path final_path = dir / ("channel_" + std::to_string(id) + ".json");
    const std::filesystem::path tmp_path = dir / ("channel_" + std::to_string(id) + ".json.tmp");
    std::lock_guard<std::mutex> io_lk(persist_io_mutex_);
    std::error_code ec;
    std::filesystem::remove(final_path, ec);
    ec.clear();
    std::filesystem::remove(tmp_path, ec);
}

void AudioEngine::FlushDirtyChannels(bool sync_all, bool bypass_debounce) {
    std::vector<ChannelPersistSnapshot> to_write;
    auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        bool any_remaining_dirty = false;
        for (const auto& [cid, ch] : channels_) {
            (void)cid;
            if (!ch || !ch->persistent) continue;
            bool needs_flush = ch->persist_dirty || (sync_all && ch->persist_write_in_flight);
            if (!needs_flush) continue;
            if (ch->persist_cancelled && ch->persist_cancelled->load()) {
                ch->persist_dirty = false;
                ch->persist_write_in_flight = false;
                continue;
            }
            if (!sync_all && ch->persist_write_in_flight) {
                any_remaining_dirty = true;
                continue;
            }
            if (!bypass_debounce && now < ch->earliest_persist_due) {
                any_remaining_dirty = true;
                continue;
            }
            ChannelPersistSnapshot snap = CaptureChannelPersistSnapshotLocked(*ch);
            ch->persist_dirty = false;
            ch->last_persist_flush = now;
            if (!sync_all) {
                ch->persist_write_in_flight = true;
            } else {
                ch->persist_write_in_flight = false;
            }
            to_write.push_back(std::move(snap));
        }
        has_dirty_channels_ = any_remaining_dirty;
    }

    if (to_write.empty()) return;

    if (sync_all) {
        std::lock_guard<std::mutex> io_lk(persist_io_mutex_);
        for (const auto& snap : to_write) {
            if (snap.cancelled && snap.cancelled->load()) continue;
            std::string write_err;
            if (!WriteChannelPersistFile(snap, write_err)) {
                Logger::Instance().Warn(
                    "Failed to persist channel " + std::to_string(snap.id) + ": " + write_err);
            }
        }
        return;
    }

    auto alive = alive_token_;
    for (auto& snap : to_write) {
        bool posted = control_pool_.Post([this, alive, snap = std::move(snap)]() {
            if (!alive->load() || shutting_down_.load()) {
                return;
            }
            bool ok = true;
            std::string write_err;
            {
                std::lock_guard<std::mutex> io_lk(persist_io_mutex_);
                if (!alive->load() || shutting_down_.load() ||
                    (snap.cancelled && snap.cancelled->load())) {
                    return;
                }
                ok = WriteChannelPersistFile(snap, write_err);
            }

            bool log_warn = false;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                auto it = channels_.find(snap.id);
                if (it != channels_.end() && it->second) {
                    it->second->persist_write_in_flight = false;
                    if (it->second->persist_dirty) {
                        has_dirty_channels_ = true;
                    }
                    if (ok) {
                        it->second->persist_disk_error_logged = false;
                    } else if (!it->second->persist_disk_error_logged) {
                        it->second->persist_disk_error_logged = true;
                        log_warn = true;
                    }
                }
            }
            if (log_warn) {
                Logger::Instance().Warn(
                    "Failed to persist channel " + std::to_string(snap.id) + ": " + write_err);
            }
        });
        if (!posted) {
            std::lock_guard<std::mutex> lock(state_mutex_);
            auto it = channels_.find(snap.id);
            if (it != channels_.end() && it->second) {
                it->second->persist_write_in_flight = false;
                it->second->persist_dirty = true;
                has_dirty_channels_ = true;
            }
        }
    }
}

void AudioEngine::LoadPersistedChannelsOnInit() {
    const std::filesystem::path dir(GetChannelsDir());
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        Logger::Instance().Warn(
            "Unable to create channel persistence directory '" + dir.string() + "': " + DescribeSystemError(ec));
        return;
    }

    auto quarantine_bad_file = [](const std::filesystem::path& p) {
        std::filesystem::path bad = p;
        bad += ".bad";
        if (std::filesystem::exists(bad)) {
            bad = p.parent_path() / (p.filename().string() + "." +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".bad");
        }
        std::string err;
        AtomicReplaceFile(p, bad, err);
    };

    struct ParsedChannelRecord {
        ChannelPersistSnapshot snap;
        std::filesystem::path path;
        std::filesystem::file_time_type mtime{};
    };

    std::unordered_map<ChannelId, ParsedChannelRecord> by_id;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        ChannelId fname_id = INVALID_CHANNEL_ID;
        if (!ParseChannelFileId(entry.path(), fname_id)) continue;

        nlohmann::json j;
        try {
            std::ifstream ifs(entry.path(), std::ios::binary);
            if (!ifs.is_open()) {
                Logger::Instance().Warn("Unable to open persisted channel file '" + entry.path().string() + "'; skipping.");
                continue;
            }
            ifs >> j;
            if (!j.is_object()) {
                throw std::runtime_error("root JSON is not an object");
            }
        } catch (const std::exception& ex) {
            Logger::Instance().Warn(
                "Corrupted persisted channel file '" + entry.path().string() + "' (" + ex.what() + "); renaming to .bad.");
            quarantine_bad_file(entry.path());
            continue;
        }

        if (!j.contains("format_version") || !j["format_version"].is_number_integer()) {
            Logger::Instance().Warn(
                "Invalid persisted channel file '" + entry.path().string() + "' (missing format_version); renaming to .bad.");
            quarantine_bad_file(entry.path());
            continue;
        }
        int fmt_ver = j["format_version"].get<int>();
        if (fmt_ver != 1) {
            Logger::Instance().Warn(
                "Unsupported format_version " + std::to_string(fmt_ver) +
                " in '" + entry.path().string() + "'; leaving file untouched.");
            continue;
        }

        if (!j.contains("id") || !j["id"].is_number_integer() || !j.contains("name") || !j["name"].is_string()) {
            Logger::Instance().Warn(
                "Invalid persisted channel file '" + entry.path().string() + "' (missing id/name); renaming to .bad.");
            quarantine_bad_file(entry.path());
            continue;
        }
        ChannelId cid = j["id"].get<ChannelId>();
        if (cid <= 0) {
            Logger::Instance().Warn(
                "Invalid channel id in '" + entry.path().string() + "'; renaming to .bad.");
            quarantine_bad_file(entry.path());
            continue;
        }

        ChannelPersistSnapshot snap;
        snap.id = cid;
        snap.name = j["name"].get<std::string>();
        if (snap.name.empty()) {
            Logger::Instance().Warn(
                "Empty channel name in '" + entry.path().string() + "'; renaming to .bad.");
            quarantine_bad_file(entry.path());
            continue;
        }
        if (j.contains("password_salt") && j["password_salt"].is_string()) {
            snap.password_salt = j["password_salt"].get<std::string>();
        }
        if (j.contains("password_hash") && j["password_hash"].is_string()) {
            snap.password_hash = j["password_hash"].get<std::string>();
        }
        if (j.contains("has_creator") && j["has_creator"].is_boolean()) {
            snap.has_creator = j["has_creator"].get<bool>();
        }
        if (j.contains("creator_name_hex") && j["creator_name_hex"].is_string()) {
            snap.creator_name = HexToBytes(j["creator_name_hex"].get<std::string>());
        } else if (j.contains("creator_name") && j["creator_name"].is_string()) {
            snap.creator_name = j["creator_name"].get<std::string>();
        }
        if (!snap.has_creator && snap.creator_name.empty()) {
            snap.creator_name = "Server";
        }
        if (j.contains("description_hex") && j["description_hex"].is_string()) {
            snap.utf8_description = SanitizeDescriptionUtf8(HexToBytes(j["description_hex"].get<std::string>()));
        }
        if (j.contains("pawn_description_hex") && j["pawn_description_hex"].is_string()) {
            snap.pawn_description = HexToBytes(j["pawn_description_hex"].get<std::string>());
        }
        if (j.contains("destroy_mode") && j["destroy_mode"].is_number_integer()) {
            int dm = j["destroy_mode"].get<int>();
            if (dm >= 0 && dm <= 2) snap.destroy_mode = static_cast<ChannelDestroyMode>(dm);
        }
        if (!snap.has_creator) {
            snap.destroy_mode = ChannelDestroyMode::Manual;
            snap.destroy_timeout_seconds = 0;
        } else if (j.contains("destroy_timeout_seconds") && j["destroy_timeout_seconds"].is_number_integer()) {
            snap.destroy_timeout_seconds = std::max<std::int32_t>(0, j["destroy_timeout_seconds"].get<std::int32_t>());
        }
        if (j.contains("volume") && j["volume"].is_number()) {
            snap.volume_percent = std::clamp(j["volume"].get<float>(), 0.0f, 200.0f);
        }
        if (j.contains("paused") && j["paused"].is_boolean()) {
            snap.paused = j["paused"].get<bool>();
        }
        if (j.contains("loop_mode") && j["loop_mode"].is_number_integer()) {
            int lm = j["loop_mode"].get<int>();
            if (lm >= 0 && lm <= 2) snap.loop_mode = static_cast<ChannelLoopMode>(lm);
        }
        if (j.contains("playlist_mode") && j["playlist_mode"].is_boolean()) {
            snap.playlist_mode = j["playlist_mode"].get<bool>();
        }
        if (j.contains("playlist_pos") && j["playlist_pos"].is_number_integer()) {
            snap.playlist_pos = j["playlist_pos"].get<std::int32_t>();
        }
        if (j.contains("current_from_queue") && j["current_from_queue"].is_boolean()) {
            snap.current_from_queue = j["current_from_queue"].get<bool>();
        }
        if (j.contains("current_input") && j["current_input"].is_object()) {
            const auto& ci = j["current_input"];
            if (ci.contains("utf8_url") && ci["utf8_url"].is_string()) {
                snap.current_input.utf8_url = ci["utf8_url"].get<std::string>();
            }
            if (ci.contains("pawn_url_hex") && ci["pawn_url_hex"].is_string()) {
                snap.current_input.pawn_url = HexToBytes(ci["pawn_url_hex"].get<std::string>());
            }
            if (ci.contains("label_hex") && ci["label_hex"].is_string()) {
                snap.current_input.utf8_label = HexToBytes(ci["label_hex"].get<std::string>());
            }
            if (ci.contains("pawn_label_hex") && ci["pawn_label_hex"].is_string()) {
                snap.current_input.pawn_label = HexToBytes(ci["pawn_label_hex"].get<std::string>());
            }
            if (ci.contains("description_hex") && ci["description_hex"].is_string()) {
                snap.current_input.utf8_description =
                    SanitizeDescriptionUtf8(HexToBytes(ci["description_hex"].get<std::string>()));
            }
            if (ci.contains("pawn_description_hex") && ci["pawn_description_hex"].is_string()) {
                snap.current_input.pawn_description = HexToBytes(ci["pawn_description_hex"].get<std::string>());
            }
        }
        if (j.contains("was_playing") && j["was_playing"].is_boolean()) {
            snap.was_playing = j["was_playing"].get<bool>();
        }
        if (j.contains("queue") && j["queue"].is_array()) {
            for (const auto& qi : j["queue"]) {
                if (!qi.is_object() || !qi.contains("utf8_url") || !qi["utf8_url"].is_string()) continue;
                ChannelQueueEntry qentry;
                qentry.utf8_url = qi["utf8_url"].get<std::string>();
                if (qentry.utf8_url.empty()) continue;
                if (qi.contains("pawn_url_hex") && qi["pawn_url_hex"].is_string()) {
                    qentry.pawn_url = HexToBytes(qi["pawn_url_hex"].get<std::string>());
                }
                if (qi.contains("label_hex") && qi["label_hex"].is_string()) {
                    qentry.utf8_label = HexToBytes(qi["label_hex"].get<std::string>());
                }
                if (qi.contains("pawn_label_hex") && qi["pawn_label_hex"].is_string()) {
                    qentry.pawn_label = HexToBytes(qi["pawn_label_hex"].get<std::string>());
                }
                if (qi.contains("description_hex") && qi["description_hex"].is_string()) {
                    qentry.utf8_description = SanitizeDescriptionUtf8(HexToBytes(qi["description_hex"].get<std::string>()));
                }
                if (qi.contains("pawn_description_hex") && qi["pawn_description_hex"].is_string()) {
                    qentry.pawn_description = HexToBytes(qi["pawn_description_hex"].get<std::string>());
                }
                snap.queue.push_back(std::move(qentry));
            }
        }
        if (j.contains("spatial") && j["spatial"].is_object()) {
            const auto& sp = j["spatial"];
            if (sp.contains("use_position") && sp["use_position"].is_boolean()) {
                snap.spatial.use_position = sp["use_position"].get<bool>();
            }
            if (sp.contains("x") && sp["x"].is_number()) snap.spatial.position.x = sp["x"].get<float>();
            if (sp.contains("y") && sp["y"].is_number()) snap.spatial.position.y = sp["y"].get<float>();
            if (sp.contains("z") && sp["z"].is_number()) snap.spatial.position.z = sp["z"].get<float>();
            if (sp.contains("distance") && sp["distance"].is_number()) {
                float d = sp["distance"].get<float>();
                snap.spatial.distance = (std::isfinite(d) && d > 0.0f) ? d : 50.0f;
            }
            if (sp.contains("world_id") && sp["world_id"].is_number_integer()) {
                snap.spatial.world_id = sp["world_id"].get<std::int32_t>();
            }
            if (sp.contains("interior_id") && sp["interior_id"].is_number_integer()) {
                snap.spatial.interior_id = sp["interior_id"].get<std::int32_t>();
            }
            if (sp.contains("auto_listen") && sp["auto_listen"].is_boolean()) {
                snap.auto_listen = sp["auto_listen"].get<bool>();
            }
            if (sp.contains("range_group") && sp["range_group"].is_number_integer()) {
                const std::int32_t g = sp["range_group"].get<std::int32_t>();
                snap.range_group = (g >= 0 && g <= 31) ? g : 0;
            }
            if (!std::isfinite(snap.spatial.position.x) ||
                !std::isfinite(snap.spatial.position.y) ||
                !std::isfinite(snap.spatial.position.z)) {
                snap.spatial.position = Vector3{};
                snap.spatial.use_position = false;
            }
        }

        std::error_code tec;
        auto mtime = std::filesystem::last_write_time(entry.path(), tec);
        auto prev = by_id.find(cid);
        if (prev != by_id.end()) {
            if (!tec && mtime > prev->second.mtime) {
                Logger::Instance().Warn(
                    "Duplicate persisted channel ID " + std::to_string(cid) + "; keeping newer file '" +
                    entry.path().string() + "' and marking '" + prev->second.path.string() + "' as .bad.");
                quarantine_bad_file(prev->second.path);
                prev->second = ParsedChannelRecord{std::move(snap), entry.path(), mtime};
            } else {
                Logger::Instance().Warn(
                    "Duplicate persisted channel ID " + std::to_string(cid) + "; marking '" +
                    entry.path().string() + "' as .bad.");
                quarantine_bad_file(entry.path());
            }
        } else {
            by_id.emplace(cid, ParsedChannelRecord{std::move(snap), entry.path(), mtime});
        }
    }

    if (by_id.empty()) return;

    std::vector<ParsedChannelRecord> sorted;
    sorted.reserve(by_id.size());
    for (auto& [cid, rec] : by_id) {
        (void)cid;
        sorted.push_back(std::move(rec));
    }
    std::sort(sorted.begin(), sorted.end(), [](const ParsedChannelRecord& a, const ParsedChannelRecord& b) {
        return a.snap.id < b.snap.id;
    });

    // Reserve all disk IDs (even those skipped above max_channels) so new channels cannot reuse them.
    ChannelId max_disk_id = sorted.empty() ? 0 : sorted.back().snap.id;

    const std::size_t max_ch = std::max<std::uint32_t>(1, config_.max_channels);
    if (sorted.size() > max_ch) {
        std::size_t skipped = sorted.size() - max_ch;
        Logger::Instance().Warn(
            "Found " + std::to_string(sorted.size()) + " persisted channels exceeding max_channels (" +
            std::to_string(max_ch) + "); skipping " + std::to_string(skipped) +
            " highest-ID channel(s) without deleting files.");
        sorted.resize(max_ch);
    }

    auto boot_now = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (next_channel_id_.load() <= max_disk_id) {
            next_channel_id_.store(max_disk_id + 1);
        }

        for (const auto& rec : sorted) {
            const auto& s = rec.snap;
            if (s.destroy_mode == ChannelDestroyMode::OnDisconnect && config_.channel_restore_grace_seconds == 0) {
                Logger::Instance().Info(
                    "Channel " + std::to_string(s.id) +
                    " configured with OnDisconnect and grace=0; removing persisted channel on boot.");
                std::error_code rec_ec;
                std::filesystem::remove(rec.path, rec_ec);
                continue;
            }

            auto ch = std::make_shared<SyncChannel>();
            ch->id = s.id;
            ch->name = s.name;
            ch->utf8_description = s.utf8_description;
            ch->pawn_description = s.pawn_description;
            ch->persistent = true;
            ch->token = GenerateUniqueEndpointCodeLocked();
            ch->password_salt = s.password_salt;
            ch->password_hash = s.password_hash;
            ch->has_creator = s.has_creator;
            ch->creator_name = s.creator_name;
            ch->creator_player_id = INVALID_PLAYER_ID;
            ch->creator_connected = false;
            ch->destroy_mode = s.destroy_mode;
            ch->destroy_timeout_seconds = s.destroy_timeout_seconds;
            if (s.destroy_mode == ChannelDestroyMode::Timeout) {
                ch->creator_disconnected_at = boot_now;
            } else if (s.destroy_mode == ChannelDestroyMode::OnDisconnect) {
                ch->restore_grace_deadline = boot_now +
                    std::chrono::seconds(std::clamp<std::uint32_t>(config_.channel_restore_grace_seconds, 1, 3600));
            }
            ch->volume_percent.store(s.volume_percent);
            ch->pause_flag.store(s.paused);
            ch->loop_mode = s.loop_mode;
            ch->current_input = s.current_input;
            ch->queue.assign(s.queue.begin(), s.queue.end());
            ch->playlist_mode = s.playlist_mode;
            if (ch->playlist_mode && s.playlist_pos >= 0 &&
                static_cast<std::size_t>(s.playlist_pos) < ch->queue.size()) {
                ch->playlist_pos = s.playlist_pos;
                ch->current_from_queue = s.current_from_queue;
            } else {
                ch->playlist_pos = -1;
                ch->current_from_queue = ch->playlist_mode ? false : s.current_from_queue;
            }
            ch->spatial = s.spatial;
            ch->auto_listen = s.auto_listen;
            ch->range_group = s.range_group;
            ch->state.store(StreamState::Idle);
            if (s.was_playing && !s.current_input.utf8_url.empty()) {
                ch->restore_wanted = true;
            }

            channels_[ch->id] = ch;
            AddSortedChannelIdLocked(ch->id);
            pending_restored_channel_ids_.push_back(ch->id);
        }
        pending_restore_notifications_ = !pending_restored_channel_ids_.empty();
    }
}

void AudioEngine::ResumeRestoredChannelsInTick() {
    if (!initialized_.load() || shutting_down_.load()) return;

    struct ResumeWork {
        std::shared_ptr<SyncChannel> ch;
        std::shared_ptr<ChannelRunState> run;
        std::string url;
    };
    std::vector<ResumeWork> work;
    std::vector<ChannelId> error_channels;
    const bool extractor_ready = binary_manager_.IsReady();
    const bool extractor_settled = binary_manager_.IsSettled();
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        for (ChannelId cid : sorted_channel_ids_) {
            auto it = channels_.find(cid);
            if (it == channels_.end() || !it->second ||
                !it->second->restore_wanted || it->second->restore_retry_blocked) {
                continue;
            }
            auto& ch = it->second;
            if (ch->active_run || ch->current_input.utf8_url.empty()) {
                ch->restore_wanted = false;
                continue;
            }
            const bool is_local_lib = StreamResolver::IsLibraryUrl(ch->current_input.utf8_url);
            if (!extractor_ready && !is_local_lib) {
                if (extractor_settled && gamemode_loaded_) {
                    // Block retries for this run only; keep restore_wanted=true so later saves preserve was_playing.
                    ch->restore_retry_blocked = true;
                    error_channels.push_back(cid);
                }
                continue;
            }
            std::size_t limit = EffectiveStreamConcurrencyLimit();
            if (CurrentStreamLoadLocked(INVALID_STREAM_ID, cid) + 1 > limit) {
                // Defer to a later tick without failing or discarding queued tracks.
                break;
            }
            const bool was_paused = ch->pause_flag.load();
            const std::int32_t saved_pos = ch->playlist_pos;
            const bool saved_from_queue = ch->current_from_queue;
            std::string url = ch->current_input.utf8_url;
            std::string pawn_url = ch->current_input.pawn_url;
            std::string utf8_label = ch->current_input.utf8_label;
            const ChannelQueueEntry saved_input = ch->current_input;
            std::shared_ptr<SyncChannel> out_ch;
            std::shared_ptr<ChannelRunState> out_run;
            if (PlayChannelLocked(cid, url, pawn_url, out_ch, out_run, utf8_label)) {
                CopyQueueEntryDescription(out_ch->current_input, saved_input);
                out_ch->pause_flag.store(was_paused);
                out_ch->playlist_pos = out_ch->playlist_mode ? saved_pos : -1;
                out_ch->current_from_queue = saved_from_queue;
                out_ch->consecutive_errors = 0;
                work.push_back(ResumeWork{out_ch, out_run, std::move(url)});
            }
        }
    }
    if (!work.empty()) {
        HttpClient::PollCancellations();
        for (const auto& w : work) {
            StartChannelWorker(w.ch, w.run, w.url);
        }
    }
    for (ChannelId cid : error_channels) {
        host_.DispatchPawnCallback("OnAudioStreamChannelError", {
            cid,
            static_cast<std::int32_t>(ErrorCode::ExtractorNotReady),
            std::string("Extractor not ready")
        });
    }
}

std::int32_t AudioEngine::RefreshAudioLibrary() {
    resolver_.InvalidateCachedLibraryTracks();
    auto scanned = StreamResolver::ScanMusicLibraryDir(
        config_.music_dir, config_.pawn_text_encoding, 200, 1000);
    std::lock_guard<std::mutex> lock(library_mutex_);
    library_folders_ = std::move(scanned);
    return static_cast<std::int32_t>(library_folders_.size());
}

std::int32_t AudioEngine::GetAudioLibraryFolderCount() const {
    std::lock_guard<std::mutex> lock(library_mutex_);
    return static_cast<std::int32_t>(library_folders_.size());
}

bool AudioEngine::GetAudioLibraryFolder(
    std::int32_t index,
    std::string& out_pawn_name,
    std::int32_t& out_track_count) const {
    out_pawn_name.clear();
    out_track_count = 0;
    if (index < 0) return false;
    std::lock_guard<std::mutex> lock(library_mutex_);
    const std::size_t idx = static_cast<std::size_t>(index);
    if (idx >= library_folders_.size()) return false;
    out_pawn_name = library_folders_[idx].pawn_name;
    out_track_count = static_cast<std::int32_t>(library_folders_[idx].pawn_tracks.size());
    return true;
}

bool AudioEngine::GetAudioLibraryTrack(
    std::int32_t folder_index,
    std::int32_t track_index,
    std::string& out_pawn_name) const {
    out_pawn_name.clear();
    if (folder_index < 0 || track_index < 0) return false;
    std::lock_guard<std::mutex> lock(library_mutex_);
    const std::size_t fidx = static_cast<std::size_t>(folder_index);
    if (fidx >= library_folders_.size()) return false;
    const auto& tracks = library_folders_[fidx].pawn_tracks;
    const std::size_t tidx = static_cast<std::size_t>(track_index);
    if (tidx >= tracks.size()) return false;
    out_pawn_name = tracks[tidx];
    return true;
}

} // namespace sampaudiostream
