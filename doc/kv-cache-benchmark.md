# Cyclone as an LLM KV-cache storage tier — benchmark

**Status:** first round, one laptop, treat as an instrument reading rather than
a marketing number. The point of this round is to find out where Cyclone
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
dropped without root on macOS. Linux/NVMe numbers with a quiesced cache are
the ones that matter for deployment and have not been taken yet.

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

1. **Hardware CRC32** (ARMv8 / SSE4.2 intrinsics run at >10 GB/s vs 0.55)
   and a way for the CRC-verified state to outlive the process — the
   validation cache could live beside the mmap directory so a restart and
   every peer process inherit it. This single item covers the two largest
   losses (restart, multi-process) and the first-touch floor.
2. **A `WriteHandle::reserve(n)` that hands back the destination span** so
   the caller writes or DMAs straight into the record, collapsing three
   copies to one; then revisit puts against file-per-block.
3. **Size-aware mapping advice**: keep `MADV_RANDOM` for small objects,
   advise `WILLNEED`/`SEQUENTIAL` per large read (`MappedFile` has the
   hooks, unused). Unproven here; measure on Linux with a quiesced page
   cache before and after.
4. Only then: a zero-copy C read entry point and a Python binding, which is
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
- No Linux/NVMe numbers yet; that is the deployment target and the next
  run, ideally with `echo 3 > drop_caches` between phases so first-touch
  becomes a real measurement.

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

## Appendix — generated tables

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
