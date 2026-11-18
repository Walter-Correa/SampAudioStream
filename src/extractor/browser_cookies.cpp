#include "sampaudiostream/extractor/browser_cookies.hpp"

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <string_view>
#include <system_error>
#include <unordered_set>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace sampaudiostream {

namespace {

std::mutex g_test_base_dirs_mutex;
bool g_use_test_base_dirs = false;
BrowserCookieSearchBaseDirs g_test_base_dirs;

std::string PathToUtf8(const std::filesystem::path& p) {
    const auto u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

std::filesystem::path Utf8ToPath(const std::string& utf8) {
    return std::filesystem::path(std::u8string_view(
        reinterpret_cast<const char8_t*>(utf8.data()),
        utf8.size()));
}

#ifdef _WIN32
std::string ReadWinEnvUtf8(const wchar_t* name) {
    const wchar_t* val = _wgetenv(name);
    if (!val || !*val) {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, val, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, val, -1, out.data(), needed, nullptr, nullptr);
    return out;
}
#endif

BrowserCookieSearchBaseDirs ResolveEffectiveBaseDirs() {
    {
        std::lock_guard<std::mutex> lock(g_test_base_dirs_mutex);
        if (g_use_test_base_dirs) {
            return g_test_base_dirs;
        }
    }

    BrowserCookieSearchBaseDirs dirs;
#ifdef _WIN32
    dirs.appdata = ReadWinEnvUtf8(L"APPDATA");
    dirs.localappdata = ReadWinEnvUtf8(L"LOCALAPPDATA");
#else
    if (const char* home = std::getenv("HOME"); home && *home) {
        dirs.home = home;
    } else {
        struct passwd pwd{};
        struct passwd* result = nullptr;
        std::vector<char> buf(4096);
        if (getpwuid_r(getuid(), &pwd, buf.data(), buf.size(), &result) == 0 && result && result->pw_dir) {
            dirs.home = result->pw_dir;
        }
    }
#endif
    return dirs;
}

bool IsChromiumProfileDirName(const std::string& name) {
    return name == "Default" || name.rfind("Profile ", 0) == 0;
}

bool FindNewestCookieMtime(
    const std::filesystem::path& profile_dir,
    bool is_firefox,
    std::filesystem::file_time_type& out_mtime) {
    std::error_code ec;
    if (is_firefox) {
        const auto db = profile_dir / "cookies.sqlite";
        if (!std::filesystem::is_regular_file(db, ec) || ec) {
            return false;
        }
        out_mtime = std::filesystem::last_write_time(db, ec);
        return !ec;
    }

    bool found = false;
    std::filesystem::file_time_type newest{};
    for (const std::filesystem::path& candidate : {
             profile_dir / "Network" / "Cookies",
             profile_dir / "Cookies"}) {
        std::error_code f_ec;
        if (!std::filesystem::is_regular_file(candidate, f_ec) || f_ec) {
            continue;
        }
        const auto mtime = std::filesystem::last_write_time(candidate, f_ec);
        if (f_ec) {
            continue;
        }
        if (!found || mtime > newest) {
            newest = mtime;
            found = true;
        }
    }
    if (found) {
        out_mtime = newest;
    }
    return found;
}

void ScanProfileSubdirs(
    const std::string& browser_name,
    const std::string& display_name,
    const std::filesystem::path& root_dir,
    bool is_firefox,
    std::vector<BrowserCookieCandidate>& out) {
    std::error_code ec;
    if (!std::filesystem::is_directory(root_dir, ec) || ec) {
        return;
    }
    std::filesystem::directory_iterator it(
        root_dir,
        std::filesystem::directory_options::skip_permission_denied,
        ec);
    const std::filesystem::directory_iterator end;
    for (; it != end && !ec; it.increment(ec)) {
        std::error_code item_ec;
        if (!it->is_directory(item_ec) || item_ec) {
            continue;
        }
        const std::filesystem::path prof_dir = it->path();
        const std::string prof_name = PathToUtf8(prof_dir.filename());
        if (!is_firefox && !IsChromiumProfileDirName(prof_name)) {
            continue;
        }
        std::filesystem::file_time_type mtime{};
        if (!FindNewestCookieMtime(prof_dir, is_firefox, mtime)) {
            continue;
        }
        std::filesystem::path preferred_dir = prof_dir;
        preferred_dir.make_preferred();
        out.push_back(BrowserCookieCandidate{
            browser_name,
            display_name,
            prof_name,
            PathToUtf8(preferred_dir),
            mtime
        });
    }
}

void ScanSingleProfileDir(
    const std::string& browser_name,
    const std::string& display_name,
    const std::filesystem::path& prof_dir,
    std::vector<BrowserCookieCandidate>& out) {
    std::error_code ec;
    if (!std::filesystem::is_directory(prof_dir, ec) || ec) {
        return;
    }
    std::filesystem::file_time_type mtime{};
    if (!FindNewestCookieMtime(prof_dir, false, mtime)) {
        return;
    }
    std::filesystem::path preferred_dir = prof_dir;
    preferred_dir.make_preferred();
    out.push_back(BrowserCookieCandidate{
        browser_name,
        display_name,
        PathToUtf8(prof_dir.filename()),
        PathToUtf8(preferred_dir),
        mtime
    });
}

void SortAndAppendUnique(
    std::vector<BrowserCookieCandidate>& bucket,
    std::unordered_set<std::string>& seen_keys,
    std::vector<BrowserCookieCandidate>& dst) {
    std::sort(
        bucket.begin(),
        bucket.end(),
        [](const BrowserCookieCandidate& a, const BrowserCookieCandidate& b) {
            if (a.modified_at != b.modified_at) {
                return a.modified_at > b.modified_at;
            }
            return a.profile_path < b.profile_path;
        });
    for (auto& c : bucket) {
        const std::string key = c.browser_name + ":" + c.profile_path;
        if (seen_keys.insert(key).second) {
            dst.push_back(std::move(c));
        }
    }
}

} // namespace

void SetBrowserCookieSearchBaseDirsForTesting(bool enabled, const BrowserCookieSearchBaseDirs& dirs) {
    std::lock_guard<std::mutex> lock(g_test_base_dirs_mutex);
    g_use_test_base_dirs = enabled;
    g_test_base_dirs = enabled ? dirs : BrowserCookieSearchBaseDirs{};
}

std::vector<BrowserCookieCandidate> DiscoverBrowserCookieCandidates() {
    const BrowserCookieSearchBaseDirs bases = ResolveEffectiveBaseDirs();
    const std::filesystem::path appdata = !bases.appdata.empty() ? Utf8ToPath(bases.appdata) : std::filesystem::path{};
    const std::filesystem::path localappdata = !bases.localappdata.empty() ? Utf8ToPath(bases.localappdata) : std::filesystem::path{};
    const std::filesystem::path home = !bases.home.empty() ? Utf8ToPath(bases.home) : std::filesystem::path{};

    std::vector<BrowserCookieCandidate> ordered;
    std::unordered_set<std::string> seen_keys;

    // 1. Firefox first (works on Windows/Linux without stopping the browser or requiring an OS keyring).
    {
        std::vector<BrowserCookieCandidate> bucket;
        if (!appdata.empty()) {
            ScanProfileSubdirs("firefox", "Firefox", appdata / "Mozilla" / "Firefox" / "Profiles", true, bucket);
        }
        if (!home.empty()) {
            ScanProfileSubdirs("firefox", "Firefox", home / ".mozilla" / "firefox", true, bucket);
            ScanProfileSubdirs("firefox", "Firefox", home / "snap" / "firefox" / "common" / ".mozilla" / "firefox", true, bucket);
            ScanProfileSubdirs("firefox", "Firefox", home / ".var" / "app" / "org.mozilla.firefox" / ".mozilla" / "firefox", true, bucket);
        }
        SortAndAppendUnique(bucket, seen_keys, ordered);
    }

    // 2. Chromium-based browsers in order: Chrome, Edge, Brave, Chromium, Vivaldi.
    struct ChromiumMultiProfileSpec {
        const char* browser_name;
        const char* display_name;
        const char* win_local_subpath;
        const char* linux_home_subpath_1;
        const char* linux_home_subpath_2;
    };
    static constexpr ChromiumMultiProfileSpec kChromiumBrowsers[] = {
        {"chrome", "Chrome", "Google/Chrome/User Data", ".config/google-chrome", nullptr},
        {"edge", "Edge", "Microsoft/Edge/User Data", ".config/microsoft-edge", nullptr},
        {"brave", "Brave", "BraveSoftware/Brave-Browser/User Data", ".config/BraveSoftware/Brave-Browser", nullptr},
        {"chromium", "Chromium", "Chromium/User Data", ".config/chromium", "snap/chromium/common/chromium"},
        {"vivaldi", "Vivaldi", "Vivaldi/User Data", ".config/vivaldi", nullptr}
    };
    for (const auto& spec : kChromiumBrowsers) {
        std::vector<BrowserCookieCandidate> bucket;
        if (!localappdata.empty() && spec.win_local_subpath) {
            ScanProfileSubdirs(spec.browser_name, spec.display_name, localappdata / spec.win_local_subpath, false, bucket);
        }
        if (!home.empty()) {
            if (spec.linux_home_subpath_1) {
                ScanProfileSubdirs(spec.browser_name, spec.display_name, home / spec.linux_home_subpath_1, false, bucket);
            }
            if (spec.linux_home_subpath_2) {
                ScanProfileSubdirs(spec.browser_name, spec.display_name, home / spec.linux_home_subpath_2, false, bucket);
            }
        }
        SortAndAppendUnique(bucket, seen_keys, ordered);
    }

    // 3. Opera (uses single profile root directory).
    {
        std::vector<BrowserCookieCandidate> bucket;
        if (!appdata.empty()) {
            ScanSingleProfileDir("opera", "Opera", appdata / "Opera Software" / "Opera Stable", bucket);
        }
        if (!home.empty()) {
            ScanSingleProfileDir("opera", "Opera", home / ".config" / "opera", bucket);
        }
        SortAndAppendUnique(bucket, seen_keys, ordered);
    }

    return ordered;
}

} // namespace sampaudiostream
