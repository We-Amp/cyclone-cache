// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <sys/types.h>  // dev_t, ino_t (Volume::backing_identity)
#endif

#include "cyclone/alternate.hpp"
#include "cyclone/config.hpp"
#include "cyclone/error.hpp"
#include "cyclone/handle.hpp"
#include "cyclone/key.hpp"
#include "cyclone/task.hpp"
#include "directory.hpp"
#include "hit_tracker.hpp"
#include "mmap_directory.hpp"
#include "thread_shard.hpp"

namespace cyclone {

class MappedFile;
class RamCache;

// Minimum stripe size.  Volume::init_stripes() clamps any configured
// stripe_size below this to it; a volume whose usable size is smaller than
// one stripe gets a single stripe spanning the whole usable area.  Shared
// with cache.cpp, where the small-tier carve-out sizing uses it as the
// per-volume floor.
// LAYOUT-CONSTANT INVARIANT (fingerprint upgrade safety): kMinStripeSize,
// kAutoStripeGranularity, kAutoStripeTarget and kStripeAlign (in volume.cpp),
// kDirectoryEntriesPerSegment (in volume.cpp) and VolumeHeader::kSize all shape
// the on-disk stripe/directory layout WITHOUT being visible to
// VolumeHeader::kFormatVersionMajor.  fingerprint_cache_path()'s geohash mixes
// only the format major plus the DERIVED geometry (num_stripes,
// base_stripe_size, stripe_remainder); it cannot see these raw constants.  So
// changing ANY of them silently produces a different on-disk layout under the
// SAME fingerprint -- two incompatible layouts collide on one file and corrupt.
// Therefore: changing any layout constant REQUIRES bumping kFormatVersionMajor.
inline constexpr size_t kMinStripeSize =
    static_cast<const size_t>(128 * 1024 * 1024);  // 128MB

// Target stripe count when VolumeConfig::stripe_size is left at 0 (auto).
// Lease-based region pinning gates wraps at STRIPE granularity, so a
// single-stripe volume lets any live read borrow block every write to the whole
// cache under mixed read/write load (writes dropped -> hit rate collapses).
// Spreading the volume across several stripes keeps a hot borrow local to one
// stripe while writes to the others proceed.  The count is
// clamp(round(usable_size / kMinStripeSize), 1, kAutoStripeTarget) -- ROUNDED,
// so a volume in [kMinStripeSize, 2*kMinStripeSize) still splits into 2 -- and
// the whole usable region is then even-tiled across that many stripes (the
// last stripe absorbs the remainder, so no tail is wasted).
// Auto-path granularity: the target per-stripe size the auto stripe COUNT
// is derived from (count = clamp(round(usable / kAutoStripeGranularity), 1,
// kAutoStripeTarget)).  Lowered from 128MB to 32MB so small volumes
// (256MB-512MB) reach enough stripes to keep lease-based region pinning
// from dropping fills -- a 256MB cache goes from 2 stripes (~70%
// hit under mixed load) to 8 (~90%).  kMinStripeSize stays 128MB: it is the
// small-tier sizing floor in cache.cpp, a separate concern.
inline constexpr size_t kAutoStripeGranularity =
    static_cast<const size_t>(32 * 1024 * 1024);  // 32MB

inline constexpr size_t kAutoStripeTarget = 16;

// Volume file header - stored at offset 0 of each cache file
// Used for version compatibility detection and cache validation
struct VolumeHeader {
  static constexpr uint32_t kMagic = 0x43594C4E;  // "CYLN" in little-endian
  static constexpr size_t kSize = 64;  // Fixed size, room for future fields

  // Current format version - increment major on breaking changes
  // Version 3: Added mmap'd directory support for multi-process
  // Version 4: stripe_size=0 auto-derives the stripe count from the volume
  //   size (kAutoStripeTarget); the on-disk stripe geometry differs from a v3
  //   single-stripe volume, so v3 files auto-reset (rebuild) on open.
  // Version 5: finer auto-stripe granularity (kAutoStripeGranularity) changes
  //   the derived stripe count/offsets, and the persisted stripe_count below
  //   is now authoritative -- pre-v5 volumes auto-reset on open.
  // Version 7: alternate chains are now DEPTH-BOUNDED at write time (a write
  //   splices the superseded same-id node out).  No byte of the layout moved,
  //   so this is not a layout bump -- it is a deliberate clean slate.  A v6
  //   ring can carry chains a v6 binary built without the bound: chains deep
  //   enough to have wedged their key, and chains made cyclic by a link
  //   created across a wrap frontier (the write-time refusal added alongside
  //   the bound prevents new ones, but cannot un-make an existing one).
  //   Rather than ship repair code for state that is, by definition, already
  //   broken, the format major separates the two rings: a v7 binary resolves
  //   to a different fingerprinted filename and starts on a fresh volume.
  //   Consumers take a COLD CACHE on upgrade, and the superseded v6 file stays
  //   on disk until it is reclaimed (CacheConfig::gc_superseded_on_start, or a
  //   manual delete -- required BEFORE start on a space-constrained cache
  //   directory, and on Windows, where the GC is compiled out).
  // Version 8: the document checksum is CRC-32C (Castagnoli) instead of
  //   CRC-32/ISO-HDLC -- CRC-32C has hardware instructions on x86-64 (SSE4.2)
  //   as well as ARMv8, and the checksum is re-verified on every cold read.
  //   Again not a layout bump: no byte moved, but every pre-v8 document's
  //   stored checksum is computed over a different polynomial and would fail
  //   verification.  The format major separates the rings, so a v8 binary
  //   resolves to a different fingerprinted filename and starts cold rather
  //   than rejecting document after document.  Same consumer consequences as
  //   the v7 bump: COLD CACHE on upgrade, and the superseded v7 file stays on
  //   disk until it is reclaimed (CacheConfig::gc_superseded_on_start, or a
  //   manual delete).
  static constexpr uint16_t kFormatVersionMajor = 8;
  static constexpr uint16_t kFormatVersionMinor = 0;

  uint32_t magic = kMagic;
  uint16_t format_version_major = kFormatVersionMajor;
  uint16_t format_version_minor = kFormatVersionMinor;
  uint64_t creation_time = 0;  // Unix timestamp (seconds)
  uint64_t volume_size = 0;    // Configured volume size
  uint32_t directory_buckets =
      0;                       // Number of directory buckets per stripe (v3+)
  uint8_t mmap_directory = 0;  // 1 if using mmap'd directory (v3+)
  uint8_t reserved0[3] = {};   // pad so stripe_count is 8-byte aligned
  uint64_t stripe_count = 0;   // Persisted derived stripe count (v5+); 0 =
                               // unrecorded (pre-v5).  Authoritative: an
                               // open whose derived count disagrees resets.
  uint8_t reserved[24] = {};   // Future use, zero-filled

  [[nodiscard]] bool is_valid() const { return magic == kMagic; }
  [[nodiscard]] bool is_compatible() const {
    return magic == kMagic && format_version_major == kFormatVersionMajor;
  }

  // Serialize header to buffer (must be at least kSize bytes)
  void serialize(std::byte *buffer) const;
  // Deserialize header from buffer (must be at least kSize bytes)
  static VolumeHeader deserialize(const std::byte *buffer);
};

static_assert(sizeof(VolumeHeader) == VolumeHeader::kSize,
              "VolumeHeader size must match kSize exactly");
static_assert(
    std::is_trivially_copyable_v<VolumeHeader>,
    "VolumeHeader must be trivially copyable for memcpy serialization");

// Structural fingerprint of a cache-file path (upgrade safety).  Rewrites the
// filename to encode the on-disk FORMAT (VolumeHeader::kFormatVersionMajor) and
// GEOMETRY (derived stripe count / base stripe size / remainder) so that two
// processes whose binaries disagree on layout resolve to DIFFERENT files and
// can never share one on-disk ring during an overlapping upgrade (an old worker
// still serving while a new binary opens the cache).  Without this, a
// format/geometry bump makes the new binary reset() the shared file under the
// live old peer -> silent CRC-clean corruption; release ordering cannot fix it.
//
// The transform is idempotent (an already-fingerprinted path is returned
// unchanged) and deterministic (no time/rand).  Defined in volume.cpp next to
// compute_stripe_geometry.  Applied at the sole volume chokepoint,
// Cache::add_volume_locked.  Opt-in startup garbage-collection of the
// SUPERSEDED files this leaves behind is available via gc_superseded_volumes
// (below), gated on CacheConfig::gc_superseded_on_start (default off).
std::string fingerprint_cache_path(const std::string &path, size_t size,
                                   size_t stripe_size, bool mmap_directory);

// Resolution for the size < VolumeHeader::kSize (notably the size == 0
// "open at on-disk size") mode, which has no derivable geometry to
// fingerprint.  Sized peers of the CURRENT format major ALWAYS resolve to a
// fingerprinted name, so an unsized opener that kept the raw path would
// silently stop sharing their volume.  Preference order:
//   1. the newest sibling "<stem>-<current major>-<16 lowercase hex><ext>"
//      in `path`'s directory (mtime, then lexicographically-last on ties,
//      so the pick is deterministic);
//   2. `path` unchanged (pure-legacy deployments, or nothing to adopt).
// A raw-path file is never preferred over a current-major sibling: sized
// binaries of this major never write the raw path, so such a file is a
// pre-fingerprint leftover.  Siblings of OTHER format majors are never
// candidates -- a mixed-version overlap must keep resolving to its own
// format's file (the whole point of the fingerprint).  Called from
// fingerprint_cache_path; defined next to it in volume.cpp.
std::string resolve_unsized_cache_path(const std::string &path);

// Shared fingerprint-name classifier (external linkage; single definition backs
// BOTH the name generator's idempotency check and the superseded-file GC
// classifier, so the shape rule can never drift between them).  True iff `stem`
// ends with a "-<one-or-more-digits>-<16 lowercase hex>" fingerprint tail.
// `stem` is a filename STEM with the final extension already stripped (e.g. via
// std::filesystem::path::stem()); by that rule `cyclone-6-<hex>`(.dat) and
// `cyclone.dat-6-<hex>`(.small) match, while legacy `cyclone`(.dat) and
// `cyclone.dat`(.small) do not.  Defined in volume.cpp.
bool stem_is_fingerprinted(const std::string &stem);

// The BASE stem preceding a fingerprint tail: strips a trailing
// "-<digits>-<16 hex>" from `stem`, or returns `stem` unchanged when it carries
// no such tail.  The GC matches a candidate file's base stem against the live
// volumes' base stems (so a superseded `cyclone-6-<old>` matches the live
// `cyclone-6-<new>` on base `cyclone`).  Parse mirrors stem_is_fingerprinted
// (no <regex>).  Defined in volume.cpp.
std::string fingerprint_base_stem(const std::string &stem);

#ifndef _WIN32
// POSIX opener-side inode re-validation (Volume::open_locked defense-in-depth):
// returns true iff `fd` and `path` still name the SAME live inode.  False when
// fstat(fd) fails, stat(path) fails (e.g. ENOENT), st_dev/st_ino differ, or the
// path's link count is 0 -- i.e. the file was unlinked or replaced out from
// under this opener.  Defined next to fingerprint_cache_path in volume.cpp.
bool inodes_match(int fd, const std::string &path);
#endif

struct Stripe {
  uint64_t offset = 0;
  uint64_t size = 0;

  // Directory storage - only one is active at a time
  // In-memory directory (single-process mode)
  std::unique_ptr<Directory> directory;
  // Mmap'd directory (multi-process mode) - points into mapped file
  std::optional<MmapDirectory> mmap_directory;

  // Offset to the data area (after directory)
  uint64_t data_offset = 0;
  std::atomic<uint64_t> write_pos{0};
  uint32_t sync_serial = 0;
  uint32_t write_serial = 0;

  // Multi-process: true if this process owns this stripe (can write)
  // When multi-process is disabled, all stripes are owned (true by default)
  bool owned = true;

  // Whether using mmap directory
  bool use_mmap_directory = false;

  // Lease state for non-mmap (single-process) stripes: same
  // protocol as the shared header slot at offset 56, on a process-local
  // atomic.  Unused when use_mmap_directory (the shared slot is
  // authoritative there).  steady-clock ns; 0 = no lease.
  std::atomic<uint64_t> local_lease_expiry_ns{0};
  // Outstanding-borrow accounting for non-mmap stripes:
  // per-thread SHARDS of packed {generation:8, count:8} slots, each
  // running the same protocol as the shared header slot at offset 34 (see
  // borrow_slot in mmap_directory.hpp).  A single slot made every
  // disk-hit read do two seq_cst CASes on one stripe-global cache line
  // (acquire on read, release on handle close) — with few stripes this
  // was the dominant concurrent-read scaling ceiling found in profiling.  A
  // reader CASes only its own thread's shard; the wrap gate, which runs
  // on the rare at-capacity write path, sums all shards (the gate only
  // needs "is the total nonzero").  The Dekker pairing with the writer's
  // intent-store-then-count-load closes per shard: each reader's CAS and
  // the gate's load of THAT shard order against the intent flag exactly
  // as the single slot did (proof at Volume::allocate_write_slot).
  // Generations are per-shard; a BorrowToken records its shard so the
  // release and the generation check land on the slot that was acquired.
  // Unused when use_mmap_directory (the shared cross-process slot is
  // format-frozen and stays single; multi-process readers keep the
  // original single-slot cost there).
  struct alignas(kShardPad) BorrowShard {
    std::atomic<uint16_t> slot{0};
  };
  static constexpr size_t kBorrowShards = 64;
  // BorrowToken routes releases through a uint8_t shard index, and
  // thread_shard_index requires a power of two — both would corrupt
  // silently (decrements landing on the wrong shard) if this grew past
  // either bound.
  static_assert(kBorrowShards <= 256 &&
                    (kBorrowShards & (kBorrowShards - 1)) == 0,
                "kBorrowShards must be a power of two that fits uint8_t");
  std::array<BorrowShard, kBorrowShards> local_borrow_shards{};
  // Wrap epoch counterpart of MmapDirectory::shared_wrap_count for
  // non-mmap stripes (per-stripe, unlike Volume::_wrap_count which
  // aggregates for stats).
  std::atomic<uint64_t> local_wrap_count{0};
  // Remove generation: bumped (under the exclusive stripe mutex) by
  // remove_sync / remove_alternate_sync AFTER their directory mutation
  // completes, and by the commit paths after they publish a head that
  // supersedes live content — in every case immediately before the
  // RAM-cache eviction that pairs with the bump.  Lock-free readers that
  // repopulate the RAM cache capture it before probing and re-check it in
  // their conditional put's predicate, evaluated under the same
  // RAM-cache write lock the eviction takes: a concurrent remove could
  // have already run its RAM invalidation BEFORE the put landed
  // (resurrection), so the put is rejected at insert time (with a post-put
  // re-check withdrawing the rare put whose predicate raced the bump
  // inside its own critical section).  The post-mutation placement is
  // load-bearing
  //: a bump at the TOP of a remove lets a reader sample the bumped
  // value and still probe the not-yet-removed entry, and nothing ever
  // withdraws that put.  PROCESS-LOCAL: in multi-process mode a REMOTE
  // process's removes are not reflected here — that exposure predates the
  // lock-free-reader work (non-owned-stripe reads never held the stripe
  // lock) and is unchanged by it.
  std::atomic<uint64_t> remove_epoch{0};
  // Non-mmap counterpart of the shared wrap_intent flag at header
  // offset 33: 1 while a writer is inside the wrap decision + publish
  // window (see Volume::allocate_write_slot).
  std::atomic<uint8_t> local_wrap_intent{0};

  // Anti-starvation bookkeeping: steady-clock ns of the first
  // deferred wrap of the current continuous-deferral episode; 0 = not
  // deferring.  Process-local by design — the writer that defers is the
  // one that eventually forces the wrap.  Mutated only under the stripe
  // write path locks.
  std::atomic<uint64_t> wrap_deferred_since_ns{0};
  // Lease-protocol STEP-3 non-mmap counterpart of the shared force-wrap
  // deadline (steady-clock ms; 0 = no deferred wrap).  Single-process
  // mode: reader and writer are the same process, so a process-local
  // atomic is authoritative.
  std::atomic<uint32_t> local_wrap_deferred_deadline_ms{0};

  std::shared_mutex mutex;

  // Current write cursor (ABSOLUTE, i.e. stripe->offset..offset+size) for the
  // Phase-ABA positional guard below.  Read SHARED when an mmap
  // directory backs the stripe: a multi-process reader may NOT own the stripe,
  // so its process-local write_pos is stale (frozen at data_offset for a
  // non-owner) and only the shared cell is authoritative; the owner keeps the
  // two in sync every allocation, so the shared read is correct for it too.
  // One acquire load — O(1), lock-free.
  [[nodiscard]] uint64_t current_write_cursor() const {
    if (use_mmap_directory && mmap_directory) {
      return mmap_directory->get_shared_write_pos();
    }
    return write_pos.load(std::memory_order_acquire);
  }

  // Phase-ABA POSITIONAL GUARD predicate — the shared check behind
  // BOTH guard legs: the directory-PROBE leg (probe_each below) and the
  // chain-HOP leg (is_valid_chain_offset in volume.cpp).  A LIVE node always
  // sits STRICTLY BEHIND the write cursor: both commit paths advance the
  // cursor past the whole document only AFTER its fill is durable
  // (commit_write_slot, F6 — never at reservation, so the cursor never
  // covers reserved-but-unwritten bytes), and strictly before the entry (or
  // any chain pointer to it) is published.  A node AT or AHEAD of the cursor is
  // therefore a stale survivor whose bytes the ordinary forward fill will
  // overwrite IN PLACE — no wrap event, so none of the lease/epoch machinery
  // fires; reading through it hands out a tearable borrow.  The `<` (i.e.
  // rejecting `>=`) is load-bearing: at the exact tear boundary the cursor
  // sits AT the survivor's offset (the overwriter is the very next
  // reservation) with the bytes still intact — admitting it there is
  // precisely the tear.  A torn/stale-low cursor read only LOWERS the cursor,
  // which only WIDENS rejection (at worst a benign miss), never admits a
  // survivor.  One O(1) lock-free load; no locks on the read path.
  [[nodiscard]] bool is_behind_write_cursor(uint64_t relative_offset) const {
    return offset + relative_offset < current_write_cursor();
  }

  // Helper to probe directory regardless of type.
  //
  // Phase-ABA positional guard, PROBE leg.  probe_each only ever yields
  // CURRENT-PHASE entries (Directory/MmapDirectory skip stale-phase slots),
  // and a legitimate current-phase entry was written this pass, so it is
  // strictly behind the cursor (see is_behind_write_cursor above).  A
  // current-phase entry at/ahead of the cursor is, by construction, a
  // survivor of two GC-phase toggles (a phase ABA); reject it BEFORE the
  // callback maps or borrows.  This leg covers every directory probe
  // (primary reads, chain-head lookups, the write-election, remove and
  // hit-count probes).  Chain HOPS consume offsets from document headers
  // instead of the directory — a same-key pre-wrap tail reachable with ONE
  // wrap — and are guarded by the same predicate in is_valid_chain_offset.
  template <typename Callback>
  void probe_each(const CacheKey &key, Callback &&callback) const {
    auto guarded = [this, &callback](const DirEntry &entry) -> bool {
      if (!is_behind_write_cursor(entry.offset())) {
        return true;  // Phase-ABA survivor: skip, keep scanning the bucket.
      }
      return callback(entry);
    };
    if (use_mmap_directory && mmap_directory) {
      mmap_directory->probe_each(key, guarded);
    } else if (directory) {
      directory->probe_each(key, guarded);
    }
  }

  // Helper to insert into directory.  verified_offset is the directory
  // offset of the entry the caller has verified (by full first_key
  // comparison) to hold this key, or Directory::kNoVerifiedEntry when no
  // entry does — a same-tag entry holding a DIFFERENT key must never be
  // updated in place (collision-destructive insert).  *collision_evicted
  // reports a full-bucket collider eviction so the caller can count it;
  // *bucket_full_evicted reports a full-bucket eviction of the entry nearest
  // the wrap cursor.
  bool insert(const CacheKey &key, uint64_t offset, uint64_t size,
              uint64_t verified_offset = Directory::kMatchAnyTag,
              bool *collision_evicted = nullptr,
              bool *bucket_full_evicted = nullptr) {
    if (use_mmap_directory && mmap_directory) {
      return mmap_directory->insert(key, offset, size, verified_offset,
                                    collision_evicted, bucket_full_evicted);
    } else if (directory) {
      return directory->insert(key, offset, size, verified_offset,
                               collision_evicted, bucket_full_evicted);
    }
    return false;
  }

  // Precise removal matching both tag and offset (collision-safe).
  // Directory mutations here are intentionally NOT view-flushed on Windows
  // (unlike the insert on the sync_on_write put path); durability is
  // deferred to the periodic sync_directory() pass.  A removal or chain
  // repoint lost to power failure is benign: the old entry still points at
  // valid, checksummed data.
  bool remove_entry_at(const CacheKey &key, uint64_t offset) {
    if (use_mmap_directory && mmap_directory) {
      return mmap_directory->remove_at(key, offset);
    } else if (directory) {
      return directory->remove_at(key, offset);
    }
    return false;
  }

  // Cross-process invalidation signal for the key's directory bucket.
  // 0 means "no shared signal available" — either this stripe is not backed
  // by the mmap directory (single-process mode) or the directory is invalid.
  // Callers must therefore gate on use_mmap_directory && mmap_directory
  // rather than on the returned value, so that single-process mode compares
  // 0 against 0 and stays inert.
  [[nodiscard]] uint32_t bucket_version(const CacheKey &key) const {
    if (use_mmap_directory && mmap_directory) {
      return mmap_directory->bucket_version(key);
    }
    return 0;
  }

  // Helper to get entry count
  [[nodiscard]] size_t entry_count() const {
    if (use_mmap_directory && mmap_directory) {
      return mmap_directory->count();
    } else if (directory) {
      return directory->count();
    }
    return 0;
  }
};

struct VolumeStats {
  uint64_t bytes_used = 0;
  uint64_t bytes_capacity = 0;  // configured volume size (incl. header)
  uint64_t entry_count = 0;
  // Stripe geometry (even-tiling guard).  stripe_count is how many
  // stripes the volume was tiled into; stripe_bytes is the sum of their sizes.
  // On the AUTO path (stripe_size == 0) stripe_bytes == bytes_capacity -
  // VolumeHeader::kSize -- the stripes even-tile the whole usable region with
  // no wasted tail.  With an EXPLICIT stripe_size the partial last stripe is
  // dropped, so stripe_bytes < usable and (usable - stripe_bytes) is the
  // wasted tail (a useful diagnostic).  Exposed so tests and ops can verify
  // the geometry.
  uint64_t stripe_count = 0;
  uint64_t stripe_bytes = 0;
  uint64_t reads = 0;
  uint64_t writes = 0;
  uint64_t evictions = 0;
  uint64_t directory_syncs = 0;  // Completed sync_directory() calls
  uint64_t fsyncs = 0;           // Total raw fd fsyncs performed

  // Large-document readahead hints actually issued on the disk read path
  // (see VolumeConfig::readahead_min_bytes).  Counts only the hints that
  // got past the re-advise filter, so it measures kernel calls made, not
  // large reads served: a hot document contributes at most one hint per
  // re-advise interval however often it is read.  PROCESS-LOCAL.
  uint64_t readahead_hints_issued = 0;

  // Wrap-cadence telemetry: how often the circular write buffer wraps back
  // to the start of a stripe's data area (steady-clock based). Sizing
  // instrument for the eviction-vs-reader race.
  //
  // Units: fields are deliberately nanoseconds (steady clock); older stats
  // in this codebase use _ms (wall clock) — do not "harmonize" them.
  //
  // Granularity: wraps happen per STRIPE, but these counters aggregate at
  // volume level. On a multi-stripe volume the intervals therefore measure
  // inter-wrap spacing across the whole volume, not any single write
  // buffer's wrap period — they understate per-stripe wrap periods (i.e.
  // bias the race-sizing metric toward "more urgent"). wrap_count is the
  // primary signal.
  //
  // Cross-process semantics (mmap-directory / multi-process mode):
  // wrap_count and last_wrap_age_ns come from shared per-stripe counters in
  // the mmap'd directory header, so they reflect wraps by ALL processes and
  // are visible to read-only/polling processes. The interval fields are
  // PER-PROCESS: they only cover wraps performed by this process (a process
  // that never wraps reads 0, and with multiple writer processes each one
  // understates global cadence). In deployments where a single process
  // performs (nearly) all writes and also serves stats, its intervals do
  // capture global cadence. Without mmap directories everything is
  // process-local.
  uint64_t wrap_count = 0;  // Total wraps since volume open
  // Interval fields are 0 (undefined) until this process observes its
  // second wrap. min <= last is NOT guaranteed when polled concurrently
  // with a wrap (last is stored before min is folded in).
  uint64_t last_wrap_interval_ns = 0;  // ns between the last two wraps
  uint64_t min_wrap_interval_ns = 0;   // Smallest observed wrap interval
  // ns since the most recent wrap, at the time stats() was called.
  // Only meaningful when wrap_count > 0.
  uint64_t last_wrap_age_ns = 0;

  // Lease-based region pinning.  All three are PROCESS-LOCAL
  // (the writer that defers/drops/forces is the one that counts), so in
  // multi-process mode each writer process reports its own view.
  uint64_t wraps_deferred_by_lease = 0;  // Wraps deferred by a live lease
  uint64_t writes_dropped_by_lease = 0;  // Fills dropped by a deferred wrap
  uint64_t wraps_forced_past_lease = 0;  // Wraps forced past the ceiling

  // Cross-process write-lock recovery telemetry (PROCESS-LOCAL).  In
  // multi-process mode the write lock (write-pos allocation) can be
  // recovered from a peer that died holding it.
  //
  // Write locks recovered from a peer PROVEN dead (kill(pid,0)/OpenProcess)
  // — routine crash recovery; expected after a peer SIGKILL/OOM.
  uint64_t write_lock_force_releases = 0;
  // Write locks taken over via the LAST-RESORT escalation from a holder we
  // could NOT prove dead (PID reuse, or a live holder wedged for seconds).
  // THE alertable counter: nonzero means a live-holder takeover happened —
  // corruption stays gated (the usurped holder aborts at its next
  // revalidate), but sustained nonzero growth here indicates a pathologically
  // oversubscribed or wedged deployment and deserves investigation.
  uint64_t write_lock_escalation_takeovers = 0;
  // Liveness escalations spent WAITING on a live-but-stalled holder instead
  // of usurping it (the behavior that would otherwise corrupt).
  uint64_t write_lock_live_holder_waits = 0;
  // Reservations abandoned because a force-release usurped us mid-critical-
  // section — no overlapping bytes are ever written; the fill self-heals.
  uint64_t write_lock_usurp_aborts = 0;

  // Live disk-hit borrows (open ReadHandles) across the volume's stripes at
  // the instant stats() ran.  A GAUGE, not a counter.  In
  // multi-process (mmap) mode the per-stripe slots are shared, so this
  // reflects borrows held by ALL processes; it saturates at 255 per stripe
  // (see borrow_slot in mmap_directory.hpp).  Nonzero at cache-full means
  // wrap-needing fills are currently being deferred on the holders' behalf
  // — the signal to look for a consumer holding handles open across its own
  // writes.
  uint64_t borrows_outstanding = 0;

  // Directory entries evicted because a DIFFERENT key colliding on
  // (bucket, 12-bit tag) had to land in a completely full bucket (the
  // collider is replaced rather than silently updated in place; see the
  // key-verified insert election in commit_write).  PROCESS-LOCAL: the
  // writer that evicts is the one that counts.
  uint64_t tag_collision_evictions = 0;

  // Directory entries evicted because a bucket was completely full of
  // current-phase entries with no tag collision to displace — the entry
  // nearest the wrap cursor is replaced so the write still lands.
  // Disjoint from tag_collision_evictions by construction.  PROCESS-LOCAL:
  // the writer that evicts is the one that counts.
  uint64_t bucket_full_evictions = 0;

  // --- Alternate-chain shadow bound (all PROCESS-LOCAL) ------------------
  //
  // Superseded same-id chain nodes spliced out by an alternate write, i.e.
  // the mechanism working.  Zero in steady state means either the key is
  // never re-recorded or the kill switch
  // (VolumeConfig::unlink_superseded_alternates) is off.
  uint64_t alternate_shadows_unlinked = 0;

  // Superseded nodes LEFT LINKED because the splice could not proceed: the
  // in-place repoint reported Busy, a wrap moved the epoch between the walk
  // and the store, or the tail beyond the walk was not visible.  The caller's
  // write always succeeds anyway (the mechanism is best-effort by contract),
  // so this is the leading indicator that the depth bound is degrading --
  // watch it against alternate_shadows_unlinked.  With the kill switch off,
  // neither counter moves.
  uint64_t alternate_splice_deferred = 0;

  // Chains reset at the traversal cap because every visible node was a
  // superseded copy of the id being written (the backstop that keeps a key
  // writable when splices are deferred, and what drains a chain inherited
  // from a pre-fix binary).  Nonzero in steady state is an alert: it means
  // depth reached the cap, which the splice alone should prevent.
  uint64_t alternate_chain_resets = 0;

  // High-water mark of the physical chain depth observed by an alternate
  // write BEFORE it prepended (the direct depth-bound regression signal).  A
  // GAUGE-like HWM, never reset; expect it to settle at the number of
  // distinct alternate ids the workload stores per key.
  uint64_t alternate_max_chain_depth = 0;

  // Wrap-frontier link refusals: an alternate write whose allocation wrapped
  // the stripe declined to link its new head to the pre-wrap chain and
  // started a fresh chain instead, orphaning the stale one.  This is
  // what a wrap-race event looks like post-migration; neither shadow counter
  // moves on a refusal (nothing was spliced, nothing was left linked), so
  // without this counter a refusal is indistinguishable from an ordinary
  // wrap.  Counted when the refusing write is published, not when the refusal
  // is decided.
  uint64_t alternate_wrap_refusals = 0;

  // 1 iff the cross-process reset gate is NOT in effect for this volume, so an
  // incompatible open will reset even under a live peer.  A GAUGE, cleared on
  // every open().  One cause, on every platform: the filesystem lacks working
  // byte-range locks (POSIX: fcntl returned EINVAL/ENOTSUP on NFS/overlay;
  // Win32: LockFileEx returned ERROR_NOT_SUPPORTED/ERROR_INVALID_FUNCTION on an
  // exotic redirector such as WebDAV or a Dokan-alike).  Always worth acting
  // on -- deploy the cache on a filesystem with working advisory locks.
  // The gate is implemented on Windows too, so a set gauge is NOT expected
  // there and is NOT normal (it was, when the gate was POSIX-only).
  uint64_t reset_gate_degraded = 0;

  // Monotonic COUNTERS of resets that actually wiped data, split by the gate
  // state they ran under.  NOT cleared by close/reopen (unlike the gauge
  // above).  A freshly created inode counts as NEITHER: it had no peer to
  // endanger, so a routine `rm cyclone.dat` + reload is not an alarm.
  //
  // THE ALARM.  A reset ran while the gate was not in effect, so a live peer
  // MAY have been wiped.  Expected 0.
  uint64_t resets_under_degraded_gate = 0;
  // The healthy upgrade path: a reset ran with the exclusive lifetime lock
  // proven held, i.e. no peer had the volume open.
  uint64_t resets_gate_verified = 0;

  // --- Cross-process RAM coherence (all PROCESS-LOCAL) -------------
  //
  // Both stay 0 unless VolumeConfig::cross_process_ram_coherence is set AND
  // the volume is backed by the shared mmap directory.  Together they are how
  // a consumer tells the feature apart from a no-op and prices the
  // bucket-granularity false-invalidation rate on its own workload.
  //
  // RAM hits DISCARDED because the entry's stamp no longer matched the shared
  // directory bucket version — some process mutated that bucket since the
  // entry was admitted.  Note the arithmetic: the RAM cache counts the hit
  // BEFORE this volume rejects it, so
  //   true_served_ram_hits    = ram_cache_hits - ram_coherence_rejections
  //   false_invalidation_rate = ram_coherence_rejections / ram_cache_hits
  uint64_t ram_coherence_rejections = 0;
  // RAM inserts DECLINED because the bucket moved during the read that would
  // have populated the entry (or was sampled mid-mutation, i.e. an odd
  // version).  The entry would have failed validation on its very first hit,
  // so declining it is a pure saving — but a high value against reads is the
  // same crowding signal as the counter above.
  uint64_t ram_coherence_put_rejections = 0;
};

class Volume;

// Per-thread-shard lifetime anchor for disk-hit read handles.
// A ReadHandle must keep the Volume (for release_borrow and the
// checked renews) and the MappedFile (its spans alias the mapping) alive
// or detectably dead.  Doing that with two weak_ptrs constructed per read
// meant four refcount RMWs per read (two on construction, two on handle
// destruction) on the ONE Volume and ONE MappedFile control block in the
// process — cache lines shared by every reading core, and found in profiling
// as a hard concurrent-read scaling ceiling.
//
// Instead the owning Cache builds one anchor per thread shard after each
// Volume opens (Volume::make_read_anchors) and installs the slot array
// with Volume::set_read_anchors; a disk-hit read copies ITS thread's
// shard's shared_ptr<VolumeReadAnchor> — one refcount RMW on a control
// block only that shard touches.  The anchor holds real strong references,
// so teardown keeps standard shared_ptr semantics with no drain or
// quarantine: Cache::stop()/~Cache stay non-blocking, and a handle that
// outlives them keeps the Volume object and the mapping alive until it
// closes (an upgrade over the weak scheme, where such a handle's spans
// dangled and only destruction/renew were safe).  Volumes not owned by a
// Cache (direct construction in tests) have no anchors and keep the
// original per-read weak_ptr path.
struct VolumeReadAnchor {
  std::shared_ptr<Volume> volume;
  std::shared_ptr<MappedFile> mapped_file;
  // Per-GENERATION teardown latch, set (under the cache's exclusive gate)
  // in Cache::stop() before the volume's stripes are freed.  Anchored
  // handle paths that dereference a raw Stripe* gate on THIS, not on
  // Volume::_teardown: the volume-level flag is reset by a later open(),
  // which would re-arm stale gen-1 handles after a stop()/start() cycle
  // and let their close/renew dereference the freed old stripes
  // (ASan-confirmed use-after-free during review).  Old-generation
  // anchors stay torn forever; start() builds fresh ones.
  //
  // Residual (pre-existing, same shape as the legacy weak path's
  // _teardown check): a handle destructor that loads torn == false while
  // stop() is concurrently mid-phase-3 can still reach a stripe being
  // freed — the latch is a deterministic-generation guard, not a lock.
  std::atomic<bool> torn{false};
};

// Receipt for a registered live borrow: returned by
// Volume::acquire_borrow when a disk-hit read registers itself in the
// stripe's outstanding-borrow slot, surrendered to Volume::release_borrow
// on ReadHandle close/destruction.  `counted` is false when the slot was
// saturated (the borrow rides along unprotected-by-count but the pegged
// count keeps the stripe protected); `active` is false for RAM hits and
// when leases are disabled.
struct BorrowToken {
  uint8_t generation = 0;
  bool counted = false;
  bool active = false;
  // Which of the stripe's local borrow shards holds this borrow's count
  // (see Stripe::local_borrow_shards).  Unused for mmap stripes.
  uint8_t shard = 0;
};

class Volume : public std::enable_shared_from_this<Volume> {
 public:
  explicit Volume(VolumeConfig config);
  Volume(VolumeConfig config, const MultiProcessConfig &mp_config);
  ~Volume();

  // ===================================================================
  // F6 (close the reservation-to-pwrite tear window): a reserved-but-
  // unfilled write slot.  allocate_write_slot() reserves the byte range but
  // does NOT advance the guard-visible write cursor and, in multi-process
  // mode, RETURNS WITH THE CROSS-PROCESS WRITE LOCK STILL HELD.  The caller
  // pwrites the reserved bytes, then calls commit_write_slot() which advances
  // the cursor (single-process: stripe->write_pos; multi-process:
  // shared_write_pos) and releases the lock -- so a lock-free reader never sees
  // the cursor cover reserved-but-torn bytes.  Holding the lock across the
  // pwrite is safe since the write-lock lifetime fix: a force-release proves
  // the holder dead (kill(pid,0)) before recovering, so a live process
  // mid-pwrite is never usurped by routine recovery.  RESIDUAL, stated honestly
  // (W2-class, bounded, NOT closed): the multi-second last-resort escalation
  // CAN usurp a live holder stalled longer than the escalation deadline inside
  // its pwrite/fsync (the held window scales with doc_size — uncapped — plus,
  // under sync_on_write, the fsync, which now also runs under the lock).
  // The generation bump makes the usurped holder's commit_write_slot
  // revalidate fail, so its INSERT is skipped — but its already-in-flight
  // pwrite can still land over the span the usurper reserved at the same
  // un-advanced cursor and already published, tearing the usurper's bytes
  // UNDER a live directory entry.  That tear is detectable, never a wrong
  // serve: the CRC + full-key gauntlet resolves it to Corrupted/miss (pinned
  // by the F6-F escalation-usurp test in test_wrap_phase_aba.cpp); only the
  // escalation deadline bounds how late the stalled pwrite can land.
  struct WriteSlot {
    uint64_t write_offset = 0;   // reserved start (absolute)
    uint64_t new_write_pos = 0;  // write_offset + doc_size (absolute)
    MmapDirectory::WriteLockToken write_token{};
    // Multi-process: write_token is held and must be released by
    // commit_write_slot.  Single-process: false (no cross-process lock).
    bool holds_write_lock = false;
    // True in the F6 fixed protocol: the guard-visible cursor advance is
    // DEFERRED to commit_write_slot (after the fill).  False only under the
    // pre-F6 demonstrator seam, where allocate_write_slot already advanced the
    // cursor + released the lock at reservation (reopening the tear window).
    bool deferred_publish = true;
  };

  // TEST SEAM ONLY -- never set in production.  When true, allocate_write_slot
  // reverts to the pre-F6 behavior: it advances the guard-visible write cursor
  // (publishes shared_write_pos / advances stripe->write_pos) AT RESERVATION
  // and releases the write lock BEFORE the caller's pwrite, reopening the
  // reservation-to-pwrite tear window F6 closes.  Lets the F6 regression tests
  // exhibit the torn read and prove the fix removes it.  Mirrors
  // MmapDirectory::s_write_lock_presume_dead_for_test.
  static inline std::atomic<bool> s_advance_cursor_at_reservation_for_test{
      false};

  // TEST SEAM ONLY -- never installed in production.  Invoked at the
  // reservation->pwrite boundary of commit_write / commit_alternate_write:
  // AFTER allocate_write_slot reserved the byte range (and, in multi-process
  // mode, while the cross-process write lock is STILL HELD) and BEFORE the
  // pwrite fills it.  Lets an F6 test freeze a writer inside the tear window
  // and probe a concurrent reader (which must miss because the guard-visible
  // cursor has not advanced).  Default-empty: one predicted-not-taken branch on
  // the write path only, never the lock-free read path.  Installed before the
  // writer starts and cleared after it joins, so accesses are ordered.
  using WriteTearGateHook =
      std::function<void(uint64_t write_offset, uint64_t new_write_pos)>;
  static inline WriteTearGateHook s_write_tear_gate_for_test{};

  // TEST SEAM ONLY -- never installed in production.  Points inside the
  // writer's wrap-intent window at which a test can pause the writer (to
  // probe a concurrent reader) or kill the process (crash recovery).  Same
  // shape and cost as s_write_tear_gate_for_test: one predicted-not-taken
  // branch, on the write path only, never on the lock-free read path.
  // Every seam fires with the stripe mutex held and, in multi-process mode,
  // the cross-process write lock held.
  enum class WriterSeam : uint8_t {
    kAfterIntentSet,   // intent stored, gate loads not yet run
    kAfterGatePassed,  // gate said "proceed", nothing published yet
    kWrapAfterCursor,  // wrap only: cursor lowered to the data-area start,
                       // epoch not yet stored
    kAfterEpochStore,  // epoch stored, intent not yet cleared
  };
  using WriterSeamHook = std::function<void(WriterSeam seam)>;
  static inline WriterSeamHook s_writer_seam_for_test{};

  Volume(const Volume &) = delete;
  Volume &operator=(const Volume &) = delete;

  std::expected<void, CacheError> open();
  void close();
  bool is_open() const { return _fd >= 0; }

#ifndef _WIN32
  // Live backing-file identity (POSIX): {st_dev, st_ino} from fstat(_fd), or
  // nullopt when _fd < 0 or fstat fails.  The superseded-file GC builds its
  // keep-set from THIS normalized kernel identity, never from string paths --
  // an un-normalized path (symlink / ".." / relative alias) could miss a live
  // file and let GC delete it.  POSIX-only: GC does not run on Windows.
  [[nodiscard]] std::optional<std::pair<dev_t, ino_t>> backing_identity() const;
#endif

  // Reset/purge the volume - clears all data and writes fresh header
  std::expected<void, CacheError> reset();

  Task<std::expected<ReadHandle, CacheError>> open_read(const CacheKey &key);
  Task<std::expected<WriteHandle, CacheError>> open_write(
      const CacheKey &key, uint64_t content_length);
  Task<std::expected<void, CacheError>> remove(const CacheKey &key);
  Task<std::expected<bool, CacheError>> exists(const CacheKey &key);

  std::expected<ReadHandle, CacheError> read_sync(const CacheKey &key);
  std::expected<WriteHandle, CacheError> write_sync(const CacheKey &key,
                                                    uint64_t content_length);
  std::expected<void, CacheError> remove_sync(const CacheKey &key);
  std::expected<bool, CacheError> exists_sync(const CacheKey &key);

  // Alternate chain operations
  std::expected<std::vector<AlternateInfo>, CacheError> list_alternates_sync(
      const CacheKey &key);
  std::expected<WriteHandle, CacheError> write_alternate_sync(
      const CacheKey &key, AlternateId alternate_id, uint64_t content_length);

  // Read with alternate selection
  std::expected<ReadHandle, CacheError> read_alternate_sync(
      const CacheKey &key, const StorageAlternateSelector &selector,
      const AlternateSelectionContext &ctx);

  // Remove specific alternate from chain
  std::expected<void, CacheError> remove_alternate_sync(
      const CacheKey &key, AlternateId alternate_id);

  const VolumeConfig &config() const { return _config; }
  VolumeStats stats() const;

  uint64_t capacity() const { return _config.size; }
  uint64_t bytes_used() const;

  void set_ram_cache(std::shared_ptr<RamCache> cache);

  // Evict a specific (key, alternate) entry from the RAM cache.
  // No-op if RAM cache is disabled or the entry is not present.
  // Best-effort: no remove_epoch bump pairs with this eviction, so an
  // in-flight read can re-put the entry after it (see the Cache
  // API's doc).
  void evict_from_ram_cache(const CacheKey &key, AlternateId id);

  // Hit tracking
  void set_hit_tracker(std::shared_ptr<HitTracker> tracker);
  HitTracker *hit_tracker() const { return _hit_tracker.get(); }

  // Update hit count for a specific alternate (called by flush callback)
  // Implements actual persistence of hit counts
  std::expected<void, CacheError> update_hit_count_sync(
      const CacheKey &key, AlternateId alternate_id, uint32_t hit_delta,
      int64_t last_access_ms);

  // Cross-process-safe in-place mutation of a live published document's fixed
  // header, shared by the two RMW sites (hit-count bump; middle/tail chain
  // repoint).  Runs `apply` (pure std::atomic_ref stores on the mapped
  // `header`, no syscalls) under the stripe write lock, fenced against a wrap
  // recycling the target's bytes since `epoch_start` (which the caller MUST
  // sample before it resolved the offset).  `blocking` selects
  // acquire_write_lock (control path) vs try_acquire_write_lock (hit path).
  // Returns Busy on contention or a raced wrap; the caller drops or retries.
  std::expected<void, CacheError> commit_header_rmw(
      Stripe *stripe, std::byte *header, const CacheKey &expected_key,
      AlternateId expected_alt, std::pair<uint64_t, bool> epoch_start,
      bool blocking, const std::function<void()> &apply);

  // Repoint one chain link in place: store `new_next_offset` into the
  // next_alternate_offset field of the LIVE published document at
  // `pred_absolute_offset`, through commit_header_rmw (write lock + wrap-epoch
  // fence + key/id identity).  Maps read-write before the lock and unmaps
  // after, so no map/unmap syscall runs under the lock.  `epoch_start` MUST be
  // the sample taken before the predecessor offset was resolved.  Returns Busy
  // when the fence or the identity check rejects the store; the caller decides
  // whether to retry (removal) or leave the chain as it was (write-path
  // splice).  Does NOT fsync -- the caller owns that.
  //
  // ORDERING INVARIANT (crash safety), binding on every caller: when a pass
  // repoints SEVERAL links, it must store FRONT-TO-BACK, i.e. in increasing
  // chain distance from the head.  After the i-th store the chain reads
  // keeper[0..i] followed by the original tail from keeper[i+1] -- a valid,
  // merely longer list.  Every intermediate state is well-formed and each
  // store is monotone, so a crash or a Busy at any point leaves a chain a
  // reader can walk.  Back-to-front stores can drop keepers.
  std::expected<void, CacheError> repoint_chain_link(
      Stripe *stripe, uint64_t pred_absolute_offset,
      AlternateId pred_alternate_id, const CacheKey &key,
      uint64_t new_next_offset, std::pair<uint64_t, bool> epoch_start,
      bool blocking);

  // Flush volume file descriptor to disk (fsync on POSIX, _commit on Windows).
  // Called once per HitTracker flush cycle to ensure pwrite'd hit counts are
  // visible to other processes (needed for Docker virtiofs / NFS / non-unified
  // page caches).
  std::expected<void, CacheError> fsync_volume() const;

  // Counting wrapper around the raw fd fsync: bumps _fsyncs then performs
  // the platform fsync.  Centralizes fsync accounting so the per-write
  // fsync convoy is observable.
  int fsync_fd() const;

  // Flush the persistent (mmap'd) directory pages to disk.  Called
  // periodically (every CacheConfig::directory_sync_interval) to bound the
  // power-loss window for published directory entries.  For volumes without
  // an mmap'd directory there are no regions to flush, but the POSIX path
  // still fsyncs the data fd (not a pure no-op) — unreachable via the
  // periodic syncer today, which only runs in persistent mode.  The
  // authoritative durability contract lives on the implementation in
  // volume.cpp.
  std::expected<void, CacheError> sync_directory() const;

  // Called by WriteHandle to commit data (internal use)
  std::expected<void, CacheError> commit_write(
      Stripe *stripe, const CacheKey &key, std::span<const std::byte> header,
      std::span<const std::byte> content);

  // Called by WriteHandle for alternates (internal use)
  std::expected<void, CacheError> commit_alternate_write(
      Stripe *stripe, const CacheKey &key, AlternateId alternate_id,
      std::span<const std::byte> header, std::span<const std::byte> content);

  // Re-stamp the read lease on a stripe (CAS-max now + T with the
  // write-avoidance guard).  Called by ReadHandle::renew_lease() for
  // client-paced transfers.  Returns false when leases are disabled
  // (T == 0), i.e. there is no protection to renew.
  bool renew_read_lease(Stripe *stripe,
                        const std::pair<uint64_t, bool> &epoch_start);
  // Lease amendment (2026-07-07): intent-checked lease renewal for the ALIASED
  // zero-copy serve path.  Stamps the lease FIRST, then Dekker-revalidates
  // (borrow_still_valid: wrap_intent loaded before epoch) so a normal wrap
  // racing at the lease-lapse boundary observes the fresh stamp and defers
  // rather than overwriting an in-flight aliased send.  The epoch-only
  // renew_read_lease() above is safe ONLY for the copy-then-verify path,
  // whose read is observable; an aliased writev's read is not.  See
  // LeaseRenewal for how the embedder must act on each result.
  LeaseRenewal renew_read_lease_strict(
      Stripe *stripe, const std::pair<uint64_t, bool> &epoch_start);
  // Lease-protocol STEP-3: ns until a ceiling-forced wrap could overwrite a
  // borrow on this stripe (UINT64_MAX = none deferred / leases off).
  [[nodiscard]] uint64_t ns_until_forced_wrap(const Stripe *stripe) const;

  // drop a live borrow from the stripe's outstanding-borrow
  // slot.  Called by the disk-hit ReadHandle on close/destruction (public
  // for the same reason renew_read_lease is: the handle impl lives outside
  // the class).  Generation-checked: a release that arrives after a
  // ceiling-forced wrap reset the slot is a no-op.  Inert tokens (RAM hit,
  // leases disabled, saturated-slot ride-alongs) are ignored.
  void release_borrow(Stripe *stripe, BorrowToken token);

  // Read-handle anchors (see VolumeReadAnchor above).  The Cache
  // calls make_read_anchors() after open() succeeds (requires this Volume
  // to be shared_ptr-owned and _mapped_file to be set), OWNS the returned
  // vector for as long as reads can run, and installs its slots with
  // set_read_anchors().  set_read_anchors(nullptr, 0) detaches (done under
  // the cache's exclusive gate before close()); count must be a power of
  // two.  Volumes without installed anchors serve reads through the
  // legacy weak_ptr path.
  static constexpr size_t kReadAnchorShards = 64;
  [[nodiscard]] std::vector<std::shared_ptr<VolumeReadAnchor>>
  make_read_anchors();
  void set_read_anchors(const std::shared_ptr<VolumeReadAnchor> *slots,
                        size_t count);

 private:
  VolumeConfig _config;
  MultiProcessConfig _mp_config;
  int _fd = -1;
  // Set when the reset gate could not use advisory locking (unsupported fs) and
  // fell back to the historical ungated behaviour.  Surfaced via stats(); see
  // VolumeStats::reset_gate_degraded.  A GAUGE: cleared on every open().
  std::atomic<bool> _reset_gate_degraded{false};
  // Monotonic COUNTERS of resets that actually wiped data, split by the gate
  // state they ran under (see ResetProvenance in volume.cpp).  Unlike the gauge
  // above they are NEVER cleared on open() -- a reset that already happened
  // stays counted for the life of the process.  A freshly created inode counts
  // as NEITHER (no peer was possible).  Surfaced via stats().
  std::atomic<std::uint64_t> _resets_under_degraded_gate{0};
  std::atomic<std::uint64_t> _resets_gate_verified{0};
  // Set at the TOP of close(), BEFORE _stripes is destroyed, and cleared
  // on (re)open.  The handle-facing entry points that dereference a raw
  // Stripe* (release_borrow, the renew paths, ns_until_forced_wrap) bail
  // out when set: a ReadHandle outliving Cache::stop() violates the
  // documented handle-lifetime contract, but — like the volume_weak pin
  // those paths already take — this makes the common violation (a
  // client-paced drain, or a handle destroyed after stop() while the
  // Volume object itself is still alive) fail cleanly instead of touching
  // freed stripes.  A borrow release skipped here is the documented
  // leaked-count case: process-local slots die with the process, shared
  // (mmap) slots are cleared by the next ceiling-forced wrap's reset.
  std::atomic<bool> _teardown{false};
#ifdef _WIN32
  std::mutex _fd_mutex;  // Serializes _lseeki64+_write/_read (not atomic like
                         // pwrite/pread)
#endif

  std::vector<std::unique_ptr<Stripe>> _stripes;
  std::shared_ptr<MappedFile>
      _mapped_file;  // shared_ptr for ReadHandle lifetime safety
  std::shared_ptr<RamCache> _ram_cache;
  std::shared_ptr<HitTracker> _hit_tracker;

  // view into the Cache-owned anchor slot array (see
  // VolumeReadAnchor).  nullptr when unowned/detached; the pointees stay
  // valid whenever a read can be in flight (installed and detached under
  // the cache's exclusive gate).
  const std::shared_ptr<VolumeReadAnchor> *_read_anchor_slots = nullptr;
  size_t _read_anchor_count = 0;
  [[nodiscard]] std::shared_ptr<VolumeReadAnchor> read_anchor() const {
    if (_read_anchor_slots == nullptr) {
      return nullptr;
    }
    return _read_anchor_slots[thread_shard_index(_read_anchor_count)];
  }

  // Sharded read counter (perf: read-path scaling).  ++_reads on
  // EVERY cache read hammered one global atomic's cache line across all
  // cores — a stripe-count-independent scaling ceiling (measured: read
  // throughput stopped scaling past ~4 threads, and 512MB/4-stripe read
  // identically to 128MB/1-stripe, ruling out the stripe lock).  Split the
  // increment across padded, per-thread shards; stats() sums them.  Pure
  // statistics — no reader ever loads an individual shard, so relaxed
  // ordering suffices.
  struct alignas(kShardPad) ReadCounterShard {
    std::atomic<uint64_t> value{0};
  };
  static constexpr size_t kReadCounterShards = 64;
  mutable std::array<ReadCounterShard, kReadCounterShards> _reads_sharded{};
  void bump_reads() const {
    _reads_sharded[thread_shard_index(kReadCounterShards)].value.fetch_add(
        1, std::memory_order_relaxed);
  }
  uint64_t reads_total() const {
    uint64_t sum = 0;
    for (const auto &shard : _reads_sharded) {
      sum += shard.value.load(std::memory_order_relaxed);
    }
    return sum;
  }
  mutable std::atomic<uint64_t> _writes{0};
  mutable std::atomic<uint64_t> _evictions{0};
  // Completed sync_directory() calls — observable evidence (via stats())
  // that the periodic directory sync actually ran; used by the power-loss
  // regression test.
  mutable std::atomic<uint64_t> _directory_syncs{0};

  // Total raw fd fsyncs performed by this volume (init + per-write when
  // sync_on_write is set + removal + periodic directory sync).  Instrument
  // for the per-write-fsync convoy.
  mutable std::atomic<uint64_t> _fsyncs{0};

  // Readahead hints actually issued by this volume (see VolumeStats).  Only
  // bumped when the re-advise filter lets a hint through, so it is off the
  // warm read path; it is the observable that makes the filter testable.
  mutable std::atomic<uint64_t> _readahead_hints{0};

  // Wrap-cadence telemetry (see VolumeStats for semantics). Each wrap event
  // is already serialized per stripe (stripe->mutex + cross-process
  // write_lock), so atomics only cover concurrent wraps on different
  // stripes and lock-free reads from stats(). Times are steady-clock ns;
  // 0 = "never wrapped". _wrap_count only counts wraps on non-mmap stripes
  // — mmap stripes count in the shared directory header instead (see
  // MmapDirectory::record_shared_wrap); stats() sums both. The time and
  // interval fields are process-local (intervals cover only wraps performed
  // by this process).
  std::atomic<uint64_t> _wrap_count{0};
  std::atomic<uint64_t> _last_wrap_time_ns{0};
  std::atomic<uint64_t> _last_wrap_interval_ns{0};
  std::atomic<uint64_t> _min_wrap_interval_ns{0};

  // Read-lease counters (process-local; see VolumeStats).
  std::atomic<uint64_t> _wraps_deferred_by_lease{0};
  std::atomic<uint64_t> _writes_dropped_by_lease{0};
  std::atomic<uint64_t> _wraps_forced_past_lease{0};

  // Full-bucket tag-collision evictions (process-local; see VolumeStats).
  std::atomic<uint64_t> _tag_collision_evictions{0};

  // Full-bucket nearest-to-clobber evictions (process-local; see VolumeStats).
  std::atomic<uint64_t> _bucket_full_evictions{0};

  // Alternate-chain shadow bound (process-local; see VolumeStats).
  std::atomic<uint64_t> _alternate_shadows_unlinked{0};
  std::atomic<uint64_t> _alternate_splice_deferred{0};
  std::atomic<uint64_t> _alternate_chain_resets{0};
  std::atomic<uint64_t> _alternate_max_chain_depth{0};
  std::atomic<uint64_t> _alternate_wrap_refusals{0};

  // Cross-process write-lock recovery telemetry (process-local; see
  // VolumeStats and MmapDirectory::acquire_write_lock).
  std::atomic<uint64_t> _write_lock_force_releases{0};
  std::atomic<uint64_t> _write_lock_escalation_takeovers{0};
  std::atomic<uint64_t> _write_lock_live_holder_waits{0};
  std::atomic<uint64_t> _write_lock_usurp_aborts{0};

  // Cross-process RAM coherence (process-local, see VolumeStats).
  std::atomic<uint64_t> _ram_coherence_rejections{0};
  std::atomic<uint64_t> _ram_coherence_put_rejections{0};

  // Does the read path validate RAM hits on this stripe against the
  // shared directory's bucket version?  The knob alone is NOT enough — the
  // signal has to exist.  Without the mmap directory Stripe::bucket_version()
  // returns 0 for everything, so validating would compare 0 against 0 forever;
  // gating here instead keeps single-process mode genuinely INERT under the
  // toggle rather than rejecting every hit.  One plain bool load off a config
  // held by value plus two already-hot loads; perfectly predicted.
  [[nodiscard]] bool ram_coherence_active(const Stripe *stripe) const {
    return _config.cross_process_ram_coherence && stripe != nullptr &&
           stripe->use_mmap_directory && stripe->mmap_directory.has_value();
  }

  // Lease parameters derived from VolumeConfig at construction
  // (steady-clock ns).  _lease_t_ns == 0 disables the protocol entirely.
  uint64_t _lease_t_ns = 0;
  uint64_t _lease_ceiling_ns = 0;

  // Record one write-buffer wrap on the given stripe (called from the
  // commit paths, under the stripe's locks, at the point where write_pos
  // resets to the start of the data area).
  void record_wrap(Stripe *stripe) noexcept;

  // --- Lease protocol helpers -------------------------------------------
  // Reader side: stamp the stripe lease (CAS-max now + T, seq_cst, with
  // the write-avoidance guard).  Must run after the entry is located and
  // BEFORE the borrow escapes; the caller then revalidates wrap_epoch().
  void stamp_read_lease(Stripe *stripe) const;

  // Reader side: register a live borrow in the stripe's
  // outstanding-borrow slot (seq_cst CAS; count+1 unless saturated).  Must
  // run — like the lease stamp — after the entry is located and BEFORE the
  // borrow escapes; the caller then revalidates via borrow_still_valid()
  // and must release_borrow() on a failed revalidation.  Returns an inert
  // token when leases are disabled.
  [[nodiscard]] BorrowToken acquire_borrow(Stripe *stripe);

  // Wrap epoch {wrap_count, phase} for stamp-then-revalidate.  Captured at
  // probe start and compared after the stamp; a change means a wrap /
  // phase toggle raced the read and the borrow must be discarded.
  // Deliberately independent of the checksum-validation cache.
  [[nodiscard]] std::pair<uint64_t, bool> wrap_epoch(
      const Stripe *stripe) const;

  // Reader-side borrow revalidation, run after stamp_read_lease(): the
  // borrow is valid only if no writer holds the wrap-intent flag (loaded
  // seq_cst FIRST — the intent-before-epoch order is part of the Dekker
  // proof at allocate_write_slot) AND the wrap epoch still matches the
  // probe-start capture.  On false the caller unmaps and retries/misses.
  [[nodiscard]] bool borrow_still_valid(
      const Stripe *stripe, const std::pair<uint64_t, bool> &epoch_start) const;

  // Writer-side wrap-intent flag (shared header offset 33 in mmap mode,
  // Stripe::local_wrap_intent otherwise), seq_cst.
  void set_wrap_intent(Stripe *stripe, bool active);
  [[nodiscard]] bool wrap_intent_set(const Stripe *stripe) const;

  // Writer side: wrap gate, run BEFORE evict_if_needed()/toggle_phase()
  // at BOTH wrap sites.  Returns true when the wrap may proceed: no
  // outstanding borrows (a residual lease timestamp without a
  // live ReadHandle no longer blocks), no/expired/staleness-clamped lease,
  // or anti-starvation ceiling exceeded (increments
  // wraps_forced_past_lease AND resets the borrow slot so leaked counts
  // cost at most one ceiling episode).  Returns false when the wrap must
  // be deferred — a live borrow with a live lease (zero side effects; the
  // caller drops the fill).  seq_cst gate loads (Dekker pairing with the
  // reader's count-CAS + stamp).
  bool lease_permits_wrap(Stripe *stripe);

  // One shared allocate-with-lease-check helper used by BOTH commit paths
  // (commit_write and commit_alternate_write) so a third write path cannot
  // fork the wrap behavior.  Handles the cross-process write lock, shared
  // write_pos sync, capacity checks, the lease gate, eviction/wrap
  // and offset reservation.  Returns the reserved absolute write offset.
  //
  // F6: reserves the byte range but returns WITHOUT advancing the guard-
  // visible write cursor and (multi-process) WITH THE WRITE LOCK STILL HELD;
  // the caller pwrites, then calls commit_write_slot().  See WriteSlot.
  std::expected<WriteSlot, CacheError> allocate_write_slot(Stripe *stripe,
                                                           size_t doc_size);

  // F6: run by BOTH commit paths AFTER the pwrite (and the optional
  // sync_on_write fsync).  On fill_ok it advances the guard-visible write
  // cursor to cover the now-durable fill and releases the cross-process write
  // lock; on !fill_ok it advances NOTHING (invariant: the guard/shared cursor
  // never exceeds the durably-written frontier) and still releases the lock --
  // the reserved span becomes a harmless gap reclaimed at the next wrap.
  // Returns Busy (does NOT release) if a last-resort escalation usurped the
  // lock mid-pwrite, so the caller skips its directory insert.
  std::expected<void, CacheError> commit_write_slot(Stripe *stripe,
                                                    const WriteSlot &slot,
                                                    bool fill_ok);

  // CRC validation cache: skip CRC32 on subsequent reads of already-verified
  // entries.  Direct-mapped by file offset; the stored CRC32 value itself
  // serves as the discriminator — if the content changes, the checksum
  // stored in the document header changes, causing a cache miss and
  // re-verification.
  //
  // Uses Fibonacci hashing (multiply by golden-ratio constant, take top bits)
  // instead of bare modulo to avoid systematic collisions when documents have
  // similar sizes (e.g., all ~8KB → offsets differ by 8192 → bare % 2048
  // maps them all to the same slot).
  //
  // Thread-safe via atomic 64-bit load/store (lock-free on x86/ARM).
  // Packed format: [48-bit offset | 16-bit checksum(top bits of CRC32)].
  // 48-bit offset supports volumes up to 256TB.  The 16-bit discriminator
  // is NOT the full CRC32: an index collision between different offsets
  // only causes a harmless re-verification (the full offset is compared),
  // but same-offset content replacement whose new CRC32 matches the old
  // TOP 16 BITS (2^-16 per rewrite) skips verification of the new bytes.
  // That is acceptable ONLY because this cache is not the overwrite guard:
  // the wrap-epoch revalidation (deliberately independent of this
  // cache) is what protects readers from raced overwrites, and any
  // document reachable through the directory was fully written before its
  // entry was published.  Do not lean on this cache for corruption
  // detection of rewritten offsets.
  // 64K slots (512 KB per volume), sized for large-cardinality working
  // sets: with the previous 2048 slots, any working set much beyond ~2k
  // documents collision-evicted validations continuously and effectively
  // re-verified CRC32 on EVERY read (profiled as the top hot-path cost on a
  // 20k-entry corpus).
  static constexpr size_t kChecksumCacheSize = 65536;
  static constexpr unsigned kChecksumCacheShift = 64 - 16;  // log2(65536)
  // Heap-backed (not an inline member): at 65536 slots this array is 512 KB.
  // As an inline member it pushed sizeof(Volume) past 520 KB, which overflows
  // a 1 MB thread stack (Windows default) the moment a Volume is stack-
  // allocated (e.g. a test's `Volume v(...)`, or any non-heap construction).
  // Volume is always make_shared'd in production; one pointer indirection on
  // the read hot path is negligible next to the CRC it saves.
  std::unique_ptr<std::array<std::atomic<uint64_t>, kChecksumCacheSize>>
      _checksum_cache = std::make_unique<
          std::array<std::atomic<uint64_t>, kChecksumCacheSize>>();

  static size_t checksum_cache_index(uint64_t offset) {
    return static_cast<size_t>((offset * uint64_t{0x9E3779B97F4A7C15}) >>
                               kChecksumCacheShift);
  }
  static uint64_t pack_checksum_entry(uint64_t offset, uint32_t checksum) {
    // Low 48 bits: offset.  High 16 bits: top 16 bits of CRC32.
    return (offset & 0x0000FFFFFFFFFFFF) |
           (static_cast<uint64_t>(checksum >> 16) << 48);
  }
  bool is_checksum_validated(uint64_t offset, uint32_t checksum) const {
    uint64_t expected = pack_checksum_entry(offset, checksum);
    uint64_t stored = (*_checksum_cache)[checksum_cache_index(offset)].load(
        std::memory_order_relaxed);
    return stored != 0 && stored == expected;
  }
  void mark_checksum_validated(uint64_t offset, uint32_t checksum) {
    (*_checksum_cache)[checksum_cache_index(offset)].store(
        pack_checksum_entry(offset, checksum), std::memory_order_relaxed);
  }

  // Large-document readahead (see VolumeConfig::readahead_min_bytes).
  // Called on the disk read path once a document's byte range is known but
  // before anything touches its content, so the kernel can fetch the range
  // with a few large asynchronous I/Os instead of one serial fault per page
  // (the volume mapping is MADV_RANDOM, which otherwise suppresses
  // readahead entirely).
  //
  // CONCURRENCY: this is a pure hint over an address range.  Apart from the
  // lossy dedupe filter below it takes no lock, reads and writes no shared
  // cache state, and never dereferences the region, so it is safe to call
  // outside the borrow/lease window and participates in none of the reader
  // protocols.  All errors are ignored.
  void maybe_advise_readahead(uint64_t doc_offset,
                              std::span<std::byte> region) noexcept;

  // Re-advise filter for the readahead hint.  The hint only pays for itself
  // when the range is NOT already resident: once it is, the madvise() still
  // walks every page, and on a warm 2 MiB view-mode read that walk cost
  // more than the read itself (261 k -> 68 k gets/s measured).  So a
  // document placement is advised at most once every
  // kReadaheadReadviseSeconds.
  //
  // Why DECAYED and not once-ever: a KV tier is normally larger than RAM,
  // so "advised once, evicted from the page cache, read cold again" is the
  // common case, not the exception -- a permanent filter would silently
  // drop the readahead exactly where it is worth most.  The interval only
  // has to be long enough that a hot key cannot pay for a madvise() per
  // read: at 275 k gets/s on one key, 2 s caps it at one hint per ~550 k
  // reads.
  //
  // Direct-mapped, lossy and lock-free BY DESIGN, exactly like
  // _checksum_cache: a collision, a torn pairing or a lost update costs one
  // redundant or one skipped hint and nothing else, so it needs no
  // synchronisation and is never consulted for correctness.  The warm path
  // is a single relaxed load; the store only happens when a hint is
  // actually issued.  8192 slots = 64 KB per volume.
  //
  // Allocated ONLY when the hint is enabled (readahead_min_bytes != 0), and
  // then once, here, at construction -- before the Volume can be published
  // to any reader.  _config is fixed for the Volume's lifetime, so the
  // pointer never changes after construction and concurrent readers load it
  // without synchronisation.  When the hint is off the pointer stays null
  // and is never dereferenced: maybe_advise_readahead() returns on the
  // zero threshold before it would reach the filter.  (Declared after
  // _config, so the initializer below sees the final configuration.)
  static constexpr size_t kReadaheadCacheSize = 8192;
  static constexpr unsigned kReadaheadCacheShift = 64 - 13;  // log2(8192)
  static constexpr uint32_t kReadaheadReadviseSeconds = 2;
  using ReadaheadCache = std::array<std::atomic<uint64_t>, kReadaheadCacheSize>;
  static std::unique_ptr<ReadaheadCache> make_readahead_cache(
      size_t min_bytes) {
    return min_bytes != 0 ? std::make_unique<ReadaheadCache>() : nullptr;
  }
  const std::unique_ptr<ReadaheadCache> _readahead_cache =
      make_readahead_cache(_config.readahead_min_bytes);

  static size_t readahead_cache_index(uint64_t offset) {
    return static_cast<size_t>((offset * uint64_t{0x9E3779B97F4A7C15}) >>
                               kReadaheadCacheShift);
  }

  // Slot layout: high 40 bits identify the document placement, low 24 bits
  // carry the steady-clock second at which it was last advised.
  static constexpr unsigned kReadaheadTickBits = 24;
  static constexpr uint32_t kReadaheadTickMask = (1u << kReadaheadTickBits) - 1;

  // Placement identity: offset AND length, so a reused offset now holding a
  // differently sized document re-advises.  Forced nonzero so a populated
  // slot never reads as the empty (zero) slot.
  static uint64_t readahead_discriminator(uint64_t offset, size_t length) {
    uint64_t mixed =
        (offset * uint64_t{0x9E3779B97F4A7C15}) ^
        (static_cast<uint64_t>(length) * uint64_t{0xD6E8FEB86659FD93});
    return ((mixed >> 24) | 1u) & 0xFFFFFFFFFFull;
  }

  // Seconds off the steady clock, truncated to kReadaheadTickBits.  The
  // epoch is irrelevant: only modular differences are compared, so the
  // ~194-day wrap costs at most one redundant or skipped hint.
  static uint32_t readahead_tick() noexcept;

  Stripe *select_stripe(const CacheKey &key);
  // Body of open() that runs under the exclusive cross-process init
  // lock; see volume.cpp.
  std::expected<void, CacheError> open_locked(bool created_new);
  // `exclusive` = this opener proved it has no live peer (it holds the
  // exclusive lifetime lock, or created the inode): only then may it touch
  // the shared reader-exclusion state (see repair_wrap_state).
  std::expected<void, CacheError> init_stripes(bool exclusive);

  // Crash recovery for a writer that died INSIDE the wrap-intent window: its
  // wrap_intent flag stays set (every read of the stripe misses until the
  // next wrap) and the phase may have drifted from the pass count.  Clears
  // the intent and re-derives phase = pass & 1.  Called ONLY when no writer
  // can be inside the window: after a write-lock acquisition that
  // force-released a holder PROVEN dead (never after an escalated takeover
  // of a holder that may still be alive), or on an exclusive open.
  void repair_wrap_state(Stripe *stripe);
  // Run repair_wrap_state iff `token` recovered the lock from a proven-dead
  // holder and escalated over no possibly-live one.
  void repair_after_forced_release(Stripe *stripe,
                                   const MmapDirectory::WriteLockToken &token);
  // Fire the writer seam (test only; a no-op in production).
  static void writer_seam(WriterSeam seam) {
    if (s_writer_seam_for_test) {
      s_writer_seam_for_test(seam);
    }
  }
  // Lease-protocol STEP-3: publish the per-stripe force-wrap deadline (ns; 0 =
  // none) to shared (mmap) or process-local storage.
  void publish_wrap_deferred_deadline(Stripe *stripe, uint64_t deadline_ns);

  // Multi-process: check if a stripe is owned by this process
  bool is_stripe_owned(size_t stripe_index) const {
    if (!_mp_config.enabled) {
      return true;  // All stripes owned when multi-process disabled
    }
    return (stripe_index % _mp_config.total_processes) ==
           _mp_config.process_index;
  }

  // Volume header operations
  std::expected<VolumeHeader, CacheError> read_header() const;
  std::expected<void, CacheError> write_header(
      const VolumeHeader &header) const;

  // Evict entries to make space
  void evict_if_needed(Stripe *stripe, size_t required_space);
};

// Volume is always heap-allocated (make_shared).  Keep it small enough that a
// stray stack allocation cannot blow a 1 MB thread stack (the Windows default):
// the 512 KB CRC-validation cache is heap-backed for exactly this reason (see
// _checksum_cache).  Heap-back any large new member instead of adding it
// inline.
static_assert(sizeof(Volume) < size_t{64} * 1024,
              "sizeof(Volume) must stay well under a 1 MB stack");

#ifndef _WIN32
// Opt-in startup GC of SUPERSEDED structural-fingerprint cache files (disk
// hygiene ONLY; full design-rationale + invariants I1..I5 at the definition in
// volume.cpp).  For each live volume's directory it deletes fingerprinted files
// that no live peer holds, proving each safe first: name-shape match +
// base-stem match against a live volume + inode NOT in the live keep-set + age
// floor, then O_RDWR open + a non-blocking EXCLUSIVE lifetime-lock probe
// (Acquired proves no live peer) + inode revalidation + a valid VolumeHeader
// magic, all BEFORE the unlink.  It NEVER deletes a live/locked file, a legacy
// un-fingerprinted cyclone.dat, or a foreign look-alike, and NEVER fails
// start(); every failure path is a non-fatal skip (fail-closed).  Called from
// Cache::start() iff CacheConfig::gc_superseded_on_start.  POSIX-only.
// `age_floor` (default 60s) is a RACE-REDUCER, not a safety property; it is
// injectable for tests.
void gc_superseded_volumes(
    const std::vector<Volume *> &live_volumes,
    std::chrono::seconds age_floor = std::chrono::seconds(60));
#endif

}  // namespace cyclone
