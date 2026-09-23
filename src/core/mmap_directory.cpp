// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "mmap_directory.hpp"

#include <chrono>
#include <cstring>
#include <thread>

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

/// One CPU pause hint for the short-critical-section spin.
inline void cpu_pause() {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
  _mm_pause();
#elif defined(__x86_64__)
  __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(_M_ARM64)
  __asm__ volatile("yield" ::: "memory");
#endif
}

/// Bounded exponential backoff (µs, capped) between liveness escalations
/// while WAITING on a live holder — yields the core instead of burning it.
void backoff_sleep(uint32_t attempt) {
  uint32_t us = attempt >= 10 ? 1024U : (1U << attempt);
  std::this_thread::sleep_for(std::chrono::microseconds(us));
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
  header->wrap_intent = 0;         // 0 = no wrap in flight
  header->stripe_borrow_slot = 0;  // {generation 0, count 0}
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

  // Retry loop for torn read detection
  for (size_t retry = 0; retry < kMaxReadRetries; ++retry) {
    // Wait for an even version before starting the scan.
    uint32_t version_before = load_version(bucket_idx);
    if ((version_before & 1) != 0) {
      for (size_t spin = 0; spin < kMaxWriterWaitSpins; ++spin) {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
        _mm_pause();
#elif defined(__x86_64__)
        __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(_M_ARM64)
        __asm__ volatile("yield" ::: "memory");
#endif
        version_before = load_version(bucket_idx);
        if ((version_before & 1) == 0) break;
      }
      if ((version_before & 1) != 0) {
        std::this_thread::yield();
        continue;
      }
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
  }

  // Exhausted retries - treat as not found
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
  acquire_phase_lock();
  bool cur_phase = current_phase();

  // Acquire writer lock (CAS even → odd, spins if another writer holds it).
  acquire_writer(bucket_idx);

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
    release_writer(bucket_idx);
    release_phase_lock();
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
  release_writer(bucket_idx);

  // Release phase lock after entry is written.
  release_phase_lock();

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
  acquire_writer(bucket_idx);

  bool found = false;
  for (size_t i = 0; i < kEntriesPerBucket; ++i) {
    if (!bucket[i].is_empty() && bucket[i].tag() == target_tag) {
      bucket[i].clear();
      found = true;
      break;
    }
  }

  // Release writer lock (odd → even).
  release_writer(bucket_idx);

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

  acquire_writer(bucket_idx);

  // EVERY entry with this tag at this offset goes (see Directory::remove_at).
  size_t removed = 0;
  for (size_t i = 0; i < kEntriesPerBucket; ++i) {
    if (!bucket[i].is_empty() && bucket[i].tag() == target_tag &&
        bucket[i].offset() == target_offset) {
      bucket[i].clear();
      ++removed;
    }
  }

  release_writer(bucket_idx);

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
  for (size_t i = 0; i < _num_buckets; ++i) {
    acquire_writer(i);
  }

  // Clear all entries and reset count while holding all locks.
  std::memset(_entries, 0, _num_buckets * kEntriesPerBucket * sizeof(DirEntry));
  std::atomic_ref<uint32_t>(_header->entry_count)
      .store(0, std::memory_order_relaxed);

  // Release all writer locks.
  for (size_t i = 0; i < _num_buckets; ++i) {
    release_writer(i);
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
  acquire_phase_lock();
  std::atomic_ref<uint8_t>(_header->current_phase)
      .fetch_xor(1, std::memory_order_seq_cst);
  release_phase_lock();
}

void MmapDirectory::set_current_phase(bool phase) {
  if (_header == nullptr) {
    return;
  }
  // Same lock and ordering as toggle_phase: an insert that read the phase
  // under phase_lock must not have it change underneath its entry store.
  acquire_phase_lock();
  std::atomic_ref<uint8_t>(_header->current_phase)
      .store(phase ? 1 : 0, std::memory_order_seq_cst);
  release_phase_lock();
}

void MmapDirectory::reset_reader_state_exclusive() {
  if (_header == nullptr) {
    return;
  }
  std::atomic_ref<uint16_t>(_header->stripe_borrow_slot)
      .store(0, std::memory_order_seq_cst);  // retired in v2; kept zero
  for (auto &slot : _retention->chunk_borrows) {
    std::atomic_ref<uint32_t>(slot).store(0, std::memory_order_seq_cst);
  }
  std::atomic_ref<uint64_t>(_header->stripe_lease_expiry_ns)
      .store(0, std::memory_order_seq_cst);
  std::atomic_ref<uint32_t>(_header->shared_wrap_deferred_deadline_ms)
      .store(0, std::memory_order_seq_cst);
}

void MmapDirectory::acquire_phase_lock() {
  auto ref = std::atomic_ref<uint8_t>(_header->phase_lock);
  // Bounded spin: if the holder crashed (SIGKILL) while holding the lock,
  // force-release after kMaxSpinIterations.  The critical section is a
  // single XOR (toggle_phase) or read+scan+write (insert), so exceeding
  // this limit almost certainly means the holder is dead.
  constexpr int kMaxSpinIterations = 100000;
  for (int spin = 0; spin < kMaxSpinIterations; ++spin) {
    uint8_t expected = 0;
    if (ref.compare_exchange_weak(expected, 1, std::memory_order_acquire,
                                  std::memory_order_relaxed)) {
      return;
    }
    // Spin hint for the short critical section
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_pause();
#elif defined(__x86_64__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(_M_ARM64)
    __asm__ volatile("yield" ::: "memory");
#endif
  }
  // Holder presumed dead — force-release and acquire.
  // AUDIT (write-lock hardening): unlike the write lock, a usurped
  // phase_lock holder cannot leak undetectable corruption.  Its critical
  // section races a phase toggle against an insert; any resulting
  // inconsistency is caught by the reader's stamp-then-revalidate epoch
  // ({wrap_count, phase}) plus the per-bucket seqlock and document
  // checksum, degrading to a reader retry or a clean miss — never accepted
  // corrupt bytes.  So this lock stays on the cheap presume-dead recovery
  // (a PID-carrying liveness proof would need header space the 64-byte
  // on-disk layout no longer has).
  ref.store(0, std::memory_order_release);
  // Re-acquire cleanly.
  uint8_t expected = 0;
  if (!ref.compare_exchange_strong(expected, 1, std::memory_order_acquire,
                                   std::memory_order_relaxed)) {
    // Another thread raced us on the force-release; retry with bounded spin.
    for (int spin = 0; spin < kMaxSpinIterations; ++spin) {
      expected = 0;
      if (ref.compare_exchange_weak(expected, 1, std::memory_order_acquire,
                                    std::memory_order_relaxed)) {
        return;
      }
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
      _mm_pause();
#elif defined(__x86_64__)
      __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(_M_ARM64)
      __asm__ volatile("yield" ::: "memory");
#endif
    }
    // Last resort: unconditional take-over.
    ref.exchange(1, std::memory_order_acquire);
  }
}

void MmapDirectory::release_phase_lock() {
  std::atomic_ref<uint8_t>(_header->phase_lock)
      .store(0, std::memory_order_release);
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
  // The residual mis-order is the bump landing BETWEEN sample and CAS with
  // us as the legitimate post-bump acquirer: the token is then stale and
  // every gate fails — a spurious Busy (safe direction; see revalidate).

  // Hot uncontended path: one generation load + a single CAS.
  token.generation = gen.load(std::memory_order_seq_cst);
  uint8_t expected = 0;
  if (lock.compare_exchange_strong(expected, 1, std::memory_order_seq_cst,
                                   std::memory_order_relaxed)) {
    owner.store(self_pid(), std::memory_order_relaxed);
    token.acquired = true;
    return token;
  }

  // Contended path.  Spin the short critical section out; only on spin
  // exhaustion do we consult liveness.  Recovery from a genuinely dead
  // holder is preserved (a crash must not deadlock the cache); usurpation of
  // a live-but-stalled holder is removed.
  constexpr int kMaxSpinIterations = 100000;
  // Last-resort deadlock breaker for the pathological case where kill(2)
  // keeps reporting the holder alive but it never releases (PID reuse, or a
  // live holder wedged for many seconds).  If this ever fires on a truly
  // live holder, the generation bump below makes that holder's revalidate
  // fail, so it aborts before its overlapping publish/pwrite — corruption
  // stays closed; only availability degrades.
  constexpr uint32_t kMaxLiveWaitEscalations = 4096;
  for (;;) {
    for (int spin = 0; spin < kMaxSpinIterations; ++spin) {
      // Same pre-CAS sample discipline as the fast path (see above).
      token.generation = gen.load(std::memory_order_seq_cst);
      expected = 0;
      if (lock.compare_exchange_weak(expected, 1, std::memory_order_seq_cst,
                                     std::memory_order_relaxed)) {
        owner.store(self_pid(), std::memory_order_relaxed);
        token.acquired = true;
        return token;
      }
      cpu_pause();
    }

    // Spin budget exhausted while the lock is held.  Prove DEAD before we
    // recover, or WAIT.
    ++token.live_waits;
    const uint32_t holder = owner.load(std::memory_order_acquire);
    const bool presume_dead =
        s_write_lock_presume_dead_for_test.load(std::memory_order_relaxed);
    const bool dead = presume_dead || holder_is_dead(holder);
    // Test seam: a nonzero override shrinks the escalation budget so the
    // last-resort takeover is reachable in test time (see the F6-F test).
    const uint32_t max_live_waits_override =
        s_write_lock_max_live_waits_for_test.load(std::memory_order_relaxed);
    const bool escalation_exhausted =
        token.live_waits >= (max_live_waits_override != 0
                                 ? max_live_waits_override
                                 : kMaxLiveWaitEscalations);

    if (dead || escalation_exhausted) {
      // Recover the lock.  Bump the generation FIRST (seq_cst) so a holder
      // that is (pathologically) still live sees the takeover at its
      // revalidate and aborts before publishing shared_write_pos / pwriting.
      gen.fetch_add(1, std::memory_order_seq_cst);
      owner.store(0, std::memory_order_relaxed);
      lock.store(0, std::memory_order_release);
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
    } else {
      // Live holder (or an owner not yet published): WAIT, never usurp.
      backoff_sleep(token.live_waits);
    }
  }
}

MmapDirectory::WriteLockToken MmapDirectory::try_acquire_write_lock() {
  // Non-blocking counterpart of acquire_write_lock for the two in-place
  // header RMW sites (update_hit_count_sync / remove_alternate_sync's chain
  // repoint).  It is EXACTLY acquire_write_lock's uncontended fast path -- one
  // pre-CAS generation sample and a single CAS -- with no spin, liveness
  // probe, or usurpation.  On contention it returns {acquired=false} so the
  // caller never blocks a request thread behind a peer's pwrite+fsync.
  //
  // It inherits the blocking path's accepted-leak policy: if a force-release
  // bumps the generation between the sample and the winning CAS, the token is
  // stale, the caller's revalidate fails, and release_write_lock (revalidate-
  // gated) no-ops, leaking the lock byte until the next waiter's escalation
  // recovers it.  That needs a force-release (a proven-dead holder or a
  // multi-second escalation) to land in the few instructions between the
  // sample and the CAS, so it is rare and self-healing.  An earlier draft
  // tried to self-release in that case, but it keyed on write_lock_owner_pid,
  // which is written relaxed, so the ownership check was unsound on weak
  // memory and could clear a lock a concurrent escalation had already handed
  // to another writer -- the exact hazard release_write_lock's checked release
  // avoids.  Dropped in favour of the same rare leak the blocking path takes.
  auto lock = std::atomic_ref<uint8_t>(_header->write_lock);
  auto gen = std::atomic_ref<uint16_t>(_header->write_lock_gen);
  auto owner = std::atomic_ref<uint32_t>(_header->write_lock_owner_pid);

  WriteLockToken token;
  token.generation = gen.load(std::memory_order_seq_cst);  // pre-CAS sample
  uint8_t expected = 0;
  if (!lock.compare_exchange_strong(expected, 1, std::memory_order_seq_cst,
                                    std::memory_order_relaxed)) {
    return token;  // Held by someone else — do not wait, do not usurp.
  }
  owner.store(self_pid(), std::memory_order_relaxed);
  token.acquired = true;
  return token;
}

bool MmapDirectory::revalidate_write_lock(const WriteLockToken &token) const {
  // Primary check: no force-release bumped the takeover generation since
  // this acquisition SAMPLED it (the sample precedes the acquire CAS, so a
  // bump racing the acquisition also invalidates the token — erring toward
  // a safe spurious abort; the caller drops the fill as Busy and the lock,
  // in that vanishing case still legitimately ours, is recovered by the
  // next waiter's escalation: an availability blip, never corruption).
  //
  // Generation aliasing bound, stated honestly: a uint16 wraps after 2^16
  // force-releases inside ONE stalled critical section.  A dead-holder
  // recovery costs only a spin budget + liveness probe (single-digit ms),
  // so aliasing needs ~65536 consecutive crash-recoveries — minutes of
  // continuous holder deaths — while the stalled holder never runs: not a
  // realistic schedule, but it is a schedule; the owner-PID complement
  // below (self must still be the registered holder) breaks it unless the
  // aliased takeover ALSO re-published our exact PID.
  const bool gen_ok =
      std::atomic_ref<uint16_t>(const_cast<uint16_t &>(_header->write_lock_gen))
          .load(std::memory_order_acquire) == token.generation;
  // Complementary structural checks: the lock byte is still held and we are
  // still the registered owner.  Cheap (two more loads on the cold
  // publish/release path) and independent of the generation arithmetic.
  const bool held =
      std::atomic_ref<uint8_t>(const_cast<uint8_t &>(_header->write_lock))
          .load(std::memory_order_acquire) != 0;
  const bool owned = std::atomic_ref<uint32_t>(
                         const_cast<uint32_t &>(_header->write_lock_owner_pid))
                         .load(std::memory_order_acquire) == self_pid();
  return token.acquired && gen_ok && held && owned;
}

void MmapDirectory::release_write_lock(const WriteLockToken &token) {
  // Ownership-checked release: if a force-release usurped us, the lock now
  // belongs to the usurper.  Storing 0 would free ITS critical section and
  // admit a third writer onto the usurper's reserved range.  Only release
  // when we still hold it.  (There is deliberately NO unconditional release:
  // it would free whoever holds the lock, reopening exactly that channel.)
  if (!revalidate_write_lock(token)) {
    return;
  }
  std::atomic_ref<uint32_t>(_header->write_lock_owner_pid)
      .store(0, std::memory_order_relaxed);
  std::atomic_ref<uint8_t>(_header->write_lock)
      .store(0, std::memory_order_release);
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
  acquire_writer(bucket_idx);
  release_writer(bucket_idx);
}

uint32_t MmapDirectory::acquire_writer(size_t bucket_idx) {
  auto ref = std::atomic_ref<uint32_t>(_versions[bucket_idx]);
  // Bounded spin: if a writer crashed (SIGKILL/OOM) while holding the lock
  // (version is odd), force-release after kMaxSpinIterations to prevent
  // permanent livelock.  This mirrors the pattern in acquire_phase_lock().
  constexpr int kMaxSpinIterations = 100000;
  for (int spin = 0;; ++spin) {
    uint32_t v = ref.load(std::memory_order_acquire);
    if ((v & 1) != 0) {
      // Another writer holds the lock (odd version).
      if (spin >= kMaxSpinIterations) {
        // Holder presumed dead.  Force to next even value to clear the lock.
        // AUDIT (write-lock hardening): a usurped per-bucket writer produces
        // a torn DirEntry, but the force-to-even is itself a seqlock version
        // change, and a reader additionally full-key-verifies and checksum-
        // verifies the document it points at — so the tear degrades to a
        // retry or a clean miss, never accepted corrupt bytes.  Presume-dead
        // recovery is therefore sufficient here (see acquire_write_lock for
        // the one channel that is NOT reader-detectable).
        uint32_t next_even = (v | 1) + 1;
        ref.store(next_even, std::memory_order_release);
        spin = 0;  // Restart acquisition with the new even version
        continue;
      }
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
      _mm_pause();
#elif defined(__x86_64__)
      __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(_M_ARM64)
      __asm__ volatile("isb" ::: "memory");
#endif
      continue;
    }
    // Try to claim: CAS even → even+1 (odd).
    if (ref.compare_exchange_weak(v, v + 1, std::memory_order_acq_rel,
                                  std::memory_order_acquire)) {
      // The fence is load-bearing on weak memory (AArch64): acq_rel on
      // the CAS alone does not order the odd-version STORE before the
      // subsequent plain entry stores, so a reader could observe
      // {even, half-mutated entry, even}.  Canonical seqlocks (Linux
      // write_seqcount_begin) place a barrier here for the same reason.
      std::atomic_thread_fence(std::memory_order_seq_cst);
      TSAN_RELEASE(&_versions[bucket_idx]);
      return v;
    }
    // CAS failed (concurrent modification), retry.
  }
}

void MmapDirectory::release_writer(size_t bucket_idx) {
  // Ensure all entry writes are visible before making the version even.
  std::atomic_thread_fence(std::memory_order_release);
  TSAN_RELEASE(&_versions[bucket_idx]);
  std::atomic_ref<uint32_t>(_versions[bucket_idx])
      .fetch_add(1, std::memory_order_release);
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
