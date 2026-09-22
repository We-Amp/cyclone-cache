# Changelog

All notable changes to Cyclone Cache are documented in this file. The format is
based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Changed

- **On-disk format v8 (was v7): the document checksum is CRC-32C
  (Castagnoli, reflected poly `0x82F63B78`) instead of CRC-32/ISO-HDLC.**
  CRC-32C has a hardware path on x86-64 (SSE4.2 `crc32q`) as well as on
  ARMv8 (`crc32cx`), and the checksum is re-verified on every cold read, so
  it was the cold-read bandwidth ceiling on x86. Both hardware paths run a
  3-way interleave: 34.9 GB/s over 2 MiB on an Apple M5, up from 12.2 GB/s
  single-stream and 3.4 GB/s for the portable table path (the x86-64 path is
  not yet measured). Dispatch is resolved once, on first use.
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
