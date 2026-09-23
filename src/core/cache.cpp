// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "cyclone/cache.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <thread>

#ifndef _WIN32
#include <unistd.h>  // getpid() for fork-safety detection in stop()
#endif

#include "../optimization/optimization_engine.hpp"
#include "../ram_cache/ram_cache.hpp"
#include "cyclone/detail/expected_compat.hpp"
#include "cyclone/plugin/plugin.hpp"
#include "hit_tracker.hpp"
#include "thread_util.hpp"
#include "volume.hpp"

namespace cyclone {

// Background thread that periodically flushes persistent (mmap'd) cache
// directories to disk.  Owns the bounded power-loss window documented on
// Volume::sync_directory(): without it, directory durability would ride
// along on the HitTracker flush fsync — which only fires when hits were
// actually flushed and disappears entirely when hit tracking is disabled.
class DirectorySyncer {
 public:
  DirectorySyncer(std::chrono::milliseconds interval,
                  std::function<void()> sync_fn)
      : _interval(interval), _sync_fn(std::move(sync_fn)) {}

  ~DirectorySyncer() { stop(); }

  DirectorySyncer(const DirectorySyncer &) = delete;
  DirectorySyncer &operator=(const DirectorySyncer &) = delete;

  void start() {
    if (_running.exchange(true)) {
      return;  // Already running
    }
    _thread = std::thread(&DirectorySyncer::thread_func, this);
  }

  void stop() {
    if (!_running.exchange(false)) {
      return;  // Already stopped
    }

    // UNBOUNDED join — deliberately not the bounded-join + leak pattern that
    // now survives only in Executor::stop() (see thread_util.hpp).  A
    // leaked-but-still-RUNNING sync thread dereferences the Cache's volume
    // list and this object's members after teardown frees them: with the
    // previous 5-second deadline, a
    // sync pass stalled in msync/fsync past the deadline was abandoned
    // alive and then read a destroyed Volume's stripe vector
    // (ASan-confirmed heap-use-after-free at Volume::sync_directory).
    // The join always completes: the loop re-checks _running at <=100 ms
    // granularity and a sync pass is one finite msync+fsync sweep.  The
    // fork-inherited "thread does not exist in this process" case never
    // reaches here — Cache::stop() release()s the whole syncer on that
    // path.
    if (_thread.joinable()) {
      _thread.join();
    }

    // Final sync so a clean stop leaves the directory durable.
    _sync_fn();
  }

 private:
  void thread_func() {
    while (_running.load()) {
      auto sleep_end = std::chrono::steady_clock::now() + _interval;

      // Sleep in small increments to check _running periodically.
      while (_running.load() && std::chrono::steady_clock::now() < sleep_end) {
        std::this_thread::sleep_for(std::chrono::milliseconds(
            std::min<int64_t>(100, _interval.count())));
      }

      if (!_running.load()) {
        break;
      }

      // Catch all exceptions to prevent silent thread death (mirrors
      // HitTracker::flush_thread_func).
      try {
        _sync_fn();
      } catch (...) {  // NOLINT(bugprone-empty-catch)
        // Continue the loop — losing one sync cycle is acceptable.
      }
    }
  }

  std::chrono::milliseconds _interval;
  std::function<void()> _sync_fn;
  std::atomic<bool> _running{false};
  std::thread _thread;
};

// Sharded reader gate for the cache-level rwlock.  The gate is taken SHARED
// on every public operation (it is the teardown/start gate, see stop()), so
// a single shared_mutex's reader count becomes a cross-core cache-line
// bounce that serializes concurrent readers on the hot read path.  Readers
// therefore take a shared lock on only the shard picked by their thread id;
// exclusive holders (start / stop / add_volume / reset_stats) take ALL
// shards in index order.  Semantics are identical to the former single
// shared_mutex — an exclusive holder still excludes every reader — only the
// reader-side cache-line sharing changes.  alignas keeps each shard's
// reader count on its own cache line pair (adjacent-line prefetcher).
struct alignas(128) GateShard {
  std::shared_mutex m;
};
inline constexpr size_t kGateShards = 16;

// RAII exclusive hold over all gate shards (lock in index order, unlock in
// reverse).  Replaces std::unique_lock on the former single mutex.
class GateExclusive {
 public:
  explicit GateExclusive(std::array<GateShard, kGateShards> &gate)
      : _gate(gate) {
    for (auto &shard : _gate) {
      shard.m.lock();
    }
  }
  ~GateExclusive() {
    for (size_t i = kGateShards; i-- > 0;) {
      _gate[i].m.unlock();
    }
  }
  GateExclusive(const GateExclusive &) = delete;
  GateExclusive &operator=(const GateExclusive &) = delete;

 private:
  std::array<GateShard, kGateShards> &_gate;
};

Task<std::expected<size_t, CacheError>> ReadHandle::read(
    std::span<std::byte> buffer) {
  (void)buffer;
  co_return make_unexpected(CacheError::NotInitialized);
}

Task<std::expected<std::vector<std::byte>, CacheError>> ReadHandle::read_all() {
  co_return make_unexpected(CacheError::NotInitialized);
}

Task<std::expected<size_t, CacheError>> WriteHandle::write(
    std::span<const std::byte> data) {
  if (!_impl) {
    co_return make_unexpected(CacheError::NotInitialized);
  }
  co_return _impl->write(data);
}

Task<std::expected<void, CacheError>> WriteHandle::close() {
  if (!_impl) {
    co_return std::expected<void, CacheError>{};
  }
  auto result = _impl->close();
  _impl.reset();
  co_return result;
}

Task<std::expected<void, CacheError>> UpdateHandle::close() {
  if (!_impl) {
    co_return std::expected<void, CacheError>{};
  }
  auto result = _impl->close();
  _impl.reset();
  co_return result;
}

struct Cache::Impl {
  CacheConfig config;
  // All volumes, in add order.  Maintenance loops (open/close, hit-count
  // flush, periodic directory sync, stats/capacity aggregation) iterate this
  // vector so they naturally cover the small-tier volume too.
  std::vector<std::shared_ptr<Volume>> volumes;
  // Routing views into `volumes` (raw pointers remain valid: volumes are
  // never removed).  Tier::kDefault hash-routes over default_volumes ONLY —
  // the small volume must never participate in default routing, otherwise a
  // fraction of untagged keys would land in (and churn) the small tier.
  std::vector<Volume *> default_volumes;
  // The dedicated Tier::kSmall volume; nullptr = small tier disabled (kSmall
  // operations then fall back to default routing).
  Volume *small_volume = nullptr;
  // One entry per element of `volumes`, same order: the caller-configured
  // path alongside the actual (structural-fingerprinted) on-disk path.
  // Append-only, recorded at the add_volume_locked() commit point -- the
  // sole place the fingerprint rewrite happens (Cache::volume_files()).
  std::vector<VolumeFileInfo> volume_files;
  std::shared_ptr<RamCache> ram_cache;
  std::shared_ptr<HitTracker> hit_tracker;
  // per-volume read-handle anchor sets (see VolumeReadAnchor in
  // volume.hpp).  Built in start() after each volume opens, installed into
  // the volume as a raw slot view, detached + cleared in stop() phase 3 —
  // all under the exclusive gate, so no read is in flight across either
  // transition.  Outstanding handles hold their own anchor refs and are
  // unaffected by the clear.
  std::vector<std::vector<std::shared_ptr<VolumeReadAnchor>>> read_anchors;
  std::unique_ptr<PluginManager> plugin_manager;
  std::unique_ptr<OptimizationEngine> optimization_engine;
  std::unique_ptr<DirectorySyncer> directory_syncer;
  std::atomic<bool> running{false};
  // True while stop() is mid-teardown.  stop() releases `mutex` between its
  // phases (the DirectorySyncer join must not run under it — its sync pass
  // takes the shared lock), so this flag is what makes teardown
  // single-flight; `running` stays true until teardown completes, keeping
  // start()/add_volume() Busy-gated exactly as before.  Guarded by the gate
  // (exclusive).
  bool stopping = false;
  // PID of the process that start()ed the background threads. Used by stop() to
  // detect a forked child (which inherited the Cache object but not the
  // threads) so it can abandon the thread-owning components instead of
  // deadlocking in their destructors. 0 = never started / not applicable (e.g.
  // Windows).
  long owner_pid{0};
  // Sharded cache-level gate (see GateShard above).  Readers:
  // std::shared_lock on reader_gate(); exclusive: GateExclusive over all
  // shards.
  mutable std::array<GateShard, kGateShards> gate;

  // The shard this thread's shared acquisitions use.  Thread-affine so a
  // reader thread always touches the same shard's cache line.  Assigned
  // round-robin at first use: hashing thread::id gives random PERMANENT
  // assignments, and by birthday statistics a handful of long-lived server
  // threads would likely share a shard for the process lifetime;
  // round-robin guarantees the spread at identical per-access cost.
  std::shared_mutex &reader_gate() const {
    static std::atomic<size_t> next_shard{0};
    thread_local const size_t shard_idx =
        next_shard.fetch_add(1, std::memory_order_relaxed) % kGateShards;
    return gate[shard_idx].m;
  }

  CacheStats stats;
  AtomicFlushStats
      flush_stats;  // Thread-safe counters for hit tracker callbacks
};

Cache::Cache() : _impl(std::make_unique<Impl>()) {
  _impl->plugin_manager = std::make_unique<PluginManager>();
}

Cache::Cache(const CacheConfig &config) : Cache() {
  _impl->config = config;

  if (config.ram_cache_size > 0) {
    _impl->ram_cache =
        RamCache::create(config.ram_cache_type, config.ram_cache_size);
  }

  if (config.enable_hit_tracking) {
    HitTrackerConfig hit_config;
    hit_config.flush_interval = config.hit_flush_interval;
    hit_config.flush_threshold = config.hit_flush_threshold;
    hit_config.enable_background_flush = true;
    _impl->hit_tracker = std::make_shared<HitTracker>(hit_config);
  }

  if (config.alternate_selector) {
    _impl->plugin_manager->set_alternate_selector(config.alternate_selector);
  }

  if (config.optimization_config.enabled) {
    _impl->optimization_engine =
        std::make_unique<OptimizationEngine>(config.optimization_config, this);
  }
}

Cache::~Cache() { stop(); }

std::expected<std::unique_ptr<Cache>, CacheError> Cache::create(
    const CacheConfig &config) {
  auto cache = std::make_unique<Cache>(config);
  return cache;
}

namespace {

// On-disk file suffix for the small-tier volume, appended to the default
// volume's path so integrators only configure one path.
constexpr const char *kSmallTierPathSuffix = ".small";

struct SmallTierSizes {
  size_t default_size;
  size_t small_size;
};

// Small-tier carve-out sizing (CacheConfig::small_tier_percent).
//
// Returns the (default, small) volume sizes, or nullopt when the small tier
// cannot be enabled for this total.  There is no logging facility, so a
// too-small total gracefully disables the tier (single-volume behavior)
// instead of failing — kSmall operations then fall back to default routing.
//
// Rules:
//   - percent is clamped to [1, 50].
//   - The small volume gets at least min_stripes stripes of kMinStripeSize
//     (plus the volume header).  min_stripes is 1, except in multi-process
//     mode where it is total_processes: stripe i is writable only by process
//     i % total_processes, so a small volume with fewer stripes than
//     processes would leave some processes permanently unable to write
//     small-tier entries (their writes would all be rejected NotOwned and
//     dropped — strictly worse than the default tier, where every process
//     owns its 1/N share of stripes).  Growing the stripe count to
//     total_processes restores exactly the default tier's per-process write
//     ownership model; the cost is a larger small-volume floor.
//   - The remainder must leave the default volume at least one
//     kMinStripeSize stripe; otherwise the tier is disabled.
std::optional<SmallTierSizes> compute_small_tier_sizes(
    size_t total_size, uint32_t percent, const MultiProcessConfig &mp_config) {
  if (percent == 0 || total_size == 0) {
    return std::nullopt;
  }
  const uint32_t pct = std::clamp<uint32_t>(percent, 1, 50);

  const size_t min_stripes =
      mp_config.enabled ? std::max<size_t>(mp_config.total_processes, 1) : 1;
  const size_t small_floor = min_stripes * kMinStripeSize + VolumeHeader::kSize;
  const size_t default_floor = kMinStripeSize + VolumeHeader::kSize;

  // total_size / 100 first to avoid overflow on very large totals.
  const size_t small_size = std::max(total_size / 100 * pct, small_floor);

  if (total_size < small_size + default_floor) {
    return std::nullopt;  // Too small to host both tiers — disable gracefully.
  }
  return SmallTierSizes{total_size - small_size, small_size};
}

}  // namespace

std::expected<void, CacheError> Cache::add_volume(const std::string &path,
                                                  size_t size) {
  VolumeConfig config;
  config.path = path;
  config.size = size;
  return add_volume(config);
}

std::expected<void, CacheError> Cache::add_volume(const VolumeConfig &config) {
  GateExclusive lock(_impl->gate);

  if (_impl->running) {
    return make_unexpected(CacheError::Busy);
  }

  // Small-object tier carve-out: when configured, the FIRST added default
  // volume is split into a default volume (remainder) and a physically
  // separate small volume at "<path>.small".  Each volume is its own file
  // with its own header/directory, so there is no on-disk format change:
  // existing caches open unchanged when the feature is off.  When the
  // feature is turned ON for an existing cache, the old volume file keeps
  // its (resharded) data as the default tier; entries previously written to
  // the default volume are simply one-time cold-start misses when later read
  // with Tier::kSmall.
  //
  // Requires an explicit size: with size == 0 the volume size is only
  // resolved from the file at open(), too late to compute the split — the
  // tier stays disabled in that case.
  if (_impl->volumes.empty() && config.tier == Tier::kDefault &&
      _impl->config.small_tier_percent > 0) {
    auto sizes =
        compute_small_tier_sizes(config.size, _impl->config.small_tier_percent,
                                 _impl->config.multi_process_config);
    if (sizes) {
      VolumeConfig default_config = config;
      default_config.size = sizes->default_size;
      auto result = add_volume_locked(default_config);
      if (!result) {
        return result;
      }

      VolumeConfig small_config = config;
      small_config.tier = Tier::kSmall;
      small_config.path = config.path + kSmallTierPathSuffix;
      small_config.size = sizes->small_size;
      // Minimum-size stripes maximize the stripe count, which (a) spreads
      // per-process write ownership in multi-process mode and (b) keeps the
      // wraparound/eviction granularity fine for small objects.
      small_config.stripe_size = kMinStripeSize;
      return add_volume_locked(small_config);
    }
    // compute_small_tier_sizes() declined the split: fall through to
    // single-volume behavior with the small tier disabled.
  }

  return add_volume_locked(config);
}

std::expected<void, CacheError> Cache::add_volume_locked(
    const VolumeConfig &config) {
  // Multi-process durability is provided by the periodic DirectorySyncer
  // (Volume::sync_directory), NOT by a per-write fsync.  Forcing sync_on_write
  // in multi-process mode made every metadata write fsync the shared volume
  // inode, collapsing concurrent writers into serialized jbd2 journal commits
  // (an fsync convoy) that stalled the integrator's serving path.
  // The periodic sync bounds the crash-loss window and the CRC32 read gauntlet
  // (forced on below) downgrades any torn/unsynced entry to a cache miss, so
  // dropping the per-write fsync loses no durability guarantee that matters for
  // a reconstructible best-effort cache.  Integrators may still opt into
  // per-write fsync explicitly via VolumeConfig::sync_on_write.
  VolumeConfig vol_config = config;
  // Structural-fingerprint the cache filename (upgrade safety): encode on-disk
  // FORMAT + GEOMETRY into the name so peers whose binaries disagree on layout
  // resolve to DIFFERENT files and never share one on-disk ring during an
  // overlapping upgrade (see fingerprint_cache_path in volume.cpp).  This is
  // the SOLE volume chokepoint -- the default, ".small" and single-volume
  // configs each arrive here with their OWN fully-resolved {size, stripe_size}
  // and the C API routes through add_volume() -> here -- so fingerprinting
  // once, before the Volume is constructed/opened below, covers every mode
  // uniformly.  The ".small" sibling arrives path-suffixed with its own small
  // size + minimum stripe_size, so it gets its own distinct geohash.
  // mmap_directory tracks multi_process_config.enabled, which selects the
  // mmap'd on-disk directory.
  vol_config.path = fingerprint_cache_path(
      vol_config.path, vol_config.size, vol_config.stripe_size,
      _impl->config.multi_process_config.enabled);
  vol_config.verify_checksum_on_read = _impl->config.verify_checksum_on_read;
  // Large-document readahead threshold: configured on CacheConfig, applied
  // by the volume's disk read path (see VolumeConfig::readahead_min_bytes).
  vol_config.readahead_min_bytes = _impl->config.readahead_min_bytes;
  // Lease-pinning knobs are configured on CacheConfig.
  vol_config.read_lease_duration = _impl->config.read_lease_duration;
  vol_config.lease_wrap_ceiling = _impl->config.lease_wrap_ceiling;
  // Alternate-chain shadow bound: the kill switch is configured on
  // CacheConfig (see VolumeConfig::unlink_superseded_alternates).
  vol_config.unlink_superseded_alternates =
      _impl->config.unlink_superseded_alternates;
  // Eviction mode: configured on CacheConfig, persisted per volume at
  // creation (see VolumeConfig::wrap_retention).
  vol_config.wrap_retention = _impl->config.wrap_retention;
  // Per-object size bound: configured on CacheConfig, enforced by the
  // volume at the write entry (see VolumeConfig::max_object_size).
  vol_config.max_object_size = _impl->config.max_object_size;
  // Cross-process RAM coherence: configured on CacheConfig, enforced
  // by the volume's read path (see VolumeConfig::cross_process_ram_coherence).
  vol_config.cross_process_ram_coherence =
      _impl->config.cross_process_ram_coherence;
  if (_impl->config.multi_process_config.enabled) {
    // Force read-side checksum verification: a persistent directory can
    // surface a torn 10-byte DirEntry after a crash/power loss, and the CRC32
    // check in the read gauntlet is what downgrades such an entry to a cache
    // miss instead of serving garbage.  (enable_checksum is already
    // hard-required for multi-process in start().)
    vol_config.verify_checksum_on_read = true;
  }

  if (vol_config.tier == Tier::kSmall && _impl->small_volume != nullptr) {
    return make_unexpected(CacheError::InvalidConfiguration);
  }

  // Pass multi-process config from CacheConfig to Volume
  auto volume =
      std::make_shared<Volume>(vol_config, _impl->config.multi_process_config);

  // The small volume deliberately does NOT share the RAM cache: RAM cache
  // entries are keyed by (CacheKey, AlternateId) with no tier dimension, so
  // sharing would let the same key cross tiers (breaking the physical
  // keyspace separation) and a default-tier overwrite would invalidate a
  // small-tier RAM entry.  Small-tier reads are served through the mmap'd
  // volume (OS page cache); integrators targeting this tier typically front
  // it with their own in-memory layer anyway.
  if (_impl->ram_cache && vol_config.tier == Tier::kDefault) {
    volume->set_ram_cache(_impl->ram_cache);
  }

  // Same exclusion for the hit tracker: its keyspace is (CacheKey,
  // AlternateId) with no tier dimension, and the flush callback resolves
  // first-match-wins across volumes — with the same key in both tiers,
  // small-tier read hits would be applied to the DEFAULT entry's persisted
  // hit count while the small entry's never flush.  Hit prioritization also
  // serves nothing on the small tier (FIFO-wrap metadata; no hit-driven
  // policy consumes the counts there).
  if (_impl->hit_tracker && vol_config.tier == Tier::kDefault) {
    volume->set_hit_tracker(_impl->hit_tracker);
  }

  Volume *raw = volume.get();
  _impl->volumes.push_back(std::move(volume));
  _impl->volume_files.push_back(
      {config.path, vol_config.path, vol_config.tier});
  if (vol_config.tier == Tier::kSmall) {
    _impl->small_volume = raw;
  } else {
    _impl->default_volumes.push_back(raw);
  }
  return {};
}

std::expected<void, CacheError> Cache::start() {
  GateExclusive lock(_impl->gate);

  if (_impl->running) {
    return make_unexpected(CacheError::Busy);
  }

  // Validate multi-process configuration
  const auto &mp_config = _impl->config.multi_process_config;
  if (mp_config.enabled) {
    if (!mp_config.is_valid()) {
      return make_unexpected(CacheError::InvalidConfiguration);
    }
    // Multi-process requires checksums for torn read detection
    if (!_impl->config.enable_checksum) {
      return make_unexpected(CacheError::InvalidConfiguration);
    }
  }

  for (auto &volume : _impl->volumes) {
    auto result = volume->open();
    if (!result) {
      for (auto &v : _impl->volumes) {
        v->close();
      }
      return result;
    }
  }

#ifndef _WIN32
  // Opt-in startup disk hygiene: reclaim SUPERSEDED structural-fingerprint
  // cache files (see gc_superseded_volumes in volume.cpp).  Runs ONLY here --
  // AFTER every volume above opened+locked (a partial open already returned
  // above, so GC never runs on a partial cache), and BEFORE _impl->running is
  // published -- so the live keep-set and the per-volume shared lifetime locks
  // are all in place.  POSIX-only and fail-closed: it never deletes a
  // live/locked/legacy/ foreign file and never fails start().
  if (_impl->config.gc_superseded_on_start) {
    std::vector<Volume *> live;
    live.reserve(_impl->volumes.size());
    for (auto &volume : _impl->volumes) {
      live.push_back(volume.get());
    }
    gc_superseded_volumes(live);
  }
#endif

  // (re)build the read-handle anchors — every open() above may
  // have created a fresh MappedFile, so anchors from a previous
  // start()/stop() cycle would pin the wrong mapping.
  _impl->read_anchors.clear();
  _impl->read_anchors.reserve(_impl->volumes.size());
  for (auto &volume : _impl->volumes) {
    _impl->read_anchors.push_back(volume->make_read_anchors());
    auto &slots = _impl->read_anchors.back();
    volume->set_read_anchors(slots.data(), slots.size());
  }

  // Start hit tracker with flush callback
  // Implement actual hit count persistence
  if (_impl->hit_tracker) {
    auto *impl = _impl.get();  // Capture raw pointer for callback

    // Register batch-done callback BEFORE start() to avoid a data race:
    // start() spawns the background flush thread, which reads
    // _batch_done_callback in flush_now().  Setting the callback after start()
    // would be a race.
    _impl->hit_tracker->set_batch_done_callback([impl]() {
      // fsync each volume once per flush cycle so pwrite'd hit counts are
      // visible to other processes (Docker virtiofs, NFS, etc.).
      // This serves hit-count propagation ONLY — persistent-directory
      // durability is owned by the DirectorySyncer above, so do not remove
      // one because the other "already fsyncs".
      for (auto &volume : impl->volumes) {
        (void)volume->fsync_volume();  // Best-effort; ignore errors
      }
      impl->flush_stats.hit_flush_fsyncs.fetch_add(1,
                                                   std::memory_order_relaxed);
    });

    _impl->hit_tracker->start(
        [impl](const CacheKey &key, AlternateId alternate_id,
               uint32_t hit_delta, int64_t last_access_ms) {
          // Find the volume containing this key and update the hit count.
          // Default-tier volumes only: hit tracking is not wired on the
          // small tier (see add_volume_locked), and the tracker keyspace has
          // no tier dimension — resolving against the small volume could
          // misattribute a default-tier delta to a same-key small entry.
          for (auto *volume : impl->default_volumes) {
            auto result = volume->update_hit_count_sync(
                key, alternate_id, hit_delta, last_access_ms);
            if (result.has_value()) {
              impl->flush_stats.hit_flush_successes.fetch_add(
                  1, std::memory_order_relaxed);
              impl->flush_stats.hit_flush_total_delta.fetch_add(
                  hit_delta, std::memory_order_relaxed);
              return true;  // Persisted.
            }
            if (result.error() == CacheError::Busy) {
              // The document is here, but the cross-process write lock was
              // contended or a wrap raced the resolve, so the delta was
              // DROPPED (hit counts are best-effort by contract).  Count the
              // drop honestly -- do NOT inflate successes/total_delta with a
              // delta that never landed -- and stop: it is handled, not
              // lost-to-eviction, so there is no point trying other volumes.
              impl->flush_stats.hit_flush_lock_contended.fetch_add(
                  1, std::memory_order_relaxed);
              return true;
            }
            // Not found in this volume - try the next.
          }
          // Key not found in any volume - this can happen if the document was
          // evicted
          impl->flush_stats.hit_flush_failures.fetch_add(
              1, std::memory_order_relaxed);
          return false;
        });
  }

  // Start periodic directory sync for persistent (mmap'd) directories.
  // This is an owned duty, not a side effect: the HitTracker flush fsync
  // happens to cover the directory on Linux, but only when hit tracking is
  // enabled AND hits were flushed that cycle.  See Volume::sync_directory().
  if (mp_config.enabled && _impl->config.directory_sync_interval.count() > 0) {
    auto *impl = _impl.get();  // Capture raw pointer for callback
    _impl->directory_syncer = std::make_unique<DirectorySyncer>(
        _impl->config.directory_sync_interval, [impl]() {
          // Hold the volume-list lock (shared) across the whole pass:
          // volume close and list mutation happen under the exclusive
          // lock, so a sync pass can never observe a dying Volume.  Cache
          // teardown joins this thread OUTSIDE the lock (see
          // Cache::stop()), so this acquisition cannot deadlock with it.
          std::shared_lock lock(impl->reader_gate());
          for (auto &volume : impl->volumes) {
            (void)volume->sync_directory();  // Best-effort; ignore errors
          }
        });
    _impl->directory_syncer->start();
  }

  // Start optimization engine
  if (_impl->optimization_engine) {
    _impl->optimization_engine->start();
  }

#ifndef _WIN32
  // Record the process that owns the just-started background threads, so a
  // forked child can detect in stop() that it does not own them (see stop()).
  _impl->owner_pid = static_cast<long>(::getpid());
#endif

  _impl->running = true;
  return {};
}

void Cache::stop() {
#ifndef _WIN32
  // Fork-safety (regression test "Cache teardown is fork-safe in a forked
  // child"): if this is not the process that start()ed the background threads,
  // we were inherited across fork(). The threads do not exist in this process,
  // and the condition variables/mutexes inside the thread-owning components
  // (OptimizationEngine + its WorkQueue, HitTracker) were fork-copied while
  // threads in the owning process held them. Running their destructors here
  // calls pthread_cond_destroy on such a condvar, which blocks forever (this is
  // what wedged mod_pagespeed 1.1 Apache children on graceful recycle). Abandon
  // (leak) those components so their destructors never run; the OS reclaims the
  // memory on process exit, and the owning process performs the real teardown.
  // We intentionally do NOT take the cache gate on this path: it too may be in
  // a fork-copied locked state. owner_pid is written once (under the lock) in
  // start() and never mutated, so reading it without the lock is safe here.
  if (_impl->running && _impl->owner_pid != 0 &&
      _impl->owner_pid != static_cast<long>(::getpid())) {
    // unique_ptr::release() abandons the OptimizationEngine without destroying
    // it (skips ~OptimizationEngine -> ~WorkQueue -> pthread_cond_destroy).
    (void)_impl->optimization_engine.release();
    // Same for the DirectorySyncer: its thread does not exist in this
    // process, so stop() would spin against a join that can never happen.
    (void)_impl->directory_syncer.release();
    // For the shared_ptr, stash a copy on the heap so the refcount never
    // reaches zero here and ~HitTracker (which would join a thread that does
    // not exist in this process) never runs.
    if (_impl->hit_tracker) {
      new std::shared_ptr<HitTracker>(_impl->hit_tracker);
      _impl->hit_tracker.reset();
    }
    _impl->running = false;
    return;
  }
#endif

  // Teardown protocol (fixes a latent thread-teardown race + deadlock
  // class).  Two hazards, resolved across three phases:
  //
  //   * DEADLOCK — the optimization workers and the directory syncer re-enter
  //     the Cache under the cache gate (workers via read_sync()'s shared lock,
  //     the syncer's pass via its own shared lock).  Joining them while this
  //     thread holds the EXCLUSIVE lock deadlocks.  So their joins run in
  //     phase 2, with no lock held.  (The older bounded join masked this by
  //     abandoning a stalled thread on timeout — which then dereferenced
  //     volumes freed by ~Cache: the heap-UAF this fix removes.)
  //   * RACE with in-flight operations — but the join must not run so late
  //     that a concurrent writer feeds a half-torn-down engine.  So intake is
  //     quiesced UNDER the lock in phase 1 (engine->request_stop()), before
  //     the lock is dropped for the phase-2 joins.
  //
  // Phases: (1) under the lock — claim teardown, quiesce engine intake, fully
  // stop the hit tracker (its thread never takes the cache gate), detach the
  // syncer; (2) no lock — join the engine workers/monitor and the syncer;
  // (3) under the lock — close volumes and clear the flags.
  std::unique_ptr<DirectorySyncer> syncer;
  {
    GateExclusive lock(_impl->gate);

    if (!_impl->running || _impl->stopping) {
      return;
    }
    _impl->stopping = true;  // Single-flight; `running` stays true so
                             // start()/add_volume() remain Busy-gated.

    // Quiesce the optimization engine's intake UNDER the lock: request_stop()
    // flips it off and stops its WorkQueue without joining, so once we release
    // the lock no concurrent Cache operation can feed it (the workers are
    // joined outside the lock, in phase 2).  Doing this under the lock is what
    // keeps teardown safe against in-flight writers (a regression
    // "Concurrent stop with in-flight writes"); the workers themselves must be
    // joined outside the lock because they re-enter via read_sync()'s shared
    // lock.
    if (_impl->optimization_engine) {
      _impl->optimization_engine->request_stop();
    }

    // Stop the hit tracker fully here: its flush thread never takes
    // the cache gate, so joining it under the exclusive lock neither deadlocks
    // nor races a concurrent operation (all of them are excluded by this
    // lock).  Its final flush runs against still-open volumes.
    if (_impl->hit_tracker) {
      _impl->hit_tracker->stop();
    }

    // Detach the syncer; it is joined below, outside the lock (its sync pass
    // takes the shared lock).  Volumes stay open until phase 3.
    syncer = std::move(_impl->directory_syncer);
  }

  // Phase 2 (cache gate not held): join the threads whose bodies re-enter the
  // Cache under the shared lock.  Safe to touch these pointers without the
  // lock: `stopping` is set and start()/add_volume() are gated, so nothing
  // mutates them.
  if (_impl->optimization_engine) {
    _impl->optimization_engine->join_threads();
  }
  if (syncer) {
    syncer->stop();
    syncer.reset();
  }

  // Phase 3: no background thread can reach the volumes anymore.
  GateExclusive lock(_impl->gate);

  // latch every outstanding anchor as torn BEFORE the stripes
  // are freed by close() — handles from this generation gate their
  // stripe-touching paths (release_borrow, the checked renews) on it, and
  // the latch survives a later start() re-arming Volume::_teardown.
  for (auto &anchor_set : _impl->read_anchors) {
    for (auto &anchor : anchor_set) {
      anchor->torn.store(true, std::memory_order_seq_cst);
    }
  }
  for (auto &volume : _impl->volumes) {
    volume->set_read_anchors(nullptr, 0);  // detach before close
    volume->close();
  }
  // Drop the Cache's anchor refs; handles still open keep their own and
  // continue to pin the closed Volume + its mapping until they close.
  // (Kept installed on the fork-child stop() path above, where no gate is
  // held: reads are already denied by running=false there, and leaving
  // the slots intact is safer for any straggler thread in the child.)
  _impl->read_anchors.clear();

  _impl->running = false;
  _impl->stopping = false;
}

bool Cache::is_running() const { return _impl->running; }

// Route a key to its volume.  Tier::kSmall goes to the dedicated small
// volume when the small tier is enabled; otherwise (and for Tier::kDefault)
// keys hash-route over the default-role volumes only — the small volume
// never participates in default hash routing.  The fallback lets integrators
// pass a tier unconditionally regardless of configuration.
static Volume *select_volume(const std::vector<Volume *> &default_volumes,
                             Volume *small_volume, const CacheKey &key,
                             Tier tier) {
  if (tier == Tier::kSmall && small_volume != nullptr) {
    return small_volume;
  }
  if (default_volumes.empty()) {
    return nullptr;
  }
  uint32_t idx = key.segment_hash() % default_volumes.size();
  return default_volumes[idx];
}

// Acquire shared lock, verify running, select volume — common guard for all
// public sync API methods.  Returns the volume or an error.
std::expected<std::pair<std::shared_lock<std::shared_mutex>, Volume *>,
              CacheError>
Cache::lock_and_select(const CacheKey &key, Tier tier) const {
  std::shared_lock lock(_impl->reader_gate());
  // Deny new operations the moment teardown is claimed (stopping), not just
  // after it completes (running).  stop() drops the exclusive lock between its
  // phases to join background threads; gating on `stopping` here
  // ensures no new WriteHandle/ReadHandle is granted during that window, so a
  // handle's lock-free commit can never race Volume::close() destroying the
  // stripe it writes.  (All five operation entry points share this gate.)
  if (!_impl->running || _impl->stopping) {
    return make_unexpected(CacheError::NotInitialized);
  }
  Volume *volume =
      select_volume(_impl->default_volumes, _impl->small_volume, key, tier);
  if (volume == nullptr) {
    return make_unexpected(CacheError::NotInitialized);
  }
  return std::pair{std::move(lock), volume};
}

Task<std::expected<ReadHandle, CacheError>> Cache::open_read(
    const CacheKey &key, Tier tier) {
  std::shared_lock lock(_impl->reader_gate());
  if (!_impl->running || _impl->stopping) {
    co_return make_unexpected(CacheError::NotInitialized);
  }

  Volume *volume =
      select_volume(_impl->default_volumes, _impl->small_volume, key, tier);
  if (volume == nullptr) {
    co_return make_unexpected(CacheError::NotInitialized);
  }

  co_return co_await volume->open_read(key);
}

Task<std::expected<WriteHandle, CacheError>> Cache::open_write(
    const CacheKey &key, Tier tier) {
  return open_write(key, 0, tier);
}

Task<std::expected<WriteHandle, CacheError>> Cache::open_write(
    const CacheKey &key, uint64_t content_length, Tier tier) {
  std::shared_lock lock(_impl->reader_gate());
  if (!_impl->running || _impl->stopping) {
    co_return make_unexpected(CacheError::NotInitialized);
  }

  Volume *volume =
      select_volume(_impl->default_volumes, _impl->small_volume, key, tier);
  if (volume == nullptr) {
    co_return make_unexpected(CacheError::NotInitialized);
  }

  co_return co_await volume->open_write(key, content_length);
}

Task<std::expected<UpdateHandle, CacheError>> Cache::open_update(
    const CacheKey &key) {
  (void)key;
  co_return make_unexpected(CacheError::NotInitialized);
}

Task<std::expected<void, CacheError>> Cache::remove(const CacheKey &key,
                                                    Tier tier) {
  std::shared_lock lock(_impl->reader_gate());
  if (!_impl->running || _impl->stopping) {
    co_return make_unexpected(CacheError::NotInitialized);
  }

  Volume *volume =
      select_volume(_impl->default_volumes, _impl->small_volume, key, tier);
  if (volume == nullptr) {
    co_return make_unexpected(CacheError::NotInitialized);
  }

  co_return co_await volume->remove(key);
}

Task<std::expected<bool, CacheError>> Cache::exists(const CacheKey &key,
                                                    Tier tier) {
  std::shared_lock lock(_impl->reader_gate());
  if (!_impl->running || _impl->stopping) {
    co_return make_unexpected(CacheError::NotInitialized);
  }

  Volume *volume =
      select_volume(_impl->default_volumes, _impl->small_volume, key, tier);
  if (volume == nullptr) {
    co_return make_unexpected(CacheError::NotInitialized);
  }

  co_return co_await volume->exists(key);
}

std::expected<ReadHandle, CacheError> Cache::read_sync(const CacheKey &key,
                                                       Tier tier) {
  auto guard = lock_and_select(key, tier);
  if (!guard) return make_unexpected(guard.error());
  return guard->second->read_sync(key);
}

std::expected<WriteHandle, CacheError> Cache::write_sync(const CacheKey &key,
                                                         Tier tier) {
  return write_sync(key, 0, tier);
}

std::expected<WriteHandle, CacheError> Cache::write_sync(
    const CacheKey &key, uint64_t content_length, Tier tier) {
  auto guard = lock_and_select(key, tier);
  if (!guard) return make_unexpected(guard.error());
  return guard->second->write_sync(key, content_length);
}

std::expected<void, CacheError> Cache::remove_sync(const CacheKey &key,
                                                   Tier tier) {
  auto guard = lock_and_select(key, tier);
  if (!guard) return make_unexpected(guard.error());
  return guard->second->remove_sync(key);
}

std::expected<bool, CacheError> Cache::exists_sync(const CacheKey &key,
                                                   Tier tier) {
  auto guard = lock_and_select(key, tier);
  if (!guard) return make_unexpected(guard.error());
  return guard->second->exists_sync(key);
}

std::expected<std::vector<AlternateInfo>, CacheError>
Cache::list_alternates_sync(const CacheKey &key, Tier tier) {
  auto guard = lock_and_select(key, tier);
  if (!guard) return make_unexpected(guard.error());
  return guard->second->list_alternates_sync(key);
}

std::expected<WriteHandle, CacheError> Cache::write_alternate_sync(
    const CacheKey &key, AlternateId alternate_id, uint64_t content_length,
    Tier tier) {
  auto guard = lock_and_select(key, tier);
  if (!guard) return make_unexpected(guard.error());
  return guard->second->write_alternate_sync(key, alternate_id, content_length);
}

std::expected<ReadHandle, CacheError> Cache::read_alternate_sync(
    const CacheKey &key, const StorageAlternateSelector &selector,
    const AlternateSelectionContext &ctx, Tier tier) {
  auto guard = lock_and_select(key, tier);
  if (!guard) return make_unexpected(guard.error());
  return guard->second->read_alternate_sync(key, selector, ctx);
}

std::expected<void, CacheError> Cache::remove_alternate_sync(
    const CacheKey &key, AlternateId alternate_id, Tier tier) {
  auto guard = lock_and_select(key, tier);
  if (!guard) return make_unexpected(guard.error());
  return guard->second->remove_alternate_sync(key, alternate_id);
}

void Cache::evict_from_ram_cache(const CacheKey &key, AlternateId id,
                                 Tier tier) {
  std::shared_lock lock(_impl->reader_gate());
  // With the small tier enabled, kSmall is intentionally a no-op: the small
  // volume has no RAM cache wired (see add_volume_locked).  With the tier
  // disabled, kSmall falls back to default routing like every other op.
  if (tier == Tier::kSmall && _impl->small_volume != nullptr) {
    return;
  }
  Volume *volume =
      select_volume(_impl->default_volumes, _impl->small_volume, key, tier);
  if (volume != nullptr) {
    volume->evict_from_ram_cache(key, id);
  }
}

#ifdef CYCLONE_HTTP_PLUGIN
Task<std::expected<ReadHandle, CacheError>> Cache::open_read_http(
    const CacheKey &key, std::span<const std::byte> request_headers) {
  (void)request_headers;
  co_return co_await open_read(key);
}
#endif

CacheStats Cache::stats() const {
  std::shared_lock lock(_impl->reader_gate());
  CacheStats result = _impl->stats;

  // Snapshot atomic flush stats (updated from hit tracker background thread)
  result.hit_flush_successes =
      _impl->flush_stats.hit_flush_successes.load(std::memory_order_relaxed);
  result.hit_flush_failures =
      _impl->flush_stats.hit_flush_failures.load(std::memory_order_relaxed);
  result.hit_flush_total_delta =
      _impl->flush_stats.hit_flush_total_delta.load(std::memory_order_relaxed);
  result.hit_flush_fsyncs =
      _impl->flush_stats.hit_flush_fsyncs.load(std::memory_order_relaxed);
  result.hit_flush_lock_contended =
      _impl->flush_stats.hit_flush_lock_contended.load(
          std::memory_order_relaxed);

  bool have_wrap = false;
  for (const auto &volume : _impl->volumes) {
    auto vs = volume->stats();
    result.disk_cache_hits += vs.reads;
    // bytes_read and bytes_written are not currently tracked per-operation.
    // VolumeStats tracks reads/writes (count) and bytes_used (total), but
    // not per-operation byte counts.
    result.current_entries += vs.entry_count;
    result.current_bytes += vs.bytes_used;
    result.stripe_count += vs.stripe_count;
    result.stripe_bytes += vs.stripe_bytes;
    result.evictions += vs.evictions;
    result.directory_syncs += vs.directory_syncs;
    result.fsyncs += vs.fsyncs;
    result.readahead_hints_issued += vs.readahead_hints_issued;

    // Wrap-cadence telemetry: sum wrap counts; take the minimum interval
    // across volumes; last_* comes from the most recently wrapped volume.
    result.write_buffer_wraps += vs.wrap_count;
    if (vs.min_wrap_interval_ns != 0 &&
        (result.min_wrap_interval_ns == 0 ||
         vs.min_wrap_interval_ns < result.min_wrap_interval_ns)) {
      result.min_wrap_interval_ns = vs.min_wrap_interval_ns;
    }
    if (vs.wrap_count > 0 &&
        (!have_wrap || vs.last_wrap_age_ns < result.last_wrap_age_ns)) {
      have_wrap = true;
      result.last_wrap_age_ns = vs.last_wrap_age_ns;
      result.last_wrap_interval_ns = vs.last_wrap_interval_ns;
    }

    // Read-lease counters (process-local, summed across volumes).
    result.wraps_deferred_by_lease += vs.wraps_deferred_by_lease;
    result.writes_dropped_by_lease += vs.writes_dropped_by_lease;
    result.borrows_outstanding += vs.borrows_outstanding;
    result.wraps_forced_past_lease += vs.wraps_forced_past_lease;

    // Full-bucket evictions (process-local, summed).
    result.tag_collision_evictions += vs.tag_collision_evictions;
    result.bucket_full_evictions += vs.bucket_full_evictions;

    // Volumes whose reset gate degraded on a lock-less filesystem (gauge).
    result.volumes_with_degraded_reset_gate += vs.reset_gate_degraded;

    // Reset provenance (monotonic counters, summed across volumes).
    result.resets_under_degraded_gate += vs.resets_under_degraded_gate;
    result.resets_gate_verified += vs.resets_gate_verified;

    // Alternate-chain shadow bound (process-local, summed) -- except the
    // depth, which is a high-water MARK and therefore maxed, not summed.
    result.alternate_shadows_unlinked += vs.alternate_shadows_unlinked;
    result.alternate_splice_deferred += vs.alternate_splice_deferred;
    result.alternate_chain_resets += vs.alternate_chain_resets;
    result.alternate_max_chain_depth = std::max(
        result.alternate_max_chain_depth, vs.alternate_max_chain_depth);
    result.alternate_wrap_refusals += vs.alternate_wrap_refusals;

    // Cross-process RAM coherence (process-local, summed).
    result.ram_coherence_rejections += vs.ram_coherence_rejections;
    result.ram_coherence_put_rejections += vs.ram_coherence_put_rejections;

    // Wrap retention (process-local, summed).
    result.frontier_advances += vs.frontier_advances;
    result.advances_deferred_by_lease += vs.advances_deferred_by_lease;
    result.early_advances_skipped += vs.early_advances_skipped;
    result.retained_hits += vs.retained_hits;
    result.stamp_rejections += vs.stamp_rejections;
  }

  if (_impl->ram_cache) {
    auto rs = _impl->ram_cache->stats();
    result.ram_cache_hits = rs.hits;
    result.ram_cache_misses = rs.misses;
    result.ram_cache_bytes = rs.bytes_used;
  }

  return result;
}

void Cache::reset_stats() {
  GateExclusive lock(_impl->gate);
  _impl->stats = CacheStats{};
  _impl->flush_stats.hit_flush_successes.store(0, std::memory_order_relaxed);
  _impl->flush_stats.hit_flush_failures.store(0, std::memory_order_relaxed);
  _impl->flush_stats.hit_flush_total_delta.store(0, std::memory_order_relaxed);
  _impl->flush_stats.hit_flush_fsyncs.store(0, std::memory_order_relaxed);
  _impl->flush_stats.hit_flush_lock_contended.store(0,
                                                    std::memory_order_relaxed);
}

const CacheConfig &Cache::config() const { return _impl->config; }

size_t Cache::volume_count() const {
  std::shared_lock lock(_impl->reader_gate());
  return _impl->volumes.size();
}

std::vector<VolumeFileInfo> Cache::volume_files() const {
  std::shared_lock lock(_impl->reader_gate());
  return _impl->volume_files;
}

bool Cache::small_tier_active() const {
  std::shared_lock lock(_impl->reader_gate());
  return _impl->small_volume != nullptr;
}

bool Cache::cross_process_ram_coherence_active() const {
  std::shared_lock lock(_impl->reader_gate());
  return _impl->config.cross_process_ram_coherence &&
         _impl->config.multi_process_config.enabled &&
         _impl->ram_cache != nullptr;
}

uint64_t Cache::total_capacity() const {
  std::shared_lock lock(_impl->reader_gate());
  uint64_t total = 0;
  for (const auto &volume : _impl->volumes) {
    total += volume->capacity();
  }
  return total;
}

uint64_t Cache::bytes_used() const {
  std::shared_lock lock(_impl->reader_gate());
  uint64_t used = 0;
  for (const auto &volume : _impl->volumes) {
    used += volume->bytes_used();
  }
  return used;
}

PluginManager &Cache::plugin_manager() { return *_impl->plugin_manager; }

const PluginManager &Cache::plugin_manager() const {
  return *_impl->plugin_manager;
}

OptimizationEngine *Cache::optimization_engine() {
  return _impl->optimization_engine.get();
}

const OptimizationEngine *Cache::optimization_engine() const {
  return _impl->optimization_engine.get();
}

}  // namespace cyclone
