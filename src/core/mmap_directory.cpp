// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "mmap_directory.hpp"

#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <immintrin.h>
#endif

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>

#include <cerrno>
#include <csignal>
#endif

// TSan annotations are defined in mmap_directory.hpp
// (CYCLONE_TSAN_ACQUIRE/RELEASE). Local aliases for brevity.
#define TSAN_ACQUIRE(addr) CYCLONE_TSAN_ACQUIRE(addr)
#define TSAN_RELEASE(addr) CYCLONE_TSAN_RELEASE(addr)

namespace cyclone {

namespace {

/// This process's PID as the value published into write_lock_owner_pid.
uint32_t self_pid() {
#if defined(_WIN32)
  return static_cast<uint32_t>(::GetCurrentProcessId());
#else
  return static_cast<uint32_t>(::getpid());
#endif
}

/// Cross-process liveness probe for a write-lock holder.  Returns true ONLY
/// when the OS positively reports the PID as gone; a holder we cannot prove
/// dead is treated as alive and never usurped (the crux of the fix).  This
/// is the robust-futex "owner-died" check without a futex: a live-but-
/// stalled holder (page fault, scheduler preemption) is waited on, not
/// force-released out from under its in-flight reservation.
bool holder_is_dead(uint32_t pid) {
  if (pid == 0) {
    return false;  // owner not yet published — cannot prove death
  }
#if defined(_WIN32)
  HANDLE h = ::OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
  if (h == nullptr) {
    // No such process => dead; ACCESS_DENIED etc. => exists, treat as alive.
    return ::GetLastError() == ERROR_INVALID_PARAMETER;
  }
  DWORD wait = ::WaitForSingleObject(h, 0);
  ::CloseHandle(h);
  return wait == WAIT_OBJECT_0;  // signaled => the process has exited
#else
  // kill(pid, 0): 0 => alive; ESRCH => no such process (dead); EPERM =>
  // exists but not ours (alive).  Only ESRCH proves death.
  if (::kill(static_cast<pid_t>(pid), 0) == 0) {
    return false;
  }
  return errno == ESRCH;
#endif
}

// Cross-process CAS lock values (phase_lock, write_lock).  0 = free.  A
// holder stores a token derived from the lock's takeover generation, which
// only a recovery bumps: every acquisition within one generation stores the
// same token, and a holder recovered from under us stores a DIFFERENT one
// than the usurper, so the usurped holder's late CAS release (token -> 0)
// fails instead of freeing the usurper.  Aliasing needs 253 recoveries
// while the usurped holder stays descheduled, each after a full budget.
// Tokens are 1..253; 254 and 255 mark a lock being recovered (the claim
// alternates so that two waiters recovering the same stuck value -- only
// possible when a previous recoverer died mid-recovery -- cannot both win).
constexpr uint8_t kLockRecoveringA = 254;
constexpr uint8_t kLockRecoveringB = 255;

constexpr uint8_t lock_token(uint16_t generation) {
  return static_cast<uint8_t>(1 + generation % 253);
}

constexpr uint8_t recovery_claim(uint8_t observed) {
  return observed == kLockRecoveringA ? kLockRecoveringB : kLockRecoveringA;
}

std::chrono::steady_clock::duration bucket_writer_budget() {
#ifdef CYCLONE_TEST_SEAMS
  const uint64_t us = MmapDirectory::s_bucket_writer_budget_us_for_test.load(
      std::memory_order_relaxed);
  if (us != 0) {
    return std::chrono::microseconds(us);
  }
#endif
  return MmapDirectory::kBucketWriterBudget;
}

std::chrono::steady_clock::duration phase_lock_budget() {
#ifdef CYCLONE_TEST_SEAMS
  const uint64_t us = MmapDirectory::s_phase_lock_budget_us_for_test.load(
      std::memory_order_relaxed);
  if (us != 0) {
    return std::chrono::microseconds(us);
  }
#endif
  return MmapDirectory::kPhaseLockBudget;
}

std::chrono::steady_clock::duration write_lock_escalation_budget() {
#ifdef CYCLONE_TEST_SEAMS
  const uint64_t us = MmapDirectory::s_write_lock_escalation_us_for_test.load(
      std::memory_order_relaxed);
  if (us != 0) {
    return std::chrono::microseconds(us);
  }
#endif
  return MmapDirectory::kWriteLockEscalationBudget;
}

}  // namespace

std::optional<MmapDirectory> MmapDirectory::init(std::span<std::byte> region,
                                                 size_t num_buckets) {
  // Validate num_buckets
  if (num_buckets == 0) {
    return std::nullopt;
  }

  // Check that num_buckets fits in uint32_t (for header storage)
  if (num_buckets > UINT32_MAX) {
    return std::nullopt;
  }

  // Calculate required size with overflow protection
  size_t required = required_size(num_buckets);
  if (required == SIZE_MAX) {
    return std::nullopt;  // Overflow in size calculation
  }
  if (region.size() < required) {
    return std::nullopt;
  }

  // Calculate offsets
  size_t versions_offset = sizeof(Header);
  const size_t entries_off = entries_offset(num_buckets);
  const size_t retention_off = retention_offset(num_buckets);

  // Initialize header
  auto *header = reinterpret_cast<Header *>(region.data());
  header->magic = kMagic;
  header->version = kVersion;
  header->reserved = 0;
  header->num_buckets = static_cast<uint32_t>(num_buckets);
  header->entry_count = 0;
  header->current_phase = 0;
  header->phase_lock = 0;
  header->write_lock_gen = 0;        // takeover generation (write-lock fix)
  header->write_lock_owner_pid = 0;  // 0 = no holder / unpublished
  header->shared_write_pos = 0;
  header->write_lock = 0;
  header->wrap_intent = 0;     // 0 = no wrap in flight
  header->phase_lock_gen = 0;  // the v1 stripe_borrow_slot
  // 0 = no wrap currently deferred.  Every shared-header field
  // must be zero-init safe: a fresh volume can be created over recycled
  // memory, so a field left unzeroed starts with garbage.
  header->shared_wrap_deferred_deadline_ms = 0;
  header->shared_wrap_count = 0;
  header->shared_last_wrap_time_ns = 0;
  header->stripe_lease_expiry_ns = 0;  // 0 = no lease

  // Initialize version counters to 0
  auto *versions =
      reinterpret_cast<uint32_t *>(region.data() + versions_offset);
  for (size_t i = 0; i < num_buckets; ++i) {
    std::atomic_ref<uint32_t>(versions[i]).store(0, std::memory_order_relaxed);
  }

  // Initialize entries to empty
  auto *entries = reinterpret_cast<DirEntry *>(region.data() + entries_off);
  std::memset(entries, 0, num_buckets * kEntriesPerBucket * sizeof(DirEntry));

  // Retention region: G = 0 (pass 0, nothing exposed yet) and every chunk
  // borrow slot {generation 0, count 0}.
  auto *retention =
      reinterpret_cast<RetentionRegion *>(region.data() + retention_off);
  std::memset(retention, 0, sizeof(RetentionRegion));

  // Ensure all writes are visible
  std::atomic_thread_fence(std::memory_order_release);

  return MmapDirectory(header, versions, entries, retention, num_buckets);
}

std::optional<MmapDirectory> MmapDirectory::open(std::span<std::byte> region) {
  if (region.size() < sizeof(Header)) {
    return std::nullopt;
  }

  auto *header = reinterpret_cast<Header *>(region.data());

  // Validate magic and version
  if (header->magic != kMagic) {
    return std::nullopt;
  }
  if (header->version != kVersion) {
    return std::nullopt;
  }

  size_t num_buckets = header->num_buckets;

  // Security: validate num_buckets before trusting it
  // This prevents malicious headers from causing integer overflow or OOB access
  if (num_buckets == 0) {
    return std::nullopt;  // Invalid: need at least one bucket
  }

  // Calculate required size with overflow protection
  size_t required = required_size(num_buckets);
  if (required == SIZE_MAX) {
    return std::nullopt;  // Overflow in size calculation
  }
  if (region.size() < required) {
    return std::nullopt;
  }

  // Calculate offsets (safe now that we've validated num_buckets fits)
  size_t versions_offset = sizeof(Header);
  auto *versions =
      reinterpret_cast<uint32_t *>(region.data() + versions_offset);
  auto *entries =
      reinterpret_cast<DirEntry *>(region.data() + entries_offset(num_buckets));
  auto *retention = reinterpret_cast<RetentionRegion *>(
      region.data() + retention_offset(num_buckets));

  return MmapDirectory(header, versions, entries, retention, num_buckets);
}

bool MmapDirectory::is_foreign_version(std::span<const std::byte> region) {
  if (region.size() < sizeof(Header)) {
    return false;
  }
  uint32_t magic = 0;
  uint16_t version = 0;
  std::memcpy(&magic, region.data() + offsetof(Header, magic), sizeof(magic));
  std::memcpy(&version, region.data() + offsetof(Header, version),
              sizeof(version));
  return magic == kMagic && version != kVersion;
}

std::span<std::byte> MmapDirectory::region() const {
  if (_header == nullptr) {
    return {};
  }
  return {reinterpret_cast<std::byte *>(_header), required_size(_num_buckets)};
}

MmapDirectory::MmapDirectory(Header *header, uint32_t *versions,
                             DirEntry *entries, RetentionRegion *retention,
                             size_t num_buckets)
    : _header(header),
      _versions(versions),
      _entries(entries),
      _retention(retention),
      _num_buckets(num_buckets) {}

MmapDirectory::MmapDirectory(MmapDirectory &&other) noexcept
    : _header(other._header),
      _versions(other._versions),
      _entries(other._entries),
      _retention(other._retention),
      _num_buckets(other._num_buckets) {
  other._header = nullptr;
  other._versions = nullptr;
  other._entries = nullptr;
  other._retention = nullptr;
  other._num_buckets = 0;
}

MmapDirectory &MmapDirectory::operator=(MmapDirectory &&other) noexcept {
  if (this != &other) {
    _header = other._header;
    _versions = other._versions;
    _entries = other._entries;
    _retention = other._retention;
    _num_buckets = other._num_buckets;

    other._header = nullptr;
    other._versions = nullptr;
    other._entries = nullptr;
    other._retention = nullptr;
    other._num_buckets = 0;
  }
  return *this;
}

std::optional<DirEntry> MmapDirectory::probe(const CacheKey &key) const {
  if (_header == nullptr) {
    return std::nullopt;
  }

  uint32_t bucket_idx = key.bucket_hash() % _num_buckets;
  uint16_t target_tag = key.tag();
  bool cur_phase = current_phase();

  // Retry loop for torn read detection, bounded like probe_each_impl;
  // `continue` lands on wait.retry().
  SeqlockReadWait wait;
  do {
    // Wait for an even version before starting the scan.
    const uint32_t version_before =
        wait.even_version([&] { return load_version(bucket_idx); });
    if ((version_before & 1) != 0) {
      continue;  // Writer still active: retry (wait.retry() paces it)
    }

    // Memory barrier to ensure we read entries after version
    std::atomic_thread_fence(std::memory_order_acquire);
    TSAN_ACQUIRE(&_versions[bucket_idx]);

    const DirEntry *bucket = &_entries[bucket_idx * kEntriesPerBucket];
    bool found_match = false;

    for (size_t i = 0; i < kEntriesPerBucket; ++i) {
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
        if (version_before == version_after) {
          return entry;  // Consistent read
        }
        // Version changed, will retry
        found_match = true;
        break;
      }
    }

    if (found_match) {
      continue;  // Retry due to version mismatch
    }

    // Check if we had a consistent read of the bucket (no match found)
    std::atomic_thread_fence(std::memory_order_acquire);
    uint32_t version_after = load_version(bucket_idx);
    if (version_before == version_after) {
      return std::nullopt;  // Consistent read, key not found
    }
    // Version changed - retry
  } while (wait.retry());

  // Budget spent: unknown, reported as absent by this test-only probe
  return std::nullopt;
}

// Writer-writer exclusion via CAS spinlock on the seqlock version counter.
// The version goes even→odd (acquire) before modification, odd→even (release)
// after.  Concurrent writers spin-wait until the version is even.
bool MmapDirectory::insert(const CacheKey &key, uint64_t offset, uint64_t size,
                           uint64_t verified_offset, bool *collision_evicted,
                           bool *bucket_full_evicted,
                           InsertAdmission *admission,
                           std::span<const uint64_t> clear_offsets) {
  if (_header == nullptr) {
    return false;
  }

  uint32_t bucket_idx = key.bucket_hash() % _num_buckets;
  uint16_t tag = key.tag();

  DirEntry *bucket = &_entries[bucket_idx * kEntriesPerBucket];

  // Acquire cross-process phase lock FIRST, then bucket writer lock.
  // This ensures no phase toggle can happen between reading current_phase
  // and writing the entry with that phase — preventing the ABA race where
  // another process's evict_if_needed() invalidates our freshly-written entry.
  const PhaseLockToken phase_token = acquire_phase_lock();
  bool cur_phase = current_phase();

  // Acquire writer lock (CAS even → odd, spins if another writer holds it).
  const uint32_t writer_token = acquire_writer(bucket_idx);

  if (admission != nullptr) {
    // Boundaries are loaded HERE, inside the bracket and under phase_lock:
    // an independently opened writer in another process may have wrapped
    // or advanced since the caller's election (S9).
    admission->refresh();
    const InsertChoice choice = choose_insert_slot(
        bucket, kEntriesPerBucket, tag, verified_offset, kMatchAnyTag,
        *admission, collision_evicted, bucket_full_evicted, offset);
    size_t cleared = 0;
    if (choice.slot >= 0) {
      DirEntry new_entry = choice.verified ? bucket[choice.slot] : DirEntry{};
      new_entry.set_offset(offset);
      new_entry.set_approx_size(size);
      new_entry.set_tag(tag);
      new_entry.set_phase(cur_phase);
      if (!choice.verified) {
        new_entry.set_head(true);
      }
      std::memcpy(&bucket[choice.slot], &new_entry, sizeof(DirEntry));
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
    }
    release_writer(bucket_idx, writer_token);
    release_phase_lock(phase_token);
    if (choice.slot >= 0 && !choice.replaces) {
      increment_count();
    }
    for (size_t i = 0; i < cleared; ++i) {
      decrement_count();
    }
    return choice.slot >= 0;
  }

  bool success = false;
  bool was_update = false;

  // Single-pass scan: find matching tag, first empty slot, or first stale slot.
  int first_empty = -1;
  int first_stale = -1;
  int first_collider = -1;
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
        // Build complete entry locally then copy atomically
        DirEntry new_entry = bucket[i];
        new_entry.set_offset(offset);
        new_entry.set_approx_size(size);
        new_entry.set_phase(cur_phase);
        std::memcpy(&bucket[i], &new_entry, sizeof(DirEntry));
        success = true;
        was_update = true;
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

  // Prefer empty slot over stale slot
  if (!success && first_empty >= 0) {
    // Build complete entry locally then copy atomically
    DirEntry new_entry;
    new_entry.set_offset(offset);
    new_entry.set_approx_size(size);
    new_entry.set_tag(tag);
    new_entry.set_phase(cur_phase);
    new_entry.set_head(true);
    std::memcpy(&bucket[first_empty], &new_entry, sizeof(DirEntry));
    success = true;
  }

  // Reuse stale slot (don't increment count — replacing an existing entry)
  if (!success && first_stale >= 0) {
    // Build complete entry locally then copy atomically
    DirEntry new_entry;
    new_entry.set_offset(offset);
    new_entry.set_approx_size(size);
    new_entry.set_tag(tag);
    new_entry.set_phase(cur_phase);
    new_entry.set_head(true);
    std::memcpy(&bucket[first_stale], &new_entry, sizeof(DirEntry));
    success = true;
    was_update = true;  // Replacing stale entry, don't increment count
  }

  // Bucket full of current-phase entries and one collides on the tag —
  // evict the collider so the write still lands (matches the pre-fix
  // outcome for the victim, but now detected and reported, never silent).
  if (!success && first_collider >= 0) {
    // Build complete entry locally then copy atomically
    DirEntry new_entry;
    new_entry.set_offset(offset);
    new_entry.set_approx_size(size);
    new_entry.set_tag(tag);
    new_entry.set_phase(cur_phase);
    new_entry.set_head(true);
    std::memcpy(&bucket[first_collider], &new_entry, sizeof(DirEntry));
    success = true;
    was_update = true;  // Replacing the collider, don't increment count
    if (collision_evicted != nullptr) {
      *collision_evicted = true;
    }
  }

  // Bucket full of current-phase entries and none collides on the tag — evict
  // the entry nearest the wrap cursor so the write still lands instead of
  // failing outright.  A DirEntry carries no recency data, but
  // write_pos advances monotonically within a phase, so the lowest offset in
  // this stripe's bucket is the entry closest to being overwritten anyway.
  // NOT strict eviction by age: a phase-ABA survivor in the trailing gap reads
  // current-phase with a HIGH offset.  Correctness never depends on the choice.
  if (!success && nearest_to_clobber >= 0) {
    // Build complete entry locally then copy atomically
    DirEntry new_entry;
    new_entry.set_offset(offset);
    new_entry.set_approx_size(size);
    new_entry.set_tag(tag);
    new_entry.set_phase(cur_phase);
    new_entry.set_head(true);
    std::memcpy(&bucket[nearest_to_clobber], &new_entry, sizeof(DirEntry));
    success = true;
    was_update = true;  // Replacing an existing entry, don't increment count
    if (bucket_full_evicted != nullptr) {
      *bucket_full_evicted = true;
    }
  }

  // Release writer lock (odd → even).
  release_writer(bucket_idx, writer_token);

  // Release phase lock after entry is written.
  release_phase_lock(phase_token);

  if (success && !was_update) {
    increment_count();
  }

  return success;
}

bool MmapDirectory::remove(const CacheKey &key) {
  if (_header == nullptr) {
    return false;
  }

  uint32_t bucket_idx = key.bucket_hash() % _num_buckets;
  uint16_t target_tag = key.tag();

  DirEntry *bucket = &_entries[bucket_idx * kEntriesPerBucket];

  // Acquire writer lock (CAS even → odd, spins if another writer holds it).
  const uint32_t writer_token = acquire_writer(bucket_idx);

  bool found = false;
  for (size_t i = 0; i < kEntriesPerBucket; ++i) {
    if (!bucket[i].is_empty() && bucket[i].tag() == target_tag) {
      bucket[i].clear();
      found = true;
      break;
    }
  }

  // Release writer lock (odd → even).
  release_writer(bucket_idx, writer_token);

  if (found) {
    decrement_count();
  }

  return found;
}

bool MmapDirectory::remove_at(const CacheKey &key, uint64_t target_offset) {
  if (_header == nullptr) {
    return false;
  }

  uint32_t bucket_idx = key.bucket_hash() % _num_buckets;
  uint16_t target_tag = key.tag();

  DirEntry *bucket = &_entries[bucket_idx * kEntriesPerBucket];

  const uint32_t writer_token = acquire_writer(bucket_idx);

  // EVERY entry with this tag at this offset goes (see Directory::remove_at).
  size_t removed = 0;
  for (size_t i = 0; i < kEntriesPerBucket; ++i) {
    if (!bucket[i].is_empty() && bucket[i].tag() == target_tag &&
        bucket[i].offset() == target_offset) {
      bucket[i].clear();
      ++removed;
    }
  }

  release_writer(bucket_idx, writer_token);

  for (size_t i = 0; i < removed; ++i) {
    decrement_count();
  }

  return removed != 0;
}

void MmapDirectory::clear() {
  if (_header == nullptr) {
    return;
  }

  // Acquire writer lock on every bucket.  This uses the same CAS
  // spinlock protocol as insert()/remove(), ensuring concurrent
  // writers on other processes spin until we release.
  std::vector<uint32_t> writer_tokens(_num_buckets);
  for (size_t i = 0; i < _num_buckets; ++i) {
    writer_tokens[i] = acquire_writer(i);
  }

  // Clear all entries and reset count while holding all locks.
  std::memset(_entries, 0, _num_buckets * kEntriesPerBucket * sizeof(DirEntry));
  std::atomic_ref<uint32_t>(_header->entry_count)
      .store(0, std::memory_order_relaxed);

  // Release all writer locks.
  for (size_t i = 0; i < _num_buckets; ++i) {
    release_writer(i, writer_tokens[i]);
  }
}

size_t MmapDirectory::count() const {
  if (_header == nullptr) {
    return 0;
  }
  return std::atomic_ref<uint32_t>(const_cast<uint32_t &>(_header->entry_count))
      .load(std::memory_order_relaxed);
}

size_t MmapDirectory::capacity() const {
  return _num_buckets * kEntriesPerBucket;
}

size_t MmapDirectory::bucket_count() const { return _num_buckets; }

bool MmapDirectory::current_phase() const {
  if (_header == nullptr) {
    return false;
  }
  return std::atomic_ref<uint8_t>(const_cast<uint8_t &>(_header->current_phase))
             .load(std::memory_order_acquire) != 0;
}

void MmapDirectory::toggle_phase() {
  if (_header == nullptr) {
    return;
  }
  // Acquire cross-process phase lock to prevent toggling phase while
  // another process is mid-insert (reading current_phase + writing entry).
  // seq_cst (upgraded from release): the phase is the other half of the
  // reader's stamp-then-revalidate epoch.
  const PhaseLockToken phase_token = acquire_phase_lock();
  std::atomic_ref<uint8_t>(_header->current_phase)
      .fetch_xor(1, std::memory_order_seq_cst);
  release_phase_lock(phase_token);
}

void MmapDirectory::set_current_phase(bool phase) {
  if (_header == nullptr) {
    return;
  }
  // Same lock and ordering as toggle_phase: an insert that read the phase
  // under phase_lock must not have it change underneath its entry store.
  const PhaseLockToken phase_token = acquire_phase_lock();
  std::atomic_ref<uint8_t>(_header->current_phase)
      .store(phase ? 1 : 0, std::memory_order_seq_cst);
  release_phase_lock(phase_token);
}

void MmapDirectory::reset_reader_state_exclusive() {
  if (_header == nullptr) {
    return;
  }
  // A phase lock left held by a process that died inside it: no live peer
  // can hold it now, so free it instead of making the first insert wait out
  // kPhaseLockBudget.  (phase_lock_gen, the former v1 borrow slot at the
  // same offset, is left alone: it only ever advances.)
  std::atomic_ref<uint8_t>(_header->phase_lock)
      .store(0, std::memory_order_seq_cst);
  for (auto &slot : _retention->chunk_borrows) {
    std::atomic_ref<uint32_t>(slot).store(0, std::memory_order_seq_cst);
  }
  std::atomic_ref<uint64_t>(_header->stripe_lease_expiry_ns)
      .store(0, std::memory_order_seq_cst);
  std::atomic_ref<uint32_t>(_header->shared_wrap_deferred_deadline_ms)
      .store(0, std::memory_order_seq_cst);
}

// The phase lock serializes an insert's read of current_phase + its entry
// store (and admission refresh) against a phase toggle / re-derivation.
//
// Waiting (issue #27).  A holder that stays in the lock is either dead or
// descheduled; a spin count cannot tell the two apart.  The waiter paces
// itself with LockHolderWait (a short spin, then sleeping backoff) and only
// presumes the holder stuck once that same holder has kept the lock for
// kPhaseLockBudget.  The header has no room to publish the holder's PID for
// a liveness proof like the write lock's, so the budget is what separates
// "descheduled" from "stuck": it is set well above the longest live hold
// measured under CPU oversubscription, and above kBucketWriterBudget, which
// bounds the holder's own nested wait on a bucket (see the PR for #27).
//
// Why presuming is acceptable here, unlike the write lock: what can a
// usurped holder that is still executing do?  Its insert already read the
// phase and still takes its bucket as a proper seqlock writer, so it can
// only publish its (fully written) entry stamped with a phase the usurper
// may have toggled away in between, possibly after the usurper's eviction
// sweep.  That is exactly the phase-ABA stale entry every read already
// handles: the positional guard / retention classification at both read
// choke points, full-key re-verification of the stored first_key and the
// document CRC turn it into a miss, never foreign or torn bytes
// (invariants 8 and 9).  A toggle racing an insert is likewise caught by
// the reader's stamp-then-revalidate epoch.  So recovering a live holder
// costs at most a spurious miss, while never recovering would let one
// crashed process wedge every writer of the stripe.
//
// Release is token-checked: a holder recovered from under us finds a
// different token (the generation was bumped) and its late release is a
// no-op, so it can never free the usurper's -- or a later holder's --
// critical section.
MmapDirectory::PhaseLockToken MmapDirectory::acquire_phase_lock() {
  auto lock = std::atomic_ref<uint8_t>(_header->phase_lock);
  auto gen = std::atomic_ref<uint16_t>(_header->phase_lock_gen);
  // Hot uncontended path: generation load, CAS, generation reload (same
  // cache line).  The reload catches a recovery that bumped the
  // generation between the sample and the CAS: the CAS acquired the 0 that
  // recovery (or a later release) stored AFTER its bump, so the bump
  // happens-before the reload.
  const uint16_t g = gen.load(std::memory_order_relaxed);
  const PhaseLockToken token = lock_token(g);
  uint8_t expected = 0;
  if (lock.compare_exchange_strong(expected, token, std::memory_order_acquire,
                                   std::memory_order_relaxed)) [[likely]] {
    if (gen.load(std::memory_order_relaxed) == g) [[likely]] {
      return token;
    }
    const PhaseLockToken moved = retoken_phase_lock(token);
    if (moved != 0) {
      return moved;
    }
  }
  return acquire_phase_lock_slow();
}

MmapDirectory::PhaseLockToken MmapDirectory::retoken_phase_lock(
    PhaseLockToken held) {
  // We hold the lock with the token of a generation that a recovery has
  // since retired -- the same token the usurped holder carries, whose late
  // release would free us.  Move to the current generation's token; if the
  // lock is no longer ours (that late release already happened), report it
  // lost and the caller re-acquires.  We never entered the critical section
  // on the stale token, so losing it here is harmless.
  auto lock = std::atomic_ref<uint8_t>(_header->phase_lock);
  auto gen = std::atomic_ref<uint16_t>(_header->phase_lock_gen);
  for (;;) {
    const PhaseLockToken want = lock_token(gen.load(std::memory_order_relaxed));
    if (want == held) {
      return held;
    }
    uint8_t expected = held;
    if (!lock.compare_exchange_strong(expected, want, std::memory_order_acquire,
                                      std::memory_order_relaxed)) {
      return 0;
    }
    held = want;
  }
}

MmapDirectory::PhaseLockToken MmapDirectory::acquire_phase_lock_slow() {
  auto lock = std::atomic_ref<uint8_t>(_header->phase_lock);
  auto gen = std::atomic_ref<uint16_t>(_header->phase_lock_gen);
  LockHolderWait wait(phase_lock_budget());
  for (;;) {
    const uint16_t g = gen.load(std::memory_order_relaxed);
    uint8_t observed = 0;
    if (lock.compare_exchange_weak(observed, lock_token(g),
                                   std::memory_order_acquire,
                                   std::memory_order_relaxed)) {
      if (gen.load(std::memory_order_relaxed) == g) {
        return lock_token(g);
      }
      const PhaseLockToken moved = retoken_phase_lock(lock_token(g));
      if (moved != 0) {
        return moved;
      }
      continue;
    }
    if (observed == 0) {
      continue;  // Spurious CAS failure
    }
    if (wait.wait(observed) != LockHolderWait::Step::kExpired) {
      continue;
    }
    // `observed` has held the lock for the whole budget: recover it from
    // exactly that holder.  Claim first (a CAS, so a holder that just
    // released, or a new holder, is never touched), retire its token by
    // bumping the generation, then free the lock and contend again.
    uint8_t stuck = observed;
    if (lock.compare_exchange_strong(stuck, recovery_claim(observed),
                                     std::memory_order_acq_rel,
                                     std::memory_order_relaxed)) {
      gen.fetch_add(1, std::memory_order_acq_rel);
      lock.store(0, std::memory_order_release);
#ifdef CYCLONE_TEST_SEAMS
      s_phase_lock_recoveries_for_test.fetch_add(1, std::memory_order_relaxed);
#endif
    }
  }
}

void MmapDirectory::release_phase_lock(PhaseLockToken token) {
  // token -> 0, only if the lock is still ours (see acquire_phase_lock).
  uint8_t expected = token;
  (void)std::atomic_ref<uint8_t>(_header->phase_lock)
      .compare_exchange_strong(expected, 0, std::memory_order_release,
                               std::memory_order_relaxed);
}

MmapDirectory::WriteLockToken MmapDirectory::acquire_write_lock() {
  // The write lock serializes the shared_write_pos → reserve-byte-range
  // publish sequence.  Unlike phase_lock and the per-bucket writer seqlock
  // — whose torn writes a reader DETECTS (seqlock version change, full-key
  // verify, per-document checksum) and degrades to a retry/miss — a usurped
  // write-lock holder produces OVERLAPPING pwrites onto reserved bytes with
  // no wrap event and no epoch bump: undetectable, so a copy-then-verify
  // consumer can accept corrupt bytes.  Hence this lock (and only this one)
  // proves the holder dead before recovering it, and hands back a token the
  // caller revalidates before it publishes or lets its pwrite proceed.
  //
  // The lock byte carries the holder's token, derived from write_lock_gen
  // (lock_token), so the release is a CAS from exactly that value and a
  // recovery claims exactly the value it gave up on.
  auto lock = std::atomic_ref<uint8_t>(_header->write_lock);
  auto gen = std::atomic_ref<uint16_t>(_header->write_lock_gen);
  auto owner = std::atomic_ref<uint32_t>(_header->write_lock_owner_pid);

  WriteLockToken token;

  // TOKEN ORDERING IS LOAD-BEARING: sample the generation BEFORE the CAS,
  // both seq_cst.  Sampling after the CAS reopens the bug through an
  // acquire-tail stall: win the CAS, stall before the sample, get escalated
  // (gen G -> G+1), resume and sample G+1 — a spuriously VALID token for a
  // usurped acquisition, whose revalidate then passes and whose checked
  // release frees the usurper's lock.  With the pre-CAS sample, seq_cst
  // program order puts the sample before the CAS in the total order S, and
  // the escalation's bump is seq_cst too — so ANY bump that lands after the
  // sample (including one racing the CAS itself) makes revalidate fail.
  // A bump landing BETWEEN sample and CAS, with us as the legitimate
  // post-recovery acquirer, is caught by confirm_write_lock, which moves
  // the held lock to the current generation's token (formerly: a spurious
  // Busy, and a lock leaked until the next escalation).

  // Hot uncontended path: one generation load, a single CAS, and the
  // confirming reload.
  token.generation = gen.load(std::memory_order_seq_cst);
  token.value = lock_token(token.generation);
  uint8_t expected = 0;
  if (lock.compare_exchange_strong(expected, token.value,
                                   std::memory_order_seq_cst,
                                   std::memory_order_relaxed) &&
      confirm_write_lock(token)) {
    owner.store(self_pid(), std::memory_order_relaxed);
    token.acquired = true;
    return token;
  }

  // Contended path.  Wait with LockHolderWait (spin, then sleeping
  // backoff); after every sleep, consult liveness.  Recovery from a
  // genuinely dead holder is preserved (a crash must not deadlock the
  // cache); usurpation of a live-but-stalled holder is removed.
  //
  // Last-resort deadlock breaker for the pathological case where kill(2)
  // keeps reporting the holder alive but it never releases (PID reuse, or a
  // live holder wedged for many seconds): the same holder has held the lock
  // for kWriteLockEscalationBudget.  If this ever fires on a truly live
  // holder, the generation bump below makes that holder's revalidate fail,
  // so it aborts before its overlapping publish/pwrite — corruption stays
  // closed; only availability degrades.
  LockHolderWait wait(write_lock_escalation_budget());
  for (;;) {
    // Same pre-CAS sample discipline as the fast path (see above).
    token.generation = gen.load(std::memory_order_seq_cst);
    token.value = lock_token(token.generation);
    expected = 0;
    if (lock.compare_exchange_weak(expected, token.value,
                                   std::memory_order_seq_cst,
                                   std::memory_order_relaxed)) {
      if (confirm_write_lock(token)) {
        owner.store(self_pid(), std::memory_order_relaxed);
        token.acquired = true;
        return token;
      }
      continue;
    }
    if (expected == 0) {
      continue;  // Spurious CAS failure
    }
    const uint32_t holder = owner.load(std::memory_order_acquire);
    // A holder is its lock value plus its published PID: another process
    // taking the lock restarts the wait (LockHolderWait).
    const LockHolderWait::Step step =
        wait.wait((uint64_t{expected} << 32) | holder);
    if (step == LockHolderWait::Step::kSpun) {
      continue;
    }

    // Slept, or the escalation budget is spent: prove DEAD before we
    // recover, or WAIT.
    const bool presume_dead =
        s_write_lock_presume_dead_for_test.load(std::memory_order_relaxed);
    const bool dead = presume_dead || holder_is_dead(holder);
    bool escalate = step == LockHolderWait::Step::kExpired;
    if (step == LockHolderWait::Step::kSlept) {
      ++token.live_waits;
      // Test seam: a nonzero override escalates after that many live
      // waits, so the last-resort takeover is reachable in test time (see
      // the F6-F test).
      const uint32_t max_live_waits_override =
          s_write_lock_max_live_waits_for_test.load(std::memory_order_relaxed);
      escalate = max_live_waits_override != 0 &&
                 token.live_waits >= max_live_waits_override;
    }
    if (!dead && !escalate) {
      continue;  // Live holder (or an owner not yet published): WAIT
    }

    // Recover the lock from exactly the holder value we gave up on: claim
    // it (a CAS, so a holder that just released, or a new holder -- whose
    // token differs once any recovery happened -- is never touched), bump
    // the generation (seq_cst) so a holder that is (pathologically) still
    // live sees the takeover at its revalidate and aborts before publishing
    // shared_write_pos / pwriting, then free the lock.
    uint8_t stuck = expected;
    if (!lock.compare_exchange_strong(stuck, recovery_claim(expected),
                                      std::memory_order_seq_cst,
                                      std::memory_order_relaxed)) {
      continue;  // The holder moved on; wait on whoever holds it now
    }
    gen.fetch_add(1, std::memory_order_seq_cst);
    owner.store(0, std::memory_order_relaxed);
    lock.store(0, std::memory_order_seq_cst);
    // Telemetry split: recovering a PROVEN-dead holder is routine crash
    // recovery; taking over a holder we could NOT prove dead (last-resort
    // escalation) is the alertable event.
    if (dead) {
      token.forced_release = true;
    } else {
      token.escalated_takeover = true;
    }
    // Re-contend for the just-freed lock (another waiter may win the race;
    // that is fine — the generation only advances).
  }
}

bool MmapDirectory::confirm_write_lock(WriteLockToken &token) {
  // Called right after a winning CAS stored token.value.  The CAS acquired
  // the 0 that a recovery (or a later release) stored after its generation
  // bump, so a bump that landed between our sample and our CAS is visible
  // here.  We then hold the lock under a retired token -- the usurped
  // holder's -- so move it to the current generation's (the same step as
  // retoken_phase_lock).  A bump that recovers the lock from US instead
  // claims our value first, so the move's CAS fails and we report the
  // lock lost before entering the critical section.
  auto lock = std::atomic_ref<uint8_t>(_header->write_lock);
  auto gen = std::atomic_ref<uint16_t>(_header->write_lock_gen);
  for (;;) {
    const uint16_t now = gen.load(std::memory_order_seq_cst);
    if (now == token.generation) {
      return true;
    }
    const uint8_t want = lock_token(now);
    uint8_t expected = token.value;
    if (want != token.value &&
        !lock.compare_exchange_strong(expected, want, std::memory_order_seq_cst,
                                      std::memory_order_relaxed)) {
      return false;
    }
    token.generation = now;
    token.value = want;
  }
}

MmapDirectory::WriteLockToken MmapDirectory::try_acquire_write_lock() {
  // Non-blocking counterpart of acquire_write_lock for the two in-place
  // header RMW sites (update_hit_count_sync / remove_alternate_sync's chain
  // repoint).  It is EXACTLY acquire_write_lock's uncontended fast path -- one
  // pre-CAS generation sample, a single CAS and the confirming reload --
  // with no wait, liveness probe, or usurpation.  On contention it returns
  // {acquired=false} so the caller never blocks a request thread behind a
  // peer's pwrite+fsync.
  //
  // A recovery that bumps the generation between the sample and the winning
  // CAS is handled by confirm_write_lock like on the blocking path: the
  // held lock moves to the current generation's token, or, if it was lost
  // meanwhile, this reports {acquired=false} without entering the critical
  // section.  (This used to be an accepted rare leak: the stale token failed
  // revalidate, and the revalidate-gated release left the lock byte held
  // until the next waiter's escalation.)
  auto lock = std::atomic_ref<uint8_t>(_header->write_lock);
  auto gen = std::atomic_ref<uint16_t>(_header->write_lock_gen);
  auto owner = std::atomic_ref<uint32_t>(_header->write_lock_owner_pid);

  WriteLockToken token;
  token.generation = gen.load(std::memory_order_seq_cst);  // pre-CAS sample
  token.value = lock_token(token.generation);
  uint8_t expected = 0;
  if (!lock.compare_exchange_strong(expected, token.value,
                                    std::memory_order_seq_cst,
                                    std::memory_order_relaxed) ||
      !confirm_write_lock(token)) {
    return token;  // Held by someone else — do not wait, do not usurp.
  }
  owner.store(self_pid(), std::memory_order_relaxed);
  token.acquired = true;
  return token;
}

bool MmapDirectory::revalidate_write_lock(const WriteLockToken &token) const {
  // Primary check: no force-release bumped the takeover generation since
  // this acquisition SAMPLED it (the sample precedes the acquire CAS, and
  // confirm_write_lock moved the token past any bump that preceded the
  // CAS, so a bump seen here recovered the lock from US).  The caller
  // then drops the fill as Busy.
  //
  // Generation aliasing bound, stated honestly: a uint16 wraps after 2^16
  // force-releases inside ONE stalled critical section.  A dead-holder
  // recovery costs a wait and a liveness probe (single-digit ms), so
  // aliasing needs ~65536 consecutive crash-recoveries — minutes of
  // continuous holder deaths — while the stalled holder never runs: not a
  // realistic schedule, but it is a schedule; the lock-value and owner-PID
  // complements below break it unless the aliased takeover ALSO stored our
  // exact token and re-published our exact PID.
  const bool gen_ok =
      std::atomic_ref<uint16_t>(const_cast<uint16_t &>(_header->write_lock_gen))
          .load(std::memory_order_acquire) == token.generation;
  // Complementary structural checks: the lock byte still holds our token
  // and we are still the registered owner.  Cheap (two more loads on the
  // cold publish/release path) and independent of the generation
  // arithmetic.
  const bool held =
      std::atomic_ref<uint8_t>(const_cast<uint8_t &>(_header->write_lock))
          .load(std::memory_order_acquire) == token.value;
  const bool owned = std::atomic_ref<uint32_t>(
                         const_cast<uint32_t &>(_header->write_lock_owner_pid))
                         .load(std::memory_order_acquire) == self_pid();
  return token.acquired && gen_ok && held && owned;
}

void MmapDirectory::release_write_lock(const WriteLockToken &token) {
  // Ownership-checked release: if a force-release usurped us, the lock now
  // belongs to the usurper.  Storing 0 would free ITS critical section and
  // admit a third writer onto the usurper's reserved range.  Only release
  // when we still hold it, and then only as CASes from our own PID and our
  // own token: a recovery that lands between the revalidate and the
  // release (it bumps the generation, so the usurper's token differs from
  // ours) makes them no-ops instead of freeing the usurper.  (There is
  // deliberately NO unconditional release: it would free whoever holds the
  // lock, reopening exactly that channel.)
  if (!revalidate_write_lock(token)) {
    return;
  }
  uint32_t self = self_pid();
  (void)std::atomic_ref<uint32_t>(_header->write_lock_owner_pid)
      .compare_exchange_strong(self, 0, std::memory_order_relaxed,
                               std::memory_order_relaxed);
  uint8_t expected = token.value;
  (void)std::atomic_ref<uint8_t>(_header->write_lock)
      .compare_exchange_strong(expected, 0, std::memory_order_release,
                               std::memory_order_relaxed);
}

uint32_t MmapDirectory::load_version(size_t bucket_idx) const {
  return std::atomic_ref<uint32_t>(
             const_cast<uint32_t &>(_versions[bucket_idx]))
      .load(std::memory_order_acquire);
}

uint32_t MmapDirectory::bucket_version(const CacheKey &key) const {
  if (_header == nullptr || _num_buckets == 0) {
    return 0;
  }
  return load_version(key.bucket_hash() % _num_buckets);
}

void MmapDirectory::touch_bucket(const CacheKey &key) {
  if (_header == nullptr || _num_buckets == 0) {
    return;
  }
  const size_t bucket_idx = key.bucket_hash() % _num_buckets;
  // Empty writer bracket: +2, fenced and parity-preserving.  A reader inside
  // probe_each's seqlock loop observes the change and retries — a spurious
  // retry, never a wrong serve.
  const uint32_t writer_token = acquire_writer(bucket_idx);
  release_writer(bucket_idx, writer_token);
}

uint32_t MmapDirectory::acquire_writer(size_t bucket_idx) {
  auto ref = std::atomic_ref<uint32_t>(_versions[bucket_idx]);
  // Hot path: the bucket is even (no writer); claim it with CAS even → odd.
  uint32_t v = ref.load(std::memory_order_acquire);
  if ((v & 1) == 0 &&
      ref.compare_exchange_strong(v, v + 1, std::memory_order_acq_rel,
                                  std::memory_order_acquire)) [[likely]] {
    // The fence is load-bearing on weak memory (AArch64): acq_rel on
    // the CAS alone does not order the odd-version STORE before the
    // subsequent plain entry stores, so a reader could observe
    // {even, half-mutated entry, even}.  Canonical seqlocks (Linux
    // write_seqcount_begin) place a barrier here for the same reason.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    TSAN_RELEASE(&_versions[bucket_idx]);
    return v;
  }
  return acquire_writer_slow(bucket_idx);
}

uint32_t MmapDirectory::acquire_writer_slow(size_t bucket_idx) {
  auto ref = std::atomic_ref<uint32_t>(_versions[bucket_idx]);
  // Another writer holds the bucket (odd version).  Every mutator holds its
  // stripe mutex, so that writer is in ANOTHER process -- or another Volume
  // on the same file, e.g. a graceful-reload / overlapped-recycle overlap in
  // which two processes own the stripe (the cross-process write lock is
  // released before the directory insert).  It is dead mid-update
  // (SIGKILL/OOM), or alive and running, or alive but descheduled.
  //
  // Wait it out with LockHolderWait, keyed on the odd version: every new
  // holder takes the bucket at a new odd value, so the budget restarts
  // whenever the holder changes, and a waiter that sat through several
  // short holders never presumes the latest one stuck (issue #27).  Only a
  // holder that kept the bucket odd for kBucketWriterBudget -- far past any
  // scheduling delay measured for a live holder -- is presumed stuck.
  LockHolderWait wait(bucket_writer_budget());
  for (;;) {
    uint32_t v = ref.load(std::memory_order_acquire);
    if ((v & 1) != 0) {
      if (wait.wait(v) == LockHolderWait::Step::kExpired) {
        // Holder presumed STUCK.  Force the version from exactly the odd
        // value we waited on to the next even one (a CAS, so a holder that
        // just released, or a new holder, is never overwritten).  A live
        // usurped holder is harmless afterwards: its release_writer is
        // token-checked and becomes a no-op, so it cannot flip the parity
        // back to odd.
        // AUDIT (write-lock hardening, #27): a usurped holder that is still
        // executing can finish its plain DirEntry stores after the force,
        // so a reader may take a torn or lost entry update in this bucket
        // under a stable even version.  A reader additionally
        // full-key-verifies (invariant 8), position-checks (invariant 9)
        // and checksum-verifies the document an entry points at, so the
        // tear degrades to a retry or a clean miss, never accepted foreign
        // or corrupt bytes.  Presumed-stuck recovery is therefore
        // sufficient here (see acquire_write_lock for the one channel that
        // is NOT reader-detectable).
        if (ref.compare_exchange_strong(v, v + 1, std::memory_order_acq_rel,
                                        std::memory_order_acquire)) {
#ifdef CYCLONE_TEST_SEAMS
          s_bucket_recoveries_for_test.fetch_add(1, std::memory_order_relaxed);
#endif
        }
      }
      continue;
    }
    // Try to claim: CAS even → even+1 (odd).
    if (ref.compare_exchange_weak(v, v + 1, std::memory_order_acq_rel,
                                  std::memory_order_acquire)) {
      // Load-bearing fence: see acquire_writer.
      std::atomic_thread_fence(std::memory_order_seq_cst);
      TSAN_RELEASE(&_versions[bucket_idx]);
      return v;
    }
    // CAS failed (concurrent modification), retry.
  }
}

void MmapDirectory::release_writer(size_t bucket_idx, uint32_t token) {
  // Ensure all entry writes are visible before making the version even.
  std::atomic_thread_fence(std::memory_order_release);
  TSAN_RELEASE(&_versions[bucket_idx]);
  // odd -> even, but only from OUR odd value (token + 1).  If a peer
  // force-released this bucket while we were descheduled inside the
  // bracket, the version has moved on and this is a no-op: a blind +1 would
  // turn the usurper's even version odd again, leaving the bucket "writer
  // active" with no writer until the next forced recovery.
  uint32_t expected = token + 1;
  (void)std::atomic_ref<uint32_t>(_versions[bucket_idx])
      .compare_exchange_strong(expected, token + 2, std::memory_order_release,
                               std::memory_order_relaxed);
}

void MmapDirectory::increment_count() {
  std::atomic_ref<uint32_t>(_header->entry_count)
      .fetch_add(1, std::memory_order_relaxed);
}

void MmapDirectory::decrement_count() {
  std::atomic_ref<uint32_t>(_header->entry_count)
      .fetch_sub(1, std::memory_order_relaxed);
}

uint64_t MmapDirectory::get_shared_write_pos() const {
  if (_header == nullptr) {
    return 0;
  }
  return std::atomic_ref<uint64_t>(
             const_cast<uint64_t &>(_header->shared_write_pos))
      .load(std::memory_order_acquire);
}

void MmapDirectory::set_shared_write_pos(uint64_t pos) {
  if (_header == nullptr) {
    return;
  }
  std::atomic_ref<uint64_t>(_header->shared_write_pos)
      .store(pos, std::memory_order_release);
}

void MmapDirectory::record_shared_wrap(uint64_t now_ns) {
  if (_header == nullptr) {
    return;
  }
  // Caller holds the write lock, so these are serialized with other
  // writers; atomic_ref only guarantees torn-free lock-free reads.
  // seq_cst: the wrap count is one half of the reader's stamp-then-
  // revalidate epoch — publish it with the strongest ordering
  // so a revalidating reader observes it as promptly as possible.
  std::atomic_ref<uint64_t>(_header->shared_wrap_count)
      .fetch_add(1, std::memory_order_seq_cst);
  std::atomic_ref<uint64_t>(_header->shared_last_wrap_time_ns)
      .store(now_ns, std::memory_order_release);
}

uint64_t MmapDirectory::shared_wrap_count() const {
  if (_header == nullptr) {
    return 0;
  }
  return std::atomic_ref<uint64_t>(
             const_cast<uint64_t &>(_header->shared_wrap_count))
      .load(std::memory_order_relaxed);
}

uint64_t MmapDirectory::shared_last_wrap_time_ns() const {
  if (_header == nullptr) {
    return 0;
  }
  return std::atomic_ref<uint64_t>(
             const_cast<uint64_t &>(_header->shared_last_wrap_time_ns))
      .load(std::memory_order_acquire);
}

uint64_t MmapDirectory::lease_expiry_ns() const {
  if (_header == nullptr) {
    return 0;
  }
  return std::atomic_ref<uint64_t>(
             const_cast<uint64_t &>(_header->stripe_lease_expiry_ns))
      .load(std::memory_order_seq_cst);
}

void MmapDirectory::stamp_lease_expiry(uint64_t new_expiry_ns,
                                       uint64_t skip_if_at_least_ns) {
  if (_header == nullptr) {
    return;
  }
  auto ref = std::atomic_ref<uint64_t>(_header->stripe_lease_expiry_ns);
  // CAS-max with write-avoidance: skip when the current expiry already
  // covers now + T - slack.  The initial seq_cst load also provides the
  // reader's side of the Dekker pairing when the CAS is skipped — the
  // visible covering stamp is what defers the writer in that case.
  uint64_t cur = ref.load(std::memory_order_seq_cst);
  while (cur < skip_if_at_least_ns) {
    if (ref.compare_exchange_weak(cur, new_expiry_ns, std::memory_order_seq_cst,
                                  std::memory_order_seq_cst)) {
      return;
    }
  }
}

uint32_t MmapDirectory::chunk_borrow_raw(size_t chunk) const {
  if (_retention == nullptr || chunk >= kMaxChunks) {
    return 0;
  }
  return std::atomic_ref<uint32_t>(
             const_cast<uint32_t &>(_retention->chunk_borrows[chunk]))
      .load(std::memory_order_seq_cst);
}

borrow_slot::Acquired MmapDirectory::chunk_borrow_acquire(size_t chunk) {
  if (_retention == nullptr || chunk >= kMaxChunks) {
    return {};
  }
  auto ref = std::atomic_ref<uint32_t>(_retention->chunk_borrows[chunk]);
  return borrow_slot::acquire(ref);
}

void MmapDirectory::chunk_borrow_release(size_t chunk, uint8_t generation) {
  if (_retention == nullptr || chunk >= kMaxChunks) {
    return;
  }
  auto ref = std::atomic_ref<uint32_t>(_retention->chunk_borrows[chunk]);
  borrow_slot::release(ref, generation);
}

bool MmapDirectory::chunk_borrows_force_reset_all() {
  if (_retention == nullptr) {
    return false;
  }
  bool any = false;
  for (auto &slot : _retention->chunk_borrows) {
    auto ref = std::atomic_ref<uint32_t>(slot);
    any = borrow_slot::force_reset(ref) || any;
  }
  return any;
}

uint64_t MmapDirectory::exposure_gen() const {
  if (_retention == nullptr) {
    return 0;
  }
  return std::atomic_ref<uint64_t>(
             const_cast<uint64_t &>(_retention->exposure_gen))
      .load(std::memory_order_seq_cst);
}

void MmapDirectory::set_exposure_gen(uint64_t gen) {
  if (_retention == nullptr) {
    return;
  }
  std::atomic_ref<uint64_t>(_retention->exposure_gen)
      .store(gen, std::memory_order_seq_cst);
}

bool MmapDirectory::wrap_intent() const {
  if (_header == nullptr) {
    return false;
  }
  return std::atomic_ref<uint8_t>(const_cast<uint8_t &>(_header->wrap_intent))
             .load(std::memory_order_seq_cst) != 0;
}

void MmapDirectory::set_wrap_intent(bool active) {
  set_wrap_intent_value(active ? kIntentStep : 0);
}

uint8_t MmapDirectory::wrap_intent_value() const {
  if (_header == nullptr) {
    return 0;
  }
  return std::atomic_ref<uint8_t>(const_cast<uint8_t &>(_header->wrap_intent))
      .load(std::memory_order_seq_cst);
}

void MmapDirectory::set_wrap_intent_value(uint8_t value) {
  if (_header == nullptr) {
    return;
  }
  std::atomic_ref<uint8_t>(_header->wrap_intent)
      .store(value, std::memory_order_seq_cst);
}

uint32_t MmapDirectory::wrap_deferred_deadline_ms() const {
  if (_header == nullptr) {
    return 0;
  }
  return std::atomic_ref<uint32_t>(
             const_cast<uint32_t &>(_header->shared_wrap_deferred_deadline_ms))
      .load(std::memory_order_seq_cst);
}

void MmapDirectory::set_wrap_deferred_deadline_ms(uint32_t deadline_ms) {
  if (_header == nullptr) {
    return;
  }
  std::atomic_ref<uint32_t>(_header->shared_wrap_deferred_deadline_ms)
      .store(deadline_ms, std::memory_order_seq_cst);
}

}  // namespace cyclone
