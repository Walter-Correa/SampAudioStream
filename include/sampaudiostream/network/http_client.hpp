#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

namespace sampaudiostream {

using HttpHeaderMap = std::unordered_map<std::string, std::string>;

struct HttpResponse {
    int status_code{0};
    std::string body;
    std::string content_type;
    std::string error;
    HttpHeaderMap headers;

    bool Ok() const { return status_code >= 200 && status_code < 300 && error.empty(); }
};

struct HttpRequestDiagnostics {
    bool redirected{false};
    std::string final_host;
    std::string error_body_snippet;
    bool connection_reused_known{false};
    bool connection_reused{false};
    bool had_cookie_header{false};
    std::string sent_headers_summary;
    bool transient_transport_error{false};
};

struct HttpAbProbeResult {
    int shared_no_headers_status{0};
    std::string shared_no_headers_error;
    int fresh_same_headers_status{0};
    std::string fresh_same_headers_error;
};

struct HttpSecurityPolicy {
    bool allow_private_network_urls{false};
    std::string bind_ip{"0.0.0.0"};
    std::string public_ip;
    std::uint16_t server_http_port{0};
};

class HttpClient {
public:
    using HeaderMap = HttpHeaderMap;
    using ChunkCallback = std::function<bool(const std::uint8_t* data, std::size_t size)>;
    using HeaderCallback = std::function<void(int status_code, const std::string& content_type, std::int64_t content_length, const HeaderMap& response_headers)>;

    static std::string FindHeaderIgnoreCase(const HeaderMap& headers, const std::string& name);

    static void SetSecurityPolicy(const HttpSecurityPolicy& policy);
    static HttpSecurityPolicy GetSecurityPolicy();
    static bool ValidateUrlSyntaxAndLiteralPolicy(const std::string& url, std::string& out_error);
    static bool ValidateRemoteUrl(const std::string& url, std::string& out_error);

    static HttpResponse Get(const std::string& url, const HeaderMap& headers = {}, int timeout_ms = 15000, const std::atomic<bool>* cancel_flag = nullptr);
    static HttpResponse Head(const std::string& url, const HeaderMap& headers = {}, int timeout_ms = 8000, const std::atomic<bool>* cancel_flag = nullptr);

    static bool DownloadToFile(
        const std::string& url,
        const std::string& dest_path,
        std::string& out_error,
        const HeaderMap& headers = {},
        int timeout_ms = 120000,
        const std::atomic<bool>* cancel_flag = nullptr);

    static bool StreamGet(
        const std::string& url,
        const HeaderMap& headers,
        const ChunkCallback& on_chunk,
        const std::atomic<bool>& cancel_flag,
        std::string& out_error,
        const HeaderCallback& on_headers = nullptr,
        int connect_timeout_ms = 15000,
        HttpRequestDiagnostics* out_diag = nullptr,
        bool fresh_session = false);

    static HttpAbProbeResult ProbeUrlSharedVsFreshForDebug(
        const std::string& url,
        const HeaderMap& headers,
        const std::atomic<bool>* cancel_flag = nullptr,
        int timeout_ms = 5000);

    static std::string DetectPublicIPv4();
    static bool CheckRuntimeSupport(std::string& out_error);
    static bool IsTransientCurlError(int code);
    static bool IsTransientWinHttpError(unsigned long code);
    static void Initialize();
    static void PollCancellations();
    static void Shutdown();
    static void SetWinHttpBetweenCallsHookForTesting(std::function<void()> hook);
    static std::uint64_t GetWinHttpReadDataCallCountForTesting();
    static std::uint64_t GetWinHttpCloserThreadCountForTesting();
    static std::uint64_t GetWinHttpFreshSessionCountForTesting();
    static void ResetWinHttpSessionCountersForTesting();
    static void FlushCancellationsForTesting();
    static std::string BuildLibcurlInstallHintFromOsRelease(const std::string& os_release_content, bool dpkg_has_i386 = false);
};

} // namespace sampaudiostream
