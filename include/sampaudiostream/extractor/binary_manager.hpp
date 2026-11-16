#pragma once

#include "sampaudiostream/core/config.hpp"
#include "sampaudiostream/core/thread_pool.hpp"

#include <atomic>
#include <mutex>
#include <string>

namespace sampaudiostream {

class BinaryManager {
public:
    explicit BinaryManager(const PluginConfig& config);
    ~BinaryManager() = default;

    void InitializeAsync(ThreadPool& pool);

    bool IsReady() const;
    bool IsInitializing() const;
    std::string GetYtDlpPath() const;
    std::string GetJsRuntimePath() const;

private:
    void EnsureBinaries();
    bool EnsureYtDlp();
    bool EnsureJsRuntime();
    void UpdateYtDlpIfDue();
    bool MakeExecutable(const std::string& path);

    PluginConfig config_;
    std::atomic<bool> ready_{false};
    std::atomic<bool> initializing_{false};
    mutable std::mutex mutex_;
    std::string ytdlp_path_;
    std::string js_runtime_path_;
};

} // namespace sampaudiostream
