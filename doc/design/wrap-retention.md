# Wrap retention: keep the previous pass readable until it is overwritten

**Status:** implemented, **default on** (`CacheConfig::wrap_retention = true`;
flush is the opt-out, C API `disable_wrap_retention = 1`). Every D1 gate
passed before the flip (section 14, step 6): all 18 tests of section 11 and
the review tests in `tests/integration/test_wrap_retention.cpp`; the full
suite and TSan in both modes on macOS and Linux; the PageSpeed
`cache_burst_test` 200/200 with retention forced on, plus 40/40 under TSan.
Review item R4 (an alternate write over a retained head dropped the key's
other alternates) is resolved by the carry-forward in section 4.5, and the
read-miss regression that retention exposed (a lock-free read whose probe
raced a same-key publish) by #20.
Deviations from the design as written are listed in section 14. Sections 0-13
are kept as the design record, except 4.5, which describes the carry-forward
that replaced the original link refusal for retained heads.

**Scope:** what a stripe's circular data area does with directory entries
when the write cursor wraps and then advances over the previous pass. Out of
scope and unchanged:

- key-to-stripe mapping
- document layout (one header field that has always been 0 gets a meaning)
- the RAM tier
- alternate-selection plugins

Anchors below are function, type and comment-marker names, not line numbers.
They are in `src/core/volume.{hpp,cpp}`, `src/core/directory.{hpp,cpp}` and
`src/core/mmap_directory.{hpp,cpp}` unless stated otherwise.

---

## 0. Decisions (lead, after review)

| # | Decision | Rationale |
|---|---|---|
| D1 | **Default ON in v8**, but only if the acceptance tests in section 11, `kv_churn`, PageSpeed `cache_burst_test` and TSan all pass in both modes. Otherwise v8 ships with it off. | v8 already forces a cold cache on upgrade, so turning retention on in the same release costs no extra cold start. A later flip would cost one (the mode is persisted per volume, section 4.2). |
| D2 | **64 chunks with a 1 MiB minimum chunk.** `Q = max(1 MiB, round_up(A/64, 8))`, `N = clamp(ceil(A/Q), 1, 64)`, with `N` persisted. `N = 1` behaves like flush. The earlier idea "chunk ≥ largest document" is dropped. | Tying `Q` to the largest document would collapse `N` to 1 under HTTP defaults (a 64 MiB `max_object_size` against 32 MiB stripes). It is not needed for correctness either, because a multi-chunk advance is still a single gated step. The 1 MiB floor keeps a stripe of about 32 MiB from running the gate every few writes. |
| D3 | **Mixed modes are refused**, through the `VolumeHeader` and not the filename (B5, section 4.13). | Mixing is unsafe in both directions. The filename does not reliably carry the mode. |
| D4 | **Per-chunk borrow counting is in scope**, together with the per-region exposure check (B1). | Without it, a borrow of any document on the stripe blocks every advance, and every advance tears every borrow. |
| D5 | **No cross-pass alternate links.** Retained chains remain walkable downward (retained → retained). An alternate write over a retained head **carries the chain forward** into its own slot instead of linking it (section 4.5, R4). | This removes the self-loop and duplicate hazards (B2). Retained PageSpeed chains still resolve past their head, and keep resolving when the optimization engine adds an alternate after a wrap. |
| D6–D8 | Out of scope: evacuation / second chance, hardening against forged in-payload headers, directory sizing. | Separate designs. |
| D9 | **The stuck-intent fix is in scope, as its own commit**, with the S5 refinements. | It is a pre-existing gap: a crash inside the wrap window leaves every read of that stripe missing for a whole pass. |

---

## 1. Summary

Today a wrap toggles the stripe's 1-bit directory phase. Every entry of the
pass that just ended then stops resolving at once, even though nearly all of
those documents are still intact on disk ahead of the write cursor. Each
stripe restarts empty on every wrap and holds on average about half of its
capacity.

This design keeps the previous pass readable until its bytes are about to be
overwritten. It has five parts:

1. **Clean frontier.** A per-stripe clean frontier `F` runs ahead of the
   write cursor `W`, in `N` fixed chunks.
   - `[W, F)` is cleaned runway that the writer fills.
   - `[F, E)` is the previous pass. It stays readable.
2. **Gated advance.** Moving `F` forward is the only step that exposes
   readable bytes to the forward fill. It runs the reader-exclusion handshake
   (intent → borrow counts + lease → publish), gated only by borrows in the
   chunks it is about to expose.
3. **Exposure generation `G`.** One monotone 64-bit word per stripe:
   `G = P·(N+1) + f`, where `P` is the pass number and `f` is the frontier
   index. A borrow of a document from pass `p` whose first byte is in chunk
   `c` is **exposed** iff `G > (p+1)·(N+1) + c`. Borrows, lease renewals and
   strict renewals check their own document's exposure instead of comparing
   the whole epoch. A wrap or an advance elsewhere in the stripe therefore
   tears nothing.
4. **Pass stamp.** Each document carries its pass number in
   `Document::write_serial`, which has always been 0 until now. The stamp is
   what makes "one pass old" exact, and it supplies `p` for the exposure
   check.
5. **Admission.** An entry is served only when its phase and position
   (against a `G` snapshot), its stamp, its exposure, and its full key and
   CRC all agree. One `admit()` helper enforces this at every site that
   resolves a directory entry or a chain hop.

The policy replay predicts:

| Pattern | Retention | Today (replayed) | Today (measured) | Plain per-stripe FIFO |
|---|---:|---:|---:|---:|
| `zipf` | **0.815** | 0.761 | 0.757 | 0.817 |
| `zipf+scan` | **0.685** | 0.644 | 0.638–0.642 | 0.687 |

That recovers 5.4 and 4.1 points of the 8–9 point gap to LRU. The rest is
FIFO versus LRU.

---

## 2. The problem, with numbers

Round 4 of [the KV benchmark](../kv-cache-benchmark.md#round-4-bounded-capacity-under-churn)
used this setup:

- 2 MiB blocks, C = 16 GiB, 16 stripes of about 511 blocks each
- Zipf(0.99), key universe 3 × C
- get-or-insert

| 2 MiB, C = 16 GiB | LRU | FIFO | FIFO per stripe | wrap flush (replay) | measured Cyclone |
|---|---:|---:|---:|---:|---:|
| `zipf` | 0.850 | 0.818 | 0.817 | 0.761 | 0.757 |
| `zipf+scan` | 0.725 | 0.687 | 0.687 | 0.644 | 0.638–0.642 |

The policy-only replay (`benchmarks/kv_churn_policy`) matches the measured
result within 0.006. The loss is therefore policy, not implementation:

- plain FIFO costs about 3 points against LRU;
- the wrap flush costs about 6 more.

The HTTP product has the same problem, in some ways worse. That is
mod_pagespeed and the PageSpeed optimizer: several nginx workers sharing one
mmap directory. Its stripes are smaller (32 MiB under the auto geometry), so
they wrap more often.

---

## 3. The current mechanism

### 3.1 Write side

`Volume::commit_write` and `Volume::commit_alternate_write` hold the stripe
`mutex` and call `Volume::allocate_write_slot`. In multi-process mode that
call also holds the cross-process `write_lock` across the pwrite (F6) until
`Volume::commit_write_slot` releases it.

When the document does not fit, the writer does this:

1. It calls `set_wrap_intent(true)`.
2. `Volume::lease_permits_wrap` loads the borrow count and the lease.
   - If a borrow is outstanding and its lease is live, the write is
     **deferred**: the intent is cleared and the fill is dropped with
     `NoSpace`.
   - The ceiling forces a wrap after `lease_wrap_ceiling`.
3. `Volume::evict_if_needed` toggles the phase. This is the O(1) flush.
4. The cursor resets and `Volume::record_wrap` bumps the wrap count. The
   wrap epoch is (count, phase).
5. It calls `set_wrap_intent(false)`, strictly after steps 3 and 4.

The Dekker proof lives at the intent-set site in `allocate_write_slot`.

### 3.2 Read side

`Volume::read_sync` does the following, in order:

1. Capture `wrap_epoch()`.
2. Run `Stripe::probe_each`. It yields only current-phase entries, and its
   positional guard (`Stripe::is_behind_write_cursor`) rejects any offset at
   or ahead of the cursor.
3. Map the region, check `is_valid()`, the full `first_key`, and the CRC (or
   the CRC-validation cache).
4. Call `acquire_borrow` and `stamp_read_lease`.
5. Call `borrow_still_valid`: load the intent first, then check that the
   whole epoch is unchanged.

Chain hops go through `is_valid_chain_offset`, which applies the same
positional predicate. That predicate is also used today on
*directory-sourced* offsets by `remove_sync`, by the `commit_write` election
and by `update_hit_count_sync` (B2c).

### 3.3 Where the reader-exclusion state lives

| State | Single-process | Multi-process (`MmapDirectory::Header`, shared by every mapping process) |
|---|---|---|
| phase | `Directory::_current_phase` | `current_phase`, offset 16 |
| wrap count | `Stripe::local_wrap_count` | `shared_wrap_count`, offset 40 |
| write cursor | `Stripe::write_pos` | `shared_write_pos`, offset 24 (absolute) |
| wrap intent | `Stripe::local_wrap_intent` | `wrap_intent`, offset 33 |
| borrow count | `Stripe::local_borrow_shards` (64 per-thread shards) | `stripe_borrow_slot`, offset 34 (single slot) |
| read lease | `Stripe::local_lease_expiry_ns` | `stripe_lease_expiry_ns`, offset 56 |
| force deadline | `Stripe::local_wrap_deferred_deadline_ms` | `shared_wrap_deferred_deadline_ms`, offset 36 |

A reader in another process keeps its borrow and lease in the shared header.
Anything else it needs is process-local: read anchors, the CRC-validation
cache, the readahead filter and `BorrowToken`.

The 64-byte header is fully spent (see the `kFixedFieldsSize` static_assert).
There is room after it, though. With 16 384 buckets the directory needs
720 960 bytes, while `data_offset` rounds that up to 177 pages (724 992
bytes). That leaves **4 032 bytes of slack** after the entries (section
4.2).

### 3.4 Why the flush exists

The phase is only 1 bit, so an entry that survives two wraps reads as
current again. Invariant 9 therefore rejects every entry at or ahead of the
cursor: the ordinary forward fill overwrites those bytes in place, and no
wrap gate protects that. Flushing the whole previous pass is the cheapest
way to honour that rule.

---

## 4. Mechanism

### 4.1 Vocabulary and geometry

Per stripe:

- `S`, `E`: start and end of the data area. `A = E − S`.
- `Q = max(1 MiB, round_up(ceil(A / 64), 8))` and
  `N = clamp(ceil(A / Q), 1, 64)` (D2). Both are pure functions of the
  stripe's own `A`. They do **not** depend on `max_object_size` or any
  per-process config, so every process computes the same values (test 15).
  - `N = 1` (a stripe of 1 MiB or less) is flush behaviour: the first advance
    after a wrap exposes the whole ring at once.
  - A 32 MiB auto-geometry stripe gets `Q = 1 MiB`, `N = 32`.
  - A 1 GiB stripe gets `Q = 16 MiB`, `N = 64`.
- `P`: the pass number (the wrap count).
- `f ∈ [0, N]`: the frontier index. `F = min(S + f·Q, E)`. The chunk of an
  offset `o` is `c(o) = (o − S) / Q`, clamped to `N − 1`.
- **`G = P·(N+1) + f`**: a single monotone 64-bit word. It is the only
  published epoch in retention mode. `P = G / (N+1)`, `f = G mod (N+1)`, and
  the phase is **derived on the reader** as `φ = P & 1` (S3). A reader never
  uses the directory's own phase load (S8).
- `W`: the write cursor. It is unchanged, including F6 (it is published only
  after the fill is durable). B3 is assumed: on a wrap, `shared_write_pos` is
  published as `data_area_start` inside the intent window.
- Regions:
  - **current** `[S, W)`: this pass;
  - **runway** `[W, F)`: cleaned, not yet written;
  - **retained** `[F, E)`: the previous pass.

A freshly initialised directory has `G = 0`, which means `P = 0` and `f = 0`.
`MmapDirectory::init` zeroes everything, so the first pass advances through
empty space. Those gates are trivially clear.

### 4.2 New state and layout

| What | Single-process | Multi-process |
|---|---|---|
| `G` | `std::atomic<uint64_t>` in `Stripe`, next to `local_wrap_count` | new **retention region** in the directory slack, 8-byte aligned |
| per-chunk borrow slots | each of the 64 `BorrowShard`s holds 64 × `uint16_t {gen:8, count:8}` slots, one per chunk (128 B per shard, exactly `kShardPad`; same 8 KiB per stripe as today) | retention region: 64 × `uint32_t {gen:8, count:24}`, one per chunk |
| pass stamp | `Document::write_serial = P mod 2^32` | same |
| mode | `VolumeHeader` reserved bytes: `uint16_t retain_chunks`. 0 = flush; otherwise this volume's base `N`. Authoritative. | same |

**Retention region (mmap).** It holds `G` (8 B) plus 64 per-chunk slots. It
sits after the entries, 8-byte aligned.

We use u32 slots rather than the u16 slots the review sized for. That is a
recommendation (S4). It closes today's saturate-at-255 hole, where a borrow
acquired at saturation "rides along" uncounted and becomes unprotected once
the counted holders drain. The size grows from 136 B to 264 B, and it still
fits:

- `required_size` goes from 720 960 to 721 224 bytes (it would be 721 096
  with u16 slots);
- `data_offset` stays at 177 pages (724 992);
- a static_assert pins both numbers.

**Which slot is used where.** The retention region is used in **both
modes**:

- The stripe-wide `stripe_borrow_slot` is replaced by the per-chunk slots. In
  flush mode the gate checks all chunks, which is equivalent to today.
- The lease slot, the intent flag, the force deadline and the wrap-count
  telemetry stay where they are.

**Version and fingerprint.** Because the layout and the borrow slot's meaning
change:

- `MmapDirectory::kVersion` goes from 1 to 2;
- `kVersion` is **mixed into the fingerprint geohash**, so old and new
  binaries resolve to different files (B5);
- `init_stripes` **never** calls `init()` over a directory with valid magic
  and the wrong version unless it holds the exclusive lifetime lock. Without
  that rule an old opener's failed `open()` would zero a live peer's header.

The in-memory `Stripe::write_serial` and `Stripe::sync_serial` fields are
deleted, so no one can stamp a per-process value by mistake (nit).

### 4.3 Writer: allocation in retention mode

All of the following runs in `Volume::allocate_write_slot`, under the stripe
`mutex` and, in multi-process mode, the `write_lock`, on both commit sites.

```
adopt shared W (as today) and shared G              // never a process-local copy (S3)
if doc > E - W:                                     // WRAP: exposes no bytes, not gated
    set_intent(WRAP to P+1)                         // 2 + ((P+1) & 1): committed (R1)
    publish shared_write_pos := S                   // B3: inside the intent window
    phase := (P+1) & 1  (seq_cst, under phase_lock) // derived, not toggled
    store G := (P+1)·(N+1)                          // P+1, f = 0; seq_cst
    record_wrap telemetry (shared_wrap_count, time)
    set_intent(false)
need := W + doc
if need > F:                                        // MANDATORY advance (S2)
    t := chunk_index_ceil(need)                     // exactly what this doc needs
    if !advance(t, episode=true):
        drop the fill (NoSpace)                     // shared cursor stays where it is (B3)
if F < E and F - need < Q/2:                        // EARLY advance (S1): optional
    advance(chunk_index_ceil(need + Q/2), episode=false)   // failure is silent
patch write_serial := P into the document buffer    // outside the CRC
reserve [W, W + doc)                                // F6 unchanged

advance(t, episode):
    t := max(t, shared f)                           // f is monotone within a pass (S3)
    set_intent(true)                                // seq_cst, BEFORE the gate loads
    clear := lease inactive OR every per-chunk count in [f, t) is 0   // seq_cst loads
    if !clear and episode:
        lease_permits_wrap episode logic: deferral clock, published force deadline,
        and on the ceiling: force, reset ALL chunk slots (gen+1), then clear := true
    if !clear: set_intent(false); return false      // zero side effects
    store G := P·(N+1) + t                          // seq_cst; exposes chunks [f, t)
    set_intent(false)                               // AFTER the G store
    return true

on every granted slot (either mode):
    end the deferral episode: clock := 0, published deadline := 0   // R3
```

Notes on the procedure:

- **The wrap is not gated.** It overwrites nothing. It does make the tail of
  the previous pass (chunks `≥ f`) unreachable, and by the exposure formula
  that tail counts as exposed from that moment. A borrow held there sees
  `kTorn` on its next renew even though its bytes stay intact until the next
  pass's frontier reaches that chunk; that frontier move still waits for the
  chunk's count. This conservative early `kTorn` is limited to about one
  chunk per pass. Current-class borrows are **not** exposed by a wrap: their
  threshold is `(P+1)(N+1) + c ≥` the new `G` (test 2).
- **A deferred first advance after a wrap** leaves `shared_write_pos = S`
  (B3). The next writer finds the document fits in `[S, E)` and retries the
  advance. It does not wrap again, and `shared_wrap_count` moves exactly once
  (test 1).
- **Mandatory versus early.**
  - The mandatory target is exactly `ceil(need)` (S2). Only the mandatory
    advance runs `lease_permits_wrap`'s episode machinery: the deferral
    clock, the published force deadline, and the ceiling-forced reset.
  - The early advance adds a `Q/2` runway margin. It is a pure gate check
    with no side effects (S1, test 8). A borrow burst shorter than the time
    it takes to write `Q/2` bytes therefore never drops a write.
- **Forced advance.** It resets **all** chunk slots (generation + 1, count
  0), not only the exposed range (S4). One ceiling episode therefore clears
  every leaked count (test 7). `ns_until_forced_wrap` stays stripe-wide
  (nit). A live borrow outside the exposed range keeps a valid `G` verdict
  but has lost its count, so the later normal advance over its chunk would
  not defer for it. Both renews and the read-time revalidation therefore
  also compare the borrow's generation with its slot's current one and
  report `kTorn` / false on a mismatch (R2, section 14).
- **Episode end.** The deferral episode bounds *continuous* starvation. Any
  write that gets its slot ends it, in both modes: a passing mandatory or
  early advance, a wrap, or a document that fits the tail or the runway
  without the deferred step (R3, section 14).
- **Large documents.** `chunk_index_ceil(need)` can move `f` across many
  chunks in one gated step. That is why D2 does not need a chunk size tied to
  the largest document.

### 4.4 Admission: what makes an entry valid

A reader first loads the snapshot `G_Σ` (seq_cst), which gives `P_Σ`,
`φ_Σ = P_Σ & 1` and `F_Σ`. It then loads `W` (acquire), **in that order**
(S3, test 17).

`Stripe::probe_each` returns entries of **both** phases in retention mode
(S8). `admit(entry, Σ)` decides.

**Before mapping** (position):

| class | condition |
|---|---|
| current | `e.phase == φ_Σ` and `S ≤ o < W` |
| retained | `e.phase != φ_Σ` and `F_Σ ≤ o < E` |
| anything else | reject |

**After mapping the header:**

1. `is_valid()` and the length bounds hold.
2. Stamp: `p = write_serial` must equal `P_Σ` for the current class and
   `P_Σ − 1` for the retained class.
3. Full `first_key == K` (invariant 8).
4. **Not exposed:** `G_Σ ≤ (p+1)(N+1) + c(o)`.
5. CRC, or the CRC-validation cache.

**Borrow (B1).**

1. Increment the per-chunk slot of `c(o)`, the chunk of the node actually
   served (S4, which matters for `read_alternate_sync`).
2. `stamp_read_lease`.
3. Load `intent`: if it is set, release and retry.
4. Load `G` and check `G ≤ (p+1)(N+1) + c(o)`.

**Renew.**

- `renew_read_lease`: acquire fence, then check exposure against a fresh `G`.
- `renew_read_lease_strict`: stamp the lease, load the intent, load `G`.
  - exposed → `kTorn`;
  - intent set but not exposed → `kCopyNow`;
  - otherwise → `kOk`.

`borrow_still_valid` becomes this check. `epoch_start` in
`VolumeReadHandleImpl` becomes `{p, c}`, the document's pass and chunk.

What each leg does:

- **Position** keeps readers out of the runway and out of
  reserved-but-unwritten bytes (F6). `F_Σ` comes from the snapshot. A stale-low
  `W` only widens rejection.
- **Stamp** makes "one pass old" exact, and it is what supplies `p`. Exposure
  is computed from the document's *own* pass, so a class misjudged from a
  snapshot that straddled a wrap still yields the correct exposure verdict
  (5.4).
- **Exposure** replaces whole-epoch equality. Only the advance that crosses
  this document's chunk can tear it. A wrap, or an advance elsewhere, leaves
  it `kOk` (test 6). The retry storm from the first draft is gone, and so is
  the proposed "class-aware revalidation" follow-up.
- **Key and CRC** are unchanged. They reject aliasing into reused space, and
  torn or unsynced bytes.

In flush mode `admit` reduces to today's predicate, and it still uses the
per-chunk slots with a gate that checks every chunk.

### 4.5 Chain hops (D5)

`admit(hop, Σ)` replaces `is_valid_chain_offset` for hops. `s` is the source
node, `t` the target.

| source class | admitted target | target class |
|---|---|---|
| current | `S ≤ t < s` | current (stamp `P_Σ`) |
| retained | `F_Σ ≤ t < s` | retained (stamp `P_Σ − 1`) |

Anything else is rejected, and that includes **every upward hop**. Within a
pass, documents lie in write order and links never cross a pass (see below),
so a live link always points downward into the same pass.

The retained → retained downward hop is what keeps a retained PageSpeed chain
(for example Brotli → Gzip → Original) walkable past its head. Without it
every retained chain would collapse to its head. The stamp, exposure, key,
CRC and borrow legs then apply per node, and the borrow is registered on the
**served** node's chunk.

**Alternate writes** (`commit_alternate_write`, B2a/b, S6):

- **Retained head: carry-forward** (R4, `Volume::carry_retained_chain`). The
  write never links the retained chain. It rewrites the key's other
  alternates as **current-pass documents in its own slot**:
  `[carried nodes, oldest first][new head]`, one reservation, one pwrite,
  one directory insert. The links are patched once the slot is known
  (head → newest carried → … → oldest → 0, every hop downward inside the
  slot), and every node takes the slot's pass stamp. So the chain lies in one
  pass, and D5 holds.
  - *What is carried:* the first (newest) copy of every id except the one
    being written, as the walk sees it. Shadows and superseded copies are
    not carried.
  - *Caps:* at most `kMaxAlternatesPerKey - 1` nodes and
    `min(A / 8, max_object_size)` bytes (never more than fits beside the new
    document). Priority: the Original, then newest first. What does not fit
    is dropped and counted in `alternates_carry_dropped`. A retained chain
    never makes the write fail (N3).
  - *Copy protocol:* each kept node is copied out of the mapping under the
    stripe mutex, **before** the allocation, and re-validated on the copy:
    magic, version, length, key, id, stamp `P_Σ − 1`, CRC. After all copies
    an acquire fence and one fresh `G` load drop any node whose chunk has
    since been exposed. That is the reader's exposure rule applied to a
    borrow-less copy: an advance stores `G` before any pwrite into the chunk
    it exposes, so a copy judged unexposed came from intact bytes.
  - A wrap inside the carry's own allocation is harmless: the sources are
    already in memory and every link points into the slot, so nothing is
    refused.
- **Current head, allocation wrapped** (the wrap race, checked by comparing
  **`P` only**, never `f`): in flush mode the planned link is refused (zeroed
  in the buffer, as before). In retention mode the wrap did not destroy the
  chain, it made it retained. The write gives its unfilled slot back (the
  cursor never covered it, F6) and retries once. The retry sees a retained
  head and carries it. Nodes the retry's own advance exposed are gone, as
  for any reader.
- The verified head entry is **updated in place** to the new document in
  every case, and never inserted with `kNoVerifiedEntry`. No retained
  duplicate can survive.
- A retained head is never linked. A fixed-size new head therefore cannot
  form a self-loop by landing on the old head's offset (test 9). What lands
  there now is the carried copy, and the head links one document down to
  it.

**Crash safety of the carry.** Until the insert, the only entry for the key
is the retained head, and the carried copies are unreferenced bytes. The
insert runs after the fill (and, with `sync_on_write`, after its fsync), so
once it has run the head and every carried node are durable. A crash at any
step therefore leaves either the old retained chain or the new complete
chain resolvable. It never leaves a published prefix of the new chain. A
crash inside the allocation is repaired like any writer crash (4.12). Power
loss with `sync_on_write = false` keeps the existing contract: the directory
can outrun the data, and each node is then CRC-checked on its own, so a lost
node ends the chain early. It is never served wrong.

**Header RMW and splices.** `commit_header_rmw` and `repoint_chain_link`
defer on a wrap race by comparing **`P`** (S6). They re-run `admit` on their
target under the lock, which also catches an advance, because advances run
under the same locks.

**Removal on a retained chain removes the whole entry** (S7). This covers
`remove_alternate_sync`, which does not splice retained headers.

### 4.6 Uniqueness: one resolvable entry per key per stripe (B2)

These sites all go through `admit`, never through `is_valid_chain_offset`, so
retained entries are visible to them (B2c):

- the `commit_write` and `commit_alternate_write` elections;
- `remove_sync`;
- `update_hit_count_sync`.

**Rule 1: elect.** A rewrite of `K` updates `K`'s admitted entry, current or
retained, in place.

**Rule 2: clean up.** Both commit paths already map every same-tag candidate.
Any *other* same-key entry, and any candidate that fails `admit`, is cleared
inside the same seqlock bracket as the insert.

**`remove_sync`** removes **all** same-key entries (B2d), not only the first.
Each clear decrements `entry_count`, so `count()` stays exact (nit).

### 4.7 Directory `insert` victim order

`Directory::insert` and `MmapDirectory::insert` load `W` and `G` **inside
their seqlock bracket**, under `phase_lock` for the mmap directory. The
reason is that in multi-process mode `commit_write_slot` releases the
`write_lock` before the insert runs, so an independently opened writer can
wrap or advance in between (S9). The victim order is:

1. verified same-key entry (in place);
2. empty slot;
3. inadmissible slot (runway; previous phase behind `F`; current phase at or
   ahead of `W`);
4. tag collider (counted);
5. oldest admissible: the retained entry nearest `F` if there is one,
   otherwise the lowest-offset current entry.

Retention roughly doubles the number of entries that resolve (section 10).

### 4.8 Other paths

- `exists_sync` stays a directory-only probe, applying the position leg only.
  It can return a false "yes" for a stamp-dead entry, the same class of error
  as its existing tag-collision false positives.
- **RAM tier and `cross_process_ram_coherence`: unchanged.** An advance
  mutates no `DirEntry`, so no bucket version moves. A RAM copy of a document
  the ring later evicts is still correct content, as with the phase toggle in
  [multi-process.md](../multi-process.md#cross-process-ram-coherence).

### 4.9 Multi-process

**Ownership (invariant 7).** Only a `write_lock` holder moves `G`, and it is
the owner. Any process that writes a stripe adopts the shared `G` and the
shared `W` under the lock; it never publishes from a local copy (S3). This
covers:

- a process-count change;
- a restart;
- forked writers;
- **independently opened writers** (`test_multiprocess_writers.cpp`, S9).

**Readers in other processes** use the shared state directly:

- they load `G` from the retention region and `shared_write_pos` from the
  header;
- they register borrows in the shared per-chunk slots;
- they stamp the shared lease.

The owner's gate reads exactly those.

### 4.10 In-memory `Directory` versus `MmapDirectory`

The protocol is identical. They differ only in where the state lives and how
long it lives: a single-process volume rebuilds an empty `Directory` on every
open, so it retains nothing across a restart.

On the single-process read path a borrow is a CAS on one `uint16_t` in *this
thread's* shard line, so invariant 6 holds. The gate sums the 64 shards'
slots for the exposed chunks. That is the same 64 lines it sums today.

### 4.11 Document sizes

Cyclone has no aggregation buffer. Each commit is one pwrite of one
document, so advances happen per document, and a document larger than the
runway advances by as many chunks as it needs in one gated step. Otherwise
behaviour is unchanged:

- documents larger than the data area still get `NoSpace`;
- documents over `max_object_size` still get `ObjectTooLarge`.

With the 1 MiB floor, a 32 MiB stripe of 2 MiB KV blocks advances about two
chunks per write. That costs a handful of loads and two stores per write. It
no longer triggers reader retries, because exposure is per chunk.

### 4.12 Crash, restart and power loss

**The open path never writes retention state (B4).** `init_stripes` runs in
every opener, including while peers are live. It does not sanitise `G`, `W`
or the slots.

- Readers clamp instead: `F = min(S + f·Q, E)`; `f > N` is treated as `N`; a
  misaligned or out-of-range `W` is already rejected by the existing
  recovery guard.
- Any repair happens as an ordinary gated advance, by a writer, under the
  `write_lock`.
- The only exception is the **exclusive open**, when this process holds the
  exclusive lifetime lock and so has no live peers. Only then are the borrow
  slots, the lease and the intent zeroed, and the phase re-derived as `P & 1`
  (S4, S5).

**Stuck intent (D9, S5).** A writer that dies inside the intent window
leaves `wrap_intent` set. Today that makes every read of the stripe miss for
a whole pass. The flag is repaired, and the phase re-derived from `P`, in two
cases only:

- on a **proven-dead `forced_release`** of the `write_lock` (`kill(pid,0)`
  confirmed the holder is gone);
- on an exclusive open.

It is **not** cleared on `escalated_takeover`: that holder may still be alive
and inside its window (test 13). This fix ships as its own commit, and it
applies in both modes.

**Committed wraps are completed, not cleared (R1).** The intent byte
carries a value, not a flag. `1` marks a gate decision or an advance: the
only irreversible store is `G` itself, so clearing is the whole repair. A
wrap stores `2 + ((P+1) & 1)` **before** its first irreversible store (the
cursor drop to `S`): in flush mode right after the gate passes, in retention
mode at the start of the ungated wrap. A writer that dies after that point
can leave `W = S` with `G` still in pass `P` (and, in retention, `F = E`).
Clearing alone would then let the next writer fill the *current* pass from
`S`, over documents whose borrows still renew `kOk` because no `G` store
ever exposed them. Recovery therefore completes the wrap, in the writer's
own order and still inside the intent window: `W := S`, then phase :=
`(P+1) & 1`, then `G := (P+1)(N+1)` (flush: `+ N`), then `record_wrap`,
then the clear. `G` moves at most one pass per wrap under the write lock,
so the parity in the intent value says whether the dead writer had already
published `G`; with equal parity only the cursor, the phase and the clear
remain. A completed retention wrap leaves `f = 0`, so the next write's
mandatory advance is gated over chunk 0 as usual and defers for any borrow
there (test "Retention review R1").

**Process crash, other points:**

- A crash after the `G` store but before the intent clear is covered by the
  above.
- Torn bytes from an interrupted pwrite in the runway are harmless: no entry
  can admit them.
- A committed document whose insert was lost is simply unreachable.

**Power loss** (default `sync_on_write = false`). Header, directory, data and
retention region can each be persisted independently. The legs cover every
combination:

| What was lost | How it is caught |
|---|---|
| Data persisted past the persisted `F`. Old entries now point at newer documents. | Stamp `P` against expected `P−1`, or bad magic |
| Data persisted past `W` | Position (as today) |
| The header or `G` was lost across a whole wrap | Stamp |

The first draft's msync of `f` before the fill is **dropped** (nit). `G` sits
in a single 64-byte-aligned write unit, and the legs above already turn
reordered persistence into a miss.

**Lost timeline.** After the next wrap, documents from a lost timeline whose
stamp coincides with the new pass can be admitted as genuine. Each is a real,
CRC-valid document of that key, possibly an older version (test 16). We
accept this. It is the same class of outcome as today's power-loss contract.

### 4.13 Mode persistence and mismatch (D3, B5)

The mode is **not** in the filename. Filenames are unreliable here for three
reasons:

- `resolve_unsized_cache_path` picks the newest file of the format major;
- `fingerprint_cache_path` returns already-fingerprinted paths unchanged;
- the C API accepts explicit paths.

Instead:

- `VolumeHeader::retain_chunks` is written at creation and is
  **authoritative**. A mismatch between it and the opener's config sets
  `needs_reset` in `Volume::open_locked` and goes through the existing
  live-peer gate:
  - **refuse** if a live peer holds the file;
  - **cold reset** if no peer does;
  - return **`IncompatibleVersion`** if `auto_reset_on_incompatible` is off.

  So a retaining process and a flushing process never run on one ring
  (test 14).
- Old binaries are kept off new files by the version rather than the mode:
  `MmapDirectory::kVersion = 2`, mixed into the fingerprint, plus the
  lifetime-lock rule in 4.2.
- **No format-major bump** is needed. v8 is unreleased, and the new header
  bytes and `write_serial` are zero in every v8 file. Zero means flush.

**C API (S11):** add a trailing field `disable_wrap_retention`, a *negative*
flag, so a zero-initialised `CycloneCacheConfig` gets the default. It carries
the same trailing-field ABI note as `small_tier_percent` in `cyclone_c.h`:
there is no mixed-version ABI safety. A caller compiled against an older
header passes a smaller struct, and the library would read garbage from the
new field. Callers must recompile against the new header when adopting it.
The C++ API adds `CacheConfig::wrap_retention`, which defaults to `true`
under D1.

---

## 5. Correctness argument

### 5.1 Invariant by invariant

1. **No reader lock.** Reads add loads only (`G`, `W`), plus a per-chunk CAS
   that replaces the existing borrow CAS one for one. The stamp is a 4-byte
   compare on a header the reader already maps.
2. **Commit ordering.** Unchanged. The stamp travels in the same pwrite as
   the document. Reordered persistence of `G` and the fill downgrades to a
   miss (4.12).
3. **Dekker.** Same shape, quantified per chunk.
   - Writer: `intent := 1` → load the counts of chunks `[f, t)` and the
     lease → store `G` → `intent := 0`.
   - Reader: increment chunk `c` → stamp the lease → load `intent` → load
     `G`.
   - All operations are seq_cst.
   - If the writer's load of chunk `c` missed the reader's increment, the
     reader's intent load comes later in the order `S`. It then sees either
     the intent, or (if the intent was already cleared) the new `G`, because
     the `G` store is program-ordered before the clear. The new `G` exposes
     `c`, so the reader backs off.
   - Borrows are released on close.
4. **Seqlock.** An advance does not touch entries. The rule-2 clears and
   `remove_sync` use the normal odd → even brackets under the stripe mutex.
5. **HitTracker.** Untouched. An advance never calls `record_hit()`.
6. **Sharding.** No new shared line. Single-process chunk slots live inside
   each thread's own shard line. The mmap borrow is the same one-slot CAS as
   today, but on a per-chunk slot, which is less contended than today's
   single stripe slot.
7. **Ownership.** Only a `write_lock` holder moves `G`. Non-owners read it.
8. **Full-key re-verify.** Kept on every candidate. It is extended by the
   stamp and by the uniqueness rules (4.6).
9. **Positional guard at both choke points.** Generalised to the tables in
   4.4 and 4.5. Its purpose still holds: no admitted borrow aliases bytes
   that the forward fill writes without a gate. The fill writes only
   `[W, F)`, and every move of `F` is gated per chunk (5.3).

### 5.2 A stale entry from two or more passes back is never served

Take an entry `e` for key `K` at offset `o`, created in pass `≤ P_Σ − 2`.
Consider what now sits at `o`:

- **The original, intact document.** Its stamp is `≤ P_Σ − 2`, which is
  never `P_Σ` or `P_Σ − 1`, so it is rejected.
- **A newer document starting at `o`,** written in pass `R ∈ {P_Σ − 1, P_Σ}`.
  - If the class implied by `e`'s phase does not expect `R`, it is rejected.
  - If it does, the key check applies. A different key is rejected. The same
    key means the document is a genuine document of `K` from the live window,
    and uniqueness (4.6) makes `e` the single entry that resolves `K`.
- **The middle of a document, or padding.** Rejected by the magic, length,
  key and CRC checks. Forged in-payload headers are a pre-existing exposure,
  out of scope (D7).
- **A 32-bit stamp alias.** It would need an entry to survive 2^32 wraps of
  one stripe, which we accept.

So every admitted document is a genuine, CRC-verified document of `K` from
pass `P_Σ` or `P_Σ − 1`.

### 5.3 The forward fill never tears an admitted borrow

**Claim.** Let `B` be a borrow of a document from pass `p`, starting at `o`
in chunk `c`, admitted with `G ≤ T = (p+1)(N+1) + c` after its increment.
While `B` holds its slot and its lease is live, no pwrite touches the
document's bytes. The exception is a ceiling-forced advance, as documented
today.

**Proof.**

1. Pwrites land only in `[W, W + doc)`, which lies inside the runway as it
   was at allocation.
2. The runway only ever covers chunks already exposed by an advance, that is,
   chunks for which `G` has passed their threshold.
3. The document's first byte, in chunk `c`, is overwritten only after some
   advance publishes `G > T`. Advances cover whole chunks from `f` upward, so
   crossing `c` means crossing it for exactly this pass `p + 1`.
4. That advance loads chunk `c`'s count after its intent store. By 5.1(3),
   either the advance sees `B`'s increment and defers, or `B`'s own check saw
   the intent or `G > T` and `B` was never admitted.
5. After admission, `B`'s count stays in chunk `c`'s slot until the handle
   closes, so every later advance across `c` sees it.

Two loose ends:

- **Bytes in chunks after `c`.** They could only be written after chunk `c`
  itself, since the fill moves forward.
- **The wrap.** It is ungated but writes no bytes.

∎

### 5.4 Torn snapshot

`G` is a single word, so `(P, f, φ)` cannot tear. The only multi-word read is
`(G, W)`, with `G` loaded first and `W` after (test 17). Two cases can
straddle:

- **A wrap between the two loads.** `W` reads low: `S` plus the new pass's
  fill.
  - An entry with `o < W_new` points at a document of the new pass, stamped
    `P_Σ + 1`. It is rejected, because admission needs `P_Σ` or `P_Σ − 1`.
  - A current-class document of pass `P_Σ` with `o ≥ W_new` is rejected by
    position. That is a spurious miss, never a wrong serve.
- **An advance between the two loads.** `W` can only move up inside the
  runway, which holds nothing admissible, and `F_Σ` is used as-is.

In every case, exposure is evaluated against the document's own stamp `p`
using a fresh `G` at borrow time, so a class misjudged from a straddled
snapshot still gets the exact verdict.

---

## 6. Failure modes considered

| # | Failure mode | Handled by |
|---|---|---|
| F1 | Reader classifies with a stale `F` | `F_Σ` from the snapshot; exposure check against a fresh `G` (5.3) |
| F2 | Two-wrap trailing-gap survivor | Stamp (5.2) |
| F3 | Duplicate entries after a rewrite, or an alternate write whose allocation wrapped | In-place update of the verified head; rule-2 cleanup (4.6, B2a) |
| F4 | Self-loop: a fixed-size new head lands on a retained old head's offset | No links to retained heads (4.5, B2b); the carried copies link downward inside the new slot |
| F17 | An alternate write over a retained head drops the key's other alternates (R4) | Carry-forward into the write's own slot, capped and counted (4.5) |
| F5 | Purge misses retained entries | `admit` at every site; `remove_sync` removes all (B2c/d) |
| F6 | Every borrow torn by any wrap or advance | Per-region exposure `G` (B1) |
| F7 | Deferred first advance leaves the cursor high | `W := S` inside the wrap intent window (B3) |
| F8 | Opener writes shared frontier state beside live peers | Open never writes; readers clamp (B4) |
| F9 | Mixed modes, or an old binary zeroing a live header | `VolumeHeader` mode check plus the reset gate; `kVersion` in the fingerprint; no `init()` over a wrong-version directory without the exclusive lock (B5) |
| F10 | Writer crash leaves the intent stuck | Cleared on a proven-dead release or an exclusive open; not on escalation (S5) |
| F11 | Phase and `P` drift apart | `φ = P & 1` derived on the reader; stored seq_cst under `phase_lock` at the wrap |
| F12 | Early advance starts or forces a deferral episode | Early advance is a gate check only (S1) |
| F13 | Leaked counts across chunks after a crash | A forced reset clears all chunks; an exclusive open zeroes them (S4) |
| F14 | Borrow saturation at 255 | u32 mmap slots with a 24-bit count (S4) |
| F15 | Long zero-copy holds block advances | Only in the holder's own chunk (D4); early advance; ceiling unchanged |
| F16 | Doubled directory pressure | Victim order (4.7); `bucket_full_evictions` telemetry |

---

## 7. How ATS does it, and where we deliberately differ

This is from our reading of ATS `iocore/cache` (9.x/10.x). Verify the call
sites before citing any of this elsewhere.

**How ATS works:**

- **Positional validity.** `dir_valid()` treats an in-phase entry as valid
  only if it is behind the write position (plus the aggregation buffer). An
  out-of-phase entry is valid only if it is ahead of the next aggregation
  write.
- **Bulk sweeps.** `dir_clean_vol()` / `dir_clean_segment()` run at the wrap,
  and `dir_clean_range_interval()` handles ranges. Recovery clears what was
  written since the last directory sync (`dir_clear_range()`), using
  `sync_serial` / `write_serial`.
- **Evacuation.** `evac_range()` copies pinned documents, and documents with
  registered readers, forward, ahead of the write.
- **Copying readers.** Readers look up the directory under the stripe mutex,
  read into an IOBuffer, and re-check the key afterwards. No borrow or lease
  is involved.

**Where we differ:**

1. Zero-copy borrows **gate the frontier per chunk** instead of evacuating.
   Evacuation (D6) is a possible later second-chance mechanism.
2. Readers are lock-free and may be in other processes. We use a `G`
   snapshot, per-document exposure, and per-chunk Dekker instead of the
   stripe mutex.
3. There is no aggregation buffer. The frontier moves in fixed chunks, and
   early.
4. The pass stamp is checked on **every read**, not only in recovery, so no
   eager sweep is needed to kill phase-ABA survivors.
5. There is no directory sweep in v1 (see section 8 for the cost of one).

---

## 8. Alternatives considered

- **Eager clean-ahead** (scan the directory and clear entries in `[F, F')`).
  A full scan of a stripe's 65 536 entries measured about 130 µs on an Apple
  M5. At 4 KB documents and `Q = 1 MiB` that is about 0.5 µs per write,
  roughly 5 % of the write p50. It also moves bucket versions and churns peer
  RAM tiers. It is not needed for correctness. A binned, O(cleaned) variant
  is held in reserve.
- **Whole-epoch revalidation** (the first draft). Rejected (B1): it tears
  every borrow on the stripe at every advance and every wrap.
- **Stripe-wide borrow count** (the first draft). Rejected (D4): one long
  zero-copy hold anywhere on the stripe blocks every advance.
- **Mode carried in the filename** (the first draft). Rejected (B5).
- **Cross-pass alternate links** (the first draft). Rejected (D5): they allow
  self-loops and duplicates, for a small gain.
- **Carry-forward variants** (R4, section 14). All of them keep D5; they
  differ in what they copy and when. See section 14 for the comparison.
- **A 2-bit phase.** It still needs the stamp or a sweep. It buys nothing.

---

## 9. Expected hit-ratio effect (policy replay)

`kv_churn_policy` reports `retain/N` for `N ∈ {16, 32, 64, 256}`. The
table below is from the replay as it now stands, with keys routed to stripes
by `segment_hash()` exactly as `Volume::select_stripe` does; its existing
columns reproduce `doc/kv-cache-benchmark/churn/policy-replay.txt` digit for
digit. (The first version of this table used the replay's earlier fixed
routing; the switch moved no cell by more than 0.003 and changed no
conclusion.) The **bold** column is the `N` that D2 gives each
configuration:

- 1 GiB and 128 MiB stripes → `N = 64`;
- 32 MiB stripes → `Q = 1 MiB`, `N = 32`.

```bash
./build/kv_churn_policy                         # 2 MiB, C = 16 GiB (round 4)
./build/kv_churn_policy 2097152 2147483648      # 2 MiB, C = 2 GiB
./build/kv_churn_policy 2097152 4294967296      # 2 MiB, C = 4 GiB (section 14 run)
./build/kv_churn_policy 524288                  # 512 KiB, C = 16 GiB
./build/kv_churn_policy 4096 536870912          # 4 KB, C = 512 MiB (HTTP-shaped)
```

| workload | LRU | stripe FIFO | flush (today) | retain/16 | retain/32 | retain/64 | retain/256 |
|---|---:|---:|---:|---:|---:|---:|---:|
| `zipf`, 2 MiB, 16 GiB | 0.8498 | 0.8172 | 0.7605 | 0.8086 | 0.8127 | **0.8150** | 0.8165 |
| `zipf+scan`, 2 MiB, 16 GiB | 0.7251 | 0.6867 | 0.6438 | 0.6803 | 0.6834 | **0.6850** | 0.6862 |
| `zipf`, 2 MiB, 2 GiB | 0.8154 | 0.7726 | 0.7029 | 0.7604 | 0.7658 | **0.7698** | 0.7698 |
| `zipf+scan`, 2 MiB, 2 GiB | 0.6935 | 0.6444 | 0.5902 | 0.6352 | 0.6393 | **0.6422** | 0.6422 |
| `zipf`, 2 MiB, 4 GiB | 0.8279 | 0.7913 | 0.7263 | 0.7801 | 0.7851 | **0.7876** | 0.7898 |
| `zipf+scan`, 2 MiB, 4 GiB | 0.7052 | 0.6612 | 0.6123 | 0.6533 | 0.6568 | **0.6587** | 0.6601 |
| `zipf`, 512 KiB, 16 GiB | 0.8667 | 0.8385 | 0.7883 | 0.8310 | 0.8347 | **0.8366** | 0.8380 |
| `zipf+scan`, 512 KiB, 16 GiB | 0.7405 | 0.7051 | 0.6680 | 0.7000 | 0.7027 | **0.7039** | 0.7048 |
| `zipf`, 4 KB, 512 MiB | 0.8756 | 0.8475 | 0.7758 | 0.8428 | **0.8460** | 0.8469 | 0.8473 |
| `zipf+scan`, 4 KB, 512 MiB | 0.7507 | 0.7150 | 0.6809 | 0.7098 | **0.7125** | 0.7138 | 0.7147 |

What the table says:

- Retention recovers 93–98 % of the flush cost at the D2 geometry.
- **Round-4 prediction:** `zipf` 0.815 (from 0.757) and `zipf+scan` 0.685
  (from 0.638–0.642). The replay matched measurements within 0.006 last time.
- **Served throughput (a model, not a measurement):** weighting T=1 latencies
  (hit 191 µs, miss+insert 4 064 µs) gives about 1.34× the hits/s, roughly
  0.84× LMDB instead of 0.62×.
- **HTTP-shaped** (4 KB, 32 MiB stripes, `N = 32`): +7.0 points. This assumes
  the directory holds every live document, and it does here: about 7 700 per
  stripe against 65 536 entries.

---

## 10. Performance

**Read path.** Four changes, all cheap:

- One more `G` load, from the same line as `W` in mmap mode, or next to it
  locally.
- A 4-byte stamp compare.
- The exposure compare.
- The borrow CAS moves to a per-chunk slot.

There is no new RMW. Reader retries now happen only inside a short intent
window, or on real exposure of the reader's own chunk, so the first draft's
"about 1 % spurious retries" is gone. Warm-read p50 (0.33 µs) should stay
within noise.

**Write path.** Every write does one compare of `need` against `F`. Each
advance does:

- the intent store;
- lease + count loads for the exposed chunks, summed over 64 shards locally
  (the same 64 lines today's gate sums);
- the `G` store and the intent clear.

That is at most about 1–2 µs, amortised over a chunk: at 4 KB documents with
`Q = 1 MiB` it is about 250 writes per advance, under 0.01 µs per write.

The wrap is still O(1) and is no longer gated. No directory scan happens
anywhere.

**`performance_baseline` (4 KB).** The documented run (5 000 × 4 KB into
512 MiB) never wraps. Its first pass does advance through empty chunks
(`G = 0`): 20 MB over 16 stripes is about two 1 MiB chunks per stripe,
with nothing to wait on.

- Acceptance: write p50/p99 and warm-read p50 within ±3 % of the baseline
  table.
- Add a churning variant (for example `--cache-size 64 --entries 50000`) that
  reports `frontier_advances` and `advances_deferred_by_lease`.

**Directory sizing.** About twice as many entries resolve.

- That is ample for KV blocks and for 4 KB documents in 32 MiB stripes.
- On large-stripe, small-object volumes the 65 536-entry directory already
  bounds capacity today (D8, out of scope).

---

## 11. Test plan

### 11.1 New tests (`tests/integration/test_wrap_retention.cpp`, tag `[retention]`, plus the listed extensions)

1. **F6-E in mmap mode (flush and retention), plus a deferred first advance
   under a borrow.** `shared_wrap_count` moves by exactly 1, and the shared
   cursor is `S`, not high (B3).
2. **The ungated wrap keeps current-class borrows `kOk`** until the frontier
   crosses the borrow's own chunk.
3. **Advance Dekker, writer paused after the intent is set.** Both orders:
   reader increments before the gate load, and after it.
4. **Writer paused after the `G` store, before the intent clear.** Readers
   see exposure (for exposed chunks) or `kCopyNow`.
5. **Reader paused between the CRC and `acquire_borrow`** while the writer
   advances over its chunk and pwrites. The reader must reject.
6. **Per-chunk gating, including a forced advance.** `kTorn` only in exposed
   chunks; borrows in other chunks stay `kOk`.
7. **One ceiling episode clears all leaked chunks** (fork, kill -9 the
   holder).
8. **Early advance never forces.** No deferral clock, no deadline, no reset.
9. **Uniqueness.** Same-offset rewrites with fixed-size documents, and
   `commit_alternate_write` with retained and wrap-raced heads: exactly one
   resolving entry, no self-loop, and (since R4) the other alternates
   carried forward.
10. **Purge of a retained key,** including `remove_alternate_sync` on a
    retained chain (the whole entry goes, S7).
11. **Retained chain hops.** Downward retained → retained works. Upward hops
    and targets below `F_Σ` are rejected. The borrow sits on the served
    node's chunk.
12. **A peer open never writes the shared frontier state** (B4).
13. **Crash at every writer seam** (fork, `_exit`). A `forced_release` clears
    the intent and re-derives the phase; an escalated takeover does not
    (S5).
14. **Mode and layout mismatch:** refused with a live peer, cold reset
    without one, `IncompatibleVersion` when auto-reset is off. A `kVersion 1`
    peer resolves to a different file, and a wrong-version directory is never
    `init()`-ed without the exclusive lock.
15. **`Q` and `N` are identical across processes with different
    `max_object_size`,** and `N` is clamped for small stripes (`N = 1`
    behaves like flush).
16. **Lost-timeline power loss.** Documents admitted after the next wrap are
    genuine and CRC-valid; there is never a wrong-key or torn serve.
17. **Snapshot load order** (`G` then `W`) across a wrap and an advance
    (5.4).
18. **The full suite in both modes,** plus TSan hammer variants of tests 3–6
    and PageSpeed `cache_burst_test` with retention on.

### 11.2 Seams

**Writer seams** (test-only hooks in the style of
`s_write_tear_gate_for_test`):

- after the intent is set;
- after the gate passes;
- after the `G` store;
- inside the wrap, between the `P` store and the `f` reset. With a single
  `G` word this seam sits between the `shared_write_pos := S` publish and the
  `G` store.

**Reader seams,** compile-time only behind `CYCLONE_TEST_SEAMS` so the hot
path carries nothing in release builds (S10):

- between the CRC and `acquire_borrow`;
- between the borrow and the intent / `G` loads.

### 11.3 Existing guards that must stay green in both modes

| Invariant | Guards |
|---|---|
| 1 | `test_lockfree_read_races.cpp` |
| 2 | `test_power_loss.cpp` |
| 3 | `test_lease_pinning.cpp` |
| 4 | `test_directory.cpp` and `test_mmap_directory.cpp` |
| 5 | `test_hit_tracker.cpp` |
| 6 | `concurrent_read_bench` rows |
| 7 | `multi_process_test.cpp` and `test_multiprocess_writers.cpp` |
| 8 | `test_wrap_phase_aba.cpp` (A), (B); `test_tag_collision.cpp` |
| 9 | `test_wrap_phase_aba.cpp` (C)–(F) and F6-A…F |

**Parameterised expectations in retention mode:**

- `test_eviction.cpp`: stale entries stay visible until the frontier passes
  them; slots are reclaimed as inadmissible.
- `test_wrap_phase_aba.cpp` (C)/(D): the trailing-gap survivor is rejected by
  the stamp.
- `test_wrap_phase_aba.cpp` (E) and the "fresh chain" case: links to retained
  heads are never made (D5). Since R4 the wrap-raced write carries the
  chain instead, so (E) serves the dark node's content from its carried,
  current copy, and the "fresh chain" case counts no refusal.

### 11.4 Benchmark acceptance (gates D1)

- `kv_churn`, round-4 configuration:
  - measured hit ratio ≥ 0.80 on `zipf` and ≥ 0.67 on `zipf+scan`;
  - `writes_dropped_by_lease` ≤ 10× today's 0–3 per run.
- `performance_baseline` as in section 10.
- `concurrent_read_bench` within noise.
- ASan/UBSan and TSan clean in both modes.

---

## 12. Rollout, telemetry and risks

**Flag.**

- C++: `CacheConfig::wrap_retention`, default `true` under D1.
- C API: `disable_wrap_retention`, zero meaning the default (S11).
- It applies at volume **creation**. On a mismatch the persisted mode wins
  through the reset gate (4.13).
- **Fallback:** if D1's gates fail, ship v8 with the default `false`. The
  persisted zero means flush, so nothing else changes.

**New counters** in `VolumeStats`/`CacheStats` and the C API:

- `frontier_advances`
- `advances_deferred_by_lease`
- `early_advances_skipped`
- `retained_hits`, the direct measure of the benefit
- `stamp_rejections`
- `alternates_carried_forward`, `alternate_carry_bytes` and
  `alternates_carry_dropped` (R4, section 14)

**Risks after the amendment:**

1. **Correctness surface.** A four-leg admission at every resolving site. One
   bypass reopens a tear. Mitigation: `admit()` is the single entry point,
   built in its own no-behaviour-change commit first; seams 3–5; TSan
   hammers.
2. **Long zero-copy holds.** These still block advances into the holder's own
   chunk, now **only** that chunk. A slow client sending a large body from
   the next chunk to be exposed stalls that stripe's writer after the runway
   is used, as today at a wrap, but more often. Watch
   `advances_deferred_by_lease` in the PageSpeed soak.
3. **Directory pressure.** About twice as many entries resolve, so there are
   more bucket-full evictions on large-stripe, small-object volumes. The gain
   there is smaller than the replay shows (D8).

---

## 13. Implementation plan

Ordered commits. **Each one builds and passes the full suite** in every mode
that exists at that point.

1. **Stuck-intent fix** (D9, S5). Clear `wrap_intent` and re-derive the
   phase on a proven-dead `forced_release` and on an exclusive open, never on
   `escalated_takeover`. Includes test 13 (flush-mode part). Independent of
   everything else.
2. **`admit()` refactor, no behaviour change.**
   - One helper replaces `probe_each`'s positional guard and
     `is_valid_chain_offset` for directory-sourced offsets.
   - Epoch handling moves behind a `{p, c}` token.
   - In flush mode it computes exactly today's predicate.
   - The existing suite is the proof.
3. **`G` word and per-chunk borrow layout,** retention still off.
   - The retention region is placed in the directory slack (static_assert on
     721 224 bytes and 177 pages).
   - `MmapDirectory::kVersion = 2` and mixed into the fingerprint.
   - No `init()` over a wrong-version directory without the exclusive lock.
   - Per-chunk local shard slots.
   - Borrow, renew and strict-renew move to the exposure check. In flush
     mode `N = 1`, so this equals today's semantics.
   - Delete `Stripe::write_serial` / `sync_serial`.
   - Tests 6, 7, 12, 14 and 15.
4. **Frontier, advance and stamp,** behind the flag (default off until
   step 6).
   - Wrap and advance in `allocate_write_slot`, mandatory and early advances.
   - Stamp patch.
   - `VolumeHeader::retain_chunks` and the mode check.
   - Seams.
   - Tests 1–5, 8, 16 and 17.
5. **Uniqueness, remove and alternates.**
   - Rule-2 cleanup.
   - `remove_sync` removes all.
   - Link refusal compares `P` and refuses retained heads.
   - In-place head update.
   - Retained downward hops.
   - S7.
   - The insert victim order, with boundaries loaded inside the bracket.
   - Tests 9–11.
6. **Flip the default** (D1). Only after section 11.4, `cache_burst_test` and
   TSan pass in both modes. Includes the C API flag and counters.
7. **Docs.** Update `doc/architecture.md` (Glossary: frontier, pass stamp,
   `G`; the lease section) and `doc/multi-process.md` (retention region,
   mode mismatch), and move this record to "design record for shipped work".

---

## 14. Implementation notes

What the implementation does where this record leaves a choice open, and
every place it departs from the text above.  Each note names the commit
that introduced it.

**Step 4 (frontier, advance, stamp).**

- *Stamp in both modes.*  The writer stamps `write_serial := P` in flush
  mode too; flush-mode admission ignores the stamp, so this changes no
  flush verdict and keeps a later mode change from meeting unstamped
  documents.
- *Seams in the retention wrap.*  The ungated wrap fires the writer seams
  `kAfterIntentSet`, `kWrapAfterCursor` and `kAfterEpochStore` (it has no
  gate, so no `kAfterGatePassed`); each advance fires `kAfterIntentSet`,
  `kAfterGatePassed` and `kAfterEpochStore`.  A third reader seam,
  `kSnapshotGen`, sits between the `G` and `W` loads of `Volume::snapshot`
  for test 17; like the other reader seams it exists only in
  `CYCLONE_TEST_SEAMS` builds.
- *Counters.*  A ceiling-forced mandatory advance is counted in the existing
  `wraps_forced_past_lease`; `wraps_deferred_by_lease` keeps counting only
  flush-mode wrap deferrals, and `advances_deferred_by_lease` the retention
  ones.  `writes_dropped_by_lease` counts drops in both modes.
- *Test placement.*  Test 6 needs advances, so it lands in step 4 rather than
  step 3; so do the mode legs of test 14 and the cross-process leg of 15
  (step 3 carries their layout and geometry legs).
- *Suite in both modes.*  The mode flag exists from step 4, but the
  uniqueness rules (4.6) only arrive in step 5, so before step 5 retention
  mode can leave a stale duplicate entry (for example after an alternate
  write whose link was refused) and the full suite is only clean in flush
  mode.  The both-modes run of test 18 starts at step 5.

**Step 5 (uniqueness, remove, alternates).**

- *Election preference.*  When a bucket holds both a current and a retained
  admitted entry of the same key (only possible for state written before
  the uniqueness rules existed), the commit paths elect the current one --
  it is the newer version -- and clear the other.
- *Same-offset takeover (addition to 4.7).*  Before step 2 of the victim
  order, `insert` takes over an entry with the key's tag that already sits
  at the offset just written.  Its bytes are the ones the new document
  replaced, so it is dead; left beside the new entry, the bucket would hold
  two same-tag entries at one offset, which `remove_at(tag, offset)` cannot
  tell apart.  This is reached with fixed-size documents, where a rewrite
  lands exactly on its own retained copy after the frontier exposed it.
  For the same reason `remove_at` now clears every entry with that tag and
  offset, not only the first.
- *A rejected hop ends the visible chain.*  The alternate write's walk
  treats an inadmissible link (and a stale node) as the end of the chain
  for the planner (`chain_fully_walked`).  That is sound because the
  verdict is permanent: a current hop must point downward, which is static,
  and a retained hop at or beyond `F`, which only rises within a pass.  A
  side effect is that a corrupt cyclic link is never followed at all (every
  cycle contains a self or upward link); the cycle guard stays as defence
  in depth.  `test_alternate_chain_bound.cpp`'s cycle case is updated
  accordingly: the write now heals by the build-time splice instead of the
  traversal-cap reset or rejection.
- *Flush-mode behaviour changes.*  All deliberate, all in the direction of
  fewer stale entries: upward hops are rejected (4.5), the commit election
  clears same-key duplicates and dead same-tag entries, `remove_sync`
  removes every same-key entry, and inadmissible slots (including
  two-wrap survivors at or ahead of the cursor) are reclaimed before a live
  collider.
- *Test 13, escalation leg.*  With retention, once the wrap has stored the
  new `G` the next write needs an advance, which opens an intent window of
  its own and would hide whether the takeover repaired.  So in retention
  mode that leg covers the `kAfterIntentSet` and `kWrapAfterCursor` seams
  only; the repair decision does not depend on the seam.

**Step 6 (default, C API, hammer).**

- *The default, first off, then on.*  D1 flips the default only after
  every gate passes. At first all passed except one: the PageSpeed
  `cache_burst_test` (a cross-process stress test in the mod_pagespeed tree)
  could not be run, because the consumer checkouts pinned an older Cyclone
  commit whose `third_party/cyclone.BUILD` predates `src/core/crc32c.cpp`.
  So the step shipped with the default off. The gates were then re-run on
  the tree with R4 resolved and #20 merged: the section 11 and review tests,
  the full suite and TSan in both modes on macOS and Linux, and
  `cache_burst_test` 200/200 with retention forced on (40/40 under TSan).
  `kDefaultWrapRetention` is now `true`. The test-seam override
  `CYCLONE_TEST_WRAP_RETENTION=0|1` still forces either default, and
  "Retention default" in `test_wrap_retention.cpp` (with the C API case in
  `test_c_api.cpp`) pins the default and the opt-out.
- *Upgrade cost, against D1's rationale.*  D1 expected the flip to ride the
  v8 cold start at no extra cost. It came after v8 and the default-off
  retention reached main, so a volume created by main's default records
  flush (`retain_chunks = 0`). The mode is not in the filename (4.13), so the
  first default open by this build finds a mismatch on the same file: a cold
  reset with no live peer, `ResetRefusedLivePeer` while one holds it. That
  is one extra cold start for such users, and a hard ordering rule for
  overlapping multi-process upgrades. Configuring `wrap_retention = false`
  keeps the old cache. `doc/api-reference.md` ("Wrap Retention") states the
  procedure.
- *Test 18 classification.*  The hammer uses a 2 s lease (so no borrow is
  unprotected by lapse) and a 60 ms ceiling (so forced steps occur). A
  content mismatch counts as a tear only when `wraps_forced_past_lease`
  moved during that read. A forced step is the documented unprotected case,
  and the test reports those reads separately. Any other mismatch fails the
  test.
- *R4, recorded as an open item (resolved below).*  Writing an alternate
  onto a key whose head is retained started a fresh chain (D5: links never
  cross a pass), so the retained alternates of that key stopped resolving
  at once even though their bytes stayed readable until the frontier
  reached them. For the PageSpeed shape (Original first, optimized
  alternates added later by the optimization engine) this dropped
  Original, Gzip and WebP when an AVIF landed after a wrap, and it was
  counted in `alternate_wrap_refusals`.
- *No hardware divide on the read path.*  The snapshot splits `G` by
  `N + 1`, and the chunk of a document is `(o - S) / Q`. Both divisors are
  fixed at open, so `Stripe` holds a reciprocal (`FastDivU64`) for each. A
  64-bit `DIV` on the i7-8750H made single-thread reads about 11 % slower
  than before this work, in both modes. With the reciprocal the gap is
  2-4 %.

**Review fixes (after step 7).**  An independent review reproduced three
bugs; each is now a test in `test_wrap_retention.cpp` ("Retention review
R1/R2/R3").

- *R1 -- crash inside a committed wrap.*  See "Committed wraps are
  completed, not cleared" in 4.12. The intent values live in
  `MmapDirectory` (`kIntentStep`, `kIntentWrapEven`, `kIntentWrapOdd`);
  readers still only test for non-zero. The process-local intent uses the
  same values, but it dies with its process, so only the mmap path ever
  repairs.
- *R2 -- a forced reset uncounts far borrows.*  Two fixes were possible:
  reset only the exposed range `[f, t)`, or make every borrow notice the
  reset. The implementation does the second. Renew, strict renew and the
  read-time revalidation compare the token's generation with its chunk
  slot's current generation (one `seq_cst` load) and report `kTorn` /
  false on a mismatch. Resetting only `[f, t)` was rejected because it
  gives up S4: a count leaked by a crashed holder in a chunk the force did
  not cross would keep deferring every later advance over that chunk, one
  ceiling episode per chunk instead of one per stripe. With the generation
  check the reset still clears every leak in one episode, and a live
  holder elsewhere learns at once that it is no longer protected, while
  its bytes are still intact. The cost is a conservative `kTorn` for
  borrows far from the forced chunk. That only happens after a hold has
  already outlived the ceiling somewhere on the stripe. Test 6 now pins
  it: the far borrow `h2` reads `kTorn` with its bytes intact, and a fresh
  read of the same document is `kOk` again.
- *R3 -- a stale deferral episode (both modes, pre-existing).*  The clock
  was reset only by a passing mandatory gate or a force. A document that fit
  the tail (flush) or the runway (retention), or a passing early advance,
  left it running, so the next first contact with a fresh borrow was forced
  immediately and the published deadline stayed stale (`ns_until_forced_wrap`
  near 0, so embedders copied needlessly). Every granted slot now ends the
  episode (`Volume::end_deferral_episode`), and so does any passing gate.
- *Nits.*
  - N1: when the verified entry wins the insert election, a dead same-tag
    entry at the offset just written is cleared in the same bracket.
  - N2: the commit election clears a candidate only for a validated bad
    header, not for a transient mapping failure.
  - N3: a write whose link to a retained head will be refused no longer
    runs the old chain's `TooManyAlternates` and truncation checks.
  - N4: `FastDivU64` falls back to 32 x 32 partial products where neither
    `__umulh` nor `unsigned __int128` exists.
  - N5: the process-local per-chunk slots are `u32 {generation:8,
    count:24}`, like the shared ones. A saturated 8-bit count was not safe
    for every holder: a ride-along acquired at saturation is uncounted, and
    once the counted holders close the slot reads 0 under it. A shard is now
    two 128-byte lines (16 KiB per stripe).
  - N6: the comment on the conditional exclusive-lock probe now describes
    both of its uses.

**Review R4 (carry-forward).**  The gap above is closed. An alternate write
over a retained head keeps the key's other alternates reachable and still
never links across a pass. Section 4.5 has the mechanism and the crash-safety
argument. The choice, against the alternatives considered:

| Option | Correctness | Complexity | Write amplification | Verdict |
|---|---|---|---|---|
| Keep the drop (before R4) | Correct, loses data | None | 0 | Rejected: cancels retention on PageSpeed's optimize-after-write path |
| Link across the pass | Self-loops and duplicates (B2) | Low | 0 | Rejected (D5) |
| **Carry at write time, in the write's own slot** | Nothing published before all of it is durable: old chain or new chain, never a prefix | One copy-and-validate helper, patched links, one retry on a wrap race | The chain once per key per pass, capped | **Chosen** |
| Carry each node with its own allocation | A crash between allocations leaves a half-built chain to clean up or hide; every allocation may wrap or advance under the next | Several slots, several inserts or a staging protocol | Same bytes | Rejected: more states, no gain |
| Carry only the Original and the newest K | As chosen | As chosen | Lower | Folded in: it is the chosen cap policy (Original first, then newest), with K set by the count and byte caps rather than a constant |
| Defer the carry to the next read of the retained chain | Readers are lock-free and may be non-owners (invariants 1, 7), so the read would have to hand the carry to a writer; the write itself must still publish a head that either links across the pass (D5) or drops the chain | High | Same bytes, later | Rejected: it cannot avoid the drop it is meant to fix |

Details:

- *When.*  Only in retention mode, and only when the elected head is
  retained, or when the allocation of a write that planned a live link
  wrapped (it then retries once, and the retry finds the head retained).
  Flush mode runs the old code path unchanged: `carry_forward` is false
  and the restart needs `stripe->retain`.
- *Write amplification.*  A carry copies each surviving alternate once. The
  copies are current, so the rest of that pass links normally: at most one
  carry per key per pass. It needs an alternate write to a key whose chain
  is retained. In the PageSpeed-shape test the AVIF write carries Original
  (30 100 B), Gzip (9 200 B) and WebP (7 000 B): 46 704 bytes on disk beside
  a 5 136-byte AVIF document. The next alternate that pass (Brotli) copies
  nothing. After the next wrap, one more write carries all five. The byte
  cap bounds one carry at `min(A / 8, max_object_size)`, which is 4 MiB on a
  32 MiB auto-geometry stripe.
- *Cost of the extra advance.*  A carry's slot is larger than the new
  document, so its mandatory advance can cross more chunks. A borrow in one
  of them defers the whole write (`NoSpace`, as for any deferred fill). It is
  never downgraded to a plain write that drops the chain: losing one
  optimized write is cheaper than losing the Original.
- *Counters.*  `alternates_carried_forward`, `alternate_carry_bytes` and
  `alternates_carry_dropped` (in `VolumeStats`, `CacheStats` and at the tail
  of `CycloneCacheStats`). `alternate_wrap_refusals` no longer moves in
  retention mode. It still counts flush-mode wrap races.
- *RAM tier.*  The carried copies are byte-identical, so only the written
  id's RAM entry is evicted, as before. A carry that dropped alternates
  evicts the whole key, because the dropped ids are no longer in the chain.
- *Tests.*  In `test_wrap_retention.cpp`:
  - "Retention R4" cases: preservation in single- and multi-process mode; a
    peer view parked at the publish sees the old chain complete; the byte
    cap and the count cap (Original kept, oldest others dropped and
    counted); a writer killed at every carry step (`copied`, the three
    writer seams of its advance, the tear gate, `filled`) leaves the old
    chain complete; and a TSan hammer in both modes where readers, half of
    them on the existing Original, walk the chains while alternate writes
    carry them.
  - The seams are the existing writer seams and tear gate, plus
    `Volume::CarrySeam` for the two steps they do not reach.
  - The cases that pinned the drop (the "PageSpeed-style" review case,
    test 9, and `test_wrap_phase_aba.cpp` (E) and (G)) now pin
    preservation.
