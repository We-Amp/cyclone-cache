// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <thread>
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
// Supports up to 512TB per stripe with 40-bit offset addressing.
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
  // Seqlock read parameters (same values as MmapDirectory).
  static constexpr size_t kMaxReadRetries = 100;
  /// Maximum spins waiting for a writer to release the seqlock (even version).
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

  [[nodiscard]] std::optional<DirEntry> probe(const CacheKey &key) const;

  // Get all entries matching this key's tag in the bucket (for collision
  // handling)
  [[nodiscard]] std::vector<DirEntry> probe_all(const CacheKey &key) const;

  // Iterate over all matching entries without allocation (returns false to
  // stop).  Seqlock read: each matching entry is validated against the
  // bucket version before the callback runs, so the callback only ever sees
  // an entry that was consistent at validation time.  As in
  // MmapDirectory::probe_each, a version change after a callback already ran
  // retries the scan — callbacks must tolerate being invoked again for the
  // same entry (the Volume read paths do: candidate probing is idempotent).
  template <typename Callback>
  void probe_each(const CacheKey &key, Callback &&callback) const {
    uint32_t bucket_idx = key.bucket_hash() % _num_buckets;
    uint16_t target_tag = key.tag();

    for (size_t retry = 0; retry < kMaxReadRetries; ++retry) {
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
        if (entry.phase() != cur_phase) {
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
        return;
      }

      // Check version one more time
      std::atomic_thread_fence(std::memory_order_acquire);
      uint32_t version_after = load_version(bucket_idx);
      if (version_before == version_after) {
        return;  // Consistent read achieved
      }
      // Version changed - retry
    }
  }

  // Insert or update an entry.  verified_offset selects which same-tag
  // entry (if any) may be updated in place — see the sentinels above.  When
  // the bucket is full and a colliding foreign entry must be evicted to
  // land the write, *collision_evicted is set to true.  When the bucket is
  // full and nothing collides, the entry nearest the wrap cursor is evicted
  // instead and *bucket_full_evicted is set to true.
  bool insert(const CacheKey &key, uint64_t offset, uint64_t size,
              uint64_t verified_offset = kMatchAnyTag,
              bool *collision_evicted = nullptr,
              bool *bucket_full_evicted = nullptr);

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

  // seq_cst: the phase is one half of the reader's stamp-then-revalidate
  // wrap epoch (see Volume::wrap_epoch) — mirror MmapDirectory.
  [[nodiscard]] bool current_phase() const {
    return _current_phase.load(std::memory_order_seq_cst);
  }
  void toggle_phase();

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
