// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "directory.hpp"

#include <cstring>

namespace cyclone {

// DirEntry bit layout (10 bytes = 5 x 16-bit words):
// w[0]: offset bits 0-15
// w[1]: offset bits 16-23 (8 bits), big (2 bits), size (6 bits)
// w[2]: tag (12 bits), phase (1 bit), head (1 bit), pinned (1 bit), reserved (1
// bit) w[3]: next pointer (16 bits) - bucket chain w[4]: offset bits 24-39 (16
// bits for 40-bit total = 512TB max)

uint64_t DirEntry::offset() const {
  uint64_t result = _w[0];
  result |= static_cast<uint64_t>(_w[1] & 0xFF) << 16;
  result |= static_cast<uint64_t>(_w[4]) << 24;
  return result;
}

void DirEntry::set_offset(uint64_t offset) {
  _w[0] = static_cast<uint16_t>(offset & 0xFFFF);
  _w[1] = static_cast<uint16_t>((_w[1] & 0xFF00) | ((offset >> 16) & 0xFF));
  _w[4] = static_cast<uint16_t>((offset >> 24) & 0xFFFF);
}

uint8_t DirEntry::big() const {
  return static_cast<uint8_t>((_w[1] >> 8) & 0x03);
}

void DirEntry::set_big(uint8_t big) {
  _w[1] = static_cast<uint16_t>((_w[1] & ~0x0300) | ((big & 0x03) << 8));
}

uint8_t DirEntry::size() const {
  return static_cast<uint8_t>((_w[1] >> 10) & 0x3F);
}

void DirEntry::set_size(uint8_t size) {
  _w[1] = static_cast<uint16_t>((_w[1] & 0x03FF) | ((size & 0x3F) << 10));
}

uint16_t DirEntry::tag() const { return _w[2] & 0x0FFF; }

void DirEntry::set_tag(uint16_t tag) {
  _w[2] = static_cast<uint16_t>((_w[2] & 0xF000) | (tag & 0x0FFF));
}

bool DirEntry::phase() const { return ((_w[2] >> 12) & 0x01) != 0; }

void DirEntry::set_phase(bool phase) {
  _w[2] = static_cast<uint16_t>((_w[2] & ~0x1000) | (phase ? 0x1000 : 0));
}

bool DirEntry::head() const { return ((_w[2] >> 13) & 0x01) != 0; }

void DirEntry::set_head(bool head) {
  _w[2] = static_cast<uint16_t>((_w[2] & ~0x2000) | (head ? 0x2000 : 0));
}

bool DirEntry::pinned() const { return ((_w[2] >> 14) & 0x01) != 0; }

void DirEntry::set_pinned(bool pinned) {
  _w[2] = static_cast<uint16_t>((_w[2] & ~0x4000) | (pinned ? 0x4000 : 0));
}

uint16_t DirEntry::next() const { return _w[3]; }

void DirEntry::set_next(uint16_t next) { _w[3] = next; }

bool DirEntry::is_empty() const { return offset() == 0; }

void DirEntry::clear() { std::memset(_w, 0, sizeof(_w)); }

uint64_t DirEntry::approx_size() const {
  // Size encoding: base size * (1 << big)
  static constexpr uint64_t kBlockSize = 512;
  uint64_t base = (static_cast<uint64_t>(size()) + 1) * kBlockSize;
  return base << big();
}

void DirEntry::set_approx_size(uint64_t bytes) {
  static constexpr uint64_t kBlockSize = 512;

  uint8_t b = 0;
  while (bytes > (63 + 1) * kBlockSize * (1ULL << b) && b < 3) {
    ++b;
  }

  // Round UP to ensure we have enough space for the full document.
  // Integer division truncates, so we use (n + d - 1) / d to round up.
  uint64_t block_size = kBlockSize << b;
  uint64_t blocks = (bytes + block_size - 1) / block_size;
  if (blocks == 0) {
    blocks = 1;
  }
  if (blocks > 64) {
    blocks = 64;
  }

  set_big(b);
  set_size(static_cast<uint8_t>(blocks - 1));
}

// Directory implementation

Directory::Directory(size_t num_buckets)
    : _num_buckets(num_buckets),
      _entries(num_buckets * kEntriesPerBucket),
      _versions(num_buckets, 0) {
  clear();
}

// Requires full quiescence (startup/reset only) — no reader or writer may
// be concurrent.  Versions are left at their current (even) values.
void Directory::clear() {
  for (auto &entry : _entries) {
    entry.clear();
  }
  _count.store(0, std::memory_order_relaxed);
}

void Directory::begin_bucket_write(size_t bucket_idx) {
  // even → odd.  The fence AFTER the increment is load-bearing on weak
  // memory (AArch64): acquire/release on the RMW alone does not order the
  // odd-version STORE before the subsequent plain entry stores, so a
  // reader could observe {even, half-mutated entry, even}.  Canonical
  // seqlocks (Linux write_seqcount_begin) place a barrier here for the
  // same reason.
  std::atomic_ref<uint32_t>(_versions[bucket_idx])
      .fetch_add(1, std::memory_order_acq_rel);
  std::atomic_thread_fence(std::memory_order_seq_cst);
  CYCLONE_TSAN_RELEASE(&_versions[bucket_idx]);
}

void Directory::end_bucket_write(size_t bucket_idx) {
  // Ensure all entry writes are visible before making the version even.
  std::atomic_thread_fence(std::memory_order_release);
  CYCLONE_TSAN_RELEASE(&_versions[bucket_idx]);
  std::atomic_ref<uint32_t>(_versions[bucket_idx])
      .fetch_add(1, std::memory_order_release);
}

std::optional<DirEntry> Directory::probe(const CacheKey &key) const {
  uint32_t bucket_idx = key.bucket_hash() % _num_buckets;
  uint16_t target_tag = key.tag();

  // Same bounded wait as probe_each(); `continue` lands on wait.retry().
  SeqlockReadWait wait;
  do {
    // Phase captured inside the retry loop — same rationale as
    // probe_each().
    bool cur_phase = current_phase();

    uint32_t version_before = load_version(bucket_idx);
    if ((version_before & 1) != 0) {
      // Writer active — spin for the even version like probe_each() does
      // (a preempted writer can hold the odd version for a scheduling
      // quantum; burning one bare retry per pause exhausts the retry
      // budget in microseconds and returns a spurious miss).
      for (size_t spin = 0; spin < kMaxWriterWaitSpins; ++spin) {
        cpu_pause();
        version_before = load_version(bucket_idx);
        if ((version_before & 1) == 0) break;
      }
      if ((version_before & 1) != 0) {
        std::this_thread::yield();
        continue;
      }
    }
    std::atomic_thread_fence(std::memory_order_acquire);
    CYCLONE_TSAN_ACQUIRE(const_cast<uint32_t *>(&_versions[bucket_idx]));

    std::optional<DirEntry> result;
    const DirEntry *bucket = &_entries[bucket_idx * kEntriesPerBucket];
    for (size_t i = 0; i < kEntriesPerBucket && !result; ++i) {
      DirEntry entry = bucket[i];  // Copy the entry
      if (entry.is_empty()) {
        continue;
      }
      // Skip stale entries from a previous GC phase
      if (entry.phase() != cur_phase) {
        continue;
      }
      if (entry.tag() == target_tag) {
        result = entry;
        break;
      }
      // No chain walk: insert() never links chains (the ATS-style `next`
      // field is unused), and chain nodes would live in OTHER buckets
      // guarded by OTHER seqlock versions — this bucket's version recheck
      // below could not validate them.
    }

    std::atomic_thread_fence(std::memory_order_acquire);
    if (load_version(bucket_idx) == version_before) {
      return result;  // Consistent read achieved
    }
    // Version changed — retry
  } while (wait.retry());
  return std::nullopt;  // Budget spent: unknown, reported as absent here
}

std::vector<DirEntry> Directory::probe_all(const CacheKey &key) const {
  std::vector<DirEntry> results;
  (void)probe_each(key, [&](const DirEntry &entry) {
    results.push_back(entry);
    return true;  // Collect all matches
  });
  return results;
}

InsertChoice choose_insert_slot(const DirEntry *bucket, size_t slots,
                                uint16_t tag, uint64_t verified_offset,
                                uint64_t match_any_tag,
                                const InsertAdmission &adm,
                                bool *collision_evicted,
                                bool *bucket_full_evicted,
                                uint64_t new_offset) {
  int same_spot = -1;
  int first_empty = -1;
  int first_inadmissible = -1;
  int first_collider = -1;
  int oldest_retained = -1;
  int oldest_current = -1;
  for (size_t i = 0; i < slots; ++i) {
    const DirEntry &e = bucket[i];
    if (e.is_empty()) {
      if (first_empty < 0) {
        first_empty = static_cast<int>(i);
      }
      continue;
    }
    const AdmitClass cls = adm.classify(e.offset(), e.phase());
    // 1. The verified same-key entry, whatever its class (rule 1: a rewrite
    //    of K updates K's admitted entry, current or retained, in place).
    if (e.tag() == tag &&
        (verified_offset == match_any_tag ? cls != AdmitClass::kReject
                                          : e.offset() == verified_offset)) {
      return {static_cast<int>(i), true, true};
    }
    if (new_offset != 0 && e.tag() == tag && e.offset() == new_offset) {
      if (same_spot < 0) {
        same_spot = static_cast<int>(i);
      }
      continue;
    }
    if (cls == AdmitClass::kReject) {
      if (first_inadmissible < 0) {
        first_inadmissible = static_cast<int>(i);
      }
      continue;
    }
    if (e.tag() == tag) {
      if (first_collider < 0) {
        first_collider = static_cast<int>(i);
      }
      continue;
    }
    int &oldest =
        cls == AdmitClass::kRetained ? oldest_retained : oldest_current;
    if (oldest < 0 || e.offset() < bucket[oldest].offset()) {
      oldest = static_cast<int>(i);
    }
  }
  if (same_spot >= 0) {
    return {same_spot, true, false};
  }
  if (first_empty >= 0) {
    return {first_empty, false, false};
  }
  if (first_inadmissible >= 0) {
    return {first_inadmissible, true, false};
  }
  if (first_collider >= 0) {
    if (collision_evicted != nullptr) {
      *collision_evicted = true;
    }
    return {first_collider, true, false};
  }
  const int oldest = oldest_retained >= 0 ? oldest_retained : oldest_current;
  if (oldest >= 0 && bucket_full_evicted != nullptr) {
    *bucket_full_evicted = true;
  }
  return {oldest, oldest >= 0, false};
}

bool Directory::insert(const CacheKey &key, uint64_t offset, uint64_t size,
                       uint64_t verified_offset, bool *collision_evicted,
                       bool *bucket_full_evicted, InsertAdmission *admission,
                       std::span<const uint64_t> clear_offsets) {
  uint32_t bucket_idx = key.bucket_hash() % _num_buckets;
  uint16_t tag = key.tag();
  bool cur_phase = current_phase();

  DirEntry *bucket = &_entries[bucket_idx * kEntriesPerBucket];

  if (admission != nullptr) {
    // Writers are externally serialized (class comment), so the refresh
    // and the choice need no bracket of their own; the mutation below is
    // published through one odd->even bracket.
    admission->refresh();
    const InsertChoice choice = choose_insert_slot(
        bucket, kEntriesPerBucket, tag, verified_offset, kMatchAnyTag,
        *admission, collision_evicted, bucket_full_evicted, offset);
    if (choice.slot < 0) {
      return false;
    }
    size_t cleared = 0;
    begin_bucket_write(bucket_idx);
    if (choice.verified) {
      bucket[choice.slot].set_offset(offset);
      bucket[choice.slot].set_approx_size(size);
      bucket[choice.slot].set_phase(cur_phase);
    } else {
      DirEntry new_entry;
      new_entry.set_offset(offset);
      new_entry.set_approx_size(size);
      new_entry.set_tag(tag);
      new_entry.set_phase(cur_phase);
      new_entry.set_head(true);
      bucket[choice.slot] = new_entry;
    }
    // Uniqueness cleanup (rule 2), in the same bracket as the insert.
    // A same-tag entry already at the offset just written is dead too:
    // its bytes are the ones the new document replaced.  The chooser
    // takes such an entry over when nothing better wins, but when the
    // verified entry wins it would survive beside the new one (review
    // N1), so it is cleared here in the same bracket.
    for (size_t i = 0; i < kEntriesPerBucket; ++i) {
      if (static_cast<int>(i) == choice.slot || bucket[i].is_empty() ||
          bucket[i].tag() != tag) {
        continue;
      }
      if (bucket[i].offset() == offset) {
        bucket[i].clear();
        ++cleared;
        continue;
      }
      for (uint64_t off : clear_offsets) {
        if (bucket[i].offset() == off) {
          bucket[i].clear();
          ++cleared;
          break;
        }
      }
    }
    end_bucket_write(bucket_idx);
    if (!choice.replaces) {
      _count.fetch_add(1, std::memory_order_relaxed);
    }
    if (cleared != 0) {
      _count.fetch_sub(cleared, std::memory_order_relaxed);
    }
    return true;
  }

  // Single-pass scan: find the verified same-tag entry, first empty slot,
  // first stale slot, first colliding foreign entry, or the entry nearest the
  // wrap cursor.  Priority: verified match > empty > stale > collider eviction
  // > nearest-to-clobber eviction.  The scan itself needs no seqlock
  // transition (writers are externally serialized; see the class comment) —
  // only the mutation below does.
  int first_empty = -1;
  int first_stale = -1;
  int first_collider = -1;
  int target = -1;
  // Last-resort victim: the occupied slot with the lowest offset.  Its data
  // sits nearest the wrap cursor, so it is the next to be clobbered anyway.
  int nearest_to_clobber = -1;
  uint64_t nearest_offset = 0;

  for (size_t i = 0; i < kEntriesPerBucket; ++i) {
    if (bucket[i].is_empty()) {
      if (first_empty < 0) {
        first_empty = static_cast<int>(i);
      }
      continue;
    }
    // Occupied: offset() is stripe-relative, which preserves the ordering of
    // the absolute write cursor.  Empties are already skipped above (an empty
    // entry reads offset() == 0 and would always win).
    if (nearest_to_clobber < 0 || bucket[i].offset() < nearest_offset) {
      nearest_to_clobber = static_cast<int>(i);
      nearest_offset = bucket[i].offset();
    }
    // Check for current-phase tag match.  A tag match alone may be a
    // DIFFERENT key colliding on (bucket, tag) — update in place only when
    // the caller verified the stored key (entry offset == verified_offset)
    // or explicitly opted into the legacy trust-the-tag behavior.
    if (bucket[i].tag() == tag && bucket[i].phase() == cur_phase) {
      if (verified_offset == kMatchAnyTag ||
          bucket[i].offset() == verified_offset) {
        target = static_cast<int>(i);
        break;
      }
      // Colliding foreign key — preserve it; last-resort eviction candidate.
      if (first_collider < 0) {
        first_collider = static_cast<int>(i);
      }
      continue;
    }
    // Track first stale entry (wrong phase) for potential reuse
    if (bucket[i].phase() != cur_phase && first_stale < 0) {
      first_stale = static_cast<int>(i);
    }
  }

  // In-place update of the verified (or legacy trust-the-tag) entry
  if (target >= 0) {
    begin_bucket_write(bucket_idx);
    bucket[target].set_offset(offset);
    bucket[target].set_approx_size(size);
    bucket[target].set_phase(cur_phase);
    end_bucket_write(bucket_idx);
    return true;
  }

  // Prefer empty slot over stale slot
  if (first_empty >= 0) {
    begin_bucket_write(bucket_idx);
    bucket[first_empty].set_offset(offset);
    bucket[first_empty].set_approx_size(size);
    bucket[first_empty].set_tag(tag);
    bucket[first_empty].set_phase(cur_phase);
    bucket[first_empty].set_head(true);
    end_bucket_write(bucket_idx);
    _count.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  // Reuse stale slot (don't increment count — replacing an existing entry)
  if (first_stale >= 0) {
    begin_bucket_write(bucket_idx);
    bucket[first_stale].set_offset(offset);
    bucket[first_stale].set_approx_size(size);
    bucket[first_stale].set_tag(tag);
    bucket[first_stale].set_phase(cur_phase);
    bucket[first_stale].set_head(true);
    end_bucket_write(bucket_idx);
    return true;
  }

  // Bucket full of current-phase entries and one collides on the tag —
  // evict the collider so the write still lands (matches the pre-fix
  // outcome for the victim, but now detected and reported, never silent).
  if (first_collider >= 0) {
    begin_bucket_write(bucket_idx);
    bucket[first_collider].set_offset(offset);
    bucket[first_collider].set_approx_size(size);
    bucket[first_collider].set_tag(tag);
    bucket[first_collider].set_phase(cur_phase);
    bucket[first_collider].set_head(true);
    end_bucket_write(bucket_idx);
    if (collision_evicted != nullptr) {
      *collision_evicted = true;
    }
    return true;
  }

  // Bucket full of current-phase entries and none collides on the tag — evict
  // the entry nearest the wrap cursor so the write still lands instead of
  // failing outright.  A DirEntry carries no recency data, but
  // write_pos advances monotonically within a phase, so the lowest offset in
  // this stripe's bucket is the entry closest to being overwritten anyway.
  // NOT strict eviction by age: a phase-ABA survivor in the trailing gap reads
  // current-phase with a HIGH offset.  Correctness never depends on the choice.
  if (nearest_to_clobber >= 0) {
    // Build a complete entry locally, then assign — a fresh DirEntry zeroes
    // next/pinned, so the new key cannot inherit the victim's chain pointer.
    // Matches the MmapDirectory twin, which memcpy's a fresh entry.
    DirEntry new_entry;
    new_entry.set_offset(offset);
    new_entry.set_approx_size(size);
    new_entry.set_tag(tag);
    new_entry.set_phase(cur_phase);
    new_entry.set_head(true);
    begin_bucket_write(bucket_idx);
    bucket[nearest_to_clobber] = new_entry;
    end_bucket_write(bucket_idx);
    // No count change — replacing an existing entry.
    if (bucket_full_evicted != nullptr) {
      *bucket_full_evicted = true;
    }
    return true;
  }

  return false;
}

bool Directory::remove(const CacheKey &key) {
  uint32_t bucket_idx = key.bucket_hash() % _num_buckets;
  uint16_t target_tag = key.tag();

  DirEntry *bucket = &_entries[bucket_idx * kEntriesPerBucket];

  for (size_t i = 0; i < kEntriesPerBucket; ++i) {
    if (!bucket[i].is_empty() && bucket[i].tag() == target_tag) {
      begin_bucket_write(bucket_idx);
      bucket[i].clear();
      end_bucket_write(bucket_idx);
      _count.fetch_sub(1, std::memory_order_relaxed);
      return true;
    }
  }

  return false;
}

bool Directory::remove_at(const CacheKey &key, uint64_t target_offset) {
  uint32_t bucket_idx = key.bucket_hash() % _num_buckets;
  uint16_t target_tag = key.tag();

  DirEntry *bucket = &_entries[bucket_idx * kEntriesPerBucket];

  // EVERY entry with this tag at this offset goes: two such entries (one of
  // them dead) cannot be told apart, and removing only the first could leave
  // the live one behind.
  size_t removed = 0;
  for (size_t i = 0; i < kEntriesPerBucket; ++i) {
    if (!bucket[i].is_empty() && bucket[i].tag() == target_tag &&
        bucket[i].offset() == target_offset) {
      if (removed == 0) {
        begin_bucket_write(bucket_idx);
      }
      bucket[i].clear();
      ++removed;
    }
  }
  if (removed != 0) {
    end_bucket_write(bucket_idx);
    _count.fetch_sub(removed, std::memory_order_relaxed);
  }
  return removed != 0;
}

void Directory::toggle_phase() {
  // seq_cst: the phase is the other half of the reader's
  // stamp-then-revalidate wrap epoch — mirror
  // MmapDirectory::toggle_phase.  Concurrent probes are not version-fenced
  // against the flip; a reader that raced it revalidates via
  // Volume::borrow_still_valid (epoch mismatch) and retries.  Load-then-
  // store is safe: mutators are externally serialized (class comment).
  _current_phase.store(!_current_phase.load(std::memory_order_seq_cst),
                       std::memory_order_seq_cst);
}

std::span<const std::byte> Directory::serialize() const {
  return {reinterpret_cast<const std::byte *>(_entries.data()),
          _entries.size() * sizeof(DirEntry)};
}

// Requires full quiescence (startup/reset only), like clear().
void Directory::deserialize(std::span<const std::byte> data) {
  size_t entry_count = data.size() / sizeof(DirEntry);
  if (entry_count > _entries.size()) {
    entry_count = _entries.size();
  }
  std::memcpy(_entries.data(), data.data(), entry_count * sizeof(DirEntry));

  size_t count = 0;
  for (const auto &entry : _entries) {
    if (!entry.is_empty()) {
      ++count;
    }
  }
  _count.store(count, std::memory_order_relaxed);
}

}  // namespace cyclone
