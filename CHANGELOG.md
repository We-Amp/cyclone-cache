# Changelog

All notable changes to Cyclone Cache are documented in this file. The format is
based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added

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

- Multi-process writers no longer take a cross-process lock from a peer that
  is alive but descheduled (issue #27). The phase lock used to presume its
  holder dead after about 33 µs of spinning (Apple M), and a directory
  bucket after about 0.9 ms. Both are below a scheduler quantum. A waiter
  now waits by time, with sleeping backoff, and recovers a lock only when
  one holder kept it for the whole budget: 1 s for the phase lock, 250 ms
  for a bucket. A new holder restarts the budget. The write lock also waits
  by time: it waits on a live holder, as before, and takes one over only
  after 5 s (previously 4096 liveness checks). Releases are CASes on the
  holder's own token, so a recovered holder that resumes cannot free the
  next holder's lock. The phase lock's token comes from a takeover
  generation stored in the directory header bytes 34-35, which format
  version 2 no longer used. The format version is unchanged, and an older
  build of the same format still excludes a new one on both locks.
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
