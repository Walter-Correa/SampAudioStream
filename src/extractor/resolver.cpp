#include "sampaudiostream/extractor/resolver.hpp"

#include "sampaudiostream/audio/sound_file.hpp"
#include "sampaudiostream/audio/transcoder.hpp"
#include "sampaudiostream/core/config.hpp"
#include "sampaudiostream/core/logger.hpp"
#include "sampaudiostream/core/thread_pool.hpp"
#include "sampaudiostream/extractor/browser_cookies.hpp"
#include "sampaudiostream/network/http_client.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
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
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace sampaudiostream {

namespace {

const char* ExtractorUnavailableMessage(const BinaryManager& binaries) {
    return binaries.HasProvisioningFailed()
        ? "Extractor unavailable: yt-dlp could not be provisioned (see server log)"
        : "Extractor is still initializing";
}

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

std::string ExtractQueryParam(const std::string& url, const char* key) {
    if (!key || !*key) return "";
    auto qpos = url.find('?');
    if (qpos == std::string::npos) return "";
    auto hash_pos = url.find('#', qpos + 1);
    std::string query = (hash_pos == std::string::npos)
                            ? url.substr(qpos + 1)
                            : url.substr(qpos + 1, hash_pos - (qpos + 1));
    const std::string key_str(key);
    std::size_t pos = 0;
    while (pos <= query.size()) {
        auto amp = query.find('&', pos);
        std::string pair = (amp == std::string::npos) ? query.substr(pos) : query.substr(pos, amp - pos);
        auto eq = pair.find('=');
        if (eq != std::string::npos && pair.substr(0, eq) == key_str) {
            return pair.substr(eq + 1);
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return "";
}

bool IsDenoRuntimeFailure(const std::string& stderr_text) {
    if (stderr_text.empty()) return false;
    const std::string lower = ToLower(stderr_text);
    // Do not treat video/availability/auth errors as JS runtime crashes.
    if (lower.find("video unavailable") != std::string::npos ||
        lower.find("private video") != std::string::npos ||
        lower.find("members-only") != std::string::npos ||
        lower.find("sign in to confirm") != std::string::npos ||
        lower.find("this live event") != std::string::npos) {
        return false;
    }
    return lower.find("deno") != std::string::npos ||
           lower.find("ejs") != std::string::npos ||
           lower.find("js challenge") != std::string::npos ||
           lower.find("challenge solver") != std::string::npos ||
           lower.find("javascript runtime") != std::string::npos ||
           lower.find("no supported js runtime") != std::string::npos ||
           lower.find("glibc_") != std::string::npos;
}

std::string RuntimeKindLabel(JsRuntimeKind kind) {
    switch (kind) {
        case JsRuntimeKind::Deno: return "deno";
        case JsRuntimeKind::QuickJs: return "quickjs";
        case JsRuntimeKind::None:
        default: return "none";
    }
}

constexpr std::size_t kMaxSubprocessStderrBytes = 64 * 1024;

std::string NormalizeForAuthCheck(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        const unsigned char c0 = static_cast<unsigned char>(raw[i]);
        // Normalize UTF-8 RIGHT SINGLE QUOTATION MARK (U+2019: E2 80 99) to ASCII apostrophe.
        if (c0 == 0xE2 && (i + 2) < raw.size() &&
            static_cast<unsigned char>(raw[i + 1]) == 0x80 &&
            static_cast<unsigned char>(raw[i + 2]) == 0x99) {
            out.push_back('\'');
            i += 2;
            continue;
        }
        out.push_back(static_cast<char>(std::tolower(c0)));
    }
    return out;
}

std::string ScrubCookieValuesImpl(const std::string& raw) {
    if (raw.empty()) {
        return raw;
    }
    std::string out = raw;

    // 1. Redact HTTP Cookie / Set-Cookie header lines/values.
    {
        std::size_t cursor = 0;
        while (cursor < out.size()) {
            std::string lower;
            lower.reserve(out.size() - cursor);
            for (std::size_t i = cursor; i < out.size(); ++i) {
                lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(out[i]))));
            }
            const std::size_t p_sc = lower.find("set-cookie:");
            const std::size_t p_ck = lower.find("cookie:");
            std::size_t rel = std::string::npos;
            std::size_t hdr_len = 0;
            if (p_sc != std::string::npos && (p_ck == std::string::npos || p_sc <= p_ck)) {
                rel = p_sc;
                hdr_len = 11;
            } else if (p_ck != std::string::npos) {
                rel = p_ck;
                hdr_len = 7;
            }
            if (rel == std::string::npos) {
                break;
            }
            const std::size_t abs_pos = cursor + rel;
            const std::size_t val_start = abs_pos + hdr_len;
            const std::size_t line_end = out.find_first_of("\r\n", val_start);
            const std::size_t rep_end = (line_end == std::string::npos) ? out.size() : line_end;
            static constexpr const char kRedacted[] = " [redacted]";
            out.replace(val_start, rep_end - val_start, kRedacted);
            cursor = val_start + sizeof(kRedacted) - 1;
        }
    }

    // 2. Redact known YouTube / Google / session cookie NAME=VALUE tokens.
    static constexpr const char* kCookieNames[] = {
        "__secure-1papisid", "__secure-3papisid", "__secure-1psidts", "__secure-3psidts",
        "__secure-1psid", "__secure-3psid", "__secure-", "__host-",
        "visitor_privacy_metadata", "visitor_info1_live", "login_info",
        "phpsessid", "sessionid", "csrftoken",
        "sapisid", "apisid", "sidcc", "hsid", "ssid", "pref", "socs", "consent", "sid", "nid", "ysc"
    };
    const auto is_ident_char = [](unsigned char ch) {
        return std::isalnum(ch) || ch == '_' || ch == '-';
    };
    std::size_t pos = 0;
    while (pos < out.size()) {
        const std::size_t eq = out.find('=', pos);
        if (eq == std::string::npos) {
            break;
        }
        std::size_t name_start = eq;
        while (name_start > 0 && is_ident_char(static_cast<unsigned char>(out[name_start - 1]))) {
            --name_start;
        }
        std::string name_lower;
        name_lower.reserve(eq - name_start);
        for (std::size_t i = name_start; i < eq; ++i) {
            name_lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(out[i]))));
        }
        bool matched = false;
        if (!name_lower.empty()) {
            for (const char* cname : kCookieNames) {
                const std::size_t clen = std::strlen(cname);
                if (cname[clen - 1] == '-') {
                    if (name_lower.rfind(cname, 0) == 0) {
                        matched = true;
                        break;
                    }
                } else if (name_lower == cname) {
                    matched = true;
                    break;
                }
            }
        }
        if (matched && (eq + 1) < out.size()) {
            std::size_t val_end = out.find_first_of(" \t\r\n;\"',)", eq + 1);
            if (val_end == std::string::npos) {
                val_end = out.size();
            }
            if (val_end > eq + 1) {
                static constexpr const char kRedactedVal[] = "[redacted]";
                out.replace(eq + 1, val_end - (eq + 1), kRedactedVal);
                pos = eq + 1 + sizeof(kRedactedVal) - 1;
                continue;
            }
        }
        pos = eq + 1;
    }

    return out;
}

std::string SanitizeWarningLineForLog(const std::string& raw_line) {
    std::string out = ScrubCookieValuesImpl(raw_line);
    for (char& ch : out) {
        const unsigned char u = static_cast<unsigned char>(ch);
        if (u < 0x20 || u == 0x7f) ch = ' ';
    }

    // Strip query strings / fragments from any HTTP(S) URLs embedded in warning text.
    std::size_t cursor = 0;
    while (cursor < out.size()) {
        std::string lower = ToLower(out.substr(cursor));
        std::size_t rel_http = lower.find("http://");
        std::size_t rel_https = lower.find("https://");
        std::size_t rel_pos = std::min(rel_http, rel_https);
        if (rel_pos == std::string::npos) {
            break;
        }
        std::size_t url_start = cursor + rel_pos;
        std::size_t url_end = out.find_first_of(" \t\r\n\"'<>)]", url_start);
        if (url_end == std::string::npos) {
            url_end = out.size();
        }
        std::size_t qpos = out.find_first_of("?#", url_start);
        if (qpos != std::string::npos && qpos < url_end) {
            out.erase(qpos, url_end - qpos);
            cursor = qpos;
        } else {
            cursor = url_end;
        }
    }

    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    if (out.size() > 200) {
        out.resize(200);
    }
    return out;
}

bool IsJsChallengeWarningLine(const std::string& warning_line) {
    const std::string lower = ToLower(warning_line);
    return lower.find("challenge") != std::string::npos ||
           lower.find("jsc") != std::string::npos ||
           lower.find("n challenge") != std::string::npos ||
           lower.find("signature") != std::string::npos ||
           lower.find("some formats may be missing") != std::string::npos ||
           lower.find("js runtime") != std::string::npos;
}

std::string SummarizeBrowserCookieFailure(const std::string& stderr_text) {
    if (StreamResolver::IsYouTubeAuthChallenge(stderr_text)) {
        return "no signed-in YouTube session (auth challenge persisted)";
    }
    const std::string lower = ToLower(stderr_text);
    if (lower.find("app-bound") != std::string::npos ||
        lower.find("app_bound") != std::string::npos ||
        lower.find("dpapi") != std::string::npos ||
        lower.find("decrypt") != std::string::npos) {
        return "cookie decryption failed (DPAPI / App-Bound encryption)";
    }
    if (lower.find("could not copy") != std::string::npos) {
        return "cookie database is locked by running browser (Could not copy)";
    }
    if (lower.find("keyring") != std::string::npos ||
        lower.find("secretstorage") != std::string::npos ||
        lower.find("libsecret") != std::string::npos ||
        lower.find("kwallet") != std::string::npos) {
        return "OS keyring unavailable in current session";
    }
    if (lower.find("permission denied") != std::string::npos) {
        return "permission denied reading profile directory";
    }
    if (stderr_text.empty()) {
        return "yt-dlp failed without output";
    }
    std::string brief = SanitizeWarningLineForLog(stderr_text);
    if (brief.size() > 120) {
        brief.resize(120);
    }
    return brief;
}

bool IsVerboseYtDlpLine(const std::string& line) {
    if (line.empty()) {
        return false;
    }
    const std::string lower = ToLower(line);
    if (lower.find("[debug] js runtimes:") != std::string::npos ||
        lower.find("js challenge providers") != std::string::npos ||
        lower.find("[jsc") != std::string::npos ||
        lower.find("solving js challenges") != std::string::npos ||
        lower.find("running deno") != std::string::npos ||
        lower.find("running quickjs") != std::string::npos ||
        lower.find("po token") != std::string::npos ||
        lower.find("player api json") != std::string::npos ||
        lower.find("downloading player") != std::string::npos ||
        lower.find("[debug] proxy map:") != std::string::npos ||
        lower.find("[debug] request handlers:") != std::string::npos ||
        lower.find("[debug] python") != std::string::npos ||
        lower.find("[debug] yt-dlp version") != std::string::npos) {
        return true;
    }
    return line.find("Forcing") != std::string::npos ||
           line.find("WARNING") != std::string::npos ||
           line.find("ERROR") != std::string::npos;
}

std::string ExtractQueryParamNamesOrdered(const std::string& url) {
    auto qpos = url.find('?');
    if (qpos == std::string::npos) {
        return "";
    }
    auto hash_pos = url.find('#', qpos + 1);
    std::string query = (hash_pos == std::string::npos)
                            ? url.substr(qpos + 1)
                            : url.substr(qpos + 1, hash_pos - (qpos + 1));
    std::string names;
    std::size_t pos = 0;
    while (pos <= query.size()) {
        auto amp = query.find('&', pos);
        std::string pair = (amp == std::string::npos) ? query.substr(pos) : query.substr(pos, amp - pos);
        auto eq = pair.find('=');
        std::string key = (eq == std::string::npos) ? pair : pair.substr(0, eq);
        if (!key.empty()) {
            if (!names.empty()) names.push_back(',');
            names += key;
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return names;
}

std::string UrlDecodeSimple(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() &&
            std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            auto hex_val = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
                return 10 + (c - 'A');
            };
            out.push_back(static_cast<char>((hex_val(s[i + 1]) << 4) | hex_val(s[i + 2])));
            i += 2;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

bool MatchesEnvDiagPrefix(const std::string& upper_name) {
    static constexpr const char* kPrefixes[] = {
        "HTTP", "HTTPS", "ALL_PROXY", "NO_PROXY", "PYTHON",
        "SSL_", "REQUESTS_", "CURL_", "DENO", "YTDLP"
    };
    for (const char* p : kPrefixes) {
        if (upper_name.rfind(p, 0) == 0) {
            return true;
        }
    }
    return false;
}

std::string FormatCommandLineWithMaskedTarget(const std::string& exe_path, const std::vector<std::string>& args) {
    auto quote_arg = [](const std::string& s) -> std::string {
        if (s.find(' ') == std::string::npos && s.find('\t') == std::string::npos) {
            return s;
        }
        return "\"" + s + "\"";
    };
    std::string out = quote_arg(exe_path);
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i + 1 == args.size() && i > 0 && args[i - 1] == "--") {
            out += " <target>";
        } else {
            out += " " + quote_arg(args[i]);
        }
    }
    return out;
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

bool HasControlOrForbiddenLibraryChars(const std::string& s) {
    for (unsigned char c : s) {
        if (c < 0x20 || c == 0x7F || c == '\\' || c == ':') {
            return true;
        }
    }
    return s.find("..") != std::string::npos;
}

bool IsValidLibrarySegmentName(const std::string& seg) {
    return !seg.empty() && seg != "." && seg != "..";
}

bool PathsEqualPlatform(const std::filesystem::path& a, const std::filesystem::path& b) {
#ifdef _WIN32
    return ToLower(PathToUtf8(a)) == ToLower(PathToUtf8(b));
#else
    return a == b;
#endif
}

bool IsPathStrictlyInsideBase(const std::filesystem::path& base_dir, const std::filesystem::path& candidate) {
    std::filesystem::path b = base_dir.lexically_normal();
    std::filesystem::path c = candidate.lexically_normal();
    // Strip trailing empty component produced by trailing slash, if any.
    auto strip_trailing_empty = [](std::vector<std::filesystem::path>& parts) {
        while (!parts.empty() && parts.back().empty()) {
            parts.pop_back();
        }
    };
    std::vector<std::filesystem::path> b_parts(b.begin(), b.end());
    std::vector<std::filesystem::path> c_parts(c.begin(), c.end());
    strip_trailing_empty(b_parts);
    strip_trailing_empty(c_parts);
    if (b_parts.empty() || c_parts.size() <= b_parts.size()) {
        return false;
    }
    for (std::size_t i = 0; i < b_parts.size(); ++i) {
        if (!PathsEqualPlatform(b_parts[i], c_parts[i])) {
            return false;
        }
    }
    return true;
}

void WarnRateLimitedLibrary(const std::string& msg) {
    static std::mutex mu;
    static std::chrono::steady_clock::time_point last_warn{};
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(mu);
        if (last_warn.time_since_epoch().count() != 0 &&
            (now - last_warn) < std::chrono::seconds(30)) {
            return;
        }
        last_warn = now;
    }
    Logger::Instance().Warn(msg);
}

struct ScannedTrackPair {
    std::string utf8_name;
    std::string lower_name;
    std::string pawn_name;
};

std::vector<ScannedTrackPair> CollectAudioTracksInDir(
    const std::filesystem::path& canonical_base,
    const std::filesystem::path& canonical_dir,
    const std::string& folder_label_for_log,
    const std::string& pawn_text_encoding,
    std::uint32_t max_tracks) {
    std::vector<ScannedTrackPair> items;
    std::error_code ec;
    std::filesystem::directory_iterator it(canonical_dir, std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec) {
        return items;
    }
    std::size_t ignored_unrepresentable = 0;
    for (const auto& entry : it) {
        std::error_code item_ec;
        if (!entry.is_regular_file(item_ec) || item_ec) {
            continue;
        }
        const std::filesystem::path entry_path = entry.path();
        const std::string filename_utf8 = PathToUtf8(entry_path.filename());
        if (!IsValidLibrarySegmentName(filename_utf8) || HasControlOrForbiddenLibraryChars(filename_utf8)) {
            continue;
        }
        const std::string ext_lower = ToLower(PathToUtf8(entry_path.extension()));
        if (!IsSupportedAudioFileExtension(ext_lower)) {
            continue;
        }
        const std::filesystem::path canonical_file = std::filesystem::weakly_canonical(entry_path, item_ec);
        if (item_ec || !IsPathStrictlyInsideBase(canonical_base, canonical_file)) {
            continue;
        }
        if (!CanRepresentTextInPawnEncoding(filename_utf8, pawn_text_encoding)) {
            ++ignored_unrepresentable;
            continue;
        }
        items.push_back(ScannedTrackPair{
            filename_utf8,
            ToLower(filename_utf8),
            ConvertTextForPawn(filename_utf8, pawn_text_encoding)
        });
    }
    if (ignored_unrepresentable > 0) {
        WarnRateLimitedLibrary(
            "[Audio] Ignored " + std::to_string(ignored_unrepresentable) +
            " library audio file(s) in '" + (folder_label_for_log.empty() ? std::string("<root>") : folder_label_for_log) +
            "' with filename(s) not representable in scripting.pawn_text_encoding=" + pawn_text_encoding);
    }
    std::sort(items.begin(), items.end(), [](const ScannedTrackPair& a, const ScannedTrackPair& b) {
        if (a.lower_name != b.lower_name) {
            return a.lower_name < b.lower_name;
        }
        return a.utf8_name < b.utf8_name;
    });
    if (max_tracks > 0 && items.size() > max_tracks) {
        WarnRateLimitedLibrary(
            "[Audio] Library folder '" + (folder_label_for_log.empty() ? std::string("<root>") : folder_label_for_log) +
            "' exceeds limit of " + std::to_string(max_tracks) + " tracks; remaining files were truncated");
        items.resize(max_tracks);
    }
    return items;
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

void DrainPipe(
    HANDLE hPipe,
    std::string& out,
    std::size_t max_bytes = 0,
    const std::function<void(const std::string&)>* on_line = nullptr) {
    std::array<char, 4096> buf{};
    DWORD read_bytes = 0;
    std::string pending;
    auto flush_line = [&](std::string& ln) {
        if (!ln.empty() && ln.back() == '\r') {
            ln.pop_back();
        }
        if (on_line && *on_line && !ln.empty()) {
            (*on_line)(ln);
        }
    };
    while (ReadFile(hPipe, buf.data(), static_cast<DWORD>(buf.size()), &read_bytes, nullptr) && read_bytes > 0) {
        if (max_bytes == 0 || out.size() < max_bytes) {
            const std::size_t to_append = (max_bytes == 0)
                ? static_cast<std::size_t>(read_bytes)
                : std::min<std::size_t>(static_cast<std::size_t>(read_bytes), max_bytes - out.size());
            out.append(buf.data(), to_append);
        }
        if (on_line && *on_line) {
            for (DWORD i = 0; i < read_bytes; ++i) {
                char ch = buf[i];
                if (ch == '\n') {
                    flush_line(pending);
                    pending.clear();
                } else if (pending.size() < 4096) {
                    pending.push_back(ch);
                }
            }
        }
    }
    if (on_line && *on_line && !pending.empty()) {
        flush_line(pending);
    }
}

void InspectWindowsEnvForDiag(
    const std::vector<std::pair<std::string, std::string>>* env_overrides,
    std::size_t& out_count,
    std::string& out_matched_names) {
    std::vector<std::string> names;
    if (LPWCH raw_env = GetEnvironmentStringsW()) {
        const wchar_t* cur = raw_env;
        while (*cur != L'\0') {
            std::wstring entry(cur);
            cur += entry.size() + 1;
            auto eq = entry.find(L'=', entry.empty() || entry.front() == L'=' ? 1 : 0);
            if (eq != std::wstring::npos && eq > 0) {
                std::wstring wk = entry.substr(0, eq);
                int needed = WideCharToMultiByte(CP_UTF8, 0, wk.c_str(), static_cast<int>(wk.size()), nullptr, 0, nullptr, nullptr);
                if (needed > 0) {
                    std::string k(needed, '\0');
                    WideCharToMultiByte(CP_UTF8, 0, wk.c_str(), static_cast<int>(wk.size()), k.data(), needed, nullptr, nullptr);
                    names.push_back(std::move(k));
                }
            }
        }
        FreeEnvironmentStringsW(raw_env);
    }
    if (env_overrides) {
        for (const auto& [k, _] : *env_overrides) {
            std::string kl = ToLower(k);
            bool found = false;
            for (const auto& ex : names) {
                if (ToLower(ex) == kl) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                names.push_back(k);
            }
        }
    }
    out_count = names.size();
    out_matched_names.clear();
    for (const auto& name : names) {
        std::string upper = name;
        for (char& ch : upper) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        if (MatchesEnvDiagPrefix(upper)) {
            if (!out_matched_names.empty()) out_matched_names.push_back(',');
            out_matched_names += name;
        }
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

std::int64_t JsonEpochMs(const nlohmann::json& obj, const char* key) {
    if (!obj.is_object()) return 0;
    auto it = obj.find(key);
    if (it == obj.end() || it->is_null() || !it->is_number()) return 0;
    const double sec = it->get<double>();
    if (!std::isfinite(sec) || sec <= 0.0 || sec > 32503680000.0) {
        return 0;
    }
    return static_cast<std::int64_t>(std::llround(sec * 1000.0));
}

TrackMetadata ParseTrackJson(const nlohmann::json& root, const std::string& original_url) {
    TrackMetadata meta;
    meta.original_url = original_url;

    const nlohmann::json* item = &root;
    if (root.contains("entries") && root["entries"].is_array() && !root["entries"].empty()) {
        item = &root["entries"][0];
    }

    meta.id = JsonStr(*item, "id");
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
    meta.format_id = JsonStr(*item, "format_id");
    meta.codec = JsonStr(*item, "acodec");
    meta.container_ext = JsonStr(*item, "ext");

    const nlohmann::json* chosen_fmt = nullptr;
    // Check requested_downloads / requested_formats if top-level url is absent
    if (meta.direct_media_url.empty() && item->contains("requested_formats") && (*item)["requested_formats"].is_array()) {
        for (const auto& fmt : (*item)["requested_formats"]) {
            if (JsonStr(fmt, "acodec", "none") != "none") {
                chosen_fmt = &fmt;
                meta.direct_media_url = JsonStr(fmt, "url");
                meta.format_id = JsonStr(fmt, "format_id");
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

    // Prefer available_at from the chosen format entry (requested_formats / formats), falling back to item / root.
    if (chosen_fmt) {
        meta.available_at_epoch_ms = JsonEpochMs(*chosen_fmt, "available_at");
    }
    if (meta.available_at_epoch_ms <= 0 && !meta.format_id.empty()) {
        for (const char* arr_key : {"requested_formats", "formats"}) {
            if (item->contains(arr_key) && (*item)[arr_key].is_array()) {
                for (const auto& fmt : (*item)[arr_key]) {
                    if (JsonStr(fmt, "format_id") == meta.format_id) {
                        meta.available_at_epoch_ms = JsonEpochMs(fmt, "available_at");
                        if (meta.available_at_epoch_ms > 0) break;
                    }
                }
            }
            if (meta.available_at_epoch_ms > 0) break;
        }
    }
    if (meta.available_at_epoch_ms <= 0) {
        meta.available_at_epoch_ms = JsonEpochMs(*item, "available_at");
    }
    if (meta.available_at_epoch_ms <= 0 && item != &root) {
        meta.available_at_epoch_ms = JsonEpochMs(root, "available_at");
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
    : config_(config), binaries_(binaries), cache_(cache) {
    InitializeCookieSource();
}

std::string StreamResolver::ScrubCookieValues(const std::string& text) {
    return ScrubCookieValuesImpl(text);
}

bool StreamResolver::IsYouTubeAuthChallenge(const std::string& stderr_text, bool* out_is_age_restricted) {
    if (out_is_age_restricted) {
        *out_is_age_restricted = false;
    }
    if (stderr_text.empty()) {
        return false;
    }
    const std::string norm = NormalizeForAuthCheck(stderr_text);
    if (norm.find("sign in to confirm your age") != std::string::npos) {
        if (out_is_age_restricted) {
            *out_is_age_restricted = true;
        }
        return true;
    }
    std::size_t pos = norm.find("confirm you");
    while (pos != std::string::npos) {
        const std::size_t after = pos + 11; // strlen("confirm you")
        const std::size_t bot_pos = norm.find("not a bot", after);
        if (bot_pos != std::string::npos && (bot_pos - after) <= 32) {
            return true;
        }
        pos = norm.find("confirm you", after);
    }
    return false;
}

static std::atomic<bool> g_simulate_root_uid_for_testing{false};
static std::atomic<std::uint64_t> g_temp_cookies_seq{0};

void StreamResolver::SetSimulateRootUidForTesting(bool simulate_root) {
    g_simulate_root_uid_for_testing.store(simulate_root, std::memory_order_release);
}

StreamResolver::TempCookiesFileGuard::~TempCookiesFileGuard() {
    Reset();
}

void StreamResolver::TempCookiesFileGuard::Reset() {
    if (!path.empty()) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        path.clear();
    }
}

std::filesystem::path StreamResolver::GetDefaultCookiesPath() const {
    const std::string base = config_.base_dir.empty() ? std::string("scriptfiles/SampAudioStream") : config_.base_dir;
    return std::filesystem::path(base) / "cookies.txt";
}

std::filesystem::path StreamResolver::GetTempCookiesDir() const {
    const std::string base = config_.base_dir.empty() ? std::string("scriptfiles/SampAudioStream") : config_.base_dir;
    return std::filesystem::path(base) / "tmp";
}

void StreamResolver::CleanupStaleTempCookieFiles() const {
    const std::filesystem::path tmp_dir = GetTempCookiesDir();
    std::error_code ec;
    if (!std::filesystem::is_directory(tmp_dir, ec) || ec) {
        return;
    }
    std::filesystem::directory_iterator it(
        tmp_dir,
        std::filesystem::directory_options::skip_permission_denied,
        ec);
    const std::filesystem::directory_iterator end;
    for (; it != end && !ec; it.increment(ec)) {
        std::error_code item_ec;
        if (!it->is_regular_file(item_ec) || item_ec) {
            continue;
        }
        const std::string name = it->path().filename().string();
        if (name.rfind("cookies-", 0) == 0 && EndsWith(name, ".txt")) {
            std::filesystem::remove(it->path(), item_ec);
        }
    }
}

StreamResolver::CookieSourceSnapshot StreamResolver::PrepareExecutionCookieSnapshot(
    const CookieSourceSnapshot* src,
    TempCookiesFileGuard& guard) const {
    guard.Reset();
    if (!src) {
        return CookieSourceSnapshot{};
    }
    if ((src->kind != CookieSourceKind::ConfiguredFile &&
         src->kind != CookieSourceKind::DefaultFile) ||
        src->file_path.empty()) {
        return *src;
    }

    const std::filesystem::path src_path(src->file_path);
    const std::filesystem::path tmp_dir = GetTempCookiesDir();
    std::error_code ec;
    std::filesystem::create_directories(tmp_dir, ec);
    if (ec) {
        Logger::Instance().Debug("Failed to create temp cookies dir: " + tmp_dir.string());
        CookieSourceSnapshot no_cookie;
        no_cookie.generation = src->generation;
        return no_cookie;
    }

#ifdef _WIN32
    const unsigned long pid = static_cast<unsigned long>(GetCurrentProcessId());
#else
    const unsigned long pid = static_cast<unsigned long>(::getpid());
#endif
    const std::uint64_t seq = g_temp_cookies_seq.fetch_add(1, std::memory_order_relaxed) + 1;
    const std::filesystem::path tmp_path =
        tmp_dir / ("cookies-" + std::to_string(pid) + "-" + std::to_string(seq) + ".txt");

#ifdef _WIN32
    if (!std::filesystem::copy_file(src_path, tmp_path, std::filesystem::copy_options::overwrite_existing, ec) || ec) {
        std::filesystem::remove(tmp_path, ec);
        CookieSourceSnapshot no_cookie;
        no_cookie.generation = src->generation;
        return no_cookie;
    }
#else
    const int src_fd = ::open(src_path.c_str(), O_RDONLY);
    if (src_fd < 0) {
        CookieSourceSnapshot no_cookie;
        no_cookie.generation = src->generation;
        return no_cookie;
    }
    const int dst_fd = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (dst_fd < 0) {
        ::close(src_fd);
        CookieSourceSnapshot no_cookie;
        no_cookie.generation = src->generation;
        return no_cookie;
    }
    ::fchmod(dst_fd, 0600);
    bool copy_ok = true;
    char buf[8192];
    for (;;) {
        const ssize_t nread = ::read(src_fd, buf, sizeof(buf));
        if (nread == 0) {
            break;
        }
        if (nread < 0) {
            if (errno == EINTR) continue;
            copy_ok = false;
            break;
        }
        ssize_t written = 0;
        while (written < nread) {
            const ssize_t nwrote = ::write(dst_fd, buf + written, static_cast<std::size_t>(nread - written));
            if (nwrote < 0) {
                if (errno == EINTR) continue;
                copy_ok = false;
                break;
            }
            written += nwrote;
        }
        if (!copy_ok) break;
    }
    ::close(src_fd);
    ::close(dst_fd);
    if (!copy_ok) {
        std::filesystem::remove(tmp_path, ec);
        CookieSourceSnapshot no_cookie;
        no_cookie.generation = src->generation;
        return no_cookie;
    }
#endif

    guard.path = tmp_path;
    CookieSourceSnapshot copy = *src;
    copy.file_path = tmp_path.string();
    return copy;
}

void StreamResolver::InitializeCookieSource() {
    CleanupStaleTempCookieFiles();
    std::lock_guard<std::mutex> lock(cookie_mutex_);
    active_cookie_source_ = CookieSourceSnapshot{};
    configured_cookies_file_missing_ = false;
    cookies_file_expired_ = false;
    have_default_cookies_mtime_ = false;

    if (!config_.cookies_file.empty()) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(config_.cookies_file, ec) && !ec) {
            active_cookie_source_.kind = CookieSourceKind::ConfiguredFile;
            active_cookie_source_.file_path = config_.cookies_file;
            active_cookie_source_.generation = cookie_state_generation_;
        } else {
            configured_cookies_file_missing_ = true;
            Logger::Instance().Warn(
                "Configured extractor.cookies_file '" + config_.cookies_file + "' does not exist; ignoring it.");
        }
        return;
    }

    const std::filesystem::path default_path = GetDefaultCookiesPath();
    std::error_code ec;
    if (std::filesystem::is_regular_file(default_path, ec) && !ec) {
        auto mtime = std::filesystem::last_write_time(default_path, ec);
        if (!ec) {
            default_cookies_mtime_ = mtime;
            have_default_cookies_mtime_ = true;
        }
        active_cookie_source_.kind = CookieSourceKind::DefaultFile;
        active_cookie_source_.file_path = default_path.string();
        active_cookie_source_.generation = cookie_state_generation_;
        Logger::Instance().Info("Using YouTube cookies file: " + active_cookie_source_.file_path);
    }
}

StreamResolver::CookieSourceSnapshot StreamResolver::GetActiveCookieSnapshot() const {
    std::lock_guard<std::mutex> lock(cookie_mutex_);
    if (cookies_file_expired_ &&
        (active_cookie_source_.kind == CookieSourceKind::ConfiguredFile ||
         active_cookie_source_.kind == CookieSourceKind::DefaultFile)) {
        CookieSourceSnapshot expired_none;
        expired_none.generation = cookie_state_generation_;
        return expired_none;
    }
    return active_cookie_source_;
}

void StreamResolver::MaybeWarnAuthRequiredLocked(bool is_age_restricted) const {
    const auto now = std::chrono::steady_clock::now();
    if (auth_warned_once_ && (now - last_auth_warn_) < std::chrono::minutes(10)) {
        return;
    }
    auth_warned_once_ = true;
    last_auth_warn_ = now;

    const char* cookies_status = cookies_file_expired_ ? "expired" : "missing";
    std::string browsers_status;
    if (config_.cookies_from_browser == "off") {
        browsers_status = "disabled (extractor.cookies_from_browser=off)";
    } else {
        browsers_status = last_checked_browsers_summary_.empty()
            ? std::string("none")
            : last_checked_browsers_summary_;
    }

    std::string msg;
    if (is_age_restricted) {
        msg = "YouTube requires age verification on this video (\"Sign in to confirm your age\").\n"
              "No usable YouTube cookies were found (cookies.txt: " + std::string(cookies_status) +
              "; browsers checked: " + browsers_status + ").\n"
              "Fix: sign in to YouTube with an adult-verified secondary Google account in Firefox on this machine (same OS user that runs the server),\n"
              "or export cookies.txt from a private browser window with an adult-verified account and place it at scriptfiles/SampAudioStream/cookies.txt.\n"
              "Step-by-step guide: docs/youtube-cookies.md";
    } else {
        msg = "YouTube is blocking this server's IP (\"Sign in to confirm you're not a bot\").\n"
              "No usable YouTube cookies were found (cookies.txt: " + std::string(cookies_status) +
              "; browsers checked: " + browsers_status + ").\n"
              "Fix: sign in to YouTube with a secondary Google account in Firefox on this machine (same OS user that runs the server),\n"
              "or export cookies.txt from a private browser window and place it at scriptfiles/SampAudioStream/cookies.txt.\n"
              "Step-by-step guide: docs/youtube-cookies.md";
    }
    bool running_as_root = g_simulate_root_uid_for_testing.load(std::memory_order_acquire);
#ifndef _WIN32
    if (!running_as_root && ::geteuid() == 0) {
        running_as_root = true;
    }
#endif
    if (running_as_root && config_.cookies_from_browser != "off") {
        msg += "\nThe server is running as root, so only /root browser profiles are searched. "
               "Run the server as the same OS user that uses the browser (recommended), or use cookies.txt.";
    }
    Logger::Instance().Warn(msg);
}

bool StreamResolver::LooksLikeUrl(const std::string& input) {
    std::string lower = ToLower(input);
    return lower.rfind("http://", 0) == 0 || lower.rfind("https://", 0) == 0;
}

bool StreamResolver::IsLibraryUrl(const std::string& input) {
    if (input.size() < 10) {
        return false;
    }
    return ToLower(input.substr(0, 10)) == "library://";
}

bool StreamResolver::LooksLikeDiskPath(const std::string& input) {
    if (input.empty()) {
        return false;
    }
    if (input[0] == '/' || input[0] == '\\') {
        return true;
    }
    if (input.size() >= 2 &&
        std::isalpha(static_cast<unsigned char>(input[0])) != 0 &&
        input[1] == ':') {
        return true;
    }
    if (input == "." || input == ".." ||
        input.rfind("./", 0) == 0 || input.rfind(".\\", 0) == 0 ||
        input.rfind("../", 0) == 0 || input.rfind("..\\", 0) == 0 ||
        input.rfind("~/", 0) == 0 || input.rfind("~\\", 0) == 0) {
        return true;
    }
    return false;
}

bool StreamResolver::ParseAndResolveLibraryFile(
    const std::string& url,
    const std::string& music_dir,
    std::filesystem::path& out_real_path,
    std::string& out_file_name,
    ErrorCode& out_error_code,
    std::string& out_error_message) {
    out_real_path.clear();
    out_file_name.clear();
    if (!IsLibraryUrl(url)) {
        out_error_code = ErrorCode::InvalidUrl;
        out_error_message = "URL not allowed";
        return false;
    }
    const std::string rel = url.substr(10);
    if (rel.empty() || HasControlOrForbiddenLibraryChars(rel)) {
        out_error_code = ErrorCode::InvalidUrl;
        out_error_message = "Invalid library track reference";
        return false;
    }

    std::string folder;
    std::string file;
    if (rel[0] == '/') {
        file = rel.substr(1);
        if (file.find('/') != std::string::npos) {
            out_error_code = ErrorCode::InvalidUrl;
            out_error_message = "Invalid library track reference";
            return false;
        }
    } else {
        const auto slash = rel.find('/');
        if (slash == std::string::npos) {
            out_error_code = ErrorCode::InvalidUrl;
            out_error_message = "Invalid library track reference";
            return false;
        }
        folder = rel.substr(0, slash);
        file = rel.substr(slash + 1);
        if (!IsValidLibrarySegmentName(folder) || file.find('/') != std::string::npos) {
            out_error_code = ErrorCode::InvalidUrl;
            out_error_message = "Invalid library track reference";
            return false;
        }
    }

    if (!IsValidLibrarySegmentName(file)) {
        out_error_code = ErrorCode::InvalidUrl;
        out_error_message = "Invalid library track filename";
        return false;
    }
    const auto dot = file.rfind('.');
    if (dot == std::string::npos) {
        out_error_code = ErrorCode::InvalidUrl;
        out_error_message = "Unsupported local audio file extension";
        return false;
    }
    const std::string ext_lower = ToLower(file.substr(dot));
    if (!IsSupportedAudioFileExtension(ext_lower)) {
        out_error_code = ErrorCode::InvalidUrl;
        out_error_message = "Unsupported local audio file extension";
        return false;
    }

    std::error_code ec;
    const std::filesystem::path base_raw = Utf8ToPath(music_dir);
    if (music_dir.empty() || !std::filesystem::exists(base_raw, ec) || ec ||
        !std::filesystem::is_directory(base_raw, ec) || ec) {
        out_error_code = ErrorCode::ExtractionFailed;
        out_error_message = "Music library directory is unavailable";
        return false;
    }
    const std::filesystem::path canonical_base = std::filesystem::weakly_canonical(base_raw, ec);
    if (ec || !std::filesystem::is_directory(canonical_base, ec) || ec) {
        out_error_code = ErrorCode::ExtractionFailed;
        out_error_message = "Music library directory is unavailable";
        return false;
    }

    std::filesystem::path candidate = folder.empty()
        ? (canonical_base / Utf8ToPath(file))
        : (canonical_base / Utf8ToPath(folder) / Utf8ToPath(file));
    const std::filesystem::path canonical_target = std::filesystem::weakly_canonical(candidate, ec);
    if (ec || !IsPathStrictlyInsideBase(canonical_base, canonical_target)) {
        out_error_code = ErrorCode::InvalidUrl;
        out_error_message = "URL not allowed";
        return false;
    }
    if (!std::filesystem::exists(canonical_target, ec) || ec ||
        !std::filesystem::is_regular_file(canonical_target, ec) || ec) {
        out_error_code = ErrorCode::ExtractionFailed;
        out_error_message = "Local library file not found";
        return false;
    }

    out_real_path = canonical_target;
    out_file_name = file;
    out_error_code = ErrorCode::None;
    out_error_message.clear();
    return true;
}

bool StreamResolver::ParseAndResolveSoundFile(
    const std::string& sound,
    const std::string& sound_dir,
    std::filesystem::path& out_real_path,
    std::string& out_ext_lower,
    ErrorCode& out_error_code,
    std::string& out_error_message) {
    out_real_path.clear();
    out_ext_lower.clear();
    std::string rel = sound;
    std::replace(rel.begin(), rel.end(), '\\', '/');
    if (rel.empty() || rel[0] == '/' || HasControlOrForbiddenLibraryChars(rel)) {
        out_error_code = ErrorCode::InvalidUrl;
        out_error_message = "Invalid sound file reference";
        return false;
    }

    std::string folder;
    std::string file = rel;
    const auto slash = rel.find('/');
    if (slash != std::string::npos) {
        folder = rel.substr(0, slash);
        file = rel.substr(slash + 1);
        if (!IsValidLibrarySegmentName(folder) || file.find('/') != std::string::npos) {
            out_error_code = ErrorCode::InvalidUrl;
            out_error_message = "Invalid sound file reference";
            return false;
        }
    }
    if (!IsValidLibrarySegmentName(file)) {
        out_error_code = ErrorCode::InvalidUrl;
        out_error_message = "Invalid sound file reference";
        return false;
    }
    const auto dot = file.rfind('.');
    const std::string ext_lower = (dot == std::string::npos) ? std::string() : ToLower(file.substr(dot));
    if (!IsSupportedSoundFileExtension(ext_lower)) {
        out_error_code = ErrorCode::UnsupportedFormat;
        out_error_message = "Unsupported sound file extension (use .mp3, .ogg or .wav)";
        return false;
    }

    std::error_code ec;
    const std::filesystem::path base_raw = Utf8ToPath(sound_dir);
    if (sound_dir.empty() || !std::filesystem::is_directory(base_raw, ec) || ec) {
        out_error_code = ErrorCode::ExtractionFailed;
        out_error_message = "Sound directory is unavailable";
        return false;
    }
    const std::filesystem::path canonical_base = std::filesystem::weakly_canonical(base_raw, ec);
    if (ec || !std::filesystem::is_directory(canonical_base, ec) || ec) {
        out_error_code = ErrorCode::ExtractionFailed;
        out_error_message = "Sound directory is unavailable";
        return false;
    }
    const std::filesystem::path candidate = folder.empty()
        ? (canonical_base / Utf8ToPath(file))
        : (canonical_base / Utf8ToPath(folder) / Utf8ToPath(file));
    const std::filesystem::path canonical_target = std::filesystem::weakly_canonical(candidate, ec);
    if (ec || !IsPathStrictlyInsideBase(canonical_base, canonical_target)) {
        out_error_code = ErrorCode::InvalidUrl;
        out_error_message = "Invalid sound file reference";
        return false;
    }
    if (!std::filesystem::is_regular_file(canonical_target, ec) || ec) {
        out_error_code = ErrorCode::StreamNotFound;
        out_error_message = "Sound file not found";
        return false;
    }

    out_real_path = canonical_target;
    out_ext_lower = ext_lower;
    out_error_code = ErrorCode::None;
    out_error_message.clear();
    return true;
}

bool StreamResolver::ParseAndResolveLibraryFolder(
    const std::string& url,
    const std::string& music_dir,
    std::string& out_folder_utf8,
    std::filesystem::path& out_real_dir,
    ErrorCode& out_error_code,
    std::string& out_error_message) {
    out_folder_utf8.clear();
    out_real_dir.clear();
    if (!IsLibraryUrl(url)) {
        out_error_code = ErrorCode::InvalidUrl;
        out_error_message = "URL not allowed";
        return false;
    }
    std::string rel = url.substr(10);
    if (HasControlOrForbiddenLibraryChars(rel)) {
        out_error_code = ErrorCode::InvalidUrl;
        out_error_message = "Invalid library folder reference";
        return false;
    }
    if (!rel.empty() && rel != "/") {
        if (rel.back() == '/') {
            rel.pop_back();
        }
        if (rel.empty() || rel.find('/') != std::string::npos || !IsValidLibrarySegmentName(rel)) {
            out_error_code = ErrorCode::InvalidUrl;
            out_error_message = "Invalid library folder reference";
            return false;
        }
        out_folder_utf8 = rel;
    }

    std::error_code ec;
    const std::filesystem::path base_raw = Utf8ToPath(music_dir);
    if (music_dir.empty() || !std::filesystem::exists(base_raw, ec) || ec ||
        !std::filesystem::is_directory(base_raw, ec) || ec) {
        out_error_code = ErrorCode::ExtractionFailed;
        out_error_message = "Music library directory is unavailable";
        return false;
    }
    const std::filesystem::path canonical_base = std::filesystem::weakly_canonical(base_raw, ec);
    if (ec || !std::filesystem::is_directory(canonical_base, ec) || ec) {
        out_error_code = ErrorCode::ExtractionFailed;
        out_error_message = "Music library directory is unavailable";
        return false;
    }

    if (out_folder_utf8.empty()) {
        out_real_dir = canonical_base;
        out_error_code = ErrorCode::None;
        out_error_message.clear();
        return true;
    }

    const std::filesystem::path canonical_sub = std::filesystem::weakly_canonical(
        canonical_base / Utf8ToPath(out_folder_utf8), ec);
    if (ec || !IsPathStrictlyInsideBase(canonical_base, canonical_sub)) {
        out_error_code = ErrorCode::InvalidUrl;
        out_error_message = "URL not allowed";
        return false;
    }
    if (!std::filesystem::exists(canonical_sub, ec) || ec ||
        !std::filesystem::is_directory(canonical_sub, ec) || ec) {
        out_error_code = ErrorCode::ExtractionFailed;
        out_error_message = "Local library folder not found";
        return false;
    }

    out_real_dir = canonical_sub;
    out_error_code = ErrorCode::None;
    out_error_message.clear();
    return true;
}

std::vector<AudioLibraryFolderData> StreamResolver::ScanMusicLibraryDir(
    const std::string& music_dir,
    const std::string& pawn_text_encoding,
    std::uint32_t max_folders,
    std::uint32_t max_tracks_per_folder) {
    std::vector<AudioLibraryFolderData> folders;
    if (music_dir.empty()) {
        return folders;
    }
    std::error_code ec;
    const std::filesystem::path base_raw = Utf8ToPath(music_dir);
    if (!std::filesystem::exists(base_raw, ec) || ec ||
        !std::filesystem::is_directory(base_raw, ec) || ec) {
        return folders;
    }
    const std::filesystem::path canonical_base = std::filesystem::weakly_canonical(base_raw, ec);
    if (ec || !std::filesystem::is_directory(canonical_base, ec) || ec) {
        return folders;
    }

    // 1. Collect tracks directly in the root folder ("")
    auto root_tracks = CollectAudioTracksInDir(
        canonical_base, canonical_base, "", pawn_text_encoding, max_tracks_per_folder);
    if (!root_tracks.empty()) {
        AudioLibraryFolderData root_folder;
        root_folder.utf8_name = "";
        root_folder.pawn_name = "";
        root_folder.utf8_tracks.reserve(root_tracks.size());
        root_folder.pawn_tracks.reserve(root_tracks.size());
        for (auto& t : root_tracks) {
            root_folder.utf8_tracks.push_back(std::move(t.utf8_name));
            root_folder.pawn_tracks.push_back(std::move(t.pawn_name));
        }
        folders.push_back(std::move(root_folder));
    }

    // 2. Collect non-empty depth-1 subfolders sorted case-insensitively
    struct SubdirCandidate {
        std::string utf8_name;
        std::string lower_name;
        std::string pawn_name;
        std::filesystem::path canonical_dir;
    };
    std::vector<SubdirCandidate> subdirs;
    std::size_t ignored_unrepresentable_folders = 0;
    std::filesystem::directory_iterator dir_it(
        canonical_base, std::filesystem::directory_options::skip_permission_denied, ec);
    if (!ec) {
        for (const auto& entry : dir_it) {
            std::error_code item_ec;
            if (!entry.is_directory(item_ec) || item_ec) {
                continue;
            }
            const std::filesystem::path entry_path = entry.path();
            const std::string folder_utf8 = PathToUtf8(entry_path.filename());
            if (!IsValidLibrarySegmentName(folder_utf8) || HasControlOrForbiddenLibraryChars(folder_utf8)) {
                continue;
            }
            const std::filesystem::path canonical_sub = std::filesystem::weakly_canonical(entry_path, item_ec);
            if (item_ec || !IsPathStrictlyInsideBase(canonical_base, canonical_sub)) {
                continue;
            }
            if (!CanRepresentTextInPawnEncoding(folder_utf8, pawn_text_encoding)) {
                ++ignored_unrepresentable_folders;
                continue;
            }
            subdirs.push_back(SubdirCandidate{
                folder_utf8,
                ToLower(folder_utf8),
                ConvertTextForPawn(folder_utf8, pawn_text_encoding),
                canonical_sub
            });
        }
    }
    if (ignored_unrepresentable_folders > 0) {
        WarnRateLimitedLibrary(
            "[Audio] Ignored " + std::to_string(ignored_unrepresentable_folders) +
            " library folder(s) with name(s) not representable in scripting.pawn_text_encoding=" + pawn_text_encoding);
    }

    std::sort(subdirs.begin(), subdirs.end(), [](const SubdirCandidate& a, const SubdirCandidate& b) {
        if (a.lower_name != b.lower_name) {
            return a.lower_name < b.lower_name;
        }
        return a.utf8_name < b.utf8_name;
    });

    for (auto& sub : subdirs) {
        if (max_folders > 0 && folders.size() >= max_folders) {
            WarnRateLimitedLibrary(
                "[Audio] Music library exceeded limit of " + std::to_string(max_folders) +
                " folders; remaining folders were omitted");
            break;
        }
        auto sub_tracks = CollectAudioTracksInDir(
            canonical_base, sub.canonical_dir, sub.utf8_name, pawn_text_encoding, max_tracks_per_folder);
        if (sub_tracks.empty()) {
            continue;
        }
        AudioLibraryFolderData fd;
        fd.utf8_name = std::move(sub.utf8_name);
        fd.pawn_name = std::move(sub.pawn_name);
        fd.utf8_tracks.reserve(sub_tracks.size());
        fd.pawn_tracks.reserve(sub_tracks.size());
        for (auto& t : sub_tracks) {
            fd.utf8_tracks.push_back(std::move(t.utf8_name));
            fd.pawn_tracks.push_back(std::move(t.pawn_name));
        }
        folders.push_back(std::move(fd));
    }

    return folders;
}

bool StreamResolver::IsKnownDirectAudioExtension(const std::string& url) {
    std::string clean = ToLower(StripUrlQuery(url));
    return EndsWith(clean, ".mp3") || EndsWith(clean, ".ogg") ||
           EndsWith(clean, ".wav") || EndsWith(clean, ".flac") ||
           EndsWith(clean, ".pls") || EndsWith(clean, ".m3u") ||
           EndsWith(clean, ".aac");
}

bool StreamResolver::IsDirectAudioFileUrl(const std::string& url) {
    if (!LooksLikeUrl(url) || IsKnownPlatformUrl(url)) {
        return false;
    }
    std::string clean = ToLower(StripUrlQuery(url));
    const auto last_dot = clean.rfind('.');
    if (last_dot == std::string::npos) {
        return false;
    }
    return IsSupportedAudioFileExtension(clean.substr(last_dot));
}

std::string StreamResolver::ExtractUrlFileNameTitle(const std::string& url) {
    const std::string base = StripUrlQuery(url);
    const std::size_t scheme_pos = base.find("://");
    const std::size_t path_start = (scheme_pos != std::string::npos)
        ? base.find('/', scheme_pos + 3)
        : 0;
    if (path_start == std::string::npos) {
        return {};
    }
    const std::size_t last_slash = base.rfind('/');
    if (last_slash == std::string::npos || last_slash < path_start) {
        return {};
    }
    const std::string raw_seg = base.substr(last_slash + 1);
    if (raw_seg.empty()) {
        return {};
    }
    return CleanIcyStationNameUtf8(UrlDecodeSimple(raw_seg));
}

bool StreamResolver::IsHlsPlaylistUrl(const std::string& url) {
    std::string clean = ToLower(StripUrlQuery(url));
    return EndsWith(clean, ".m3u8");
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
    std::string& out_station_name_utf8,
    const std::atomic<bool>* cancel_flag) const {
    out_security_blocked = false;
    out_station_name_utf8.clear();
    auto resp = HttpClient::Head(url, {}, 4000, cancel_flag);
    if (resp.error == "URL not allowed") {
        out_security_blocked = true;
        return false;
    }
    if (resp.Ok()) {
        std::string ct = ToLower(resp.content_type);
        out_content_type = ct;
        if (IsAudioContentType(ct)) {
            out_station_name_utf8 = CleanIcyStationNameUtf8(
                HttpClient::FindHeaderIgnoreCase(resp.headers, "icy-name"));
            return true;
        }
    }

    // Fallback: small GET probe (handles servers rejecting HEAD or replying with ICY 200 OK)
    std::atomic<bool> local_cancel{false};
    int get_status = 0;
    std::string get_ct;
    std::string get_station_name;
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
        [&](int st, const std::string& ct, std::int64_t, const HttpClient::HeaderMap& resp_headers) {
            get_status = st;
            get_ct = ToLower(ct);
            if (st >= 200 && st < 300) {
                get_station_name = CleanIcyStationNameUtf8(
                    HttpClient::FindHeaderIgnoreCase(resp_headers, "icy-name"));
            }
        },
        4000);

    if (probe_err == "URL not allowed") {
        out_security_blocked = true;
        return false;
    }
    if (get_status >= 200 && get_status < 300) {
        out_content_type = get_ct;
        if (IsAudioContentType(get_ct) || HasAudioStreamMagic(sample_bytes)) {
            out_station_name_utf8 = std::move(get_station_name);
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
        res.source_type = (cached->extractor_name == "direct" && !IsHlsPlaylistUrl(cached->direct_media_url))
                              ? SourceType::DirectAudio
                              : SourceType::ExtractorStream;
        res.metadata = *cached;
        res.metadata.source_type = res.source_type;
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

    if (IsLibraryUrl(url_or_query)) {
        std::filesystem::path real_path;
        std::string file_name;
        ErrorCode err_code = ErrorCode::None;
        std::string err_msg;
        if (!ParseAndResolveLibraryFile(url_or_query, config_.music_dir, real_path, file_name, err_code, err_msg)) {
            res.error = err_code;
            res.error_message = err_msg.empty() ? "Invalid library track reference" : err_msg;
            return res;
        }
        std::int32_t duration_seconds = 0;
        ProbeLocalAudioFileDurationSeconds(real_path, duration_seconds);

        res.success = true;
        res.source_type = SourceType::ExtractorStream;
        res.metadata.original_url = url_or_query;
        res.metadata.local_file_path = PathToUtf8(real_path);
        res.metadata.extractor_name = "library";
        res.metadata.title = file_name;
        res.metadata.title_is_placeholder = false;
        res.metadata.is_live = false;
        res.metadata.duration_seconds = duration_seconds;
        res.metadata.source_type = SourceType::ExtractorStream;
        cache_.PutTrack(url_or_query, res.metadata);
        return res;
    }

    bool is_url = LooksLikeUrl(url_or_query);
    if (!is_url && (HasExplicitUrlScheme(url_or_query) || LooksLikeDiskPath(url_or_query))) {
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
            std::string probed_station_name;
            bool probed_audio = false;
            bool security_blocked = false;

            if (!looks_playlist && IsKnownDirectAudioExtension(url_or_query)) {
                const std::string file_title = ExtractUrlFileNameTitle(url_or_query);
                res.success = true;
                res.source_type = SourceType::DirectAudio;
                res.metadata.original_url = url_or_query;
                res.metadata.direct_media_url = url_or_query;
                res.metadata.extractor_name = "direct";
                res.metadata.title = file_title.empty() ? "Direct Audio Stream" : file_title;
                res.metadata.title_is_placeholder = true;
                res.metadata.is_live = false;
                cache_.PutTrack(url_or_query, res.metadata);
                return res;
            }

            if (!looks_playlist) {
                probed_audio = ProbeDirectHttpAudio(url_or_query, probed_ct, security_blocked, probed_station_name, cancel_flag);
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
                            if (inner.metadata.title.empty() || inner.metadata.title_is_placeholder ||
                                inner.metadata.title == "Direct Audio Stream") {
                                inner.metadata.title = "Radio Stream";
                                inner.metadata.title_is_placeholder = true;
                            }
                            cache_.PutTrack(url_or_query, inner.metadata);
                            return inner;
                        }
                    }
                }
            }

            if (probed_audio) {
                const bool direct_file = IsDirectAudioFileUrl(url_or_query);
                res.success = true;
                res.source_type = SourceType::DirectAudio;
                res.metadata.original_url = url_or_query;
                res.metadata.direct_media_url = url_or_query;
                res.metadata.extractor_name = "direct";
                if (!probed_station_name.empty()) {
                    res.metadata.title = probed_station_name;
                    res.metadata.title_is_placeholder = false;
                } else if (direct_file) {
                    const std::string file_title = ExtractUrlFileNameTitle(url_or_query);
                    res.metadata.title = file_title.empty() ? "Direct Audio Stream" : file_title;
                    res.metadata.title_is_placeholder = true;
                } else {
                    res.metadata.title = "Radio Stream";
                    res.metadata.title_is_placeholder = true;
                }
                res.metadata.is_live = !direct_file;
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
        res.error_message = ExtractorUnavailableMessage(binaries_);
        return res;
    }

    std::string target = is_url ? url_or_query : ("ytsearch1:" + url_or_query);

    std::vector<std::string> base_args = {
        "--ignore-config",
        "--dump-single-json",
        "--no-playlist",
        "--playlist-items", "1",
        "-f", "bestaudio[ext=webm]/bestaudio[ext=m4a]/bestaudio[ext=mp3]/bestaudio[ext=ogg]/bestaudio/best"
    };
    if (config_.debug_logging) {
        base_args.push_back("-v");
    }

    auto t_yt_start = std::chrono::steady_clock::now();
    std::string stdout_data;
    std::string stderr_data;
    std::string js_runtime_used = "none";
    std::int64_t js_check_ms = 0;
    bool auth_required = false;
    bool age_restricted = false;
    bool yt_ok = ExecuteYtDlpWithAuthFallback(
        base_args,
        target,
        stdout_data,
        stderr_data,
        30000,
        cancel_flag,
        &js_runtime_used,
        &js_check_ms,
        true,
        auth_required,
        age_restricted);
    auto yt_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t_yt_start).count();
    Logger::Instance().Debug(
        "yt-dlp resolve finished in " + std::to_string(yt_ms) + " ms (ok=" +
        std::string(yt_ok ? "true" : "false") + ", js_check=" + std::to_string(js_check_ms) +
        " ms, js_runtime=" + js_runtime_used + ")");
    if (!yt_ok) {
        if (auth_required) {
            res.error = ErrorCode::AuthRequired;
            res.error_message = age_restricted
                ? "YouTube age restriction requires an adult-verified account on this server; see the server log"
                : "YouTube requires sign-in on this server; see the server log";
        } else {
            res.error = ErrorCode::ExtractionFailed;
            const std::string safe_err = ScrubCookieValues(stderr_data);
            res.error_message = safe_err.empty() ? "yt-dlp execution failed" : safe_err.substr(0, 256);
        }
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
        const std::string yt_client = ExtractQueryParam(meta.direct_media_url, "c");
        const std::string n_val = ExtractQueryParam(meta.direct_media_url, "n");
        const bool has_sig = !ExtractQueryParam(meta.direct_media_url, "sig").empty();
        const bool has_lsig = !ExtractQueryParam(meta.direct_media_url, "lsig").empty();
        const std::string url_ip = UrlDecodeSimple(ExtractQueryParam(meta.direct_media_url, "ip"));
        const std::string pub_ip = HttpClient::GetSecurityPolicy().public_ip;
        const std::string ip_match_str = (pub_ip.empty() || url_ip.empty())
            ? std::string("unknown")
            : ((url_ip == pub_ip) ? std::string("true") : std::string("false"));
        const std::string param_names = ExtractQueryParamNamesOrdered(meta.direct_media_url);
        const auto now_epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const std::int64_t available_in_ms = (meta.available_at_epoch_ms > 0)
            ? (meta.available_at_epoch_ms - now_epoch_ms)
            : 0;
        Logger::Instance().Debug(
            "yt-dlp selected format: format_id=" + (meta.format_id.empty() ? std::string("none") : meta.format_id) +
            ", ext=" + meta.container_ext +
            ", codec=" + meta.codec +
            ", c=" + (yt_client.empty() ? std::string("none") : yt_client) +
            ", live=" + std::string(meta.is_live ? "true" : "false") +
            ", available_in_ms=" + std::to_string(available_in_ms) +
            ", url_len=" + std::to_string(meta.direct_media_url.size()) +
            ", n_len=" + std::to_string(n_val.size()) +
            ", has_sig=" + std::string(has_sig ? "true" : "false") +
            ", has_lsig=" + std::string(has_lsig ? "true" : "false") +
            ", ip_matches_public=" + ip_match_str +
            ", params=" + (param_names.empty() ? std::string("none") : param_names) +
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
        res.metadata.source_type = SourceType::ExtractorStream;
        res.metadata.resolved_at = std::chrono::steady_clock::now();
        res.metadata.js_runtime_used = js_runtime_used;
        cache_.PutTrack(url_or_query, res.metadata);
        return res;
    } catch (const std::exception& ex) {
        res.error = ErrorCode::ExtractionFailed;
        res.error_message = std::string("JSON parse error: ") + ex.what();
        return res;
    }
}

void StreamResolver::InvalidateCachedTrack(const std::string& url_or_query) {
    if (!url_or_query.empty()) {
        cache_.InvalidateTrack(url_or_query);
    }
}

void StreamResolver::InvalidateCachedLibraryTracks() {
    cache_.InvalidateTrackPrefix("library://");
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
        res.error_message = ExtractorUnavailableMessage(binaries_);
        return res;
    }

    std::string cache_key = std::to_string(limit) + ":" + query;
    std::string search_target = "ytsearch" + std::to_string(limit) + ":" + query;
    std::vector<std::string> base_args = {
        "--ignore-config",
        "--dump-single-json",
        "--flat-playlist",
        "--no-warnings"
    };

    std::string stdout_data;
    std::string stderr_data;
    bool auth_required = false;
    bool age_restricted = false;
    if (!ExecuteYtDlpWithAuthFallback(
            base_args,
            search_target,
            stdout_data,
            stderr_data,
            20000,
            cancel_flag,
            nullptr,
            nullptr,
            false,
            auth_required,
            age_restricted)) {
        if (auth_required) {
            res.error = ErrorCode::AuthRequired;
            res.error_message = age_restricted
                ? "YouTube age restriction requires an adult-verified account on this server; see the server log"
                : "YouTube requires sign-in on this server; see the server log";
        } else {
            res.error = ErrorCode::SearchFailed;
            const std::string safe_err = ScrubCookieValues(stderr_data);
            res.error_message = safe_err.empty() ? "Search subprocess failed" : safe_err.substr(0, 256);
        }
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

PlaylistExtractionResult StreamResolver::ExtractPlaylist(
    const std::string& url,
    std::uint32_t max_items,
    const std::atomic<bool>* cancel_flag) {
    PlaylistExtractionResult res;
    if (IsLibraryUrl(url)) {
        std::string folder_utf8;
        std::filesystem::path real_dir;
        ErrorCode err_code = ErrorCode::None;
        std::string err_msg;
        if (!ParseAndResolveLibraryFolder(url, config_.music_dir, folder_utf8, real_dir, err_code, err_msg)) {
            res.error = err_code;
            res.error_message = err_msg.empty() ? "Invalid library folder reference" : err_msg;
            return res;
        }
        std::error_code ec;
        const std::filesystem::path canonical_base = std::filesystem::weakly_canonical(Utf8ToPath(config_.music_dir), ec);
        if (ec) {
            res.error = ErrorCode::ExtractionFailed;
            res.error_message = "Music library directory is unavailable";
            return res;
        }
        const std::uint32_t limit = std::min<std::uint32_t>(std::max<std::uint32_t>(1, max_items), 1000u);
        auto tracks = CollectAudioTracksInDir(
            canonical_base, real_dir, folder_utf8, config_.pawn_text_encoding, limit);
        if (tracks.empty()) {
            res.error = ErrorCode::ExtractionFailed;
            res.error_message = "Library folder contains no playable audio tracks";
            return res;
        }
        res.urls.reserve(tracks.size());
        res.titles.reserve(tracks.size());
        for (auto& t : tracks) {
            res.urls.push_back(folder_utf8.empty()
                ? ("library:///" + t.utf8_name)
                : ("library://" + folder_utf8 + "/" + t.utf8_name));
            res.titles.push_back(std::move(t.utf8_name));
        }
        res.success = true;
        return res;
    }
    if (!LooksLikeUrl(url)) {
        res.error = ErrorCode::InvalidUrl;
        res.error_message = "Playlist URL must start with http:// or https://";
        return res;
    }
    if (IsCancelled(cancel_flag)) {
        res.error = ErrorCode::ExtractionFailed;
        res.error_message = "Cancelled";
        return res;
    }
    if (!binaries_.IsReady()) {
        res.error = ErrorCode::ExtractorNotReady;
        res.error_message = ExtractorUnavailableMessage(binaries_);
        return res;
    }

    std::uint32_t capped = std::max<std::uint32_t>(1, max_items);
    std::vector<std::string> base_args = {
        "--ignore-config",
        "--flat-playlist",
        "--dump-single-json",
        "--playlist-end", std::to_string(capped),
        "--yes-playlist"
    };

    std::string stdout_data;
    std::string stderr_data;
    bool auth_required = false;
    bool age_restricted = false;
    if (!ExecuteYtDlpWithAuthFallback(
            base_args,
            url,
            stdout_data,
            stderr_data,
            60000,
            cancel_flag,
            nullptr,
            nullptr,
            false,
            auth_required,
            age_restricted)) {
        if (auth_required) {
            res.error = ErrorCode::AuthRequired;
            res.error_message = age_restricted
                ? "YouTube age restriction requires an adult-verified account on this server; see the server log"
                : "YouTube requires sign-in on this server; see the server log";
        } else {
            res.error = ErrorCode::ExtractionFailed;
            const std::string safe_err = ScrubCookieValues(stderr_data);
            res.error_message = safe_err.empty() ? "Playlist extraction failed" : safe_err.substr(0, 256);
        }
        return res;
    }

    try {
        auto json = nlohmann::json::parse(stdout_data);
        const std::string url_lower = ToLower(url);
        const bool url_is_youtube =
            (url_lower.find("youtube.com") != std::string::npos ||
             url_lower.find("youtu.be") != std::string::npos);

        if (json.contains("entries") && json["entries"].is_array()) {
            for (const auto& entry : json["entries"]) {
                if (res.urls.size() >= capped) {
                    break;
                }
                if (!entry.is_object()) continue;
                std::string entry_url = JsonStr(entry, "url");
                if (!LooksLikeUrl(entry_url)) {
                    entry_url = JsonStr(entry, "webpage_url");
                }
                if (!LooksLikeUrl(entry_url)) {
                    std::string id = JsonStr(entry, "id");
                    std::string ie = ToLower(JsonStr(entry, "ie_key", JsonStr(entry, "extractor")));
                    if (!id.empty() && (url_is_youtube || ie.find("youtube") != std::string::npos)) {
                        entry_url = "https://www.youtube.com/watch?v=" + id;
                    }
                }
                if (LooksLikeUrl(entry_url)) {
                    res.urls.push_back(std::move(entry_url));
                    res.titles.push_back(JsonStr(entry, "title"));
                }
            }
        } else if (json.is_object()) {
            std::string single_url = JsonStr(json, "webpage_url");
            if (!LooksLikeUrl(single_url)) single_url = JsonStr(json, "original_url");
            if (!LooksLikeUrl(single_url)) single_url = url;
            if (LooksLikeUrl(single_url)) {
                res.urls.push_back(std::move(single_url));
                res.titles.push_back(JsonStr(json, "title"));
            }
        }

        if (res.urls.empty()) {
            res.error = ErrorCode::ExtractionFailed;
            res.error_message = "Playlist is empty or no playable URLs found";
            return res;
        }
        res.success = true;
        return res;
    } catch (const std::exception& ex) {
        res.error = ErrorCode::ExtractionFailed;
        res.error_message = std::string("Playlist JSON parse error: ") + ex.what();
        return res;
    }
}

void StreamResolver::SetYtDlpTestHook(YtDlpTestHook hook) {
    std::lock_guard<std::mutex> lock(ytdlp_hook_mutex_);
    ytdlp_test_hook_ = std::move(hook);
}

void StreamResolver::SetYtDlpEnvObserverForTesting(EnvObserver observer) {
    std::lock_guard<std::mutex> lock(ytdlp_hook_mutex_);
    ytdlp_env_observer_ = std::move(observer);
}

bool ExecuteSubprocess(
    const std::string& exe_path,
    const std::vector<std::string>& args,
    std::string& out_stdout,
    std::string& out_stderr,
    int timeout_ms,
    const std::atomic<bool>* cancel_flag,
    std::atomic<std::uint32_t>* active_counter,
    const std::vector<std::pair<std::string, std::string>>* env_overrides,
    const std::function<void(const std::string&)>& on_stderr_line,
    bool log_exec_diag) {
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
    std::wstring custom_env_block;
    void* env_ptr = nullptr;
    if (env_overrides && !env_overrides->empty()) {
        std::vector<std::pair<std::wstring, std::wstring>> merged_env;
        if (LPWCH raw_env = GetEnvironmentStringsW()) {
            const wchar_t* cur = raw_env;
            while (*cur != L'\0') {
                std::wstring entry(cur);
                cur += entry.size() + 1;
                auto eq = entry.find(L'=', entry.empty() || entry.front() == L'=' ? 1 : 0);
                if (eq != std::wstring::npos) {
                    merged_env.emplace_back(entry.substr(0, eq), entry.substr(eq + 1));
                }
            }
            FreeEnvironmentStringsW(raw_env);
        }
        for (const auto& [k, v] : *env_overrides) {
            std::wstring wk = Utf8ToWide(k);
            std::wstring wv = Utf8ToWide(v);
            bool replaced = false;
            for (auto& kv : merged_env) {
                if (_wcsicmp(kv.first.c_str(), wk.c_str()) == 0) {
                    kv.second = wv;
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                merged_env.emplace_back(std::move(wk), std::move(wv));
            }
        }
        for (const auto& kv : merged_env) {
            custom_env_block += kv.first;
            custom_env_block.push_back(L'=');
            custom_env_block += kv.second;
            custom_env_block.push_back(L'\0');
        }
        custom_env_block.push_back(L'\0');
        env_ptr = custom_env_block.data();
        create_flags |= CREATE_UNICODE_ENVIRONMENT;
    }
    if (has_attr_list) {
        create_flags |= EXTENDED_STARTUPINFO_PRESENT;
    }

    if (log_exec_diag) {
        std::error_code cwd_ec;
        const std::string cwd = std::filesystem::current_path(cwd_ec).string();
        std::size_t env_count = 0;
        std::string matched_vars;
        InspectWindowsEnvForDiag(env_overrides, env_count, matched_vars);
        Logger::Instance().Debug(
            "yt-dlp child env: cwd=" + (cwd.empty() ? std::string("unknown") : cwd) +
            ", env_count=" + std::to_string(env_count) +
            ", matched_vars=" + (matched_vars.empty() ? std::string("none") : matched_vars));

        const bool stdin_nul = (nul_in && nul_in != INVALID_HANDLE_VALUE && siex.StartupInfo.hStdInput == nul_in);
        const std::string stdin_desc = stdin_nul
            ? "NUL"
            : (siex.StartupInfo.hStdInput ? "pipe_or_handle" : "inherited_or_none");
        const std::string handles_desc = has_attr_list
            ? ("restricted_list(" + std::to_string(inherit_count) + ":stdout_pipe,stderr_pipe" +
               (stdin_nul ? ",nul_stdin" : "") + ")")
            : "unrestricted_inherit_true";
        Logger::Instance().Debug(
            std::string("yt-dlp child process: job_object=") + (job ? "true" : "false") +
            ", inherited_handles=" + handles_desc +
            ", stdin_redirected=" + ((siex.StartupInfo.dwFlags & STARTF_USESTDHANDLES) ? "true" : "false") +
            ", stdin_target=" + stdin_desc);
    }

    PROCESS_INFORMATION pi{};
    BOOL created = CreateProcessW(
        nullptr,
        cmdline.data(),
        nullptr,
        nullptr,
        TRUE,
        create_flags,
        env_ptr,
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
    const auto* line_cb_ptr = on_stderr_line ? &on_stderr_line : nullptr;
    t_out.Start([stdout_read, &out_stdout]() { DrainPipe(stdout_read, out_stdout); }, 128 * 1024);
    t_err.Start([stderr_read, &out_stderr, line_cb_ptr]() {
        DrainPipe(stderr_read, out_stderr, kMaxSubprocessStderrBytes, line_cb_ptr);
    }, 128 * 1024);

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

    std::vector<std::string> env_storage;
    std::vector<char*> custom_envp;
    char** spawn_envp = environ;
    if (env_overrides && !env_overrides->empty()) {
        if (environ) {
            for (char** ep = environ; *ep != nullptr; ++ep) {
                env_storage.emplace_back(*ep);
            }
        }
        for (const auto& [k, v] : *env_overrides) {
            const std::string prefix = k + "=";
            const std::string line = prefix + v;
            bool replaced = false;
            for (auto& existing : env_storage) {
                if (existing.rfind(prefix, 0) == 0) {
                    existing = line;
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                env_storage.push_back(line);
            }
        }
        custom_envp.reserve(env_storage.size() + 1);
        if (env_overrides && !env_overrides->empty()) {
            for (auto& s : env_storage) {
                custom_envp.push_back(s.data());
            }
            custom_envp.push_back(nullptr);
            spawn_envp = custom_envp.data();
        }
    } else if (environ) {
        for (char** ep = environ; *ep != nullptr; ++ep) {
            env_storage.emplace_back(*ep);
        }
    }

    if (log_exec_diag) {
        std::error_code cwd_ec;
        const std::string cwd = std::filesystem::current_path(cwd_ec).string();
        std::string matched_vars;
        for (const auto& kv : env_storage) {
            auto eq = kv.find('=');
            if (eq == std::string::npos || eq == 0) continue;
            std::string name = kv.substr(0, eq);
            std::string upper = name;
            for (char& ch : upper) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            if (MatchesEnvDiagPrefix(upper)) {
                if (!matched_vars.empty()) matched_vars.push_back(',');
                matched_vars += name;
            }
        }
        Logger::Instance().Debug(
            "yt-dlp child env: cwd=" + (cwd.empty() ? std::string("unknown") : cwd) +
            ", env_count=" + std::to_string(env_storage.size()) +
            ", matched_vars=" + (matched_vars.empty() ? std::string("none") : matched_vars));
        Logger::Instance().Debug(
            "yt-dlp child process: job_object=false(pgid=setpgid), inherited_handles=cloexec(stdout_pipe,stderr_pipe,/dev/null), stdin_redirected=true, stdin_target=/dev/null");
    }

    pid_t pid = -1;
    int spawn_rc = posix_spawnp(&pid, exe_path.c_str(), &actions, &attr, argv.data(), spawn_envp);

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
    std::string err_line_pending;
    auto flush_err_line = [&](std::string& ln) {
        if (!ln.empty() && ln.back() == '\r') {
            ln.pop_back();
        }
        if (on_stderr_line && !ln.empty()) {
            on_stderr_line(ln);
        }
    };

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
                const bool is_err = (fds[i].fd == err_pipe[0]);
                std::string& dst = is_err ? out_stderr : out_stdout;
                ssize_t n = read(fds[i].fd, buf.data(), buf.size());
                if (n > 0) {
                    if (!is_err || dst.size() < kMaxSubprocessStderrBytes) {
                        const std::size_t to_append = !is_err
                            ? static_cast<std::size_t>(n)
                            : std::min<std::size_t>(static_cast<std::size_t>(n), kMaxSubprocessStderrBytes - dst.size());
                        dst.append(buf.data(), to_append);
                    }
                    if (is_err && on_stderr_line) {
                        for (ssize_t k = 0; k < n; ++k) {
                            char ch = buf[static_cast<std::size_t>(k)];
                            if (ch == '\n') {
                                flush_err_line(err_line_pending);
                                err_line_pending.clear();
                            } else if (err_line_pending.size() < 4096) {
                                err_line_pending.push_back(ch);
                            }
                        }
                    }
                } else if (n == 0) {
                    if (fds[i].fd == out_pipe[0]) out_open = false;
                    else err_open = false;
                }
            }
        }
    }
    if (on_stderr_line && !err_line_pending.empty()) {
        flush_err_line(err_line_pending);
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

bool StreamResolver::AppendYtDlpCommonArgs(
    std::vector<std::string>& args,
    ActiveJsRuntime* out_runtime,
    bool force_quickjs,
    const CookieSourceSnapshot* cookie_source) const {
    std::string cache_dir = (std::filesystem::path(config_.bin_dir) / "yt-dlp-cache").string();
    args.push_back("--cache-dir");
    args.push_back(std::move(cache_dir));

    if (cookie_source) {
        if ((cookie_source->kind == CookieSourceKind::ConfiguredFile ||
             cookie_source->kind == CookieSourceKind::DefaultFile) &&
            !cookie_source->file_path.empty()) {
            args.push_back("--cookies");
            args.push_back(cookie_source->file_path);
        } else if (cookie_source->kind == CookieSourceKind::BrowserProfile &&
                   !cookie_source->browser_name.empty() &&
                   !cookie_source->profile_path.empty()) {
            args.push_back("--cookies-from-browser");
            args.push_back(cookie_source->browser_name + ":" + cookie_source->profile_path);
        }
    }

    ActiveJsRuntime selected;
    if (force_quickjs) {
        if (config_.enable_js_runtime) {
            std::string qjs_path = binaries_.GetJsRuntimePath();
            if (!qjs_path.empty() && std::filesystem::exists(qjs_path)) {
                selected.kind = JsRuntimeKind::QuickJs;
                selected.path = std::move(qjs_path);
            }
        }
    } else {
        selected = binaries_.SelectJsRuntime();
    }

    if (out_runtime) {
        *out_runtime = selected;
    }

    if (selected.kind == JsRuntimeKind::Deno && !selected.path.empty()) {
        args.push_back("--js-runtimes");
        args.push_back("deno:" + selected.path);
        return true;
    }
    if (selected.kind == JsRuntimeKind::QuickJs && !selected.path.empty()) {
        args.push_back("--js-runtimes");
        args.push_back("quickjs:" + selected.path);
        return true;
    }
    return false;
}

bool StreamResolver::ExecuteYtDlp(
    const std::vector<std::string>& args,
    std::string& out_stdout,
    std::string& out_stderr,
    int timeout_ms,
    const std::atomic<bool>* cancel_flag,
    bool use_deno_env,
    const std::function<void(const std::string&)>& on_stderr_line,
    bool log_exec_diag) const {
    if (IsCancelled(cancel_flag)) {
        out_stderr = "Cancelled";
        return false;
    }
    if (!binaries_.IsReady()) {
        out_stderr = "yt-dlp binary is currently unavailable";
        return false;
    }

    std::vector<std::pair<std::string, std::string>> env_overrides;
    if (use_deno_env) {
        env_overrides.emplace_back("DENO_DIR", binaries_.GetDenoCacheDir());
        env_overrides.emplace_back("DENO_NO_UPDATE_CHECK", "1");
    }

    if (log_exec_diag && config_.debug_logging) {
        Logger::Instance().Debug(
            "yt-dlp command line (first resolve): " +
            FormatCommandLineWithMaskedTarget(binaries_.GetYtDlpPath(), args));
    }

    YtDlpTestHook test_hook;
    EnvObserver env_observer;
    {
        std::lock_guard<std::mutex> lock(ytdlp_hook_mutex_);
        test_hook = ytdlp_test_hook_;
        env_observer = ytdlp_env_observer_;
    }
    if (env_observer) {
        env_observer(env_overrides);
    }
    if (test_hook) {
        const bool ok = test_hook(args, out_stdout, out_stderr, timeout_ms, cancel_flag);
        if (on_stderr_line && !out_stderr.empty()) {
            std::istringstream iss(out_stderr);
            std::string ln;
            while (std::getline(iss, ln)) {
                if (!ln.empty() && ln.back() == '\r') ln.pop_back();
                if (!ln.empty()) on_stderr_line(ln);
            }
        }
        return ok;
    }
    return ExecuteSubprocess(
        binaries_.GetYtDlpPath(),
        args,
        out_stdout,
        out_stderr,
        timeout_ms,
        cancel_flag,
        &active_processes_,
        env_overrides.empty() ? nullptr : &env_overrides,
        on_stderr_line,
        log_exec_diag && config_.debug_logging);
}

bool StreamResolver::InspectYtDlpWarnings(
    const std::string& stderr_text,
    std::string& out_first_challenge_line) const {
    out_first_challenge_line.clear();
    if (stderr_text.empty()) {
        return false;
    }
    std::istringstream iss(stderr_text);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        std::size_t start = 0;
        while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) {
            ++start;
        }
        if (line.size() - start < 8 || line.compare(start, 8, "WARNING:") != 0) {
            continue;
        }
        const std::string warning_line = line.substr(start);
        if (config_.debug_logging) {
            Logger::Instance().Debug(SanitizeWarningLineForLog(warning_line));
        }
        if (out_first_challenge_line.empty() && IsJsChallengeWarningLine(warning_line)) {
            out_first_challenge_line = warning_line;
        }
    }
    return !out_first_challenge_line.empty();
}

void StreamResolver::MaybeWarnJsChallengeRateLimited() const {
    const auto now = std::chrono::steady_clock::now();
    bool should_log = false;
    {
        std::lock_guard<std::mutex> lock(js_warn_mutex_);
        if (!js_challenge_warned_once_ ||
            (now - last_js_challenge_warn_) >= std::chrono::minutes(10)) {
            js_challenge_warned_once_ = true;
            last_js_challenge_warn_ = now;
            should_log = true;
        }
    }
    if (should_log) {
        Logger::Instance().Warn(
            "yt-dlp could not solve the YouTube JS challenge; the media URL may be rejected");
    }
}

bool StreamResolver::ExecuteYtDlpWithJsFallback(
    const std::vector<std::string>& base_args,
    const std::string& target,
    std::string& out_stdout,
    std::string& out_stderr,
    int timeout_ms,
    const std::atomic<bool>* cancel_flag,
    std::string* out_runtime_name,
    std::int64_t* out_js_check_ms,
    bool is_resolve_call,
    const CookieSourceSnapshot* cookie_source) const {
    std::vector<std::string> args = base_args;
    ActiveJsRuntime selected_rt;
    TempCookiesFileGuard main_cookie_guard;
    const CookieSourceSnapshot exec_cookie = PrepareExecutionCookieSnapshot(cookie_source, main_cookie_guard);
    auto t_js_start = std::chrono::steady_clock::now();
    AppendYtDlpCommonArgs(args, &selected_rt, false, &exec_cookie);
    auto js_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t_js_start).count();
    if (out_js_check_ms) {
        *out_js_check_ms = js_ms;
    }
    args.push_back("--");
    args.push_back(target);

    if (out_runtime_name) {
        *out_runtime_name = RuntimeKindLabel(selected_rt.kind);
    }

    const bool log_diag_once =
        is_resolve_call && config_.debug_logging && !logged_exec_env_once_.exchange(true);

    std::vector<std::string> verbose_lines;
    std::function<void(const std::string&)> line_collector;
    if (is_resolve_call && config_.debug_logging) {
        line_collector = [&verbose_lines](const std::string& line) {
            if (IsVerboseYtDlpLine(line) && verbose_lines.size() < 256) {
                verbose_lines.push_back(line);
            }
        };
    }

    auto flush_verbose_lines = [&](bool will_inspect_warnings) {
        if (!config_.debug_logging || verbose_lines.empty()) {
            return;
        }
        for (const auto& ln : verbose_lines) {
            std::size_t s = 0;
            while (s < ln.size() && (ln[s] == ' ' || ln[s] == '\t')) ++s;
            if (will_inspect_warnings && ln.size() - s >= 8 && ln.compare(s, 8, "WARNING:") == 0) {
                continue; // Logged once by InspectYtDlpWarnings below
            }
            Logger::Instance().Debug("yt-dlp -v: " + SanitizeWarningLineForLog(ln));
        }
    };

    auto build_warning_scan_text = [&](const std::string& raw_stderr) -> std::string {
        if (!config_.debug_logging || raw_stderr.size() < kMaxSubprocessStderrBytes || verbose_lines.empty()) {
            return raw_stderr;
        }
        // If raw_stderr hit the 64 KB cap, append filtered lines so late WARNINGs still count.
        std::string combined = raw_stderr;
        for (const auto& ln : verbose_lines) {
            std::size_t s = 0;
            while (s < ln.size() && (ln[s] == ' ' || ln[s] == '\t')) ++s;
            if (ln.size() - s >= 8 && ln.compare(s, 8, "WARNING:") == 0) {
                combined.push_back('\n');
                combined += ln;
            }
        }
        return combined;
    };

    auto retry_with_quickjs = [&](const std::string& reason_snippet) -> bool {
        binaries_.RecordDenoRuntimeFailure();
        std::string brief_err = SanitizeWarningLineForLog(reason_snippet);
        if (brief_err.size() > 160) {
            brief_err.resize(160);
        }
        Logger::Instance().Warn(
            "yt-dlp JS runtime (deno) failed; retrying once with QuickJS: " + brief_err);

        std::vector<std::string> fb_args = base_args;
        ActiveJsRuntime fb_rt;
        TempCookiesFileGuard fb_cookie_guard;
        const CookieSourceSnapshot fb_exec_cookie = PrepareExecutionCookieSnapshot(cookie_source, fb_cookie_guard);
        AppendYtDlpCommonArgs(fb_args, &fb_rt, true, &fb_exec_cookie);
        fb_args.push_back("--");
        fb_args.push_back(target);

        if (out_runtime_name) {
            *out_runtime_name = RuntimeKindLabel(fb_rt.kind);
        }
        out_stdout.clear();
        out_stderr.clear();
        verbose_lines.clear();
        const bool fb_ok = ExecuteYtDlp(fb_args, out_stdout, out_stderr, timeout_ms, cancel_flag, false, line_collector, false);
        flush_verbose_lines(fb_ok);
        if (fb_ok) {
            std::string fb_challenge_warn;
            if (InspectYtDlpWarnings(build_warning_scan_text(out_stderr), fb_challenge_warn)) {
                MaybeWarnJsChallengeRateLimited();
            }
        }
        return fb_ok;
    };

    const bool used_deno = (selected_rt.kind == JsRuntimeKind::Deno);
    bool ok = ExecuteYtDlp(args, out_stdout, out_stderr, timeout_ms, cancel_flag, used_deno, line_collector, log_diag_once);
    main_cookie_guard.Reset();
    flush_verbose_lines(ok);
    if (ok) {
        std::string challenge_warn;
        if (InspectYtDlpWarnings(build_warning_scan_text(out_stderr), challenge_warn)) {
            if (used_deno && !IsCancelled(cancel_flag)) {
                return retry_with_quickjs(challenge_warn);
            }
            MaybeWarnJsChallengeRateLimited();
        } else if (used_deno) {
            binaries_.RecordDenoSuccess();
        }
        return true;
    }

    if (used_deno && !IsCancelled(cancel_flag) && IsDenoRuntimeFailure(out_stderr)) {
        return retry_with_quickjs(out_stderr);
    }

    return false;
}

bool StreamResolver::ExecuteYtDlpWithAuthFallback(
    const std::vector<std::string>& base_args,
    const std::string& target,
    std::string& out_stdout,
    std::string& out_stderr,
    int timeout_ms,
    const std::atomic<bool>* cancel_flag,
    std::string* out_runtime_name,
    std::int64_t* out_js_check_ms,
    bool is_resolve_call,
    bool& out_auth_required,
    bool& out_age_restricted) {
    out_auth_required = false;
    out_age_restricted = false;

    CookieSourceSnapshot initial_cookie = GetActiveCookieSnapshot();
    if (ExecuteYtDlpWithJsFallback(
            base_args,
            target,
            out_stdout,
            out_stderr,
            timeout_ms,
            cancel_flag,
            out_runtime_name,
            out_js_check_ms,
            is_resolve_call,
            &initial_cookie)) {
        return true;
    }

    if (IsCancelled(cancel_flag)) {
        return false;
    }

    bool age_restricted = false;
    if (!IsYouTubeAuthChallenge(out_stderr, &age_restricted)) {
        return false;
    }

    std::unique_lock<std::mutex> lock(cookie_mutex_);
    while (cookie_discovery_in_progress_ && !IsCancelled(cancel_flag)) {
        cookie_cv_.wait_for(lock, std::chrono::milliseconds(50));
    }
    if (IsCancelled(cancel_flag)) {
        return false;
    }

    if ((initial_cookie.kind == CookieSourceKind::ConfiguredFile ||
         initial_cookie.kind == CookieSourceKind::DefaultFile) &&
        active_cookie_source_.kind == initial_cookie.kind &&
        active_cookie_source_.generation == initial_cookie.generation) {
        cookies_file_expired_ = true;
    }

    // Check if scriptfiles/SampAudioStream/cookies.txt was newly created or updated at runtime.
    bool switched_to_new_default_cookies = false;
    CookieSourceSnapshot retry_cookie;
    if (config_.cookies_file.empty()) {
        const std::filesystem::path default_path = GetDefaultCookiesPath();
        std::error_code ec;
        if (std::filesystem::is_regular_file(default_path, ec) && !ec) {
            const auto mtime = std::filesystem::last_write_time(default_path, ec);
            const bool is_new_or_updated =
                !ec && (!have_default_cookies_mtime_ || mtime != default_cookies_mtime_);
            if (is_new_or_updated) {
                default_cookies_mtime_ = mtime;
                have_default_cookies_mtime_ = true;
                cookies_file_expired_ = false;
                failed_discovery_cooldown_active_ = false;
                active_cookie_source_ = CookieSourceSnapshot{};
                active_cookie_source_.kind = CookieSourceKind::DefaultFile;
                active_cookie_source_.file_path = default_path.string();
                active_cookie_source_.generation = ++cookie_state_generation_;
                retry_cookie = active_cookie_source_;
                switched_to_new_default_cookies = true;
            }
        }
    }

    if (switched_to_new_default_cookies) {
        lock.unlock();
        Logger::Instance().Info(
            "Detected YouTube cookies file at " + retry_cookie.file_path + "; using it for YouTube requests.");
        out_stdout.clear();
        out_stderr.clear();
        if (ExecuteYtDlpWithJsFallback(
                base_args,
                target,
                out_stdout,
                out_stderr,
                timeout_ms,
                cancel_flag,
                out_runtime_name,
                out_js_check_ms,
                is_resolve_call,
                &retry_cookie)) {
            return true;
        }
        if (IsCancelled(cancel_flag)) {
            return false;
        }
        bool retry_age = false;
        if (!IsYouTubeAuthChallenge(out_stderr, &retry_age)) {
            return false;
        }
        age_restricted = retry_age;

        lock.lock();
        while (cookie_discovery_in_progress_ && !IsCancelled(cancel_flag)) {
            cookie_cv_.wait_for(lock, std::chrono::milliseconds(50));
        }
        if (IsCancelled(cancel_flag)) {
            return false;
        }
        if ((retry_cookie.kind == CookieSourceKind::ConfiguredFile ||
             retry_cookie.kind == CookieSourceKind::DefaultFile) &&
            active_cookie_source_.kind == retry_cookie.kind &&
            active_cookie_source_.generation == retry_cookie.generation) {
            cookies_file_expired_ = true;
        }
        initial_cookie = retry_cookie;
    }

    // If a concurrent thread just discovered a working source while we waited, reuse it directly.
    if (cookie_state_generation_ != initial_cookie.generation &&
        active_cookie_source_.kind != CookieSourceKind::None &&
        !cookies_file_expired_) {
        retry_cookie = active_cookie_source_;
        lock.unlock();
        out_stdout.clear();
        out_stderr.clear();
        if (ExecuteYtDlpWithJsFallback(
                base_args,
                target,
                out_stdout,
                out_stderr,
                timeout_ms,
                cancel_flag,
                out_runtime_name,
                out_js_check_ms,
                is_resolve_call,
                &retry_cookie)) {
            return true;
        }
        if (IsCancelled(cancel_flag)) {
            return false;
        }
        bool retry_age = false;
        if (!IsYouTubeAuthChallenge(out_stderr, &retry_age)) {
            return false;
        }
        out_auth_required = true;
        out_age_restricted = retry_age;
        std::lock_guard<std::mutex> wlk(cookie_mutex_);
        MaybeWarnAuthRequiredLocked(retry_age);
        return false;
    }

    // If a previously pinned browser source stopped working, invalidate it so one fresh discovery can run.
    if (initial_cookie.kind == CookieSourceKind::BrowserProfile &&
        active_cookie_source_.generation == initial_cookie.generation) {
        active_cookie_source_ = CookieSourceSnapshot{};
        failed_discovery_cooldown_active_ = false;
    }

    const auto now_tp = std::chrono::steady_clock::now();
    const bool cooldown_active =
        failed_discovery_cooldown_active_ &&
        (now_tp - last_failed_discovery_) < std::chrono::minutes(30);
    if (config_.cookies_from_browser == "off" || cooldown_active) {
        MaybeWarnAuthRequiredLocked(age_restricted);
        out_auth_required = true;
        out_age_restricted = age_restricted;
        return false;
    }

    // Become the single discovery leader for all concurrently blocked requests.
    cookie_discovery_in_progress_ = true;
    cookie_discovery_count_.fetch_add(1);
    struct DiscoveryGuard {
        std::unique_lock<std::mutex>& lk;
        bool& in_progress;
        std::condition_variable& cv;
        ~DiscoveryGuard() {
            if (!lk.owns_lock()) {
                lk.lock();
            }
            in_progress = false;
            cv.notify_all();
        }
    } discovery_guard{lock, cookie_discovery_in_progress_, cookie_cv_};
    lock.unlock();

    const std::vector<BrowserCookieCandidate> candidates = DiscoverBrowserCookieCandidates();
    const std::size_t max_candidates = std::min<std::size_t>(candidates.size(), 4);
    const int total_budget_ms = std::max(1, cookie_discovery_budget_ms_.load());
    const auto discovery_deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(total_budget_ms);
    std::string checked_summary;

    for (std::size_t i = 0; i < max_candidates; ++i) {
        if (IsCancelled(cancel_flag)) {
            return false;
        }
        const auto rem_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            discovery_deadline - std::chrono::steady_clock::now()).count();
        if (rem_ms <= 0) {
            Logger::Instance().Debug(
                "YouTube browser cookie discovery reached the " +
                std::to_string(std::max(1, total_budget_ms / 1000)) + " s total timeout cap.");
            break;
        }
        const int cand_timeout_ms = static_cast<int>(std::min<long long>(timeout_ms, rem_ms));

        const auto& c = candidates[i];
        if (!checked_summary.empty()) {
            checked_summary += ", ";
        }
        checked_summary += c.display_name + " (" + c.profile_name + ")";

        CookieSourceSnapshot cand;
        cand.kind = CookieSourceKind::BrowserProfile;
        cand.browser_name = c.browser_name;
        cand.display_name = c.display_name;
        cand.profile_name = c.profile_name;
        cand.profile_path = c.profile_path;

        std::string cand_stdout;
        std::string cand_stderr;
        std::string cand_rt;
        std::int64_t cand_js_ms = 0;
        const bool cand_ok = ExecuteYtDlpWithJsFallback(
            base_args,
            target,
            cand_stdout,
            cand_stderr,
            cand_timeout_ms,
            cancel_flag,
            out_runtime_name ? &cand_rt : nullptr,
            out_js_check_ms ? &cand_js_ms : nullptr,
            is_resolve_call,
            &cand);
        if (cand_ok) {
            out_stdout = std::move(cand_stdout);
            out_stderr = std::move(cand_stderr);
            if (out_runtime_name) {
                *out_runtime_name = std::move(cand_rt);
            }
            if (out_js_check_ms) {
                *out_js_check_ms = cand_js_ms;
            }
            lock.lock();
            cand.generation = ++cookie_state_generation_;
            active_cookie_source_ = cand;
            failed_discovery_cooldown_active_ = false;
            cookies_file_expired_ = false;
            lock.unlock();
            Logger::Instance().Info(
                "YouTube challenged this server's IP; using cookies from " + cand.display_name +
                " (profile: " + cand.profile_name +
                "). The account logged in there is used for YouTube requests; a secondary account is recommended.");
            return true;
        }
        if (IsCancelled(cancel_flag)) {
            return false;
        }
        Logger::Instance().Debug(
            "YouTube browser cookie candidate " + cand.display_name +
            " (profile: " + cand.profile_name + ") failed: " +
            SummarizeBrowserCookieFailure(cand_stderr));
    }

    lock.lock();
    if (active_cookie_source_.kind == CookieSourceKind::BrowserProfile) {
        active_cookie_source_ = CookieSourceSnapshot{};
    }
    ++cookie_state_generation_;
    failed_discovery_cooldown_active_ = true;
    last_failed_discovery_ = std::chrono::steady_clock::now();
    last_checked_browsers_summary_ = checked_summary.empty() ? std::string("none") : checked_summary;
    out_auth_required = true;
    out_age_restricted = age_restricted;
    MaybeWarnAuthRequiredLocked(age_restricted);
    return false;
}

std::size_t StreamResolver::PruneYtDlpCacheDir(
    const std::string& cache_dir,
    std::chrono::seconds max_age,
    std::uint64_t max_total_bytes,
    std::chrono::seconds min_protect_age,
    std::unordered_set<std::string>* logged_unknown_subdirs) {
    if (cache_dir.empty() || max_age.count() <= 0) {
        return 0;
    }

    std::error_code ec;
    const std::filesystem::path root(cache_dir);
    if (!std::filesystem::exists(root, ec) || ec || !std::filesystem::is_directory(root, ec) || ec) {
        return 0;
    }

    // Verified against yt-dlp 2026.10.26 (yt_dlp/cache.py + yt_dlp/extractor/youtube/_video.py):
    // 'youtube-sigfuncs' holds rotatable per-player-JS signature caches safe to prune by age.
    // Other sections (e.g. 'challenge-solver', 'deno', 'deno-cache', auth/token caches) stay untouched.
    auto is_allowlisted_subdir = [](const std::string& name) -> bool {
        return name == "youtube-sigfuncs";
    };
    auto is_preserved_known_subdir = [](const std::string& name) -> bool {
        return name == "deno" || name == "deno-cache";
    };

    struct CacheFileEntry {
        std::filesystem::path path;
        std::filesystem::file_time_type mtime;
        std::uint64_t size{0};
    };

    std::vector<CacheFileEntry> candidates;
    std::uint64_t managed_bytes = 0;

    std::filesystem::directory_iterator top_it(
        root,
        std::filesystem::directory_options::skip_permission_denied,
        ec);
    const std::filesystem::directory_iterator top_end;
    for (; top_it != top_end && !ec; top_it.increment(ec)) {
        std::error_code entry_ec;
        if (top_it->is_symlink(entry_ec) || entry_ec || !top_it->is_directory(entry_ec) || entry_ec) {
            continue;
        }

        const std::string subdir_name = top_it->path().filename().string();
        if (is_preserved_known_subdir(subdir_name)) {
            continue;
        }
        const bool allowlisted = is_allowlisted_subdir(subdir_name);
        if (!allowlisted) {
            if (logged_unknown_subdirs && logged_unknown_subdirs->insert(subdir_name).second) {
                Logger::Instance().Debug("Skipping unmanaged yt-dlp cache subdirectory: " + subdir_name);
            }
            continue;
        }

        std::error_code rec_ec;
        std::filesystem::recursive_directory_iterator sub_it(
            top_it->path(),
            std::filesystem::directory_options::skip_permission_denied,
            rec_ec);
        const std::filesystem::recursive_directory_iterator sub_end;
        for (; sub_it != sub_end && !rec_ec; sub_it.increment(rec_ec)) {
            std::error_code file_ec;
            if (sub_it->is_symlink(file_ec) || file_ec || !sub_it->is_regular_file(file_ec) || file_ec) {
                continue;
            }
            std::error_code meta_ec;
            const auto sz = sub_it->file_size(meta_ec);
            const std::uint64_t file_bytes = meta_ec ? 0ULL : static_cast<std::uint64_t>(sz);
            managed_bytes += file_bytes;
            meta_ec.clear();
            const auto mtime = sub_it->last_write_time(meta_ec);
            if (meta_ec) {
                continue;
            }
            candidates.push_back(CacheFileEntry{sub_it->path(), mtime, file_bytes});
        }
    }

    const auto now = std::filesystem::file_time_type::clock::now();
    std::size_t removed = 0;
    std::vector<CacheFileEntry> survivors;
    survivors.reserve(candidates.size());

    // 1. Delete allowlisted entries older than max_age (minimum 1 day per config range).
    for (const auto& entry : candidates) {
        const bool older_than_max =
            (now > entry.mtime) &&
            (std::chrono::duration_cast<std::chrono::seconds>(now - entry.mtime) >= max_age);
        if (older_than_max) {
            std::error_code rm_ec;
            if (std::filesystem::remove(entry.path, rm_ec) && !rm_ec) {
                ++removed;
                managed_bytes -= std::min(managed_bytes, entry.size);
                continue;
            }
        }
        survivors.push_back(entry);
    }

    // 2. Safety cap: if managed allowlist size still exceeds max_total_bytes (64 MB default),
    // prune oldest managed entries first while preserving anything modified within min_protect_age (1 h).
    if (max_total_bytes > 0 && managed_bytes > max_total_bytes && !survivors.empty()) {
        std::sort(survivors.begin(), survivors.end(), [](const CacheFileEntry& a, const CacheFileEntry& b) {
            if (a.mtime != b.mtime) {
                return a.mtime < b.mtime;
            }
            return a.path.filename().string() < b.path.filename().string();
        });

        for (const auto& entry : survivors) {
            if (managed_bytes <= max_total_bytes) {
                break;
            }
            if (min_protect_age.count() > 0) {
                if (entry.mtime >= now ||
                    std::chrono::duration_cast<std::chrono::seconds>(now - entry.mtime) < min_protect_age) {
                    continue;
                }
            }
            std::error_code rm_ec;
            if (std::filesystem::remove(entry.path, rm_ec) && !rm_ec) {
                ++removed;
                managed_bytes -= std::min(managed_bytes, entry.size);
            }
        }
    }

    return removed;
}

void StreamResolver::MaybePruneYtDlpCache(bool force) {
    if (shutting_down_.load() || config_.bin_dir.empty() || config_.yt_dlp_cache_max_age_days == 0) {
        return;
    }
    if (!force && active_processes_.load() > 0) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    std::unordered_set<std::string> unknown_snapshot;
    {
        std::lock_guard<std::mutex> lock(cache_prune_mutex_);
        constexpr auto kPruneInterval = std::chrono::minutes(10);
        if (!force && last_cache_prune_ != std::chrono::steady_clock::time_point{} &&
            (now - last_cache_prune_) < kPruneInterval) {
            return;
        }
        last_cache_prune_ = now;
        unknown_snapshot = logged_unknown_cache_subdirs_;
    }

    const std::string cache_dir = (std::filesystem::path(config_.bin_dir) / "yt-dlp-cache").string();
    const auto max_age = std::chrono::hours(24LL * static_cast<long long>(config_.yt_dlp_cache_max_age_days));
    const std::size_t removed = PruneYtDlpCacheDir(
        cache_dir,
        max_age,
        kYtDlpCacheMaxTotalBytes,
        std::chrono::hours(1),
        &unknown_snapshot);
    {
        std::lock_guard<std::mutex> lock(cache_prune_mutex_);
        logged_unknown_cache_subdirs_.insert(unknown_snapshot.begin(), unknown_snapshot.end());
    }
    if (removed > 0) {
        Logger::Instance().Debug(
            "Pruned " + std::to_string(removed) + " obsolete file(s) from yt-dlp cache: " + cache_dir);
    }
}

void StreamResolver::MaybeScheduleCachePrune(ThreadPool& pool) {
    if (shutting_down_.load() || config_.bin_dir.empty() || config_.yt_dlp_cache_max_age_days == 0) {
        return;
    }
    if (active_processes_.load() > 0 || cache_prune_in_progress_.load()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(cache_prune_mutex_);
        constexpr auto kPruneInterval = std::chrono::minutes(10);
        if (last_cache_prune_ != std::chrono::steady_clock::time_point{} &&
            (now - last_cache_prune_) < kPruneInterval) {
            return;
        }
    }

    bool expected = false;
    if (!cache_prune_in_progress_.compare_exchange_strong(expected, true)) {
        return;
    }

    if (!pool.Post([this]() {
            if (!shutting_down_.load() && active_processes_.load() == 0) {
                MaybePruneYtDlpCache(true);
            }
            cache_prune_in_progress_.store(false);
        })) {
        cache_prune_in_progress_.store(false);
    }
}

} // namespace sampaudiostream
