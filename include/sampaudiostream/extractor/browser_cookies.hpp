#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace sampaudiostream {

struct BrowserCookieCandidate {
    std::string browser_name; // yt-dlp browser identifier ("firefox", "chrome", ...)
    std::string display_name; // Human-readable browser name ("Firefox", "Chrome", ...)
    std::string profile_name; // Profile directory basename
    std::string profile_path; // Explicit profile path passed after "<browser_name>:"
    std::filesystem::file_time_type modified_at{};
};

struct BrowserCookieSearchBaseDirs {
    std::string appdata;
    std::string localappdata;
    std::string home;
};

// Discovers browser cookie profiles on disk without invoking external processes.
// Order: Firefox first (newest cookie DB first), then Chromium-based browsers
// (Chrome, Edge, Brave, Chromium, Vivaldi, Opera) with newest cookie DB first per browser.
std::vector<BrowserCookieCandidate> DiscoverBrowserCookieCandidates();

// Test-only override redirecting %APPDATA%, %LOCALAPPDATA%, and $HOME to temporary directories.
void SetBrowserCookieSearchBaseDirsForTesting(bool enabled, const BrowserCookieSearchBaseDirs& dirs = {});

} // namespace sampaudiostream
