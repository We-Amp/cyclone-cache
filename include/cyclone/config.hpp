// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace cyclone {

class CachePlugin;

inline constexpr size_t operator""_KB(unsigned long long v) { return v * 1024; }
inline constexpr size_t operator""_MB(unsigned long long v) {
  return v * 1024 * 1024;
}
inline constexpr size_t operator""_GB(unsigned long long v) {
  return v * 1024 * 1024 * 1024;
}

enum class RamCacheType : std::uint8_t {
  LRU,
  CLFUS  // Scan-resistant
};

// Storage tier for key-routed operations.
//
// kDefault is the existing behavior: keys hash-route over the default
// (payload) volume(s).  kSmall routes to a physically separate small-object
// volume (see CacheConfig::small_tier_percent), so large-payload churn on the
// default tier can never evict small-tier entries.  The two tiers are
// separate keyspaces: the same key written to both tiers refers to two
// independent entries, and reads only see the tier they were issued against.
//
// When the small tier is not enabled (small_tier_percent == 0, or the
// configured cache is too small to host both tiers), kSmall operations fall
// back to default routing, so callers may pass a tier unconditionally.
enum class Tier : std::uint8_t {
  kDefault = 0,
  kSmall = 1,
};

// Multi-process configuration for stripe affinity
// When enabled, each process owns a subset of stripes and can only write to
// those stripes. All processes can read from all stripes via mmap, with torn
// read detection via CRC32.
struct MultiProcessConfig {
  bool enabled = false;        // Disabled by default for backward compatibility
  uint32_t process_index = 0;  // This process's index (0 to total_processes-1)
  uint32_t total_processes = 1;  // Total number of processes sharing the cache
  uint32_t max_read_retries =
      1;  // Max retries on checksum mismatch (torn read)

  // Builder methods
  MultiProcessConfig &set_enabled(bool val) {
    enabled = val;
    return *this;
  }
  MultiProcessConfig &set_process_index(uint32_t val) {
    process_index = val;
    return *this;
  }
  MultiProcessConfig &set_total_processes(uint32_t val) {
    total_processes = val;
    return *this;
  }
  MultiProcessConfig &set_max_read_retries(uint32_t val) {
    max_read_retries = val;
    return *this;
  }

  // Validation
  [[nodiscard]] bool is_valid() const {
    if (!enabled) return true;
    return total_processes > 0 && process_index < total_processes;
  }
};

struct OptimizationConfig {
  // Thread pool sizing
  size_t min_threads = 1;
  size_t max_threads = 0;  // 0 = auto (hardware_concurrency / 2)

  // Autoscaling thresholds (queue depth)
  size_t scale_up_threshold = 10;   // Scale up when queue > this per thread
  size_t scale_down_threshold = 2;  // Scale down when queue < this per thread
  std::chrono::milliseconds scale_check_interval{1000};

  // Resource limits (reserved for future use - not currently enforced)
  double max_cpu_usage = 0.5;  // Max fraction of CPU for optimization
  double max_io_bandwidth_fraction =
      0.3;  // Max fraction of I/O for optimization
  size_t max_queue_size = 10000;
  size_t max_memory_bytes = 256_MB;

  // Load shedding watermarks
  double load_high_watermark = 0.8;  // Pause when load exceeds this
  double load_low_watermark = 0.5;   // Resume when load drops below this
  std::chrono::milliseconds load_shedding_cooldown{
      5000};  // Wait this long after load drops before resuming

  // LoadMonitor baseline capacities for load estimation
  double baseline_ops_per_sec = 100000.0;  // Ops/sec at 100% CPU
  double baseline_bytes_per_sec = 500_MB;  // Bytes/sec at 100% I/O

  // Priority settings
  bool prioritize_by_hit_count = true;
  uint32_t min_hits_before_optimize =
      2;  // Require N hits before queuing optimization

  // Enable/disable the entire optimization system
  bool enabled = true;

  // Builder methods for fluent configuration
  OptimizationConfig &set_enabled(bool val) {
    enabled = val;
    return *this;
  }
  OptimizationConfig &set_min_threads(size_t val) {
    min_threads = val;
    return *this;
  }
  OptimizationConfig &set_max_threads(size_t val) {
    max_threads = val;
    return *this;
  }
  OptimizationConfig &set_scale_up_threshold(size_t val) {
    scale_up_threshold = val;
    return *this;
  }
  OptimizationConfig &set_scale_down_threshold(size_t val) {
    scale_down_threshold = val;
    return *this;
  }
  OptimizationConfig &set_scale_check_interval(std::chrono::milliseconds val) {
    scale_check_interval = val;
    return *this;
  }
  OptimizationConfig &set_max_cpu_usage(double val) {
    max_cpu_usage = val;
    return *this;
  }
  OptimizationConfig &set_max_io_bandwidth_fraction(double val) {
    max_io_bandwidth_fraction = val;
    return *this;
  }
  OptimizationConfig &set_max_queue_size(size_t val) {
    max_queue_size = val;
    return *this;
  }
  OptimizationConfig &set_max_memory_bytes(size_t val) {
    max_memory_bytes = val;
    return *this;
  }
  OptimizationConfig &set_load_high_watermark(double val) {
    load_high_watermark = val;
    // Ensure high > low invariant
    if (load_low_watermark >= load_high_watermark) {
      load_low_watermark = load_high_watermark * 0.6;
    }
    return *this;
  }
  OptimizationConfig &set_load_low_watermark(double val) {
    load_low_watermark = val;
    // Ensure high > low invariant
    if (load_low_watermark >= load_high_watermark) {
      load_high_watermark = load_low_watermark / 0.6;
    }
    return *this;
  }
  OptimizationConfig &set_prioritize_by_hit_count(bool val) {
    prioritize_by_hit_count = val;
    return *this;
  }
  OptimizationConfig &set_min_hits_before_optimize(uint32_t val) {
    min_hits_before_optimize = val;
    return *this;
  }
  OptimizationConfig &set_load_shedding_cooldown(
      std::chrono::milliseconds val) {
    load_shedding_cooldown = val;
    return *this;
  }
  OptimizationConfig &set_baseline_ops_per_sec(double val) {
    baseline_ops_per_sec = val;
    return *this;
  }
  OptimizationConfig &set_baseline_bytes_per_sec(double val) {
    baseline_bytes_per_sec = val;
    return *this;
  }

  // Compute effective max threads (resolves 0 = auto)
  [[nodiscard]] size_t effective_max_threads() const {
    if (max_threads == 0) {
      size_t hw = std::thread::hardware_concurrency();
      return hw > 2 ? hw / 2 : 1;
    }
    return max_threads;
  }
};

struct CacheConfig {
  // In-memory (RAM) tier size.  0 disables the RAM tier outright: the cache
  // runs disk-only (see Cache::Cache, which only builds a RamCache when this
  // is > 0).  The C API copies CycloneCacheConfig::ram_cache_size_bytes here
  // verbatim with no sentinel mapping, so a zero-initialised C config gets no
  // RAM tier rather than this 256 MB default -- the opposite convention from
  // max_object_size below, where the C layer maps 0 to the default.
  size_t ram_cache_size = 256_MB;
  RamCacheType ram_cache_type = RamCacheType::CLFUS;
  size_t max_mapped_size = 256_MB;
  size_t directory_entry_overhead = 10;

  uint32_t num_segments = 4;
  // Per-object content-size bound, enforced at the volume write entry for
  // originals and alternates alike: a put whose content exceeds this
  // fails with CacheError::ObjectTooLarge before anything is buffered or
  // written; a put at exactly the bound succeeds.  0 disables the bound.
  // The C API deliberately reads zero the other way round (0 = this default,
  // UINT64_MAX = disabled), and differently again from ram_cache_size
  // above; see CycloneCacheConfig::max_object_size for that mapping.
  size_t max_object_size = 64_MB;
  size_t target_frag_size = 1_MB;
  double avg_object_size = 8_KB;
  double bucket_multiplier = 1.0;

  std::chrono::seconds gc_interval{60};
  double gc_evacuate_threshold = 0.8;

  // Store a CRC32 with each document (verified on read whenever the stored
  // CRC is non-zero).  The C API copies CycloneCacheConfig::enable_checksum
  // here verbatim with no default and no sentinel mapping, so a
  // zero-initialised C config runs with checksums DISABLED rather than this
  // default of true -- the one remaining C field whose zero-init
  // diverges; see CycloneCacheConfig::enable_checksum.
  bool enable_checksum = true;
  bool verify_checksum_on_read =
      true;  // Verify CRC32 on every read (disable for perf)
  bool enable_compression = false;
  int io_queue_depth = 64;

  // Hit tracking
  std::chrono::milliseconds hit_flush_interval{
      1000};  // How often to flush hit counts to disk
  uint32_t hit_flush_threshold{
      10000};                      // Flush key immediately if hits exceed this
  bool enable_hit_tracking{true};  // Enable persistent hit tracking

  // Small-object tier (opt-in, 0 = disabled).
  //
  // When > 0, the first added default volume is split: this percentage of its
  // configured size is carved out into a physically separate small-object
  // volume (stored next to the main volume file, at "<path>.small") that only
  // Tier::kSmall operations route to.  The default volume keeps the
  // remainder.  Values are clamped to [1, 50].
  //
  // Sizing guards (applied at add_volume() time):
  //   - The small volume is never smaller than one minimum-size stripe
  //     (128 MB) plus the volume header.  In multi-process mode the floor
  //     grows to total_processes stripes so that every process owns at least
  //     one small-tier stripe (stripe ownership is stripe_index %
  //     total_processes; with fewer stripes than processes, some processes
  //     could never write to the small tier).
  //   - If the configured total cannot host BOTH the small-volume floor and
  //     one minimum-size stripe for the default volume, the small tier is
  //     silently disabled and the cache behaves exactly as a single volume
  //     (kSmall operations fall back to default routing).  Query
  //     Cache::small_tier_active() after add_volume() to detect this: false
  //     with a nonzero percent means the total was below the floor
  //     (~256 MB single-process; (total_processes + 1) x 128 MB
  //     multi-process).
  //
  // Multi-process deployments: all processes sharing the cache files MUST
  // pass the same small_tier_percent and total size — the volume geometry is
  // validated against the on-disk headers at open(), and a mismatch resets
  // the affected volume (see doc/multi-process.md).
  uint32_t small_tier_percent = 0;

  // Opt-in (default OFF) startup disk hygiene: on start(), after every volume
  // has opened+locked, delete SUPERSEDED structural-fingerprint cache files
  // (files from an earlier on-disk FORMAT/GEOMETRY that fingerprinting left
  // behind; see fingerprint_cache_path / gc_superseded_volumes in volume.cpp).
  // Safety-critical, so it is strictly additive and OFF by default: GC never
  // deletes a file a live peer holds, a legacy un-fingerprinted cyclone.dat, or
  // a foreign look-alike, and never fails start().  POSIX-only (a no-op on
  // Windows): live peers CAN exist across a Windows upgrade, but the safety
  // argument GC needs is not yet established there -- the Windows open path
  // drops its exclusive lock before taking the shared one, and the inode
  // revalidation that backstops that gap is itself POSIX-only (see
  // gc_superseded_volumes in volume.cpp).
  //
  // "Superseded" is a STRUCTURAL definition, not a provenance one: GC reclaims
  // ANY unheld, aged, valid-header, fingerprint-shaped file that shares a live
  // volume's base stem IN THAT VOLUME'S DIRECTORY.  It deliberately does not
  // try to tell an old file this cache wrote from a co-located FOREIGN but
  // valid Cyclone volume parked under a matching name -- such a foreign file
  // would also be reclaimed.  Enable ONLY when the cache directory is owned
  // exclusively by this cache path.
  bool gc_superseded_on_start = false;

  // Unlink superseded alternates on write (default ON); propagated to every
  // volume added to this cache.  See VolumeConfig::unlink_superseded_alternates
  // for the full semantics -- it is a kill switch for the in-place chain
  // repoint, and the wrap-frontier link refusal / single-id chain reset stay
  // active regardless.
  bool unlink_superseded_alternates = true;

  // Cross-process RAM-cache coherence.  Default OFF; propagated to
  // every volume added to this cache.
  //
  // WHAT IT REMOVES.  The RAM tier is process-local while the directory is
  // shared, so a peer process that re-records or purges content leaves THIS
  // process serving the superseded bytes from its own RAM tier until its
  // LRU/CLFUS happens to evict them.  With this on, every RAM entry carries
  // the shared directory bucket version sampled when it was admitted, and
  // every RAM hit revalidates that stamp against the bucket's current
  // version: a mismatch drops the entry and falls through to disk.
  //
  // PROCESS-LOCAL DECLARATION, NOT A PROTOCOL.  Set it per process,
  // independently.  It needs no agreement from peers and no restart
  // ordering: every directory mutation already publishes the bucket-version
  // bump as part of the existing per-bucket seqlock, so a peer protects this
  // process whether or not it (or its binary) knows about this setting.
  //
  // COST.  One shared-memory acquire load per RAM hit, against a path that
  // already takes a RAM-cache lock and copies the document — small, but not
  // free.  When OFF, the read path is unchanged: no shared line is touched
  // and no atomic executes.
  //
  // INERT UNLESS IT CAN WORK.  Validation only engages on a volume backed by
  // the shared mmap directory (multi_process_config.enabled) and only when a
  // RAM tier exists (ram_cache_size > 0).  Enabling it in a single-process
  // cache changes nothing at all.  Query
  // Cache::cross_process_ram_coherence_active() after add_volume() to confirm
  // it took effect.
  //
  // GRANULARITY WARNING — this is why it is OFF by default.  The signal is
  // per BUCKET, not per entry: ANY directory write to the bucket a key hashes
  // to (an unrelated insert, an eviction, a collision replacement) invalidates
  // this process's RAM copy.  Hot keys are essentially unaffected; warm and
  // cold entries in a write-active cache are dropped often.  Watch
  // CacheStats::ram_coherence_rejections against ram_cache_hits to measure it
  // on your own workload (see doc/multi-process.md for the rate table).
  //
  // ASSUMES A FIXED VOLUME SET.  Key-to-volume routing is by
  // segment_hash() % default_volumes.size(), so a key's stamp always
  // originates from the same volume's directory only while the default-volume
  // set is unchanged after start().  Do not add volumes to a running cache
  // with this enabled.
  bool cross_process_ram_coherence = false;

  // Periodic sync of persistent (mmap'd) directories.  Only used when
  // multi_process_config.enabled — the only mode with an on-disk directory.
  // Bounds the power-loss window for published directory entries: entries
  // inserted since the last sync may be lost on power failure, and an in-place
  // overwrite whose new data was not yet synced may serve its prior value (see
  // the durability contract in Volume::sync_directory(), src/core/volume.cpp).
  // 0 = disabled (directory pages then reach disk only via OS writeback).
  //
  // Default 30s: since multi-process mode no longer forces per-write fsync
  // (it caused an fsync convoy on the shared inode), this periodic sync is the
  // durability mechanism.  30s aligns with the kernel's dirty-expire writeback
  // horizon (so the fsync mostly makes durable what writeback was already
  // flushing) and cuts journal-commit frequency ~30x vs the old 1s, while the
  // crash-loss / stale-hit window stays well inside the tolerance for a
  // reconstructible best-effort cache.
  std::chrono::milliseconds directory_sync_interval{30000};

  // Lease-based region pinning (borrow-scoped
  // since the write-starvation fix): every disk-borrow read (read_sync /
  // read_alternate_sync disk hit) registers itself in a per-stripe
  // outstanding-borrow count — dropped when the ReadHandle is closed or
  // destroyed — and stamps a per-stripe lease of this duration.  A writer
  // defers a circular write-buffer wrap (dropping the fill) only while
  // BOTH hold: the count is nonzero (a live ReadHandle still aliases the
  // stripe) AND the lease is unexpired, so the borrowed mmap bytes are
  // never overwritten in place under a live ReadHandle, while a
  // promptly-closed read costs writers nothing beyond its open window
  // (before the borrow gate the lease alone gated wraps, write-starving any
  // at-capacity stripe read more often than once per T).  RAM-cache hits and
  // misses never stamp or count.  The write-avoidance guard skips re-stamping
  // while the current lease still covers now + 3T/4, so the guaranteed
  // protection floor for an OPEN handle is 3T/4 — embedders holding a
  // borrow longer must call ReadHandle::renew_lease() at a cadence
  // <= 3T/4 (an open handle whose lease lapsed is unprotected; this is
  // also what lets a crashed holder's leaked state stop blocking writers
  // within T).  0 = leases disabled (pre-lease unprotected behavior).
  std::chrono::milliseconds read_lease_duration{5000};

  // Anti-starvation ceiling for the wrap gate: if a stripe's wrap has been
  // continuously deferred for longer than this, the writer wraps anyway
  // (counted in wraps_forced_past_lease) and resets the stripe's
  // outstanding-borrow count, so state leaked by a crashed
  // borrow holder starves the stripe for at most one ceiling episode.
  // The ceiling is per-STRIPE-EPISODE, NOT per-hold: the deferral clock
  // (wrap_deferred_since_ns) starts when ANY borrow first defers a wrap on
  // the stripe and runs until the episode ends (a permitted or forced
  // wrap), so a borrow taken late in an in-progress episode inherits an
  // already-elapsed clock and has less-than-ceiling — possibly near-zero —
  // headroom before a forced wrap. A holder must therefore not budget the
  // full ceiling; use ns_until_forced_wrap() (which reports the actual
  // remaining headroom) to decide when to copy. Borrows still aliasing at
  // the forced wrap are unprotected — bounded, observable via
  // ns_until_forced_wrap(), and part of the embedder contract.
  std::chrono::milliseconds lease_wrap_ceiling{60000};

  std::shared_ptr<CachePlugin> alternate_selector;

  // Background optimization configuration
  OptimizationConfig optimization_config;

  // Multi-process configuration
  MultiProcessConfig multi_process_config;

  CacheConfig &set_ram_cache_size(size_t size) {
    ram_cache_size = size;
    return *this;
  }
  CacheConfig &set_ram_cache_type(RamCacheType type) {
    ram_cache_type = type;
    return *this;
  }
  CacheConfig &set_max_mapped_size(size_t size) {
    max_mapped_size = size;
    return *this;
  }
  CacheConfig &set_alternate_selector(std::shared_ptr<CachePlugin> plugin) {
    alternate_selector = std::move(plugin);
    return *this;
  }
  CacheConfig &set_num_segments(uint32_t n) {
    num_segments = n;
    return *this;
  }
  CacheConfig &set_enable_checksum(bool enable) {
    enable_checksum = enable;
    return *this;
  }
  CacheConfig &set_verify_checksum_on_read(bool verify) {
    verify_checksum_on_read = verify;
    return *this;
  }
  CacheConfig &set_directory_sync_interval(std::chrono::milliseconds val) {
    directory_sync_interval = val;
    return *this;
  }
  CacheConfig &set_small_tier_percent(uint32_t percent) {
    small_tier_percent = percent;
    return *this;
  }
  CacheConfig &set_gc_superseded_on_start(bool enable) {
    gc_superseded_on_start = enable;
    return *this;
  }
  CacheConfig &set_cross_process_ram_coherence(bool enable) {
    cross_process_ram_coherence = enable;
    return *this;
  }
  CacheConfig &set_read_lease_duration(std::chrono::milliseconds val) {
    read_lease_duration = val;
    return *this;
  }
  CacheConfig &set_lease_wrap_ceiling(std::chrono::milliseconds val) {
    lease_wrap_ceiling = val;
    return *this;
  }
  CacheConfig &set_multi_process(uint32_t process_index,
                                 uint32_t total_processes) {
    multi_process_config.enabled = true;
    multi_process_config.process_index = process_index;
    multi_process_config.total_processes = total_processes;
    return *this;
  }
  CacheConfig &set_multi_process_config(const MultiProcessConfig &config) {
    multi_process_config = config;
    return *this;
  }
};

struct VolumeConfig {
  std::string path;
  size_t size = 0;

  // Which tier this volume serves.  kDefault volumes participate in the
  // hash routing used by Tier::kDefault operations; the (at most one)
  // kSmall volume exclusively serves Tier::kSmall operations.  Normally set
  // automatically by the CacheConfig::small_tier_percent carve-out — only
  // set explicitly when managing the small volume's size/path manually.
  Tier tier = Tier::kDefault;

  // 0 (default) = auto: derive the stripe count from the volume size, targeting
  // ~kAutoStripeTarget stripes floored at kMinStripeSize, so lease-based region
  // pinning (per-stripe) never pins the whole cache under mixed read/write. Set
  // a nonzero value to pin the stripe size explicitly.
  size_t stripe_size = 0;
  bool direct_io = false;
  bool sync_on_write = false;
  size_t max_fragments = 0;
  double ram_cache_proportion = 1.0;

  // Verify CRC32 checksums on every read.  When false, read-side verification
  // is skipped (write-side checksums are still computed).  Safe to disable when
  // using persistent mmap with a single writer (no torn reads possible).
  bool verify_checksum_on_read = true;

  // Version compatibility behavior
  // If true (default): automatically reset/purge cache on version mismatch
  // If false: return IncompatibleVersion error instead
  bool auto_reset_on_incompatible = true;

  // Unlink superseded alternates on write (default ON).  Re-recording an
  // alternate id prepends a new document; with this enabled the write also
  // splices the superseded same-id node(s) out of the chain, so the physical
  // chain depth stays bounded by the number of DISTINCT ids on the key
  // instead of growing with every refresh.  Normally set from
  // CacheConfig::unlink_superseded_alternates.
  //
  // This is a KILL SWITCH, not a tuning knob: it exists so a deployment can
  // disable the in-place chain repoint without a rebuild.  Turning it off
  // restores the unbounded-shadow behaviour, in which a key refreshed enough
  // times becomes permanently unwritable (TooManyAlternates).  The two
  // safety guards that ship with the mechanism -- the write-time
  // wrap-frontier link refusal and the single-id chain reset at the
  // traversal cap -- stay ACTIVE regardless of this setting.
  bool unlink_superseded_alternates = true;

  // Per-object content-size bound enforced at the write entry.
  // Normally set from CacheConfig::max_object_size; see its documentation
  // there.  0 disables the bound.
  size_t max_object_size = 64_MB;

  // Validate every RAM-cache hit against the shared directory's bucket
  // version, so a peer process's re-record or purge is not served from this
  // process's RAM tier.  Normally set from
  // CacheConfig::cross_process_ram_coherence; see its documentation there for
  // the full semantics, the cost, and the bucket-granularity warning.  Inert
  // unless this volume is backed by the shared mmap directory.
  bool cross_process_ram_coherence = false;

  // Lease-based region pinning parameters.  When the volume is
  // created via Cache::add_volume these are overwritten from the
  // CacheConfig fields of the same name (see their documentation there).
  std::chrono::milliseconds read_lease_duration{5000};
  std::chrono::milliseconds lease_wrap_ceiling{60000};
};

}  // namespace cyclone
