#pragma once

#include "sampaudiostream/core/config.hpp"
#include "sampaudiostream/core/thread_pool.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace sampaudiostream {

enum class JsRuntimeKind {
    None = 0,
    QuickJs = 1,
    Deno = 2
};

struct ActiveJsRuntime {
    JsRuntimeKind kind{JsRuntimeKind::None};
    std::string path;
    std::string version;
};

enum class YtDlpUpdateAction {
    SkipRecentlyChecked,
    CheckLatestVersion,
    DownloadAndUpdate,
    UpToDate
};

struct YtDlpUpdateDecisionInput {
    std::int64_t now_epoch{0};
    std::int64_t last_check_epoch{0};
    std::string current_tag;
    std::string latest_tag;
};

// Pure decision helper for yt-dlp periodic update evaluation.
YtDlpUpdateAction DecideYtDlpUpdateAction(const YtDlpUpdateDecisionInput& input);

// Standalone RFC 6234 SHA-256 hex digest helpers.
std::string ComputeStringSha256Hex(const std::string& input);
std::string ComputeFileSha256Hex(const std::string& file_path);

// Shared subprocess runner using isolated pipe handles / process groups.
bool ExecuteSubprocess(
    const std::string& exe_path,
    const std::vector<std::string>& args,
    std::string& out_stdout,
    std::string& out_stderr,
    int timeout_ms = 25000,
    const std::atomic<bool>* cancel_flag = nullptr,
    std::atomic<std::uint32_t>* active_counter = nullptr,
    const std::vector<std::pair<std::string, std::string>>* env_overrides = nullptr,
    const std::function<void(const std::string&)>& on_stderr_line = {},
    bool log_exec_diag = false);

class BinaryManager {
public:
    explicit BinaryManager(const PluginConfig& config);
    ~BinaryManager() = default;

    void InitializeAsync(ThreadPool& pool, std::function<void(bool ready)> on_settled = {});
    void MaybePeriodicUpdate(ThreadPool& pool, std::uint32_t active_extractors);
    void Cancel();

    bool IsReady() const;
    bool IsInitializing() const;
    bool IsSettled() const;
    bool HasProvisioningFailed() const;
    bool NeedsInitialDownload() const;
    bool WaitUntilSettled(std::chrono::milliseconds timeout) const;
    std::string GetYtDlpPath() const;
    std::string GetJsRuntimePath() const;
    std::string GetDenoPath() const;
    std::string GetDenoCacheDir() const;
    std::string GetDenoVersion() const;
    bool IsDenoSupported() const;
    bool IsDenoReady() const;
    ActiveJsRuntime SelectJsRuntime();
    std::string GetActiveJsRuntimeLabel();
    void RecordDenoSuccess();
    void RecordDenoRuntimeFailure();

    using DenoDownloadTestHook = std::function<bool(
        const std::string& dest_exe_path,
        const std::string& version,
        const std::atomic<bool>* cancel_flag,
        std::string& out_error)>;
    using DenoHealthTestHook = std::function<bool(
        const std::string& deno_path,
        std::string& out_version,
        std::string& out_error)>;

    void SetDenoSupportedOverrideForTesting(int supported_state);
    void SetDenoDownloadHookForTesting(DenoDownloadTestHook hook);
    void SetDenoHealthCheckHookForTesting(DenoHealthTestHook hook);
    void SetPostDownloadRetryDelayMsForTesting(int delay_ms);
    bool WaitForBackgroundProvisioningForTesting(std::chrono::milliseconds timeout) const;

private:
    void MarkSettled();
    void MarkBackgroundDone();
    void EnsureBinaries();
    bool EnsureYtDlp();
    bool EnsureJsRuntime();
    void EnsureDenoBackground();
    bool ProbeDenoHealth(const std::string& state_file_path, int timeout_ms = 10000, bool* out_timed_out = nullptr);
    bool ProbeDenoHealthPostDownload(const std::string& state_file_path);
    void UpdateYtDlpIfDue();
    bool DownloadAndInstallYtDlpTag(const std::string& tag, bool is_update, std::string& out_installed_path);
    bool MakeExecutable(const std::string& path);

    PluginConfig config_;
    std::atomic<bool> ready_{false};
    std::atomic<bool> provisioning_failed_{false};
    std::atomic<bool> initializing_{false};
    std::atomic<bool> updating_{false};
    std::atomic<bool> cancel_flag_{false};
    std::atomic<bool> deno_healthy_{false};
    std::atomic<bool> deno_disabled_for_session_{false};
    std::atomic<int> consecutive_deno_failures_{0};
    std::atomic<int> deno_supported_override_{-1};
    std::chrono::steady_clock::time_point last_periodic_check_{};
    mutable std::mutex mutex_;
    mutable std::mutex settle_mutex_;
    mutable std::condition_variable settled_cv_;
    mutable std::mutex bg_mutex_;
    mutable std::condition_variable bg_cv_;
    bool settled_{false};
    bool bg_done_{false};
    bool deno_unavailable_warned_{false};
    bool deno_missing_zip_warned_{false};
    std::function<void(bool ready)> on_settled_;
    std::string ytdlp_path_;
    std::string js_runtime_path_;
    std::string deno_path_;
    std::string deno_version_;
    DenoDownloadTestHook deno_download_hook_;
    DenoHealthTestHook deno_health_hook_;
    std::atomic<int> post_download_retry_delay_ms_override_{-1};
};

} // namespace sampaudiostream
