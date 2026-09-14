// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef CYCLONE_C_H
#define CYCLONE_C_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Error codes
 * -------------------------------------------------------------------------- */

using CycloneError = enum : uint8_t {
  CYCLONE_OK = 0,    /* Success */
  CYCLONE_NOT_FOUND, /* Entry not found (also used by exists() for "does not
                        exist") */
  CYCLONE_EXISTS,    /* Entry already exists */
  CYCLONE_NO_SPACE, /* No space available (disk full or in-flight limit reached)
                     */
  CYCLONE_IO_ERROR, /* I/O operation failed */
  CYCLONE_CORRUPTED,        /* Data corruption detected */
  CYCLONE_INVALID_KEY,      /* Invalid cache key */
  CYCLONE_INVALID_ARGUMENT, /* Invalid function argument (NULL pointer,
                               zero-length key, etc.) */
  CYCLONE_NOT_INITIALIZED,  /* Cache not started or shutting down */
  CYCLONE_INTERNAL_ERROR,   /* Unexpected internal error (should not occur in
                               normal use) */
  CYCLONE_RESET_REFUSED_LIVE_PEER, /* Cache reset refused: another process
                                      still has this cache open; drain it and
                                      retry.  This enum is APPEND-ONLY: new
                                      codes go at the tail, existing values
                                      never renumber. */
  CYCLONE_OBJECT_TOO_LARGE         /* Write content exceeds the configured
                                      max_object_size */
};

/* --------------------------------------------------------------------------
 * Storage tiers
 * -------------------------------------------------------------------------- */

/*
 * Storage tier for key-routed operations (see the *_tier function variants
 * and CycloneCacheConfig::small_tier_percent).  The two tiers are physically
 * separate keyspaces: the same key written to both tiers refers to two
 * independent entries.
 */
using CycloneTier = enum : uint8_t {
  CYCLONE_TIER_DEFAULT = 0, /* Hash-routed default (payload) volume — the
                               behavior of the tier-less functions */
  CYCLONE_TIER_SMALL = 1    /* Dedicated small-object volume, isolated from
                               default-tier eviction churn.  Falls back to
                               the default tier when the small tier is
                               disabled, so callers may pass it
                               unconditionally. */
};

/* --------------------------------------------------------------------------
 * Opaque handles
 * -------------------------------------------------------------------------- */

using CycloneCacheHandle = struct CycloneCacheHandle;
using CycloneReadHandle = struct CycloneReadHandle;

/* --------------------------------------------------------------------------
 * Configuration
 * -------------------------------------------------------------------------- */

using CycloneCacheConfig = struct {
  const char *cache_path;
  uint64_t cache_size_bytes;
  uint64_t ram_cache_size_bytes; /* In-memory (RAM) tier size in bytes, copied
                                    verbatim to CacheConfig::ram_cache_size.
                                    There is NO sentinel mapping here, so 0
                                    means the RAM tier is DISABLED -- not the
                                    256 MB C++ default.  A
                                    zero-initialised config therefore runs
                                    disk-only; pass an explicit size to get a
                                    RAM tier.  This is the opposite convention
                                    from max_object_size below, where C 0 means
                                    "the library default": for a size knob a
                                    zero-byte cache is the natural reading of
                                    zero, and remapping it would silently
                                    change every config that passes 0 today. */
  int enable_checksum;           /* Non-zero: store a CRC32 with each document
                                    (verified on read whenever the stored CRC is
                                    non-zero).  Copied verbatim to
                                    CacheConfig::enable_checksum, whose C++
                                    default is TRUE -- so a zero-initialised
                                    config runs with checksums DISABLED, the one
                                    remaining field where C zero-init diverges
                                    from the C++ default.  Set this to 1
                                    explicitly for the C++ behaviour.  0 keeps
                                    meaning "disabled": remapping it to the C++
                                    default would silently change every config
                                    that passes 0 today and, absent a new opt-out
                                    sentinel, remove the only way a C caller can
                                    turn checksums off.  (Multi-process mode --
                                    enable_mmap_directory -- refuses to start
                                    with checksums off either way.) */
  uint32_t num_segments;
  uint32_t max_in_flight_requests; /* Max concurrent miss handler calls.  0 is
                                      NOT unlimited: the bridge maps 0 to the
                                      default limit of 10000, so a C caller
                                      cannot disable the limit by passing 0. */
  int enable_mmap_directory; /* Non-zero: use mmap'd directory for cross-process
                                sharing */
  uint32_t small_tier_percent;  /* 0 = small tier disabled (default).  When > 0
                                   (clamped to 1..50), this percentage of
                                   cache_size_bytes is carved out into a
                                   physically separate small-object volume
                                   (stored at "<cache_path>.small") that only
                                   CYCLONE_TIER_SMALL operations route to.  If
                                   cache_size_bytes is too small to host both
                                   tiers, the tier is silently disabled and
                                   CYCLONE_TIER_SMALL falls back to the default
                                   tier — check cyclone_cache_small_tier_active()
                                   after create.
                                   ABI note: this trailing field extends the
                                   struct (same additive precedent as
                                   enable_mmap_directory).  There is NO
                                   mixed-version ABI safety for a distributed
                                   shared library: a caller compiled against an
                                   older header passes a smaller struct and the
                                   library would read garbage here.  Recompile
                                   against this header when adopting. */
  int disable_alternate_unlink; /* KILL SWITCH, default off (0 = the unlink is
                                   ENABLED, which is what you want).  Non-zero
                                   stops alternate writes from splicing the
                                   superseded same-id chain node out, restoring
                                   the unbounded-shadow behaviour in which a
                                   key refreshed enough times becomes
                                   permanently unwritable.  Stated in the
                                   negative deliberately: a zero-initialised
                                   config must land on the SAFE setting.  The
                                   wrap-frontier link refusal and the
                                   single-id chain reset stay active either
                                   way.  Same trailing-field ABI note as
                                   small_tier_percent above. */
  uint64_t max_object_size; /* Per-object content-size bound in bytes.  Sentinel
                               semantics, deliberately different from the C++
                               CacheConfig::max_object_size (where 0
                               disables):
                                 0          = the library default
                                              (64 MB) — NOT "disabled",
                                              so a zero-initialised config
                                              lands on the safe bounded
                                              setting;
                                 UINT64_MAX = the bound is disabled
                                              (maps to C++ 0);
                                 any other N = reject writes whose content
                                              exceeds N bytes with
                                              CYCLONE_OBJECT_TOO_LARGE; a
                                              write at exactly N bytes
                                              succeeds.
                               Note the asymmetry with
                               ram_cache_size_bytes above, which has no
                               sentinel mapping at all: there 0 means
                               DISABLED, here 0 means the default.
                               Same trailing-field ABI note as
                               small_tier_percent above. */
  int enable_cross_process_ram_coherence;
  /* Non-zero: validate every RAM-cache hit against the shared directory's
     bucket version, so a peer process's re-record or purge is not served
     from this process's RAM tier.  Zero-initialised = OFF = the
     the historical behaviour, unchanged -- positive logic, because the C++
     default is already the safe one.

     Set it PER PROCESS, independently: it is a local statement ("validate
     my own RAM hits"), never a duty owed to peers.  No peer needs the same
     setting, and no peer needs to be restarted or upgraded first -- every
     directory mutation already publishes the signal this reads.

     Costs one shared-memory load per RAM hit.  Inert unless
     enable_mmap_directory is also set AND ram_cache_size_bytes > 0; call
     cyclone_cache_cross_process_ram_coherence_active() after create to
     confirm it took effect, and watch ram_coherence_rejections against
     ram_cache_hits in CycloneCacheStats to price it -- the signal is per
     BUCKET, so unrelated writes to the same bucket also invalidate.

     Same trailing-field ABI note as small_tier_percent above. */
};

/* --------------------------------------------------------------------------
 * Statistics
 * -------------------------------------------------------------------------- */

using CycloneCacheStats = struct {
  uint64_t ram_cache_hits;
  uint64_t ram_cache_misses;
  uint64_t disk_cache_hits;
  uint64_t disk_cache_misses;
  uint64_t bytes_read;
  uint64_t bytes_written;
  uint64_t evictions;
  uint64_t current_size;
  uint64_t current_entries;

  /* Wrap-cadence telemetry (append-only extension; see CacheStats in
   * cache.hpp for full semantics). Intervals are steady-clock ns, 0 until
   * a volume has wrapped at least twice; in multi-process mode the
   * interval fields are per-process while write_buffer_wraps and
   * last_wrap_age_ns are shared. Appending here is safe only because
   * embedders compile against vendored headers in lockstep with this
   * library; a separately-compiled old caller passing its old, smaller
   * struct would be unsafe (the struct is caller-allocated and
   * cyclone_cache_stats() writes all fields). */
  uint64_t write_buffer_wraps;
  uint64_t last_wrap_interval_ns;
  uint64_t min_wrap_interval_ns;
  uint64_t last_wrap_age_ns;

  /* Lease-based region pinning counters (append-only extension,
   * same lockstep-compilation caveat as above).  All three are
   * process-local: the writer that defers/drops/forces is the one that
   * counts. */
  uint64_t wraps_deferred_by_lease; /* Wraps deferred by a live read lease */
  uint64_t writes_dropped_by_lease; /* Fills dropped by deferred wraps */
  uint64_t wraps_forced_past_lease; /* Wraps forced past lease_wrap_ceiling */

  /* Directory entries evicted because a different key colliding on the
   * (bucket, 12-bit tag) pair had to land in a completely full bucket
   * (append-only extension, same lockstep-compilation caveat as above).
   * Process-local: the writer that evicts is the one that counts. */
  uint64_t tag_collision_evictions;

  /* Live disk-hit borrows (open read handles) across all volumes at the
   * instant the stats call ran (append-only extension, same
   * lockstep-compilation caveat as above).  A GAUGE, not a counter; in
   * multi-process mode it includes borrows held by ALL processes
   * (saturating at 255 per stripe). */
  uint64_t borrows_outstanding;

  /* Cross-process reset gate.  NOTE: a prior field (volumes_coexisting_with_
   * live_peer) was REMOVED here -- this struct is compiled in lockstep with its
   * consumers (vendored build), so rebuild ALL consumers; a stale prebuilt
   * artifact would read this gauge from the wrong offset.  GAUGE over
   * currently-open volumes:
   *   volumes_with_degraded_reset_gate: the reset gate is not in effect (the
   *     filesystem lacks working byte-range locks, so an incompatible open
   *     resets even under a live peer).  Nonzero on ANY platform => use a
   *     filesystem with working advisory locks if live-peer safety matters.
   *     The gate is implemented on Windows too, so nonzero is NOT expected
   *     there either (it was, when the gate was POSIX-only; in practice a
   *     Windows filesystem where LockFileEx genuinely fails tends to fail
   *     the blocking init lock first, so open() fails with an I/O error
   *     before the gate can degrade). */
  uint64_t volumes_with_degraded_reset_gate;

  /* Directory entries evicted because a bucket was completely full of
   * current-phase entries with no tag collision to displace -- the entry
   * nearest the wrap cursor is replaced so the write still lands (append-only
   * extension, same lockstep-compilation caveat as above). Disjoint from
   * tag_collision_evictions.  Process-local: the writer that evicts is the one
   * that counts. */
  uint64_t bucket_full_evictions;

  /* Reset provenance (append-only extension at the TAIL, same lockstep-
   * compilation caveat as above -- rebuild ALL consumers).  Both are monotonic
   * COUNTERS since process start, summed across volumes, and are NOT cleared by
   * close/reopen (unlike volumes_with_degraded_reset_gate, which is a gauge).
   *
   *   resets_under_degraded_gate: THE ALARM.  A cache reset ran on some volume
   *     while the reset gate was NOT in effect, so a live peer MAY have been
   *     wiped out from under itself.  Expected 0; investigate any nonzero value
   *     together with volumes_with_degraded_reset_gate (the fix is a filesystem
   *     with working byte-range locks).  Deliberately does NOT count a reset of
   *     a FRESHLY CREATED cache file: a brand-new inode cannot be shared with a
   *     peer, so a routine `rm cyclone.dat` + reload is not an alarm and must
   *     not read as one.
   *
   *   resets_gate_verified: the healthy upgrade path.  A reset ran with the
   *     exclusive lifetime lock proven held, i.e. no live peer had the volume
   *     open.  Together the two say how many real wipes happened and how many
   *     of them were actually protected. */
  uint64_t resets_under_degraded_gate;
  uint64_t resets_gate_verified;

  /* Alternate-chain shadow bound (append-only extension at the TAIL, same
   * lockstep-compilation caveat as above -- rebuild ALL consumers).
   * Process-local; see CacheStats in cache.hpp for the full semantics.
   * Aggregated across volumes: every counter is SUMMED unless its per-field
   * note says otherwise -- alternate_max_chain_depth is a high-water MARK
   * and is maxed, not summed.
   *
   *   alternate_shadows_unlinked: superseded same-id chain nodes spliced out
   *     by an alternate write.  Re-recording an alternate id prepends a new
   *     document; this counts the superseded copies removed from the chain,
   *     i.e. the depth bound working.
   *   alternate_splice_deferred: superseded nodes LEFT LINKED because the
   *     splice could not proceed (contention, or a wrap racing the write).
   *     The write itself always succeeded -- the mechanism is best-effort by
   *     contract.  A rising ratio against the counter above is the leading
   *     indicator that chain depth is creeping up again.
   *   alternate_chain_resets: chains reset at the traversal cap because every
   *     visible node was a superseded copy of the id being written -- the
   *     backstop that keeps a heavily-refreshed key writable.  Nonzero in
   *     steady state is an alert, not routine.
   *   alternate_max_chain_depth: HIGH-WATER MARK (a max, NOT a sum) of the
   *     physical chain depth an alternate write saw before prepending.
   *     Expect it to settle at the number of distinct alternate ids the
   *     workload stores per key.
   *   alternate_wrap_refusals: writes whose allocation wrapped the circular
   *     buffer and that therefore refused to link their new head to the
   *     pre-wrap chain, starting a fresh chain instead and orphaning the
   *     stale one.  Neither shadow counter above moves on a refusal,
   *     so without this counter a refusal is indistinguishable from an
   *     ordinary wrap. */
  uint64_t alternate_shadows_unlinked;
  uint64_t alternate_splice_deferred;
  uint64_t alternate_chain_resets;
  uint64_t alternate_max_chain_depth;
  uint64_t alternate_wrap_refusals;

  /* Cross-process RAM coherence (append-only extension at the TAIL,
   * same lockstep-compilation caveat as above -- rebuild ALL consumers).
   * Process-local, summed across volumes; see CacheStats in cache.hpp for
   * the full semantics.  Both stay 0 unless
   * enable_cross_process_ram_coherence is set AND the mmap directory is in
   * use -- which is how a C-API-only consumer tells the feature apart from a
   * no-op.
   *
   *   ram_coherence_rejections: RAM hits discarded because a process mutated
   *     the key's directory bucket since the entry was admitted.  Mind the
   *     arithmetic -- the RAM cache counts the hit BEFORE the volume rejects
   *     it:
   *       true_served_ram_hits    = ram_cache_hits - ram_coherence_rejections
   *       false_invalidation_rate = ram_coherence_rejections / ram_cache_hits
   *     A high ratio means bucket granularity is costing you; the honest
   *     responses are a smaller RAM tier, better write partitioning, or
   *     turning the knob back off.
   *   ram_coherence_put_rejections: RAM inserts declined because the bucket
   *     moved during the read that would have populated the entry.  A pure
   *     saving, but the same crowding signal. */
  uint64_t ram_coherence_rejections;
  uint64_t ram_coherence_put_rejections;
};

/* --------------------------------------------------------------------------
 * Thread Safety
 * --------------------------------------------------------------------------
 *
 * All functions are thread-safe unless noted otherwise.
 * - Multiple threads may call read/write/exists/delete concurrently.
 * - cyclone_cache_set_miss_handler() may be called while reads are in flight;
 *   the new handler takes effect for subsequent misses only.
 * - Callbacks (CycloneReadCallback, CycloneMissDoneCallback) may be invoked
 *   from any thread, including the calling thread or internal worker threads.
 * - Do not call cyclone_cache_destroy() while other operations are in progress
 *   on other threads; call cyclone_cache_drain_pending() first to ensure
 *   graceful shutdown.
 */

/* --------------------------------------------------------------------------
 * Core synchronous API
 * -------------------------------------------------------------------------- */

CycloneError cyclone_cache_create(const CycloneCacheConfig *config,
                                  CycloneCacheHandle **out);
void cyclone_cache_destroy(CycloneCacheHandle *cache);

CycloneError cyclone_cache_read(CycloneCacheHandle *cache, const char *key,
                                size_t key_len, CycloneReadHandle **out);
CycloneError cyclone_cache_read_data(CycloneReadHandle *handle,
                                     const char **data, size_t *data_len);
void cyclone_cache_read_close(CycloneReadHandle *handle);

CycloneError cyclone_cache_write(CycloneCacheHandle *cache, const char *key,
                                 size_t key_len, const char *data,
                                 size_t data_len);
CycloneError cyclone_cache_delete(CycloneCacheHandle *cache, const char *key,
                                  size_t key_len);
/*
 * Check if an entry exists in the cache.
 * Returns:
 *   CYCLONE_OK         - Entry exists
 *   CYCLONE_NOT_FOUND  - Entry does not exist (this is NOT an error condition)
 *   CYCLONE_INVALID_ARGUMENT - Invalid parameters
 */
CycloneError cyclone_cache_exists(CycloneCacheHandle *cache, const char *key,
                                  size_t key_len);
CycloneError cyclone_cache_stats(CycloneCacheHandle *cache,
                                 CycloneCacheStats *out);

/* --------------------------------------------------------------------------
 * Tier-aware synchronous API
 * --------------------------------------------------------------------------
 *
 * Additive variants of the core operations that take an explicit storage
 * tier.  Passing CYCLONE_TIER_DEFAULT behaves exactly like the tier-less
 * function of the same name; the tier-less functions are unchanged.
 * cyclone_cache_read_async() always operates on the default tier.
 */

CycloneError cyclone_cache_read_tier(CycloneCacheHandle *cache, const char *key,
                                     size_t key_len, CycloneTier tier,
                                     CycloneReadHandle **out);
CycloneError cyclone_cache_write_tier(CycloneCacheHandle *cache,
                                      const char *key, size_t key_len,
                                      const char *data, size_t data_len,
                                      CycloneTier tier);
CycloneError cyclone_cache_delete_tier(CycloneCacheHandle *cache,
                                       const char *key, size_t key_len,
                                       CycloneTier tier);
/* Same return convention as cyclone_cache_exists(). */
CycloneError cyclone_cache_exists_tier(CycloneCacheHandle *cache,
                                       const char *key, size_t key_len,
                                       CycloneTier tier);

/*
 * Returns non-zero when the small-object tier is enabled (a dedicated small
 * volume exists), 0 otherwise — including when small_tier_percent was set
 * but cache_size_bytes was below the sizing floor and the tier was silently
 * disabled.  Callers that require the tier should check this after
 * cyclone_cache_create().  Returns 0 for a NULL cache.
 */
int cyclone_cache_small_tier_active(CycloneCacheHandle *cache);

/*
 * Returns non-zero when cross-process RAM-cache coherence is actually
 * in effect: enable_cross_process_ram_coherence was set AND
 * enable_mmap_directory was set AND ram_cache_size_bytes > 0.  Enabling the
 * knob without the other two is silently inert -- there is either nothing to
 * validate or no shared signal to validate against -- so callers that require
 * the guarantee should check this after cyclone_cache_create() rather than
 * trust the config field.  Returns 0 for a NULL cache.
 */
int cyclone_cache_cross_process_ram_coherence_active(CycloneCacheHandle *cache);

/* --------------------------------------------------------------------------
 * Miss callback hook (async read with request coalescing)
 * -------------------------------------------------------------------------- */

/*
 * Completion callback: the miss handler calls this when it has fetched the
 * data (or failed). The cache then stores the result and notifies all
 * coalesced waiters.
 *   user_data:  opaque pointer passed through from the internal context
 *   data/data_len: fetched value (NULL/0 on failure)
 *   err:        CYCLONE_OK on success, error code on failure
 */
using CycloneMissDoneCallback = void (*)(void *, const char *, size_t,
                                         CycloneError);

/*
 * Miss handler: called by the cache when a read misses and no fetch is
 * already in flight for this key. The handler should initiate an async
 * fetch and call done_cb(done_user_data, ...) when complete.
 *   key/key_len:        the cache key that missed
 *   handler_user_data:  opaque pointer registered with set_miss_handler
 *   done_cb:            completion callback the handler MUST call exactly once
 *   done_user_data:     opaque pointer to pass back to done_cb
 */
using CycloneMissHandler = void (*)(const char *, size_t, void *,
                                    CycloneMissDoneCallback, void *);

/*
 * Register a miss handler. When set, cyclone_cache_read_async() will invoke
 * this handler on cache miss instead of returning CYCLONE_NOT_FOUND.
 * Pass NULL handler to clear.
 */
CycloneError cyclone_cache_set_miss_handler(CycloneCacheHandle *cache,
                                            CycloneMissHandler handler,
                                            void *handler_user_data);

/*
 * Async read callback: called exactly once with the result.
 *   user_data:  opaque pointer passed to cyclone_cache_read_async
 *   data/data_len: the cached (or fetched) value (NULL/0 on error)
 *   err:        CYCLONE_OK on success, error code on failure
 *
 * IMPORTANT: The data pointer is only valid for the duration of this callback.
 * If you need to retain the data, copy it before returning from the callback.
 */
using CycloneReadCallback = void (*)(void *, const char *, size_t,
                                     CycloneError);

/*
 * Async read that uses the miss handler on cache miss.
 * - Cache hit: invokes read_cb immediately with the data.
 * - Cache miss + miss handler set:
 *     - No fetch in flight for this key: invokes miss handler
 *     - Fetch already in flight: coalesces (waits for in-flight fetch)
 *     - When fetch completes: stores result, invokes read_cb for ALL waiters
 * - Cache miss + no miss handler: invokes read_cb with CYCLONE_NOT_FOUND.
 * - Cache shutting down: invokes read_cb with CYCLONE_NOT_INITIALIZED.
 */
CycloneError cyclone_cache_read_async(CycloneCacheHandle *cache,
                                      const char *key, size_t key_len,
                                      CycloneReadCallback read_cb,
                                      void *read_user_data);

/*
 * Drain all in-flight miss handler invocations. Blocks until all pending
 * fetches complete or timeout_ms elapses. After this call, new read_async
 * calls return CYCLONE_NOT_INITIALIZED.
 * Called automatically by cyclone_cache_destroy().
 */
CycloneError cyclone_cache_drain_pending(CycloneCacheHandle *cache,
                                         uint32_t timeout_ms);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CYCLONE_C_H */
