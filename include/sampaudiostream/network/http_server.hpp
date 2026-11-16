#pragma once

#include "sampaudiostream/audio/ring_buffer.hpp"
#include "sampaudiostream/core/types.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace sampaudiostream {

struct StreamEndpointInfo {
    std::string token;
    std::string title;
    std::string artist;
    std::uint32_t bitrate_kbps{128};
    bool is_live_channel{false};
    std::size_t live_burst_bytes{48 * 1024};
    std::shared_ptr<AudioStreamBuffer> buffer;
};

class HttpStreamingServer {
public:
    HttpStreamingServer();
    ~HttpStreamingServer();

    HttpStreamingServer(const HttpStreamingServer&) = delete;
    HttpStreamingServer& operator=(const HttpStreamingServer&) = delete;

    bool Start(const std::string& bind_ip, std::uint16_t port, std::string& out_error);
    void Stop();

    bool IsRunning() const;
    std::uint16_t BoundPort() const;

    void RegisterEndpoint(const StreamEndpointInfo& info);
    void UnregisterEndpoint(const std::string& token);
    void UpdateEndpointMetadata(const std::string& token, const std::string& title, const std::string& artist, std::shared_ptr<AudioStreamBuffer> new_buffer = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace sampaudiostream
