#pragma once

#include "sampaudiostream/core/config.hpp"
#include "sampaudiostream/core/types.hpp"
#include "sampaudiostream/extractor/binary_manager.hpp"
#include "sampaudiostream/extractor/metadata_cache.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sampaudiostream {

class ThreadPool;

struct ResolveResult {
    bool success{false};
    SourceType source_type{SourceType::DirectAudio};
    ErrorCode error{ErrorCode::None};
    std::string error_message;
    TrackMetadata metadata;
};

struct SearchExecutionResult {
    bool success{false};
    ErrorCode error{ErrorCode::None};
    std::string error_message;
    std::vector<SearchResultItem> items;
};

struct PlaylistExtractionResult {
    bool success{false};
    ErrorCode error{ErrorCode::None};
    std::string error_message;
    std::vector<std::string> urls;
    std::vector<std::string> titles;
};

struct AudioLibraryFolderData {
    std::string utf8_name;
    std::string pawn_name;
    std::vector<std::string> utf8_tracks;
    std::vector<std::string> pawn_tracks;
};

class StreamResolver {
public:
    StreamResolver(const PluginConfig& config, BinaryManager& binaries, MetadataCache& cache);

    static bool LooksLikeUrl(const std::string& input);
    static bool IsLibraryUrl(const std::string& input);
    static bool LooksLikeDiskPath(const std::string& input);
    static bool ParseAndResolveLibraryFile(
        const std::string& url,
        const std::string& music_dir,
        std::filesystem::path& out_real_path,
        std::string& out_file_name,
        ErrorCode& out_error_code,
        std::string& out_error_message);
    static bool ParseAndResolveLibraryFolder(
        const std::string& url,
        const std::string& music_dir,
        std::string& out_folder_utf8,
        std::filesystem::path& out_real_dir,
        ErrorCode& out_error_code,
        std::string& out_error_message);
    // Validates a sound reference ("file.ext" or "Folder/file.ext") strictly inside sound_dir.
    static bool ParseAndResolveSoundFile(
        const std::string& sound,
        const std::string& sound_dir,
        std::filesystem::path& out_real_path,
        std::string& out_ext_lower,
        ErrorCode& out_error_code,
        std::string& out_error_message);
    static std::vector<AudioLibraryFolderData> ScanMusicLibraryDir(
        const std::string& music_dir,
        const std::string& pawn_text_encoding,
        std::uint32_t max_folders = 200,
        std::uint32_t max_tracks_per_folder = 1000);
    static bool IsKnownDirectAudioExtension(const std::string& url);
    static bool IsDirectAudioFileUrl(const std::string& url);
    static std::string ExtractUrlFileNameTitle(const std::string& url);
    static bool IsHlsPlaylistUrl(const std::string& url);
    static bool IsKnownPlatformUrl(const std::string& url);
    static bool IsYouTubeAuthChallenge(const std::string& stderr_text, bool* out_is_age_restricted = nullptr);
    static std::string ScrubCookieValues(const std::string& text);
    static std::string ParsePlaylistFirstStreamUrl(const std::string& body, const std::string& base_url = "");
    static constexpr std::uint32_t kYtDlpCacheDefaultMaxAgeDays = 7;
    static constexpr std::uint64_t kYtDlpCacheMaxTotalBytes = 64ULL * 1024ULL * 1024ULL;

    static std::size_t PruneYtDlpCacheDir(
        const std::string& cache_dir,
        std::chrono::seconds max_age = std::chrono::hours(24 * kYtDlpCacheDefaultMaxAgeDays),
        std::uint64_t max_total_bytes = kYtDlpCacheMaxTotalBytes,
        std::chrono::seconds min_protect_age = std::chrono::hours(1),
        std::unordered_set<std::string>* logged_unknown_subdirs = nullptr);

    void CancelAll();
    void ResetCancel();
    void PollCancellations();
    void MaybePruneYtDlpCache(bool force = false);
    void MaybeScheduleCachePrune(ThreadPool& pool);
    std::uint32_t ActiveProcessCount() const { return active_processes_.load(); }

    using YtDlpTestHook = std::function<bool(
        const std::vector<std::string>& args,
        std::string& out_stdout,
        std::string& out_stderr,
        int timeout_ms,
        const std::atomic<bool>* cancel_flag)>;

    ResolveResult Resolve(const std::string& url_or_query, const std::atomic<bool>* cancel_flag = nullptr);
    void InvalidateCachedTrack(const std::string& url_or_query);
    void InvalidateCachedLibraryTracks();
    SearchExecutionResult Search(const std::string& query, std::uint32_t max_results, const std::atomic<bool>* cancel_flag = nullptr);
    PlaylistExtractionResult ExtractPlaylist(const std::string& url, std::uint32_t max_items, const std::atomic<bool>* cancel_flag = nullptr);
    void SetYtDlpTestHook(YtDlpTestHook hook);
    using EnvObserver = std::function<void(const std::vector<std::pair<std::string, std::string>>& env)>;
    void SetYtDlpEnvObserverForTesting(EnvObserver observer);
    int GetCookieDiscoveryCountForTesting() const { return cookie_discovery_count_.load(); }
    static void SetSimulateRootUidForTesting(bool simulate_root);
    void SetCookieDiscoveryBudgetMsForTesting(int budget_ms) { cookie_discovery_budget_ms_.store(budget_ms); }

private:
    enum class CookieSourceKind {
        None = 0,
        ConfiguredFile = 1,
        DefaultFile = 2,
        BrowserProfile = 3
    };

    struct CookieSourceSnapshot {
        CookieSourceKind kind{CookieSourceKind::None};
        std::string file_path;
        std::string browser_name;
        std::string display_name;
        std::string profile_name;
        std::string profile_path;
        std::uint64_t generation{0};
    };

    struct TempCookiesFileGuard {
        std::filesystem::path path;
        TempCookiesFileGuard() = default;
        ~TempCookiesFileGuard();
        TempCookiesFileGuard(const TempCookiesFileGuard&) = delete;
        TempCookiesFileGuard& operator=(const TempCookiesFileGuard&) = delete;
        void Reset();
    };
    template <typename ResultT>
    struct InflightEntry {
        std::mutex mutex;
        std::condition_variable cv;
        bool done{false};
        std::atomic<bool> combined_cancel{false};
        std::vector<const std::atomic<bool>*> subscriber_cancels;
        ResultT result;

        void AddSubscriber(const std::atomic<bool>* flag) {
            std::lock_guard<std::mutex> lk(mutex);
            subscriber_cancels.push_back(flag);
            combined_cancel.store(false);
        }

        void RemoveSubscriber(const std::atomic<bool>* flag, bool shutting_down) {
            std::lock_guard<std::mutex> lk(mutex);
            auto it = std::find(subscriber_cancels.begin(), subscriber_cancels.end(), flag);
            if (it != subscriber_cancels.end()) {
                subscriber_cancels.erase(it);
            }
            EvaluateLocked(shutting_down);
        }

        bool RefreshCombinedCancel(bool shutting_down) {
            if (shutting_down) {
                combined_cancel.store(true);
                return true;
            }
            std::lock_guard<std::mutex> lk(mutex);
            return EvaluateLocked(false);
        }

    private:
        bool EvaluateLocked(bool shutting_down) {
            if (shutting_down) {
                combined_cancel.store(true);
                return true;
            }
            if (done) {
                return false;
            }
            if (subscriber_cancels.empty()) {
                return false;
            }
            bool all_cancelled = std::all_of(
                subscriber_cancels.begin(),
                subscriber_cancels.end(),
                [](const std::atomic<bool>* p) { return p && p->load(); });
            if (all_cancelled) {
                combined_cancel.store(true);
            }
            return all_cancelled;
        }
    };

    ResolveResult ResolveUncached(const std::string& url_or_query, const std::atomic<bool>* cancel_flag, int playlist_depth = 0);
    SearchExecutionResult SearchUncached(const std::string& query, std::uint32_t limit, const std::atomic<bool>* cancel_flag);
    void InitializeCookieSource();
    std::filesystem::path GetDefaultCookiesPath() const;
    std::filesystem::path GetTempCookiesDir() const;
    void CleanupStaleTempCookieFiles() const;
    CookieSourceSnapshot PrepareExecutionCookieSnapshot(
        const CookieSourceSnapshot* src,
        TempCookiesFileGuard& guard) const;
    CookieSourceSnapshot GetActiveCookieSnapshot() const;
    bool AppendYtDlpCommonArgs(
        std::vector<std::string>& args,
        ActiveJsRuntime* out_runtime = nullptr,
        bool force_quickjs = false,
        const CookieSourceSnapshot* cookie_source = nullptr) const;
    bool ExecuteYtDlp(
        const std::vector<std::string>& args,
        std::string& out_stdout,
        std::string& out_stderr,
        int timeout_ms = 25000,
        const std::atomic<bool>* cancel_flag = nullptr,
        bool use_deno_env = false,
        const std::function<void(const std::string&)>& on_stderr_line = {},
        bool log_exec_diag = false) const;
    bool ExecuteYtDlpWithJsFallback(
        const std::vector<std::string>& base_args,
        const std::string& target,
        std::string& out_stdout,
        std::string& out_stderr,
        int timeout_ms,
        const std::atomic<bool>* cancel_flag,
        std::string* out_runtime_name = nullptr,
        std::int64_t* out_js_check_ms = nullptr,
        bool is_resolve_call = false,
        const CookieSourceSnapshot* cookie_source = nullptr) const;
    bool ExecuteYtDlpWithAuthFallback(
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
        bool& out_age_restricted);
    void MaybeWarnAuthRequiredLocked(bool is_age_restricted) const;
    bool ProbeDirectHttpAudio(
        const std::string& url,
        std::string& out_content_type,
        bool& out_security_blocked,
        std::string& out_station_name_utf8,
        const std::atomic<bool>* cancel_flag = nullptr) const;
    bool InspectYtDlpWarnings(const std::string& stderr_text, std::string& out_first_challenge_line) const;
    void MaybeWarnJsChallengeRateLimited() const;
    bool IsCancelled(const std::atomic<bool>* cancel_flag) const;

    PluginConfig config_;
    BinaryManager& binaries_;
    MetadataCache& cache_;
    std::atomic<bool> shutting_down_{false};
    mutable std::atomic<std::uint32_t> active_processes_{0};

    std::mutex inflight_mutex_;
    std::unordered_map<std::string, std::shared_ptr<InflightEntry<ResolveResult>>> inflight_resolves_;
    std::unordered_map<std::string, std::shared_ptr<InflightEntry<SearchExecutionResult>>> inflight_searches_;

    mutable std::mutex ytdlp_hook_mutex_;
    YtDlpTestHook ytdlp_test_hook_;
    EnvObserver ytdlp_env_observer_;

    mutable std::mutex cache_prune_mutex_;
    std::chrono::steady_clock::time_point last_cache_prune_{};
    std::atomic<bool> cache_prune_in_progress_{false};
    std::unordered_set<std::string> logged_unknown_cache_subdirs_;

    mutable std::mutex js_warn_mutex_;
    mutable bool js_challenge_warned_once_{false};
    mutable std::chrono::steady_clock::time_point last_js_challenge_warn_{};
    mutable std::atomic<bool> logged_exec_env_once_{false};

    mutable std::mutex cookie_mutex_;
    mutable std::condition_variable cookie_cv_;
    CookieSourceSnapshot active_cookie_source_;
    std::uint64_t cookie_state_generation_{0};
    bool cookie_discovery_in_progress_{false};
    bool configured_cookies_file_missing_{false};
    bool cookies_file_expired_{false};
    bool have_default_cookies_mtime_{false};
    std::filesystem::file_time_type default_cookies_mtime_{};
    bool failed_discovery_cooldown_active_{false};
    std::chrono::steady_clock::time_point last_failed_discovery_{};
    std::string last_checked_browsers_summary_{"none"};
    mutable bool auth_warned_once_{false};
    mutable std::chrono::steady_clock::time_point last_auth_warn_{};
    std::atomic<int> cookie_discovery_count_{0};
    std::atomic<int> cookie_discovery_budget_ms_{60000};
};

} // namespace sampaudiostream
