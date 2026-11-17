#include "sampaudiostream/extractor/metadata_cache.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>

namespace sampaudiostream {

MetadataCache::MetadataCache(std::size_t max_entries, std::uint32_t ttl_seconds)
    : track_cache_(max_entries, ttl_seconds),
      search_cache_(max_entries, ttl_seconds / 4 > 60 ? ttl_seconds / 4 : 60) {}

std::optional<TrackMetadata> MetadataCache::GetTrack(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    return track_cache_.Get(key);
}

void MetadataCache::PutTrack(const std::string& key, const TrackMetadata& metadata) {
    std::lock_guard<std::mutex> lock(mutex_);
    track_cache_.Put(key, metadata);
}

std::optional<std::vector<SearchResultItem>> MetadataCache::GetSearch(const std::string& query_key) {
    std::lock_guard<std::mutex> lock(mutex_);
    return search_cache_.Get(query_key);
}

void MetadataCache::PutSearch(const std::string& query_key, const std::vector<SearchResultItem>& items) {
    std::lock_guard<std::mutex> lock(mutex_);
    search_cache_.Put(query_key, items);
}

void MetadataCache::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    track_cache_.Clear();
    search_cache_.Clear();
}

} // namespace sampaudiostream
