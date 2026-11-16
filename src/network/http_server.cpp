#include "sampaudiostream/network/http_server.hpp"

#include "sampaudiostream/core/logger.hpp"

#include <asio.hpp>
#include <array>
#include <chrono>
#include <sstream>
#include <thread>
#include <vector>

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

class ClientConnection : public std::enable_shared_from_this<ClientConnection> {
public:
    using EndpointLookupFn = std::function<bool(const std::string&, StreamEndpointInfo&)>;

    ClientConnection(asio::ip::tcp::socket socket, EndpointLookupFn lookup_fn)
        : socket_(std::move(socket)),
          timer_(socket_.get_executor()),
          lookup_fn_(std::move(lookup_fn)) {}

    void Start() {
        auto self = shared_from_this();
        asio::async_read_until(
            socket_,
            request_buf_,
            "\r\n\r\n",
            [self](std::error_code ec, std::size_t) {
                if (!ec) {
                    self->HandleRequest();
                }
            });
    }

private:
    void HandleRequest() {
        std::istream is(&request_buf_);
        std::string method, target, version;
        is >> method >> target >> version;

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

        if (endpoint_.is_live_channel) {
            std::uint64_t written = endpoint_.buffer->TotalWritten();
            std::uint64_t earliest = endpoint_.buffer->EarliestAvailableOffset();
            if (written > endpoint_.live_burst_bytes) {
                read_offset_ = std::max(earliest, written - endpoint_.live_burst_bytes);
            } else {
                read_offset_ = earliest;
            }
        } else {
            read_offset_ = 0;
        }

        std::ostringstream oss;
        oss << "HTTP/1.1 200 OK\r\n"
            << "Content-Type: audio/mpeg\r\n"
            << "Accept-Ranges: none\r\n"
            << "Cache-Control: no-cache, no-store\r\n"
            << "Connection: close\r\n"
            << "icy-br: " << endpoint_.bitrate_kbps << "\r\n";

        if (!endpoint_.title.empty()) {
            oss << "icy-name: " << SanitizeHeaderValue(endpoint_.title) << "\r\n";
        }
        oss << "\r\n";

        header_response_ = oss.str();
        auto self = shared_from_this();
        asio::async_write(
            socket_,
            asio::buffer(header_response_),
            [self](std::error_code ec, std::size_t) {
                if (ec || self->is_head_) {
                    self->CloseSocket();
                    return;
                }
                self->PumpNextChunk();
            });
    }

    void PumpNextChunk() {
        if (!endpoint_.buffer) {
            CloseSocket();
            return;
        }

        std::size_t n = endpoint_.buffer->Read(read_offset_, write_buf_.data(), write_buf_.size(), 0);
        if (n > 0) {
            auto self = shared_from_this();
            asio::async_write(
                socket_,
                asio::buffer(write_buf_.data(), n),
                [self](std::error_code ec, std::size_t) {
                    if (ec) {
                        self->CloseSocket();
                        return;
                    }
                    self->PumpNextChunk();
                });
            return;
        }

        if (endpoint_.buffer->IsClosed() ||
            (endpoint_.buffer->IsEof() && read_offset_ >= endpoint_.buffer->TotalWritten())) {
            CloseSocket();
            return;
        }

        auto self = shared_from_this();
        timer_.expires_after(std::chrono::milliseconds(15));
        timer_.async_wait([self](std::error_code ec) {
            if (!ec) {
                self->PumpNextChunk();
            }
        });
    }

    void SendErrorResponse(int code, const std::string& reason) {
        std::ostringstream oss;
        oss << "HTTP/1.1 " << code << " " << reason << "\r\n"
            << "Content-Length: 0\r\n"
            << "Connection: close\r\n\r\n";
        header_response_ = oss.str();
        auto self = shared_from_this();
        asio::async_write(
            socket_,
            asio::buffer(header_response_),
            [self](std::error_code, std::size_t) {
                self->CloseSocket();
            });
    }

    void CloseSocket() {
        std::error_code ec;
        try {
            timer_.cancel();
        } catch (...) {
        }
        socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        socket_.close(ec);
    }

    asio::ip::tcp::socket socket_;
    asio::steady_timer timer_;
    asio::streambuf request_buf_;
    EndpointLookupFn lookup_fn_;
    StreamEndpointInfo endpoint_;
    std::string header_response_;
    std::array<std::uint8_t, 16384> write_buf_{};
    std::uint64_t read_offset_{0};
    bool is_head_{false};
};

} // namespace

struct HttpStreamingServer::Impl {
    asio::io_context io_ctx;
    std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>> work_guard;
    std::unique_ptr<asio::ip::tcp::acceptor> acceptor;
    std::vector<std::thread> io_threads;
    std::atomic<bool> running{false};
    std::uint16_t bound_port{0};

    std::mutex endpoints_mutex;
    std::unordered_map<std::string, StreamEndpointInfo> endpoints;

    bool LookupEndpoint(const std::string& token, StreamEndpointInfo& out_info) {
        std::lock_guard<std::mutex> lock(endpoints_mutex);
        auto it = endpoints.find(token);
        if (it == endpoints.end()) {
            return false;
        }
        out_info = it->second;
        return true;
    }

    void DoAccept() {
        if (!running.load() || !acceptor || !acceptor->is_open()) {
            return;
        }
        acceptor->async_accept([this](std::error_code ec, asio::ip::tcp::socket socket) {
            if (!ec && running.load()) {
                std::error_code opt_ec;
                socket.set_option(asio::ip::tcp::no_delay(true), opt_ec);
                auto conn = std::make_shared<ClientConnection>(
                    std::move(socket),
                    [this](const std::string& token, StreamEndpointInfo& info) {
                        return LookupEndpoint(token, info);
                    });
                conn->Start();
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

bool HttpStreamingServer::Start(const std::string& bind_ip, std::uint16_t port, std::string& out_error) {
    if (impl_->running.load()) {
        return true;
    }

    try {
        impl_->io_ctx.restart();
        impl_->work_guard = std::make_unique<asio::executor_work_guard<asio::io_context::executor_type>>(
            impl_->io_ctx.get_executor());

        asio::ip::address addr = bind_ip.empty()
                                     ? asio::ip::address_v4::any()
                                     : asio::ip::make_address(bind_ip);
        asio::ip::tcp::endpoint ep(addr, port);

        impl_->acceptor = std::make_unique<asio::ip::tcp::acceptor>(impl_->io_ctx);
        impl_->acceptor->open(ep.protocol());
        impl_->acceptor->set_option(asio::ip::tcp::acceptor::reuse_address(true));
        impl_->acceptor->bind(ep);
        impl_->acceptor->listen(asio::socket_base::max_listen_connections);

        impl_->bound_port = impl_->acceptor->local_endpoint().port();
        impl_->running.store(true);
        impl_->DoAccept();

        for (int i = 0; i < 2; ++i) {
            impl_->io_threads.emplace_back([this]() {
                impl_->io_ctx.run();
            });
        }

        Logger::Instance().Info("HTTP audio streaming server listening on " + addr.to_string() + ":" + std::to_string(impl_->bound_port));
        return true;
    } catch (const std::exception& ex) {
        out_error = ex.what();
        Stop();
        return false;
    }
}

void HttpStreamingServer::Stop() {
    bool expected = true;
    if (!impl_->running.compare_exchange_strong(expected, false)) {
        return;
    }

    std::error_code ec;
    if (impl_->acceptor) {
        impl_->acceptor->close(ec);
        impl_->acceptor.reset();
    }
    impl_->work_guard.reset();
    impl_->io_ctx.stop();

    for (auto& t : impl_->io_threads) {
        if (t.joinable()) {
            t.join();
        }
    }
    impl_->io_threads.clear();

    std::lock_guard<std::mutex> lock(impl_->endpoints_mutex);
    impl_->endpoints.clear();
}

bool HttpStreamingServer::IsRunning() const {
    return impl_->running.load();
}

std::uint16_t HttpStreamingServer::BoundPort() const {
    return impl_->bound_port;
}

void HttpStreamingServer::RegisterEndpoint(const StreamEndpointInfo& info) {
    std::lock_guard<std::mutex> lock(impl_->endpoints_mutex);
    impl_->endpoints[info.token] = info;
}

void HttpStreamingServer::UnregisterEndpoint(const std::string& token) {
    std::lock_guard<std::mutex> lock(impl_->endpoints_mutex);
    impl_->endpoints.erase(token);
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

} // namespace sampaudiostream
