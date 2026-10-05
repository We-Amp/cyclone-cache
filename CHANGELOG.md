# Changelog

All notable changes to Cyclone Cache are documented in this file. The format is
based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Fixed

- A process that forked while a cache was running could leave the child
  blocked for good. The parent's background threads take in-process locks
  while they work (the hit-count flush sweeps the hit-tracker stripe
  mutexes, the directory sync holds a shard of the cache gate for its
  msync/fsync sweep), and a child forked at that instant inherited the lock
  held by a thread it does not have. Such a child blocked on its first read
  of a key that hashed to that stripe, or, at exit, in the destructor after
  an explicit `stop()`. This affects forking servers that open the cache in
  the parent; a blocked worker did not go away on a graceful restart. Now `fork()` waits (in the library's `pthread_atfork`
  handler) for a background pass in flight and holds new ones off until it
  returns, and `stop()` in a forked child marks the inherited `Cache`
  finished, so a later `stop()` or the destructor returns without taking a
  lock. Update recommended for forking servers.
  - Behaviour: `fork()` can now wait for one background pass, normally
    microseconds and at most as long as an fsync of the volume (or a
    plugin's transform when the optimization engine is working). No timeout.
  - Behaviour: in a forked child that inherited a running cache,
    `optimization_engine()` returns `nullptr` from the fork on (it used to
    return a pointer to an engine whose threads were not there, until
    `stop()`), and `start()` after `stop()` returns `CacheError::Closed`
    (it used to fail with `AlreadyOpen` and close the volumes).
  - No API, on-disk format or shared-mapping change. The fork contract is
    now written down on `Cache::start()` / `Cache::stop()`, in
    `cyclone_c.h` and in the README ("Forking with an open cache").
    Windows is unaffected (no fork).

### Added

- `CacheConfig::for_kv_tier()`: a static factory returning the recommended
  configuration for an LLM KV-cache tier, as round 7 of the KV benchmark
  recommends. It is a default `CacheConfig` with exactly
  `fill_large_document_tail` and `write_behind` turned on; wrap retention,
  the readahead settings and every other field keep their defaults, and
  sizing stays the caller's. Measured in round 7 (Linux, 2 MiB churn, four
  threads, same-day LMDB): hit p99 about 0.19× LMDB's on both patterns,
  served 1.55–1.66× LMDB's. Costs: hit p50 +6–22 %, bulk-load PUT
  throughput −18–31 %, `write_behind` Linux-only (a no-op elsewhere), and
  the write-back call can block the writer under device congestion. Not for
  event-loop callers such as nginx. Library defaults are unchanged,
  PageSpeed is unaffected, and there is no C API equivalent (neither option
  is in the C API). `kv_bench` and `kv_churn` gained `--preset default|kv`
  (the explicit `--fill-tail` / `--write-behind` flags still override it),
  and record the preset in their JSON output (`preset` / `cy_preset`). See
  doc/api-reference.md, "KV-Tier Preset".
- `CacheConfig::write_behind` (and `VolumeConfig::write_behind`, fluent
  `set_write_behind()`), opt-in, default off (issue #43). When on, after a
  document above 64 KiB is committed, the writing thread starts the
  kernel's write-back of the document up to its last whole page (Linux
  `sync_file_range(SYNC_FILE_RANGE_WRITE)`; no wait, not a durability
  point), after the stripe mutex and the cross-process write lock are
  released; both commit paths. Elsewhere it does nothing. Not persisted and
  not in the C API. New `CacheStats::write_behind_ranges` /
  `write_behind_us`. Measured with `fill_large_document_tail` on the Linux
  benchmark machine (2 MiB churn, saturated NVMe, four threads): cgroup
  dirty data 300 → 8 MiB, miss+insert p99 20.8 → 11.0 ms (`zipf`) and
  21.9 → 12.5 ms (`zipf+scan`) against the fill alone, hit p99 24.8 → 8.4
  and 29.6 → 10.5 ms, and the churn latency clause is met on both patterns
  (0.22× and 0.25× LMDB). Hit p50 rises 10–21 %. Can block while the
  device queue is full, so not for event-loop callers. See
  doc/design/writer-admission-control.md and doc/kv-cache-benchmark.md,
  "Writer admission". `kv_churn` gained `--fill-tail on|off` and
  `--write-behind on|off`.
- `CacheConfig::fill_large_document_tail` (and
  `VolumeConfig::fill_large_document_tail`, fluent
  `set_fill_large_document_tail()`), opt-in, default off (issue #35). When
  on, a document above 64 KiB is written together with zeros up to the next
  4 KiB boundary of the file, clamped to the data area and, with wrap
  retention, to the clean frontier, all in one `pwritev`. Its write then
  never covers part of a page, so ext4 and XFS no longer read that page
  from the device inside the write. Documents stay packed and the cursor,
  capacity and on-disk format are unchanged; the alternate write path fills
  its tail the same way. Not persisted and not in the C API. With it off,
  writes are byte-for-byte and syscall-for-syscall as before. Measured on
  the Linux benchmark machine (2 MiB churn, saturated NVMe): the insert
  p99 roughly halves (26 → 15 ms at one thread, 57 → 30 ms at four), but
  the hit p99 at four threads rises 25 % (`zipf`) and 68 % (`zipf+scan`).
  The removed read was acting as writer admission control under device
  saturation. For write-latency-sensitive, insert-heavy workloads; see
  doc/kv-cache-benchmark.md, "Insert tail". Follow-up: #43.
- `kv_churn --ops-per-second R`: paces each thread's measured phase as a
  Poisson process of R operations per second, so two trees can be compared
  at the same offered load.

- Cold-read readahead below the large-document threshold, and a
  sequential window (issue #29). `CacheConfig::cold_readahead_min_bytes`
  (default 16 KiB, `0` = off) and `CacheConfig::sequential_readahead_bytes`
  (default 1 MiB, `0` = off), mirrored on `VolumeConfig`, with fluent
  setters. On a read that is about to run the CRC pass (the first read of
  a document incarnation in this process), a document of at least 16 KiB
  and below `readahead_min_bytes` gets a readahead hint over its own range,
  and a per-thread, per-stripe detector extends the hint by up to 1 MiB past
  the document when the read continues where that stripe's previous read
  ended (a KV prefix read back in insertion order). Validated warm re-reads
  never reach either hint. An existing mmap directory also gets one
  readahead hint per stripe at open, so a restart with a cold page cache no
  longer faults it in page by page. A first read of a document written
  within the stripe's last 4 MiB of puts (write, then serve on the next
  request) skips the hint with no syscall, as does a sequential run that
  catches up with a writer still appending. Cold 64 KiB reads on the Linux benchmark
  machine: 0.04 → 2.6 GB/s (2.0× a same-day LMDB); 512 KiB 1.6 → 2.9 GB/s.
  `CacheStats::cold_readahead_hints`, `sequential_readahead_hints` and
  `recent_write_hint_skips` count the hints and the skips (C++ only, appended at the tail). Internal:
  `MappedFile::range_resident()` and `advise_willneed_unchecked()`, and
  `Stripe::index`. Not in the C API.
- `WriteHandle::reserve(length)`: appends `length` bytes to the object and
  returns them as a writable span, so a producer generates the content in
  the handle's buffer instead of in its own buffer that `write_sync()` then
  copies. Mixes with `write_sync()`; the checksum is taken at close; an
  abort after a partial fill publishes nothing; the `write_sync()` limits
  apply. Additive: `WriteHandleImpl` gains a virtual with a default body,
  appended after its existing members. Not in the C API, which has no
  streaming write handle.
- `insert_bench` benchmark target: one put split into key / open / write /
  commit, over a first lap and steady-state laps, with page faults per
  insert (`--reserve` for the `reserve()` path). `kv_churn --reserve`
  inserts through `reserve()`.

- `CacheConfig::readahead_min_bytes` (and `VolumeConfig::readahead_min_bytes`,
  fluent `set_readahead_min_bytes()`): a readahead hint over exactly a
  document's byte range on the disk read path (`read_sync` and the
  selected-alternate path) for documents of at least this many bytes,
  issued before the CRC pass first touches the content. Default 256 KiB;
  `0` disables it. Not exposed in the C API. Per platform:
  - Linux: `madvise(MADV_WILLNEED)`, page-aligned, in 64 KiB chunks over
    the first 4 MiB of the document and 512 KiB chunks after that (a cold
    512 KiB read is about 2x faster than with uniform 512 KiB chunks), and
    skipped when `mincore()` reports every page of the range resident.
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
- **Wrap retention** (`CacheConfig::wrap_retention`, default `true` since
  the change below; C API `disable_wrap_retention`). In flush mode a
  stripe's wrap drops its whole previous pass at once. With retention on,
  the previous pass stays readable
  until its bytes are needed: a clean frontier moves ahead of the write
  cursor one chunk at a time. A frontier advance waits only for live borrows
  in the chunks it crosses, and each document is stamped with its pass, so a
  stale entry from two or more wraps back can never resolve. In a Linux
  KV-churn run (2 MiB blocks, 4 GiB tier, 4 threads) the hit ratio rose from
  0.726 to 0.788 on Zipf and from 0.614 to 0.660 on Zipf with scans. The
  mode is persisted per volume (`VolumeHeader::retain_chunks`), and an open
  in the other mode resets through the live-peer gate, so every process
  sharing a cache must use the same setting. New counters in `CacheStats`
  and at the tail of `CycloneCacheStats`: `frontier_advances`,
  `advances_deferred_by_lease`, `early_advances_skipped`, `retained_hits`,
  `stamp_rejections`. See `doc/api-reference.md#wrap-retention` and
  `doc/design/wrap-retention.md`.
  It first shipped off by default. The one gap that kept it off, the
  PageSpeed `cache_burst_test`, is resolved (see "Wrap retention is the
  default" under Changed).
- **Wrap retention: alternate carry-forward.** An alternate write whose
  chain head is retained no longer drops the key's other alternates (review
  R4). A chain may still not link across a pass. Instead the write rewrites
  the surviving alternates as current-pass documents in its own slot, below
  the new head, and publishes all of them with one directory insert. A
  crash at any step leaves either the old retained chain or the new complete
  chain. For PageSpeed that means the Original, Gzip and WebP keep resolving
  when the optimization engine adds an AVIF after a wrap. A carry happens at
  most once per key per pass. It keeps at most `kMaxAlternatesPerKey - 1`
  alternates and `min(A / 8, max_object_size)` bytes, the Original first,
  then the newest. A write whose allocation wraps while it links a live
  chain retries once and carries that chain, where it used to refuse the
  link. Flush mode is unchanged. New counters in `CacheStats` and at the
  tail of `CycloneCacheStats`: `alternates_carried_forward`,
  `alternate_carry_bytes`, `alternates_carry_dropped` (appended, so C
  consumers must be rebuilt against the new header, as for the earlier
  tail fields). `alternate_wrap_refusals` no longer moves in retention
  mode. See
  `doc/design/wrap-retention.md` sections 4.5 and 14.
- `performance_baseline --wrap-retention on|off`, a `retain|flush` argument
  for `concurrent_read_bench`, and `kv_churn --wrap-retention on|off`, which
  adds the retention counters to its JSON. `kv_churn_policy` gains
  `retain/16`, `retain/32`, `retain/64` and `retain/256` columns, the
  retention policy replayed without I/O.
- `CYCLONE_BUSY` C API error code, appended after `CYCLONE_OBJECT_TOO_LARGE`
  (existing codes keep their values). It maps `CacheError::Busy`, which the
  C API used to report as `CYCLONE_INTERNAL_ERROR`, so a C caller that got
  `CYCLONE_INTERNAL_ERROR` from a contended write, delete or hit-count update
  now gets `CYCLONE_BUSY`.
- `CacheStats::directory_read_timeouts` (also on `VolumeStats`, and appended
  to `CycloneCacheStats`): directory probes that spent the whole seqlock wait
  budget because a writer held the key's bucket. Expected near 0. Appending
  it grows `CycloneCacheStats`, and `cyclone_cache_stats()` writes the whole
  struct, so a C caller built against an older `cyclone_c.h` passes a
  smaller buffer: rebuild every consumer against this header (the same
  lockstep rule as the earlier tail appends).

### Changed

- **The background optimization engine is off by default and
  embedder-driven** (issue #52). `OptimizationConfig::enabled` now defaults
  to `false`. `OptimizationEngine::on_write_complete()` has no caller in
  Cyclone (`Volume::commit_write` / `Cache` never invoke it) nor in any known
  embedder, so with the old default every `Cache::start()` spun up a
  `LoadMonitor` thread and `min_threads` `AdaptiveThreadPool` workers per
  cache instance that idled forever, and any registered `OptimizationPlugin`
  was unreachable. With the engine off, `Cache::optimization_engine()`
  returns `nullptr` and `start()` starts no engine threads; the fork-safety
  and two-phase-stop paths already handle the absent engine. Migration for
  an embedder that wants background optimization: set
  `optimization_config.enabled = true`, register plugins on
  `cache->optimization_engine()`, and call
  `cache->optimization_engine()->on_write_complete(...)` after each of your
  own writes; nothing in Cyclone calls that hook. Code that already
  called `cache->optimization_engine()->...` (for example
  `register_plugin`) without setting `optimization_config.enabled = true`
  now dereferences a null pointer: set `enabled` first, or null-check the
  accessor. The `OptimizationConfig` layout is unchanged, and the C API
  has no optimization settings.
- **Plain writes no longer assemble the document in a second and third
  buffer** (issue #16). `commit_write` used to copy the content into the
  document builder and again into one contiguous document, two fresh heap
  buffers per put whose page faults and frees cost more than the copies:
  at 2 MiB on the Linux benchmark machine they were about 1.9 of the
  2.5 ms a put took. It now builds only the 132-byte header plus the
  caller's header bytes, with the CRC-32C chained over header bytes and
  content, and writes the content from the handle's buffer right behind
  it. Objects up to 64 KiB are still joined behind the head and written
  with one `pwrite`, where the copy is cheaper than a second syscall. The
  bytes on disk are unchanged, and so is the commit order (the whole fill,
  then `sync_on_write`'s fsync, then the directory insert). The alternate
  write path is unchanged.
- **Wrap retention is the default** (`kDefaultWrapRetention = true`, so
  `CacheConfig::wrap_retention` and `VolumeConfig::wrap_retention` default
  to `true`, and a zero-initialised `CycloneCacheConfig` retains). Flush is
  the opt-out: `wrap_retention = false`, or `disable_wrap_retention = 1` in
  the C API. Every gate of design decision D1 passed first: the design and
  review tests, the full suite and TSan in both modes on macOS and Linux,
  and the PageSpeed `cache_burst_test` 200/200 with retention forced on
  (40/40 under TSan), with review item R4 resolved by the alternate
  carry-forward and the read-miss regression by the same-key publish retry.
  **Upgrade impact.** A volume created with the default config by any
  earlier build records flush (`VolumeHeader::retain_chunks = 0`). The mode
  is not part of the fingerprinted filename, so this build opens the same
  file and finds a mode mismatch:
  - single process, or every old process stopped first: the volume is reset
    cold in place on the first open. All entries are lost once; no second
    file is created.
  - a process of the other mode still holds the file (an overlapping
    multi-process upgrade, or a peer that opts out): `Cache::start()` fails
    with `ResetRefusedLivePeer` (`CYCLONE_RESET_REFUSED_LIVE_PEER` from
    `cyclone_cache_create`) until that process exits. Stop every old
    process before starting new ones.
  - `auto_reset_on_incompatible = false`: `IncompatibleVersion`.
  To keep an existing cache warm, set `wrap_retention = false` on every
  process. Volumes configured with `wrap_retention = true` before this change
  are unaffected. The filename, the format version and the C ABI do not
  change. See `doc/api-reference.md#wrap-retention`.
- **Mmap directory version 2** (`MmapDirectory::kVersion`). A retention
  region after the directory entries holds the stripe's exposure generation
  and 64 per-chunk borrow slots. It replaces the stripe-wide borrow slot and
  the `{wrap count, phase}` reader epoch. The version is mixed into the
  fingerprinted filename of mmap volumes, so **multi-process caches start
  cold** after the upgrade. The old file is left on disk. The data offset
  does not move (177 pages at the default 16384 buckets).
- Borrows are counted per frontier chunk: 64 `u32` slots per thread shard
  locally (two 128-byte lines), 64 `u32` slots in the mmap directory. In flush mode the stripe is
  one chunk, so the lease gate behaves as before.
- The GC phase is derived from the pass (`P & 1`) rather than toggled. The
  writer stamps every document with its pass in both modes
  (`Document::write_serial`, outside the CRC). Flush mode ignores the stamp.
- `CycloneCacheConfig` gains a trailing `int disable_wrap_retention`. As with
  earlier trailing fields, callers must be recompiled against the new
  header.
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

- Two processes writing alternates of one key at the same moment could lose
  one of them. An alternate write resolves the key's chain before it writes
  and publishes afterwards; when another process published or removed the
  same key in between, the write published a second directory entry for the
  key, and the next write of that key kept one chain and cleared the other,
  dropping every alternate only that chain held, with no error reported.
  Removing a key's head alternate republished its successor the same way.
  Both publishes are now conditional: when the key's directory entries are
  no longer the ones the operation resolved, nothing is published and the
  operation resolves again. So a concurrent write or head removal can no
  longer silently drop a stored alternate or leave a key with two heads --
  once every process sharing the volume runs this version (a process on an
  older build still publishes unconditionally; newer writers clear the
  second entry at their next write of the key, as before), and apart from
  the wrap-retention limit named below. A write resolves again until it is published, for any number of concurrent
  writers: each round it loses is another writer's completed operation, so
  the writers always make progress together, and the delay a write can see
  is bounded by the other writers' completions, not by a time limit. A
  write with a current head that notices the change before writing (the
  usual case under contention) only walks the chain again; one that notices
  it at the publish writes its document a second time. A head removal
  resolves again within its existing bound of eight attempts and then
  reports `Busy`, having removed nothing. `VolumeStats` gains
  `alternate_publish_retries` and `alternate_publish_rewrites`. Without
  concurrent writers of one key the added cost is one more pass over the
  key's four-entry directory bucket per write, and one more lock-free read
  of that bucket. No on-disk change. The plain (non-alternate) write path
  is unchanged. Not changed, and still not coordinated with a concurrent
  write of the same key: removing an alternate from the middle or the tail
  of a chain, and the unlinking of superseded copies after a publish. Such
  a removal can report success and leave the alternate listed, or have it
  listed again; no other alternate is dropped by it. Known limit, with wrap
  retention only: a write that carries a retained chain forward and whose
  publish is refused because an unrelated entry with the same tag appeared
  in the bucket meanwhile starts over without the carry, so the retained
  alternates are dropped (a cache loss, never a wrong serve). New
  regressions: `tests/integration/test_alternate_publish_race.cpp` and a
  carry case in `tests/integration/test_wrap_retention.cpp`.

- A process may exit with `cyclone::Cache` objects still open (an embedder
  that never calls `cyclone_cache_destroy`). The process-wide liveness mutex
  (`liveness_mutex()` in `src/core/mmap_directory.cpp`) is now immortal: a
  writer waiting on the cross-process write lock at exit locked it after its
  static destructor had run, which terminated the process on Apple's libc++.
  The rest of that exit path was audited safe; see "Process exit with open
  caches" in doc/architecture.md and the new regression
  `tests/integration/test_exit_with_open_cache.cpp`.

- RAM tier: a served alternate version could go backwards by one step for
  a few microseconds around a re-record. The read path's conditional RAM
  put re-checked only the stripe's remove generation, which the commit
  bumps AFTER publishing the new head; a reader that walked the old head
  and landed its put inside that publish→bump window passed the re-check
  and replaced the newer copy another reader had just admitted, so RAM hits
  served the superseded version until the commit's eviction healed it (CI:
  one 2207→2206 dip on a contended macOS runner; reproduced locally only
  under CPU load, 2 in 400 runs). The put's predicate now also re-checks the
  key's directory bucket version, which moves at the publish, in
  single-process mode as well (it already did under
  `cross_process_ram_coherence`); such puts are rejected and counted in
  `ram_coherence_put_rejections`. No superseded copy ever outlived the
  eviction before either; what changes is per-reader monotonicity, which
  the concurrency test asserted and a new seam-driven test now pins
  deterministically (`WriterSeam::kAfterPublish`). The residual is precise:
  a copy admitted BEFORE the publish may still be served by RAM hits until
  the committer's eviction (a read concurrent with a write still in flight,
  so still linearizable), while no copy of the pre-publish chain can be
  admitted AFTER the publish. One extra acquire load per
  `read_alternate_sync`, of the bucket word the probe loads next anyway.

- Every descriptor Cyclone opens on its cache files is now close-on-exec
  (`O_CLOEXEC`; `_O_NOINHERIT` on Windows CRT opens). A child the
  application exec's (for example a fetcher helper) no longer inherits the
  volume, which pinned the file and kept its byte-range locks alive: a dead
  writer's liveness slot read as held until the helper exited, delaying
  write-lock recovery to the 5 s escalation. `fork()` still shares the
  descriptors, so a cache opened before forking workers is unaffected.

- A multi-process writer no longer decides that a write-lock holder is dead
  from its PID (issue #32). Across PID namespaces (two containers sharing a
  volume) `kill(pid, 0)` reported a live holder in the other namespace as
  gone, or a dead one as alive when its PID named an unrelated process, and
  taking the lock from a live holder overlaps two writes that readers cannot
  detect.
  - Each process now claims one of 251 liveness slots on the volume file
    (bytes `0x7FFFFFFE00000000 + slot`) and holds a byte-range lock on it
    (fcntl OFD locks on Linux and macOS, `LockFileEx` on Windows). The
    kernel drops it when the process dies, in any namespace.
  - A write-lock holder encodes its slot in the lock token. A waiter
    recovers the lock only when no process holds that slot; a live holder
    can never look dead.
  - A forked child claims its own slot at its first write-lock acquisition,
    so its death is visible while the parent lives.
  - A holder without a slot (all slots taken, byte-range locks unsupported,
    the volume file replaced by name, or a build from before this change)
    stores the plain token and is never proven dead; only the 5 s
    escalation recovers it. New gauge
    `VolumeStats::write_lock_liveness_unregistered`.
  - No format change: the slot lives in the token value, and the owner PID
    stays at directory header offset 20 for older builds.
  - The uncontended acquisition makes no extra syscall; the probe runs only
    after 50 ms behind one holder.
  - Mixed builds: an older build still uses `kill(pid, 0)` on the PID a new
    holder publishes, so keep one PID namespace until every process sharing
    the volume runs this build. A new waiter never probes an older holder:
    it recovers a dead one after 5 s instead of about 50 ms.
  - The one-PID-namespace requirement in `doc/multi-process.md` is now a
    recommendation for mixed-build overlaps only.
- Multi-process writers no longer take a cross-process lock from a peer that
  is alive but descheduled (issue #27). The phase lock used to presume its
  holder dead after about 33 µs of spinning (Apple M), and a directory
  bucket after about 0.9 ms. Both are below a scheduler quantum. A waiter
  now waits by time, with sleeping backoff. It recovers a lock only when
  one holder kept it for the whole budget (1 s for the phase lock, 250 ms
  for a bucket) and a further 2 ms of continuous polling saw no release. A
  new holder restarts the budget, and so does the lock seen free.
  - Every phase-lock and write-lock acquisition now bumps that lock's
    generation counter. So a peer that releases and re-acquires at once
    still counts as a new holder, and is never taken over.
  - The phase lock's counter is stored in directory header bytes 34-35,
    which format version 2 no longer used.
  - The write lock still waits on a live holder, as before. It checks the
    holder's liveness only after one holder has kept the lock for 50 ms,
    and takes over a live holder only after 5 s (previously 4096 liveness
    checks).
  - Releases are CASes on the holder's own token, so a recovered holder
    that resumes cannot free the next holder's lock.
  - The format version is unchanged. An older build of the same format
    still excludes a new one in normal locking, but it still takes over the
    phase lock after about 33 µs, so during an upgrade overlap the pair
    behaves like the older build.
  - These waits are writer-side, but a write can run on a request thread.
    The locks are not fair, so waits by inserts, removes, hit-count updates
    and write-slot reservation are capped. Once the holders a waiter has
    seen come and go add up to 250 ms, the operation returns
    `CacheError::Busy` (`CYCLONE_BUSY`) and publishes nothing; it never
    takes a lock over on the cap. Time on one stuck holder does not count,
    so dead holders are still recovered. A capped acquisition waits at most
    about 0.5 s (bucket), 1.25 s (phase lock), or 0.3 s / 5.25 s (write
    lock, behind a dead holder / a live one it cannot prove dead).
  - All processes sharing a volume must share one PID namespace (now
    documented), because a live write-lock holder in another namespace
    looks dead.
- On Windows, the sleeps of the seqlock read wait and the lock waits use a
  high-resolution waitable timer. At the default 15.6 ms timer resolution,
  the first 10 µs sleep used to take about 15.6 ms, past the 5 ms read
  budget. The process-wide timer resolution is left unchanged.
- The per-process CRC-validation cache identified an already-verified
  document by its offset and the top 16 bits of its CRC. A later document at
  the same offset (rewritten after a wrap, torn by a usurped writer's late
  write, or read back unsynced) whose CRC matched in those 16 bits was
  trusted without its payload being checked, so corrupt bytes could be
  served (probability 2^-16 per same-offset replacement). A slot now holds a
  salted 64-bit token of the document incarnation: offset, pass stamp, full
  CRC, `len`/`header_len` and a key prefix, computed from the header the
  reader has just read and key-verified. Same memory (512 KB per volume),
  still lock-free, no on-disk or shared-memory format change.
- A directory lookup no longer reports a present key as a miss when a writer
  is descheduled while it is updating the key's bucket (#21). Readers gave up
  after 100 seqlock retries, which a preempted writer can outlast. They now
  make those 100 retries, then sleep between further retries (10 µs
  doubling to 1 ms) for up to 5 ms (`SeqlockReadWait`), which is usually
  enough for the writer to be scheduled again without burning the reader's
  CPU. The uncontended path reads no clock and costs what it did before.
  If the bucket is still busy after 5 ms, `read_sync`, `exists_sync`,
  `read_alternate_sync` and `list_alternates_sync` return `CacheError::Busy`
  (`CYCLONE_BUSY`) instead of `NotFound`. This applies to both the in-memory
  and the mmap directory.
- The write, remove and hit-count paths no longer act on a partial
  directory probe. When a bucket stays busy past the budget (its holder is
  stuck: a peer process that died mid-update, or, during a graceful-reload
  overlap where two processes own a stripe, one that is descheduled), they
  force-release it and probe again, so the write still goes through.
- A peer that was force-released while it was descheduled inside a
  directory-bucket update could, on waking, advance the bucket's version by
  one and leave it odd ("writer active") with no writer. Every read of that
  bucket then waited out the full retry budget until the next forced
  recovery. Bucket releases in the mmap directory are now token-checked (a
  CAS from the releaser's own odd version), so a late release is a no-op,
  and the forced release is a CAS from the exact version it waited on.
- A writer that died inside a *committed* wrap (after the cursor dropped
  to the data-area start, before the new pass was published) was repaired by
  clearing its intent flag only. The next writer then filled the current
  pass from the start, over documents whose live borrows still renewed
  `kOk`. The intent byte now marks a committed wrap with its target pass,
  and crash recovery completes the wrap instead.
- A ceiling-forced step reset every borrow slot of the stripe but exposed
  only the chunks it crossed, so a live borrow elsewhere lost its count and
  kept renewing `kOk` until a later normal advance overwrote it with no
  deferral. `renew_lease()`, `renew_lease_strict()` and the read-time
  revalidation now also check the borrow's slot generation and report
  `false` / `kTorn` once it is uncounted.
- A lease-deferral episode (flush and retention) only ended on a passing
  mandatory gate or a force. A write that fit without the deferred step
  left the clock running, so a later first contact with a fresh borrow was
  forced immediately, and the published force deadline stayed stale
  (`ns_until_forced_wrap()` near 0). Every write that gets its slot now ends
  the episode.
- Process-local borrow slots widened from an 8-bit to a 24-bit count: a
  borrow acquired at saturation rode along uncounted and lost its protection
  once the counted holders closed.
- An alternate write over a retained head no longer fails with
  `TooManyAlternates` because of the old chain it will not link to.
- A writer that died inside the wrap-intent window left the intent flag set,
  and every read of that stripe retried until it missed. The flag is now
  cleared by a `forced_release` that proves the holder dead and by an open
  that holds the exclusive lifetime lock. It is never cleared on an escalated
  takeover, because that holder may still be running.
- `remove_at(tag, offset)` cleared only the first matching directory entry.
  A bucket could hold two same-tag entries at one offset, so the survivor
  stayed resolvable. It now clears every match, and an insert at an offset
  that already holds a same-tag entry takes that slot over.
- `remove_sync` now removes every directory entry of the key, not only the
  first one it verifies.
- Alternate-chain walks followed a link to a node above its source. A live
  link always points downward, into the same pass, so an upward link can
  only be stale. Such hops are now rejected. As a consequence, a corrupt
  cyclic chain is never followed at all.
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
