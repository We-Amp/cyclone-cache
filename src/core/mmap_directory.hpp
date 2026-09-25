// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <thread>
#include <type_traits>
#include <utility>

#include "cyclone/key.hpp"
#include "directory.hpp"

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <immintrin.h>
#endif

// TSan annotations for seqlock synchronization on DirEntry fields.
// Defined as no-ops when ThreadSanitizer is not active.
#ifndef CYCLONE_TSAN_ANNOTATIONS_DEFINED
#define CYCLONE_TSAN_ANNOTATIONS_DEFINED
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#include <sanitizer/tsan_interface.h>
#define CYCLONE_TSAN_ACQUIRE(addr) __tsan_acquire(addr)
#define CYCLONE_TSAN_RELEASE(addr) __tsan_release(addr)
#else
#define CYCLONE_TSAN_ACQUIRE(addr)
#define CYCLONE_TSAN_RELEASE(addr)
#endif
#elif defined(__SANITIZE_THREAD__)
#include <sanitizer/tsan_interface.h>
#define CYCLONE_TSAN_ACQUIRE(addr) __tsan_acquire(addr)
#define CYCLONE_TSAN_RELEASE(addr) __tsan_release(addr)
#else
#define CYCLONE_TSAN_ACQUIRE(addr)
#define CYCLONE_TSAN_RELEASE(addr)
#endif
#endif

namespace cyclone {

// Outstanding-borrow accounting: a packed {generation:8, count} slot
// manipulated with seq_cst RMWs.  Two widths share one implementation:
//
//   uint16_t {gen:8, count:8}   the process-local per-thread, per-chunk
//                               shard slots (Stripe::BorrowShard);
//   uint32_t {gen:8, count:24}  the cross-process per-chunk slots in the
//                               mmap directory's retention region (see
//                               MmapDirectory::RetentionRegion), where every
//                               process's borrows of one chunk share a slot.
//
// Templated on the atomic-like type (std::atomic or std::atomic_ref); the
// packed word type is whatever that type's load() returns.  The generation
// is always the top 8 bits.
//
// count — live disk-hit borrows (open ReadHandles) whose document starts in
// the slot's chunk.  The gate (Volume::lease_gate) defers a wrap or a
// frontier advance over that chunk only while count > 0 AND the read lease
// is live: a closed handle releases write capacity immediately instead of
// blocking writers for the rest of its lease (the write-starvation fix).
// Saturates at the field maximum (255 for u16, 2^24-1 for u32): a borrow
// acquired at saturation rides along uncounted (counted == false) and its
// release is a no-op — the slot stays maximally protective and drains as
// the counted holders close.  The 24-bit cross-process field makes that
// ride-along unreachable in practice.
//
// generation — ABA guard for leaked counts.  A ceiling-forced wrap or
// advance calls force_reset (generation+1, count = 0) on EVERY chunk slot,
// so a count leaked by a crashed borrow holder starves the stripe for at
// most one lease_wrap_ceiling episode; release() drops decrements whose
// generation no longer matches (their increment is gone with the reset).
// Residual risk: an 8-bit generation recurs after exactly 256 forced
// resets, so a handle held across 256 ceiling episodes (>= 256 x
// lease_wrap_ceiling, ~4h at defaults, its bytes long since
// force-overwritten) that closes at that precise recurrence mis-decrements
// one live borrow — accepted as negligible.
namespace borrow_slot {

template <typename Word>
inline constexpr unsigned kCountBits = sizeof(Word) * 8 - 8;
template <typename Word>
inline constexpr Word kCountMaskOf =
    static_cast<Word>((uint64_t{1} << kCountBits<Word>)-1);

template <typename Word>
[[nodiscard]] constexpr uint8_t generation_of(Word slot) {
  return static_cast<uint8_t>(slot >> kCountBits<Word>);
}
template <typename Word>
[[nodiscard]] constexpr uint32_t count_of(Word slot) {
  return static_cast<uint32_t>(slot & kCountMaskOf<Word>);
}
template <typename Word>
[[nodiscard]] constexpr Word pack_of(uint8_t gen, uint32_t cnt) {
  return static_cast<Word>((static_cast<uint64_t>(gen) << kCountBits<Word>) |
                           (cnt & kCountMaskOf<Word>));
}

// The 16-bit spelling (process-local shard slots), kept by name.
inline constexpr uint16_t kCountMask = kCountMaskOf<uint16_t>;
inline constexpr unsigned kGenerationShift = kCountBits<uint16_t>;
inline constexpr uint8_t kCountSaturated = 0xFFU;

[[nodiscard]] constexpr uint8_t generation(uint16_t slot) {
  return generation_of<uint16_t>(slot);
}
[[nodiscard]] constexpr uint8_t count(uint16_t slot) {
  return static_cast<uint8_t>(count_of<uint16_t>(slot));
}
[[nodiscard]] constexpr uint16_t pack(uint8_t gen, uint8_t cnt) {
  return pack_of<uint16_t>(gen, cnt);
}

struct Acquired {
  uint8_t generation = 0;
  bool counted = false;  // false = slot was saturated; release is a no-op
};

// Register a borrow: count+1 (seq_cst CAS), unless saturated.  Run BEFORE
// the borrow escapes and before the wrap-intent/epoch revalidation — the
// seq_cst RMW is the reader's half of the Dekker pairing with the
// writer's intent-store-then-count-load (proof at
// Volume::allocate_write_slot).
template <typename AtomicWord>
[[nodiscard]] Acquired acquire(AtomicWord &slot) {
  using Word = std::remove_cv_t<decltype(slot.load())>;
  Word cur = slot.load(std::memory_order_seq_cst);
  for (;;) {
    if (count_of<Word>(cur) == kCountMaskOf<Word>) {
      return {generation_of<Word>(cur), false};
    }
    Word next =
        pack_of<Word>(generation_of<Word>(cur), count_of<Word>(cur) + 1);
    if (slot.compare_exchange_weak(cur, next, std::memory_order_seq_cst,
                                   std::memory_order_seq_cst)) {
      return {generation_of<Word>(cur), true};
    }
  }
}

// Drop a counted borrow: count-1 iff the generation still matches (a
// mismatch means a forced reset cleared the slot since the acquire — the
// increment is gone, so the release must not touch the new epoch's count).
template <typename AtomicWord>
void release(AtomicWord &slot, uint8_t gen) {
  using Word = std::remove_cv_t<decltype(slot.load())>;
  Word cur = slot.load(std::memory_order_seq_cst);
  for (;;) {
    if (generation_of<Word>(cur) != gen || count_of<Word>(cur) == 0) {
      return;
    }
    Word next = pack_of<Word>(gen, count_of<Word>(cur) - 1);
    if (slot.compare_exchange_weak(cur, next, std::memory_order_seq_cst,
                                   std::memory_order_seq_cst)) {
      return;
    }
  }
}

// Ceiling-forced reset: invalidate all outstanding counts (generation+1,
// count = 0) so leaked state cannot starve the stripe past one ceiling
// episode.  No-op (generation preserved) when the count is already 0, so
// generations only burn when there was live-or-leaked state to clear.
// Returns true when a reset happened.
template <typename AtomicWord>
bool force_reset(AtomicWord &slot) {
  using Word = std::remove_cv_t<decltype(slot.load())>;
  Word cur = slot.load(std::memory_order_seq_cst);
  for (;;) {
    if (count_of<Word>(cur) == 0) {
      return false;
    }
    Word next =
        pack_of<Word>(static_cast<uint8_t>(generation_of<Word>(cur) + 1), 0);
    if (slot.compare_exchange_weak(cur, next, std::memory_order_seq_cst,
                                   std::memory_order_seq_cst)) {
      return true;
    }
  }
}

}  // namespace borrow_slot

/// Memory-mapped directory for cross-process cache sharing.
///
/// MmapDirectory stores directory entries directly in mmap'd memory,
/// allowing multiple processes to share the same directory. It uses
/// a seqlock pattern for lock-free reads with torn-read detection.
///
/// Memory Layout:
/// ```
/// ┌────────────────────────────────────────┐
/// │ MmapDirectoryHeader (64 bytes)         │
/// ├────────────────────────────────────────┤
/// │ Version counters (4 bytes per bucket)  │
/// ├────────────────────────────────────────┤
/// │ Directory entries (10 bytes each)      │
/// │   - 4 entries per bucket               │
/// ├────────────────────────────────────────┤
/// │ Retention region (v2, 8-byte aligned)  │
/// │   - exposure generation G (8 bytes)    │
/// │   - 64 per-chunk borrow slots (u32)    │
/// └────────────────────────────────────────┘
/// ```
/// The retention region lives in what used to be slack between the entries
/// and the page-rounded data offset, so the stripe's data offset is
/// unchanged (static_assert in volume.cpp).
///
/// Thread/Process Safety:
/// - Multiple readers can read concurrently (lock-free)
/// - Writers use atomic version counters for synchronization
/// - Torn reads are detected via version mismatch and retried
class MmapDirectory {
 public:
  static constexpr size_t kEntriesPerBucket = 4;
  // Seqlock read pacing (spins, retries, how long) is SeqlockReadWait's
  // call (directory.hpp), shared with Directory.
  static constexpr uint32_t kMagic = 0x4D444952;  // "MDIR"
  // Version 2 (wrap retention): the retention region after the entries
  // (exposure generation + per-chunk borrow slots) replaces the stripe-wide
  // borrow slot at header offset 34, and the reader epoch is the exposure
  // generation instead of {shared_wrap_count, current_phase}.  A v1 binary
  // would neither count its borrows where a v2 writer looks nor honour the
  // generation, so the two must never share a directory: kVersion is mixed
  // into the fingerprinted filename (fingerprint_cache_path), and a v2
  // opener never init()s over a v1 directory unless it holds the exclusive
  // lifetime lock (Volume::open_locked).
  static constexpr uint16_t kVersion = 2;

  // Wrap retention: the maximum number of frontier chunks per stripe (N in
  // doc/design/wrap-retention.md) and therefore of per-chunk borrow slots.
  static constexpr size_t kMaxChunks = 64;

  /// Cross-process retention state, placed 8-byte aligned right after the
  /// directory entries.  Zeroed by init() like everything else.
  struct RetentionRegion {
    // G = pass * (N + 1) + frontier: the exposure generation, the ONLY
    // reader epoch in v2.  Stored seq_cst, only by a write_lock holder.
    uint64_t exposure_gen;
    // Per-chunk outstanding-borrow slots, packed {generation:8, count:24}
    // (borrow_slot helpers).  Chunk c counts every live borrow, from ANY
    // process, of a document whose first byte lies in chunk c.
    uint32_t chunk_borrows[kMaxChunks];
  };
  static_assert(sizeof(RetentionRegion) == 264,
                "RetentionRegion is on-disk format (G + 64 x u32)");
  static_assert(offsetof(RetentionRegion, chunk_borrows) == 8,
                "chunk slots follow G");

  /// Header stored at the beginning of the mmap'd region
  struct Header {
    uint32_t magic;         // Magic number for validation
    uint16_t version;       // Format version
    uint16_t reserved;      // Padding
    uint32_t num_buckets;   // Number of buckets
    uint32_t entry_count;   // Current entry count (approximate)
    uint8_t current_phase;  // GC phase (atomic access via std::atomic_ref)
    // Cross-process CAS lock serializing an insert's phase read + entry
    // store against a phase toggle.  0 = unlocked; otherwise the holder's
    // token, derived from the acquisition generation phase_lock_gen
    // (offset 34; see acquire_phase_lock for the protocol).  A pre-#27
    // build of this same
    // format (kVersion 2) takes it with CAS 0->1 and releases with a store
    // of 0, so both treat any nonzero value as held and exclude each other.
    uint8_t phase_lock;
    // write_lock takeover generation + owner, carved from the former
    // pad1[6] (offsets 18-23; the bytes stay naturally aligned: gen at 18
    // is 2-aligned, owner_pid at 20 is 4-aligned, shared_write_pos stays at
    // 24).  Same layout-compat argument as every other carved field:
    // init() has always zeroed this padding, open() never validated it, and
    // older builds never read or wrote it — so version stays 1 and a
    // skewed old-build peer degrades to today's presume-dead behavior on
    // the write lock, never to corruption (it simply won't publish an owner
    // or bump the generation).  See acquire_write_lock() for the protocol:
    // every acquisition and every force-release bumps write_lock_gen (the
    // "was I usurped" signal a holder revalidates, and the holder identity
    // a waiter watches), write_lock_owner_pid carries the
    // holder's PID so a waiter can PROVE it dead (kill(pid,0)) before ever
    // force-releasing, instead of presuming death after a spin count.
    // write_lock_owner_pid is the current holder's PID (0 = none/unpublished).
    uint16_t write_lock_gen;
    uint32_t write_lock_owner_pid;
    uint64_t shared_write_pos;  // Shared write position — an ABSOLUTE file
                                // offset (set from stripe->write_pos; bounds
                                // stripe->data_offset..stripe->offset+size),
                                // NOT relative to the stripe.  The phase-ABA
                                // positional guard compares absolute entry
                                // offsets against it — do not "fix" these
                                // semantics.  0 = uninitialized (use
                                // data_offset)

    uint8_t write_lock;  // Cross-process CAS spinlock for write-pos allocation
                         // (0=unlocked, 1=locked)

    // Wrap-intent flag, carved from the FIRST byte of the former
    // pad2 (offset 33; same layout-compat argument as offsets 40/48/56:
    // init() has always zeroed this region, open() never validates it,
    // and older builds never read or write it outside init()).  Set to 1
    // (seq_cst) by a writer BEFORE its lease-gate load and cleared after
    // the wrap decision completes (defer, or toggle/reset/record done),
    // all under write_lock.  Readers load it (seq_cst, BEFORE the epoch
    // re-read) during borrow revalidation: the store-then-load shape on
    // BOTH sides closes the Dekker window the lease load alone leaves
    // open — proof at Volume::allocate_write_slot (volume.cpp).
    uint8_t wrap_intent;

    // Offsets 34-35.  In format version 2: phase_lock's acquisition
    // generation, bumped by every acquisition and every recovery of the
    // phase lock (acquire_phase_lock).  It identifies the holder to a
    // waiter, and a holder's token is derived from it, so a usurped
    // holder's late release is a no-op.
    // Pre-#27 version-2 builds never read these bytes and wrote them (zero)
    // only in init() and on an exclusive open, where no holder can exist.
    //
    // In format version 1 this was the per-stripe outstanding-borrow slot,
    // carved from the LAST 2 bytes of the former pad2 (pad2 is now fully
    // spent).
    // Packed {generation:8, count:8} — see the borrow_slot helpers below.
    // count is the number of live disk-hit borrows (open ReadHandles) on
    // this stripe across ALL processes, incremented (seq_cst CAS) before a
    // borrow escapes and decremented on ReadHandle close/destruction.  The
    // wrap gate defers a wrap only while count > 0 AND the lease at offset
    // 56 is live — a promptly-closed read no longer blocks writers for the
    // rest of its lease (the write-starvation fix).  generation is
    // bumped (and count zeroed) by a ceiling-forced wrap so state leaked
    // by a crashed borrow holder costs at most one lease_wrap_ceiling
    // episode; stale releases are dropped by the generation check.
    // Same layout-compat argument as the other carved fields: init() has
    // always zeroed these bytes, open() never validates them, and older
    // builds never read or write them outside init() — version stays 1.
    // Skew caveat: an old-build reader process does not count its borrows,
    // so during a rolling upgrade its borrows are protected only by the
    // lease timestamp, as before this fix.
    //
    // RETIRED in version 2: borrows are counted per chunk in the retention
    // region (RetentionRegion::chunk_borrows), and the two bytes were reused
    // for phase_lock_gen (above).
    uint16_t phase_lock_gen;

    // Lease-protocol STEP-3 (2026-07-07): cross-process per-stripe force-wrap
    // deadline, carved from pad2 (offset 36).  steady-clock MILLISECONDS
    // (truncated to uint32) of the instant a ceiling-forced wrap becomes
    // reachable on this stripe (= deferral start + lease_wrap_ceiling);
    // 0 = no wrap currently deferred.  Published seq_cst by the deferring
    // writer so a READER process serving a zero-copy borrow can copy the
    // aliased bytes out of the mmap BEFORE the force-wrap overwrites them
    // (win-preserving copy-trigger).  Same layout-compat argument as the
    // other carved fields: init() zeroes it, open() never validates it,
    // old builds never touch it -> version stays 1; skew degrades to the
    // conservative always-copy behavior, never to corruption.
    uint32_t shared_wrap_deferred_deadline_ms;

    // Wrap-cadence telemetry, carved out of the former trailing padding
    // (offsets 40 and 48). Layout-compatible with version-1 headers written
    // by older builds: init() has always zeroed this region, open() never
    // validates it, and older builds never read or write these bytes — so
    // 0 keeps its "no data yet" meaning in both skew directions. Caveat:
    // an old-build writer process does not update these fields, so during
    // a rolling upgrade the shared counters can undercount.
    // Updated under write_lock; read lock-free via std::atomic_ref.
    uint64_t shared_wrap_count;         // Write-buffer wraps on this stripe,
                                        // across all processes
    uint64_t shared_last_wrap_time_ns;  // steady-clock ns of the most recent
                                        // wrap (CLOCK_MONOTONIC is
                                        // boot-relative, so comparable across
                                        // processes on one host).
                                        // 0 = never wrapped.

    // Read-lease slot, carved out of the LAST
    // 8 bytes of the former trailing padding.  steady-clock ns expiry of
    // the newest read lease on this stripe; 0 = no lease.  Same
    // layout-compat argument as the wrap counters above: init() has always
    // zeroed this region, open() never validates it, and older builds
    // never read or write it outside init() — so version stays 1 and skew
    // degrades to today's (unprotected) behavior, never worse.
    // Readers stamp via seq_cst CAS-max (stamp_lease_expiry); writers load
    // seq_cst before any wrap side effect (Dekker pairing, see volume.cpp).
    //
    // The header is FULLY SPENT — no padding remains anywhere: the former
    // pad1 is carved into write_lock_gen (18) and write_lock_owner_pid (20),
    // the former pad2 (33-39) into wrap_intent (33), phase_lock_gen
    // (34-35; the v1 stripe_borrow_slot) and shared_wrap_deferred_deadline_ms
    // (36-39), and the former trailing padding into the wrap counters (40/48)
    // and this lease (56). This note cannot drift: kFixedFieldsSize below sums
    // every field and the static_assert beneath the struct pins the total to
    // kHeaderSize, so any new field breaks the build until it forces a version
    // bump and a migration story.
    uint64_t stripe_lease_expiry_ns;

    static constexpr size_t kFixedFieldsSize =
        sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint16_t) +
        sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint8_t) +
        sizeof(uint8_t) + sizeof(uint16_t) + sizeof(uint32_t) +
        sizeof(uint64_t) + sizeof(uint8_t) + sizeof(uint8_t) +
        sizeof(uint16_t) + sizeof(uint32_t) + sizeof(uint64_t) +
        sizeof(uint64_t) + sizeof(uint64_t);
    static constexpr size_t kHeaderSize = 64;
  };
  static_assert(sizeof(Header) == 64, "Header must be 64 bytes");
  static_assert(Header::kFixedFieldsSize == Header::kHeaderSize,
                "Header layout accounting is stale; the header is fully "
                "spent, so a new field means a version bump and a layout "
                "decision");
  static_assert(offsetof(Header, write_lock_gen) == 18,
                "write_lock_gen must stay at offset 18 (on-disk format)");
  static_assert(offsetof(Header, write_lock_owner_pid) == 20,
                "write_lock_owner_pid must stay at offset 20 (on-disk format)");
  static_assert(offsetof(Header, wrap_intent) == 33,
                "wrap_intent must stay at offset 33 (on-disk format)");
  static_assert(offsetof(Header, phase_lock_gen) == 34,
                "phase_lock_gen must stay at offset 34 (on-disk format)");
  static_assert(offsetof(Header, shared_wrap_deferred_deadline_ms) == 36,
                "shared_wrap_deferred_deadline_ms must stay at offset 36 "
                "(on-disk format)");
  static_assert(offsetof(Header, shared_wrap_count) == 40,
                "shared_wrap_count must stay at offset 40 (on-disk format)");
  static_assert(offsetof(Header, shared_last_wrap_time_ns) == 48,
                "shared_last_wrap_time_ns must stay at offset 48 "
                "(on-disk format)");
  static_assert(offsetof(Header, stripe_lease_expiry_ns) == 56,
                "stripe_lease_expiry_ns must stay at offset 56 "
                "(on-disk format)");
  // Cross-process atomics require lock-free operations (no internal mutex).
  static_assert(std::atomic_ref<uint8_t>::is_always_lock_free,
                "phase_lock requires lock-free uint8_t atomics");
  static_assert(std::atomic_ref<uint16_t>::is_always_lock_free,
                "phase_lock_gen requires lock-free uint16_t atomics");
  static_assert(std::atomic_ref<uint32_t>::is_always_lock_free,
                "seqlock versions require lock-free uint32_t atomics");
  static_assert(std::atomic_ref<uint64_t>::is_always_lock_free,
                "shared_write_pos requires lock-free uint64_t atomics");

  /// Byte offsets of the version counters, the entries and the retention
  /// region for `num_buckets` (caller guarantees no overflow).
  static constexpr size_t entries_offset(size_t num_buckets) {
    return (sizeof(Header) + num_buckets * sizeof(uint32_t) + 7) &
           ~static_cast<size_t>(7);
  }
  static constexpr size_t retention_offset(size_t num_buckets) {
    return (entries_offset(num_buckets) +
            num_buckets * kEntriesPerBucket * sizeof(DirEntry) + 7) &
           ~static_cast<size_t>(7);
  }

  /// Calculate the total size needed for a directory with given bucket count
  /// (SIZE_MAX on overflow).
  static constexpr size_t required_size(size_t num_buckets) {
    // Overflow guards: each product and sum below must fit size_t.
    if (num_buckets > SIZE_MAX / (kEntriesPerBucket * sizeof(DirEntry) +
                                  sizeof(uint32_t) + 1)) {
      return SIZE_MAX;
    }
    return retention_offset(num_buckets) + sizeof(RetentionRegion);
  }

  /// Initialize a new directory in the given memory region
  /// Returns nullopt if region is too small or num_buckets would overflow
  static std::optional<MmapDirectory> init(std::span<std::byte> region,
                                           size_t num_buckets);

  /// Open an existing directory from the given memory region
  static std::optional<MmapDirectory> open(std::span<std::byte> region);

  /// Probe for an entry matching the key.  Test-only (no production
  /// callers).  Returns nullopt if not found, and also if a writer held the
  /// bucket past the wait budget (probe_each reports that case instead).
  [[nodiscard]] std::optional<DirEntry> probe(const CacheKey &key) const;

  /// Iterate over all matching entries without allocation
  /// Callback returns false to stop iteration.  Returns false iff a writer
  /// held the bucket for the whole SeqlockReadWait budget, i.e. the bucket's
  /// contents are unknown (see Directory::probe_each).
  template <typename Callback>
  bool probe_each(const CacheKey &key, Callback &&callback) const {
    return probe_each_impl<true>(key, std::forward<Callback>(callback));
  }

  /// As probe_each, but yields tag matches of BOTH phases; the caller
  /// classifies each entry against its own stripe snapshot (see
  /// Directory::probe_each_all_phases).
  template <typename Callback>
  bool probe_each_all_phases(const CacheKey &key, Callback &&callback) const {
    return probe_each_impl<false>(key, std::forward<Callback>(callback));
  }

#ifdef CYCLONE_TEST_SEAMS
  /// TEST-SEAM BUILDS ONLY.  Take / release this key's bucket as a writer
  /// (even->odd / odd->even), so a test can park a writer inside its odd
  /// window.  The caller provides the writer serialization, as production
  /// mutators do.
  [[nodiscard]] uint32_t begin_bucket_write_for_test(const CacheKey &key) {
    uint32_t token = 0;
    (void)acquire_writer(key.bucket_hash() % _num_buckets, token,
                         /*capped=*/false);
    return token;
  }
  void end_bucket_write_for_test(const CacheKey &key, uint32_t token) {
    release_writer(key.bucket_hash() % _num_buckets, token);
  }
#endif

  /// Sentinels for insert()'s verified_offset parameter — shared semantics
  /// with the in-memory directory (see the discussion on Directory).
  static constexpr uint64_t kMatchAnyTag = Directory::kMatchAnyTag;
  static constexpr uint64_t kNoVerifiedEntry = Directory::kNoVerifiedEntry;

  /// Insert or update an entry
  /// Returns true on success
  /// verified_offset selects which same-tag entry (if any) may be updated
  /// in place (a DirEntry holds no key material, so a tag match alone may
  /// be a colliding foreign key); *collision_evicted reports a full-bucket
  /// collider eviction, *bucket_full_evicted a full-bucket eviction of the
  /// entry nearest the wrap cursor.  See the sentinels on
  /// Directory.
  /// With `admission`: the victim order and the in-bracket uniqueness
  /// cleanup of Directory::insert (see there); the admission view is
  /// refreshed INSIDE the bracket, under phase_lock.
  /// A capped wait on the phase lock or the bucket that gave up (see
  /// kLockWaitCap) sets *busy and publishes nothing.
  bool insert(const CacheKey &key, uint64_t offset, uint64_t size,
              uint64_t verified_offset = kMatchAnyTag,
              bool *collision_evicted = nullptr,
              bool *bucket_full_evicted = nullptr,
              InsertAdmission *admission = nullptr,
              std::span<const uint64_t> clear_offsets = {},
              bool *busy = nullptr);

  /// Remove an entry
  /// Returns true if entry was found and removed
  /// WARNING: matches on the 12-bit tag only (a DirEntry holds no key
  /// material), so a colliding foreign key's entry can be removed.
  /// Collision-safe removal must verify the stored first_key and use
  /// remove_at() — see Volume::remove_sync.  No production call sites.
  bool remove(const CacheKey &key);

  /// Remove an entry matching both tag and offset (precise removal)
  /// A capped wait on the bucket that gave up sets *busy and removes
  /// nothing (see kLockWaitCap).
  bool remove_at(const CacheKey &key, uint64_t target_offset,
                 bool *busy = nullptr);

  /// Clear all entries
  void clear();

  /// Get approximate entry count
  [[nodiscard]] size_t count() const;

  /// Get total capacity (number of entries)
  [[nodiscard]] size_t capacity() const;

  /// Get number of buckets
  [[nodiscard]] size_t bucket_count() const;

  /// Get current GC phase
  [[nodiscard]] bool current_phase() const;

  /// Toggle GC phase
  void toggle_phase();

  /// Store the GC phase outright (seq_cst, under phase_lock).  Used only to
  /// RE-DERIVE the phase from the pass count (phase = pass & 1) after a
  /// writer was proven to have died inside the wrap window, or on an
  /// exclusive open -- never on the ordinary wrap path.
  void set_current_phase(bool phase);

  /// Exclusive-open reset of the reader-exclusion state (no live peer can
  /// exist, the caller holds the exclusive lifetime lock): zero the chunk
  /// borrow slots, the read lease and the published force deadline, and
  /// free the phase lock.  Anything left there belonged to processes that
  /// are gone.
  void reset_reader_state_exclusive();

  /// Get shared write position (ABSOLUTE file offset, 0 = unset)
  [[nodiscard]] uint64_t get_shared_write_pos() const;

  /// Set shared write position (ABSOLUTE file offset)
  void set_shared_write_pos(uint64_t pos);

  /// Record one write-buffer wrap in the shared header (wrap-cadence
  /// telemetry): increments shared_wrap_count and publishes now_ns as
  /// shared_last_wrap_time_ns. Must be called with the write lock held
  /// (the wrap decision itself happens under it).
  void record_shared_wrap(uint64_t now_ns);

  /// Total write-buffer wraps on this stripe, across all processes.
  [[nodiscard]] uint64_t shared_wrap_count() const;

  /// steady-clock ns of the most recent wrap on this stripe (any process);
  /// 0 = never wrapped.
  [[nodiscard]] uint64_t shared_last_wrap_time_ns() const;

  /// Read-lease expiry (steady-clock ns, 0 = no lease), seq_cst load.
  /// The writer-side lease gate uses this before any wrap side effect
  /// (Dekker pairing with the reader's stamp).
  [[nodiscard]] uint64_t lease_expiry_ns() const;

  /// Stamp the read lease: CAS-max to new_expiry_ns, seq_cst, with the
  /// write-avoidance guard — the CAS is skipped entirely when the current
  /// expiry already covers skip_if_at_least_ns (= now + T - T/4).  Never
  /// lowers the stored value (a bogus far-future value is handled by the
  /// writer's staleness clamp instead).
  void stamp_lease_expiry(uint64_t new_expiry_ns, uint64_t skip_if_at_least_ns);

  /// Raw packed {generation:8, count:24} borrow slot of chunk `chunk`
  /// (< kMaxChunks), seq_cst load.  The writer-side gate reads the slots of
  /// the chunks it is about to expose (count > 0 = borrows outstanding)
  /// after its intent store and before any side effect.
  [[nodiscard]] uint32_t chunk_borrow_raw(size_t chunk) const;

  /// Register a live borrow of a document starting in `chunk` (seq_cst
  /// CAS; count+1 unless saturated).  Must run BEFORE the borrow escapes
  /// and before the intent/epoch revalidation — see borrow_slot::acquire.
  [[nodiscard]] borrow_slot::Acquired chunk_borrow_acquire(size_t chunk);

  /// Drop a counted borrow (generation-checked decrement); no-op when a
  /// forced reset cleared the slot since the matching acquire.
  void chunk_borrow_release(size_t chunk, uint8_t generation);

  /// Ceiling-forced reset of EVERY chunk slot (generation+1, count = 0 on
  /// each nonzero slot), so one ceiling episode clears leaked counts in all
  /// chunks.  Returns true when any outstanding state was cleared.
  bool chunk_borrows_force_reset_all();

  /// Exposure generation G (seq_cst load); 0 on an invalid directory.
  [[nodiscard]] uint64_t exposure_gen() const;

  /// Publish a new exposure generation (seq_cst store).  Writer-side only,
  /// under the write lock, inside the intent window.
  void set_exposure_gen(uint64_t gen);

  /// Wrap-intent flag (seq_cst load).  True while a writer is
  /// inside the wrap decision + publish window; readers must discard the
  /// borrow and retry.
  [[nodiscard]] bool wrap_intent() const;

  /// Set/clear the wrap-intent flag (seq_cst store).  Writer-side only,
  /// under the write lock: set BEFORE the lease-gate load, cleared once
  /// the wrap decision completes (defer or publish).
  void set_wrap_intent(bool active);

  /// Wrap-intent VALUES.  Readers only test != 0.  The value tells crash
  /// recovery how far a dead writer got (Volume::repair_wrap_state):
  ///   kIntentStep       a gate decision or a frontier advance is in
  ///                     flight; nothing irreversible has happened yet,
  ///                     or the step's only store is G itself -- clearing
  ///                     is the whole repair;
  ///   kIntentWrapEven/  a wrap is COMMITTED to pass P' (P' even / odd):
  ///   kIntentWrapOdd    stored before the cursor drops to S, so the
  ///                     cursor, phase and G may be anywhere between the
  ///                     old pass and P'.  Recovery must COMPLETE the wrap
  ///                     (cursor := S, phase := P' & 1, G := P'), never
  ///                     just clear the flag.
  static constexpr uint8_t kIntentStep = 1;
  static constexpr uint8_t kIntentWrapEven = 2;
  static constexpr uint8_t kIntentWrapOdd = 3;
  [[nodiscard]] uint8_t wrap_intent_value() const;
  void set_wrap_intent_value(uint8_t value);

  /// Lease-protocol STEP-3: the published cross-process force-wrap deadline
  /// (steady-clock ms; 0 = no deferred wrap).  Set by the deferring
  /// writer; read by a zero-copy embedder to copy-out before the force.
  [[nodiscard]] uint32_t wrap_deferred_deadline_ms() const;
  void set_wrap_deferred_deadline_ms(uint32_t deadline_ms);

  /// A phase-lock holder's token: the acquisition generation it bumped
  /// Header::phase_lock_gen to, and the nonzero value derived from it that
  /// it stored into Header::phase_lock (see acquire_phase_lock).
  struct PhaseLockToken {
    uint16_t generation = 0;
    uint8_t value = 0;
  };

  // How long ONE holder may keep a cross-process lock before a waiter
  // presumes it stuck and recovers the lock (issue #27; LockHolderWait).
  // Measured on macOS (10 cores): two processes writing one volume as
  // process 0 of 1, 2 writer threads each, plus CPU hogs.  Live holders held
  // a bucket for at most 0.6 ms with 12 hogs, but up to 56 ms with 24-48
  // hogs (2.8-5.2 runnable threads per core); the phase lock, whose holder
  // also waits on a bucket, up to 112 ms.  The budgets sit well above
  // that: recovering a live holder costs a spurious miss, while a larger
  // budget only lengthens the one-time stall after a process died holding
  // the lock.  The phase-lock budget must exceed the bucket budget plus a
  // live phase hold, since the holder (insert) waits on a bucket inside it.
  static constexpr std::chrono::milliseconds kBucketWriterBudget{250};
  static constexpr std::chrono::milliseconds kPhaseLockBudget{1000};
  // The write lock never presumes a live holder stuck: it recovers the lock
  // from a holder PROVEN dead, and takes over a holder it cannot prove dead
  // only after this last-resort escalation budget.
  static constexpr std::chrono::milliseconds kWriteLockEscalationBudget{5000};
  // A phase-lock holder waits on a bucket; a write-lock holder (wrap) waits
  // on the phase lock.  An inner wait must end well inside the outer budget.
  static_assert(kPhaseLockBudget >= 2 * kBucketWriterBudget);
  static_assert(kWriteLockEscalationBudget >= 2 * kPhaseLockBudget);
  // The write lock consults the holder's liveness (kill(pid, 0) /
  // OpenProcess) only once one holder has kept it this long, far past any
  // ordinary contention, so a waiter does not probe on every short wait.
  // (A holder in another PID namespace looks dead to that probe: every
  // process sharing a volume must share a PID namespace.)
  static constexpr std::chrono::milliseconds kWriteLockProbeAfter{50};
  // Capped waits (inserts, removes, hit-count updates, write-slot
  // reservation): once the holders a waiter has seen COME AND GO add up to
  // this, it gives up and its operation reports Busy -- never a takeover.
  // The locks are not fair, so without a cap a peer that re-acquires within
  // nanoseconds could stall a writer (inline on an nginx event loop, say)
  // for as long as it kept doing so.  Time on the current holder does not
  // count, so a stuck holder is still recovered by the per-holder budget:
  // a capped acquisition waits at most about cap + budget + 2 ms (bucket
  // ~0.5 s, phase lock ~1.25 s, write lock ~5.25 s for a live holder it
  // cannot prove dead, ~0.3 s for a dead one).  250 ms is over twice the
  // longest total phase-lock wait measured under 5.2 runnable threads per
  // core (103 ms), so ordinary contention does not give up.
  static constexpr std::chrono::milliseconds kLockWaitCap{250};

#ifdef CYCLONE_TEST_SEAMS
  /// TEST-SEAM BUILDS ONLY.  Nonzero overrides kBucketWriterBudget /
  /// kPhaseLockBudget / kWriteLockEscalationBudget, in microseconds.
  static inline std::atomic<uint64_t> s_bucket_writer_budget_us_for_test{0};
  static inline std::atomic<uint64_t> s_phase_lock_budget_us_for_test{0};
  static inline std::atomic<uint64_t> s_write_lock_escalation_us_for_test{0};
  /// Nonzero overrides kLockWaitCap, in microseconds.
  static inline std::atomic<uint64_t> s_lock_wait_cap_us_for_test{0};
  /// Capped waits that gave up (process-wide, all three locks).
  static inline std::atomic<uint64_t> s_lock_give_ups_for_test{0};
  /// Recoveries of a stuck holder, per lock kind (process-wide).
  static inline std::atomic<uint64_t> s_bucket_recoveries_for_test{0};
  static inline std::atomic<uint64_t> s_phase_lock_recoveries_for_test{0};

  /// Take / release the phase lock as insert() does, so a test can park a
  /// live holder inside it.
  [[nodiscard]] PhaseLockToken acquire_phase_lock_for_test() {
    PhaseLockToken token;
    (void)acquire_phase_lock(token, /*capped=*/false);
    return token;
  }
  void release_phase_lock_for_test(PhaseLockToken token) {
    release_phase_lock(token);
  }
  /// Current phase-lock byte (0 = free) and generation.
  [[nodiscard]] uint8_t phase_lock_value_for_test() const {
    return std::atomic_ref<uint8_t>(const_cast<uint8_t &>(_header->phase_lock))
        .load(std::memory_order_acquire);
  }
  [[nodiscard]] uint16_t phase_lock_gen_for_test() const {
    return std::atomic_ref<uint16_t>(
               const_cast<uint16_t &>(_header->phase_lock_gen))
        .load(std::memory_order_acquire);
  }
  /// The raw write-lock byte (0 = free).
  [[nodiscard]] uint8_t write_lock_value_for_test() const {
    return std::atomic_ref<uint8_t>(const_cast<uint8_t &>(_header->write_lock))
        .load(std::memory_order_acquire);
  }
#endif

  /// Proof that a write-lock acquisition still owns the lock.  Returned by
  /// acquire_write_lock(); passed to revalidate_write_lock()/
  /// release_write_lock().  `generation` is the acquisition generation this
  /// acquisition bumped write_lock_gen to: any later acquisition or
  /// force-release bumps it again, so a holder whose generation still
  /// matches has NOT been usurped.  The remaining
  /// fields are process-local telemetry (never read from shared memory).
  struct WriteLockToken {
    // The acquisition generation this acquisition bumped write_lock_gen to
    // (see acquire_write_lock).  Every later acquisition or force-release
    // bumps it again, so while it still matches we have NOT been usurped.
    uint16_t generation = 0;
    // The value this acquisition's CAS stored into Header::write_lock,
    // derived from `generation` (see acquire_write_lock); the release is a
    // CAS from exactly this value.
    uint8_t value = 0;
    // True once the lock is held.
    bool acquired = false;
    // True when this acquisition recovered the lock from a PROVEN-dead
    // holder (routine crash recovery).
    bool forced_release = false;
    // True when this acquisition took over a holder it could NOT prove
    // dead via the last-resort escalation — the alertable event (PID reuse
    // or a live holder wedged for many seconds).
    bool escalated_takeover = false;
    // True when a capped wait gave up (acquired stays false).
    bool gave_up = false;
    // Number of liveness re-checks spent waiting on a still-live holder
    // (one per sleep of the wait; see LockHolderWait).
    uint32_t live_waits = 0;
  };

  /// Acquire cross-process write lock for write-pos allocation.
  /// Serializes the read-shared_write_pos → pwrite → update sequence
  /// across processes to prevent overlapping writes.  The hot uncontended
  /// path is one generation load, a single CAS and a generation fetch_add.  On
  /// contention the waiter (LockHolderWait: spin, then sleeping backoff) never
  /// usurps a holder it cannot PROVE dead (kill(pid,0)); a genuinely dead
  /// holder is force-released so a crash cannot deadlock the cache, and a
  /// holder that cannot be proven dead is taken over only after it held the
  /// lock for kWriteLockEscalationBudget. Returns a token the caller must feed
  /// to revalidate_write_lock() before ANY shared side effect and to
  /// release_write_lock() when done — discarding it leaks the lock until a
  /// waiter's escalation recovers it.
  /// Capped by default (kLockWaitCap): behind live holders that keep
  /// changing it returns {acquired=false, gave_up=true} and the caller
  /// reports Busy.
  [[nodiscard]] WriteLockToken acquire_write_lock(bool capped = true);

  /// Non-blocking single-CAS acquire for the in-place header RMW sites.
  /// NEVER spins, waits, or usurps: on contention returns {acquired=false}
  /// and the caller applies its own policy (the hit path drops the delta —
  /// best-effort by contract; the control path retries then reports Busy).
  /// A blocking acquire on the hit path would park a request thread behind a
  /// peer's pwrite+fsync.  A race with a recovery is resolved as on the
  /// blocking path (try_take_token_lock), never by a leaked lock.
  [[nodiscard]] WriteLockToken try_acquire_write_lock();

  /// True iff this acquisition still holds the lock (no force-release has
  /// bumped the generation since acquire).  A usurped holder MUST NOT
  /// publish shared_write_pos or let its caller pwrite — doing so is the
  /// overlapping-write corruption this guard closes.
  [[nodiscard]] bool revalidate_write_lock(const WriteLockToken &token) const;

  /// Ownership-checked release: a no-op if a force-release usurped us (the
  /// lock now belongs to the usurper; storing 0 would free ITS critical
  /// section and admit a third writer).  Deliberately the ONLY release —
  /// an unconditional release would free whoever currently holds the lock.
  void release_write_lock(const WriteLockToken &token);

#ifdef CYCLONE_TEST_SEAMS
  /// TEST-SEAM BUILDS ONLY.  When true, acquire_write_lock
  /// reverts to the pre-fix "presume the holder dead after the spin budget"
  /// behavior (no kill(2) liveness proof), so the multi-process regression
  /// test can exhibit the old live-holder usurpation and prove the fix
  /// removes it.
  static inline std::atomic<bool> s_write_lock_presume_dead_for_test{false};

  /// TEST-SEAM BUILDS ONLY.  When nonzero, escalates
  /// after that many live waits (one per sleep of the wait) instead of
  /// kWriteLockEscalationBudget, so the F6-F test can force an
  /// escalated_takeover against a deliberately stalled LIVE holder in
  /// milliseconds and pin the bounded escalation-usurp residual (torn bytes
  /// stay detectable; the usurped holder's commit is refused).  0 = use the
  /// production budget.
  static inline std::atomic<uint32_t> s_write_lock_max_live_waits_for_test{0};
#endif

  /// Current seqlock version of the bucket this key hashes to.  Every
  /// COMPLETED directory mutation advances it by exactly 2
  /// (acquire_writer even->odd, release_writer odd->even), published with
  /// release semantics and shared across processes — so a change since a
  /// sampled value means "something in this bucket moved", whichever process
  /// moved it.  A bare acquire load; no lock, no retry.  Returns 0 on an
  /// invalid directory.
  [[nodiscard]] uint32_t bucket_version(const CacheKey &key) const;

  /// Publish "something reachable through this bucket changed" WITHOUT
  /// changing a DirEntry.  Used only by the chain-repoint path, which
  /// rewrites a predecessor DOCUMENT header and would otherwise leave the
  /// bucket version untouched — invisible to a peer validating its RAM tier
  /// against it.  Implemented as an empty acquire_writer/release_writer
  /// bracket so it inherits the seqlock's fences, TSan annotations and
  /// crashed-holder recovery, and advances the counter by exactly 2 like
  /// every other mutation.  NEVER open-code this as a fetch_add: an odd
  /// delta breaks the even/odd parity the seqlock depends on.
  /// Capped by default: false when it gave up behind live, changing
  /// holders (nothing was published).
  [[nodiscard]] bool touch_bucket(const CacheKey &key, bool capped = true);

  /// Full mapped byte range backing this directory (header + version
  /// counters + entries).  Used for explicit flushes of the directory
  /// region — see Volume::sync_directory().  Empty if invalid.
  [[nodiscard]] std::span<std::byte> region() const;

  /// Check if directory is valid
  [[nodiscard]] bool is_valid() const { return _header != nullptr; }

  // Non-copyable (doesn't own memory, but copying would be confusing)
  MmapDirectory(const MmapDirectory &) = delete;
  MmapDirectory &operator=(const MmapDirectory &) = delete;

  // Movable
  MmapDirectory(MmapDirectory &&other) noexcept;
  MmapDirectory &operator=(MmapDirectory &&other) noexcept;

  /// Default constructor (creates invalid directory)
  MmapDirectory()
      : _header(nullptr),
        _versions(nullptr),
        _entries(nullptr),
        _retention(nullptr),
        _num_buckets(0) {}

  /// True iff `region` starts with a directory header that carries our magic
  /// but a DIFFERENT version -- a directory another binary is (or was)
  /// using.  Such a region must never be init()ed without proof that no
  /// peer is live (Volume::init_stripes).
  static bool is_foreign_version(std::span<const std::byte> region);

 private:
  template <bool kFilterPhase, typename Callback>
  bool probe_each_impl(const CacheKey &key, Callback &&callback) const;

  MmapDirectory(Header *header, uint32_t *versions, DirEntry *entries,
                RetentionRegion *retention, size_t num_buckets);

  /// Get version counter for a bucket (atomic load)
  [[nodiscard]] uint32_t load_version(size_t bucket_idx) const;

  // NOTE: there is deliberately NO single-step "increment_version" helper.
  // A bucket version is a SEQLOCK counter: even = stable, odd = a writer
  // holds the bucket.  The only legal way to advance it is the balanced
  // acquire_writer/release_writer pair (+2, parity preserved).  A helper that
  // added 1 would leave the bucket permanently "write in progress", spinning
  // every subsequent writer and failing every reader's retry loop.  Callers
  // that need to publish a change without touching a DirEntry use the public
  // touch_bucket() above.

  /// Acquire writer lock on a bucket (CAS even→odd; waits out a holder via
  /// LockHolderWait, and recovers the bucket from a holder that kept it odd
  /// for kBucketWriterBudget).  Returns the pre-lock (even) version: the
  /// token release_writer needs.
  /// False only for a capped wait that gave up (see kLockWaitCap).
  [[nodiscard]] bool acquire_writer(size_t bucket_idx, uint32_t &token,
                                    bool capped);

  /// acquire_writer's contended path, out of line.
  bool acquire_writer_slow(size_t bucket_idx, uint32_t &token, bool capped);

  /// Release writer lock: CAS token+1 (odd) -> token+2 (even).  A no-op if
  /// the bucket was force-released from under us (see acquire_writer), so a
  /// late release can never flip the parity back to odd.
  void release_writer(size_t bucket_idx, uint32_t token);

  /// Acquire cross-process phase lock (CAS on header->phase_lock).
  /// Prevents toggle_phase() from invalidating entries mid-insert.  Returns
  /// the holder token release_phase_lock needs.
  /// False only for a capped wait that gave up (see kLockWaitCap).
  [[nodiscard]] bool acquire_phase_lock(PhaseLockToken &token, bool capped);

  /// Release cross-process phase lock: CAS token -> 0, a no-op if the lock
  /// was recovered from under us.
  void release_phase_lock(PhaseLockToken token);

  /// acquire_phase_lock's contended path, out of line.
  bool acquire_phase_lock_slow(PhaseLockToken &token, bool capped);

  /// Atomically increment entry count
  void increment_count();

  /// Atomically decrement entry count
  void decrement_count();

  Header *_header;
  uint32_t *_versions;  // One per bucket (accessed via std::atomic_ref)
  DirEntry *_entries;
  RetentionRegion *_retention;  // after the entries (see RetentionRegion)
  size_t _num_buckets;
};

// Template implementation
template <bool kFilterPhase, typename Callback>
bool MmapDirectory::probe_each_impl(const CacheKey &key,
                                    Callback &&callback) const {
  if (!_header) {
    return true;  // No directory, no entries: a definite (empty) answer
  }

  uint32_t bucket_idx = key.bucket_hash() % _num_buckets;
  uint16_t target_tag = key.tag();

  // Retry loop for torn read detection, bounded by SeqlockReadWait.  Every
  // failed attempt, including each `continue` below, goes through
  // wait.retry() in the loop condition.
  SeqlockReadWait wait;
  do {
    // Capture the phase INSIDE the retry loop: a retry triggered by a
    // concurrent toggle_phase() must rescan with the new phase, or
    // entries stamped after the flip would be invisibly skipped
    // (spurious miss).
    bool cur_phase = current_phase();

    // Wait for an even version before starting the scan.  An odd version
    // means a writer currently holds the bucket lock — waiting here avoids
    // wasting a full retry attempt on a guaranteed-inconsistent read.
    const uint32_t version_before =
        wait.even_version([&] { return load_version(bucket_idx); });
    if ((version_before & 1) != 0) {
      continue;  // Writer still active: retry (wait.retry() paces it)
    }

    // Memory barrier to ensure we read entries after version
    std::atomic_thread_fence(std::memory_order_acquire);
    CYCLONE_TSAN_ACQUIRE(&_versions[bucket_idx]);

    const DirEntry *bucket = &_entries[bucket_idx * kEntriesPerBucket];
    bool should_stop = false;

    for (size_t i = 0; i < kEntriesPerBucket && !should_stop; ++i) {
      DirEntry entry = bucket[i];  // Copy the entry

      if (entry.is_empty()) {
        continue;
      }
      // Skip stale entries from a previous GC phase
      if (kFilterPhase && entry.phase() != cur_phase) {
        continue;
      }
      if (entry.tag() == target_tag) {
        // Memory barrier before reading version again
        std::atomic_thread_fence(std::memory_order_acquire);

        uint32_t version_after = load_version(bucket_idx);
        if (version_before != version_after) {
          // Version changed during read - retry
          break;
        }

        if (!callback(entry)) {
          should_stop = true;
        }
      }
    }

    if (should_stop) {
      return true;
    }

    // Check version one more time
    std::atomic_thread_fence(std::memory_order_acquire);
    uint32_t version_after = load_version(bucket_idx);
    if (version_before == version_after) {
      return true;  // Consistent read achieved
    }
    // Version changed - retry
  } while (wait.retry());
  return false;  // A writer held the bucket past the budget: unknown
}

}  // namespace cyclone
