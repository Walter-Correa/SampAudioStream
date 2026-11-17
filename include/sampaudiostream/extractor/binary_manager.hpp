#pragma once

#include "sampaudiostream/core/config.hpp"
#include "sampaudiostream/core/thread_pool.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace sampaudiostream {

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

// Shared subprocess runner using isolated pipe handles / process groups.
bool ExecuteSubprocess(
    const std::string& exe_path,
    const std::vector<std::string>& args,
    std::string& out_stdout,
    std::string& out_stderr,
    int timeout_ms = 25000,
    const std::atomic<bool>* cancel_flag = nullptr,
    std::atomic<std::uint32_t>* active_counter = nullptr);

class BinaryManager {
public:
    explicit BinaryManager(const PluginConfig& config);
    ~BinaryManager() = default;

    void InitializeAsync(ThreadPool& pool);
    void MaybePeriodicUpdate(ThreadPool& pool, std::uint32_t active_extractors);
    void Cancel();

    bool IsReady() const;
    bool IsInitializing() const;
    std::string GetYtDlpPath() const;
    std::string GetJsRuntimePath() const;

private:
    void EnsureBinaries();
    bool EnsureYtDlp();
    bool EnsureJsRuntime();
    void UpdateYtDlpIfDue();
    bool DownloadAndInstallYtDlpTag(const std::string& tag, bool is_update, std::string& out_installed_path);
    bool MakeExecutable(const std::string& path);

    PluginConfig config_;
    std::atomic<bool> ready_{false};
    std::atomic<bool> initializing_{false};
    std::atomic<bool> updating_{false};
    std::atomic<bool> cancel_flag_{false};
    std::chrono::steady_clock::time_point last_periodic_check_{};
    mutable std::mutex mutex_;
    std::string ytdlp_path_;
    std::string js_runtime_path_;
};

} // namespace sampaudiostream
