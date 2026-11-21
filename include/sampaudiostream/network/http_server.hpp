#pragma once

#include "sampaudiostream/audio/ring_buffer.hpp"
#include "sampaudiostream/core/types.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
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
    std::string station_name;
    std::string live_title;
    bool send_icy_headers{true};
    bool is_channel{false};
    // Session icy-name when no station name is known (platform such as "YouTube", or the audio file label).
    std::string source_label;
};

// Complete in-memory file served once per request at /f/<token> (sound effects from audio.sound_dir).
struct FileEndpointInfo {
    std::string token;
    std::shared_ptr<const std::string> data;
    std::string content_type;
};

struct FileLimitPlan {
    bool raise{false};
    std::uint64_t new_soft{0};
    bool warn{false};
};

// Total simultaneous HTTP audio connections derived from the server's player slots.
std::size_t ComputeHttpConnectionLimit(std::int32_t max_players);

// Decides whether to raise the RLIMIT_NOFILE soft limit up to needed without lowering or raising to full hard cap.
FileLimitPlan PlanFileDescriptorLimit(std::uint64_t soft, std::uint64_t hard, std::uint64_t needed);

class HttpStreamingServer {
public:
    HttpStreamingServer();
    ~HttpStreamingServer();

    HttpStreamingServer(const HttpStreamingServer&) = delete;
    HttpStreamingServer& operator=(const HttpStreamingServer&) = delete;

    void SetConnectionLimits(
        std::size_t max_total_connections,
        std::size_t max_connections_per_ip,
        std::int32_t player_slots = 0);
    bool Start(const std::string& bind_ip, std::uint16_t port, std::string& out_error, bool log_listen = true);
    void Stop();

    bool IsRunning() const;
    std::uint16_t BoundPort() const;

    void RegisterEndpoint(const StreamEndpointInfo& info);
    void UnregisterEndpoint(const std::string& token);
    void UpdateEndpointMetadata(const std::string& token, const std::string& title, const std::string& artist, std::shared_ptr<AudioStreamBuffer> new_buffer = nullptr);
    void UpdateEndpointTitle(const std::string& token, const std::string& title);
    void UpdateEndpointLiveTitle(const std::string& token, const std::string& live_title);
    void UpdateEndpointStationName(const std::string& token, const std::string& station_name);
    std::uint64_t GetEndpointServedOffset(const std::string& token) const;
    std::uint64_t GetEndpointPlayedOffset(const std::string& token) const;
    std::size_t GetActiveConnectionCount(const std::string& token) const;
    std::uint64_t GetEndpointTotalConnectionCount(const std::string& token) const;

    void RegisterFileEndpoint(const FileEndpointInfo& info);
    void UnregisterFileEndpoint(const std::string& token);
    // Steady-clock time of the first GET received for a file endpoint; false while it was not requested.
    bool GetFileEndpointFirstRequestTime(const std::string& token, std::chrono::steady_clock::time_point& out_time) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace sampaudiostream
