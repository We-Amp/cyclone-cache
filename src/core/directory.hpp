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
#include <utility>
#include <vector>

#include "cyclone/key.hpp"

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

// Compact 10-byte directory entry matching ATS layout for proven efficiency.
// The 40-bit offset is a BYTE offset into the stripe (see
// `stripe->offset + dir_entry.offset()` in volume.cpp), so a stripe can
// address up to 1 TiB.
struct alignas(2) DirEntry {
  static constexpr size_t kSize = 10;

  uint16_t _w[5] = {0, 0, 0, 0, 0};

  [[nodiscard]] uint64_t offset() const;
  void set_offset(uint64_t offset);

  [[nodiscard]] uint8_t big() const;
  void set_big(uint8_t big);

  [[nodiscard]] uint8_t size() const;
  void set_size(uint8_t size);

  [[nodiscard]] uint16_t tag() const;
  void set_tag(uint16_t tag);

  [[nodiscard]] bool phase() const;
  void set_phase(bool phase);

  [[nodiscard]] bool head() const;
  void set_head(bool head);

  [[nodiscard]] bool pinned() const;
  void set_pinned(bool pinned);

  [[nodiscard]] uint16_t next() const;
  void set_next(uint16_t next);

  [[nodiscard]] bool is_empty() const;
  void clear();

  [[nodiscard]] uint64_t approx_size() const;
  void set_approx_size(uint64_t bytes);
};

static_assert(sizeof(DirEntry) == DirEntry::kSize,
              "DirEntry must be exactly 10 bytes");

// Outcome of the position leg of admission (see Stripe::admit_position in
// volume.hpp).  kCurrent = an entry of the pass the snapshot is in, sitting
// behind the write cursor.  kRetained (wrap retention only) = an entry of
// the PREVIOUS pass whose bytes lie at or beyond the clean frontier, i.e.
// not yet handed to the forward fill.
enum class AdmitClass : uint8_t { kReject, kCurrent, kRetained };

// How insert() tells admissible entries from dead ones, supplied by the
// stripe that owns the directory (doc/design/wrap-retention.md section
// 4.7).  refresh() is called INSIDE the insert's seqlock bracket (and, for
// the mmap directory, under phase_lock): it reloads the stripe's exposure
// generation and write cursor there, because in multi-process mode an
// independently opened writer can wrap or advance between the caller's
// election and the insert.
class InsertAdmission {
 public:
  virtual ~InsertAdmission() = default;
  virtual void refresh() = 0;
  [[nodiscard]] virtual AdmitClass classify(uint64_t relative_offset,
                                            bool phase) const = 0;
};

// Victim choice for an insert under an InsertAdmission (section 4.7):
//   1. the verified same-key entry, whatever its class (in place);
//   2. an empty slot;
//   3. an inadmissible slot (runway, previous pass behind the frontier,
//      current phase at or ahead of the cursor);
//   4. a tag collider (reported via *collision_evicted);
//   5. the oldest admissible entry: the retained one nearest the frontier
//      (lowest offset) if any, else the lowest-offset current one
//      (reported via *bucket_full_evicted).
// Returns the slot index and whether the slot held an entry (no count
// change) -- or -1 when the bucket is empty of candidates (impossible for a
// 4-slot bucket).  Shared by Directory and MmapDirectory.
struct InsertChoice {
  int slot = -1;
  bool replaces = false;  // slot held an entry: the count does not change
  bool verified = false;  // slot is the verified same-key entry
};
//
// Before 2, an entry with this tag already AT `new_offset` (the offset the
// caller just wrote) is taken over: its bytes are the ones the new document
// replaced, so it is dead, and leaving it beside the new entry would give
// the bucket two same-tag entries at one offset -- which a precise
// remove_at(tag, offset) cannot tell apart.  0 = no such check.
[[nodiscard]] InsertChoice choose_insert_slot(
    const DirEntry *bucket, size_t slots, uint16_t tag,
    uint64_t verified_offset, uint64_t match_any_tag,
    const InsertAdmission &adm, bool *collision_evicted,
    bool *bucket_full_evicted, uint64_t new_offset = 0);

// How long a lock-free directory reader waits out a writer (issue #21).
//
// A seqlock reader retries while its bucket is odd (a writer is mid-update)
// or its scan was torn (the version moved under it).  A fixed retry count
// alone is not enough: a writer descheduled INSIDE its odd window holds the
// bucket for a scheduling quantum, every retry lands in that window, and a
// reader that then gave up would report a key that is present as absent.
//
// So the wait has two phases:
//   1. kFastAttempts retries exactly as before -- no clock read, so an
//      uncontended or briefly contended probe costs what it always did;
//   2. past them the writer is presumed descheduled: keep retrying (each
//      odd-bucket attempt still spins, then yields its CPU, which is what
//      lets the writer run) until kBudget of steady-clock time has passed.
// Only when the budget is spent does the probe give up, and it reports that
// as "unknown" (probe_each returns false, which the Volume turns into
// CacheError::Busy) -- never as a miss.
//
// kBudget covers one full scheduler quantum with room to spare: 10 ms on
// macOS, and on Linux a few EEVDF slices (3 ms base) of queueing behind CPU
// hogs.  A writer is never legitimately odd for longer than a few hundred
// nanoseconds of its own work, so the budget is only ever spent waiting for
// a writer to be scheduled again -- or, in multi-process mode, on a bucket
// left odd by a process that died mid-update, until the next write to that
// bucket force-recovers it (MmapDirectory::acquire_writer).
class SeqlockReadWait {
 public:
  static constexpr size_t kFastAttempts = 100;
  static constexpr std::chrono::milliseconds kBudget{20};

  // Call after a failed attempt.  True: try again.  False: the budget is
  // spent and the probe must report Busy.
  bool retry() {
    if (_attempts < kFastAttempts) {
      ++_attempts;
      return true;
    }
    const auto now = std::chrono::steady_clock::now();
    if (_attempts == kFastAttempts) {
      // Entering the timed phase: the old code gave up here.
      ++_attempts;
      _deadline = now + budget();
#ifdef CYCLONE_TEST_SEAMS
      s_timed_waits_for_test.fetch_add(1, std::memory_order_relaxed);
#endif
      return true;
    }
    return now < _deadline;
  }

#ifdef CYCLONE_TEST_SEAMS
  // TEST-SEAM BUILDS ONLY (see Volume::ReaderSeam).  Nonzero overrides
  // kBudget, in microseconds, so a test can spend the budget quickly or make
  // it effectively unbounded.  The counter records each probe that entered
  // the timed phase, i.e. got past the point the old fixed retry count gave
  // up at.
  static inline std::atomic<uint64_t> s_budget_us_for_test{0};
  static inline std::atomic<uint64_t> s_timed_waits_for_test{0};
#endif

 private:
  static std::chrono::steady_clock::duration budget() {
#ifdef CYCLONE_TEST_SEAMS
    const uint64_t us = s_budget_us_for_test.load(std::memory_order_relaxed);
    if (us != 0) {
      return std::chrono::microseconds(us);
    }
#endif
    return kBudget;
  }

  size_t _attempts = 0;
  std::chrono::steady_clock::time_point _deadline{};
};

// In-memory (single-process) directory.
//
// Reader synchronization is a per-bucket SEQLOCK, mirroring MmapDirectory:
// readers never write shared state (a shared_mutex reader count is an atomic
// RMW on one shared cache line, which bounces across cores and serializes
// concurrent readers), they only LOAD the bucket's version counter and retry
// on a torn read.  Odd version = writer active.
//
// Writer mutual exclusion is NOT provided here: every mutator (insert /
// remove / remove_at / toggle_phase) is already serialized externally by the
// owning stripe's exclusive mutex (see Volume's commit/remove paths), so
// begin/end_bucket_write only publish the odd/even version transitions for
// readers — they do not lock against other writers.  clear() and
// deserialize() additionally require full quiescence (startup/reset only),
// as before.
class Directory {
 public:
  static constexpr size_t kEntriesPerBucket = 4;
  static constexpr size_t kMaxChainDepth =
      64;  // Limit chain traversal to prevent infinite loops
  // Seqlock read parameters (same values as MmapDirectory).  How many
  // attempts a reader makes, and for how long, is SeqlockReadWait's call.
  /// Maximum spins per attempt waiting for a writer to release the seqlock
  /// (even version) before the attempt yields.
  static constexpr size_t kMaxWriterWaitSpins = 1000;

  // Sentinels for insert()'s verified_offset parameter.  A DirEntry holds no
  // key material — only a 12-bit tag — so a same-tag entry in the bucket may
  // belong to a DIFFERENT key colliding on (bucket, tag).  Callers that can
  // read the stored documents (Volume) verify the full key first and pass
  // the verified entry's offset; only that entry may be updated in place.
  // Real offsets are 40-bit, so the top of the uint64_t range is free.
  //
  // kMatchAnyTag: legacy trust-the-tag behavior — any same-tag current-phase
  //   entry is updated in place.  Collision-destructive; only safe when the
  //   caller cannot collide (single-key tests).
  // kNoVerifiedEntry: no existing entry holds the inserted key — every
  //   same-tag entry is a colliding foreign key and must be preserved (or,
  //   when the bucket is completely full, evicted and reported via
  //   collision_evicted).
  static constexpr uint64_t kMatchAnyTag = UINT64_MAX;
  static constexpr uint64_t kNoVerifiedEntry = UINT64_MAX - 1;

  explicit Directory(size_t num_buckets);

  void clear();

  // First current-phase entry with this key's tag.  Test-only (no
  // production callers): a bucket a writer held past the wait budget reads
  // as nullopt here, where probe_each reports it.
  [[nodiscard]] std::optional<DirEntry> probe(const CacheKey &key) const;

  // Get all entries matching this key's tag in the bucket (for collision
  // handling).  Test-only, like probe().
  [[nodiscard]] std::vector<DirEntry> probe_all(const CacheKey &key) const;

  // Iterate over all matching entries without allocation (returns false to
  // stop).  Seqlock read: each matching entry is validated against the
  // bucket version before the callback runs, so the callback only ever sees
  // an entry that was consistent at validation time.  As in
  // MmapDirectory::probe_each, a version change after a callback already ran
  // retries the scan — callbacks must tolerate being invoked again for the
  // same entry (the Volume read paths do: candidate probing is idempotent).
  //
  // Returns true when the scan completed against a consistent bucket (or the
  // callback stopped it), false when a writer held the bucket for the whole
  // SeqlockReadWait budget: the bucket's contents are then UNKNOWN, and a
  // caller that found nothing must not report a miss (issue #21).
  template <typename Callback>
  bool probe_each(const CacheKey &key, Callback &&callback) const {
    return probe_each_impl<true>(key, std::forward<Callback>(callback));
  }

  // As probe_each, but yields tag matches of BOTH phases: the caller
  // classifies each entry against its own stripe snapshot (see
  // Stripe::probe_each / admit_position in volume.hpp).  A reader must never
  // mix the directory's own phase load into that decision -- the phase is
  // derived from the pass count in the snapshot instead.
  template <typename Callback>
  bool probe_each_all_phases(const CacheKey &key, Callback &&callback) const {
    return probe_each_impl<false>(key, std::forward<Callback>(callback));
  }

#ifdef CYCLONE_TEST_SEAMS
  // TEST-SEAM BUILDS ONLY.  Publish / clear "writer active" on this key's
  // bucket, so a test can park a writer inside its odd window.  The caller
  // provides the writer serialization (holds the stripe mutex), exactly as
  // every production mutator does.
  void begin_bucket_write_for_test(const CacheKey &key) {
    begin_bucket_write(key.bucket_hash() % _num_buckets);
  }
  void end_bucket_write_for_test(const CacheKey &key) {
    end_bucket_write(key.bucket_hash() % _num_buckets);
  }
#endif

 private:
  template <bool kFilterPhase, typename Callback>
  bool probe_each_impl(const CacheKey &key, Callback &&callback) const {
    uint32_t bucket_idx = key.bucket_hash() % _num_buckets;
    uint16_t target_tag = key.tag();

    // Every failed attempt, including each `continue` below, goes through
    // wait.retry() in the loop condition.
    SeqlockReadWait wait;
    do {
      // Capture the phase INSIDE the retry loop: a retry triggered by a
      // concurrent toggle_phase() must rescan with the new phase, or
      // entries stamped after the flip would be invisibly skipped
      // (spurious miss).
      bool cur_phase = current_phase();

      // Wait for an even version before starting the scan.  An odd version
      // means a writer is currently mutating the bucket — spinning here
      // avoids wasting a full retry attempt on a guaranteed-inconsistent
      // read.
      uint32_t version_before = load_version(bucket_idx);
      if ((version_before & 1) != 0) {
        for (size_t spin = 0; spin < kMaxWriterWaitSpins; ++spin) {
          cpu_pause();
          version_before = load_version(bucket_idx);
          if ((version_before & 1) == 0) break;
        }
        if ((version_before & 1) != 0) {
          // Writer still active after spin limit — yield and retry
          std::this_thread::yield();
          continue;
        }
      }

      // Memory barrier to ensure we read entries after version
      std::atomic_thread_fence(std::memory_order_acquire);
      CYCLONE_TSAN_ACQUIRE(const_cast<uint32_t *>(&_versions[bucket_idx]));

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

 public:
  // Insert or update an entry.  verified_offset selects which same-tag
  // entry (if any) may be updated in place — see the sentinels above.  When
  // the bucket is full and a colliding foreign entry must be evicted to
  // land the write, *collision_evicted is set to true.  When the bucket is
  // full and nothing collides, the entry nearest the wrap cursor is evicted
  // instead and *bucket_full_evicted is set to true.
  //
  // With `admission` (the Volume always passes one): the victim order of
  // choose_insert_slot, the verified entry is updated in place whatever its
  // phase, and every OTHER entry with this key's tag at an offset listed in
  // `clear_offsets` is cleared in the same seqlock bracket (the uniqueness
  // rule: one resolvable entry per key; the caller verified those offsets
  // hold a stale or duplicate copy of this key).  Without it: the legacy
  // current-phase-only behaviour described above.
  bool insert(const CacheKey &key, uint64_t offset, uint64_t size,
              uint64_t verified_offset = kMatchAnyTag,
              bool *collision_evicted = nullptr,
              bool *bucket_full_evicted = nullptr,
              InsertAdmission *admission = nullptr,
              std::span<const uint64_t> clear_offsets = {});

  // WARNING: matches on the 12-bit tag only (a DirEntry holds no key
  // material), so a colliding foreign key's entry can be removed.
  // Collision-safe removal must verify the stored first_key and use
  // remove_at() — see Volume::remove_sync.  No production call sites.
  bool remove(const CacheKey &key);

  // Remove entry matching both tag and offset (precise removal for collision
  // safety)
  bool remove_at(const CacheKey &key, uint64_t target_offset);

  [[nodiscard]] size_t count() const {
    return _count.load(std::memory_order_relaxed);
  }
  [[nodiscard]] size_t capacity() const { return _entries.size(); }
  [[nodiscard]] size_t bucket_count() const { return _num_buckets; }

  // The phase new entries are stamped with.  Readers never use it: they
  // derive the phase from the pass in their stripe snapshot (see
  // Volume::snapshot); only insert() and the legacy probe paths load it.
  [[nodiscard]] bool current_phase() const {
    return _current_phase.load(std::memory_order_seq_cst);
  }
  void toggle_phase();
  // Store the phase outright (seq_cst).  The wrap derives it from the pass
  // (phase = pass & 1) instead of toggling; mutators are externally
  // serialized (class comment), like toggle_phase.
  void set_current_phase(bool phase) {
    _current_phase.store(phase, std::memory_order_seq_cst);
  }

  [[nodiscard]] std::span<const std::byte> serialize() const;
  void deserialize(std::span<const std::byte> data);

 private:
  [[nodiscard]] uint32_t load_version(size_t bucket_idx) const {
    return std::atomic_ref<uint32_t>(
               const_cast<uint32_t &>(_versions[bucket_idx]))
        .load(std::memory_order_acquire);
  }

  // Publish "writer active" (even → odd).  Caller must hold the external
  // writer serialization (stripe exclusive mutex); see the class comment.
  void begin_bucket_write(size_t bucket_idx);
  // Publish "writer done" (odd → even) after all entry mutations.
  void end_bucket_write(size_t bucket_idx);

  static void cpu_pause() {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_pause();
#elif defined(__x86_64__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(_M_ARM64)
    __asm__ volatile("yield" ::: "memory");
#endif
  }

  size_t _num_buckets;
  std::vector<DirEntry> _entries;
  // One seqlock version per bucket, accessed via std::atomic_ref (odd =
  // writer active).  Sized in the constructor and never resized.
  std::vector<uint32_t> _versions;
  std::atomic<size_t> _count{0};
  std::atomic<bool> _current_phase{false};
};

}  // namespace cyclone
