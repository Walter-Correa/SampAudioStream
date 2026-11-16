#include "sampaudiostream/network/http_client.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#else
#include <dlfcn.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace sampaudiostream {

namespace {

std::string Trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
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

std::string FormatWinError(DWORD err) {
    return "WinHTTP error code " + std::to_string(err);
}

bool PerformWinHttpRequest(
    const std::string& method,
    const std::string& url,
    const HttpClient::HeaderMap& headers,
    int timeout_ms,
    int& out_status,
    std::string& out_content_type,
    std::int64_t& out_content_length,
    const HttpClient::ChunkCallback& on_chunk,
    const std::atomic<bool>* cancel_flag,
    std::string& out_error) {
    out_status = 0;
    out_content_length = -1;

    std::wstring wurl = Utf8ToWide(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host_buf[512]{};
    wchar_t path_buf[4096]{};
    uc.lpszHostName = host_buf;
    uc.dwHostNameLength = sizeof(host_buf) / sizeof(wchar_t);
    uc.lpszUrlPath = path_buf;
    uc.dwUrlPathLength = sizeof(path_buf) / sizeof(wchar_t);

    if (!WinHttpCrackUrl(wurl.c_str(), static_cast<DWORD>(wurl.size()), 0, &uc)) {
        out_error = "Invalid URL: " + url;
        return false;
    }

    WinHttpHandle session(WinHttpOpen(
        L"SampAudioStream/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0));
    if (!session.valid()) {
        out_error = FormatWinError(GetLastError());
        return false;
    }

    WinHttpSetTimeouts(session, timeout_ms, timeout_ms, timeout_ms, timeout_ms);

    WinHttpHandle connect(WinHttpConnect(session, uc.lpszHostName, uc.nPort, 0));
    if (!connect.valid()) {
        out_error = FormatWinError(GetLastError());
        return false;
    }

    std::wstring wmethod = Utf8ToWide(method);
    DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;

    WinHttpHandle request(WinHttpOpenRequest(
        connect,
        wmethod.c_str(),
        uc.lpszUrlPath,
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        flags));
    if (!request.valid()) {
        out_error = FormatWinError(GetLastError());
        return false;
    }

    DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect_policy, sizeof(redirect_policy));

    std::wstring header_str;
    for (const auto& [k, v] : headers) {
        header_str += Utf8ToWide(k) + L": " + Utf8ToWide(v) + L"\r\n";
    }
    if (!header_str.empty()) {
        WinHttpAddRequestHeaders(request, header_str.c_str(), static_cast<DWORD>(header_str.size()), WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    }

    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        out_error = FormatWinError(GetLastError());
        return false;
    }

    if (!WinHttpReceiveResponse(request, nullptr)) {
        out_error = FormatWinError(GetLastError());
        return false;
    }

    DWORD status_code = 0;
    DWORD status_size = sizeof(status_code);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &status_size, WINHTTP_NO_HEADER_INDEX);
    out_status = static_cast<int>(status_code);

    wchar_t ct_buf[256]{};
    DWORD ct_size = sizeof(ct_buf);
    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX, ct_buf, &ct_size, WINHTTP_NO_HEADER_INDEX)) {
        out_content_type = WideToUtf8(std::wstring(ct_buf, ct_size / sizeof(wchar_t)));
    }

    wchar_t cl_buf[64]{};
    DWORD cl_size = sizeof(cl_buf);
    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, cl_buf, &cl_size, WINHTTP_NO_HEADER_INDEX)) {
        try {
            out_content_length = std::stoll(WideToUtf8(cl_buf));
        } catch (...) {}
    }

    if (method == "HEAD" || !on_chunk) {
        return true;
    }

    std::array<std::uint8_t, 32768> buffer{};
    DWORD bytes_read = 0;
    while (true) {
        if (cancel_flag && cancel_flag->load()) {
            out_error = "Cancelled";
            return false;
        }
        if (!WinHttpReadData(request, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes_read)) {
            out_error = FormatWinError(GetLastError());
            return false;
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

#else

// Dynamic libcurl loader on Linux so the .so has zero hard link-time dependency on libcurl
struct CurlApi {
    using CURL = void;
    struct curl_slist;

    void* handle{nullptr};
    CURL* (*easy_init)(){nullptr};
    void (*easy_cleanup)(CURL*){nullptr};
    int (*easy_setopt)(CURL*, int, ...){nullptr};
    int (*easy_perform)(CURL*){nullptr};
    int (*easy_getinfo)(CURL*, int, ...){nullptr};
    const char* (*easy_strerror)(int){nullptr};
    curl_slist* (*slist_append)(curl_slist*, const char*){nullptr};
    void (*slist_free_all)(curl_slist*){nullptr};

    static constexpr int OPT_URL = 10002;
    static constexpr int OPT_WRITEFUNCTION = 20011;
    static constexpr int OPT_WRITEDATA = 10001;
    static constexpr int OPT_HTTPHEADER = 10023;
    static constexpr int OPT_FOLLOWLOCATION = 52;
    static constexpr int OPT_NOBODY = 44;
    static constexpr int OPT_CONNECTTIMEOUT_MS = 156;
    static constexpr int OPT_USERAGENT = 10018;
    static constexpr int OPT_HEADERFUNCTION = 20079;
    static constexpr int OPT_HEADERDATA = 10029;
    static constexpr int INFO_RESPONSE_CODE = 0x200002;
    static constexpr int INFO_CONTENT_TYPE = 0x100012;

    bool Load() {
        if (handle) return true;
        const char* candidates[] = {"libcurl.so.4", "libcurl-gnutls.so.4", "libcurl.so"};
        for (const char* name : candidates) {
            handle = dlopen(name, RTLD_LAZY);
            if (handle) break;
        }
        if (!handle) return false;

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

struct LinuxWriteCtx {
    const HttpClient::ChunkCallback* on_chunk{nullptr};
    const std::atomic<bool>* cancel_flag{nullptr};
    std::int64_t content_length{-1};
};

size_t CurlWriteCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<LinuxWriteCtx*>(userdata);
    const size_t total = size * nmemb;
    if (ctx->cancel_flag && ctx->cancel_flag->load()) {
        return 0;
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
    std::string lower = line;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower.rfind("content-length:", 0) == 0) {
        try {
            ctx->content_length = std::stoll(Trim(line.substr(15)));
        } catch (...) {}
    }
    return total;
}

bool PerformLinuxCurlRequest(
    const std::string& method,
    const std::string& url,
    const HttpClient::HeaderMap& headers,
    int timeout_ms,
    int& out_status,
    std::string& out_content_type,
    std::int64_t& out_content_length,
    const HttpClient::ChunkCallback& on_chunk,
    const std::atomic<bool>* cancel_flag,
    std::string& out_error) {
    auto& curl = CurlApi::Instance();
    if (!curl.handle) {
        out_error = "libcurl not found on host system";
        return false;
    }

    void* c = curl.easy_init();
    if (!c) {
        out_error = "curl_easy_init failed";
        return false;
    }

    LinuxWriteCtx ctx{&on_chunk, cancel_flag, -1};
    CurlApi::curl_slist* header_list = nullptr;
    for (const auto& [k, v] : headers) {
        std::string h = k + ": " + v;
        header_list = curl.slist_append(header_list, h.c_str());
    }

    curl.easy_setopt(c, CurlApi::OPT_URL, url.c_str());
    curl.easy_setopt(c, CurlApi::OPT_USERAGENT, "SampAudioStream/1.0");
    curl.easy_setopt(c, CurlApi::OPT_FOLLOWLOCATION, 1L);
    curl.easy_setopt(c, CurlApi::OPT_CONNECTTIMEOUT_MS, static_cast<long>(timeout_ms));
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
    curl.easy_getinfo(c, CurlApi::INFO_RESPONSE_CODE, &http_code);
    curl.easy_getinfo(c, CurlApi::INFO_CONTENT_TYPE, &ct_ptr);

    out_status = static_cast<int>(http_code);
    if (ct_ptr) out_content_type = ct_ptr;
    out_content_length = ctx.content_length;

    if (header_list) curl.slist_free_all(header_list);
    curl.easy_cleanup(c);

    if (res != 0) {
        out_error = curl.easy_strerror ? curl.easy_strerror(res) : ("curl error " + std::to_string(res));
        return false;
    }
    return true;
}

#endif

} // namespace

HttpResponse HttpClient::Get(const std::string& url, const HeaderMap& headers, int timeout_ms) {
    HttpResponse resp;
    std::int64_t content_len = -1;
    ChunkCallback collector = [&resp](const std::uint8_t* data, std::size_t size) {
        resp.body.append(reinterpret_cast<const char*>(data), size);
        return true;
    };

#ifdef _WIN32
    PerformWinHttpRequest("GET", url, headers, timeout_ms, resp.status_code, resp.content_type, content_len, collector, nullptr, resp.error);
#else
    PerformLinuxCurlRequest("GET", url, headers, timeout_ms, resp.status_code, resp.content_type, content_len, collector, nullptr, resp.error);
#endif
    return resp;
}

HttpResponse HttpClient::Head(const std::string& url, const HeaderMap& headers, int timeout_ms) {
    HttpResponse resp;
    std::int64_t content_len = -1;
#ifdef _WIN32
    PerformWinHttpRequest("HEAD", url, headers, timeout_ms, resp.status_code, resp.content_type, content_len, nullptr, nullptr, resp.error);
#else
    PerformLinuxCurlRequest("HEAD", url, headers, timeout_ms, resp.status_code, resp.content_type, content_len, nullptr, nullptr, resp.error);
#endif
    return resp;
}

bool HttpClient::DownloadToFile(
    const std::string& url,
    const std::string& dest_path,
    std::string& out_error,
    const HeaderMap& headers,
    int timeout_ms) {
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
    ChunkCallback writer = [&ofs](const std::uint8_t* data, std::size_t size) {
        ofs.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
        return ofs.good();
    };

#ifdef _WIN32
    bool ok = PerformWinHttpRequest("GET", url, headers, timeout_ms, status, content_type, content_len, writer, nullptr, out_error);
#else
    bool ok = PerformLinuxCurlRequest("GET", url, headers, timeout_ms, status, content_type, content_len, writer, nullptr, out_error);
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
    int connect_timeout_ms) {
    int status = 0;
    std::string content_type;
    std::int64_t content_len = -1;

    bool header_emitted = false;
    ChunkCallback wrapped_chunk = [&](const std::uint8_t* data, std::size_t size) {
        if (!header_emitted) {
            header_emitted = true;
            if (on_headers) {
                on_headers(status, content_type, content_len);
            }
        }
        return on_chunk(data, size);
    };

#ifdef _WIN32
    bool ok = PerformWinHttpRequest("GET", url, headers, connect_timeout_ms, status, content_type, content_len, wrapped_chunk, &cancel_flag, out_error);
#else
    bool ok = PerformLinuxCurlRequest("GET", url, headers, connect_timeout_ms, status, content_type, content_len, wrapped_chunk, &cancel_flag, out_error);
#endif
    if (ok && (status < 200 || status >= 300)) {
        out_error = "HTTP status " + std::to_string(status);
        return false;
    }
    return ok;
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
            if (!ip.empty() && ip.size() <= 15) {
                return ip;
            }
        }
    }
    return "127.0.0.1";
}

} // namespace sampaudiostream
