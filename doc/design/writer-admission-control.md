# Writer admission control on a saturated device

**Status:** prototype, opt-in (`CacheConfig::write_behind`, default off).
Issue #43. Measured results are in
[`doc/kv-cache-benchmark.md`](../kv-cache-benchmark.md), section "Writer
admission (issue #43)".

**Scope:** holding back large writes when the device is saturated, so that
`fill_large_document_tail` can keep its insert-tail win without the
hit-tail cost. Out of scope and unchanged: the on-disk format, the read
path, the directory, the lock protocols, durability, and small writes
(64 KiB and below).

This document went through one round of measurement before it was
finished. Sections 2–4 are the design as first written: a delay before
each large write, driven by the device's read latency. Section 5 is the
pilot that rejected it, and what the device statistics showed instead.
Sections 6–10 are the design that replaced it. Anchors are function and
constant names in `src/core/volume.{hpp,cpp}`, not line numbers.

---

## 1. The problem, in one paragraph

On ext4, a large document's last page is shared with previous-pass data
that usually is not cached, so every such insert used to wait, inside
`pwrite`, for a synchronous 4 KiB read queued behind everything else the
device was doing. That wait was the insert tail (#35), and it was also,
by accident, writer admission control: inserts slowed exactly when the
device was congested. `fill_large_document_tail` removes the read. Insert
p99 halves, but at four threads the hit p99 rises 25–68 % and reads wait
longer at the device (read `await` 3.54 against 2.62 ms at equal load).
Putting back a synchronous 4 KiB `pread` after each large write restored
the hit tail at T=4 but gave back the whole T=1 insert gain; the same read
issued asynchronously made things worse. The wait helped, not the read.

## 2. The signal (first design)

| signal | what it sees | cost | verdict |
|---|---|---|---|
| **Device read `await`**: Δ(read ticks) / Δ(reads completed) from `/sys/dev/block/MAJ:MIN/stat` of the volume file's device | every read the device served, whoever issued it | one `pread` of a small sysfs file per 50 ms window, by one writer; nothing on the read path | chosen for the pilot (Linux) |
| In-process read latency: time the CRC pass of a CRC-pending read, per-thread samples folded lazily | only the first read of each document incarnation per process | two clock reads per CRC-pending read | rejected |
| Write-completion latency or queue depth | writes, or both directions mixed | cheap | rejected |

**Why not in-process read latency.** Cyclone does not time reads, and the
natural place to start, the CRC pass in `read_sync` where a cold
document's pages are faulted in, sees too little. The CRC-validation cache
(`_checksum_cache`, 65 536 entries) remembers every document that passed;
at 2 MiB a 16 GiB store holds 8 192 documents, so after the first read of
each incarnation every later hit skips the CRC. Those later hits are the
ones that miss the page cache under a 4 GiB cgroup, and their faults
happen in the caller's `memcpy` of a borrowed mapping, where Cyclone
cannot time them. The sample would be sparse and biased low, would need
normalising by document size, and would add clock reads to the lock-free
read path.

**Why not write latency or queue depth.** Buffered `pwrite` returns from the
page cache. Write-back completion latency moved the wrong way with the
fill (11.5 against 13.4 ms, faster) while reads got slower, and queue depth
barely moved (31.0 against 29.3) while read `await` moved by 35 %.

The sysfs signal needs no reader involvement and is shared across
processes for free (every process reads the same device counters).

## 3. The control law (first design)

Token bucket (needs a device-specific rate target), AIMD on a write rate (a
sawtooth by design) and a delay proportional to the excess read latency
were weighed. The delay was chosen: zero when the device is healthy,
continuous in its input, bounded, and shaped like the brake that was
measured to work (one queued read's wait per insert, about 3 ms):

```
await = Δread_ms / Δreads            (window ≥ 50 ms and ≥ 16 reads)
ewma  = ewma + (await − ewma) / 4
delay = clamp(gain × (ewma − target), 0, max_delay)
        target 2 ms (blk-wbt's read target), gain 2, max_delay 10 ms
```

Placement: at the top of both commit sites, before `stripe->mutex`, the
cross-process write lock and `allocate_write_slot`, so no reader and no
other writer waits on a sleeping one, and nothing is reserved while it
sleeps. Multi-process: each process throttles itself from the shared device
counters. nginx: bounded by `max_delay`; a reject (`Busy`) mode for the
event loop was planned.

## 4. Failure modes it was designed against

Oscillation (EWMA, continuous output, modest gain), starving writes
(bounded per write), a device slow when idle (the target is a knob), page
cache hits (invisible, by design), other tenants (a good-neighbour brake),
counters unavailable (inert).

## 5. The pilot, and what the device statistics showed

The design was prototyped as written (the patch is kept as
`doc/kv-cache-benchmark/admission-control/admission-control-sleep-throttle.patch`)
and piloted at the hardest point, `zipf+scan` at T=4 (2 MiB, 16 GiB, 4 GiB
cgroup, wrap retention on, one run each):

| `zipf+scan`, T=4 | served GB/s | hit p99 | miss+insert p99 | inserts delayed / mean delay |
|---|---:|---:|---:|---:|
| main | 1.41 | 21.1 ms | 61.4 ms | — |
| fill on | 1.49 | 44.4 ms | 33.0 ms | — |
| fill on + delay (gain 2) | **1.00** | **49.7 ms** | 46.4 ms | 91 % / 7.2 ms |
| fill on + delay (gain 4, a runner job started mid-run) | 1.18 | 40.4 ms | 39.1 ms | 81 % / 7.3 ms |

The delay engaged on nine inserts in ten, cut served throughput by a third,
and did not shorten the hit tail. The device statistics sampled beside each
run say why:

| `zipf+scan`, T=4 | write MB/s | read `await` | write `await` | queue | cgroup dirty |
|---|---:|---:|---:|---:|---:|
| main | 616 | 2.00 ms | 14.6 ms | 31.9 | 312 MiB |
| fill on | 653 | 6.41 ms | 13.2 ms | 45.0 | 335 MiB |
| fill on + delay | 447 | 5.65 ms | 15.8 ms | 32.4 | 302 MiB |

With the delay, the application wrote 31 % less and read `await` barely
moved. Read latency here is not set by how much is written but by how it
reaches the device: hundreds of MiB of dirty page cache that the kernel's
flusher writes back in deep batches (write `await` 13–16 ms), with every
read that misses the page cache queued behind a batch. Slowing `pwrite`
down lowers the average but leaves the batches as they are. And a sleep on
a 50 ms average cannot line up with those batches, whereas the old brake
did: a writer waiting for a queued read waited exactly while a batch was
in the queue.

That pointed at the write-back, not at the writer. A second pilot started
each large document's write-back from the writer right after its write
(`sync_file_range(SYNC_FILE_RANGE_WRITE)`, no wait):

| `zipf+scan`, T=4 | served GB/s | hit p99 | miss+insert p99 | write `await` | cgroup dirty |
|---|---:|---:|---:|---:|---:|
| main (same session) | 1.36 | 23.6 ms | 63.6 ms | 14.7 ms | 297 MiB |
| fill on + write-behind | **2.20** | **15.4 ms** | **14.5 ms** | 2.1 ms | 8 MiB |
| fill on + write-behind, then wait for the previous document's write-back | 1.94 | 17.0 ms | 19.1 ms | 2.7 ms | 8 MiB |

Dirty data fell from about 300 MiB to 8 MiB, write `await` from 14 to 2 ms,
and the queue halved. Hit p99, insert p99 and throughput all improved at
once. Waiting for the previous document on top (the last row, the patch
`admission-control-write-behind-pilot.patch` with mode 2) added nothing.
The first design was dropped in favour of the second.

## 6. The design: write-behind

After a document above `Volume::kTailFillAboveBytes` (64 KiB) has been
written and committed, the writing thread asks the kernel to start writing
the document, from its first byte to the last page boundary at or before
its end:

```cpp
sync_file_range(fd, offset, length, SYNC_FILE_RANGE_WRITE);
```

**Why the range stops short of the last page.** The page a document ends
in is shared with the next document: the tail fill's zeros start at the
cursor, and the next document begins inside that page either way. Starting
its write-back now would write it twice, and on devices with stable pages
(T10-PI / integrity devices, some RAID) the next writer's `pwrite` into it
would wait for that write-back while holding the stripe mutex and the
cross-process write lock. The page is instead the first page of the next
document's range, or left to the flusher, so every page is started once
and no writer waits on another's write-back. The counters only count calls
the kernel accepted (a file system that refuses `sync_file_range` returns
`EINVAL` or `ESPIPE` and is not counted). The benchmark in section 5 and
in `doc/kv-cache-benchmark.md` ran the first version, whose range included
that last page and the fill's zeros; the difference is at most one page of
a 2 MiB document.

This neither waits for the write to finish nor makes anything durable. It
moves the moment the pages are submitted from "whenever the flusher
decides, in a batch" to "now, by this thread, one document at a time".

**Why it works: the shape of the write-back.** Each document's pages go
to the device as it is produced, in document-sized writes, instead of
accumulating in the page cache until the flusher writes hundreds of MiB
in deep batches. In the full measurement (the benchmark section) the
cgroup's dirty data fell from about 300 MiB to 7–9 MiB, write `await` from
10–13 ms to about 1 ms, and the requests in the device from 29–39 to
7–12 at four threads, with the device just as busy (99.9 %) and moving
more bytes. A read that misses the page cache now finds a short queue.

**Why it is also admission control.** Submitting I/O is where the block
layer pushes back. When the device queue is full, or when blk-wbt is
holding writes back because reads are missing their latency target (the
kernel's own read-latency controller, 2 ms on this device), the submitter
sleeps. Without write-behind the submitter is the flusher thread, so the
application never feels it and only piles up more dirty pages. With
write-behind it is the writer, which is then admitted at the rate the
device accepts, per request, in step with the actual queue, and not held
at all when the device is idle. The read-latency signal of the first
design is still there; it is blk-wbt's, measured per request in the block
layer, instead of an average sampled from user space. Measured honestly,
this safety net was barely exercised: the call took 0.21 ms per insert at
one thread and 0.32 ms at four, mostly the submission work itself, and
blk-wbt was almost never at its tightest limit (0–0.2 % of samples, against 4–10 %
without write-behind). The shallow queue did the work.

## 7. Where it sits

In the public wrappers, after the commit has returned: `commit_write`
calls `commit_write_impl` (the former body) and then
`start_write_behind`; `commit_alternate_write` does the same after its one
or two `commit_alternate_write_once` attempts. The commit sites only report
the range their fill wrote (`WrittenRange`). So:

- **No reader can wait on it.** Readers take no stripe lock (invariant 1),
  and the stripe mutex and the cross-process write lock are released
  before the call.
- **No other writer waits on it.** A writer held back by the device holds
  up only itself.
- **Invariants untouched.** Commit ordering (2): the document is already
  durable to the extent configured and published; write-behind is not a
  durability point and never gates the insert. The wrap-intent handshake
  (3), seqlocks (4), HitTracker (5), sharding (6), stripe ownership (7)
  and both positional guards (8, 9) are not involved.
- **Leases and the frontier:** nothing is reserved or held. A range the
  stripe has wrapped over since (possible only with a long stall) costs one
  redundant write-back.

## 8. Interactions

**Multi-process mode.** Each process starts write-back for its own writes.
blk-wbt and the device queue are per device, so all processes are held back
by the same state without any shared field.

**The #30 lock caps and `Busy`.** The call runs after every lock is
released, so it never counts against a capped wait, and never shows up as a
live holder to peers. It cannot turn a success into `Busy` or the other way
round.

**PageSpeed's inline nginx write path.** `sync_file_range` can block while
the device queue is full. On a worker thread that is the point; on an nginx
event loop it would stall every connection of that worker. The option is
off by default and not in the C API, which is what the nginx module uses,
so it cannot be turned on there by accident. The ext4 read-before-write
the fill removes blocked the same event loop, unbounded, whenever the fill
was off. If nginx ever needs it, the call belongs on a helper thread (the
range is all it needs), not inline.

**Wrap retention.** No interaction beyond section 7.

**Other platforms.** macOS has no `sync_file_range` (and APFS does not read
before a partial write, so the fill costs nothing there); Windows writes
through a mapping. On both the option does nothing but fire the test seam.
XFS behaves like ext4.

## 9. Configuration

`CacheConfig::write_behind` (bool, default off, mirrored into
`VolumeConfig`, fluent `set_write_behind()`). No knobs: the threshold is
the fill's, 64 KiB, and the call has no parameters worth tuning. Telemetry
on `CacheStats`: `write_behind_ranges` (documents whose write-back was
started) and `write_behind_us` (total time writers spent in the call, which
is how much the device held them back).

Opt-in for now because it changes when write-back happens for everything
on the device, and it was measured on one machine and one workload. It
helped with the fill off as well (the benchmark's `wb` rows): with a short
queue the ext4 read-before-write the fill removes is itself fast. Making
it, or it and the fill, the default is a separate decision that wants a
second device and the PageSpeed workload.

## 10. Failure modes

- **Starving writes.** A writer is held back only while the device queue is
  full or blk-wbt throttles; that wait is what the flusher would otherwise
  absorb, and the time is reported in `write_behind_us`. There is no
  sleep and no cap of our own: the block layer bounds it by the rate the
  device drains.
- **Small or mixed writes.** Only documents above 64 KiB are started; small
  objects (PageSpeed metadata) keep the page cache's batching.
- **Write amplification.** Starting write-back early can write a page twice
  if it is dirtied again before the flusher would have run. Cyclone's
  stripes are append-only and a document is written once, so a page is
  dirtied again only by the next document sharing it (at most one page
  per document). Measured: 2.103–2.104 MB written to the device per 2 MiB
  insert with write-behind, 2.106–2.108 without.
- **Latency at the median.** The call costs the writer 0.2–0.3 ms per
  2 MiB document, and hit p50 rose 10–21 % (for example 270 to 310 µs at
  one thread) while every tail percentile fell. A workload that cares
  only about the median of an idle device gains nothing.
- **Fast devices.** On a device that is never saturated the call returns
  at once and write-back simply starts sooner.
- **Cgroups and dirty limits.** With write-behind the cgroup holds little
  dirty data, so dirty throttling (`balance_dirty_pages`), which already
  never slept here, becomes even less likely. The kernel's dirty limits
  are not changed.
- **Not durability.** Nobody may read `write_behind` as `sync_on_write`:
  it does not wait, does not flush metadata, and a power loss can still
  lose any document not covered by the directory syncer's fsync.

## 11. What was not done

- Starting write-back from a helper thread for event-loop callers, and a
  C API field.
- Grouping several small documents into one write-back range.
- A self-tuning variant (wait for the previous range only when blk-wbt is
  throttling); the pilot's unconditional wait added nothing.
