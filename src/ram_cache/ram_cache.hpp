// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#pragma once

#include <cstddef>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "cyclone/alternate.hpp"
#include "cyclone/config.hpp"
#include "cyclone/key.hpp"

namespace cyclone {

// Composite key for alternate-aware RAM cache.
// Each (CacheKey, AlternateId) pair is a distinct cache entry, so
// multiple alternates of the same URL can coexist in RAM.
struct RamCacheKey {
  CacheKey cache_key;
  AlternateId alternate_id = AlternateId::Original;

  bool operator==(const RamCacheKey&) const = default;
};

struct RamCacheEntry {
  RamCacheKey key;
  std::vector<std::byte> data;
  uint32_t hits = 0;
  bool pinned = false;
};

struct RamCacheStats {
  size_t bytes_used = 0;
  size_t max_bytes = 0;
  size_t entry_count = 0;
  uint64_t hits = 0;
  uint64_t misses = 0;
  uint64_t evictions = 0;
};

class RamCache {
 public:
  virtual ~RamCache() = default;

  // out_stamp (when non-null) receives the opaque stamp the entry was
  // admitted with.  The RAM cache never interprets it: it carries the
  // value for the caller and nothing more.  Pass nullptr — the default — when
  // the caller does not validate, and the store is skipped entirely, keeping
  // the hit path byte-for-byte what it was before cross-process coherence.
  virtual std::optional<std::vector<std::byte>> get(
      const CacheKey& key, AlternateId id = AlternateId::Original,
      uint32_t* out_stamp = nullptr) = 0;
  // Unconditional put, expressed via the conditional primitive below.
  bool put(const CacheKey& key, AlternateId id,
           std::span<const std::byte> data) {
    return put_if(key, id, data, [] { return true; });
  }
  // Conditional put: the implementation evaluates predicate() while
  // holding the SAME write lock that serializes remove()/remove_all() for
  // this entry, and inserts only if it returns true.  A caller that pairs
  // its own generation sample with a remover's generation-bump-then-evict
  // (see Volume's Stripe::remove_epoch) gets a put that is atomic with
  // respect to the bump+evict: a put made against a superseded generation
  // is dropped at insert time instead of being inserted and then
  // withdrawn, so no such put outlives the remover's paired eviction (one
  // inserted before that eviction lands is still visible until it lands).
  // The predicate runs under a lock: it must
  // be fast, non-blocking, and must not call back into this cache.
  //
  // stamp is stored verbatim on the entry and handed back by get()'s
  // out_stamp.  It is opaque here; the volume read path uses it to carry the
  // shared directory bucket version the entry was admitted against.  An
  // update of an existing entry re-stamps it, so the stamp always describes
  // the bytes currently held.
  virtual bool put_if(const CacheKey& key, AlternateId id,
                      std::span<const std::byte> data,
                      const std::function<bool()>& predicate,
                      uint32_t stamp = 0) = 0;
  virtual bool remove(const CacheKey& key,
                      AlternateId id = AlternateId::Original) = 0;
  // Remove all alternates for a given CacheKey.  O(n) scan;
  // called only on PURGE (rare).
  virtual void remove_all(const CacheKey& key) = 0;
  virtual void clear() = 0;

  [[nodiscard]] virtual RamCacheStats stats() const = 0;
  [[nodiscard]] virtual size_t bytes_used() const = 0;
  [[nodiscard]] virtual size_t max_bytes() const = 0;
  [[nodiscard]] virtual size_t entry_count() const = 0;

  static std::unique_ptr<RamCache> create(RamCacheType type, size_t max_bytes);
};

}  // namespace cyclone

namespace std {
template <>
struct hash<cyclone::RamCacheKey> {
  size_t operator()(const cyclone::RamCacheKey& k) const noexcept {
    size_t h = std::hash<cyclone::CacheKey>{}(k.cache_key);
    auto id = static_cast<size_t>(static_cast<uint8_t>(k.alternate_id));
    // Fibonacci-hash mix of the alternate byte.
    return h ^ (id * size_t{0x9E3779B97F4A7C15} + (h << 6) + (h >> 2));
  }
};
}  // namespace std
