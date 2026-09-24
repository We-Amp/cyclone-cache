# CLAUDE.md

This file provides context and guidance for contributors (human and AI-assisted) working in this repository.

## Related Repositories

This is a shared library used by both PageSpeed products:

- **the PageSpeed optimizer** (`We-Amp/pagespeed-optimizer`) — uses Cyclone via Bazel
- **mod_pagespeed** (`We-Amp/mod_pagespeed`) — uses Cyclone via a vendored copy

This repo is the source of truth for Cyclone, consumed by both engines via a
downstream Bazel `git_repository` (branch `main`, pinned to a commit at
release). The copies embedded in consumer checkouts — under `reference/` or
`vendor/` directories there — are consumers overwritten on sync, so make ALL
Cyclone edits here. Before editing, run `git rev-parse --show-toplevel` and
`pwd` to confirm your checkout path is NOT under a `reference/` or `vendor/`
directory. Never search or edit vendored copies or sibling worktrees; anchor
all paths to the resolved toplevel.

**Default branch:** `main`.


**Design decisions** this repo relies on, stated in plain words:

- Cyclone is consumed downstream via a Bazel `git_repository` (branch `main`),
  pinned to a commit at release.
- This `CLAUDE.md` / `AGENTS.md` pair follows the cross-repo AI-agent
  ergonomics standard shared by all We-Amp repositories.

## mod_pagespeed 2.1 Integration

This is the Cyclone cache library, used as the storage layer for mod_pagespeed
2.1. It is built as a Bazel dependency (`@cyclone` in `MODULE.bazel`) via a
custom BUILD file at `third_party/cyclone.BUILD`. **When working on
mod_pagespeed 2.1, use Bazel (not CMake) to build and test.**

### How PageSpeed Uses Cyclone

- **Nginx module** uses the C API (`cyclone_c.h`) for cache reads/writes
- **Worker** uses the C++ API directly (`cyclone::` namespace)
- **PageSpeedCache** (`lib/cache/cache.h`) wraps Cyclone with alternate metadata
  encoding, capability mask selection, and content-type-aware freshness
- Cache keys are `SHA-256(URL, hostname)` with multiple alternates per key
- Cross-process sharing requires `enable_mmap_directory = true` on both processes

### Testing Cyclone Integration

```bash
bazel test //test/lib/cache/...       # PageSpeed cache wrapper tests
bazel test //test/lib/cache:cache_burst_test  # Cross-process stress test
```

### Cross-Process Model

See [doc/multi-process.md](doc/multi-process.md) for the cross-process model
(seqlock directory, CRC-32C torn-read detection). The phase-toggle and write-pos
CAS spinlocks (`phase_lock` / `write_lock`) are documented inline in
`src/core/mmap_directory.hpp`. The durable rules distilled from past
concurrency fixes live in "Concurrency invariants you must NOT break" below.

## License

Cyclone Cache is licensed under the Apache License 2.0.
See LICENSE for full terms.

All source files must include the Apache-2.0 SPDX header:
```cpp
// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
```

CI enforces this with an Apache RAT audit (`bash tools/ci/rat.sh`); files that
legitimately carry no header are listed, with a reason, in `.rat-excludes`.

## Project Overview

Cyclone Cache is a high-performance C++23 disk cache library inspired by Apache Traffic Server's cache design. It features scan-resistant caching, memory-mapped I/O, and a flexible plugin system.

**Key Technologies:**
- Language: C++23 (requires GCC 13+, Clang 16+, or MSVC 2022+)
- Build System: CMake 3.20+
- Testing: Catch2
- Dependencies: OpenSSL, or the bundled SHA-256 (`CYCLONE_USE_BUNDLED_SHA256`)

## Build Commands

```bash
# Configure and build
cmake -B build
cmake --build build

# Run all tests
ctest --test-dir build

# Run specific test
./build/cyclone-tests "CacheKey*"  # Run tests matching pattern

# Run benchmarks
./build/cache_benchmark 50 500 2048  # 50MB cache, 500 entries, 2KB each
./build/performance_baseline         # Detailed latency measurements

# Clean build
rm -rf build && cmake -B build && cmake --build build
```

The bare `cmake -B build` defaults to `find_package(OpenSSL REQUIRED)`. CI
instead builds hermetically with `-DCYCLONE_USE_BUNDLED_SHA256=ON`, which is
also required on machines without OpenSSL dev headers. See
`CYCLONE_USE_BUNDLED_SHA256` in `CMakeLists.txt` and the README build-options
table.

`CMakePresets.json` provides per-platform presets (`cmake --list-presets` shows
only the presets whose host condition matches the current OS; the full set is
enumerated below and in `CMakePresets.json`). Example:

```bash
cmake --preset macos-arm64 && cmake --build --preset macos-arm64 && ctest --preset macos-arm64
```

Configure presets include `linux-x64`, `linux-arm64`, `macos-arm64`,
`macos-x64`, `macos-universal`, `windows-x64`, `windows-arm64` (each with a
`-release` variant) plus `*-zig` cross-compile presets. Windows/vcpkg presets
need `VCPKG_ROOT`; the `*-zig` presets disable tests/examples/benchmarks.

## Reproduce CI locally

CI runs on GitHub-hosted runners only: a build-and-test matrix
(`ubuntu-latest`, `windows-latest`, `macos-latest`) that configures with
`cmake -B build -DCMAKE_BUILD_TYPE=Release -DCYCLONE_USE_BUNDLED_SHA256=ON`,
builds, and runs the test binary, plus an Apache RAT license-audit job
(`bash tools/ci/rat.sh`). The bundled-SHA256 build is hermetic: no OpenSSL or
vcpkg dependency. The reference local build, with the LLVM 20 toolchain the
project pins:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=clang++-20 \
  -DCYCLONE_USE_BUNDLED_SHA256=ON
cmake --build build -j$(nproc)
(cd build && ./cyclone-tests)
```

Run the test binary directly rather than `ctest -j`: the Catch2 cases share a
fixed on-disk temp path, so parallel test processes race on file sharing.

## Linting (required gate — match exactly)

Lint is a hard-failing contribution gate and requires LLVM 20 specifically
(`clang-format-20` / `clang-tidy-20` / `run-clang-tidy-20`, installed from
apt.llvm.org); other major versions produce gate-failing formatting even when
locally green. The exact commands:

clang-format check (and the `-i` fix variant):

```bash
# Check (what the gate runs):
find src include/cyclone tests benchmarks examples -type f \
  \( -name '*.c' -o -name '*.cpp' -o -name '*.hpp' -o -name '*.h' -o -name '*.mm' \
     -o -name '*.cu' \) \
  | xargs clang-format-20 --dry-run --Werror

# Fix in place:
find src include/cyclone tests benchmarks examples -type f \
  \( -name '*.c' -o -name '*.cpp' -o -name '*.hpp' -o -name '*.h' -o -name '*.mm' \
     -o -name '*.cu' \) \
  | xargs clang-format-20 -i
```

**One entry point — `tools/format.sh`.** Rather than invoking `clang-format`
directly, use `tools/format.sh` (`--check` to verify like the gate, `--changed`
for a fast changed-files check). It resolves a clang-format **20.x** binary — a
local `clang-format-20`, else a pinned `clang-format==20.1.8` pip wheel
bootstrapped into a shared cache venv (byte-identical to a stock
`clang-format-20`). If your `clang-format` is not **20.x**, do not format by
hand — run `tools/format.sh`. `pre-commit install` also wires a **pre-push**
gate (`clang-format-push-gate`) that runs this check on the files being pushed.

clang-tidy (needs `build/compile_commands.json`, emitted by
`CMAKE_EXPORT_COMPILE_COMMANDS=ON`, on by default in `CMakeLists.txt`):

```bash
cmake -B build -DCMAKE_CXX_COMPILER=clang++-20 -DCYCLONE_USE_BUNDLED_SHA256=ON
run-clang-tidy-20 -p build "cyclone/(src|include/cyclone|tests|benchmarks)/.*\.(cpp|cc)$"
```

The leading `cyclone/` in the tidy regex is intentional: it matches the
`cyclone/` source-tree directory segment in the absolute paths CMake writes
into `compile_commands.json` (e.g. a `…/cyclone/cyclone/…` checkout nested
inside a parent directory of the same name). Lint is a pure CMake build — no
Bazel involved.

## Project Structure

```
cyclone/
├── include/cyclone/          # Public API headers
│                             #   (run `git ls-files include/` for the
│                             #    authoritative list; key headers below)
│   ├── cache.hpp             # Main Cache class
│   ├── key.hpp               # CacheKey (SHA-256 based)
│   ├── handle.hpp            # ReadHandle, WriteHandle
│   ├── config.hpp            # CacheConfig, VolumeConfig
│   ├── error.hpp             # CacheError enum
│   ├── task.hpp              # Coroutine support
│   ├── alternate.hpp         # Alternate-chain metadata
│   ├── cyclone_c.h           # C ABI wrapper header
│   ├── detail/
│   │   └── expected_compat.hpp  # std::expected shim
│   └── plugin/               # Plugin system API
│       ├── plugin.hpp
│       ├── metadata.hpp
│       ├── alternate.hpp     # Alternate-selection plugin interface
│       └── optimization.hpp  # Background optimization plugin interface
├── src/
│   ├── core/                 # Core implementation (cache, volume, stripe,
│   │                         #   directory, document, write buffer)
│   ├── io/                   # Cross-platform mmap
│   ├── ram_cache/            # RamCache: CLFUS + LRU
│   ├── optimization/         # Background optimization engine
│   │                         #   (engine, adaptive_pool, load_monitor,
│   │                         #    work_queue)
│   ├── plugin/               # Built-in plugins (HTTP alternate selection)
│   └── c_api/                # C ABI wrapper implementation
├── tests/
│   ├── unit/                 # Unit tests
│   └── integration/          # Integration tests
├── benchmarks/               # Performance benchmarks
├── examples/                 # Usage examples
└── doc/                      # Documentation
```

## Subsystem Navigation Map

| Subsystem | Source | Public header | Tests | Doc |
|-----------|--------|---------------|-------|-----|
| Core (Cache/Volume/Stripe/Directory/Document/HitTracker) | `src/core/` | `include/cyclone/cache.hpp`, `handle.hpp`, `key.hpp`, `config.hpp` | `tests/integration/test_lockfree_read_races.cpp`, `test_power_loss.cpp`, `tests/unit/` | `doc/architecture.md`, `doc/multi-process.md` |
| IO (MappedFile/Executor) | `src/io/` | `include/cyclone/task.hpp` | `tests/unit/test_mapped_file.cpp` | `doc/architecture.md#memory-mapped-io` |
| RAM cache (CLFUS/LRU, segmented) | `src/ram_cache/` | internal: `src/ram_cache/ram_cache.hpp` | `tests/unit/test_clfus.cpp`, `test_lru.cpp` | `doc/architecture.md#ram-cache` |
| Optimization engine | `src/optimization/` | `include/cyclone/plugin/optimization.hpp` | `tests/integration/test_optimization.cpp` | `doc/architecture.md#background-optimization-engine` |
| Plugins (HTTP + custom) | `src/plugin/` | `include/cyclone/plugin/plugin.hpp` | `tests/unit/test_plugin.cpp`, `test_http_plugin.cpp` | `doc/plugin-development.md` |
| C ABI (+ async miss handler) | `src/c_api/` | `include/cyclone/cyclone_c.h` | `tests/unit/test_c_api.cpp` | `doc/api-reference.md#c-api` |
| Multi-process (mmap directory, stripe ownership) | `src/core/mmap_directory.hpp` | `include/cyclone/config.hpp` (`MultiProcessConfig`) | `tests/unit/multi_process_test.cpp`, `test_mmap_directory.cpp` | `doc/multi-process.md` |

## Architecture Notes

For load-bearing terms see the Glossary in
[doc/architecture.md](doc/architecture.md). Note especially that **Stripe and
Segment are the same thing** — there is exactly one partitioning dimension; the
Glossary's Stripe row carries the `num_segments` / `segment_hash()` details.

### Key Components

1. **Cache** (`cache.hpp/cpp`): Main entry point, manages volumes and plugins
2. **Volume** (`volume.hpp/cpp`): Manages stripes, handles reads/writes
3. **Stripe**: A partition of a volume with its own directory and write buffer
4. **Directory** (`directory.hpp/cpp`): Maps keys to offsets, 10-byte entries
5. **Document** (`document.hpp/cpp`): On-disk format with 132-byte header
6. **RAM Cache** (`ram_cache.hpp`, `clfus.cpp`, `lru.cpp`): In-memory caching
7. **VolumeHeader** (`volume.hpp`): 64-byte header at offset 0 for version tracking
8. **C API** (`cyclone_c.h`, `cyclone_c.cpp`): C ABI wrapper with miss callback hook and request coalescing
9. **Optimization engine** (`src/optimization/`: `optimization_engine`, `adaptive_pool`, `load_monitor`, `work_queue`; public interface `include/cyclone/plugin/optimization.hpp`): background generation of optimized alternates (e.g. compression, transcoding) after writes, via a load-aware adaptive thread pool

### Version Compatibility

Each cache volume file starts with a 64-byte `VolumeHeader` containing:
- Magic number (`0x43594C4E` = "CYLN")
- Format version (major/minor)
- Creation timestamp
- Volume size

On `Volume::open()`:
- If header is missing or invalid: automatically reset/reinitialize
- If major version differs: reset (or return `IncompatibleVersion` if `auto_reset_on_incompatible = false`)
- If only minor version differs: compatible, proceed normally

This ensures old cache data doesn't cause undefined behavior when the format changes.

### Thread Safety Model

- **Reads take no stripe lock** (lock-free hot path, from the read-path scaling series).
  Correctness comes from per-bucket seqlock directories, commit ordering
  (data durable before directory insert), CRC validation, and read
  leases that pin borrowed mmap regions against writer wraps
- The **only** stripe lock is the writer's exclusive lock in `commit_write`
- Directories (in-memory `Directory` and `MmapDirectory`) use per-bucket
  seqlocks — readers retry on version mismatch, never block
- RAM cache (CLFUS) is **segmented** (up to 64 independent segments, each with
  its own `shared_mutex`), so readers don't contend on one mutex
- Everything a reader touches is sharded per thread (read anchors, borrow
  shards, read counters, teardown gate, HitTracker) — see the sharding map in
  [doc/architecture.md](doc/architecture.md#concurrency-model) and
  [doc/multi-process.md](doc/multi-process.md)

### Concurrency invariants you must NOT break

The read hot path is lock-free (the read-path scaling series). Before refactoring
`src/core/`, preserve these — each is pinned by a test:

1. **Readers take no stripe lock.** `read_sync` uses the directory seqlock +
   CRC + read lease, never a reader-side stripe lock
   (the "Lock-free read" comment in `Volume::read_sync`, `src/core/volume.cpp`).
   Guard: `tests/integration/test_lockfree_read_races.cpp`.
2. **Commit ordering: data durable BEFORE directory insert**
   (the "ORDERING INVARIANT (power loss)" comment in `Volume::commit_write`).
   Guard: `tests/integration/test_power_loss.cpp`.
3. **Dekker handshake** — writer stores wrap_intent then loads
   borrow/lease; reader stamps borrow/lease then loads intent; all `seq_cst`
   (the wrap-intent Dekker proof in `Volume::allocate_write_slot`). Don't reorder or weaken the memory
   order. Borrow is released on ReadHandle **close**, not lease expiry.
4. **Per-bucket seqlock**: writers publish odd→even under the stripe mutex
   only; readers retry up to `kMaxReadRetries = 100`. Applies to BOTH
   `Directory` and `MmapDirectory` (`src/core/directory.hpp`,
   `src/core/mmap_directory.hpp`).
5. **HitTracker is a leaf lock** — never hold a stripe lock across
   `record_hit()` (the old AB/BA deadlock). 4096 stripes; key-striped so the
   pending bound stays exact.
6. **Per-thread sharding**: read anchors (64/volume), borrow shards
   (64/stripe), read counters (64), teardown gate (16), CLFUS segments (≤64)
   exist to keep readers off shared cache lines. Don't fold them back into a
   single lock or counter.
7. **Multi-process**: only the owner writes a stripe
   (`stripe_index % total_processes == process_index`). Per-write fsync is
   only taken when `sync_on_write` (default false) is explicitly set — it is
   deliberately no longer forced in multi-process mode (fsync convoy,
   since fixed); durability is the periodic DirectorySyncer.
   Guard: `tests/unit/multi_process_test.cpp`.
8. **Full-key re-verification on every tag match.** A `DirEntry` carries only
   a 12-bit tag and a 1-bit phase; the phase bit ABAs after two wraps of a
   stripe, so a surviving entry can alias reused space. Reads must re-verify
   the stored 256-bit `first_key` before serving, and in-place updates must
   elect their target by full key — an aliased entry then resolves to a clean
   miss, never a stale or foreign serve.
   Guard: `tests/integration/test_wrap_phase_aba.cpp`, `test_tag_collision.cpp`.
9. **Phase-ABA positional guard at BOTH read choke points.** A stale entry or
   chain pointer can survive two wraps and point at intact bytes sitting
   AHEAD of the write cursor; every other gauntlet leg passes (real bytes,
   key matches, CRC verifies, no wrap event), yet the ordinary forward fill
   later overwrites them in place with no wrap gate firing. Reads must reject
   at/ahead-of-cursor offsets on both the directory-probe and chain-hop paths
   (the "phase-ABA positional guard"; uses `shared_write_pos` in
   multi-process mode). Don't remove or weaken either leg.
   Guard: `tests/integration/test_wrap_phase_aba.cpp` (the forward-fill cases,
   `[!mayfail]` until the guard fixed them).

### Error Handling

All fallible operations return `std::expected<T, CacheError>`. Never use exceptions.

```cpp
auto result = cache->read_sync(key);
if (!result) {
    // Handle result.error()
}
```

## Development Guidelines

### Code Style

- Google-based style enforced by `.clang-format` (`BasedOnStyle: Google`,
  `ColumnLimit: 80`, `PointerAlignment: Left`, `Standard: Latest`), 2-space
  indent. `.clang-format` is the single source of truth.
- 80 columns, not 120. Run `clang-format-20 -i` before committing — the
  private contribution gate rejects non-conforming code via
  `clang-format-20 --dry-run --Werror` (see the Linting section above for the
  exact commands).
- Naming: classes `CamelCase`, functions/methods `snake_case`, member
  variables `_prefix`, constants `kCamelCase`.
- Use `std::expected` for error handling, not exceptions
- Prefer `std::span` over raw pointers
- Use `noexcept` for simple getters

### Performance Considerations

- Avoid allocations in hot paths (use `probe_each` instead of `probe_all`)
- Use shared locks for read operations
- Pre-allocate buffers when size is known
- RAM cache is checked before disk
- Memory-mapped I/O for disk reads

### Adding New Features

1. **New cache operation**: Add to `Cache` class, implement in `Volume`
2. **New plugin hook**: Extend `CachePlugin` interface
3. **New config option**: Add to `CacheConfig` or `VolumeConfig`

### Security Considerations

- Validate all sizes before allocation (see `kMaxSectionCount`, `kMaxSectionSize`)
- Add cycle detection for chain traversal (see `kMaxChainDepth`)
- Check for integer overflow in buffer operations
- Use RAII for resource cleanup (handles abort in destructor)

## Testing

### Test Categories

Tests are tagged by category (e.g. `[directory]`, `[security]`, `[lru]`,
`[http]`, `[alternate]`, `[concurrent]`, `[c_api]`). List the live tags with
`./build/cyclone-tests --list-tags`, then run a category by tag:

```bash
./build/cyclone-tests "[security]"   # run one category
```

### Writing Tests

- Use Catch2 TEST_CASE macro
- Tag tests appropriately: `[component]`, `[security]`, `[edge]`
- Test both success and error paths
- Include security tests for deserialization

```cpp
TEST_CASE("Description", "[tag1][tag2]")
{
  // Arrange
  // Act
  // Assert with REQUIRE()
}
```

### Running Sanitizer Tests

The suppressions files (`tools/{tsan,lsan}_suppressions.txt`) carry known false
positives (seqlock directory access, intentional shutdown thread leak), so a
sanitizer run must wire them in to stay signal-only; without them you see
noise that the pinned configuration does not.

```bash
# ASan + UBSan (memory errors, undefined behavior)
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=clang++-20 \
  -DCYCLONE_USE_BUNDLED_SHA256=ON \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-sanitize=vptr -g" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-asan -j$(nproc)
(cd build-asan && LSAN_OPTIONS="suppressions=$PWD/../tools/lsan_suppressions.txt" ./cyclone-tests)

# TSan (data races)
cmake -B build-tsan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=clang++-20 \
  -DCYCLONE_USE_BUNDLED_SHA256=ON \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -g" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
cmake --build build-tsan -j$(nproc)
(cd build-tsan && TSAN_OPTIONS="suppressions=$PWD/../tools/tsan_suppressions.txt" ./cyclone-tests)
```

`-fno-sanitize=vptr` avoids spurious UBSan vptr reports. For a quick local
ASan build without reproducing the exact flags above, the one-flag
`-DCYCLONE_ENABLE_ASAN=ON` (`CMakeLists.txt`) is simpler.

## Common Tasks

### Adding a New Configuration Option

1. Add field to `CacheConfig` or `VolumeConfig` in `include/cyclone/config.hpp`
2. Add builder method if using fluent API
3. Use the option in the relevant component
4. Document in README.md

### Optimizing a Hot Path

1. Run benchmarks to establish baseline: `./build/performance_baseline`
2. Profile with appropriate tools (perf, Instruments)
3. Consider: reducing allocations, using shared locks, pre-allocation
4. Run tests after changes: `ctest --test-dir build`
5. Re-run benchmarks to verify improvement

### Debugging

Enable debug output: drop a temporary `std::cerr << ... << std::endl;` in the hot path.

Run specific test with verbose output:
```bash
./build/cyclone-tests "Test name" --reporter console
```

## Performance Baselines

Measured 2026-09-23 at commit `87cd986` (Apple M5, 10 cores, macOS 27.0,
Release build, bundled SHA-256, single thread except where noted, 4 KB
objects, median of three runs;
`performance_baseline --cache-size 512 --entries 5000 --content-size 4096`):

| Operation | Throughput | Notes |
|-----------|------------|-------|
| Key generation | 2.9-4.8M ops/sec | SHA-256 hashing, 0.2-0.3 µs |
| Write (4KB) | 229K ops/sec | p50 2.6 µs, p99 21 µs; varies up to 6× run to run |
| Read (first-touch) | 1.18M ops/sec | p50 0.67 µs — page-in + CRC-32C |
| Read (warm, random) | 2.3M ops/sec | p50 0.38 µs |
| Exists check | 4.1M ops/sec | 0.21 µs; directory lookup only |
| Cache miss | 3.4M ops/sec | 0.29 µs; fast path |

`read_sync` never populates the RAM tier, so the "warm" reads above are served
from the mapped file (OS page cache), not the RAM cache; first-touch reads pay
page-in plus CRC-32C verification (hardware on ARMv8 and x86-64 with
SSE4.2).

Read scaling (`concurrent_read_bench 20000 512 2 0 512 ramoff` — 512 B objects,
RAM tier off; a different harness, not comparable to the table above):

| Threads | Reads/sec |
|---------|-----------|
| 1 | 5.3M/s |
| 4 | 17.6M/s |
| 16 | 21.1M/s |

## Resources

External: Apache Traffic Server cache — https://trafficserver.apache.org/

## Documentation map

Start here, in order: `CLAUDE.md` (this file) → `doc/architecture.md` (living
reference, includes the Glossary) → the relevant subsystem doc below.

Status legend: **Current** = authoritative living reference; **Design/plan** =
design record for shipped work (background, not a task list);
**Historical/fix-log** = completed-work record, not current spec.

| Doc | Role | Status |
|-----|------|--------|
| [README.md](README.md) | Quick start, API overview, build options | Current |
| [CONTRIBUTING.md](CONTRIBUTING.md) | Dev setup, style, PR process | Current |
| [doc/architecture.md](doc/architecture.md) | Internal architecture + Glossary | Current |
| [doc/api-reference.md](doc/api-reference.md) | API documentation | Current |
| [doc/plugin-development.md](doc/plugin-development.md) | Plugin guide | Current |
| [doc/multi-process.md](doc/multi-process.md) | Cross-process stripe affinity and locking | Current |
| [doc/kv-cache-benchmark.md](doc/kv-cache-benchmark.md) | LLM KV-cache tier benchmark vs LMDB, RocksDB, file-per-block; results by round, what to change | Current (Summary); rounds are dated records |
| [doc/kv-cache-benchmark/kv-workload-spec.md](doc/kv-cache-benchmark/kv-workload-spec.md) | Workload spec for rounds 1–3b | Current (v1.1) |
| [doc/kv-cache-benchmark/kv-churn-spec.md](doc/kv-cache-benchmark/kv-churn-spec.md) | Bounded-capacity churn spec and decision criteria (rounds 4–5) | Current (v1) |
| [doc/design/wrap-retention.md](doc/design/wrap-retention.md) | Keep the previous pass readable past a wrap (clean frontier, pass stamp) | Design/plan — implemented, default off |
| [doc/design/verified-state.md](doc/design/verified-state.md) | Share and persist the CRC-verified verdict across processes and restarts (per-stripe token table, nonce, seal) | Design proposal — not implemented |

Historical fix-logs are archived separately from the living docs — they record
completed work and are not current spec.
