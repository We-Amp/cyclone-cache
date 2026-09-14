// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "cyclone/alternate.hpp"
#include "cyclone/key.hpp"
#include "thread_shard.hpp"

namespace cyclone {

// Key for tracking hits per-alternate (per-alternate tracking)
struct HitTrackingKey {
  CacheKey key;
  AlternateId alternate_id = AlternateId::Original;

  bool operator==(const HitTrackingKey& other) const {
    return key == other.key && alternate_id == other.alternate_id;
  }
};

}  // namespace cyclone

// Hash specialization for HitTrackingKey
template <>
struct std::hash<cyclone::HitTrackingKey> {
  size_t operator()(const cyclone::HitTrackingKey& k) const noexcept {
    // Combine key hash with alternate_id
    size_t h1 = std::hash<cyclone::CacheKey>{}(k.key);
    size_t h2 = std::hash<uint8_t>{}(static_cast<uint8_t>(k.alternate_id));
    return h1 ^ (h2 << 1);
  }
};

namespace cyclone {

// Record of pending hits for a single alternate
struct HitRecord {
  uint32_t delta = 0;          // Hits accumulated since last flush
  int64_t last_access_ms = 0;  // Most recent access timestamp (ms since epoch)
};

// Callback for flushing hit records to persistent storage
// Now receives full CacheKey and AlternateId for document lookup
// Parameters: key, alternate_id, hit_delta, last_access_ms
// Returns: true if flush succeeded
using HitFlushCallback =
    std::function<bool(const CacheKey& key, AlternateId alternate_id,
                       uint32_t hit_delta, int64_t last_access_ms)>;

// Callback invoked once at the end of flush_now() after all per-key callbacks
// complete (only when at least one key was flushed).  Allows batching a single
// fsync per flush cycle instead of per-key.
using HitFlushBatchDoneCallback = std::function<void()>;

// Configuration for the hit tracker
struct HitTrackerConfig {
  std::chrono::milliseconds flush_interval{
      1000};                           // How often to flush (default 1s)
  uint32_t flush_threshold{10000};     // Flush key if hits exceed this
  bool enable_background_flush{true};  // Enable background flush thread
  size_t max_pending_entries{100000};  // Max entries before forced flush
};

// In-memory hit tracker with periodic flush to disk.
// Accumulates hit counts in memory and periodically flushes them to persistent
// storage to minimize write amplification while still maintaining hit
// statistics. Tracks hits per-alternate.
class HitTracker {
 public:
  using Config = HitTrackerConfig;

  explicit HitTracker(const Config& config = Config{});
  ~HitTracker();

  HitTracker(const HitTracker&) = delete;
  HitTracker& operator=(const HitTracker&) = delete;

  // Start the background flush thread (if enabled in config)
  void start(HitFlushCallback callback);

  // Register a callback invoked once after each flush_now() cycle completes.
  // Typically used to batch a single fsync per cycle.
  void set_batch_done_callback(HitFlushBatchDoneCallback callback);

  // Stop the background flush thread and perform final flush
  void stop();

  // Record a cache hit for the given key and alternate
  void record_hit(const CacheKey& key,
                  AlternateId alternate_id = AlternateId::Original);

  // Get pending hit count for a key/alternate (for testing/debugging)
  uint32_t pending_hits(const CacheKey& key,
                        AlternateId alternate_id = AlternateId::Original) const;

  // Force an immediate flush of all pending hits
  void flush_now();

  // Flush hits for a specific key/alternate (e.g., before eviction)
  void flush_key(const CacheKey& key,
                 AlternateId alternate_id = AlternateId::Original);

  // Flush all alternates for a specific key
  void flush_all_alternates(const CacheKey& key);

  // Get number of entries with pending hits
  size_t pending_count() const;

  // Get total pending hits across all entries
  uint64_t total_pending_hits() const;

  const Config& config() const { return _config; }

 private:
  void flush_thread_func();

  Config _config;
  HitFlushCallback _flush_callback;
  HitFlushBatchDoneCallback _batch_done_callback;

  // Sharded pending-hit accumulation.  record_hit() runs on EVERY cache read,
  // so a single mutex here serializes all readers (throughput stops scaling
  // and even regresses past a few threads).  Striping by CacheKey lets readers
  // touching different keys proceed in parallel; all alternates of one key land
  // in the same stripe so flush_all_alternates() stays single-stripe.  Each
  // stripe mutex is a LEAF lock: never held across a flush callback, and never
  // two at once, preserving the read-path lock ordering (volume stripe->mutex
  // then this leaf).
  //
  // Stripe count and padding: with 64 unpadded stripes every
  // reader thread still hashed onto every stripe, so each lock/unlock was
  // an RMW on a cache line shared with all other readers and record_hit
  // remained a measured scaling ceiling at high core counts.  4096 stripes
  // padded to kShardPad make a same-line collision between two concurrent
  // readers rare (~64 threads over 4096 lines), while key-striping keeps
  // exactly ONE record per (key, alternate) — a per-THREAD scheme was
  // tried and reverted: it duplicated hot keys' records across stripes,
  // which breaks the max_pending_entries memory bound at high thread
  // counts and turns the bound check into a forced-flush storm.
  static constexpr size_t kNumStripes = 4096;
  struct alignas(kShardPad) Stripe {
    mutable std::mutex mutex;
    std::unordered_map<HitTrackingKey, HitRecord> pending;
  };
  // Heap-backed (not an inline member) for the same reason as
  // Volume::_checksum_cache (see the static_assert below volume.hpp's
  // Volume): at 4096 alignas(kShardPad) stripes this array is 512 KB with
  // libc++/libstdc++ and ~1 MB with MSVC (std::mutex is 80 bytes there, so
  // Stripe rounds up to 2*kShardPad).  Inline it made sizeof(HitTracker)
  // overflow a 1 MB thread stack (the Windows default) the moment a
  // HitTracker was stack-allocated, crashing before any test assertion.
  std::unique_ptr<std::array<Stripe, kNumStripes>> _stripes =
      std::make_unique<std::array<Stripe, kNumStripes>>();
  static size_t stripe_index(const CacheKey& key) {
    return std::hash<CacheKey>{}(key) % kNumStripes;
  }

  // Exact count of pending records across all stripes, maintained on
  // insert/erase under the owning stripe's mutex and read lock-free by
  // record_hit.  Replaces the old per-stripe size share
  // (max_pending_entries / kNumStripes), which at 4096 stripes would round
  // to ~24 entries and let ordinary hash variance trigger forced flushes.
  std::atomic<size_t> _pending_entries{0};

  std::atomic<bool> _running{false};
  std::thread _flush_thread;
};

// HitTracker is embedded in heap-allocated owners in production, but tests
// (and embedders) may construct one on the stack.  Keep it small enough
// that a stack instance cannot blow a 1 MB thread stack (the Windows
// default): the 4096-stripe pending-hit array is heap-backed for exactly
// this reason (see _stripes).  Same guard pattern as Volume.
static_assert(sizeof(HitTracker) < size_t{64} * 1024,
              "sizeof(HitTracker) must stay well under a 1 MB stack");

}  // namespace cyclone
