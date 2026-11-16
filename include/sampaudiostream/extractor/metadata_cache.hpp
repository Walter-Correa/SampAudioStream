#pragma once

#include "sampaudiostream/core/types.hpp"

#include <chrono>
#include <cstddef>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace sampaudiostream {

class MetadataCache {
public:
    MetadataCache(std::size_t max_entries = 256, std::uint32_t ttl_seconds = 3600);

    std::optional<TrackMetadata> GetTrack(const std::string& key);
    void PutTrack(const std::string& key, const TrackMetadata& metadata);

    std::optional<std::vector<SearchResultItem>> GetSearch(const std::string& query_key);
    void PutSearch(const std::string& query_key, const std::vector<SearchResultItem>& items);

    void Clear();

private:
    template <typename ValueT>
    struct CacheBucket {
        using Clock = std::chrono::steady_clock;
        struct Entry {
            std::string key;
            ValueT value;
            Clock::time_point expires_at;
        };

        std::size_t max_size;
        std::chrono::seconds ttl;
        std::list<Entry> lru_list;
        std::unordered_map<std::string, typename std::list<Entry>::iterator> map;

        CacheBucket(std::size_t max_entries, std::uint32_t ttl_sec)
            : max_size(max_entries > 0 ? max_entries : 1), ttl(ttl_sec) {}

        std::optional<ValueT> Get(const std::string& key) {
            auto it = map.find(key);
            if (it == map.end()) {
                return std::nullopt;
            }
            if (Clock::now() >= it->second->expires_at) {
                lru_list.erase(it->second);
                map.erase(it);
                return std::nullopt;
            }
            lru_list.splice(lru_list.begin(), lru_list, it->second);
            return it->second->value;
        }

        void Put(const std::string& key, const ValueT& val) {
            auto it = map.find(key);
            if (it != map.end()) {
                it->second->value = val;
                it->second->expires_at = Clock::now() + ttl;
                lru_list.splice(lru_list.begin(), lru_list, it->second);
                return;
            }
            if (lru_list.size() >= max_size) {
                auto last = lru_list.end();
                --last;
                map.erase(last->key);
                lru_list.pop_back();
            }
            lru_list.push_front(Entry{key, val, Clock::now() + ttl});
            map[key] = lru_list.begin();
        }

        void Clear() {
            lru_list.clear();
            map.clear();
        }
    };

    std::mutex mutex_;
    CacheBucket<TrackMetadata> track_cache_;
    CacheBucket<std::vector<SearchResultItem>> search_cache_;
};

} // namespace sampaudiostream
