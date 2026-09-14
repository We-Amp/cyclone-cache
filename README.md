# Cyclone Cache

A modern C++23 disk cache library inspired by Apache Traffic Server's proven cache design. Cyclone Cache provides high-performance caching with scan-resistant eviction, memory-mapped I/O, and a flexible plugin system.

## Features

- **Scan-Resistant Caching**: CLFUS (Clock LRU Frequency Size) algorithm prevents streaming workloads from evicting hot data
- **Memory-Mapped I/O**: Fast reads with OS-assisted paging and zero-copy potential
- **Compact Directory**: 10-byte entries supporting up to 512TB per stripe
- **Plugin System**: Extensible architecture for custom key generation, variant selection, and freshness checking
- **HTTP Support**: Built-in plugin for RFC 7234 content negotiation (Vary headers, Accept matching, freshness calculation)
- **Cross-Platform**: Supports Linux, macOS, and Windows
- **Modern C++**: Uses C++23 features including `std::expected`, coroutines, and `std::span`

## Requirements

- CMake 3.20 or later
- C++23 compatible compiler (GCC 13+, Clang 16+, MSVC 2022+)
- OpenSSL (for SHA-256 hashing)

## Building

```bash
# Configure
cmake -B build

# Build
cmake --build build

# Run tests
ctest --test-dir build

# Run benchmarks
./build/cache_benchmark 100 1000 4096  # 100MB cache, 1000 entries, 4KB each
```

`CMakePresets.json` provides per-platform presets (run `cmake --list-presets`):

```bash
cmake --preset macos-arm64 && cmake --build --preset macos-arm64 && ctest --preset macos-arm64
```

Windows/vcpkg presets need `VCPKG_ROOT`; the `*-zig` cross-compile presets
disable tests, examples, and benchmarks.

### Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `CYCLONE_BUILD_TESTS` | ON | Build unit and integration tests |
| `CYCLONE_BUILD_HTTP_PLUGIN` | ON | Build HTTP alternate selection plugin |
| `CYCLONE_BUILD_EXAMPLES` | ON | Build example programs |
| `CYCLONE_BUILD_BENCHMARKS` | ON | Build benchmark programs |
| `CYCLONE_ENABLE_ASAN` | OFF | Enable AddressSanitizer |
| `CYCLONE_USE_BUNDLED_SHA256` | OFF | Use bundled SHA-256 instead of OpenSSL — ON in CI for a hermetic build, and required on machines without OpenSSL dev headers |

## Quick Start

```cpp
#include "cyclone/cache.hpp"
#include "cyclone/key.hpp"

using namespace cyclone;

int main() {
    // Create cache with default configuration
    CacheConfig config;
    auto cache_result = Cache::create(config);
    if (!cache_result) {
        return 1;
    }
    auto& cache = *cache_result;

    // Add a storage volume
    VolumeConfig vol_config;
    vol_config.path = "/var/cache/myapp/cache.dat";
    vol_config.size = 1024 * 1024 * 1024;  // 1GB
    cache->add_volume(vol_config);
    cache->start();

    // Write to cache
    CacheKey key("http://example.com/page.html");
    std::string content = "Hello, World!";
    std::vector<std::byte> data(content.size());
    std::memcpy(data.data(), content.data(), content.size());

    auto write_handle = cache->write_sync(key, data.size());
    if (write_handle) {
        write_handle->write_sync(std::span<const std::byte>(data));
        write_handle->close_sync();
    }

    // Read from cache
    auto read_handle = cache->read_sync(key);
    if (read_handle) {
        auto cached_content = read_handle->content();
        // Use cached_content...
    }

    cache->stop();
    return 0;
}
```

## API Overview

### Cache

The main entry point for all cache operations.

```cpp
class Cache {
    // Creation
    static std::expected<std::unique_ptr<Cache>, CacheError> create(const CacheConfig& config);

    // Volume management
    std::expected<void, CacheError> add_volume(const VolumeConfig& config);
    std::expected<void, CacheError> start();
    void stop();

    // Synchronous operations
    std::expected<ReadHandle, CacheError> read_sync(const CacheKey& key);
    std::expected<WriteHandle, CacheError> write_sync(const CacheKey& key, uint64_t content_length);
    std::expected<void, CacheError> remove_sync(const CacheKey& key);
    std::expected<bool, CacheError> exists_sync(const CacheKey& key);

    // Asynchronous operations (coroutines)
    Task<std::expected<ReadHandle, CacheError>> open_read(const CacheKey& key);
    Task<std::expected<WriteHandle, CacheError>> open_write(const CacheKey& key, uint64_t content_length);
    Task<std::expected<void, CacheError>> remove(const CacheKey& key);

    // Alternate chain operations (for content variants like compressed versions)
    std::expected<std::vector<AlternateInfo>, CacheError> list_alternates_sync(const CacheKey& key);
    std::expected<WriteHandle, CacheError> write_alternate_sync(const CacheKey& key, AlternateId id, uint64_t length);
    std::expected<ReadHandle, CacheError> read_alternate_sync(const CacheKey& key, const StorageAlternateSelector& sel, const AlternateSelectionContext& ctx);
    std::expected<void, CacheError> remove_alternate_sync(const CacheKey& key, AlternateId id);

    // Statistics
    CacheStats stats() const;
};
```

### CacheKey

Cache keys are SHA-256 hashes that can be created from strings or URLs.

```cpp
class CacheKey {
    explicit CacheKey(std::string_view s);
    static CacheKey from_url(std::string_view url, std::string_view hostname = {});

    uint32_t segment_hash() const;  // For stripe selection
    uint32_t bucket_hash() const;   // For bucket selection
    uint16_t tag() const;           // 12-bit collision tag
};
```

### ReadHandle / WriteHandle

Handles for reading and writing cached content.

```cpp
class ReadHandle {
    std::span<const std::byte> header() const;
    std::span<const std::byte> content() const;
    uint64_t content_length() const;
    bool is_ram_cache_hit() const;
    std::optional<std::span<const std::byte>> mapped_view() const;
};

class WriteHandle {
    void set_header(std::span<const std::byte> header);
    std::expected<size_t, CacheError> write_sync(std::span<const std::byte> data);
    std::expected<void, CacheError> close_sync();
    void abort();
};
```

### Configuration

```cpp
struct CacheConfig {
    size_t ram_cache_size = 256 * 1024 * 1024;  // 256MB default
    RamCacheType ram_cache_type = RamCacheType::CLFUS;
    uint32_t small_tier_percent = 0;  // 0 = small-object tier disabled

    // Validate every RAM-cache hit against the shared directory's bucket
    // version, so a peer process's re-record or purge is not served from
    // this process's RAM tier.  Per-process declaration, needs no peer
    // agreement; inert without multi-process mode and a RAM tier.  See
    // doc/multi-process.md.
    bool cross_process_ram_coherence = false;

    // Lease-based region pinning (borrow-scoped since the write-starvation fix): a
    // disk-hit read registers a per-stripe borrow (released when its
    // ReadHandle closes) and stamps a per-stripe lease of this duration;
    // a wrap over the borrowed region is deferred (and the fill dropped)
    // only while a borrow is outstanding AND the lease holds — closing
    // the handle returns write capacity immediately.  0 disables.
    // Renew long holds via ReadHandle::renew_lease() at a cadence
    // <= 3T/4; holds past lease_wrap_ceiling are unprotected.
    std::chrono::milliseconds read_lease_duration{5000};
    std::chrono::milliseconds lease_wrap_ceiling{60000};
};

struct VolumeConfig {
    std::string path;
    size_t size = 0;  // 0 = use file size
    size_t stripe_size = 0;  // 0 = auto (32MB granularity, up to 16 stripes);
                             // explicit values have a 128MB minimum
};
```

### Small-Object Tier

Cyclone's eviction is FIFO-by-wraparound: when a stripe wraps, everything in
its previous phase is evicted. Small, long-lived entries (metadata,
manifests) sharing a volume with large-payload churn are wiped on every wrap.
The small-object tier gives them a hard physical guarantee instead of a
policy: a separate small volume that only explicitly tagged operations route
to, so payload churn *cannot* evict them.

```cpp
CacheConfig config;
config.set_small_tier_percent(10);  // carve 10% out of the volume below

auto cache = Cache::create(config);
cache->add_volume("/var/cache/app.cache", 10_GB);  // default gets 9GB,
                                                   // small tier gets 1GB at
                                                   // /var/cache/app.cache.small
cache->start();

// Tag small-object operations explicitly; untagged calls are unchanged.
cache->write_sync(key, len, Tier::kSmall);
cache->read_sync(key, Tier::kSmall);
```

Behavior and sizing rules:

- `small_tier_percent` is clamped to **[1, 50]**; the carve-out comes off the
  first added volume's configured size. The small volume lives in a sidecar
  file at **`<path>.small`** — provision and back it up alongside the main
  file.
- The small volume is floored at one **128 MB** stripe (plus header). In
  multi-process mode the floor grows to **`total_processes` × 128 MB** so
  every process owns at least one writable small-tier stripe.
- If the configured total cannot host both tiers' floors, the tier is
  **silently disabled** — check `cache->small_tier_active()` (C:
  `cyclone_cache_small_tier_active()`) after `add_volume()` if you require
  it. As a rule of thumb the total must be at least ~256 MB single-process,
  `(total_processes + 1)` × 128 MB multi-process.
- The two tiers are separate keyspaces: the same key names two independent
  entries, and reads only see the tier they were issued against. When the
  tier is disabled, `Tier::kSmall` falls back to default routing, so callers
  may pass the tier unconditionally.
- Enabling the feature on an existing cache keeps the old volume's data as
  the default tier; small-tier reads start cold once. (Multi-process caches
  reset on any geometry change — see [doc/multi-process.md](doc/multi-process.md).)
- Small-tier reads bypass the RAM cache (served via mmap / OS page cache);
  hit tracking is also default-tier-only.
- `total_capacity()` / `bytes_used()` include the carve-out;
  `cyclone_cache_read_async()` remains default-tier-only.

## RAM Cache Algorithms

### CLFUS (Clock LRU Frequency Size)

The default algorithm, designed for scan resistance:

- First access records key in a "seen filter" but doesn't cache
- Second access admits to cache
- Eviction uses value function: `(hits + 1) / (size + 256)`
- History list tracks recently evicted entries for smarter re-admission

### LRU (Least Recently Used)

Simple LRU for predictable behavior:

```cpp
CacheConfig config;
config.ram_cache_type = RamCacheType::LRU;
```

## Plugin System

Cyclone Cache supports plugins for customizing cache behavior.

### Creating a Plugin

```cpp
class MyPlugin : public CachePlugin {
public:
    PluginInfo info() const override {
        return {"my-plugin", "1.0.0", 100};  // name, version, plugin_id
    }

    CacheKey generate_key(const KeyContext& ctx) override {
        // Custom key generation
        return CacheKey::from_url(ctx.url, ctx.hostname);
    }

    std::optional<size_t> select_variant(
        const VariantCollection& variants,
        const LookupContext& ctx) override {
        // Select best variant for request
        if (variants.empty()) return std::nullopt;
        return 0;  // Return first variant
    }

    FreshnessResult check_freshness(
        const CacheVariant& variant,
        const FreshnessContext& ctx) override {
        // Check if cached entry is still valid
        return FreshnessResult::Fresh;
    }
};
```

### Registering a Plugin

```cpp
auto plugin = std::make_shared<MyPlugin>();
cache->plugin_manager().register_plugin(plugin);
cache->plugin_manager().set_alternate_selector(plugin);
```

### HTTP Alternate Plugin

Built-in plugin for HTTP content negotiation:

```cpp
#ifdef CYCLONE_HTTP_PLUGIN
// Enable HTTP-aware caching
auto http_plugin = cyclone::create_http_alternate_plugin();
cache->plugin_manager().set_alternate_selector(http_plugin);
#endif
```

Features:
- Vary header matching
- Accept/Accept-Encoding/Accept-Language quality calculation
- Cache-Control freshness (max-age, must-revalidate)
- RFC 7234 age calculation

### Background Optimization System

Cyclone Cache includes a background optimization system that allows plugins to automatically
generate optimized alternates (e.g., Brotli-compressed versions) after content is written.

```cpp
class MyCompressionPlugin : public OptimizationPlugin {
public:
    PluginInfo info() const override {
        return {"my-compressor", "1.0.0", 42};
    }

    OptimizationPlan plan_optimization(
        const CacheKey& key,
        std::span<const std::byte> header,
        uint64_t content_length,
        AlternateId written_alternate,
        uint32_t hit_count) override
    {
        OptimizationPlan plan;
        // Only optimize original content above a certain hit count
        if (written_alternate == AlternateId::Original && hit_count >= 5) {
            plan.add(AlternateId::Brotli, 10, true, content_length * 2);
        }
        return plan;
    }

    std::expected<TransformResult, CacheError> transform(
        AlternateId target,
        const OptimizationContext& ctx) override
    {
        // Check for cancellation periodically
        if (ctx.is_cancelled()) {
            return std::unexpected(CacheError::OptimizationCancelled);
        }

        // Perform compression...
        TransformResult result;
        result.alternate_id = target;
        result.content = compress(ctx.source_content());
        return result;
    }
};

// Register the plugin
auto plugin = std::make_shared<MyCompressionPlugin>();
cache->optimization_engine()->register_plugin(plugin);
```

Configuration options:

```cpp
CacheConfig config;
config.optimization_config
    .set_enabled(true)
    .set_min_threads(1)
    .set_max_threads(4)  // 0 = auto (hardware_concurrency / 2)
    .set_min_hits_before_optimize(5)
    .set_load_high_watermark(0.8)  // Pause when system load exceeds this
    .set_load_low_watermark(0.5);  // Resume when load drops below this
```

Features:
- Adaptive thread pool with autoscaling based on queue depth
- Priority queue with deduplication
- Load-aware throttling (pauses during high system load)
- Cancellation support for graceful shutdown
- Memory usage tracking and limits

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                      Cache API                          │
├─────────────────────────────────────────────────────────┤
│                   Plugin Manager                         │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────┐  │
│  │ Key Gen     │  │ Variant Sel │  │ Freshness Check │  │
│  └─────────────┘  └─────────────┘  └─────────────────┘  │
├─────────────────────────────────────────────────────────┤
│                     RAM Cache                            │
│  ┌─────────────────────┐  ┌─────────────────────────┐   │
│  │       CLFUS         │  │         LRU             │   │
│  └─────────────────────┘  └─────────────────────────┘   │
├─────────────────────────────────────────────────────────┤
│                      Volumes                             │
│  ┌─────────────────────────────────────────────────┐    │
│  │  Stripe 0   │  Stripe 1   │  Stripe 2   │  ...  │    │
│  │ ┌─────────┐ │ ┌─────────┐ │ ┌─────────┐ │       │    │
│  │ │Directory│ │ │Directory│ │ │Directory│ │       │    │
│  │ └─────────┘ │ └─────────┘ │ └─────────┘ │       │    │
│  │ ┌─────────┐ │ ┌─────────┐ │ ┌─────────┐ │       │    │
│  │ │  Data   │ │ │  Data   │ │ │  Data   │ │       │    │
│  │ └─────────┘ │ └─────────┘ │ └─────────┘ │       │    │
│  └─────────────────────────────────────────────────┘    │
├─────────────────────────────────────────────────────────┤
│                    Mapped File I/O                       │
│  ┌─────────────────────┐  ┌─────────────────────────┐   │
│  │   POSIX (mmap)      │  │   Win32 (MapViewOfFile) │   │
│  └─────────────────────┘  └─────────────────────────┘   │
└─────────────────────────────────────────────────────────┘
```

### Directory Entry Layout (10 bytes)

```
Word 0 (16 bits): offset[0:15]
Word 1 (16 bits): offset[16:23] | big[2] | size[6]
Word 2 (16 bits): tag[12] | phase[1] | head[1] | pinned[1] | reserved[1]
Word 3 (16 bits): next pointer (bucket chain)
Word 4 (16 bits): offset[24:39]
```

- 40-bit offset supports up to 512TB per stripe
- 12-bit tag for collision detection
- 6-bit size with 2-bit multiplier for approximate sizing

### Document Format (132-byte header, v5)

```
┌────────────────────────────────────────┐
│ magic (4) | len (4) | total_len (8)    │
├────────────────────────────────────────┤
│ first_key (32 bytes - SHA-256)         │
├────────────────────────────────────────┤
│ fragment_key (32 bytes - SHA-256)      │
├────────────────────────────────────────┤
│ header_len | type | ver | flags        │
│ sync_serial | write_serial             │
│ pin_until | checksum | frag_offset     │
│ hit_count | next_alternate_offset (8)  │
│ alternate_id | reserved | last_access  │
├────────────────────────────────────────┤
│ Header data (variable)                 │
├────────────────────────────────────────┤
│ Content data (variable)                │
└────────────────────────────────────────┘
```

## Performance

Benchmark results on Apple M-series (50MB cache, 500 entries, 2KB each):

| Operation | Throughput | Latency |
|-----------|------------|---------|
| Key generation (SHA-256) | 2-3M ops/sec | 0.3-0.5 us |
| Write | 7-10K ops/sec | 92-130 us |
| Read (sequential) | 60-180K ops/sec | 5-17 us |
| Read (random) | 80-160K ops/sec | 6-12 us |
| Exists check | 1.5-4M ops/sec | 0.25-0.7 us |
| Read miss | 1.5-3M ops/sec | 0.35-0.7 us |

*Ranges reflect variance due to OS page cache effects. Write latency depends on `sync_on_write` configuration.*

## Error Handling

All operations return `std::expected<T, CacheError>`:

```cpp
enum class CacheError {
    None,
    NotFound,
    AlreadyExists,
    NotInitialized,
    AlreadyOpen,
    Closed,
    IoError,
    InvalidArgument,
    OutOfSpace,
    Corrupted,
    InternalError
};
```

Example error handling:

```cpp
auto result = cache->read_sync(key);
if (!result) {
    switch (result.error()) {
        case CacheError::NotFound:
            // Handle cache miss
            break;
        case CacheError::IoError:
            // Handle I/O error
            break;
        default:
            // Handle other errors
            break;
    }
}
```

## C API

Cyclone Cache provides a C ABI wrapper for integration with C code or languages with C FFI (Python, Rust, Go, etc.).

### Basic Usage

```c
#include "cyclone/cyclone_c.h"

// Create cache
CycloneCacheConfig config = {
    .cache_path = "/var/cache/myapp.cache",
    .cache_size_bytes = 100 * 1024 * 1024,  // 100MB
    .ram_cache_size_bytes = 10 * 1024 * 1024,  // 10MB
    .enable_checksum = 1,
    .num_segments = 4
    // NOTE: .small_tier_percent needs a total of ~256MB+ (see "Small-Object
    // Tier" above) — at this example's 100MB the tier would silently disable;
    // check cyclone_cache_small_tier_active() after create when using it.
};

CycloneCacheHandle *cache = NULL;
CycloneError err = cyclone_cache_create(&config, &cache);

// Write
const char *key = "my-key";
const char *data = "Hello, World!";
cyclone_cache_write(cache, key, strlen(key), data, strlen(data));

// Read
CycloneReadHandle *rh = NULL;
if (cyclone_cache_read(cache, key, strlen(key), &rh) == CYCLONE_OK) {
    const char *read_data;
    size_t read_len;
    cyclone_cache_read_data(rh, &read_data, &read_len);
    // Use read_data...
    cyclone_cache_read_close(rh);
}

// Cleanup
cyclone_cache_destroy(cache);
```

### Async Read with Miss Callback (Request Coalescing)

The C API supports async reads with a miss callback hook. When multiple concurrent reads request the same missing key, only one fetch is triggered and all waiters receive the result.

```c
// Miss handler - called when cache misses
void my_miss_handler(const char *key, size_t key_len, void *user_data,
                     CycloneMissDoneCallback done_cb, void *done_ud) {
    // Fetch data from origin (e.g., HTTP request)
    const char *fetched = "fetched data";
    done_cb(done_ud, fetched, strlen(fetched), CYCLONE_OK);
}

// Register handler
cyclone_cache_set_miss_handler(cache, my_miss_handler, NULL);

// Async read callback
void my_read_cb(void *user_data, const char *data, size_t len, CycloneError err) {
    if (err == CYCLONE_OK) {
        // Use data...
    }
}

// Async read - will call miss handler on cache miss
cyclone_cache_read_async(cache, key, strlen(key), my_read_cb, NULL);
```

See `include/cyclone/cyclone_c.h` for the complete C API.

## Thread Safety

- Cache operations are thread-safe
- **Reads are lock-free**: readers take no stripe lock in any mode. Correctness
  comes from per-bucket seqlock directories, commit ordering (data durable
  before the directory entry is published), CRC validation, and lease-based
  region pinning that keeps a borrowed mmap region from being
  wrapped out from under a reader
- Writes take the stripe mutex exclusively in `commit_write` — the only stripe
  lock in the system
- RAM cache (CLFUS) is segmented (up to 64 segments) so concurrent readers
  don't contend on one mutex

See [doc/architecture.md](doc/architecture.md#concurrency-model) and
[doc/multi-process.md](doc/multi-process.md) for the full model.

## Security

Cyclone Cache includes hardening against malicious or corrupted cache data:

- **Bounds checking**: Deserialization validates counts and lengths before allocation
- **Cycle detection**: Directory chain traversal limited to prevent infinite loops
- **Overflow protection**: Size calculations checked for integer overflow
- **RAII cleanup**: WriteHandle destructor aborts incomplete writes
- **Checksum validation**: Optional CRC32 for content integrity

See [CONTRIBUTING.md](CONTRIBUTING.md) for security testing guidelines.

## Documentation

- [Architecture Guide](doc/architecture.md) - Internal design and data structures
- [API Reference](doc/api-reference.md) - Complete API documentation
- [Plugin Development](doc/plugin-development.md) - Creating custom plugins
- [Contributing](CONTRIBUTING.md) - Development guidelines

## License

Licensed under the Apache License 2.0. See [LICENSE](LICENSE) for details.

## Acknowledgments

Inspired by [Apache Traffic Server](https://trafficserver.apache.org/)'s cache implementation, particularly:
- 10-byte directory entry format
- CLFUS scan-resistant algorithm
