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
    uint64_t evictions;

    // Wrap-cadence telemetry (aggregated across volumes)
    uint64_t write_buffer_wraps;     // Circular write-buffer wraps since open
    uint64_t last_wrap_interval_ns;  // ns between the last two wraps
    uint64_t min_wrap_interval_ns;   // Smallest observed wrap interval
    uint64_t last_wrap_age_ns;       // ns since the most recent wrap

    // Lease-based region pinning (all three are process-local)
    uint64_t wraps_deferred_by_lease;  // Wraps deferred by a live read lease
    uint64_t writes_dropped_by_lease;  // Fills dropped by deferred wraps
    uint64_t wraps_forced_past_lease;  // Wraps forced past lease_wrap_ceiling

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
    uint64_t readahead_hints_issued;  // Readahead hints that reached the kernel
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
from an ordinary wrap.

`readahead_hints_issued` counts the large-document readahead hints the disk
read path actually issued (see ["Large-Document
Readahead"](#large-document-readahead) below), summed across volumes. It
counts hints that got past the per-placement re-advise filter, so it measures
kernel calls made, not large reads served: a hot document contributes at most
one hint per re-advise interval (2 s) however often it is read, and a document
below `readahead_min_bytes` never contributes. Process-local.

The C API mirrors these fields at the end of `CycloneCacheStats`
(append-only extension), **except `readahead_hints_issued`**, which is
C++-only.

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

    // Validate RAM-cache hits against the shared directory's bucket version
    // (default: false = off).  See "Cross-Process RAM Coherence" below.
    bool cross_process_ram_coherence = false;

    // Readahead hint for large documents on the disk read path (default:
    // 256 KiB; 0 = off).  Fluent setter: set_readahead_min_bytes().
    // See "Large-Document Readahead" below.
    size_t readahead_min_bytes = 256 * 1024;
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
    issued in 512 KiB chunks (Linux caps a single `MADV_WILLNEED` at the
    device's readahead budget, so one call would cover only the first ~1 MB
    of a multi-megabyte range).
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
    CYCLONE_OBJECT_TOO_LARGE          /* write exceeds max_object_size */
};

typedef uint8_t CycloneTier;
enum {
    CYCLONE_TIER_DEFAULT = 0,  /* hash-routed payload volume */
    CYCLONE_TIER_SMALL = 1     /* small-object sidecar volume, when enabled */
};
```

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
    InternalError      // Internal error
};
```

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
