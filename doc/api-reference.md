# Cyclone Cache API Reference

This document provides detailed API documentation for Cyclone Cache.

## Table of Contents

- [Core Classes](#core-classes)
  - [Cache](#cache)
  - [CacheKey](#cachekey)
  - [ReadHandle](#readhandle)
  - [WriteHandle](#writehandle)
- [Configuration](#configuration)
  - [CacheConfig](#cacheconfig)
  - [VolumeConfig](#volumeconfig)
- [Plugin System](#plugin-system)
  - [CachePlugin](#cacheplugin)
  - [PluginManager](#pluginmanager)
  - [Metadata](#metadata)
- [HTTP Plugin](#http-plugin)
  - [HttpCacheAlt](#httpcachealt)
  - [HttpAlternatePlugin](#httpalternateplugin)
- [C API](#c-api)
  - [Error Codes](#error-codes)
  - [Core Functions](#core-functions)
  - [Miss Callback Hook](#miss-callback-hook)
- [Error Handling](#error-handling)
- [Async Operations](#async-operations)

---

## Core Classes

### Cache

The main cache interface for all storage operations.

#### Creation

```cpp
static std::expected<std::unique_ptr<Cache>, CacheError>
Cache::create(const CacheConfig& config);
```

Creates a new cache instance with the specified configuration.

**Parameters:**
- `config`: Cache configuration options

**Returns:**
- `std::unique_ptr<Cache>` on success
- `CacheError` on failure

**Example:**
```cpp
CacheConfig config;
config.ram_cache_size = 128 * 1024 * 1024;  // 128MB RAM cache
auto result = Cache::create(config);
if (result) {
    auto& cache = *result;
    // Use cache...
}
```

#### Volume Management

```cpp
std::expected<void, CacheError> add_volume(const VolumeConfig& config);
std::expected<void, CacheError> add_volume(const std::string& path, size_t size);
```

Adds a storage volume to the cache.

**Parameters:**
- `config`: Volume configuration, or
- `path`: Path to cache file
- `size`: Size in bytes (0 = use file size)

**Returns:**
- `void` on success
- `CacheError::IoError` if file cannot be opened
- `CacheError::InvalidArgument` if size is invalid

```cpp
std::expected<void, CacheError> start();
```

Starts the cache, initializing all volumes and enabling operations.

```cpp
void stop();
```

Stops the cache, flushing pending writes and closing volumes.

```cpp
bool is_running() const;
```

Returns `true` if the cache is started and ready for operations.

#### Synchronous Operations

```cpp
std::expected<ReadHandle, CacheError> read_sync(const CacheKey& key);
```

Reads an entry from the cache.

**Returns:**
- `ReadHandle` on cache hit
- `CacheError::NotFound` on cache miss
- `CacheError::Busy` if a writer held the key's directory bucket for the
  whole seqlock wait budget (5 ms), so the read could not tell whether the
  key is present. This is not a miss; retry, or treat it as a miss if a
  refetch is acceptable. See [Busy lookups](#busy-lookups). The same applies
  to `exists_sync`, `read_alternate_sync` and `list_alternates_sync`.
- `CacheError::NotInitialized` if cache not started

```cpp
std::expected<WriteHandle, CacheError> write_sync(const CacheKey& key);
std::expected<WriteHandle, CacheError> write_sync(const CacheKey& key, uint64_t content_length);
```

Opens a write handle for storing content.

**Parameters:**
- `key`: Cache key for the entry
- `content_length`: Expected content size (optional, used for space reservation)

```cpp
std::expected<void, CacheError> remove_sync(const CacheKey& key);
```

Removes an entry from the cache.

**Returns:**
- `void` on success
- `CacheError::NotFound` if entry doesn't exist

```cpp
std::expected<bool, CacheError> exists_sync(const CacheKey& key);
```

Checks if an entry exists without reading it.

**Returns:**
- `true` if entry exists
- `false` if entry doesn't exist

#### Statistics

```cpp
CacheStats stats() const;
```

Returns current cache statistics.

```cpp
struct CacheStats {
    uint64_t ram_cache_hits;
    uint64_t ram_cache_misses;
    uint64_t disk_cache_hits;
    uint64_t disk_cache_misses;
    uint64_t bytes_read;
    uint64_t bytes_written;
    uint64_t current_entries;
    uint64_t current_bytes;
    uint64_t ram_cache_bytes;
    uint64_t stripe_count;           // Stripes summed across disk volumes
    uint64_t stripe_bytes;           // Their summed sizes
    uint64_t evictions;

    // HitTracker flush diagnostics
    uint64_t hit_flush_successes;
    uint64_t hit_flush_failures;
    uint64_t hit_flush_total_delta;     // Sum of all flushed hit deltas
    uint64_t hit_flush_fsyncs;          // fsyncs after flush cycles
    uint64_t hit_flush_lock_contended;  // Deltas dropped on lock contention

    // Durability (see below)
    uint64_t directory_syncs;  // Completed periodic directory syncs
    uint64_t fsyncs;           // Every raw fd fsync, all volumes, all causes

    // Wrap-cadence telemetry (aggregated across volumes)
    uint64_t write_buffer_wraps;     // Circular write-buffer wraps since open
    uint64_t last_wrap_interval_ns;  // ns between the last two wraps
    uint64_t min_wrap_interval_ns;   // Smallest observed wrap interval
    uint64_t last_wrap_age_ns;       // ns since the most recent wrap

    // Lease-based region pinning (all three are process-local)
    uint64_t wraps_deferred_by_lease;  // Wraps deferred by a live read lease
    uint64_t writes_dropped_by_lease;  // Fills dropped by deferred wraps
    uint64_t wraps_forced_past_lease;  // Wraps forced past lease_wrap_ceiling
    uint64_t borrows_outstanding;      // GAUGE: open disk-hit borrows

    // Directory-entry evictions that are not wraps (process-local; see below)
    uint64_t tag_collision_evictions;  // Full bucket, colliding tag replaced
    uint64_t bucket_full_evictions;    // Full bucket, no collision to displace

    // Cross-process reset gate (see below)
    uint64_t volumes_with_degraded_reset_gate;  // GAUGE; expected 0
    uint64_t resets_under_degraded_gate;        // THE ALARM; expected 0
    uint64_t resets_gate_verified;              // Healthy upgrade resets

    // Alternate-chain depth bound (process-local; see below)
    uint64_t alternate_shadows_unlinked;   // Superseded nodes spliced out
    uint64_t alternate_splice_deferred;    // Superseded nodes left linked
    uint64_t alternate_chain_resets;       // Chains reset at the traversal cap
    uint64_t alternate_max_chain_depth;    // HIGH-WATER MARK, not a sum
    uint64_t alternate_wrap_refusals;      // Wrap-frontier link refusals

    // Cross-process RAM coherence (process-local; see below).  Both stay 0
    // unless cross_process_ram_coherence is on AND the mmap directory is in
    // use, which is how you tell the feature apart from a no-op.
    uint64_t ram_coherence_rejections;      // RAM hits dropped as stale
    uint64_t ram_coherence_put_rejections;  // RAM inserts declined

    // Large-document readahead (process-local; C++ only, see below)
    uint64_t readahead_hints_issued;  // Readahead hints past the re-advise filter

    // Wrap retention (process-local; all 0 in flush mode; see
    // "Wrap Retention" below)
    uint64_t frontier_advances;           // Gated frontier moves published
    uint64_t advances_deferred_by_lease;  // Mandatory advances deferred
    uint64_t early_advances_skipped;      // Optional runway advances skipped
    uint64_t retained_hits;               // Hits served from the previous pass
    uint64_t stamp_rejections;            // Stale pass stamps rejected

    // Alternate carry-forward (process-local; all 0 in flush mode; see
    // "Wrap Retention" below)
    uint64_t alternates_carried_forward;  // Retained alternates rewritten
    uint64_t alternate_carry_bytes;       // Bytes those rewrites cost
    uint64_t alternates_carry_dropped;    // Retained alternates not kept

    // Lookups that returned Busy: a writer held the key's directory bucket
    // for the whole seqlock wait budget (process-local; expected 0; see
    // "Busy lookups" below)
    uint64_t directory_read_timeouts;
};
```

`ram_cache_hits` counts a hit **before** the volume decides whether to serve
it, so under cross-process RAM coherence the two counters combine as:

```
true_served_ram_hits    = ram_cache_hits - ram_coherence_rejections
false_invalidation_rate = ram_coherence_rejections / ram_cache_hits
```

The wrap interval fields are steady-clock nanoseconds (deliberately `_ns`;
older stats use `_ms`) and remain 0 (undefined) until a volume has wrapped
at least twice; `last_wrap_age_ns` is only meaningful when
`write_buffer_wraps > 0`. Caveats:

- Wraps happen per stripe but are counted per volume: on a multi-stripe
  volume the intervals measure inter-wrap spacing across the whole volume,
  not a single write buffer's wrap period. `write_buffer_wraps` is the
  primary signal.
- In multi-process (mmap-directory) mode, `write_buffer_wraps` and
  `last_wrap_age_ns` are shared across processes (a read-only process sees
  wraps performed by writers); the interval fields are per-process and only
  cover wraps performed by the calling process.
- `last_wrap_interval_ns` can be 0 while `min_wrap_interval_ns > 0` when
  the most recently wrapped volume has wrapped exactly once, and
  `min <= last` is not guaranteed when polled concurrently with a wrap.

The lease counters observe the read-lease protocol (see
`CacheConfig::read_lease_duration` / `lease_wrap_ceiling`): every disk-hit
read stamps a per-stripe lease; a writer needing to wrap the circular
write buffer defers the wrap while an unexpired lease exists
(`wraps_deferred_by_lease`) and drops the fill (`writes_dropped_by_lease`,
best-effort semantics), until the wrap has been continuously deferred
longer than the ceiling, after which it proceeds anyway
(`wraps_forced_past_lease` — borrows held past the ceiling are not
protected).  All three counters are process-local even in multi-process
mode: the writer that defers/drops/forces is the one that counts.

The alternate counters observe the chain-depth bound (see "Alternate-Chain
Depth Bound" below).  In a healthy deployment `alternate_shadows_unlinked`
grows with refresh traffic, `alternate_max_chain_depth` settles at the number
of distinct alternate ids stored per key, and the other two stay at zero.  A
rising `alternate_splice_deferred` means superseded nodes are being left
linked (the write still succeeded — the splice is best-effort), and a nonzero
`alternate_chain_resets` in steady state means depth reached the traversal cap
and the backstop fired.  `alternate_wrap_refusals` counts writes whose
allocation wrapped the circular buffer and which therefore started a fresh
chain rather than linking to the pre-wrap one — expected on any cache that
wraps while alternates are being re-recorded, and otherwise indistinguishable
from an ordinary wrap.  In practice it moves in flush mode only: with wrap
retention the pre-wrap chain is retained rather than lost, and the write
carries it forward instead (see the carry-forward counters below).

`tag_collision_evictions` and `bucket_full_evictions` count directory
entries a write had to evict because the target bucket (4 entries) was
full. The first counts the case where a *different* key with the same 12-bit
tag already sat in the full bucket: that entry is replaced rather than
updated in place, since only a full-key match may update in place. The
second counts a full bucket of current-phase entries with no collision to
displace: the entry nearest the wrap cursor is replaced so the write still
lands. The two are disjoint, and both are process-local (the writer that
evicts is the one that counts). Neither is counted in `evictions`, which
counts wraps (one per phase flip). A steadily rising value suggests too
few directory entries for the object count; see `VolumeStats` in
`src/core/volume.hpp`.

`fsyncs` counts every raw `fsync` on the volume files: per-write syncs when
`VolumeConfig::sync_on_write` is set, the periodic `DirectorySyncer`
(`directory_syncs` counts its completed cycles), HitTracker flushes
(`hit_flush_fsyncs`), and syncs at initialisation and removal. With the
default `sync_on_write = false` it should grow at about the sync interval,
not with the write rate; a rate that follows writes is an fsync convoy.

`borrows_outstanding` is a gauge of open disk-hit read handles (in
multi-process mode it includes every process's borrows, saturating at 255
per stripe). Nonzero while the cache is full means fills that need a wrap
are being deferred for those readers. The reset-gate fields report whether
an incompatible open could reset a volume under a live peer:
`volumes_with_degraded_reset_gate` is nonzero only on a filesystem without
working byte-range locks, and `resets_under_degraded_gate` counts resets
that ran without the gate and should stay 0.

`readahead_hints_issued` counts the large-document readahead hints the disk
read path actually issued (see ["Large-Document
Readahead"](#large-document-readahead) below), summed across volumes. It
counts hints that got past the per-placement re-advise filter, so it measures
hints issued, not large reads served: a hot document contributes at most one
hint per re-advise interval (2 s) however often it is read, and a document
below `readahead_min_bytes` never contributes. On Linux a counted hint whose
range is already fully resident stops at a `mincore()` check. Process-local.

`cold_readahead_hints`, `sequential_readahead_hints` and
`recent_write_hint_skips` (appended at the tail of `CacheStats`, C++ only)
count the hints issued on reads that ran the CRC pass, and the reads that
skipped one as recently written (see ["Cold and Sequential Readahead"](#cold-and-sequential-readahead)
below): a hint over one document below `readahead_min_bytes`, and a hint
that extended past the document because the read continued a sequential run
of its stripe. Process-local.

The wrap-retention counters observe the eviction mode (see ["Wrap
Retention"](#wrap-retention) below). `retained_hits` is the direct measure of
what retention buys. `advances_deferred_by_lease` is the retention
counterpart of `wraps_deferred_by_lease`: a borrow in a chunk the frontier
must cross defers that advance and drops the fill (also counted in
`writes_dropped_by_lease`; a ceiling-forced advance counts in
`wraps_forced_past_lease`). `early_advances_skipped` never drops a fill.
`stamp_rejections` counts candidates whose pass stamp contradicted their
class: stale survivors, or a lost timeline after a power loss. All five stay
0 in flush mode and are process-local.

The carry-forward counters observe alternate writes over a retained chain
(see ["Wrap Retention"](#wrap-retention)). `alternates_carried_forward`
counts the alternates such a write rewrote into the current pass.
`alternate_carry_bytes` counts their on-disk bytes: the write amplification
of the carry. `alternates_carry_dropped` counts the visible retained
alternates a carry did not keep, because they were over its count or byte
cap or unreadable when copied. It is the one way a retained alternate is
still lost on a write. All three stay 0 in flush mode and are process-local.

`CycloneCacheStats` (C API) carries the core counters, the wrap and lease
counters, `tag_collision_evictions`, `borrows_outstanding`, the reset-gate
fields, `bucket_full_evictions`, the alternate counters, the RAM-coherence
counters, the five wrap-retention counters and the three carry-forward
counters, in that append-only order. The
stripe geometry, HitTracker flush,
`directory_syncs`, `fsyncs` and `readahead_hints_issued` fields are C++-only.

```cpp
void reset_stats();
```

Resets all statistics counters to zero.

#### Capacity

```cpp
size_t volume_count() const;
uint64_t total_capacity() const;
uint64_t bytes_used() const;
bool small_tier_active() const;  // See "Small-Object Tier" under Configuration
```

`total_capacity()` and `bytes_used()` include the small-tier carve-out when
the small-object tier is enabled.

---

### CacheKey

Represents a cache key using SHA-256 hashing.

#### Construction

```cpp
explicit CacheKey(std::string_view s);
```

Creates a key by hashing the given string.

```cpp
explicit CacheKey(std::span<const std::byte> data);
```

Creates a key by hashing raw bytes.

```cpp
static CacheKey from_url(std::string_view url, std::string_view hostname = {});
```

Creates a key from a URL, optionally including hostname for better distribution.

**Example:**
```cpp
// Simple string key
CacheKey key1("my-cache-key");

// URL-based key
CacheKey key2 = CacheKey::from_url("http://example.com/page.html");

// URL with explicit hostname
CacheKey key3 = CacheKey::from_url("/page.html", "example.com");
```

#### Hash Functions

```cpp
uint32_t segment_hash() const;
```

Returns hash for stripe/segment selection.

```cpp
uint32_t bucket_hash() const;
```

Returns hash for bucket selection within a segment.

```cpp
uint16_t tag() const;
```

Returns 12-bit tag for collision detection.

#### Comparison

```cpp
bool operator==(const CacheKey& other) const;
bool operator!=(const CacheKey& other) const;
bool operator<(const CacheKey& other) const;
```

Keys are comparable and can be used in ordered containers.

#### Utilities

```cpp
std::string to_hex() const;
static std::optional<CacheKey> from_hex(std::string_view hex);
bool is_zero() const;
```

---

### ReadHandle

Handle for reading cached content.

> **Lifetime**: A ReadHandle is safe to hold across (and destroy after)
> `Cache::stop()`. Disk-hit handles are pinned by per-thread read anchors that
> hold strong references to the Volume and its mapping; teardown latches the
> anchors' `torn` flag before freeing stripes, and handles gate their
> stripe-touching paths on it. Best practice is still to release handles
> promptly — each one pins mapped memory (and, for disk hits, a borrow that
> defers writer wraps). See
> [multi-process.md](multi-process.md#readhandle-lifetime).

```cpp
std::span<const std::byte> header() const;
```

Returns the metadata header stored with the entry.

```cpp
std::span<const std::byte> content() const;
```

Returns the cached content data.

```cpp
uint64_t content_length() const;
```

Returns the total content length.

```cpp
bool is_ram_cache_hit() const;
```

Returns `true` if content was served from RAM cache.

```cpp
std::optional<std::span<const std::byte>> mapped_view() const;
```

Returns direct memory-mapped view if available (zero-copy access).

```cpp
bool renew_lease();
```

Re-stamps the read lease pinning this handle's stripe.  A disk-hit
handle's borrowed bytes are protected from in-place overwrite by a
write-buffer wrap while the stripe lease holds; the lease is stamped for
`CacheConfig::read_lease_duration` (T) at read time.  Holders that keep the
borrow longer (e.g. client-paced transfers) must call `renew_lease()` at a
cadence of at most 3T/4 and keep the total hold below
`CacheConfig::lease_wrap_ceiling`, or copy the bytes.  Returns `false` when
there is no lease to renew (RAM-cache hit, leases disabled, invalid
handle).

**Example:**
```cpp
auto result = cache->read_sync(key);
if (result) {
    auto& handle = *result;

    // Access content
    auto content = handle.content();

    // Check if RAM cache hit
    if (handle.is_ram_cache_hit()) {
        // Fast path - data in memory
    }

    // Try zero-copy access
    if (auto view = handle.mapped_view()) {
        // Direct access to mmap'd region
    }
}
```

---

### WriteHandle

Handle for writing content to cache.

```cpp
void set_header(std::span<const std::byte> header);
```

Sets the metadata header to store with the entry.

```cpp
void set_content_length(uint64_t length);
```

Sets the expected content length.

```cpp
std::expected<size_t, CacheError> write_sync(std::span<const std::byte> data);
```

Writes content data. Can be called multiple times for streaming writes.

**Returns:**
- Number of bytes written on success
- `CacheError::Closed` if handle was closed or aborted

```cpp
std::expected<std::span<std::byte>, CacheError> reserve(size_t length);
```

Appends `length` bytes to the object and returns them for the caller to
fill in place: the zero-copy form of `write_sync()`, for a producer that
generates the content itself (a KV engine staging tensors, a rewriter
emitting output). `write_sync()` copies the caller's buffer into the
handle; with `reserve()` the content is produced in the handle's buffer
and that copy never happens. It mixes freely with `write_sync()`, in call
order, and the bytes count toward `bytes_written()` as soon as it returns.

- The span stays valid until the next `write_sync()`, `reserve()`, close or
  abort on the handle.
- Every write into the span must be complete before `close_sync()` is
  called. No fill may still be in flight: not an async DMA, not io_uring,
  not another thread. Close computes the checksum from the span first and
  writes the bytes to the file afterwards, possibly after waiting on a wrap
  or a reader lease. A byte that changes after close begins is stored under
  a checksum that does not match it, and reads back as
  `CacheError::Corrupted` or torn.
- **Security:** the reserved bytes are not initialized. They hold whatever
  the heap held before, which can be other data of the same process. Fill
  every byte: an unfilled byte is persisted, and served to readers, as that
  old heap content.
- Nothing is visible to readers before the close commits. An abort, or
  destroying the handle unclosed, after a partial fill discards everything.
- Same limits as `write_sync()`: `CacheError::ObjectTooLarge` past
  `max_object_size`, `CacheError::NoSpace` past the 4 GiB document limit,
  `CacheError::Closed` on a closed or aborted handle. A refused `reserve()`
  appends nothing.

Not exposed in the C API, which has no streaming write handle.

```cpp
std::expected<void, CacheError> close_sync();
```

Closes the handle and commits the write to disk.

```cpp
void abort();
```

Aborts the write, discarding any data written.

```cpp
size_t bytes_written() const;
```

Returns total bytes written so far.

**Example:**
```cpp
auto result = cache->write_sync(key, content.size());
if (result) {
    auto& handle = *result;

    // Set optional header
    handle.set_header(std::span<const std::byte>(header_data));

    // Write content (can be chunked)
    handle.write_sync(std::span<const std::byte>(chunk1));
    handle.write_sync(std::span<const std::byte>(chunk2));

    // Commit
    auto close_result = handle.close_sync();
    if (!close_result) {
        // Handle write error
    }
}
```

Generating the content in place with `reserve()`:
```cpp
auto handle = cache->write_sync(key, block_size);
if (handle) {
    auto dst = handle->reserve(block_size);
    if (dst) {
        produce_block(*dst);  // writes all block_size bytes
        auto committed = handle->close_sync();
    }
}
```

---

## Configuration

### CacheConfig

```cpp
struct CacheConfig {
    // RAM cache size in bytes (default: 256MB).  0 disables the RAM tier
    // outright.  See "Zero Values" below.
    size_t ram_cache_size = 256 * 1024 * 1024;

    // RAM cache algorithm (default: CLFUS)
    RamCacheType ram_cache_type = RamCacheType::CLFUS;

    // Per-object content-size bound in bytes (default: 64MB).  0 disables
    // the bound.  See "Zero Values" below.
    size_t max_object_size = 64 * 1024 * 1024;

    // Store a CRC-32C with each document (default: true).  The C API copies
    // this verbatim, so C 0 disables checksums outright.
    // See "Zero Values" below.
    bool enable_checksum = true;

    // Small-object tier carve-out percentage (default: 0 = disabled,
    // clamped to [1, 50]).  See "Small-Object Tier" below.
    uint32_t small_tier_percent = 0;

    // Kill switch for the alternate-chain depth bound (default: true = on).
    // See "Alternate-Chain Depth Bound" below.
    bool unlink_superseded_alternates = true;

    // Eviction mode (default: true = retention; false = flush).  Persisted
    // per volume at creation.  See "Wrap Retention" below.
    bool wrap_retention = true;

    // Validate RAM-cache hits against the shared directory's bucket version
    // (default: false = off).  See "Cross-Process RAM Coherence" below.
    bool cross_process_ram_coherence = false;

    // Readahead hint for large documents on the disk read path (default:
    // 256 KiB; 0 = off).  Fluent setter: set_readahead_min_bytes().
    // See "Large-Document Readahead" below.
    size_t readahead_min_bytes = 256 * 1024;

    // Cold-read readahead for documents of at least this size, on reads
    // that run the CRC pass (default: 16 KiB; 0 = off, window included),
    // and the sequential window past the document (default: 1 MiB;
    // 0 = off).  Fluent setters: set_cold_readahead_min_bytes(),
    // set_sequential_readahead_bytes().  See "Cold and Sequential
    // Readahead" below.
    // Both run only on checksum-verifying reads: with
    // verify_checksum_on_read = false, small cold reads stay unhinted.
    size_t cold_readahead_min_bytes = 16 * 1024;
    size_t sequential_readahead_bytes = 1024 * 1024;
};

enum class RamCacheType {
    LRU,    // Simple LRU eviction
    CLFUS   // Scan-resistant Clock-LRU-Frequency-Size
};

// Storage tier for key-routed operations (see "Small-Object Tier" below).
enum class Tier : uint8_t {
    kDefault = 0,  // Hash-routed default (payload) volume(s)
    kSmall = 1,    // Dedicated small-object volume
};
```

**Removed field.** `CacheConfig::min_object_size` (default 128) was declared
but never read anywhere in the library, and was removed. No behavior
change.

### Zero Values

The two size knobs read a zero differently, the C API reads it differently
again from C++, and one integrity knob drops its C++ default at the C
boundary. The asymmetry is deliberate; it is stated here rather than left
to be discovered at runtime.

**RAM tier size.**

- C++ `CacheConfig::ram_cache_size` defaults to 256 MB. Setting it to `0`
  disables the RAM tier: no in-memory cache is built and the cache runs
  disk-only.
- C `CycloneCacheConfig::ram_cache_size_bytes` is copied to that field
  verbatim. There is **no default and no sentinel mapping**, so `0` carries
  straight through and means **disabled**. A zero-initialised
  `CycloneCacheConfig` therefore gets no RAM tier — not the 256 MB C++
  default. Pass an explicit size to get one.

**Per-object size bound.**

- C++ `CacheConfig::max_object_size` defaults to 64 MB. A write whose content
  exceeds it fails with `CacheError::ObjectTooLarge`; a write at exactly the
  bound succeeds. Setting it to `0` **disables** the bound.
- C `CycloneCacheConfig::max_object_size` maps zero the other way round
   `0` = the library default (64 MB), `UINT64_MAX` = the bound
  is disabled (C++ `0`), any other `N` = a bound of `N` bytes. That way a
  zero-initialised config lands on the bounded setting rather than an
  unbounded one.

**Checksums.**

- C++ `CacheConfig::enable_checksum` defaults to `true`: every document
  stored carries a CRC-32C, verified on read. (A stored CRC of `0` skips
  verification, so documents written with checksums off still read back.)
- C `CycloneCacheConfig::enable_checksum` is copied verbatim (`!= 0`). There
  is **no default and no sentinel mapping**, so `0` means **disabled** — the
  opposite of the C++ default. A zero-initialised `CycloneCacheConfig`
  therefore runs with **no integrity checking**: corruption is served rather
  than detected. Set it to `1` explicitly for the C++ behaviour.
  (Multi-process mode — `enable_mmap_directory` — refuses to start with
  checksums off either way, so that combination fails loudly instead.)

So a zero-initialised `CycloneCacheConfig` runs with **no RAM tier**, **a
64 MB per-object bound**, and **checksums disabled**. All are working
configurations, but they get there by opposite conventions: `max_object_size`
treats `0` as "unset, use the default", while `ram_cache_size_bytes` and
`enable_checksum` treat it as the literal value it says.

`ram_cache_size_bytes = 0` keeps meaning "disabled". For a size knob, a
zero-byte cache is the natural reading of zero, and giving it the
`max_object_size` sentinel treatment would silently change the behaviour of
every caller that passes `0` today — a caller that cannot currently distinguish
"default" from "disabled". The asymmetry is documented rather than papered
over.

`enable_checksum = 0` keeps meaning "disabled" for the same reason, and then
some: remapping `0` to the C++ `true` would silently switch integrity checking
on for every caller that passes `0` today and, without a new opt-out sentinel,
remove the only way a C caller can turn checksums off. For an integrity knob a
silent change in either direction is worse than a documented divergence — so
documented it is.

### Alternate-Chain Depth Bound

Writing an alternate id that a key already carries prepends a new document.
The write also splices the superseded copy out of the chain, so the physical
chain depth stays bounded by the number of **distinct** alternate ids on the
key rather than growing with every refresh. Without that bound, a key
refreshed often enough (a TTL-driven re-record, for example) eventually
exceeds the chain traversal limit and becomes permanently unwritable.

Two guards ship with it and are **always active**, independent of the knob
below:

- A write whose allocation wraps the circular buffer refuses to link its new
  head to the pre-wrap chain, and starts a fresh chain instead. Linking across
  a wrap frontier can produce a chain that walks into recycled bytes.
- A chain that has already reached the traversal cap, and whose every visible
  node is a superseded copy of the id being written, is reset rather than
  refused — so a key can never stay wedged. A cap-length chain carrying
  **several** ids is still refused (`TooManyAlternates`): resetting it would
  drop alternates the caller never asked to lose.

`CacheConfig::unlink_superseded_alternates` (C API:
`CycloneCacheConfig::disable_alternate_unlink`, stated in the negative so a
zero-initialised config lands on the safe setting) is a **kill switch**, not a
tuning knob: it disables the splice for a deployment that needs to back the
mechanism out without a rebuild, at the cost of restoring unbounded chain
growth.

The splice is best-effort. If it cannot proceed — contention on the
cross-process write lock, or a wrap racing the write — the caller's write
still succeeds, the superseded node stays linked, and
`alternate_splice_deferred` counts it.

**Upgrade note.** This bound shipped with an on-disk format major bump, so a
cache written by an older binary is not reused: the new binary resolves to a
different volume filename and starts on a **cold cache**. The old file is left
on disk. Reclaim it with `CacheConfig::gc_superseded_on_start` (POSIX only,
and only when the cache directory is owned exclusively by this cache) or by
deleting it manually. On a cache directory sized for exactly one volume,
delete the old file **before** starting the new binary — the new volume is
extended to its full configured size during `start()`, ahead of any GC.

### Wrap Retention

Each stripe's data area is a circular log.  What happens to the previous pass
when the write cursor wraps is the eviction policy, chosen by
`CacheConfig::wrap_retention` (C API: `disable_wrap_retention`, below):

- **Retention (`true`, the default).**  The previous pass stays readable
  until its bytes are about to be overwritten.  A clean frontier runs ahead
  of the write cursor in fixed chunks (`N <= 64` of at least 1 MiB each, per
  stripe); moving it is the only step that hands readable bytes to the
  forward fill, and it waits for live borrows in exactly the chunks it is
  about to expose.  A borrow's bytes therefore stay intact until the
  frontier crosses its own chunk.  Each document is stamped with its pass, so a stale entry from two
  or more passes back can never resolve.
- **Flush (`false`, the opt-out).**  The wrap flips the stripe's phase bit
  and every entry of the pass that just ended stops resolving at once,
  although nearly all of those documents are still intact on disk.  A stripe
  therefore holds, on average, about half of its capacity.  Choose it to keep
  a volume created by an earlier default-off build warm (see below), or to
  keep the old eviction behaviour.

In the policy replay of the KV-churn workload (`benchmarks/kv_churn_policy`)
retention recovers 93-98 % of the gap between flush and a plain per-stripe
FIFO; measured on Linux (2 MiB blocks, 4 GiB tier, Zipf, 4 threads) the hit
ratio went from 0.726 to 0.788, matching the replay within 0.001.  See
`doc/design/wrap-retention.md` for the mechanism and the correctness argument.

Rules:

- The mode is **persisted in the volume header when the volume is created**.
  An open whose configured mode disagrees with the file goes through the same
  live-peer gate as a format change: refused with `ResetRefusedLivePeer`
  while another process holds the volume, `IncompatibleVersion` when
  `VolumeConfig::auto_reset_on_incompatible` is off, and a cold reset
  otherwise.  **Every process sharing a cache must use the same setting.**
- Retention roughly doubles the number of directory entries that resolve.
  That is ample for KV blocks and for small objects in 32 MB stripes; on
  large-stripe, small-object volumes the 65 536-entry directory already
  bounds what can be indexed.
- Borrow protection is unchanged in kind: `renew_lease_strict()` returns
  `kTorn` once a step has exposed the borrow's own chunk, and `kCopyNow`
  while a step is in flight. It also returns `kTorn` (and `renew_lease()`
  returns false) after a ceiling-forced step anywhere on the stripe, even if
  the borrow's bytes are intact: the force reset every borrow count, so
  nothing protects the borrow any more. The steps that expose a chunk are a
  frontier advance across it, a ceiling-forced step, and the wrap. The wrap
  exposes only the tail of the retained pass that the frontier never
  reached. For that tail the verdict is conservative: the bytes are still
  intact.
- Alternates never link across a pass. An alternate write over a retained
  head **carries the chain forward**: it rewrites the key's other
  alternates as current-pass copies in its own slot and publishes them
  with the new head in one directory insert. A PageSpeed-style key whose
  optimized alternates arrive after a wrap keeps its Original, Gzip and so
  on. A carry happens at most once per key per pass. It keeps at most
  `kMaxAlternatesPerKey - 1` alternates and `min(A / 8, max_object_size)`
  bytes (A is the stripe's data area), the Original first and then the
  newest. What it drops is counted in `alternates_carry_dropped`. A carry
  never makes the write fail, but its larger slot can be deferred by a
  borrow like any fill (`NoSpace`). Removing one alternate from a retained
  chain removes the whole entry.

**Upgrading from a default-off build.**  Before this release the default was
flush, so a volume created with a default config records flush
(`VolumeHeader::retain_chunks = 0`; volumes from builds that predate wrap
retention carry the same zero).  The mode is not part of the fingerprinted
filename, so the new build resolves to the same file and finds a mode
mismatch on its first open with a default config:

- **No other process holds the file** (the normal restart): the volume is
  reset cold in place and recreated in retention mode.  Every entry is lost
  once; no second file is created, so no extra disk is used.
- **Another process still holds the file** (an overlapping upgrade, or a peer
  configured with `wrap_retention = false`): `Cache::start()` fails with
  `ResetRefusedLivePeer` (C API: `cyclone_cache_create` returns
  `CYCLONE_RESET_REFUSED_LIVE_PEER`).  The running peer is not disturbed.
- **`auto_reset_on_incompatible = false`**: `IncompatibleVersion`.

To keep an existing cache warm across the upgrade, set
`wrap_retention = false` (C API: `disable_wrap_retention = 1`) on every
process.  Switching to retention later costs the same one cold reset.

**Mixed deployments.**  Every process that opens a volume must configure the
same mode, and a process whose mode disagrees with the file fails fast while
any peer holds it, rather than joining the ring.  That is deliberate: a
retaining and a flushing process on one ring is unsafe in both directions (a
flushing writer overwrites retained documents a retaining reader still
admits).  The mode is not put in the filename because filenames do not
reliably carry it (explicit and already-fingerprinted paths are used as
given, and an unsized open picks the newest file of the format), so two
modes cannot be made to run side by side on separate files.  Two
consequences to plan for:

- A multi-process upgrade that keeps old processes running while new ones
  start (for example an nginx binary upgrade, where old workers drain while
  the new master starts) must either stop every old process first, or pin
  the new binary to `wrap_retention = false` for the overlap and switch in a
  later full restart.
- Whichever mode opens the file last with no peer holding it wins, and
  resets it.  Two groups of processes configured differently that take
  turns on one cache wipe it each time; configure the mode in one place.

### Cross-Process RAM Coherence

The mmap'd directory is shared between processes; the RAM cache is not. When a
peer re-records or purges content, this process keeps serving the superseded
bytes out of its own RAM tier until its LRU/CLFUS happens to evict them —
silently, with no error to observe.

`CacheConfig::cross_process_ram_coherence` (default **false**) closes that.
Every RAM entry is stamped, at admission, with the shared seqlock version of
the directory bucket its key hashes to; every RAM hit revalidates that stamp
against the bucket's current version and drops the entry on a mismatch.

```cpp
CacheConfig config;
config.set_multi_process(0, 1);
config.set_cross_process_ram_coherence(true);

auto cache = std::move(*Cache::create(config));
cache->add_volume("/var/cache/cyclone.dat", 10ULL << 30);
cache->start();

// Did it actually take effect?  The knob is inert without the mmap
// directory and without a RAM tier.
assert(cache->cross_process_ram_coherence_active());
```

`bool Cache::cross_process_ram_coherence_active() const` returns true only
when all three preconditions hold: the knob is set, `multi_process_config` is
enabled, and `ram_cache_size > 0`. Enabling the knob without the other two is
silently inert — never an error — so integrators that depend on the guarantee
should assert this rather than trust the config field.

**It is a per-process declaration, not a protocol.** No peer needs the same
setting and no peer needs restarting or upgrading first: the signal it reads
is the per-bucket seqlock version every directory mutation has always
published, in every build. A process running a pre-feature binary protects a
coherent peer just as well as a new one.

**Cost, and why it is off by default.** One shared-memory acquire load per RAM
hit — small against a path that already takes a RAM-cache lock and copies the
document. The real cost is precision: the signal is per BUCKET, so any
directory write to the bucket a key hashes to invalidates this process's RAM
copy, not only a re-record of that key. Hot keys are essentially unaffected;
warm and cold entries in a write-active cache are dropped often, and a
rejected hit costs more than a plain miss. `doc/multi-process.md` carries the
survival-rate table and the decision tree; watch `ram_coherence_rejections`
against `ram_cache_hits` to price it on your own workload.

**Not covered.** The GC phase toggle and a write-buffer wrap produce no
bucket-version bump. Neither is superseded content: a phase toggle is a
directory-wide logical eviction, and a RAM entry is a snapshot that stays a
correct answer for the bytes it holds.

**Drain behaviour.** `read_sync` validates RAM hits but never populates the
RAM tier — only the alternate read path puts. Under this toggle a
`read_sync`-dominant workload therefore progressively drains its RAM tier:
stale entries are rejected and removed, and refill depends on
`read_alternate_sync` traffic.

**No format change.** The bucket-version array predates the feature, so
adopting the knob costs no cold cache and needs no migration. Rollback is
turning it back off; nothing persists.

### Large-Document Readahead

```cpp
size_t readahead_min_bytes = 256 * 1024;          // CacheConfig field (0 = off)
CacheConfig& set_readahead_min_bytes(size_t bytes);  // fluent setter
```

The volume mapping is advised `MADV_RANDOM` at open, which is right for small
HTTP objects (no readahead pollution) but turns a cold read of a large
document into one serial page fault per page. When a document on the disk hit
path is **at least `readahead_min_bytes` long** — measured as the stored
document length, i.e. the 132-byte document header plus any HTTP header plus
the content — the read issues one readahead hint over exactly that document's
byte range, after the full-key re-verification and before the CRC pass makes
the first content touch. Both `read_sync` and the selected-alternate read path
(`read_alternate_sync`) do this. Documents below the threshold keep the
fault-per-page behaviour; the open-time `MADV_RANDOM` is left in place.

- **Default** 256 KiB. **`0` disables** the hint entirely (and the volume then
  does not allocate the 64 KiB per-volume re-advise filter at all).
- The value is copied into each volume's `VolumeConfig::readahead_min_bytes`
  when the volume is added; changing `CacheConfig` afterwards has no effect
  on volumes already open.
- **Per-platform mechanism** (a build-time choice, not a runtime fallback):
  - **Linux:** `madvise(MADV_WILLNEED)` over the mapping, page-aligned and
    issued in chunks: 64 KiB over the first 4 MiB of the document, 512 KiB
    after that. Chunking is needed because Linux caps a single
    `MADV_WILLNEED` at the device's readahead budget, so one call would
    cover only the first ~1 MB of a multi-megabyte range. The small head
    chunks get the first read to the device sooner and keep several reads in
    flight, which is what makes 512 KiB documents fast.
  - **Darwin (macOS):** `fcntl(F_RDADVISE)` over the **file** range. Darwin's
    `MADV_WILLNEED` is synchronous and serialises on the shared VM object,
    which costs most of the multi-process read throughput; `F_RDADVISE` is the
    native asynchronous readahead.
  - **Windows:** `PrefetchVirtualMemory` over the mapping (Windows 8 /
    Server 2012 or later; the build imports it from `kernel32`).
- A given document placement is advised at most once per 2 s, through a
  lossy lock-free filter, so a hot document does not pay a kernel call per
  read while one that has since been evicted from the page cache gets its
  hint back. `CacheStats::readahead_hints_issued` counts the hints issued.
- The hint is best-effort: it takes no lock, never dereferences the range,
  and every error is ignored — a failed hint only costs the old behaviour.
- **Not exposed in the C API.** `CycloneCacheConfig` has no field for it, so a
  cache created through `cyclone_cache_create` always runs with the C++
  default (256 KiB).

See `doc/architecture.md` ("Readahead policy") for the measurements.

### Cold and Sequential Readahead

```cpp
size_t cold_readahead_min_bytes = 16 * 1024;     // CacheConfig field (0 = off)
size_t sequential_readahead_bytes = 1024 * 1024; // CacheConfig field (0 = off)
CacheConfig& set_cold_readahead_min_bytes(size_t bytes);
CacheConfig& set_sequential_readahead_bytes(size_t bytes);
```

Below `readahead_min_bytes` the large-document hint does not fire, and a cold
64 KiB read used to fault its 16 pages in one at a time. These two fields add
readahead there, and past the document, **only on a read that is about to
run the CRC pass**: `verify_checksum_on_read` is on, the document carries a
checksum, and this incarnation has not been verified in this process yet (the
first read after a write, after a restart, or after the offset fell out of
the checksum-validation cache). That is the read that makes Cyclone's first
touch of the content. A warm re-read whose checksum is already validated
never reaches either hint, so it needs no re-advise filter and the warm path
pays nothing.

- **Cold hint.** A document of at least `cold_readahead_min_bytes` and below
  `readahead_min_bytes` gets a readahead hint over its own byte range, with
  the same per-platform call as the large-document hint. Documents below
  `cold_readahead_min_bytes` are untouched (no hint, no detector); the 16 KiB
  default leaves one- to three-page HTTP objects alone.
- **Sequential window.** On the same reads, for documents of at least
  `cold_readahead_min_bytes` of any size, a thread-local detector remembers
  where each stripe's last read ended. A read that starts there (or up to
  64 KiB past it) continues a run, and the hint is extended by up to
  `sequential_readahead_bytes` past the document, within the stripe. It is
  re-issued once less than half of the window is left ahead of the reader.
  One thread reading documents back in the order they were written -- a KV
  tier reusing a prompt prefix -- hops across stripes by key hash, but within
  each stripe the documents are adjacent, so each stripe gets its own window.
- **Cost on a CRC-pending read of a document that is already resident:** a
  hint it did not need. On the Linux benchmark machine about 2 µs for a
  64 KiB document read out of order (one `madvise()`), and in a sequential
  run one short `mincore()` per window (4–7 % of a resident read); on macOS
  one `F_RDADVISE`, about 0.3 µs.
- **Only checksum-verifying reads are hinted:** with
  `verify_checksum_on_read = false` neither hint fires, and small cold reads
  stay unhinted (one fault per page, as before).
- **Recently written documents are skipped:** a first read of a document
  that ends within 4 MiB behind its stripe's write cursor gets no hint
  (it was just written and is resident; this is the write-then-serve
  pattern of an optimized alternate), and a sequential run that catches up
  with a writer still appending stops re-issuing its window. No syscall:
  the cursor comes from the read's stripe snapshot. A cold read-back of
  data the writer has finished with is unaffected.
  `CacheStats::recent_write_hint_skips` counts these reads.
- `CacheStats::cold_readahead_hints` and `sequential_readahead_hints` count
  the hints issued. Like the large-document hint, both are best-effort:
  no lock, no shared state besides the counters, errors ignored.
- **Not exposed in the C API**; a C-created cache runs with the defaults.

Measurements: `doc/kv-cache-benchmark.md`, "Small cold reads (issue #29)".

### Small-Object Tier

Every key-routed `Cache` operation (`read_sync`, `write_sync`, `remove_sync`,
`exists_sync`, the async variants, and the alternate-chain operations) takes
an optional trailing `Tier` parameter defaulting to `Tier::kDefault` (the
existing behavior).

When `CacheConfig::small_tier_percent > 0`, the first added volume is split:
that percentage of its configured size becomes a physically separate
small-object volume in a sidecar file at `"<path>.small"` (provision and back
it up alongside the main file), and only `Tier::kSmall` operations route to
it. This protects small, long-lived entries from the FIFO-wraparound eviction
churn of large payloads — the small volume never participates in default
routing, so payload churn cannot evict small-tier entries.

Key facts:

- **Separate keyspaces**: the same key written to both tiers names two
  independent entries.
- **Sizing**: percent clamped to [1, 50]; small volume floored at one 128 MB
  stripe (multi-process: `total_processes` × 128 MB so each process owns a
  writable stripe). If the total cannot host both tiers, the tier is
  *silently disabled* — check `small_tier_active()`.
- **Fallback**: with the tier disabled, `Tier::kSmall` falls back to default
  routing (callers may pass the tier unconditionally).
- **Cold start**: enabling the feature on an existing cache keeps existing
  data as the default tier; small-tier reads start cold once. Multi-process
  caches reset on geometry change (see doc/multi-process.md).
- **RAM cache / hit tracking**: small-tier reads bypass the RAM cache
  (served via mmap / OS page cache) and are not hit-tracked.
- **Accounting**: `total_capacity()` and `bytes_used()` include the
  carve-out. `cyclone_cache_read_async()` (C API) is default-tier-only.

```cpp
bool small_tier_active() const;
```

Returns `true` when the dedicated small volume exists. `false` means
`small_tier_percent` was 0 **or** the configured total was below the sizing
floor (~256 MB single-process, `(total_processes + 1)` × 128 MB
multi-process) and the tier was silently disabled. C API:
`cyclone_cache_small_tier_active()`.

### VolumeConfig

```cpp
struct VolumeConfig {
    // Path to cache file (required)
    std::string path;

    // Volume size in bytes (0 = use file size)
    size_t size = 0;

    // Which tier this volume serves (default: Tier::kDefault).  Set
    // automatically by the small_tier_percent carve-out; only set
    // explicitly when managing the small volume's size/path manually.
    Tier tier = Tier::kDefault;

    // Stripe size. Default 0 = automatic: the stripe count is derived from
    // the volume size at 32MB granularity (clamped to 1..16 stripes) and the
    // usable region is evenly tiled. Explicitly-set values have a 128MB
    // minimum and are floored to a page boundary.
    size_t stripe_size = 0;
};
```

---

## Plugin System

### CachePlugin

Base class for cache plugins.

```cpp
class CachePlugin {
public:
    virtual ~CachePlugin() = default;

    // Plugin identification
    virtual PluginInfo info() const = 0;

    // Key generation (optional override)
    virtual CacheKey generate_key(const KeyContext& ctx);

    // Variant selection (required)
    virtual std::optional<size_t> select_variant(
        const VariantCollection& variants,
        const LookupContext& ctx) = 0;

    // Freshness checking (optional override)
    virtual FreshnessResult check_freshness(
        const CacheVariant& variant,
        const FreshnessContext& ctx);

    // Eviction priority (optional override)
    virtual double eviction_priority(const CacheVariant& variant);
};
```

#### PluginInfo

```cpp
struct PluginInfo {
    std::string name;      // Human-readable name
    std::string version;   // Version string
    uint32_t plugin_id;    // Unique identifier
};
```

#### Context Types

```cpp
struct KeyContext {
    std::string_view url;
    std::string_view hostname;
    std::span<const std::byte> request_headers;
    void* user_data;
};

struct LookupContext {
    std::span<const std::byte> request_headers;
    void* user_data;
};

struct FreshnessContext {
    std::chrono::system_clock::time_point now;
    bool force_revalidate;
    void* user_data;
};
```

#### FreshnessResult

```cpp
enum class FreshnessResult {
    Fresh,           // Entry is valid, can be served
    Stale,           // Entry is stale but may be served
    MustRevalidate,  // Entry must be revalidated with origin
    Error            // Error checking freshness
};
```

### PluginManager

Manages registered plugins.

```cpp
class PluginManager {
public:
    void register_plugin(std::shared_ptr<CachePlugin> plugin);
    void unregister_plugin(uint32_t plugin_id);

    std::shared_ptr<CachePlugin> get_plugin(uint32_t plugin_id) const;

    void set_alternate_selector(std::shared_ptr<CachePlugin> plugin);
    std::shared_ptr<CachePlugin> get_alternate_selector() const;
};
```

### Metadata

Storage for plugin-specific metadata.

```cpp
class Metadata {
public:
    Metadata();
    explicit Metadata(std::vector<std::byte> data);
    explicit Metadata(std::span<const std::byte> data);

    std::span<const std::byte> data() const;
    std::span<std::byte> mutable_data();
    size_t size() const;
    bool empty() const;

    void resize(size_t size);
    void clear();
    void assign(std::span<const std::byte> data);
};
```

```cpp
class MetadataCollection {
public:
    static constexpr uint32_t kCorePluginId = 0;

    void set(uint32_t plugin_id, Metadata data);
    std::optional<Metadata> get(uint32_t plugin_id) const;
    bool has(uint32_t plugin_id) const;
    void remove(uint32_t plugin_id);
    void clear();

    std::vector<std::byte> serialize() const;
    static MetadataCollection deserialize(std::span<const std::byte> data);
};
```

---

## HTTP Plugin

### HttpCacheAlt

HTTP alternate metadata for content negotiation.

```cpp
class HttpCacheAlt {
public:
    static constexpr uint32_t kPluginId = 1;

    // Request information
    void set_request_method(std::string_view method);
    void set_request_url(std::string_view url);
    void set_request_header(std::string_view name, std::string_view value);

    std::string_view request_method() const;
    std::string_view request_url() const;
    std::optional<std::string_view> get_request_header(std::string_view name) const;

    // Response information
    void set_status_code(uint16_t code);
    void set_response_header(std::string_view name, std::string_view value);

    uint16_t status_code() const;
    std::optional<std::string_view> get_response_header(std::string_view name) const;

    // Timestamps
    void set_request_time(std::time_t t);
    void set_response_time(std::time_t t);
    std::time_t request_time() const;
    std::time_t response_time() const;

    // Freshness
    std::chrono::seconds age() const;
    std::optional<std::chrono::seconds> max_age() const;
    bool is_fresh() const;

    // Serialization
    std::vector<std::byte> serialize() const;
    static HttpCacheAlt deserialize(std::span<const std::byte> data);
};
```

### HttpAlternatePlugin

Creates the built-in HTTP content negotiation plugin.

```cpp
std::shared_ptr<CachePlugin> create_http_alternate_plugin();
```

**Features:**
- Vary header matching
- Accept header quality calculation
- Accept-Encoding matching (gzip, br, identity)
- Accept-Language prefix matching
- Cache-Control freshness (max-age, must-revalidate, no-cache)

---

## C API

Cyclone Cache provides a C ABI wrapper (`include/cyclone/cyclone_c.h`) for integration with C code or languages with C FFI.

**Zero-initialised configs.** `CycloneCacheConfig` does not mirror the C++
defaults field for field. In particular `ram_cache_size_bytes = 0` means the
RAM tier is **disabled** (not the 256 MB C++ default), `max_object_size = 0`
means the **library default** (64 MB, not "unbounded"), and
`enable_checksum = 0` means checksums are **disabled** (the C++ default is
on). See ["Zero Values"](#zero-values) above for
the full mapping, and the field comments in `cyclone_c.h`.

**Eviction mode.**  `disable_wrap_retention` is stated in the negative, like
`disable_alternate_unlink`: `0` keeps the library default
(`CacheConfig::wrap_retention`, which is retention), non-zero selects flush
mode.  A zero-initialised `CycloneCacheConfig` therefore retains; set it to
`1` to keep a cache created by an earlier default-off build warm (see
["Upgrading from a default-off build"](#wrap-retention)).  It is a
trailing field with the same ABI note as `small_tier_percent`: a caller
compiled against an older header passes a smaller struct, so recompile
against the new header when adopting it.  Every process sharing a cache must
pass the same value (see ["Wrap Retention"](#wrap-retention)).

### Error Codes

`CycloneError` is a one-byte `uint8_t` typedef (not an `enum` type) so that
its width is identical in C and C++; the codes are an anonymous enum. Values
are append-only for ABI stability. Note that as a `uint8_t` it streams as a
character in C++ — log it as `static_cast<unsigned>(err)`.

```c
typedef uint8_t CycloneError;
enum {
    CYCLONE_OK = 0,
    CYCLONE_NOT_FOUND,
    CYCLONE_EXISTS,
    CYCLONE_NO_SPACE,
    CYCLONE_IO_ERROR,
    CYCLONE_CORRUPTED,
    CYCLONE_INVALID_KEY,
    CYCLONE_INVALID_ARGUMENT,
    CYCLONE_NOT_INITIALIZED,
    CYCLONE_INTERNAL_ERROR,
    CYCLONE_RESET_REFUSED_LIVE_PEER,  /* a live peer holds the cache */
    CYCLONE_OBJECT_TOO_LARGE,         /* write exceeds max_object_size */
    CYCLONE_BUSY                      /* transient contention; retry */
};

typedef uint8_t CycloneTier;
enum {
    CYCLONE_TIER_DEFAULT = 0,  /* hash-routed payload volume */
    CYCLONE_TIER_SMALL = 1     /* small-object sidecar volume, when enabled */
};
```

`CYCLONE_BUSY` is `CacheError::Busy`. From `cyclone_cache_read`,
`cyclone_cache_exists` and their `_tier` variants it means the key's presence
is unknown, not that the key is absent (see [Busy lookups](#busy-lookups)).
From a write, delete or hit-count update it means a contended or raced lock;
before this code existed those cases were reported as
`CYCLONE_INTERNAL_ERROR`. `cyclone_cache_read_async` passes `CYCLONE_BUSY` to
`read_cb` only when no miss handler is set. With a miss handler set, a Busy
read goes to the handler like any other non-hit, so the waiters get the
fetched value. The write-back does not always fix the bucket. In the process
that owns the key's stripe, the write releases a bucket whose holder is stuck
and then stores the value. In any other process the write fails with
`NotOwned` before it reaches the directory, so the bucket stays busy until
its owner next writes, removes or updates hit counts in it.

### Core Functions

#### Cache Lifecycle

```c
// Create a cache instance
CycloneError cyclone_cache_create(const CycloneCacheConfig *config,
                                  CycloneCacheHandle **out);

// Destroy a cache instance (drains pending async operations)
void cyclone_cache_destroy(CycloneCacheHandle *cache);
```

#### Synchronous Operations

```c
// Read an entry (returns CYCLONE_NOT_FOUND on miss)
CycloneError cyclone_cache_read(CycloneCacheHandle *cache,
                                const char *key, size_t key_len,
                                CycloneReadHandle **out);

// Get data from read handle
CycloneError cyclone_cache_read_data(CycloneReadHandle *handle,
                                     const char **data, size_t *data_len);

// Close read handle
void cyclone_cache_read_close(CycloneReadHandle *handle);

// Write an entry
CycloneError cyclone_cache_write(CycloneCacheHandle *cache,
                                 const char *key, size_t key_len,
                                 const char *data, size_t data_len);

// Delete an entry
CycloneError cyclone_cache_delete(CycloneCacheHandle *cache,
                                  const char *key, size_t key_len);

// Check if entry exists (returns CYCLONE_OK or CYCLONE_NOT_FOUND)
CycloneError cyclone_cache_exists(CycloneCacheHandle *cache,
                                  const char *key, size_t key_len);

// Get statistics
CycloneError cyclone_cache_stats(CycloneCacheHandle *cache,
                                 CycloneCacheStats *out);
```

### Miss Callback Hook

The C API supports async reads with automatic request coalescing. When multiple concurrent reads request the same missing key, only one miss handler invocation occurs and all waiters receive the result.

#### Types

```c
// Called when a fetch completes (success or failure)
typedef void (*CycloneMissDoneCallback)(void *user_data,
                                        const char *data, size_t data_len,
                                        CycloneError err);

// Called when a cache miss occurs and no fetch is in flight for the key
typedef void (*CycloneMissHandler)(const char *key, size_t key_len,
                                   void *handler_user_data,
                                   CycloneMissDoneCallback done_cb,
                                   void *done_user_data);

// Called when an async read completes
typedef void (*CycloneReadCallback)(void *user_data,
                                    const char *data, size_t data_len,
                                    CycloneError err);
```

#### Functions

```c
// Register a miss handler (NULL to clear)
CycloneError cyclone_cache_set_miss_handler(CycloneCacheHandle *cache,
                                            CycloneMissHandler handler,
                                            void *handler_user_data);

// Async read with miss callback support
// - Cache hit: calls read_cb immediately
// - Cache miss + handler set: triggers handler (or coalesces with in-flight)
// - Cache miss + no handler: calls read_cb with CYCLONE_NOT_FOUND
CycloneError cyclone_cache_read_async(CycloneCacheHandle *cache,
                                      const char *key, size_t key_len,
                                      CycloneReadCallback read_cb,
                                      void *read_user_data);

// Drain pending async operations (called automatically by destroy)
CycloneError cyclone_cache_drain_pending(CycloneCacheHandle *cache,
                                         uint32_t timeout_ms);
```

#### Example: Fetch-on-Miss with Coalescing

```c
void my_miss_handler(const char *key, size_t key_len, void *user_data,
                     CycloneMissDoneCallback done_cb, void *done_ud) {
    // Fetch from origin (can be async - call done_cb from any thread)
    char *fetched = fetch_from_origin(key, key_len);
    if (fetched) {
        done_cb(done_ud, fetched, strlen(fetched), CYCLONE_OK);
        free(fetched);
    } else {
        done_cb(done_ud, NULL, 0, CYCLONE_IO_ERROR);
    }
}

void my_read_cb(void *user_data, const char *data, size_t len, CycloneError err) {
    if (err == CYCLONE_OK) {
        printf("Got %zu bytes\n", len);
    } else {
        printf("Error: %d\n", err);
    }
}

// Setup
cyclone_cache_set_miss_handler(cache, my_miss_handler, NULL);

// Multiple concurrent reads for same key trigger only ONE fetch
for (int i = 0; i < 10; i++) {
    cyclone_cache_read_async(cache, "key", 3, my_read_cb, NULL);
}
```

---

## Error Handling

```cpp
enum class CacheError {
    None,              // No error
    NotFound,          // Entry not found in cache
    AlreadyExists,     // Entry already exists
    NotInitialized,    // Cache not started
    AlreadyOpen,       // Resource already open
    Closed,            // Handle was closed
    IoError,           // I/O operation failed
    InvalidArgument,   // Invalid parameter
    OutOfSpace,        // No space available
    Corrupted,         // Data corruption detected
    Busy,              // Transient contention; retry (see below)
    InternalError      // Internal error
};
```

### Busy lookups

Directory lookups are lock-free: a reader checks a per-bucket seqlock version
and retries when a writer is mid-update. A writer that is descheduled inside
that window can hold the bucket for a whole scheduling quantum. So a reader
retries 100 times without reading the clock, then sleeps between further
retries (10 µs, doubling up to 1 ms per sleep) for up to 5 ms
(`SeqlockReadWait::kBudget` in `src/core/directory.hpp`). It sleeps rather
than spins so that a waiting reader, possibly on an event-loop thread, does
not burn its CPU. If the bucket is still busy after that, the lookup returns
`CacheError::Busy`, not `NotFound`. It never saw the bucket in a consistent
state, so it cannot say whether the key is there. The 5 ms cap is a
trade-off: under heavy CPU oversubscription a writer can stay descheduled for
longer, and those lookups report `Busy`; the caller can retry or refetch.

Every directory probe that spends the whole budget is counted in
`CacheStats::directory_read_timeouts`. The count is expected to be near 0.
A steadily growing count means writers are being descheduled for longer
than 5 ms, or, in multi-process mode, that a writer's process died in the
middle of a directory update. In the second case, the owning process's
next write, delete or hit-count update of a key in that bucket releases it;
that write waits out the budget once and then goes through.

All operations return `std::expected<T, CacheError>`. Use `.has_value()`, `.value()`, and `.error()` to check results:

```cpp
auto result = cache->read_sync(key);
if (result.has_value()) {
    // Success - use result.value() or *result
    auto content = result->content();
} else {
    // Failure - check result.error()
    if (result.error() == CacheError::NotFound) {
        // Cache miss
    }
}
```

---

## Async Operations

Cyclone Cache provides C++20 coroutine-based async operations via `Task<T>`.

```cpp
Task<std::expected<ReadHandle, CacheError>> open_read(const CacheKey& key);
Task<std::expected<WriteHandle, CacheError>> open_write(const CacheKey& key, uint64_t content_length);
Task<std::expected<void, CacheError>> remove(const CacheKey& key);
Task<std::expected<bool, CacheError>> exists(const CacheKey& key);
```

**Example with coroutines:**
```cpp
Task<void> cache_operation(Cache& cache, const CacheKey& key) {
    auto result = co_await cache.open_read(key);
    if (result) {
        auto content = result->content();
        // Process content...
    }
}
```

The `Task<T>` type supports:
- `co_await` for suspension
- `co_return` for completion
- Automatic continuation chaining
