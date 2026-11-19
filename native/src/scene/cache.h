// scene/cache -- the derived-data cache: immutable products keyed by object identity and validated by an inputs hash.
//
// Owns: DerivedCache<Key, Value>. A product is stored under (key, inputsHash); get() finds it only when the hash of everything the product
// depends on still matches, so a product is recomputed exactly when its inputs changed and an old one is found again when they change back
// (undo / redo, "move it back"). Several hashes of one key live side by side until the least recently used are evicted beyond the memory
// budget. Values are immutable once stored and shared (shared_ptr<const Value>).
// Thread: any (one mutex; get / put are short).
// Spec: 04_SCENE_AND_DERIVED.md §2, 08_MILESTONES.md T3.2.
#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace native::scene {

template <class Key, class Value, class KeyHash = std::hash<Key>>
class DerivedCache {
public:
    // Memory budget of all entries (the caller says how large each value is). 04 §2: 2 GB for all caches together.
    explicit DerivedCache(size_t budgetBytes = size_t(2) << 30) : budget_(budgetBytes) {}

    // The product made for exactly these inputs, or null (never stored, stale, or evicted). Counts a hit / miss and refreshes the LRU position.
    std::shared_ptr<const Value> get(const Key& key, uint64_t inputsHash) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = map_.find(CacheKey{key, inputsHash});
        if (it == map_.end()) {
            ++misses_;
            return nullptr;
        }
        ++hits_;
        lru_.splice(lru_.begin(), lru_, it->second.pos);
        return it->second.value;
    }

    void put(const Key& key, uint64_t inputsHash, std::shared_ptr<const Value> value, size_t bytes) {
        std::lock_guard<std::mutex> lock(mutex_);
        CacheKey ck{key, inputsHash};
        auto it = map_.find(ck);
        if (it != map_.end()) {
            bytes_ -= it->second.bytes;
            lru_.erase(it->second.pos);
            map_.erase(it);
        }
        lru_.push_front(ck);
        map_.emplace(ck, Entry{std::move(value), bytes, lru_.begin()});
        bytes_ += bytes;
        evictLocked();
    }

    // Drops every entry whose key is not in `keys` (called once per published snapshot: objects that left the scene).
    void retain(const std::unordered_set<Key, KeyHash>& keys) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = map_.begin(); it != map_.end();) {
            if (keys.count(it->first.key)) {
                ++it;
                continue;
            }
            bytes_ -= it->second.bytes;
            lru_.erase(it->second.pos);
            it = map_.erase(it);
        }
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        map_.clear();
        lru_.clear();
        bytes_ = 0;
    }

    void setBudget(size_t bytes) {
        std::lock_guard<std::mutex> lock(mutex_);
        budget_ = bytes;
        evictLocked();
    }

    uint64_t hits() const { std::lock_guard<std::mutex> lock(mutex_); return hits_; }
    uint64_t misses() const { std::lock_guard<std::mutex> lock(mutex_); return misses_; }
    size_t entries() const { std::lock_guard<std::mutex> lock(mutex_); return map_.size(); }
    size_t bytes() const { std::lock_guard<std::mutex> lock(mutex_); return bytes_; }

private:
    struct CacheKey {
        Key key;
        uint64_t hash;
        bool operator==(const CacheKey& o) const { return hash == o.hash && key == o.key; }
    };
    struct CacheKeyHash {
        size_t operator()(const CacheKey& k) const { return KeyHash()(k.key) ^ (static_cast<size_t>(k.hash) * 0x9e3779b97f4a7c15ull); }
    };
    struct Entry {
        std::shared_ptr<const Value> value;
        size_t bytes;
        typename std::list<CacheKey>::iterator pos;
    };

    void evictLocked() {
        while (bytes_ > budget_ && !lru_.empty()) {
            auto it = map_.find(lru_.back());
            bytes_ -= it->second.bytes;
            map_.erase(it);
            lru_.pop_back();
        }
    }

    mutable std::mutex mutex_;
    size_t budget_;
    size_t bytes_ = 0;
    uint64_t hits_ = 0, misses_ = 0;
    std::list<CacheKey> lru_;   // front = most recently used
    std::unordered_map<CacheKey, Entry, CacheKeyHash> map_;
};

} // namespace native::scene
