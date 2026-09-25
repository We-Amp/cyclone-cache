# Multi-Process Stripe Affinity

Cyclone Cache supports multi-process deployment where multiple processes share the same cache files. This document describes the model, configuration, and deployment patterns.

## Overview

Multi-process mode enables **write sharding** with **shared directory** across multiple processes:

- **Each process owns specific stripes** (assigned via round-robin)
- **Only the owning process can write** to its assigned stripes
- **All processes can read from all stripes** (directory is shared via mmap)
- **Writes to non-owned stripes are rejected** with `NotOwned` error
- **Torn reads are detected** via CRC-32C checksum validation
- **No inter-process locking** is required

This design provides write isolation, shared cache visibility, and torn-read safety for multi-process deployments.

## Configuration

### Basic Setup

```cpp
#include <cyclone/cache.hpp>
#include <cyclone/config.hpp>

// Configure for process 0 of 4 processes
CacheConfig config;
config.set_multi_process(0, 4);  // process_index=0, total_processes=4

auto cache = Cache::create(config);
cache->add_volume("/shared/cache.dat", 10_GB);
cache->start();
```

### Detailed Configuration

```cpp
MultiProcessConfig mp_config;
mp_config.set_enabled(true)
         .set_process_index(0)       // This process's index (0 to N-1)
         .set_total_processes(4)     // Total number of processes
         .set_max_read_retries(1);   // Retries on torn read detection

CacheConfig config;
config.set_multi_process_config(mp_config);
```

### Configuration Parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| `enabled` | `false` | Enable multi-process mode |
| `process_index` | `0` | This process's index (0 to total_processes-1) |
| `total_processes` | `1` | Total number of processes sharing the cache |
| `max_read_retries` | `1` | Maximum retries when a torn read is detected |

`CacheConfig` carries one further multi-process-relevant knob, outside
`MultiProcessConfig`:

| Parameter | Default | Description |
|-----------|---------|-------------|
| `cross_process_ram_coherence` | `false` | Validate every RAM-cache hit against the shared directory's bucket version, so a peer's re-record or purge is not served from this process's RAM tier. See [Cross-Process RAM Coherence](#cross-process-ram-coherence) below. |

### Requirements

- **Checksums must be enabled** (`enable_checksum = true`, the default)
- **Valid process index**: `process_index < total_processes`
- **All processes must use the same** `total_processes` value
- **All processes must share one host**, and the volume file must support
  byte-range locks (local filesystems do; see
  [Writer liveness](#writer-liveness)).
- **Sharing one PID namespace is recommended, no longer required.** Processes
  in different PID namespaces, such as two containers sharing the volume, are
  safe with each other: a writer's liveness is a lock on the volume file, not
  its PID. Keep one namespace while any process sharing the volume runs a
  build from before issue #32, which still decides liveness with
  `kill(pid, 0)` / `OpenProcess` (see [Writer liveness](#writer-liveness)).

## Architecture

### Mmap-Backed Directory

When multi-process mode is enabled, the cache directory is stored directly in the cache file via memory-mapped I/O:

```
Cache File Layout (multi-process mode):
┌────────────────────────────────────────┐
│ Volume Header (64 bytes)               │
├────────────────────────────────────────┤
│ Stripe 0:                              │
│   ├─ MmapDirectory Header (64 bytes)   │
│   ├─ Version Counters (4B per bucket)  │
│   ├─ Directory Entries (4 entries ×    │
│   │   10B = 40B per bucket)            │
│   ├─ Retention Region (264 bytes,      │
│   │   8-aligned): exposure gen G +     │
│   │   64 per-chunk borrow slots (u32)  │
│   └─ Data Area (page-aligned)          │
├────────────────────────────────────────┤
│ Stripe 1: ...                          │
└────────────────────────────────────────┘
```

The directory region is `MmapDirectory::required_size(buckets)` bytes, rounded
up to a page. For the default 16384 buckets that is 721 224 bytes, so the data
area still starts 177 pages into the stripe. The retention region was added in
directory **version 2** (`MmapDirectory::kVersion`). It holds the stripe's
exposure generation `G = P * (N + 1) + f` and one `{generation:8, count:24}`
borrow slot per frontier chunk. It replaces the version-1 stripe-wide borrow
slot at header offset 34. Those two bytes now hold the phase lock's takeover
generation (see [Cross-Process Writer Locks](#cross-process-writer-locks)).
`G` also replaces
the version-1 reader epoch, `{shared_wrap_count, current_phase}`. See
[architecture.md](architecture.md#eviction-and-wrap-retention).

`kVersion` is mixed into the fingerprinted file name of mmap volumes. A
version-2 build therefore never opens a version-1 file. It creates a fresh one
next to it, so the first start after the upgrade is a cold cache. The old file
is left for the operator to delete. A v2 opener re-initializes a directory
that carries a foreign version only when it holds the exclusive lifetime lock.

The `MmapDirectory` provides:
- **Cross-process visibility**: All processes see the same directory entries
- **Lock-free reads**: Seqlock pattern with version counters per bucket
- **Torn read detection**: Version mismatch triggers retry
- **Persistence**: Directory survives process restart

### Single-Process Mode (Default)

When multi-process mode is disabled (default), an in-memory directory is used:
- Faster (no mmap overhead)
- Directory lost on process restart
- No cross-process sharing

## Stripe Ownership

Stripes are assigned to processes using round-robin:

```
stripe_owner = stripe_index % total_processes
```

For example, with 4 processes and 8 stripes:
- Process 0 owns stripes: 0, 4
- Process 1 owns stripes: 1, 5
- Process 2 owns stripes: 2, 6
- Process 3 owns stripes: 3, 7

### Write Operations

When a process attempts to write to a non-owned stripe, the operation returns `CacheError::NotOwned`:

```cpp
auto result = cache->write_sync(key, content_length);
if (!result && result.error() == CacheError::NotOwned) {
    // This key maps to a stripe owned by another process
    // Option 1: Forward the write request to the owning process
    // Option 2: Use a different cache key
}
```

### Cross-Process Writer Locks

Normally one process owns a stripe. During a graceful reload (nginx, Apache)
or an overlapped recycle (IIS), an old and a new process can both own it, so
writers in two processes contend on the stripe's shared locks: the write
lock (write-cursor reservation), the phase lock (an insert's phase read and
entry store against a phase toggle) and each directory bucket's seqlock.

A waiter spins for about 20 µs, then sleeps with backoff (10 µs doubling to
1 ms), spinning briefly after each wake. It treats a holder as stuck only
when that same holder kept the lock for the whole budget and then for a
further 2 ms of continuous polling. A new holder, or the lock seen free,
restarts the budget. Each bucket acquisition takes a new version. Each
phase-lock and write-lock acquisition bumps that lock's generation counter.
So a peer that releases and re-acquires at once still counts as a new
holder.

| Lock | Budget | After the budget |
|------|--------|------------------|
| Bucket seqlock | 250 ms | The waiter forces the bucket to even. |
| Phase lock | 1 s | The waiter recovers the lock. |
| Write lock | 5 s | Once one holder has kept the lock for 50 ms, the waiter checks after every sleep whether the holder's liveness lock is gone (see [Writer liveness](#writer-liveness)), and recovers the lock if so. It takes over a holder it cannot prove dead only after the budget. |

The budgets are far above the longest live hold measured with 5.2 runnable
threads per core (56 ms for a bucket, 112 ms for the phase lock). A holder
that is only descheduled is therefore waited out. A larger budget only
lengthens the one-time stall after a process died holding a lock.

These waits run on the writer side only: the stripe mutex is held, and a
phase-lock wait can also run inside a wrap while the write lock is held.
They are still reachable inline from a request thread. For example, the
PageSpeed nginx module writes an alternate through `write_sync` /
`close_sync` on its event loop, and the C API's miss-handler write-back
also writes. Under load, a write waits out a live holder instead of taking
the lock from it: about 106 ms at worst in the measurement above.

The locks are not fair. A peer that releases and re-acquires within
nanoseconds wins against a sleeping waiter every time. Such a peer is never
taken over, because every acquisition is a new holder. So that it cannot
stall a writer indefinitely either, most waits are **capped**:

- **What gives up:** inserts, removes, hit-count updates and write-slot
  reservation.
- **When:** once the holders a waiter has seen come and go add up to
  `kLockWaitCap` (250 ms), the waiter gives up.
- **What happens then:** the operation returns `CacheError::Busy`
  (`CYCLONE_BUSY` in the C API) and publishes nothing. An abandoned write
  leaves its document unreachable, like any fill whose insert never ran.
  The cap never takes a lock over.

Time spent on the current holder does not count toward the cap, so a stuck
holder is still governed by its per-holder budget alone, and a dead holder
is still recovered. The cap is 250 ms because that is over twice the longest
total phase-lock wait measured at 5.2 runnable threads per core (103 ms),
so ordinary contention does not give up.

The longest one capped acquisition can wait is about the cap plus one
holder's budget plus the 2 ms confirmation:

| Lock | Longest capped wait |
|------|---------------------|
| Bucket seqlock | About 0.5 s. |
| Phase lock | About 1.25 s. An insert takes the phase lock, then a bucket, so an insert can wait about 1.75 s. |
| Write lock | About 0.3 s behind a dead holder. About 5.25 s behind a live holder it cannot prove dead, the pathological case the escalation exists for. |

Three waits stay uncapped, because giving up there would leave work half
done:

- the phase toggle and the phase re-derivation, which run inside a wrap
  under the write lock (a peer needs that lock before it can start another
  insert, so contention drains);
- `clear()`;
- the RAM-coherence signal published after an alternate's chain was
  already repointed.

Every release is a CAS on the holder's own token. A holder that was
recovered from under it and resumes later cannot free the next holder's
lock. A usurped bucket or phase-lock holder can at worst publish a torn or
stale directory entry. Full-key verification, the positional guard and the
CRC turn that into a miss. The write lock guards overlapping writes, which
readers cannot detect, so it never presumes a live holder stuck.

In a mixed-version overlap, a build from before issue #27 still takes over
the phase lock after about 33 µs, and it releases both locks and takes them
over with a blind store of 0. Builds of the same format still exclude each
other in normal locking, because both treat any nonzero lock value as held.
During an upgrade overlap, the pair therefore behaves like the older build,
and no worse than before.

### Writer liveness

A write-lock holder that dies must be recovered quickly, and a live one must
never be taken over: its overlapping write is the one failure readers cannot
detect. The waiter therefore needs a proof of death that holds in any PID
namespace. A PID is not one: to a process in another namespace, a live
holder's PID reads as gone, or names an unrelated live process.

**Slots.** Each process that opens a multi-process volume claims one of
**251 liveness slots**: bytes `0x7FFFFFFE00000000 + slot` of the volume
file, far past any data and below the two reset-gate lock bytes. It holds a
byte-range lock on its slot for as long as it lives. The kernel drops the
lock when the process dies, whatever namespace or container it ran in,
because the lock belongs to the file, not to a process ID.

**The token names the slot.** A holder stores a lock token that encodes its
slot. For each generation of the lock, the 252 values 2 to 253 form a ring:
the plain token (what a holder without a slot, and every older build, stores)
takes one position, and the 251 slot tokens take the others. So a slot token
never equals the plain token of the same generation, and never 0 (free), 1
(the only value builds before #27 store) or a recovery claim (254, 255). No
header bytes are added. The owner PID stays where it was, for older builds.

**The probe.** A waiter that has watched one holder for 50 ms decodes the
slot from the token and generation it read, and asks whether any process
holds a lock on that slot's byte:

| Platform | Claim | Probe |
|----------|-------|-------|
| Linux, macOS | on a descriptor of its own, reopened read-only by path and checked to be the volume's inode: `F_OFD_GETLK` that the slot is free, `F_OFD_SETLK` `F_RDLCK`, then `F_OFD_GETLK` again to back off from a racing claimer | `F_OFD_GETLK` on the volume descriptor, which holds no slot lock |
| Windows | exclusive `LockFileEx` on the volume handle | an exclusive `LockFileEx` try, undone at once when it succeeds |

The holder is dead iff no process holds its slot. A live holder can never
look dead. The one way a dead holder looks alive is that a new process
claimed its slot in the moments since it died; the waiter then falls back
to the 5 s escalation, the safe direction. Claims start at a pseudo-random
slot to make that rare. Nothing depends on PID numbers, so two containers
whose processes share PID numbers (every container's first process is
pid 1) do not confuse each other.

A forked child (an nginx or Apache worker of a master that opened the
volume) claims a slot of its own at its first write-lock acquisition, on a
descriptor of its own, so its lock dies with it and not with the family.
The claim reopens the file read-only, so a worker that dropped privileges
can still claim.

**Keep the slot's descriptor open.** On Linux and macOS the slot lock lives
on a descriptor the cache opened. An application that closes every
descriptor after opening the cache (a `closefrom()`-style daemonize step, or
a child that closes inherited descriptors) drops its slot lock while it
still stores slot tokens, and a waiter could then take over one of its live
write-lock holders. Open the cache after daemonizing, and never close
descriptors behind its back. `fork()` itself is safe: the claim, probe and
release run under one process-wide mutex that a `pthread_atfork` handler
takes around `fork()`, so a child never inherits it locked; everything done
under it is non-blocking (`F_OFD_GETLK`, `F_OFD_SETLK`, never `SETLKW`), so
a fork waits for at most a few syscalls.

**Network filesystems.** Byte-range lock behaviour on NFS and SMB shares is
unverified. The one-host requirement above already rules out sharing a
volume between hosts; on a network mount used by one host, a claim that
fails leaves the process without a slot (5 s escalation), but a server that
accepts locks without keeping them across a client or server restart is
not detected.

**Without a slot.** A process that holds no slot (all 251 taken,
byte-range locks unsupported on the filesystem, the file replaced by name)
stores the plain token. A waiter never probes such a holder: it is never
proven dead, and if it dies holding the lock the waiter recovers it only by
the 5 s escalation. `VolumeStats::write_lock_liveness_unregistered` reports
that state.

**Cost.** A claim costs one `open` and a few `fcntl` calls per process and
volume (Windows: one `LockFileEx` or a few). The uncontended acquisition
makes the same `getpid()` call as before plus one atomic load; the probe
runs only on the contended path, after 50 ms behind one holder.

**Mixed builds.** Builds from before issue #32 keep deciding liveness with
`kill(pid, 0)` / `OpenProcess`, on the PID the new build still publishes. So
during an upgrade overlap:

- an older waiter behind a new holder behaves exactly as before: safe in
  one PID namespace, and able to take over a live holder in another one;
- a new waiter behind an older holder (plain token) never probes it: it
  never takes over a live one, and recovers a dead one only after the 5 s
  escalation instead of within about 50 ms.

Keep all processes in one PID namespace until every build sharing the
volume includes this change.

### Read Operations

All processes can read from all stripes. Reads from non-owned stripes:
- Skip mutex acquisition (optimization: no local writers)
- Validate checksums to detect torn reads
- Retry on checksum failure (up to `max_read_retries` times)

## Cross-Process Cache Sharing

With mmap-backed directories, **cross-process cache sharing works automatically**:

```cpp
// Process A (owner of stripe 0)
CacheKey key("shared-data");  // Assume this maps to stripe 0
auto write = cache->write_sync(key, data.size());
write->write_sync(data);
write->close_sync();

// Process B (not owner of stripe 0) - can read immediately
auto read = cache->read_sync(key);  // Returns the data
```

### How It Works

1. **Process A writes**: Updates mmap'd directory entry
2. **Directory visible to all processes**: Changes appear immediately via mmap
3. **Process B reads**: Finds entry in shared directory, reads data from disk
4. **Checksum validation**: Ensures data integrity across processes

## Torn Read Detection

When a reader reads a document while another process is writing, the data may be partially updated (torn read). Cyclone detects this via multiple mechanisms:

### Directory Level (Seqlock)

1. Reader loads version counter for bucket
2. Reader reads directory entries
3. Reader loads version counter again
4. If the version was odd or changed, retry: 100 retries with no clock read,
   then sleeping retries (10 µs doubling to 1 ms) for up to
   `SeqlockReadWait::kBudget` (5 ms), so a writer descheduled mid-update is
   usually waited out without burning the reader's CPU
5. If the bucket is still busy after that, the lookup returns
   `CacheError::Busy` (`CYCLONE_BUSY`): the key's presence is unknown, which
   is not reported as a miss. The next write, delete or hit-count update of
   a key in that bucket by the process that owns the stripe releases a
   holder that is stuck (dead, or descheduled for too long), then proceeds.
   Bucket releases are token-checked, so a usurped holder that wakes up
   later cannot turn the bucket odd again.

### Data Level (CRC-32C)

1. Reader maps the document region
2. Reader validates the CRC-32C checksum
3. If checksum fails:
   - Continue probing for the next candidate
   - After exhausting candidates, yield and retry
4. If all retries exhausted, treat as cache miss

This provides **eventual consistency**: a torn read is treated as a temporary miss, not a corrupted read.

## Deployment Patterns

### Pattern 1: Request Routing by Key Owner (Recommended)

Route all requests for a key to the process that owns it:

```
                         ┌─────────────────────┐
                         │   Load Balancer     │
                         │ (routes by key hash)│
                         └─────────┬───────────┘
                                   │
              ┌────────────────────┼────────────────────┐
              ▼                    ▼                    ▼
     Worker 0 (stripes 0,3)  Worker 1 (stripes 1,4)  Worker 2 (stripes 2,5)
              │                    │                    │
              └────────────────────┴────────────────────┘
                                   │
                            Cache File (shared)
```

Each worker handles all operations (read/write) for its owned keys:
```cpp
uint32_t owner = key.segment_hash() % total_processes;
if (owner != my_process_index) {
    return forward_to_worker(owner, request);
}
// Handle locally - both reads and writes
```

**Advantages**: Simple, efficient, no cross-process reads needed
**Trade-off**: Requires intelligent load balancer or request forwarding

### Pattern 2: Write Sharding with Shared Reads

All processes accept reads (from shared directory), forward writes to owners:

```cpp
// Any process can read from shared cache
auto read_result = cache->read_sync(key);
if (read_result) {
    return serve_from_cache(*read_result);  // Hit from any stripe
}

// Miss - need to fetch and cache
uint32_t owner = key.segment_hash() % total_processes;
if (owner != my_process_index) {
    // Forward write to owner (they have write permission)
    return forward_fetch_request(owner, key);
}

// We are the owner - fetch and cache
auto data = fetch_from_origin(key);
auto write = cache->write_sync(key, data.size());
write->write_sync(data);
write->close_sync();
return serve_data(data);
```

**Advantages**: Any process can serve cache hits locally
**Trade-off**: Write path requires inter-process communication

### Pattern 3: Asymmetric - One Writer, Many Readers

One process handles all writes:

```cpp
// Writer process (process 0, total_processes=1 for writes)
// Or: owns all stripes

// Reader processes open the same file with multi-process mode
// They can read anything written by the writer
CacheConfig reader_config;
reader_config.set_multi_process(1, 2);  // Process 1, can read but not write
```

**Advantages**: Simple write logic, reads scale horizontally
**Trade-off**: Writer can become a bottleneck

## Limitations

### No Inter-Process Push Notification

When one process writes a new version of a document:
- Other processes see the update immediately (mmap'd directory)
- But there's no explicit notification mechanism — nothing is pushed, and a
  process only learns of the change when it next looks

The **RAM-tier half** of this limitation is now fixable with one knob: set
`CacheConfig::cross_process_ram_coherence` and this process stops serving a
peer's superseded bytes out of its own RAM cache. See below. The **push half**
still stands: there is no callback, no invalidation channel, and a process
that never reads a key never learns anything about it. Application-layer
signalling remains the answer when you need a peer to *act* on a change rather
than merely not serve stale bytes.

### Cross-Process RAM Coherence

**The exposure.** The mmap'd directory is shared; the RAM cache is not. It is
a process-local heap structure, and the guard that covers this race within one
process (`Stripe::remove_epoch`) is process-local by design. So when a peer
re-records or purges content, this process keeps serving the superseded bytes
from RAM until its own LRU/CLFUS happens to evict them — silently, with no
error to observe.

**The fix, opt-in.** With `cross_process_ram_coherence` set, every RAM entry
is stamped at admission with the shared seqlock version of the directory
bucket its key hashes to, and every RAM hit revalidates that stamp against the
bucket's current version. A mismatch drops the entry and falls through to
disk.

```cpp
CacheConfig config;
config.set_multi_process(0, 1);              // the mmap directory
config.set_cross_process_ram_coherence(true);
// ...
assert(cache->cross_process_ram_coherence_active());  // it took effect
```

**It is a LOCAL declaration, not a protocol.** Set it per process,
independently. No peer needs the same setting, no peer needs restarting, and
no peer even needs a binary that knows the feature exists: the signal it reads
is the per-bucket seqlock version that every directory mutation has always
published.

| This process | Peer process | Outcome |
|---|---|---|
| ON | ON | Both coherent |
| ON | OFF | **This process is coherent.** The peer publishes the bump either way |
| OFF | ON | This process behaves exactly as before; the peer is coherent |
| OFF | OFF | Exactly the historical behaviour |

There is no combination in which a process believes it is protected and is
not.

**Enable it in a process iff all of:**

1. `multi_process_config.enabled` — it is inert otherwise, **and**
2. this process has a RAM cache (`ram_cache_size > 0`), **and**
3. another process re-records or purges content this process reads, **and**
4. this process does not already invalidate out of band — a targeted
   `Cache::evict_from_ram_cache`, or a write partitioning that guarantees no
   peer ever re-records an id this process caches.

Point 4 is not hypothetical: an application that has already solved coherence
at its own layer gets **zero** benefit and pays the cost, which is why the
default is OFF and why the library cannot derive the answer for you. Nothing
observable distinguishes "needs coherence" from "already handles it".

**Pattern 3 (one writer, many readers) is precisely the shape that wants
this**: the readers set it, the writer need not, and the writer commonly runs
with no RAM tier at all.

**The cost: bucket granularity.** The signal is per BUCKET, not per entry. Any
directory write to the bucket a key hashes to — an unrelated insert, an
eviction, a collision replacement — invalidates this process's RAM copy. With
16384 buckets per stripe and a uniform write stream of *W* directory mutations
per second into a stripe, a RAM entry that has sat idle for *T* seconds
survives with probability about e^(-WT/16384):

| W (writes/s/stripe) | T = 10 ms (hot key) | T = 1 s | T = 60 s |
|---|---|---|---|
| 100 | ~100% | ~99.4% | ~69% |
| 1,000 | ~99.9% | ~94% | ~2.6% |
| 10,000 | ~99.4% | ~54% | ~0% |

Hot keys are essentially unaffected. Warm and cold entries in a write-active
cache are dropped often — and a dropped entry costs more than a plain miss
(the hit path has already taken the RAM lock and copied the document before
the entry is discarded). Measure it on your own workload rather than guessing:

```
true_served_ram_hits    = ram_cache_hits - ram_coherence_rejections
false_invalidation_rate = ram_coherence_rejections / ram_cache_hits
```

Both counters are on `CacheStats` (and mirrored in the C API's
`CycloneCacheStats`). A high ratio means bucket granularity is costing you;
the honest responses are a smaller RAM tier, better write partitioning, or
turning the knob back off. Rollback is exactly that — nothing persists.

**What it does NOT cover.** Two shared-state changes produce no bucket-version
bump and are deliberately out of scope, because neither is superseded content:

- **The GC phase toggle** — a directory-wide logical eviction. No version
  changes; peers keep serving their RAM copies of content that is still
  correct for what it holds.
- **A write-buffer wrap** — overwrites document bytes. A RAM entry is a
  snapshot taken when the bytes were live and remains a correct answer for the
  content it holds.

**One operational surprise.** `read_sync` validates RAM hits but never
*populates* the RAM tier (only the alternate read path puts). Under this
toggle a `read_sync`-dominant workload therefore progressively **drains** its
RAM tier: stale entries are rejected and removed, and refill depends on
`read_alternate_sync` traffic. Correct, but worth knowing before you read the
hit-rate graph.

**No format change.** The bucket-version array predates this feature;
`VolumeHeader::kFormatVersionMajor` and `MmapDirectory::kVersion` are both
untouched by it, so adopting the knob costs no cold cache and needs no
migration. (`MmapDirectory::kVersion` moved to 2 later, for wrap retention;
see "Mmap-Backed Directory" above.)

### No Migration on Process Count Changes

When `total_processes` changes:
- Stripe ownership changes instantly
- Old data remains readable by any process
- New writes go to the new owner
- No automatic migration of existing data

### Stripe Granularity

Keys are assigned to stripes based on hash:
- Cannot control which process owns a specific key
- For predictable distribution, use consistent key naming

### Small-Object Tier

When `CacheConfig::small_tier_percent > 0` (see the API reference), the
small-object volume's size floor scales with `total_processes`: it always
gets at least `total_processes` minimum-size (128 MB) stripes, so **every
process owns at least one writable small-tier stripe**. This matters because
writes to non-owned stripes are rejected with `NotOwned` and dropped by
contract — with fewer stripes than processes, some processes could never
write small-tier entries at all.

Two hard requirements for multi-process deployments:

- **All processes MUST pass identical `small_tier_percent` and total size.**
  The stripe geometry of both volumes is derived from them; divergent values
  would compute different layouts for the same shared files.
- **Geometry changes reset the volume.** `Volume::open()` validates the
  configured size against the on-disk header in multi-process mode; enabling
  or changing `small_tier_percent` (or the total size) on an existing
  multi-process cache therefore resets the affected volume's data — a
  one-time cost (entries are cache data), and strictly better than silent
  cross-process divergence. With `auto_reset_on_incompatible = false`, the
  open fails with `IncompatibleVersion` instead.

### ReadHandle Lifetime

ReadHandles hold references to memory-mapped regions. Disk-hit handles are
pinned by **per-thread read anchors** (`kReadAnchorShards = 64` per volume):
each anchor holds a strong `shared_ptr` to the Volume and its MappedFile plus a
`torn` latch. `Cache::stop()` latches every anchor `torn = true` before freeing
stripes, and handles gate their stripe-touching paths on it — so ReadHandles
are safe to hold after `cache->stop()`:

```cpp
auto read_result = cache->read_sync(key);
// Use the handle...

cache->stop(); // Safe - the read anchor keeps the mapping alive,
               // and its torn latch fences off freed stripe state

// read_result destructor runs here - no crash
```

(Volumes not owned by a Cache — e.g. constructed directly in unit tests — fall
back to a `weak_ptr`-based path with the same safety property.)

While this is now safe, best practice is still to release handles promptly to free mapped memory.

### Borrow Safety Under Eviction (Read Leases)

The seqlock + CRC-32C gauntlet protects bytes only *inside* `read_sync`; a
borrowed `mapped_view()` span outlives it.  Lease-based region pinning protects the borrow itself: every disk-hit read stamps
a shared per-stripe lease (`CacheConfig::read_lease_duration`, default 5s)
in the mmap-directory header (offset 56). It also counts itself in the
shared borrow slot of its document's chunk (retention region). A writer that
must expose chunks the forward fill will overwrite defers that step (dropping
the fill) while any of those chunks holds a live borrow under an unexpired
lease, in every process sharing the file. In flush mode the step is the wrap,
and the stripe is one chunk. Under wrap retention the step is a frontier
advance over one or a few chunks. Readers revalidate after stamping. The
shared wrap-intent flag (offset 33, held by the writer across the whole
step) must be clear, and the stripe's exposure generation `G` must not have
passed the document's threshold. A step that raced, or is racing, the probe
is therefore always detected, and the read retries or misses instead of
returning soon-to-be-overwritten bytes. The intent-flag pairing makes this
detection exact within the lease window (a hard guarantee), not best-effort.

A writer that dies inside that window leaves the intent flag set, and every
read of the stripe would then retry. The flag is repaired by the next
`forced_release` that proves the holder dead, or by an open that holds the
exclusive lifetime lock. It is never repaired on an escalated takeover of a
holder that may still be running. A wrap marks the intent byte with its
target pass (`2`/`3`) before it lowers the shared cursor, and recovery
completes such a wrap (cursor, phase, `G`) rather than just clearing it.

**Eviction mode must match across processes.** `CacheConfig::wrap_retention`
(default `true`, retention; `false` is flush) is persisted in the volume
header (`VolumeHeader::retain_chunks`, offset 40; 0 means flush). The mode is
not in the fingerprinted filename, so processes in different modes resolve
to the same file. An open whose configured mode disagrees with the file goes
through the same live-peer reset gate as a format change. While another
process holds the volume the open is refused (`ResetRefusedLivePeer`), so a
mixed-mode deployment fails fast instead of corrupting the other side's
view. With no holder, the opener resets the file cold into its own mode.

This matters for upgrades. Volumes created by a build whose default was
flush record `retain_chunks = 0`. During an overlapping upgrade, a new
default-configured process that starts while old processes still hold the
file is refused until they exit. The first one to open after that resets the
cache cold. Either stop every old process before starting new ones, or run
the new binary with `wrap_retention = false` (C API:
`disable_wrap_retention = 1`) to keep the existing cache. See
[api-reference.md](api-reference.md#wrap-retention).

The guarantee is bounded: holds longer than
`CacheConfig::lease_wrap_ceiling` (default 60s) are not protected — a
continuously deferred wrap or mandatory frontier advance is eventually forced
(observable via the `wraps_forced_past_lease` counter).  Long holders must call
`ReadHandle::renew_lease()` at a cadence of at most 3/4 of the lease
duration and keep the total hold below the ceiling, or copy the bytes.

Scope note: the lease guards circular-buffer *wraps* over content regions.
In-place metadata updates on a live document's *header* fields (hit-count
updates, alternate-chain next-offset edits) are not lease-gated; the
content CRC does not cover those header bytes, and the borrowed content
span itself is unaffected.

## Error Handling

```cpp
// Multi-process specific errors
switch (result.error()) {
    case CacheError::NotOwned:
        // Write rejected: stripe owned by another process
        break;
    case CacheError::InvalidConfiguration:
        // Invalid multi-process configuration at start()
        break;
    default:
        // Standard cache errors
        break;
}
```

## Performance Considerations

### Advantages
- Lock-free reads from non-owned stripes
- Shared cache increases effective cache size
- Memory-mapped I/O enables efficient cross-process reads
- Directory shared via mmap (no IPC needed for lookups)

### Trade-offs
- Checksum validation adds CPU overhead on reads
- Torn read retries add latency in rare cases
- Mmap'd directory has slight overhead vs in-memory
- Write forwarding may be needed in some architectures

## On-Disk Format Versions

The current on-disk format major version is **v8**
(`VolumeHeader::kFormatVersionMajor` and `Document::kVersionMajor`, kept in
lockstep). Version history:

| Version | Change |
|---------|--------|
| v3 | mmap-backed directory (multi-process support) |
| v4 | `stripe_size = 0` auto-derives the stripe count from the volume size |
| v5 | finer auto-stripe granularity (32 MB) + persisted, authoritative `stripe_count` in the header |
| v6 | `hit_count` / `next_alternate_offset` / `last_access` laid out naturally aligned, so the in-place header RMW sites can store them atomically |
| v7 | alternate chains are depth-bounded at write time; the bump leaves pre-bound (over-deep, possibly cyclic) chains behind rather than repairing them |
| v8 | the document checksum is CRC-32C (Castagnoli) instead of CRC-32/ISO-HDLC, so it has hardware instructions on x86-64 as well as ARMv8; pre-v8 checksums were computed over a different polynomial and the bump leaves those rings behind |

### Automatic Migration

Migration is automatic: opening a volume whose major version predates v8
resets (reinitializes) it with the current format, losing the cached data —
entries are cache data, so the cost is a one-time miss spike while the cache
re-populates. With `auto_reset_on_incompatible = false`, the open fails with
`IncompatibleVersion` instead.

### Recommended Migration Strategy

For production deployments:

1. **Deploy during low-traffic period**: Cache will need to warm up
2. **Consider gradual rollout**: Deploy to subset of processes first
3. **Monitor cache hit rates**: Expect temporary dip during migration

### No Manual Migration Required

The cache handles format differences automatically. You only need to:

1. Update your application to use the new multi-process configuration
2. Deploy the new version
3. Accept temporary cache miss spike during warmup

## Testing

Run the multi-process and mmap directory tests:

```bash
./build/cyclone-tests "[multiprocess]"
./build/cyclone-tests "[mmap_directory]"
```

Test categories:
- `[multiprocess][config]` - Configuration validation
- `[multiprocess][stripe]` - Stripe ownership
- `[multiprocess][write]` - Write rejection
- `[multiprocess][read]` - Cross-process reads
- `[multiprocess][compat]` - Backward compatibility
- `[mmap_directory]` - MmapDirectory unit tests
- `[mmap_directory][integration]` - Cache integration tests
- `[mmap_directory][security]` - Security tests for overflow/validation
