#include "sampaudiostream/network/http_server.hpp"

#include "sampaudiostream/core/config.hpp"
#include "sampaudiostream/core/logger.hpp"
#include "sampaudiostream/core/thread_pool.hpp"

#include <algorithm>
#include <asio.hpp>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>
#ifndef _WIN32
#include <sys/resource.h>
#endif

namespace sampaudiostream {

namespace {

std::string SanitizeHeaderValue(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c != '\r' && c != '\n') {
            out.push_back(c);
        }
    }
    return out;
}

std::string ExtractEndpointToken(const std::string& target) {
    // Expected: /stream/<token>.mp3 or /channel/<token>.mp3 (optional query string)
    std::string path = target;
    auto qpos = path.find('?');
    if (qpos != std::string::npos) {
        path = path.substr(0, qpos);
    }

    auto last_slash = path.rfind('/');
    if (last_slash == std::string::npos || last_slash + 1 >= path.size()) {
        return "";
    }
    std::string filename = path.substr(last_slash + 1);
    auto dot = filename.rfind('.');
    if (dot != std::string::npos) {
        filename = filename.substr(0, dot);
    }
    return filename;
}

bool ExtractResumeOffset(const std::string& target, std::uint64_t& out_offset) {
    auto qpos = target.find('?');
    if (qpos == std::string::npos) return false;
    std::string query = target.substr(qpos + 1);
    std::size_t pos = query.find("resume=");
    if (pos == std::string::npos || (pos > 0 && query[pos - 1] != '&')) return false;
    const char* start = query.c_str() + pos + 7;
    if (*start < '0' || *start > '9') return false;
    char* endptr = nullptr;
    unsigned long long val = std::strtoull(start, &endptr, 10);
    if (endptr == start) return false;
    out_offset = static_cast<std::uint64_t>(val);
    return true;
}

std::size_t ParseMp3FrameLength(const std::uint8_t* h) {
    if (h[0] != 0xFF || (h[1] & 0xE0u) != 0xE0u) return 0;
    unsigned int version = (h[1] >> 3) & 0x03u; // 0=2.5, 1=reserved, 2=2, 3=1
    unsigned int layer = (h[1] >> 1) & 0x03u;   // 1=Layer III
    unsigned int br_idx = (h[2] >> 4) & 0x0Fu;
    unsigned int sr_idx = (h[2] >> 2) & 0x03u;
    unsigned int pad = (h[2] >> 1) & 0x01u;
    if (version == 1 || layer != 1 || br_idx == 0 || br_idx == 15 || sr_idx == 3) {
        return 0;
    }

    static constexpr int kBitratesV1L3[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
    static constexpr int kBitratesV2L3[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
    static constexpr int kSampleRates[4][3] = {
        {11025, 12000, 8000},  // MPEG 2.5 (version 0)
        {0, 0, 0},
        {22050, 24000, 16000}, // MPEG 2 (version 2)
        {44100, 48000, 32000}  // MPEG 1 (version 3)
    };
    int bitrate = (version == 3 ? kBitratesV1L3[br_idx] : kBitratesV2L3[br_idx]) * 1000;
    int srate = kSampleRates[version][sr_idx];
    if (bitrate <= 0 || srate <= 0) return 0;

    int coef = (version == 3) ? 144 : 72;
    return static_cast<std::size_t>((coef * bitrate) / srate + static_cast<int>(pad));
}

void AlignOffsetToMp3Frame(const AudioStreamBuffer& buf, std::uint64_t& in_out_offset) {
    std::array<std::uint8_t, 4096> peek{};
    std::size_t n = buf.PeekAt(in_out_offset, peek.data(), peek.size());
    if (n < 4) return;

    std::size_t first_single_hit = std::string::npos;
    for (std::size_t i = 0; i + 4 <= n; ++i) {
        std::size_t flen = ParseMp3FrameLength(peek.data() + i);
        if (flen >= 24 && flen <= 2880) {
            if (i + flen + 4 <= n) {
                if (ParseMp3FrameLength(peek.data() + i + flen) > 0) {
                    in_out_offset += i;
                    return;
                }
            } else if (first_single_hit == std::string::npos) {
                first_single_hit = i;
            }
        }
    }
    if (first_single_hit != std::string::npos) {
        in_out_offset += first_single_hit;
    }
}

class ClientConnection : public std::enable_shared_from_this<ClientConnection> {
public:
    using Strand = asio::strand<asio::io_context::executor_type>;
    using EndpointLookupFn = std::function<bool(const std::string&, StreamEndpointInfo&)>;
    using ProgressReportFn = std::function<void(
        const std::string&,
        std::uint64_t,
        std::uint64_t,
        std::chrono::steady_clock::time_point,
        std::uint32_t)>;
    using BindEndpointFn = std::function<void(const std::string&)>;
    using CloseCallbackFn = std::function<void(ClientConnection*, const std::string&, const std::string&)>;

    ClientConnection(
        asio::ip::tcp::socket socket,
        Strand strand,
        std::string remote_ip,
        EndpointLookupFn lookup_fn,
        ProgressReportFn progress_fn,
        BindEndpointFn bind_endpoint_fn,
        CloseCallbackFn close_fn)
        : socket_(std::move(socket)),
          strand_(std::move(strand)),
          header_timer_(strand_),
          write_timer_(strand_),
          request_buf_(8192), // Cap HTTP request headers at 8 KB
          remote_ip_(std::move(remote_ip)),
          lookup_fn_(std::move(lookup_fn)),
          progress_fn_(std::move(progress_fn)),
          bind_endpoint_fn_(std::move(bind_endpoint_fn)),
          close_fn_(std::move(close_fn)) {}

    ~ClientConnection() {
        NotifyClosedOnce();
    }

    void Start() {
        auto self = shared_from_this();
        asio::post(strand_, [self]() {
            self->StartOnStrand();
        });
    }

    void CloseAsync() {
        auto self = shared_from_this();
        asio::post(strand_, [self]() {
            self->CloseSocket();
        });
    }

private:
    void StartOnStrand() {
        if (closed_) return;
        auto self = shared_from_this();
        header_timer_.expires_after(std::chrono::seconds(5));
        header_timer_.async_wait(asio::bind_executor(strand_, [self](std::error_code ec) {
            if (!ec && !self->closed_) {
                self->CloseSocket();
            }
        }));

        asio::async_read_until(
            socket_,
            request_buf_,
            "\r\n\r\n",
            asio::bind_executor(strand_, [self](std::error_code ec, std::size_t) {
                try {
                    self->header_timer_.cancel();
                } catch (...) {
                }
                if (self->closed_) return;
                if (!ec) {
                    self->HandleRequest();
                } else {
                    self->CloseSocket();
                }
            }));
    }
    void HandleRequest() {
        std::istream is(&request_buf_);
        std::string request_line;
        std::getline(is, request_line);
        std::istringstream rlss(request_line);
        std::string method, target, version;
        rlss >> method >> target >> version;

        std::string hline;
        while (std::getline(is, hline)) {
            if (!hline.empty() && hline.back() == '\r') hline.pop_back();
            if (hline.empty()) break;
            auto colon = hline.find(':');
            if (colon == std::string::npos) continue;
            std::string key = hline.substr(0, colon);
            std::string val = hline.substr(colon + 1);
            for (char& ch : key) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(val.begin());
            while (!val.empty() && (val.back() == ' ' || val.back() == '\t')) val.pop_back();
            if (key == "icy-metadata" && val == "1") {
                icy_metadata_requested_ = true;
            }
        }

        if ((method != "GET" && method != "HEAD") || target.empty()) {
            SendErrorResponse(400, "Bad Request");
            return;
        }

        std::string token = ExtractEndpointToken(target);
        if (token.empty() || !lookup_fn_(token, endpoint_) || !endpoint_.buffer) {
            SendErrorResponse(404, "Not Found");
            return;
        }

        is_head_ = (method == "HEAD");
        bound_token_ = endpoint_.token;
        if (bind_endpoint_fn_) {
            bind_endpoint_fn_(bound_token_);
        }
        Logger::Instance().Debug(
            "HTTP client connected: " + method + " " + target + " from " + remote_ip_ +
            (icy_metadata_requested_ ? " (ICY metadata=1)" : ""));

        if (endpoint_.is_live_channel) {
            std::uint64_t written = endpoint_.buffer->TotalWritten();
            std::uint64_t earliest = endpoint_.buffer->EarliestAvailableOffset();
            std::uint64_t gen_start = endpoint_.buffer->CurrentGenerationStartOffset();
            std::uint64_t floor_off = std::max(earliest, gen_start);
            if (written > endpoint_.live_burst_bytes) {
                read_offset_ = std::max(floor_off, written - endpoint_.live_burst_bytes);
            } else {
                read_offset_ = floor_off;
            }
            if (read_offset_ > gen_start) {
                AlignOffsetToMp3Frame(*endpoint_.buffer, read_offset_);
            }
        } else {
            std::uint64_t earliest = endpoint_.buffer->EarliestAvailableOffset();
            std::uint64_t written = endpoint_.buffer->TotalWritten();
            std::uint64_t resume_off = 0;
            if (ExtractResumeOffset(target, resume_off)) {
                read_offset_ = std::clamp(resume_off, earliest, written);
                if (read_offset_ > 0) {
                    AlignOffsetToMp3Frame(*endpoint_.buffer, read_offset_);
                }
            } else if (earliest > 0) {
                read_offset_ = earliest;
                AlignOffsetToMp3Frame(*endpoint_.buffer, read_offset_);
            } else {
                read_offset_ = 0;
            }
            initial_read_offset_ = read_offset_;
        }

        std::ostringstream oss;
        oss << "HTTP/1.1 200 OK\r\n"
            << "Content-Type: audio/mpeg\r\n"
            << "Accept-Ranges: none\r\n"
            << "Cache-Control: no-cache, no-store\r\n"
            << "Connection: close\r\n"
            << "icy-br: " << endpoint_.bitrate_kbps << "\r\n";

        const std::string& header_name = !endpoint_.station_name.empty() ? endpoint_.station_name : endpoint_.title;
        if (!header_name.empty()) {
            std::string icy_name = ConvertTextForPawn(header_name, "cp1252");
            oss << "icy-name: " << SanitizeHeaderValue(icy_name) << "\r\n";
        }
        if (icy_metadata_requested_) {
            oss << "icy-metaint: " << kIcyMetaInterval << "\r\n";
        }
        oss << "\r\n";

        header_response_ = oss.str();
        auto self = shared_from_this();
        AsyncWriteWithDeadline(asio::buffer(header_response_), [self]() {
            if (self->is_head_) {
                self->CloseSocket();
                return;
            }
            self->StartDisconnectWatcher();
            self->PumpNextChunk();
        });
    }

    void StartDisconnectWatcher() {
        if (closed_) return;
        auto self = shared_from_this();
        socket_.async_read_some(
            asio::buffer(disconnect_probe_buf_),
            asio::bind_executor(strand_, [self](std::error_code ec, std::size_t n) {
                if (self->closed_) return;
                if (ec || n == 0) {
                    self->CloseSocket();
                    return;
                }
                self->StartDisconnectWatcher();
            }));
    }

    template <typename Callback>
    void AsyncWriteWithDeadline(const asio::const_buffer& buf, Callback&& on_done) {
        if (closed_) return;
        auto self = shared_from_this();
        std::uint64_t seq = ++write_seq_;
        write_timer_.expires_after(std::chrono::seconds(30));
        write_timer_.async_wait(asio::bind_executor(strand_, [self, seq](std::error_code ec) {
            if (!ec && !self->closed_ && self->write_seq_ == seq) {
                self->CloseSocket();
            }
        }));

        asio::async_write(
            socket_,
            buf,
            asio::bind_executor(strand_, [self, seq, cb = std::forward<Callback>(on_done)](std::error_code ec, std::size_t) mutable {
                if (self->write_seq_ == seq) {
                    ++self->write_seq_;
                    try {
                        self->write_timer_.cancel();
                    } catch (...) {
                    }
                }
                if (self->closed_ || ec) {
                    self->CloseSocket();
                    return;
                }
                cb();
            }));
    }

    void BuildIcyMetaFrame(const StreamEndpointInfo& ep) {
        std::string raw;
        if (!ep.artist.empty() && !ep.title.empty()) {
            std::string lt = ep.title;
            std::string la = ep.artist;
            std::transform(lt.begin(), lt.end(), lt.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            std::transform(la.begin(), la.end(), la.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            raw = (lt.find(la) != std::string::npos) ? ep.title : (ep.artist + " - " + ep.title);
        } else if (!ep.title.empty()) {
            raw = ep.title;
        } else {
            raw = ep.artist;
        }

        std::string cp = ConvertTextForPawn(raw, "cp1252");
        std::string clean;
        clean.reserve(cp.size());
        for (unsigned char c : cp) {
            if (c == '\'' || c == '\r' || c == '\n' || c == '\0') continue;
            clean.push_back(static_cast<char>(c));
            if (clean.size() >= 200) break;
        }

        if (clean.empty()) {
            icy_meta_buf_.assign(1, 0);
            return;
        }

        std::string payload = "StreamTitle='" + clean + "';";
        std::size_t blocks = (payload.size() + 15) / 16;
        if (blocks > 255) blocks = 255;
        std::size_t payload_fit = std::min(payload.size(), blocks * 16);
        icy_meta_buf_.assign(1 + blocks * 16, 0);
        icy_meta_buf_[0] = static_cast<std::uint8_t>(blocks);
        std::memcpy(icy_meta_buf_.data() + 1, payload.data(), payload_fit);
    }

    void PumpNextChunk() {
        if (closed_ || !endpoint_.buffer) {
            CloseSocket();
            return;
        }

        if (icy_metadata_requested_ && bytes_since_meta_ >= kIcyMetaInterval) {
            StreamEndpointInfo latest{};
            if (lookup_fn_ && lookup_fn_(bound_token_, latest)) {
                endpoint_.title = latest.title;
                endpoint_.artist = latest.artist;
            }
            if (!sent_initial_icy_meta_ ||
                endpoint_.title != last_icy_title_ ||
                endpoint_.artist != last_icy_artist_) {
                BuildIcyMetaFrame(endpoint_);
                last_icy_title_ = endpoint_.title;
                last_icy_artist_ = endpoint_.artist;
                sent_initial_icy_meta_ = true;
            } else {
                icy_meta_buf_.assign(1, 0);
            }
            bytes_since_meta_ = 0;
            auto self = shared_from_this();
            AsyncWriteWithDeadline(asio::buffer(icy_meta_buf_), [self]() {
                self->PumpNextChunk();
            });
            return;
        }

        std::size_t to_read = write_buf_.size();
        if (icy_metadata_requested_) {
            to_read = std::min(to_read, kIcyMetaInterval - bytes_since_meta_);
        }

        std::size_t n = endpoint_.buffer->Read(read_offset_, write_buf_.data(), to_read, 0);
        if (n > 0) {
            if (icy_metadata_requested_) {
                bytes_since_meta_ += n;
            }
            if (!endpoint_.is_live_channel && progress_fn_) {
                auto now = std::chrono::steady_clock::now();
                bool first_chunk = (first_send_time_ == std::chrono::steady_clock::time_point{});
                if (first_chunk) {
                    first_send_time_ = now;
                }
                if (first_chunk ||
                    last_progress_report_ == std::chrono::steady_clock::time_point{} ||
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - last_progress_report_).count() >= 1000) {
                    last_progress_report_ = now;
                    progress_fn_(
                        endpoint_.token,
                        initial_read_offset_,
                        read_offset_,
                        first_send_time_,
                        endpoint_.bitrate_kbps);
                }
            }
            auto self = shared_from_this();
            AsyncWriteWithDeadline(asio::buffer(write_buf_.data(), n), [self]() {
                self->PumpNextChunk();
            });
            return;
        }

        if (endpoint_.buffer->IsClosed() ||
            (endpoint_.buffer->IsEof() && read_offset_ >= endpoint_.buffer->TotalWritten())) {
            CloseSocket();
            return;
        }

        // Event-driven notification when writer pushes new audio, EOF, or Close.
        // Captures strong self so the connection remains alive while waiting for data;
        // cycle is broken both when the buffer notifies (clears waiters_) and in CloseSocket (RemoveWaiter).
        auto self = shared_from_this();
        auto strand = strand_;
        waiter_id_ = endpoint_.buffer->RegisterWaiter([self, strand]() {
            asio::post(strand, [self]() {
                if (self->closed_) return;
                self->waiter_id_ = 0;
                self->PumpNextChunk();
            });
        });

        // Re-check after registering waiter so no notification is missed between Read and RegisterWaiter.
        // Only schedule another pump if we removed the waiter before it already fired and posted one.
        if (endpoint_.buffer->TotalWritten() > read_offset_ ||
            endpoint_.buffer->IsEof() ||
            endpoint_.buffer->IsClosed()) {
            if (waiter_id_ == 0 || endpoint_.buffer->RemoveWaiter(waiter_id_)) {
                waiter_id_ = 0;
                asio::post(strand_, [self]() {
                    if (!self->closed_) {
                        self->PumpNextChunk();
                    }
                });
            }
        }
    }

    void SendErrorResponse(int code, const std::string& reason) {
        std::ostringstream oss;
        oss << "HTTP/1.1 " << code << " " << reason << "\r\n"
            << "Content-Length: 0\r\n"
            << "Connection: close\r\n\r\n";
        header_response_ = oss.str();
        auto self = shared_from_this();
        AsyncWriteWithDeadline(asio::buffer(header_response_), [self]() {
            self->CloseSocket();
        });
    }

    void NotifyClosedOnce() {
        if (!closed_notified_.exchange(true) && close_fn_) {
            close_fn_(this, remote_ip_, bound_token_);
        }
    }

    void CloseSocket() {
        closed_ = true;
        ++write_seq_;
        if (endpoint_.buffer && waiter_id_ != 0) {
            endpoint_.buffer->RemoveWaiter(waiter_id_);
            waiter_id_ = 0;
        }
        std::error_code ec;
        try {
            header_timer_.cancel();
            write_timer_.cancel();
        } catch (...) {
        }
        socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        socket_.close(ec);
        NotifyClosedOnce();
    }

    asio::ip::tcp::socket socket_;
    Strand strand_;
    asio::steady_timer header_timer_;
    asio::steady_timer write_timer_;
    asio::streambuf request_buf_;
    std::string remote_ip_;
    std::string bound_token_;
    EndpointLookupFn lookup_fn_;
    ProgressReportFn progress_fn_;
    BindEndpointFn bind_endpoint_fn_;
    CloseCallbackFn close_fn_;
    StreamEndpointInfo endpoint_;
    std::string header_response_;
    std::array<std::uint8_t, 16384> write_buf_{};
    std::array<std::uint8_t, 1> disconnect_probe_buf_{};
    std::uint64_t initial_read_offset_{0};
    std::uint64_t read_offset_{0};
    std::uint64_t write_seq_{0};
    std::chrono::steady_clock::time_point first_send_time_{};
    std::chrono::steady_clock::time_point last_progress_report_{};
    AudioStreamBuffer::WaiterId waiter_id_{0};
    std::atomic<bool> closed_notified_{false};
    bool closed_{false};
    bool is_head_{false};
    bool icy_metadata_requested_{false};
    bool sent_initial_icy_meta_{false};
    std::size_t bytes_since_meta_{0};
    static constexpr std::size_t kIcyMetaInterval = 16000;
    std::string last_icy_title_;
    std::string last_icy_artist_;
    std::vector<std::uint8_t> icy_meta_buf_;
};

} // namespace

struct HttpStreamingServer::Impl {
    std::unique_ptr<asio::io_context> io_ctx;
    std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>> work_guard;
    std::unique_ptr<asio::ip::tcp::acceptor> acceptor;
    std::vector<ManagedThread> io_threads;
    std::atomic<bool> running{false};
    std::uint16_t bound_port{0};

    struct EndpointPlaybackStats {
        std::uint64_t initial_offset{0};
        std::uint64_t served_offset{0};
        std::chrono::steady_clock::time_point first_send_time{};
        std::uint32_t bitrate_kbps{128};
    };

    mutable std::mutex endpoints_mutex;
    std::unordered_map<std::string, StreamEndpointInfo> endpoints;
    std::unordered_map<std::string, std::uint64_t> last_served_offsets;
    std::unordered_map<std::string, EndpointPlaybackStats> playback_stats;

    std::size_t max_total_connections{1024};
    std::size_t max_connections_per_ip{64};
    mutable std::mutex connections_mutex;
    std::condition_variable connections_cv;
    bool warned_total_limit{false};
    std::chrono::steady_clock::time_point last_total_limit_warn{};
    bool warned_per_ip_limit{false};
    std::chrono::steady_clock::time_point last_per_ip_limit_warn{};
    std::size_t total_connections{0};
    std::unordered_map<std::string, std::size_t> ip_connections;
    std::unordered_map<std::string, std::size_t> endpoint_connections;
    std::unordered_map<std::string, std::uint64_t> endpoint_total_connections;
    std::unordered_map<ClientConnection*, std::shared_ptr<ClientConnection>> live_connections;

    enum class RejectReason {
        None,
        TotalLimit,
        PerIpLimit
    };

    struct RejectWarnInfo {
        RejectReason reason{RejectReason::None};
        std::size_t current{0};
        std::size_t limit{0};
    };

    bool TryAcquireConnectionSlot(const std::string& ip, RejectWarnInfo& out_warn) {
        out_warn = RejectWarnInfo{};
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(connections_mutex);
        if (total_connections >= max_total_connections) {
            if (!warned_total_limit || (now - last_total_limit_warn) >= std::chrono::minutes(1)) {
                warned_total_limit = true;
                last_total_limit_warn = now;
                out_warn.reason = RejectReason::TotalLimit;
                out_warn.current = total_connections;
                out_warn.limit = max_total_connections;
            }
            return false;
        }
        auto it = ip_connections.find(ip);
        const std::size_t curr_ip = (it != ip_connections.end()) ? it->second : 0;
        if (curr_ip >= max_connections_per_ip) {
            if (!warned_per_ip_limit || (now - last_per_ip_limit_warn) >= std::chrono::minutes(1)) {
                warned_per_ip_limit = true;
                last_per_ip_limit_warn = now;
                out_warn.reason = RejectReason::PerIpLimit;
                out_warn.current = curr_ip;
                out_warn.limit = max_connections_per_ip;
            }
            return false;
        }
        ++ip_connections[ip];
        ++total_connections;
        return true;
    }

    void RegisterLiveConnection(const std::shared_ptr<ClientConnection>& conn) {
        if (!conn) return;
        std::lock_guard<std::mutex> lock(connections_mutex);
        live_connections[conn.get()] = conn;
    }

    void BindConnectionEndpoint(const std::string& token) {
        if (token.empty()) return;
        std::lock_guard<std::mutex> lock(connections_mutex);
        ++endpoint_connections[token];
        ++endpoint_total_connections[token];
    }

    void OnConnectionClosed(ClientConnection* raw, const std::string& ip, const std::string& token) {
        std::shared_ptr<ClientConnection> removed;
        bool all_gone = false;
        {
            std::lock_guard<std::mutex> lock(connections_mutex);
            if (total_connections > 0) --total_connections;
            auto it = ip_connections.find(ip);
            if (it != ip_connections.end()) {
                if (it->second <= 1) ip_connections.erase(it);
                else --(it->second);
            }
            if (!token.empty()) {
                auto eit = endpoint_connections.find(token);
                if (eit != endpoint_connections.end()) {
                    if (eit->second <= 1) endpoint_connections.erase(eit);
                    else --(eit->second);
                }
            }
            auto lit = live_connections.find(raw);
            if (lit != live_connections.end()) {
                removed = std::move(lit->second);
                live_connections.erase(lit);
            }
            all_gone = live_connections.empty();
        }
        if (all_gone) {
            connections_cv.notify_all();
        }
    }

    bool LookupEndpoint(const std::string& token, StreamEndpointInfo& out_info) {
        std::lock_guard<std::mutex> lock(endpoints_mutex);
        auto it = endpoints.find(token);
        if (it == endpoints.end()) {
            return false;
        }
        out_info = it->second;
        return true;
    }

    void ReportServedOffset(
        const std::string& token,
        std::uint64_t initial_offset,
        std::uint64_t served_offset,
        std::chrono::steady_clock::time_point first_send_time,
        std::uint32_t bitrate_kbps) {
        std::lock_guard<std::mutex> lock(endpoints_mutex);
        last_served_offsets[token] = served_offset;
        playback_stats[token] = EndpointPlaybackStats{
            initial_offset, served_offset, first_send_time, bitrate_kbps
        };
    }

    void DoAccept() {
        if (!running.load() || !acceptor || !acceptor->is_open() || !io_ctx) {
            return;
        }
        auto strand = asio::make_strand(*io_ctx);
        acceptor->async_accept(strand, [this, strand](std::error_code ec, asio::ip::tcp::socket socket) {
            if (!ec && running.load()) {
                std::error_code ep_ec;
                auto remote_ep = socket.remote_endpoint(ep_ec);
                std::string remote_ip = ep_ec ? "unknown" : remote_ep.address().to_string();

                Impl::RejectWarnInfo reject_warn;
                if (!TryAcquireConnectionSlot(remote_ip, reject_warn)) {
                    std::error_code close_ec;
                    socket.close(close_ec);
                    if (reject_warn.reason == Impl::RejectReason::PerIpLimit) {
                        Logger::Instance().Warn(
                            "HTTP audio connection from " + remote_ip +
                            " rejected: per-IP limit reached (" +
                            std::to_string(reject_warn.current) + "/" +
                            std::to_string(reject_warn.limit) +
                            "). Players sharing this IP (mobile carrier CGNAT, LAN house) beyond this limit hear nothing; "
                            "raise 'network.max_connections_per_ip' if needed.");
                    } else if (reject_warn.reason == Impl::RejectReason::TotalLimit) {
                        Logger::Instance().Warn(
                            "HTTP audio connection rejected: total connection limit reached (" +
                            std::to_string(reject_warn.current) + "/" +
                            std::to_string(reject_warn.limit) +
                            "); raise 'network.max_http_connections' if more simultaneous listeners are expected.");
                    }
                } else {
                    std::error_code opt_ec;
                    socket.set_option(asio::ip::tcp::no_delay(true), opt_ec);
                    auto conn = std::make_shared<ClientConnection>(
                        std::move(socket),
                        strand,
                        remote_ip,
                        [this](const std::string& token, StreamEndpointInfo& info) {
                            return LookupEndpoint(token, info);
                        },
                        [this](const std::string& token,
                               std::uint64_t init_off,
                               std::uint64_t served_off,
                               std::chrono::steady_clock::time_point first_sent,
                               std::uint32_t br_kbps) {
                            ReportServedOffset(token, init_off, served_off, first_sent, br_kbps);
                        },
                        [this](const std::string& token) {
                            BindConnectionEndpoint(token);
                        },
                        [this](ClientConnection* raw, const std::string& ip, const std::string& token) {
                            OnConnectionClosed(raw, ip, token);
                        });
                    RegisterLiveConnection(conn);
                    conn->Start();
                }
            }
            if (running.load()) {
                DoAccept();
            }
        });
    }
};

HttpStreamingServer::HttpStreamingServer()
    : impl_(std::make_unique<Impl>()) {}

HttpStreamingServer::~HttpStreamingServer() {
    Stop();
}

void HttpStreamingServer::SetConnectionLimits(
    std::size_t max_total_connections,
    std::size_t max_connections_per_ip) {
    std::lock_guard<std::mutex> lock(impl_->connections_mutex);
    impl_->max_total_connections = std::max<std::size_t>(1, max_total_connections);
    impl_->max_connections_per_ip = std::max<std::size_t>(1, max_connections_per_ip);
}

bool HttpStreamingServer::Start(const std::string& bind_ip, std::uint16_t port, std::string& out_error, bool log_listen) {
    if (impl_->running.load()) {
        return true;
    }

    std::size_t configured_max_total = 1024;
    {
        std::lock_guard<std::mutex> lock(impl_->connections_mutex);
        impl_->warned_total_limit = false;
        impl_->warned_per_ip_limit = false;
        configured_max_total = impl_->max_total_connections;
    }

#ifndef _WIN32
    {
        struct rlimit rl{};
        if (::getrlimit(RLIMIT_NOFILE, &rl) == 0 &&
            rl.rlim_cur != RLIM_INFINITY &&
            rl.rlim_cur < static_cast<rlim_t>(configured_max_total + 256)) {
            Logger::Instance().Warn(
                "Process open file descriptor limit (RLIMIT_NOFILE=" +
                std::to_string(static_cast<unsigned long long>(rl.rlim_cur)) +
                ") is lower than recommended (" +
                std::to_string(configured_max_total + 256) +
                " for network.max_http_connections=" +
                std::to_string(configured_max_total) +
                "): Linux may reject HTTP audio connections before reaching the configured limit. "
                "Raise it with 'ulimit -n <N>', 'LimitNOFILE=' in systemd, or /etc/security/limits.conf.");
        }
    }
#else
    (void)configured_max_total;
#endif

    try {
        impl_->io_ctx = std::make_unique<asio::io_context>();
        impl_->work_guard = std::make_unique<asio::executor_work_guard<asio::io_context::executor_type>>(
            impl_->io_ctx->get_executor());

        asio::ip::address addr = bind_ip.empty()
                                     ? asio::ip::address_v4::any()
                                     : asio::ip::make_address(bind_ip);
        asio::ip::tcp::endpoint ep(addr, port);

        impl_->acceptor = std::make_unique<asio::ip::tcp::acceptor>(*impl_->io_ctx);
        impl_->acceptor->open(ep.protocol());
#ifdef _WIN32
        impl_->acceptor->set_option(asio::ip::tcp::acceptor::reuse_address(false));
        {
            BOOL exclusive = TRUE;
            ::setsockopt(
                impl_->acceptor->native_handle(),
                SOL_SOCKET,
                SO_EXCLUSIVEADDRUSE,
                reinterpret_cast<const char*>(&exclusive),
                sizeof(exclusive));
        }
#else
        impl_->acceptor->set_option(asio::ip::tcp::acceptor::reuse_address(true));
#endif
        impl_->acceptor->bind(ep);
        impl_->acceptor->listen(asio::socket_base::max_listen_connections);

        impl_->bound_port = impl_->acceptor->local_endpoint().port();
        impl_->running.store(true);
        impl_->DoAccept();

        for (int i = 0; i < 2; ++i) {
            ManagedThread t;
            if (t.Start([this]() { impl_->io_ctx->run(); }, 256 * 1024)) {
                impl_->io_threads.push_back(std::move(t));
            }
        }
        if (impl_->io_threads.empty()) {
            out_error = "Failed to spawn HTTP I/O threads";
            Stop();
            return false;
        }

        if (log_listen) {
            Logger::Instance().Info("HTTP audio streaming server listening on " + addr.to_string() + ":" + std::to_string(impl_->bound_port));
        }
        return true;
    } catch (const std::exception& ex) {
        out_error = ex.what();
        Stop();
        return false;
    }
}

void HttpStreamingServer::Stop() {
    bool was_running = impl_->running.exchange(false);
    if (!was_running && !impl_->acceptor && !impl_->work_guard && !impl_->io_ctx && impl_->io_threads.empty()) {
        return;
    }

    std::error_code ec;
    if (impl_->acceptor) {
        impl_->acceptor->close(ec);
        impl_->acceptor.reset();
    }
    impl_->bound_port = 0;

    std::vector<std::shared_ptr<ClientConnection>> to_close;
    {
        std::lock_guard<std::mutex> lock(impl_->connections_mutex);
        to_close.reserve(impl_->live_connections.size());
        for (const auto& [_, conn] : impl_->live_connections) {
            if (conn) to_close.push_back(conn);
        }
    }
    for (auto& conn : to_close) {
        conn->CloseAsync();
    }
    to_close.clear();

    impl_->work_guard.reset();
    if (was_running) {
        std::unique_lock<std::mutex> lock(impl_->connections_mutex);
        impl_->connections_cv.wait_for(lock, std::chrono::milliseconds(500), [this]() {
            return impl_->live_connections.empty();
        });
    }

    if (impl_->io_ctx) {
        impl_->io_ctx->stop();
    }

    for (auto& t : impl_->io_threads) {
        t.Join();
    }
    impl_->io_threads.clear();

    // Destroy any remaining ClientConnection objects outside connections_mutex and
    // before io_ctx.reset() so Asio sockets/timers tear down while io_ctx is valid.
    std::vector<std::shared_ptr<ClientConnection>> remaining_connections;
    {
        std::lock_guard<std::mutex> lock(impl_->connections_mutex);
        remaining_connections.reserve(impl_->live_connections.size());
        for (auto& [_, conn] : impl_->live_connections) {
            if (conn) remaining_connections.push_back(std::move(conn));
        }
        impl_->live_connections.clear();
        impl_->endpoint_connections.clear();
        impl_->endpoint_total_connections.clear();
        impl_->ip_connections.clear();
        impl_->total_connections = 0;
    }
    remaining_connections.clear();

    impl_->io_ctx.reset();

    std::lock_guard<std::mutex> lock(impl_->endpoints_mutex);
    impl_->endpoints.clear();
    impl_->last_served_offsets.clear();
    impl_->playback_stats.clear();
}

bool HttpStreamingServer::IsRunning() const {
    return impl_->running.load();
}

std::uint16_t HttpStreamingServer::BoundPort() const {
    return impl_->bound_port;
}

void HttpStreamingServer::RegisterEndpoint(const StreamEndpointInfo& info) {
    {
        std::lock_guard<std::mutex> lock(impl_->endpoints_mutex);
        impl_->endpoints[info.token] = info;
        impl_->last_served_offsets[info.token] = 0;
        impl_->playback_stats.erase(info.token);
    }
    std::lock_guard<std::mutex> conn_lock(impl_->connections_mutex);
    impl_->endpoint_total_connections[info.token] = 0;
}

void HttpStreamingServer::UnregisterEndpoint(const std::string& token) {
    {
        std::lock_guard<std::mutex> lock(impl_->endpoints_mutex);
        impl_->endpoints.erase(token);
        impl_->last_served_offsets.erase(token);
        impl_->playback_stats.erase(token);
    }
    std::lock_guard<std::mutex> conn_lock(impl_->connections_mutex);
    impl_->endpoint_total_connections.erase(token);
}

void HttpStreamingServer::UpdateEndpointMetadata(
    const std::string& token,
    const std::string& title,
    const std::string& artist,
    std::shared_ptr<AudioStreamBuffer> new_buffer) {
    std::lock_guard<std::mutex> lock(impl_->endpoints_mutex);
    auto it = impl_->endpoints.find(token);
    if (it != impl_->endpoints.end()) {
        it->second.title = title;
        it->second.artist = artist;
        if (new_buffer) {
            it->second.buffer = std::move(new_buffer);
        }
    }
}

void HttpStreamingServer::UpdateEndpointStationName(const std::string& token, const std::string& station_name) {
    std::lock_guard<std::mutex> lock(impl_->endpoints_mutex);
    auto it = impl_->endpoints.find(token);
    if (it != impl_->endpoints.end()) {
        it->second.station_name = station_name;
    }
}

std::uint64_t HttpStreamingServer::GetEndpointServedOffset(const std::string& token) const {
    std::lock_guard<std::mutex> lock(impl_->endpoints_mutex);
    auto it = impl_->last_served_offsets.find(token);
    return it != impl_->last_served_offsets.end() ? it->second : 0;
}

std::uint64_t HttpStreamingServer::GetEndpointPlayedOffset(const std::string& token) const {
    std::lock_guard<std::mutex> lock(impl_->endpoints_mutex);
    auto it = impl_->playback_stats.find(token);
    if (it == impl_->playback_stats.end()) {
        return 0;
    }
    const auto& st = it->second;
    if (st.first_send_time == std::chrono::steady_clock::time_point{}) {
        return st.initial_offset;
    }
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - st.first_send_time).count();
    if (elapsed_ms <= 0) {
        return st.initial_offset;
    }
    std::uint64_t bytes_per_sec = static_cast<std::uint64_t>(std::max<std::uint32_t>(1, st.bitrate_kbps)) * 125ULL;
    std::uint64_t played_bytes = (static_cast<std::uint64_t>(elapsed_ms) * bytes_per_sec) / 1000ULL;
    std::uint64_t estimated = st.initial_offset + played_bytes;
    return std::min<std::uint64_t>(st.served_offset, estimated);
}

std::size_t HttpStreamingServer::GetActiveConnectionCount(const std::string& token) const {
    std::lock_guard<std::mutex> lock(impl_->connections_mutex);
    auto it = impl_->endpoint_connections.find(token);
    return it != impl_->endpoint_connections.end() ? it->second : 0;
}

std::uint64_t HttpStreamingServer::GetEndpointTotalConnectionCount(const std::string& token) const {
    std::lock_guard<std::mutex> lock(impl_->connections_mutex);
    auto it = impl_->endpoint_total_connections.find(token);
    return it != impl_->endpoint_total_connections.end() ? it->second : 0;
}

} // namespace sampaudiostream
