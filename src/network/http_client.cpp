#include "sampaudiostream/network/http_client.hpp"

#include "sampaudiostream/core/logger.hpp"

#include <algorithm>
#include <array>
#include <asio.hpp>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_set>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <dlfcn.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace sampaudiostream {

bool ValidateRemoteUrlWithClass(
    const std::string& url,
    std::string& out_error,
    bool* out_is_private_or_loopback);

namespace {

std::string Trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::string ToLowerCopy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

void StoreParsedResponseHeader(HttpClient::HeaderMap* out_headers, const std::string& raw_line) {
    if (!out_headers) return;
    std::string line = raw_line;
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.pop_back();
    }
    auto colon = line.find(':');
    if (colon == std::string::npos) return;
    std::string k = ToLowerCopy(Trim(line.substr(0, colon)));
    if (k.empty()) return;
    (*out_headers)[std::move(k)] = Trim(line.substr(colon + 1));
}

std::mutex g_security_policy_mtx;
HttpSecurityPolicy g_security_policy;

bool ParseHttpUrlComponents(
    const std::string& url,
    std::string& out_scheme,
    std::string& out_host,
    std::uint16_t& out_port,
    std::string& out_path) {
    if (url.empty()) return false;
    for (unsigned char c : url) {
        if (c < 0x20 || c == 0x7F || std::isspace(c)) return false;
    }

    auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return false;
    out_scheme = ToLowerCopy(url.substr(0, scheme_end));
    if (out_scheme != "http" && out_scheme != "https") return false;

    std::size_t auth_start = scheme_end + 3;
    auto path_start = url.find_first_of("/?#", auth_start);
    std::string authority = (path_start == std::string::npos)
                                ? url.substr(auth_start)
                                : url.substr(auth_start, path_start - auth_start);
    out_path = (path_start == std::string::npos) ? "/" : url.substr(path_start);
    if (out_path.empty() || out_path[0] != '/') {
        if (out_path[0] == '?' || out_path[0] == '#') {
            out_path.insert(out_path.begin(), '/');
        }
    }

    auto at_pos = authority.rfind('@');
    if (at_pos != std::string::npos) {
        authority = authority.substr(at_pos + 1);
    }
    if (authority.empty()) return false;

    std::string port_str;
    if (authority.front() == '[') {
        auto close_bracket = authority.find(']');
        if (close_bracket == std::string::npos) return false;
        out_host = authority.substr(1, close_bracket - 1);
        if (close_bracket + 1 < authority.size()) {
            if (authority[close_bracket + 1] != ':') return false;
            port_str = authority.substr(close_bracket + 2);
        }
    } else {
        auto colon = authority.rfind(':');
        if (colon != std::string::npos) {
            out_host = authority.substr(0, colon);
            port_str = authority.substr(colon + 1);
        } else {
            out_host = authority;
        }
    }
    out_host = ToLowerCopy(out_host);
    if (out_host.empty()) return false;

    if (port_str.empty()) {
        out_port = (out_scheme == "https") ? 443 : 80;
    } else {
        long parsed = 0;
        for (unsigned char c : port_str) {
            if (!std::isdigit(c)) return false;
            parsed = parsed * 10 + (c - '0');
            if (parsed > 65535) return false;
        }
        if (parsed <= 0) return false;
        out_port = static_cast<std::uint16_t>(parsed);
    }
    return true;
}

bool IsRestrictedIpv4(const asio::ip::address_v4& v4) {
    std::uint32_t ip = v4.to_uint();
    if ((ip & 0xFF000000u) == 0x00000000u) return true; // 0.0.0.0/8 ("this" network)
    if ((ip & 0xFF000000u) == 0x0A000000u) return true; // 10.0.0.0/8 (RFC 1918)
    if ((ip & 0xFFC00000u) == 0x64400000u) return true; // 100.64.0.0/10 (CGNAT)
    if ((ip & 0xFF000000u) == 0x7F000000u) return true; // 127.0.0.0/8 (Loopback)
    if ((ip & 0xFFFF0000u) == 0xA9FE0000u) return true; // 169.254.0.0/16 (Link-local / cloud metadata)
    if ((ip & 0xFFF00000u) == 0xAC100000u) return true; // 172.16.0.0/12 (RFC 1918)
    if ((ip & 0xFFFFFF00u) == 0xC0000000u) return true; // 192.0.0.0/24 (IETF protocol assignments)
    if ((ip & 0xFFFF0000u) == 0xC0A80000u) return true; // 192.168.0.0/16 (RFC 1918)
    if ((ip & 0xE0000000u) == 0xE0000000u) return true; // 224.0.0.0/4 multicast + 240.0.0.0/4 reserved
    return false;
}

asio::ip::address UnwrapAddress(const asio::ip::address& addr) {
    if (addr.is_v6() && addr.to_v6().is_v4_mapped()) {
        return asio::ip::make_address_v4(asio::ip::v4_mapped, addr.to_v6());
    }
    return addr;
}

bool IsRestrictedIpAddress(const asio::ip::address& raw_addr) {
    asio::ip::address addr = UnwrapAddress(raw_addr);
    if (addr.is_v4()) {
        return IsRestrictedIpv4(addr.to_v4());
    }
    if (addr.is_v6()) {
        const auto v6 = addr.to_v6();
        if (v6.is_loopback() || v6.is_unspecified() || v6.is_multicast() ||
            v6.is_link_local() || v6.is_site_local()) {
            return true;
        }
        auto bytes = v6.to_bytes();
        if ((bytes[0] & 0xFEu) == 0xFCu) return true; // fc00::/7 (Unique Local IPv6)
        if (bytes[0] == 0xFEu && (bytes[1] & 0xC0u) == 0x80u) return true; // fe80::/10
    }
    return false;
}

bool IsSelfPluginEndpoint(
    const asio::ip::address& raw_addr,
    const std::string& raw_host,
    std::uint16_t port,
    const HttpSecurityPolicy& policy) {
    if (policy.server_http_port == 0 || port != policy.server_http_port) {
        return false;
    }
    asio::ip::address addr = UnwrapAddress(raw_addr);
    if (addr.is_loopback() || addr.is_unspecified()) {
        return true;
    }
    if (raw_host == "localhost" || raw_host == "0.0.0.0" || raw_host == "::" || raw_host == "::1") {
        return true;
    }
    std::error_code ec;
    if (!policy.bind_ip.empty() && policy.bind_ip != "0.0.0.0" && policy.bind_ip != "::") {
        auto bind_addr = UnwrapAddress(asio::ip::make_address(policy.bind_ip, ec));
        if (!ec && addr == bind_addr) return true;
    }
    if (!policy.public_ip.empty()) {
        ec.clear();
        auto pub_addr = UnwrapAddress(asio::ip::make_address(policy.public_ip, ec));
        if (!ec && addr == pub_addr) return true;
    }
    return false;
}

bool IsAddressAllowed(
    const asio::ip::address& addr,
    const std::string& raw_host,
    std::uint16_t port,
    const HttpSecurityPolicy& policy) {
    if (IsSelfPluginEndpoint(addr, raw_host, port, policy)) {
        return false;
    }
    if (!policy.allow_private_network_urls && IsRestrictedIpAddress(addr)) {
        return false;
    }
    return true;
}

std::string DescribeBlockedAddressReason(
    const asio::ip::address& raw_addr,
    const std::string& raw_host,
    std::uint16_t port,
    const HttpSecurityPolicy& policy) {
    if (IsSelfPluginEndpoint(raw_addr, raw_host, port, policy)) {
        return "self-server endpoint";
    }
    asio::ip::address addr = UnwrapAddress(raw_addr);
    if (addr.is_v4()) {
        std::uint32_t ip = addr.to_v4().to_uint();
        if ((ip & 0xFFC00000u) == 0x64400000u ||
            (ip & 0xFF000000u) == 0x0A000000u ||
            (ip & 0xFFF00000u) == 0xAC100000u ||
            (ip & 0xFFFF0000u) == 0xC0A80000u) {
            return "private/CGNAT range";
        }
        if ((ip & 0xFF000000u) == 0x7F000000u) return "loopback range";
        if ((ip & 0xFFFF0000u) == 0xA9FE0000u) return "link-local/metadata range";
        if ((ip & 0xFF000000u) == 0x00000000u || (ip & 0xFFFFFF00u) == 0xC0000000u ||
            (ip & 0xE0000000u) == 0xE0000000u) {
            return "reserved/multicast range";
        }
    } else if (addr.is_v6()) {
        const auto v6 = addr.to_v6();
        if (v6.is_loopback()) return "IPv6 loopback range";
        if (v6.is_link_local()) return "IPv6 link-local range";
        return "restricted IPv6 range";
    }
    return "restricted network policy";
}

void LogBlockedHostOnce(
    const std::string& host,
    const asio::ip::address* resolved_addr,
    const std::string& reason) {
    static std::mutex g_logged_mtx;
    static std::unordered_set<std::string> g_logged_hosts;
    std::string key = ToLowerCopy(host);
    {
        std::lock_guard<std::mutex> lk(g_logged_mtx);
        if (!g_logged_hosts.insert(key).second) {
            return;
        }
    }
    if (resolved_addr) {
        Logger::Instance().Warn(
            "Blocked request to " + host + " (resolved " + UnwrapAddress(*resolved_addr).to_string() +
            ", " + reason + ")");
    } else {
        Logger::Instance().Warn(
            "Blocked request to " + host + " (" + reason + ")");
    }
}

#ifndef _WIN32
bool SockaddrToAsioAddress(const struct sockaddr* sa, asio::ip::address& out_addr) {
    if (!sa) return false;
    if (sa->sa_family == AF_INET) {
        const auto* sin = reinterpret_cast<const struct sockaddr_in*>(sa);
        out_addr = asio::ip::address_v4(ntohl(sin->sin_addr.s_addr));
        return true;
    }
    if (sa->sa_family == AF_INET6) {
        const auto* sin6 = reinterpret_cast<const struct sockaddr_in6*>(sa);
        asio::ip::address_v6::bytes_type bytes{};
        std::memcpy(bytes.data(), sin6->sin6_addr.s6_addr, 16);
        out_addr = asio::ip::address_v6(bytes, sin6->sin6_scope_id);
        return true;
    }
    return false;
}
#endif

std::string ResolveRedirectLocation(const std::string& base_url, const std::string& location) {
    std::string loc = Trim(location);
    if (loc.empty()) return "";
    if (loc.find("://") != std::string::npos) {
        return loc;
    }
    auto scheme_end = base_url.find("://");
    if (scheme_end == std::string::npos) return "";
    std::string scheme = ToLowerCopy(base_url.substr(0, scheme_end));
    if (loc.rfind("//", 0) == 0) {
        return scheme + ":" + loc;
    }
    auto path_start = base_url.find('/', scheme_end + 3);
    std::string origin = (path_start == std::string::npos) ? base_url : base_url.substr(0, path_start);
    if (loc.front() == '/') {
        return origin + loc;
    }
    auto last_slash = base_url.rfind('/');
    if (last_slash == std::string::npos || last_slash < scheme_end + 3) {
        return origin + "/" + loc;
    }
    return base_url.substr(0, last_slash + 1) + loc;
}

#ifdef _WIN32

struct WinHttpHandle {
    HINTERNET h{nullptr};
    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET handle) : h(handle) {}
    ~WinHttpHandle() {
        if (h) WinHttpCloseHandle(h);
    }
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
    operator HINTERNET() const { return h; }
    bool valid() const { return h != nullptr; }
};

struct CancelableWinHttpRequest {
    std::mutex mtx;
    HINTERNET h{nullptr};
    bool closed{false};
    bool in_call{false};
    std::atomic<bool> saw_connect_to_server{false};

    explicit CancelableWinHttpRequest(HINTERNET handle) : h(handle) {}
    ~CancelableWinHttpRequest() { Close(); }

    HINTERNET BeginCall() {
        std::lock_guard<std::mutex> lock(mtx);
        if (closed || !h) {
            return nullptr;
        }
        in_call = true;
        return h;
    }

    bool EndCall() {
        std::lock_guard<std::mutex> lock(mtx);
        in_call = false;
        return !closed && h != nullptr;
    }

    bool IsClosed() {
        std::lock_guard<std::mutex> lock(mtx);
        return closed || h == nullptr;
    }

    void Close() {
        HINTERNET tmp = nullptr;
        {
            std::lock_guard<std::mutex> lock(mtx);
            if (closed) {
                return;
            }
            closed = true;
            tmp = h;
            h = nullptr;
        }
        if (tmp) {
            WinHttpSetStatusCallback(tmp, nullptr, 0, 0);
            WinHttpCloseHandle(tmp);
        }
    }
};

struct ActiveWinHttpEntry {
    std::weak_ptr<CancelableWinHttpRequest> req;
    const std::atomic<bool>* cancel_flag{nullptr};
};

std::mutex g_active_winhttp_mtx;
std::vector<ActiveWinHttpEntry> g_active_winhttp;

std::mutex g_winhttp_hook_mtx;
std::function<void()> g_winhttp_between_calls_hook;
std::atomic<std::uint64_t> g_winhttp_read_data_calls{0};
std::atomic<std::uint64_t> g_winhttp_fresh_session_count{0};

struct WinHttpCloserState {
    std::mutex mtx;
    std::condition_variable cv;
    std::condition_variable done_cv;
    std::vector<std::shared_ptr<CancelableWinHttpRequest>> queue;
    std::thread worker;
    bool stopping{false};
    bool running{false};
    bool shut_down{false};
    std::size_t closing_in_flight{0};
    std::atomic<std::uint64_t> threads_created{0};

    ~WinHttpCloserState() {
        // Static teardown may run under the Windows DLL loader lock (DLL_PROCESS_DETACH).
        // Never join() here; detach any lingering worker so teardown cannot deadlock.
        std::lock_guard<std::mutex> lock(mtx);
        shut_down = true;
        stopping = true;
        cv.notify_all();
        if (worker.joinable()) {
            worker.detach();
        }
        running = false;
    }

    void Reenable() {
        std::lock_guard<std::mutex> lock(mtx);
        shut_down = false;
    }

    void EnsureStartedLocked() {
        if (worker.joinable()) {
            if (!running) {
                worker.join();
            } else {
                return;
            }
        }
        stopping = false;
        running = true;
        threads_created.fetch_add(1, std::memory_order_relaxed);
        worker = std::thread([this]() { Run(); });
    }

    void Enqueue(std::vector<std::shared_ptr<CancelableWinHttpRequest>>& items) {
        if (items.empty()) {
            return;
        }
        std::vector<std::shared_ptr<CancelableWinHttpRequest>> inline_close;
        {
            std::lock_guard<std::mutex> lock(mtx);
            if (shut_down) {
                inline_close.swap(items);
            } else {
                EnsureStartedLocked();
                for (auto& item : items) {
                    queue.push_back(std::move(item));
                }
                cv.notify_one();
                return;
            }
        }
        for (auto& req : inline_close) {
            if (req) {
                req->Close();
            }
        }
    }

    void Run() {
        while (true) {
            std::vector<std::shared_ptr<CancelableWinHttpRequest>> batch;
            {
                std::unique_lock<std::mutex> lock(mtx);
                cv.wait(lock, [&]() { return stopping || !queue.empty(); });
                if (queue.empty() && stopping) {
                    running = false;
                    done_cv.notify_all();
                    return;
                }
                batch.swap(queue);
                closing_in_flight = batch.size();
            }
            for (auto& req : batch) {
                if (req) {
                    req->Close();
                }
            }
            {
                std::lock_guard<std::mutex> lock(mtx);
                closing_in_flight = 0;
                done_cv.notify_all();
            }
        }
    }

    bool WaitUntilIdle(int timeout_ms) {
        std::unique_lock<std::mutex> lock(mtx);
        return done_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&]() {
            return queue.empty() && closing_in_flight == 0;
        });
    }

    void Shutdown(int deadline_ms = 3000) {
        std::thread to_join;
        {
            std::unique_lock<std::mutex> lock(mtx);
            shut_down = true;
            if (!worker.joinable()) {
                stopping = false;
                running = false;
                return;
            }
            stopping = true;
            cv.notify_all();
            bool finished = done_cv.wait_for(lock, std::chrono::milliseconds(deadline_ms), [&]() {
                return !running;
            });
            if (finished) {
                to_join = std::move(worker);
            } else {
                worker.detach();
                running = false;
                stopping = false;
                return;
            }
        }
        if (to_join.joinable()) {
            to_join.join();
        }
    }
};

WinHttpCloserState& GetWinHttpCloserState() {
    static WinHttpCloserState state;
    return state;
}

template <typename Fn>
bool RunWinHttpCall(
    const std::shared_ptr<CancelableWinHttpRequest>& guard,
    const std::atomic<bool>* cancel_flag,
    DWORD& out_err,
    bool& out_cancelled,
    Fn&& fn) {
    out_err = 0;
    out_cancelled = false;
    if (cancel_flag && cancel_flag->load()) {
        out_cancelled = true;
        return false;
    }
    HINTERNET req = guard->BeginCall();
    if (!req) {
        out_cancelled = true;
        return false;
    }
    BOOL ok = fn(req);
    DWORD err = ok ? ERROR_SUCCESS : GetLastError();
    bool still_open = guard->EndCall();
    if (!still_open || (cancel_flag && cancel_flag->load())) {
        out_cancelled = true;
        return false;
    }
    out_err = err;
    return ok != FALSE;
}

void RegisterCancelableWinHttpRequest(const std::shared_ptr<CancelableWinHttpRequest>& req, const std::atomic<bool>* cancel_flag) {
    if (!req || !cancel_flag) return;
    std::lock_guard<std::mutex> lock(g_active_winhttp_mtx);
    g_active_winhttp.erase(
        std::remove_if(g_active_winhttp.begin(), g_active_winhttp.end(), [](const ActiveWinHttpEntry& e) {
            return e.req.expired();
        }),
        g_active_winhttp.end());
    g_active_winhttp.push_back({req, cancel_flag});
}

void CALLBACK OnWinHttpDiagCallback(
    HINTERNET,
    DWORD_PTR dwContext,
    DWORD dwInternetStatus,
    LPVOID,
    DWORD) {
    if (dwContext == 0) {
        return;
    }
    if (dwInternetStatus == WINHTTP_CALLBACK_STATUS_CONNECTING_TO_SERVER ||
        dwInternetStatus == WINHTTP_CALLBACK_STATUS_CONNECTED_TO_SERVER) {
        auto* req = reinterpret_cast<CancelableWinHttpRequest*>(dwContext);
        req->saw_connect_to_server.store(true, std::memory_order_relaxed);
    }
}

HINTERNET CreateWinHttpSession(bool bypass_proxy) {
    HINTERNET session = nullptr;
    if (bypass_proxy) {
        session = WinHttpOpen(
            L"SampAudioStream/1.0",
            WINHTTP_ACCESS_TYPE_NO_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0);
    } else {
        session = WinHttpOpen(
            L"SampAudioStream/1.0",
            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0);
        if (!session) {
            session = WinHttpOpen(
                L"SampAudioStream/1.0",
                WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                WINHTTP_NO_PROXY_NAME,
                WINHTTP_NO_PROXY_BYPASS,
                0);
        }
    }
    return session;
}

HINTERNET GetSharedWinHttpSession(bool bypass_proxy) {
    static std::mutex session_mtx;
    static HINTERNET proxy_session = nullptr;
    static HINTERNET no_proxy_session = nullptr;
    std::lock_guard<std::mutex> lock(session_mtx);
    if (bypass_proxy) {
        if (!no_proxy_session) {
            no_proxy_session = CreateWinHttpSession(true);
        }
        return no_proxy_session;
    }
    if (!proxy_session) {
        proxy_session = CreateWinHttpSession(false);
    }
    return proxy_session;
}

std::wstring Utf8ToWide(const std::string& str) {
    if (str.empty()) return L"";
    int needed = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), nullptr, 0);
    if (needed <= 0) return L"";
    std::wstring res(needed, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), res.data(), needed);
    return res;
}

std::string WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int needed = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return "";
    std::string res(needed, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), res.data(), needed, nullptr, nullptr);
    return res;
}

std::string QuerySentWinHttpHeadersSummary(HINTERNET req, bool& out_had_cookie) {
    out_had_cookie = false;
    DWORD needed_bytes = 0;
    WinHttpQueryHeaders(
        req,
        WINHTTP_QUERY_RAW_HEADERS_CRLF | WINHTTP_QUERY_FLAG_REQUEST_HEADERS,
        WINHTTP_HEADER_NAME_BY_INDEX,
        nullptr,
        &needed_bytes,
        WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || needed_bytes == 0 || needed_bytes > 16384) {
        return "";
    }
    std::wstring raw_buf(needed_bytes / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(
            req,
            WINHTTP_QUERY_RAW_HEADERS_CRLF | WINHTTP_QUERY_FLAG_REQUEST_HEADERS,
            WINHTTP_HEADER_NAME_BY_INDEX,
            raw_buf.data(),
            &needed_bytes,
            WINHTTP_NO_HEADER_INDEX)) {
        return "";
    }
    raw_buf.resize(needed_bytes / sizeof(wchar_t));
    while (!raw_buf.empty() && raw_buf.back() == L'\0') {
        raw_buf.pop_back();
    }

    const std::string raw_utf8 = WideToUtf8(raw_buf);
    std::istringstream iss(raw_utf8);
    std::string line;
    std::string summary;
    bool skipped_request_line = false;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        line = Trim(line);
        if (line.empty()) {
            continue;
        }
        if (!skipped_request_line) {
            skipped_request_line = true;
            if (line.find(':') == std::string::npos || line.rfind("HTTP/", 0) != std::string::npos ||
                line.find(" HTTP/") != std::string::npos) {
                continue;
            }
        }
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        std::string name = Trim(line.substr(0, colon));
        std::string val = Trim(line.substr(colon + 1));
        if (name.empty()) {
            continue;
        }
        std::string entry;
        const std::string lower = ToLowerCopy(name);
        if (lower == "user-agent") {
            std::string clean_val;
            clean_val.reserve(std::min<std::size_t>(val.size(), 120));
            for (unsigned char ch : val) {
                clean_val.push_back((ch < 0x20 || ch == 0x7f) ? ' ' : static_cast<char>(ch));
                if (clean_val.size() >= 120) break;
            }
            entry = name + "=\"" + clean_val + "\"";
        } else if (lower == "cookie") {
            out_had_cookie = true;
            entry = name + "=<present>";
        } else {
            entry = name;
        }
        if (!summary.empty()) {
            summary += ", ";
        }
        if (summary.size() + entry.size() > 512) {
            summary += "...";
            break;
        }
        summary += entry;
    }
    return summary;
}

std::string FormatWinError(DWORD err) {
    return "WinHTTP error code " + std::to_string(err);
}

// Minimal raw TCP HTTP/ICY fallback for legacy Shoutcast v1 ("ICY 200 OK") servers that WinHTTP rejects
bool PerformRawHttpIcyRequest(
    const std::string& method,
    const std::string& url,
    const HttpClient::HeaderMap& headers,
    int timeout_ms,
    int& out_status,
    std::string& out_content_type,
    std::int64_t& out_content_length,
    const HttpClient::ChunkCallback& on_chunk,
    const std::atomic<bool>* cancel_flag,
    std::string& out_error,
    int redirect_depth = 0,
    HttpClient::HeaderMap* out_response_headers = nullptr) {
    if (redirect_depth > 5) {
        out_error = "Too many redirects";
        return false;
    }

    std::string scheme, host, path;
    std::uint16_t port_num = 80;
    if (!ParseHttpUrlComponents(url, scheme, host, port_num, path) || scheme != "http") {
        out_error = "URL not allowed";
        return false;
    }

    try {
        asio::io_context ioc;
        asio::ip::tcp::resolver resolver(ioc);
        asio::ip::tcp::socket sock(ioc);

        std::error_code ec;
        std::string port_str = std::to_string(port_num);
        auto endpoints = resolver.resolve(host, port_str, ec);
        if (ec || endpoints.empty()) {
            out_error = "DNS resolution failed for " + host;
            return false;
        }

        HttpSecurityPolicy policy = HttpClient::GetSecurityPolicy();
        for (const auto& ep : endpoints) {
            auto addr = ep.endpoint().address();
            if (!IsAddressAllowed(addr, host, port_num, policy)) {
                LogBlockedHostOnce(host, &addr, DescribeBlockedAddressReason(addr, host, port_num, policy));
                out_error = "URL not allowed";
                return false;
            }
        }

        asio::connect(sock, endpoints, ec);
        if (ec) {
            out_error = "TCP connect failed: " + ec.message();
            return false;
        }

        DWORD tv_ms = static_cast<DWORD>(std::clamp(timeout_ms, 1000, 30000));
        ::setsockopt(sock.native_handle(), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv_ms), sizeof(tv_ms));
        ::setsockopt(sock.native_handle(), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv_ms), sizeof(tv_ms));

        std::ostringstream req;
        req << method << " " << path << " HTTP/1.0\r\n"
            << "Host: " << host << "\r\n"
            << "User-Agent: SampAudioStream/1.0\r\n"
            << "Connection: close\r\n";
        for (const auto& [k, v] : headers) {
            req << k << ": " << v << "\r\n";
        }
        req << "\r\n";
        std::string req_str = req.str();
        asio::write(sock, asio::buffer(req_str), ec);
        if (ec) {
            out_error = "TCP write failed: " + ec.message();
            return false;
        }

        std::string hdr_buf;
        hdr_buf.reserve(4096);
        std::array<char, 4096> tmp{};
        std::size_t header_end = std::string::npos;

        while (header_end == std::string::npos && hdr_buf.size() < 16384) {
            if (cancel_flag && cancel_flag->load()) {
                out_error = "Cancelled";
                return false;
            }
            std::size_t n = sock.read_some(asio::buffer(tmp), ec);
            if (n > 0) {
                hdr_buf.append(tmp.data(), n);
                header_end = hdr_buf.find("\r\n\r\n");
            }
            if (ec) {
                break;
            }
        }

        if (header_end == std::string::npos) {
            out_error = "Malformed HTTP/ICY response header";
            return false;
        }

        std::string header_block = hdr_buf.substr(0, header_end);
        std::string leftover = hdr_buf.substr(header_end + 4);

        std::istringstream hss(header_block);
        std::string status_line;
        std::getline(hss, status_line);
        status_line = Trim(status_line);

        if (status_line.rfind("ICY ", 0) == 0) {
            out_status = std::atoi(status_line.c_str() + 4);
            if (out_content_type.empty()) out_content_type = "audio/mpeg";
        } else if (status_line.rfind("HTTP/", 0) == 0) {
            auto sp = status_line.find(' ');
            if (sp != std::string::npos) {
                out_status = std::atoi(status_line.c_str() + sp + 1);
            }
        }

        if (out_response_headers) {
            out_response_headers->clear();
        }
        std::string location;
        std::string hline;
        while (std::getline(hss, hline)) {
            StoreParsedResponseHeader(out_response_headers, hline);
            auto colon = hline.find(':');
            if (colon == std::string::npos) continue;
            std::string k = Trim(hline.substr(0, colon));
            std::string v = Trim(hline.substr(colon + 1));
            std::string kl = k;
            for (char& c : kl) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (kl == "content-type") {
                out_content_type = v;
            } else if (kl == "content-length") {
                try { out_content_length = std::stoll(v); } catch (...) {}
            } else if (kl == "location") {
                location = v;
            }
        }

        if ((out_status == 301 || out_status == 302 || out_status == 303 || out_status == 307 || out_status == 308) && !location.empty()) {
            sock.close(ec);
            std::string next_url = ResolveRedirectLocation(url, location);
            if (!HttpClient::ValidateRemoteUrl(next_url, out_error)) {
                return false;
            }
            return PerformRawHttpIcyRequest(method, next_url, headers, timeout_ms, out_status, out_content_type, out_content_length, on_chunk, cancel_flag, out_error, redirect_depth + 1, out_response_headers);
        }

        if (method == "HEAD" || !on_chunk) {
            return out_status > 0;
        }

        if (!leftover.empty()) {
            if (!on_chunk(reinterpret_cast<const std::uint8_t*>(leftover.data()), leftover.size())) {
                out_error = "Aborted by consumer";
                return false;
            }
        }

        std::array<std::uint8_t, 16384> rbuf{};
        while (true) {
            if (cancel_flag && cancel_flag->load()) {
                out_error = "Cancelled";
                return false;
            }
            std::size_t n = sock.read_some(asio::buffer(rbuf), ec);
            if (n > 0) {
                if (!on_chunk(rbuf.data(), n)) {
                    out_error = "Aborted by consumer";
                    return false;
                }
            }
            if (ec == asio::error::eof) {
                break;
            }
            if (ec) {
                out_error = ec.message();
                return false;
            }
        }
        return true;
    } catch (const std::exception& ex) {
        out_error = ex.what();
        return false;
    }
}

bool PerformWinHttpRequest(
    const std::string& method,
    const std::string& url,
    const HttpClient::HeaderMap& headers,
    int timeout_ms,
    bool fail_on_http_error,
    int& out_status,
    std::string& out_content_type,
    std::int64_t& out_content_length,
    const HttpClient::ChunkCallback& on_chunk,
    const std::atomic<bool>* cancel_flag,
    std::string& out_error,
    HttpRequestDiagnostics* out_diag = nullptr,
    bool fresh_session = false,
    HttpClient::HeaderMap* out_response_headers = nullptr) {
    out_status = 0;
    out_content_length = -1;
    if (out_response_headers) {
        out_response_headers->clear();
    }
    if (out_diag) {
        *out_diag = {};
    }

    constexpr int kMaxRedirects = 5;
    std::string current_url = url;
    std::string initial_host;
    {
        std::string sch, pth;
        std::uint16_t prt = 0;
        ParseHttpUrlComponents(url, sch, initial_host, prt, pth);
    }

    for (int redir = 0; redir <= kMaxRedirects; ++redir) {
        bool is_private_or_loopback = false;
        if (!ValidateRemoteUrlWithClass(current_url, out_error, &is_private_or_loopback)) {
            return false;
        }

        std::string cur_scheme, cur_host, cur_path;
        std::uint16_t cur_port = 0;
        ParseHttpUrlComponents(current_url, cur_scheme, cur_host, cur_port, cur_path);
        const auto req_start = std::chrono::steady_clock::now();
        auto fail_winhttp = [&](const char* stage, DWORD err_code, bool allow_transient) -> bool {
            const std::string formatted = FormatWinError(err_code);
            if (out_diag && allow_transient &&
                HttpClient::IsTransientWinHttpError(static_cast<unsigned long>(err_code))) {
                out_diag->transient_transport_error = true;
            }
            if (Logger::Instance().IsDebugEnabled()) {
                const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - req_start).count();
                Logger::Instance().Debug(
                    "HTTP transport failed (winhttp): error=" + std::to_string(err_code) +
                    " (" + formatted + "), stage=" + stage +
                    ", elapsed_ms=" + std::to_string(elapsed_ms) +
                    ", host=" + cur_host);
            }
            out_error = formatted;
            return false;
        };
        HttpClient::HeaderMap effective_headers = headers;
        if (!initial_host.empty() && cur_host != initial_host) {
            for (auto hit = effective_headers.begin(); hit != effective_headers.end();) {
                std::string kl = ToLowerCopy(hit->first);
                if (kl == "authorization" || kl == "cookie") {
                    hit = effective_headers.erase(hit);
                } else {
                    ++hit;
                }
            }
        }

        std::wstring wurl = Utf8ToWide(current_url);
        URL_COMPONENTS uc{};
        uc.dwStructSize = sizeof(uc);
        wchar_t host_buf[512]{};
        wchar_t path_buf[4096]{};
        uc.lpszHostName = host_buf;
        uc.dwHostNameLength = sizeof(host_buf) / sizeof(wchar_t);
        uc.lpszUrlPath = path_buf;
        uc.dwUrlPathLength = sizeof(path_buf) / sizeof(wchar_t);

        if (!WinHttpCrackUrl(wurl.c_str(), static_cast<DWORD>(wurl.size()), 0, &uc) ||
            (uc.nScheme != INTERNET_SCHEME_HTTP && uc.nScheme != INTERNET_SCHEME_HTTPS)) {
            out_error = "URL not allowed";
            return false;
        }

        WinHttpHandle fresh_session_guard(fresh_session ? CreateWinHttpSession(is_private_or_loopback) : nullptr);
        if (fresh_session && redir == 0 && fresh_session_guard.valid()) {
            g_winhttp_fresh_session_count.fetch_add(1, std::memory_order_relaxed);
        }
        HINTERNET session = fresh_session ? static_cast<HINTERNET>(fresh_session_guard) : GetSharedWinHttpSession(is_private_or_loopback);
        if (!session) {
            return fail_winhttp("open", GetLastError(), false);
        }

        WinHttpHandle connect(WinHttpConnect(session, uc.lpszHostName, uc.nPort, 0));
        if (!connect.valid()) {
            return fail_winhttp("connect", GetLastError(), true);
        }

        std::wstring wmethod = Utf8ToWide(method);
        DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;

        auto request_guard = std::make_shared<CancelableWinHttpRequest>(WinHttpOpenRequest(
            connect,
            wmethod.c_str(),
            uc.lpszUrlPath,
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            flags));
        if (request_guard->IsClosed()) {
            return fail_winhttp("open_request", GetLastError(), false);
        }
        if (cancel_flag && cancel_flag->load()) {
            request_guard->Close();
            out_error = "Cancelled";
            return false;
        }
        RegisterCancelableWinHttpRequest(request_guard, cancel_flag);

        DWORD err = 0;
        bool cancelled = false;

        RunWinHttpCall(request_guard, cancel_flag, err, cancelled, [&](HINTERNET req) {
            return WinHttpSetTimeouts(req, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
        });
        if (cancelled) {
            out_error = "Cancelled";
            return false;
        }

        // Disable automatic redirects so every hop is re-validated against SSRF rules.
        DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        RunWinHttpCall(request_guard, cancel_flag, err, cancelled, [&](HINTERNET req) {
            return WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &redirect_policy, sizeof(redirect_policy));
        });
        if (cancelled) {
            out_error = "Cancelled";
            return false;
        }

        // Disable WinHTTP's automatic cookie engine on the request handle (explicit Cookie headers still pass through).
        DWORD disable_cookies = WINHTTP_DISABLE_COOKIES;
        RunWinHttpCall(request_guard, cancel_flag, err, cancelled, [&](HINTERNET req) {
            return WinHttpSetOption(req, WINHTTP_OPTION_DISABLE_FEATURE, &disable_cookies, sizeof(disable_cookies));
        });
        if (cancelled) {
            out_error = "Cancelled";
            return false;
        }

        std::wstring header_str;
        for (const auto& [k, v] : effective_headers) {
            header_str += Utf8ToWide(k) + L": " + Utf8ToWide(v) + L"\r\n";
        }
        if (!header_str.empty()) {
            RunWinHttpCall(request_guard, cancel_flag, err, cancelled, [&](HINTERNET req) {
                return WinHttpAddRequestHeaders(req, header_str.c_str(), static_cast<DWORD>(header_str.size()), WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
            });
            if (cancelled) {
                out_error = "Cancelled";
                return false;
            }
        }

        bool diag_cb_installed = false;
        if (out_diag && Logger::Instance().IsDebugEnabled()) {
            DWORD cb_err = 0;
            bool cb_cancelled = false;
            diag_cb_installed = RunWinHttpCall(request_guard, cancel_flag, cb_err, cb_cancelled, [&](HINTERNET req) -> BOOL {
                DWORD_PTR ctx_ptr = reinterpret_cast<DWORD_PTR>(request_guard.get());
                WinHttpSetOption(req, WINHTTP_OPTION_CONTEXT_VALUE, &ctx_ptr, sizeof(ctx_ptr));
                WINHTTP_STATUS_CALLBACK prev = WinHttpSetStatusCallback(
                    req,
                    OnWinHttpDiagCallback,
                    WINHTTP_CALLBACK_FLAG_CONNECT_TO_SERVER,
                    0);
                return prev != WINHTTP_INVALID_STATUS_CALLBACK;
            });
            if (cb_cancelled) {
                out_error = "Cancelled";
                return false;
            }
        }

        if (!RunWinHttpCall(request_guard, cancel_flag, err, cancelled, [&](HINTERNET req) {
                return WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
            })) {
            if (cancelled) {
                out_error = "Cancelled";
                return false;
            }
            if (err == ERROR_WINHTTP_INVALID_SERVER_RESPONSE && uc.nScheme == INTERNET_SCHEME_HTTP) {
                return PerformRawHttpIcyRequest(method, current_url, effective_headers, timeout_ms, out_status, out_content_type, out_content_length, on_chunk, cancel_flag, out_error, redir, out_response_headers);
            }
            return fail_winhttp("send", err, true);
        }

        if (!RunWinHttpCall(request_guard, cancel_flag, err, cancelled, [&](HINTERNET req) {
                return WinHttpReceiveResponse(req, nullptr);
            })) {
            if (cancelled) {
                out_error = "Cancelled";
                return false;
            }
            if (err == ERROR_WINHTTP_INVALID_SERVER_RESPONSE && uc.nScheme == INTERNET_SCHEME_HTTP) {
                return PerformRawHttpIcyRequest(method, current_url, effective_headers, timeout_ms, out_status, out_content_type, out_content_length, on_chunk, cancel_flag, out_error, redir, out_response_headers);
            }
            return fail_winhttp("receive", err, true);
        }

        if (out_diag && diag_cb_installed) {
            DWORD clr_err = 0;
            bool clr_cancelled = false;
            RunWinHttpCall(request_guard, cancel_flag, clr_err, clr_cancelled, [&](HINTERNET req) -> BOOL {
                WinHttpSetStatusCallback(req, nullptr, 0, 0);
                return TRUE;
            });
            if (clr_cancelled) {
                out_error = "Cancelled";
                return false;
            }
            out_diag->connection_reused_known = true;
            out_diag->connection_reused = !request_guard->saw_connect_to_server.load(std::memory_order_relaxed);
        }

        DWORD status_code = 0;
        DWORD status_size = sizeof(status_code);
        RunWinHttpCall(request_guard, cancel_flag, err, cancelled, [&](HINTERNET req) {
            return WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &status_size, WINHTTP_NO_HEADER_INDEX);
        });
        if (cancelled) {
            out_error = "Cancelled";
            return false;
        }
        out_status = static_cast<int>(status_code);

        if (out_status == 301 || out_status == 302 || out_status == 303 || out_status == 307 || out_status == 308) {
            wchar_t loc_buf[4096]{};
            DWORD loc_size = sizeof(loc_buf);
            bool has_loc = RunWinHttpCall(request_guard, cancel_flag, err, cancelled, [&](HINTERNET req) {
                return WinHttpQueryHeaders(req, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, loc_buf, &loc_size, WINHTTP_NO_HEADER_INDEX);
            });
            if (cancelled) {
                out_error = "Cancelled";
                return false;
            }
            if (!has_loc) {
                out_error = "Redirect without Location header";
                return false;
            }
            if (redir >= kMaxRedirects) {
                out_error = "Too many redirects";
                return false;
            }
            std::string loc = WideToUtf8(std::wstring(loc_buf, loc_size / sizeof(wchar_t)));
            std::string next_url = ResolveRedirectLocation(current_url, loc);
            if (next_url.empty()) {
                out_error = "URL not allowed";
                return false;
            }
            current_url = std::move(next_url);
            continue;
        }

        if (out_diag) {
            out_diag->redirected = (redir > 0);
            out_diag->final_host = cur_host;
        }

        wchar_t ct_buf[256]{};
        DWORD ct_size = sizeof(ct_buf);
        bool has_ct = RunWinHttpCall(request_guard, cancel_flag, err, cancelled, [&](HINTERNET req) {
            return WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX, ct_buf, &ct_size, WINHTTP_NO_HEADER_INDEX);
        });
        if (cancelled) {
            out_error = "Cancelled";
            return false;
        }
        if (has_ct) {
            out_content_type = WideToUtf8(std::wstring(ct_buf, ct_size / sizeof(wchar_t)));
        }

        wchar_t cl_buf[64]{};
        DWORD cl_size = sizeof(cl_buf);
        bool has_cl = RunWinHttpCall(request_guard, cancel_flag, err, cancelled, [&](HINTERNET req) {
            return WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, cl_buf, &cl_size, WINHTTP_NO_HEADER_INDEX);
        });
        if (cancelled) {
            out_error = "Cancelled";
            return false;
        }
        if (has_cl) {
            try {
                out_content_length = std::stoll(WideToUtf8(cl_buf));
            } catch (...) {}
        }

        if (out_response_headers) {
            out_response_headers->clear();
            DWORD raw_bytes_needed = 0;
            RunWinHttpCall(request_guard, cancel_flag, err, cancelled, [&](HINTERNET req) {
                return WinHttpQueryHeaders(
                    req,
                    WINHTTP_QUERY_RAW_HEADERS_CRLF,
                    WINHTTP_HEADER_NAME_BY_INDEX,
                    nullptr,
                    &raw_bytes_needed,
                    WINHTTP_NO_HEADER_INDEX);
            });
            if (cancelled) {
                out_error = "Cancelled";
                return false;
            }
            if (raw_bytes_needed > 0 && raw_bytes_needed <= 65536) {
                std::wstring raw_buf(raw_bytes_needed / sizeof(wchar_t), L'\0');
                bool has_raw = RunWinHttpCall(request_guard, cancel_flag, err, cancelled, [&](HINTERNET req) {
                    return WinHttpQueryHeaders(
                        req,
                        WINHTTP_QUERY_RAW_HEADERS_CRLF,
                        WINHTTP_HEADER_NAME_BY_INDEX,
                        raw_buf.data(),
                        &raw_bytes_needed,
                        WINHTTP_NO_HEADER_INDEX);
                });
                if (cancelled) {
                    out_error = "Cancelled";
                    return false;
                }
                if (has_raw) {
                    raw_buf.resize(raw_bytes_needed / sizeof(wchar_t));
                    while (!raw_buf.empty() && raw_buf.back() == L'\0') {
                        raw_buf.pop_back();
                    }
                    bool all_byte_range = true;
                    for (wchar_t wc : raw_buf) {
                        if (static_cast<unsigned int>(wc) > 0xFFu) {
                            all_byte_range = false;
                            break;
                        }
                    }
                    std::string raw_text;
                    if (all_byte_range) {
                        raw_text.reserve(raw_buf.size());
                        for (wchar_t wc : raw_buf) {
                            raw_text.push_back(static_cast<char>(static_cast<unsigned char>(wc)));
                        }
                    } else {
                        raw_text = WideToUtf8(raw_buf);
                    }
                    std::istringstream rhs(raw_text);
                    std::string hline;
                    bool first_line = true;
                    while (std::getline(rhs, hline)) {
                        if (!hline.empty() && hline.back() == '\r') {
                            hline.pop_back();
                        }
                        if (first_line) {
                            first_line = false;
                            if (hline.rfind("HTTP/", 0) == 0 || hline.rfind("ICY ", 0) == 0) {
                                continue;
                            }
                        }
                        StoreParsedResponseHeader(out_response_headers, hline);
                    }
                }
            }
        }

        if (fail_on_http_error && out_status >= 400) {
            if (out_diag && Logger::Instance().IsDebugEnabled()) {
                DWORD qerr = 0;
                bool qcancelled = false;
                RunWinHttpCall(request_guard, cancel_flag, qerr, qcancelled, [&](HINTERNET req) -> BOOL {
                    out_diag->sent_headers_summary =
                        QuerySentWinHttpHeadersSummary(req, out_diag->had_cookie_header);
                    return TRUE;
                });
                if (qcancelled) {
                    out_error = "Cancelled";
                    return false;
                }
            }
            if (out_diag && method != "HEAD" && out_content_length != 0) {
                std::string snippet;
                std::array<std::uint8_t, 200> ebuf{};
                while (snippet.size() < 200) {
                    DWORD bytes_avail = 0;
                    if (!RunWinHttpCall(request_guard, cancel_flag, err, cancelled, [&](HINTERNET req) {
                            return WinHttpQueryDataAvailable(req, &bytes_avail);
                        }) || bytes_avail == 0) {
                        break;
                    }
                    DWORD want = (std::min)(
                        static_cast<DWORD>(200 - snippet.size()),
                        bytes_avail);
                    DWORD got = 0;
                    if (!RunWinHttpCall(request_guard, cancel_flag, err, cancelled, [&](HINTERNET req) {
                            return WinHttpReadData(req, ebuf.data(), want, &got);
                        }) || got == 0) {
                        break;
                    }
                    snippet.append(reinterpret_cast<const char*>(ebuf.data()), static_cast<std::size_t>(got));
                }
                out_diag->error_body_snippet = std::move(snippet);
            }
            out_error = "HTTP status " + std::to_string(out_status);
            return false;
        }

        if (method == "HEAD" || !on_chunk) {
            return true;
        }

        {
            std::function<void()> hook;
            {
                std::lock_guard<std::mutex> hook_lock(g_winhttp_hook_mtx);
                hook = g_winhttp_between_calls_hook;
            }
            if (hook) {
                hook();
            }
        }

        std::array<std::uint8_t, 32768> buffer{};
        while (true) {
            DWORD bytes_avail = 0;
            if (!RunWinHttpCall(request_guard, cancel_flag, err, cancelled, [&](HINTERNET req) {
                    return WinHttpQueryDataAvailable(req, &bytes_avail);
                })) {
                if (cancelled) {
                    out_error = "Cancelled";
                    return false;
                }
                return fail_winhttp("read", err, out_status < 400);
            }
            if (bytes_avail == 0) {
                break;
            }
            DWORD to_read = (std::min)(bytes_avail, static_cast<DWORD>(buffer.size()));
            DWORD bytes_read = 0;
            if (!RunWinHttpCall(request_guard, cancel_flag, err, cancelled, [&](HINTERNET req) {
                    g_winhttp_read_data_calls.fetch_add(1, std::memory_order_relaxed);
                    return WinHttpReadData(req, buffer.data(), to_read, &bytes_read);
                })) {
                if (cancelled) {
                    out_error = "Cancelled";
                    return false;
                }
                return fail_winhttp("read", err, out_status < 400);
            }
            if (bytes_read == 0) {
                break;
            }
            if (!on_chunk(buffer.data(), static_cast<std::size_t>(bytes_read))) {
                out_error = "Aborted by consumer";
                return false;
            }
        }

        return true;
    }

    out_error = "Too many redirects";
    return false;
}

#else

std::string ReadSmallTextFile(const char* path, std::size_t max_bytes = 16384) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) return "";
    std::string out(max_bytes, '\0');
    ifs.read(out.data(), static_cast<std::streamsize>(max_bytes));
    out.resize(static_cast<std::size_t>(ifs.gcount()));
    return out;
}

std::string BuildLibcurlInstallHint() {
    std::string os_release = ReadSmallTextFile("/etc/os-release");
    if (os_release.empty()) {
        os_release = ReadSmallTextFile("/usr/lib/os-release");
    }
    bool dpkg_has_i386 = false;
    std::string dpkg_arch = ReadSmallTextFile("/var/lib/dpkg/arch", 4096);
    if (!dpkg_arch.empty()) {
        std::istringstream iss(dpkg_arch);
        std::string line;
        while (std::getline(iss, line)) {
            if (Trim(line) == "i386") {
                dpkg_has_i386 = true;
                break;
            }
        }
    }
    return HttpClient::BuildLibcurlInstallHintFromOsRelease(os_release, dpkg_has_i386);
}

// Dynamic libcurl loader on Linux so the .so has zero hard link-time dependency on libcurl
struct CurlApi {
    using CURL = void;
    struct curl_slist;

    void* handle{nullptr};
    int (*global_init)(long){nullptr};
    CURL* (*easy_init)(){nullptr};
    void (*easy_cleanup)(CURL*){nullptr};
    int (*easy_setopt)(CURL*, int, ...){nullptr};
    int (*easy_perform)(CURL*){nullptr};
    int (*easy_getinfo)(CURL*, int, ...){nullptr};
    const char* (*easy_strerror)(int){nullptr};
    curl_slist* (*slist_append)(curl_slist*, const char*){nullptr};
    void (*slist_free_all)(curl_slist*){nullptr};

    static constexpr long GLOBAL_DEFAULT = 3L;
    static constexpr int OPT_URL = 10002;
    static constexpr int OPT_WRITEFUNCTION = 20011;
    static constexpr int OPT_WRITEDATA = 10001;
    static constexpr int OPT_HTTPHEADER = 10023;
    static constexpr int OPT_FOLLOWLOCATION = 52;
    static constexpr int OPT_MAXREDIRS = 68;
    static constexpr int OPT_FAILONERROR = 45;
    static constexpr int OPT_NOBODY = 44;
    static constexpr int OPT_NOPROGRESS = 43;
    static constexpr int OPT_LOW_SPEED_LIMIT = 19;
    static constexpr int OPT_LOW_SPEED_TIME = 20;
    static constexpr int OPT_NOSIGNAL = 99;
    static constexpr int OPT_TIMEOUT_MS = 155;
    static constexpr int OPT_CONNECTTIMEOUT_MS = 156;
    static constexpr int OPT_USERAGENT = 10018;
    static constexpr int OPT_HEADERFUNCTION = 20079;
    static constexpr int OPT_HEADERDATA = 10029;
    static constexpr int OPT_OPENSOCKETFUNCTION = 20163;
    static constexpr int OPT_OPENSOCKETDATA = 10164;
    static constexpr int OPT_PROTOCOLS = 181;
    static constexpr int OPT_REDIR_PROTOCOLS = 182;
    static constexpr int OPT_PROTOCOLS_STR = 10318;
    static constexpr int OPT_REDIR_PROTOCOLS_STR = 10319;
    static constexpr int OPT_XFERINFOFUNCTION = 20219;
    static constexpr int OPT_XFERINFODATA = 10057;
    static constexpr int OPT_HTTP09_ALLOWED = 285;
    static constexpr long PROTO_HTTP_HTTPS = 3L; // CURLPROTO_HTTP (1) | CURLPROTO_HTTPS (2)
    static constexpr int INFO_RESPONSE_CODE = 0x200002;
    static constexpr int INFO_CONTENT_TYPE = 0x100012;
    static constexpr int INFO_REDIRECT_URL = 0x10001F;
    static constexpr int INFO_PRIMARY_IP = 0x100020;
    static constexpr int INFO_TOTAL_TIME_T = 0x600032;
    static constexpr int INFO_NAMELOOKUP_TIME_T = 0x600033;
    static constexpr int INFO_CONNECT_TIME_T = 0x600034;
    static constexpr int INFO_STARTTRANSFER_TIME_T = 0x600036;
    static constexpr int INFO_APPCONNECT_TIME_T = 0x600038;

    static constexpr int ERR_COULDNT_RESOLVE_HOST = 6;
    static constexpr int ERR_COULDNT_CONNECT = 7;
    static constexpr int ERR_OPERATION_TIMEDOUT = 28;
    static constexpr int ERR_SSL_CONNECT_ERROR = 35;
    static constexpr int ERR_GOT_NOTHING = 52;
    static constexpr int ERR_SEND_ERROR = 55;
    static constexpr int ERR_RECV_ERROR = 56;

    bool Load() {
        std::lock_guard<std::mutex> lock(load_mtx);
        if (handle) return true;
        const char* candidates[] = {"libcurl.so.4", "libcurl-gnutls.so.4", "libcurl.so"};
        for (const char* name : candidates) {
            handle = dlopen(name, RTLD_LAZY);
            if (handle) break;
        }
        if (!handle) {
            if (!logged_missing) {
                logged_missing = true;
                std::istringstream iss(BuildLibcurlInstallHint());
                std::string hint_line;
                while (std::getline(iss, hint_line)) {
                    if (!hint_line.empty()) {
                        Logger::Instance().Error(hint_line);
                    }
                }
            }
            return false;
        }
        if (!ResolveSymbols()) {
            return false;
        }
        if (global_init && !global_initialized) {
            global_init(GLOBAL_DEFAULT);
            global_initialized = true;
        }
        return true;
    }

    std::mutex load_mtx;
    bool logged_missing{false};
    bool global_initialized{false};

    bool ResolveSymbols() {
        global_init = reinterpret_cast<decltype(global_init)>(dlsym(handle, "curl_global_init"));
        easy_init = reinterpret_cast<decltype(easy_init)>(dlsym(handle, "curl_easy_init"));
        easy_cleanup = reinterpret_cast<decltype(easy_cleanup)>(dlsym(handle, "curl_easy_cleanup"));
        easy_setopt = reinterpret_cast<decltype(easy_setopt)>(dlsym(handle, "curl_easy_setopt"));
        easy_perform = reinterpret_cast<decltype(easy_perform)>(dlsym(handle, "curl_easy_perform"));
        easy_getinfo = reinterpret_cast<decltype(easy_getinfo)>(dlsym(handle, "curl_easy_getinfo"));
        easy_strerror = reinterpret_cast<decltype(easy_strerror)>(dlsym(handle, "curl_easy_strerror"));
        slist_append = reinterpret_cast<decltype(slist_append)>(dlsym(handle, "curl_slist_append"));
        slist_free_all = reinterpret_cast<decltype(slist_free_all)>(dlsym(handle, "curl_slist_free_all"));

        return easy_init && easy_cleanup && easy_setopt && easy_perform && easy_getinfo;
    }

    static CurlApi& Instance() {
        static CurlApi api;
        api.Load();
        return api;
    }
};

struct CurlSockAddr {
    int family;
    int socktype;
    int protocol;
    unsigned int addrlen;
    struct sockaddr addr;
};

struct LinuxWriteCtx {
    const CurlApi* api{nullptr};
    void* curl_handle{nullptr};
    const HttpClient::ChunkCallback* on_chunk{nullptr};
    const std::atomic<bool>* cancel_flag{nullptr};
    int* out_status{nullptr};
    std::string* out_content_type{nullptr};
    std::int64_t* out_content_length{nullptr};
    std::int64_t content_length{-1};
    std::string redirect_location;
    std::string raw_host;
    std::uint16_t port{80};
    HttpSecurityPolicy policy;
    bool ssrf_blocked{false};
    bool headers_published{false};
    HttpRequestDiagnostics* diag{nullptr};
    HttpClient::HeaderMap* out_response_headers{nullptr};
};

int CurlOpenSocketCb(void* clientp, int /*purpose*/, CurlSockAddr* address) {
    auto* ctx = static_cast<LinuxWriteCtx*>(clientp);
    if (!ctx || !address) return -1;
    asio::ip::address addr;
    if (!SockaddrToAsioAddress(&address->addr, addr)) {
        ctx->ssrf_blocked = true;
        return -1;
    }
    if (!IsAddressAllowed(addr, ctx->raw_host, ctx->port, ctx->policy)) {
        LogBlockedHostOnce(
            ctx->raw_host,
            &addr,
            DescribeBlockedAddressReason(addr, ctx->raw_host, ctx->port, ctx->policy));
        ctx->ssrf_blocked = true;
        return -1; // CURL_SOCKET_BAD - aborts connection even under DNS rebinding
    }
    return ::socket(address->family, address->socktype, address->protocol);
}

int CurlXferInfoCb(void* userdata, std::int64_t, std::int64_t, std::int64_t, std::int64_t) {
    auto* ctx = static_cast<LinuxWriteCtx*>(userdata);
    if (ctx && ctx->cancel_flag && ctx->cancel_flag->load()) {
        return 1;
    }
    return 0;
}

size_t CurlWriteCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<LinuxWriteCtx*>(userdata);
    const size_t total = size * nmemb;
    if (ctx->cancel_flag && ctx->cancel_flag->load()) {
        return 0;
    }
    long http_code = 0;
    if (ctx->api && ctx->curl_handle) {
        ctx->api->easy_getinfo(ctx->curl_handle, CurlApi::INFO_RESPONSE_CODE, &http_code);
        if (http_code == 301 || http_code == 302 || http_code == 303 || http_code == 307 || http_code == 308) {
            return total; // Drain redirect body without feeding audio consumers
        }
    }
    if (!ctx->headers_published) {
        ctx->headers_published = true;
        if (ctx->out_status) {
            *ctx->out_status = static_cast<int>(http_code);
        }
        if (ctx->out_content_type) {
            const char* ct_ptr = nullptr;
            if (ctx->api && ctx->curl_handle) {
                ctx->api->easy_getinfo(ctx->curl_handle, CurlApi::INFO_CONTENT_TYPE, &ct_ptr);
            }
            *ctx->out_content_type = ct_ptr ? std::string(ct_ptr) : std::string();
        }
        if (ctx->out_content_length) {
            *ctx->out_content_length = ctx->content_length;
        }
    }
    if (http_code >= 400 && ctx->diag) {
        if (ctx->diag->error_body_snippet.size() < 200) {
            const size_t take = std::min<size_t>(total, 200 - ctx->diag->error_body_snippet.size());
            ctx->diag->error_body_snippet.append(ptr, take);
        }
        if (ctx->diag->error_body_snippet.size() >= 200) {
            return 0;
        }
        return total;
    }
    if (ctx->on_chunk && *ctx->on_chunk) {
        if (!(*ctx->on_chunk)(reinterpret_cast<const std::uint8_t*>(ptr), total)) {
            return 0;
        }
    }
    return total;
}

size_t CurlHeaderCb(char* buffer, size_t size, size_t nitems, void* userdata) {
    auto* ctx = static_cast<LinuxWriteCtx*>(userdata);
    const size_t total = size * nitems;
    std::string line(buffer, total);
    std::string lower = ToLowerCopy(line);
    if (lower.rfind("http/", 0) == 0 || lower.rfind("icy ", 0) == 0) {
        ctx->content_length = -1;
        ctx->redirect_location.clear();
        if (ctx->out_response_headers) {
            ctx->out_response_headers->clear();
        }
        ctx->headers_published = false;
    } else {
        if (lower.rfind("content-length:", 0) == 0) {
            try {
                ctx->content_length = std::stoll(Trim(line.substr(15)));
            } catch (...) {}
        } else if (lower.rfind("location:", 0) == 0) {
            ctx->redirect_location = Trim(line.substr(9));
        }
        StoreParsedResponseHeader(ctx->out_response_headers, line);
    }
    return total;
}

bool PerformLinuxCurlRequest(
    const std::string& method,
    const std::string& url,
    const HttpClient::HeaderMap& headers,
    int timeout_ms,
    bool apply_total_timeout,
    bool fail_on_error,
    int& out_status,
    std::string& out_content_type,
    std::int64_t& out_content_length,
    const HttpClient::ChunkCallback& on_chunk,
    const std::atomic<bool>* cancel_flag,
    std::string& out_error,
    HttpRequestDiagnostics* out_diag = nullptr,
    HttpClient::HeaderMap* out_response_headers = nullptr) {
    if (out_diag) {
        *out_diag = {};
    }
    if (out_response_headers) {
        out_response_headers->clear();
    }
    auto& curl = CurlApi::Instance();
    if (!curl.handle) {
        out_error = "libcurl not found on host system (install libcurl4:i386)";
        return false;
    }

    constexpr int kMaxRedirects = 5;
    std::string current_url = url;
    std::string initial_host;
    {
        std::string sch, pth;
        std::uint16_t prt = 0;
        ParseHttpUrlComponents(url, sch, initial_host, prt, pth);
    }

    for (int redir = 0; redir <= kMaxRedirects; ++redir) {
        if (!HttpClient::ValidateRemoteUrl(current_url, out_error)) {
            return false;
        }

        std::string cur_scheme, cur_host, cur_path;
        std::uint16_t cur_port = 80;
        ParseHttpUrlComponents(current_url, cur_scheme, cur_host, cur_port, cur_path);

        void* c = curl.easy_init();
        if (!c) {
            out_error = "curl_easy_init failed";
            return false;
        }

        out_status = 0;
        out_content_type.clear();
        out_content_length = -1;

        LinuxWriteCtx ctx;
        ctx.api = &curl;
        ctx.curl_handle = c;
        ctx.on_chunk = &on_chunk;
        ctx.cancel_flag = cancel_flag;
        ctx.out_status = &out_status;
        ctx.out_content_type = &out_content_type;
        ctx.out_content_length = &out_content_length;
        ctx.raw_host = cur_host;
        ctx.port = cur_port;
        ctx.policy = HttpClient::GetSecurityPolicy();
        ctx.diag = out_diag;
        ctx.out_response_headers = out_response_headers;
        if (out_diag) {
            out_diag->redirected = (redir > 0);
            out_diag->final_host = cur_host;
        }

        CurlApi::curl_slist* header_list = nullptr;
        for (const auto& [k, v] : headers) {
            if (!initial_host.empty() && cur_host != initial_host) {
                std::string kl = ToLowerCopy(k);
                if (kl == "authorization" || kl == "cookie") continue;
            }
            std::string h = k + ": " + v;
            header_list = curl.slist_append(header_list, h.c_str());
        }

        curl.easy_setopt(c, CurlApi::OPT_URL, current_url.c_str());
        curl.easy_setopt(c, CurlApi::OPT_USERAGENT, "SampAudioStream/1.0");
        // Follow redirects manually so every hop is re-validated against SSRF rules.
        curl.easy_setopt(c, CurlApi::OPT_FOLLOWLOCATION, 0L);
        curl.easy_setopt(c, CurlApi::OPT_MAXREDIRS, static_cast<long>(kMaxRedirects));
        curl.easy_setopt(c, CurlApi::OPT_PROTOCOLS, CurlApi::PROTO_HTTP_HTTPS);
        curl.easy_setopt(c, CurlApi::OPT_REDIR_PROTOCOLS, CurlApi::PROTO_HTTP_HTTPS);
        curl.easy_setopt(c, CurlApi::OPT_PROTOCOLS_STR, "http,https");
        curl.easy_setopt(c, CurlApi::OPT_REDIR_PROTOCOLS_STR, "http,https");
        curl.easy_setopt(c, CurlApi::OPT_OPENSOCKETFUNCTION, CurlOpenSocketCb);
        curl.easy_setopt(c, CurlApi::OPT_OPENSOCKETDATA, &ctx);

        if (fail_on_error && !out_diag) {
            curl.easy_setopt(c, CurlApi::OPT_FAILONERROR, 1L);
        }
        curl.easy_setopt(c, CurlApi::OPT_NOSIGNAL, 1L);
        curl.easy_setopt(c, CurlApi::OPT_LOW_SPEED_LIMIT, 1L);
        curl.easy_setopt(c, CurlApi::OPT_LOW_SPEED_TIME, 15L);
        curl.easy_setopt(c, CurlApi::OPT_CONNECTTIMEOUT_MS, static_cast<long>(timeout_ms));
        if (apply_total_timeout && timeout_ms > 0) {
            curl.easy_setopt(c, CurlApi::OPT_TIMEOUT_MS, static_cast<long>(timeout_ms));
        }
        curl.easy_setopt(c, CurlApi::OPT_HTTP09_ALLOWED, 1L);
        curl.easy_setopt(c, CurlApi::OPT_NOPROGRESS, 0L);
        curl.easy_setopt(c, CurlApi::OPT_XFERINFOFUNCTION, CurlXferInfoCb);
        curl.easy_setopt(c, CurlApi::OPT_XFERINFODATA, &ctx);
        curl.easy_setopt(c, CurlApi::OPT_WRITEFUNCTION, CurlWriteCb);
        curl.easy_setopt(c, CurlApi::OPT_WRITEDATA, &ctx);
        curl.easy_setopt(c, CurlApi::OPT_HEADERFUNCTION, CurlHeaderCb);
        curl.easy_setopt(c, CurlApi::OPT_HEADERDATA, &ctx);

        if (method == "HEAD") {
            curl.easy_setopt(c, CurlApi::OPT_NOBODY, 1L);
        }
        if (header_list) {
            curl.easy_setopt(c, CurlApi::OPT_HTTPHEADER, header_list);
        }

        int res = curl.easy_perform(c);
        long http_code = 0;
        const char* ct_ptr = nullptr;
        const char* redir_ptr = nullptr;
        std::string primary_ip;
        std::int64_t dns_us = -1;
        std::int64_t connect_us = -1;
        std::int64_t tls_us = -1;
        std::int64_t fb_us = -1;
        std::int64_t total_us = -1;
        if (curl.easy_getinfo) {
            curl.easy_getinfo(c, CurlApi::INFO_RESPONSE_CODE, &http_code);
            curl.easy_getinfo(c, CurlApi::INFO_CONTENT_TYPE, &ct_ptr);
            curl.easy_getinfo(c, CurlApi::INFO_REDIRECT_URL, &redir_ptr);
            if (res != 0 && !ctx.ssrf_blocked) {
                const char* ip_ptr = nullptr;
                if (curl.easy_getinfo(c, CurlApi::INFO_PRIMARY_IP, &ip_ptr) == 0 && ip_ptr && *ip_ptr) {
                    primary_ip = ip_ptr;
                }
                std::int64_t t_val = -1;
                if (curl.easy_getinfo(c, CurlApi::INFO_NAMELOOKUP_TIME_T, &t_val) == 0) dns_us = t_val;
                t_val = -1;
                if (curl.easy_getinfo(c, CurlApi::INFO_CONNECT_TIME_T, &t_val) == 0) connect_us = t_val;
                t_val = -1;
                if (curl.easy_getinfo(c, CurlApi::INFO_APPCONNECT_TIME_T, &t_val) == 0) tls_us = t_val;
                t_val = -1;
                if (curl.easy_getinfo(c, CurlApi::INFO_STARTTRANSFER_TIME_T, &t_val) == 0) fb_us = t_val;
                t_val = -1;
                if (curl.easy_getinfo(c, CurlApi::INFO_TOTAL_TIME_T, &t_val) == 0) total_us = t_val;
            }
        }

        out_status = static_cast<int>(http_code);
        out_content_type = ct_ptr ? std::string(ct_ptr) : std::string();
        out_content_length = ctx.content_length;
        std::string next_loc = (redir_ptr && *redir_ptr) ? std::string(redir_ptr) : ctx.redirect_location;

        if (header_list) curl.slist_free_all(header_list);
        curl.easy_cleanup(c);

        if (ctx.ssrf_blocked) {
            out_error = "URL not allowed";
            return false;
        }

        if (res == 0 && (out_status == 301 || out_status == 302 || out_status == 303 || out_status == 307 || out_status == 308)) {
            if (next_loc.empty()) {
                out_error = "Redirect without Location header";
                return false;
            }
            if (redir >= kMaxRedirects) {
                out_error = "Too many redirects";
                return false;
            }
            std::string next_url = ResolveRedirectLocation(current_url, next_loc);
            if (next_url.empty()) {
                out_error = "URL not allowed";
                return false;
            }
            current_url = std::move(next_url);
            continue;
        }

        if (fail_on_error && out_status >= 400) {
            if (out_error.empty() || (cancel_flag && !cancel_flag->load())) {
                out_error = "HTTP status " + std::to_string(out_status);
            }
            return false;
        }

        if (res != 0) {
            const std::string err_text = curl.easy_strerror ? std::string(curl.easy_strerror(res)) : ("curl error " + std::to_string(res));
            const bool cancelled = (cancel_flag && cancel_flag->load());
            const bool is_transient =
                !cancelled &&
                out_status < 400 &&
                HttpClient::IsTransientCurlError(res);
            if (out_diag && is_transient) {
                out_diag->transient_transport_error = true;
            }
            if (!cancelled && Logger::Instance().IsDebugEnabled()) {
                auto fmt_ms = [](std::int64_t us) -> std::string {
                    return (us >= 0) ? (std::to_string(us / 1000) + "ms") : std::string("n/a");
                };
                const char* ip_family = primary_ip.empty()
                    ? "unknown"
                    : (primary_ip.find(':') != std::string::npos ? "ipv6" : "ipv4");
                Logger::Instance().Debug(
                    "HTTP transport failed: curl error " + std::to_string(res) +
                    " (" + err_text + "), dns=" + fmt_ms(dns_us) +
                    ", connect=" + fmt_ms(connect_us) +
                    ", tls=" + fmt_ms(tls_us) +
                    ", first_byte=" + fmt_ms(fb_us) +
                    ", total=" + fmt_ms(total_us) +
                    ", ip=" + (primary_ip.empty() ? std::string("none") : primary_ip) +
                    " (" + ip_family + "), host=" + cur_host);
            }
            out_error = err_text;
            return false;
        }
        return true;
    }

    out_error = "Too many redirects";
    return false;
}

#endif

} // namespace

void HttpClient::SetSecurityPolicy(const HttpSecurityPolicy& policy) {
    std::lock_guard<std::mutex> lock(g_security_policy_mtx);
    g_security_policy = policy;
}

HttpSecurityPolicy HttpClient::GetSecurityPolicy() {
    std::lock_guard<std::mutex> lock(g_security_policy_mtx);
    return g_security_policy;
}

bool ValidateUrlPolicyInternal(
    const std::string& url,
    std::string& out_host,
    std::uint16_t& out_port,
    HttpSecurityPolicy& out_policy,
    bool& out_is_literal,
    std::string& out_error) {
    out_is_literal = false;
    std::string scheme, path;
    if (!ParseHttpUrlComponents(url, scheme, out_host, out_port, path)) {
        out_error = "URL not allowed";
        return false;
    }

    out_policy = HttpClient::GetSecurityPolicy();
    if (!out_policy.allow_private_network_urls) {
        if (out_host == "localhost" ||
            (out_host.size() > 10 && out_host.compare(out_host.size() - 10, 10, ".localhost") == 0)) {
            LogBlockedHostOnce(out_host, nullptr, "localhost is restricted");
            out_error = "URL not allowed";
            return false;
        }
    }

    std::error_code ec;
    auto literal_addr = asio::ip::make_address(out_host, ec);
    if (!ec) {
        out_is_literal = true;
        if (!IsAddressAllowed(literal_addr, out_host, out_port, out_policy)) {
            LogBlockedHostOnce(
                out_host,
                &literal_addr,
                DescribeBlockedAddressReason(literal_addr, out_host, out_port, out_policy));
            out_error = "URL not allowed";
            return false;
        }
    }
    return true;
}

bool HttpClient::ValidateUrlSyntaxAndLiteralPolicy(const std::string& url, std::string& out_error) {
    std::string host;
    std::uint16_t port = 0;
    HttpSecurityPolicy policy;
    bool is_literal = false;
    return ValidateUrlPolicyInternal(url, host, port, policy, is_literal, out_error);
}

bool ValidateRemoteUrlWithClass(
    const std::string& url,
    std::string& out_error,
    bool* out_is_private_or_loopback) {
    if (out_is_private_or_loopback) {
        *out_is_private_or_loopback = false;
    }
    std::string host;
    std::uint16_t port = 0;
    HttpSecurityPolicy policy;
    bool is_literal = false;
    if (!ValidateUrlPolicyInternal(url, host, port, policy, is_literal, out_error)) {
        return false;
    }
    if (out_is_private_or_loopback &&
        (host == "localhost" ||
         (host.size() > 10 && host.compare(host.size() - 10, 10, ".localhost") == 0))) {
        *out_is_private_or_loopback = true;
    }
    if (is_literal) {
        if (out_is_private_or_loopback) {
            std::error_code ec;
            auto literal_addr = asio::ip::make_address(host, ec);
            if (!ec && (literal_addr.is_loopback() || IsRestrictedIpAddress(literal_addr))) {
                *out_is_private_or_loopback = true;
            }
        }
        return true;
    }

    try {
        std::error_code ec;
        asio::io_context ioc;
        asio::ip::tcp::resolver resolver(ioc);
        auto results = resolver.resolve(host, std::to_string(port), ec);
        if (ec || results.empty()) {
            out_error = "DNS resolution failed for " + host;
            return false;
        }
        bool any_private_or_loopback = false;
        for (const auto& entry : results) {
            auto addr = entry.endpoint().address();
            if (!IsAddressAllowed(addr, host, port, policy)) {
                LogBlockedHostOnce(
                    host,
                    &addr,
                    DescribeBlockedAddressReason(addr, host, port, policy));
                out_error = "URL not allowed";
                return false;
            }
            if (addr.is_loopback() || IsRestrictedIpAddress(addr)) {
                any_private_or_loopback = true;
            }
        }
        if (out_is_private_or_loopback && any_private_or_loopback) {
            *out_is_private_or_loopback = true;
        }
        return true;
    } catch (const std::exception& ex) {
        out_error = std::string("DNS resolution failed for ") + host + ": " + ex.what();
        return false;
    } catch (...) {
        out_error = "DNS resolution failed for " + host;
        return false;
    }
}

bool HttpClient::ValidateRemoteUrl(const std::string& url, std::string& out_error) {
    return ValidateRemoteUrlWithClass(url, out_error, nullptr);
}

std::string HttpClient::FindHeaderIgnoreCase(const HeaderMap& headers, const std::string& name) {
    const std::string target = ToLowerCopy(name);
    if (target.empty()) {
        return {};
    }
    for (const auto& [k, v] : headers) {
        if (ToLowerCopy(k) == target) {
            return v;
        }
    }
    return {};
}

HttpResponse HttpClient::Get(const std::string& url, const HeaderMap& headers, int timeout_ms, const std::atomic<bool>* cancel_flag) {
    HttpResponse resp;
    std::int64_t content_len = -1;
    ChunkCallback collector = [&resp](const std::uint8_t* data, std::size_t size) {
        resp.body.append(reinterpret_cast<const char*>(data), size);
        return true;
    };

#ifdef _WIN32
    PerformWinHttpRequest("GET", url, headers, timeout_ms, false, resp.status_code, resp.content_type, content_len, collector, cancel_flag, resp.error, nullptr, false, &resp.headers);
#else
    PerformLinuxCurlRequest("GET", url, headers, timeout_ms, true, false, resp.status_code, resp.content_type, content_len, collector, cancel_flag, resp.error, nullptr, &resp.headers);
#endif
    return resp;
}

HttpResponse HttpClient::Head(const std::string& url, const HeaderMap& headers, int timeout_ms, const std::atomic<bool>* cancel_flag) {
    HttpResponse resp;
    std::int64_t content_len = -1;
#ifdef _WIN32
    PerformWinHttpRequest("HEAD", url, headers, timeout_ms, false, resp.status_code, resp.content_type, content_len, nullptr, cancel_flag, resp.error, nullptr, false, &resp.headers);
#else
    PerformLinuxCurlRequest("HEAD", url, headers, timeout_ms, true, false, resp.status_code, resp.content_type, content_len, nullptr, cancel_flag, resp.error, nullptr, &resp.headers);
#endif
    return resp;
}

bool HttpClient::DownloadToFile(
    const std::string& url,
    const std::string& dest_path,
    std::string& out_error,
    const HeaderMap& headers,
    int timeout_ms,
    const std::atomic<bool>* cancel_flag) {
    std::filesystem::path target(dest_path);
    std::filesystem::path temp = target;
    temp += ".part";

    if (target.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(target.parent_path(), ec);
    }

    std::ofstream ofs(temp, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        out_error = "Failed to open destination file: " + temp.string();
        return false;
    }

    int status = 0;
    std::string content_type;
    std::int64_t content_len = -1;
    ChunkCallback writer = [&ofs, cancel_flag](const std::uint8_t* data, std::size_t size) {
        if (cancel_flag && cancel_flag->load()) return false;
        ofs.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
        return ofs.good();
    };

#ifdef _WIN32
    bool ok = PerformWinHttpRequest("GET", url, headers, timeout_ms, true, status, content_type, content_len, writer, cancel_flag, out_error);
#else
    bool ok = PerformLinuxCurlRequest("GET", url, headers, timeout_ms, false, true, status, content_type, content_len, writer, cancel_flag, out_error);
#endif
    ofs.close();

    if (!ok || status < 200 || status >= 300) {
        std::error_code ec;
        std::filesystem::remove(temp, ec);
        if (out_error.empty()) {
            out_error = "HTTP status " + std::to_string(status);
        }
        return false;
    }

    std::error_code ec;
    std::filesystem::rename(temp, target, ec);
    if (ec) {
        std::filesystem::remove(target, ec);
        ec.clear();
        std::filesystem::rename(temp, target, ec);
        if (ec) {
            out_error = "Failed to move downloaded file: " + ec.message();
            return false;
        }
    }
    return true;
}

bool HttpClient::StreamGet(
    const std::string& url,
    const HeaderMap& headers,
    const ChunkCallback& on_chunk,
    const std::atomic<bool>& cancel_flag,
    std::string& out_error,
    const HeaderCallback& on_headers,
    int connect_timeout_ms,
    HttpRequestDiagnostics* out_diag,
    bool fresh_session) {
    int status = 0;
    std::string content_type;
    std::int64_t content_len = -1;
    HeaderMap resp_headers;
    if (out_diag) {
        *out_diag = {};
    }

    bool header_emitted = false;
    ChunkCallback wrapped_chunk = [&](const std::uint8_t* data, std::size_t size) {
        if (!header_emitted) {
            header_emitted = true;
            if (on_headers) {
                on_headers(status, content_type, content_len, resp_headers);
            }
        }
        return on_chunk(data, size);
    };

#ifdef _WIN32
    bool ok = PerformWinHttpRequest("GET", url, headers, connect_timeout_ms, true, status, content_type, content_len, wrapped_chunk, &cancel_flag, out_error, out_diag, fresh_session, &resp_headers);
#else
    (void)fresh_session;
    bool ok = PerformLinuxCurlRequest("GET", url, headers, connect_timeout_ms, false, true, status, content_type, content_len, wrapped_chunk, &cancel_flag, out_error, out_diag, &resp_headers);
#endif
    if (!header_emitted && status > 0 && on_headers) {
        header_emitted = true;
        on_headers(status, content_type, content_len, resp_headers);
    }
    if (!cancel_flag.load() && status >= 400 && out_error != "Cancelled" && out_error != "URL not allowed") {
        out_error = "HTTP status " + std::to_string(status);
        return false;
    }
    if (ok && (status < 200 || status >= 300)) {
        out_error = "HTTP status " + std::to_string(status);
        return false;
    }
    return ok;
}

HttpAbProbeResult HttpClient::ProbeUrlSharedVsFreshForDebug(
    const std::string& url,
    const HeaderMap& headers,
    const std::atomic<bool>* cancel_flag,
    int timeout_ms) {
    HttpAbProbeResult res;
    HeaderMap probe_a_headers = {{"Range", "bytes=0-1023"}};
    HeaderMap probe_b_headers = headers;
    probe_b_headers["Range"] = "bytes=0-1023";

    std::string ct_a, ct_b;
    std::int64_t cl_a = -1, cl_b = -1;
#ifdef _WIN32
    PerformWinHttpRequest(
        "GET",
        url,
        probe_a_headers,
        timeout_ms,
        false,
        res.shared_no_headers_status,
        ct_a,
        cl_a,
        nullptr,
        cancel_flag,
        res.shared_no_headers_error,
        nullptr,
        false);
    if (cancel_flag && cancel_flag->load()) {
        return res;
    }
    PerformWinHttpRequest(
        "GET",
        url,
        probe_b_headers,
        timeout_ms,
        false,
        res.fresh_same_headers_status,
        ct_b,
        cl_b,
        nullptr,
        cancel_flag,
        res.fresh_same_headers_error,
        nullptr,
        true);
#else
    ChunkCallback accept_probe = [](const std::uint8_t*, std::size_t) { return true; };
    PerformLinuxCurlRequest(
        "GET",
        url,
        probe_a_headers,
        timeout_ms,
        true,
        false,
        res.shared_no_headers_status,
        ct_a,
        cl_a,
        accept_probe,
        cancel_flag,
        res.shared_no_headers_error,
        nullptr);
    if (cancel_flag && cancel_flag->load()) {
        return res;
    }
    PerformLinuxCurlRequest(
        "GET",
        url,
        probe_b_headers,
        timeout_ms,
        true,
        false,
        res.fresh_same_headers_status,
        ct_b,
        cl_b,
        accept_probe,
        cancel_flag,
        res.fresh_same_headers_error,
        nullptr);
#endif
    return res;
}

bool HttpClient::CheckRuntimeSupport(std::string& out_error) {
#ifdef _WIN32
    (void)out_error;
    return true;
#else
    auto& curl = CurlApi::Instance();
    if (!curl.handle) {
        out_error = "32-bit libcurl.so.4 missing; see installation instructions above.";
        return false;
    }
    return true;
#endif
}

bool HttpClient::IsTransientCurlError(int code) {
    return code == 6 ||  // CURLE_COULDNT_RESOLVE_HOST
           code == 7 ||  // CURLE_COULDNT_CONNECT
           code == 28 || // CURLE_OPERATION_TIMEDOUT
           code == 35 || // CURLE_SSL_CONNECT_ERROR
           code == 52 || // CURLE_GOT_NOTHING
           code == 55 || // CURLE_SEND_ERROR
           code == 56;   // CURLE_RECV_ERROR
}

bool HttpClient::IsTransientWinHttpError(unsigned long code) {
    return code == 12002UL || // ERROR_WINHTTP_TIMEOUT
           code == 12007UL || // ERROR_WINHTTP_NAME_NOT_RESOLVED
           code == 12029UL || // ERROR_WINHTTP_CANNOT_CONNECT
           code == 12030UL || // ERROR_WINHTTP_CONNECTION_ERROR
           code == 12152UL || // ERROR_WINHTTP_INVALID_SERVER_RESPONSE
           code == 12175UL;   // ERROR_WINHTTP_SECURE_FAILURE
}

std::string HttpClient::BuildLibcurlInstallHintFromOsRelease(const std::string& os_release_content, bool dpkg_has_i386) {
    auto sanitize_value = [](std::string val) {
        val = Trim(val);
        if (val.size() >= 2 &&
            ((val.front() == '"' && val.back() == '"') || (val.front() == '\'' && val.back() == '\''))) {
            val = val.substr(1, val.size() - 2);
        }
        std::string clean;
        clean.reserve(std::min<std::size_t>(val.size(), 100));
        for (unsigned char c : val) {
            if (c < 0x20 || c == 0x7F) continue;
            clean.push_back(static_cast<char>(c));
            if (clean.size() >= 100) break;
        }
        return clean;
    };

    auto parse_ver = [](const std::string& v, int& major, int& minor) -> bool {
        if (v.empty()) return false;
        std::size_t i = 0;
        long long maj = 0;
        if (!std::isdigit(static_cast<unsigned char>(v[i]))) return false;
        while (i < v.size() && std::isdigit(static_cast<unsigned char>(v[i]))) {
            maj = maj * 10 + (v[i] - '0');
            if (maj > 100000) return false;
            ++i;
        }
        long long min_v = 0;
        if (i < v.size()) {
            if (v[i] != '.') return false;
            ++i;
            if (i >= v.size() || !std::isdigit(static_cast<unsigned char>(v[i]))) return false;
            while (i < v.size() && std::isdigit(static_cast<unsigned char>(v[i]))) {
                min_v = min_v * 10 + (v[i] - '0');
                if (min_v > 100000) return false;
                ++i;
            }
            // Allow optional third patch segment like 22.04.5, reject non-numeric suffixes
            if (i < v.size()) {
                if (v[i] != '.') return false;
                ++i;
                if (i >= v.size()) return false;
                for (; i < v.size(); ++i) {
                    if (!std::isdigit(static_cast<unsigned char>(v[i])) && v[i] != '.') return false;
                }
            }
        }
        major = static_cast<int>(maj);
        minor = static_cast<int>(min_v);
        return true;
    };

    std::string id, id_like, version_id, pretty_name;
    std::istringstream iss(os_release_content);
    std::string line;
    while (std::getline(iss, line)) {
        std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] == '#') continue;
        auto eq = trimmed.find('=');
        if (eq == std::string::npos) continue;
        std::string key = Trim(trimmed.substr(0, eq));
        std::string val = sanitize_value(trimmed.substr(eq + 1));
        if (key == "ID") {
            id = val;
        } else if (key == "ID_LIKE") {
            id_like = val;
        } else if (key == "VERSION_ID") {
            version_id = val;
        } else if (key == "PRETTY_NAME") {
            pretty_name = val;
        }
    }

    const std::string id_low = ToLowerCopy(id);
    const std::string id_like_low = ToLowerCopy(id_like);
    std::vector<std::string> like_tokens;
    {
        std::istringstream lss(id_like_low);
        std::string tok;
        while (lss >> tok) {
            like_tokens.push_back(tok);
        }
    }
    auto has_family = [&](const char* name) {
        return id_low == name || std::find(like_tokens.begin(), like_tokens.end(), name) != like_tokens.end();
    };

    int ver_major = 0;
    int ver_minor = 0;
    const bool has_num_ver = parse_ver(version_id, ver_major, ver_minor);

    const std::string apt_prefix = dpkg_has_i386
        ? "sudo apt update && sudo apt install "
        : "sudo dpkg --add-architecture i386 && sudo apt update && sudo apt install ";

    std::string detected = pretty_name;
    if (detected.empty()) {
        if (!id.empty()) {
            detected = id + (version_id.empty() ? std::string() : (" " + version_id));
        } else {
            detected = "unknown Linux distribution";
        }
    }

    std::string install_line;
    if (id_low == "ubuntu") {
        if (has_num_ver) {
            const bool is_t64 = (ver_major > 24) || (ver_major == 24 && ver_minor >= 4);
            install_line = "Install it with: " + apt_prefix + (is_t64 ? "libcurl4t64:i386" : "libcurl4:i386");
        } else {
            install_line = "Install it with: " + apt_prefix +
                "libcurl4:i386  (if the package does not exist, use libcurl4t64:i386)";
        }
    } else if (id_low == "debian") {
        if (has_num_ver) {
            const bool is_t64 = (ver_major >= 13);
            install_line = "Install it with: " + apt_prefix + (is_t64 ? "libcurl4t64:i386" : "libcurl4:i386");
        } else {
            install_line = "Install it with: " + apt_prefix +
                "libcurl4:i386  (if the package does not exist, use libcurl4t64:i386)";
        }
    } else if (has_family("ubuntu") || has_family("debian")) {
        install_line = "Install it with: " + apt_prefix +
            "libcurl4:i386  (if the package does not exist, use libcurl4t64:i386)";
    } else if (has_family("rhel") || has_family("fedora") || has_family("centos") ||
               has_family("rocky") || has_family("almalinux")) {
        if (id_low == "centos" && has_num_ver && ver_major < 8) {
            install_line = "Install it with: sudo yum install libcurl.i686";
        } else {
            install_line = "Install it with: sudo dnf install libcurl.i686";
        }
    } else if (has_family("arch") || id_low == "archarm") {
        install_line = "Install it with: sudo pacman -S lib32-curl (requires the [multilib] repository enabled)";
    } else if (id_low.find("suse") != std::string::npos || id_like_low.find("suse") != std::string::npos) {
        install_line = "Install it with: sudo zypper install libcurl4-32bit";
    } else {
        install_line =
            "Install 32-bit libcurl: Debian/Ubuntu: " + apt_prefix +
            "libcurl4:i386 (or libcurl4t64:i386 on Ubuntu 24.04+/Debian 13+) | "
            "RHEL/Fedora/Rocky: sudo dnf install libcurl.i686 | Arch: sudo pacman -S lib32-curl | "
            "openSUSE: sudo zypper install libcurl4-32bit | Docs: https://github.com/Walter-Correa/SampAudioStream/blob/main/docs/getting-started.md";
    }

    return std::string("32-bit libcurl (libcurl.so.4) not found - audio streaming will not work.\n") +
           "Detected system: " + detected + "\n" +
           install_line + "\n" +
           "Then restart the server. Verify with: ldconfig -p | grep libcurl.so.4  (a line with \"(libc6)\" without \"x86-64\" must appear)";
}

void HttpClient::Initialize() {
#ifdef _WIN32
    GetWinHttpCloserState().Reenable();
#endif
}

void HttpClient::PollCancellations() {
#ifdef _WIN32
    std::vector<std::shared_ptr<CancelableWinHttpRequest>> to_close;
    {
        std::lock_guard<std::mutex> lock(g_active_winhttp_mtx);
        g_active_winhttp.erase(
            std::remove_if(g_active_winhttp.begin(), g_active_winhttp.end(), [&](const ActiveWinHttpEntry& e) {
                auto locked = e.req.lock();
                if (!locked) return true;
                if (e.cancel_flag && e.cancel_flag->load()) {
                    to_close.push_back(std::move(locked));
                    return true;
                }
                return false;
            }),
            g_active_winhttp.end());
    }
    if (!to_close.empty()) {
        GetWinHttpCloserState().Enqueue(to_close);
    }
#endif
}

void HttpClient::Shutdown() {
#ifdef _WIN32
    GetWinHttpCloserState().Shutdown(3000);
    PollCancellations();
#endif
}

void HttpClient::SetWinHttpBetweenCallsHookForTesting(std::function<void()> hook) {
#ifdef _WIN32
    std::lock_guard<std::mutex> lock(g_winhttp_hook_mtx);
    g_winhttp_between_calls_hook = std::move(hook);
#else
    (void)hook;
#endif
}

std::uint64_t HttpClient::GetWinHttpReadDataCallCountForTesting() {
#ifdef _WIN32
    return g_winhttp_read_data_calls.load(std::memory_order_relaxed);
#else
    return 0;
#endif
}

std::uint64_t HttpClient::GetWinHttpCloserThreadCountForTesting() {
#ifdef _WIN32
    return GetWinHttpCloserState().threads_created.load(std::memory_order_relaxed);
#else
    return 0;
#endif
}

std::uint64_t HttpClient::GetWinHttpFreshSessionCountForTesting() {
#ifdef _WIN32
    return g_winhttp_fresh_session_count.load(std::memory_order_relaxed);
#else
    return 0;
#endif
}

void HttpClient::ResetWinHttpSessionCountersForTesting() {
#ifdef _WIN32
    g_winhttp_fresh_session_count.store(0, std::memory_order_relaxed);
#endif
}

void HttpClient::FlushCancellationsForTesting() {
#ifdef _WIN32
    PollCancellations();
    GetWinHttpCloserState().WaitUntilIdle(5000);
#endif
}

std::string HttpClient::DetectPublicIPv4() {
    const char* endpoints[] = {
        "https://api.ipify.org",
        "https://ipv4.icanhazip.com",
        "https://checkip.amazonaws.com"
    };

    for (const char* url : endpoints) {
        auto resp = Get(url, {}, 5000);
        if (resp.Ok()) {
            std::string ip = Trim(resp.body);
            struct in_addr addr4{};
            if (!ip.empty() && ::inet_pton(AF_INET, ip.c_str(), &addr4) == 1) {
                return ip;
            }
        }
    }
    return "127.0.0.1";
}

} // namespace sampaudiostream
