# Wrap retention: keep the previous pass readable until it is overwritten

**Status:** proposed design (phase 1). No production code has changed. The
only code in this change is the policy-replay extension in
`benchmarks/kv_churn_policy.cpp` (the `retain/N` columns).

**Scope:** the eviction behaviour of a stripe's circular data area, i.e. what
happens to directory entries when the write cursor wraps and then advances
over the previous pass. The key-to-stripe mapping, the document format
(except one header field that was never set before), the RAM tier, and the
alternate-selection plugins do not change.

Anchors below are function, type and comment-marker names, not line numbers.
They are all in `src/core/volume.{hpp,cpp}`, `src/core/directory.{hpp,cpp}`
and `src/core/mmap_directory.{hpp,cpp}` unless stated otherwise.

---

## 1. Summary

Today a wrap toggles the stripe's 1-bit directory phase. Every entry written
in the pass that just ended stops resolving at once, although nearly all of
those documents are still intact on disk, ahead of the write cursor. Each
stripe starts empty again on every wrap and holds on average about half its
capacity.

This design keeps the previous pass readable until the bytes it points at are
about to be overwritten. A per-stripe **clean frontier** `F` runs ahead of the
write cursor `W` in fixed chunks. The region `[W, F)` is the cleaned runway
the writer fills. The previous pass is readable in `[F, E)`. Moving `F` is the
only operation that exposes live bytes to the forward fill. It runs the same
reader-exclusion handshake a wrap runs today (wrap intent, borrow count, read
lease, Dekker ordering), once per chunk instead of once per pass. The stripe's
pass number is stamped into each document (`Document::write_serial`, which no
code has set so far). An entry is served only if four things agree: its
phase, its position relative to `W` and `F`, the pass stamp in its document,
and its full key and CRC. The stamp is what rules out a two-wraps-old entry
exactly, instead of by position alone.

The policy replay predicts a hit ratio of **0.815 on `zipf`** (today 0.761
replayed and 0.757 measured; plain per-stripe FIFO 0.817) and **0.685 on
`zipf+scan`** (today 0.644 replayed and 0.638 to 0.642 measured; FIFO 0.687).
That recovers about 5.4 and 4.1 points of the 8 to 9 point gap to LRU. The
rest of the gap is FIFO against LRU, which this design does not address.

---

## 2. The problem, with numbers

Round 4 of [the KV benchmark](../kv-cache-benchmark.md#round-4-bounded-capacity-under-churn)
ran a bounded tier under get-or-insert churn: 2 MiB blocks, C = 16 GiB,
16 stripes of about 511 blocks each, Zipf(0.99), key universe 3 × C.

| 2 MiB, C = 16 GiB | LRU | FIFO | FIFO per stripe | wrap flush (replay) | measured Cyclone |
|---|---:|---:|---:|---:|---:|
| `zipf` | 0.850 | 0.818 | 0.817 | 0.761 | 0.757 |
| `zipf+scan` | 0.725 | 0.687 | 0.687 | 0.644 | 0.638–0.642 |

The replay (`benchmarks/kv_churn_policy`, no I/O) matches the measured
numbers within 0.006. So the loss comes from the policy, not from the
implementation. Plain FIFO costs about 3 points against LRU. The wrap flush
costs about 6 more. The HTTP product (mod_pagespeed and the PageSpeed
optimizer, several nginx workers sharing one mmap directory) loses capacity
the same way. Its stripes are smaller: 32 MiB with the auto geometry, so
512 MiB gives 16 stripes. They wrap more often, and every wrap is a cliff for
that stripe.

---

## 3. The current mechanism

### 3.1 Write side

`Volume::commit_write` and `Volume::commit_alternate_write` take the stripe's
exclusive `mutex` and call `Volume::allocate_write_slot`. In multi-process
mode that call also takes the cross-process `write_lock` in the
`MmapDirectory` header. It holds the lock across the pwrite (F6) and releases
it in `Volume::commit_write_slot`. When the document does not fit in
`[write_pos, stripe end)`:

1. `set_wrap_intent(true)` (seq_cst).
2. `Volume::lease_permits_wrap` loads the borrow count and the lease expiry.
   If a borrow is outstanding and its lease is live, the wrap is **deferred**:
   the intent is cleared, the fill is dropped (`NoSpace`), and
   `writes_dropped_by_lease` is incremented. After `lease_wrap_ceiling` of
   continuous deferral the wrap is forced and the borrow slot is reset.
3. `Volume::evict_if_needed` calls `toggle_phase()` on the `Directory` or the
   `MmapDirectory`. This is the O(1) flush.
4. `write_pos = data_offset`, then `Volume::record_wrap` increments the wrap
   count. That count and the phase form the **wrap epoch**.
5. `set_wrap_intent(false)`. The order matters: the toggle and the epoch
   increment must be program-ordered before this store.

The Dekker proof lives at the intent-set site in `allocate_write_slot`. The
full statement is in [architecture.md](../architecture.md#lease-based-region-pinning).

### 3.2 Read side

`Volume::read_sync` works as follows. It captures `wrap_epoch()`. It runs
`Stripe::probe_each`, a seqlock probe that yields only current-phase entries
and passes each through `Stripe::is_behind_write_cursor`, so offsets at or
ahead of the cursor are rejected. Then it maps the document, checks
`is_valid()` and `first_key` against the requested key, and verifies the CRC
(or hits the CRC-validation cache). It then calls `acquire_borrow` and
`stamp_read_lease`, and finally `borrow_still_valid` (intent loaded first,
then the epoch). Chain hops (`next_alternate_offset`) go through
`is_valid_chain_offset`, which applies the same positional predicate. That is
the hop leg of invariant 9.

### 3.3 Where the reader-exclusion state lives

| State | Single-process (`Directory`) | Multi-process (`MmapDirectory::Header`, shared by every process that maps the file) |
|---|---|---|
| phase | `Directory::_current_phase` | `current_phase`, offset 16 |
| wrap count (epoch) | `Stripe::local_wrap_count` | `shared_wrap_count`, offset 40 |
| write cursor | `Stripe::write_pos` | `shared_write_pos`, offset 24 (absolute) |
| wrap intent | `Stripe::local_wrap_intent` | `wrap_intent`, offset 33 |
| borrow count | `Stripe::local_borrow_shards` (64 per-thread shards) | `stripe_borrow_slot`, offset 34 (a **single** slot, not sharded) |
| read lease | `Stripe::local_lease_expiry_ns` | `stripe_lease_expiry_ns`, offset 56 |
| force-wrap deadline | `Stripe::local_wrap_deferred_deadline_ms` | `shared_wrap_deferred_deadline_ms`, offset 36 |

A reader in another process therefore keeps its borrow and its lease in the
shared header of the stripe it reads. The writer's gate sees them there. Some
state is process-local: read anchors, the CRC-validation cache, the readahead
filter, and the `BorrowToken` a handle keeps for its release. The 64-byte
header is fully used (see the `kFixedFieldsSize` static_assert), with one
exception: `reserved` at offset 6, a `uint16_t` that `MmapDirectory::init`
zeroes and that nothing reads. `VolumeHeader` still has 24 zero-filled
`reserved` bytes.

### 3.4 Why the flush exists

The phase is one bit. An entry that survives two wraps reads as current again.
Invariant 9 rejects every entry at or ahead of the cursor because such an
entry can point at bytes that are intact now but will be overwritten in place
by the ordinary forward fill. No wrap event, lease check or epoch change
covers that fill. So today "ahead of the cursor" means "not yours". The
flush makes the whole previous pass unreachable, and that is simply the
cheapest way to satisfy that rule.

---

## 4. Proposed mechanism

### 4.1 Vocabulary

Per stripe:

- `S`, `E`: the start and end of the data area (`data_offset`,
  `offset + size`).
- `P`: the **pass number**, the existing wrap count (`shared_wrap_count` or
  `local_wrap_count`). In retention mode the phase is *derived* as
  `phase = P & 1` and stored, not toggled. A double wrap, such as the
  usurpation case noted in `allocate_write_slot`, then cannot leave the phase
  and the pass number out of step.
- `W`: the write cursor. It is unchanged, including F6: it is advanced only
  after the fill is durable.
- `N`: chunks per stripe. `Q = round_up(ceil((E - S) / N), 8)`. Proposed
  `N = 64`.
- `f`: the **frontier index**, 0..N. The frontier is `F = min(S + f·Q, E)`.
  The invariant is `W <= F`.
- **Current region** `[S, W)`: this pass. **Runway** `[W, F)`: cleaned and
  not yet written. No admissible entry points into it. **Retained region**
  `[F, E)`: the previous pass, still readable.
- **Epoch** `Σ = (P, phase, f)`. It extends today's `(wrap_count, phase)`.

A fresh stripe starts with `P = 0` and `f = N`. There is no previous pass, so
the first pass needs no advances.

### 4.2 New state

| What | Single-process | Multi-process | Format note |
|---|---|---|---|
| frontier index `f` | new `std::atomic<uint16_t>` in `Stripe`, next to `local_wrap_count` | `MmapDirectory::Header::reserved` (offset 6), renamed `retain_frontier`, accessed through `std::atomic_ref<uint16_t>` | Zero-initialized by every `init()` so far and never validated. Header stays 64 B, `MmapDirectory::kVersion` stays 1 |
| pass stamp | `Document::write_serial` = `P` mod 2^32 at allocation | same | Field exists at v8 and was always 0 |
| retention mode + `N` | `VolumeHeader` `reserved` bytes: `uint16_t retain_chunks` (0 = flush mode, today's behaviour) | same | Zero means today's semantics, so existing v8 files are flush-mode volumes |

The mode is a **property of the volume**, not of a process. A reader has to
know whether the *writer* of a stripe retains. A retaining reader facing a
flushing writer would admit entries that the writer's forward fill overwrites
without a gate. So the mode is decided when the volume is created, persisted,
and mixed into the fingerprint geohash in `fingerprint_cache_path` as a sixth
field, emitted only when retention is on. That gives two properties:

- Flush-mode filenames do not change. Users who leave the flag off take no
  cold cache.
- Processes configured differently resolve to *different files*. A retaining
  process and a flushing process (or an older v8 development build) can never
  share a ring. `open_locked` also compares the persisted `retain_chunks` as
  a backstop, like `stripe_count`.

### 4.3 Writer: allocation in retention mode

All of this runs inside `Volume::allocate_write_slot`, under the stripe
`mutex` and, in multi-process mode, the `write_lock`, exactly where the wrap
runs today. It covers both commit sites.

```
adopt shared W (as today) and shared f
if wrap_intent is set: clear it              // stale: see 4.12; nobody else can
                                             // be in the window, we hold write_lock
if doc > E - W:                              // WRAP: exposes no bytes, so no gate
    set_intent(true)
    P += 1; phase := P & 1                   // record_wrap + plain store
    W := S; f := 0                           // F = S: the whole ring is the previous pass
    set_intent(false)
need := W + doc
if need > F:                                 // ADVANCE (mandatory)
    if !advance(chunk_index_ceil(need + Q/2)): drop the fill (NoSpace), as today
elif F < E and F - need < Q/2:               // ADVANCE (early, best effort)
    advance(chunk_index_ceil(need + Q/2))    // failure is ignored: the doc fits
patch write_serial := P into the document buffer (outside the CRC)
reserve [W, W + doc)                         // F6 unchanged: W published after the fill

advance(target):
    set_intent(true)                         // seq_cst, BEFORE the gate loads
    if !lease_permits_wrap(stripe):          // same gate, same ceiling, same counters
        set_intent(false); return false      // zero side effects
    f := target                              // seq_cst store; part of the epoch
    set_intent(false)                        // AFTER the epoch store (Dekker leg)
    return true
```

A few details:

- **The wrap is no longer gated.** It overwrites nothing. It turns the
  current pass into the retained region and orphans only the remainder of
  the previous pass in `[F, E)`, which is at most about one chunk plus one
  document. Those bytes stay intact until the *next* pass's frontier reaches
  them, and that frontier move is gated. The wrap still brackets its epoch
  change with the intent flag, so a reader racing it retries instead of
  classifying against a half-published state.
- **The gate is today's `lease_permits_wrap`, unchanged.** It keeps the
  stripe-wide borrow count, the lease, the anti-starvation ceiling, the
  published force deadline, and `borrow_force_reset`. What changes is how
  often it runs: once per advance, so about N plus a few times per pass
  instead of once. See section 10 and risk 1 in section 12.
- **Early advance.** The writer keeps at least `Q/2` of runway ahead of the
  document it places. A refused early advance costs nothing. So a borrow burst
  shorter than the time needed to write `Q/2` bytes into the stripe never
  drops a write. The cost is dead capacity of about one chunk on average
  (about 1.6 % at `N = 64`). The replay includes it.
- **The stamp** is written into the already-built document buffer after the
  allocation has fixed `P`. This is the same pattern the wrap-frontier link
  refusal uses when it zeroes `next_alternate_offset` in the buffer.
  `write_serial` is outside the CRC (the CRC covers header data and content
  only), so nothing is recomputed.
- **No directory mutation.** The advance does not scan or clear entries. Stale
  entries are made inadmissible by the predicate in 4.4 and reclaimed lazily
  by `insert` (4.7). Section 8 covers the eager-clean variant and why it is
  not the default.

### 4.4 The validity predicate: what makes an entry valid

A reader first loads the snapshot `Σ = (P, φ, f)`, which gives `F_Σ`. It
loads `W` fresh from `current_write_cursor()`. A tag-matched directory entry
`e` for key `K` is **admitted** as

| class | phase leg | position leg (against Σ, before mapping) | stamp leg (document header) |
|---|---|---|---|
| current | `e.phase == φ` | `S <= e.off < W` | `write_serial == P` |
| retained | `e.phase != φ` | `F_Σ <= e.off < E` | `write_serial == P - 1` (mod 2^32) |

Anything else is rejected before the region is mapped: runway offsets, a
current-phase entry at or ahead of `W`, a previous-phase entry behind `F_Σ`.
After admission the existing gauntlet runs unchanged: header `is_valid()` and
length bounds, **full 256-bit `first_key == K`** (invariant 8), CRC or the
CRC-validation cache, `acquire_borrow`, `stamp_read_lease`, then
**revalidation**: `wrap_intent == 0` loaded first, then `epoch() == Σ` over all
three fields.

In flush mode the retained row is disabled and the stamp is not checked. The
predicate then reduces exactly to today's.

Each leg has one job:

- **Position against Σ** keeps readers out of the runway, the only place the
  forward fill writes. It must use the frontier *from the snapshot*, not a
  fresh load. If `F` moves after Σ was taken, the revalidation fails because
  `f` is part of the epoch. If it moved before, the snapshot already holds
  the new value. A stale-low `W` only widens rejection, as today.
- **Phase** selects which stamp to expect and splits the two classes cheaply
  before any mapping.
- **Stamp** is what makes "one pass old" exact. It rejects two-wraps-old
  survivors whose bytes are still intact (the trailing-gap case), which
  position and phase alone cannot do once the retained region reaches ahead
  of the cursor.
- **Full key and CRC** are unchanged. They reject aliasing into reused space
  and torn or unsynced bytes.
- **Epoch revalidation** is unchanged in shape, with more fields. It is what
  makes an admitted borrow safe against *future* frontier moves (5.3).

### 4.5 Chain hops and alternate links

A hop follows `next_alternate_offset` from a node `s` whose class is known to
a target `t`. The rule replaces the positional check in
`is_valid_chain_offset`, which today is "t is behind W":

| source class | admitted target | target class |
|---|---|---|
| current | `t < s` | current (same pass, older) |
| current | `t > s` and `t >= F_Σ` | retained (a cross-pass link) |
| retained | `F_Σ <= t < s` | retained |

Every other target is rejected, including any upward hop from a retained node
and any upward hop into `[s, W)`. Then the stamp, key, CRC and borrow legs
run exactly as for the directory probe.

This is sound because documents in one pass are laid out in write order. A
link written when `s` was committed points either down into the same pass or
up into the then-retained region. An upward target is live only while the
frontier has not reached it, and `t >= F_Σ` checks exactly that. Once `s`
itself is retained, anything above it belongs to a pass at least two back and
has been overwritten. Anything below it is live only while it is still ahead
of the frontier.

The **wrap-frontier link refusal** in `commit_alternate_write` compares
`wrap_epoch` before and after the allocation. In retention mode that would
also fire on every advance. It is replaced by an exact liveness check under
the lock after the allocation. The planned link `t` may be stamped only if
`t >= F_new`, or if no wrap happened and `t < write_offset`. Otherwise the
field is zeroed, as today. Chains can therefore span one pass boundary. An
alternate generated in the background for a retained Original keeps the
Original reachable until the frontier reaches it. Splices through
`repoint_chain_link` keep the downward or upward relation of the link they
replace, so they stay admissible under the same table.

### 4.6 Uniqueness: one resolvable entry per key per stripe

Retention makes the previous pass visible, so a key can now have a retained
entry when it is written again. Two rules keep "at most one admissible entry
per key per stripe":

1. The **full-key election** in both commit paths (`stripe->probe_each` plus
   `map_document` plus the `first_key` compare) must see retained entries too.
   It uses the same predicate and the same stamp check. A rewrite of `K` then
   updates `K`'s retained entry in place (new offset, current phase) instead
   of adding a second one.
2. The election already maps every same-tag candidate. While doing so it
   records any *other* same-key entry and any inadmissible one, and `insert`
   clears them inside the same seqlock bracket. Same-tag candidates are rare,
   so this costs nothing measurable.

Without rule 2, a key that lands at exactly the offset of its own stale entry
could leave two entries. The first one the reader meets might be the older
version. With fixed-size KV blocks every pass lays documents out at the same
offsets, so this is not negligible. Today the same case is avoided only
because `insert` prefers reusing a stale slot in the key's own bucket.

### 4.7 Directory `insert` victim order

`Directory::insert` and `MmapDirectory::insert` take the retention boundaries
(`W`, `F`, `φ`) as extra parameters. The victim order becomes:

1. the verified same-key entry (in-place update, unchanged)
2. an empty slot
3. an **inadmissible** slot: runway, previous phase behind `F`, current phase
   at or ahead of `W`. This replaces today's "stale" (previous-phase), which
   is now live.
4. a tag collider (unchanged, counted)
5. the **oldest admissible** entry. That is the retained entry nearest `F`
   (lowest offset at or above `F`), and only if there is none, the current
   entry with the lowest offset. Today's "nearest to clobber" means the same
   thing, and the new boundaries make it exact rather than approximate.

Retention roughly **doubles directory occupancy**, because about a full
stripe of documents resolves instead of about half. See section 10 for when
that matters.

### 4.8 Other paths

- `exists_sync` stays a directory-only probe. It applies the phase and
  position legs but not the stamp. So, like today's tag collisions, it can
  return a false "yes" for a stamp-dead entry. Its contract is already
  probabilistic.
- `remove_sync`, `remove_alternate_sync`, `update_hit_count_sync` and
  `commit_header_rmw` verify their target with the same helper, including the
  stamp. They already recheck `wrap_epoch` under the lock before the pwrite.
  With `f` in the epoch that recheck also covers an advance, which runs under
  the same locks.
- `renew_read_lease` and `renew_read_lease_strict` compare the three-field
  epoch. Their premise comment ("any overwrite of a borrowed region is a
  wrap") becomes "any overwrite of a borrowed region is preceded by a gated
  epoch change (a wrap or a frontier advance)".
- The RAM tier and `cross_process_ram_coherence` do not change. An advance
  changes no `DirEntry`, so no bucket version moves. The same reasoning as for
  the phase toggle in [multi-process.md](../multi-process.md#cross-process-ram-coherence)
  applies: a RAM copy of a document the ring later evicts is still correct
  content.

### 4.9 Multi-process

- **Ownership (invariant 7).** Only the owner writes, so only the owner moves
  `f`, under the `write_lock`, inside `allocate_write_slot`. Any process that
  writes the stripe adopts the shared `f` together with `shared_write_pos`,
  under the lock. That covers a process-count change, a restart, or the
  forked-writers layout that `test_multiprocess_writers.cpp` covers.
- **Readers in other processes** load `P`, `phase` and `f` from the shared
  header for Σ and `shared_write_pos` for `W`. They register their borrow in
  `stripe_borrow_slot` and stamp `stripe_lease_expiry_ns`, both in the same
  header, which the owner's gate reads. This is exactly how cross-process
  wraps are gated today (`test_multiprocess_writers.cpp`, "A borrow held in
  one process defers another process's wrap").
- **Cache lines.** `retain_frontier` (offset 6) sits in the same 64-byte
  header line as the phase, the wrap count and the intent. The reader's
  snapshot is one line, as today.

### 4.10 In-memory `Directory` compared with `MmapDirectory`

The protocol is the same in both. The differences are only where the state
lives (table 4.2) and its lifetime. A single-process volume rebuilds an empty
`Directory` and restarts at `W = S` on every open, so it retains nothing
across a restart. It gains exactly what the replay shows while it runs. The
local gate sums the 64 borrow shards (`Stripe::local_borrow_shards`). The
mmap gate reads the single shared slot. Both are unchanged.

### 4.11 Document sizes, and why there is no aggregation buffer

Cyclone has no ATS-style aggregation buffer. `WriteBuffer` is a per-handle
staging buffer, and every commit is one pwrite of one document. So an advance
happens per document, not per aggregation flush, and a document larger than
the runway advances by as many chunks as it needs in **one** gated step.
`chunk_index_ceil(need + Q/2)` can jump from `f` to any index up to `N`.
Consequences:

- A 32 MiB document in a large stripe evicts the retained documents under its
  span and nothing else.
- A document that does not fit in `[W, E)` wraps first, then advances from
  `S`. That matches today, where it lands at `S`.
- A document larger than the data area is still `NoSpace`, and one over
  `max_object_size` is still `ObjectTooLarge`.
- In small stripes, where the default auto geometry gives about 32 MiB, a
  2 MiB document needs 4 or 5 chunks per write. So nearly every KV write
  advances. That costs the gate (a handful of loads and two stores) per write
  and a higher epoch-change rate for readers. Open question 2 asks whether
  `Q` should have a floor.

### 4.12 Crash, restart and power loss

**Process crash (no power loss).** The mmap directory and the data share the
page cache, so a crash leaves exactly the state at the instruction where it
happened. The possible stopping points:

| crash point | state left | outcome |
|---|---|---|
| inside the advance window, `wrap_intent = 1` | intent stuck at 1, `f` old | Readers of the stripe fail revalidation until the flag clears. The next writer that takes the `write_lock` clears it, because only a `write_lock` holder ever sets it, so an observed 1 under the lock is stale. **This is a pre-existing gap**: today a crash inside the wrap window leaves the flag set until the stripe's *next wrap*, a whole pass of misses. This design fixes it on the first allocation. |
| after the `f` store, before the intent clear | new `f`, intent stuck | as above |
| after the advance, mid-pwrite | torn bytes in the runway, `W` not advanced (F6) | No admissible entry points into the runway. The next writer reuses the span. |
| after the pwrite, before the insert | a document in `[W_old, W_new)` with no entry | Unreachable, overwritten next pass. Same as today. |

**Power loss.** With `sync_on_write = false`, the default, the
`DirectorySyncer` fsyncs data and then msyncs the directory every
`directory_sync_interval`. Between syncs, data pages, directory pages and the
header page can each be durable independently. On reboot the header (`P`,
phase, `W`, `f`) may be older or newer than the data. The legs cover every
combination:

- Data written after the persisted `F` (the header was lost, the data made
  it). Previous-phase entries there point at new documents whose stamp is
  `P`, or at the middle of one. The stamp or the magic rejects them.
- Data written after the persisted `W`. Current-phase entries at or ahead of
  `W` are rejected by position, as today.
- The header was lost across a whole wrap. Entries of the "previous" pass are
  then two passes old relative to the documents that overwrote them. The
  stamp rejects them.
- An inconsistent header (`W > F`, `f > N`, `W` misaligned). `init_stripes`
  already sanitizes `W`. It also sets `f := N`, meaning no retained region for
  that pass: a miss spike, never a wrong serve.

With `sync_on_write = true` there is a new ordering rule that mirrors
invariant 2. The header page holding the new `f` is msynced **before** the
first pwrite into the new runway, so a persisted entry never points into
bytes that a persisted fill overwrote. It is one msync per advance, amortized
over `Q` bytes.

### 4.13 On-disk format

- There is **no change to the format major.** v8 is unreleased. The design
  uses bytes v8 already zeroes and never reads: `MmapDirectory::Header::reserved`,
  `VolumeHeader::reserved`, and `Document::write_serial`. Zero in each means
  today's behaviour. A v8 volume created before this lands is a flush-mode
  volume and stays one.
- The mode is isolated by filename (the fingerprint), not by version. So the
  change needs no further bump as long as it ships in the same release as the
  CRC-32C bump (v8). If it slips past a v8 release, it still needs no bump:
  old binaries never resolve a retention volume's filename. It does need the
  `open_locked` backstop, because an operator can hand-name a file. See the
  known limitation in `fingerprint_cache_path`.

---

## 5. Correctness argument

### 5.1 Invariant by invariant

1. **Readers take no stripe lock.** New read-side work is loads only: `f` in
   the snapshot, `W` as today, and a 4-byte compare of `write_serial` in a
   header the path already maps. There is no new lock and no new RMW.
   `read_sync` keeps the "Lock-free read" shape.
2. **Commit ordering.** For inserts it is unchanged: data durable before the
   directory insert, and the stamp travels in the same pwrite as the
   document. There is a new counterpart for advances (4.12): with
   `sync_on_write`, `f` is durable before the runway is written. Without it,
   the stamp, position, key and CRC legs downgrade a reordered persist to a
   miss, which is the same contract as today.
3. **Dekker handshake.** The shape is unchanged: the writer stores the intent
   and then loads the borrow count and the lease; the reader stamps the
   borrow and the lease and then loads the intent and then the epoch; all
   seq_cst. What changes is the set of events that run it (every frontier
   advance, plus each wrap) and the epoch, which is now `(P, phase, f)`, with
   every epoch store program-ordered before the intent clear. The proof at
   the intent-set site in `allocate_write_slot` carries over by replacing
   "wrap-count publish" with "epoch publish". Borrows are still released on
   handle close.
4. **Per-bucket seqlock.** The advance does not mutate the directory. `insert`
   changes only which slot it picks. Every mutation is still odd→even under
   the stripe mutex, with `acquire_writer`/`release_writer` in the mmap case.
5. **HitTracker is a leaf lock.** It is not touched. The advance runs inside
   `allocate_write_slot` and never calls `record_hit()`.
6. **Per-thread sharding.** There is no new shared RMW line on the read path.
   The frontier is read from a line the reader already loads.
7. **Multi-process ownership.** Only a `write_lock` holder moves `f`, and it
   is always the owner. Non-owners read it (4.9). Per-write fsync stays
   opt-in.
8. **Full-key re-verification.** It is kept on both classes and at every
   election. It is extended by uniqueness (4.6) and by the stamp, which
   closes the one alias the key check cannot see: the key's own older
   document at a reused offset.
9. **Phase-ABA positional guard at both choke points.** It is generalized,
   not removed. The probe leg rejects runway offsets, current-phase entries at
   or ahead of `W`, and previous-phase entries behind `F_Σ`. The hop leg uses
   the direction table in 4.5. The guard's purpose, "no borrow can alias
   bytes the forward fill will overwrite without a gate", now holds because
   the forward fill writes only into `[W, F)` and every move of `F` is gated
   (5.3). It no longer holds because everything ahead of `W` is off limits.

### 5.2 A stale entry from two or more passes back is never served

Let `e` be a directory entry for key `K` at offset `o`, with phase bit `b`,
created in pass `Q <= P - 2`. Look at what is at `o` when a reader admits `e`:

- **The original document `D` is intact.** This is the trailing-gap survivor.
  Its stamp is `Q`, and `Q` is neither `P` nor `P - 1`. The stamp leg rejects
  it whatever the phase or position legs say.
- **A newer document `D'` starts at `o`**, written in pass `R` in `{P - 1, P}`.
  The stamp is `R`. Admission needs the class implied by `b` to expect `R`.
  If it does not, `e` is rejected. If it does, the key leg compares
  `D'.first_key` with `K`. A different key is rejected. The same key means
  `D'` is a genuine document of `K` from the live window, and uniqueness
  (4.6) means `e` is the one entry that resolves `K`. So a stale entry can
  only ever resolve to `K`'s live document, never to an older one.
- **`o` falls inside a document or in padding.** The magic and length checks
  reject it, then the key, then the CRC. An attacker who can store content
  could forge a document header inside a payload. That is a **pre-existing**
  exposure of any aliasing entry, not something this design introduces (open
  question 7).
- **`D` was written in pass `P - 2^32`.** The 32-bit stamp would alias. That
  needs an entry to survive 4.29 × 10^9 wraps of one stripe untouched, which
  is about 13 years at 10 wraps/s on a single stripe. We accept it.

So every document the predicate admits is a genuine, intact, CRC-verified
document of `K`, written in pass `P` or `P - 1`.

### 5.3 The forward fill never tears an admitted borrow

**Claim.** While a borrow `B` of document `D = [o, o+len)` is outstanding and
its lease is live, no pwrite overwrites any byte of `D`. The exception is a
ceiling-forced advance, which is unchanged from today and documented.

**Proof.** Pwrites land only in `[W, W + doc)`, which is inside the runway
`[W, F)` as it stands when the slot is allocated. The writer holds the stripe
mutex (and the `write_lock`) from allocation to commit, so no advance or wrap
interleaves with one fill. `B` was admitted with snapshot `Σ` and revalidated
with `intent == 0` and `epoch == Σ`.

- **Current class** (`o + len <= W`): these bytes are in `[S, W)`. They can be
  rewritten only after a wrap (so `P` changes) and then an advance over them.
- **Retained class** (`o >= F_Σ`): the runway at revalidation time is
  `[W, F_Σ)`, because the epoch is unchanged, so `f` is unchanged. It is
  disjoint from `D`. Any later write into `D` needs `F` to move past `o` first.

In both cases some gated epoch change must come first. That change runs
`set_intent(true)` and then the gate loads, and `B` runs its stamp and then
`intent`/`epoch` loads. By the existing Dekker argument, either the gate
observes `B`'s borrow and defers, or `B`'s revalidation observes the intent or
the moved epoch. The second case contradicts `B` being admitted. Once `B` is
admitted its count stays in the slot until the handle closes, so every later
gate sees it. ∎

Two notes on the proof:

- The **wrap** is not gated, and that is safe. It writes no bytes, and it
  changes the epoch inside an intent window. A borrow that raced it retries.
  A borrow that was already admitted keeps its intact bytes until the next
  gated advance reaches them.
- **Pwrites in flight** when Σ was taken target a runway allocated no later
  than Σ in the same pass. The frontier only grows within a pass, so that
  runway is below `F_Σ`. Across a wrap the stripe mutex serializes them. The
  escalated-takeover residual in `commit_write_slot` is unchanged and still
  detectable only.

---

## 6. Failure modes considered

| # | Failure mode | Handled by |
|---|---|---|
| F1 | A reader classifies with a stale `F` and borrows bytes the writer is about to fill | Position uses `F_Σ` from the snapshot; `f` is in the epoch; revalidation (5.3) |
| F2 | A two-wraps-old entry with intact bytes (trailing gap) | Stamp (5.2) |
| F3 | A same key at the same offset leaves duplicate entries, and the older version is served | Election plus dedupe (4.6) |
| F4 | A chain pointer into a region the frontier has passed | Direction table (4.5); upward hops from retained nodes are rejected |
| F5 | A link from a new head to a retained node that is cleaned before commit | Liveness check under the lock replaces the epoch-based link refusal (4.5) |
| F6 | A writer crashes with the intent set | Cleared by the next `write_lock` holder (4.12); also fixes the pre-existing wrap-window gap |
| F7 | Header, directory and data persist out of order at power loss | Stamp, position, key and CRC; `f := N` sanitize; `sync_on_write` ordering (4.12) |
| F8 | A retaining reader and a flushing writer on one ring | Mode is per volume and fingerprinted; `open_locked` backstop (4.2) |
| F9 | Phase and pass count diverge after a double wrap (usurpation) | `phase := P & 1` is derived, not toggled (4.1) |
| F10 | A long-held zero-copy borrow blocks advances, and writes drop sooner than today | Early advance; ceiling unchanged; counters; region-scoped borrows as follow-up (section 12, open question 4) |
| F11 | Directory pressure doubles and bucket-full evictions eat the gain | Victim order (4.7); `bucket_full_evictions` telemetry; sizing (section 10) |
| F12 | The CRC-validation cache holds a verdict for bytes the fill has since replaced | As today: every document the directory can reach was fully written before its entry was published, and old entries into the runway are inadmissible (5.3). The same argument as `test_wrap_phase_aba.cpp` case (B) |
| F13 | A forced advance under the ceiling | Same semantics and counters as a forced wrap; holders see `kTorn` on renew |

---

## 7. How ATS does it, and where we deliberately differ

This is based on our reading of the ATS `iocore/cache` sources (9.x/10.x).
Check the exact call sites before citing this section elsewhere.

- **Validity is positional.** An ATS directory entry also carries one phase
  bit. `dir_valid()` accepts an in-phase entry only if it lies behind the
  write position (plus the pending aggregation buffer). It accepts an
  out-of-phase entry only if it lies ahead of the region the next
  aggregation write will cover. So ATS keeps the previous pass readable until
  the write head reaches it. That is the behaviour this design adopts.
- **Cleaning is bulk, and eager where it matters.** Entries that became
  invalid are swept out of the directory in bulk (`dir_clean_vol()` and
  `dir_clean_segment()` when the stripe wraps, `dir_clean_range_interval()`
  for a range). On recovery ATS clears the range written since the last
  directory sync (`dir_clear_range()`), using the documents' `sync_serial` and
  `write_serial` to tell what was written after the checkpoint.
- **Evacuation.** Before the aggregation write overwrites a region, ATS copies
  selected documents forward (`evac_range()` and evacuation blocks). These are
  pinned documents, and documents with an open reader, which register in the
  evacuation block. ATS moves the *document* out of the writer's way. It does
  not hold the writer back.
- **Readers copy.** ATS looks up the directory under the stripe mutex, reads
  the document into an IOBuffer, and re-checks the document key after the
  read. A reader never aliases live disk bytes, so ATS needs no borrow or
  lease protocol.

Where Cyclone differs:

1. **Zero-copy borrows gate the frontier instead of evacuating.** Cyclone
   hands out views into the live mapping, so a document under a borrow cannot
   be moved. It has to be protected in place. Copying forward (evacuation) is
   a possible second step for hot retained documents, at a cost in write
   amplification (open question 6).
2. **Lock-free, multi-process readers.** ATS readers serialize on the stripe
   mutex. Cyclone readers take no lock and may live in another process. So
   validity is evaluated against an epoch snapshot and closed with the Dekker
   revalidation, instead of under a lock.
3. **No aggregation buffer.** The frontier moves in fixed chunks, and
   early, instead of per aggregation write.
4. **The pass stamp is a read-time check.** ATS uses serials for recovery.
   Here `write_serial` is compared on every read, which removes the need for
   an eager sweep to kill phase-ABA survivors.
5. **No eager directory sweep in v1.** Position plus stamp make stale entries
   inadmissible, and `insert` reclaims them. Section 8 has the measured cost
   of sweeping.

---

## 8. Alternatives considered

- **Eager clean-ahead: scan the directory and clear the entries in
  `[F, F')` at each advance.** This is the literal ATS analogue. A full scan
  of one stripe's 65 536 entries measured **about 130 µs** on an Apple M5
  (Release build, 44 536 occupied entries, through the out-of-line `DirEntry`
  accessors). At 4 KB documents and `Q = 512 KiB` (a 32 MiB stripe with
  `N = 64`) that is about 1 µs per write, roughly 10 % of today's 10.1 µs
  write p50. It also moves bucket versions, which spuriously invalidates peer
  RAM copies under `cross_process_ram_coherence`. It is not needed for
  correctness (5.2, 5.3). A **binned** variant is kept in reserve in case
  directory hygiene turns out to matter. It builds per-chunk lists of
  `(bucket, slot)` once per pass, or incrementally on insert, and clears only
  those entries, so it costs O(entries cleaned).
- **Widen the phase to 2 bits (use `DirEntry`'s reserved bit).** That pushes
  the ABA out to four wraps, but it still needs the stamp or a sweep. The
  stamp is free because the header is read anyway, so the wider phase buys
  nothing.
- **No stamp, sweep at every wrap (ATS style).** This is correct but costs an
  O(directory) spike under the lock on every wrap, plus bucket-version churn.
  The stamp does the same job for a 4-byte compare.
- **Evacuate hot retained documents (a second chance).** This would move
  Cyclone from FIFO toward CLOCK and could close part of the remaining 3-point
  gap to LRU. It is out of scope here (open question 6).
- **Region-scoped borrow counts (one counter per chunk).** This lets an
  advance defer only for borrows inside the chunk it is about to expose. It
  is the fix for risk 1. It needs header space the mmap layout does not have,
  so it would be a v8-internal layout change (open question 4).

---

## 9. Expected hit-ratio effect (policy replay)

`kv_churn_policy` now has `retain/N` columns: per stripe, a frontier of `N`
chunks with the early advance described in 4.3. Commands:

```bash
./build/kv_churn_policy                         # 2 MiB, C = 16 GiB (round 4)
./build/kv_churn_policy 2097152 2147483648      # 2 MiB, C = 2 GiB
./build/kv_churn_policy 524288                  # 512 KiB, C = 16 GiB
./build/kv_churn_policy 4096 536870912          # 4 KB, C = 512 MiB (HTTP-shaped)
```

The existing columns reproduce `doc/kv-cache-benchmark/churn/policy-replay.txt`
digit for digit.

| workload | LRU | stripe FIFO | wrap flush (today) | retain/16 | **retain/64** | retain/256 |
|---|---:|---:|---:|---:|---:|---:|
| `zipf`, 2 MiB, 16 GiB | 0.8498 | 0.8174 | 0.7608 | 0.8086 | **0.8150** | 0.8166 |
| `zipf+scan`, 2 MiB, 16 GiB | 0.7251 | 0.6865 | 0.6437 | 0.6802 | **0.6848** | 0.6860 |
| `zipf`, 2 MiB, 2 GiB (63 blocks/stripe) | 0.8154 | 0.7754 | 0.7058 | 0.7629 | **0.7724** | 0.7724 |
| `zipf+scan`, 2 MiB, 2 GiB | 0.6935 | 0.6463 | 0.5929 | 0.6371 | **0.6444** | 0.6444 |
| `zipf`, 512 KiB, 16 GiB | 0.8667 | 0.8383 | 0.7887 | 0.8309 | **0.8366** | 0.8379 |
| `zipf+scan`, 512 KiB, 16 GiB | 0.7405 | 0.7052 | 0.6678 | 0.7002 | **0.7040** | 0.7049 |
| `zipf`, 4 KB, 512 MiB | 0.8756 | 0.8476 | 0.7758 | 0.8424 | **0.8470** | 0.8475 |
| `zipf+scan`, 4 KB, 512 MiB | 0.7507 | 0.7151 | 0.6810 | 0.7099 | **0.7138** | 0.7148 |

What the table says:

- At `N = 64` retention recovers 90 to 97 % of the flush cost on every
  workload. It lands within 0.003 of per-stripe FIFO, and the residual is the
  roughly one chunk of dead runway. `N = 16` leaves about 0.6 points on the
  table. `N = 256` buys less than 0.2 points over 64 while running the gate
  four times as often.
- **Predicted round-4 result:** `zipf` 0.815 (from 0.757), `zipf+scan` 0.685
  (from 0.638 to 0.642). The replay tracked the measured value within 0.006
  before, so we expect measured values of about 0.81 and 0.68.
- **Served throughput (a model, not a measurement):** weighting round 4's
  T=1 per-operation latencies (hit 191 µs, miss+insert 4 064 µs) by the new
  hit ratio gives about 1.34× the hits per second at T=1. That is roughly
  0.84× LMDB instead of 0.62×. The FIFO-versus-LRU gap remains.
- The HTTP-shaped row (4 KB documents, 16 stripes of 32 MiB) gains 7.1
  points. It assumes the directory holds every live document: about 7 700
  per stripe against 65 536 entries, so it does (section 10).

---

## 10. Performance

**Read path.** One extra load of `f` in the same header line (mmap) or the
same stripe-local line (in-memory). One 4-byte compare of `write_serial`.
Revalidation compares three fields instead of two. No new RMW and no new
shared line (invariants 1 and 6). The effect should be in the noise of the
warm-read p50 of 0.33 µs. The new cost is **spurious retries**: every advance
changes the epoch, so a reader whose probe-to-revalidate window straddles an
advance retries. For the round-4 KV load (about 28 MB/s inserted per stripe,
`Q` = 16 MiB, so about 1.7 advances/s per stripe), a cold 2 MiB read with a
5 ms window retries about 1 % of the time. With the default one retry, a
false miss happens about 10^-4 of the time. A class-aware revalidation, where
a current-class borrow ignores intent and `f` changes that come from an
advance, removes most of these retries. It is a follow-up.

**Write path.** For each write, one compare of `W + doc` against `F`. For
each advance: the intent store, the gate (lease plus borrow-count loads; the
in-memory gate sums 64 shards), the `f` store and the intent clear. That is
about 1 to 2 µs at most, amortized over `Q` bytes. For 4 KB documents and
`Q` = 512 KiB it is one advance per about 125 writes, under 0.02 µs per
write. The wrap stays O(1) and loses its gate. There is no directory scan
anywhere (section 8 has the cost of one).

**HTTP small objects (`performance_baseline`, 4 KB).** The documented run
(`--cache-size 512 --entries 5000 --content-size 4096`) writes 20 MB into
512 MiB and **never wraps**. A fresh stripe starts at `f = N`, so that run
exercises no advance at all. Only the read-path loads change. Acceptance:
write p50 and p99 and warm-read p50 within run-to-run noise (±3 %) of the
current baseline table. A churning variant (for example
`--cache-size 64 --entries 50000`) must be added to cover the advance path,
and must report `frontier_advances` and `advances_deferred_by_lease`.

**Directory sizing.** Retention roughly doubles the entries that resolve.
Each stripe has 65 536 entries in buckets of 4. That is ample for KV blocks
(511 per stripe) and for 4 KB documents in 32 MiB stripes (about 7 700 per
stripe). With the auto geometry on large volumes, for example 16 GiB giving
1 GiB stripes, 4 KB documents number about 250 000 per stripe. There the
directory, not the ring, bounds capacity **today already**. Retention cannot
retain what the directory cannot hold, and bucket-full evictions will rise
(risk 3, open question 8).

---

## 11. Test plan

### 11.1 Existing guards that must stay green, in both modes

The test suite runs twice: as today, and with retention forced on through a
test-only default. Expected results are identical except for the tests in
11.2.

| Invariant | Guard |
|---|---|
| 1 | `tests/integration/test_lockfree_read_races.cpp` |
| 2 | `tests/integration/test_power_loss.cpp` |
| 3 | `tests/integration/test_lease_pinning.cpp` (every case; in retention mode "wrap pressure" becomes "advance pressure") |
| 4 | `tests/unit/test_directory.cpp`, `tests/unit/test_mmap_directory.cpp` |
| 5 | `tests/unit/test_hit_tracker.cpp` |
| 6 | `concurrent_read_bench` scaling rows (no regression) |
| 7 | `tests/unit/multi_process_test.cpp`, `tests/integration/test_multiprocess_writers.cpp` |
| 8 | `tests/integration/test_wrap_phase_aba.cpp` (A), (B); `tests/integration/test_tag_collision.cpp` |
| 9 | `tests/integration/test_wrap_phase_aba.cpp` (C) to (F), F6-A to F6-F |
| other | `test_alternate_chain_bound.cpp`, `test_header_rmw_races.cpp`, `test_ram_coherence_cross_process.cpp`, `test_wrap_telemetry.cpp`, `test_fingerprint_filenames.cpp` |

### 11.2 Expectations that change in retention mode (parameterize, don't delete)

- `test_eviction.cpp` "Stale entries invisible after wraparound". In
  retention mode, previous-pass entries stay visible until the frontier
  passes them.
- `test_eviction.cpp` "Insert succeeds after phase-based eviction frees stale
  slots". Slots are reclaimed as inadmissible (4.7), not as stale.
- `test_wrap_phase_aba.cpp` (C) and (D). The trailing-gap survivor is
  rejected by the **stamp** rather than by position, and the test asserts
  which leg fired.
- `test_wrap_phase_aba.cpp` (E) and "An alternate write whose allocation
  wraps starts a fresh chain". A link to a still-retained old head is now
  *kept*. A link to a node behind the new frontier is still refused.

### 11.3 New tests (`tests/integration/test_wrap_retention.cpp`, tag `[retention]`)

1. **Deterministic retention.** One stripe with small fixed documents. Fill
   one pass, wrap, and write `k` documents. Assert that every previous-pass
   key at or above `F` hits, every key in `[S, F)` misses, and each advance
   evicts exactly the keys in the chunk it crossed. Rewrite a retained key
   and assert the new version is served and only one entry resolves
   (uniqueness).
2. **Reader against forward fill under TSan.** N reader threads take
   zero-copy borrows of retained documents near `F` and verify content
   derived from the key after `renew_read_lease_strict`. M writer threads
   force advances. Add seams in the style of `s_write_tear_gate_for_test` that
   freeze the writer (a) between `set_intent` and the gate, (b) between the
   `f` store and the intent clear, and (c) inside the runway pwrite. Each
   Dekker leg is then hit deterministically. Assert no torn content, borrows
   either valid or reported `kTorn`/`kCopyNow`, and a TSan-clean run with
   `tools/tsan_suppressions.txt`.
3. **Two-wrap ABA.** Build, as case (A) and (C) do, an entry that survives
   two wraps in a cold bucket, in three variants: intact trailing-gap bytes
   with stamp `P-2`, which must miss; offset reused by another key, which
   must miss; offset reused by the *same* key, which must serve the new
   version with no duplicate left behind.
4. **Multi-process.** In forked processes: (a) a reader borrows a retained
   document and holds it, the owner's advance is deferred, and it proceeds
   after the close; (b) a non-owner reader classifies with the shared `f`;
   (c) the writer is killed at each seam from test 2 and the next writer
   clears the stale intent and resumes; (d) ownership moves between processes
   and the new owner adopts `f`.
5. **Reopen and power loss.** Reopen a wrapped retention mmap volume.
   Retained and current entries serve, and runway entries miss. Corrupt the
   header (`W > F`, `f > N`), which must sanitize to "no retained region"
   with no wrong serve. With `sync_on_write`, check the msync-before-fill
   ordering through a seam.
6. **Chains across the pass boundary.** A head in the current pass with a
   retained tail enumerates both until the frontier passes the tail, then
   truncates. An upward hop from a retained node is rejected. A stale
   pointer below `F` is rejected before any map.
7. **Victim order.** `Directory` and `MmapDirectory` unit tests for the order
   in 4.7, including "oldest admissible" when the bucket is full.
8. **Fingerprint.** Retention on and off resolve to different filenames, and
   the flush-mode filename is byte-identical to today's.

### 11.4 Benchmarks as acceptance

- `kv_churn` round-4 configuration (2 MiB, T=1 and T=4, both patterns):
  measured hit ratio at least 0.80 on `zipf` and at least 0.67 on
  `zipf+scan`; `writes_dropped_by_lease` at most 10× today's 0 to 3 per run.
- `performance_baseline` as in section 10, plus the churning variant.
- `concurrent_read_bench 20000 512 2 0 512 ramoff`: the 1/4/16-thread rows
  stay within noise.
- Sanitizers: the full suite under ASan+UBSan and TSan in both modes.
- Downstream: `bazel test //test/lib/cache:cache_burst_test` in the
  PageSpeed consumer (cross-process stress) with retention on.

---

## 12. Rollout, risks and telemetry

**Flag.** `CacheConfig::wrap_retention` (bool) and
`CycloneCacheConfig::wrap_retention` in the C API. It is applied when a volume
is **created** and persisted as `retain_chunks`. It is part of the filename
fingerprint, so flipping it means a new file and a cold cache, never an
in-place conversion.

**Default.** Off in the first release that ships it. Flip it to on after:

1. `kv_churn` meets 11.4;
2. a PageSpeed soak with retention on shows `advances_deferred_by_lease` and
   `writes_dropped_by_lease` within budget under real zero-copy send
   durations;
3. both sanitizer runs are clean.

v8 already forces a cold cache on upgrade. So if the maintainers want it on
from the start, turning it on *in the same release as v8* costs users no
extra cold start (open question 1).

**New counters** in `VolumeStats`/`CacheStats` and the C API:
`frontier_advances`, `advances_deferred_by_lease` (a subset of the existing
`wraps_deferred_by_lease`, or a sibling of it), `retained_hits` (hits served
from the previous pass: the direct measure of the benefit), and
`stamp_rejections`.

**The three biggest risks**

1. **The gate runs about 64× as often.** A long-held zero-copy borrow, such
   as a large body sent to a slow client through the PageSpeed nginx module,
   blocks the stripe's writer once the runway (`Q/2` to `3Q/2`) is used,
   instead of once the pass is full. The borrow count is stripe-wide, not
   region-scoped, so a borrow of a *current* document blocks advances too.
   Expect `writes_dropped_by_lease` to rise under that traffic. Mitigations:
   early advance, copy-out on `ns_until_forced_wrap`, and region-scoped
   borrow counts (open question 4).
2. **The correctness surface widens.** One predicate, "phase plus a
   positional check against the cursor", becomes four legs evaluated against
   a snapshot. They are applied at the probe, the hop, the election, both
   remove paths, the header-RMW sites and the renew paths. One site that
   loads a fresh `F` instead of `F_Σ`, or skips the stamp, reopens the
   forward-fill tear. Mitigation: a single `Stripe::admit(entry_or_hop, Σ)`
   helper with no bypass, plus the seam-driven tests in 11.3.
3. **Directory pressure and the size of the HTTP win.** About twice as many
   entries resolve. On large-stripe, small-object volumes the directory
   already bounds capacity, so the gain is smaller than the replay shows, and
   bucket-full evictions rise. There are also more spurious reader retries,
   because the epoch changes per chunk, not per pass.

---

## 13. Open questions for the maintainers

1. **Default.** Ship off and flip later (the conservative choice, but a
   second cold cache for adopters, since the mode is fingerprinted)? Or on,
   inside the v8 release that already forces a cold cache?
2. **Chunk count and floor.** `N = 64` fixed, or
   `N = clamp(data_area / 1 MiB, 4, 64)`, so small stripes holding large
   documents do not advance on every write? Whichever it is gets persisted
   and fingerprinted.
3. **Mode mismatch between processes.** Filename isolation (the proposal)
   means differently configured workers silently use *different rings*.
   Should `Cache::start` refuse to start instead when it finds an existing
   sibling file of the other mode?
4. **Region-scoped borrow accounting.** Ship v1 with the stripe-wide count
   (simple, and the proof is unchanged)? Or grow the `MmapDirectory` header
   by a per-chunk borrow-count line (a v8-internal layout change, which bumps
   `MmapDirectory::kVersion` and needs an argument that is exact per chunk)
   before turning retention on by default for the HTTP product?
5. **Cross-pass alternate links.** Keep them (4.5, better alternate
   retention for background-optimized variants)? Or keep today's
   refuse-on-any-epoch-change rule and accept that retained Originals are
   orphaned when a new alternate is written?
6. **Evacuation or second chance.** Should a hot retained document be copied
   forward when the frontier reaches it (ATS-style, CLOCK-like, closing part
   of the 3-point FIFO-versus-LRU gap), at a cost in write amplification? It
   would be a separate design.
7. **Forged in-payload headers (pre-existing).** Any aliasing entry or
   pointer that lands inside a payload can meet a forged header with a valid
   CRC. Harden this, for example by stamping the document's own offset into
   the unused `sync_serial` and checking it on read? Or accept it as today?
8. **Directory sizing for HTTP on large volumes.** 65 536 entries per stripe
   caps small-object capacity on 1 GiB stripes today. Scale
   `kDirectoryEntriesPerSegment` with stripe size (a layout-constant change),
   or leave it?
9. **Fix the stuck `wrap_intent` after a writer crash** (4.12) in this change
   or separately? It is pre-existing, cheap to fix, and a whole pass of
   misses on the affected stripe today.
