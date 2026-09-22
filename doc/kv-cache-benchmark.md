# Cyclone as an LLM KV-cache storage tier — benchmark

**Status:** first round, two laptops (macOS/Apple silicon and Linux/NVMe with
a quiesced page cache), treat as an instrument reading rather than a
marketing number. The point of this round is to find out where Cyclone
stands against the storage backends that LLM serving stacks put behind their
KV connectors today, and to measure — not guess — which of its known
limitations matter for this workload.

## The role being benchmarked

Inference engines (vLLM, SGLang, TensorRT-LLM) keep the transformer key/value
tensors of a prompt prefix so that a later request sharing that prefix skips
recomputation. When GPU/CPU memory is full, those tensors are offloaded to a
node-local storage tier through a "KV connector" (LMCache, vLLM's
`SharedStorageConnector`, SGLang HiCache, NVIDIA Dynamo KVBM). The tier's job
is simple: put a multi-megabyte blob under a hash, get it back fast, survive
restarts, and let several worker processes on the node share it.

That is a storage-engine job, so the comparison is against storage backends in
that role, not against the engines themselves.

| Store | Why it is the right peer |
|---|---|
| `filedir` | one file per block, `mmap` on read — what `SharedStorageConnector` and HiCache's `file` backend effectively do |
| `filedir-read` | same layout, `pread` instead of `mmap` on read |
| LMDB | mmap + zero-copy reads, single file, C — Cyclone's closest design cousin |
| RocksDB (BlobDB) | the default reach-for key-value store |
| Cyclone | `benchmarks/kv_bench.cpp` |

CacheLib (Meta's hybrid DRAM/NVMe cache) is the obvious missing peer; it did
not make round one.

## Workload

One written spec drives every harness so the numbers are comparable; both
harnesses print reference vectors (`kv_bench --print-vectors`) that must
agree byte-for-byte.

- **Block sizes:** 512 KiB, 2 MiB, 8 MiB, 32 MiB. Llama-3-8B fp16 KV is ≈131
  KB/token, so a 16-token block is ≈2 MiB and a 256-token chunk ≈32 MiB;
  512 KiB covers small or quantized models.
- **Dataset:** `N = min(4096, 4 GiB / block_size)` blocks; keys are the
  SHA-256 of `prefix-<i>`; values are incompressible xorshift64* output; a
  64-byte metadata header rides with each value.
- **Phases, per block size, fresh store each time:**
  1. **PUT** — single writer, sequential, no per-put fsync unless the store
     always does it.
  2. **GET first touch** — every block once, in order, right after PUT; every
     4 KiB page is touched.
  3. **GET warm** — Zipf(θ=0.99) over the dataset, 1/4/8 threads, 10 s per
     point after a 2 s warm-up; `view` (touch every page of the returned
     span) and `copy` (memcpy into a preallocated per-thread buffer, i.e.
     staging for a device transfer).
  4. **RESTART** — close, reopen, GET all N once: hit fraction and rate.
  5. **MULTI-PROCESS READ** — 4 forked reader processes, each running phase 3
     `view` at one thread.
- Latency is measured around the get/put call *plus* the touch/copy.
  GB/s is decimal, over value bytes only.

## Store tuning (all printed by the harnesses)

- **Cyclone:** `max_object_size = 0`, `ram_cache_size = 0` (the CLFUS tier is
  for small objects; the OS page cache is the RAM tier here), mmap directory
  on (`set_multi_process(0, 1)`) so the index persists across the restart
  phase and can be shared by the reader processes, `enable_checksum = true`
  (mandatory in that mode, and it forces `verify_checksum_on_read`). Two
  variants isolate specific costs: `--no-mmap-dir --no-verify` (in-memory
  directory, no CRC on read) and a build without the blanket `MADV_RANDOM`
  on the whole-file mapping.
- **filedir / filedir-read:** `open(O_CREAT|O_TRUNC)` + `writev`, no fsync;
  reads `mmap`/`munmap` per get, or `pread` into a per-thread buffer.
- **LMDB 1.0.2:** 16 GiB map, default (durable) flags — it is the only store
  here that fsyncs every put; one read txn per get, zero-copy `MDB_val`.
- **RocksDB 11.8.1:** BlobDB (`min_blob_size = 0`), no compression, no block
  cache, WAL on, `sync = false`. `Get` copies into a `std::string`, so its
  `view` mode is a copy plus the page touch. Phase 5 skipped (one RW process
  per directory).

## Machine

Apple M5 (4 performance + 6 efficiency cores), 16 GiB RAM, macOS 27.0, APFS on
the internal SSD, everything Release/`-O2`. With 16 GiB RAM a 4 GiB dataset
stays page-cache resident once written, so every phase here measures the
software path over cached pages, not the SSD; the page cache cannot be
dropped without root on macOS. The [Linux section](#linux-cold-page-cache-ubuntu-2204-i7-8750h-samsung-970-pro)
repeats the sweep with a quiesced cache, which is where the cold-read
result comes from.

## Results (2026-09-21, Apple M5, second run)

Full generated tables are in the appendix; raw JSON lines are in
[`doc/kv-cache-benchmark/`](kv-cache-benchmark/). `cyclone` is the stock
build with the tuning above; `cyclone-noverify` uses the in-memory directory
and no CRC on read; `cyclone-nomadv` is a build without the blanket
`MADV_RANDOM` (2 MiB only). The first run of this benchmark was withdrawn —
see [Corrections](#corrections-to-the-first-run).

### Headline, 2 MiB blocks (the Llama-8B 16-token block)

| | cyclone | cyclone-noverify | filedir | filedir-read | lmdb | rocksdb |
|---|---:|---:|---:|---:|---:|---:|
| PUT, GB/s | **0.42** | 0.39 | 1.46 | 1.21 | 0.18 ¹ | 0.63 |
| Warm GET copy, 1 thread, GB/s | **70.9** | 70.6 | 18.0 | 18.9 | 64.9 | 8.1 |
| Warm GET copy, 1 thread, p99 | 38 µs | 37 µs | 146 µs | 156 µs | 72 µs | 342 µs |
| Warm GET copy, 8 threads, GB/s | **107** | 107 | 57 | 40 | 100 | 18.6 |
| 4 reader processes × 1 thread, copy, GB/s | **46** | — | 50 | 29 | 78 | n/a |
| Restart: GET all, GB/s (hit fraction) | **0.56** (1.0) | — ² | 28.0 (1.0) | 14.6 (1.0) | 32.1 (1.0) | 6.6 (1.0) |
| First-touch GET, GB/s ³ | 0.56 | 37.1 | 8.0 | 4.8 | 0.74 | 3.4 |

¹ LMDB is the only store that fsyncs every put. ² In-memory directory: not
persistent, restart is a cold cache by design. ³ Noisy for every store
(depends on how much of the just-written data is still dirty); Cyclone's
ranges 0.18–0.56 GB/s across sizes, LMDB's 0.74–29.

Across block sizes the picture holds: Cyclone puts at 0.40–0.48 GB/s
(file-per-block 1.1–1.5, RocksDB 0.6–0.7), ties LMDB on warm copy reads at
every size (58–125 vs 59–119 GB/s), and restarts at a flat 0.56 GB/s where
LMDB and file-per-block restart at 18–32.

### What the numbers say

**Warm reads: Cyclone and LMDB are the same class, and it is the
zero-syscall class.** Both serve a hot block as a span into a mapping that
already exists; the file stores pay an `open`+`mmap`+`munmap` or `pread`
per get and RocksDB a copy through its block layer. On one thread that is
71 vs 65 GB/s for Cyclone and LMDB against 18 for file-per-block and 8 for
RocksDB, with p99s of 38–72 µs against 150–340 µs. Absolute GB/s in this
phase is inflated for everyone — with Zipf(0.99) over 2048 blocks the hot
set is largely CPU-cache-resident — so read these rows as per-get overhead
plus copy cost, not as storage bandwidth. Cyclone's small edge over LMDB is
its shorter per-get path (no transaction begin/end); it is not a
differentiator.

**View vs copy.** The spec's `view` mode touches one byte per 4 KiB page —
1/64th of the bytes — so for a zero-copy store it measures get-call
overhead against cache-resident data: 668 k gets/s for Cyclone, 513 k for
LMDB, 4–15 k for the others. Those ratios are real but say nothing about
moving tensors. Only `copy` figures are compared in this document; `view`
is in the appendix as what it is.

**Restart is Cyclone's clearest loss, and it is one cost: CRC32.** The
first read of an offset verifies the whole document's CRC32 with a
table-driven routine at ≈0.55 GB/s; the validation cache that skips
re-verification is per-process and dies with the process. So a restart
re-reads the dataset at a flat 0.56 GB/s at every block size — 25–60×
behind LMDB and file-per-block — and the multi-process mode that makes the
index persistent also forces `verify_checksum_on_read`, so restart-warmth
and CRC-on-first-read come as a package. With verification off
(`cyclone-noverify`) first-touch runs at 37 GB/s.

> **Update — fixed.** The table routine is gone; see fix-list item 2. The
> checksum now runs at 12.2 GB/s on the M5 (ARMv8 `crc32`) and 2.8 GB/s on
> the i7-8750H (slice-by-16), and the restart phase in this same
> configuration moved from 0.558 to 8.54 GB/s. Every number in the tables
> below predates that change. The checksum has since moved again, to
> CRC-32C at on-disk format v8 (34 GB/s on the M5) — see the follow-up under
> fix-list item 2.

**Multi-process readers scale worse than threads, for the same reason.**
Four Cyclone reader processes reach 46 GB/s in copy mode, 0.65× a single
thread, where four threads reach 1.1× and LMDB's four processes 1.2× (78
GB/s), file-per-block 2.8×. Threads share the validation cache and the page
tables; each forked process starts with both empty, so every child
re-verifies and re-faults the Zipf tail inside its 10 s window. Sharing
costs no locks and no IPC, but a fresh process pays the first-touch price
again.

**First touch vs restart.** Both read every block once, in order. First
touch runs right after PUT, on pages that are freshly written and still
being written back; restart runs on clean, already-faulted pages. Every
store reads clean pages faster, and the first-touch column varies 2–40×
between runs and sizes for every store, so it is reported but not used to
rank anything. The `cyclone-nomadv` experiment (removing the blanket
`MADV_RANDOM` on the mapping) landed inside that noise: it is a plausible
cause of per-page faulting on 2 MiB blocks and stays on the list below, but
this round did not measure it.

**Writes.** 0.40–0.48 GB/s per thread, a third of file-per-block and below
RocksDB. The write path copies the value three times (handle buffer →
document builder → serialized record) and CRCs it before a single
`pwrite`; there is no scatter-gather or reserve-in-place API yet. LMDB's
0.18 is not comparable — it fsyncs every put.

### What to change, in order

1. **Readahead for large reads — DONE, 3.7× cold (13× with CRC out of the
   way), with a per-platform hint.** The blanket `MADV_RANDOM` on the whole
   mapping (right for 4 KB HTTP objects) turned a cold 2 MiB read into ≈512
   serial NVMe faults: 0.06 GB/s on Linux against 1.7–2.2 for the peers.
   The disk read path now advises exactly the document's byte range, after
   the full-key re-verification and before the CRC pass makes the first
   content touch, for documents ≥ `CacheConfig::readahead_min_bytes`
   (default 256 KiB, 0 = off). `MADV_RANDOM` stays, so small objects are
   untouched. Two details carry most of the win: on Linux the advice is
   issued in 512 KiB chunks, because the kernel clamps a single
   `MADV_WILLNEED` to `max(bdi->io_pages, ra_pages)` pages and so silently
   covers only the first ~1.25 MB of a 2 MiB range; and each document
   placement is advised at most once every 2 s (a lossy direct-mapped
   filter with a per-slot timestamp), because on a warm re-read the advice
   walk costs more than the read — unfiltered it took warm `view` from
   261 k to 68 k gets/s. The filter decays rather than remembering forever:
   a KV tier is normally larger than RAM, so a block that was advised,
   evicted and read cold again is the common case and must get its hint
   back. `CacheStats::readahead_hints_issued` counts the hints that reach
   the kernel.

   **`MADV_WILLNEED` is not portable in cost — the macOS finding.** The
   first version of this used `madvise(MADV_WILLNEED)` everywhere, which is
   right on Linux and wrong on Darwin. Darwin's `MADV_WILLNEED` is not a
   queue-and-return: it walks and populates the range under the shared VM
   object's lock, so it is expensive per call *and* serialises across every
   process mapping the volume. A standalone probe on an M5 — one 2 MiB
   range of a `MAP_SHARED` read-write mapping whose pages are in the buffer
   cache but not yet in the caller's page tables — measured
   `MADV_WILLNEED` in 512 KiB chunks at **50 µs** with one process and
   **305 µs** with four concurrent ones, against **5 µs / 10 µs** for
   `fcntl(F_RDADVISE)`, Darwin's native asynchronous readahead. Against
   phase 5 (four reader processes) that is the whole story: the madvise
   hint cost macOS **86 %** of `multiprocess_read view`, **85 %** of
   `copy`, and **29 %** of `restart`. The read path therefore picks by
   platform — `F_RDADVISE` on the volume fd on Darwin,
   `madvise(MADV_WILLNEED)` on Linux, `PrefetchVirtualMemory` on Windows —
   via `MappedFile::supports_advise_readahead()`. With `F_RDADVISE` every
   macOS phase is back inside noise of no hint at all, and the cold-read
   win survives where the page cache is genuinely cold.

   Linux, 2 MiB blocks, 1 thread, `--seconds 5`, caches dropped before the
   cold phases; median of three runs:

   | phase | before | after | |
   |---|---:|---:|---:|
   | `get_first_touch` | 57.4 gets/s, 0.120 GB/s, p99 24.2 ms | 212.9 gets/s, 0.446 GB/s, p99 5.3 ms | **3.7×** |
   | `restart` | 57.9 gets/s, 0.121 GB/s, p99 24.3 ms | 209.5 gets/s, 0.439 GB/s, p99 5.5 ms | **3.6×** |
   | `get_first_touch`, `--no-verify` | 85.5 gets/s, 0.179 GB/s, p99 18.5 ms | 1109.7 gets/s, 2.327 GB/s, p99 1.5 ms | **13.0×** |
   | `put` | 132.0 puts/s, 0.277 GB/s | 131.7 puts/s, 0.276 GB/s | — |
   | `get_warm` view | 271.6 k gets/s, p99 4.8 µs | 265.5 k gets/s, p99 5.0 µs | 98 % |
   | `get_warm` copy | 5350 gets/s, 11.22 GB/s | 4990 gets/s, 10.47 GB/s | 93 % |
   | `multiprocess_read` view | 8379 gets/s, 17.57 GB/s | 8287 gets/s, 17.38 GB/s | 99 % |
   | `multiprocess_read` copy | 3508 gets/s, 7.36 GB/s | 3588 gets/s, 7.52 GB/s | 102 % |

   Four children re-advising is 4× the hint traffic, so phase 5 is the case
   that had to be checked: on Linux it is unchanged, because the 2 s
   re-advise filter caps the traffic and `MADV_WILLNEED` returns without
   blocking anyone else.

   macOS (M5, 16 GB, APFS/NVMe), same command, median of three runs. No
   `drop_caches` equivalent exists, so the "cold" phases here run against a
   partly warm page cache and sit on the 0.55 GB/s software-CRC32 ceiling
   rather than on I/O — which is exactly why the *cost* of the hint is what
   this table is for:

   | phase | stock | `MADV_WILLNEED` | `F_RDADVISE` |
   |---|---:|---:|---:|
   | `put` | 198.0 puts/s, 0.415 GB/s | 198.7, 0.417 | 205.6, 0.431 |
   | `get_first_touch` | 277.6 gets/s, 0.582 GB/s | 240.4, 0.504 (87 %) | 284.4, 0.597 (**102 %**) |
   | `restart` | 280.4 gets/s, 0.588 GB/s | 200.3, 0.420 (71 %) | 280.7, 0.589 (**100 %**) |
   | `get_warm` view | 685.7 k gets/s | 679.5 k (99 %) | 684.4 k (100 %) |
   | `get_warm` copy | 33.9 k gets/s, 71.09 GB/s | 33.7 k, 70.64 (99 %) | 33.3 k, 69.81 (98 %) |
   | `multiprocess_read` view | 15502 gets/s, 32.51 GB/s | 2094, 4.39 (**14 %**) | 14743, 30.92 (95 %) |
   | `multiprocess_read` copy | 10526 gets/s, 22.07 GB/s | 1596, 3.35 (**15 %**) | 10329, 21.66 (98 %) |

   The one run in the set that started with a genuinely cold page cache is
   the only macOS sample where readahead has anything to do: there stock
   managed 118.4 gets/s (0.248 GB/s) on `get_first_touch` and `F_RDADVISE`
   225.9 (0.474 GB/s). Apple Silicon's 16 KiB base page and Darwin's own
   clustered pagein are why the upside is smaller than on Linux to begin
   with — a cold 2 MiB read is ~128 faults there, not ~512.

   The `--no-verify` row is the honest ceiling of the I/O fix: **2.33 GB/s,
   past LMDB (2.15) and file-per-block (1.76)**. With verification on, the
   cold path is now bounded by the 0.55 GB/s software CRC32, which is item
   2 below — readahead has taken the I/O out of the picture and handed the
   remaining gap to the checksum. `MADV_POPULATE_READ` (Linux ≥ 5.14) was
   measured on top of the chunked `WILLNEED` and did not help (2.25 vs 2.33
   GB/s), so it is not used: `WILLNEED` already queues the large reads, and
   populating the PTEs up front only moves the per-page work.
2. **Hardware CRC32** (ARMv8 / SSE4.2 intrinsics, >10 GB/s vs 0.55) and a
   way for the verified state to outlive the process — the validation cache
   could live beside the mmap directory so a restart and every peer process
   inherit it. Covers the restart floor and the multi-process sub-scaling
   on both platforms.
2. **Fast CRC32** — ✅ **done** (then `src/core/crc32.{hpp,cpp}`; renamed
   to `crc32c.{hpp,cpp}` by the follow-up below). The byte-wise
   table routine was replaced by slice-by-16 tables plus an ARMv8
   `crc32b/w/x` path, selected once through a function pointer. The on-disk
   convention is untouched (reflected `0xEDB88320`, init/xorout
   `0xFFFFFFFF`), so existing cache files keep verifying. Measured with
   `./build-rel/crc32_bench`, 2 MiB buffer:

   | | byte-wise (was) | slice-by-16 | ARMv8 crc32 |
   |---|---:|---:|---:|
   | Apple M5 (clang, Release) | 0.61 GB/s | 3.44 GB/s | **12.21 GB/s** |
   | i7-8750H (clang-20, Release) | 0.50 GB/s | **2.83 GB/s** | n/a |

   Both machines print the same checksum for the same buffer, and the unit
   test pins every path against a verbatim copy of the old byte-wise
   routine (then `tests/unit/test_crc32.cpp`).

   End to end on the M5 (`kv_bench --block-size 2097152 --seconds 5
   --threads 1 --skip-multiprocess`, warm page cache): restart
   **0.558 → 8.54 GB/s** (15×), first touch 0.566 → 8.72 GB/s, put
   0.421 → 1.38 GB/s. `performance_baseline --content-size 4096`
   first-touch read **137 k → 1.12 M ops/s**. The restart floor is now the
   page-cache/copy rate, not the checksum.

   Follow-up — **CRC-32C, on-disk format v8** (✅ done). The note this item
   used to carry said x86-64 had to stay on the table path because SSE4.2's
   `crc32` instruction computes CRC-32**C**, a different polynomial. That
   was the open decision, and it has been taken: the document checksum *is*
   CRC-32C from format major v8 (reflected `0x82F63B78`, init/xorout
   `0xFFFFFFFF`), which has hardware instructions on x86-64 (SSE4.2
   `crc32q`) *and* ARMv8 (`crc32cx`). Existing cache files are discarded —
   the format major is part of the fingerprinted filename, so consumers take
   a cold cache, never a misparse. The files are now
   `src/core/crc32c.{hpp,cpp}`, `tests/unit/test_crc32c.cpp` and
   `./build-rel/crc32c_bench`. Both hardware paths run three interleaved CRC
   registers (8192- then 256-byte blocks, recombined through
   compile-time-generated GF(2) zero-shift operators), because the
   instruction is latency- not throughput-bound:

   | 2 MiB buffer | byte-wise | slice-by-16 | hw, 1 stream | hw, 3-way |
   |---|---:|---:|---:|---:|
   | Apple M5 (clang, Release) | 0.61 GB/s | 3.28 GB/s | 12.02 GB/s | **34.03 GB/s** |

   > **TODO (lead):** the i7-8750H `crc32c_bench` row and the Linux
   > `kv_bench` restart/first-touch numbers for v8 are not measured yet —
   > the Linux host was unreachable when this landed. Fill in the x86-64
   > SSE4.2 column and the end-to-end sweep here.

   Still open from this item: a way for the *verified state* to outlive the
   process — the CRC-validation cache could live beside the mmap directory
   so a restart and every peer process inherit it, removing the
   re-verification entirely rather than making it cheap.
3. **A `WriteHandle::reserve(n)` that hands back the destination span** so
   the caller writes or DMAs straight into the record, collapsing three
   copies to one; then revisit puts against file-per-block (4× today).
4. **Zero-copy device transfer, measured.** `view` mode shows the zero-copy
   path is the cheapest per-get of any store here (≈1.5 µs for a 2 MiB
   block; LMDB the only peer in the same class), and the Metal section
   below shows the payoff: wrap the volume mapping once and blit by
   `content_file_offset()` for 1.9× over a staged copy. The API for the
   winning form exists (`content()`, `content_file_offset()`,
   `volume_files()`); what is missing is an entry point that hands an
   embedder the mapping identity directly. The discrete-GPU half now agrees
   and more strongly: over PCIe 3.0 ×16 on a GTX 1050,
   `cudaHostRegister` accepts the volume mapping whole (7.56 GiB in one
   call) and transferring from it reaches **12.79 GB/s — 99.8% of the
   12.82 GB/s pinned-buffer bus ceiling — against 5.51 GB/s staged, 2.3×**,
   while registering *per block* is a 21% **loss** against staging (see
   [Device transfer: CUDA](#device-transfer-cuda-gtx-1050-pcie)).
   GPUDirect Storage (`cuFileRead` at `content_file_offset()`, NVMe→GPU
   with no host copy) needs a data-center GPU.
5. Only then: a zero-copy C read entry point and a Python binding, which is
   what a vLLM/SGLang connector would call.

### Corrections to the first run

Two methodology faults were found in the first sweep by review and
retraction is the honest fix:

- The peer harness wiped each block size's data only *before* that size
  ran, so 2–6 GiB of earlier sizes stayed resident and squeezed the 16 GiB
  page cache during the larger-block runs; `kv_bench` deletes its volume
  per size, so Cyclone never paid that. Peer warm reads were depressed up to
  100× (LMDB `view` 4 k → 513 k gets/s) and the first draft claimed a 5–20×
  Cyclone read advantage that does not exist. Fixed in the harness; every
  number above is from the corrected sweep.
- The first headline table put a `view`-mode multi-process row under
  `copy`-mode rows, inflating the multi-process story ~10×. Phase 5 now
  runs both modes and only `copy` is compared.

### Caveats

- One laptop, one SSD, APFS, 16 GiB RAM, 10 s per point, single run. Treat
  differences under ~20 % as noise; the multi-× differences are not.
- The peers measure a `std::string` allocation and a SHA-256 inside their
  warm-phase sample (≈1 µs); Cyclone hashes outside it. Irrelevant at
  100 µs+ latencies, slightly flatters Cyclone at the µs scale.
- The Linux run is one consumer laptop (dual-channel DDR4, one NVMe); a
  server-class NVMe array and a data-center GPU would move the absolute
  numbers, not the ordering.

## Linux, cold page cache (Ubuntu 22.04, i7-8750H, Samsung 970 PRO)

The same sweep on a Linux laptop with root, so `sync; echo 3 >
/proc/sys/vm/drop_caches` runs before the first-touch and restart phases
(`--drop-caches-cmd` in both harnesses). Everything else identical:
clang-20, 10 s per point, each size's data deleted before the next, nothing
else running. Raw data in
[`doc/kv-cache-benchmark/linux/`](kv-cache-benchmark/linux/); full tables
in [Appendix B](#appendix-b--linux-generated-tables). This machine has
dual-channel DDR4, so `copy` saturates at ≈12 GB/s for every store; read
the warm rows as per-get overhead again.

### Headline, 2 MiB blocks, page cache dropped before cold phases

| | cyclone | cyclone-noverify | filedir | filedir-read | lmdb | rocksdb |
|---|---:|---:|---:|---:|---:|---:|
| PUT, GB/s | 0.34 | 0.29 | 1.44 | 1.39 | 0.15 ¹ | 0.61 |
| **Cold first-touch GET, GB/s** | **0.06** | 0.18 | 1.76 | 1.30 | 2.15 | 0.82 |
| **Cold restart GET, GB/s** | **0.06** | — ² | 1.73 | 1.31 | 2.16 | 0.82 |
| Warm GET copy, 1 thread, GB/s / p99 | 11.9 / 235 µs | 11.8 | 6.5 / 400 µs | 8.0 / 328 µs | 12.9 / 224 µs | 3.0 / 767 µs |
| Warm GET view, 1 thread, gets/s | 274 k | 279 k | 7.0 k | 5.3 k | 245 k | 1.8 k |
| 4 reader processes, copy, GB/s | 9.3 | — | 11.8 | 7.8 | 10.9 | n/a |

¹ fsync per put. ² In-memory directory, cold by design.

### What Linux adds

**Cold reads are the real loss, and it is mostly not the CRC.** With the
page cache actually empty, Cyclone reads a 2 MiB block it wrote seconds
ago at 0.06 GB/s — 10–25× behind every peer, on an SSD that delivers 2+
GB/s to LMDB and file-per-block in the same phase. Turning verification off
(`cyclone-noverify`) only recovers to 0.18 GB/s, so CRC32 is a third of it;
the rest is the whole-volume mapping advised `MADV_RANDOM`: each 4 KiB page
of the block is a separate fault and a separate NVMe round-trip, ≈512 per
block, with no readahead. The file stores get per-file readahead on `open`,
and LMDB's mapping has no such advice. This is the same mechanism the macOS
run could only hint at (there the page cache was never cold) and it is now
the top item in the fix list. It also explains why four Cyclone reader
processes fall below one thread on both platforms: each process re-faults
and re-verifies from scratch.

> **Fixed since this run.** The table above is the pre-fix measurement and
> is kept as the baseline. Per-document `MADV_WILLNEED` on the cold read
> path (fix-list item 1, now landed) takes cold first-touch to 0.449 GB/s
> with CRC on and 2.327 GB/s with it off — past LMDB. See item 1 below for
> the full before/after.

**Warm reads confirm the macOS picture on cheaper hardware.** Cyclone and
LMDB are the same zero-syscall class (274 k vs 245 k gets/s `view`; 11.9 vs
12.9 GB/s `copy`, DRAM-bound), file-per-block 6–8 GB/s, RocksDB 3.

**Writes** are 0.25–0.35 GB/s here vs 1.3–1.4 for file-per-block — the
same 4× gap as on macOS, from the same three copies + CRC.

## Device transfer: does zero-copy pay off? (Metal, Apple silicon)

Item 4 above asks whether the zero-copy read pays off *end to end* — a KV
connector only benefits if the device transfer can source from the mapping
instead of staging through a pinned buffer. `benchmarks/kv_gpu_metal.mm`
answers the Apple half of that question with a measurement. For a block that
is already in the store, it times how fast it reaches GPU-private memory
(`MTLStorageModePrivate`) along five paths — a per-block
`newBufferWithBytesNoCopy` wrap (`zerocopy`), a single wrap of the store's
whole mapping blitted by offset (`zerocopy-persistent`), a `memcpy` into a
persistent shared buffer then a blit (`staged`), the copy alone (`memcpy`),
and two fixed-cost probes (an empty command buffer, and a 4 KiB blit).
Batched variants put 16 blocks in one command buffer and report per-block
amortized. The same three stores are compared, and the blocks come from
`benchmarks/kv_workload.hpp`, so they are bit-identical to `kv_bench`'s.

### Acceptance: Metal wraps a file-backed `MAP_SHARED` mapping, no copy

This was the open question, and the answer is yes — no fallback was needed.
Every surface was accepted, `buffer.contents` came back **equal to the
mapping base** (so Metal is aliasing the pages, not copying them), and every
blit finished `MTLCommandBufferStatusCompleted`:

```
mmap(MAP_SHARED, PROT_READ|PROT_WRITE) : accepted; contents == mapping base
mmap(MAP_SHARED, PROT_READ)            : accepted; contents == mapping base
A1 content() page-aligned range        : accepted; contents == mapping base
A2 mapped_view() page-aligned range    : accepted; contents == mapping base
A3 own mmap at content_file_offset()   : accepted; contents == mapping base
```

A read-**only** (`PROT_READ`) file mapping is accepted too, which is not
obvious — `MTLResourceStorageModeShared` is nominally CPU-writable. So A2 and
A3 were never needed; they stay in the benchmark as a regression check,
because acceptance is a macOS/driver property that could change.

Two mechanics matter. The wrapped pointer must be page-aligned, and on Apple
silicon a page is **16 KiB**, not 4 KiB (the benchmark takes it from
`getpagesize()`). And `content()` starts 4356 bytes into a page — it sits
behind the 132-byte document header inside the volume — so the wrap is taken
at the page-aligned address below it and the blit uses
`sourceOffset: content - pagebase`, which is 4-byte aligned as the blit
encoder requires.

### Results, 2 MiB blocks

Apple M5 (10 core), 16 GiB, macOS 27.0 (26A428), Metal device "Apple M5",
unified memory, `maxBufferLength` 8.9 GiB, page size 16384. N = 512, 10 s per
path, otherwise-idle machine. Raw lines:
[`metal-gpu-2mib.jsonl`](kv-cache-benchmark/metal-gpu-2mib.jsonl).

| store | path | batch | blocks/s | GB/s | p50 µs | p99 µs | correct |
|---|---|---:|---:|---:|---:|---:|---|
| metal | submit-only-0B | 1 | 72252 | 0.00 | 13.8 | 19.0 | ok |
| metal | blit-4KiB | 1 | 6433 | 0.03 | 173.7 | 227.3 | ok |
| cyclone | zerocopy | 1 | 4457 | 9.35 | 217.6 | 266.0 | ok |
| cyclone | zerocopy-persistent | 1 | 4548 | 9.54 | 214.4 | 251.9 | ok |
| cyclone | staged | 1 | 4940 | 10.36 | 203.5 | 239.8 | ok |
| cyclone | memcpy | 1 | 33595 | 70.45 | 29.2 | 37.2 | ok |
| cyclone | zerocopy | 16 | 13688 | 28.71 | 70.3 | 109.6 | ok |
| **cyclone** | **zerocopy-persistent** | **16** | **21171** | **44.40** | **46.3** | **87.0** | **ok** |
| cyclone | staged | 16 | 11247 | 23.59 | 87.8 | 123.8 | ok |
| lmdb | zerocopy | 1 | 4726 | 9.91 | 206.8 | 332.9 | ok |
| lmdb | zerocopy-persistent | 1 | 4568 | 9.58 | 213.9 | 252.0 | ok |
| lmdb | staged | 1 | 4928 | 10.33 | 203.6 | 286.3 | ok |
| lmdb | memcpy | 1 | 33768 | 70.82 | 29.4 | 35.8 | ok |
| lmdb | zerocopy | 16 | 8680 | 18.20 | 114.7 | 162.1 | ok |
| lmdb | zerocopy-persistent | 16 | 20510 | 43.01 | 47.7 | 92.1 | ok |
| lmdb | staged | 16 | 11317 | 23.73 | 87.1 | 123.6 | ok |
| filedir-pread | staged | 1 | 3443 | 7.22 | 289.3 | 412.7 | ok |
| filedir-pread | memcpy | 1 | 10353 | 21.71 | 97.6 | 124.9 | ok |
| filedir-pread | staged | 16 | 5098 | 10.69 | 198.0 | 238.7 | ok |

### What it says

**1. A single-block GPU transfer is entirely submission-bound — so batch.**
An empty command buffer costs 13.8 µs p50 to commit and wait; a command
buffer with a blit encoder moving **4 KiB** costs 173.7 µs. That ~160 µs is
the blit-encoder round trip and is independent of the byte count: a 2 MiB
single-block transfer is 203–218 µs, barely more than the 4 KiB one. At
batch 1 nothing else is visible — zero-copy, staged and persistent-wrap all
land between 203 and 218 µs, inside the noise of Metal submission. Saving a
29 µs memcpy is irrelevant when the submission costs 170 µs. Anyone doing
one-block-per-command-buffer transfers is measuring Metal, not their storage
tier.

**2. Wrapping per block is a wash.** Batched 16-per-command-buffer, the
per-block `newBufferWithBytesNoCopy` path is a win for Cyclone (28.7 vs
23.6 GB/s staged) and a *loss* for LMDB (18.2 vs 23.7). Making a 2 MiB range
GPU-addressable costs roughly what memcpying it costs, and the cost is
variable — it depends on how the driver finds and wires the underlying
pages, which is why two stores making identical Metal calls disagree.
Wrapping a borrowed span per read is not where zero-copy pays off.

**3. Wrapping the mapping *once* is — 1.9×.** Both zero-copy stores keep
every block inside one `MAP_SHARED` region (Cyclone in the volume file
mapping, 7.56 GiB of it wrapped here; LMDB in its map, 1.01 GiB). Wrap that
region once into a single `MTLBuffer` and each block becomes a
`sourceOffset` into it. At batch 16 Cyclone reaches **44.40 GB/s**, p50
**46.3 µs**/block, against 23.59 GB/s staged — **1.88× throughput, 1.9×
lower latency**. LMDB gets 43.01 GB/s, the same 1.8×. `filedir-pread`, which
has no borrowed-buffer API at all, reaches only 10.69 GB/s — **4.2×
behind**. That is the real answer: Cyclone's zero-copy read pays off for GPU
transfer, but in the form "the cache file is one persistent GPU-visible
mapping and a read hands you an offset into it", not "wrap whatever span the
read returned". The `ReadHandle` API already supports the useful form —
`content()` gives the pointer, `content_file_offset()` the offset, and
`Cache::volume_files()` the file — so an embedder can build the persistent
wrapper itself. Note `maxBufferLength` is 8.9 GiB here: an 8 GiB volume fits
one wrapper, a larger cache needs the mapping split into several wrapped
windows.

### Caveat: unified memory

`MTLStorageModePrivate` on Apple silicon is **still system DRAM**. It is not
a discrete VRAM aperture and there is no PCIe bus in the path. What is
measured is a real DMA-engine copy between two regions of the same physical
memory, plus the driver work to make the source range GPU-addressable —
which is exactly the cost `newBufferWithBytesNoCopy` pays per block on the
`zerocopy` path and once on `zerocopy-persistent`. That is the honest cost of
"get these bytes into a resource the GPU owns and the CPU cannot touch",
which is what an offload tier must do before a kernel reads them. It is
**not** a host-to-device upload measurement and must not be compared to a
discrete-GPU PCIe figure. On a discrete GPU the staged path would
additionally pay a real bus transfer, widening the gap in favour of any path
that avoids a host copy — so the 1.9× here is the conservative end. In the
other direction: because memory is unified, an application that can let the
GPU read the cache pages *in place* (a shared-storage-mode buffer used
directly by the kernel, no blit at all) skips this measurement entirely. The
benchmark deliberately measures the transfer-to-private case, because that is
what a consumer wanting an isolated, GPU-owned copy has to do.

One deviation from the rest of this document: LMDB is opened with
`MDB_NOTLS` here. Without it LMDB pins a read-only transaction to a thread
slot and a batch of 16 simultaneous borrows from one thread cannot be opened
at all. It changes reader-slot bookkeeping only, not durability.

### Running it

The target is Apple-only and built by the ordinary benchmark configure;
LMDB is optional (without it the `lmdb` rows are absent and the header
prints `lmdb: not built`).

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCYCLONE_USE_BUNDLED_SHA256=ON
cmake --build build -j
./build/kv_gpu_metal --block-size 2097152 --seconds 10
```

## Device transfer: CUDA (GTX 1050, PCIe)

**In one line:** `cudaHostRegister` does accept Cyclone's file-backed
`MAP_SHARED` volume mapping — 7.56 GiB in a single call — and transferring
2 MiB blocks straight out of it reaches **12.79 GB/s, 99.8% of this
machine's 12.82 GB/s pinned-buffer PCIe ceiling, 2.32× the staged path**;
registering *per block* instead is a **21% loss** against staging and 1.33×
worse than not trying at all. The Metal 1.9× was the conservative end, as
predicted.

The Metal section above answers the Apple half of item 4 on unified memory,
where there is no bus at all. `benchmarks/kv_gpu_cuda.cu` (plus its C++23
host half, `benchmarks/kv_gpu_cuda_host.cpp`) asks the same question where
there *is* one: a discrete GPU behind PCIe, on which the staged path really
does pay a host copy that a mapping-sourced transfer does not. Same workload
(`benchmarks/kv_workload.hpp`, so the blocks are bit-identical to
`kv_bench`'s and the Metal run's), same Cyclone tuning, same N = 512 blocks
of 2 MiB, same "already in the store, read once, every page touched before
any timing" discipline.

The paths, all landing the same bytes in the same `cudaMalloc` buffer:

| path | what it does |
|---|---|
| `pageable` | `cudaMemcpy(H2D)` straight from `content()`. The naive path: the driver stages it through its own internal pinned buffers, synchronously, so it cannot batch. |
| `zerocopy` | `cudaHostRegister` the page-aligned range containing `content()`, `cudaMemcpyAsync`, sync, `cudaHostUnregister` — once per batch, for each block read. "Pin whatever span the read returned." Batched, the spans are coalesced first, because packed records make them overlap (see the acceptance notes). |
| `zerocopy-persistent` | `cudaHostRegister` the store's whole mapping **once**; a block is then nothing but its borrowed pointer. The decisive variant. |
| `staged` | `memcpy` into a `cudaMallocHost` buffer, then `cudaMemcpyAsync`. The conventional path. |
| `memcpy` | the host copy alone, no GPU — `kv_bench`'s `copy` access mode, as a reference cost. |

Three references involve no store at all: `submit-only-0B` (a 0-byte
`cudaMemcpyAsync` plus `cudaStreamSynchronize`), `h2d-4KiB`, and
`pinned-ceiling` — a full block out of the pinned buffer, which is the PCIe
host-to-device bandwidth ceiling the machine can reach at all and which no
store-backed row can beat. `zerocopy`, `zerocopy-persistent` and `staged`
also run batched: 16 `cudaMemcpyAsync` on one stream, one
`cudaStreamSynchronize`, reported per-block amortized.

The load-bearing unknown is the same one Metal answered with "yes": whether
`cudaHostRegister` will page-lock a file-backed `MAP_SHARED` mapping at all.
If it refuses, both zero-copy paths are simply unavailable to an embedder and
the answer for CUDA is "stage it". The benchmark therefore probes every
surface at startup — an own `MAP_SHARED` mapping read-write and read-only,
each with `cudaHostRegisterDefault` and `cudaHostRegisterReadOnly`, a private
anonymous mapping as the control, and Cyclone's `content()`, `mapped_view()`
and own-mmap-at-`content_file_offset()` surfaces — and prints the exact
`cudaGetErrorString` for each. Acceptance alone is not enough: each probe
also transfers and compares the device bytes against the source.

### Machine

Intel Core i7-8750H (6 cores / 12 threads, 2.20 GHz), 23 GiB RAM, Samsung
970 PRO NVMe root, **NVIDIA GeForce GTX 1050** (GP107M, Pascal, `sm_61`,
compute capability 6.1, 5 SMs, 3.9 GiB), on **PCIe 3.0 ×16** (link reported
at gen 3 / width 16, both current and max), Ubuntu 22.04.5, kernel
5.15.0-191. Driver 550.163.01 and CUDA toolkit 12.4 from the NVIDIA
`ubuntu2204` apt repository; `nvcc` builds the `.cu` with `-ccbin g++-13`
(CUDA 12.4 accepts GCC ≤ 13) while the host half is built as C++23 by
`clang++-20`. Host page size 4096. It is an Optimus hybrid laptop:
`nvidia-prime` had it in `intel` mode, which installs a `blacklist nvidia` /
`alias nvidia off` modprobe drop-in and stops the driver loading no matter
how well it built — `prime-select on-demand` is what makes the GPU usable
for compute.

Because `cudaHostRegister` page-locks multi-GiB ranges of the cache mapping,
the `memlock` rlimit has to be raised (a drop-in under
`/etc/security/limits.d/`); the default ~3 GiB is smaller than the span an
8 GiB Cyclone volume occupies. With `memlock` unlimited the **single
registration succeeded** — 7.56 GiB of Cyclone's mapping in **one** window,
in 4.9 s — so the 1 GiB window fallback was never exercised in these runs. It
stays in the benchmark, and reports itself when it fires, because the rlimit
is a machine property and not every host will allow the whole span.

One build note that is not about the GPU: CMake only knows a CUDA language
dialect it has a flag for, and `CUDA20` arrived in CMake 3.25 — Ubuntu 22.04
ships 3.22, where asking for it fails the *generate* step outright. The
`.cu` is deliberately plain (runtime calls, `memset`/`strncpy`), so
`CMakeLists.txt` requests C++20 only where CMake knows the flag and falls
back to C++17 otherwise, announcing which it took.

### Acceptance: CUDA page-locks a file-backed `MAP_SHARED` mapping — with the right flag

The answer is **yes**, with one condition that costs an embedder nothing to
meet but fails confusingly if missed: the *protection* of the mapping, not
its file backing, is what decides the flag. A read-write `MAP_SHARED` file
mapping is taken by plain `cudaHostRegisterDefault`; a **`PROT_READ`** one is
refused by it with `invalid argument`, and needs
`cudaHostRegisterReadOnly`. Verbatim, at 2 MiB:

```
mmap(MAP_SHARED, RW)  RegisterDefault  : accepted; transfer completed; device bytes == source
mmap(MAP_SHARED, RW)  RegisterReadOnly : accepted; transfer completed; device bytes == source
mmap(MAP_SHARED, RO)  RegisterDefault  : REFUSED: cudaHostRegister -> invalid argument
mmap(MAP_SHARED, RO)  RegisterReadOnly : accepted; transfer completed; device bytes == source
MAP_PRIVATE|ANONYMOUS (control)        : accepted; transfer completed; device bytes == source

A1 content() page-aligned range      : accepted; transfer completed; device bytes == source
   (content ptr % page = 260)
A2 mapped_view() page-aligned range  : accepted; transfer completed; device bytes == source
   (document view 2097348 B incl. its header)
A3 own mmap(PROT_READ) Default       : REFUSED: cudaHostRegister -> invalid argument
A3 own mmap(PROT_READ) ReadOnly      : accepted; transfer completed; device bytes == source
```

Acceptance alone was not taken as the answer: every probe also ran a
`cudaMemcpyAsync` out of the registered range and compared the device bytes
against the source, and all of them matched.

So all three Cyclone surfaces work. A1 and A2 are read-write (Cyclone maps
its volume `PROT_READ|PROT_WRITE`) and go through on `Default`; A3 — the
GPUDirect-shaped surface, a consumer's own `mmap(MAP_SHARED, PROT_READ)` at
`content_file_offset()` — needs `ReadOnly`, and so does **LMDB's** map, which
is why LMDB's rows exist at all. `content()` sits 260 bytes into a page (the
132-byte document header, plus the record's place in the stripe), so the
registration is taken at the page below it and the copy sources from
`content()` itself.

The persistent registration that the decisive path needs was accepted whole:
**7.56 GiB of Cyclone's mapping in a single `cudaHostRegister`**, with
`cudaHostRegisterDefault`, in 4.9 s — no windowing. LMDB's 1.00 GiB map was
taken in one `ReadOnly` registration in 0.1 s.

One mechanic worth recording, because the naive shape of the per-block path
is simply broken. Records are packed back to back, so two blocks of one
16-block batch routinely share a page. CUDA refuses a range overlapping one
already pinned — and, less obviously, a `cudaMemcpyAsync` whose source
*straddles the end* of a registration returns `invalid argument` even though
every byte is resident. The `zerocopy` path therefore sorts and coalesces the
batch's page-aligned spans before registering any of them, which is what an
embedder pinning borrowed spans would have to do too. It also registers
everything before enqueuing any copy and unregisters only after the
synchronize: unregistering host memory the DMA engine is still reading is a
use-after-free.

### Results, 2 MiB blocks

N = 512, 10 s per path, otherwise-idle machine, `--path` on the NVMe. Raw
lines: [`cuda-gtx1050-2mib.jsonl`](kv-cache-benchmark/cuda-gtx1050-2mib.jsonl).

| store | path | batch | blocks/s | GB/s | p50 µs | p99 µs | correct |
|---|---|---:|---:|---:|---:|---:|---|
| cuda | submit-only-0B | 1 | 2027874 | 0.00 | 0.5 | 0.5 | ok |
| cuda | h2d-4KiB | 1 | 254471 | 1.04 | 3.8 | 4.2 | ok |
| cuda | pinned-ceiling | 1 | 6078 | 12.75 | 164.0 | 190.2 | ok |
| **cuda** | **pinned-ceiling** | **16** | **6112** | **12.82** | **162.9** | **167.3** | **ok** |
| cyclone | pageable | 1 | 2774 | 5.82 | 372.1 | 416.1 | ok |
| cyclone | zerocopy | 1 | 2272 | 4.77 | 434.9 | 484.0 | ok |
| cyclone | zerocopy | 16 | 2079 | 4.36 | 407.8 | 1167.3 | ok |
| cyclone | zerocopy-persistent | 1 | 5949 | 12.48 | 165.9 | 193.9 | ok |
| **cyclone** | **zerocopy-persistent** | **16** | **6098** | **12.79** | **162.5** | **168.6** | **ok** |
| cyclone | staged | 1 | 2428 | 5.09 | 421.3 | 491.4 | ok |
| cyclone | staged | 16 | 2626 | 5.51 | 394.5 | 401.4 | ok |
| cyclone | memcpy | 1 | 5156 | 10.81 | 204.1 | 246.7 | ok |
| lmdb | pageable | 1 | 2752 | 5.77 | 372.7 | 410.9 | ok |
| lmdb | zerocopy | 1 | 2230 | 4.68 | 443.2 | 508.2 | ok |
| lmdb | zerocopy | 16 | 2561 | 5.37 | 346.7 | 622.7 | ok |
| lmdb | zerocopy-persistent | 1 | 5997 | 12.58 | 165.4 | 191.8 | ok |
| lmdb | zerocopy-persistent | 16 | 6107 | 12.81 | 163.8 | 169.0 | ok |
| lmdb | staged | 1 | 2434 | 5.10 | 421.3 | 465.3 | ok |
| lmdb | staged | 16 | 2600 | 5.45 | 394.3 | 420.8 | ok |
| lmdb | memcpy | 1 | 5085 | 10.66 | 204.0 | 247.3 | ok |
| filedir-pread | staged | 1 | 1964 | 4.12 | 510.8 | 559.5 | ok |
| filedir-pread | staged | 16 | 2399 | 5.03 | 424.7 | 433.6 | ok |
| filedir-pread | memcpy | 1 | 4984 | 10.45 | 206.9 | 223.6 | ok |

**The ceiling reference.** `pinned-ceiling` — a 2 MiB `cudaMemcpyAsync` out of
a `cudaMallocHost` buffer, no store in the path — reaches **12.82 GB/s**.
PCIe 3.0 ×16 is 15.75 GB/s of raw lane rate after 128b/130b encoding, so
12.82 GB/s is ~81% of it, which is what a gen-3 ×16 link delivers once TLP
headers and the DMA engine are accounted for. Nothing store-backed can beat
it, and it is the denominator below:

| path (batch 16) | GB/s | % of the 12.82 GB/s pinned ceiling |
|---|---:|---:|
| cyclone `zerocopy-persistent` | 12.79 | **99.8%** |
| lmdb `zerocopy-persistent` | 12.81 | 99.9% |
| cyclone `pageable` (batch 1) | 5.82 | 45.4% |
| cyclone `staged` | 5.51 | 43.0% |
| lmdb `staged` | 5.45 | 42.5% |
| filedir-pread `staged` | 5.03 | 39.2% |
| cyclone `zerocopy` | 4.36 | 34.0% |

### What it says

**1. CUDA submission is nearly free, so this is a bandwidth story, not a
submission story — the opposite of Metal.** A 0-byte `cudaMemcpyAsync` plus
`cudaStreamSynchronize` costs **0.46 µs**; Metal's empty command buffer cost
13.8 µs, thirty times more. A 4 KiB transfer costs 3.8 µs against Metal's
173.7 µs. The consequence is that **batching barely matters here**: the
ceiling moves 12.75 → 12.82 GB/s from batch 1 to batch 16, and
`zerocopy-persistent` 12.48 → 12.79. On Metal the headline advice was "batch
or you are measuring the submission"; on CUDA a single 2 MiB transfer already
runs at 98% of what sixteen batched ones do. Everything that separates the
rows below is the **host copy**, not the enqueue.

**2. Registering per block is a loss — and a bigger one than doing nothing
clever at all.** Cyclone's `zerocopy` reaches 4.36 GB/s batched, against 5.51
staged: **0.79×, a 21% loss**. It is also slower than `pageable` (5.82 GB/s),
the naive `cudaMemcpy` straight from `content()` that lets the driver stage
through its own internal buffers — so pinning the borrowed span per read is
worse than not trying, by 1.33×. The p99 tells the same story louder: 1167 µs
against 401 µs staged, because `cudaHostRegister`/`cudaHostUnregister` walk
and wire page tables on every block and that work is spiky. LMDB agrees
(5.37 vs 5.45, a wash at best). Making 2 MiB of host memory DMA-able costs
about what copying it costs, on both platforms — this is the one conclusion
Metal and CUDA reach identically.

**3. Registering the mapping *once* is the win — 2.3×, and it lands on the
bus ceiling.** Page-lock Cyclone's whole volume mapping once and a block
becomes nothing but its borrowed pointer: **12.79 GB/s at batch 16, p50
162.5 µs**, against **5.51 GB/s / 394.5 µs** staged — **2.32× throughput,
2.43× lower latency** — and **99.8% of the 12.82 GB/s pinned-buffer PCIe
ceiling**. The staged path cannot get there and the reason is arithmetic, not
tuning: the host `memcpy` alone runs at 10.81 GB/s, so staging serializes a
10.81 GB/s copy with a 12.82 GB/s bus and lands at 1/(1/10.81 + 1/12.82) =
5.87 GB/s — within noise of the 5.51 measured. Removing the copy removes the
whole of that. `filedir-pread`, which has no borrowed-buffer API and *must*
stage, reaches 5.03 GB/s: Cyclone's persistent form is **2.54× ahead** of it.
LMDB reaches the same ceiling (12.81 GB/s), which is the honest framing — this
is a property of *any* store that keeps every value inside one registrable
mapping, and Cyclone and LMDB are the two here that do.

That is the discrete-GPU confirmation of the Metal result, and it is
stronger: on unified memory the persistent wrap won 1.9× against a copy
between two regions of the same DRAM, while over a real bus it wins 2.3× and
saturates the link. The API for the winning form already exists — `content()`
for the pointer, `content_file_offset()` for the offset, `volume_files()` for
the file — so an embedder can build the persistent registration today. What
is missing is the same thing Metal wanted: an entry point that hands over the
mapping identity directly, instead of making the caller infer the span from
the pointers reads happen to return.

**8 MiB, partial.** An 8 MiB sweep was attempted and abandoned, but its
Cyclone half completed before the box ran out of room and says the same
thing: ceiling 13.07 GB/s, `zerocopy-persistent` **12.82 GB/s (98.1% of it)**
against **5.60 GB/s** staged — **2.29×** — while per-block `zerocopy`
collapses to 2.15 GB/s, a **61% loss** against staging rather than 2 MiB's
21%, because the registration cost scales with the span while the copy it
saves does not. The run was stopped during the LMDB phase: at 8 MiB the three
stores hold ~12 GiB of blocks and the registered volume mapping another
7.75 GiB, which does not fit this machine's 23 GiB, and what it was measuring
by then was page reclaim. There is no `.jsonl` for it for that reason. The
2 MiB configuration above (1 GiB per store) sits comfortably in RAM and is
the one to cite.

Two practical notes for anyone doing this. The mapping must be registered
with `cudaHostRegisterReadOnly` if it is `PROT_READ` — including the
GPUDirect-shaped A3 surface — and page-locking multi-GiB spans needs
`memlock` raised; the benchmark degrades to 1 GiB windows and says so when a
single registration is refused, which is functionally identical for the
transfer.

### Running it

The target is opt-in — `CYCLONE_BUILD_CUDA_BENCHMARKS` is `OFF` by default
and everything it needs, `enable_language(CUDA)` and
`find_package(CUDAToolkit)` included, is inside that guard, so an ordinary
configure never probes for `nvcc`. LMDB is optional exactly as for the Metal
target.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=clang++-20 \
  -DCYCLONE_USE_BUNDLED_SHA256=ON \
  -DCYCLONE_BUILD_CUDA_BENCHMARKS=ON \
  -DCMAKE_CUDA_HOST_COMPILER=g++-13 \
  -DCMAKE_CUDA_ARCHITECTURES=61 \
  -DCMAKE_CUDA_COMPILER=/usr/local/cuda/bin/nvcc
cmake --build build -j --target kv_gpu_cuda
./build/kv_gpu_cuda --block-size 2097152 --seconds 10 --path /fast/ssd/kvdata
```

`-DCMAKE_CUDA_COMPILER` is only needed when the toolkit's `nvcc` is not on
`PATH`, which is the default for the NVIDIA apt packages (they install under
`/usr/local/cuda`). `-DCMAKE_CUDA_ARCHITECTURES` must match the device —
`61` is Pascal; `nvidia-smi --query-gpu=compute_cap --format=csv` prints it.

Cyclone's public headers are C++23 (`std::expected`) and nvcc 12.4 stops at
C++20, so the target is deliberately two translation units: everything that
touches Cyclone, LMDB or the workload is compiled as C++23 by the host
compiler in `kv_gpu_cuda_host.cpp`, and only the CUDA runtime calls go
through nvcc in `kv_gpu_cuda.cu`, behind the plain-C seam in
`kv_gpu_cuda.h`.

## Reproducing

```bash
# Cyclone (this repository)
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCYCLONE_USE_BUNDLED_SHA256=ON && cmake --build build -j
./build/kv_bench --print-vectors                # must match the peer harness
./build/kv_bench --seconds 10 --path /fast/ssd --output results/cyclone.jsonl
./build/kv_bench --block-size 2097152 --no-mmap-dir --no-verify --output results/cyclone-noverify.jsonl
```

The peer harness (file-per-block, LMDB, RocksDB adapters, `run_all.sh`,
`report.py`) lives outside this repository so it carries no third-party
dependencies here; it implements
[`kv-workload-spec.md`](kv-cache-benchmark/kv-workload-spec.md) and prints
the same reference vectors. Run each store's sizes with nothing else on the
machine and delete each size's data before the next.

## Appendix A — macOS generated tables

Cells are `ops/s / GB/s / p99 µs` (restart adds hit fraction). `view` touches
one byte per 4 KiB page of the returned span; `copy` memcpys the value into
a preallocated buffer.

### Phase 1 - PUT (single writer, sequential)

Cells: ops/s / GB/s / p99 us.

| block size | cyclone | cyclone-nomadv | cyclone-noverify | filedir | filedir-read | lmdb | rocksdb |
|---|---|---|---|---|---|---|---|
| 512 KiB | 910.6 / 0.48 / 1030 | - | - | 2.7k / 1.43 / 3403 | 2.6k / 1.36 / 3196 | 112.2 / 0.06 / 12145 | 1.4k / 0.72 / 8173 |
| 2 MiB | 201.3 / 0.42 / 8065 | 196.0 / 0.41 / 7342 | 187.4 / 0.39 / 8090 | 695.7 / 1.46 / 6420 | 575.8 / 1.21 / 5071 | 88.2 / 0.18 / 18425 | 302.5 / 0.63 / 39768 |
| 8 MiB | 53.9 / 0.45 / 47693 | - | - | 155.8 / 1.31 / 26998 | 145.1 / 1.22 / 14575 | 63.5 / 0.53 / 24333 | 78.9 / 0.66 / 51013 |
| 32 MiB | 12.0 / 0.40 / 117506 | - | - | 37.2 / 1.25 / 128608 | 32.5 / 1.09 / 80565 | 28.9 / 0.97 / 52087 | 19.9 / 0.67 / 112563 |

### Phase 2 - GET first touch (single thread, view)

Cells: ops/s / GB/s / p99 us.

| block size | cyclone | cyclone-nomadv | cyclone-noverify | filedir | filedir-read | lmdb | rocksdb |
|---|---|---|---|---|---|---|---|
| 512 KiB | 1.1k / 0.56 / 982 | - | - | 39.3k / 20.63 / 32 | 29.4k / 15.40 / 43 | 36.7k / 19.23 / 75 | 18.0k / 9.42 / 76 |
| 2 MiB | 268.1 / 0.56 / 3926 | 180.3 / 0.38 / 15341 | 17.7k / 37.11 / 64 | 3.8k / 8.04 / 5683 | 2.3k / 4.79 / 518 | 353.7 / 0.74 / 9811 | 1.6k / 3.42 / 1745 |
| 8 MiB | 24.6 / 0.21 / 67259 | - | - | 3.8k / 31.84 / 303 | 2.7k / 23.00 / 443 | 3.4k / 28.31 / 365 | 824.5 / 6.92 / 3002 |
| 32 MiB | 5.3 / 0.18 / 323226 | - | - | 64.1 / 2.15 / 78738 | 540.4 / 18.13 / 2028 | 868.4 / 29.14 / 1226 | 124.1 / 4.16 / 15897 |

### Phase 3 - GET warm, Zipf(0.99)

#### view, T = 1

Cells: gets/s / GB/s / p99 us.

| block size | cyclone | cyclone-nomadv | cyclone-noverify | filedir | filedir-read | lmdb | rocksdb |
|---|---|---|---|---|---|---|---|
| 512 KiB | 2.18M / 1140.52 / 1 | - | - | 42.1k / 22.07 / 33 | 39.0k / 20.45 / 36 | 1.28M / 673.07 / 1 | 18.7k / 9.79 / 66 |
| 2 MiB | 668.1k / 1401.04 / 3 | 603.4k / 1265.43 / 4 | 672.4k / 1410.10 / 3 | 15.4k / 32.31 / 85 | 11.6k / 24.27 / 149 | 513.1k / 1076.11 / 3 | 4.4k / 9.21 / 665 |
| 8 MiB | 152.0k / 1275.12 / 11 | - | - | 4.0k / 33.77 / 378 | 2.7k / 22.46 / 610 | 139.4k / 1169.45 / 11 | 953.5 / 8.00 / 2127 |
| 32 MiB | 35.4k / 1188.92 / 44 | - | - | 1.0k / 35.08 / 1275 | 544.9 / 18.28 / 2115 | 33.1k / 1109.51 / 43 | 221.6 / 7.43 / 9707 |

#### copy, T = 1

Cells: gets/s / GB/s / p99 us.

| block size | cyclone | cyclone-nomadv | cyclone-noverify | filedir | filedir-read | lmdb | rocksdb |
|---|---|---|---|---|---|---|---|
| 512 KiB | 131.1k / 68.75 / 10 | - | - | 27.7k / 14.55 / 49 | 30.9k / 16.21 / 43 | 126.5k / 66.34 / 10 | 16.7k / 8.76 / 72 |
| 2 MiB | 33.8k / 70.88 / 38 | 31.9k / 66.88 / 45 | 33.6k / 70.55 / 37 | 8.6k / 17.98 / 146 | 9.0k / 18.94 / 156 | 30.9k / 64.89 / 72 | 3.9k / 8.07 / 342 |
| 8 MiB | 8.3k / 69.38 / 157 | - | - | 2.2k / 18.65 / 550 | 2.2k / 18.85 / 543 | 8.1k / 67.55 / 197 | 834.1 / 7.00 / 2160 |
| 32 MiB | 1.7k / 57.59 / 742 | - | - | 500.1 / 16.78 / 3023 | 422.4 / 14.17 / 5574 | 1.8k / 58.82 / 629 | 201.6 / 6.77 / 10228 |

#### view, T = 4

Cells: gets/s / GB/s / p99 us.

| block size | cyclone | cyclone-nomadv | cyclone-noverify | filedir | filedir-read | lmdb | rocksdb |
|---|---|---|---|---|---|---|---|
| 512 KiB | 6.83M / 3579.28 / 1 | - | - | 100.6k / 52.73 / 59 | 95.4k / 50.04 / 69 | 3.16M / 1656.23 / 2 | 53.0k / 27.81 / 136 |
| 2 MiB | 1.77M / 3709.75 / 5 | 1.52M / 3197.42 / 5 | 1.73M / 3636.05 / 4 | 38.8k / 81.28 / 138 | 18.0k / 37.66 / 330 | 783.3k / 1642.63 / 6 | 8.9k / 18.60 / 593 |
| 8 MiB | 332.1k / 2785.89 / 21 | - | - | 10.5k / 88.20 / 496 | 3.6k / 29.98 / 1822 | 183.6k / 1540.40 / 25 | 1.5k / 12.61 / 5635 |
| 32 MiB | 83.5k / 2801.02 / 81 | - | - | 2.8k / 93.71 / 2406 | 891.3 / 29.91 / 7502 | 45.0k / 1508.55 / 99 | 389.0 / 13.05 / 13921 |

#### copy, T = 4

Cells: gets/s / GB/s / p99 us.

| block size | cyclone | cyclone-nomadv | cyclone-noverify | filedir | filedir-read | lmdb | rocksdb |
|---|---|---|---|---|---|---|---|
| 512 KiB | 149.4k / 78.35 / 34 | - | - | 72.9k / 38.24 / 77 | 70.5k / 36.98 / 116 | 152.9k / 80.16 / 34 | 39.6k / 20.74 / 142 |
| 2 MiB | 37.8k / 79.17 / 131 | 37.9k / 79.52 / 144 | 37.3k / 78.31 / 128 | 22.0k / 46.15 / 256 | 13.8k / 29.00 / 669 | 37.1k / 77.73 / 125 | 6.2k / 12.91 / 796 |
| 8 MiB | 8.4k / 70.40 / 659 | - | - | 5.1k / 43.17 / 1008 | 2.6k / 21.64 / 2297 | 8.4k / 70.64 / 603 | 1.3k / 11.00 / 4980 |
| 32 MiB | 1.9k / 64.52 / 2468 | - | - | 1.2k / 41.93 / 4284 | 618.4 / 20.75 / 9599 | 1.9k / 64.56 / 2663 | 325.4 / 10.92 / 15881 |

#### view, T = 8

Cells: gets/s / GB/s / p99 us.

| block size | cyclone | cyclone-nomadv | cyclone-noverify | filedir | filedir-read | lmdb | rocksdb |
|---|---|---|---|---|---|---|---|
| 512 KiB | 8.38M / 4394.24 / 2 | - | - | 99.2k / 52.00 / 141 | 117.6k / 61.63 / 125 | 4.81M / 2520.37 / 3 | 77.2k / 40.49 / 187 |
| 2 MiB | 2.46M / 5155.15 / 8 | 2.11M / 4425.18 / 9 | 2.52M / 5289.80 / 7 | 37.0k / 77.65 / 335 | 23.9k / 50.15 / 749 | 1.22M / 2566.45 / 10 | 12.8k / 26.81 / 1295 |
| 8 MiB | 490.9k / 4118.14 / 36 | - | - | 10.4k / 87.20 / 1398 | 4.5k / 37.93 / 3559 | 297.4k / 2494.53 / 41 | 1.8k / 15.45 / 9034 |
| 32 MiB | 131.3k / 4406.26 / 118 | - | - | 2.8k / 92.74 / 8053 | 1.0k / 34.34 / 16255 | 72.6k / 2436.09 / 178 | 446.6 / 14.98 / 25765 |

#### copy, T = 8

Cells: gets/s / GB/s / p99 us.

| block size | cyclone | cyclone-nomadv | cyclone-noverify | filedir | filedir-read | lmdb | rocksdb |
|---|---|---|---|---|---|---|---|
| 512 KiB | 238.4k / 125.00 / 57 | - | - | 82.2k / 43.10 / 159 | 98.3k / 51.52 / 149 | 227.6k / 119.35 / 61 | 57.2k / 29.97 / 256 |
| 2 MiB | 51.2k / 107.38 / 314 | 40.6k / 85.11 / 586 | 51.1k / 107.25 / 300 | 27.1k / 56.79 / 427 | 19.1k / 40.02 / 871 | 47.8k / 100.34 / 395 | 8.9k / 18.57 / 2280 |
| 8 MiB | 8.8k / 73.90 / 1858 | - | - | 6.4k / 53.74 / 2558 | 2.9k / 24.49 / 5014 | 8.8k / 73.42 / 1872 | 1.5k / 12.98 / 9965 |
| 32 MiB | 2.1k / 69.35 / 7488 | - | - | 1.6k / 53.57 / 10840 | 686.0 / 23.02 / 19165 | 1.9k / 65.14 / 8400 | 359.4 / 12.06 / 31399 |

### Phase 4 - RESTART (close, reopen, GET all N)

Cells: ops/s / GB/s / p99 us / hit fraction.

| block size | cyclone | cyclone-nomadv | cyclone-noverify | filedir | filedir-read | lmdb | rocksdb |
|---|---|---|---|---|---|---|---|
| 512 KiB | 1.1k / 0.56 / 987 / 1.000 | - | - | 37.2k / 19.51 / 35 / 1.000 | 32.1k / 16.84 / 46 / 1.000 | 61.4k / 32.17 / 23 / 1.000 | 18.5k / 9.70 / 76 / 1.000 |
| 2 MiB | 268.0 / 0.56 / 3859 / 1.000 | 254.8 / 0.53 / 9029 / 1.000 | 0.0 / 0.00 / 0 / 0.000 | 13.3k / 27.98 / 91 / 1.000 | 6.9k / 14.56 / 511 / 1.000 | 15.3k / 32.09 / 94 / 1.000 | 3.2k / 6.62 / 705 / 1.000 |
| 8 MiB | 67.0 / 0.56 / 15985 / 1.000 | - | - | 3.8k / 31.62 / 311 / 1.000 | 1.3k / 11.20 / 10620 / 1.000 | 3.6k / 29.80 / 312 / 1.000 | 738.7 / 6.20 / 2260 / 1.000 |
| 32 MiB | 16.6 / 0.56 / 62559 / 1.000 | - | - | 547.9 / 18.38 / 13301 / 1.000 | 366.8 / 12.31 / 6262 / 1.000 | 856.1 / 28.73 / 1248 / 1.000 | 169.5 / 5.69 / 8697 / 1.000 |

### Phase 5 - MULTI-PROCESS READ (P=4 processes, T=1)

#### view

Cells: ops/s / GB/s / hit fraction.

| block size | cyclone | cyclone-nomadv | cyclone-noverify | filedir | filedir-read | lmdb | rocksdb |
|---|---|---|---|---|---|---|---|
| 512 KiB | 5.30M / 2779.63 | - | - | 116.4k / 61.01 / 1.000 | 91.6k / 48.00 / 1.000 | 3.23M / 1691.04 / 1.000 | - |
| 2 MiB | 728.5k / 1527.81 | 564.3k / 1183.39 | - | 49.5k / 103.72 / 1.000 | 17.7k / 37.09 / 1.000 | 780.6k / 1636.98 / 1.000 | - |
| 8 MiB | 134.5k / 1128.32 | - | - | 14.2k / 118.80 / 1.000 | 3.5k / 29.59 / 1.000 | 188.2k / 1578.43 / 1.000 | - |
| 32 MiB | 34.9k / 1172.20 | - | - | 3.0k / 99.17 / 1.000 | 855.5 / 28.70 / 1.000 | 43.9k / 1473.22 / 1.000 | - |

#### copy

Cells: ops/s / GB/s / hit fraction.

| block size | cyclone | cyclone-nomadv | cyclone-noverify | filedir | filedir-read | lmdb | rocksdb |
|---|---|---|---|---|---|---|---|
| 512 KiB | 132.8k / 69.62 | - | - | 80.0k / 41.93 / 1.000 | 78.7k / 41.26 / 1.000 | 151.3k / 79.33 / 1.000 | - |
| 2 MiB | 22.0k / 46.12 | - | - | 24.0k / 50.31 / 1.000 | 13.8k / 28.93 / 1.000 | 37.0k / 77.64 / 1.000 | - |
| 8 MiB | 5.0k / 41.77 | - | - | 5.6k / 46.62 / 1.000 | 2.6k / 21.54 / 1.000 | 8.4k / 70.74 / 1.000 | - |
| 32 MiB | 1.1k / 38.17 | - | - | 1.3k / 45.11 / 1.000 | 603.2 / 20.24 / 1.000 | 1.9k / 63.08 / 1.000 | - |

### Read scaling

#### Read scaling at 2 MiB, mode = view

Cells: gets/s / GB/s.

| threads | cyclone | cyclone-nomadv | cyclone-noverify | filedir | filedir-read | lmdb | rocksdb |
|---|---|---|---|---|---|---|---|
| 1 | 668.1k / 1401.04 | 603.4k / 1265.43 | 672.4k / 1410.10 | 15.4k / 32.31 | 11.6k / 24.27 | 513.1k / 1076.11 | 4.4k / 9.21 |
| 4 | 1.77M / 3709.75 | 1.52M / 3197.42 | 1.73M / 3636.05 | 38.8k / 81.28 | 18.0k / 37.66 | 783.3k / 1642.63 | 8.9k / 18.60 |
| 8 | 2.46M / 5155.15 | 2.11M / 4425.18 | 2.52M / 5289.80 | 37.0k / 77.65 | 23.9k / 50.15 | 1.22M / 2566.45 | 12.8k / 26.81 |

#### Read scaling at 2 MiB, mode = copy

Cells: gets/s / GB/s.

| threads | cyclone | cyclone-nomadv | cyclone-noverify | filedir | filedir-read | lmdb | rocksdb |
|---|---|---|---|---|---|---|---|
| 1 | 33.8k / 70.88 | 31.9k / 66.88 | 33.6k / 70.55 | 8.6k / 17.98 | 9.0k / 18.94 | 30.9k / 64.89 | 3.9k / 8.07 |
| 4 | 37.8k / 79.17 | 37.9k / 79.52 | 37.3k / 78.31 | 22.0k / 46.15 | 13.8k / 29.00 | 37.1k / 77.73 | 6.2k / 12.91 |
| 8 | 51.2k / 107.38 | 40.6k / 85.11 | 51.1k / 107.25 | 27.1k / 56.79 | 19.1k / 40.02 | 47.8k / 100.34 | 8.9k / 18.57 |

## Appendix B — Linux generated tables

Ubuntu 22.04, i7-8750H, Samsung 970 PRO; page cache dropped before phases 2
and 4. Cells as in Appendix A.

### Phase 1 - PUT (single writer, sequential)

Cells: ops/s / GB/s / p99 us.

| block size | cyclone | cyclone-noverify | filedir | filedir-read | lmdb | rocksdb |
|---|---|---|---|---|---|---|
| 512 KiB | 579.0 / 0.30 / 1888 | - | 2.6k / 1.38 / 330 | 2.6k / 1.37 / 351 | 115.6 / 0.06 / 12651 | 1.1k / 0.59 / 1469 |
| 2 MiB | 163.8 / 0.34 / 13113 | 136.1 / 0.29 / 7776 | 686.5 / 1.44 / 1092 | 660.7 / 1.39 / 1134 | 72.1 / 0.15 / 20484 | 291.9 / 0.61 / 39055 |
| 8 MiB | 41.6 / 0.35 / 27850 | - | 159.8 / 1.34 / 5255 | 156.8 / 1.32 / 4770 | 42.0 / 0.35 / 30866 | 71.9 / 0.60 / 55080 |
| 32 MiB | 7.5 / 0.25 / 130183 | - | 42.5 / 1.42 / 18911 | 38.8 / 1.30 / 21741 | 13.5 / 0.45 / 70673 | 15.6 / 0.52 / 60536 |

### Phase 2 - GET first touch (single thread, view)

Cells: ops/s / GB/s / p99 us.

| block size | cyclone | cyclone-noverify | filedir | filedir-read | lmdb | rocksdb |
|---|---|---|---|---|---|---|
| 512 KiB | 137.0 / 0.07 / 13232 | - | 1.8k / 0.94 / 879 | 2.3k / 1.20 / 798 | 3.7k / 1.93 / 1417 | 1.7k / 0.87 / 982 |
| 2 MiB | 30.5 / 0.06 / 59454 | 84.2 / 0.18 / 18729 | 838.8 / 1.76 / 2356 | 621.5 / 1.30 / 2548 | 1.0k / 2.15 / 3917 | 390.2 / 0.82 / 4571 |
| 8 MiB | 17.2 / 0.14 / 63363 | - | 299.4 / 2.51 / 6188 | 253.6 / 2.13 / 6470 | 252.7 / 2.12 / 6555 | 103.2 / 0.87 / 15543 |
| 32 MiB | 4.7 / 0.16 / 229454 | - | 77.5 / 2.60 / 16553 | 33.0 / 1.11 / 39479 | 65.5 / 2.20 / 21097 | 13.7 / 0.46 / 102098 |

### Phase 3 - GET warm, Zipf(0.99)

#### view, T = 1

Cells: gets/s / GB/s / p99 us.

| block size | cyclone | cyclone-noverify | filedir | filedir-read | lmdb | rocksdb |
|---|---|---|---|---|---|---|
| 512 KiB | 854.7k / 448.13 / 2 | - | 25.4k / 13.33 / 57 | 21.5k / 11.29 / 73 | 678.9k / 355.95 / 2 | 9.5k / 5.01 / 138 |
| 2 MiB | 274.4k / 575.37 / 5 | 278.7k / 584.57 / 5 | 7.0k / 14.73 / 195 | 5.3k / 11.17 / 258 | 244.6k / 512.96 / 5 | 1.8k / 3.87 / 620 |
| 8 MiB | 69.5k / 582.87 / 16 | - | 1.9k / 15.56 / 679 | 1.1k / 9.22 / 1127 | 66.8k / 560.36 / 16 | 364.6 / 3.06 / 3087 |
| 32 MiB | 17.6k / 589.45 / 74 | - | 457.4 / 15.35 / 2725 | 217.7 / 7.30 / 5702 | 17.1k / 574.40 / 62 | 29.8 / 1.00 / 34898 |

#### copy, T = 1

Cells: gets/s / GB/s / p99 us.

| block size | cyclone | cyclone-noverify | filedir | filedir-read | lmdb | rocksdb |
|---|---|---|---|---|---|---|
| 512 KiB | 33.1k / 17.36 / 46 | - | 13.9k / 7.28 / 106 | 16.4k / 8.58 / 94 | 31.4k / 16.48 / 46 | 7.8k / 4.07 / 148 |
| 2 MiB | 5.7k / 11.91 / 235 | 5.6k / 11.83 / 235 | 3.1k / 6.49 / 400 | 3.8k / 8.03 / 328 | 6.1k / 12.85 / 224 | 1.4k / 2.99 / 767 |
| 8 MiB | 1.4k / 11.65 / 806 | - | 808.3 / 6.78 / 1420 | 772.5 / 6.48 / 1441 | 1.4k / 11.82 / 808 | 289.8 / 2.43 / 3628 |
| 32 MiB | 346.0 / 11.61 / 3181 | - | 206.2 / 6.92 / 5491 | 135.5 / 4.55 / 7648 | 314.8 / 10.56 / 3388 | 27.6 / 0.93 / 40346 |

#### view, T = 4

Cells: gets/s / GB/s / p99 us.

| block size | cyclone | cyclone-noverify | filedir | filedir-read | lmdb | rocksdb |
|---|---|---|---|---|---|---|
| 512 KiB | 2.97M / 1556.56 / 2 | - | 27.3k / 14.32 / 332 | 52.8k / 27.66 / 104 | 2.13M / 1117.56 / 3 | 24.0k / 12.57 / 225 |
| 2 MiB | 754.9k / 1583.13 / 9 | 764.4k / 1603.02 / 9 | 11.1k / 23.31 / 772 | 6.4k / 13.43 / 710 | 624.9k / 1310.58 / 9 | 1.7k / 3.63 / 2843 |
| 8 MiB | 158.2k / 1326.75 / 41 | - | 3.6k / 30.27 / 1976 | 1.2k / 10.15 / 3569 | 146.0k / 1224.42 / 45 | 390.2 / 3.27 / 10656 |
| 32 MiB | 38.0k / 1274.45 / 145 | - | 1.1k / 37.82 / 5419 | 249.6 / 8.38 / 16921 | 36.3k / 1216.70 / 150 | 68.9 / 2.31 / 64826 |

#### copy, T = 4

Cells: gets/s / GB/s / p99 us.

| block size | cyclone | cyclone-noverify | filedir | filedir-read | lmdb | rocksdb |
|---|---|---|---|---|---|---|
| 512 KiB | 59.1k / 30.96 / 106 | - | 27.5k / 14.44 / 223 | 41.7k / 21.88 / 122 | 60.1k / 31.53 / 110 | 13.5k / 7.09 / 343 |
| 2 MiB | 5.6k / 11.64 / 864 | 5.6k / 11.81 / 854 | 4.8k / 10.00 / 1087 | 3.8k / 7.95 / 1154 | 5.7k / 11.86 / 836 | 1.4k / 2.86 / 3174 |
| 8 MiB | 1.4k / 11.36 / 3222 | - | 1.2k / 10.42 / 4172 | 697.8 / 5.85 / 6034 | 1.3k / 11.30 / 3207 | 300.8 / 2.52 / 13734 |
| 32 MiB | 308.6 / 10.36 / 14711 | - | 324.8 / 10.90 / 15112 | 124.9 / 4.19 / 33132 | 316.0 / 10.60 / 13624 | 57.4 / 1.93 / 75925 |

#### view, T = 8

Cells: gets/s / GB/s / p99 us.

| block size | cyclone | cyclone-noverify | filedir | filedir-read | lmdb | rocksdb |
|---|---|---|---|---|---|---|
| 512 KiB | 4.26M / 2233.45 / 4 | - | 28.7k / 15.05 / 721 | 48.3k / 25.30 / 266 | 2.77M / 1452.10 / 6 | 10.0k / 5.24 / 1667 |
| 2 MiB | 799.9k / 1677.44 / 21 | 812.6k / 1704.24 / 20 | 10.2k / 21.38 / 1717 | 5.0k / 10.50 / 2431 | 649.2k / 1361.45 / 21 | 1.6k / 3.40 / 7254 |
| 8 MiB | 158.4k / 1328.49 / 86 | - | 3.4k / 28.79 / 4157 | 1.2k / 10.11 / 9760 | 149.0k / 1250.24 / 88 | 374.2 / 3.14 / 29980 |
| 32 MiB | 38.6k / 1296.12 / 345 | - | 1.1k / 35.98 / 11570 | 235.8 / 7.91 / 48260 | 37.2k / 1248.82 / 351 | 69.7 / 2.34 / 152073 |

#### copy, T = 8

Cells: gets/s / GB/s / p99 us.

| block size | cyclone | cyclone-noverify | filedir | filedir-read | lmdb | rocksdb |
|---|---|---|---|---|---|---|
| 512 KiB | 51.5k / 27.02 / 264 | - | 25.2k / 13.19 / 503 | 24.2k / 12.67 / 614 | 47.0k / 24.63 / 287 | 6.1k / 3.19 / 2396 |
| 2 MiB | 5.5k / 11.61 / 2363 | 5.5k / 11.56 / 2353 | 4.4k / 9.32 / 2581 | 2.9k / 6.05 / 4107 | 5.4k / 11.42 / 2073 | 1.3k / 2.64 / 9224 |
| 8 MiB | 1.3k / 11.11 / 9293 | - | 1.2k / 9.66 / 10079 | 644.6 / 5.41 / 17486 | 1.3k / 11.05 / 8197 | 271.9 / 2.28 / 42142 |
| 32 MiB | 299.6 / 10.05 / 45565 | - | 288.8 / 9.69 / 40300 | 122.8 / 4.12 / 91355 | 321.5 / 10.79 / 40234 | 56.5 / 1.90 / 192802 |

### Phase 4 - RESTART (close, reopen, GET all N)

Cells: ops/s / GB/s / p99 us / hit fraction.

| block size | cyclone | cyclone-noverify | filedir | filedir-read | lmdb | rocksdb |
|---|---|---|---|---|---|---|
| 512 KiB | 144.6 / 0.08 / 12856 / 1.000 | - | 1.8k / 0.94 / 935 / 1.000 | 2.0k / 1.03 / 751 / 1.000 | 3.7k / 1.92 / 1390 / 1.000 | 1.8k / 0.92 / 857 / 1.000 |
| 2 MiB | 30.9 / 0.06 / 56714 / 1.000 | 0.0 / 0.00 / 1 / 0.000 | 823.0 / 1.73 / 2440 / 1.000 | 624.8 / 1.31 / 2537 / 1.000 | 1.0k / 2.16 / 3628 / 1.000 | 390.6 / 0.82 / 4361 / 1.000 |
| 8 MiB | 17.3 / 0.14 / 63020 / 1.000 | - | 300.2 / 2.52 / 5731 / 1.000 | 256.1 / 2.15 / 6462 / 1.000 | 258.8 / 2.17 / 6377 / 1.000 | 101.0 / 0.85 / 15478 / 1.000 |
| 32 MiB | 4.6 / 0.15 / 227921 / 1.000 | - | 78.5 / 2.63 / 14696 / 1.000 | 33.2 / 1.11 / 39372 / 1.000 | 64.1 / 2.15 / 20329 / 1.000 | 16.6 / 0.56 / 73849 / 1.000 |

### Phase 5 - MULTI-PROCESS READ (P=4 processes, T=1)

#### view

Cells: ops/s / GB/s / hit fraction.

| block size | cyclone | cyclone-noverify | filedir | filedir-read | lmdb | rocksdb |
|---|---|---|---|---|---|---|
| 512 KiB | 2.17M / 1138.23 | - | 85.6k / 44.90 / 1.000 | 51.8k / 27.15 / 1.000 | 2.09M / 1097.54 / 1.000 | - |
| 2 MiB | 216.8k / 454.73 | - | 26.0k / 54.53 / 1.000 | 6.5k / 13.54 / 1.000 | 585.0k / 1226.91 / 1.000 | - |
| 8 MiB | 46.5k / 389.89 | - | 6.3k / 52.54 / 1.000 | 1.2k / 9.70 / 1.000 | 146.4k / 1227.97 / 1.000 | - |
| 32 MiB | 11.2k / 375.25 | - | 1.5k / 51.48 / 1.000 | 260.4 / 8.74 / 1.000 | 35.9k / 1203.07 / 1.000 | - |

#### copy

Cells: ops/s / GB/s / hit fraction.

| block size | cyclone | cyclone-noverify | filedir | filedir-read | lmdb | rocksdb |
|---|---|---|---|---|---|---|
| 512 KiB | 50.7k / 26.56 | - | 40.5k / 21.25 / 1.000 | 39.9k / 20.90 / 1.000 | 61.9k / 32.44 / 1.000 | - |
| 2 MiB | 4.4k / 9.29 | - | 5.6k / 11.84 / 1.000 | 3.7k / 7.77 / 1.000 | 5.2k / 10.89 / 1.000 | - |
| 8 MiB | 1.0k / 8.63 | - | 1.4k / 11.66 / 1.000 | 646.4 / 5.42 / 1.000 | 1.3k / 10.55 / 1.000 | - |
| 32 MiB | 239.1 / 8.02 | - | 333.0 / 11.17 / 1.000 | 130.8 / 4.39 / 1.000 | 270.2 / 9.07 / 1.000 | - |

### Read scaling

#### Read scaling at 2 MiB, mode = view

Cells: gets/s / GB/s.

| threads | cyclone | cyclone-noverify | filedir | filedir-read | lmdb | rocksdb |
|---|---|---|---|---|---|---|
| 1 | 274.4k / 575.37 | 278.7k / 584.57 | 7.0k / 14.73 | 5.3k / 11.17 | 244.6k / 512.96 | 1.8k / 3.87 |
| 4 | 754.9k / 1583.13 | 764.4k / 1603.02 | 11.1k / 23.31 | 6.4k / 13.43 | 624.9k / 1310.58 | 1.7k / 3.63 |
| 8 | 799.9k / 1677.44 | 812.6k / 1704.24 | 10.2k / 21.38 | 5.0k / 10.50 | 649.2k / 1361.45 | 1.6k / 3.40 |

#### Read scaling at 2 MiB, mode = copy

Cells: gets/s / GB/s.

| threads | cyclone | cyclone-noverify | filedir | filedir-read | lmdb | rocksdb |
|---|---|---|---|---|---|---|
| 1 | 5.7k / 11.91 | 5.6k / 11.83 | 3.1k / 6.49 | 3.8k / 8.03 | 6.1k / 12.85 | 1.4k / 2.99 |
| 4 | 5.6k / 11.64 | 5.6k / 11.81 | 4.8k / 10.00 | 3.8k / 7.95 | 5.7k / 11.86 | 1.4k / 2.86 |
| 8 | 5.5k / 11.61 | 5.5k / 11.56 | 4.4k / 9.32 | 2.9k / 6.05 | 5.4k / 11.42 | 1.3k / 2.64 |
