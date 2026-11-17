#pragma once

#include "sampaudiostream/core/config.hpp"
#include "sampaudiostream/core/types.hpp"
#include "sampaudiostream/extractor/binary_manager.hpp"
#include "sampaudiostream/extractor/metadata_cache.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace sampaudiostream {

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

class StreamResolver {
public:
    StreamResolver(const PluginConfig& config, BinaryManager& binaries, MetadataCache& cache);

    static bool LooksLikeUrl(const std::string& input);
    static bool IsKnownDirectAudioExtension(const std::string& url);
    static bool IsKnownPlatformUrl(const std::string& url);
    static std::string ParsePlaylistFirstStreamUrl(const std::string& body, const std::string& base_url = "");

    void CancelAll();
    void ResetCancel();
    void PollCancellations();
    std::uint32_t ActiveProcessCount() const { return active_processes_.load(); }

    ResolveResult Resolve(const std::string& url_or_query, const std::atomic<bool>* cancel_flag = nullptr);
    SearchExecutionResult Search(const std::string& query, std::uint32_t max_results, const std::atomic<bool>* cancel_flag = nullptr);

private:
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
    bool ExecuteYtDlp(
        const std::vector<std::string>& args,
        std::string& out_stdout,
        std::string& out_stderr,
        int timeout_ms = 25000,
        const std::atomic<bool>* cancel_flag = nullptr) const;
    bool ProbeDirectHttpAudio(
        const std::string& url,
        std::string& out_content_type,
        bool& out_security_blocked,
        const std::atomic<bool>* cancel_flag = nullptr) const;
    bool IsCancelled(const std::atomic<bool>* cancel_flag) const;

    PluginConfig config_;
    BinaryManager& binaries_;
    MetadataCache& cache_;
    std::atomic<bool> shutting_down_{false};
    mutable std::atomic<std::uint32_t> active_processes_{0};

    std::mutex inflight_mutex_;
    std::unordered_map<std::string, std::shared_ptr<InflightEntry<ResolveResult>>> inflight_resolves_;
    std::unordered_map<std::string, std::shared_ptr<InflightEntry<SearchExecutionResult>>> inflight_searches_;
};

} // namespace sampaudiostream
