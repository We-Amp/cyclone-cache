// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#pragma once

#include <atomic>
#include <expected>
#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>

#include "cyclone/alternate.hpp"
#include "cyclone/config.hpp"
#include "cyclone/error.hpp"
#include "cyclone/handle.hpp"
#include "cyclone/key.hpp"
#include "cyclone/task.hpp"

namespace cyclone {

class Volume;
class PluginManager;
class OptimizationEngine;

struct CacheStats {
  uint64_t ram_cache_hits = 0;
  uint64_t ram_cache_misses = 0;
  uint64_t disk_cache_hits = 0;
  uint64_t disk_cache_misses = 0;

  uint64_t bytes_read = 0;     // Not yet tracked per-operation (always 0)
  uint64_t bytes_written = 0;  // Not yet tracked per-operation (always 0)

  uint64_t current_entries = 0;
  uint64_t current_bytes = 0;
  uint64_t ram_cache_bytes = 0;

  // Stripe geometry summed across all disk volumes.  For a
  // single-volume cache stripe_count is that volume's stripe count and
  // stripe_bytes is the sum of its stripe sizes -- equal to the usable
  // (header-excluded) size on the auto path, or less by the dropped tail when
  // an explicit stripe_size leaves a partial last stripe.
  uint64_t stripe_count = 0;
  uint64_t stripe_bytes = 0;

  uint64_t evictions = 0;

  // Hit tracking flush diagnostics
  uint64_t hit_flush_successes = 0;
  uint64_t hit_flush_failures = 0;
  uint64_t hit_flush_total_delta = 0;  // Sum of all flushed hit deltas
  uint64_t hit_flush_fsyncs = 0;  // Number of fsync calls after flush cycles
  // Deltas dropped because the cross-process write lock was contended or a
  // wrap raced the resolve; best-effort by contract, but surfaced so the
  // drop is observable rather than silently counted as a success.
  uint64_t hit_flush_lock_contended = 0;

  // Completed periodic directory syncs across all volumes (persistent mode;
  // see CacheConfig::directory_sync_interval)
  uint64_t directory_syncs = 0;

  // Total raw fd fsyncs across all volumes (per-write + periodic +
  // init/removal).  Convoy instrument.
  uint64_t fsyncs = 0;

  // Wrap-cadence telemetry: how often the circular write buffer wraps back
  // to the start of a stripe's data area (aggregated across all volumes).
  // Sizing instrument for the eviction-vs-reader race.
  //
  // Units are deliberately nanoseconds on a steady clock (older stats in
  // this codebase use _ms on the wall clock).
  //
  // Semantics and caveats (see VolumeStats in src/core/volume.hpp for the
  // full discussion):
  // - Wraps happen per stripe but are counted per volume; on multi-stripe
  //   volumes the intervals measure inter-wrap spacing across the whole
  //   volume, not one buffer's wrap period. write_buffer_wraps is the
  //   primary signal.
  // - In multi-process (mmap-directory) mode, write_buffer_wraps and
  //   last_wrap_age_ns are shared across processes; the interval fields
  //   are per-process (only wraps performed by this process).
  // - last_wrap_interval_ns can be 0 while min_wrap_interval_ns > 0 when
  //   the most recently wrapped volume has wrapped exactly once.
  // - min <= last is not guaranteed when polled concurrently with a wrap.
  uint64_t write_buffer_wraps = 0;  // Total wraps since the volumes opened
  // Interval fields are 0 (undefined) until a volume has wrapped at least
  // twice. last_* fields describe the volume that wrapped most recently;
  // min is the smallest interval seen on any volume.
  uint64_t last_wrap_interval_ns = 0;
  uint64_t min_wrap_interval_ns = 0;
  // ns since the most recent wrap on any volume, at the time stats() was
  // called. Only meaningful when write_buffer_wraps > 0.
  uint64_t last_wrap_age_ns = 0;

  // Lease-based region pinning, aggregated
  // across all volumes.  All three are PROCESS-LOCAL — the writer that
  // defers/drops/forces is the one that counts — so in multi-process mode
  // each writer process reports its own view.
  uint64_t wraps_deferred_by_lease = 0;  // Wraps deferred by a live lease
  uint64_t writes_dropped_by_lease = 0;  // Fills dropped by deferred wraps
  uint64_t wraps_forced_past_lease = 0;  // Wraps forced past the
                                         // lease_wrap_ceiling (holds longer
                                         // than the ceiling are unprotected)

  // Live disk-hit borrows (open ReadHandles) across all volumes at the
  // instant stats() ran.  A GAUGE, not a counter; in
  // multi-process (mmap) mode it includes borrows held by ALL processes
  // (saturating at 255 per stripe).  Nonzero at cache-full means
  // wrap-needing fills are being deferred on the holders' behalf — the
  // signal that a consumer is holding handles open across its own writes.
  uint64_t borrows_outstanding = 0;

  // Directory entries evicted because a DIFFERENT key colliding on
  // (bucket, 12-bit tag) had to land in a completely full bucket — the
  // collider is replaced rather than silently updated in place (see
  // VolumeStats in src/core/volume.hpp).  Aggregated across all volumes;
  // PROCESS-LOCAL (the writer that evicts is the one that counts).
  uint64_t tag_collision_evictions = 0;

  // Number of currently-open disk volumes whose cross-process reset gate is
  // NOT in effect -- an incompatible open will reset even under a live peer.
  // A GAUGE.  Nonzero on ANY platform => the filesystem lacks working
  // byte-range locks (NFS/overlay, or an exotic Win32 redirector); move the
  // cache to one that has them if live-peer safety matters.  The gate is
  // implemented on Windows too, so nonzero is NOT expected there either (and
  // is nearly unreachable in practice: a filesystem where LockFileEx genuinely
  // fails typically fails the blocking init lock first, so open() fails with
  // IoError before the gate can degrade).
  uint64_t volumes_with_degraded_reset_gate = 0;

  // Directory entries evicted because a bucket was completely full of
  // current-phase entries with no tag collision to displace — the entry
  // nearest the wrap cursor is replaced so the write still lands (see
  // VolumeStats in src/core/volume.hpp).  Disjoint from
  // tag_collision_evictions.  Aggregated across all volumes; PROCESS-LOCAL
  // (the writer that evicts is the one that counts).  Appended at the tail to
  // match the field order of CycloneCacheStats (see cyclone_c.h:148-151).
  uint64_t bucket_full_evictions = 0;

  // Reset provenance COUNTERS, summed across volumes, monotonic since process
  // start and NOT cleared by close/reopen (unlike the gauge above).  Appended
  // at the tail to match the field order of CycloneCacheStats.
  //
  // THE ALARM: a cache reset ran on some volume while the reset gate was NOT in
  // effect, so a live peer MAY have been wiped.  Expected 0.  Deliberately does
  // NOT count a reset of a freshly created inode (no peer was possible), so a
  // routine `rm cyclone.dat` + reload never raises it.
  uint64_t resets_under_degraded_gate = 0;
  // The healthy upgrade path: a reset ran with the exclusive lifetime lock
  // proven held, i.e. no live peer had the volume open.
  uint64_t resets_gate_verified = 0;

  // Alternate-chain shadow bound; PROCESS-LOCAL, and appended at the tail to
  // match the field order of CycloneCacheStats.  Full semantics on
  // VolumeStats in src/core/volume.hpp.  Aggregated across volumes: every
  // counter is SUMMED unless its per-field note says otherwise --
  // alternate_max_chain_depth is a high-water MARK and is maxed, not summed.
  //
  //   alternate_shadows_unlinked: superseded same-id chain nodes spliced out
  //     by an alternate write -- the mechanism working.
  //   alternate_splice_deferred: superseded nodes left linked because the
  //     splice could not proceed (the write still succeeded).  The leading
  //     indicator that the depth bound is degrading.
  //   alternate_chain_resets: chains reset at the traversal cap because every
  //     visible node was a superseded copy of the id being written.  Nonzero
  //     in steady state is an alert.
  //   alternate_max_chain_depth: HIGH-WATER MARK (not a sum) of the physical
  //     chain depth seen by an alternate write before it prepended.  Expect it
  //     to settle at the number of distinct alternate ids stored per key.
  //   alternate_wrap_refusals: writes whose allocation wrapped the stripe and
  //     that therefore refused to link their new head to the pre-wrap chain,
  //     starting a fresh chain instead.  This is what a wrap-race
  //     event looks like post-migration.
  uint64_t alternate_shadows_unlinked = 0;
  uint64_t alternate_splice_deferred = 0;
  uint64_t alternate_chain_resets = 0;
  uint64_t alternate_max_chain_depth = 0;
  uint64_t alternate_wrap_refusals = 0;

  // Cross-process RAM coherence (PROCESS-LOCAL, summed across volumes,
  // appended at the tail to match the field order of CycloneCacheStats).
  // Both stay 0 unless CacheConfig::cross_process_ram_coherence is set AND
  // the cache runs with the shared mmap directory -- which is how a consumer
  // tells the feature apart from a no-op.  Full semantics on VolumeStats in
  // src/core/volume.hpp.
  //
  //   ram_coherence_rejections: RAM hits discarded because a peer (or this
  //     process) mutated the key's directory bucket since the entry was
  //     admitted.  Nonzero PROVES the feature is doing work and quantifies
  //     real peer-write staleness.  Mind the arithmetic -- the RAM cache
  //     counts the hit before the volume rejects it:
  //       true_served_ram_hits    = ram_cache_hits - ram_coherence_rejections
  //       false_invalidation_rate = ram_coherence_rejections / ram_cache_hits
  //     A high ratio means BUCKET granularity is costing you (any write to
  //     the bucket invalidates, not just a re-record of your key); the honest
  //     responses are a smaller RAM tier, better write partitioning, or
  //     turning the knob back off.
  //   ram_coherence_put_rejections: RAM inserts declined because the bucket
  //     moved during the read that would have populated the entry.  A pure
  //     saving (the entry would have failed its first validation), but the
  //     same crowding signal.
  uint64_t ram_coherence_rejections = 0;
  uint64_t ram_coherence_put_rejections = 0;
};

// Thread-safe counters for stats updated from background threads (hit tracker).
// Separated from CacheStats to avoid making the public struct harder to use.
struct AtomicFlushStats {
  std::atomic<uint64_t> hit_flush_successes{0};
  std::atomic<uint64_t> hit_flush_failures{0};
  std::atomic<uint64_t> hit_flush_total_delta{0};
  std::atomic<uint64_t> hit_flush_fsyncs{0};
  std::atomic<uint64_t> hit_flush_lock_contended{0};
};

// Identity of one open volume's backing file (see Cache::volume_files()).
struct VolumeFileInfo {
  // VolumeConfig::path as configured by the caller.  The automatic small-tier
  // carve-out volume appears with its derived "<configured path>.small".
  std::string configured_path;
  // Actual on-disk path after structural-fingerprint naming -- generally
  // "<stem>-<format major>-<geohash><ext>", or configured_path itself when an
  // existing legacy file at that path was adopted (see fingerprint_cache_path
  // in src/core/volume.cpp).
  std::string file_path;
  Tier tier = Tier::kDefault;
};

class Cache {
 public:
  Cache();
  explicit Cache(const CacheConfig &config);
  ~Cache();

  Cache(const Cache &) = delete;
  Cache &operator=(const Cache &) = delete;
  Cache(Cache &&) = default;
  Cache &operator=(Cache &&) = default;

  static std::expected<std::unique_ptr<Cache>, CacheError> create(
      const CacheConfig &config);

  std::expected<void, CacheError> add_volume(const std::string &path,
                                             size_t size);
  std::expected<void, CacheError> add_volume(const VolumeConfig &config);

  std::expected<void, CacheError> start();
  void stop();

  [[nodiscard]] bool is_running() const;

  // All key-routed operations take an optional storage tier (see the Tier
  // documentation in config.hpp).  Tier::kDefault preserves the existing
  // behavior; Tier::kSmall routes to the dedicated small-object volume when
  // the small tier is enabled, and falls back to default routing otherwise.
  Task<std::expected<ReadHandle, CacheError>> open_read(
      const CacheKey &key, Tier tier = Tier::kDefault);
  Task<std::expected<WriteHandle, CacheError>> open_write(
      const CacheKey &key, Tier tier = Tier::kDefault);
  Task<std::expected<WriteHandle, CacheError>> open_write(
      const CacheKey &key, uint64_t content_length, Tier tier = Tier::kDefault);
  Task<std::expected<UpdateHandle, CacheError>> open_update(
      const CacheKey &key);
  Task<std::expected<void, CacheError>> remove(const CacheKey &key,
                                               Tier tier = Tier::kDefault);
  Task<std::expected<bool, CacheError>> exists(const CacheKey &key,
                                               Tier tier = Tier::kDefault);

  std::expected<ReadHandle, CacheError> read_sync(const CacheKey &key,
                                                  Tier tier = Tier::kDefault);
  std::expected<WriteHandle, CacheError> write_sync(const CacheKey &key,
                                                    Tier tier = Tier::kDefault);
  std::expected<WriteHandle, CacheError> write_sync(const CacheKey &key,
                                                    uint64_t content_length,
                                                    Tier tier = Tier::kDefault);
  std::expected<void, CacheError> remove_sync(const CacheKey &key,
                                              Tier tier = Tier::kDefault);
  std::expected<bool, CacheError> exists_sync(const CacheKey &key,
                                              Tier tier = Tier::kDefault);

  // Alternate chain operations
  std::expected<std::vector<AlternateInfo>, CacheError> list_alternates_sync(
      const CacheKey &key, Tier tier = Tier::kDefault);
  std::expected<WriteHandle, CacheError> write_alternate_sync(
      const CacheKey &key, AlternateId alternate_id, uint64_t content_length,
      Tier tier = Tier::kDefault);
  std::expected<ReadHandle, CacheError> read_alternate_sync(
      const CacheKey &key, const StorageAlternateSelector &selector,
      const AlternateSelectionContext &ctx, Tier tier = Tier::kDefault);
  std::expected<void, CacheError> remove_alternate_sync(
      const CacheKey &key, AlternateId alternate_id,
      Tier tier = Tier::kDefault);

  // Evict a specific (key, alternate) entry from the RAM cache.
  // No-op if RAM cache is disabled or the entry is not present.
  // (The small tier has no RAM cache, so kSmall is always a no-op there.)
  //
  // Best-effort hint only: unlike the remove/commit paths, this eviction
  // is NOT paired with a Stripe::remove_epoch bump, so it is not
  // synchronized with in-flight read-path repopulation — a concurrent read
  // that already probed the directory can re-put the entry after this call
  // returns (its conditional put's predicate still passes).  Callers
  // needing a no-resurrection guarantee must use remove_sync /
  // remove_alternate_sync instead.
  void evict_from_ram_cache(const CacheKey &key, AlternateId id,
                            Tier tier = Tier::kDefault);

#ifdef CYCLONE_HTTP_PLUGIN
  Task<std::expected<ReadHandle, CacheError>> open_read_http(
      const CacheKey &key, std::span<const std::byte> request_headers);
#endif

  [[nodiscard]] CacheStats stats() const;
  void reset_stats();

  [[nodiscard]] const CacheConfig &config() const;

  [[nodiscard]] size_t volume_count() const;

  // The backing file of every added volume, in add_volume() order (the
  // automatic small-tier carve-out appears as its own entry).  Structural-
  // fingerprint naming means file_path generally DIFFERS from the configured
  // path, and the mapping is stateful (an adopted legacy file keeps its raw
  // name) -- so embedders that touch the volume file itself (permission
  // fix-ups, reset-by-delete, opening a sendfile FD) must take file_path from
  // here rather than re-deriving it.  Populated by add_volume(); entries are
  // never removed for the life of the Cache.
  [[nodiscard]] std::vector<VolumeFileInfo> volume_files() const;
  [[nodiscard]] uint64_t total_capacity() const;
  [[nodiscard]] uint64_t bytes_used() const;

  // True when the small-object tier is enabled (a dedicated small volume
  // exists).  False either because small_tier_percent is 0 or because the
  // configured total was too small to host both tiers and the carve-out was
  // silently disabled (see CacheConfig::small_tier_percent) — integrators
  // that require the tier should assert this after add_volume().
  [[nodiscard]] bool small_tier_active() const;

  // True when cross-process RAM-cache coherence is actually in effect:
  // the knob is set AND the cache runs with the shared mmap directory AND a
  // RAM tier exists.  Enabling the knob without the other two is silently
  // inert (nothing to validate, or no shared signal to validate against), so
  // integrators that require the guarantee should assert this after
  // add_volume() rather than trust the config field.  See
  // CacheConfig::cross_process_ram_coherence.
  [[nodiscard]] bool cross_process_ram_coherence_active() const;

  PluginManager &plugin_manager();
  [[nodiscard]] const PluginManager &plugin_manager() const;

  OptimizationEngine *optimization_engine();
  [[nodiscard]] const OptimizationEngine *optimization_engine() const;

 private:
  [[nodiscard]] std::expected<
      std::pair<std::shared_lock<std::shared_mutex>, Volume *>, CacheError>
  lock_and_select(const CacheKey &key, Tier tier) const;

  // add_volume() body without locking; callers hold _impl->mutex exclusively.
  std::expected<void, CacheError> add_volume_locked(const VolumeConfig &config);

  struct Impl;
  std::unique_ptr<Impl> _impl;
};

}  // namespace cyclone
