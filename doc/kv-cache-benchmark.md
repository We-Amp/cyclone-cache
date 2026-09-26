# Cyclone as an LLM KV-cache storage tier — benchmark

## Summary

Seven rounds of measurements, plus two fix studies, taken 2026-09-21 to
2026-09-25 on two laptops. One is an Apple M5 running macOS, where only a
warm page cache can be measured. The other is an i7-8750H with a Samsung
970 PRO NVMe running Linux, where the page cache is dropped before each
cold phase. The peers are LMDB, RocksDB (BlobDB) and one file per block
(`filedir`). The Cyclone trees measured, in order:

- **Rounds 1 and 2:** the stock tree (macOS warm, then Linux cold). This is
  the baseline every later Cyclone number is compared against.
- **Round 3:** stock plus platform-aware readahead and a fast CRC32.
- **Round 3b:** round 3 with the CRC-32C checksum (on-disk format v8). This
  is the current read path.
- **Round 4:** the round-3b tree as a bounded-capacity tier under churn.
- **Round 5:** main at 2aed24c, which adds opt-in wrap retention. The
  cold sweep and the churn run were repeated, with churn in both retention
  modes against a same-day LMDB.
- **Readahead chunking (issue #18):** main at b94540d against the fix
  (dd487fb): the Linux readahead hint in 64 KiB chunks, and skipped on
  resident documents. The cold sweep ran against a same-day LMDB, and a
  shorter check repeated it after rebasing onto main with wrap retention
  on by default.
- **Insert path (issue #16):** main at 6e2077e against the fix, which
  writes the content from the handle's buffer instead of assembling a
  contiguous document and adds `WriteHandle::reserve()`. Put, churn at
  T=1 and T=4, and same-day file-per-block and LMDB at T=1.
- **Round 6:** main at b5c31e8, which includes both fixes and has wrap
  retention on by default. The round-5 matrix was repeated with three
  interleaved runs per point and same-day LMDB, file-per-block and RocksDB
  for every comparison. Churn was run in both retention modes and with
  `--reserve`, at one and four threads for every store. The round also
  traced the one-thread insert tail.

Each number is one machine's reading. Treat differences under about 20 % as
noise unless a section says otherwise.

Where Cyclone stands now (round 6, Linux, same-day peers):

- **Warm reads:** same class as LMDB. Both return a span into a mapping that
  already exists, with no syscall per get. At 2 MiB, `copy` reads 12.1
  against 12.5 GB/s and `view` 265 k against 242 k gets/s. This is not a
  differentiator.
- **Cold reads:** ahead of every peer at 8 and 32 MiB (3.00 and 3.39 GB/s;
  LMDB 2.76 and 2.76). Behind LMDB below that: 1.14× at 2 MiB (2.39 against
  2.73), 1.32× at 512 KiB (1.70 against 2.25), 1.5× at 1 MiB and 1.9× at
  256 KiB. Below the 256 KiB readahead threshold it is far behind (64 KiB:
  0.04 against 1.61 GB/s, issue #29). The checksum is verified in every
  case. **Since round 6** ([small cold reads](#small-cold-reads-issue-29)):
  cold-read and sequential readahead put Cyclone ahead of a same-day LMDB at
  every size from 64 KiB to 32 MiB (64 KiB 2.15–2.63 against 1.42 GB/s,
  512 KiB 2.93 against 1.70), with warm reads unchanged; LMDB read 12–27 %
  below its round-6 rates that day, and Cyclone is ahead of those too except
  at 2 MiB, where they tie.
- **Writes:** faster than round 5 at every size (×1.25–2.1). Against a
  same-day file-per-block, Cyclone is ahead at 512 KiB (1.62 against 1.38
  GB/s) and level at 2 MiB (1.47 against 1.52). It is behind at 8 MiB
  (1.27 against 1.43) and at 32 MiB (0.88 against 1.41); those were not
  profiled.
- **Bounded tier under churn:** with wrap retention on, the default,
  Cyclone meets the pre-registered "significantly better than LMDB" bar
  through the latency clause on both patterns at 4 threads. Its hit p99
  is 0.45× (`zipf`) and 0.38× (`zipf+scan`) of LMDB's, while it serves
  1.24× and 1.14× as much. The `zipf` margin is thin: 0.45× against the
  0.5× bar, and 0.52× in the worst pairing of runs. In round 5 it was
  0.27–0.37×; Cyclone's tail did not move, LMDB's got shorter. At one
  thread Cyclone now serves the most of the three stores: 1.22–1.28×
  LMDB and 1.22–1.26× file-per-block. Its hit ratio is still 3.6–4.1
  points below an LRU. Flush mode (`wrap_retention = false`) is partial:
  at 4 threads it serves 0.88–0.89× LMDB, just under the 0.9× floor.
- **The one-thread insert tail is explained:** the miss+insert p99 is
  24 ms, against 6 ms for file-per-block and 16 ms for LMDB. Documents
  sit at 8-byte-aligned offsets, so each one ends partway through a page
  whose other bytes are old data. When that page is not cached, ext4
  reads it synchronously inside the `pwrite`. About 1 % of inserts wait
  over 10–20 ms for that 4 KiB read behind the device's other I/O. The
  peers never do this read. Nothing was changed
  ([round 6](#the-one-thread-insert-tail-24-ms-p99)).
- **GPU transfer:** registering the whole volume mapping once reaches the
  PCIe ceiling (12.79 of 12.82 GB/s, 2.3× over staging). Pinning each
  returned span is slower than staging. LMDB also keeps every value in one
  mapping and reaches the same ceiling.

| Current number (2 MiB unless stated) | Cyclone | Peers (same day unless stated) | Round |
|---|---:|---|---|
| Warm GET copy, 1 thread, macOS | 63.6 GB/s | LMDB 64.9, filedir 18.0 (round 1) | [3](#round-3-readahead-and-a-fast-crc32) |
| Warm GET copy, 1 thread, Linux (DRAM-bound) | 12.1 GB/s | LMDB 12.5, filedir 6.9, RocksDB 2.9 | [6](#round-6-re-benchmark-at-main-b5c31e8) |
| Cold first-touch GET, Linux | 2.39 GB/s | LMDB 2.73, filedir 1.77, RocksDB 0.81 | [6](#round-6-re-benchmark-at-main-b5c31e8) |
| Cold restart GET, Linux | 2.08 GB/s | LMDB 2.71, filedir 1.77, RocksDB 0.83 | [6](#round-6-re-benchmark-at-main-b5c31e8) |
| Cold first-touch, 512 KiB / 8 MiB / 32 MiB, Linux | 1.70 / 3.00 / 3.39 GB/s | LMDB 2.25 / 2.76 / 2.76 | [6](#round-6-re-benchmark-at-main-b5c31e8) |
| Cold first-touch, 64 KiB / 256 KiB, Linux | 0.04 / 1.12 GB/s | LMDB 1.61 / 2.11 | [6](#cold-reads-from-64-kib-to-32-mib-issue-29-baseline) |
| Cold first-touch, 64 KiB / 256 KiB / 512 KiB, Linux, after #29 | 2.15 / 2.69 / 2.93 GB/s | LMDB 1.42 / 1.60 / 1.70 (same day) | [#29](#small-cold-reads-issue-29) |
| PUT, 1 thread, Linux, 512 KiB / 2 MiB / 32 MiB | 1.62 / 1.47 / 0.88 GB/s | filedir 1.38 / 1.52 / 1.41, RocksDB 0.52 / 0.44 / 0.41 | [6](#round-6-re-benchmark-at-main-b5c31e8) |
| 4 reader processes, `view`, Linux | 743 k gets/s | LMDB 613 k | [6](#round-6-re-benchmark-at-main-b5c31e8) |
| Churn, `zipf`, 4 threads, retention on: hit ratio / served / hit p99 | 0.814 / 2.40 GB/s / 29.9 ms | LMDB 0.850 / 1.94 / 67.0 ms; filedir 0.849 / 2.16 / 38.4 ms | [6](#churn-linux-2-mib-c--16-gib-4-gib-cgroup) |
| Churn, `zipf+scan`, 4 threads, retention on | 0.685 / 1.45 GB/s / 22.9 ms | LMDB 0.726 / 1.27 / 60.8 ms; filedir 0.725 / 1.58 / 37.5 ms | [6](#churn-linux-2-mib-c--16-gib-4-gib-cgroup) |
| Churn, `zipf`, 1 thread, retention on: served / miss+insert p50 / p99 | 1.78 GB/s / 1.49 / 24.4 ms | LMDB 1.46 / 1.66 / 15.6; filedir 1.41 / 1.46 / 6.4 | [6](#churn-linux-2-mib-c--16-gib-4-gib-cgroup) |
| Host→GPU, mapping registered once (CUDA, batch 16) | 12.79 GB/s | LMDB 12.81, staged 5.51 | [CUDA](#device-transfer-cuda-gtx-1050-pcie) |

All Linux rows are round-6 medians of three interleaved runs, with the
peers run the same day. The macOS row and the CUDA row were not re-run.
Churn rows are at 2 MiB, C = 16 GiB, in a 4 GiB cgroup; flush-mode and
`--reserve` rows are in round 6.

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

CacheLib (Meta's hybrid DRAM/NVMe cache) is the obvious missing peer; it is
not included in any round.

## Method

### Workload (v1)

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

### Churn workload (round 4)

Round 4 uses a separate spec,
[`kv-churn-spec.md`](kv-cache-benchmark/kv-churn-spec.md), whose decision
criteria were fixed before any run. The tier has a bounded capacity
C = 16 GiB of payload, and the key universe is 3 × C. Every operation is
get-or-insert: a hit copies the block into a staging buffer, and a miss
generates the block and puts it. Two access patterns run: Zipf(0.99), and
`zipf+scan`, where 10 % of operations are never-seen keys. Each store runs in
a 4 GiB memory cgroup and is measured for 120 s after a warm-up that inserts
2 × C. Stores, tuning and results are in
[Round 4](#round-4-bounded-capacity-under-churn).

### Store tuning (all printed by the harnesses)

- **Cyclone:** `max_object_size = 0`, `ram_cache_size = 0` (the CLFUS tier is
  for small objects; the OS page cache is the RAM tier here), mmap directory
  on (`set_multi_process(0, 1)`) so the index persists across the restart
  phase and can be shared by the reader processes, `enable_checksum = true`
  (mandatory in that mode, and it forces `verify_checksum_on_read`). Two
  variants isolate specific costs: `--no-mmap-dir --no-verify` (in-memory
  directory, no checksum on read) and, in round 1, a build without the
  blanket `MADV_RANDOM` on the whole-file mapping.
- **filedir / filedir-read:** `open(O_CREAT|O_TRUNC)` + `writev`, no fsync;
  reads `mmap`/`munmap` per get, or `pread` into a per-thread buffer.
- **LMDB:** 16 GiB map, default (durable) flags — it is the only store here
  that fsyncs every put; one read txn per get, zero-copy `MDB_val`. Round 4
  uses different flags (listed there).
- **RocksDB:** BlobDB (`min_blob_size = 0`), no compression, no block cache,
  WAL on, `sync = false`. `Get` copies into a `std::string`, so its `view`
  mode is a copy plus the page touch. Phase 5 skipped (one RW process per
  directory).

Store versions, as recorded in the raw JSON lines:

| Store | macOS (rounds 1, 3, 5; Metal) | Linux (rounds 2, 3, 3b, 4, 5, 6; CUDA) |
|---|---|---|
| LMDB | 1.0.2 | 0.9.24 |
| RocksDB | 11.8.1 | 9.10.0 (not in round 4) |
| filedir | plain files on APFS | plain files on ext4 |
| Cyclone | this repository, at the tree named by each round | same |

The Metal and CUDA result files do not record the LMDB version. They ran on
the same two machines.

### Machines

- **macOS:** Apple M5 (4 performance + 6 efficiency cores), 16 GiB RAM,
  macOS 27.0 (26A428), APFS on the internal SSD, Release/`-O2` builds. With
  16 GiB of RAM a 4 GiB dataset stays in the page cache once written, and
  macOS cannot drop the page cache without root. Every macOS phase therefore
  measures the software path over cached pages, not the SSD.
- **Linux:** Intel Core i7-8750H (6 cores / 12 threads), 23 GiB RAM, Samsung
  970 PRO NVMe, ext4, Ubuntu 22.04.5 (kernel 5.15.0-191), clang-20. It has
  root, so `sync; echo 3 > /proc/sys/vm/drop_caches` runs before the
  first-touch and restart phases (`--drop-caches-cmd` in both harnesses).
  All cold-read results come from this machine. It has dual-channel DDR4, so
  `copy` saturates at about 12 GB/s for every store. The CUDA section adds
  this laptop's GTX 1050.

### Reproducing

```bash
# Cyclone (this repository)
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCYCLONE_USE_BUNDLED_SHA256=ON && cmake --build build -j
./build/kv_bench --print-vectors                # must match the peer harness
./build/kv_bench --seconds 10 --path /fast/ssd --output results/cyclone.jsonl
./build/kv_bench --block-size 2097152 --no-mmap-dir --no-verify --output results/cyclone-noverify.jsonl
```

Round 4 (churn) runs one point per invocation, inside a 4 GiB memory
cgroup:

```bash
./build/kv_churn --print-vectors                # must match kvchurn's
doc/kv-cache-benchmark/churn/run-churn.sh cyclone zipf+scan 4 17179869184 120 churn.jsonl churn.txt
doc/kv-cache-benchmark/churn/run-churn.sh cyclone zipf 4 17179869184 120 churn.jsonl churn.txt --wrap-retention on
./build/kv_churn_policy                         # eviction-policy replay, no I/O
```

Round 6 ran the whole matrix through one driver, which waits for an idle
machine before every point and samples the cgroup beside it:

```bash
SRC=$PWD PEER_BUILD=/path/to/peer/build doc/kv-cache-benchmark/round6/driver.sh sweep 3
SRC=$PWD PEER_BUILD=/path/to/peer/build doc/kv-cache-benchmark/round6/driver.sh churn 3
python3 doc/kv-cache-benchmark/round6/summarize.py "$R6/out" churn   # R6: the driver's output root
```

The peer harness is not published. It contains the file-per-block, LMDB and
RocksDB adapters for both workloads (`kvchurn` is its churn driver), plus a
runner and a report generator. It is a plain implementation of
[`kv-workload-spec.md`](kv-cache-benchmark/kv-workload-spec.md) and
[`kv-churn-spec.md`](kv-cache-benchmark/kv-churn-spec.md), and those two
files are enough to reimplement it. A reimplementation must print the same
reference vectors as `kv_bench --print-vectors` and
`kv_churn --print-vectors`. `run-churn.sh` finds a peer build through
`PEER_BUILD`. Run each store's sizes with nothing else on the machine, and
delete each size's data before the next.

## Round 1: macOS, warm page cache

> **Cyclone rows superseded by rounds 3 and 3b; peer rows current.** Stock
> tree, 2026-09-21, Apple M5. This is the second run; the first was
> withdrawn (see [Corrections](#corrections-to-the-first-run)).

Full generated tables are in the appendix; raw JSON lines are in
[`doc/kv-cache-benchmark/`](kv-cache-benchmark/). `cyclone` is the stock
build with the tuning above; `cyclone-noverify` uses the in-memory directory
and no CRC on read; `cyclone-nomadv` is a build without the blanket
`MADV_RANDOM` (2 MiB only).

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
cause of per-page faulting on 2 MiB blocks, and round 3's readahead work
addressed that mechanism.

**Writes.** 0.40–0.48 GB/s per thread, a third of file-per-block and below
RocksDB. The write path copies the value three times (handle buffer →
document builder → serialized record) and CRCs it before a single
`pwrite`; there is no scatter-gather or reserve-in-place API yet. LMDB's
0.18 is not comparable — it fsyncs every put.

## Round 2: Linux, cold page cache

> **Cyclone rows superseded by rounds 3 and 3b; peer rows current.** Stock
> tree, 2026-09-21, i7-8750H / Samsung 970 PRO. The `cyclone` column is the
> cold-read baseline for rounds 3 and 3b.

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
run could only hint at (there the page cache was never cold) and it became
the first fix (round 3). It also explains why four Cyclone reader
processes fall below one thread on both platforms: each process re-faults
and re-verifies from scratch.

**Warm reads confirm the macOS picture on cheaper hardware.** Cyclone and
LMDB are the same zero-syscall class (274 k vs 245 k gets/s `view`; 11.9 vs
12.9 GB/s `copy`, DRAM-bound), file-per-block 6–8 GB/s, RocksDB 3.

**Writes** are 0.25–0.35 GB/s here vs 1.3–1.4 for file-per-block — the
same 4× gap as on macOS, from the same three copies + CRC.

## Round 3: readahead and a fast CRC32

Round 3 (2026-09-22) measures the first two fixes that rounds 1 and 2
called for. It shows each fix alone, then both together. The checksum in
this round is still CRC-32/ISO-HDLC at on-disk format v7; round 3b replaces
it.

### Readahead alone

The whole volume mapping is advised `MADV_RANDOM`, which is right for 4 KB
HTTP objects. For a cold 2 MiB read it meant about 512 serial NVMe faults.
The read path now adds a readahead hint over exactly the document's byte
range for documents of at least `CacheConfig::readahead_min_bytes` (default
256 KiB). The hint is issued after the full-key check and before the
checksum pass first touches the content. On Linux it is issued in 512 KiB
chunks. Each placement is re-advised at most every 2 s, so warm re-reads do
not pay for it. The mechanism and the reasons for each detail are in
[architecture.md, Memory-Mapped I/O](architecture.md#memory-mapped-io).
`CacheStats::readahead_hints_issued` counts the hints that reach the kernel.

Linux, 2 MiB blocks, 1 thread, `--seconds 5`, caches dropped before the
cold phases; median of three runs. The checksum is still the byte-wise
CRC32 of the stock tree:

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

The 0.120 GB/s baseline is from 5 s single-size runs, not the 10 s sweep
that gave round 2's 0.06 GB/s.

Four child processes re-advising cause 4× the hint traffic, so phase 5 is
the case that had to be checked. On Linux it is unchanged: the 2 s
re-advise filter caps the traffic, and `MADV_WILLNEED` returns without
blocking any other process.

**`MADV_WILLNEED` is not portable in cost — the macOS finding.** The first
version of the hint used `madvise(MADV_WILLNEED)` on every platform. That is
right on Linux and wrong on Darwin, where `MADV_WILLNEED` does not queue and
return. It walks and populates the range under the shared VM object's lock,
so each call is expensive *and* serialises across every process that maps
the volume. A standalone probe on the M5 used one 2 MiB range of a
`MAP_SHARED` read-write mapping whose pages were in the buffer cache but
not yet in the caller's page tables. `MADV_WILLNEED` in 512 KiB chunks took
**50 µs** with one process and **305 µs** with four concurrent ones.
`fcntl(F_RDADVISE)`, Darwin's native asynchronous readahead, took
**5 µs / 10 µs**. In phase 5 (four reader processes) the madvise hint cost
macOS **86 %** of `multiprocess_read view`, **85 %** of `copy`, and **29 %**
of `restart`. The read path therefore picks by platform:
`F_RDADVISE` on the volume fd on Darwin, `madvise(MADV_WILLNEED)` on Linux,
`PrefetchVirtualMemory` on Windows. With `F_RDADVISE`, every macOS phase is
back within noise of no hint at all.

macOS (M5, 16 GB, APFS/NVMe), same command, median of three runs. There is
no `drop_caches` equivalent, so the "cold" phases ran against a partly warm
page cache. They sat on the 0.55 GB/s ceiling of the byte-wise CRC32, not on
I/O, so this table measures what the hint *costs*:

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

The `--no-verify` row in the Linux table is the ceiling of the I/O fix:
**2.33 GB/s, past LMDB (2.15) and file-per-block (1.76)**. With verification
on, the cold path was then bounded by the 0.55 GB/s byte-wise CRC32.
Readahead took I/O out of the picture and left the remaining gap to the
checksum, which is the next fix. `MADV_POPULATE_READ` (Linux ≥ 5.14) was
measured on top of the chunked `WILLNEED` and did not help (2.25 vs
2.33 GB/s), so it is not used. `WILLNEED` already queues the large reads, and
populating the page-table entries up front only moves the per-page work.

### Fast CRC32 alone

The byte-wise table routine was replaced by slice-by-16 tables plus an ARMv8
`crc32b/w/x` path, selected once through a function pointer. The checksum
convention did not change (reflected `0xEDB88320`, init/xorout
`0xFFFFFFFF`), so format-v7 cache files kept verifying. Measured with
`crc32_bench`, 2 MiB buffer:

| | byte-wise (was) | slice-by-16 | ARMv8 crc32 |
|---|---:|---:|---:|
| Apple M5 (clang, Release) | 0.61 GB/s | 3.44 GB/s | **12.21 GB/s** |
| i7-8750H (clang-20, Release) | 0.50 GB/s | **2.83 GB/s** | n/a |

Both machines print the same checksum for the same buffer. The unit test
checked every path against a verbatim copy of the old byte-wise routine.
Round 3b later renamed these files (`crc32c.{hpp,cpp}`, `test_crc32c.cpp`,
`crc32c_bench`) when it changed the polynomial.

End to end on the M5 (`kv_bench --block-size 2097152 --seconds 5
--threads 1 --skip-multiprocess`, warm page cache): restart went from
**0.558 to 8.54 GB/s** (15×), first touch from 0.566 to 8.72 GB/s, and put
from 0.421 to 1.38 GB/s. `performance_baseline --content-size 4096`
first-touch reads went from **137 k to 1.12 M ops/s**. After this change
the restart floor on the M5 is the page-cache and copy rate, not the
checksum.

On x86-64 the ISO-HDLC polynomial has no hardware instruction (SSE4.2
`crc32` computes CRC-32C), so the i7 stayed on slice-by-16 at 2.8 GB/s.

### Both fixes together

`round 3` = the stock tree plus both fixes. Same harnesses, and the peers'
numbers are the round-1 and round-2 sweeps; raw data in
[`doc/kv-cache-benchmark/round3/`](kv-cache-benchmark/round3/). Full Linux
tables are in [Appendix C](#appendix-c--linux-round-3-generated-tables).

### Linux, page cache dropped before cold phases — 2 MiB blocks

| | stock | **round 3** | filedir | filedir-read | lmdb | rocksdb |
|---|---:|---:|---:|---:|---:|---:|
| PUT, GB/s | 0.34 | **0.81** | 1.44 | 1.39 | 0.15 ¹ | 0.61 |
| Cold first-touch GET, GB/s | 0.06 | **1.62** (27×) | 1.76 | 1.30 | 2.15 | 0.82 |
| Cold restart GET, GB/s | 0.06 | **1.44** (24×) | 1.73 | 1.31 | 2.16 | 0.82 |
| Cold first-touch, verification off | 0.18 | 2.31 | — | — | — | — |
| Warm GET copy, 1 thread, GB/s | 11.9 | 10.5 | 6.5 | 8.0 | 12.9 | 3.0 |
| Warm GET view, 1 thread, gets/s | 274 k | 265 k | 7.0 k | 5.3 k | 245 k | 1.8 k |
| 4 reader processes, view, gets/s | 217 k | **621 k** | 26 k | 6.5 k | 585 k | n/a |
| 4 reader processes, copy, GB/s | 9.3 | 9.8 | 11.8 | 7.8 | 10.9 | n/a |

¹ fsync per put. Other sizes: cold first-touch 0.63 / 1.63 / 1.28 GB/s and
restart 0.55 / 1.53 / 1.18 at 512 KiB / 8 MiB / 32 MiB (stock: 0.07–0.16).

### macOS, Apple M5 — 2 MiB blocks (60 s writeback settle before warm)

| | stock | **round 3** |
|---|---:|---:|
| PUT, GB/s | 0.42 | **1.28** |
| First-touch GET, GB/s | 0.56 | **5.44** |
| Restart GET, GB/s | 0.56 | **8.45** |
| Warm GET copy, 1 / 8 threads, GB/s | 70.9 / 107 | 63.6 / 114 |
| Warm GET view, 1 thread, gets/s | 668 k | 698 k |
| 4 reader processes, view / copy | 728 k gets/s / 46 GB/s | **1.67 M / 78 GB/s** |

### What round 3 says

- **Cold reads on Linux improved 27× on first touch and 24× on restart.**
  That put Cyclone level with file-per-block (1.6 vs 1.7 GB/s) and within
  25–35 % of LMDB. With verification off it read 2.31 GB/s, past every peer.
  The gap to LMDB was then the x86 slice-by-16 CRC32 at 2.8 GB/s: ARM had
  the 12 GB/s hardware path, and the next step on x86 was a PCLMULQDQ path
  or the CRC-32C format decision. (Since resolved: CRC-32C at format v8
  takes this to 2.17 GB/s; see
  [Round 3b](#round-3b-crc-32c-on-disk-format-v8).)
- **Multi-process readers now scale.** Four processes reached 621 k gets/s
  on Linux (2.9× stock, about LMDB's rate) and 1.67 M on macOS (2.3×). Each
  child's re-verification and re-faulting became cheap enough not to
  dominate its window. Copy-mode phase 5 is DRAM-bound on both machines, as
  for every store.
- **Writes** improved 2.4–3× from the checksum alone (0.81 GB/s on Linux,
  1.28 on macOS). They were still 1.7× behind file-per-block. The three-copy
  write path is the next item.
- **Warm reads** stayed within noise on macOS and in the 4 KB HTTP baseline.
  On Linux, warm `view` at 4 and 8 threads came out 11–17 % below stock
  (Appendix C: 647 k vs 755 k and 663 k vs 800 k gets/s at 2 MiB). This was
  not investigated. The Linux machine was in the degraded state described
  under [History](#history-and-corrections) during this round. Round 3b ran
  after a fresh boot and measured 807 k and 921 k.

The caveats specific to this round (a macOS writeback artifact, one 512 KiB
cell re-run, and the Linux machine's dirty-page state) are under
[History and corrections](#history-and-corrections).

## Round 3b: CRC-32C, on-disk format v8

> **Current Cyclone read path.** 2026-09-22, i7-8750H / Samsung 970 PRO;
> peer rows as in round 2. Raw data in
> [`kv-cache-benchmark/crc32c/`](kv-cache-benchmark/crc32c/); full tables in
> [Appendix D](#appendix-d--linux-round-3b-generated-tables).

The document checksum is CRC-32C from format major v8 (reflected
`0x82F63B78`, init/xorout `0xFFFFFFFF`). CRC-32C has a hardware path on
x86-64 (SSE4.2 `crc32q`) as well as on ARMv8 (`crc32cx`). Opening an older
cache file abandons it but does not delete it. The format major is part of
the fingerprinted file name, so consumers get a cold cache, never a
misparse, and the superseded v7 file stays on disk until something
reclaims it. Both hardware paths run three interleaved CRC registers
(8192-byte, then 256-byte blocks, recombined through GF(2) zero-shift
operators generated at compile time), because the instruction is limited by
latency, not throughput. Details are in
[architecture.md, Document Format](architecture.md#document-format).

| 2 MiB buffer | byte-wise | slice-by-16 | hw, 1 stream | hw, 3-way |
|---|---:|---:|---:|---:|
| Apple M5 (clang, Release) | 0.61 GB/s | 3.37 GB/s | 12.19 GB/s | **34.93 GB/s** |
| i7-8750H (clang-20, Release) | 0.50 GB/s | 2.82 GB/s | 10.41 GB/s | **26.51 GB/s** |
| i7-8750H (g++-13, Release) | — | 3.76 GB/s | 8.36 GB/s | 19.26 GB/s |

These are the best of five `crc32c_bench --seconds 1` runs on an otherwise
idle machine; the raw i7 output is in
[`kv-cache-benchmark/crc32c/i7-8750H-crc32c_bench.txt`](kv-cache-benchmark/crc32c/i7-8750H-crc32c_bench.txt).
GCC schedules the same intrinsics less well than clang; the project's
reference toolchain is clang-20.

**End to end on Linux**, page cache dropped before the cold phases. This is
round 3's tree with the change merged in (readahead + CRC-32C), the same
`kv_bench` invocation, and the same peer numbers as round 3. It ran after a
fresh boot at kernel-default dirty-page limits.

| 2 MiB blocks | round 3 | **+ CRC-32C** | filedir | lmdb |
|---|---:|---:|---:|---:|
| PUT, GB/s | 0.81 | **1.01** | 1.44 | 0.15 |
| Cold first-touch GET, GB/s | 1.62 | **2.17** | 1.76 | 2.15 |
| Cold restart GET, GB/s | 1.44 | **1.97** | 1.73 | 2.16 |
| Cold first-touch, verification off | 2.31 | 2.33 | — | — |
| Warm GET copy, 1 thread, GB/s | 10.5 | **13.1** | 6.5 | 12.9 |
| 4 reader processes, view, gets/s | 621 k | **755 k** | 26 k | 585 k |
| 4 reader processes, copy, GB/s | 9.8 | **12.3** | 11.8 | 10.9 |

Cold first-touch by block size, GB/s (restart in parentheses):

| | 512 KiB | 2 MiB | 8 MiB | 32 MiB |
|---|---:|---:|---:|---:|
| round 3 | 0.63 (0.55) | 1.62 (1.44) | 1.63 (1.53) | 1.28 (1.18) |
| **+ CRC-32C** | **1.19 (0.67)** | **2.17 (1.97)** | **3.03 (2.80)** | **3.40 (3.33)** |
| filedir | 0.94 | 1.76 | 2.51 | 2.60 |
| lmdb | 1.93 | 2.15 | 2.12 | 2.20 |

What it says: with verification on, the 2 MiB cold path now sits 7 % under
its own no-verify ceiling (2.17 vs 2.33 GB/s), where round 3 was 30 % under,
and it is level with LMDB. At 8 and 32 MiB, where readahead has the most to
queue, Cyclone reads cold faster than every peer. 512 KiB is still limited
by per-get overhead (1.2 GB/s against LMDB's 1.9). The open cold-path item
is verified state that outlives the process (see
[What to change](#what-to-change)). Puts gain 10–40 % because the write path
computes the checksum too. The warm and multi-process rows also moved, but
round 3 ran on a machine in a degraded state and this round on a fresh boot.
Treat gains outside the cold phases as partly machine state. The cold
phases drop the page cache first, and they are the comparison this round is
about. The i7's CRC-32C rate is 9.4× its old slice-by-16 ceiling, so on x86
the checksum is no longer on the critical path.

## Round 4: bounded capacity under churn

Rounds 1–3 measured stores that never fill. A real KV tier is bounded,
larger than RAM, and full: every miss inserts and something has to go. This
round measures that case against LMDB and file-per-block, with the decision
criteria fixed in the spec before any run.

**Workload** ([`kv-churn-spec.md`](kv-cache-benchmark/kv-churn-spec.md)):
get-or-insert (hit → memcpy of the block into a per-thread staging buffer;
miss → generate and put), capacity C = 16 GiB of payload, key universe 3 × C
(24 576 keys at 2 MiB), Zipf(0.99) over a fixed permutation, and a second
pattern `zipf+scan` where 10 % of operations are never-seen keys. Every store
runs in a memory cgroup with `memory.max = 4 GiB` (RAM : tier = 1 : 4;
verified: page cache is charged to the scope, and `memory.current` peaked at
4.00 GiB in every run). Per point: fresh store, caches dropped, warm-up until
2 × C has been inserted, `syncfs`, 120 s measured, `syncfs`, then close +
reopen and 10 000 get-only Zipf reads. One hit in 64 is checked byte for
byte; no run failed the check. Both harnesses print identical stream heads
(`kv_churn --print-vectors` / `kvchurn --print-vectors`).

**Stores and tuning** (printed by each run):

- **Cyclone** (`benchmarks/kv_churn`, round 3 + CRC-32C tree): volume sized so
  the stripes' data areas sum to 17 179 873 216 B (1.0000 × C), 16 stripes of
  ~511 blocks, 65 536 directory entries per stripe (11 MiB of mmap directory
  in total). Entries never ran out: tag-collision evictions stayed ≤ 7 per run
  at 2 MiB and ≤ 246 at 512 KiB (whole-run totals, warm-up included;
  `cy_tag_collision_evictions` in the recorded JSONL,
  `cy_tag_collision_evictions_total` in the current harness). mmap directory
  on, checksum verified on read, default readahead, no RAM tier, no fsync.
  Eviction is Cyclone's own; the harness keeps no index.
- **LMDB 0.9.24**: `MDB_NOSYNC | MDB_NOMETASYNC | MDB_NOTLS`, map 1.5 × C.
  `MDB_WRITEMAP` was measured in the 2 GiB smoke run and dropped (28 % less
  served at T=1, equal at T=4). An in-memory LRU (mutex, `std::list` + hash
  map) is updated on every hit and insert; an insert `mdb_del`s the LRU
  victims in the same write txn as its `mdb_put(MDB_RESERVE)`. No
  `MDB_MAP_FULL` in any run: the high-water mark stayed at 16.04–16.13 GiB
  with ≤ 14 freelist records, so 1.5 × C was enough and the 2 × C retry was
  not needed.
- **filedir**: one file per block, same LRU, `unlink()` to evict, temp file +
  `rename()` to insert, `preadv(header, staging buffer)` on a hit, no fsync.

Linux (i7-8750H, 970 PRO NVMe, ext4, kernel 5.15), one run per point, with the
2 MiB T=4 points run twice. Background load was present: the 1-minute load at
run start was 1.1–4.7 on 12 threads, recorded per run. Raw data:
[`doc/kv-cache-benchmark/churn/`](kv-cache-benchmark/churn/).

### Headline, 2 MiB blocks

Served = hits × block / s. Latencies are in µs; hit = get + copy, miss =
failed get + put (block generation excluded). Reopen = hit ratio of the
first 10 000 Zipf gets after close + reopen.

**`zipf`**

| T | store | hit ratio | served GB/s | hit p50 / p99 | miss+insert p50 / p99 | write amp | footprint | reopen |
|---:|---|---:|---:|---:|---:|---:|---:|---:|
| 1 | cyclone | 0.757 | 1.03 | 191 / 4 550 | 4 064 / 21 697 | 1.004 | 15.98 GiB | 0.721 |
| 1 | lmdb | 0.850 | 1.65 | 178 / 4 124 | 1 631 / 11 320 | 1.004 | 16.04 GiB | 0.847 |
| 1 | filedir | 0.851 | 1.72 | 215 / 14 019 | 1 429 / 6 254 | 1.008 | 16.03 GiB | 0.848 |
| 4 | cyclone | 0.757 | 1.41 / 1.40 | 360 / 23 812 | 7 457 / 74 378 | 1.003 | 15.98 GiB | 0.761 |
| 4 | lmdb | 0.849 | 1.61 / 1.59 | 387 / 83 660 | 5 639 / 44 421 | 1.004 | 16.05 GiB | 0.853 |
| 4 | filedir | 0.849 | 1.83 / 1.70 | 487 / 35 178 | 2 819 / 8 753 | 1.008 | 16.03 GiB | 0.853 |

**`zipf+scan`**

| T | store | hit ratio | served GB/s | hit p50 / p99 | miss+insert p50 / p99 | write amp | footprint | reopen |
|---:|---|---:|---:|---:|---:|---:|---:|---:|
| 1 | cyclone | 0.638 | 0.63 | 194 / 6 239 | 3 951 / 22 984 | 1.003 | 15.98 GiB | 0.691 |
| 1 | lmdb | 0.727 | 0.99 | 184 / 29 803 | 1 573 / 17 034 | 1.003 | 16.04 GiB | 0.804 |
| 1 | filedir | 0.727 | 1.08 | 234 / 19 830 | 1 437 / 6 253 | 1.007 | 16.03 GiB | 0.804 |
| 4 | cyclone | 0.642 | 1.00 / 0.89 | 354 / 18 662 | 6 774 / 69 121 | 1.003 | 15.98 GiB | 0.759 |
| 4 | lmdb | 0.726 | 0.98 / 0.95 | 342 / 73 066 | 8 497 / 68 670 | 1.003 | 16.06 GiB | 0.810 |
| 4 | filedir | 0.726 | 1.18 / 1.14 | 455 / 36 954 | 3 396 / 66 227 | 1.006 | 16.03 GiB | 0.806 |

T=4 served is "first run / repeat"; the latencies are from the first run (hit
p99 in the repeat, `zipf` / `zipf+scan`: Cyclone 23.7 / 21.4 ms, LMDB 82.2 /
82.9 ms, filedir 38.0 / 38.4 ms). Peak cgroup memory was 4.00 GiB for every
store; peak RSS was 3.3–3.7 GiB for the two mmap stores (resident file pages)
and under 30 MiB for filedir.

### Verdict against the decision criteria

Ratios Cyclone / LMDB, 2 MiB, 4 GiB cgroup (T=4: first run / repeat):

| pattern | T | served | hit p99 | criterion met? |
|---|---:|---:|---:|---|
| `zipf` | 4 | 0.87× / 0.88× | 0.28× / 0.29× | **no**: p99 is below half, but served < 0.9× |
| `zipf+scan` | 4 | 1.02× / 0.94× | 0.26× / 0.26× | yes, second clause (p99 ≤ 0.5× at ≥ 0.9× served) |
| `zipf` | 1 | 0.62× | 1.10× | no |
| `zipf+scan` | 1 | 0.64× | 0.21× | no (served < 0.9×) |

**Verdict: partial. Cyclone is not "significantly better" than LMDB here.**
It meets the bar on one pattern (`zipf+scan`), at T=4 only. There it passes
only the latency clause, with served ratios (1.02× and 0.94×) just above the
0.9× floor. On plain Zipf it serves 12–13 % less than LMDB at T=4 and 38 %
less at T=1. File-per-block with an LRU serves more than both at every 2 MiB
point.

### Why: the hit ratio, and where it comes from

Cyclone's hit ratio is 8–9 points below the LRU stores on both patterns
(9.2–9.4 on `zipf`, 8.4–8.9 on `zipf+scan`). That is a real cost of Cyclone's
eviction, and it is the main reason for the served-throughput gap at T=1. The
per-hit cost is the same (hit p50 191 vs 178 µs); Cyclone simply has fewer
hits and more slow misses. The gap is larger than "FIFO vs LRU". On a wrap
in the default flush mode, Cyclone flips the stripe's directory phase
(`Volume::publish_wrap_phase`), and every entry of the previous pass stops
resolving at once, although most of those blocks are still intact on disk
ahead of the write cursor. Each stripe
therefore restarts empty on every wrap and holds roughly half its capacity on
average. Replaying the same streams through the policies alone
(`benchmarks/kv_churn_policy`, no I/O, keys routed to stripes by
`segment_hash()` as `Volume::select_stripe` does;
[`policy-replay.txt`](kv-cache-benchmark/churn/policy-replay.txt)) reproduces
the measured numbers to within 0.6 points:

| 2 MiB, C = 16 GiB | LRU | FIFO | FIFO per stripe | wrap flush (Cyclone) | measured Cyclone |
|---|---:|---:|---:|---:|---:|
| `zipf` | 0.850 | 0.818 | 0.817 | 0.761 | 0.757 |
| `zipf+scan` | 0.725 | 0.687 | 0.687 | 0.644 | 0.638–0.642 |

Plain FIFO would cost 3.2 points against LRU on `zipf` and 3.8 on
`zipf+scan`; the phase flush costs 5.7 and 4.3 more (FIFO per stripe vs wrap
flush). So FIFO explains about 3–4 of the 8–9 points and the flush the
rest. No store here has scan resistance: the scan stream costs every store
about 12 points. Keeping the previous pass resolvable until it is
actually overwritten would recover the FIFO number. This round does not try
that; the design for it, with its own replay numbers, is
[`design/wrap-retention.md`](design/wrap-retention.md).

That design has since been implemented as `CacheConfig::wrap_retention`, off
by default at first and the default since (see the
[CHANGELOG](../CHANGELOG.md)). A later Linux run used a
smaller tier than the table above: 2 MiB blocks, C = 4 GiB, T=4,
`memory.max = 1 GiB` (the same 1 : 4 ratio). There, turning retention on moved the
measured hit ratio from 0.726 to 0.788 on `zipf` and from 0.614 to 0.660 on
`zipf+scan`, each within 0.002 of its policy replay (flush 0.726 / 0.612,
retention 0.788 / 0.659). The round-4 numbers above are all flush mode;
[Round 5](#round-5-re-benchmark-at-main-2aed24c) measures both modes at the
round-4 size.

Cyclone is better at the tail under concurrency. At T=4 its hit p99 is
3.5–4× lower than LMDB's (19–24 ms vs 73–84 ms) and lower than filedir's
(35–38 ms). Two effects are measured but not separated. First, a lower hit
ratio means Cyclone's hits skew to hotter, more often cached blocks, which
flatters its hit tail. Second, LMDB has a single writer: inserts serialise on
one write txn (miss+insert p50 5.6–8.5 ms at T=4), and that is where LMDB's
throughput stops scaling, while Cyclone's writers run per stripe. The cause
of LMDB's 80 ms hit tail was not profiled. Cyclone's own inserts are slow:
miss+insert p50 is 4.0 ms at T=1 against 1.4–1.6 ms for the peers. That is
consistent with the three-copy write path already on the list in
[What to change](#what-to-change), but it was not profiled in this round.

### 512 KiB blocks (supplementary; not part of the criteria)

| pattern, T | cyclone: hit / served / hit p99 | lmdb | filedir |
|---|---|---|---|
| `zipf`, 1 | 0.784 / 1.05 GB/s / 0.8 ms | 0.867 / 1.23 / 1.8 ms | 0.867 / 1.44 / 4.8 ms |
| `zipf`, 4 | 0.782 / **1.68** / **3.5 ms** | 0.866 / 1.45 / 21.2 ms | 0.866 / 1.67 / 17.1 ms |
| `zipf+scan`, 1 | 0.659 / 0.67 / 0.8 ms | 0.739 / 0.88 / 1.9 ms | 0.740 / 0.98 / 7.0 ms |
| `zipf+scan`, 4 | 0.667 / **1.12** / **2.7 ms** | 0.743 / 0.87 / 13.1 ms | 0.743 / 1.14 / 18.9 ms |

At 512 KiB and T=4, Cyclone would pass on both patterns (1.16× and 1.30×
LMDB's served GB/s, hit p99 0.17× and 0.21×), and it ties file-per-block on
throughput. At T=1 it is still behind on served GB/s. Smaller blocks cut the
per-insert cost that dominates the 2 MiB case.

### What each store needed

- **LMDB** needed an eviction layer in the application, which the harness
  had to write: an LRU list + hash map behind a mutex taken on every hit, and
  victim deletes inside the write txn. That index is volatile (1.2 MiB at
  2 MiB blocks, 4.8 MiB at 512 KiB). It is lost on restart and rebuilt by a
  cursor scan (1–7 ms here), and the recency order is lost with it. LMDB also
  needed a map size chosen up front, and `MDB_NOSYNC`: without it every
  insert is an fsync (rounds 1 and 2 ran LMDB that way: 0.15–0.18 GB/s
  for 2 MiB puts, the slowest writer in both). It is single-writer by
  design.
- **filedir** needed the same LRU, a temp-file + rename protocol, and unlinks
  under the LRU lock to stay correct against concurrent re-inserts. Its index
  is rebuilt from `readdir` (11–44 ms).
- **Cyclone** needed nothing on top. Capacity is the volume size, eviction
  and its state are persistent (the reopen hit ratio is about the
  steady-state hit ratio, with no rebuild step), and writers run per stripe.
  In exchange it gives up hit ratio (above) and insert latency. It also
  dropped a few inserts at T=4 (`writes_dropped_by_lease`: 0–3 per run) when
  a wrap met a live reader lease; the peers dropped none.

## Round 5: re-benchmark at main (2aed24c)

> 2026-09-24, same two machines. main at 2aed24c: readahead, CRC-32C
> (format v8), the multi-process wrap-cursor fix, and opt-in wrap retention
> (off by default). Raw data, logs and per-run load are in
> [`kv-cache-benchmark/round5/`](kv-cache-benchmark/round5/).

This round asks two questions. Did main move the round-3b cold numbers? And
what does wrap retention do to the round-4 churn verdict? Everything ran
from one clang-20 Release build (bundled SHA-256), one benchmark at a time,
at kernel-default dirty-page limits (`vm.dirty_ratio = 20`,
`dirty_background_ratio = 10`), and each run's data was deleted after it.
The Linux box had a 1-minute load of 0.39 before the first run, with no
other busy process. Each later point's start-of-run load (1.1–4.8) is the
previous point's own threads decaying; every value is in `round5-progress.txt`
and the churn logs. The macOS machine was shared: a VM and other sessions
were running, the load was 1.8–4.3, and the disk was 97 % full.

**Same-day controls.** Two cells moved by more than the ±10 % re-run
threshold. To tell code from machine, the round-3b/4 code (3823122: the
same tree minus wrap retention) was rebuilt and re-run the same day at
512 KiB and 2 MiB (`kv_bench`), and at 2 MiB T=4 in flush mode on both
churn patterns (`kv_churn`). Its output is in the `*-control-3823122*`
files.

### Cold sweep vs round 3b (Linux, page cache dropped)

Same `kv_bench` arguments as round 3b (`--seconds 10`,
`--drop-caches-cmd`, all four block sizes), then the 2 MiB
`--no-mmap-dir --no-verify` run. GB/s; restart in parentheses. "Re-run" is
the single confirmation run for cells that moved by more than 10 %.

| | round 3b | **round 5** | re-run | control (3823122, same day) |
|---|---:|---:|---:|---:|
| Cold first-touch, 512 KiB | 1.19 (0.67) | **0.80 (0.70)** | 0.76 (0.66) | 0.86 (0.77) |
| Cold first-touch, 2 MiB | 2.17 (1.97) | **2.16 (1.90)** | 2.12 (1.91) | 2.16 (1.97) |
| Cold first-touch, 8 MiB | 3.03 (2.80) | **3.04 (2.85)** | — | — |
| Cold first-touch, 32 MiB | 3.40 (3.33) | **3.40 (3.29)** | — | — |
| Cold first-touch, 2 MiB, verification off | 2.33 | **2.23** | 2.37 | — |
| PUT, 2 MiB | 1.01 | **1.11** | 0.59 | 1.01 |
| PUT, 2 MiB, verification off | 0.48 | **0.58** | 0.57 | — |
| Warm GET copy, 2 MiB, 1 thread | 13.0 | **13.0** | 11.9 | 11.7 |
| 4 reader processes, 2 MiB, view (gets/s) | 755 k | **744 k** | 741 k | 741 k |
| 4 reader processes, 2 MiB, copy | 12.3 | **10.5** | 11.5 | 12.1 |

Every cell was compared with
[`crc32c/linux-cyclone*.jsonl`](kv-cache-benchmark/crc32c/). The cells that
moved by more than 10 %, and their re-runs:

- **512 KiB first touch, −33 %: confirmed (0.76), but this is the machine,
  not the code.** The same-day control at 3823122, which has no wrap
  retention, reads 0.86. main is 7–11 % under it, which is inside this
  round's run-to-run spread. The readahead hints fired in every run: 4 096
  per phase, as in round 3b. The round-3b 1.19 was not reproduced by either
  tree today.
- **512 KiB warm `view`, 1 and 4 threads, +16 % / +20 %: confirmed** (756 k
  and 2.73 M gets/s). The control reads the same (761 k, 2.75 M), so this
  is machine state too.
- **2 MiB 4-process `copy`, −14 %: dismissed.** The re-run read 11.5 GB/s
  (−6 %) and the control 12.1.
- **Verification-off, 2 MiB: warm `view` at 1 thread +26 %, confirmed**
  (268 k and 266 k against 212 k). It now matches the verified path
  (265 k), so the round-3b value looks like the outlier. **Put +19 %:
  confirmed** (0.58 and 0.57). **Warm `copy` at 1 thread −11 %: dismissed**
  (the re-run read −6 %).
- **2 MiB put** read 1.11 in the sweep (within 10 %, so no re-run was
  triggered) and 0.59 in the 2 MiB re-run, which had p50 2.9 ms against
  1.4 ms. The control read 1.01. This one reading is unexplained. It was not
  run a third time.

The `kv_bench --output` file is truncated on every invocation, so
`linux-cyclone-rerun.jsonl` holds only the 2 MiB re-run. The 512 KiB re-run
values are in the table of `round5-linux-cyclone-rerun-log.txt`.

**Verdict on the cold path:** unchanged at 2, 8 and 32 MiB, with
verification on and off. Wrap retention is off by default and costs the read
path nothing measurable here. The 512 KiB cell moved with the machine.

### Churn vs round 4, both retention modes (Linux, 2 MiB, C = 16 GiB, 4 GiB cgroup)

This is the round-4 configuration through the same `run-churn.sh`
(`--block-size 2097152 --max-warmup-seconds 1800`). Cyclone ran with
`kv_churn --wrap-retention off|on`; the flag already existed. LMDB and
filedir were re-run at T=4 as a same-day anchor, with the same peer build and
invocation as round 4. Cyclone and LMDB were run a second time at T=4 ("first
/ repeat"). Latencies are µs, from the first run. The `kv_churn_policy`
replay prediction is given next to each hit ratio: `wrap-flush` for flush
mode, and `retain/64` for retention mode, because 16 stripes of about 1 GiB
give N = 64 frontier chunks.

**`zipf`**

| T | store | hit ratio (replay) | served GB/s | round 4 served | hit p50 / p99 | miss+insert p50 / p99 | reopen |
|---:|---|---:|---:|---:|---:|---:|---:|
| 1 | cyclone, flush | 0.758 (0.761) | 0.98 | 1.03 | 192 / 4 726 | 4 109 / 27 085 | 0.759 |
| 1 | **cyclone, retention** | **0.815 (0.815)** | **1.14** | — | 192 / 5 718 | 4 331 / 29 944 | 0.814 |
| 4 | cyclone, flush | 0.758 (0.761) | 1.87 / 1.96 | 1.41 / 1.40 | 369 / 17 656 | 6 654 / 56 709 | 0.769 |
| 4 | **cyclone, retention** | **0.814 (0.815)** | **1.73 / 2.13** | — | 381 / 31 292 | 7 387 / 74 204 | 0.823 |
| 4 | lmdb | 0.849 (LRU 0.850) | 1.51 / 1.44 | 1.61 / 1.59 | 379 / 85 730 | 5 685 / 51 051 | 0.853 |
| 4 | filedir | 0.850 (LRU 0.850) | 1.57 | 1.83 / 1.70 | 520 / 40 809 | 3 037 / 32 088 | 0.853 |

**`zipf+scan`**

| T | store | hit ratio (replay) | served GB/s | round 4 served | hit p50 / p99 | miss+insert p50 / p99 | reopen |
|---:|---|---:|---:|---:|---:|---:|---:|
| 1 | cyclone, flush | 0.638 (0.644) | 0.65 | 0.63 | 192 / 5 069 | 3 988 / 22 556 | 0.708 |
| 1 | **cyclone, retention** | **0.686 (0.685)** | **0.75** | — | 192 / 5 951 | 4 044 / 22 616 | 0.754 |
| 4 | cyclone, flush | 0.642 (0.644) | 1.01 / 0.83 | 1.00 / 0.89 | 352 / 17 302 | 6 735 / 70 579 | 0.766 |
| 4 | **cyclone, retention** | **0.686 (0.685)** | **1.01 / 1.22** | — | 356 / 29 049 | 7 338 / 76 945 | 0.750 |
| 4 | lmdb | 0.726 (LRU 0.725) | 1.00 / 1.04 | 0.98 / 0.95 | 332 / 77 871 | 8 890 / 63 838 | 0.811 |
| 4 | filedir | 0.726 (LRU 0.725) | 1.16 | 1.18 / 1.14 | 463 / 38 999 | 3 582 / 56 984 | 0.807 |

Repeat-run hit p99, in ms (`zipf` / `zipf+scan`): Cyclone flush 15.6 /
22.7, Cyclone retention 24.9 / 24.3, LMDB 91.6 / 75.6. No run failed the
content check. Peak cgroup memory was 4.00 GiB in every run; write
amplification was 1.003–1.008; footprints were as in round 4. Cyclone
dropped 0–6 inserts per run to a live lease. Tag-collision evictions were
≤ 9 per run.

What moved:

- **Retention recovers most of the hit-ratio gap, exactly as replayed.**
  `zipf` rises from 0.758 to 0.814–0.815 and `zipf+scan` from 0.638–0.642 to
  0.686. Every measured value is within 0.002 of its `retain/64` replay, and
  flush mode is within 0.006 of `wrap-flush`
  ([`policy-replay.txt`](kv-cache-benchmark/round5/policy-replay.txt)). The
  gap to LRU shrinks from 9.1 to 3.5 points on `zipf` and from 8.4–8.8 to
  4.0 on `zipf+scan`. What remains is FIFO against LRU (FIFO per stripe in
  the replay: 0.817 and 0.687).
- **Served GB/s, T=1: +16 % on both patterns** (0.98 → 1.14, 0.65 → 0.75).
  That is the extra hits, at an unchanged hit p50 of 192 µs. **T=4: within
  noise.** Run-to-run spread today was up to 32 % (flush `zipf+scan` 1.01
  vs 0.83; retention `zipf` 1.73 vs 2.13). The retention and flush ranges
  overlap on both patterns.
- **Hit p99 at T=4 rises with retention** (flush 16–23 ms, retention
  24–31 ms). Retention's extra hits are colder blocks from the previous lap,
  which are less often in the page cache. This is the other side of the
  round-4 observation that a lower hit ratio flatters the hit tail. Reopen
  hit ratio follows the steady-state ratio in both modes.
- **The T=4 served rise against round 4 is the machine, not main.** Flush
  mode at T=4 served 1.87 / 1.96 GB/s on `zipf`, against 1.41 / 1.40 in
  round 4, with the same hit ratio. The round-4 tree (3823122), re-run
  today, served 2.04 (`zipf`) and 1.25 (`zipf+scan`). T=4 served on this
  machine therefore moves by 25–45 % between days. The criteria are applied
  against a same-day LMDB for that reason. T=1 served matches round 4 within
  5 %.

### Verdict against the decision criteria (retention on)

The criteria are from [`kv-churn-spec.md`](kv-cache-benchmark/kv-churn-spec.md),
quoted verbatim: *"Cyclone counts as "significantly better than LMDB" for
this use only if, on Linux at 2 MiB with the 4 GiB cgroup, for BOTH patterns
at T=4: served GB/s ≥ 1.5× LMDB's, OR hit-get p99 ≤ 0.5× LMDB's at no worse
than 0.9× the served GB/s. A win at T=1 only, or on one pattern only, is
reported as "partial"."*

Ratios are Cyclone with retention over same-day LMDB, first run / repeat:

| pattern | T | served | hit p99 | criterion met? |
|---|---:|---:|---:|---|
| `zipf` | 4 | 1.15× / 1.48× | 0.37× / 0.27× | yes, second clause, in both runs |
| `zipf+scan` | 4 | 1.02× / 1.17× | 0.37× / 0.32× | yes, second clause, in both runs |

**Verdict: win, by the latency clause, with retention on.** Both patterns
at T=4 meet the bar in both runs: hit p99 at 0.27–0.37× LMDB's, while
serving 1.02–1.48× as much. The throughput clause (≥ 1.5×) is not met in
any run. Stated without normalising:

- The served margin that the clause rests on is thin on `zipf+scan`: 1.02×
  in the first run, against a 0.9× floor and a 32 % day-to-day spread.
- Cyclone's hit ratio is still 3.5–4.0 points below LMDB's LRU.
- The hit-p99 advantage has two causes, measured together but not
  separated. One is LMDB's single writer: its miss+insert p50 is 5.7–8.9 ms
  at T=4. The other is that Cyclone's lower hit ratio favours its tail.
- At T=1 Cyclone with retention serves 1.14 and 0.75 GB/s. Round 4's LMDB
  served 1.65 and 0.99 at T=1, so Cyclone is at about 0.7–0.8× there.
  LMDB T=1 was not re-run today.
- Flush mode (the default) would also pass on `zipf` today (1.24× / 1.36×
  served, p99 0.17–0.21×). On `zipf+scan` it passes in the first run
  (1.01×) and fails the 0.9× floor in the repeat (0.80×). So flush mode is
  still **partial**. Retention is the only mode that passed on both
  patterns in both runs. The round-4 "partial" in flush mode was a
  different, slower machine day; nothing here re-grades it.

### macOS, Apple M5 (shared machine, warm page cache)

`kv_bench --block-size 2097152 --pause-before-warm 60` (the round-3 recipe),
once. Load 1.8 before and 4.3 after; a VM and other sessions were running;
the disk was 97 % full.

| 2 MiB | round 3 | round 5 |
|---|---:|---:|
| PUT, GB/s | 1.28 | 0.70 |
| First-touch GET, GB/s | 5.44 | 4.79 |
| Restart GET, GB/s | 8.45 | 12.61 |
| Warm GET copy, 1 / 8 threads, GB/s | 63.6 / 114 | 68.4 / 109 |
| Warm GET view, 1 thread, gets/s | 698 k | 670 k |
| 4 reader processes, view / copy | 1.67 M / 77.7 GB/s | 1.64 M / 78.7 GB/s |

The warm and multi-process rows are unchanged. PUT, first touch and restart
depend on writeback and APFS allocation on a nearly full disk, with other
load present. They moved −46 %, −12 % and +49 %, were not re-run, and
should not be read as a code effect.

Churn smoke run: C = 4 GiB, 2 MiB, T=4, 120 s. There is no cgroup on macOS,
and with 16 GiB of RAM the whole tier stays in the page cache, so only the
hit ratio carries over to Linux. Load was 2.8–4.3.

| pattern | mode | hit ratio (replay) | served GB/s | reopen |
|---|---|---:|---:|---:|
| `zipf` | flush | 0.727 (0.726) | 6.17 | 0.722 |
| `zipf` | retention | 0.788 (0.788) | 7.04 | 0.791 |
| `zipf+scan` | flush | 0.612 (0.612) | 2.98 | 0.664 |
| `zipf+scan` | retention | 0.659 (0.659) | 3.84 | 0.737 |

These are the same hit ratios as the earlier Linux 4 GiB run (0.726 →
0.788 and 0.614 → 0.660), on a different OS and filesystem.

## Readahead chunking (issue #18)

> 2026-09-24/25, Linux machine only. main at b94540d against the fix
> (dd487fb): the Linux readahead hint goes out in 64 KiB chunks over the
> first 4 MiB of a document, and is skipped when every page is already
> resident. Same-day LMDB. Wrap retention was off by default in both
> trees; a shorter check after rebasing onto main e4c051e (retention on
> by default; the fix as f9cc804, its code unchanged) is at the end of this
> section. Raw data, per-run load, and every script and throwaway patch
> used are in
> [`kv-cache-benchmark/readahead/`](kv-cache-benchmark/readahead/).

Round 5 left cold 512 KiB reads at 0.80 GB/s against LMDB's 1.93 (round 2).
This section finds where the time went, fixes it, and re-measures every size
from 64 KiB to 32 MiB.

### Where the time went

Measured on main, cold 512 KiB `kv_bench` first-touch phase
([`readahead-diagnosis.txt`](kv-cache-benchmark/readahead/readahead-diagnosis.txt)):

- **Not page faults, not the checksum.** 4096 gets took about 180 major
  faults in total. The hint covers each document, and a document's first
  page is already cached as the last page of the previous document in the
  same stripe. The verified-state study had put the checksum at 2–9 % of a
  cold read.
- **Not the device's bandwidth.** The same volume file read with `O_DIRECT`,
  one 512 KiB read at a time, ran at 1.2–1.9 GB/s (280–430 µs per read).
- **The hint serialises everything.** Timing `read_sync` in pieces: 8.5 µs
  to find and key-check the document, **159 µs inside
  `madvise(MADV_WILLNEED)`**, then 410 µs in the checksum pass, almost all
  of it waiting for I/O. `perf` puts the madvise time in page-cache setup
  (`__add_to_page_cache_locked`, `clear_page_erms` — the kernel zeroes each
  new page — and `xas_load`). Linux allocates and inserts every page of an
  advised chunk before it submits the read, and all pages of one read unlock
  when the whole read completes. With 512 KiB chunks each document left the
  device idle for ~150 µs, then gave it one ~508 KiB request that the
  checksum pass waited out in full. Nothing overlapped.
- **Against LMDB,** from `/proc/diskstats` over the same phase: both stores
  keep about 1.4 reads in flight on average, but LMDB's 126 KiB reads (the
  kernel's own fault readahead) complete in 73 µs and Cyclone's in 424 µs.

### The fix

In `PosixMappedFile::advise_willneed`, the Linux hint:

1. **Small chunks.** The first 4 MiB of a document is advised in 64 KiB
   chunks, the rest in 512 KiB chunks as before. The first read reaches the
   device after 16 pages of setup instead of 128, several reads are in
   flight while the rest are set up, and the checksum pass never waits on
   one large read.
2. **No hint on a resident document.** On resident pages the hint queues no
   I/O but still walks every page, once per call, and the re-advise filter
   lets a warm document through after 2 s or on a slot collision. With 9
   calls per 512 KiB document instead of 2, that walk cost 7 % of warm
   `view` reads. The hint is now skipped when `mincore()` reports every page
   of the range resident (a 16-page window first, so a cold document
   answers after one short call). Checking one page instead was tried and
   rejected: about 3 % of cold documents had that page cached and the rest
   not, so their hint was skipped, and each took ~128 serial 4 KiB faults.
   Cold 512 KiB fell to 0.61 GB/s.

macOS (`F_RDADVISE`), Windows (`PrefetchVirtualMemory`) and the
`readahead_min_bytes` threshold (256 KiB) are unchanged.

### Choosing the chunks

Cold first touch, GB/s, median of three interleaved runs per rule
([`readahead-rule-sweep.txt`](kv-cache-benchmark/readahead/readahead-rule-sweep.txt);
the single-run chunk-size sweep before it is in
[`readahead-chunk-sweep.txt`](kv-cache-benchmark/readahead/readahead-chunk-sweep.txt)):

| chunk rule | 512 KiB | 1 MiB | 2 MiB | 8 MiB | 32 MiB |
|---|---:|---:|---:|---:|---:|
| 64 KiB throughout | 1.69 | 1.70 | **2.41** | 3.10 | 3.21 |
| 128 KiB throughout | 1.50 | **2.22** | 2.02 | 3.04 | 3.38 |
| 64 KiB over the first 1 MiB, then 512 KiB | 1.71 | 1.62 | 1.88 | 3.09 | 3.40 |
| 64 KiB over the first 256 KiB and the last 512 KiB, 512 KiB between | 1.70 | 1.42 | 1.91 | 3.10 | 3.39 |

- A 512 KiB chunk anywhere in a document of up to 2 MiB costs 10–20 %: the
  checksum pass catches up with the reads and stalls on the large one.
- 32 KiB and 16 KiB chunks were slower at 512 KiB (1.04 and 1.32 GB/s,
  single runs).
- At 32 MiB, 64 KiB chunks throughout cost 5 %. The device is the
  bottleneck there, and the chunks only add syscalls (8× as many).
- 1 MiB documents read fastest with 128 KiB chunks, in every run, while
  512 KiB and 2 MiB documents read slowest with them. This was not
  explained, and the chosen rule does not depend on it.

Hence 64 KiB chunks over the first 4 MiB, then 512 KiB. That behaves as
"64 KiB throughout" up to 2 MiB with margin, and as the old hint for the
bulk of a 32 MiB document.

### Before and after, cold (Linux, page cache dropped)

`kv_bench --seconds 1 --threads 1 --skip-multiprocess` over seven block
sizes in one invocation. main, the fix and LMDB ran interleaved, three runs
each. Median GB/s, first touch (restart in parentheses):

| block | main (b94540d) | fix (dd487fb) | fix / main | LMDB, same day | LMDB / fix |
|---|---:|---:|---:|---:|---:|
| 64 KiB | 0.04 (0.04) | 0.04 (0.04) | — | 1.56 (1.62) | 39× |
| 256 KiB | 0.65 (0.51) | 1.12 (0.87) | 1.72× (1.68×) | 2.05 (1.99) | 1.83× (2.30×) |
| **512 KiB** | 0.81 (0.71) | **1.70 (1.41)** | **2.09× (1.97×)** | 2.37 (2.32) | **1.39×** (1.65×) |
| 1 MiB | 1.44 (1.30) | 1.65 (1.63) | 1.14× (1.26×) | 2.71 (2.65) | 1.64× (1.63×) |
| 2 MiB | 2.13 (1.95) | 2.38 (2.09) | 1.12× (1.07×) | 2.77 (2.68) | 1.16× (1.28×) |
| 8 MiB | 3.07 (2.89) | 3.01 (2.83) | 0.98× (0.98×) | 2.82 (2.74) | 0.94× (0.97×) |
| 32 MiB | 3.37 (3.26) | 3.40 (3.28) | 1.01× (1.01×) | 2.94 (2.85) | 0.86× (0.87×) |

At 512 KiB, first touch is now 1.39× behind LMDB on the same day. The
issue's acceptance bar was 1.5×. Restart is 1.65× behind. 8 MiB is 2 %
slower in both phases, in every run (see below). LMDB's 512 KiB rate today
(2.37) is above round 2's 1.93, so compare these ratios only with each
other.

### Before and after, everything else

The full round-5 sweep (`kv_bench --seconds 10`: all phases, four sizes,
the 4-process phase), main and the fix interleaved, three runs each. Median
ratio, fix / main
([`readahead-final.txt`](kv-cache-benchmark/readahead/readahead-final.txt)):

| | 512 KiB | 2 MiB | 8 MiB | 32 MiB |
|---|---:|---:|---:|---:|
| Cold first touch | **1.93** | 1.08 | 0.95 | 1.01 |
| Cold restart | **1.81** | 1.07 | 0.98 | 0.97 |
| Warm `view`, 1 / 4 / 8 threads | 1.07 / 1.06 / 1.06 | 1.02 / 1.00 / 1.00 | 1.01 / 1.00 / 1.00 | 1.01 / 1.00 / 1.00 |
| Warm `copy`, 1 / 4 / 8 threads | 1.01 / 0.99 / 1.00 | 1.00 / 0.98 / 0.98 | 1.02 / 0.99 / 0.99 | 1.01 / 1.00 / 1.00 |
| 4 reader processes, `view` / `copy` | 1.10 / 1.00 | 1.02 / 0.98 | 1.00 / 0.99 | 1.01 / 1.00 |
| PUT | 1.00 | 1.03 | 0.95 | 0.99 |

- **Warm 512 KiB `view` is 6–10 % faster.** The residency check replaces
  the re-advise's page walk (two `madvise` calls on main) with a cheaper
  `mincore()`. A separate 512 KiB check with three interleaved runs agreed:
  warm `view` +7 % at 1 and 4 threads, 4-process `view` +6 %.
- **8 MiB cold reads are 2–5 % slower,** here and in the cold runs above.
  (One of the three first-touch runs here also hit a slow-device window at
  1.16 GB/s.) In the rule sweep, every small-chunk rule read the same at
  8 MiB, so the number of calls does not explain it. The difference is
  about the size of the machine's session-to-session spread, but it held in
  every interleaved pair. 8 MiB PUT ranged 0.90–1.01 against 0.99–1.00;
  the write path did not change.
- **4 KB objects are unchanged.** `performance_baseline --cache-size 512
  --entries 5000 --content-size 4096` reads below the threshold, where no
  code changed. On every operation the median of the fix's three runs is
  within 1 % of main's (single runs within 2.5 %). The first main run of
  each series ran straight after the `kv_bench` sweeps and was slow on
  every operation; it is excluded.

### After rebasing onto main (wrap retention on by default)

main e4c051e against the fix rebased onto it (f9cc804), 512 KiB and 2 MiB,
all phases, `--seconds 5 --threads 1,4`, two interleaved runs each
([`readahead-rebase-check.txt`](kv-cache-benchmark/readahead/readahead-rebase-check.txt)).
Median, fix / main:

| | 512 KiB | 2 MiB |
|---|---:|---:|
| Cold first touch / restart | 1.94 / 1.80 | 1.07 / 1.07 |
| Warm `view`, T = 1 / 4 | 1.09 / 1.08 | 1.02 / 1.00 |
| Warm `copy`, T = 1 / 4 | 1.00 / 1.00 | 0.94 / 0.95 |
| 4 reader processes, `view` / `copy` | 1.09 / 1.01 | 0.97 / 0.98 |
| PUT | 1.00 | 1.01 |

The same picture as before the rebase. The 2 MiB warm `copy` ratio comes
from one run of the fix (10.69 GB/s; its other run read 12.49, main 12.35
and 12.43); the three-run sweep above had it at 0.98–1.00.

### What is left of the gap

At 512 KiB and 1 MiB, Cyclone is still 1.4–1.65× behind LMDB. Two causes
were measured:

- **Order across documents.** `kv_bench` reads in insertion order. LMDB
  stores values in that order, so its reads are sequential on disk, and the
  kernel's fault readahead and the SSD's own prefetch run across value
  boundaries. Cyclone hashes keys over 16 stripes, so consecutive gets
  alternate between 16 regions of the file. Read with `O_DIRECT` from the
  same file, 16-way interleaved 128 KiB reads ran at 0.80 GB/s against
  1.61 sequential. A per-document hint cannot cover the next document.
- **Page setup before the checksum.** The hint loop (~100 µs per 512 KiB
  document) still runs before the checksum pass starts. LMDB pays the same
  setup inside its page faults, but the kernel's asynchronous readahead
  overlaps it with reads further ahead, into the next value. Interleaving the
  hint with the checksum pass could hide at most the checksum's own
  ~30–40 µs, so it was not done.

Below the 256 KiB threshold there is no hint at all: 64 KiB blocks read cold
at 0.04 GB/s, 16 serial faults per document, 39× behind LMDB. Lowering
the threshold to 64 KiB was measured but not changed
(`--readahead-min-bytes 65536`, one run each, in `thr-*` of
[`readahead-final.txt`](kv-cache-benchmark/readahead/readahead-final.txt)).
Cold 64 KiB rises from 0.04 to 0.24 GB/s and 128 KiB from 0.04 to 0.51.
Warm `view` falls 8–9 % at 64 KiB and 16–18 % at 128 KiB. A warm get there
takes under a microsecond, and every miss in the re-advise filter now adds
a syscall to it. Changing the default needs a cheaper warm path first. It is
left as a follow-up.

### Caveats

- One machine, one SSD (Samsung 970 PRO, `read_ahead_kb` 128,
  `max_sectors_kb` 1280, kernel 5.15). The chunk sizes are tuned there.
- In 4 of about 45 multi-size runs, every get of one block size in one run
  was 2.4–10× slower in one or both cold phases. The device itself was
  slow in those windows (per-request latency); where it was checked, no
  other job was running. The medians of three absorb them; the raw files
  keep them.
- The machine is shared with other background jobs. Every run waited for
  them to finish and for the 1-minute load to drop below 1.0. A job that
  starts mid-run is not prevented; the sampler records one, and none was
  recorded in the runs where it was on (from the second full sweep on).

## Insert path profile (issue #16)

> 2026-09-25, Linux machine, with a 4 KiB check on the M5. main at 6e2077e
> (wrap retention on by default) against this change. Before/after numbers
> are medians of three interleaved runs unless a table says otherwise, and
> every run waited for the 1-minute load to drop below 1.0 with no other
> job running. Raw data, the drivers, the timing patch and the perf
> reports are in
> [`kv-cache-benchmark/insert-path/`](kv-cache-benchmark/insert-path/).

Rounds 4 and 5 measured a miss+insert p50 of about 4 ms at one thread and
2 MiB, against 1.4–1.6 ms for file-per-block and LMDB, and a 2 MiB PUT
1.4× behind file-per-block. Both were put down to a three-copy write path;
neither was profiled. This section profiles it, removes the cost it finds,
and re-measures.

### Where the time went

The churn run at main reproduces the round-5 number: miss+insert p50
4.41 ms at T=1 (round 5: 4.33). Two tools split it. `insert_bench` (new;
one put timed as key / open / write / commit, with page faults per
insert from `getrusage`) runs the same store configuration as `kv_churn`
without a cgroup. A throwaway timing patch around each step of
`Volume::commit_write` and `DocumentBuilder::build`
([`insert-path-instr.patch`](kv-cache-benchmark/insert-path/insert-path-instr.patch))
ran under both. `perf record -g` on `insert_bench` gave the call graph.

Mean µs per 2 MiB insert at main:

| step | `insert_bench`, no cgroup | `kv_churn` T=1, 4 GiB cgroup |
|---|---:|---:|
| key (SHA-256) + open the handle | 0.8 | — |
| copy 1: caller buffer → handle buffer (`write_sync`) | 113 | not timed |
| copy 2: handle buffer → `DocumentBuilder::_content`, with its fresh 2 MiB heap buffer and that buffer's free | ≈ 850 | ≈ 1 040 |
| copy 3: header + content → one contiguous document, into another fresh 2 MiB buffer | 891 | 1 160 |
| CRC-32C over 2 MiB | 134 | 136 |
| stripe lock + `allocate_write_slot` (write lock, lease gate, wrap, retention frontier) | 2.0 | 3.3 |
| `pwrite` of the document | 588 | 1 533 |
| `commit_write_slot` (cursor publish, lock release) | 1.1 | 1.5 |
| directory probe + insert | 2.4 | 6.3 |
| free of the document buffer | 138 | 0.4 |
| **sum** | **≈ 2 720** | **≈ 3 880** + copy 1 + the failed get |

The `kv_churn` column averages 31 259 inserts, warm-up included; its
measured miss+insert p50 in that run was 4.40 ms.

- **Copies 2 and 3 are most of it, and it is the allocator, not memcpy.**
  Each put allocates two more 2 MiB buffers for them. glibc hands memory of that size
  back to the kernel on free, so every put faults 2 × 512 fresh zeroed
  pages back in: 992 minor faults per insert. perf puts 24 % of all
  samples in user-space `memmove` and another 25 % in the page-fault path
  under it (fault entry, `clear_page_erms`, memcg charging), plus 4.6 % in
  `brk` shrinking the heap. Copy 1 goes into a buffer the allocator does
  reuse, and costs 113 µs for the same 2 MiB. On the M5 the allocator
  keeps the buffers, and the same path takes 0.25 ms per put; the problem
  is Linux-specific.
- **`pwrite` is the kernel copy into the page cache** (ext4 delayed
  allocation, page-cache page allocation, dirtying). Inside the full
  4 GiB cgroup it takes 2.6× longer, 1.5 ms, which is consistent with the
  page-cache pages the write needs being reclaimed first; it was not
  profiled separately. File-per-block's whole insert, which writes the
  same 2 MiB through `writev` in the same cgroup, was 1.43 ms p50 in
  round 4.
- **Not a cause:** nothing syncs (`sync_on_write` is false; perf shows
  `fsync` at 0.01 %, from open and close). The wrap and lease gate and
  the retention frontier advance cost 2–3 µs, the directory insert 2–6 µs,
  key hashing 0.4 µs. HitTracker is off in the KV configuration. There
  are no page faults on the write region: writes go through `pwrite`,
  not the mapping. The first lap costs about 0.3 ms more than later laps
  (2.8 against 2.5 ms p50), which is ext4 allocating blocks for a file
  region written for the first time.

### The fix

1. **No contiguous document** (`Volume::commit_write`). The builder now
   produces only the head, the 132-byte header plus the caller's header
   bytes (`DocumentBuilder::build_head`), with the CRC-32C chained over
   header bytes and then content, so the payload is never contiguous in
   memory. The head and then the content, straight from the handle's
   buffer, go to the file with two `pwrite` calls. The bytes on disk are
   identical (a unit test compares `build_head(c) ++ c` with `build()`).
   So is the commit order: the whole fill, then `sync_on_write`'s fsync,
   then `commit_write_slot` and the directory insert (invariant 2).
   Objects up to 64 KiB are still copied behind the head and written with
   one `pwrite`, because below that the copy is cheaper than a second
   syscall. This removes copies 2 and 3, both allocations and all the
   faults: 992 faults per insert become 0.
2. **`WriteHandle::reserve(n)`** (new, additive). It returns `n` bytes of
   the handle's own buffer for the caller to fill, so a producer generates
   the block where the commit will write it from, and copy 1 goes too. The
   buffer grows without zero-filling. Contract: the span is valid until
   the next write, reserve, close or abort; the checksum is taken at
   close; an abort after a partial fill publishes nothing; the
   `write_sync` limits apply. `kv_churn --reserve` and
   `insert_bench --reserve` generate into it. It is not in the C API,
   which has no streaming write handle.

The destination is not the mapping itself: a slot is reserved only at
commit, under the write lock held across the fill (F6), so the content
cannot be written there before the commit. With both changes a 2 MiB put
is one CRC pass over the caller's bytes and the kernel's copy into the page
cache. The alternate write path (`commit_alternate_write`, which PageSpeed
uses) still builds the contiguous document and was not changed.

### Before and after

**`insert_bench`** (no cgroup, C = 4 GiB, one lap then two, p50 µs;
the first lap in parentheses):

| block | main | this change | + `reserve()` |
|---|---:|---:|---:|
| 2 MiB | 2 500 (2 800) | 490 (860) | 410 (770) |
| 512 KiB | 490 (580) | 120 (210) | 93 (190) |
| 2 MiB, GB/s over the steady laps | 0.71 | 2.30 | 2.40 |

**`kv_bench` PUT, 2 MiB** (single writer, fresh store): main 0.62 GB/s
(p50 2.82 ms), this change 1.42 GB/s (p50 0.87 ms), interleaved. A second
set later the same night, this change interleaved with file-per-block
(the peer harness, `--store filedir`): Cyclone 1.52 GB/s (p50 0.87 ms),
file-per-block 1.46 GB/s (p50 0.98 ms). The 1.4× PUT gap to
file-per-block is gone; Cyclone is 4 % ahead.

Main's 0.62 is the low reading round 5 could not explain (0.59 in its
2 MiB re-run, 1.11 in its full sweep). It is the allocator. main's
`kv_bench` run on 2 MiB alone reads 0.60–0.61 GB/s with 2.18 M minor
faults per process; run after a 512 KiB size, as in a full sweep, its
2 MiB PUT reads 0.99–1.08 GB/s, and the process takes 1.17 M faults over
both sizes. So the heap state that the earlier size leaves behind decides
whether the per-put 2 MiB buffers are faulted in afresh. Every earlier PUT
number in this file comes from a full sweep, the lucky state. This change
reads 1.46–1.55 GB/s at 2 MiB either way (two runs of each order; `/usr/bin/time -v`
fault counts in
[`insert-path-order.txt`](kv-cache-benchmark/insert-path/insert-path-order.txt)).

**Churn, `zipf`, 2 MiB, C = 16 GiB, 4 GiB cgroup** (`run-churn.sh`,
Cyclone with wrap retention on; latencies in µs). The peers ran after the
Cyclone runs the same night, two runs each, with the round-4 peer build
and flags:

| T | store | hit ratio | served GB/s | miss+insert p50 / p99 | hit p50 / p99 | inserted GB/s |
|---:|---|---:|---:|---:|---:|---:|
| 1 | Cyclone, main | 0.815 | 1.20 | 4 414 / 18 850 | 185 / 5 717 | 0.27 |
| 1 | Cyclone, this change | 0.815 | **1.76** | **1 486** / 24 408 | 196 / 7 216 | 0.40 |
| 1 | Cyclone, this change, `--reserve` | 0.815 | **1.80** | **1 374** / 24 625 | 192 / 7 104 | 0.41 |
| 1 | file-per-block | 0.850 | 1.71 / 1.73 | 1 433 / 6 275 | 219 / 14 742 | — |
| 1 | LMDB | 0.851 | 1.54 / 1.48 | 1 653 / 13 073 | 180 / 7 292 | — |
| 4 | Cyclone, main | 0.814 | 2.09 | 7 313 / 52 109 | 387 / 28 727 | 0.48 |
| 4 | Cyclone, this change | 0.814 | **2.52** | **4 850** / 49 896 | 396 / 29 064 | 0.57 |

Cyclone rows are medians of three; per run, served was
1.26 / 1.20 / 1.20 → 1.97 / 1.75 / 1.76 at T=1 and
2.09 / 2.12 / 1.87 → 2.42 / 2.52 / 2.55 at T=4. Peer rows give both
served values and the first run's latencies. No run failed the content
check; no Cyclone run dropped an insert to a lease or reported a read
error.

- **T=1: miss+insert p50 −66 % (4.41 → 1.49 ms), served +47 %.** The
  insert p50 is now 1.04× file-per-block's and 0.90× LMDB's (it was 3.1×
  file-per-block's; the issue's bar was 1.5×). Cyclone serves 1.02× what
  file-per-block serves and 1.14–1.19× LMDB, with a hit ratio 3.5 points
  lower. At one thread it no longer serves less than LMDB.
- **T=4: miss+insert p50 −34 %, served +21 %.** The hit tail is unchanged
  (29 ms).
- **The T=1 tails rose**: miss+insert p99 18.9 → 24.4 ms and hit p99
  5.7 → 7.2 ms. Cyclone now inserts 47 % more bytes per second into the
  same 4 GiB of page cache, so every operation has more writeback and
  reclaim to wait behind; the medians did not move up (hit p50 +6 %).
  The insert tail is Cyclone's weak point at one thread: p99 24 ms against
  6 ms for file-per-block and 13 ms for LMDB. It was not profiled here.

**No regression elsewhere:**

- `performance_baseline` 4 KiB writes (`--cache-size 512 --entries 5000
  --content-size 4096`, five interleaved runs each): Linux p50 2.95 →
  2.84 µs (320 k → 332 k ops/s); M5 p50 2.08 → 2.04 µs (462 k → 475 k
  ops/s). An earlier Linux pass ran each point right after a multi-GiB
  `insert_bench` run and read about 100 µs for both trees, because the
  4 KiB writes waited behind writeback of the previous run's data; it is
  kept in the raw data and not used. The head of an object of 64 KiB or
  less is now built with room for its content, so folding the content in
  no longer reallocates. A later five-round interleave of main, the tree
  before that change and after it read p50 2.96 / 2.83 / 2.84 µs (319 k /
  331 k / 335 k ops/s), within noise of each other on the change itself
  (`insert-path-ab5-*`). main's first round read 100 µs and is excluded
  from its median for the writeback reason above.
- Read path, `concurrent_read_bench 20000 512 2 0 512 ramoff` (two runs
  each; reads/s): 1 thread 1.89 M → 1.87 M, 4 threads 6.17 M → 6.13 M,
  16 threads 12.0 M → 11.5 M. The read code is untouched. Only the 16-thread
  point (on 12 hardware threads) moved by more than 1 %.

### What is left

perf over the final tree's `insert_bench` run (2 MiB, no cgroup): 71 % of
an insert's samples are the `pwrite` (the kernel copy, ext4 delayed
allocation, page-cache lookup and dirtying), 15 % the handle copy that
`reserve()` removes, and 13 % the CRC-32C. Under the 4 GiB cgroup the
`pwrite` alone measured 1.5 ms at main. That is about what a whole insert
now takes (1.49 ms p50), and about what file-per-block's buffered insert
takes (1.43 ms). What remains at the median is the page cache under
memory pressure, not Cyclone's code. The one-thread insert tail (p99
24 ms) is not explained by this profile and is left open.

To reproduce the split:
`./build/insert_bench --block-size 2097152 --capacity 4294967296 --laps 2`
(add `--reserve` for the `reserve()` path).

## Round 6: re-benchmark at main (b5c31e8)

> 2026-09-25, Linux machine only. main at b5c31e8: since round 5, wrap
> retention is on by default, the CRC-validation cache is keyed per
> document (#24), a reader waits out a descheduled writer instead of
> reporting a miss (#25), the Linux readahead hint goes out in 64 KiB
> chunks (#28), a put no longer builds a contiguous document and
> `WriteHandle::reserve()` exists (#31), and cross-process lock waits are
> bounded by holder liveness (#30). Raw data, per-run load, logs and every
> script are in [`kv-cache-benchmark/round6/`](kv-cache-benchmark/round6/).

This round repeats the round-5 matrix at main, three interleaved runs per
point, with same-day peers for every comparison. It adds four things:
`kv_churn --reserve` as a Cyclone row, one-thread peers in the churn
matrix, the 64 KiB and 256 KiB cold rows (the issue #29 baseline), and a
trace of the one-thread insert tail that the insert-path section left open.

**Method.** One clang-20 Release build (bundled SHA-256), kernel-default
dirty limits (`vm.dirty_ratio = 20`, `dirty_background_ratio = 10`,
recorded in `round6-machine.txt`), one benchmark at a time
([`driver.sh`](kv-cache-benchmark/round6/driver.sh)). Before every point
the driver waited until no GitHub Actions runner job was running and the
1-minute load was below 1.0. A 1 Hz sampler
([`sampler.sh`](kv-cache-benchmark/round6/sampler.sh)) ran beside every
point and recorded the cgroup's dirty, writeback and reclaim counters and
whether a runner job had started. Jobs did start mid-run: in 8 of the 60
churn points and in 4 of the 21 sweep points. Each affected churn point
was re-run as a fourth run. Churn medians leave out the runs a job
started into ([`summarize.py`](kv-cache-benchmark/round6/summarize.py) marks
them `*`). One point, Cyclone flush mode on `zipf+scan` at T=4, was hit
again in its re-run, so its median is over two runs.
Sweep medians keep all three runs; the affected ones are listed in
`round6-sweep-runner-jobs.txt`. The reference vectors of `kv_churn` and the
peer harness's `kvchurn` matched.

### Cold sweep, all phases (Linux, page cache dropped)

The round-5 command (`kv_bench --seconds 10 --drop-caches-cmd …`, four
block sizes, all phases), the 2 MiB `--no-mmap-dir --no-verify` run, and
the peer harness's full sweep for LMDB, file-per-block and RocksDB, with
each store's three runs interleaved. GB/s, median of three; restart in
parentheses. LMDB here is the durable default (one fsync per put), as in
rounds 1 and 2.

| | round 5 | **round 6** | LMDB | filedir | RocksDB |
|---|---:|---:|---:|---:|---:|
| Cold first touch, 512 KiB | 0.80 (0.70) | **1.70 (1.36)** | 2.25 (2.23) | 1.08 (1.04) | 0.84 (0.85) |
| Cold first touch, 2 MiB | 2.16 (1.90) | **2.39 (2.08)** | 2.73 (2.71) | 1.77 (1.77) | 0.81 (0.83) |
| Cold first touch, 8 MiB | 3.04 (2.85) | **3.00 (2.87)** | 2.76 (2.70) | 2.75 (2.72) | 0.85 (0.84) |
| Cold first touch, 32 MiB | 3.40 (3.29) | **3.39 (3.27)** | 2.76 (2.75) | 2.88 (2.90) | 0.58 (0.59) |
| Cold first touch, 2 MiB, verification off | 2.23 | **2.47** | — | — | — |
| PUT, 512 KiB | 0.76 | **1.62** | 0.06 | 1.38 | 0.52 |
| PUT, 2 MiB | 1.11 | **1.47** | 0.14 | 1.52 | 0.44 |
| PUT, 8 MiB | 1.01 | **1.27** | 0.27 | 1.43 | 0.37 |
| PUT, 32 MiB | 0.48 | **0.88** | 0.36 | 1.41 | 0.41 |
| PUT, 2 MiB, verification off | 0.58 | **1.34** | — | — | — |
| Warm GET `copy`, 2 MiB, 1 thread | 13.0 | **12.1** | 12.5 | 6.9 | 2.9 |
| Warm GET `view`, 2 MiB, 1 thread (gets/s) | 265 k | **265 k** | 242 k | 7.2 k | 1.8 k |
| Warm GET `view`, 512 KiB, 1 thread (gets/s) | 751 k | **776 k** | 678 k | 25 k | 9.7 k |
| 4 reader processes, 2 MiB, `view` (gets/s) | 744 k | **743 k** | 613 k | 25 k | — |
| 4 reader processes, 2 MiB, `copy` | 10.5 | **11.8** | 11.9 | 11.9 | — |

Every Cyclone cell was compared with round 5
([`round6-compare-round5.txt`](kv-cache-benchmark/round6/round6-compare-round5.txt)).
No cell fell by more than 10 %. The largest drops are 6 %: 2 MiB warm
`copy` at one thread, and 8 MiB warm `copy` at 4 and 8 threads. That copy
is DRAM-bound, and LMDB's copy today reads within 3 % of Cyclone's. The
cells that rose by more than 10 % are the ones the merged fixes
target: 512 KiB cold, ×2.1 first touch and ×1.9 restart (#28); 2 MiB cold,
+11 % (#28); PUT at every size, ×1.25 to ×2.3 (#31); and 4-process `copy`,
+12 %. Three single slow-device runs (0.14 GB/s at 512 KiB, one each for
Cyclone restart, filedir first touch and filedir restart) are inside
these medians, as in the readahead section.

### Cold reads from 64 KiB to 32 MiB (issue #29 baseline)

`kv_bench --seconds 1 --threads 1 --skip-multiprocess` over seven block
sizes, Cyclone and a same-day LMDB interleaved, three runs each (the
readahead section's command). Median GB/s, first touch (restart):

| block | Cyclone | LMDB | LMDB / Cyclone | readahead section: fix / LMDB |
|---|---:|---:|---:|---:|
| 64 KiB | 0.04 (0.04) | 1.61 (1.48) | 40× (37×) | 0.04 / 1.56 |
| 256 KiB | 1.12 (0.85) | 2.11 (1.97) | 1.88× (2.32×) | 1.12 / 2.05 |
| 512 KiB | 1.69 (1.40) | 2.19 (2.18) | 1.30× (1.56×) | 1.70 / 2.37 |
| 1 MiB | 1.77 (1.47) | 2.65 (2.64) | 1.50× (1.80×) | 1.65 / 2.71 |
| 2 MiB | 2.38 (2.07) | 2.65 (2.57) | 1.11× (1.24×) | 2.38 / 2.77 |
| 8 MiB | 2.96 (2.83) | 2.62 (2.59) | 0.89× (0.92×) | 3.01 / 2.82 |
| 32 MiB | 3.40 (3.30) | 2.75 (2.63) | 0.81× (0.80×) | 3.40 / 2.94 |

Cyclone reproduces the readahead section's numbers to within 0.12 GB/s at
every size. Below the 256 KiB readahead threshold nothing changed: 64 KiB
still reads at 0.04 GB/s, 40× behind LMDB. At 256 KiB, the smallest size
that gets the hint, it is 1.9× behind. One Cyclone run had a runner job
for most of it (`cyclone-cold-3`: 1 MiB read 1.21 there); the medians
absorb it.

### Churn (Linux, 2 MiB, C = 16 GiB, 4 GiB cgroup)

The round-5 configuration through `run-churn.sh`
(`--block-size 2097152 --max-warmup-seconds 1800`), 120 s measured, three
runs per point. Cyclone ran with retention on (the default), in flush mode
(`--wrap-retention off`), and with retention on plus `--reserve`, which
generates each missed block straight into `WriteHandle::reserve()`. LMDB
and file-per-block ran at both thread counts with the round-4 peer build
and flags. Medians; latencies in µs. The `kv_churn_policy` replay is
unchanged from round 5
([`policy-replay.txt`](kv-cache-benchmark/round6/policy-replay.txt)).

**`zipf`**

| T | store | hit ratio (replay) | served GB/s | round 5 | hit p50 / p99 | miss+insert p50 / p99 | reopen |
|---:|---|---:|---:|---:|---:|---:|---:|
| 1 | cyclone, flush | 0.759 (0.761) | 1.61 | 0.98 | 197 / 6 283 | 1 416 / 25 385 | 0.734 |
| 1 | **cyclone, retention** | **0.815 (0.815)** | **1.78** | 1.14 | 194 / 6 649 | 1 489 / 24 432 | 0.812 |
| 1 | cyclone, retention, `--reserve` | 0.815 | 1.78 | — | 192 / 7 288 | 1 381 / 25 195 | 0.812 |
| 1 | lmdb | 0.851 (LRU 0.850) | 1.46 | — | 182 / 9 143 | 1 660 / 15 648 | 0.846 |
| 1 | filedir | 0.851 (LRU 0.850) | 1.41 | — | 232 / 19 041 | 1 462 / 6 375 | 0.848 |
| 4 | cyclone, flush | 0.757 (0.761) | 1.73 | 1.87 / 1.96 | 347 / 19 607 | 5 755 / 77 126 | 0.756 |
| 4 | **cyclone, retention** | **0.814 (0.815)** | **2.40** | 1.73 / 2.13 | 398 / 29 916 | 4 968 / 54 280 | 0.812 |
| 4 | cyclone, retention, `--reserve` | 0.814 | 2.28 | — | 372 / 33 584 | 4 897 / 57 204 | 0.815 |
| 4 | lmdb | 0.850 (LRU 0.850) | 1.94 | 1.51 / 1.44 | 370 / 66 996 | 5 694 / 57 906 | 0.851 |
| 4 | filedir | 0.849 (LRU 0.850) | 2.16 | 1.57 | 480 / 38 447 | 2 399 / 8 787 | 0.850 |

**`zipf+scan`**

| T | store | hit ratio (replay) | served GB/s | round 5 | hit p50 / p99 | miss+insert p50 / p99 | reopen |
|---:|---|---:|---:|---:|---:|---:|---:|
| 1 | cyclone, flush | 0.643 (0.644) | 1.13 | 0.65 | 202 / 7 419 | 1 320 / 24 153 | 0.741 |
| 1 | **cyclone, retention** | **0.686 (0.685)** | **1.24** | 0.75 | 202 / 9 352 | 1 366 / 25 059 | 0.764 |
| 1 | cyclone, retention, `--reserve` | 0.686 | 1.17 | — | 200 / 11 202 | 1 279 / 27 235 | 0.762 |
| 1 | lmdb | 0.727 (LRU 0.725) | 0.97 | — | 184 / 29 470 | 1 580 / 18 589 | 0.806 |
| 1 | filedir | 0.727 (LRU 0.725) | 1.02 | — | 242 / 21 993 | 1 451 / 6 292 | 0.805 |
| 4 | cyclone, flush | 0.642 (0.644) | 1.12 | 1.01 / 0.83 | 300 / 15 543 | 5 863 / 78 826 | 0.696 |
| 4 | **cyclone, retention** | **0.685 (0.685)** | **1.45** | 1.01 / 1.22 | 332 / 22 949 | 5 529 / 64 083 | 0.756 |
| 4 | cyclone, retention, `--reserve` | 0.686 | 1.38 | — | 306 / 25 193 | 5 677 / 68 087 | 0.760 |
| 4 | lmdb | 0.726 (LRU 0.725) | 1.27 | 1.00 / 1.04 | 299 / 60 828 | 7 411 / 61 021 | 0.803 |
| 4 | filedir | 0.725 (LRU 0.725) | 1.58 | 1.16 | 405 / 37 467 | 2 091 / 76 761 | 0.804 |

The round-5 column is that round's served GB/s ("first / repeat" at T=4);
round 5 ran no peers at T=1. Per-run values are in
[`round6-churn-summary.txt`](kv-cache-benchmark/round6/round6-churn-summary.txt).
No run failed the content check or reported a read error. Peak cgroup
memory was 4.00 GiB in every run. Cyclone dropped 0–7 inserts per run to a
live lease, all at T=4, and evicted at most 23 entries per run on a tag
collision. Write amplification was 1.003–1.010 in runs with no runner job
(the device counter covers the whole partition, so runs a job wrote into
read up to 1.11).

What moved against round 5:

- **Hit ratios are unchanged**: retention 0.814–0.815 (`zipf`) and
  0.685–0.686 (`zipf+scan`), within 0.001 of the replay; flush
  0.757–0.759 and 0.642–0.643, within 0.004. The gap to the LRU peers is
  still 3.6 and 4.1 points with retention, and 9.2 and 8.4 points in
  flush mode.
- **One thread: Cyclone now serves the most.** With retention it serves
  1.78 and 1.24 GB/s: 1.22× and 1.28× LMDB, and 1.26× and 1.22×
  file-per-block. Round 5 had it at 0.7–0.8× of round 4's LMDB. The
  insert-path fix did this (miss+insert p50 4.3 → 1.49 ms). Hit p50 is
  unchanged at 194–202 µs.
- **Four threads: served is up 13–44 % on round 5** (2.40 against
  1.73 / 2.13 on `zipf`, 1.45 against 1.01 / 1.22 on `zipf+scan`), and the
  peers rose as well (LMDB 1.94 against 1.44–1.51). Cyclone's hit p99 did
  not move (29.9 ms against 24.9–31.3); LMDB's fell (67 against 86–92).
- **`--reserve` buys nothing measurable under churn.** Insert p50 is 6–7 %
  lower at T=1 (1 381 against 1 489 µs, 1 279 against 1 366). Served is
  the same at T=1 on `zipf` and 5–6 % lower in the other three cells
  (2.28 against 2.40, 1.38 against 1.45, 1.17 against 1.24), inside the
  run-to-run spread. Hit p99 is 10–20 % higher in all four cells. The copy
  it saves (about 0.1 ms per 2 MiB insert) is small next to the
  page-cache work of the insert.

### Verdict against the decision criteria

The criteria are from [`kv-churn-spec.md`](kv-cache-benchmark/kv-churn-spec.md),
quoted verbatim: *"Cyclone counts as "significantly better than LMDB" for
this use only if, on Linux at 2 MiB with the 4 GiB cgroup, for BOTH patterns
at T=4: served GB/s ≥ 1.5× LMDB's, OR hit-get p99 ≤ 0.5× LMDB's at no worse
than 0.9× the served GB/s. A win at T=1 only, or on one pattern only, is
reported as "partial"."*

Ratios are Cyclone's median over the same-day LMDB median. Clause A is
served ≥ 1.5×. Clause B is hit p99 ≤ 0.5× with served ≥ 0.9×. The range in
brackets pairs every Cyclone run with every clean LMDB run.

| mode | pattern | T | served | hit p99 | clause A | clause B |
|---|---|---:|---:|---:|---|---|
| retention (default) | `zipf` | 4 | 1.24× [1.12–1.70] | 0.45× [0.34–0.52] | no | **yes** |
| retention (default) | `zipf+scan` | 4 | 1.14× [1.11–1.19] | 0.38× [0.32–0.41] | no | **yes** |
| retention (default) | `zipf` | 1 | 1.22× | 0.73× | no | no |
| retention (default) | `zipf+scan` | 1 | 1.28× | 0.32× | no | yes |
| flush | `zipf` | 4 | 0.89× [0.82–1.46] | 0.29× | no | no (served < 0.9×) |
| flush | `zipf+scan` | 4 | 0.88× [0.85–0.93] | 0.26× | no | no (served < 0.9×) |
| flush | `zipf` | 1 | 1.10× | 0.69× | no | no |
| flush | `zipf+scan` | 1 | 1.16× | 0.25× | no | yes |
| retention + `--reserve` | `zipf` | 4 | 1.18× | 0.50× (0.501) | no | no (p99 > 0.5×) |
| retention + `--reserve` | `zipf+scan` | 4 | 1.09× | 0.41× | no | yes |

- **Retention, the default: WIN**, through clause B on both patterns at
  T=4 (hit p99 0.45× and 0.38× of LMDB's at 1.24× and 1.14× its served
  GB/s). The `zipf` margin is thin: 0.45× against the 0.5× bar, and the
  worst run pairing is 0.52×, which would fail. In round 5 the same cell
  was 0.27–0.37×. Cyclone's tail did not move; LMDB's got shorter. Clause
  A (≥ 1.5×) is not met on the median of either pattern.
- **Flush mode: PARTIAL.** At T=4 it fails the served floor on both
  patterns (0.89× and 0.88×, against 0.9×), although its hit p99 is the
  lowest of any store (0.26–0.29× LMDB's). It meets clause B only on
  `zipf+scan` at T=1. Round 5 had flush mode passing `zipf` at T=4 (1.24×
  / 1.36×). Cyclone's flush runs today served 2.21 / 1.63 / 1.73 GB/s
  (round 5: 1.87 / 1.96), and LMDB is faster today, so this is the
  day-to-day spread moving a cell that sits on the 0.9× line; no
  flush-mode cost was measured.
- **Retention with `--reserve`: PARTIAL.** `zipf` at T=4 misses clause B
  by 0.001 (hit p99 33.6 against 67.0 ms). This row is not the product
  default, but it is the path a producer that fills `reserve()` takes.

Hit ratios, as the spec requires, separately: Cyclone with retention is
3.6 points below LMDB's LRU on `zipf` and 4.1 on `zipf+scan`. That is
FIFO against LRU (the replay's FIFO-per-stripe gives 0.817 and 0.687),
and it is a real cost. Every served-GB/s ratio above already pays it. What
each store needed is unchanged from
[round 4](#what-each-store-needed): LMDB needs an application LRU, a
volatile index rebuilt on open, a map size chosen up front and
`MDB_NOSYNC`; file-per-block needs the same LRU and a rename protocol;
Cyclone needs nothing on top, and gives up hit ratio.

### The one-thread insert tail (24 ms p99)

At one thread Cyclone's miss+insert p99 is 24.4 ms, against 6.4 ms for
file-per-block and 15.6 ms for LMDB, while its p50 is level with both.
The insert-path section left this open. Two measurements explain it.

**The sampler rules out dirty throttling and reclaim stalls.** In every
one-thread `zipf` run the cgroup held 250–405 MiB of dirty page cache on
average, peaking at 463–596 MiB for all three stores, with
under 10 MiB under writeback
([`round6-samples-summary.txt`](kv-cache-benchmark/round6/round6-samples-summary.txt)).
Cyclone dirties more per second than the peers (300–460 MB/s against
210–250) because it inserts more; its dirty levels are no higher. No
`balance_dirty_pages` call in the trace below slept.

**A trace finds the stall.** [`diag.sh`](kv-cache-benchmark/round6/diag.sh)
re-ran the one-thread `zipf` point for each store and, 10 s into the
measured phase, recorded 60 s of `sched:sched_switch` with kernel call
chains for the benchmark's threads, plus `writeback:balance_dirty_pages`
and every device read. [`offcpu.py`](kv-cache-benchmark/round6/offcpu.py)
turns that into off-CPU intervals by blocking stack. Tracing costs the
Cyclone runs 4–7 % of their served GB/s; the traced runs are used for
this section only. Their summaries are the
`round6-diag/round6-offcpu-*.txt` files. A runner job started during the
first traced Cyclone (both modes) and file-per-block runs; they were
repeated (`-2`, used below). The file-per-block repeat saw a job start
too, but its trace window shows no I/O from it.

| 60 s traced, T=1 `zipf` | Cyclone, retention | Cyclone, flush | filedir | LMDB |
|---|---:|---:|---:|---:|
| inserts in the 120 s run | 19 595 | 25 011 | 15 995 | 13 814 |
| miss+insert p99 / p99.9 in that run, ms | 26.7 / 59.3 | 27.7 / 60.4 | 6.3 / 6.9 | 13.3 / 37.5 |
| synchronous 4 KiB reads inside `pwrite` (`ext4_block_write_begin`) | **6 417** | **10 269** | 0 | 0 |
| … waits over 10 ms / over 20 ms / longest | 126 / 71 / 56 ms | 225 / 101 / 48 ms | — | — |
| `balance_dirty_pages` calls that slept | 0 | 0 | 0 | 0 |
| waits on page writeback | 0 | 0 | 0 | 0 |
| longest stop in memcg reclaim inside a write (the page-cache charge) | ≤ 0.1 ms | ≤ 0.1 ms | ≤ 0.1 ms | ≤ 0.1 ms |

Every slow Cyclone insert blocks in the same place:
`pwrite → ext4_buffered_write_iter → ext4_da_write_begin →
ext4_block_write_begin → __wait_on_buffer`. The kernel is reading a 4 KiB
block from the device before it lets the write change part of it. Cyclone
places documents at 8-byte-aligned offsets. A 2 MiB document with its
~200-byte header therefore starts and ends in the middle of a page, and
shares its first and last pages with the neighbouring bytes. When a
shared page is not in the page cache, ext4 has to read it first, and it
does so synchronously inside the `pwrite`. The first page's neighbour is
the previous document in the same stripe, written moments earlier and
normally still cached. The last page's neighbour is data from the
previous lap, which usually is not. That gives at most one such read per
insert: 0.65 per insert measured with retention, 0.82 in flush mode.
(That it is the last page is inferred from this layout; the trace
records sectors, not document offsets.)

The read is 4 KiB, but it queues on the NVMe behind the thread's own
readahead (about 4 000 hint-issued reads per second of up to 64 KiB) and
behind writeback. About 1.3 % of inserts waited over 10 ms for it and
0.7 % over 20 ms, which is where the 24–28 ms p99 sits. Device-side
latency was not traced (no `block_rq_complete`), so the split between
queueing and service time is not measured.

The peers never pay it. File-per-block writes every block into a fresh
file, so there is no old data under a partial page. LMDB writes whole
pages. Both show 0 such reads. Their own tails come from the read side:
file-per-block's hit p99 (19 ms) is `preadv` waiting for reads, and
LMDB's is page faults on its map.

Why the p99 rose with the insert-path fix (18.9 → 24.4 ms): the same
partial pages are written before and after that change, so the number of
reads per insert should not have changed. What changed is that Cyclone
now inserts and serves about 47 % more bytes per second, which keeps more
reads and writeback queued at the device for the synchronous read to wait
behind. This is inferred; the pre-fix tree was not traced.

Not tried here, because this round changes no code: aligning document
starts to 4 KiB, which is an on-disk format change costing up to 4 KiB
of padding per document (0.2 % at 2 MiB); or reading the boundary page
ahead of the `pwrite`, for example with a hint when the slot is
allocated. Padding the final write out to the page end is not an option
with retention on: those bytes can belong to a previous-lap document
that is still readable.

### Regressions and losses since round 5

- **No kv_bench cell regressed.** Nothing fell more than 6 % against
  round 5, and every cell that moved more than 10 % moved up.
- **The latency margin on `zipf` at T=4 shrank from 0.27–0.37× to 0.45×**
  (worst run pairing 0.52×). This is LMDB's tail falling on the day,
  not Cyclone's rising, but the verdict on that pattern now rests on a
  thin margin.
- **Flush mode dropped from "passes `zipf` at T=4" to "partial"**,
  because its served ratio sits on the 0.9× floor (0.88–0.89× today).
- **Large-block puts are behind file-per-block:** 8 MiB at 1.27 against
  1.43 GB/s (0.89×), and 32 MiB at 0.88 against 1.41 (0.62×; p50 28 against
  18 ms). Both are faster than in round 5 (1.01 and 0.48), and 2 MiB is
  level (1.47 against 1.52), but the insert-path section measured only
  2 MiB. Not profiled. `kv_bench` does not use `reserve()`, so each put
  still copies the value into the handle buffer once.
- **`--reserve` raises the T=4 hit tail by 10–20 %** and misses the
  latency clause on `zipf` by 0.001. It is an opt-in path.
- **Unchanged losses:** a hit ratio 3.6–4.1 points below an LRU, cold
  reads 1.3–1.9× behind LMDB from 256 KiB to 1 MiB, and 40× behind at
  64 KiB.

## Small cold reads (issue #29)

> 2026-09-26, Linux machine, plus a warm-path check on macOS. main 775af03
> against the change. The cold, resident and 4 KB rows are its final code
> (built at b76d481). The warm rows and a second cold set are an earlier
> build (171a48f: before the write-cursor clamp, the resident-run probe and
> the unchecked hint, which only change what a CRC-pending read of a
> resident document costs), and the window sweep an earlier one still
> (6c060e9). Same-day LMDB. Raw data, per-run samples, logs and every script
> are in [`kv-cache-benchmark/small-reads/`](kv-cache-benchmark/small-reads/).

The readahead section and round 6 left two gaps. Below the 256 KiB
large-document threshold there was no readahead hint at all, and a cold
64 KiB read ran at 0.04 GB/s against LMDB's 1.61 (round 6). Above it,
`kv_bench` reads blocks back in insertion order, which is sequential on disk
for LMDB but hops across 16 stripes for Cyclone (512 KiB: 1.69 against 2.19).

### Why 64 KiB was 40× behind

A cold 64 KiB get took 1.71 ms at the median on main, against LMDB's 32 µs
([`small-reads-cold-summary.txt`](kv-cache-benchmark/small-reads/small-reads-cold-summary.txt)
and the per-run JSONL). That is 16 page faults of about 107 µs each, one
after the other. The volume mapping is advised `MADV_RANDOM` at open, and on
Linux that turns off fault readaround: each fault reads one 4 KiB page,
synchronously. LMDB maps its file with the default policy, so a fault reads
128 KiB around the faulting page (`read_ahead_kb`) and arms asynchronous
readahead further on; its reads stream ahead of the gets.

Two measurements confirm it. main with the open-time `MADV_RANDOM` removed
and nothing else changed reads cold 64 KiB at 2.40 GB/s and 128 KiB at 2.04
([`small-reads-normal-summary.txt`](kv-cache-benchmark/small-reads/small-reads-normal-summary.txt)).
And a per-document hint alone, the change below with its sequential window
off, reads 64 KiB at only 0.23 GB/s (p50 263 µs): one 64 KiB read per
document goes out when that document is asked for, and nothing is in flight
ahead of it. The #28 experiment of lowering `readahead_min_bytes` to 64 KiB
got the same 0.24. A per-document hint cannot close this gap; reading ahead
across documents can.

The #28 experiment also cost warm `view` reads 8–18 %. That came from
running the large-document hint on every read: each get read the clock and
probed the re-advise filter, and the filter's collisions (4096 documents in
8192 direct-mapped slots, under a Zipf load) sent a share of warm gets into
a `mincore()` call, on a read that otherwise takes 0.4–0.5 µs. A cheaper
residency check does not fix that shape: any per-read call is a large share
of a sub-microsecond read.

### Stripe interleaving is a real pattern

`kv_bench` reads blocks back in the order it wrote them. That is the order a
KV tier sees when a prompt prefix is reused: the prefix's blocks were written
in order when the sequence was first computed, and are read back in the same
order. Their keys are hashes, so consecutive blocks land on different
stripes, but each stripe's share of them sits back to back in that stripe's
log. So the pattern is real, and it is sequential per stripe. Changing
placement (keeping a sequence's blocks in one stripe) would need the caller
to name the sequence, and would give up the hash spread that keeps stripes
evenly loaded. Predicting *which* stripe comes next would need the next key,
which only the caller has. A per-stripe window needs neither: it reads ahead
in each stripe on its own, and with 16 stripes that keeps up to 16 windows
in flight at once.

### The change

`Volume::advise_cold_read` runs on one kind of read only: a read that is
about to run the CRC pass, because this incarnation of the document has not
been verified in this process yet (the first read after a write, after a
restart, or after the offset fell out of the checksum-validation cache).
That is the read that makes Cyclone's first touch of the content, and the
one a cold document takes. A validated warm re-read never reaches it, so it
needs no filter, reads no clock and costs the warm path nothing. On that
read, for documents of at least `CacheConfig::cold_readahead_min_bytes`
(16 KiB):

1. **Cold hint.** A document below `readahead_min_bytes` gets the
   per-platform readahead hint over its own range.
2. **Sequential window.** A thread-local detector keeps, per stripe, where
   the last read ended. A read that starts there, or up to 64 KiB past it,
   continues the run, and the hint is extended by
   `CacheConfig::sequential_readahead_bytes` (1 MiB) past the document,
   within the stripe and never past its write cursor. It is re-issued once
   less than half of the window is left ahead of the reader, so a run of
   64 KiB documents issues one hint per 8 of them. The detector is indexed
   by the stripe's position in its volume, so the stripes of one volume
   never share a slot.
3. **Directory hint at open.** After a restart with a cold page cache, the
   mmap directory is cold too, and each first lookup in a bucket took a
   serial 4 KiB fault before the document read could start. With the first
   two parts in place, cold 64 KiB reads after a restart still ran at 0.49
   GB/s against 2.46 before it. `Volume::open` now issues one readahead hint
   over each existing stripe directory (about 0.7 MiB per stripe).

On Linux the hints go out without a residency check. On pages the process
has not mapped yet, which is what a CRC-pending read touches, `mincore()`
costs more than the `madvise()` it would save: 1.6 against 1.0 µs for 16
pages, 12.6 against 7.7 µs for 256, measured on this machine. The exception
is a window of a run that is already resident (documents read back soon
after they were written, or after a restart with a warm page cache): once a
full check finds one window of the run all resident, the next windows check
only their first 64 KiB and are skipped while that holds.

Documents below 16 KiB, and every read with `verify_checksum_on_read` off,
behave exactly as before. `cold_readahead_min_bytes = 0` turns all three
parts off; `sequential_readahead_bytes = 0` turns off the window only.
`CacheStats::cold_readahead_hints` and `sequential_readahead_hints` count the
hints issued.

The options in the issue, and what became of them:

- **A cheaper residency check for small documents.** Replaced by the CRC
  gate, which needs no call on a warm read at all.
- **A per-stripe sequential detector.** Taken (part 2), per thread, so it
  adds no shared cache line to the read path.
- **The mapping-wide policy.** Measured, not taken. `MADV_NORMAL` on main
  reads 64 KiB and 128 KiB cold at 2.40 and 2.04 GB/s with no code, but
  does nothing for larger documents (256 KiB 1.04, 512 KiB 1.57, 1 MiB
  1.67, about main's rates), and it turns every cold miss of a small HTTP
  object into a 128 KiB read until the kernel's per-file miss heuristic
  switches readaround off. The change gets more at every size, keeps
  `MADV_RANDOM` for everything else, and works the same way on macOS and
  Windows.
- **A `pread` fallback for cold small reads.** Not done. `read_sync` returns
  a view into the mapping; a read into a private buffer would need a copy
  and a different handle, and a synchronous read of one document is the
  0.23 GB/s case again.

### Choosing the window

Cold first touch (restart in parentheses), GB/s, median of three runs per
window, one binary (6c060e9)
([`small-reads-ablate-summary.txt`](kv-cache-benchmark/small-reads/small-reads-ablate-summary.txt)):

| window | 64 KiB | 128 KiB | 256 KiB | 512 KiB | 1 MiB |
|---|---:|---:|---:|---:|---:|
| 0 (cold hint only) | 0.23 (0.23) | 0.50 (0.48) | 1.09 (1.09) | 1.61 (1.59) | 1.52 (1.56) |
| 256 KiB | 2.44 (2.40) | 2.51 (2.48) | 2.24 (2.23) | 2.05 (1.97) | 1.72 (1.76) |
| 512 KiB | 2.63 (2.58) | 2.49 (2.73) | 2.70 (2.55) | 2.69 (2.73) | 1.87 (1.86) |
| **1 MiB** | **2.63 (2.58)** | **2.80 (2.86)** | **2.76 (2.77)** | **2.96 (2.93)** | **3.03 (3.00)** |
| 2 MiB | 2.58 (2.53) | 2.84 (2.84) | 2.91 (2.77) | 2.97 (2.94) | 3.01 (3.13) |

A window smaller than the document helps little: at 1 MiB documents only
windows of 1 MiB and up lift the rate. 2 MiB reads the same as 1 MiB, so the
default is 1 MiB. On a run that stops, the most a thread can have read ahead
for nothing is one window per stripe.

### Before and after, cold (Linux, page cache dropped)

`kv_bench --seconds 1 --threads 1 --skip-multiprocess` over eight block
sizes, main, the change and LMDB interleaved, three runs each. Median GB/s,
first touch (restart in parentheses)
([`small-reads-cold-summary.txt`](kv-cache-benchmark/small-reads/small-reads-cold-summary.txt);
the earlier build's set, run first, is in
[`small-reads-cold-earlier-summary.txt`](kv-cache-benchmark/small-reads/small-reads-cold-earlier-summary.txt)):

| block | main | change | change / main | LMDB, same day | change / LMDB | earlier build | LMDB, round 6 |
|---|---:|---:|---:|---:|---:|---:|---:|
| **64 KiB** | 0.04 (0.04) | **2.15 (2.60)** | 54× (67×) | 1.42 (1.15) | **1.5×** (2.3×) | 2.63 (2.62) | 1.61 (1.48) |
| 128 KiB | 0.04 (0.04) | 2.70 (2.85) | 69× (73×) | 1.43 (1.45) | 1.9× (2.0×) | 2.83 (2.64) | — |
| **256 KiB** | 1.10 (0.84) | **2.69 (2.78)** | 2.4× (3.3×) | 1.60 (1.52) | **1.7×** (1.8×) | 2.76 (2.82) | 2.11 (1.97) |
| **512 KiB** | 1.63 (1.33) | **2.93 (2.99)** | 1.8× (2.2×) | 1.70 (1.72) | **1.7×** (1.7×) | 2.94 (2.90) | 2.19 (2.18) |
| 1 MiB | 1.43 (1.53) | 2.99 (2.99) | 2.1× (2.0×) | 1.96 (1.94) | 1.5× (1.5×) | 3.03 (3.00) | 2.65 (2.64) |
| 2 MiB | 2.25 (2.02) | 2.64 (2.62) | 1.17× (1.30×) | 2.08 (2.02) | 1.27× (1.30×) | 2.63 (2.64) | 2.65 (2.57) |
| 8 MiB | 2.84 (2.60) | 3.01 (2.94) | 1.06× (1.13×) | 2.03 (2.05) | 1.48× (1.44×) | 3.00 (2.96) | 2.62 (2.59) |
| 32 MiB | 3.15 (3.01) | 3.17 (3.12) | 1.01× (1.04×) | 2.00 (2.00) | 1.58× (1.56×) | 3.17 (3.13) | 2.75 (2.63) |

- **64 KiB: 0.04 → 2.15–2.63 GB/s.** The three final-build runs read 2.15,
  1.03 and 2.58 at first touch. The second hit a slow-device window (p99
  1.1 ms against 0.16–0.28 ms in the others); the earlier build's three runs
  read 2.61–2.65. Either way it is ahead of LMDB on the same day and of
  round 6's LMDB. A median get takes 8–9 µs, served from pages a window
  already brought in; the p99 is the get that waits for a window.
- **Every size up to 1 MiB is 1.8–2.4× faster than main**, and restart
  matches first touch (the directory hint): 256 KiB restart 0.84 → 2.78.
- **LMDB read 12–27 % slower today than in round 6** at every size, with
  the same harness binary on the same machine, and main 2–19 % slower: the
  device was slower on the day. Compare within the day. The change is
  ahead of round 6's LMDB too, at every size except 2 MiB, where they tie
  (2.64 against 2.65).
- **8 MiB**, flagged in #28 as 2–5 % slower after the chunking change, reads
  6 % faster than main here: the window reaches the next document.
- A runner job ran during two of the three main runs (the samples record
  it). main's 64 KiB and 128 KiB rates are fault-bound and did not move.

### Warm reads, the 4-process phase and puts

64 KiB, 256 KiB and 512 KiB, all phases, `--seconds 5 --threads 1,4`, main
and the change (build 171a48f) interleaved, three runs each. Median, change
/ main
([`small-reads-warm-summary.txt`](kv-cache-benchmark/small-reads/small-reads-warm-summary.txt)):

| | 64 KiB | 256 KiB | 512 KiB |
|---|---:|---:|---:|
| Warm `view`, T = 1 / 4 | 1.01 / 1.01 | 1.01 / 0.99 | 1.01 / 1.00 |
| Warm `copy`, T = 1 / 4 | 1.00 / 1.00 | 1.00 / 1.01 | 1.01 / 1.00 |
| 4 reader processes, `view` / `copy` | 0.99 / 1.00 | 0.98 / 1.01 | 0.98 / 1.01 |
| PUT | 1.00 | 0.99 | 0.98 |
| Cold first touch (restart) | 71× (73×) | 2.77× (3.58×) | 1.96× (2.29×) |

Warm reads do not move: a validated re-read never reaches the new code, and
that path is the same in the final build. The 4-process `view` medians are
1–2 % lower, inside the per-run spread (each side has one run 3–10 % below
its others); each reader process runs the CRC pass once per document and
pays one hint there. The final build's own warm set is in the raw data but
not used: a containerised build job started during its first run (load 14
at its end) that the runner-job check did not see, and it halved warm
`copy` in two of the three runs. `small-reads/idle.sh` now also waits for
container jobs.

### What the hints cost when the document is already resident

The one place the change costs something is a CRC-pending read of a document
that is already in the page cache: the first read after a write while the
pages are still cached, or after a restart with a warm page cache. The cold
command without dropping caches, three interleaved runs, median GB/s, first
touch (restart). "Window off" runs the change with
`--sequential-readahead-bytes 0`, so every read takes the per-document hint,
as a first read out of order does
([`small-reads-resident-summary.txt`](kv-cache-benchmark/small-reads/small-reads-resident-summary.txt)):

| block | main | change | change / main | window off | window off / main |
|---|---:|---:|---:|---:|---:|
| 64 KiB | 8.06 (8.10) | 7.52 (7.62) | 0.93 (0.94) | 6.54 (6.64) | 0.81 (0.82) |
| 128 KiB | 8.68 (8.63) | 8.15 (8.10) | 0.94 (0.94) | 7.27 (7.20) | 0.84 (0.83) |
| 256 KiB | 7.93 (7.80) | 7.61 (7.47) | 0.96 (0.96) | 7.91 (7.77) | 1.00 (1.00) |
| 512 KiB | 8.60 (8.49) | 8.23 (8.12) | 0.96 (0.96) | 8.62 (8.48) | 1.00 (1.00) |

In a sequential run the cost is the residency check per window, 4–7 %. Out
of order, a 64 KiB document pays one `madvise()` it did not need, about 2 µs
on this machine (19 %). From 256 KiB up the per-document hint is the
large-document one, as before. The first build, with a residency check
before every hint and without the cursor clamp or the resident-run probe,
cost the sequential case 12–21 %. On macOS the same reads cost 0–3 %
(below).

### 4 KB objects

`performance_baseline --cache-size 512 --entries 5000 --content-size 4096`,
three interleaved runs: every operation's median is within 4 % of main's,
the change's higher on all but key generation
([`small-reads-baseline-summary.txt`](kv-cache-benchmark/small-reads/small-reads-baseline-summary.txt)).
Documents below 16 KiB never reach the new code. main's first run followed
the resident runs and was slow on every operation, as in the readahead
section.

### macOS (Apple M5, warm page cache)

macOS cannot drop the page cache on the shared machine, so this checks the
warm path and the resident cost only. main and the final build interleaved,
five runs each, with other jobs running (1-minute load 1–5;
[`small-reads-macos-summary.txt`](kv-cache-benchmark/small-reads/small-reads-macos-summary.txt)):

- `performance_baseline` (4 KB): every read, lookup and miss within 2 %.
  Its write and mixed rows are bimodal on that machine (about 49 k or 305 k
  writes/s, run to run, in both builds) and say nothing either way.
- `concurrent_read_bench`, 512 B objects: 0.98 / 1.00 / 0.99 at 1 / 4 / 16
  threads; 64 KiB objects: 0.98 / 0.98 / 1.00 (0.95 at 8 threads, inside
  its run spread).
- `kv_bench` 64 KiB and 128 KiB warm `view` and `copy`: within 1 %;
  4-process `view` 0.97 and 0.99.
- First touch and restart, which there CRC-verify resident documents: 0.97
  to 1.01. That is one `F_RDADVISE` per window, about 0.3 µs.

### Caveats

- One Linux machine and SSD (Samsung 970 PRO, `read_ahead_kb` 128, kernel
  5.15), shared with CI jobs. Every point waited for the runner to be idle
  and the 1-minute load to drop below 1.0; a job that starts mid-run is not
  prevented, and the samples record runner jobs (and, from `small-reads/`'s
  own sampler on, container jobs).
- The window helps one thread reading one stripe's documents in the order
  they were appended. More than 64 KiB of other writers' documents appended
  in between breaks the run, and several threads splitting one prefix each
  see a partial run; both fall back to the per-document hint.
- The hints need the CRC pass. With `verify_checksum_on_read = false`, or
  on a document whose CRC verdict is still cached but whose pages were
  evicted, a small document faults in page by page as before.

## Device transfer: does zero-copy pay off? (Metal, Apple silicon)

The open question from rounds 1 and 2 is whether the zero-copy read pays
off *end to end* — a KV
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

The Metal section above answers the Apple half of the zero-copy question on
unified memory, where there is no bus at all. `benchmarks/kv_gpu_cuda.cu`
(plus its C++23 host half, `benchmarks/kv_gpu_cuda_host.cpp`) asks the same
question where there *is* one: a discrete GPU behind PCIe, on which the staged
path really does pay a host copy that a mapping-sourced transfer does not.
Same workload (`benchmarks/kv_workload.hpp`, so the blocks are bit-identical
to `kv_bench`'s and the Metal run's), same Cyclone tuning, same N = 512 blocks
of 2 MiB, same "already in the store, read once, every page touched before any
timing" discipline.

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
Cyclone half completed before the machine ran out of memory and says the same
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

## What to change

| Item | Status | Evidence |
|---|---|---|
| Readahead for large cold reads: a per-document hint, with a per-platform call | **Done** | [Round 3](#readahead-alone); mechanism in [architecture.md](architecture.md#memory-mapped-io) |
| Readahead for medium cold reads: the Linux hint in 64 KiB chunks over a document's first 4 MiB, and no hint on a resident document | **Done** (#18) | [Readahead chunking](#readahead-chunking-issue-18): cold 512 KiB 0.81 → 1.70 GB/s, 1.39× behind a same-day LMDB; warm 512 KiB `view` +7 % |
| Readahead below the 256 KiB threshold, and across stripe-interleaved documents | **Done** (#29): a hint on the CRC-pending read only (no warm-path cost), a per-stripe sequential window, and a directory hint at open. Cold 64 KiB 0.04 → 2.15–2.63 GB/s, 512 KiB 1.63 → 2.93, ahead of a same-day LMDB at every size; warm reads unchanged; a CRC-pending read of a resident 64 KiB document costs 6–19 % | [Small cold reads](#small-cold-reads-issue-29) |
| Fast document checksum: slice-by-16 / ARMv8 CRC32, then CRC-32C at format v8 | **Done** | [Round 3](#fast-crc32-alone), [Round 3b](#round-3b-crc-32c-on-disk-format-v8); [architecture.md](architecture.md#document-format) |
| Verified state that outlives the process: keep the CRC-validation cache beside the mmap directory, so a restart and every peer process skip re-verification | Designed, then shelved ([design](design/verified-state.md)); the 512 KiB gap was tracked as a readahead item (#18, since [fixed](#readahead-chunking-issue-18)). Measured there: about half of a warm-page-cache first read (view) is the CRC pass, but only 2–9 % of a cold NVMe read. The 512 KiB cold gap to LMDB is the I/O pattern, not the CRC | [Design, section 2](design/verified-state.md#2-what-is-avoidable-measurement) |
| `WriteHandle::reserve(n)` that returns the destination span, so the caller writes or DMAs straight into the record (three copies become one) | **Done** (#16), with the destination being the handle's buffer: a slot exists only at commit, under the write lock held across the fill. The larger win was dropping the contiguous-document build; puts went from 0.62 to 1.52 GB/s at 2 MiB, 4 % ahead of a same-day file-per-block, and `reserve()` takes a 2 MiB insert from 490 to 410 µs p50 | [Insert path](#insert-path-profile-issue-16) |
| An entry point that gives an embedder the mapping identity for one-time GPU registration, instead of inferring it from `content()` / `content_file_offset()` / `volume_files()` | Open | [Metal](#device-transfer-does-zero-copy-pay-off-metal-apple-silicon), [CUDA](#device-transfer-cuda-gtx-1050-pcie) |
| A zero-copy C read entry point and a Python binding, which is what a vLLM/SGLang connector would call | Open | — |
| Keep the previous lap resolvable until it is actually overwritten ([design](design/wrap-retention.md)) | **Done**: implemented and on by default (`wrap_retention = false` is the flush opt-out); see [CHANGELOG](../CHANGELOG.md). Measured: at 16 GiB it meets the churn decision criteria (latency clause, both patterns, T=4), in rounds 5 and 6; flush mode stays partial | [Round 5](#round-5-re-benchmark-at-main-2aed24c): hit ratio 0.758 → 0.814 (`zipf`) and 0.640 → 0.686 (`zipf+scan`), matching the replay within 0.002; served +16 % at T=1, within noise at T=4; hit p99 0.27–0.37× LMDB's. [Round 6](#round-6-re-benchmark-at-main-b5c31e8), with it on by default: hit p99 0.45× (`zipf`, worst run pairing 0.52×) and 0.38× LMDB's at 1.24× and 1.14× its served GB/s |
| Profile insert latency (miss+insert p50 4.0 ms vs 1.4–1.6 ms for the peers) | **Done** (#16): two fresh 2 MiB heap buffers per put and their ~1,000 page faults were most of it. Removed; T=1 miss+insert p50 4.41 → 1.49 ms (file-per-block 1.43, same day) | [Insert path](#insert-path-profile-issue-16) |
| The one-thread insert tail (miss+insert p99 24 ms against 6 for file-per-block and 16 for LMDB) | Explained, open. A document ends partway through a page that holds older data; when that page is not cached, ext4 reads 4 KiB synchronously inside the `pwrite`, and about 1 % of inserts wait 10–56 ms for it behind the device's other I/O. Candidates: 4 KiB-aligned document starts (a format change), or reading the boundary page ahead of the write | [Round 6](#the-one-thread-insert-tail-24-ms-p99) |
| Large-block puts: 8 MiB and 32 MiB behind file-per-block (1.27 against 1.43, 0.88 against 1.41 GB/s) | Open; not profiled. Faster than round 5 at both sizes (1.01, 0.48) | [Round 6](#regressions-and-losses-since-round-5) |
| Scan resistance or admission control on the disk tier | Open | [Round 4](#why-the-hit-ratio-and-where-it-comes-from): a scan costs every store about 12 points |

GPUDirect Storage (`cuFileRead` at `content_file_offset()`, NVMe to GPU with
no host copy) needs a data-center GPU and was not measured.

## History and corrections

The rounds above keep the numbers as measured. This section records what
was withdrawn, what was superseded, and the incidents that affected
specific runs.

### Corrections to the first run

Two methodology faults were found in the first sweep by review and
retraction is the honest fix:

- The peer harness wiped each block size's data only *before* that size ran,
  so 2–6 GiB of earlier sizes stayed resident and squeezed the 16 GiB page
  cache during the larger-block runs; `kv_bench` deletes its volume per size,
  so Cyclone never paid that. Peer warm reads were depressed up to 100× (LMDB
  `view` 4 k → 513 k gets/s) and the first draft claimed a 5–20× Cyclone read
  advantage that does not exist. Fixed in the harness; every number above is
  from the corrected sweep.
- The first headline table put a `view`-mode multi-process row under
  `copy`-mode rows, inflating the multi-process story ~10×. Phase 5 now
  runs both modes and only `copy` is compared.

### Superseded headline claims

| Claim | Where | Superseded by |
|---|---|---|
| Cyclone reads 5–20× faster than the peers | First draft of round 1 | Withdrawn: harness fault (above) |
| Multi-process read row about 10× better | First draft of round 1 | Withdrawn: `view` row under `copy` rows (above) |
| Restart is Cyclone's clearest loss, 0.56 GB/s at every size | Round 1 | Round 3: 8.45 GB/s on macOS; Round 3b: 1.97 GB/s cold on Linux |
| Cold reads are the real loss, 0.06 GB/s at 2 MiB | Round 2 | Round 3: 1.62 GB/s; Round 3b: 2.17 GB/s |
| Multi-process readers scale worse than threads | Rounds 1 and 2 | Round 3: 621 k gets/s on Linux, 1.67 M on macOS |
| Writes at a third of file-per-block | Rounds 1 and 2 | Round 3b: 1.4× behind (1.01 vs 1.44 GB/s) |
| The remaining cold-read gap to LMDB is the x86 CRC32 | Round 3 | Round 3b: level with LMDB at 2 MiB |
| Writes about 1 GB/s, 1.4× behind file-per-block | Rounds 3b and 5 | Insert path, round 6: level at 2 MiB (1.47 vs 1.52), ahead at 512 KiB; behind at 8 and 32 MiB |
| Single-thread churn serves 0.7–0.8× LMDB | Round 5 | Round 6: 1.22–1.28× a same-day LMDB |
| Churn hit p99 0.27–0.37× LMDB's (retention, T=4) | Round 5 | Round 6: 0.38–0.45× against a same-day LMDB whose tail was shorter |
| Flush mode passes on `zipf` at T=4 | Round 5 | Round 6: 0.89× served, under the 0.9× floor |

Earlier drafts quoted the readahead-alone result as 0.449 GB/s and as
0.140 → 0.427 GB/s. The median-of-three table in
[Round 3](#readahead-alone) is authoritative: 0.120 → 0.446 GB/s with the
checksum verified.

### Round 3: the macOS writeback artifact

This was a benchmark artifact, not a regression. On macOS the faster put
and first touch (about 6 s instead of about 17 s for 4 GiB) meant the warm
phase started while the OS was still writing the dataset back. Reads of
in-flight pages then measured writeback (2 MiB warm `view`: 23 k gets/s,
p99 250 µs). With the hint off it was worse (870 gets/s). A 60 s settle
(`kv_bench --pause-before-warm`) restores 615–700 k. Linux is immune,
because `drop_caches` syncs first. The round-3 macOS table is the settled
run.

### Round 3: the 512 KiB restart re-run

One cell, 512 KiB restart, came out at 0.10 GB/s in the Linux sweep and at
0.55 GB/s in two clean re-runs. The re-runs confirmed all 4096 readahead
hints firing after the reopen (`kv_bench` now prints
`readahead_hints_issued` per phase). The table uses the clean re-run, and
the raw file records both.

### The Linux dirty-page incident

During round 3 the Linux machine ran with `vm.dirty_bytes` raised to 12 GiB
and its writers were throttled by a dirty-page accounting leak that
persisted until reboot; later rounds ran after a fresh boot at
kernel-default limits.

## Caveats

All rounds:

- One laptop per platform, one SSD each, 10 s per point, one run per point
  except where a table says "median of three" or round 4 lists a repeat.
  Treat differences under about 20 % as noise; the multi-× differences are
  not.
- Consumer hardware: dual-channel DDR4 and one NVMe on Linux. A
  server-class NVMe array and a data-center GPU would change the absolute
  numbers, not the ordering.
- Warm-phase GB/s is inflated for every store. With Zipf(0.99) over 2048
  blocks the hot set is largely CPU-cache-resident, so read those rows as
  per-get overhead plus copy cost, not as storage bandwidth.
- The peers measure a `std::string` allocation and a SHA-256 inside their
  warm-phase sample (about 1 µs); Cyclone hashes outside it. This does not
  matter at latencies of 100 µs and more, and slightly flatters Cyclone at
  the µs scale.

Rounds 3 and 3b:

- The peers were not re-run. Their numbers are from the round-1 and round-2
  sweeps, taken on the same machines before the dirty-page incident.

Round 4:

- One run per point, except 2 MiB T=4 (two runs). T=4 served varied by up
  to 11 % between runs (Cyclone `zipf+scan`: 1.00 vs 0.89 GB/s). That is
  the same size as the margin the only passing criterion rests on.
- Background load was not stopped. The load at run start is in the logs; it
  was higher (3–4.7) during the repeat.
- Write amplification is device sectors written (whole partition) over
  payload inserted, including a final `syncfs`. It is about 1.00 for every
  store, so it does not tell them apart here.
- Only copy-mode consumption is measured; Cyclone's zero-copy `view` path is
  not exercised by this workload.

Round 5:

- Two runs of each 2 MiB T=4 point for Cyclone and LMDB, and one run
  otherwise. T=4 served varied by up to 32 % between runs on the same day,
  and by 25–45 % between days for the same code (the 3823122 control).
  The verdict rests on the latency clause, which has margin (0.27–0.37×
  against 0.5×). Its served floor has less margin: 1.02× on `zipf+scan`
  in one run, against 0.9×.
- LMDB and filedir were re-run at T=4 only; T=1 comparisons use round 4's
  peers.
- The macOS runs shared the machine with a VM and other sessions on a 97 %
  full disk. Only their warm-path and hit-ratio numbers are comparable with
  earlier rounds.

Round 6:

- Three interleaved runs per point, medians. Every point waited for no
  runner job and a 1-minute load below 1.0, but a job can start mid-run:
  it did in 8 of 60 churn points (re-run, and left out of the churn
  medians) and in 4 of 21 sweep points (kept; the medians absorb them).
  The sampler detects a job's worker process, not its I/O.
- T=4 served still varies by up to 36 % (fastest over slowest) between
  runs of one point on the same day (Cyclone flush `zipf`: 1.63–2.21 GB/s;
  LMDB `zipf`: 1.51–2.00). The retention verdict on `zipf` rests on a hit-p99 ratio of
  0.45× whose worst run pairing is 0.52×; flush mode's T=4 served ratios
  sit at 0.88–0.89× against a 0.9× floor.
- The insert-tail trace is of separate traced runs (4–7 % slower), one
  per store. It identifies where the thread blocks, not the device-side
  latency of the blocking read. Which page of a document is read is
  inferred from the layout.
- Linux only; the macOS numbers were not re-run.

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

## Appendix C — Linux round-3 generated tables

`cyclone` = round 3; `stock` = the stock tree from the round-2 sweep; peers
as in Appendix B. Cells as in Appendix A.

### Phase 1 - PUT (single writer, sequential)

Cells: ops/s / GB/s / p99 us.

| block size | cyclone | cyclone-noverify | stock | filedir | filedir-read | lmdb | rocksdb |
|---|---|---|---|---|---|---|---|
| 512 KiB | 1.2k / 0.61 / 847 | - | 579.0 / 0.30 / 1888 | 2.6k / 1.38 / 330 | 2.6k / 1.37 / 351 | 115.6 / 0.06 / 12651 | 1.1k / 0.59 / 1469 |
| 2 MiB | 386.3 / 0.81 / 3413 | 230.7 / 0.48 / 5814 | 163.8 / 0.34 / 13113 | 686.5 / 1.44 / 1092 | 660.7 / 1.39 / 1134 | 72.1 / 0.15 / 20484 | 291.9 / 0.61 / 39055 |
| 8 MiB | 90.1 / 0.76 / 13970 | - | 41.6 / 0.35 / 27850 | 159.8 / 1.34 / 5255 | 156.8 / 1.32 / 4770 | 42.0 / 0.35 / 30866 | 71.9 / 0.60 / 55080 |
| 32 MiB | 11.7 / 0.39 / 80314 | - | 7.5 / 0.25 / 130183 | 42.5 / 1.42 / 18911 | 38.8 / 1.30 / 21741 | 13.5 / 0.45 / 70673 | 15.6 / 0.52 / 60536 |

### Phase 2 - GET first touch (single thread, view)

Cells: ops/s / GB/s / p99 us.

| block size | cyclone | cyclone-noverify | stock | filedir | filedir-read | lmdb | rocksdb |
|---|---|---|---|---|---|---|---|
| 512 KiB | 1.2k / 0.63 / 1445 | - | 137.0 / 0.07 / 13232 | 1.8k / 0.94 / 879 | 2.3k / 1.20 / 798 | 3.7k / 1.93 / 1417 | 1.7k / 0.87 / 982 |
| 2 MiB | 771.4 / 1.62 / 1868 | 1.1k / 2.31 / 1551 | 30.5 / 0.06 / 59454 | 838.8 / 1.76 / 2356 | 621.5 / 1.30 / 2548 | 1.0k / 2.15 / 3917 | 390.2 / 0.82 / 4571 |
| 8 MiB | 194.8 / 1.63 / 5310 | - | 17.2 / 0.14 / 63363 | 299.4 / 2.51 / 6188 | 253.6 / 2.13 / 6470 | 252.7 / 2.12 / 6555 | 103.2 / 0.87 / 15543 |
| 32 MiB | 38.2 / 1.28 / 30223 | - | 4.7 / 0.16 / 229454 | 77.5 / 2.60 / 16553 | 33.0 / 1.11 / 39479 | 65.5 / 2.20 / 21097 | 13.7 / 0.46 / 102098 |

### Phase 3 - GET warm, Zipf(0.99)

#### view, T = 1

Cells: gets/s / GB/s / p99 us.

| block size | cyclone | cyclone-noverify | stock | filedir | filedir-read | lmdb | rocksdb |
|---|---|---|---|---|---|---|---|
| 512 KiB | 757.7k / 397.26 / 7 | - | 854.7k / 448.13 / 2 | 25.4k / 13.33 / 57 | 21.5k / 11.29 / 73 | 678.9k / 355.95 / 2 | 9.5k / 5.01 / 138 |
| 2 MiB | 265.1k / 555.97 / 5 | 266.1k / 558.13 / 5 | 274.4k / 575.37 / 5 | 7.0k / 14.73 / 195 | 5.3k / 11.17 / 258 | 244.6k / 512.96 / 5 | 1.8k / 3.87 / 620 |
| 8 MiB | 68.2k / 572.34 / 20 | - | 69.5k / 582.87 / 16 | 1.9k / 15.56 / 679 | 1.1k / 9.22 / 1127 | 66.8k / 560.36 / 16 | 364.6 / 3.06 / 3087 |
| 32 MiB | 17.2k / 576.47 / 92 | - | 17.6k / 589.45 / 74 | 457.4 / 15.35 / 2725 | 217.7 / 7.30 / 5702 | 17.1k / 574.40 / 62 | 29.8 / 1.00 / 34898 |

#### copy, T = 1

Cells: gets/s / GB/s / p99 us.

| block size | cyclone | cyclone-noverify | stock | filedir | filedir-read | lmdb | rocksdb |
|---|---|---|---|---|---|---|---|
| 512 KiB | 31.1k / 16.32 / 48 | - | 33.1k / 17.36 / 46 | 13.9k / 7.28 / 106 | 16.4k / 8.58 / 94 | 31.4k / 16.48 / 46 | 7.8k / 4.07 / 148 |
| 2 MiB | 5.0k / 10.48 / 263 | 5.2k / 10.96 / 259 | 5.7k / 11.91 / 235 | 3.1k / 6.49 / 400 | 3.8k / 8.03 / 328 | 6.1k / 12.85 / 224 | 1.4k / 2.99 / 767 |
| 8 MiB | 1.2k / 10.18 / 937 | - | 1.4k / 11.65 / 806 | 808.3 / 6.78 / 1420 | 772.5 / 6.48 / 1441 | 1.4k / 11.82 / 808 | 289.8 / 2.43 / 3628 |
| 32 MiB | 328.9 / 11.04 / 3413 | - | 346.0 / 11.61 / 3181 | 206.2 / 6.92 / 5491 | 135.5 / 4.55 / 7648 | 314.8 / 10.56 / 3388 | 27.6 / 0.93 / 40346 |

#### view, T = 4

Cells: gets/s / GB/s / p99 us.

| block size | cyclone | cyclone-noverify | stock | filedir | filedir-read | lmdb | rocksdb |
|---|---|---|---|---|---|---|---|
| 512 KiB | 2.71M / 1422.80 / 8 | - | 2.97M / 1556.56 / 2 | 27.3k / 14.32 / 332 | 52.8k / 27.66 / 104 | 2.13M / 1117.56 / 3 | 24.0k / 12.57 / 225 |
| 2 MiB | 646.8k / 1356.52 / 10 | 647.2k / 1357.20 / 10 | 754.9k / 1583.13 / 9 | 11.1k / 23.31 / 772 | 6.4k / 13.43 / 710 | 624.9k / 1310.58 / 9 | 1.7k / 3.63 / 2843 |
| 8 MiB | 131.6k / 1103.53 / 53 | - | 158.2k / 1326.75 / 41 | 3.6k / 30.27 / 1976 | 1.2k / 10.15 / 3569 | 146.0k / 1224.42 / 45 | 390.2 / 3.27 / 10656 |
| 32 MiB | 32.4k / 1086.91 / 171 | - | 38.0k / 1274.45 / 145 | 1.1k / 37.82 / 5419 | 249.6 / 8.38 / 16921 | 36.3k / 1216.70 / 150 | 68.9 / 2.31 / 64826 |

#### copy, T = 4

Cells: gets/s / GB/s / p99 us.

| block size | cyclone | cyclone-noverify | stock | filedir | filedir-read | lmdb | rocksdb |
|---|---|---|---|---|---|---|---|
| 512 KiB | 50.8k / 26.63 / 128 | - | 59.1k / 30.96 / 106 | 27.5k / 14.44 / 223 | 41.7k / 21.88 / 122 | 60.1k / 31.53 / 110 | 13.5k / 7.09 / 343 |
| 2 MiB | 4.7k / 9.86 / 1019 | 5.4k / 11.38 / 952 | 5.6k / 11.64 / 864 | 4.8k / 10.00 / 1087 | 3.8k / 7.95 / 1154 | 5.7k / 11.86 / 836 | 1.4k / 2.86 / 3174 |
| 8 MiB | 1.1k / 9.63 / 3799 | - | 1.4k / 11.36 / 3222 | 1.2k / 10.42 / 4172 | 697.8 / 5.85 / 6034 | 1.3k / 11.30 / 3207 | 300.8 / 2.52 / 13734 |
| 32 MiB | 323.9 / 10.87 / 13947 | - | 308.6 / 10.36 / 14711 | 324.8 / 10.90 / 15112 | 124.9 / 4.19 / 33132 | 316.0 / 10.60 / 13624 | 57.4 / 1.93 / 75925 |

#### view, T = 8

Cells: gets/s / GB/s / p99 us.

| block size | cyclone | cyclone-noverify | stock | filedir | filedir-read | lmdb | rocksdb |
|---|---|---|---|---|---|---|---|
| 512 KiB | 3.76M / 1973.14 / 11 | - | 4.26M / 2233.45 / 4 | 28.7k / 15.05 / 721 | 48.3k / 25.30 / 266 | 2.77M / 1452.10 / 6 | 10.0k / 5.24 / 1667 |
| 2 MiB | 662.9k / 1390.24 / 25 | 636.6k / 1335.11 / 25 | 799.9k / 1677.44 / 21 | 10.2k / 21.38 / 1717 | 5.0k / 10.50 / 2431 | 649.2k / 1361.45 / 21 | 1.6k / 3.40 / 7254 |
| 8 MiB | 130.9k / 1098.19 / 105 | - | 158.4k / 1328.49 / 86 | 3.4k / 28.79 / 4157 | 1.2k / 10.11 / 9760 | 149.0k / 1250.24 / 88 | 374.2 / 3.14 / 29980 |
| 32 MiB | 32.8k / 1100.70 / 408 | - | 38.6k / 1296.12 / 345 | 1.1k / 35.98 / 11570 | 235.8 / 7.91 / 48260 | 37.2k / 1248.82 / 351 | 69.7 / 2.34 / 152073 |

#### copy, T = 8

Cells: gets/s / GB/s / p99 us.

| block size | cyclone | cyclone-noverify | stock | filedir | filedir-read | lmdb | rocksdb |
|---|---|---|---|---|---|---|---|
| 512 KiB | 42.6k / 22.33 / 319 | - | 51.5k / 27.02 / 264 | 25.2k / 13.19 / 503 | 24.2k / 12.67 / 614 | 47.0k / 24.63 / 287 | 6.1k / 3.19 / 2396 |
| 2 MiB | 4.6k / 9.57 / 2543 | 5.2k / 10.91 / 2515 | 5.5k / 11.61 / 2363 | 4.4k / 9.32 / 2581 | 2.9k / 6.05 / 4107 | 5.4k / 11.42 / 2073 | 1.3k / 2.64 / 9224 |
| 8 MiB | 1.1k / 9.38 / 11333 | - | 1.3k / 11.11 / 9293 | 1.2k / 9.66 / 10079 | 644.6 / 5.41 / 17486 | 1.3k / 11.05 / 8197 | 271.9 / 2.28 / 42142 |
| 32 MiB | 316.9 / 10.63 / 37391 | - | 299.6 / 10.05 / 45565 | 288.8 / 9.69 / 40300 | 122.8 / 4.12 / 91355 | 321.5 / 10.79 / 40234 | 56.5 / 1.90 / 192802 |

### Phase 4 - RESTART (close, reopen, GET all N)

Cells: ops/s / GB/s / p99 us / hit fraction.

| block size | cyclone | cyclone-noverify | stock | filedir | filedir-read | lmdb | rocksdb |
|---|---|---|---|---|---|---|---|
| 512 KiB | 1.1k / 0.55 / 1524 / 1.000 | - | 144.6 / 0.08 / 12856 / 1.000 | 1.8k / 0.94 / 935 / 1.000 | 2.0k / 1.03 / 751 / 1.000 | 3.7k / 1.92 / 1390 / 1.000 | 1.8k / 0.92 / 857 / 1.000 |
| 2 MiB | 686.8 / 1.44 / 2226 / 1.000 | 0.0 / 0.00 / 1 / 0.000 | 30.9 / 0.06 / 56714 / 1.000 | 823.0 / 1.73 / 2440 / 1.000 | 624.8 / 1.31 / 2537 / 1.000 | 1.0k / 2.16 / 3628 / 1.000 | 390.6 / 0.82 / 4361 / 1.000 |
| 8 MiB | 182.7 / 1.53 / 5769 / 1.000 | - | 17.3 / 0.14 / 63020 / 1.000 | 300.2 / 2.52 / 5731 / 1.000 | 256.1 / 2.15 / 6462 / 1.000 | 258.8 / 2.17 / 6377 / 1.000 | 101.0 / 0.85 / 15478 / 1.000 |
| 32 MiB | 35.0 / 1.18 / 35343 / 1.000 | - | 4.6 / 0.15 / 227921 / 1.000 | 78.5 / 2.63 / 14696 / 1.000 | 33.2 / 1.11 / 39372 / 1.000 | 64.1 / 2.15 / 20329 / 1.000 | 16.6 / 0.56 / 73849 / 1.000 |

### Phase 5 - MULTI-PROCESS READ (P=4 processes, T=1)

#### view

Cells: ops/s / GB/s / hit fraction.

| block size | cyclone | cyclone-noverify | stock | filedir | filedir-read | lmdb | rocksdb |
|---|---|---|---|---|---|---|---|
| 512 KiB | 2.49M / 1304.35 / 1.000 | - | 2.17M / 1138.23 | 85.6k / 44.90 / 1.000 | 51.8k / 27.15 / 1.000 | 2.09M / 1097.54 / 1.000 | - |
| 2 MiB | 620.7k / 1301.69 / 1.000 | - | 216.8k / 454.73 | 26.0k / 54.53 / 1.000 | 6.5k / 13.54 / 1.000 | 585.0k / 1226.91 / 1.000 | - |
| 8 MiB | 127.5k / 1069.91 / 1.000 | - | 46.5k / 389.89 | 6.3k / 52.54 / 1.000 | 1.2k / 9.70 / 1.000 | 146.4k / 1227.97 / 1.000 | - |
| 32 MiB | 31.1k / 1043.57 / 1.000 | - | 11.2k / 375.25 | 1.5k / 51.48 / 1.000 | 260.4 / 8.74 / 1.000 | 35.9k / 1203.07 / 1.000 | - |

#### copy

Cells: ops/s / GB/s / hit fraction.

| block size | cyclone | cyclone-noverify | stock | filedir | filedir-read | lmdb | rocksdb |
|---|---|---|---|---|---|---|---|
| 512 KiB | 49.5k / 25.94 / 1.000 | - | 50.7k / 26.56 | 40.5k / 21.25 / 1.000 | 39.9k / 20.90 / 1.000 | 61.9k / 32.44 / 1.000 | - |
| 2 MiB | 4.7k / 9.82 / 1.000 | - | 4.4k / 9.29 | 5.6k / 11.84 / 1.000 | 3.7k / 7.77 / 1.000 | 5.2k / 10.89 / 1.000 | - |
| 8 MiB | 1.3k / 10.54 / 1.000 | - | 1.0k / 8.63 | 1.4k / 11.66 / 1.000 | 646.4 / 5.42 / 1.000 | 1.3k / 10.55 / 1.000 | - |
| 32 MiB | 315.6 / 10.59 / 1.000 | - | 239.1 / 8.02 | 333.0 / 11.17 / 1.000 | 130.8 / 4.39 / 1.000 | 270.2 / 9.07 / 1.000 | - |

### Read scaling

#### Read scaling at 2 MiB, mode = view

Cells: gets/s / GB/s.

| threads | cyclone | cyclone-noverify | stock | filedir | filedir-read | lmdb | rocksdb |
|---|---|---|---|---|---|---|---|
| 1 | 265.1k / 555.97 | 266.1k / 558.13 | 274.4k / 575.37 | 7.0k / 14.73 | 5.3k / 11.17 | 244.6k / 512.96 | 1.8k / 3.87 |
| 4 | 646.8k / 1356.52 | 647.2k / 1357.20 | 754.9k / 1583.13 | 11.1k / 23.31 | 6.4k / 13.43 | 624.9k / 1310.58 | 1.7k / 3.63 |
| 8 | 662.9k / 1390.24 | 636.6k / 1335.11 | 799.9k / 1677.44 | 10.2k / 21.38 | 5.0k / 10.50 | 649.2k / 1361.45 | 1.6k / 3.40 |

#### Read scaling at 2 MiB, mode = copy

Cells: gets/s / GB/s.

| threads | cyclone | cyclone-noverify | stock | filedir | filedir-read | lmdb | rocksdb |
|---|---|---|---|---|---|---|---|
| 1 | 5.0k / 10.48 | 5.2k / 10.96 | 5.7k / 11.91 | 3.1k / 6.49 | 3.8k / 8.03 | 6.1k / 12.85 | 1.4k / 2.99 |
| 4 | 4.7k / 9.86 | 5.4k / 11.38 | 5.6k / 11.64 | 4.8k / 10.00 | 3.8k / 7.95 | 5.7k / 11.86 | 1.4k / 2.86 |
| 8 | 4.6k / 9.57 | 5.2k / 10.91 | 5.5k / 11.61 | 4.4k / 9.32 | 2.9k / 6.05 | 5.4k / 11.42 | 1.3k / 2.64 |

## Appendix D — Linux round-3b generated tables

`crc32c` = round 3b (readahead + CRC-32C); `crc32c-noverify` = the same
tree with `--no-mmap-dir --no-verify` (2 MiB only); `round3` = round 3, for
comparison. Peers are in Appendix B. Generated from
[`kv-cache-benchmark/crc32c/`](kv-cache-benchmark/crc32c/) and
[`kv-cache-benchmark/round3/`](kv-cache-benchmark/round3/). Cells as in
Appendix A.

### Phase 1 - PUT (single writer, sequential)

Cells: ops/s / GB/s / p99 us.

| block size | crc32c | crc32c-noverify | round3 |
|---|---|---|---|
| 512 KiB | 1.3k / 0.69 / 908 | - | 1.2k / 0.61 / 847 |
| 2 MiB | 483.9 / 1.01 / 7764 | 230.7 / 0.48 / 5744 | 386.3 / 0.81 / 3413 |
| 8 MiB | 129.9 / 1.09 / 10597 | - | 90.1 / 0.76 / 13970 |
| 32 MiB | 14.6 / 0.49 / 65649 | - | 11.7 / 0.39 / 80314 |

### Phase 2 - GET first touch (single thread, view)

Cells: ops/s / GB/s / p99 us.

| block size | crc32c | crc32c-noverify | round3 |
|---|---|---|---|
| 512 KiB | 2.3k / 1.19 / 671 | - | 1.2k / 0.63 / 1445 |
| 2 MiB | 1.0k / 2.17 / 1555 | 1.1k / 2.33 / 1076 | 771.4 / 1.62 / 1868 |
| 8 MiB | 360.8 / 3.03 / 3337 | - | 194.8 / 1.63 / 5310 |
| 32 MiB | 101.5 / 3.40 / 10511 | - | 38.2 / 1.28 / 30223 |

### Phase 3 - GET warm, Zipf(0.99)

#### view, T = 1

Cells: gets/s / GB/s / p99 us.

| block size | crc32c | crc32c-noverify | round3 |
|---|---|---|---|
| 512 KiB | 647.1k / 339.25 / 8 | - | 757.7k / 397.26 / 7 |
| 2 MiB | 266.5k / 558.97 / 5 | 211.9k / 444.38 / 8 | 265.1k / 555.97 / 5 |
| 8 MiB | 67.6k / 567.07 / 20 | - | 68.2k / 572.34 / 20 |
| 32 MiB | 16.8k / 562.99 / 95 | - | 17.2k / 576.47 / 92 |

#### copy, T = 1

Cells: gets/s / GB/s / p99 us.

| block size | crc32c | crc32c-noverify | round3 |
|---|---|---|---|
| 512 KiB | 33.1k / 17.34 / 45 | - | 31.1k / 16.32 / 48 |
| 2 MiB | 6.2k / 13.05 / 213 | 5.9k / 12.28 / 265 | 5.0k / 10.48 / 263 |
| 8 MiB | 1.5k / 12.49 / 785 | - | 1.2k / 10.18 / 937 |
| 32 MiB | 363.5 / 12.20 / 3143 | - | 328.9 / 11.04 / 3413 |

#### view, T = 4

Cells: gets/s / GB/s / p99 us.

| block size | crc32c | crc32c-noverify | round3 |
|---|---|---|---|
| 512 KiB | 2.26M / 1182.88 / 9 | - | 2.71M / 1422.80 / 8 |
| 2 MiB | 807.1k / 1692.66 / 7 | 766.3k / 1607.11 / 8 | 646.8k / 1356.52 / 10 |
| 8 MiB | 179.1k / 1502.05 / 30 | - | 131.6k / 1103.53 / 53 |
| 32 MiB | 43.3k / 1453.38 / 110 | - | 32.4k / 1086.91 / 171 |

#### copy, T = 4

Cells: gets/s / GB/s / p99 us.

| block size | crc32c | crc32c-noverify | round3 |
|---|---|---|---|
| 512 KiB | 68.3k / 35.83 / 97 | - | 50.8k / 26.63 / 128 |
| 2 MiB | 6.1k / 12.82 / 798 | 5.9k / 12.42 / 856 | 4.7k / 9.86 / 1019 |
| 8 MiB | 1.5k / 12.41 / 2981 | - | 1.1k / 9.63 / 3799 |
| 32 MiB | 356.9 / 11.97 / 12526 | - | 323.9 / 10.87 / 13947 |

#### view, T = 8

Cells: gets/s / GB/s / p99 us.

| block size | crc32c | crc32c-noverify | round3 |
|---|---|---|---|
| 512 KiB | 3.88M / 2033.26 / 12 | - | 3.76M / 1973.14 / 11 |
| 2 MiB | 920.8k / 1931.01 / 17 | 849.0k / 1780.53 / 18 | 662.9k / 1390.24 / 25 |
| 8 MiB | 184.2k / 1545.46 / 74 | - | 130.9k / 1098.19 / 105 |
| 32 MiB | 44.7k / 1500.75 / 295 | - | 32.8k / 1100.70 / 408 |

#### copy, T = 8

Cells: gets/s / GB/s / p99 us.

| block size | crc32c | crc32c-noverify | round3 |
|---|---|---|---|
| 512 KiB | 60.0k / 31.48 / 223 | - | 42.6k / 22.33 / 319 |
| 2 MiB | 5.8k / 12.20 / 2024 | 5.8k / 12.12 / 2061 | 4.6k / 9.57 / 2543 |
| 8 MiB | 1.4k / 11.95 / 7937 | - | 1.1k / 9.38 / 11333 |
| 32 MiB | 348.1 / 11.68 / 36925 | - | 316.9 / 10.63 / 37391 |

### Phase 4 - RESTART (close, reopen, GET all N)

Cells: ops/s / GB/s / p99 us / hit fraction.

| block size | crc32c | crc32c-noverify | round3 |
|---|---|---|---|
| 512 KiB | 1.3k / 0.67 / 1240 / 1.000 | - | 1.1k / 0.55 / 1524 / 1.000 |
| 2 MiB | 937.6 / 1.97 / 2102 / 1.000 | 0.0 / 0.00 / 0 / 0.000 | 686.8 / 1.44 / 2226 / 1.000 |
| 8 MiB | 333.2 / 2.80 / 4508 / 1.000 | - | 182.7 / 1.53 / 5769 / 1.000 |
| 32 MiB | 99.3 / 3.33 / 10930 / 1.000 | - | 35.0 / 1.18 / 35343 / 1.000 |

### Phase 5 - MULTI-PROCESS READ (P=4 processes, T=1)

#### view

Cells: ops/s / GB/s.

| block size | crc32c | crc32c-noverify | round3 |
|---|---|---|---|
| 512 KiB | 2.49M / 1305.78 | - | 2.49M / 1304.35 |
| 2 MiB | 755.2k / 1583.76 | - | 620.7k / 1301.69 |
| 8 MiB | 170.9k / 1433.93 | - | 127.5k / 1069.91 |
| 32 MiB | 38.1k / 1277.25 | - | 31.1k / 1043.57 |

#### copy

Cells: ops/s / GB/s.

| block size | crc32c | crc32c-noverify | round3 |
|---|---|---|---|
| 512 KiB | 68.1k / 35.72 | - | 49.5k / 25.94 |
| 2 MiB | 5.9k / 12.32 | - | 4.7k / 9.82 |
| 8 MiB | 1.4k / 12.10 | - | 1.3k / 10.54 |
| 32 MiB | 329.9 / 11.07 | - | 315.6 / 10.59 |
