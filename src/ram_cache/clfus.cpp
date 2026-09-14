// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// CLFUS: Clock LRU Frequency Size
// Scan-resistant RAM cache algorithm based on ATS RamCacheCLFUS.
// Key insight: value = (hits + 1) / (size + ENTRY_OVERHEAD)
// This prioritizes hit rate over byte hit rate.
//
// SEGMENTED for read scaling: one CLFUS instance = one shared_mutex, and a
// shared_mutex reader count is an atomic RMW on a single shared cache line —
// under concurrent get() load that line bounces across cores and serializes
// readers (ATS shards its RAM cache per partition for the same reason).  The
// cache is therefore split into hardware-concurrency-derived segments, each
// a self-contained CLFUS with its own lock, LRU lists, map, seen filter and
// byte budget (max_bytes / num_segments).  Entries route by CacheKey hash
// only (NOT alternate id), so all alternates of a key share a segment and
// remove_all() stays a single-segment scan.
//
// Trade-offs of per-segment budgets (standard for sharded caches): an object
// larger than max_bytes / num_segments is no longer RAM-cacheable, and
// eviction pressure is per-segment rather than global.  With the default
// 256 MB cache and <=64 segments that floor is >=4 MB — far above the small
// hot objects the RAM tier exists for.

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>
#include <list>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_map>

#include "ram_cache.hpp"

namespace cyclone {

namespace {

constexpr size_t ENTRY_OVERHEAD = 256;
constexpr int REQUEUE_LIMIT = 100;
// Total seen-filter slots across the whole cache (scan-resistance admission
// filter); divided over the segments.
constexpr size_t SEEN_FILTER_SIZE = 65536;
constexpr size_t MIN_SEEN_FILTER_PER_SEGMENT = 1024;
// Per-segment byte-budget floor: below this, fewer (down to one) segments
// are used so small caches keep meaningful capacity per shard and a small
// cache behaves exactly like the pre-segmentation implementation.  4 MB
// keeps at least ~128 entries per segment at the 32 KB RAM-tier admission
// threshold, so per-segment eviction pressure stays statistically close to
// the old global-LRU behavior even for modest configured sizes.
constexpr size_t MIN_SEGMENT_BYTES = size_t{4} * 1024 * 1024;

// Power-of-two segment count derived from hardware concurrency: enough
// shards that concurrent readers rarely share a lock cache line, few enough
// that per-segment budgets stay meaningful.
size_t pick_segment_count(size_t max_bytes) {
  size_t hw = std::thread::hardware_concurrency();
  if (hw == 0) {
    hw = 8;
  }
  size_t by_cores = std::bit_ceil(std::clamp<size_t>(2 * hw, 8, 64));
  size_t by_budget =
      std::bit_floor(std::max<size_t>(max_bytes / MIN_SEGMENT_BYTES, 1));
  return std::min(by_cores, by_budget);
}

}  // namespace

class RamCacheCLFUS : public RamCache {
 public:
  explicit RamCacheCLFUS(size_t max_bytes);

  std::optional<std::vector<std::byte>> get(const CacheKey& key, AlternateId id,
                                            uint32_t* out_stamp) override;
  bool put_if(const CacheKey& key, AlternateId id,
              std::span<const std::byte> data,
              const std::function<bool()>& predicate, uint32_t stamp) override;
  bool remove(const CacheKey& key, AlternateId id) override;
  void remove_all(const CacheKey& key) override;
  void clear() override;

  RamCacheStats stats() const override;
  size_t bytes_used() const override;
  size_t max_bytes() const override { return _max_bytes; }
  size_t entry_count() const override;

 private:
  struct Entry {
    RamCacheKey key;
    std::vector<std::byte> data;
    mutable std::atomic<uint32_t> hits{0};
    bool in_cache = true;
    // Opaque caller stamp; see RamCache::put_if.  Deliberately on the
    // Entry and NOT in RamCacheKey: the key is the lookup identity, and
    // remove(key, id) must keep finding an entry whatever it was stamped
    // with.
    uint32_t stamp = 0;

    Entry() = default;
    ~Entry() = default;

    Entry(Entry&& other) noexcept
        : key(other.key),
          data(std::move(other.data)),
          hits(other.hits.load(std::memory_order_relaxed)),
          in_cache(other.in_cache),
          stamp(other.stamp) {}

    Entry& operator=(Entry&& other) noexcept {
      if (this != &other) {
        key = other.key;
        data = std::move(other.data);
        hits.store(other.hits.load(std::memory_order_relaxed),
                   std::memory_order_relaxed);
        in_cache = other.in_cache;
        stamp = other.stamp;
      }
      return *this;
    }

    Entry(const Entry&) = delete;
    Entry& operator=(const Entry&) = delete;

    double value() const {
      return static_cast<double>(hits.load(std::memory_order_relaxed) + 1) /
             static_cast<double>(data.size() + ENTRY_OVERHEAD);
    }
  };

  using EntryList = std::list<Entry>;
  using EntryIter = EntryList::iterator;

  // One self-contained CLFUS shard.  This is the pre-segmentation
  // RamCacheCLFUS implementation verbatim, scoped to 1/Nth of the byte
  // budget and seen filter.
  struct Segment {
    Segment(size_t max_bytes, size_t seen_filter_size)
        : _max_bytes(max_bytes), _seen_filter(seen_filter_size, 0) {}

    std::optional<std::vector<std::byte>> get(const RamCacheKey& rk,
                                              uint32_t* out_stamp);
    // predicate (when non-null) is evaluated under _mutex BEFORE any map or
    // list is touched — see RamCache::put_if.
    bool put(const RamCacheKey& rk, std::span<const std::byte> data,
             const std::function<bool()>* predicate, uint32_t stamp);
    bool remove(const RamCacheKey& rk);
    void remove_all(const CacheKey& key);
    void clear();

    uint16_t compute_seen_tag(const RamCacheKey& key) const;
    bool check_seen(const RamCacheKey& key) const;
    void mark_seen(const RamCacheKey& key);

    void evict_if_needed(size_t required_space);
    void promote_from_history(EntryIter it, std::span<const std::byte> data,
                              uint32_t stamp);
    void update_average(double new_value);

    size_t _max_bytes;
    size_t _bytes_used = 0;

    EntryList _active_lru;   // In-memory entries
    EntryList _history_lru;  // Evicted metadata (data cleared)

    std::unordered_map<RamCacheKey, EntryIter> _entry_map;

    std::vector<uint16_t> _seen_filter;
    double _average_value = 0.0;

    // Atomic counters for thread-safe updates under shared_lock
    mutable std::atomic<uint64_t> _hits{0};
    mutable std::atomic<uint64_t> _misses{0};
    mutable std::atomic<uint64_t> _evictions{0};

    mutable std::shared_mutex _mutex;
  };

  Segment& segment_for(const CacheKey& key) const {
    // Fibonacci mix of the key hash, high bits — decorrelated from the
    // modulo-based stripe/bucket routing that uses segment_hash() /
    // bucket_hash().
    size_t h = std::hash<CacheKey>{}(key)*size_t{0x9E3779B97F4A7C15};
    return *_segments[(h >> 33) & _shard_mask];
  }

  size_t _max_bytes;
  std::vector<std::unique_ptr<Segment>> _segments;
  size_t _shard_mask;  // segment count (power of two) minus one
};

RamCacheCLFUS::RamCacheCLFUS(size_t max_bytes) : _max_bytes(max_bytes) {
  size_t num_segments = pick_segment_count(max_bytes);
  size_t per_segment_bytes = std::max<size_t>(max_bytes / num_segments, 1);
  size_t per_segment_filter =
      std::max(SEEN_FILTER_SIZE / num_segments, MIN_SEEN_FILTER_PER_SEGMENT);
  _segments.reserve(num_segments);
  for (size_t i = 0; i < num_segments; ++i) {
    _segments.push_back(
        std::make_unique<Segment>(per_segment_bytes, per_segment_filter));
  }
  _shard_mask = num_segments - 1;
}

uint16_t RamCacheCLFUS::Segment::compute_seen_tag(
    const RamCacheKey& key) const {
  auto digest = key.cache_key.digest();
  uint16_t tag;
  std::memcpy(&tag, digest.data() + 16, sizeof(tag));
  // Mix in alternate id so different alternates have distinct tags.
  tag ^= static_cast<uint16_t>(static_cast<uint8_t>(key.alternate_id));
  return tag;
}

bool RamCacheCLFUS::Segment::check_seen(const RamCacheKey& key) const {
  uint16_t tag = compute_seen_tag(key);
  size_t idx = key.cache_key.bucket_hash() % _seen_filter.size();
  return _seen_filter[idx] == tag;
}

void RamCacheCLFUS::Segment::mark_seen(const RamCacheKey& key) {
  uint16_t tag = compute_seen_tag(key);
  size_t idx = key.cache_key.bucket_hash() % _seen_filter.size();
  _seen_filter[idx] = tag;
}

std::optional<std::vector<std::byte>> RamCacheCLFUS::Segment::get(
    const RamCacheKey& rk, uint32_t* out_stamp) {
  std::shared_lock lock(_mutex);

  auto it = _entry_map.find(rk);
  if (it == _entry_map.end()) {
    ++_misses;
    return std::nullopt;
  }

  auto& entry = *it->second;

  if (!entry.in_cache) {
    ++_misses;
    return std::nullopt;
  }

  ++entry.hits;
  ++_hits;

  if (out_stamp != nullptr) {
    *out_stamp = entry.stamp;
  }

  return std::vector<std::byte>(entry.data);
}

bool RamCacheCLFUS::Segment::put(const RamCacheKey& rk,
                                 std::span<const std::byte> data,
                                 const std::function<bool()>* predicate,
                                 uint32_t stamp) {
  if (data.size() > _max_bytes) {
    return false;
  }

  std::unique_lock lock(_mutex);

  // Conditional put: evaluated with the segment write lock held, so
  // the verdict is atomic with respect to a remove()/remove_all() on this
  // segment — a put the predicate rejects is never visible to a get().
  if (predicate != nullptr && !(*predicate)()) {
    return false;
  }

  auto existing = _entry_map.find(rk);
  if (existing != _entry_map.end()) {
    auto& entry = *existing->second;

    if (entry.in_cache) {
      _bytes_used -= entry.data.size();
      entry.data.assign(data.begin(), data.end());
      entry.stamp = stamp;
      _bytes_used += entry.data.size();
      ++entry.hits;

      if (existing->second != _active_lru.begin()) {
        _active_lru.splice(_active_lru.begin(), _active_lru, existing->second);
      }
      return true;
    } else {
      if (entry.value() > _average_value ||
          entry.hits.load(std::memory_order_relaxed) > 0) {
        promote_from_history(existing->second, data, stamp);
        return true;
      }
      ++entry.hits;
      return false;
    }
  }

  if (!check_seen(rk)) {
    mark_seen(rk);
    return false;
  }

  evict_if_needed(data.size());

  Entry new_entry;
  new_entry.key = rk;
  new_entry.data.assign(data.begin(), data.end());
  new_entry.in_cache = true;
  new_entry.stamp = stamp;

  _active_lru.push_front(std::move(new_entry));
  _entry_map[rk] = _active_lru.begin();
  _bytes_used += data.size();

  return true;
}

bool RamCacheCLFUS::Segment::remove(const RamCacheKey& rk) {
  std::unique_lock lock(_mutex);

  auto it = _entry_map.find(rk);
  if (it == _entry_map.end()) {
    return false;
  }

  auto& entry = *it->second;

  if (entry.in_cache) {
    _bytes_used -= entry.data.size();
    _active_lru.erase(it->second);
  } else {
    _history_lru.erase(it->second);
  }

  _entry_map.erase(it);
  return true;
}

void RamCacheCLFUS::Segment::remove_all(const CacheKey& key) {
  std::unique_lock lock(_mutex);

  for (auto it = _entry_map.begin(); it != _entry_map.end();) {
    if (it->first.cache_key == key) {
      auto& entry = *it->second;
      if (entry.in_cache) {
        _bytes_used -= entry.data.size();
        _active_lru.erase(it->second);
      } else {
        _history_lru.erase(it->second);
      }
      it = _entry_map.erase(it);
    } else {
      ++it;
    }
  }
}

void RamCacheCLFUS::Segment::clear() {
  std::unique_lock lock(_mutex);

  _active_lru.clear();
  _history_lru.clear();
  _entry_map.clear();
  std::fill(_seen_filter.begin(), _seen_filter.end(), 0);

  _bytes_used = 0;
  _average_value = 0.0;
}

void RamCacheCLFUS::Segment::evict_if_needed(size_t required_space) {
  int requeues = 0;

  while (_bytes_used + required_space > _max_bytes && !_active_lru.empty()) {
    auto victim_it = std::prev(_active_lru.end());
    auto& victim = *victim_it;

    uint32_t current_hits = victim.hits.load(std::memory_order_relaxed);
    if (current_hits > 0 && requeues < REQUEUE_LIMIT) {
      victim.hits.store(current_hits / 2, std::memory_order_relaxed);
      _active_lru.splice(_active_lru.begin(), _active_lru, victim_it);
      ++requeues;
      continue;
    }

    update_average(victim.value());

    _bytes_used -= victim.data.size();
    victim.data.clear();
    victim.data.shrink_to_fit();
    victim.in_cache = false;

    _history_lru.splice(_history_lru.begin(), _active_lru, victim_it);
    _entry_map[victim.key] = _history_lru.begin();

    ++_evictions;

    static constexpr size_t MAX_HISTORY = 1024;
    while (_history_lru.size() > MAX_HISTORY) {
      auto old = std::prev(_history_lru.end());
      _entry_map.erase(old->key);
      _history_lru.pop_back();
    }
  }
}

void RamCacheCLFUS::Segment::promote_from_history(
    EntryIter it, std::span<const std::byte> data, uint32_t stamp) {
  evict_if_needed(data.size());

  auto& entry = *it;
  entry.data.assign(data.begin(), data.end());
  entry.stamp = stamp;
  entry.in_cache = true;
  ++entry.hits;

  _active_lru.splice(_active_lru.begin(), _history_lru, it);
  _entry_map[entry.key] = _active_lru.begin();
  _bytes_used += data.size();
}

void RamCacheCLFUS::Segment::update_average(double new_value) {
  static constexpr double ALPHA = 0.1;
  _average_value = ALPHA * new_value + (1.0 - ALPHA) * _average_value;
}

std::optional<std::vector<std::byte>> RamCacheCLFUS::get(const CacheKey& key,
                                                         AlternateId id,
                                                         uint32_t* out_stamp) {
  return segment_for(key).get(RamCacheKey{key, id}, out_stamp);
}

bool RamCacheCLFUS::put_if(const CacheKey& key, AlternateId id,
                           std::span<const std::byte> data,
                           const std::function<bool()>& predicate,
                           uint32_t stamp) {
  return segment_for(key).put(RamCacheKey{key, id}, data, &predicate, stamp);
}

bool RamCacheCLFUS::remove(const CacheKey& key, AlternateId id) {
  return segment_for(key).remove(RamCacheKey{key, id});
}

void RamCacheCLFUS::remove_all(const CacheKey& key) {
  // Alternates route by CacheKey only, so they all live in one segment.
  segment_for(key).remove_all(key);
}

void RamCacheCLFUS::clear() {
  for (auto& segment : _segments) {
    segment->clear();
  }
}

RamCacheStats RamCacheCLFUS::stats() const {
  RamCacheStats result;
  result.max_bytes = _max_bytes;
  for (const auto& segment : _segments) {
    std::shared_lock lock(segment->_mutex);
    result.bytes_used += segment->_bytes_used;
    result.entry_count += segment->_entry_map.size();
    result.hits += segment->_hits;
    result.misses += segment->_misses;
    result.evictions += segment->_evictions;
  }
  return result;
}

size_t RamCacheCLFUS::bytes_used() const {
  size_t total = 0;
  for (const auto& segment : _segments) {
    std::shared_lock lock(segment->_mutex);
    total += segment->_bytes_used;
  }
  return total;
}

size_t RamCacheCLFUS::entry_count() const {
  size_t total = 0;
  for (const auto& segment : _segments) {
    std::shared_lock lock(segment->_mutex);
    total += segment->_entry_map.size();
  }
  return total;
}

std::unique_ptr<RamCache> create_clfus(size_t max_bytes) {
  return std::make_unique<RamCacheCLFUS>(max_bytes);
}

}  // namespace cyclone
