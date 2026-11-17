#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

namespace sampaudiostream {

struct HttpResponse {
    int status_code{0};
    std::string body;
    std::string content_type;
    std::string error;

    bool Ok() const { return status_code >= 200 && status_code < 300 && error.empty(); }
};

struct HttpSecurityPolicy {
    bool allow_private_network_urls{false};
    std::string bind_ip{"0.0.0.0"};
    std::string public_ip;
    std::uint16_t server_http_port{0};
};

class HttpClient {
public:
    using HeaderMap = std::unordered_map<std::string, std::string>;
    using ChunkCallback = std::function<bool(const std::uint8_t* data, std::size_t size)>;
    using HeaderCallback = std::function<void(int status_code, const std::string& content_type, std::int64_t content_length)>;

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
        int connect_timeout_ms = 15000);

    static std::string DetectPublicIPv4();
    static bool CheckRuntimeSupport(std::string& out_error);
    static void PollCancellations();
    static std::string BuildLibcurlInstallHintFromOsRelease(const std::string& os_release_content, bool dpkg_has_i386 = false);
};

} // namespace sampaudiostream
