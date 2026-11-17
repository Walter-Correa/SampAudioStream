#include "sampaudiostream/extractor/resolver.hpp"

#include "sampaudiostream/core/logger.hpp"
#include "sampaudiostream/core/thread_pool.hpp"
#include "sampaudiostream/network/http_client.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sstream>
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
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
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

bool HasExplicitUrlScheme(const std::string& input) {
    auto pos = input.find("://");
    if (pos == std::string::npos || pos == 0) return false;
    for (std::size_t i = 0; i < pos; ++i) {
        unsigned char c = static_cast<unsigned char>(input[i]);
        if (!std::isalnum(c) && c != '+' && c != '-' && c != '.') return false;
    }
    return true;
}

std::string ExtractUrlHost(const std::string& lower_url) {
    auto scheme_end = lower_url.find("://");
    std::size_t start = (scheme_end == std::string::npos) ? 0 : (scheme_end + 3);
    auto path_start = lower_url.find_first_of("/?#", start);
    std::string authority = (path_start == std::string::npos)
                                ? lower_url.substr(start)
                                : lower_url.substr(start, path_start - start);
    auto at_pos = authority.rfind('@');
    if (at_pos != std::string::npos) {
        authority = authority.substr(at_pos + 1);
    }
    if (!authority.empty() && authority.front() == '[') {
        auto close_bracket = authority.find(']');
        return (close_bracket == std::string::npos) ? authority : authority.substr(0, close_bracket + 1);
    }
    auto colon_pos = authority.find(':');
    if (colon_pos != std::string::npos) {
        authority = authority.substr(0, colon_pos);
    }
    return authority;
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
    std::string host = ExtractUrlHost(lower_url);
    if (host.empty()) return false;
    for (const char* domain : kPlatforms) {
        std::size_t dlen = std::strlen(domain);
        if (host == domain) return true;
        if (host.size() > dlen + 1 &&
            host[host.size() - dlen - 1] == '.' &&
            EndsWith(host, domain)) {
            return true;
        }
    }
    return false;
}

bool IsLivePlaylistExtension(const std::string& url) {
    std::string clean = ToLower(StripUrlQuery(url));
    return EndsWith(clean, ".pls") || EndsWith(clean, ".m3u") || EndsWith(clean, ".m3u8");
}

std::string TrimWhitespace(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::string ResolveRelativeUrl(const std::string& entry, const std::string& base_url) {
    std::string lower = ToLower(entry);
    if (lower.rfind("http://", 0) == 0 || lower.rfind("https://", 0) == 0) {
        return entry;
    }
    if (base_url.empty()) return "";
    auto scheme_end = base_url.find("://");
    if (scheme_end == std::string::npos) return "";
    if (!entry.empty() && entry.front() == '/') {
        auto host_end = base_url.find('/', scheme_end + 3);
        std::string origin = (host_end == std::string::npos) ? base_url : base_url.substr(0, host_end);
        return origin + entry;
    }
    auto last_slash = base_url.rfind('/');
    if (last_slash == std::string::npos || last_slash < scheme_end + 3) {
        return base_url + "/" + entry;
    }
    return base_url.substr(0, last_slash + 1) + entry;
}

bool IsAudioContentType(const std::string& ct_lower) {
    return ct_lower.find("audio/") != std::string::npos ||
           ct_lower.find("application/ogg") != std::string::npos ||
           ct_lower.find("audio/x-scpls") != std::string::npos ||
           ct_lower.find("audio/x-mpegurl") != std::string::npos ||
           ct_lower.find("application/vnd.apple.mpegurl") != std::string::npos;
}

bool HasAudioStreamMagic(const std::string& sample) {
    if (sample.size() < 4) return false;
    if (sample.compare(0, 3, "ID3") == 0 ||
        sample.compare(0, 4, "OggS") == 0 ||
        sample.compare(0, 4, "fLaC") == 0 ||
        sample.compare(0, 4, "RIFF") == 0 ||
        sample.compare(0, 4, "ADIF") == 0 ||
        sample.compare(0, 4, "ICY ") == 0 ||
        sample.find("#EXTM3U") != std::string::npos ||
        sample.find("[playlist]") != std::string::npos) {
        return true;
    }
    const auto b0 = static_cast<unsigned char>(sample[0]);
    const auto b1 = static_cast<unsigned char>(sample[1]);
    // MP3 / ADTS AAC frame sync (11 set bits: 0xFF followed by 0xE0+)
    if (b0 == 0xFF && (b1 & 0xE0) == 0xE0) {
        return true;
    }
    return false;
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

std::string JsonStr(const nlohmann::json& obj, const char* key, const std::string& def = "") {
    if (!obj.is_object()) return def;
    auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) return def;
    if (it->is_string()) return it->get<std::string>();
    return def;
}

bool JsonBool(const nlohmann::json& obj, const char* key, bool def = false) {
    if (!obj.is_object()) return def;
    auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) return def;
    if (it->is_boolean()) return it->get<bool>();
    return def;
}

TrackMetadata ParseTrackJson(const nlohmann::json& root, const std::string& original_url) {
    TrackMetadata meta;
    meta.original_url = original_url;

    const nlohmann::json* item = &root;
    if (root.contains("entries") && root["entries"].is_array() && !root["entries"].empty()) {
        item = &root["entries"][0];
    }

    meta.title = JsonStr(*item, "title");
    meta.artist = JsonStr(*item, "artist");
    if (meta.artist.empty()) {
        meta.artist = JsonStr(*item, "uploader");
    }
    if (meta.artist.empty()) {
        meta.artist = JsonStr(*item, "channel");
    }
    meta.extractor_name = JsonStr(*item, "extractor_key", JsonStr(*item, "extractor"));
    meta.is_live = JsonBool(*item, "is_live", false) || JsonStr(*item, "live_status") == "is_live";

    if (item->contains("duration") && (*item)["duration"].is_number()) {
        meta.duration_seconds = static_cast<std::int32_t>((*item)["duration"].get<double>());
    }

    meta.direct_media_url = JsonStr(*item, "url");
    meta.codec = JsonStr(*item, "acodec");
    meta.container_ext = JsonStr(*item, "ext");

    // Check requested_downloads / requested_formats if top-level url is absent
    if (meta.direct_media_url.empty() && item->contains("requested_formats") && (*item)["requested_formats"].is_array()) {
        for (const auto& fmt : (*item)["requested_formats"]) {
            if (JsonStr(fmt, "acodec", "none") != "none") {
                meta.direct_media_url = JsonStr(fmt, "url");
                meta.codec = JsonStr(fmt, "acodec");
                meta.container_ext = JsonStr(fmt, "ext");
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

std::string StreamResolver::ParsePlaylistFirstStreamUrl(const std::string& body, const std::string& base_url) {
    if (body.empty()) return "";
    std::string lower_body = ToLower(body);
    bool looks_pls = lower_body.find("[playlist]") != std::string::npos ||
                     lower_body.find("file1=") != std::string::npos;

    std::istringstream iss(body);
    std::string line;
    if (looks_pls) {
        while (std::getline(iss, line)) {
            std::string trimmed = TrimWhitespace(line);
            if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') continue;
            std::string lline = ToLower(trimmed);
            if (lline.rfind("file", 0) == 0) {
                auto eq = trimmed.find('=');
                if (eq != std::string::npos) {
                    std::string target = TrimWhitespace(trimmed.substr(eq + 1));
                    std::string resolved = ResolveRelativeUrl(target, base_url);
                    if (!resolved.empty()) return resolved;
                }
            }
        }
        return "";
    }

    // .m3u / .m3u8
    while (std::getline(iss, line)) {
        std::string trimmed = TrimWhitespace(line);
        if (trimmed.empty() || trimmed[0] == '#') continue;
        // Skip HLS media segments (.ts / .m4s) - let yt-dlp handle segmented HLS
        std::string ltrim = ToLower(StripUrlQuery(trimmed));
        if (EndsWith(ltrim, ".ts") || EndsWith(ltrim, ".m4s")) {
            return "";
        }
        std::string resolved = ResolveRelativeUrl(trimmed, base_url);
        if (!resolved.empty()) return resolved;
    }
    return "";
}

bool StreamResolver::IsKnownPlatformUrl(const std::string& url) {
    return IsKnownPlatformHost(ToLower(url));
}

void StreamResolver::CancelAll() {
    shutting_down_.store(true);
    PollCancellations();
}

void StreamResolver::ResetCancel() {
    shutting_down_.store(false);
}

void StreamResolver::PollCancellations() {
    std::vector<std::shared_ptr<InflightEntry<ResolveResult>>> resolve_entries;
    std::vector<std::shared_ptr<InflightEntry<SearchExecutionResult>>> search_entries;
    {
        std::lock_guard<std::mutex> lock(inflight_mutex_);
        resolve_entries.reserve(inflight_resolves_.size());
        for (const auto& [_, e] : inflight_resolves_) resolve_entries.push_back(e);
        search_entries.reserve(inflight_searches_.size());
        for (const auto& [_, e] : inflight_searches_) search_entries.push_back(e);
    }
    bool sd = shutting_down_.load();
    for (auto& e : resolve_entries) e->RefreshCombinedCancel(sd);
    for (auto& e : search_entries) e->RefreshCombinedCancel(sd);
}

bool StreamResolver::IsCancelled(const std::atomic<bool>* cancel_flag) const {
    return shutting_down_.load() || (cancel_flag && cancel_flag->load());
}

bool StreamResolver::ProbeDirectHttpAudio(
    const std::string& url,
    std::string& out_content_type,
    bool& out_security_blocked,
    const std::atomic<bool>* cancel_flag) const {
    out_security_blocked = false;
    auto resp = HttpClient::Head(url, {}, 4000, cancel_flag);
    if (resp.error == "URL not allowed") {
        out_security_blocked = true;
        return false;
    }
    if (resp.Ok()) {
        std::string ct = ToLower(resp.content_type);
        out_content_type = ct;
        if (IsAudioContentType(ct)) {
            return true;
        }
    }

    // Fallback: small GET probe (handles servers rejecting HEAD or replying with ICY 200 OK)
    std::atomic<bool> local_cancel{false};
    int get_status = 0;
    std::string get_ct;
    std::string sample_bytes;
    std::string probe_err;

    HttpClient::HeaderMap headers{{"Icy-MetaData", "1"}, {"Range", "bytes=0-2047"}};
    HttpClient::StreamGet(
        url,
        headers,
        [&](const std::uint8_t* data, std::size_t size) {
            if (cancel_flag && cancel_flag->load()) return false;
            std::size_t take = std::min<std::size_t>(size, 1024 - sample_bytes.size());
            sample_bytes.append(reinterpret_cast<const char*>(data), take);
            // Abort immediately once we have inspected the initial audio/playlist frame
            return sample_bytes.size() < 64 && (!cancel_flag || !cancel_flag->load());
        },
        cancel_flag ? *cancel_flag : local_cancel,
        probe_err,
        [&](int st, const std::string& ct, std::int64_t) {
            get_status = st;
            get_ct = ToLower(ct);
        },
        4000);

    if (probe_err == "URL not allowed") {
        out_security_blocked = true;
        return false;
    }
    if (get_status >= 200 && get_status < 300) {
        out_content_type = get_ct;
        if (IsAudioContentType(get_ct) || HasAudioStreamMagic(sample_bytes)) {
            return true;
        }
    }
    return false;
}

ResolveResult StreamResolver::Resolve(const std::string& url_or_query, const std::atomic<bool>* cancel_flag) {
    ResolveResult res;

    if (url_or_query.empty()) {
        res.error = ErrorCode::InvalidUrl;
        res.error_message = "Empty URL or query";
        return res;
    }

    if (IsCancelled(cancel_flag)) {
        res.error = ErrorCode::ExtractionFailed;
        res.error_message = "Cancelled";
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

    // Single-flight deduplication for identical in-progress resolutions
    std::shared_ptr<InflightEntry<ResolveResult>> entry;
    bool is_leader = false;
    {
        std::lock_guard<std::mutex> lock(inflight_mutex_);
        auto it = inflight_resolves_.find(url_or_query);
        if (it != inflight_resolves_.end() && !it->second->combined_cancel.load()) {
            entry = it->second;
        } else {
            entry = std::make_shared<InflightEntry<ResolveResult>>();
            inflight_resolves_[url_or_query] = entry;
            is_leader = true;
        }
        entry->AddSubscriber(cancel_flag);
    }

    if (!is_leader) {
        std::unique_lock<std::mutex> lk(entry->mutex);
        while (!entry->done && !IsCancelled(cancel_flag)) {
            entry->cv.wait_for(lk, std::chrono::milliseconds(50));
        }
        if (entry->done) {
            return entry->result;
        }
        lk.unlock();
        entry->RemoveSubscriber(cancel_flag, shutting_down_.load());
        res.error = ErrorCode::ExtractionFailed;
        res.error_message = "Cancelled";
        return res;
    }

    ResolveResult computed = ResolveUncached(url_or_query, &entry->combined_cancel);
    {
        std::lock_guard<std::mutex> lk(entry->mutex);
        entry->result = computed;
        entry->done = true;
    }
    entry->cv.notify_all();

    {
        std::lock_guard<std::mutex> lock(inflight_mutex_);
        auto it = inflight_resolves_.find(url_or_query);
        if (it != inflight_resolves_.end() && it->second == entry) {
            inflight_resolves_.erase(it);
        }
    }
    return computed;
}

ResolveResult StreamResolver::ResolveUncached(const std::string& url_or_query, const std::atomic<bool>* cancel_flag, int playlist_depth) {
    ResolveResult res;

    bool is_url = LooksLikeUrl(url_or_query);
    if (!is_url && HasExplicitUrlScheme(url_or_query)) {
        res.error = ErrorCode::InvalidUrl;
        res.error_message = "URL not allowed";
        return res;
    }

    if (is_url) {
        std::string lower = ToLower(url_or_query);
        if (!IsKnownPlatformHost(lower)) {
            std::string url_err;
            if (!HttpClient::ValidateRemoteUrl(url_or_query, url_err)) {
                res.error = (url_err == "URL not allowed") ? ErrorCode::InvalidUrl : ErrorCode::NetworkError;
                res.error_message = url_err.empty() ? "URL not allowed" : url_err;
                return res;
            }

            bool looks_playlist = IsLivePlaylistExtension(url_or_query);
            std::string probed_ct;
            bool probed_audio = false;
            bool security_blocked = false;

            if (!looks_playlist && IsKnownDirectAudioExtension(url_or_query)) {
                res.success = true;
                res.source_type = SourceType::DirectAudio;
                res.metadata.original_url = url_or_query;
                res.metadata.direct_media_url = url_or_query;
                res.metadata.extractor_name = "direct";
                res.metadata.title = "Direct Audio Stream";
                res.metadata.is_live = false;
                cache_.PutTrack(url_or_query, res.metadata);
                return res;
            }

            if (!looks_playlist) {
                probed_audio = ProbeDirectHttpAudio(url_or_query, probed_ct, security_blocked, cancel_flag);
                if (security_blocked) {
                    res.error = ErrorCode::InvalidUrl;
                    res.error_message = "URL not allowed";
                    return res;
                }
                if (probed_audio && (probed_ct.find("scpls") != std::string::npos ||
                                     probed_ct.find("mpegurl") != std::string::npos)) {
                    looks_playlist = true;
                }
            }

            if (looks_playlist && playlist_depth < 2) {
                auto pl_resp = HttpClient::Get(url_or_query, {}, 6000, cancel_flag);
                if (pl_resp.error == "URL not allowed") {
                    res.error = ErrorCode::InvalidUrl;
                    res.error_message = "URL not allowed";
                    return res;
                }
                if (pl_resp.Ok() && pl_resp.body.size() <= 65536) {
                    std::string target_url = ParsePlaylistFirstStreamUrl(pl_resp.body, url_or_query);
                    if (!target_url.empty()) {
                        ResolveResult inner = ResolveUncached(target_url, cancel_flag, playlist_depth + 1);
                        if (inner.error == ErrorCode::InvalidUrl) {
                            return inner;
                        }
                        if (inner.success) {
                            inner.metadata.original_url = url_or_query;
                            inner.metadata.is_live = true;
                            if (inner.metadata.title.empty() || inner.metadata.title == "Direct Audio Stream") {
                                inner.metadata.title = "Radio Stream";
                            }
                            cache_.PutTrack(url_or_query, inner.metadata);
                            return inner;
                        }
                    }
                }
            }

            if (probed_audio) {
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

    if (IsCancelled(cancel_flag)) {
        res.error = ErrorCode::ExtractionFailed;
        res.error_message = "Cancelled";
        return res;
    }

    if (!binaries_.IsReady()) {
        res.error = ErrorCode::ExtractorNotReady;
        res.error_message = "Extractor is still initializing";
        return res;
    }

    std::string target = is_url ? url_or_query : ("ytsearch1:" + url_or_query);
    std::string cache_dir = (std::filesystem::path(config_.bin_dir) / "yt-dlp-cache").string();

    std::vector<std::string> args = {
        "--ignore-config",
        "--dump-single-json",
        "--no-playlist",
        "--no-warnings",
        "--cache-dir", cache_dir,
        "-f", "bestaudio[ext=webm]/bestaudio[ext=m4a]/bestaudio[ext=mp3]/bestaudio[ext=ogg]/bestaudio/best"
    };

    if (!config_.cookies_file.empty() && std::filesystem::exists(config_.cookies_file)) {
        args.push_back("--cookies");
        args.push_back(config_.cookies_file);
    }

    auto t_js_start = std::chrono::steady_clock::now();
    bool using_js_runtime = false;
    if (config_.enable_js_runtime) {
        std::string js_path = binaries_.GetJsRuntimePath();
        if (!js_path.empty() && std::filesystem::exists(js_path)) {
            args.push_back("--js-runtimes");
            args.push_back("quickjs:" + js_path);
            using_js_runtime = true;
        }
    }
    auto js_check_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t_js_start).count();

    args.push_back("--");
    args.push_back(target);

    auto t_yt_start = std::chrono::steady_clock::now();
    std::string stdout_data;
    std::string stderr_data;
    bool yt_ok = ExecuteYtDlp(args, stdout_data, stderr_data, 30000, cancel_flag);
    auto yt_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t_yt_start).count();
    Logger::Instance().Debug(
        "yt-dlp resolve finished in " + std::to_string(yt_ms) + " ms (ok=" +
        std::string(yt_ok ? "true" : "false") + ", js_check=" + std::to_string(js_check_ms) +
        " ms, js_runtime=" + (using_js_runtime ? "quickjs" : "none") + ")");
    if (!yt_ok) {
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
        Logger::Instance().Debug(
            "yt-dlp selected format: ext=" + meta.container_ext +
            ", codec=" + meta.codec +
            ", live=" + std::string(meta.is_live ? "true" : "false") +
            ", url=" + StripUrlQuery(meta.direct_media_url));
        std::string media_url_err;
        if (!HttpClient::ValidateRemoteUrl(meta.direct_media_url, media_url_err)) {
            res.error = (media_url_err == "URL not allowed") ? ErrorCode::InvalidUrl : ErrorCode::NetworkError;
            res.error_message = media_url_err.empty() ? "URL not allowed" : media_url_err;
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

SearchExecutionResult StreamResolver::Search(
    const std::string& query,
    std::uint32_t max_results,
    const std::atomic<bool>* cancel_flag) {
    SearchExecutionResult res;
    if (query.empty()) {
        res.error = ErrorCode::SearchFailed;
        res.error_message = "Empty search query";
        return res;
    }

    if (IsCancelled(cancel_flag)) {
        res.error = ErrorCode::SearchFailed;
        res.error_message = "Cancelled";
        return res;
    }

    std::uint32_t limit = std::clamp<std::uint32_t>(max_results, 1, config_.max_search_results);
    std::string cache_key = std::to_string(limit) + ":" + query;

    if (auto cached = cache_.GetSearch(cache_key)) {
        res.success = true;
        res.items = *cached;
        return res;
    }

    // Single-flight deduplication for identical in-progress searches
    std::shared_ptr<InflightEntry<SearchExecutionResult>> entry;
    bool is_leader = false;
    {
        std::lock_guard<std::mutex> lock(inflight_mutex_);
        auto it = inflight_searches_.find(cache_key);
        if (it != inflight_searches_.end() && !it->second->combined_cancel.load()) {
            entry = it->second;
        } else {
            entry = std::make_shared<InflightEntry<SearchExecutionResult>>();
            inflight_searches_[cache_key] = entry;
            is_leader = true;
        }
        entry->AddSubscriber(cancel_flag);
    }

    if (!is_leader) {
        std::unique_lock<std::mutex> lk(entry->mutex);
        while (!entry->done && !IsCancelled(cancel_flag)) {
            entry->cv.wait_for(lk, std::chrono::milliseconds(50));
        }
        if (entry->done) {
            return entry->result;
        }
        lk.unlock();
        entry->RemoveSubscriber(cancel_flag, shutting_down_.load());
        res.error = ErrorCode::SearchFailed;
        res.error_message = "Cancelled";
        return res;
    }

    SearchExecutionResult computed = SearchUncached(query, limit, &entry->combined_cancel);
    {
        std::lock_guard<std::mutex> lk(entry->mutex);
        entry->result = computed;
        entry->done = true;
    }
    entry->cv.notify_all();

    {
        std::lock_guard<std::mutex> lock(inflight_mutex_);
        auto it = inflight_searches_.find(cache_key);
        if (it != inflight_searches_.end() && it->second == entry) {
            inflight_searches_.erase(it);
        }
    }
    return computed;
}

SearchExecutionResult StreamResolver::SearchUncached(
    const std::string& query,
    std::uint32_t limit,
    const std::atomic<bool>* cancel_flag) {
    SearchExecutionResult res;
    if (!binaries_.IsReady()) {
        res.error = ErrorCode::ExtractorNotReady;
        res.error_message = "Extractor is still initializing";
        return res;
    }

    std::string cache_key = std::to_string(limit) + ":" + query;
    std::string search_target = "ytsearch" + std::to_string(limit) + ":" + query;
    std::vector<std::string> args = {
        "--ignore-config",
        "--dump-single-json",
        "--flat-playlist",
        "--no-warnings",
        "--",
        search_target
    };

    std::string stdout_data;
    std::string stderr_data;
    if (!ExecuteYtDlp(args, stdout_data, stderr_data, 20000, cancel_flag)) {
        res.error = ErrorCode::SearchFailed;
        res.error_message = stderr_data.empty() ? "Search subprocess failed" : stderr_data.substr(0, 256);
        return res;
    }

    try {
        auto json = nlohmann::json::parse(stdout_data);
        if (json.contains("entries") && json["entries"].is_array()) {
            for (const auto& entry : json["entries"]) {
                SearchResultItem item;
                item.id = JsonStr(entry, "id");
                item.title = JsonStr(entry, "title", "Unknown Title");
                item.artist = JsonStr(entry, "uploader", JsonStr(entry, "channel", "Unknown Artist"));
                item.url = JsonStr(entry, "url");
                if (item.url.empty() && !item.id.empty()) {
                    item.url = "https://www.youtube.com/watch?v=" + item.id;
                }
                if (entry.contains("duration") && entry["duration"].is_number()) {
                    item.duration_seconds = static_cast<std::int32_t>(entry["duration"].get<double>());
                }
                item.is_live = JsonBool(entry, "is_live", false) || JsonStr(entry, "live_status") == "is_live";
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

bool ExecuteSubprocess(
    const std::string& exe_path,
    const std::vector<std::string>& args,
    std::string& out_stdout,
    std::string& out_stderr,
    int timeout_ms,
    const std::atomic<bool>* cancel_flag,
    std::atomic<std::uint32_t>* active_counter) {
    if (cancel_flag && cancel_flag->load()) {
        out_stderr = "Cancelled";
        return false;
    }
    if (exe_path.empty() || !std::filesystem::exists(exe_path)) {
        out_stderr = "Executable does not exist: " + exe_path;
        return false;
    }

    struct ActiveProcGuard {
        std::atomic<std::uint32_t>* counter;
        explicit ActiveProcGuard(std::atomic<std::uint32_t>* c) : counter(c) {
            if (counter) counter->fetch_add(1);
        }
        ~ActiveProcGuard() {
            if (counter) counter->fetch_sub(1);
        }
    } proc_guard(active_counter);

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
        if (stderr_read) CloseHandle(stderr_read);
        if (stderr_write) CloseHandle(stderr_write);
        out_stderr = "Failed to create pipes";
        return false;
    }

    SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);

    HANDLE nul_in = CreateFileW(
        L"NUL",
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &sa,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    HANDLE inherit_handles[3] = {stdout_write, stderr_write, nul_in};
    DWORD inherit_count = (nul_in && nul_in != INVALID_HANDLE_VALUE) ? 3 : 2;

    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<std::uint8_t> attr_buf(attr_size);
    auto* attr_list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    bool has_attr_list = InitializeProcThreadAttributeList(attr_list, 1, 0, &attr_size) &&
                         UpdateProcThreadAttribute(
                             attr_list,
                             0,
                             PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                             inherit_handles,
                             inherit_count * sizeof(HANDLE),
                             nullptr,
                             nullptr);

    STARTUPINFOEXW siex{};
    siex.StartupInfo.cb = has_attr_list ? sizeof(STARTUPINFOEXW) : sizeof(STARTUPINFOW);
    siex.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    siex.StartupInfo.wShowWindow = SW_HIDE;
    siex.StartupInfo.hStdOutput = stdout_write;
    siex.StartupInfo.hStdError = stderr_write;
    siex.StartupInfo.hStdInput = (nul_in && nul_in != INVALID_HANDLE_VALUE) ? nul_in : nullptr;
    siex.lpAttributeList = has_attr_list ? attr_list : nullptr;

    std::wstring cmdline = QuoteWindowsArg(Utf8ToWide(exe_path));
    for (const auto& a : args) {
        cmdline += L" " + QuoteWindowsArg(Utf8ToWide(a));
    }

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
    }

    DWORD create_flags = CREATE_NO_WINDOW | CREATE_SUSPENDED;
    if (has_attr_list) {
        create_flags |= EXTENDED_STARTUPINFO_PRESENT;
    }

    PROCESS_INFORMATION pi{};
    BOOL created = CreateProcessW(
        nullptr,
        cmdline.data(),
        nullptr,
        nullptr,
        TRUE,
        create_flags,
        nullptr,
        nullptr,
        &siex.StartupInfo,
        &pi);

    if (has_attr_list) {
        DeleteProcThreadAttributeList(attr_list);
    }
    CloseHandle(stdout_write);
    CloseHandle(stderr_write);
    if (nul_in && nul_in != INVALID_HANDLE_VALUE) {
        CloseHandle(nul_in);
    }

    if (!created) {
        if (job) CloseHandle(job);
        CloseHandle(stdout_read);
        CloseHandle(stderr_read);
        out_stderr = "CreateProcessW failed with error " + std::to_string(GetLastError());
        return false;
    }

    if (job) {
        AssignProcessToJobObject(job, pi.hProcess);
    }
    ResumeThread(pi.hThread);

    ManagedThread t_out;
    ManagedThread t_err;
    t_out.Start([stdout_read, &out_stdout]() { DrainPipe(stdout_read, out_stdout); }, 128 * 1024);
    t_err.Start([stderr_read, &out_stderr]() { DrainPipe(stderr_read, out_stderr); }, 128 * 1024);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    bool ok = false;
    while (true) {
        if (cancel_flag && cancel_flag->load()) {
            if (job) TerminateJobObject(job, 1);
            TerminateProcess(pi.hProcess, 1);
            out_stderr = "Cancelled";
            break;
        }
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            if (job) TerminateJobObject(job, 1);
            TerminateProcess(pi.hProcess, 1);
            out_stderr = "Extractor timed out";
            break;
        }
        DWORD wait_ms = static_cast<DWORD>(std::min<long long>(
            100,
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count()));
        DWORD wait_res = WaitForSingleObject(pi.hProcess, wait_ms);
        if (wait_res == WAIT_OBJECT_0) {
            DWORD exit_code = 1;
            GetExitCodeProcess(pi.hProcess, &exit_code);
            ok = (exit_code == 0);
            break;
        }
        if (wait_res != WAIT_TIMEOUT) {
            break;
        }
    }

    if (job) CloseHandle(job);
    WaitForSingleObject(pi.hProcess, 1000);
    t_out.Join();
    t_err.Join();

    CloseHandle(stdout_read);
    CloseHandle(stderr_read);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return ok;
#else
    int out_pipe[2]{-1, -1};
    int err_pipe[2]{-1, -1};
    if (pipe2(out_pipe, O_CLOEXEC) != 0 || pipe2(err_pipe, O_CLOEXEC) != 0) {
        if (out_pipe[0] != -1) { close(out_pipe[0]); close(out_pipe[1]); }
        if (err_pipe[0] != -1) { close(err_pipe[0]); close(err_pipe[1]); }
        out_stderr = "pipe2() failed";
        return false;
    }

    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attr;
    posix_spawn_file_actions_init(&actions);
    posix_spawnattr_init(&attr);

    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, err_pipe[1], STDERR_FILENO);

    short flags = POSIX_SPAWN_SETPGROUP;
#ifdef POSIX_SPAWN_USEVFORK
    flags |= POSIX_SPAWN_USEVFORK;
#endif
    posix_spawnattr_setflags(&attr, flags);
    posix_spawnattr_setpgroup(&attr, 0);

    std::vector<char*> argv;
    argv.reserve(args.size() + 2);
    argv.push_back(const_cast<char*>(exe_path.c_str()));
    for (const auto& a : args) {
        argv.push_back(const_cast<char*>(a.c_str()));
    }
    argv.push_back(nullptr);

    pid_t pid = -1;
    int spawn_rc = posix_spawnp(&pid, exe_path.c_str(), &actions, &attr, argv.data(), environ);

    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    close(out_pipe[1]);
    close(err_pipe[1]);

    if (spawn_rc != 0 || pid <= 0) {
        close(out_pipe[0]);
        close(err_pipe[0]);
        out_stderr = "posix_spawnp() failed with code " + std::to_string(spawn_rc);
        return false;
    }

    fcntl(out_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(err_pipe[0], F_SETFL, O_NONBLOCK);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    bool out_open = true;
    bool err_open = true;
    std::array<char, 4096> buf{};

    while (out_open || err_open) {
        if (cancel_flag && cancel_flag->load()) {
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
            out_stderr = "Cancelled";
            break;
        }
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            kill(-pid, SIGKILL);
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
    auto wait_deadline = std::min(
        deadline,
        std::chrono::steady_clock::now() + std::chrono::milliseconds(2000));
    pid_t wp = 0;
    while (true) {
        wp = waitpid(pid, &status, WNOHANG);
        if (wp == pid || wp < 0) {
            break;
        }
        if ((cancel_flag && cancel_flag->load()) || std::chrono::steady_clock::now() >= wait_deadline) {
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return wp == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}

bool StreamResolver::ExecuteYtDlp(
    const std::vector<std::string>& args,
    std::string& out_stdout,
    std::string& out_stderr,
    int timeout_ms,
    const std::atomic<bool>* cancel_flag) const {
    if (IsCancelled(cancel_flag)) {
        out_stderr = "Cancelled";
        return false;
    }
    if (!binaries_.IsReady()) {
        out_stderr = "yt-dlp binary is currently unavailable";
        return false;
    }
    return ExecuteSubprocess(
        binaries_.GetYtDlpPath(),
        args,
        out_stdout,
        out_stderr,
        timeout_ms,
        cancel_flag,
        &active_processes_);
}

} // namespace sampaudiostream
