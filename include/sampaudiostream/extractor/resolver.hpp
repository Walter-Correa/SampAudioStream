#pragma once

#include "sampaudiostream/core/config.hpp"
#include "sampaudiostream/core/types.hpp"
#include "sampaudiostream/extractor/binary_manager.hpp"
#include "sampaudiostream/extractor/metadata_cache.hpp"

#include <string>
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

    // Determines quickly whether a string is a direct audio URL (e.g. .mp3, .ogg, radio stream)
    // vs a platform/web link or search query requiring extraction.
    static bool LooksLikeUrl(const std::string& input);
    static bool IsKnownDirectAudioExtension(const std::string& url);

    ResolveResult Resolve(const std::string& url_or_query);
    SearchExecutionResult Search(const std::string& query, std::uint32_t max_results);

private:
    bool ExecuteYtDlp(const std::vector<std::string>& args, std::string& out_stdout, std::string& out_stderr, int timeout_ms = 25000) const;
    bool ProbeDirectHttpAudio(const std::string& url, std::string& out_content_type) const;

    PluginConfig config_;
    BinaryManager& binaries_;
    MetadataCache& cache_;
};

} // namespace sampaudiostream
