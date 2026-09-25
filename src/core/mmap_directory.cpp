// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "mmap_directory.hpp"

#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <immintrin.h>
#endif

#if defined(_WIN32)
#include <io.h>
#include <process.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#endif

// TSan annotations are defined in mmap_directory.hpp
// (CYCLONE_TSAN_ACQUIRE/RELEASE). Local aliases for brevity.
#define TSAN_ACQUIRE(addr) CYCLONE_TSAN_ACQUIRE(addr)
#define TSAN_RELEASE(addr) CYCLONE_TSAN_RELEASE(addr)

namespace cyclone {

namespace {

/// This process's PID as the value published into write_lock_owner_pid.
uint32_t self_pid() { return WriterLiveness::current_pid(); }

#if defined(_WIN32)
OVERLAPPED slot_overlapped(unsigned slot) {
  const uint64_t byte = WriterLiveness::kLockBase + slot;
  OVERLAPPED ov{};
  ov.Offset = static_cast<DWORD>(byte & 0xFFFFFFFFULL);
  ov.OffsetHigh = static_cast<DWORD>(byte >> 32);
  return ov;
}
#endif

// Cross-process token locks (phase_lock, write_lock), each a lock byte plus
// a 16-bit acquisition generation.  The byte is 0 when free; a holder
// stores a token derived from the generation it bumped the counter to, so
//   - every acquisition and every recovery bumps the generation: a waiter
//     that sees it change knows the lock changed hands, however briefly it
//     was free (LockHolderWait's per-holder budget), and a holder whose
//     generation is no longer current knows it was usurped;
//   - a usurped holder's late release (a CAS from its token to 0, behind a
//     check that its generation is still current) cannot free the next
//     holder unless both the generation (65536 values) and the token (253
//     values) alias.
// Any nonzero value means held, exactly as for pre-#27 builds of the same
// format (CAS 0 -> 1, store 0), so both builds still exclude each other.
// Tokens are 1..253; 254 and 255 mark a lock being recovered (the claim
// alternates so that two waiters recovering the same stuck value -- only
// possible when a previous recoverer died mid-recovery -- cannot both win).
constexpr uint8_t kLockRecoveringA = 254;
constexpr uint8_t kLockRecoveringB = 255;

constexpr uint8_t lock_token(uint16_t generation) {
  return static_cast<uint8_t>(1 + generation % 253);
}

// The token a write-lock holder that holds WriterLiveness slot `slot` stores
// instead (issue #32), so a waiter can find the slot's lock.  The values
// 2..253 form a ring of 252; for each generation, lock_token(generation) --
// what holders without a slot and every build before #32 store -- takes one
// position of it (or none, when it is 1), and the kSlots = 251 slot tokens
// take the positions after it.  So a slot token never equals the plain token
// of the same generation, and it is never 1 (the only value builds before
// #27 store), 0 (free) or a recovery claim (254, 255).  A waiter decodes a
// slot only from a slot token of the generation it read: a holder that took
// no slot, or an older build's, is never proven dead by the absence of a
// lock it never took.
constexpr unsigned kTokenRing = 252;
static_assert(WriterLiveness::kSlots == kTokenRing - 1,
              "every ring position but the plain token's is a slot");

constexpr unsigned plain_ring_index(uint16_t generation) {
  // lock_token 2..253 -> 0..251; lock_token 1 (outside the ring) -> 251.
  return (lock_token(generation) + kTokenRing - 2) % kTokenRing;
}

constexpr uint8_t slot_lock_token(uint16_t generation, unsigned slot) {
  return static_cast<uint8_t>(2 + (plain_ring_index(generation) + 1 + slot) %
                                      kTokenRing);
}

// The slot a lock value encodes for `generation`, or kNoSlot.
constexpr int slot_of_token(uint8_t value, uint16_t generation) {
  if (value < 2 || value > 253) {
    return WriterLiveness::kNoSlot;
  }
  const unsigned slot =
      (value - 2U + 2 * kTokenRing - plain_ring_index(generation) - 1) %
      kTokenRing;
  return slot < WriterLiveness::kSlots ? static_cast<int>(slot)
                                       : WriterLiveness::kNoSlot;
}

// Tokens depend on the generation only through generation % 253.  Checked
// exhaustively at run time (test_write_lock_liveness.cpp); here, every
// generation at the edge slots and every slot at the edge generations.
constexpr bool slot_token_ok(uint16_t gen, unsigned slot) {
  const uint8_t v = slot_lock_token(gen, slot);
  return v >= 2 && v <= 253 && v != lock_token(gen) &&
         slot_of_token(v, gen) == static_cast<int>(slot);
}
constexpr bool slot_tokens_are_distinct() {
  for (uint32_t g = 0; g < 253; ++g) {
    const auto gen = static_cast<uint16_t>(g);
    if (slot_of_token(lock_token(gen), gen) != WriterLiveness::kNoSlot ||
        slot_of_token(1, gen) != WriterLiveness::kNoSlot ||
        slot_of_token(0, gen) != WriterLiveness::kNoSlot ||
        slot_of_token(254, gen) != WriterLiveness::kNoSlot ||
        slot_of_token(255, gen) != WriterLiveness::kNoSlot) {
      return false;
    }
    for (unsigned slot : {0U, 1U, 125U, WriterLiveness::kSlots - 1}) {
      if (!slot_token_ok(gen, slot)) {
        return false;
      }
    }
  }
  for (uint32_t g : {0U, 1U, 251U, 252U, 65535U}) {
    for (unsigned slot = 0; slot < WriterLiveness::kSlots; ++slot) {
      if (!slot_token_ok(static_cast<uint16_t>(g), slot)) {
        return false;
      }
    }
  }
  return true;
}
static_assert(slot_tokens_are_distinct(),
              "a slot token must decode to its slot and never equal the plain "
              "token of its generation, 0, 1 or a recovery claim");

constexpr uint8_t recovery_claim(uint8_t observed) {
  return observed == kLockRecoveringA ? kLockRecoveringB : kLockRecoveringA;
}

// The holder identity a waiter watches: token, generation and (write lock
// only) owner PID.
constexpr uint64_t token_lock_holder(uint8_t value, uint16_t generation,
                                     uint32_t pid) {
  return (uint64_t{value} << 48) | (uint64_t{generation} << 32) | pid;
}

// One acquisition attempt.  CAS the byte 0 -> the token of the generation we
// expect to claim, then claim the generation (seq_cst fetch_add).  On
// success `generation` and `value` are this holder's; on failure the lock
// is not ours and `observed` is the value seen.
//
// While the byte is ours, only a RECOVERY of the lock from us can bump the
// generation (every other acquirer needs the byte free first).  So the
// generation is re-read right after the winning CAS, and the claim must
// land exactly one past it: if it does not, a recovery took the lock from
// us between that read and our bump -- after a stall past the whole
// budget -- and the lock is reported lost WITHOUT touching the byte.  That
// check is load-bearing: tokens recycle every 253 generations, so the
// byte's value alone cannot tell us apart from a later holder whose token
// happens to match ours, and moving (or keeping) the byte on that basis
// would let both believe they hold the lock.  Bumps that landed between
// the pre-CAS sample and the CAS (a stale sample: other holders acquired
// and released before our CAS) are harmless; we then only move the byte
// from the provisional token to the claimed generation's, which is safe
// because no recovery intervened.  The residual is a recovery landing in
// the two instructions between the CAS and the re-read: it needs a waiter
// whose budget and 2 ms confirmation window, spent watching an identical
// holder value, expire in exactly that window.
// A `slot` (write lock of a process holding a WriterLiveness slot only)
// selects slot_lock_token over lock_token.
bool try_take_token_lock(std::atomic_ref<uint8_t> lock,
                         std::atomic_ref<uint16_t> gen, uint16_t &generation,
                         uint8_t &value, uint8_t &observed,
                         int slot = WriterLiveness::kNoSlot) {
  const auto token_of = [slot](uint16_t g) {
    return slot < 0 ? lock_token(g)
                    : slot_lock_token(g, static_cast<unsigned>(slot));
  };
  const auto expect =
      static_cast<uint16_t>(gen.load(std::memory_order_relaxed) + 1);
  uint8_t held = token_of(expect);
  uint8_t expected = 0;
  if (!lock.compare_exchange_strong(expected, held, std::memory_order_seq_cst,
                                    std::memory_order_relaxed)) {
    observed = expected;
    return false;
  }
  const uint16_t after_cas = gen.load(std::memory_order_seq_cst);
  const auto claimed =
      static_cast<uint16_t>(gen.fetch_add(1, std::memory_order_seq_cst) + 1);
  if (claimed != static_cast<uint16_t>(after_cas + 1)) {
    observed = lock.load(std::memory_order_relaxed);  // Recovered from us
    return false;
  }
  if (token_of(claimed) != held) {
    uint8_t current = held;
    if (!lock.compare_exchange_strong(current, token_of(claimed),
                                      std::memory_order_seq_cst,
                                      std::memory_order_relaxed)) {
      observed = current;  // Defensive: cannot happen without a recovery
      return false;
    }
    held = token_of(claimed);
  }
  generation = claimed;
  value = held;
  return true;
}

// Recover a token lock from a holder seen as {stuck, stuck_gen} for the
// whole budget: only if that is still the holder, claim its byte (a CAS, so
// a holder that just released, or a new holder, is never touched), retire
// its generation, clear the owner PID (write lock), then free the lock.
bool recover_token_lock(std::atomic_ref<uint8_t> lock,
                        std::atomic_ref<uint16_t> gen, uint8_t stuck,
                        uint16_t stuck_gen, std::atomic_ref<uint32_t> *owner) {
  if (gen.load(std::memory_order_seq_cst) != stuck_gen) {
    return false;
  }
  uint8_t expected = stuck;
  if (!lock.compare_exchange_strong(expected, recovery_claim(stuck),
                                    std::memory_order_seq_cst,
                                    std::memory_order_relaxed)) {
    return false;
  }
  gen.fetch_add(1, std::memory_order_seq_cst);
  if (owner != nullptr) {
    owner->store(0, std::memory_order_relaxed);
  }
  lock.store(0, std::memory_order_seq_cst);
  return true;
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

// The changing-holder cap of a capped wait (LockHolderWait), or zero.
std::chrono::steady_clock::duration lock_wait_cap(bool capped) {
  if (!capped) {
    return std::chrono::steady_clock::duration::zero();
  }
#ifdef CYCLONE_TEST_SEAMS
  const uint64_t us = MmapDirectory::s_lock_wait_cap_us_for_test.load(
      std::memory_order_relaxed);
  if (us != 0) {
    return std::chrono::microseconds(us);
  }
#endif
  return MmapDirectory::kLockWaitCap;
}

void count_give_up() {
#ifdef CYCLONE_TEST_SEAMS
  MmapDirectory::s_lock_give_ups_for_test.fetch_add(1,
                                                    std::memory_order_relaxed);
#endif
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

// --- WriterLiveness (issue #32) --------------------------------------------

namespace {

// How many slots a claim may use.
unsigned claimable_slots() {
#ifdef CYCLONE_TEST_SEAMS
  const unsigned n =
      WriterLiveness::s_slots_for_test.load(std::memory_order_relaxed);
  if (n != 0) {
    return n < WriterLiveness::kSlots ? n : WriterLiveness::kSlots;
  }
#endif
  return WriterLiveness::kSlots;
}

// Where a claim starts looking: a pseudo-random slot, so that a slot freed
// by a crash is rarely re-claimed before a waiter has probed it.
unsigned first_slot_to_try(uint32_t pid, unsigned slots) {
#ifdef CYCLONE_TEST_SEAMS
  if (WriterLiveness::s_slots_for_test.load(std::memory_order_relaxed) != 0) {
    return 0;
  }
#endif
  const auto now = static_cast<uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  uint64_t x = (uint64_t{pid} << 32) ^ now;
  x ^= x >> 33;
  x *= 0xFF51AFD7ED558CCDULL;
  x ^= x >> 33;
  return static_cast<unsigned>(x % slots);
}

#if !defined(_WIN32) && defined(F_OFD_GETLK)
// Is a lock held on `slot` through an open file description other than
// fd's?  1 yes, 0 no, -1 the query failed.
int slot_locked_elsewhere(int fd, unsigned slot) {
  struct flock fl{};
  fl.l_type = F_WRLCK;
  fl.l_whence = SEEK_SET;
  fl.l_start = static_cast<off_t>(WriterLiveness::kLockBase + slot);
  fl.l_len = 1;
  fl.l_pid = 0;
  int rc = 0;
  do {
    rc = ::fcntl(fd, F_OFD_GETLK, &fl);
  } while (rc != 0 && errno == EINTR);
  if (rc != 0) {
    return -1;
  }
  return fl.l_type == F_UNLCK ? 0 : 1;
}

bool set_slot_lock(int fd, unsigned slot, short type) {
  struct flock fl{};
  fl.l_type = type;
  fl.l_whence = SEEK_SET;
  fl.l_start = static_cast<off_t>(WriterLiveness::kLockBase + slot);
  fl.l_len = 1;
  int rc = 0;
  do {
    rc = ::fcntl(fd, F_OFD_SETLK, &fl);
  } while (rc != 0 && errno == EINTR);
  return rc == 0;
}
#endif

// Slot offsets go to fcntl as off_t: a 32-bit off_t would truncate them
// into the data range (same guard as the reset-gate bytes in volume.cpp).
#if !defined(_WIN32)
static_assert(sizeof(off_t) == 8,
              "liveness slot offsets need a 64-bit off_t "
              "(_FILE_OFFSET_BITS=64 on 32-bit targets)");
#endif

// Bumped in every forked child (pthread_atfork), so a WriterLiveness can
// tell that it runs in a new process whatever PIDs say.  Windows has no
// fork: it stays 0 there.
std::atomic<uint32_t> g_fork_epoch{0};

// ONE mutex for every WriterLiveness of the process: claim, probe and detach
// run under it.  It is fork-safe by construction: the pthread_atfork
// PREPARE handler takes it, so no other thread can hold it at the instant of
// fork(), and the parent and child handlers release it -- a child never
// inherits it locked.  (A per-object mutex held by another thread across
// fork() used to hang the child's first write-lock acquisition forever.)
// Everything done under it is non-blocking by design: F_OFD_GETLK,
// F_OFD_SETLK (never SETLKW), LockFileEx with LOCKFILE_FAIL_IMMEDIATELY,
// open/fstat/close of the volume file.  So a fork waits in PREPARE only for
// a few syscalls -- on a hung network filesystem, as long as that open()
// takes.  It is taken only on cold paths (claims, and probes after 50 ms
// behind one holder), so sharing it between volumes costs nothing on the
// hot path.
std::mutex &liveness_mutex() {
  static std::mutex mu;
  return mu;
}

#if !defined(_WIN32)
void on_fork_prepare() { liveness_mutex().lock(); }
void on_fork_parent() { liveness_mutex().unlock(); }
void on_fork_child() {
  g_fork_epoch.fetch_add(1, std::memory_order_relaxed);
  liveness_mutex().unlock();  // Taken by this thread in on_fork_prepare
}
#endif

void install_fork_hook() {
#if !defined(_WIN32)
  static const bool installed = [] {
    return ::pthread_atfork(&on_fork_prepare, &on_fork_parent,
                            &on_fork_child) == 0;
  }();
  (void)installed;
#endif
}

}  // namespace

uint32_t WriterLiveness::current_pid() {
#ifdef CYCLONE_TEST_SEAMS
  const uint32_t pid = s_pid_override_for_test.load(std::memory_order_relaxed);
  if (pid != 0) {
    return pid;
  }
#endif
#if defined(_WIN32)
  return static_cast<uint32_t>(::GetCurrentProcessId());
#else
  return static_cast<uint32_t>(::getpid());
#endif
}

void WriterLiveness::attach(int probe_fd, const std::string &path) {
  install_fork_hook();
  detach();
  std::lock_guard<std::mutex> lock(liveness_mutex());
  _probe_fd = probe_fd;
  _path = path;
  (void)claim_locked(g_fork_epoch.load(std::memory_order_relaxed));
}

void WriterLiveness::detach() {
  std::lock_guard<std::mutex> lock(liveness_mutex());
  const uint64_t state = _state.load(std::memory_order_relaxed);
#if defined(_WIN32)
  // The slot lock is on the volume handle itself; the caller closes that
  // handle after us, which would release it too.
  if ((state & kClaimed) != 0 && _probe_fd >= 0) {
    HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(_probe_fd));
    if (handle != INVALID_HANDLE_VALUE) {
      OVERLAPPED ov =
          slot_overlapped(static_cast<unsigned>((state >> kSlotShift) & 0xFFU));
      (void)::UnlockFileEx(handle, 0, 1, 0, &ov);
    }
  }
#else
  (void)state;
  if (_claim_fd >= 0) {
    ::close(_claim_fd);  // Releases the slot (last close of its OFD)
    _claim_fd = -1;
  }
#endif
  _probe_fd = -1;
  _path.clear();
  _state.store(0, std::memory_order_release);
}

namespace {

constexpr uint64_t kSlotMask = 0xFF;

}  // namespace

int WriterLiveness::slot_of() const {
  const uint64_t state = _state.load(std::memory_order_acquire);
  if (static_cast<uint32_t>(state) !=
          g_fork_epoch.load(std::memory_order_relaxed) ||
      (state & kClaimed) == 0) {
    return kNoSlot;
  }
  return static_cast<int>((state >> kSlotShift) & kSlotMask);
}

int WriterLiveness::slot_for() {
  const uint32_t epoch = g_fork_epoch.load(std::memory_order_relaxed);
  const auto known = [epoch](uint64_t state) {
    return static_cast<uint32_t>(state) == epoch &&
           (state & (kClaimed | kFailed)) != 0;
  };
  const auto slot_in = [](uint64_t state) {
    return (state & kClaimed) != 0
               ? static_cast<int>((state >> kSlotShift) & kSlotMask)
               : kNoSlot;
  };
  const uint64_t state = _state.load(std::memory_order_acquire);
  if (known(state)) {
    return slot_in(state);
  }
  // First acquisition in a forked child (or nothing is attached).
  std::lock_guard<std::mutex> lock(liveness_mutex());
  const uint64_t again = _state.load(std::memory_order_relaxed);
  if (known(again)) {
    return slot_in(again);
  }
  if (_probe_fd < 0) {
    return kNoSlot;
  }
  (void)claim_locked(epoch);
  return slot_in(_state.load(std::memory_order_relaxed));
}

bool WriterLiveness::claim_locked(uint32_t fork_epoch) {
  int claimed = kNoSlot;
#ifdef CYCLONE_TEST_SEAMS
  const bool fail = s_claims_fail_for_test.load(std::memory_order_relaxed);
#else
  constexpr bool fail = false;
#endif
  const unsigned slots = claimable_slots();
  const unsigned first = first_slot_to_try(current_pid(), slots);
#if defined(_WIN32)
  // No fork on Windows, so the slot lock can live on the volume handle: it
  // is this process's alone, and closing the handle (or exiting) releases
  // it.  An exclusive lock makes the claim atomic; a slot held by a live
  // process, or probed by a waiter at this very moment, is skipped.
  HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(_probe_fd));
  if (!fail && handle != INVALID_HANDLE_VALUE) {
    for (unsigned i = 0; i < slots && claimed < 0; ++i) {
      const unsigned slot = (first + i) % slots;
      OVERLAPPED ov = slot_overlapped(slot);
      if (::LockFileEx(handle,
                       LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0,
                       1, 0, &ov) != 0) {
        claimed = static_cast<int>(slot);
      } else if (::GetLastError() != ERROR_LOCK_VIOLATION) {
        break;  // No byte-range locks here: stay without a slot
      }
    }
  }
#elif defined(F_OFD_SETLK) && defined(F_OFD_GETLK)
  // A descriptor of our own, not the volume fd, for two reasons.  A forked
  // child shares its parent's descriptors and their OFD locks, so a lock
  // taken through one of them would last as long as ANY process of the
  // family: a dead child would look alive.  And the volume fd is what
  // probe() queries, and a query never reports a lock held through the
  // querying OFD itself.
  //
  // Read-only, so that a worker that dropped the privileges its master
  // opened the volume with can still claim.  A read-only descriptor can
  // take only a shared lock, so a claim is "query free, take it shared,
  // query again": a second claimer racing us onto the same slot shows up in
  // the second query, and we back off to another slot.  (Should two keep a
  // slot anyway, it is merely shared: a dead holder in it looks alive while
  // the other lives, and the waiter falls back to the escalation.  Never
  // the unsafe way.)
  int fd = -1;
  if (!fail) {
    do {
      fd = ::open(_path.c_str(), O_RDONLY | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
  }
  if (fd >= 0) {
    struct stat ours{};
    struct stat volume{};
    // The name may have been replaced since the volume was opened: a lock
    // on another inode would be invisible to every peer.
    const bool same_inode =
        ::fstat(fd, &ours) == 0 && ::fstat(_probe_fd, &volume) == 0 &&
        ours.st_dev == volume.st_dev && ours.st_ino == volume.st_ino;
    for (unsigned i = 0; same_inode && i < slots && claimed < 0; ++i) {
      const unsigned slot = (first + i) % slots;
      const int before = slot_locked_elsewhere(fd, slot);
      if (before < 0) {
        break;  // No OFD locks here: stay without a slot
      }
      if (before != 0 || !set_slot_lock(fd, slot, F_RDLCK)) {
        continue;
      }
      if (slot_locked_elsewhere(fd, slot) != 0) {
        (void)set_slot_lock(fd, slot, F_UNLCK);  // A racing claimer
        continue;
      }
      claimed = static_cast<int>(slot);
    }
    if (claimed >= 0) {
      if (_claim_fd >= 0) {
        // A forked child's inherited copy of its parent's claim: closing
        // our copy leaves the parent's slot to the parent.
        ::close(_claim_fd);
      }
      _claim_fd = fd;
    } else {
      ::close(fd);
    }
  }
#else
  (void)fail;
  (void)first;
#endif
  const uint64_t outcome =
      claimed >= 0 ? (static_cast<uint64_t>(claimed) << kSlotShift) | kClaimed
                   : kFailed;
  _state.store(uint64_t{fork_epoch} | outcome, std::memory_order_release);
  return claimed >= 0;
}

WriterLiveness::Verdict WriterLiveness::probe(unsigned slot) const {
  std::lock_guard<std::mutex> lock(liveness_mutex());
  if (_probe_fd < 0 || slot >= kSlots) {
    return Verdict::kUnknown;
  }
#if defined(_WIN32)
  HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(_probe_fd));
  if (handle == INVALID_HANDLE_VALUE) {
    return Verdict::kUnknown;
  }
  OVERLAPPED ov = slot_overlapped(slot);
  if (::LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                   0, 1, 0, &ov) != 0) {
    (void)::UnlockFileEx(handle, 0, 1, 0, &ov);
    return Verdict::kDead;
  }
  return ::GetLastError() == ERROR_LOCK_VIOLATION ? Verdict::kAlive
                                                  : Verdict::kUnknown;
#elif defined(F_OFD_GETLK)
  switch (slot_locked_elsewhere(_probe_fd, slot)) {
    case 0:
      return Verdict::kDead;
    case 1:
      return Verdict::kAlive;
    default:
      return Verdict::kUnknown;
  }
#else
  (void)slot;
  return Verdict::kUnknown;
#endif
}

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
      _num_buckets(other._num_buckets),
      _liveness(other._liveness) {
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
    _liveness = other._liveness;

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
                           std::span<const uint64_t> clear_offsets,
                           bool *busy) {
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
  //
  // Both waits are capped: behind live holders that keep changing, give up
  // and publish nothing (*busy) rather than stall the writer.
  PhaseLockToken phase_token;
  if (!acquire_phase_lock(phase_token, /*capped=*/true)) {
    if (busy != nullptr) {
      *busy = true;
    }
    return false;
  }
  bool cur_phase = current_phase();

  // Acquire writer lock (CAS even → odd, waits if another writer holds it).
  uint32_t writer_token = 0;
  if (!acquire_writer(bucket_idx, writer_token, /*capped=*/true)) {
    release_phase_lock(phase_token);
    if (busy != nullptr) {
      *busy = true;
    }
    return false;
  }

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

  // Acquire writer lock (CAS even → odd, waits if another writer holds it).
  uint32_t writer_token = 0;
  if (!acquire_writer(bucket_idx, writer_token, /*capped=*/true)) {
    return false;
  }

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

bool MmapDirectory::remove_at(const CacheKey &key, uint64_t target_offset,
                              bool *busy) {
  if (_header == nullptr) {
    return false;
  }

  uint32_t bucket_idx = key.bucket_hash() % _num_buckets;
  uint16_t target_tag = key.tag();

  DirEntry *bucket = &_entries[bucket_idx * kEntriesPerBucket];

  uint32_t writer_token = 0;
  if (!acquire_writer(bucket_idx, writer_token, /*capped=*/true)) {
    if (busy != nullptr) {
      *busy = true;  // Nothing removed
    }
    return false;
  }

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

uint32_t MmapDirectory::remove_all_at(const CacheKey &key,
                                      std::span<const uint64_t> offsets,
                                      bool *busy) {
  if (_header == nullptr || offsets.empty() || offsets.size() > 32) {
    return 0;
  }

  uint32_t bucket_idx = key.bucket_hash() % _num_buckets;
  uint16_t target_tag = key.tag();

  DirEntry *bucket = &_entries[bucket_idx * kEntriesPerBucket];

  uint32_t writer_token = 0;
  if (!acquire_writer(bucket_idx, writer_token, /*capped=*/true)) {
    if (busy != nullptr) {
      *busy = true;  // Nothing removed
    }
    return 0;
  }

  // EVERY entry with this tag at one of the offsets goes (as remove_at).
  uint32_t mask = 0;
  size_t removed = 0;
  for (size_t i = 0; i < kEntriesPerBucket; ++i) {
    if (bucket[i].is_empty() || bucket[i].tag() != target_tag) {
      continue;
    }
    for (size_t o = 0; o < offsets.size(); ++o) {
      if (bucket[i].offset() == offsets[o]) {
        bucket[i].clear();
        ++removed;
        mask |= 1U << o;
        break;
      }
    }
  }

  release_writer(bucket_idx, writer_token);

  for (size_t i = 0; i < removed; ++i) {
    decrement_count();
  }

  return mask;
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
    (void)acquire_writer(i, writer_tokens[i], /*capped=*/false);
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
  // Uncapped: this runs inside a wrap under the write lock, which a peer
  // needs before it can start another insert, so contention drains.
  PhaseLockToken phase_token;
  (void)acquire_phase_lock(phase_token, /*capped=*/false);
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
  PhaseLockToken phase_token;
  (void)acquire_phase_lock(phase_token, /*capped=*/false);  // As toggle_phase
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
// presumes the holder stuck once that same holder -- identified by the
// acquisition generation it bumped phase_lock_gen to -- has kept the lock
// for kPhaseLockBudget, and a continuous confirmation poll saw neither a
// release nor another holder.  The header has no room to publish the
// holder's PID for a liveness proof like the write lock's, so the budget is
// what separates "descheduled" from "stuck": it is set well above the
// longest live hold measured under CPU oversubscription, and above
// kBucketWriterBudget, which bounds the holder's own nested wait on a
// bucket (see the PR for #27).
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
// Release is checked twice: the generation must still be ours (any later
// acquisition or recovery bumped it) and the lock byte is CASed from our
// own token, so a holder recovered from under us can never free the
// usurper's -- or a later holder's -- critical section.
bool MmapDirectory::acquire_phase_lock(PhaseLockToken &token, bool capped) {
  uint8_t observed = 0;
  if (try_take_token_lock(std::atomic_ref<uint8_t>(_header->phase_lock),
                          std::atomic_ref<uint16_t>(_header->phase_lock_gen),
                          token.generation, token.value, observed)) [[likely]] {
    return true;
  }
  return acquire_phase_lock_slow(token, capped);
}

bool MmapDirectory::acquire_phase_lock_slow(PhaseLockToken &token,
                                            bool capped) {
  auto lock = std::atomic_ref<uint8_t>(_header->phase_lock);
  auto gen = std::atomic_ref<uint16_t>(_header->phase_lock_gen);
  LockHolderWait wait(phase_lock_budget(), lock_wait_cap(capped));
  for (;;) {
    uint8_t observed = 0;
    if (try_take_token_lock(lock, gen, token.generation, token.value,
                            observed)) {
      return true;
    }
    if (observed == 0) {
      wait.restart();  // Seen free: whoever holds it next is a new holder
      continue;
    }
    const uint16_t holder_gen = gen.load(std::memory_order_seq_cst);
    const LockHolderWait::Step step =
        wait.wait(token_lock_holder(observed, holder_gen, 0));
    if (step == LockHolderWait::Step::kGiveUp) {
      count_give_up();
      return false;  // Live, changing holders for the whole cap
    }
    if (step != LockHolderWait::Step::kExpired) {
      continue;
    }
    // One holder has kept the lock for the whole budget and the
    // confirmation window: recover it from exactly that holder.
    if (recover_token_lock(lock, gen, observed, holder_gen, nullptr)) {
#ifdef CYCLONE_TEST_SEAMS
      s_phase_lock_recoveries_for_test.fetch_add(1, std::memory_order_relaxed);
#endif
    }
    wait.restart();
  }
}

void MmapDirectory::release_phase_lock(PhaseLockToken token) {
  // Only if the lock is still ours (see acquire_phase_lock): our generation
  // is still current -- any later acquisition or recovery bumped it -- and
  // the byte still holds our token.  A late release by a holder recovered
  // from under us fails the first check unless exactly 65536*k bumps landed
  // meanwhile, and between the two checks a new holder would need a token
  // equal to ours, i.e. 253*k bumps inside those two instructions: both
  // negligible (and a phase-lock overlap is contained anyway, see above).
  if (std::atomic_ref<uint16_t>(_header->phase_lock_gen)
          .load(std::memory_order_seq_cst) != token.generation) {
    return;
  }
  uint8_t expected = token.value;
  (void)std::atomic_ref<uint8_t>(_header->phase_lock)
      .compare_exchange_strong(expected, 0, std::memory_order_seq_cst,
                               std::memory_order_relaxed);
}

MmapDirectory::WriteLockToken MmapDirectory::acquire_write_lock(bool capped) {
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
  // The lock byte carries the holder's token, derived from the acquisition
  // generation it bumped write_lock_gen to (try_take_token_lock), so the
  // release is a CAS from exactly that value, a recovery claims exactly the
  // value it gave up on, and any later acquisition or recovery makes the
  // holder's revalidate fail.
  //
  // TOKEN ORDERING IS LOAD-BEARING: the generation is claimed with a seq_cst
  // fetch_add AFTER the winning CAS.  A recovery that claims our byte
  // between our CAS and our fetch_add bumps the generation too; either our
  // claim sees that bump (it does not land one past the post-CAS re-read,
  // and try_take_token_lock reports the lock lost before the critical
  // section), or the bump lands after ours and our revalidate fails before
  // any shared side effect.
  auto lock = std::atomic_ref<uint8_t>(_header->write_lock);
  auto gen = std::atomic_ref<uint16_t>(_header->write_lock_gen);
  auto owner = std::atomic_ref<uint32_t>(_header->write_lock_owner_pid);

  WriteLockToken token;
  uint8_t observed = 0;
  // The PID we publish (the same getpid() every acquisition always made; only
  // builds before #32 read it, for their kill(pid, 0) probe) and the
  // WriterLiveness slot we hold, which our token encodes: after the first
  // acquisition of a process, one atomic load and a compare, no syscall.
  const uint32_t pid = self_pid();
  token.liveness_slot =
      _liveness != nullptr ? _liveness->slot_for() : WriterLiveness::kNoSlot;

  // Hot uncontended path: generation load, CAS, generation fetch_add.
  if (try_take_token_lock(lock, gen, token.generation, token.value, observed,
                          token.liveness_slot)) {
    owner.store(pid, std::memory_order_relaxed);
    token.acquired = true;
    return token;
  }

  // Contended path.  Wait with LockHolderWait (spin, then sleeping
  // backoff).  Once one holder has kept the lock for kWriteLockProbeAfter
  // (far past any ordinary contention), consult its liveness after every
  // sleep (write_lock_holder_proven_dead).  Recovery from a genuinely dead
  // holder is preserved (a crash must not deadlock the cache); usurpation of
  // a live-but-stalled holder is removed.
  //
  // Last-resort deadlock breaker for the cases the liveness proof cannot
  // decide (a holder without a liveness slot -- an older build, a process
  // that found none --, a dead holder whose slot a new process re-claimed at
  // once, or a live holder wedged for many seconds): the same holder -- same
  // token, generation and PID -- has held the lock for
  // kWriteLockEscalationBudget and a continuous confirmation poll saw no
  // release.  If this ever fires
  // on a truly live holder, the generation bump below makes that holder's
  // revalidate fail, so it aborts before its overlapping publish/pwrite —
  // corruption stays closed; only availability degrades.
  LockHolderWait wait(write_lock_escalation_budget(), lock_wait_cap(capped));
  for (;;) {
    if (try_take_token_lock(lock, gen, token.generation, token.value, observed,
                            token.liveness_slot)) {
      owner.store(pid, std::memory_order_relaxed);
      token.acquired = true;
      return token;
    }
    if (observed == 0) {
      wait.restart();  // Seen free: whoever holds it next is a new holder
      continue;
    }
    const uint16_t holder_gen = gen.load(std::memory_order_seq_cst);
    const uint32_t holder = owner.load(std::memory_order_acquire);
    const LockHolderWait::Step step =
        wait.wait(token_lock_holder(observed, holder_gen, holder));
    if (step == LockHolderWait::Step::kSpun) {
      continue;
    }
    if (step == LockHolderWait::Step::kGiveUp) {
      // Live holders kept changing for the whole cap: give up WITHOUT the
      // lock (acquired stays false); the caller reports Busy.
      count_give_up();
      token.gave_up = true;
      return token;
    }

    // Slept, or the escalation budget is spent: prove DEAD before we
    // recover, or WAIT.
    bool dead = false;
    bool escalate = step == LockHolderWait::Step::kExpired;
    if (step == LockHolderWait::Step::kSlept) {
      ++token.live_waits;
    }
#ifdef CYCLONE_TEST_SEAMS
    // Test seams: presume the holder dead without a liveness proof (the
    // pre-fix behaviour), or escalate after that many live waits.
    dead = s_write_lock_presume_dead_for_test.load(std::memory_order_relaxed);
    const uint32_t max_live_waits_override =
        s_write_lock_max_live_waits_for_test.load(std::memory_order_relaxed);
    if (step == LockHolderWait::Step::kSlept && max_live_waits_override != 0 &&
        token.live_waits >= max_live_waits_override) {
      escalate = true;
    }
#endif
    if (!dead && wait.held() >= kWriteLockProbeAfter) {
      dead = write_lock_holder_proven_dead(observed, holder_gen, holder);
    }
    if (!dead && !escalate) {
      continue;  // Live holder (or an owner not yet published): WAIT
    }

    // Recover the lock from exactly the holder we gave up on (claim, bump
    // the generation so a pathologically live holder's revalidate fails
    // before it publishes shared_write_pos / pwrites, clear the owner,
    // free).  A holder that just released, or a new holder, is never
    // touched.
    if (recover_token_lock(lock, gen, observed, holder_gen, &owner)) {
      // Telemetry split: recovering a PROVEN-dead holder is routine crash
      // recovery; taking over a holder we could NOT prove dead (last-resort
      // escalation) is the alertable event.
      if (dead) {
        token.forced_release = true;
      } else {
        token.escalated_takeover = true;
      }
    }
    // Re-contend for the lock (another waiter may win the race; that is
    // fine — the generation only advances).
    wait.restart();
  }
}

MmapDirectory::WriteLockToken MmapDirectory::try_acquire_write_lock() {
  // Non-blocking counterpart of acquire_write_lock for the two in-place
  // header RMW sites (update_hit_count_sync / remove_alternate_sync's chain
  // repoint).  It is EXACTLY acquire_write_lock's uncontended fast path --
  // one attempt of try_take_token_lock -- with no wait, liveness probe, or
  // usurpation.  On contention (or a lock lost to a racing recovery before
  // the critical section) it returns {acquired=false} so the caller never
  // blocks a request thread behind a peer's pwrite+fsync.
  WriteLockToken token;
  uint8_t observed = 0;
  const uint32_t pid = self_pid();
  token.liveness_slot =
      _liveness != nullptr ? _liveness->slot_for() : WriterLiveness::kNoSlot;
  if (try_take_token_lock(std::atomic_ref<uint8_t>(_header->write_lock),
                          std::atomic_ref<uint16_t>(_header->write_lock_gen),
                          token.generation, token.value, observed,
                          token.liveness_slot)) {
    std::atomic_ref<uint32_t>(_header->write_lock_owner_pid)
        .store(pid, std::memory_order_relaxed);
    token.acquired = true;
  }
  return token;
}

#ifdef CYCLONE_TEST_SEAMS
uint8_t MmapDirectory::write_lock_token_for_test(uint16_t generation,
                                                 int slot) {
  return slot < 0 ? lock_token(generation)
                  : slot_lock_token(generation, static_cast<unsigned>(slot));
}

int MmapDirectory::write_lock_slot_for_test(uint8_t value,
                                            uint16_t generation) {
  return slot_of_token(value, generation);
}
#endif

bool MmapDirectory::write_lock_holder_proven_dead(uint8_t value,
                                                  uint16_t generation,
                                                  uint32_t pid) const {
  // Only a holder whose token encodes a WriterLiveness slot has a lock to
  // look for.  Any other holder (an older build, which may still sit in
  // another PID namespace, or a process that holds no slot) is never proven
  // dead: the absence of a lock it never took proves nothing.
  //
  // The value and generation come from one stable observation
  // (LockHolderWait saw the same {value, generation, pid} for
  // kWriteLockProbeAfter).  A nonzero PID means the holder got past its
  // generation claim and token fix-up (it publishes the PID last, and a
  // release or recovery clears it before freeing the byte), so the value
  // is the final token of that generation, not the provisional one a holder
  // stalled between its CAS and its fetch_add would show -- which could
  // decode to another slot.
  if (pid == 0 || _liveness == nullptr) {
    return false;
  }
  const int slot = slot_of_token(value, generation);
  if (slot < 0) {
    return false;
  }
  // Our own slot: this process is alive.
  if (slot == _liveness->slot_of()) {
    return false;
  }
  return _liveness->probe(static_cast<unsigned>(slot)) ==
         WriterLiveness::Verdict::kDead;
}

bool MmapDirectory::revalidate_write_lock(const WriteLockToken &token) const {
  // Primary check: the acquisition generation is still the one this
  // acquisition claimed.  Every later acquisition and every force-release
  // bumps it, and nobody else acquires while we hold, so a changed value
  // means the lock was recovered from US.  The caller then drops the fill
  // as Busy.
  //
  // Generation aliasing bound, stated honestly: a uint16 wraps after 2^16
  // acquisitions + recoveries while ONE usurped holder stays stalled.  The
  // complements below break that unless, at the moment of the check, the
  // lock is also held under our exact token (implied by an aliased
  // generation) AND by our exact PID.  Another holder with our PID would
  // have to be a thread of this process writing the same stripe, which the
  // stripe mutex the usurped holder still holds excludes; only two Volumes
  // on one file in one process (tests) share a PID without sharing it.
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

bool MmapDirectory::touch_bucket(const CacheKey &key, bool capped) {
  if (_header == nullptr || _num_buckets == 0) {
    return true;
  }
  const size_t bucket_idx = key.bucket_hash() % _num_buckets;
  // Empty writer bracket: +2, fenced and parity-preserving.  A reader inside
  // probe_each's seqlock loop observes the change and retries — a spurious
  // retry, never a wrong serve.
  uint32_t writer_token = 0;
  if (!acquire_writer(bucket_idx, writer_token, capped)) {
    return false;
  }
  release_writer(bucket_idx, writer_token);
  return true;
}

bool MmapDirectory::acquire_writer(size_t bucket_idx, uint32_t &token,
                                   bool capped) {
#ifdef CYCLONE_TEST_SEAMS
  if (capped) {
    int left = s_bucket_give_up_after_for_test.load(std::memory_order_relaxed);
    while (left >= 0) {
      if (left == 0) {
        count_give_up();
        return false;  // As if the cap had run out
      }
      if (s_bucket_give_up_after_for_test.compare_exchange_weak(
              left, left - 1, std::memory_order_relaxed)) {
        break;
      }
    }
  }
#endif
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
    token = v;
    return true;
  }
  return acquire_writer_slow(bucket_idx, token, capped);
}

bool MmapDirectory::acquire_writer_slow(size_t bucket_idx, uint32_t &token,
                                        bool capped) {
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
  //
  // Capped (every production mutator but clear()): behind live holders that
  // keep changing for kLockWaitCap, give up (false) and let the operation
  // report Busy; the bucket is never forced on the cap.
  LockHolderWait wait(bucket_writer_budget(), lock_wait_cap(capped));
  for (;;) {
    uint32_t v = ref.load(std::memory_order_acquire);
    if ((v & 1) != 0) {
      const LockHolderWait::Step step = wait.wait(v);
      if (step == LockHolderWait::Step::kGiveUp) {
        count_give_up();
        return false;
      }
      if (step == LockHolderWait::Step::kExpired) {
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
      token = v;
      return true;
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
