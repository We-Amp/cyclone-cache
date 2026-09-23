# KV-cache storage tier under churn — workload spec (churn v1)

Purpose: measure a node-local KV-cache / prefix-cache tier in the case the v1
spec ([kv-workload-spec.md](kv-workload-spec.md)) never reaches: a store of
**bounded capacity**, **larger than RAM**, under a get-or-insert workload that
keeps it full, so something has to evict. Every harness implements exactly
this. It reuses the v1 key and value generators unchanged.

## Semantics

A real prefix cache: `get(key)`; on a hit the value is **consumed** (copy
mode: memcpy of the whole value into a preallocated per-thread staging buffer,
i.e. staging for a device transfer); on a miss the block is generated and
`put` (get-or-insert). There is no simulated compute delay: the storage tier
is what is measured.

## Data model (unchanged from v1)

- Key for index `i`: SHA-256 of the ASCII string `prefix-<i>`.
- Value for index `i`: `block_size` bytes of xorshift64* with state `i` (the
  zero state is replaced by `0x9E3779B97F4A7C15`, as in v1), little-endian.
- Metadata: the 64-byte header (digest twice). Cyclone stores it in its
  `set_header()` slot; the peers prefix it to the value.

## Sizes

- Block size 2 MiB (primary); 512 KiB as a repeat if time allows.
- Capacity `C` = 16 GiB of payload. Peers count a record as
  `64 + block_size` bytes against `C`; Cyclone's data area is sized so its
  usable bytes ≈ `C` (it pays its own ~200 B document header per record).
- Key universe `U = floor(3 × C / block_size)` (24 576 keys at 2 MiB, 98 304
  at 512 KiB), so the tier holds one third of the universe.

## Access patterns

Every thread `t` (0-based) owns its streams; nothing is shared between
threads.

- **Permutation.** Zipf ranks are mapped to key indices through one fixed
  permutation of `[0, U)`, so popular keys are not numerically adjacent:
  Fisher–Yates, `for i = U-1 down to 1: j = next() % (i + 1); swap(p[i],
  p[j])`, driven by xorshift64* with state `0x243F6A8885A308D3`. Rank `r`
  → key index `p[r]`.
- **`zipf`.** Zipf θ = 0.99 over `U` (the v1 Gray et al. / YCSB generator,
  uniform source xorshift64* with state `42 + t`), then the permutation.
- **`zipf+scan`.** Per operation a choice stream (xorshift64*, state
  `1000003 + t`, `u = (x >> 11) · 2^-53`) picks a scan operation iff
  `u < 0.1`; otherwise the next Zipf key exactly as in `zipf` (the Zipf
  stream only advances on Zipf operations). Scan operation `k` (0, 1, 2, …)
  of thread `t` uses key index `2^40 + t · 2^32 + k`: a sequential run of
  keys that were never seen before (new sessions, one-off prompts). Every
  scan operation is a miss and an insert; this is the pollution that
  scan resistance / admission control is about.

Both harnesses print the first 5 key indices (and digest prefixes) of each
stream for threads 0 and 1; they must be identical.

## Memory bound

Every store runs inside a memory cgroup with `memory.max = 4 GiB`
(RAM : tier = 1 : 4): `sudo -n systemd-run --scope -p MemoryMax=4G --uid=…
--gid=… <binary> …`. Page cache — including mmap'd file pages — is charged to
the cgroup, so the OS cannot cache more than 4 GiB of the 16 GiB tier, and
writeback is throttled per cgroup. The harness samples its cgroup's
`memory.current` every 200 ms and reports the maximum.

## Procedure, per (store, pattern, threads)

1. Fresh store (data directory emptied), `sync; echo 3 >
   /proc/sys/vm/drop_caches`.
2. **Warm-up**: run the workload, unrecorded, until `2 × C` bytes of payload
   have been inserted (summed over threads) — the store is full and has
   evicted at least one full capacity.
3. `syncfs` on the store's filesystem (untimed), snapshot the device
   counters.
4. **Measured phase: 120 s** of the workload, threads `T ∈ {1, 4}`.
5. `syncfs` again (untimed), snapshot the device counters.
6. **Reopen**: close the store, reopen it, run the first 10 000 gets of the
   `zipf` stream of thread 0 (state 42), **get only** — a miss does not
   insert — and report the hit ratio. A store whose eviction index is
   volatile must rebuild it on open (as a real deployment would); the
   rebuild time is reported.
7. Delete the store.

Harness work — generating a missed block and the content check below — is
excluded from the timed region and from the per-thread active time; ops/s is
the sum over threads of `ops_t / active_seconds_t`.

## Metrics (measured phase)

JSON line per run, with at least:

- `ops_per_s`, `hit_ratio` (hits / gets);
- `served_gb_per_s` = hits × block_size / s; `inserted_gb_per_s` = inserts ×
  block_size / s (decimal GB);
- get-hit latency p50 / p99 / p99.9 (get + copy into the staging buffer);
- miss+insert latency p50 / p99 / p99.9 (the failed get + the put, block
  generation excluded);
- `write_amp` = device bytes written (the block device holding the store,
  `/sys/dev/block/<maj:min>/stat`, including the final `syncfs`) / payload
  bytes inserted; device bytes read is reported alongside;[^null]
- on-disk footprint at the end (sum of allocated blocks under the store
  directory) and apparent file size;
- peak RSS (`getrusage`) and peak cgroup `memory.current`;
- LMDB: map size, high-water page, whether `MDB_MAP_FULL` occurred;
- LRU stores: index entries and estimated index RAM;
- Cyclone: `stats()` evictions, write-buffer wraps, writes dropped by lease,
  tag-collision evictions, entries.[^totals]

## Correctness

One hit in 64 (per thread, outside the timed region) is checked byte for
byte against the regenerated block and header. A wrong-content hit is a hard
failure: the run exits non-zero.

## Stores and tuning (printed by every run)

1. **Cyclone**: one volume, usable data area ≈ `C` (actual value printed),
   mmap directory ON, checksum ON and verified on read, default readahead,
   `max_object_size = 0`, `ram_cache_size = 0`, no per-write fsync, no
   background subsystems. Eviction is Cyclone's own FIFO-by-wrap;[^wrap] no
   application index.
2. **LMDB**: `MDB_NOSYNC | MDB_NOMETASYNC | MDB_NOTLS` (± `MDB_WRITEMAP`:
   both measured briefly, the faster is kept and printed), map size
   `1.5 × C` (on `MDB_MAP_FULL`: rerun with `2 × C`, report both). LMDB has
   no eviction, so the harness implements what a user would have to: an
   in-memory LRU (key → list node, byte count) updated on every hit and
   insert; an insert evicts least-recently-used keys with `mdb_del` in the
   SAME write transaction as the `mdb_put` until used + new ≤ `C`. The LRU is
   mutex-protected and volatile (rebuilt by a cursor scan on reopen, recency
   lost). Reads use one read transaction per get, reset/renewed per thread.
3. **filedir**: one file per block (`<hex digest>`), same in-memory LRU,
   `unlink()` to evict, write to a temporary name + `rename()` to insert,
   `preadv(header, staging buffer)` on a hit, no fsync. On reopen the LRU is
   rebuilt from a directory listing.

## Decision criteria (fixed before any run; applied afterwards unchanged)

Cyclone counts as "significantly better than LMDB" for this use only if, on
Linux at 2 MiB with the 4 GiB cgroup, for BOTH patterns at T=4: served GB/s ≥
1.5× LMDB's, OR hit-get p99 ≤ 0.5× LMDB's at no worse than 0.9× the served
GB/s. A win at T=1 only, or on one pattern only, is reported as "partial".
Hit ratio differences are reported separately and explained (FIFO vs LRU) —
Cyclone having a lower hit ratio because FIFO evicts popular blocks is a real
cost and must be stated, not normalised away. Also state qualitatively what
LMDB needed that Cyclone didn't (app-level eviction code, volatile LRU index,
fsync-off durability tradeoff) and vice versa.

---

Notes added after round 4 (the text above is unchanged from the
pre-registered spec):

[^null]: Device and cgroup counters are Linux-only. `kv_churn` emits
    `write_amp`, `device_*` and `cgroup_*` as `null` where they are not
    measured (another OS, or no cgroup v2).

[^totals]: `kv_churn` reports these two as whole-run totals,
    `cy_tag_collision_evictions_total` and `cy_entries_total` (warm-up
    included); the other `cy_*` fields are measured-phase deltas. The round-4
    JSONL files predate the rename and carry them as
    `cy_tag_collision_evictions` and `cy_entries`.

[^wrap]: Round 4 found that Cyclone's wrap eviction is worse than FIFO: a
    wrap toggles the stripe's directory phase, so the whole previous pass
    stops resolving at once (the phase flush). See
    [Round 4](../kv-cache-benchmark.md#round-4-bounded-capacity-under-churn),
    "Why: the hit ratio, and where it comes from".
