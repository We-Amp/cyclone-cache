# Changelog

All notable changes to Cyclone Cache are documented in this file. The format is
based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added

- `CacheConfig::readahead_min_bytes` (and `VolumeConfig::readahead_min_bytes`,
  fluent `set_readahead_min_bytes()`): a readahead hint over exactly a
  document's byte range on the disk read path (`read_sync` and the
  selected-alternate path) for documents of at least this many bytes,
  issued before the CRC pass first touches the content. Default 256 KiB;
  `0` disables it. Not exposed in the C API. Per platform:
  - Linux: `madvise(MADV_WILLNEED)`, page-aligned and in 512 KiB chunks.
  - macOS: `fcntl(F_RDADVISE)` over the file range (Darwin's
    `MADV_WILLNEED` is synchronous and serialises across processes).
  - Windows: `PrefetchVirtualMemory` (previously a no-op).
  A placement is re-advised at most once per 2 s, so warm re-reads do not
  pay for the hint.
- `CacheStats::readahead_hints_issued` (also on `VolumeStats`): readahead
  hints issued, appended at the end of `CacheStats`. C++ only; not mirrored
  in `CycloneCacheStats`.
- The Windows build now imports `PrefetchVirtualMemory` from `kernel32`, so
  the library requires Windows 8 / Windows Server 2012 or later when built
  with `_WIN32_WINNT >= 0x0602`.
- `crc32c_bench` benchmark target: times every CRC-32C implementation
  (byte-wise reference, portable slice-by-16, hardware single-stream and
  3-way interleaved, and whatever runtime dispatch selected) over 4 KiB,
  64 KiB and 2 MiB buffers, and prints the selected path.
- KV-cache storage-tier benchmarks, built with `CYCLONE_BUILD_BENCHMARKS`
  (results and method in `doc/kv-cache-benchmark.md`):
  - `kv_bench`: the workload in `doc/kv-cache-benchmark/kv-workload-spec.md`
    (put, first-touch get, warm Zipf get in `view` / `copy` mode, restart,
    multi-process read) over 512 KiB-32 MiB blocks; `--drop-caches-cmd`,
    `--readahead-min-bytes` and `--pause-before-warm` for cold-cache and
    readahead runs, and `readahead_hints_issued` printed per cold phase.
  - `kv_churn`: a bounded-capacity tier under churn
    (`doc/kv-cache-benchmark/kv-churn-spec.md`), get-or-insert against a full
    volume, reporting hit ratio, served bandwidth, latency and Linux
    device/cgroup counters.
  - `kv_churn_policy`: replays the churn key streams through LRU, FIFO and
    Cyclone's wrap eviction without I/O, to attribute hit-ratio differences.
  - `kv_gpu_metal` (Apple only): host-to-GPU transfer of stored blocks on
    Metal, including a single wrap of the whole volume mapping.
  - `kv_gpu_cuda`, behind the new `CYCLONE_BUILD_CUDA_BENCHMARKS` option
    (default `OFF`, not supported on Windows): the same experiment over
    PCIe with `cudaHostRegister`.

### Changed

- **On-disk format v8 (was v7): the document checksum is CRC-32C
  (Castagnoli, reflected poly `0x82F63B78`) instead of CRC-32/ISO-HDLC.**
  The checksum used to be a byte-at-a-time ISO-HDLC table inside
  `document.cpp`, at about 0.5-0.6 GB/s, and it is re-verified on every cold
  read, so it capped cold-read bandwidth. It now lives in
  `src/core/crc32c.{hpp,cpp}` (`crc32c()` / `crc32c_update()`), and CRC-32C
  has a hardware path on x86-64 (SSE4.2 `crc32q`) as well as on ARMv8
  (`crc32cx`). Both hardware paths run a 3-way interleave: 34.9 GB/s over
  2 MiB on an Apple M5 (12.2 GB/s single-stream), 26.5 GB/s on an
  i7-8750H (10.4 GB/s single-stream); every other target runs a portable
  slice-by-16 table path at 3.4 GB/s (M5) / 2.8 GB/s (i7), against 0.61 /
  0.50 GB/s for the old byte-wise table. End to end on Linux (i7-8750H),
  the 2 MiB cold read goes from 1.6 GB/s, measured against an interim
  slice-by-16 CRC-32, to 2.2 GB/s, level with LMDB
  (`doc/kv-cache-benchmark.md`). The path is picked by a runtime CPU probe,
  resolved once, on first use.
  **Existing cache files are abandoned (not deleted) on open.** The format
  major is mixed into the fingerprinted volume filename, so a v8 binary
  resolves to a different file and starts cold rather than failing to verify
  v7 documents; the superseded v7 file stays on disk until
  `gc_superseded_on_start` (POSIX only) or a manual delete reclaims it.
- **Bazel and vendored consumers:** add the new sources
  `src/core/crc32c.cpp` and `src/core/crc32c.hpp` to any downstream BUILD
  file that lists srcs explicitly. No new copts or defines are needed: the
  hardware paths use per-function target attributes and runtime dispatch,
  so the stock compiler flags still build every path.
- C API: `cyclone_c.h` now compiles as C11 as well as C++. `CycloneError`
  and `CycloneTier` are `typedef uint8_t` plus an anonymous enum of their
  constants (previously C++ `using X = enum : uint8_t`), so the ABI is
  unchanged. From C++ this is observable: they are no longer distinct
  types, so overloads on them collide with `uint8_t`, and streaming one
  with `<<` prints a character rather than a number.
- License: Apache License 2.0 (was BUSL-1.1). See `LICENSE` and `NOTICE`.
- License: every file carries an Apache-2.0 SPDX header or is a documented
  exception, verified with Apache RAT.

### Fixed

- Multi-process: a wrap now lowers the shared write cursor to the data-area
  start inside the wrap-intent window instead of when its first write commits.
  Before, for the whole reservation-to-pwrite window of a wrapping write, a
  reader in any process could pass the phase-ABA positional guard with a
  two-wrap survivor at the wrap target and take a borrow that the pwrite then
  tore. A wrap whose first write failed also left the cursor high, so the next
  write wrapped again (a second phase toggle with no pass in between).
- HitTracker: the flush thread slept in fixed 100 ms slices, so a
  `CacheConfig::hit_flush_interval` below 100 ms flushed late (a 50 ms
  interval flushed after at least 100 ms). It now sleeps
  `min(100 ms, remaining)`.
