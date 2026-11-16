#include "sampaudiostream/extractor/resolver.hpp"

#include "sampaudiostream/core/logger.hpp"
#include "sampaudiostream/network/http_client.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace sampaudiostream {

namespace {

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

std::string StripUrlQuery(const std::string& url) {
    auto pos = url.find_first_of("?#");
    if (pos == std::string::npos) return url;
    return url.substr(0, pos);
}

bool EndsWith(const std::string& str, const std::string& suffix) {
    if (str.size() < suffix.size()) return false;
    return str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool IsKnownPlatformHost(const std::string& lower_url) {
    static const char* kPlatforms[] = {
        "youtube.com", "youtu.be", "music.youtube.com",
        "soundcloud.com", "sndcdn.com",
        "twitch.tv", "kick.com",
        "vimeo.com", "dailymotion.com",
        "tiktok.com", "instagram.com", "facebook.com", "fb.watch",
        "twitter.com", "x.com", "reddit.com",
        "bandcamp.com", "mixcloud.com", "audiomack.com",
        "bilibili.com", "nicovideo.jp", "vk.com"
    };
    for (const char* host : kPlatforms) {
        if (lower_url.find(host) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool IsLivePlaylistExtension(const std::string& url) {
    std::string clean = ToLower(StripUrlQuery(url));
    return EndsWith(clean, ".pls") || EndsWith(clean, ".m3u");
}

#ifdef _WIN32

std::wstring Utf8ToWide(const std::string& str) {
    if (str.empty()) return L"";
    int needed = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), nullptr, 0);
    if (needed <= 0) return L"";
    std::wstring res(needed, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), res.data(), needed);
    return res;
}

// Safely quotes an argument according to Windows CreateProcess CommandLineToArgvW rules
std::wstring QuoteWindowsArg(const std::wstring& arg) {
    if (arg.empty()) return L"\"\"";
    if (arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return arg;
    }
    std::wstring out = L"\"";
    for (auto it = arg.begin();; ++it) {
        std::size_t backslashes = 0;
        while (it != arg.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == arg.end()) {
            out.append(backslashes * 2, L'\\');
            break;
        } else if (*it == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
        } else {
            out.append(backslashes, L'\\');
            out.push_back(*it);
        }
    }
    out.push_back(L'"');
    return out;
}

void DrainPipe(HANDLE hPipe, std::string& out) {
    std::array<char, 4096> buf{};
    DWORD read_bytes = 0;
    while (ReadFile(hPipe, buf.data(), static_cast<DWORD>(buf.size()), &read_bytes, nullptr) && read_bytes > 0) {
        out.append(buf.data(), read_bytes);
    }
}

#endif

TrackMetadata ParseTrackJson(const nlohmann::json& root, const std::string& original_url) {
    TrackMetadata meta;
    meta.original_url = original_url;

    const nlohmann::json* item = &root;
    if (root.contains("entries") && root["entries"].is_array() && !root["entries"].empty()) {
        item = &root["entries"][0];
    }

    meta.title = item->value("title", "");
    meta.artist = item->value("artist", "");
    if (meta.artist.empty()) {
        meta.artist = item->value("uploader", "");
    }
    if (meta.artist.empty()) {
        meta.artist = item->value("channel", "");
    }
    meta.extractor_name = item->value("extractor_key", item->value("extractor", ""));
    meta.is_live = item->value("is_live", false);

    if (item->contains("duration") && (*item)["duration"].is_number()) {
        meta.duration_seconds = static_cast<std::int32_t>((*item)["duration"].get<double>());
    }

    meta.direct_media_url = item->value("url", "");
    meta.codec = item->value("acodec", "");
    meta.container_ext = item->value("ext", "");

    // Check requested_downloads / requested_formats if top-level url is absent
    if (meta.direct_media_url.empty() && item->contains("requested_formats") && (*item)["requested_formats"].is_array()) {
        for (const auto& fmt : (*item)["requested_formats"]) {
            if (fmt.value("acodec", "none") != "none") {
                meta.direct_media_url = fmt.value("url", "");
                meta.codec = fmt.value("acodec", "");
                meta.container_ext = fmt.value("ext", "");
                if (fmt.contains("http_headers") && fmt["http_headers"].is_object()) {
                    for (auto& [k, v] : fmt["http_headers"].items()) {
                        if (v.is_string()) meta.http_headers[k] = v.get<std::string>();
                    }
                }
                break;
            }
        }
    }

    if (meta.http_headers.empty() && item->contains("http_headers") && (*item)["http_headers"].is_object()) {
        for (auto& [k, v] : (*item)["http_headers"].items()) {
            if (v.is_string()) meta.http_headers[k] = v.get<std::string>();
        }
    }

    return meta;
}

} // namespace

StreamResolver::StreamResolver(const PluginConfig& config, BinaryManager& binaries, MetadataCache& cache)
    : config_(config), binaries_(binaries), cache_(cache) {}

bool StreamResolver::LooksLikeUrl(const std::string& input) {
    std::string lower = ToLower(input);
    return lower.rfind("http://", 0) == 0 || lower.rfind("https://", 0) == 0;
}

bool StreamResolver::IsKnownDirectAudioExtension(const std::string& url) {
    std::string clean = ToLower(StripUrlQuery(url));
    return EndsWith(clean, ".mp3") || EndsWith(clean, ".ogg") ||
           EndsWith(clean, ".wav") || EndsWith(clean, ".flac") ||
           EndsWith(clean, ".pls") || EndsWith(clean, ".m3u") ||
           EndsWith(clean, ".aac");
}

bool StreamResolver::ProbeDirectHttpAudio(const std::string& url, std::string& out_content_type) const {
    auto resp = HttpClient::Head(url, {}, 4000);
    if (!resp.Ok()) {
        return false;
    }
    std::string ct = ToLower(resp.content_type);
    out_content_type = ct;
    return ct.find("audio/") != std::string::npos ||
           ct.find("application/ogg") != std::string::npos ||
           ct.find("audio/x-scpls") != std::string::npos ||
           ct.find("audio/x-mpegurl") != std::string::npos;
}

ResolveResult StreamResolver::Resolve(const std::string& url_or_query) {
    ResolveResult res;

    if (url_or_query.empty()) {
        res.error = ErrorCode::InvalidUrl;
        res.error_message = "Empty URL or query";
        return res;
    }

    // Fast path: check LRU cache first
    if (auto cached = cache_.GetTrack(url_or_query)) {
        res.success = true;
        res.source_type = IsKnownDirectAudioExtension(cached->direct_media_url) && cached->extractor_name == "direct"
                              ? SourceType::DirectAudio
                              : SourceType::ExtractorStream;
        res.metadata = *cached;
        return res;
    }

    bool is_url = LooksLikeUrl(url_or_query);
    if (is_url) {
        std::string lower = ToLower(url_or_query);
        if (!IsKnownPlatformHost(lower)) {
            if (IsKnownDirectAudioExtension(url_or_query)) {
                res.success = true;
                res.source_type = SourceType::DirectAudio;
                res.metadata.original_url = url_or_query;
                res.metadata.direct_media_url = url_or_query;
                res.metadata.extractor_name = "direct";
                res.metadata.title = IsLivePlaylistExtension(url_or_query) ? "Radio Stream" : "Direct Audio Stream";
                res.metadata.is_live = IsLivePlaylistExtension(url_or_query);
                cache_.PutTrack(url_or_query, res.metadata);
                return res;
            }

            // Probe unknown HTTP URLs (e.g. Shoutcast/Icecast ports like http://ip:8000/stream)
            std::string ct;
            if (ProbeDirectHttpAudio(url_or_query, ct)) {
                res.success = true;
                res.source_type = SourceType::DirectAudio;
                res.metadata.original_url = url_or_query;
                res.metadata.direct_media_url = url_or_query;
                res.metadata.extractor_name = "direct";
                res.metadata.title = "Radio Stream";
                res.metadata.is_live = true;
                cache_.PutTrack(url_or_query, res.metadata);
                return res;
            }
        }
    }

    if (!binaries_.IsReady()) {
        res.error = ErrorCode::ExtractorNotReady;
        res.error_message = "Extractor is still initializing";
        return res;
    }

    std::string target = is_url ? url_or_query : ("ytsearch1:" + url_or_query);

    std::vector<std::string> args = {
        "--dump-single-json",
        "--no-playlist",
        "--no-warnings",
        "--no-check-certificates",
        "-f", "bestaudio[ext=webm]/bestaudio[ext=m4a]/bestaudio[ext=mp3]/bestaudio[ext=ogg]/bestaudio/best"
    };

    if (!config_.cookies_file.empty() && std::filesystem::exists(config_.cookies_file)) {
        args.push_back("--cookies");
        args.push_back(config_.cookies_file);
    }

    if (config_.enable_js_runtime) {
        std::string js_path = binaries_.GetJsRuntimePath();
        if (!js_path.empty() && std::filesystem::exists(js_path)) {
            args.push_back("--js-runtimes");
            args.push_back("quickjs:" + js_path);
        }
    }

    args.push_back(target);

    std::string stdout_data;
    std::string stderr_data;
    if (!ExecuteYtDlp(args, stdout_data, stderr_data, 30000)) {
        res.error = ErrorCode::ExtractionFailed;
        res.error_message = stderr_data.empty() ? "yt-dlp execution failed" : stderr_data.substr(0, 256);
        return res;
    }

    try {
        auto json = nlohmann::json::parse(stdout_data);
        TrackMetadata meta = ParseTrackJson(json, url_or_query);
        if (meta.direct_media_url.empty()) {
            res.error = ErrorCode::ExtractionFailed;
            res.error_message = "No playable audio format returned by extractor";
            return res;
        }

        res.success = true;
        res.source_type = SourceType::ExtractorStream;
        res.metadata = std::move(meta);
        cache_.PutTrack(url_or_query, res.metadata);
        return res;
    } catch (const std::exception& ex) {
        res.error = ErrorCode::ExtractionFailed;
        res.error_message = std::string("JSON parse error: ") + ex.what();
        return res;
    }
}

SearchExecutionResult StreamResolver::Search(const std::string& query, std::uint32_t max_results) {
    SearchExecutionResult res;
    if (query.empty()) {
        res.error = ErrorCode::SearchFailed;
        res.error_message = "Empty search query";
        return res;
    }

    std::uint32_t limit = std::clamp<std::uint32_t>(max_results, 1, config_.max_search_results);
    std::string cache_key = std::to_string(limit) + ":" + query;

    if (auto cached = cache_.GetSearch(cache_key)) {
        res.success = true;
        res.items = *cached;
        return res;
    }

    if (!binaries_.IsReady()) {
        res.error = ErrorCode::ExtractorNotReady;
        res.error_message = "Extractor is still initializing";
        return res;
    }

    std::string search_target = "ytsearch" + std::to_string(limit) + ":" + query;
    std::vector<std::string> args = {
        "--dump-single-json",
        "--flat-playlist",
        "--no-warnings",
        "--no-check-certificates",
        search_target
    };

    std::string stdout_data;
    std::string stderr_data;
    if (!ExecuteYtDlp(args, stdout_data, stderr_data, 20000)) {
        res.error = ErrorCode::SearchFailed;
        res.error_message = stderr_data.empty() ? "Search subprocess failed" : stderr_data.substr(0, 256);
        return res;
    }

    try {
        auto json = nlohmann::json::parse(stdout_data);
        if (json.contains("entries") && json["entries"].is_array()) {
            for (const auto& entry : json["entries"]) {
                SearchResultItem item;
                item.id = entry.value("id", "");
                item.title = entry.value("title", "Unknown Title");
                item.artist = entry.value("uploader", entry.value("channel", "Unknown Artist"));
                item.url = entry.value("url", "");
                if (item.url.empty() && !item.id.empty()) {
                    item.url = "https://www.youtube.com/watch?v=" + item.id;
                }
                if (entry.contains("duration") && entry["duration"].is_number()) {
                    item.duration_seconds = static_cast<std::int32_t>(entry["duration"].get<double>());
                }
                item.is_live = entry.value("is_live", false) || entry.value("live_status", "") == "is_live";
                res.items.push_back(std::move(item));
            }
        }
        res.success = true;
        cache_.PutSearch(cache_key, res.items);
        return res;
    } catch (const std::exception& ex) {
        res.error = ErrorCode::SearchFailed;
        res.error_message = std::string("Search parse error: ") + ex.what();
        return res;
    }
}

bool StreamResolver::ExecuteYtDlp(
    const std::vector<std::string>& args,
    std::string& out_stdout,
    std::string& out_stderr,
    int timeout_ms) const {
    std::string exe_path = binaries_.GetYtDlpPath();
    if (exe_path.empty() || !std::filesystem::exists(exe_path)) {
        out_stderr = "yt-dlp binary does not exist";
        return false;
    }

#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE stdout_read = nullptr, stdout_write = nullptr;
    HANDLE stderr_read = nullptr, stderr_write = nullptr;

    if (!CreatePipe(&stdout_read, &stdout_write, &sa, 0) ||
        !CreatePipe(&stderr_read, &stderr_write, &sa, 0)) {
        if (stdout_read) CloseHandle(stdout_read);
        if (stdout_write) CloseHandle(stdout_write);
        out_stderr = "Failed to create pipes";
        return false;
    }

    SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = stdout_write;
    si.hStdError = stderr_write;
    si.hStdInput = nullptr;

    std::wstring cmdline = QuoteWindowsArg(Utf8ToWide(exe_path));
    for (const auto& a : args) {
        cmdline += L" " + QuoteWindowsArg(Utf8ToWide(a));
    }

    PROCESS_INFORMATION pi{};
    BOOL created = CreateProcessW(
        nullptr,
        cmdline.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &si,
        &pi);

    CloseHandle(stdout_write);
    CloseHandle(stderr_write);

    if (!created) {
        CloseHandle(stdout_read);
        CloseHandle(stderr_read);
        out_stderr = "CreateProcessW failed with error " + std::to_string(GetLastError());
        return false;
    }

    std::thread t_out([stdout_read, &out_stdout]() { DrainPipe(stdout_read, out_stdout); });
    std::thread t_err([stderr_read, &out_stderr]() { DrainPipe(stderr_read, out_stderr); });

    DWORD wait_res = WaitForSingleObject(pi.hProcess, static_cast<DWORD>(timeout_ms));
    bool ok = false;
    if (wait_res == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        out_stderr = "Extractor timed out";
    } else {
        DWORD exit_code = 1;
        GetExitCodeProcess(pi.hProcess, &exit_code);
        ok = (exit_code == 0);
    }

    CloseHandle(stdout_read);
    CloseHandle(stderr_read);
    if (t_out.joinable()) t_out.join();
    if (t_err.joinable()) t_err.join();

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return ok;
#else
    int out_pipe[2]{-1, -1};
    int err_pipe[2]{-1, -1};
    if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
        if (out_pipe[0] != -1) { close(out_pipe[0]); close(out_pipe[1]); }
        out_stderr = "pipe() failed";
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        out_stderr = "fork() failed";
        return false;
    }

    if (pid == 0) {
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);

        std::vector<char*> argv;
        argv.reserve(args.size() + 2);
        argv.push_back(const_cast<char*>(exe_path.c_str()));
        for (const auto& a : args) {
            argv.push_back(const_cast<char*>(a.c_str()));
        }
        argv.push_back(nullptr);

        execvp(exe_path.c_str(), argv.data());
        _exit(127);
    }

    close(out_pipe[1]);
    close(err_pipe[1]);

    fcntl(out_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(err_pipe[0], F_SETFL, O_NONBLOCK);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    bool out_open = true;
    bool err_open = true;
    std::array<char, 4096> buf{};

    while (out_open || err_open) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            kill(pid, SIGKILL);
            out_stderr = "Extractor timed out";
            break;
        }

        struct pollfd fds[2]{};
        nfds_t nfds = 0;
        if (out_open) fds[nfds++] = {out_pipe[0], POLLIN, 0};
        if (err_open) fds[nfds++] = {err_pipe[0], POLLIN, 0};

        int remain_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        int pr = poll(fds, nfds, std::min(remain_ms, 200));
        if (pr < 0) break;

        for (nfds_t i = 0; i < nfds; ++i) {
            if (fds[i].revents & (POLLIN | POLLHUP)) {
                std::string& dst = (fds[i].fd == out_pipe[0]) ? out_stdout : out_stderr;
                ssize_t n = read(fds[i].fd, buf.data(), buf.size());
                if (n > 0) {
                    dst.append(buf.data(), static_cast<std::size_t>(n));
                } else if (n == 0) {
                    if (fds[i].fd == out_pipe[0]) out_open = false;
                    else err_open = false;
                }
            }
        }
    }

    close(out_pipe[0]);
    close(err_pipe[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}

} // namespace sampaudiostream
