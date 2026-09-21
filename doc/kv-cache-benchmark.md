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
the internal SSD, everything Release/`-O2`. **16 GiB RAM matters:** the 2 MiB
dataset is 4 GiB, so first-touch and restart phases include real SSD reads
and the warm phase is bimodal (Zipf head from the page cache, tail from disk).
Linux/NVMe numbers are the ones that matter for deployment and have not been
taken yet.

## Results (2026-09-21, Apple M5)

Full generated tables are in the appendix; raw JSON lines are in
[`doc/kv-cache-benchmark/`](kv-cache-benchmark/). `cyclone` is the stock
build with the tuning above; `cyclone-nomadv` is the same build with the
blanket `MADV_RANDOM` removed; `cyclone-noverify` uses the in-memory
directory and no CRC on read.

### Headline, 2 MiB blocks (the Llama-8B 16-token block)

| | cyclone | cyclone-nomadv | cyclone-noverify | filedir | filedir-read | lmdb | rocksdb |
|---|---:|---:|---:|---:|---:|---:|---:|
| PUT, GB/s | 0.36 | 0.41 | 0.39 | 0.86 | 2.40 | 0.19 ¹ | 0.64 |
| First-touch GET, GB/s | **0.14** | 0.38 | 37.1 | 0.51 | 4.35 | 0.45 | 3.51 |
| Warm GET copy, 1 thread, GB/s | **63.7** | 66.9 | 70.6 | 3.2 | 13.0 | 8.8 | 7.4 |
| Warm GET copy, 1 thread, p99 | 94 µs | 45 µs | 37 µs | 6.4 ms | 557 µs | 5.6 ms | 725 µs |
| Warm GET copy, 8 threads, GB/s | **113** | 85 | 107 | 24 | 27 | 30 | 16 |
| Restart: GET all, GB/s | 0.56 | 0.53 | — ² | 0.98 | 6.49 | 1.36 | 5.65 |
| Restart hit fraction | 1.0 | 1.0 | 0.0 ² | 1.0 | 1.0 | 1.0 | 1.0 |
| 4 reader processes, view, gets/s | **726 k** | 564 k | — | 9.7 k | 15.0 k | 9.3 k | n/a |

¹ LMDB is the only store that fsyncs every put. ² In-memory directory: the
index is not persistent, so a restart is a cold cache by design.

Across the other block sizes the picture is the same: Cyclone puts at
0.36–0.46 GB/s (file-per-block: 0.8–2.4, RocksDB 0.45–0.73), restarts at a
flat 0.56 GB/s at every size, and wins warm reads by 5–20× once blocks are
≥ 2 MiB (at 512 KiB, LMDB's zero-copy read ties it: 64 vs 60 GB/s).

### What the numbers say

**Where Cyclone wins — serving a warm prefix.** A warm get costs a
directory probe and a span into a mapping that already exists: no `open`,
no `mmap`/`munmap`, no `pread`, no transaction. That is why Cyclone reads a
hot 2 MiB block at 64 GB/s on one thread with a 94 µs p99 while the file
stores pay 0.5–6 ms p99 for their per-get syscalls, and why four reader
processes sharing one file sustain 726 k gets/s against 9–15 k. The absolute
GB/s in the warm phase is an artifact for *every* store — with Zipf(0.99)
the hot set sits in the CPU's last-level cache — so read the warm rows as
per-get overhead, not as memory bandwidth.

**Where Cyclone loses — the first read, and restart.** The first touch of a
freshly written 2 MiB block runs at 0.14 GB/s, 3–30× behind the peers, and a
restart re-reads the whole dataset at a flat 0.56 GB/s at every block size.
Two costs, both measured:

1. **Page-fault clustering.** Cyclone advises `MADV_RANDOM` on the whole
   mapping at open (right for 4 KB HTTP objects, wrong for 2 MiB tensors), so
   every 16 KiB page of a block faults on its own. Removing that one call
   (`cyclone-nomadv`) takes first-touch from 0.14 to 0.38 GB/s and p99 from
   26 ms to 15 ms.
2. **Table-driven CRC32.** The first read of an offset verifies the whole
   document's CRC32 at ≈0.55 GB/s — exactly the restart figure. The
   multi-process mode that makes the index persistent also forces
   `verify_checksum_on_read`, so restart-warmth and CRC-on-first-read come
   as a package today. With verification off (`cyclone-noverify`) first-touch
   is 37 GB/s.

**Writes.** 0.36–0.46 GB/s per thread, about half of file-per-block and on
par with RocksDB. The write path copies the value three times (handle
buffer → document builder → serialized record) and CRCs it before a single
`pwrite`; there is no scatter-gather or reserve-in-place API yet.

### What to change, in order

1. **Make the mapping advice size-aware.** Keep `MADV_RANDOM` for the small
   objects it was chosen for, but don't apply it blanket to the whole file;
   advise `WILLNEED`/`SEQUENTIAL` per large read (`MappedFile` already has
   `advise_sequential` / `advise_willneed`, unused). Measured: 2.7× on
   first-touch reads, no downside seen elsewhere in this workload.
2. **Hardware CRC32** (ARMv8 / SSE4.2 intrinsics) or a per-volume "trust the
   page cache" mode for read verification. Restart and first-touch are
   CRC-bound at 0.55 GB/s; the intrinsics run at >10 GB/s.
3. **A `WriteHandle::reserve(n)` that hands back the destination span** so
   a caller can generate or DMA straight into the record, collapsing three
   copies to one. Then revisit put bandwidth against file-per-block.
4. Only then: a zero-copy C read entry point and a Python binding, which is
   what a vLLM/SGLang connector would actually call.

### Caveats

- One laptop, one SSD, APFS, 16 GiB RAM, 10 s per point, single run. Treat
  differences under ~20 % as noise; the multi-× differences above are not.
- The peers measure a `std::string` allocation and a SHA-256 inside their
  warm-phase sample (≈1 µs); Cyclone hashes outside it. Irrelevant at ms
  latencies, slightly flatters Cyclone at µs ones.
- An earlier Cyclone run taken while other builds were running on the same
  machine showed a 1000× worse warm tail; it was discarded and re-run on an
  idle machine, which is the standard every number here meets.
- No Linux/NVMe numbers yet; that is the deployment target and the next run.

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
the same reference vectors.

## Appendix — generated tables

Cells are `ops/s / GB/s / p99 µs` (restart adds hit fraction). `view` touches
one byte per 4 KiB page of the returned span; `copy` memcpys the value into
a preallocated buffer.

### Phase 1 - PUT (single writer, sequential)

Cells: ops/s / GB/s / p99 us.

| block size | cyclone | cyclone-nomadv | cyclone-noverify | filedir | filedir-read | lmdb | rocksdb |
|---|---|---|---|---|---|---|---|
| 512 KiB | 883.5 / 0.46 / 1728 | - | - | 3.0k / 1.60 / 2352 | 4.5k / 2.33 / 1998 | 116.8 / 0.06 / 10813 | 1.4k / 0.73 / 9607 |
| 2 MiB | 171.0 / 0.36 / 10755 | 196.0 / 0.41 / 7342 | 187.4 / 0.39 / 8090 | 411.2 / 0.86 / 5662 | 1.1k / 2.40 / 3235 | 91.1 / 0.19 / 15090 | 302.8 / 0.64 / 36689 |
| 8 MiB | 52.4 / 0.44 / 45480 | - | - | 93.3 / 0.78 / 17582 | 110.2 / 0.92 / 18997 | 54.1 / 0.45 / 30858 | 59.9 / 0.50 / 49407 |
| 32 MiB | 12.3 / 0.41 / 105485 | - | - | 23.7 / 0.80 / 54846 | 21.4 / 0.72 / 62216 | 16.2 / 0.54 / 95198 | 13.5 / 0.45 / 106707 |

### Phase 2 - GET first touch (single thread, view)

Cells: ops/s / GB/s / p99 us.

| block size | cyclone | cyclone-nomadv | cyclone-noverify | filedir | filedir-read | lmdb | rocksdb |
|---|---|---|---|---|---|---|---|
| 512 KiB | 424.2 / 0.22 / 5610 | - | - | 36.0k / 18.87 / 36 | 32.8k / 17.19 / 39 | 7.5k / 3.91 / 1621 | 17.5k / 9.18 / 79 |
| 2 MiB | 66.8 / 0.14 / 25777 | 180.3 / 0.38 / 15341 | 17.7k / 37.11 / 64 | 240.8 / 0.51 / 6744 | 2.1k / 4.35 / 1561 | 216.9 / 0.45 / 11887 | 1.7k / 3.51 / 905 |
| 8 MiB | 67.4 / 0.57 / 15715 | - | - | 61.2 / 0.51 / 25098 | 743.2 / 6.23 / 1438 | 59.0 / 0.49 / 26615 | 488.6 / 4.10 / 2516 |
| 32 MiB | 11.0 / 0.37 / 259974 | - | - | 15.1 / 0.51 / 83592 | 198.3 / 6.65 / 6133 | 15.2 / 0.51 / 92548 | 128.6 / 4.31 / 10369 |

### Phase 3 - GET warm, Zipf(0.99)

#### view, T = 1

Cells: gets/s / GB/s / p99 us.

| block size | cyclone | cyclone-nomadv | cyclone-noverify | filedir | filedir-read | lmdb | rocksdb |
|---|---|---|---|---|---|---|---|
| 512 KiB | 1.94M / 1014.56 / 1 | - | - | 41.0k / 21.49 / 35 | 36.9k / 19.34 / 39 | 1.26M / 661.48 / 1 | 17.6k / 9.25 / 79 |
| 2 MiB | 657.2k / 1378.18 / 3 | 603.4k / 1265.43 / 4 | 672.4k / 1410.10 / 3 | 1.8k / 3.79 / 7074 | 7.2k / 15.15 / 529 | 4.0k / 8.32 / 6368 | 4.3k / 9.02 / 668 |
| 8 MiB | 158.7k / 1331.13 / 11 | - | - | 498.2 / 4.18 / 20754 | 1.8k / 15.03 / 1473 | 898.1 / 7.53 / 21801 | 890.4 / 7.47 / 2148 |
| 32 MiB | 37.4k / 1254.98 / 42 | - | - | 138.7 / 4.65 / 73448 | 388.6 / 13.04 / 5564 | 148.1 / 4.97 / 94915 | 206.8 / 6.94 / 10120 |

#### copy, T = 1

Cells: gets/s / GB/s / p99 us.

| block size | cyclone | cyclone-nomadv | cyclone-noverify | filedir | filedir-read | lmdb | rocksdb |
|---|---|---|---|---|---|---|---|
| 512 KiB | 114.7k / 60.11 / 11 | - | - | 25.9k / 13.57 / 52 | 29.5k / 15.45 / 49 | 122.2k / 64.08 / 12 | 15.5k / 8.13 / 87 |
| 2 MiB | 30.4k / 63.69 / 94 | 31.9k / 66.88 / 45 | 33.6k / 70.55 / 37 | 1.5k / 3.23 / 6418 | 6.2k / 12.97 / 557 | 4.2k / 8.79 / 5638 | 3.5k / 7.41 / 725 |
| 8 MiB | 8.7k / 73.23 / 130 | - | - | 518.6 / 4.35 / 21081 | 1.5k / 12.95 / 1596 | 681.5 / 5.72 / 23282 | 765.2 / 6.42 / 2340 |
| 32 MiB | 1.7k / 57.96 / 647 | - | - | 109.2 / 3.66 / 76350 | 318.6 / 10.69 / 6737 | 137.9 / 4.63 / 93047 | 183.3 / 6.15 / 10640 |

#### view, T = 4

Cells: gets/s / GB/s / p99 us.

| block size | cyclone | cyclone-nomadv | cyclone-noverify | filedir | filedir-read | lmdb | rocksdb |
|---|---|---|---|---|---|---|---|
| 512 KiB | 6.64M / 3483.84 / 1 | - | - | 95.7k / 50.20 / 82 | 90.0k / 47.18 / 74 | 3.22M / 1690.16 / 2 | 53.3k / 27.97 / 130 |
| 2 MiB | 1.68M / 3525.16 / 5 | 1.52M / 3197.42 / 5 | 1.73M / 3636.05 / 4 | 7.6k / 16.03 / 7252 | 15.0k / 31.41 / 872 | 8.8k / 18.48 / 13251 | 8.5k / 17.78 / 866 |
| 8 MiB | 331.8k / 2783.25 / 21 | - | - | 1.7k / 14.46 / 26470 | 3.1k / 26.03 / 2685 | 1.9k / 15.93 / 49319 | 1.5k / 12.74 / 3472 |
| 32 MiB | 83.7k / 2809.93 / 80 | - | - | 473.5 / 15.89 / 94843 | 741.1 / 24.87 / 12552 | 312.4 / 10.48 / 195118 | 360.1 / 12.08 / 15458 |

#### copy, T = 4

Cells: gets/s / GB/s / p99 us.

| block size | cyclone | cyclone-nomadv | cyclone-noverify | filedir | filedir-read | lmdb | rocksdb |
|---|---|---|---|---|---|---|---|
| 512 KiB | 153.4k / 80.42 / 34 | - | - | 67.2k / 35.22 / 98 | 75.3k / 39.49 / 92 | 152.4k / 79.93 / 33 | 38.5k / 20.18 / 153 |
| 2 MiB | 37.4k / 78.52 / 128 | 37.9k / 79.52 / 144 | 37.3k / 78.31 / 128 | 7.1k / 14.83 / 7072 | 12.0k / 25.11 / 874 | 8.1k / 16.93 / 12928 | 6.1k / 12.86 / 1034 |
| 8 MiB | 8.5k / 71.22 / 553 | - | - | 1.8k / 15.27 / 24964 | 2.2k / 18.61 / 3214 | 1.9k / 15.83 / 48184 | 1.3k / 10.54 / 4246 |
| 32 MiB | 1.8k / 61.35 / 4540 | - | - | 373.7 / 12.54 / 94617 | 525.5 / 17.63 / 14333 | 239.5 / 8.04 / 199330 | 302.3 / 10.14 / 17484 |

#### view, T = 8

Cells: gets/s / GB/s / p99 us.

| block size | cyclone | cyclone-nomadv | cyclone-noverify | filedir | filedir-read | lmdb | rocksdb |
|---|---|---|---|---|---|---|---|
| 512 KiB | 7.66M / 4016.92 / 2 | - | - | 99.8k / 52.30 / 283 | 106.5k / 55.86 / 305 | 4.73M / 2481.01 / 3 | 66.9k / 35.08 / 386 |
| 2 MiB | 995.3k / 2087.39 / 8 | 2.11M / 4425.18 / 9 | 2.52M / 5289.80 / 7 | 15.2k / 31.82 / 7864 | 17.4k / 36.56 / 1175 | 16.9k / 35.45 / 15023 | 10.0k / 20.98 / 2010 |
| 8 MiB | 505.9k / 4243.67 / 33 | - | - | 3.5k / 29.32 / 29177 | 3.6k / 30.22 / 4508 | 3.3k / 27.82 / 61211 | 1.7k / 14.05 / 7773 |
| 32 MiB | 132.2k / 4434.79 / 117 | - | - | 820.8 / 27.54 / 113221 | 881.7 / 29.58 / 19415 | 575.1 / 19.30 / 230330 | 392.6 / 13.17 / 28328 |

#### copy, T = 8

Cells: gets/s / GB/s / p99 us.

| block size | cyclone | cyclone-nomadv | cyclone-noverify | filedir | filedir-read | lmdb | rocksdb |
|---|---|---|---|---|---|---|---|
| 512 KiB | 223.0k / 116.90 / 83 | - | - | 77.6k / 40.68 / 352 | 88.9k / 46.61 / 401 | 224.7k / 117.81 / 80 | 51.4k / 26.96 / 500 |
| 2 MiB | 53.9k / 113.10 / 290 | 40.6k / 85.11 / 586 | 51.1k / 107.25 / 300 | 11.6k / 24.42 / 8206 | 12.8k / 26.75 / 1539 | 14.4k / 30.11 / 14921 | 7.5k / 15.72 / 2291 |
| 8 MiB | 9.1k / 76.04 / 1562 | - | - | 2.7k / 22.79 / 30375 | 2.4k / 20.37 / 5640 | 2.7k / 22.65 / 61419 | 1.4k / 11.62 / 9474 |
| 32 MiB | 2.1k / 70.38 / 6881 | - | - | 577.0 / 19.36 / 121553 | 582.4 / 19.54 / 21885 | 413.8 / 13.89 / 245107 | 322.2 / 10.81 / 34510 |

### Phase 4 - RESTART (close, reopen, GET all N)

Cells: ops/s / GB/s / p99 us / hit fraction.

| block size | cyclone | cyclone-nomadv | cyclone-noverify | filedir | filedir-read | lmdb | rocksdb |
|---|---|---|---|---|---|---|---|
| 512 KiB | 1.1k / 0.56 / 996 / 1.000 | - | - | 35.9k / 18.80 / 36 / 1.000 | 27.6k / 14.46 / 250 / 1.000 | 60.5k / 31.70 / 21 / 1.000 | 14.6k / 7.67 / 302 / 1.000 |
| 2 MiB | 267.0 / 0.56 / 3937 / 1.000 | 254.8 / 0.53 / 9029 / 1.000 | 0.0 / 0.00 / 0 / 0.000 | 467.4 / 0.98 / 6641 / 1.000 | 3.1k / 6.49 / 552 / 1.000 | 647.0 / 1.36 / 11118 / 1.000 | 2.7k / 5.65 / 749 / 1.000 |
| 8 MiB | 67.1 / 0.56 / 15756 / 1.000 | - | - | 135.3 / 1.14 / 21992 / 1.000 | 980.9 / 8.23 / 1551 / 1.000 | 210.2 / 1.76 / 26120 / 1.000 | 675.6 / 5.67 / 2305 / 1.000 |
| 32 MiB | 16.8 / 0.56 / 60015 / 1.000 | - | - | 35.0 / 1.17 / 77642 / 1.000 | 248.1 / 8.32 / 6629 / 1.000 | 45.4 / 1.52 / 87748 / 1.000 | 149.8 / 5.03 / 9687 / 1.000 |

### Phase 5 - MULTI-PROCESS READ (P=4 processes, T=1)

Cells: ops/s / GB/s / hit fraction.

| block size | cyclone | cyclone-nomadv | cyclone-noverify | filedir | filedir-read | lmdb | rocksdb |
|---|---|---|---|---|---|---|---|
| 512 KiB | 5.18M / 2713.31 | - | - | 113.4k / 59.45 / 1.000 | 87.6k / 45.92 / 1.000 | 3.19M / 1670.06 / 1.000 | - |
| 2 MiB | 726.0k / 1522.60 | 564.3k / 1183.39 | - | 9.7k / 20.24 / 1.000 | 15.0k / 31.37 / 1.000 | 9.3k / 19.56 / 1.000 | - |
| 8 MiB | 143.2k / 1201.34 | - | - | 2.4k / 19.78 / 1.000 | 3.1k / 26.19 / 1.000 | 1.7k / 14.08 / 1.000 | - |
| 32 MiB | 35.5k / 1192.24 | - | - | 554.5 / 18.60 / 1.000 | 744.7 / 24.99 / 1.000 | 351.2 / 11.79 / 1.000 | - |

### Read scaling

#### Read scaling at 2 MiB, mode = view

Cells: gets/s / GB/s.

| threads | cyclone | cyclone-nomadv | cyclone-noverify | filedir | filedir-read | lmdb | rocksdb |
|---|---|---|---|---|---|---|---|
| 1 | 657.2k / 1378.18 | 603.4k / 1265.43 | 672.4k / 1410.10 | 1.8k / 3.79 | 7.2k / 15.15 | 4.0k / 8.32 | 4.3k / 9.02 |
| 4 | 1.68M / 3525.16 | 1.52M / 3197.42 | 1.73M / 3636.05 | 7.6k / 16.03 | 15.0k / 31.41 | 8.8k / 18.48 | 8.5k / 17.78 |
| 8 | 995.3k / 2087.39 | 2.11M / 4425.18 | 2.52M / 5289.80 | 15.2k / 31.82 | 17.4k / 36.56 | 16.9k / 35.45 | 10.0k / 20.98 |

#### Read scaling at 2 MiB, mode = copy

Cells: gets/s / GB/s.

| threads | cyclone | cyclone-nomadv | cyclone-noverify | filedir | filedir-read | lmdb | rocksdb |
|---|---|---|---|---|---|---|---|
| 1 | 30.4k / 63.69 | 31.9k / 66.88 | 33.6k / 70.55 | 1.5k / 3.23 | 6.2k / 12.97 | 4.2k / 8.79 | 3.5k / 7.41 |
| 4 | 37.4k / 78.52 | 37.9k / 79.52 | 37.3k / 78.31 | 7.1k / 14.83 | 12.0k / 25.11 | 8.1k / 16.93 | 6.1k / 12.86 |
| 8 | 53.9k / 113.10 | 40.6k / 85.11 | 51.1k / 107.25 | 11.6k / 24.42 | 12.8k / 26.75 | 14.4k / 30.11 | 7.5k / 15.72 |
