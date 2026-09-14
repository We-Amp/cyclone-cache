// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Simple LRU RAM cache implementation.
// Less sophisticated than CLFUS but simpler and predictable.

#include <atomic>
#include <list>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

#include "ram_cache.hpp"

namespace cyclone {

class RamCacheLRU : public RamCache {
 public:
  explicit RamCacheLRU(size_t max_bytes);

  std::optional<std::vector<std::byte>> get(const CacheKey& key, AlternateId id,
                                            uint32_t* out_stamp) override;
  bool put_if(const CacheKey& key, AlternateId id,
              std::span<const std::byte> data,
              const std::function<bool()>& predicate, uint32_t stamp) override;
  bool remove(const CacheKey& key, AlternateId id) override;
  void remove_all(const CacheKey& key) override;
  void clear() override;

  RamCacheStats stats() const override;
  size_t bytes_used() const override {
    std::shared_lock lock(_mutex);
    return _bytes_used;
  }
  size_t max_bytes() const override { return _max_bytes; }
  size_t entry_count() const override {
    std::shared_lock lock(_mutex);
    return _entries.size();
  }

 private:
  struct Entry {
    RamCacheKey key;
    std::vector<std::byte> data;
    // Opaque caller stamp; see RamCache::put_if.  Deliberately on the
    // Entry and NOT in RamCacheKey: the key is the lookup identity, and
    // remove(key, id) must keep finding an entry whatever it was stamped
    // with.
    uint32_t stamp = 0;
  };

  using EntryList = std::list<Entry>;
  using EntryIter = EntryList::iterator;

  size_t _max_bytes;
  size_t _bytes_used = 0;

  EntryList _lru;
  std::unordered_map<RamCacheKey, EntryIter> _entries;

  mutable std::atomic<uint64_t> _hits{0};
  mutable std::atomic<uint64_t> _misses{0};
  mutable std::atomic<uint64_t> _evictions{0};

  mutable std::shared_mutex _mutex;

  void evict_if_needed(size_t required_space);
};

RamCacheLRU::RamCacheLRU(size_t max_bytes) : _max_bytes(max_bytes) {}

std::optional<std::vector<std::byte>> RamCacheLRU::get(const CacheKey& key,
                                                       AlternateId id,
                                                       uint32_t* out_stamp) {
  RamCacheKey rk{key, id};
  std::unique_lock lock(_mutex);

  auto it = _entries.find(rk);
  if (it == _entries.end()) {
    ++_misses;
    return std::nullopt;
  }

  ++_hits;

  if (it->second != _lru.begin()) {
    _lru.splice(_lru.begin(), _lru, it->second);
  }

  if (out_stamp != nullptr) {
    *out_stamp = it->second->stamp;
  }

  return std::vector<std::byte>(it->second->data);
}

bool RamCacheLRU::put_if(const CacheKey& key, AlternateId id,
                         std::span<const std::byte> data,
                         const std::function<bool()>& predicate,
                         uint32_t stamp) {
  if (data.size() > _max_bytes) {
    return false;
  }

  RamCacheKey rk{key, id};
  std::unique_lock lock(_mutex);

  // Conditional put: evaluated with the instance write lock held, so
  // the verdict is atomic with respect to remove()/remove_all() — a put the
  // predicate rejects is never visible to a get().
  if (!predicate()) {
    return false;
  }

  auto existing = _entries.find(rk);
  if (existing != _entries.end()) {
    _bytes_used -= existing->second->data.size();
    existing->second->data.assign(data.begin(), data.end());
    existing->second->stamp = stamp;
    _bytes_used += data.size();

    if (existing->second != _lru.begin()) {
      _lru.splice(_lru.begin(), _lru, existing->second);
    }
    return true;
  }

  evict_if_needed(data.size());

  Entry new_entry;
  new_entry.key = rk;
  new_entry.data.assign(data.begin(), data.end());
  new_entry.stamp = stamp;

  _lru.push_front(std::move(new_entry));
  _entries[rk] = _lru.begin();
  _bytes_used += data.size();

  return true;
}

bool RamCacheLRU::remove(const CacheKey& key, AlternateId id) {
  RamCacheKey rk{key, id};
  std::unique_lock lock(_mutex);

  auto it = _entries.find(rk);
  if (it == _entries.end()) {
    return false;
  }

  _bytes_used -= it->second->data.size();
  _lru.erase(it->second);
  _entries.erase(it);
  return true;
}

void RamCacheLRU::remove_all(const CacheKey& key) {
  std::unique_lock lock(_mutex);

  for (auto it = _entries.begin(); it != _entries.end();) {
    if (it->first.cache_key == key) {
      _bytes_used -= it->second->data.size();
      _lru.erase(it->second);
      it = _entries.erase(it);
    } else {
      ++it;
    }
  }
}

void RamCacheLRU::clear() {
  std::unique_lock lock(_mutex);

  _lru.clear();
  _entries.clear();
  _bytes_used = 0;
}

RamCacheStats RamCacheLRU::stats() const {
  std::shared_lock lock(_mutex);

  RamCacheStats result;
  result.bytes_used = _bytes_used;
  result.max_bytes = _max_bytes;
  result.entry_count = _entries.size();
  result.hits = _hits;
  result.misses = _misses;
  result.evictions = _evictions;
  return result;
}

void RamCacheLRU::evict_if_needed(size_t required_space) {
  while (_bytes_used + required_space > _max_bytes && !_lru.empty()) {
    auto victim = std::prev(_lru.end());
    _bytes_used -= victim->data.size();
    _entries.erase(victim->key);
    _lru.pop_back();
    ++_evictions;
  }
}

std::unique_ptr<RamCache> create_lru(size_t max_bytes) {
  return std::make_unique<RamCacheLRU>(max_bytes);
}

// Forward declaration for CLFUS
std::unique_ptr<RamCache> create_clfus(size_t max_bytes);

// Factory function
std::unique_ptr<RamCache> RamCache::create(RamCacheType type,
                                           size_t max_bytes) {
  switch (type) {
    case RamCacheType::LRU:
      return create_lru(max_bytes);
    case RamCacheType::CLFUS:
    default:
      return create_clfus(max_bytes);
  }
}

}  // namespace cyclone
