# Verified state that outlives the process

**Status:** shelved (2026-09-24). The persistent, cross-process table is
not being built: its gain is mainly for data already in the page cache
(2–9 % of a cold NVMe first read), and it would cost a directory layout
change, another cold start for users of `main`, and trust in the disk that
reaches across reboots. Revisit if a consumer shows restart or peer-process
verification cost matters. Two findings from this study are being acted on
separately: the per-process CRC cache's 16-bit identity weakness (section
3; fixed by moving that cache to the full token format, no layout change),
and the 512 KiB cold-read gap, which is the I/O pattern rather than the
checksum (We-Amp/cyclone-cache#18).

**Scope:** whether the "this document's payload already passed its CRC-32C"
verdict may be shared between processes and kept across a restart, and if
so, where it lives, what it is keyed by, when it dies, and what it may and
may not vouch for. Out of scope and unchanged:

- the document format (no byte of the 132-byte header moves);
- the read gauntlet's other legs (position, pass stamp, exposure, full key,
  borrow and lease);
- the single-process in-memory `Directory`, which is rebuilt empty on every
  open and so has nothing to persist (section 4.6);
- the RAM tier.

Anchors below are function, type and comment-marker names, not line numbers.
They are in `src/core/volume.{hpp,cpp}`, `src/core/mmap_directory.{hpp,cpp}`
and `src/core/document.{hpp,cpp}` unless stated otherwise.

---

## 0. Decisions needed from the maintainer

| # | Question | Recommendation |
|---|---|---|
| Q1 | Is the win worth a layout change? It is real for warm-page-cache first reads (restart on a live host, peer processes) and small for cold reads from disk (section 2). | Yes, but ship it opt-in first. It does **not** close the 512 KiB cold gap to LMDB; that gap is not the CRC. |
| Q2 | May a persisted verdict be trusted after a reboot? That skips re-verification of bytes that went through a power cycle. | Only for stripes sealed by a clean stop (section 7.3). Everything else is discarded when a new boot is detected. |
| Q3 | May a verdict skip re-verification indefinitely, so bit rot after verification goes unseen? Today's per-process cache already does this for the life of a process. | Yes by default, matching today. Offer `verified_state_max_age` (hours) for operators who want a bound (section 3.2). |
| Q4 | May the writer mark a document verified at commit, since it computed the CRC? | Yes, same trust class as a reader's first verification in the same boot (section 8). Behind its own knob so it can be turned off. |
| Q5 | Placement: the mmap directory region (data offset moves 177 → 305 pages) or a side file (no layout change, its own lifecycle)? | The directory region, with `MmapDirectory::kVersion` 2 → 3 (section 10). |
| Q6 | Default in multi-process mode. | `kShared` (cross-process, same boot) on; `kPersistent` (across restart and sealed reboots) opt-in until soaked (section 9). |
| Q7 | Pre-existing weakness: the per-process CRC-validation cache trusts any document at a verified offset whose CRC matches in its top 16 bits. That includes a later incarnation at the same offset, whether torn, unsynced or rewritten (section 3.3). Fix it in the same change? | Yes. Move the process-local cache to the same token format as a first, format-free step. **Done** (section 3.3). |

---

## 1. Summary

Each stripe of an mmap-directory volume gets a **verification table** in its
directory region: 8192 sets of eight 64-bit **tokens**, one 64-byte cache
line per set, 512 KiB per stripe. A token is a self-certifying hash:

```
token = mix64(nonce, salt, stripe, rel_offset, write_serial,
              checksum, len, first_key[0..8])       (top 48 bits)
      | verified_hour                               (low 16 bits)
```

- **Set:** a Fibonacci hash of the stripe-relative offset picks the set, as
  `Volume::checksum_cache_index` picks a slot today.
- **Nonce:** a 64-bit random value per stripe in the table header. `0` means
  "no trust": readers verify every payload and store nothing. Replacing the
  nonce kills every token of the stripe in O(1).
- **Store:** whoever verifies a payload stores its token, whether that is a
  reader in any process or the writer at commit. It is one relaxed 64-bit
  store.
- **Check:** a reader that has **already admitted** a document (position,
  pass stamp equal to the pass its class implies, exposure, full key)
  recomputes the token from the header it just read and its own load of the
  nonce. If the token is present in the set, it skips the CRC pass.
  Everything after the CRC site is unchanged: acquire the borrow, stamp the
  lease, and `borrow_still_valid` still gates the result.

**The safety argument in one paragraph.** Within one nonce's lifetime, the
pair (stripe-relative offset, pass stamp) names at most one committed
document, and that document's bytes do not change while any admission of it
can succeed. The fill is the only writer of payload bytes, it is monotone
within a pass, and it reaches a document's bytes only after an exposure step
that admission and the borrow revalidation already reject (wrap-retention
design, section 5.3). A matching token therefore means that some process
checked the CRC of these exact bytes. A nonce lives only as long as that
uniqueness holds:

- It is replaced when a reboot is detected, unless the stripe was sealed
  with its data fsync'd and no fill since.
- It is set to 0 when an fsync fails.
- It is set to 0 while a write-lock holder that was usurped without proof of
  death may still land a late pwrite. That is the one path in which two
  documents can share (offset, pass).

The residual risks are the ones today's per-process cache already accepts:
silent media corruption after verification, and a 2^-45 chance per lookup of
a hash collision. Both are bounded by knobs (section 3).

---

## 2. What is avoidable: measurement

The question is what share of a first read's cost disappears when the CRC
pass is skipped. The consumer still touches or copies the bytes, so page-in
does not go away; it moves to the consumer.

**Method.** A scratch microbenchmark (not committed) maps a 2 GiB file of
random bytes the way `Volume` does: whole-file `MAP_SHARED`, `MADV_RANDOM`,
and a per-document readahead hint (`F_RDADVISE` on Darwin; `MADV_WILLNEED`
in 512 KiB chunks on Linux, as in `Volume::maybe_advise_readahead`). It then
reads every document once, sequentially, in a fresh process. The per-document
work has four variants:

- **crc+touch:** CRC-32C through `crc32c()` (the real `src/core/crc32c.cpp`),
  then touch one byte per 4 KiB page. This is today's first read with a
  `kv_bench` view-mode consumer.
- **touch:** touch only. This is the read with a trusted token.
- **crc+copy:** CRC, then `memcpy` out. This is today's first read with a
  copy-mode consumer.
- **copy:** copy only.

Two page-cache states:

- **Warm:** the file is in the page cache but not in this process's page
  tables. This is a restart without a cache drop, or a peer process reading
  what another process wrote.
- **Cold:** Linux only, after `drop_caches`.

Numbers are the median of three runs, in µs per document.

| Machine, state | size | crc+touch | touch | avoidable | crc+copy | copy | avoidable |
|---|---:|---:|---:|---:|---:|---:|---:|
| Apple M5 (macOS), warm | 512 KiB | 51.6 | 19.8 | **62 %** | 64.8 | 41.8 | **35 %** |
| Apple M5 (macOS), warm | 2 MiB | 226.7 | 97.1 | **57 %** | 249.4 | 165.9 | **33 %** |
| i7-8750H (Linux), warm | 512 KiB | 57.7 | 27.5 | **52 %** | 75.3 | 62.5 | **17 %** |
| i7-8750H (Linux), warm | 2 MiB | 230.8 | 110.3 | **52 %** | 304.9 | 273.4 | **10 %** |
| i7-8750H (Linux), cold NVMe | 512 KiB | 506.7 | 493.3 | **3 %** | 543.1 | 532.2 | **2 %** |
| i7-8750H (Linux), cold NVMe | 2 MiB | 942.2 | 905.4 | **4 %** | 1023.9 | 930.9 | **9 %** |

On a warm cache the CRC runs at DRAM speed, not at the in-cache
`crc32c_bench` rate: 17–18 GB/s over resident but uncached pages on both
machines, against 26.5 / 34.9 GB/s when the data is in cache. The macOS
machine was shared (load 2–3), so its rows are noisier; the Linux machine
was idle.

What this says:

- **Warm first reads: half the cost is avoidable** when the consumer only
  touches the bytes, and 10–35 % when it copies them. This is the case that
  persisted and shared verification serves: every peer process's first read
  of a block, and every read after a process restart on a host whose page
  cache survived. The macOS `restart` and `multiprocess_read` phases of
  `kv_bench` are this case.
- **Cold reads from NVMe: 2–9 %.** On Linux the kernel read dominates. The
  Linux `kv_bench` `restart` phase drops the page cache first, so it is this
  case, and persisted verification would move it by a few percent at most.
  That agrees with the round-3b ceiling: 2.17 GB/s verified against
  2.33 GB/s with verification off at 2 MiB.
- **The 512 KiB cold gap to LMDB (0.80–1.19 against 1.93 GB/s) is not the
  CRC.** This microbenchmark reads 512 KiB documents cold at 0.99–1.06 GB/s
  with **no** CRC and none of Cyclone's per-get overhead. The limit is the
  I/O pattern: one 512 KiB hint per document with serial faults, where
  LMDB gets the kernel's sequential readahead over contiguous overflow
  pages. That is a readahead-depth question (hinting ahead across
  consecutive documents), not a verification one. The benchmark doc's "What
  to change" row cites the 512 KiB cold number as the evidence for this
  work, and it should be corrected when this lands.

---

## 3. Threat model: what the CRC protects against today

The CRC covers header_data + content, everything after the 132-byte header
(`Document::verify_checksum`). It says nothing about the header itself: key,
length, stamp and the mutable fields are guarded by the other legs.

### 3.1 Threats and whether a token may stand in for the CRC

| Threat | What catches it today | May a token be trusted? |
|---|---|---|
| **Torn write by a crashed writer** (pwrite interrupted) | No directory entry: the insert follows the pwrite and `commit_write_slot`'s cursor publish (F6). The bytes are unreachable. | Yes. A token needs a committed, admitted document. The torn span has none, and its (offset, pass) is rewritten only by a later reservation that also has no token. |
| **Power loss before data is durable** (entry durable, data not) | Magic → key → **CRC** in the read gauntlet, plus data-before-directory in `Volume::sync_directory` | **Not across a reboot**, unless the stripe was sealed (7.3). A token can be persisted while the bytes it vouches for are not. Detecting the new boot replaces the nonce. |
| **Reordered persistence of `G` against the fill** (wrap-retention 4.12: fill bytes durable, the advance that exposed them lost) | Stamp, magic, and the **CRC** for a document whose tail was overwritten while its header survived | **Not across a reboot.** Same rule. This is the case that rules out cheaper schemes, such as trusting tokens behind the last durable checkpoint: see 7.4. |
| **Stale bytes after a wrap** (a live borrow while the fill overwrites) | Lease/borrow Dekker handshake plus `G` exposure, **not** the CRC ("a cached CRC verdict must never short-circuit it", comment at the borrow in `Volume::read_sync`) | Unchanged. The token replaces only the CRC site. `borrow_still_valid` runs after it, exactly as today. |
| **Phase-ABA aliasing** (an entry surviving two wraps, intact bytes behind or ahead of the cursor) | Retention: pass stamp (`Volume::stamp_admits`) plus position. Flush: position plus full key plus **CRC**, because `stamp_admits` returns true in flush mode. | Only when the document's `write_serial` equals the pass its class implies, **in both modes** (6.2). Flush mode does not check the stamp at admission, so the token check adds it for itself. |
| **Usurped writer's late pwrite** (last-resort escalated takeover, `commit_write_slot`'s honesty note): two documents reserved at the same cursor, so the same (offset, pass) | **CRC** plus full key (test F6-F) | **No**, while the usurped holder may be alive: the nonce is 0 (7.2). The token's CRC, length and key binding would already refuse the victim's document unless its CRC coincided (2^-32). The nonce rule makes uniqueness (L1) hold outright instead of relying on that. |
| **Bit rot or a media error after verification** | CRC, only in a process that has not already cached the verdict | By knob. Today's per-process cache already skips it for the process's lifetime (3.2). |
| **Writeback failure** (fsync `EIO`: dirty pages marked clean, later re-read from stale media) | CRC on a later read, in a process without a cached verdict | **No.** A failed `fsync_volume` sets every stripe's nonce to 0 until the next exclusive open (7.2). |
| **Foreign or corrupted file** (wrong volume, restored copy, garbage) | Volume and directory magic, versions and geometry, then magic, key and **CRC** per document | Tokens live inside the directory region they describe, so they come and go with it. A per-table random salt stops a token from a previous incarnation of the table (reset, re-init) from matching. A byte copy of a *live* volume restored over the same inode in the same boot is an accepted residual (13, R3). |
| **Hash collision** (a token of another incarnation lands in the same set with equal top 48 bits) | — | Probability 8 × 2^-48 = 2^-45 per lookup, and it matters only if the bytes are also bad. Accepted. |

### 3.2 Media errors: is skipping re-verification acceptable?

Today's answer is already "yes, within a process". `_checksum_cache` keeps a
verdict for as long as the process lives. It survives the page being evicted
and re-read from the device. A block that rots after the first read is
served unchecked by that process until its slot is evicted by a collision.

Persisted verification extends the same trust in time (restart) and in
space (peers). It does not add a new kind of trust. Three observations
bound the exposure:

- Media errors on NVMe and SATA SSDs overwhelmingly surface as `EIO`. On an
  mmap read, that means `SIGBUS`, not silently wrong bytes. Silent
  corruption needs the drive's ECC and the end-to-end path to fail together.
- A writeback failure is the one common way the page cache and the device
  disagree without an error on the read. It is handled by nonce invalidation
  (7.2).
- LMDB, the peer this work is measured against, verifies nothing.

For operators who want a bound anyway, `verified_state_max_age` (hours,
0 = unbounded, the default) puts the verification hour in the token's low 16
bits. A token older than the bound is treated as absent: the payload is
verified again and the token refreshed. Staggered by each document's own
verification time, this re-verifies continuously instead of in a storm.

### 3.3 A pre-existing weakness this design closes

> **Fixed** (We-Amp/cyclone-cache#24), with no
> on-disk or shared-memory format change. The rest of this section records
> the weakness as it was; "Fix as shipped" below says what changed.

`_checksum_cache` was keyed by `{offset, top 16 bits of the CRC}`. Its own
comment said a same-offset replacement whose CRC matches in those 16 bits
skips verification (2^-16 per rewrite). It argues that this is acceptable
because "any document reachable through the directory was fully written
before its entry was published".

Two paths break that premise, and at those paths the 16-bit discriminator
is the only thing left between a reader and unverified bytes:

- **After a power loss.** An entry whose data was not durable points at
  whatever the device holds. Within one process this matters only after a
  restart, and then the cache is empty. So today it is harmless.
- **The usurp tear.** The honesty note in `Volume::commit_write_slot`
  describes a stalled holder's late pwrite landing on the usurper's span at
  the same cursor. The victim's document replaces the usurper's header at
  the same offset. The cached `{offset, crc16}` then vouches for it with
  probability 2^-16 when its CRC field matches in the top bits. With
  lower probability it vouches for a short or failed late write that left
  a header over mixed bytes.

Both are rare, but the fix is free: store the same 64-bit token as the
shared table (4.5), with a per-process nonce. That brings the aliasing
probability from 2^-16 down to about 2^-45, and the process-local cache
then follows the same usurp rule (7.2).

**Fix as shipped.** Each of the 65 536 slots still holds one 64-bit word,
direct-mapped by offset, so the footprint (512 KB per volume) and the warm
path (one relaxed load and compare) are unchanged. The word is now a token
of the document incarnation, `Volume::checksum_token`:

```
a     = fold(offset ^ s0, first_key[0..8] ^ s1)
b     = fold((checksum << 32 | write_serial) ^ s2,
             (len << 32 | header_len) ^ s3)
token = fold(a ^ c0, b ^ c1)          fold(x, y) = lo64(x*y) ^ hi64(x*y)
```

`s0..s3` are a per-Volume random salt drawn at construction (the
per-process nonce of the proposal) and `c0, c1` are fixed odd constants.
All 64 bits are compared, with no truncation and no separate offset field,
so another incarnation matches only on a token collision (about 2^-64 per
lookup instead of 2^-16 per same-offset replacement). The token is
computed at both CRC sites (`Volume::read_sync` and the selected-alternate
site in `read_alternate_sync`) from the header the reader has just
deserialized from the mapping and key-verified (invariant 8). The CRC pass
and the stored token use the same header copy. No other site consults the
cache; the RAM-tier path never reaches it.

Not adopted: the usurp rule (7.2). The process-local cache has no nonce to
zero. With the full token, a usurped holder's late pwrite that reaches the
header gives the document a new identity and forces a re-verification. The
residual is a reader that copied the usurper's header before the late
pwrite reached it and then reads a payload the pwrite is overwriting. That
window needs the nonce rule of the shared table.

Tests: (G1)–(G3) in `tests/integration/test_wrap_phase_aba.cpp`. G1 places
document A at offset X and reads it. A wrap then lands document B at X,
whose CRC differs from A's but shares its top 16 bits, and B's payload is
corrupted after its checksum was written. Under the old key G1 served the
corrupt bytes; now the CRC pass runs and rejects them. G2 changes only the
pass stamp at the same offset and CRC and expects one more CRC pass, on
both read paths. It also pins that the identical incarnation skips the
pass. G3 pins that every identity field changes the token and that the
mutable header fields do not.

---

## 4. Where the verified state lives

### 4.1 Options

| Option | For | Against | Verdict |
|---|---|---|---|
| **A. A bit in `DirEntry`** (the reserved bit in word 2, or the unused `pinned`) | No layout change. One bit. | Only the owner may write an entry (invariants 4 and 7), so a reader in a peer process cannot set it. Every set is a seqlock write that bumps the bucket version, which falsely invalidates `cross_process_ram_coherence` stamps. The bit is not tied to the bytes: a stale entry that resolves again, or an entry updated in place, keeps a bit that described other bytes. Chain nodes reached through `next_alternate_offset` have no entry at all. | Rejected |
| **B. A token in the document header** (`sync_serial`, always 0) | Tied to the document by construction: a new document at that offset brings its own header. | Readers would dirty **data** pages. Every first read would cost a page of writeback, and a reader in a peer process would write into a stripe it does not own. A late store could land in a newer document's header (it self-certifies, so it would be harmless, but it is still a write into the fill region). A token and payload in the same page cache are persisted in arbitrary order, which is the power-loss hole of 3.1, now inside the data file. | Rejected |
| **C. A per-stripe bitmap in the directory slack** | No layout change. | The slack after the retention region is 3 768 bytes. One bit per possible entry needs 8 KiB (65 536 entry slots). A bit also cannot certify an incarnation. | Does not fit |
| **D. A token array parallel to the entry slots** | Exact: one slot per entry, no conflict misses. | `probe_each` does not expose the slot index. Chain nodes have no slot. Moving an entry within a bucket would need to move its token. | Rejected in favour of E |
| **E. A set-associative token table in the directory region, keyed by offset** | Covers directory hits and chain nodes. Self-certifying, lock-free, one cache line per lookup. Lives and dies with the directory: `MmapDirectory::init` zeroes it, a reset wipes it, and `DirectorySyncer` already msyncs the region. | The data offset moves (section 10). | **Chosen** |
| **F. The same table in a side file** (`<volume>.vstate`) | No layout change, no `kVersion` bump. Deleting it is always safe. | A second file per volume needs naming, fingerprinting, creation races, lifetime locks, garbage collection and a binding to the volume's identity. It duplicates everything the directory region gets for free. | Fallback if Q5 goes against E |

### 4.2 Layout (option E)

The table follows the retention region, 64-byte aligned:

```
VerifyTable (per stripe, in the MmapDirectory region)
  header, 64 bytes
    magic       u32   "VTBL"
    version     u16   1
    ways        u16   8
    sets        u32   8192  (num_buckets / 2)
    pad         u32
    salt        u64   random, written once by init
    nonce       u64   atomic; 0 = no trust        (7.1)
    seal        u64   atomic; 0 = unsealed        (7.3)
    boot_tag    u128  boot identity of the last adopting opener (7.3)
    victim_pid  u32   usurped write-lock holder not yet proven dead (7.2)
    hour_base   u32   Unix hour at init, base of verified_hour
  sets[8192]: u64 token[8]   one 64-byte line per set
```

This is 524 352 bytes per stripe. `required_size` goes from 721 224 to
1 245 632 bytes, and the page-rounded data offset from 177 to 305 pages. The
data area of every stripe loses 512 KiB: 1.6 % of a 32 MiB auto-geometry
stripe and 0.05 % of a 1 GiB one.

**Why 8192 × 8.** A document is reachable only through a directory entry, or
as a chain node behind one. A stripe has 65 536 entry slots, so 65 536
tokens hold the whole reachable set of a full directory. Eight ways per
64-byte line absorb clustering without a second probe. Two hot documents
that fall in the same set only evict each other when more than eight hot
documents share it.

The table size could instead scale with the stripe size or the expected
document count. That is an open question (section 12); the fixed size keeps
`required_size` a function of `num_buckets` alone, as it is today.

### 4.3 Lookup and store

```
check(stripe, rel_off, doc):
    n := nonce.load(acquire)                       // after admission (6.1)
    if n == 0: return false
    h := token_hash(n, salt, stripe_idx, rel_off, doc.write_serial,
                    doc.checksum, doc.len, doc.first_key[0..8])   // 48 bits, != 0
    set := &sets[fib(rel_off) >> shift]            // one 64-byte line
    for w in 0..7:
        t := set[w].load(relaxed)
        if t >> 16 == h and age_ok(t & 0xFFFF): return true
    return false

record(stripe, rel_off, doc, n):                   // n as loaded by check()
    if n == 0: return
    t := (h << 16) | current_hour_since(hour_base)
    w := first empty or expired way, else h & 7     // pseudo-random victim
    set[w].store(t, relaxed)
```

- The warm path is one acquire load (the nonce, on a read-mostly line) and
  one cache-line load. That is the same shape as today's
  `is_checksum_validated`, and cheaper than its false-miss rate.
- A store happens once per incarnation per verifier, only after a CRC pass
  that succeeded (or at commit, section 8). No reader writes on a hit, so
  the concurrency model's "a reader never writes to a shared cache line"
  holds on the warm path. First reads write once, as
  `mark_checksum_validated` does today.
- `token_hash` is a keyed 64-bit mixer, for example two rounds of a
  multiply-xorshift over the eight inputs. It is not cryptographic: an
  attacker who can write the volume file can already forge a document with
  a valid CRC-32C, which is not a MAC either.

### 4.4 Call sites

`Volume::is_checksum_validated` / `Volume::mark_checksum_validated` are
replaced at their two call sites:

- the CRC site in `Volume::read_sync`;
- the selected-alternate CRC site in `read_alternate_sync`.

In both, the readahead hint stays where it is, ahead of the check. A
consumer that touches the bytes still benefits from it.

### 4.5 Relation to `_checksum_cache`

With the shared table active on a stripe, the process-local cache is not
consulted for it. Two levels would add a second trust root for no gain,
because the shared lookup costs the same. The process-local cache remains
for in-memory `Directory` stripes and for mmap stripes with the feature off.
It has adopted the token format with a per-process salt, which replaced
the 16-bit discriminator and its 2^-16 same-offset aliasing (3.3). It
does not honour the usurp rule of 7.2; 3.3 states the residual.

### 4.6 Single-process mode

A single-process volume rebuilds an empty `Directory` on every open, so
nothing it verified can be looked up after a restart, and it has no peers.
It keeps the process-local cache, upgraded as in 4.5.

---

## 5. Keying: why (offset, pass stamp), and not `G`

A token must identify an **incarnation**: these bytes, written once. The
candidates:

- **Offset alone** (today's cache): offsets are reused every pass.
- **`G`**: it moves on every frontier advance, including advances that
  expose nothing near the document, so tokens would die needlessly. `G` is
  also a stripe property, not a document property: a document keeps its
  pass while `G` moves.
- **Pass stamp (`write_serial`) plus offset**: the fill is monotone within
  a pass, so a pass writes a given offset at most once (6.2, L1). The stamp
  is written in the same pwrite as the document (`Volume::patch_pass_stamp`)
  in both modes, and it is what admission already compares against. This
  is the key.
- **Plus `checksum`, `len` and a key prefix**: redundant under L1. They make
  a token useless for any other document even if L1 were violated with a
  different payload length, CRC or key. The 32-bit CRC field in particular
  means a violation also has to produce an equal CRC to be served.
- **Plus the nonce and salt**: they carry the lifetime (section 7) and
  separate table incarnations.

**Invalidation needs no per-document action:**

| Event | Why tokens need no action | What does act |
|---|---|---|
| Overwrite by the fill | The new document at that offset carries a new stamp. An old document partly overwritten keeps its header, but it is exposed (retention) or out of phase or at the wrong stamp (flush), and admission rejects it before the token is consulted (6.2). | nothing |
| `remove_sync` / `remove_alternate_sync` | The bytes are unchanged, so the token still says something true about them. Nothing reaches them once the entries are gone. | nothing |
| Wrap or frontier advance | The same as an overwrite. The token is never consulted for a document that fails admission. | nothing |
| Ceiling-forced advance | `G` moves before the fill. New admissions fail. Held borrows see `kTorn` exactly as today. | nothing |
| Process crash | Tokens are single words, each written after the verification it records. A lost store costs one re-verification. | nothing |
| Reboot | The page cache is gone, and persistence may have been reordered (3.1). | nonce replaced at the first exclusive open, unless the stripe is sealed (7.3) |
| fsync failure | The page cache and the device may disagree. | nonce := 0 (7.2) |
| Usurped writer possibly alive | Two documents can share (offset, pass). | nonce := 0 until the holder is proven dead (7.2) |
| Volume reset, geometry or mode change, directory re-init | The table is part of the directory region. | `MmapDirectory::init` zeroes it and draws a new salt |
| Format change (`VolumeHeader` major, `kVersion`) | A new file or a reset. | as above |

---

## 6. Ordering and the safety argument

### 6.1 Order on the read path

The check sits exactly where the CRC sits today. That is after admission
and before the borrow:

```
snapshot (G, then W)                  unchanged
probe (seqlock, acquire)              unchanged
map, is_valid, remap by len           unchanged
first_key == key                      unchanged   (invariant 8)
admit_document: stamp, exposure       unchanged
maybe_advise_readahead                unchanged
TOKEN: stamp_trusted(cls, snap, doc)  new: write_serial == expected_pass(cls, snap)
       && check(stripe, rel, doc)     new: nonce loaded HERE, after the probe
  else CRC; on success record(...)    unchanged CRC, new record
acquire_borrow, stamp_read_lease      unchanged
borrow_still_valid                    unchanged   (Dekker, invariant 3)
```

The nonce is loaded with acquire **after** the directory probe. The usurp
rule (7.2) depends on that: the usurper's `nonce := 0` is sequenced before
its first directory insert, so a reader that has seen the usurper's entry
also sees the zero, or a later fresh nonce.

### 6.2 The argument

Definitions:

- A **history** is the lifetime of one nonce value `N ≠ 0` of one stripe.
- A document **D = (o, p)** is committed at stripe-relative offset `o` in
  pass `p` = `D.write_serial`.

**L1 (uniqueness).** Within a history, at most one committed document
exists at a given (o, p), and its bytes `[o, o + len)` do not change while
an admission of it can succeed. By cases:

1. **Only the fill writes payload bytes.** The in-place writes
   (`commit_header_rmw`: hit count, last access, chain repoint) touch header
   fields outside both the CRC and the token. They are owner-only and fenced
   by the exposure check under the stripe lock.
2. **Within a pass the fill is monotone.** `W` is published only after the
   pwrite (F6) and never moves back within a history:
   - a process crash leaves the page cache intact;
   - a crashed wrap is completed, not undone (`Volume::repair_wrap_state`);
   - power loss ends the history (7.3).
   A span reserved by a writer that died before its publish is re-reserved,
   but it never got a directory entry, so it was never admitted and never
   received a token.
3. **The exception is the usurped writer.** A holder that is taken over
   without proof of death may still pwrite over the usurper's span at the
   same `W`. That makes two documents at one (o, p). Rule 7.2 keeps `N = 0`
   for the whole window in which this can happen, so no history contains it.
4. **Bytes of a committed D change only when a later pass's fill reaches
   them.** In retention mode that needs an advance past D's chunk:
   `G > (p+1)(N+1) + c`, and `admit_document` rejects D from then on
   (wrap-retention 5.3). In flush mode it needs the wrap to `p+1`: D is out
   of phase until `p+2`, and from then on its stamp `p ≠ P`, so the token
   leg refuses (the stamp condition in 6.1) and the CRC runs as it does
   today.
5. **A partial overwrite leaves D's header intact but not its tail.** That
   is the case where a token would lie. It still requires the fill to reach
   D's bytes, which is case 4.

**L2 (a token means verified).** A token `T(N, o, p, crc, len, k)` is present
only if a process that held `N`:

- CRC-verified a document with exactly those header fields, or
- was the writer, and computed that CRC over the exact buffer it pwrote
  (section 8).

The chance that some other token matches instead is at most 2^-45 per
lookup. A store computed under an older nonce cannot match under a newer
one.

**L3 (a history is coherent).** Every process that uses `N` observes the
same bytes for (o, p). Within one boot, all processes map the same page
cache. The events that break that — power loss (7.3), a writeback error
(7.2), a possibly-live usurped writer (7.2) — end the history. A reboot
continues a history only for a sealed stripe. There the data was durable
before the seal, the seal was durable before any later fill, and so the
bytes after the reboot are the bytes that were verified.

**Theorem.** A reader that skips the CRC for a document D serves bytes that
were CRC-verified (or written with that CRC) in D's current incarnation.

- The reader admitted D and checked its stamp. It found `T` under its
  nonce `N`.
- By L2, some process verified D' with the same (o, p, crc, len, k) under
  `N`.
- By L1 and L3, D' is D, with unchanged bytes.
- The borrow and `borrow_still_valid` then protect the bytes from a
  concurrent exposure exactly as they do today. The token did not
  short-circuit them.

∎

Residuals, as in 3.1:

- silent media corruption after verification (knob, 3.2);
- a 2^-45 collision;
- a 2^32-pass stamp wrap, the same as wrap retention accepts;
- a byte copy of a live volume restored in place in the same boot.

### 6.3 Invariant by invariant

1. **No reader lock:** adds loads only on the warm path. The store happens
   once per first verification.
2. **Commit ordering:** unchanged. A writer token is stored after the fill
   is published and before the insert (section 8). It is never persisted
   ahead of its data across a reboot (7.3).
3. **Dekker:** untouched. The token sits before `acquire_borrow`, and
   `borrow_still_valid` is unconditional.
4. **Seqlock:** tokens are not directory entries and bump no bucket version.
   `cross_process_ram_coherence` is unaffected.
5. **HitTracker:** untouched.
6. **Sharding:** no new RMW. The table lines are per set, and so spread by
   offset.
7. **Ownership:** readers in any process store tokens (7.1). That is
   reader-side shared state like borrow slots and the lease, not a stripe
   write. The nonce and the seal are written only by a `write_lock` holder
   or by the exclusive opener.
8. **Full-key re-verification:** runs before the token check, on every
   candidate.
9. **Positional guard:** runs before the token check, at both choke points.

---

## 7. Multi-process and crash semantics

### 7.1 Who may store a token

**Any process that verified**, reader or owner. A token certifies itself,
and the table is reader-side shared state like the borrow slots. Restricting
stores to the owner would leave pattern 2 and pattern 3 deployments (readers
of stripes they do not own) re-verifying every first read. They are the
deployments that motivate this work.

Concurrent first readers of one document in several processes may each
store the same token. Equal values, relaxed stores, no harm.

### 7.2 Nonce transitions within a boot

All under the stripe's `write_lock` except where noted, stored `seq_cst`.

| Event | Action |
|---|---|
| Escalated takeover (`WriteLockToken::escalated_takeover`) | The usurper sets `victim_pid := previous owner pid` and `nonce := 0`, **before** its first reservation. |
| Writer finds `nonce == 0 && victim_pid != 0` | If `kill(victim_pid, 0)` proves the victim dead: `nonce := fresh random`, `victim_pid := 0`. A reused pid only delays re-enabling. |
| Usurped writer sees `Busy` from `commit_write_slot` | Stores `nonce := 0` (it may not hold the lock; the store is idempotent and only ever towards "no trust"). |
| `fsync_volume` fails, in any process (`DirectorySyncer`, HitTracker flush) | That process stores `nonce := 0` on every stripe of the volume without the lock. Re-enabled only by the next exclusive open. |
| Proven-dead `forced_release` | Nothing. A dead holder's pwrite has completed or never started. |

`nonce := 0` is always safe to store without the lock: it can only make
readers verify more. Only transitions *to* a new non-zero value need the
lock (or the exclusive open), so two writers never race two different
fresh nonces.

### 7.3 Across restarts and reboots: adoption and sealing

**Boot identity:** Linux `/proc/sys/kernel/random/boot_id`, Darwin
`kern.bootsessionuuid`. Where no stable per-boot id exists (Windows, as a
first cut), every exclusive open counts as a new boot, so only sealed
stripes keep their tokens. That is conservative.

**Adoption.** This runs only in the exclusive opener (`init_stripes` with
`exclusive_open`). After a reboot no peer can hold the lifetime lock, so
the first opener of every boot is exclusive. Non-exclusive openers never
write the table header (the wrap-retention B4 rule).

```
per stripe:
    if boot_tag == this_boot and nonce != 0:  keep nonce       // restart, same page cache
    elif seal != 0:                           keep nonce       // clean stop, data durable
    else:                                     nonce := fresh   // unclean, or fsync failure
    boot_tag := this_boot; victim_pid := 0
sync_barrier(table headers)
```

When locking is degraded (no advisory locks on the filesystem) there is no
exclusive opener: the feature stays off, with the nonce at 0.

**Sealing (at a graceful `Cache::stop`, by the owner of each stripe it
wrote).**

```
snap := (G, W)
fsync(data fd)                            // every process's pwrites to the inode
msync(MS_SYNC) the stripe's table         // tokens durable; only for usefulness
lock write_lock
if (G, W) == snap:                        // no fill since the fsync
    seal := 1; sync_barrier(table header)
unlock
```

**Unsealing (before a writer's first fill of a sealed stripe).** In
`allocate_write_slot` under the `write_lock`: if `seal != 0`, then
`seal := 0` and `sync_barrier(table header)` **before** the pwrite. That is
one barrier per stripe per writer session. The seal load is one relaxed
load per write.

`sync_barrier` is `msync(MS_SYNC)`, plus `fcntl(F_FULLFSYNC)` on Darwin
(where fsync and msync do not flush the drive's write cache; see the
platform notes on `Volume::sync_directory`), and
`FlushViewOfFile` + `FlushFileBuffers` on Windows. Seal and unseal are once
per session, so the cost of a full barrier does not matter. Without it, a
drive cache could persist a fill ahead of the unseal, and a reboot would
then trust tokens for overwritten bytes.

Tokens stored by readers **while a stripe is sealed** vouch for bytes that
are already durable and cannot change until an unseal is durable, so they
survive a power loss correctly. Tokens stored while it is **unsealed** die
at the next reboot with the nonce.

### 7.4 Why not trust tokens behind the last durable checkpoint

One could record at each `DirectorySyncer` cycle a checkpoint `(G, W)`
before the fsync, and after a power loss keep only tokens for documents
older than it. That fails on the `G`-reordering case of 3.1:

- A fill after the checkpoint may have been persisted while the `G` store
  that exposed its chunk was not.
- It may have overwritten the tail of a document from before the
  checkpoint, whose header, and token, survive.
- At 1 GB/s a 1 GiB stripe wraps every second, so within one 30 s sync
  interval the whole stripe may have been rewritten.

Only a quiescent seal gives a sound statement.

### 7.5 A torn table update

There is no bitmap and no multi-word record to tear:

- **Tokens:** each is one aligned 64-bit word, stored with one atomic store.
  A lost store costs one re-verification. A word damaged on the medium
  matches nothing, except with 2^-48 probability.
- **Nonce and seal:** single words.
- **`boot_tag`:** 128 bits. It is written only by the exclusive opener,
  before its barrier, and read only by the next exclusive opener. A torn
  `boot_tag` compares unequal, and that means "new boot": conservative.
- **Crash between verify and store:** the next read verifies again.
- **Crash between store and serve:** the token is true.

---

## 8. Marking at commit

The writer computes the CRC over its own buffer when it builds the document.
The pass stamp is patched later but sits outside the CRC. The writer then
pwrites that buffer. After `commit_write_slot` succeeds, and before the
directory insert, the payload in the page cache is exactly the buffer the
CRC was computed over. So the writer may store the token there, with the
nonce loaded under the `write_lock`:

- **Same boot:** this is the same trust as a reader's first verification.
  A reader's first CRC pass after a write normally also reads the writer's
  own dirty pages out of the page cache, not the device. The one thing
  writer marking gives up is the case where the pages are evicted **before**
  the first read, so that the first read would have re-read them from the
  device. That is the media-error threat of 3.2, under the same knob.
- **Across a reboot:** the token is trusted only for a sealed stripe. The
  seal's fsync made the data durable first. This is the answer to the
  `DirectorySyncer` question: persisted tokens never depend on the syncer's
  data-before-directory order. A reboot without a seal replaces the nonce,
  and the periodic sync is irrelevant to that.
- **Failures:** on `Busy` (usurped) or a short pwrite nothing is stored. The
  usurper stores nothing either, because it runs with `nonce == 0`.

With writer marking on, `kv_bench`'s first-touch phase also stops running
the CRC, because the writer and the reader are one process. That phase
would then measure page-in only. The benchmark doc should say so when the
feature lands.

---

## 9. Knobs, defaults and the C API

C++ (`CacheConfig`):

| Field | Values | Default |
|---|---|---|
| `verified_state` | `VerifiedState::kProcess`: today's per-process cache, upgraded to tokens (4.5). `kShared`: the shared table, nonce never kept across an exclusive open (so a restart re-verifies). `kPersistent`: `kShared` plus adoption and sealing (7.3). | `kShared` in multi-process mode; single-process volumes are always `kProcess`. Recommended to start as `kProcess` for one release if Q6 goes the conservative way. |
| `mark_verified_on_write` | bool | `true` (ignored under `kProcess`) |
| `verified_state_max_age` | hours, 0 = unbounded | 0 |

All of these are per-process declarations, like
`cross_process_ram_coherence`. They are not persisted mode bits: the table
exists in every v3 directory whatever the knob says. Mixed settings across
processes are safe:

- A `kProcess` process ignores the table and stores nothing. It verifies
  as today.
- A `kShared` process trusts tokens that a `kPersistent` peer kept alive
  through a sealed reboot. The seal makes them sound, so this is fine.
- Only exclusive openers with `kPersistent` keep a nonce across an
  exclusive open. A `kShared` or `kProcess` exclusive opener replaces it.
  The strictest opener wins, which is the safe direction.
- A process with `verify_checksum_on_read = false` neither trusts nor
  stores. That setting is forced on in multi-process mode anyway.

C API (`CycloneCacheConfig`, trailing fields, zero-initialised defaults,
with the same ABI note as `disable_wrap_retention`: callers must recompile
against the new header):

```c
int verified_state;            /* 0 = library default, 1 = process,
                                  2 = shared, 3 = persistent */
int disable_mark_verified_on_write;
unsigned int verified_state_max_age_hours;   /* 0 = unbounded */
```

Statistics (`CacheStats`, mirrored in `CycloneCacheStats`):

- `checksum_verifications`: CRC passes run;
- `verified_token_hits`: CRC passes skipped on a token;
- `verified_tokens_stored`;
- `verified_state_invalidations`: nonce replacements and zeroings, by any
  cause.

A hit ratio of `verified_token_hits / (hits + verifications)` shows whether
the table is sized right.

---

## 10. Layout and version impact

- **Document format and `VolumeHeader`:** unchanged. No format-major bump.
  `sync_serial` stays unused.
- **`MmapDirectory`:**
  - `required_size` grows by the 64-byte-aligned table: 721 224 →
    1 245 632 bytes.
  - The data offset moves from 177 to 305 pages.
  - The static_asserts that pin both numbers (next to
    `Volume::has_foreign_directory_version`) change.
  - The `RetentionRegion` does not move.
- **`MmapDirectory::kVersion` 2 → 3.** `kVersion` is mixed into the
  fingerprinted file name, so a v3 build creates a fresh file and never maps
  a v2 file with the wrong data offset. A v3 opener re-initialises a
  foreign-version directory only under the exclusive lifetime lock (the
  existing rule).
- **Does it fit in the unreleased v8 / v2?** Both `Document::kVersionMajor`
  8 and `kVersion` 2 are unreleased, so the layout could be folded into v2
  with no bump and no extra cold start. It should not be. Consumers pin
  `main` commits (for example the PageSpeed `git_repository`), and a v2 file
  written by today's `main` must not be opened by a build that expects the
  table. The bump costs one cold start for users of `main` and nothing
  extra for anyone upgrading from a release: v8 already forces a cold
  start, and 2 → 3 in the same release costs no second one.
- **Upgrade and rollback:** the old binary keeps using its v2 file. The two
  files sit side by side until an operator deletes one. This is the same
  story as the v1 → v2 bump.

---

## 11. Test plan

New file `tests/integration/test_verified_state.cpp`, tag `[verified]`. The
tests run in both retention modes where that applies, and under TSan in CI.

### 11.1 Seams (compiled only under `CYCLONE_TEST_SEAMS`, like `reader_seam`)

- `ReaderSeam::kAfterVerifyBeforeRecord`: between a successful CRC pass and
  the token store.
- `ReaderSeam::kBeforeTokenCheck`: after admission, before the nonce load.
- A process-wide override of the boot identity, so a "reboot" can be
  simulated without one.
- A hook that fails `fsync_volume` once.
- The existing write-lock seams (`s_write_lock_max_live_waits_for_test`,
  presume-dead), which make an escalated takeover reachable.

### 11.2 Cases

1. **Shared across processes.** Process A writes and reads a document.
   Process B (fork) reads it: `verified_token_hits == 1` and
   `checksum_verifications == 0` in B. Flip one payload byte through a raw
   pwrite: B still hits the token (this documents the 3.2 trust, as a
   deliberate assertion), and with `verified_state_max_age` elapsed (clock
   seam) B verifies and reports `Corrupted`.
2. **Crash between verify and record.** Kill the reader at
   `kAfterVerifyBeforeRecord`. The next read verifies again and stores the
   token. No spurious hit.
3. **Overwrite racing a stored token (retention).** Store a token for D at
   (o, p). Advance and wrap until the fill overwrites D's tail but not its
   header. This needs a document that starts inside D's payload: construct
   it with a reserved-span gap. Reading D must fail admission (exposure),
   never serve from the token.
4. **The same in flush mode.** Two wraps, D's header intact in a
   failed-pwrite gap, D's tail overwritten. The token check refuses on the
   stamp (`write_serial != P`), the CRC runs, and the result is a miss or
   `Corrupted`, never the torn bytes. This is the case that pins the 6.1
   stamp condition.
5. **Wrap and advance leave tokens inert.** After a wrap, a new document at
   the same offset with the same key and length: the token of the old one
   does not match (different stamp). The CRC runs once.
6. **Usurp.**
   - Force an escalated takeover while the victim is parked mid-pwrite.
     Assert `nonce == 0` from the takeover on, and that no token is stored
     while it is 0.
   - Let the victim's late pwrite land, including a short write through a
     pwrite-failure seam. A reader in every process gets either a
     CRC-consistent document of the requested key or a miss/`Corrupted`.
     That includes a process that verified the usurper's document before
     the tear.
   - Kill the victim: the next write restores a non-zero nonce.
   - The same case with `kProcess`, and a CRC forced to collide in its top
     16 bits through a test-only checksum override, pins the 3.3 fix to
     `_checksum_cache`.
7. **fsync failure.** Fail `fsync_volume` once: every stripe's nonce is 0,
   reads verify, and nothing is stored until an exclusive reopen.
8. **Restart in the same boot.** `kPersistent`: close all processes without
   sealing (SIGKILL), reopen, and read: token hits. `kShared`: a reopen
   replaces the nonce and reads verify.
9. **Reboot, unsealed.** Override the boot id and reopen exclusively: new
   nonce, no hits.
10. **Reboot, sealed.** Graceful stop (seal), override the boot id, reopen:
    token hits. Then a writer fills once. Assert that the unseal barrier ran
    before the pwrite (seam counter), and that after another simulated
    reboot without a seal the nonce is replaced.
11. **Seal skipped under a racing fill.** Fill between the seal's fsync and
    its `write_lock`: the seal is not set.
12. **Mixed knobs.** A `kProcess` process beside a `kShared` process: the
    first stores nothing, the second trusts only its own and peers' tokens.
    A `kShared` exclusive opener replaces a nonce that a `kPersistent`
    process had kept.
13. **Collision behaviour.** Nine documents forced into one set with a
    set-index seam: ways are evicted, the evicted document verifies again,
    and nothing false is served.
14. **Layout.** static_asserts for `required_size` and the 305-page data
    offset. A v2 file is left alone and a new fingerprinted v3 file is
    created beside it.
15. **Chain nodes.** `read_alternate_sync` serving a non-head alternate
    stores and hits a token for the node's own offset.

### 11.3 Guards that must stay green

- `test_lockfree_read_races.cpp` (invariant 1)
- `test_power_loss.cpp` (invariant 2): extended so that tokens persisted
  before a simulated power loss are never trusted afterwards without a seal
- `test_wrap_phase_aba.cpp` and `test_tag_collision.cpp` (invariants 8, 9)
- `multi_process_test.cpp` (invariant 7)
- `test_wrap_retention.cpp`
- the F6-F usurp test

### 11.4 Benchmark acceptance

`kv_bench` macOS `restart` and `multiprocess_read` at 512 KiB and 2 MiB,
and Linux with `--drop-caches-cmd` omitted for a warm-restart variant.
Expect the first-pass read time to fall by roughly the "avoidable" columns
of section 2. On Linux cold, expect it within noise. No warm-read
regression beyond noise in `concurrent_read_bench`.

---

## 12. Rollout and open questions

**Rollout.**

1. `_checksum_cache` to tokens, plus the usurp rule of 7.2. Process-local,
   no format change. Safe to land alone, and it closes the 2^-16 aliasing
   of 3.3.
2. The table, `kVersion` 3, `kShared`, `mark_verified_on_write`. The
   default depends on Q6.
3. `kPersistent`: adoption, sealing and barriers, behind the knob. Soak it
   in the PageSpeed `cache_burst_test` and in a crash loop (SIGKILL plus
   simulated reboots) before any default flip.
4. Correct the benchmark doc's "What to change" row: the 512 KiB cold gap
   needs its own item (readahead ahead across documents).

**Open questions.**

- **Table sizing.** A fixed 8192 × 8 per stripe costs 512 KiB per stripe
  whatever the stripe size. PageSpeed's 32 MiB stripes of mostly 4 KB
  objects hold up to about 8 000 documents, which the fixed table suits.
  A 1 GiB stripe of 2 MiB KV blocks holds 512 documents and would do with
  a 64th of it. Scale with `num_buckets` (today's rule) or with the stripe
  size?
- **Windows boot identity.** Every exclusive open is treated as a new boot
  until a reliable per-boot id is chosen. Is that acceptable for the first
  cut?
- **Small documents.** A token lookup is one cache-line load, which can
  miss, at roughly 100 ns. A CRC of 1–2 KiB costs about the same. Should
  documents below a threshold skip the table entirely, both lookup and
  store, so the table's capacity goes to the large ones?
- **Scrubbing.** Should a background low-priority re-verification (reusing
  the optimization engine's pool) replace `verified_state_max_age`, turning
  bit rot into an early `Corrupted` rather than a late one?
- **The 3.3 weakness.** It should be confirmed with a targeted test on
  today's tree (a forced top-16 CRC collision at a reused offset) before
  phase 2 starts, so the fix lands with a regression test.

---

## 13. Risks

| # | Risk | Mitigation |
|---|---|---|
| R1 | **L1 has an unseen exception.** A path other than the usurp that writes payload bytes of an admissible document would turn a torn document from "detected" into "served". | The stamp condition in 6.1 in both modes. An audit of every write into the data area is part of phase 2. There are two document pwrite sites, `Volume::commit_write` and `Volume::commit_alternate_write`, and the header-only `commit_header_rmw`. Test cases 3, 4 and 6. A debug build option that runs the CRC anyway on a token hit and asserts equality, left on in the TSan and ASan CI jobs. |
| R2 | **Reordered persistence across a reboot** (drive cache, `F_FULLFSYNC` on Darwin, a filesystem that does not order msync). | Tokens cross a reboot only through a seal, whose barriers are full-cache flushes. Anything else replaces the nonce. `kPersistent` is opt-in until soaked. |
| R3 | **Trust in the medium is extended in time and across processes** (3.2), and an externally restored byte copy of a live volume in the same boot would be trusted. | The default keeps today's trust class. `verified_state_max_age` bounds it. The restore case needs someone to overwrite a live cache file in place, which already breaks the lifetime-lock and inode assumptions. |
| R4 | **The layout bump** costs a cold start for users of `main`, and 512 KiB per stripe. | Ship it in the same release as v8 and `kVersion` 2, so it adds no cold start to a release upgrade. Table sizing is an open question. |
