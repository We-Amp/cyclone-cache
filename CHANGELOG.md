# Changelog

All notable changes to Cyclone Cache are documented in this file. The format is
based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added

- **Wrap retention** (`CacheConfig::wrap_retention`, default `false`; C API
  `disable_wrap_retention`). Normally a stripe's wrap drops its whole
  previous pass at once. With retention on, the previous pass stays readable
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
- `performance_baseline --wrap-retention on|off`, a `retain|flush` argument
  for `concurrent_read_bench`, and `kv_churn --wrap-retention on|off`, which
  adds the retention counters to its JSON.

### Changed

- **Mmap directory version 2** (`MmapDirectory::kVersion`). A retention
  region after the directory entries holds the stripe's exposure generation
  and 64 per-chunk borrow slots. It replaces the stripe-wide borrow slot and
  the `{wrap count, phase}` reader epoch. The version is mixed into the
  fingerprinted filename of mmap volumes, so **multi-process caches start
  cold** after the upgrade. The old file is left on disk. The data offset
  does not move (177 pages at the default 16384 buckets).
- Borrows are counted per frontier chunk: 64 `u16` slots per thread shard
  locally, 64 `u32` slots in the mmap directory. In flush mode the stripe is
  one chunk, so the lease gate behaves as before.
- The GC phase is derived from the pass (`P & 1`) rather than toggled. The
  writer stamps every document with its pass in both modes
  (`Document::write_serial`, outside the CRC). Flush mode ignores the stamp.
- `CycloneCacheConfig` gains a trailing `int disable_wrap_retention`. As with
  earlier trailing fields, callers must be recompiled against the new
  header.

- **On-disk format v8 (was v7): the document checksum is CRC-32C
  (Castagnoli, reflected poly `0x82F63B78`) instead of CRC-32/ISO-HDLC.**
  CRC-32C has a hardware path on x86-64 (SSE4.2 `crc32q`) as well as on
  ARMv8 (`crc32cx`), and the checksum is re-verified on every cold read, so
  it was the cold-read bandwidth ceiling on x86. Both hardware paths run a
  3-way interleave: 34.9 GB/s over 2 MiB on an Apple M5, up from 12.2 GB/s
  single-stream and 3.4 GB/s for the portable table path; 26.5 GB/s on an
  i7-8750H, up from 2.8 GB/s. On Linux that takes the 2 MiB cold read from
  1.6 to 2.2 GB/s, level with LMDB (`doc/kv-cache-benchmark.md`). Dispatch
  is resolved once, on first use.
  **Existing cache files are abandoned (not deleted) on open.** The format
  major is mixed into the fingerprinted volume filename, so a v8 binary
  resolves to a different file and starts cold rather than failing to verify
  v7 documents; the superseded v7 file stays on disk until
  `gc_superseded_on_start` (POSIX only) or a manual delete reclaims it.
- `src/core/crc32.{hpp,cpp}` is now `src/core/crc32c.{hpp,cpp}`, with
  `crc32c()` / `crc32c_update()` and the matching probes; the `crc32_bench`
  benchmark target is now `crc32c_bench`.
- License: Apache License 2.0 (was BUSL-1.1). See `LICENSE` and `NOTICE`.
- License: every file carries an Apache-2.0 SPDX header or is a documented
  exception, verified with Apache RAT.

### Fixed

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
