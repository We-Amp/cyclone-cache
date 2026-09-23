// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "volume.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "../io/mapped_file.hpp"
#include "../ram_cache/ram_cache.hpp"
#include "cyclone/detail/expected_compat.hpp"
#include "document.hpp"
#include "hit_tracker.hpp"

#ifdef _WIN32
#ifdef _MSC_VER
// MSVC uses different function names
#include <BaseTsd.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
using ssize_t = SSIZE_T;
#define CYCLONE_OPEN _open
#define CYCLONE_CLOSE _close
#define CYCLONE_FSTAT _fstat64
#define CYCLONE_STAT_STRUCT struct _stat64
#define CYCLONE_FTRUNCATE _chsize_s
#define CYCLONE_LSEEK _lseeki64
#define CYCLONE_READ _read
#define CYCLONE_WRITE _write
#define CYCLONE_FSYNC _commit
#define CYCLONE_OPEN_FLAGS (O_RDWR | O_CREAT | O_BINARY)
#define CYCLONE_OPEN_MODE (_S_IREAD | _S_IWRITE)
#else
// MinGW uses POSIX-style functions but _commit for sync
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <unistd.h>
#define CYCLONE_OPEN ::open
#define CYCLONE_CLOSE ::close
#define CYCLONE_FSTAT ::fstat
#define CYCLONE_STAT_STRUCT struct stat
#define CYCLONE_FTRUNCATE ::ftruncate
#define CYCLONE_LSEEK ::lseek
#define CYCLONE_READ ::read
#define CYCLONE_WRITE ::write
#define CYCLONE_FSYNC ::_commit
#define CYCLONE_OPEN_FLAGS (O_RDWR | O_CREAT | O_BINARY)
#define CYCLONE_OPEN_MODE 0666
#endif
#else
// POSIX systems
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#define CYCLONE_OPEN ::open
#define CYCLONE_CLOSE ::close
#define CYCLONE_FSTAT ::fstat
#define CYCLONE_STAT_STRUCT struct stat
#define CYCLONE_FTRUNCATE ::ftruncate
#define CYCLONE_LSEEK ::lseek
#define CYCLONE_READ ::read
#define CYCLONE_WRITE ::write
#define CYCLONE_FSYNC ::fsync
#define CYCLONE_OPEN_FLAGS (O_RDWR | O_CREAT)
#define CYCLONE_OPEN_MODE 0666
#endif

// cross-process serialization of Volume::open() initialization.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX  // windows.h min/max macros would break std::min/std::max
#endif
#include <windows.h>
#else
#include <sys/file.h>  // flock
#endif

namespace cyclone {

namespace {

// --- Cross-process lock substrate (init lock + reset gate) --------
//
// TWO advisory locks live on the volume fd, both fcntl OFD byte-range locks at
// distinct past-EOF bytes (real volume I/O never reaches them):
//
//   * INIT lock (kInitLockByte): EXCLUSIVE, held only across open_locked() to
//     serialize concurrent openers.  A non-creator blocks until the
//     creator finished initializing (ftruncate -> reset() -> init_stripes()),
//     or the creator died -- the kernel drops the lock on the LAST close of the
//     OFD, so a creator crash cannot wedge future openers; the next opener then
//     repairs the partial state under this lock (reset() publishes the header
//     LAST, so a torn init reads as a missing header => rerun reset()).
//
//   * LIFETIME lock (kLifetimeLockByte): SHARED, held for the whole life of an
//     open Volume.  A would-be resetter first PROBES it exclusively
//     (non-blocking); if any live peer holds it shared, the probe fails and we
//     REFUSE TO OPEN instead of resetting a volume a worker is still serving.
//
// WHY fcntl OFD byte-range locks and not flock (load-bearing -- this substrate
// was gotten wrong twice):
//
//   1. On macOS, flock() and fcntl() locks are NOT independent: a flock and an
//      fcntl byte-range lock on the same inode CONFLICT -- even across
//      different fds and at a far byte (verified empirically on the target
//      SDK).  So the init lock and the lifetime lock cannot live in different
//      lock families; both must be fcntl, on DIFFERENT bytes (fcntl byte ranges
//      ARE mutually independent).  Linux keeps flock and fcntl independent, but
//      the two-byte fcntl scheme is equally correct there, so we run ONE scheme
//      on both. (An earlier sidecar-file + whole-file-flock design deadlocked
//      here and wiped live caches; see the git history of this file.)
//
//   2. OFD locks (F_OFD_SETLK) are owned by the OPEN FILE DESCRIPTION, so an
//      nginx/Apache master that opens the volume and then forks shares ONE OFD
//      with every worker: the lifetime lock survives as long as ANY of them
//      holds the fd and is released only by the LAST close.  Old workers
//      outliving a graceful reload therefore keep the volume held down -- so a
//      new master that would reset REFUSES TO OPEN instead of wiping their
//      cache.  This is why the
//      lifetime lock is NEVER explicitly unlocked: an explicit F_UNLCK on one
//      worker's graceful close would drop the lock for the WHOLE family (the
//      exact bug that made a graceful reload wipe the cache).  It is released
//      ONLY by close(_fd).
//
//   3. The lifetime lock lives on the REAL volume inode, not a sidecar -- so it
//      cannot be defeated by deleting a sidecar out from under a live peer.
//      `rm cyclone.dat` under a peer is fine: the peer keeps its now-unlinked
//      inode's lock, and a new opener's O_CREAT gets a FRESH empty inode with
//      no peer to protect, so it resets an empty file.
//
// Corollary (why the EX probe is CONDITIONAL on needs_reset): a process in the
// same fork family that constructs a NEW Volume and open()s gets a FRESH OFD,
// so its EX probe WOULD conflict with its own family's shared lock and it would
// wrongly decline to reset.  Harmless only because a same-version,
// same-geometry process never sets needs_reset and so never probes.
//
// Windows has no fork and independent byte-range locks, so each process is
// simply independent (correct for IIS overlapped recycle); the two locks sit on
// the same two past-EOF bytes, taken with LockFileEx on the HANDLE behind _fd
// (_get_osfhandle), and the lifetime lock is likewise released only by closing
// the handle.  Win32 byte-range locks conflict between handles of the SAME
// process too, matching the OFD semantics this substrate is built on.  The one
// asymmetry: LockFileEx has no atomic downgrade, so the EX probe is dropped
// before the shared acquire -- see the downgrade-gap note in open_locked.
//
// Degradation vs. fail-closed (asymmetric, and deliberately so): the reset gate
// only ever DEGRADES to the historical ungated behaviour (reset allowed) when
// byte-range locking is PROVEN absent -- fcntl returns EINVAL/ENOTSUP/
// EOPNOTSUPP (some NFS/overlay), or LockFileEx returns ERROR_NOT_SUPPORTED /
// ERROR_INVALID_FUNCTION (exotic redirectors: WebDAV, Dokan-alikes).  There an
// ungated reset is strictly better than a cache that cannot start, and with no
// locks there is provably no peer to protect.  On any AMBIGUOUS status -- above
// all ENOLCK, which on Linux is TRANSIENT kernel-lock-table exhaustion and can
// occur while a real peer holds the lock, and its Win32 analogue
// ERROR_INVALID_PARAMETER -- the gate fails CLOSED: it does NOT reset (it
// refuses to open), and a later opener resets once the transient clears.  Note
// ERROR_LOCK_VIOLATION is the ordinary Win32 CONFLICT (a peer holds it), never
// a no-support signal.  See the reset-gate probe below.
// Degradation is surfaced via stats (reset_gate_degraded).  The init lock falls
// back to whole-file flock when OFD is unsupported so the init-lock
// serialization still holds (there is then no fcntl lifetime lock for the flock
// to fight).

// Outcome of an advisory-lock attempt.
enum class LockResult : std::uint8_t { Acquired, Conflict, Unsupported };

// How the init lock was actually taken (so release matches; mixing flock and
// fcntl unlocks on macOS is unsafe -- they share a lock space there).
enum class InitLockKind : std::uint8_t { None, Ofd, Flock };

// Under WHICH gate state did this open decide to reset?  Recorded as an
// explicit local in open_locked because the reset counters must NOT be derived
// from the _reset_gate_degraded gauge: on a lock-less filesystem the
// created_new (F1) path resets PERFECTLY SAFELY -- a fresh inode has no peer --
// and only degrades the gauge LATER, at the shared acquire.  Keying the alarm
// off the gauge would therefore make every routine `rm cyclone.dat` + reload
// raise a false "we may have wiped a live peer".
//   NotNeeded    no reset ran
//   FreshInode   F1: we O_CREAT'd the inode; no peer possible, gate not needed
//   GateDegraded reset ran with the gate NOT in effect -- THE ALARM
//   GateVerified reset ran with the exclusive lifetime lock proven held
enum class ResetProvenance : std::uint8_t {
  NotNeeded,
  FreshInode,
  GateVerified,
  GateDegraded
};

// Past-EOF bytes for the two advisory locks.  Distinct from each other so the
// byte ranges are independent (fcntl on POSIX, LockFileEx on Win32); both far
// past any real volume offset.
//
// std::uint64_t, NOT off_t: off_t is 32-bit on MSVC and would TRUNCATE these to
// 0xFFFFFFFE / 0xFFFFFFFD -- collapsing the two locks toward each other and
// dragging them into a range a real (huge) volume could reach.  The POSIX
// ofd_lock() casts back to off_t at the fcntl call, where off_t is 64-bit.
//
// The two values MUST stay distinct: the init lock is held across the WHOLE of
// open_locked, so if the lifetime byte aliased it the reset gate's exclusive
// probe would always self-succeed -- a gate that reports healthy while never
// actually gating anything.
constexpr std::uint64_t kInitLockByte = 0x7FFFFFFFFFFFFFFEULL;
constexpr std::uint64_t kLifetimeLockByte = 0x7FFFFFFFFFFFFFFDULL;
static_assert(kInitLockByte != kLifetimeLockByte,
              "the init and lifetime locks must sit on DISTINCT bytes or the "
              "reset-gate probe self-succeeds against our own init lock");

#ifdef _WIN32
// Split a 64-bit lock byte into the OVERLAPPED offset pair LockFileEx/
// UnlockFileEx read the range start from.  (Win32 has no 64-bit scalar offset
// argument; the low DWORD goes in Offset, the high DWORD in OffsetHigh.)
inline OVERLAPPED lock_overlapped(std::uint64_t byte) {
  OVERLAPPED ov{};
  ov.Offset = static_cast<DWORD>(byte & 0xFFFFFFFFULL);
  ov.OffsetHigh = static_cast<DWORD>(byte >> 32);
  return ov;
}

// Mirror of classify_lock_errno.  BROAD Unsupported: for the INIT lock and the
// SHARED lifetime acquire, Unsupported is SAFE (init falls back / shared
// degrades to ungated) and Conflict is the DANGEROUS one -- a spurious Conflict
// at the shared acquire fails the open outright (see open_locked).  The
// RESET-GATE probe must NOT use this; see win_error_is_persistent_unsupported.
LockResult classify_lock_win32(DWORD e) {
  if (e == ERROR_LOCK_VIOLATION || e == ERROR_SHARING_VIOLATION ||
      e == ERROR_IO_PENDING) {
    return LockResult::Conflict;
  }
  return LockResult::Unsupported;
}

// Mirror of ofd_errno_is_persistent_unsupported, and just as NARROW.  ONLY
// these may degrade the RESET GATE to ungated: if byte-range locking cannot
// exist on this volume, there is provably no peer to protect.  Every other
// status is AMBIGUOUS and the gate MUST fail CLOSED on it (-> Conflict =
// "assume a peer, do not reset").  ERROR_LOCK_VIOLATION is the documented
// LOCKFILE_FAIL_IMMEDIATELY conflict and is NEVER a no-support signal.  Local
// NTFS/ReFS and the SMB redirector all implement byte-range locks; only exotic
// redirectors (WebDAV, Dokan-alikes) report no-support, and they report it
// PERSISTENTLY.
//
// Deliberately NOT in this set: ERROR_INVALID_PARAMETER (87).  It is the shape
// of "this filesystem dislikes a past-EOF offset", indistinguishable from a
// transient, so it must fail closed -- the Win32 analogue of ENOLCK.
constexpr bool win_error_is_persistent_unsupported(DWORD e) {
  return e == ERROR_NOT_SUPPORTED || e == ERROR_INVALID_FUNCTION;
}
static_assert(!win_error_is_persistent_unsupported(ERROR_LOCK_VIOLATION),
              "ERROR_LOCK_VIOLATION is a PEER CONFLICT and must never degrade "
              "the reset gate");
#endif  // _WIN32

#ifndef _WIN32
// Classify an fcntl lock errno for the INIT-lock and shared-lifetime-acquire
// paths.  EAGAIN/EACCES => a peer holds a conflicting lock.  Anything else =>
// Unsupported.  Both callers treat Unsupported safely: the init lock falls back
// to flock, and a failed shared acquire degrades to ungated -- NEITHER carries
// a wipe risk.  The RESET-GATE probe deliberately does NOT use this classifier;
// it must fail CLOSED on an ambiguous errno (see the probe below).
LockResult classify_lock_errno(int e) {
  if (e == EAGAIN || e == EACCES) {
    return LockResult::Conflict;
  }
  return LockResult::Unsupported;
}

// Errnos that PROVE the filesystem/kernel has no OFD byte-range locking at all.
// ONLY these may degrade the RESET GATE to ungated (and thus allow reset()):
// if OFD locks cannot exist here, there is provably no peer to protect.
//
// This set is deliberately NARROW.  Every other errno is AMBIGUOUS and the
// reset gate must fail CLOSED on it -- most importantly ENOLCK, which on Linux
// means the kernel lock table is momentarily EXHAUSTED: a TRANSIENT condition
// under which a real live peer may hold the lifetime lock that the kernel just
// could not evaluate.  Misread as "lockless filesystem" it would degrade and
// reset under the live peer -- the exact 400/400 -> 0/400 wipe this whole gate
// exists to prevent.  On ambiguity we simply do not reset THIS time (refuse to
// open); a later opener resets once the
// transient clears.  Strictly the safe direction.
constexpr bool ofd_errno_is_persistent_unsupported(int e) {
  return e == EINVAL
#ifdef ENOTSUP
         || e == ENOTSUP
#endif
#ifdef EOPNOTSUPP
         || e == EOPNOTSUPP
#endif
      ;
}
// Lock the intent at compile time: ENOLCK must NEVER be treated as persistent
// (it is transient), and a genuine no-support errno must be.
static_assert(
    !ofd_errno_is_persistent_unsupported(ENOLCK),
    "ENOLCK is transient (kernel lock-table exhaustion) and must fail "
    "CLOSED at the reset-gate probe, not degrade-and-reset");
static_assert(ofd_errno_is_persistent_unsupported(EINVAL),
              "EINVAL (OFD unsupported) must be allowed to degrade the gate");

// fcntl OFD lock on a single past-EOF byte.  cmd is F_OFD_SETLK (non-blocking)
// or F_OFD_SETLKW (blocking); type is F_RDLCK / F_WRLCK / F_UNLCK.
LockResult ofd_lock(int fd, int cmd, int type, std::uint64_t byte) {
#if defined(F_OFD_SETLK)
  struct flock fl{};
  fl.l_type = static_cast<short>(type);
  fl.l_whence = SEEK_SET;
  fl.l_start = static_cast<off_t>(byte);
  fl.l_len = 1;
  int rc = 0;
  do {
    rc = fcntl(fd, cmd, &fl);
  } while (rc != 0 && errno == EINTR);
  if (rc == 0) {
    return LockResult::Acquired;
  }
  return classify_lock_errno(errno);
#else
  (void)fd;
  (void)cmd;
  (void)type;
  (void)byte;
  return LockResult::Unsupported;
#endif
}
#endif  // !_WIN32

// Acquire the EXCLUSIVE init lock (blocking).  Returns how it was taken so the
// matching release can undo the right lock family (None on failure).
InitLockKind acquire_init_lock(int fd) {
#ifdef _WIN32
  HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
  if (handle == INVALID_HANDLE_VALUE) {
    return InitLockKind::None;
  }
  OVERLAPPED ov = lock_overlapped(kInitLockByte);
  return LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &ov) != 0
             ? InitLockKind::Ofd  // (Windows: single kind; value unused)
             : InitLockKind::None;
#else
  LockResult r = ofd_lock(fd, F_OFD_SETLKW, F_WRLCK, kInitLockByte);
  if (r == LockResult::Acquired) {
    return InitLockKind::Ofd;
  }
  if (r != LockResult::Unsupported) {
    return InitLockKind::None;  // genuine error (should not happen for SETLKW)
  }
  // OFD locks unavailable on this fs: fall back to whole-file flock so
  // the init-lock serialization still holds.  Safe because the lifetime gate
  // also degrades to ungated here, so there is no fcntl lock for flock to
  // fight.
  int rc = 0;
  do {
    rc = ::flock(fd, LOCK_EX);
  } while (rc != 0 && errno == EINTR);
  return rc == 0 ? InitLockKind::Flock : InitLockKind::None;
#endif
}

void release_init_lock(int fd, InitLockKind kind) {
  if (kind == InitLockKind::None) {
    return;
  }
#ifdef _WIN32
  HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
  if (handle == INVALID_HANDLE_VALUE) {
    return;
  }
  OVERLAPPED ov = lock_overlapped(kInitLockByte);
  UnlockFileEx(handle, 0, 1, 0, &ov);
#else
  if (kind == InitLockKind::Ofd) {
    (void)ofd_lock(fd, F_OFD_SETLK, F_UNLCK, kInitLockByte);
  } else {
    ::flock(fd, LOCK_UN);
  }
#endif
}

// Non-blocking EXCLUSIVE probe of the lifetime byte, for the RESET GATE only.
// Acquired iff NO other process holds the volume open.  Must be taken BEFORE
// this Volume takes its own shared lock (never hold both; a blocking EX against
// our own shared lock on a second OFD would self-deadlock).  Openers are
// serialized by the init lock, so at most one probe runs at a time.
//
// FAILS CLOSED on an ambiguous errno.  The caller resets ONLY on Acquired and
// degrades-then-resets ONLY on Unsupported, so this returns Unsupported for a
// PROVEN no-OFD-support errno and Conflict (= "assume a peer, do not reset")
// for anything ambiguous -- see ofd_errno_is_persistent_unsupported.
LockResult try_lock_lifetime_exclusive(int fd) {
#ifdef _WIN32
  HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
  if (handle == INVALID_HANDLE_VALUE) {
    return LockResult::Conflict;  // FAIL CLOSED: cannot prove absence of a peer
  }
  OVERLAPPED ov = lock_overlapped(kLifetimeLockByte);
  if (LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0,
                 1, 0, &ov) != 0) {
    return LockResult::Acquired;
  }
  const DWORD e = GetLastError();
  if (win_error_is_persistent_unsupported(e)) {
    return LockResult::Unsupported;
  }
  return LockResult::Conflict;  // ERROR_LOCK_VIOLATION and every ambiguity
#else
  struct flock fl{};
  fl.l_type = F_WRLCK;
  fl.l_whence = SEEK_SET;
  fl.l_start = static_cast<off_t>(kLifetimeLockByte);
  fl.l_len = 1;
  int rc = 0;
  do {
    rc = fcntl(fd, F_OFD_SETLK, &fl);
  } while (rc != 0 && errno == EINTR);
  if (rc == 0) {
    return LockResult::Acquired;
  }
  const int e = errno;
  if (e == EAGAIN || e == EACCES) {
    return LockResult::Conflict;  // a peer holds it
  }
  // ONLY a proven no-OFD-support errno may degrade the gate to ungated.  Every
  // other errno -- notably ENOLCK (transient lock-table exhaustion, under which
  // a live peer may hold the lock) -- fails CLOSED: assume a peer and do NOT
  // reset.  See ofd_errno_is_persistent_unsupported.
  if (ofd_errno_is_persistent_unsupported(e)) {
    return LockResult::Unsupported;
  }
  return LockResult::Conflict;
#endif
}

// Take the SHARED lifetime lock (RDLCK / LockFileEx without the EXCLUSIVE
// flag).  On POSIX this ALSO performs the in-place downgrade from a held
// exclusive probe: F_OFD_SETLK replaces whatever this OFD holds on the byte
// atomically, with no release gap (and we still hold the init lock throughout,
// so no opener can slip in).  Win32 has NO atomic downgrade, so the caller
// drops the EX first -- see the downgrade-gap note in open_locked.  Never
// explicitly released -- see the substrate comment: release is the LAST close
// of _fd only.
//
// Reached on EVERY successful open on both platforms, including the ordinary
// steady-state one: _reset_gate_degraded is cleared at the top of open() and is
// only ever set inside the needs_reset branch, so a same-format open (the
// overwhelmingly common case) arrives here with the gate NOT degraded and takes
// the shared lock.  This is the acquire that makes a live process VISIBLE to a
// future resetter's probe -- on Windows it is what makes the gate exist at all.
LockResult lock_lifetime_shared(int fd) {
#ifdef _WIN32
  // A bad HANDLE DEGRADES here rather than failing the open: unlike the reset
  // gate, a missing shared lock carries no wipe risk of its own (it only means
  // a FUTURE opener cannot see us), and Conflict would fail this open outright.
  HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
  if (handle == INVALID_HANDLE_VALUE) {
    return LockResult::Unsupported;
  }
  // NO LOCKFILE_EXCLUSIVE_LOCK: this is the SHARED acquire, and shared locks
  // stack -- every live peer holds this same byte shared at the same time.
  OVERLAPPED ov = lock_overlapped(kLifetimeLockByte);
  if (LockFileEx(handle, LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov) != 0) {
    return LockResult::Acquired;
  }
  return classify_lock_win32(GetLastError());
#else
  return ofd_lock(fd, F_OFD_SETLK, F_RDLCK, kLifetimeLockByte);
#endif
}

// Drop a held EXCLUSIVE lifetime probe WITHOUT taking the shared lock (used on
// error paths, and on Windows before taking the shared lock).
void unlock_lifetime(int fd) {
#ifdef _WIN32
  HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
  if (handle == INVALID_HANDLE_VALUE) {
    return;
  }
  OVERLAPPED ov = lock_overlapped(kLifetimeLockByte);
  UnlockFileEx(handle, 0, 1, 0, &ov);
#else
  (void)ofd_lock(fd, F_OFD_SETLK, F_UNLCK, kLifetimeLockByte);
#endif
}

constexpr size_t kDirectoryEntriesPerSegment =
    static_cast<const size_t>(16 * 1024);
// kMinStripeSize lives in volume.hpp (shared with the small-tier sizing in
// cache.cpp).
// LAYOUT-CONSTANT INVARIANT: this segment count shapes the on-disk directory
// size but is invisible to kFormatVersionMajor and to the fingerprint geohash.
// Changing it REQUIRES bumping VolumeHeader::kFormatVersionMajor (see the full
// invariant note next to the layout constants in volume.hpp).

// Single source of truth for auto/explicit stripe geometry: init_stripes()
// lays it out, reset() stamps the count into the header, open() validates a
// reopened volume against it.  One definition keeps the three paths from ever
// disagreeing on the on-disk layout.
struct StripeGeometry {
  size_t num_stripes;
  size_t base_stripe_size;
  size_t stripe_remainder;
};
StripeGeometry compute_stripe_geometry(size_t volume_size,
                                       size_t explicit_stripe_size) {
  const size_t usable_size = volume_size - VolumeHeader::kSize;
  constexpr size_t kStripeAlign = 4096;
  size_t num_stripes;
  size_t base_stripe_size;
  size_t stripe_remainder = 0;
  if (explicit_stripe_size == 0) {
    // Auto (default): target ~kAutoStripeGranularity per stripe, ROUNDED to
    // the nearest count and capped at kAutoStripeTarget.  Rounding closes the
    // single-stripe gap; the granularity is only a heuristic (a stripe's sole
    // hard floor is its ~700KB directory), so a slightly-under stripe is safe.
    // Even-tile the whole usable region (stripes SUM to usable_size, no wasted
    // tail); page-floor the base so every stripe offset stays 8-byte aligned
    // (the mmap directory's seq_cst atomics fault if misaligned) and let the
    // LAST stripe absorb the page-aligned remainder.
    num_stripes =
        (usable_size + kAutoStripeGranularity / 2) / kAutoStripeGranularity;
    num_stripes = std::clamp<size_t>(num_stripes, size_t{1}, kAutoStripeTarget);
    base_stripe_size = (usable_size / num_stripes) & ~(kStripeAlign - 1);
    if (base_stripe_size == 0) {
      num_stripes = 1;
      base_stripe_size = usable_size;
    } else {
      stripe_remainder = usable_size - base_stripe_size * num_stripes;
    }
  } else {
    // Explicit stripe_size: fixed-size stripes at the configured granularity,
    // clamped to kMinStripeSize and page-floored (a non-page-aligned value
    // would drift offsets out of alignment and fault the mmap atomics under
    // multi-process mode).  Any sub-stripe tail stays unused rather than
    // growing the last stripe, preserving the geometry the small-tier
    // carve-out depends on.
    base_stripe_size =
        std::max(explicit_stripe_size, kMinStripeSize) & ~(kStripeAlign - 1);
    num_stripes = usable_size / base_stripe_size;
    if (num_stripes == 0) {
      num_stripes = 1;
      base_stripe_size = usable_size;
    }
  }
  return {num_stripes, base_stripe_size, stripe_remainder};
}

// Validate that a relative offset within a stripe is within bounds AND live.
// Returns true if the offset is valid for reading a document header.
// A valid offset must:
// - Be non-zero (0 means end of chain)
// - Be within the stripe's data area (after directory, before stripe end)
// - Leave enough space for at least a document header
// - Sit strictly BEHIND the stripe's write cursor (the phase-ABA positional
//   guard's HOP leg — see below)
bool is_valid_chain_offset(const Stripe* stripe, uint64_t relative_offset) {
  if (relative_offset == 0) {
    return false;  // 0 means end of chain, not an error but not valid to follow
  }

  // Calculate data area bounds using data_offset (accounts for both directory
  // types)
  uint64_t data_area_start = stripe->data_offset - stripe->offset;
  uint64_t data_area_end = stripe->size;

  // Check offset is within data area and has room for at least a header
  if (relative_offset < data_area_start || relative_offset >= data_area_end) {
    return false;
  }

  // Ensure there's room for at least a document header
  if (relative_offset + Document::kHeaderSize > data_area_end) {
    return false;
  }

  // Phase-ABA positional guard, HOP leg (probe leg + full argument at
  // Stripe::is_behind_write_cursor / probe_each).  Chain hops consume
  // next_alternate_offset values from document headers, bypassing the
  // directory probe entirely — and a SAME-KEY dark node passes both the
  // bounds checks above and the walks' cross-key guard.  Reachable with ONE
  // wrap: commit_alternate_write probes the old head H (behind the pre-wrap
  // cursor: passes), stamps next = H into the new document, then
  // allocate_write_slot WRAPS and lands the new head L at the data-area
  // start — leaving L.next pointing at an intact, ahead-of-cursor node the
  // ordinary forward fill will overwrite with no wrap event.  Reject the hop
  // here so no walk (read_alternate_sync, list_alternates_sync,
  // commit_alternate_write's counter, remove_alternate_sync,
  // update_hit_count_sync) ever reaches — let alone borrows, repoints to, or
  // pwrites — a dark node.  Same >= rejection boundary and same
  // shared-vs-local cursor source as the probe leg.  Directory-derived
  // offsets re-checked through here already passed the probe leg, so the
  // predicate never fires for them.
  if (!stripe->is_behind_write_cursor(relative_offset)) {
    return false;
  }

  return true;
}

// Helper function to build AlternateInfo from a document
// Extracts common code from chain traversal lambdas
AlternateInfo build_alternate_info(const DocumentReader& reader,
                                   uint64_t disk_offset) {
  const Document& doc = reader.document();

  AlternateInfo info;
  info.id = static_cast<AlternateId>(doc.alternate_id);
  info.disk_offset = disk_offset;
  info.content_length = doc.total_len;
  info.hit_count = doc.hit_count;
  if (doc.last_access > 0) {
    info.last_access = std::chrono::system_clock::time_point(
        std::chrono::milliseconds(doc.last_access));
  }
  auto header_span = reader.header();
  info.header.assign(header_span.begin(), header_span.end());

  return info;
}

// Map a document at abs_offset, validate the header, and optionally remap
// if the on-disk document is larger than initial_size.  Returns the mapped
// region (caller must unmap) and a valid DocumentReader, or nullopt.
struct MappedDocument {
  std::span<std::byte> region;
  DocumentReader reader;
};

static std::optional<MappedDocument> map_document(MappedFile& mf,
                                                  uint64_t abs_offset,
                                                  size_t initial_size,
                                                  uint64_t stripe_size,
                                                  bool allow_remap) {
  auto mapped =
      mf.map_region(abs_offset, initial_size, MappedFile::MapMode::ReadOnly);
  if (!mapped) return std::nullopt;

  DocumentReader reader(*mapped);
  if (!reader.is_valid()) {
    mf.unmap_region(*mapped);
    return std::nullopt;
  }

  if (allow_remap && reader.document().len > initial_size) {
    if (reader.document().len > stripe_size) {
      mf.unmap_region(*mapped);
      return std::nullopt;  // Corrupted len
    }
    mf.unmap_region(*mapped);
    size_t full_size = reader.document().len;
    mapped =
        mf.map_region(abs_offset, full_size, MappedFile::MapMode::ReadOnly);
    if (!mapped) return std::nullopt;
    reader = DocumentReader(*mapped);
    if (!reader.is_valid()) {
      mf.unmap_region(*mapped);
      return std::nullopt;
    }
  }

  return MappedDocument{*mapped, reader};
}

class VolumeReadHandleImpl : public ReadHandleImpl {
 public:
  // per-thread-shard lifetime anchor (see VolumeReadAnchor in
  // volume.hpp).  Set on disk-hit reads of a Cache-owned Volume; pins the
  // Volume AND the MappedFile for the handle's lifetime through one
  // thread-affine refcount.  When null (direct Volume use in tests), the
  // legacy weak_ptr members below carry the lifetime instead.
  std::shared_ptr<VolumeReadAnchor> anchor;
  std::weak_ptr<MappedFile> mapped_file_weak;  // legacy path (no anchor)
  std::span<std::byte> mapping;
  Document doc;
  std::vector<std::byte> ram_buffer;  // Owns RAM cache data (spans point here)
  std::span<const std::byte> header_data;
  std::span<const std::byte> content_data;
  bool is_ram_hit = false;
  // Lease amendment (2026-07-07): references for the checked
  // ReadHandle::renew_lease().  Set only on disk-hit (mmap-borrow) paths.
  // volume_weak PINS the Volume for the duration of a renew (see
  // renew_lease below) so the raw `stripe` and the mapping outlive a
  // client-paced drain that races Cache::stop() — the borrow-lifetime
  // use-after-free the old raw Volume* left open (mapped_file_weak kept
  // the mapping alive but NOT the Volume or its _stripes).  epoch_start is
  // the wrap epoch captured at probe time; the checked renew fails (so the
  // embedder copies/aborts) iff it moved.
  std::weak_ptr<Volume> volume_weak;
  Stripe* stripe = nullptr;
  std::pair<uint64_t, bool> epoch_start{};
  // receipt for this borrow's entry in the stripe's
  // outstanding-borrow slot; surrendered below so that CLOSING the handle
  // (not lease expiry) is what returns write capacity to the stripe.
  BorrowToken borrow{};

  ~VolumeReadHandleImpl() override {
    if (anchor) {
      // Anchored path: the Volume object and the MappedFile are
      // strongly pinned.  The raw Stripe* is NOT — anchor->torn (see its
      // comment in volume.hpp) says this handle's stripe generation was
      // freed, so the borrow count died with it and must not be touched.
      // The unmap is unconditional: the pinned MappedFile owns that
      // mapping regardless of stripe teardown.
      if (borrow.active && stripe != nullptr &&
          !anchor->torn.load(std::memory_order_seq_cst)) {
        anchor->volume->release_borrow(stripe, borrow);
      }
      if (!mapping.empty()) {
        anchor->mapped_file->unmap_region(mapping);
      }
      return;
    }
    // Legacy weak path (no anchor: direct Volume use).
    // drop this borrow from the stripe's outstanding-borrow
    // slot.  Same Volume-pin as renew_lease(): lock the weak_ptr so
    // `stripe` is not freed mid-release; if the Volume is gone the slot's
    // memory is gone with it (nothing to release — a slot persisted in a
    // shared mmap is cleared by the next ceiling-forced wrap's reset).
    if (borrow.active && stripe != nullptr) {
      if (auto vol = volume_weak.lock()) {
        vol->release_borrow(stripe, borrow);
      }
    }
    // Only unmap if MappedFile still exists (cache not stopped)
    if (auto mf = mapped_file_weak.lock()) {
      if (!mapping.empty()) {
        mf->unmap_region(mapping);
      }
    }
    // If weak_ptr is expired, the mapping was already cleaned up by MappedFile
    // destructor
  }

  [[nodiscard]] std::span<const std::byte> header() const override {
    return header_data;
  }
  [[nodiscard]] std::span<const std::byte> content() const override {
    return content_data;
  }
  [[nodiscard]] uint64_t content_length() const override {
    return doc.total_len;
  }
  [[nodiscard]] bool is_ram_cache_hit() const override { return is_ram_hit; }
  [[nodiscard]] std::optional<std::span<const std::byte>> mapped_view()
      const override {
    if (!mapping.empty()) {
      return std::span<const std::byte>(mapping);
    }
    return std::nullopt;
  }

  [[nodiscard]] uint64_t content_file_offset() const override {
    if (is_ram_hit || content_data.empty()) return kNoFileOffset;
    const std::byte* base = nullptr;
    if (anchor) {
      base = anchor->mapped_file->persistent_base();
    } else if (auto mf = mapped_file_weak.lock()) {
      base = mf->persistent_base();
    }
    if (base == nullptr) return kNoFileOffset;
    return static_cast<uint64_t>(
        reinterpret_cast<const std::byte*>(content_data.data()) - base);
  }

  bool renew_lease() override {
    if (is_ram_hit || stripe == nullptr) {
      return false;
    }
    // Lease amendment (2026-07-07): pin the Volume (hence its _stripes, the raw
    // `stripe` we deref, and its _mapped_file mapping) for the duration of
    // the call.  A client-paced zero-copy drain can outlive Cache::stop();
    // locking the Volume weak_ptr guarantees `stripe` is not freed
    // mid-renew (the prior raw Volume* + mapped_file_weak.lock() kept the
    // mapping but NOT the Volume/_stripes -> a use-after-free under nginx
    // reload / worker exit).  Expired == cache torn down: nothing to renew.
    if (anchor) {  // strong pin, Volume always reachable
      if (anchor->torn.load(std::memory_order_seq_cst)) {
        return false;  // this handle's stripe generation was torn down
      }
      return anchor->volume->renew_read_lease(stripe, epoch_start);
    }
    auto vol = volume_weak.lock();
    if (!vol) {
      return false;
    }
    return vol->renew_read_lease(stripe, epoch_start);
  }

  LeaseRenewal renew_lease_strict() override {
    // Aliased-path per-send validation.  Same Volume-pin as
    // renew_lease(): a client-paced aliased drain can outlive Cache::stop(),
    // so lock the Volume weak_ptr to keep `stripe` and the mapping alive for
    // the call.  RAM hit / torn-down cache / no stripe: no lease applies.
    if (is_ram_hit || stripe == nullptr) {
      return LeaseRenewal::kLeasesOff;
    }
    if (anchor) {  // strong pin; torn generation => do not alias
      if (anchor->torn.load(std::memory_order_seq_cst)) {
        return LeaseRenewal::kTorn;
      }
      return anchor->volume->renew_read_lease_strict(stripe, epoch_start);
    }
    auto vol = volume_weak.lock();
    if (!vol) {
      return LeaseRenewal::kTorn;  // cache gone mid-drain: do not keep
                                   // aliasing.
    }
    return vol->renew_read_lease_strict(stripe, epoch_start);
  }

  uint64_t ns_until_forced_wrap() const override {
    if (is_ram_hit || stripe == nullptr) {
      return UINT64_MAX;
    }
    if (anchor) {  // strong pin
      if (anchor->torn.load(std::memory_order_seq_cst)) {
        return UINT64_MAX;  // torn generation: no ceiling to report
      }
      return anchor->volume->ns_until_forced_wrap(stripe);
    }
    auto vol = volume_weak.lock();
    if (!vol) {
      return UINT64_MAX;
    }
    return vol->ns_until_forced_wrap(stripe);
  }
};

// Common base class for write handle implementations
// Eliminates code duplication between regular and alternate write handles
class VolumeWriteHandleBase : public WriteHandleImpl {
 public:
  // The Volume is NOT owned by this handle, and the handle can outlive it: an
  // embedder that still holds an open write handle when the Cache is reset or
  // torn down is exactly the case Cache::Impl's volume vector destroys out
  // from under it.  A raw Volume* here read freed memory the moment any
  // operation dereferenced it, which is the same borrow-lifetime fault
  // VolumeReadHandleImpl already carries a weak_ptr to avoid.  So: hold a weak
  // reference and take a strong one for the duration of any operation that
  // touches the Volume -- including `stripe`, which points INTO the Volume's
  // storage and is only valid while the Volume is pinned.  A handle whose
  // Volume is gone fails with CacheError::Closed, the same error the adjacent
  // aborted/closed branches return, so a caller sees a clean failure rather
  // than undefined behaviour.
  //
  // NOT every Volume is shared-owned. Cache::add_volume_locked builds them
  // with make_shared, and those are the ones destroyed under an open handle;
  // a Volume constructed DIRECTLY (as several tests and any standalone
  // embedder do) has no shared owner, so weak_from_this() is empty and there
  // is nothing to track. Its lifetime stays the caller's contract, exactly as
  // before this guard existed, so that case keeps the raw pointer rather than
  // failing every write. `guarded` records which of the two applies.
  std::weak_ptr<Volume> volume_weak;
  Volume* volume_raw = nullptr;
  bool guarded = false;
  Stripe* stripe = nullptr;
  CacheKey key;
  std::vector<std::byte> header;
  std::vector<std::byte> content;
  uint64_t expected_length = 0;
  size_t written = 0;
  bool closed = false;
  bool aborted = false;

  ~VolumeWriteHandleBase() override = default;

  // The Volume to operate on, pinned for the caller's scope when it is
  // shared-owned. `pin` is null in the unguarded case; `ptr` is null only
  // when a shared-owned Volume has already been destroyed -- the stale-handle
  // case every caller reports as CacheError::Closed.
  struct VolumeRef {
    std::shared_ptr<Volume> pin;
    Volume* ptr = nullptr;
    explicit operator bool() const { return ptr != nullptr; }
    Volume* operator->() const { return ptr; }
    Volume& operator*() const { return *ptr; }
  };

  [[nodiscard]] VolumeRef acquire_volume() const {
    if (!guarded) {
      return VolumeRef{nullptr, volume_raw};
    }
    auto pin = volume_weak.lock();
    if (!pin) {
      return VolumeRef{};
    }
    Volume* const ptr = pin.get();
    return VolumeRef{std::move(pin), ptr};
  }

  void set_header(std::span<const std::byte> h) override {
    header.assign(h.begin(), h.end());
  }

  void set_content_length(uint64_t length) override {
    expected_length = length;
  }

  std::expected<size_t, CacheError> write(
      std::span<const std::byte> data) override {
    if (aborted || closed) {
      return make_unexpected(CacheError::Closed);
    }
    // Pin the Volume for this call: it may have been destroyed while this
    // handle stayed open.  Nothing below may touch it (or `stripe`) without
    // this.
    const VolumeRef vol = acquire_volume();
    if (!vol) {
      return make_unexpected(CacheError::Closed);
    }
    // Configured per-object bound, checked on the running total
    // BEFORE anything is buffered, so a streamed write fails at the first
    // chunk that would cross the bound.  Covers both the plain and the
    // alternate write paths (they share this base).  0 = unbounded.
    // When max_object_size is set above the kMaxContentSize guard below
    // (~4 GB), kMaxContentSize fires first with NoSpace instead of
    // ObjectTooLarge — both reject; the ordering only picks which error
    // the caller sees.
    const size_t max_object_size = vol->config().max_object_size;
    if (max_object_size > 0 && written + data.size() > max_object_size) {
      return make_unexpected(CacheError::ObjectTooLarge);
    }
    // Enforce size limit: Document::len is uint32_t, so the maximum
    // content size is bounded.  Also prevents unbounded memory
    // accumulation from a malicious origin that never ends a response.
    constexpr size_t kMaxContentSize =
        std::numeric_limits<uint32_t>::max() - Document::kHeaderSize;
    if (written + data.size() > kMaxContentSize) {
      return make_unexpected(CacheError::NoSpace);
    }
    // Reserve space if we know the expected length to avoid reallocations
    if (content.empty() && expected_length > 0) {
      content.reserve(expected_length);
    }
    content.insert(content.end(), data.begin(), data.end());
    written += data.size();
    return data.size();
  }

  std::expected<void, CacheError> close() override {
    if (aborted) {
      return make_unexpected(CacheError::Closed);
    }
    if (closed) {
      return {};
    }
    // Pin the Volume across the commit, for the same reason write() does.
    // Taken HERE rather than inside each do_commit() override so that a new
    // subclass cannot reintroduce the raw dereference by forgetting to.
    const VolumeRef vol = acquire_volume();
    closed = true;
    if (!vol) {
      return make_unexpected(CacheError::Closed);
    }

    return do_commit(*vol);
  }

  void abort() override { aborted = true; }

  [[nodiscard]] size_t bytes_written() const override { return written; }

 protected:
  // Subclasses implement this to perform the actual commit
  virtual std::expected<void, CacheError> do_commit(Volume& vol) = 0;
};

class VolumeWriteHandleImpl : public VolumeWriteHandleBase {
 protected:
  std::expected<void, CacheError> do_commit(Volume& vol) override {
    return vol.commit_write(stripe, key, std::span<const std::byte>(header),
                            std::span<const std::byte>(content));
  }
};

class VolumeAlternateWriteHandleImpl : public VolumeWriteHandleBase {
 public:
  AlternateId alternate_id = AlternateId::Original;

 protected:
  std::expected<void, CacheError> do_commit(Volume& vol) override {
    return vol.commit_alternate_write(stripe, key, alternate_id,
                                      std::span<const std::byte>(header),
                                      std::span<const std::byte>(content));
  }
};

// Convert a lease config duration to steady-clock ns (0 = disabled).
uint64_t lease_ms_to_ns(std::chrono::milliseconds ms) {
  if (ms.count() <= 0) {
    return 0;
  }
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(ms).count());
}

// Reader stamp for a process-local lease slot (non-mmap stripes):
// CAS-max with the write-avoidance guard, seq_cst — mirrors
// MmapDirectory::stamp_lease_expiry for the shared slot.
void cas_max_lease(std::atomic<uint64_t>& slot, uint64_t new_expiry_ns,
                   uint64_t skip_if_at_least_ns) {
  uint64_t cur = slot.load(std::memory_order_seq_cst);
  while (cur < skip_if_at_least_ns) {
    if (slot.compare_exchange_weak(cur, new_expiry_ns,
                                   std::memory_order_seq_cst,
                                   std::memory_order_seq_cst)) {
      return;
    }
  }
}

// Monotonic high-water-mark update for a telemetry counter.  A plain
// load-compare-store loses a concurrent higher sample (the reader that
// observed the real maximum can be the one whose store is overwritten), so
// the update is a CAS loop and the mark only ever moves up.
void cas_max_counter(std::atomic<uint64_t>& slot, uint64_t candidate) {
  uint64_t cur = slot.load(std::memory_order_relaxed);
  while (candidate > cur) {
    if (slot.compare_exchange_weak(cur, candidate, std::memory_order_relaxed,
                                   std::memory_order_relaxed)) {
      return;
    }
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// fingerprint_cache_path (declared in volume.hpp): encode on-disk FORMAT +
// GEOMETRY into the cache filename so peers with different layouts open
// DIFFERENT files during an overlapping upgrade and never share a ring.
//
// Lives just OUTSIDE the anonymous namespace above so it has external linkage
// for cache.cpp, but pairs with compute_stripe_geometry (visible here via the
// unnamed namespace) which it calls directly for the canonical geometry.
namespace {

// FNV-1a 64-bit.  offset basis / prime are the canonical constants.
constexpr uint64_t kFnv1a64OffsetBasis = 0xcbf29ce484222325ULL;
constexpr uint64_t kFnv1a64Prime = 0x100000001b3ULL;

inline uint64_t fnv1a64_step(uint64_t h, uint8_t byte) {
  return (h ^ static_cast<uint64_t>(byte)) * kFnv1a64Prime;
}

}  // namespace

// Shared fingerprint-name classifier (declared in volume.hpp).  External
// linkage so the name generator's idempotency guard and the superseded-file GC
// classifier share ONE definition of the shape rule.  True iff `stem` ends with
// a "-<one-or-more-digits>-<16 lowercase hex>" tail (manual parse; no <regex>).
bool stem_is_fingerprinted(const std::string& stem) {
  const size_t n = stem.size();
  // Minimum shape: "-" + one digit + "-" + 16 hex == 19 chars.
  if (n < 19) {
    return false;
  }
  const size_t hex_begin = n - 16;
  for (size_t j = hex_begin; j < n; ++j) {
    const char c = stem[j];
    const bool is_hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    if (!is_hex) {
      return false;
    }
  }
  // '-' immediately before the 16-hex hash.
  if (hex_begin < 1 || stem[hex_begin - 1] != '-') {
    return false;
  }
  // One-or-more decimal digits (the format major) before that '-'.
  size_t d = hex_begin - 1;
  while (d > 0 && stem[d - 1] >= '0' && stem[d - 1] <= '9') {
    --d;
  }
  if (d == hex_begin - 1) {
    return false;  // no digits
  }
  // '-' immediately before the digit run.
  return d >= 1 && stem[d - 1] == '-';
}

// The base stem preceding a fingerprint tail (declared in volume.hpp).  Parse
// mirrors stem_is_fingerprinted EXACTLY so the two never disagree on where the
// tail begins.  Returns `stem` unchanged when it carries no fingerprint tail.
std::string fingerprint_base_stem(const std::string& stem) {
  if (!stem_is_fingerprinted(stem)) {
    return stem;
  }
  const size_t n = stem.size();
  const size_t hex_begin = n - 16;  // start of the 16-hex hash
  // Walk the decimal digit run backward (same scan as the classifier).
  size_t d = hex_begin - 1;  // sits on the '-' before the hash
  while (d > 0 && stem[d - 1] >= '0' && stem[d - 1] <= '9') {
    --d;
  }
  // stem[d - 1] is the '-' that opens the tail (guaranteed by the classifier);
  // the base stem is everything strictly before it.
  return stem.substr(0, d - 1);
}

std::string resolve_unsized_cache_path(const std::string& path) {
  namespace fs = std::filesystem;
  const fs::path p(path);
  const std::string stem = p.stem().string();
  const std::string ext = p.extension().string();
  const std::string prefix =
      stem + "-" + std::to_string(VolumeHeader::kFormatVersionMajor) + "-";
  const size_t expected_size = prefix.size() + 16 + ext.size();

  std::error_code ec;
  const fs::path dir = p.has_parent_path() ? p.parent_path() : fs::path(".");
  std::string best;
  fs::file_time_type best_mtime{};
  for (fs::directory_iterator it(dir, ec), end; it != end && !ec;
       it.increment(ec)) {
    const std::string name = it->path().filename().string();
    if (name.size() != expected_size ||
        name.compare(0, prefix.size(), prefix) != 0) {
      continue;
    }
    if (!ext.empty() &&
        name.compare(name.size() - ext.size(), ext.size(), ext) != 0) {
      continue;
    }
    bool all_hex = true;
    for (size_t i = prefix.size(); i < prefix.size() + 16; ++i) {
      const char c = name[i];
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
        all_hex = false;
        break;
      }
    }
    if (!all_hex) {
      continue;
    }
    std::error_code mtime_ec;
    const auto mtime = fs::last_write_time(it->path(), mtime_ec);
    if (mtime_ec) {
      continue;
    }
    const std::string full = it->path().string();
    if (best.empty() || mtime > best_mtime ||
        (mtime == best_mtime && full > best)) {
      best = full;
      best_mtime = mtime;
    }
  }
  return best.empty() ? path : best;
}

std::string fingerprint_cache_path(const std::string& path, size_t size,
                                   size_t stripe_size, bool mmap_directory) {
  namespace fs = std::filesystem;
  const fs::path p(path);
  const std::string stem = p.stem().string();
  const std::string ext = p.extension().string();

  // Unsized mode: a size below the header floor (notably the documented
  // size == 0 "resolve from the file at open()" mode, see Cache::add_volume)
  // has no derivable geometry -- compute_stripe_geometry's `size - kSize`
  // would underflow and hash garbage that even differs by word size.
  // Resolve to the file a sized current-major peer would be using (see
  // resolve_unsized_cache_path), falling back to the raw legacy path.
  if (size < VolumeHeader::kSize) {
    return resolve_unsized_cache_path(path);
  }

  // Idempotency: never re-fingerprint an already-fingerprinted path.
  //
  // Known limitation (inherent to the no-<regex> tail parse): a contrived
  // operator-supplied filename whose stem literally ends in
  // "-<digits>-<16 lowercase hex>" is mistaken for one of our fingerprints and
  // returned unchanged, so that one hand-named cache does not gain the
  // fingerprint's cross-format isolation.  It never collides with a genuinely
  // fingerprinted sibling (whose hash is a 64-bit function of the real
  // geometry), and same-named peers normally carry the same layout the
  // operator gave them.  The residual exposure: same-named MP peers whose
  // configs derive DIFFERENT layouts fall back to the open_locked geometry
  // gate, whose backstop compares stripe_count only (the raw-size clause is
  // dropped there) -- a same-count/different-stripe-size change under such a
  // contrived name is not caught.  Acceptable versus parsing the full grammar
  // here; operators must not hand-name cache files in the fingerprint shape.
  if (stem_is_fingerprinted(stem)) {
    return path;
  }

  const StripeGeometry g = compute_stripe_geometry(size, stripe_size);

  // geohash = FNV-1a 64-bit over a CANONICAL, fixed-width, LITTLE-ENDIAN byte
  // serialization, emitted via EXPLICIT byte extraction (never memcpy a struct,
  // never hash a raw size_t) so the value is stable across compiler, word size
  // and host endianness.  Field ORDER is load-bearing.
  uint64_t h = kFnv1a64OffsetBasis;
  const auto mix_u16_le = [&](uint16_t v) {
    h = fnv1a64_step(h, static_cast<uint8_t>(v & 0xFFu));
    h = fnv1a64_step(h, static_cast<uint8_t>((v >> 8) & 0xFFu));
  };
  const auto mix_u8 = [&](uint8_t v) { h = fnv1a64_step(h, v); };
  const auto mix_u64_le = [&](uint64_t v) {
    for (int i = 0; i < 8; ++i) {
      h = fnv1a64_step(h, static_cast<uint8_t>((v >> (8 * i)) & 0xFFu));
    }
  };

  mix_u16_le(VolumeHeader::kFormatVersionMajor);          // 1. format major
  mix_u8(static_cast<uint8_t>(mmap_directory ? 1 : 0));   // 2. mmap flag
  mix_u64_le(static_cast<uint64_t>(g.num_stripes));       // 3. stripe count
  mix_u64_le(static_cast<uint64_t>(g.base_stripe_size));  // 4. base stripe size
  mix_u64_le(static_cast<uint64_t>(g.stripe_remainder));  // 5. remainder

  // Render the full u64 as 16 lowercase hex chars, most-significant nibble
  // first.
  static constexpr char kHex[] = "0123456789abcdef";
  char geohash[17];
  for (int i = 0; i < 16; ++i) {
    geohash[15 - static_cast<size_t>(i)] = kHex[(h >> (4 * i)) & 0xFu];
  }
  geohash[16] = '\0';

  const std::string new_stem =
      stem + "-" + std::to_string(VolumeHeader::kFormatVersionMajor) + "-" +
      geohash;
  // Rejoin under the ORIGINAL parent (filesystem-correct; empty parent stays a
  // bare relative name).  Multi-dot inputs (e.g. cyclone.dat.small) split on
  // the LAST dot, giving stem "cyclone.dat" + ext ".small" -> a distinct,
  // correct "cyclone.dat-<fmt>-<hash>.small".
  const fs::path result = p.parent_path() / (new_stem + ext);
  return result.string();
}

#ifndef _WIN32
// POSIX opener-side inode re-validation (declared in volume.hpp).  True iff
// `fd` and `path` still resolve to the SAME live inode; false if either stat
// fails, the device/inode differ, or the path's link count is 0 -- meaning the
// file was unlinked or replaced out from under the caller.  Kept tiny and free
// of side effects so open_locked's fail-closed branch, and its unit test, share
// one definition.
bool inodes_match(int fd, const std::string& path) {
  struct stat st_fd{};
  struct stat st_path{};
  if (::fstat(fd, &st_fd) != 0) {
    return false;
  }
  if (::stat(path.c_str(), &st_path) != 0) {
    return false;
  }
  return st_fd.st_dev == st_path.st_dev && st_fd.st_ino == st_path.st_ino &&
         st_fd.st_nlink != 0;
}
#endif

#ifndef _WIN32
// Live backing-file identity from the OPEN fd (declared in volume.hpp).  The GC
// keep-set is built from this kernel {dev,ino}, never from string paths, so a
// symlinked / relative / ".."-laden alias of a live volume can never be
// mistaken for a deletable superseded file.
std::optional<std::pair<dev_t, ino_t>> Volume::backing_identity() const {
  if (_fd < 0) {
    return std::nullopt;
  }
  struct stat st{};
  if (::fstat(_fd, &st) != 0) {
    return std::nullopt;
  }
  return std::make_pair(st.st_dev, st.st_ino);
}
#endif

#ifndef _WIN32
namespace {

// Age floor for GC candidates: skip any file modified within this window of
// now.  This is a RACE-REDUCER, NOT a safety property -- the exclusive
// lifetime-lock probe below is what actually proves a file is unheld.  The
// floor merely spares a brand-new superseded-named file that a peer just
// created but has not yet opened+locked (a narrow open()->lock() window) from
// being probed at all.  Overridable per call for deterministic tests.
constexpr std::chrono::seconds kDefaultGcAgeFloor{60};

}  // namespace

// ===========================================================================
// gc_superseded_volumes  --  opt-in startup GC of SUPERSEDED fingerprint files
// ===========================================================================
//
// Disk hygiene ONLY.  Structural-fingerprint filenames (fingerprint_cache_path)
// make peers with different on-disk layouts open DIFFERENT files, so an upgrade
// leaves the OLD fingerprinted file on disk forever.  This reclaims those, and
// it must NEVER weaken the upgrade-safety the fingerprinting bought.  Each
// candidate is proven safe THREE independent ways before unlink -- keep-set +
// exclusive lifetime-lock probe + inode revalidation + header magic -- and any
// failure on any path is a non-fatal SKIP.  GC must never fail start().
//
// SCOPE / "superseded" is STRUCTURAL, not provenance-based: a candidate is any
// unheld, aged, valid-header, fingerprint-shaped file whose base stem matches a
// live volume ROOTED IN THE SAME DIRECTORY.  GC does not (and cannot cheaply)
// distinguish a file THIS cache wrote earlier from a co-located FOREIGN but
// valid Cyclone volume under a matching name -- both are reclaimed.  The
// base-stem match is scoped PER DIRECTORY (not against any live volume
// anywhere) so a same-base coincidence across two directories cannot make an
// unrelated cache's file in one a candidate on the strength of a live volume in
// the other.  Callers enable this only for a cache-exclusive directory (see the
// CacheConfig::gc_superseded_on_start note).
//
// SAFETY INVARIANTS (each maps to a reviewer-mandated property):
//
//   I1  We unlink ONLY while THIS call holds the volume's EXCLUSIVE lifetime
//       lock (try_lock_lifetime_exclusive == Acquired).  A live peer holds that
//       byte SHARED for the whole life of its open Volume
//       (lock_lifetime_shared, released only by its last close), and
//       shared/exclusive conflict, so an Acquired exclusive PROVES no peer
//       currently holds the file open.  We keep the exclusive lock held THROUGH
//       the unlink, so no peer can open+lock in the gap between the proof and
//       the unlink.
//
//   I2  A DRAINING peer (an old worker still serving mid-upgrade) keeps its
//       inode open and its shared OFD lock held until its last close, so our
//       exclusive probe returns Conflict and we skip -- we never wipe a file a
//       worker is still serving from.
//
//   I3  The REAL protection for a peer that opens the SAME superseded file
//   while
//       GC runs is the lock conflict at its lock_lifetime_shared vs our
//       exclusive probe, plus its own open_locked inode revalidation -- NOT the
//       reset-gate exclusive probe inside open_locked (a same-format opener has
//       needs_reset == false and never reaches that probe).  GC couples to
//       inodes_match for exactly the name->inode TOCTOU that revalidation
//       covers on the opener side.
//
//   I4  A legacy un-fingerprinted cyclone.dat is never even a candidate: its
//       stem fails stem_is_fingerprinted, so we skip it before any fd work.
//
//   I5  Live volumes -- INCLUDING the ".small" sibling, which is its own entry
//       in the live set -- are never candidates: their inode is in the keep-set
//       (skipped at classification) and, even if a backing_identity() lookup
//       failed, they still hold the shared lifetime lock so the exclusive probe
//       would Conflict (I1/I2).  Two independent guards.
//
// POSIX-ONLY, and that is a deliberate SCOPE choice -- NOT a claim that Windows
// has no live peers.  It once was: this function used to argue that an
// MSI/package upgrade fully stops every old worker before the new binary opens
// the cache.  That is false (a w3wp can outlive a WAS stop, and an overlapped
// recycle deliberately runs two), which is exactly why the lifetime lock is now
// implemented on Windows too.
//
// GC stays compiled out on Windows because the safety argument it needs is not
// established there, not because it would be pointless.  Specifically: the
// Windows open path drops its exclusive probe before taking the shared lock (no
// atomic downgrade), leaving a window in which a GC that took the exclusive
// lock would wrongly conclude a LIVE volume is unheld -- and the POSIX
// opener-side inodes_match revalidation that backstops precisely this TOCTOU is
// itself POSIX-only.  Enabling GC on Windows therefore requires closing that
// downgrade gap and providing an inode-revalidation equivalent first; see the
// WINDOWS DOWNGRADE GAP note in open_locked.
void gc_superseded_volumes(const std::vector<Volume*>& live_volumes,
                           std::chrono::seconds age_floor) {
  namespace fs = std::filesystem;

  // (a) Build the GLOBAL keep-set K of live backing identities, plus a
  //     PER-DIRECTORY map from each directory to the base stems of the live
  //     volumes ROOTED IN IT.  The keep-set is global because a live inode is
  //     live regardless of which name resolves to it, but the base-stem match
  //     MUST be scoped per directory: a candidate in directory B may share a
  //     base stem with a live volume that lives only in directory A, and it is
  //     NOT superseded-by-us just because an unrelated cache elsewhere happens
  //     to use the same base name.  Two independent global sets would let such
  //     a cross-directory base-stem coincidence make a valid FOREIGN Cyclone
  //     volume in B a candidate (the lock + keep-set still stop a LIVE/held
  //     file, but we must not even consider an unrelated cache's file).
  std::set<std::pair<dev_t, ino_t>> keep;  // K: identities we must never delete
  std::map<fs::path, std::set<std::string>>
      dir_base_stems;  // dir -> base stems of live volumes rooted THERE
  for (const Volume* v : live_volumes) {
    if (v == nullptr) {
      continue;
    }
    if (auto id = v->backing_identity()) {
      keep.insert(*id);
    }
    const fs::path vpath(
        v->config().path);  // public accessor (_config private)
    fs::path parent = vpath.parent_path();
    if (parent.empty()) {
      parent = fs::path(".");
    }
    dir_base_stems[parent].insert(fingerprint_base_stem(vpath.stem().string()));
  }
  // Fail closed if we learned no live identity or no directory/base stem:
  // without a keep-set we cannot prove a candidate is not one of the live
  // files.  (The per-file lock probe would still protect a live file, but
  // refusing here is strictly safer and costs nothing -- start() always opens
  // the volumes first.)
  if (keep.empty() || dir_base_stems.empty()) {
    return;
  }

  const auto now = std::chrono::system_clock::now();

  for (const auto& [dir, base_stems] : dir_base_stems) {
    std::error_code ec;
    // ERROR_CODE overload: never throws (a GC that threw would fail start()).
    fs::directory_iterator it(dir, ec);
    const fs::directory_iterator end;
    if (ec) {
      continue;  // unreadable directory -> skip it
    }
    for (; it != end; it.increment(ec)) {
      if (ec) {
        break;  // iteration error -> abandon this directory, do not throw
      }
      const fs::path f = it->path();

      // --- Classify (cheap, no fd) --------------------------------------
      // (a) fingerprint NAME shape: strip the FINAL extension, require the
      //     remainder to end "-<digits>-<16 hex>".  (I4: legacy names fail.)
      const std::string stem = f.stem().string();
      if (!stem_is_fingerprinted(stem)) {
        continue;
      }
      // (b) base stem must match a live volume ROOTED IN THIS directory (not
      //     merely any live volume anywhere -- see the per-dir map above).
      if (base_stems.find(fingerprint_base_stem(stem)) == base_stems.end()) {
        continue;
      }
      // (c) not a live inode (keep-set).  stat by NAME here; the authoritative
      //     re-check is inodes_match on the locked fd below.
      struct stat st{};
      if (::stat(f.c_str(), &st) != 0) {
        continue;  // vanished / raced away
      }
      if (keep.find(std::make_pair(st.st_dev, st.st_ino)) != keep.end()) {
        continue;  // I5: a live volume's own file
      }
      // (d) age floor (race-reducer only; see kDefaultGcAgeFloor).
      if (age_floor.count() > 0) {
        const auto mtime = std::chrono::system_clock::from_time_t(st.st_mtime);
        if (now - mtime < age_floor) {
          continue;
        }
      }

      // --- Prove-safe-then-delete ---------------------------------------
      // O_RDWR (NOT O_RDONLY): an O_RDONLY fd rejects the F_WRLCK exclusive
      // probe with EBADF, which we would misread as "no lock support".  Any
      // open failure (any errno) -> skip.
      const int fd = CYCLONE_OPEN(f.c_str(), O_RDWR);
      if (fd < 0) {
        continue;
      }

      const LockResult probe = try_lock_lifetime_exclusive(fd);
      // POLARITY FOOTGUN (deliberate -- the OPPOSITE of the reset gate): the
      // reset gate in open_locked treats LockResult::Unsupported as "advisory
      // locking is proven absent -> DEGRADE -> ALLOW the reset".  GC treats the
      // SAME enum value as its opposite: Unsupported means we CANNOT PROVE that
      // no peer holds this file, so we FAIL CLOSED and SKIP (keep it).  Only an
      // Acquired exclusive lock proves the file is unheld; Conflict means a
      // live peer holds the shared lock (skip); the persistent-unsupported
      // errno classification (ofd_errno_is_persistent_unsupported) is already
      // folded into try_lock_lifetime_exclusive, which returns Unsupported ONLY
      // for a proven no-lock medium and Conflict for every ambiguous errno.
      if (probe == LockResult::Acquired) {
        // Holding the EXCLUSIVE lifetime lock (I1): no peer holds the shared
        // one.  Two more proofs, both REQUIRED, before we unlink:
        //   (i) name->inode TOCTOU (I3): the lock proof is on fd's inode, but
        //       unlink is BY NAME.  Require that f still resolves to fd's inode
        //       with nlink>0 (inodes_match), so a file swapped in at this name
        //       after our stat/open is never the one we delete.
        //  (ii) header magic: a FOREIGN look-alike that merely matches the name
        //       shape and holds no lock must never be deleted -- require a
        //       valid VolumeHeader (the same magic check read_header uses).
        if (inodes_match(fd, f.string())) {
          std::byte buf[VolumeHeader::kSize];
          const ssize_t nread = ::pread(fd, buf, VolumeHeader::kSize, 0);
          if (nread == static_cast<ssize_t>(VolumeHeader::kSize) &&
              VolumeHeader::deserialize(buf).is_valid()) {
            // Unlink WHILE STILL HOLDING the exclusive lock (I1): closes the
            // proof->unlink gap for every actor honoring the open+lock
            // protocol.  (A lockless rename() onto this NAME could still swap
            // the target -- the library never renames volume files, and
            // external renamers are excluded by the cache-exclusive-directory
            // contract on gc_superseded_on_start.)  Failure is non-fatal
            // (best-effort hygiene).
            (void)::unlink(f.c_str());
          }
        }
      }
      // Conflict / Unsupported / anything else -> skip (fail closed).  Closing
      // fd releases any exclusive lock we took (last close of this OFD).
      CYCLONE_CLOSE(fd);
    }
  }
}
#endif  // !_WIN32

// VolumeHeader implementation — sizeof(VolumeHeader) == kSize (64 bytes),
// verified by static_assert in volume.hpp, so single-memcpy is safe.
void VolumeHeader::serialize(std::byte* buffer) const {
  std::memcpy(buffer, this, kSize);
}

VolumeHeader VolumeHeader::deserialize(const std::byte* buffer) {
  VolumeHeader header;
  std::memcpy(&header, buffer, kSize);
  return header;
}

Volume::Volume(VolumeConfig config)
    : _config(std::move(config)),
      _mp_config(),
      _lease_t_ns(lease_ms_to_ns(_config.read_lease_duration)),
      _lease_ceiling_ns(lease_ms_to_ns(_config.lease_wrap_ceiling)) {}

Volume::Volume(VolumeConfig config, const MultiProcessConfig& mp_config)
    : _config(std::move(config)),
      _mp_config(mp_config),
      _lease_t_ns(lease_ms_to_ns(_config.read_lease_duration)),
      _lease_ceiling_ns(lease_ms_to_ns(_config.lease_wrap_ceiling)) {}

Volume::~Volume() { close(); }

std::expected<void, CacheError> Volume::open() {
  if (_fd >= 0) {
    return make_unexpected(CacheError::AlreadyOpen);
  }
  _teardown.store(false, std::memory_order_seq_cst);  // (Re)opening.

  // Two-stage open to prevent TOCTOU race when multiple processes initialize
  // the same volume file concurrently.  O_EXCL ensures only one process
  // creates the file; others see EEXIST and open the existing file.
  bool created_new = false;
#ifdef _WIN32
  _fd = CYCLONE_OPEN(_config.path.c_str(), CYCLONE_OPEN_FLAGS | O_EXCL,
                     CYCLONE_OPEN_MODE);
#else
  _fd = CYCLONE_OPEN(_config.path.c_str(), O_RDWR | O_CREAT | O_EXCL,
                     CYCLONE_OPEN_MODE);
#endif
  if (_fd >= 0) {
    created_new = true;
  } else if (errno == EEXIST) {
    // File already exists — open normally without O_CREAT to avoid racing
    // another process's initialization.  We re-add O_CREAT as a fallback so
    // that the call matches the original semantics for corner cases (e.g. the
    // file was deleted between our two open() calls).
    _fd = CYCLONE_OPEN(_config.path.c_str(), CYCLONE_OPEN_FLAGS,
                       CYCLONE_OPEN_MODE);
  }

  if (_fd < 0) {
    return make_unexpected(CacheError::IoError);
  }

  _reset_gate_degraded.store(false, std::memory_order_relaxed);

  // the two-stage open above only picks the CREATOR of the file
  // -- it does not order a non-creator against the creator's initialization
  // sequence (ftruncate -> reset() -> init_stripes()).  Without further
  // synchronization a non-creator can observe any prefix of that sequence:
  // read a not-yet-written header and run a SECOND reset() that zero-fills
  // the shared directory underneath the creator's live mmap, race
  // MmapDirectory::init against the creator's zero-fill, or fstat a 0-byte
  // file.  Hold an exclusive lock across the whole validate + initialize
  // sequence: the creator initializes under the lock, non-creators block
  // until it is done and then observe a fully initialized volume.
  //
  // Crash safety: the kernel releases the lock when its holder exits or
  // crashes, so a creator dying mid-init cannot wedge future openers.  The
  // next opener then repairs the partial state under this lock: reset()
  // publishes the volume header only AFTER the directory region is zeroed
  // (header-last, see reset()), so a torn init reads as a missing/invalid
  // header => rerun reset(); and init_stripes() open-or-inits each stripe
  // directory, so a zeroed directory region behind a valid header is
  // rebuilt.
  const InitLockKind init_lock = acquire_init_lock(_fd);
  if (init_lock == InitLockKind::None) {
    close();
    return make_unexpected(CacheError::IoError);
  }

  auto result = open_locked(created_new);
  release_init_lock(_fd, init_lock);
  if (!result) {
    close();
  }
  return result;
}

// Body of open() that runs under the exclusive init lock: size
// validation/allocation, header validation, reset when needed, and stripe
// (directory) initialization.  Error paths return without cleanup; open()
// releases the lock and closes the volume on failure.
std::expected<void, CacheError> Volume::open_locked(bool created_new) {
  CYCLONE_STAT_STRUCT st;
  if (CYCLONE_FSTAT(_fd, &st) < 0) {
    return make_unexpected(CacheError::IoError);
  }

  if (_config.size == 0) {
    _config.size = st.st_size;
  }

  if (_config.size == 0) {
    return make_unexpected(CacheError::InvalidArgument);
  }

  // ORDER MATTERS.  The header decision comes FIRST -- before the
  // pre-allocating ftruncate and before the mmap: read_header()
  // preads through _fd, so it needs neither the mapping nor the allocation;
  // reset() likewise pwrites through _fd only.
  //
  // Check volume header for version compatibility
  bool needs_reset = false;
  if (created_new) {
    // We just created this file — it definitely needs initialization.
    needs_reset = true;
  } else {
    auto header_result = read_header();
    if (!header_result) {
      // No valid header - treat as incompatible (new or corrupted file)
      needs_reset = true;
    } else {
      const VolumeHeader& header = *header_result;
      // Multi-process geometry guard: stripe layout (count, offsets, and the
      // shared mmap directory positions) is derived from the volume size, so
      // processes opening the SAME file with divergent geometry would corrupt
      // the shared directory.  As of the structural-fingerprint filenames
      // (fingerprint_cache_path), format+geometry are encoded in the FILENAME,
      // so geometry divergence is now prevented UPSTREAM -- differing layouts
      // resolve to different files and never reach this guard on one inode.
      // The check below is retained as a defence-in-depth backstop only.
      // Single-process mode: fingerprinting applies there too, so a
      // geometry-CHANGING resize now resolves to a DIFFERENT file (fresh cold
      // cache; the old file is orphaned).  The historical graceful-shrink
      // behavior survives only for raw-size drift WITHIN one derived layout
      // (e.g. an explicit-stripe band), where peers still open the same file
      // (in-memory directories are rebuilt per run; self-contained stripes
      // tolerate the tail change).
      // Persisted stripe_count is authoritative (v5+): if the count this
      // build would derive disagrees with what created the volume, the
      // on-disk per-stripe directory offsets no longer line up -- reset
      // rather than read a mp directory at the wrong offset.  0 = pre-v5
      // (unrecorded); those are already caught by is_compatible() above.
      const size_t derived_count =
          compute_stripe_geometry(_config.size, _config.stripe_size)
              .num_stripes;
      // With structural-fingerprint filenames (see fingerprint_cache_path),
      // any two configs that resolve to the SAME on-disk file have IDENTICAL
      // derived geometry by construction, so a raw `volume_size != size`
      // comparison would spuriously reset/refuse two same-layout peers that
      // differ only in raw size -- a whole 128MB band maps to one layout in
      // explicit-stripe mode.  The raw-size clause is therefore DROPPED.  The
      // persisted stripe_count clause stays as the hash-collision /
      // pre-fingerprint-legacy backstop: 0 = unrecorded (pre-v5, already caught
      // by is_compatible()); a nonzero disagreement means the on-disk
      // per-stripe directory offsets no longer line up -> reset rather than
      // read at the wrong offset.
      const bool geometry_mismatch =
          _mp_config.enabled &&
          (header.stripe_count != 0 && header.stripe_count != derived_count);
      if (!header.is_compatible() || geometry_mismatch) {
        if (!_config.auto_reset_on_incompatible) {
          return make_unexpected(CacheError::IncompatibleVersion);
        }
        needs_reset = true;
      }
    }
  }

  // --- The reset gate: NEVER reset under a live peer -----------------------
  // The init lock we hold here orders OPENERS only -- a process that already
  // has this volume open and mapped is invisible to it -- so probe for the
  // exclusive lifetime lock (see the lock substrate above).  The probe is
  // deliberately CONDITIONAL on needs_reset; the substrate comment explains
  // why that matters for the post-fork case.
  bool holding_exclusive = false;
  ResetProvenance provenance = ResetProvenance::NotNeeded;
  if (needs_reset && created_new) {
    // F1: a freshly O_CREAT'd inode cannot be shared with anyone -- the init
    // lock we hold serialized us against every other opener, and O_EXCL means
    // WE created this inode.  Skip the peer probe and the refuse logic
    // entirely and just initialize the empty file below.  Without this
    // shortcut, `rm cyclone.dat` + reload would create a new inode, read no
    // header, and fall into the live-peer refuse path -- BRICKING the new
    // master on a routine cache wipe.
    // (holding_exclusive stays false: there is no peer, so no lock to take
    // around the reset; the shared lifetime lock is acquired after reset.)
    provenance = ResetProvenance::FreshInode;
  } else if (needs_reset) {
    const LockResult ex = try_lock_lifetime_exclusive(_fd);
    if (ex == LockResult::Unsupported) {
      // The reset gate FAILS CLOSED on ambiguity: try_lock_lifetime_exclusive
      // returns Unsupported ONLY for a PROVEN no-lock-support medium (POSIX:
      // EINVAL / ENOTSUP / EOPNOTSUPP; Win32: ERROR_NOT_SUPPORTED /
      // ERROR_INVALID_FUNCTION), never for a transient/ambiguous status like
      // ENOLCK or ERROR_INVALID_PARAMETER (those come back as Conflict ->
      // refuse).  So reaching here means byte-range locking genuinely does not
      // exist on this filesystem (NFS/overlay, or an exotic Win32 redirector);
      // DEGRADE to the historical ungated behaviour -- allow the reset rather
      // than refuse to boot -- since with no locks there is provably no peer to
      // protect.  Surfaced via stats().
      _reset_gate_degraded.store(true, std::memory_order_relaxed);
      provenance = ResetProvenance::GateDegraded;
    } else if (ex == LockResult::Acquired) {
      // No other process holds the volume: safe to wipe it.  Keep the
      // exclusive lock until reset() has run, then downgrade to shared.
      holding_exclusive = true;
      provenance = ResetProvenance::GateVerified;
    } else {
      // A live peer (an old worker still serving, e.g. mid Apache `graceful`
      // reload across a format/geometry bump) has the volume open and mapped.
      // NEVER reset under a live peer: that wipes the shared directory and the
      // lock bytes out from under a process that is actively serving.  We do
      // not coexist/adopt the peer's layout either (that machinery was more
      // risk than it bought) -- REFUSE TO OPEN at the new format and let the
      // embedder serve pass-through until the peers drain, at which point the
      // next opener probes clean and resets.
      //
      // Reachability: needs_reset is only set on a format/geometry change, so a
      // normal same-version graceful reload never reaches here.  Beyond that,
      // do NOT assume this path is exotic.  It is reachable on every port that
      // can overlap an old- and new-format process on one cyclone.dat: Apache
      // `graceful`, and -- now that the gate actually exists on Windows -- IIS,
      // where an overlapped recycle or a w3wp that outlives a WAS stop leaves a
      // live peer holding the volume across an upgrade.  On IIS this refusal is
      // a PRIMARY reachable path, not a theoretical one.
      //
      // ResetRefusedLivePeer, deliberately NOT IncompatibleVersion: this is the
      // ONLY observability channel a refusal has.  A refused open makes open()
      // close() the volume and Cache::start() close every volume and fail, so
      // the Cache never starts and no stats reader can ever observe a counter
      // for this event -- the error string is all an operator ever sees (the
      // embedder logs it verbatim).  "cache format version incompatible" would
      // MISDIAGNOSE it: the format is fine, a live peer is holding the volume
      // and the cure is to drain the peer, not to touch the format.
      return make_unexpected(CacheError::ResetRefusedLivePeer);
    }
  }

  // --- Exclusive open: repair a crashed writer's wrap state ----------------
  // A multi-process opener that finds NO live peer (the exclusive lifetime
  // probe succeeds) may reset the shared reader-exclusion state that dead
  // processes left behind: a stuck wrap_intent (a writer killed inside its
  // wrap window makes every read of the stripe miss for a whole pass),
  // leaked borrow counts, a stale lease, and a phase that drifted from the
  // pass count (see repair_wrap_state).  The probe is safe to run on every
  // such open, unlike the reset gate's: a Conflict here only means "not
  // exclusive" and the open proceeds without the repair -- it never refuses.
  // A fresh inode (created_new) or a reset volume has nothing to repair.
  // (A same-fork-family opener conflicts with its own family's shared lock;
  // it simply skips the repair, which is the correct answer: the family is
  // live.)
  bool exclusive_open = created_new;
  if (!needs_reset && !created_new && _mp_config.enabled) {
    if (try_lock_lifetime_exclusive(_fd) == LockResult::Acquired) {
      holding_exclusive = true;
      exclusive_open = true;
    }
  }

  // Pre-allocate the file to the configured size.  On POSIX this uses
  // ftruncate(); on Windows it uses _chsize_s() which also extends files.
  // _chsize_s returns 0 on success and a positive errno on failure (unlike
  // ftruncate which returns -1), so we use != 0 for cross-platform compat.
  //
  if (static_cast<size_t>(st.st_size) < _config.size) {
    if (CYCLONE_FTRUNCATE(_fd, _config.size) != 0) {
      if (holding_exclusive) {
        unlock_lifetime(_fd);
      }
      return make_unexpected(CacheError::IoError);
    }
  }

  // reset() pwrites through _fd; it needs the file at full size (above) but
  // not the mapping (below).  Runs while we still hold the EXCLUSIVE lifetime
  // probe (holding_exclusive), or -- for created_new / degraded -- with no
  // peer possible.
  if (needs_reset) {
    auto reset_result = reset();
    if (!reset_result) {
      if (holding_exclusive) {
        unlock_lifetime(_fd);
      }
      return reset_result;
    }
    // Count only a reset that ACTUALLY WIPED data (past the early return
    // above), and key off the provenance recorded at the gate -- never off
    // _reset_gate_degraded, which the F1/fresh-inode path also sets later at
    // the shared acquire (see ResetProvenance).  FreshInode and NotNeeded count
    // nothing: an empty inode we just created had no peer to endanger.
    if (provenance == ResetProvenance::GateDegraded) {
      _resets_under_degraded_gate.fetch_add(1, std::memory_order_relaxed);
    } else if (provenance == ResetProvenance::GateVerified) {
      _resets_gate_verified.fetch_add(1, std::memory_order_relaxed);
    }
  }

  _mapped_file = MappedFile::create();
  if (!_mapped_file) {
    return make_unexpected(CacheError::InternalError);
  }

  auto open_result =
      _mapped_file->open(_config.path, MappedFile::OpenMode::ReadWrite);
  if (!open_result) {
    return make_unexpected(CacheError::IoError);
  }

  // Map the entire volume file persistently.  All subsequent map_region()
  // calls become pointer arithmetic (zero syscalls).  Failure is non-fatal:
  // falls back to per-region mapping (e.g., on 32-bit systems).
  (void)_mapped_file->map_whole_file(MappedFile::MapMode::ReadWrite);

  // Hold the SHARED lifetime lock for the whole life of this open Volume, so
  // any future opener that wants to reset sees us and refuses.  On POSIX this
  // atomically REPLACES a held exclusive probe (holding_exclusive) with the
  // shared lock -- no release gap, and we still hold the init lock so no opener
  // can slip in.  Windows has no atomic downgrade, so drop the EX first (still
  // under the init lock).  Skipped entirely when the gate degraded (no advisory
  // locking on this fs).
  //
  // WINDOWS DOWNGRADE GAP -- currently safe, but safe BY ACCIDENT.  Between the
  // UnlockFileEx below and the shared acquire that follows, this volume holds
  // NO lifetime lock at all.  Nothing exploits that window today because:
  //   (a) OPENERS cannot slip in: we hold the init lock across the whole of
  //       open_locked (see open()), so every other opener is blocked; and
  //   (b) the ONE actor that probes the lifetime byte WITHOUT the init lock --
  //       gc_superseded_volumes -- is compiled out on Windows (it lives inside
  //       an #ifndef _WIN32 block).
  // (b) is a compile fence in a DIFFERENT part of this file, not a property of
  // the code here, and Windows additionally lacks the POSIX inodes_match
  // backstop below (also #ifndef _WIN32).  IF GC IS EVER ENABLED ON WINDOWS,
  // THIS GAP BECOMES A REAL WIPE-UNDER-PEER WINDOW WITH NO BACKSTOP: a GC
  // probing this file during the gap would acquire the exclusive lock, conclude
  // the file is unheld, and unlink it out from under this live open -- and no
  // inode revalidation would catch it.  Anyone enabling GC on Windows must
  // close this gap FIRST (e.g. hold the init lock as the interlock and have GC
  // take it too, or acquire the shared lock on a second handle before dropping
  // the EX on this one).
  if (!_reset_gate_degraded.load(std::memory_order_relaxed)) {
#ifdef _WIN32
    if (holding_exclusive) {
      unlock_lifetime(_fd);
    }
#endif
    const LockResult life = lock_lifetime_shared(_fd);
    if (life == LockResult::Unsupported) {
      // Locking turned out to be unsupported at shared-acquire time (e.g. the
      // created_new / degraded paths that never probed).  Degrade rather than
      // fail: an unlocked open is the historical behaviour.
      _reset_gate_degraded.store(true, std::memory_order_relaxed);
    } else if (life != LockResult::Acquired) {
      // A Conflict here means an EXCLUSIVE holder exists on this byte.  Under
      // the init lock only shared holders (peers) can race us -- and shared
      // locks stack -- but gc_superseded_volumes takes the exclusive lifetime
      // lock WITHOUT the init lock, so another process's GC probing this file
      // (even a candidate it will end up skipping) can legitimately Conflict
      // this shared acquire.  Fail closed either way rather than proceed
      // without the lifetime lock: if the racer was GC and it unlinks us, the
      // inode re-validation below refuses the orphaned inode on retry; if it
      // skips us, the very next open succeeds.  Transient, availability-only,
      // never a corruption path -- the embedder serves pass-through and
      // retries.
      return make_unexpected(CacheError::IoError);
    }
  }

#ifndef _WIN32
  // Opener-side inode re-validation (defense-in-depth; POSIX only).  We now
  // hold the SHARED lifetime lock on _fd, but the lifetime lock protects the
  // OPEN INODE, not the NAME: between our open() and here, the path could have
  // been unlinked and a fresh file created in its place (a manual `rm
  // cyclone.dat` / `--reset`, or a future GC of superseded fingerprinted
  // files).  If so, _fd now points at a dead/replaced inode while other openers
  // attach to the NEW one -> split-brain on two rings behind one name.  Fail
  // CLOSED so the embedder falls back to pass-through rather than serving from
  // an orphaned inode.  The normal create/attach paths leave the name pointing
  // at exactly this inode with nlink>=1, so this never fires there.
  if (!inodes_match(_fd, _config.path)) {
    return make_unexpected(CacheError::IoError);
  }
#endif

  return init_stripes(exclusive_open);
}

void Volume::close() {
  // Publish teardown BEFORE freeing the stripes: handle-facing paths that
  // dereference a raw Stripe* (release_borrow, renew, ns_until_forced_wrap)
  // check this and bail (see the field's comment in volume.hpp).
  _teardown.store(true, std::memory_order_seq_cst);
  _stripes.clear();
  _mapped_file.reset();

  // Closing _fd releases the SHARED lifetime lock -- but only when this is the
  // LAST close of the OFD.  That is deliberate and load-bearing: an
  // nginx/Apache master that forked shares one OFD with its workers, so the
  // lock (and the reset protection) persists until the last worker exits.  The
  // lifetime lock is NEVER explicitly unlocked; an explicit unlock on one
  // worker's graceful close would drop it for the whole family (the bug that
  // made a graceful reload wipe the cache).
  if (_fd >= 0) {
    CYCLONE_CLOSE(_fd);
    _fd = -1;
  }
}

std::expected<VolumeHeader, CacheError> Volume::read_header() const {
  if (_fd < 0) {
    return make_unexpected(CacheError::NotInitialized);
  }

  std::byte buffer[VolumeHeader::kSize];

  // Read header from file start
#ifdef _WIN32
  if (CYCLONE_LSEEK(_fd, 0, SEEK_SET) != 0) {
    return make_unexpected(CacheError::IoError);
  }
  ssize_t bytes_read = CYCLONE_READ(_fd, buffer, VolumeHeader::kSize);
#else
  ssize_t bytes_read = pread(_fd, buffer, VolumeHeader::kSize, 0);
#endif

  if (bytes_read < 0) {
    return make_unexpected(CacheError::IoError);
  }

  if (static_cast<size_t>(bytes_read) < VolumeHeader::kSize) {
    // File too small to have a valid header
    return make_unexpected(CacheError::Corrupted);
  }

  VolumeHeader header = VolumeHeader::deserialize(buffer);
  if (!header.is_valid()) {
    return make_unexpected(CacheError::Corrupted);
  }

  return header;
}

std::expected<void, CacheError> Volume::write_header(
    const VolumeHeader& header) const {
  if (_fd < 0) {
    return make_unexpected(CacheError::NotInitialized);
  }

  std::byte buffer[VolumeHeader::kSize];
  header.serialize(buffer);

#ifdef _WIN32
  if (CYCLONE_LSEEK(_fd, 0, SEEK_SET) != 0) {
    return make_unexpected(CacheError::IoError);
  }
  ssize_t bytes_written = CYCLONE_WRITE(_fd, buffer, VolumeHeader::kSize);
#else
  ssize_t bytes_written = pwrite(_fd, buffer, VolumeHeader::kSize, 0);
#endif

  if (bytes_written < 0 ||
      static_cast<size_t>(bytes_written) != VolumeHeader::kSize) {
    return make_unexpected(CacheError::IoError);
  }

  return {};
}

std::expected<void, CacheError> Volume::reset() {
  if (_fd < 0) {
    return make_unexpected(CacheError::NotInitialized);
  }

  // Minimum size: must fit at least the volume header
  if (_config.size <= VolumeHeader::kSize) {
    return make_unexpected(CacheError::InvalidArgument);
  }

  // Clear stripes (they'll be reinitialized by init_stripes)
  _stripes.clear();

  // Zero-fill EVERY stripe's directory region.
  //
  // This used to zero ONE directory's worth of bytes at VolumeHeader::kSize --
  // i.e. stripe 0's directory only.  Production volumes are multi-stripe (the
  // auto geometry gives a 512MB volume 16 stripes), so the MmapDirectory
  // structures in stripes 1..N-1 survived the "reset" with their MDIR magic,
  // their entries AND their shared_write_pos intact; init_stripes() then
  // re-ADOPTED them and the reset volume happily kept serving pre-reset
  // documents.  Only multi-process (mmap-directory) volumes have a persistent
  // on-disk directory -- single-process volumes rebuild an in-memory Directory
  // on every open -- so the bug bit exactly the production configuration.
  //
  // _stripes was just cleared, so the geometry is recomputed from _config
  // rather than read back off the (now empty) stripe vector.  Same helper
  // init_stripes() uses, so the two cannot disagree about the layout.
  //
  // Cost: a 512MB/16-stripe multi-process volume zeroes 16 x ~720KB ~= 11.3MB,
  // once, on an incompatible/geometry-changed open.  That is fine.
  //
  // The DATA area deliberately stays untouched: with the directory gone,
  // shared_write_pos is zeroed too, init_stripes() re-seeds write_pos to
  // data_offset, and the phase-ABA positional guard rejects every stale
  // document as ahead-of-cursor.  Zeroing the whole volume would turn a reset
  // into a multi-GB write for no added safety.
  const size_t dir_size =
      _mp_config.enabled
          ? MmapDirectory::required_size(kDirectoryEntriesPerSegment)
          : kDirectoryEntriesPerSegment * DirEntry::kSize;

  const StripeGeometry geom =
      compute_stripe_geometry(_config.size, _config.stripe_size);

  // Write in 64KB chunks to avoid a single huge allocation.
  constexpr size_t kChunkSize = static_cast<const size_t>(64 * 1024);
  std::vector<std::byte> zeros(std::min(kChunkSize, dir_size), std::byte{0});

  uint64_t stripe_offset = VolumeHeader::kSize;
  for (size_t i = 0; i < geom.num_stripes; ++i) {
    const size_t stripe_size =
        geom.base_stripe_size +
        (i + 1 == geom.num_stripes ? geom.stripe_remainder : 0);

    // Clamp twice: a directory can never exceed its own stripe, and no write
    // may run past the end of the file (paranoia for tiny/odd test volumes,
    // where a stripe can be smaller than a full directory).
    size_t remaining = std::min(dir_size, stripe_size);
    if (stripe_offset >= _config.size) {
      break;
    }
    remaining = std::min<size_t>(remaining, _config.size - stripe_offset);

    uint64_t clear_offset = stripe_offset;
    while (remaining > 0) {
      const size_t to_write = std::min(remaining, zeros.size());
#ifdef _WIN32
      if (CYCLONE_LSEEK(_fd, clear_offset, SEEK_SET) !=
          static_cast<int64_t>(clear_offset)) {
        return make_unexpected(CacheError::IoError);
      }
      ssize_t bytes_written = CYCLONE_WRITE(_fd, zeros.data(), to_write);
#else
      ssize_t bytes_written =
          pwrite(_fd, zeros.data(), to_write, static_cast<off_t>(clear_offset));
#endif
      // <= 0, not < 0: a 0-byte write makes no progress, so treating it as
      // success (as this loop used to) would spin here forever.
      if (bytes_written <= 0) {
        return make_unexpected(CacheError::IoError);
      }
      clear_offset += static_cast<size_t>(bytes_written);
      remaining -= static_cast<size_t>(bytes_written);
    }

    stripe_offset += stripe_size;
  }

  // publish the header LAST, after the directory region has
  // been zeroed, making a valid header an init-complete marker for reset():
  // a creator that dies mid-reset leaves a missing/invalid header behind,
  // and the next opener (serialized by the open() init lock) simply reruns
  // reset().  A crash between reset() and the end of init_stripes() is
  // likewise recoverable: init_stripes() open-or-inits each stripe
  // directory, so a zeroed directory region behind a valid header is
  // rebuilt on the next open.
  VolumeHeader header;
  header.magic = VolumeHeader::kMagic;
  header.format_version_major = VolumeHeader::kFormatVersionMajor;
  header.format_version_minor = VolumeHeader::kFormatVersionMinor;
  header.creation_time = static_cast<uint64_t>(std::time(nullptr));
  header.volume_size = _config.size;
  header.directory_buckets = kDirectoryEntriesPerSegment;
  header.mmap_directory = _mp_config.enabled ? 1 : 0;
  const StripeGeometry reset_geom =
      compute_stripe_geometry(_config.size, _config.stripe_size);
  header.stripe_count = reset_geom.num_stripes;

  auto write_result = write_header(header);
  if (!write_result) {
    return write_result;
  }

  // Sync to ensure the header and zeroed directory are on disk
  fsync_fd();

  return {};
}

std::expected<void, CacheError> Volume::init_stripes(bool exclusive) {
  // Account for volume header at the start
  size_t usable_size = _config.size - VolumeHeader::kSize;

  // Stripe count matters for lease-based region pinning: wraps gate at
  // STRIPE granularity, so a single-stripe volume lets one live read borrow
  // block every write to the whole cache (writes dropped -> hit rate
  // collapses).  Spreading across stripes keeps a hot borrow local to its own
  // stripe.
  //
  // Every stripe offset MUST stay 8-byte aligned: the mmap directory places
  // seq_cst uint64_t atomics at a fixed offset within each stripe, and a
  // misaligned atomic faults (SIGBUS on arm64).  The volume header is a whole
  // number of 8-byte words, so offsets stay aligned as long as each stripe's
  // size is a multiple of the page size -- which is why BOTH paths page-floor
  // their base stripe size.
  const StripeGeometry geom =
      compute_stripe_geometry(_config.size, _config.stripe_size);
  const size_t num_stripes = geom.num_stripes;
  const size_t base_stripe_size = geom.base_stripe_size;
  const size_t stripe_remainder = geom.stripe_remainder;

  // Determine if we should use mmap'd directories
  bool use_mmap_dir = _mp_config.enabled;

  // Stripes start after the volume header
  uint64_t offset = VolumeHeader::kSize;
  for (size_t i = 0; i < num_stripes; ++i) {
    const size_t stripe_size =
        base_stripe_size + (i + 1 == num_stripes ? stripe_remainder : 0);
    auto stripe = std::make_unique<Stripe>();
    stripe->offset = offset;
    stripe->size = stripe_size;
    stripe->use_mmap_directory = use_mmap_dir;

    if (use_mmap_dir) {
      // Calculate directory size and map it from the file
      size_t dir_size =
          MmapDirectory::required_size(kDirectoryEntriesPerSegment);

      // Map the directory region from the file
      auto dir_region = _mapped_file->map_region(
          offset, dir_size, MappedFile::MapMode::ReadWrite);
      if (!dir_region) {
        return make_unexpected(CacheError::IoError);
      }

      // Check if directory needs initialization or can be opened
      auto mmap_dir = MmapDirectory::open(*dir_region);
      if (!mmap_dir) {
        // Initialize new directory
        auto new_dir =
            MmapDirectory::init(*dir_region, kDirectoryEntriesPerSegment);
        if (!new_dir) {
          _mapped_file->unmap_region(*dir_region);
          return make_unexpected(CacheError::IoError);
        }
        stripe->mmap_directory = std::move(*new_dir);
      } else {
        stripe->mmap_directory = std::move(*mmap_dir);
      }

      // Data starts after directory, aligned to page size
      size_t page_size = 4096;
      stripe->data_offset =
          offset + ((dir_size + page_size - 1) / page_size) * page_size;

      // Recover write_pos from the mmap'd directory header.
      // This ensures cross-process writes don't overwrite each other.
      uint64_t saved_write_pos = stripe->mmap_directory->get_shared_write_pos();
      // Reject a non-8-aligned persisted cursor (v6): every legitimate advance
      // is a multiple of 8 from an 8-aligned base, so a misaligned value is a
      // foreign/corrupt header.  Restoring it would seat documents at
      // unaligned offsets and SIGBUS the atomic_ref header RMW on ARM64 -- fail
      // closed to data_offset instead.
      if (saved_write_pos > 0 && saved_write_pos >= stripe->data_offset &&
          saved_write_pos <= offset + stripe_size && saved_write_pos % 8 == 0) {
        stripe->write_pos = saved_write_pos;
      } else {
        stripe->write_pos = stripe->data_offset;
      }

      // Exclusive open: no live peer can hold a borrow, a lease or a wrap
      // window on this stripe, so whatever those shared slots still carry
      // was left by processes that are gone.  Clear it and re-derive the
      // phase (see repair_wrap_state).  NEVER done on a shared open: a live
      // peer's borrow or in-flight wrap would be torn out from under it.
      if (exclusive) {
        stripe->mmap_directory->reset_reader_state_exclusive();
        repair_wrap_state(stripe.get());
      }
    } else {
      // Use in-memory directory
      stripe->directory =
          std::make_unique<Directory>(kDirectoryEntriesPerSegment);

      size_t dir_area = stripe->directory->capacity() * DirEntry::kSize;
      // 8-align the data region so document starts stay 8-aligned (the v6
      // atomic-ref RMW requirement); DirEntry::kSize is 10, so without this
      // the alignment would only be accidental (holds iff capacity % 4 == 0).
      dir_area = (dir_area + 7u) & ~static_cast<size_t>(7u);
      stripe->data_offset = stripe->offset + dir_area;
      stripe->write_pos = stripe->data_offset;
    }

    // Multi-process: mark stripe ownership
    stripe->owned = is_stripe_owned(i);

    _stripes.push_back(std::move(stripe));
    offset += stripe_size;
  }

  return {};
}

void Volume::maybe_advise_readahead(uint64_t doc_offset,
                                    std::span<std::byte> region) noexcept {
  // 0 disables the hint entirely; below the threshold small objects keep the
  // open-time MADV_RANDOM behaviour (no readahead pollution).
  const size_t threshold = _config.readahead_min_bytes;
  if (threshold == 0 || region.size() < threshold) {
    return;
  }
  if (!_mapped_file) {
    return;
  }
  // Re-advise filter: at most one hint per document placement per
  // kReadaheadReadviseSeconds (see _readahead_cache).  A warm re-read must
  // not pay for a range walk it cannot benefit from, but a placement that
  // has since been evicted from the page cache must get its hint back --
  // on a cache larger than RAM that is the common case, not the exception.
  const uint64_t disc = readahead_discriminator(doc_offset, region.size());
  const uint32_t tick = readahead_tick();
  auto& slot = (*_readahead_cache)[readahead_cache_index(doc_offset)];
  const uint64_t stored = slot.load(std::memory_order_relaxed);
  if ((stored >> kReadaheadTickBits) == disc) {
    // Modular difference: correct across the tick wrap, and ticks only ever
    // move forward, so a stale slot reads as "old" and re-advises.
    const uint32_t stored_tick =
        static_cast<uint32_t>(stored) & kReadaheadTickMask;
    const uint32_t age = (tick - stored_tick) & kReadaheadTickMask;
    if (age < kReadaheadReadviseSeconds) {
      return;
    }
  }
  slot.store((disc << kReadaheadTickBits) | tick, std::memory_order_relaxed);
  // Best-effort hint over exactly this document's byte range.  WHICH call
  // is a platform decision, because the two families behave very
  // differently (measured; see doc/architecture.md):
  //
  //   Linux/Windows — advise the ADDRESS range of the mapping.
  //     MADV_WILLNEED is honoured on a file mapping even though the mapping
  //     carries MADV_RANDOM: the kernel queues a few large asynchronous
  //     reads and returns, so the CRC pass below walks already-in-flight or
  //     resident pages instead of taking one serial major fault per 4 KB
  //     page.  Windows uses PrefetchVirtualMemory, same shape.
  //
  //   Darwin — advise the FILE range through the descriptor.
  //     Darwin's madvise(MADV_WILLNEED) is NOT the asynchronous queue-and-
  //     return of Linux: it walks and populates the range under the shared
  //     VM object's lock, so it both costs far more per call and serialises
  //     across every process mapping the same volume.  Probed on an M5 over
  //     a 2 MiB range of a MAP_SHARED read-write mapping: 50 us with one
  //     process, 305 us with four concurrent ones, against 5 us / 10 us for
  //     fcntl(F_RDADVISE) doing the same job.  F_RDADVISE is the native
  //     asynchronous readahead and is what advise_readahead() issues, so
  //     Darwin takes that path and never the madvise one.
  //
  // supports_advise_readahead() is a static property of the build, not an
  // error code: a runtime failure of F_RDADVISE must NOT reroute Darwin
  // onto the madvise path, which is the expensive one there.  Errors from
  // either call are discarded — a failed hint only costs the old
  // behaviour.
  if (_mapped_file->supports_advise_readahead()) {
    (void)_mapped_file->advise_readahead(doc_offset, region.size());
  } else {
    (void)_mapped_file->advise_willneed(region);
  }
  _readahead_hints.fetch_add(1, std::memory_order_relaxed);
}

uint32_t Volume::readahead_tick() noexcept {
  // Seconds off the steady clock.  Only modular differences are ever
  // compared, so the arbitrary epoch and the ~194-day truncation wrap are
  // both harmless -- see the slot layout in volume.hpp.
  auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                  std::chrono::steady_clock::now().time_since_epoch())
                  .count();
  return static_cast<uint32_t>(secs) & kReadaheadTickMask;
}

Stripe* Volume::select_stripe(const CacheKey& key) {
  if (_stripes.empty()) {
    return nullptr;
  }
  uint32_t idx = key.segment_hash() % _stripes.size();
  return _stripes[idx].get();
}

Task<std::expected<ReadHandle, CacheError>> Volume::open_read(
    const CacheKey& key) {
  auto result = read_sync(key);
  co_return result;
}

Task<std::expected<WriteHandle, CacheError>> Volume::open_write(
    const CacheKey& key, uint64_t content_length) {
  auto result = write_sync(key, content_length);
  co_return result;
}

Task<std::expected<void, CacheError>> Volume::remove(const CacheKey& key) {
  auto result = remove_sync(key);
  co_return result;
}

Task<std::expected<bool, CacheError>> Volume::exists(const CacheKey& key) {
  auto result = exists_sync(key);
  co_return result;
}

std::expected<ReadHandle, CacheError> Volume::read_sync(const CacheKey& key) {
  Stripe* stripe = select_stripe(key);
  if (stripe == nullptr) {
    return make_unexpected(CacheError::NotInitialized);
  }

  // Retry loop for torn/raced read handling.  Retries were historically
  // multi-process-only (only remote writers could race a read); readers now
  // take no stripe lock in ANY mode, so a local writer's wrap can race a
  // read the same way a remote one always could — checksum validation and
  // the wrap-epoch revalidation catch it and land here for another
  // attempt.
  uint32_t max_attempts = _mp_config.max_read_retries + 1;

  bool saw_checksum_failure = false;

  for (uint32_t attempt = 0; attempt < max_attempts; ++attempt) {
    if (attempt > 0) {
      std::this_thread::yield();  // Brief pause before retry
    }

    // Track whether to record a hit after the probe section (record_hit()
    // can trigger a synchronous HitTracker flush → update_hit_count_sync →
    // exclusive stripe->mutex; deferring keeps that heavyweight path out of
    // the hot probe window).
    bool should_record_hit = false;

    // Lock-free read: readers take NO stripe lock.  A shared_mutex reader
    // count is an atomic RMW on one shared cache line, which bounces across
    // cores and serializes concurrent readers (measured: read throughput
    // DROPS below single-threaded past ~4 threads).  Safety comes from the
    // same protocol that has always covered non-owned stripes in
    // multi-process mode, where remote writers commit with no common lock
    // at all:
    //   - the directory's own synchronization for probe consistency,
    //   - commit-path publish ordering (document bytes are fully written
    //     and synced BEFORE the directory entry is inserted),
    //   - CRC validation for torn reads, and
    //   - the stamp-then-revalidate wrap protocol (epoch capture +
    //     lease stamp + wrap-intent recheck) for the eviction-overwrite
    //     race,
    // with the outer retry loop as the recovery path.

    // Check RAM cache first (non-alternate reads use Original).
    if (_ram_cache) {
      // RAM-coherence gate (d).  read_sync serves (key, Original) from RAM
      // without consulting the directory at all and never repopulates it (the
      // only put_if in this file is on the alternate path), so it is the path
      // on which a peer's re-record or purge would otherwise be served
      // indefinitely.  When coherence is active, revalidate the entry's
      // stamp against the shared bucket version and drop it on a mismatch.
      //
      // OPERATIONAL CONSEQUENCE, by design: a read_sync-dominant workload
      // progressively DRAINS its RAM tier under this toggle — it rejects and
      // removes stale entries but never refills them; refill depends on
      // read_alternate_sync traffic.
      const bool validate = ram_coherence_active(stripe);
      uint32_t stamp = 0;
      auto ram_data = _ram_cache->get(key, AlternateId::Original,
                                      validate ? &stamp : nullptr);
      if (ram_data && validate && stamp != stripe->bucket_version(key)) {
        // Stale: the bucket moved since this entry was admitted.  Removing it
        // (rather than merely skipping it) is what keeps this path from
        // re-validating and re-rejecting the same entry on every subsequent
        // read; nothing here ever re-populates it.
        _ram_cache->remove(key, AlternateId::Original);
        _ram_coherence_rejections.fetch_add(1, std::memory_order_relaxed);
        ram_data.reset();
      }
      if (ram_data) {
        auto impl = std::make_shared<VolumeReadHandleImpl>();
        impl->ram_buffer = std::move(*ram_data);
        DocumentReader ram_reader(impl->ram_buffer);
        if (ram_reader.is_valid()) {
          bump_reads();
          should_record_hit = true;

          impl->is_ram_hit = true;
          impl->doc = ram_reader.document();
          impl->header_data = ram_reader.header();
          impl->content_data = ram_reader.content();

          if (_hit_tracker && should_record_hit) {
            _hit_tracker->record_hit(key);
          }
          return ReadHandle(impl);
        }
      }
    }

    // Try each candidate until we find the right one (allocation-free
    // iteration)
    std::expected<ReadHandle, CacheError> result =
        make_unexpected(CacheError::NotFound);
    bool found = false;
    bool checksum_failed = false;  // Track if we had a checksum failure
    bool epoch_changed = false;    // wrap raced the read

    // Capture the wrap epoch at probe start for the reader's
    // stamp-then-revalidate protocol (disk borrows only).
    const auto epoch_start = wrap_epoch(stripe);

    stripe->probe_each(key, [&](const DirEntry& dir_entry) {
      if (found) {
        return false;  // Already found, stop iteration
      }

      uint64_t doc_offset = stripe->offset + dir_entry.offset();

      size_t map_size = dir_entry.approx_size();
      auto mapped = _mapped_file->map_region(doc_offset, map_size,
                                             MappedFile::MapMode::ReadOnly);
      if (!mapped) {
        return true;  // Continue to next candidate
      }

      DocumentReader reader(*mapped);
      if (!reader.is_valid()) {
        _mapped_file->unmap_region(*mapped);
        return true;  // Continue to next candidate
      }

      // If the directory's approx_size underestimates the actual document
      // length, remap with the correct size.  This can happen because
      // DirEntry encodes sizes in a compact format that caps at ~256KB.
      if (reader.document().len > map_size) {
        // Bounds check: reject documents claiming to be larger than the stripe
        if (reader.document().len > stripe->size) {
          _mapped_file->unmap_region(*mapped);
          return true;  // Skip corrupted entry
        }
        _mapped_file->unmap_region(*mapped);
        map_size = reader.document().len;
        mapped = _mapped_file->map_region(doc_offset, map_size,
                                          MappedFile::MapMode::ReadOnly);
        if (!mapped) {
          return true;
        }
        reader = DocumentReader(*mapped);
        if (!reader.is_valid()) {
          _mapped_file->unmap_region(*mapped);
          return true;
        }
      }

      // Verify the key matches
      CacheKey stored_key = reader.first_key();
      if (stored_key != key) {
        _mapped_file->unmap_region(*mapped);
        return true;  // Continue to next candidate
      }

      // Large-document readahead.  The key matched, so this IS the document
      // the caller asked for and its exact byte range is known — but
      // nothing has touched its content yet (is_valid()/first_key() read
      // header fields only).  Issue the hint HERE, before the CRC pass
      // below makes the first content touch, so a cold 2 MiB document
      // costs a few large asynchronous reads instead of ~512 serial page
      // faults.  Pure address-range hint: no lock, no shared state, no
      // dereference — it is outside the borrow/lease window by design and
      // does not participate in the reader protocols (see
      // maybe_advise_readahead).  Advise exactly the document, never the
      // slack a coarse approx_size mapping may carry past its end.
      maybe_advise_readahead(
          doc_offset, mapped->first(std::min<size_t>(mapped->size(),
                                                     reader.document().len)));

      // Verify checksum to detect corruption or torn reads.
      // Skip if this {offset, checksum} pair was already verified.  NOTE:
      // the validation cache stores a 16-bit discriminator, not the full
      // CRC32 (see kChecksumCacheSize) — it is NOT the overwrite guard;
      // the wrap-epoch revalidation below is.
      if (_config.verify_checksum_on_read && reader.document().checksum != 0 &&
          !is_checksum_validated(doc_offset, reader.document().checksum)) {
        if (!reader.document().verify_checksum(reader.payload())) {
          _mapped_file->unmap_region(*mapped);
          checksum_failed = true;
          return true;  // Continue to next candidate
        }
        mark_checksum_validated(doc_offset, reader.document().checksum);
      }

      // Lease protocol + register the borrow (count+1) and stamp the
      // read lease (seq_cst CAS-max now + T, with the write-avoidance
      // guard) BEFORE the borrow escapes, then revalidate: wrap-intent
      // flag clear AND wrap epoch (captured at probe start) unchanged
      // (Dekker closure — see allocate_write_slot).  A failure means a
      // wrap raced (or is racing) this read between probe and stamp — the
      // bytes may be (about to be) overwritten: release, discard and
      // retry/miss.  This revalidation is deliberately independent of the
      // checksum-validation cache above (a cached CRC verdict must never
      // short-circuit it).
      BorrowToken borrow = acquire_borrow(stripe);
      stamp_read_lease(stripe);
      if (!borrow_still_valid(stripe, epoch_start)) {
        release_borrow(stripe, borrow);
        _mapped_file->unmap_region(*mapped);
        epoch_changed = true;
        return false;  // Stop iteration; retry the whole attempt
      }

      // Found the right entry
      bump_reads();
      should_record_hit = true;

      auto impl = std::make_shared<VolumeReadHandleImpl>();
      // pin Volume + MappedFile through this thread's shard
      // anchor — one refcount RMW on a thread-affine control block — with
      // the two per-read weak_ptrs as the anchor-less fallback.
      impl->anchor = read_anchor();
      if (!impl->anchor) {
        impl->mapped_file_weak = _mapped_file;
        impl->volume_weak = weak_from_this();  // pin for lease renew
      }
      impl->mapping = *mapped;
      impl->doc = reader.document();
      impl->header_data = reader.header();
      impl->content_data = reader.content();
      impl->stripe = stripe;
      impl->epoch_start = epoch_start;  // checked-renew snapshot
      impl->borrow = borrow;            // released on handle destruction

      result = ReadHandle(impl);
      found = true;
      return false;  // Stop iteration
    });

    if (_hit_tracker && should_record_hit) {
      _hit_tracker->record_hit(key);
    }

    // If we found a valid entry, or neither a checksum failure nor an
    // epoch change occurred, return
    if (found || (!checksum_failed && !epoch_changed)) {
      return result;
    }
    if (checksum_failed) {
      saw_checksum_failure = true;
    }
    // Otherwise, checksum failed or a wrap raced the read (wrap epoch
    // revalidation) — retry if we have attempts left
  }

  // All retries exhausted.  Checksum failures indicate corruption; pure
  // epoch churn means the entry kept being evicted mid-read — a miss.
  return make_unexpected(saw_checksum_failure ? CacheError::Corrupted
                                              : CacheError::NotFound);
}

std::expected<WriteHandle, CacheError> Volume::write_sync(
    const CacheKey& key, uint64_t content_length) {
  Stripe* stripe = select_stripe(key);
  if (stripe == nullptr) {
    return make_unexpected(CacheError::NotInitialized);
  }

  // Multi-process: reject writes to non-owned stripes
  if (!stripe->owned) {
    return make_unexpected(CacheError::NotOwned);
  }

  // Per-object bound: a declared length already over the configured limit
  // fails here, before a handle (and its buffer) exists.  0 = unbounded.
  if (_config.max_object_size > 0 && content_length > _config.max_object_size) {
    return make_unexpected(CacheError::ObjectTooLarge);
  }

  auto impl = std::make_shared<VolumeWriteHandleImpl>();
  impl->volume_raw = this;
  impl->volume_weak = weak_from_this();
  // Empty weak_ptr => no shared owner (constructed directly, not via
  // Cache::add_volume): nothing can free it behind our back, so the raw
  // pointer stands.
  impl->guarded = (impl->volume_weak.use_count() > 0);
  impl->stripe = stripe;
  impl->key = key;
  impl->expected_length = content_length;

  return WriteHandle(impl);
}

std::expected<void, CacheError> Volume::remove_sync(const CacheKey& key) {
  Stripe* stripe = select_stripe(key);
  if (stripe == nullptr) {
    return make_unexpected(CacheError::NotInitialized);
  }

  // Multi-process: reject writes to non-owned stripes
  if (!stripe->owned) {
    return make_unexpected(CacheError::NotOwned);
  }

  if (!_mapped_file) {
    return make_unexpected(CacheError::NotInitialized);
  }

  std::unique_lock lock(stripe->mutex);

  // Phase 1: Collect all candidate directory entries matching the 12-bit tag.
  // The two-phase collect-then-remove discipline is still required now that
  // the directory is a per-bucket seqlock: calling remove_entry_at from
  // inside a probe_each callback would bump the same bucket's version
  // mid-scan, so the scan's own validation could never succeed (futile
  // retries, then a silent inconsistent return) — not a deadlock anymore,
  // but still wrong.  Since we hold stripe->mutex exclusively, no
  // concurrent modifications can happen between phases.
  struct Candidate {
    uint64_t dir_offset;  // DirEntry::offset() (relative to stripe start)
    uint64_t approx_size;
  };
  std::array<Candidate, Directory::kEntriesPerBucket> candidates;
  size_t num_candidates = 0;

  stripe->probe_each(key, [&](const DirEntry& dir_entry) {
    if (num_candidates < candidates.size()) {
      candidates[num_candidates++] = {dir_entry.offset(),
                                      dir_entry.approx_size()};
    }
    return true;  // Continue — collect all matching entries
  });

  // Phase 2: For each candidate, read the document to verify the full SHA-256
  // key (not just the 12-bit tag), then remove the correct entry precisely.
  for (size_t ci = 0; ci < num_candidates; ++ci) {
    const auto& candidate = candidates[ci];

    // Bounds check: skip entries with offsets outside the stripe's data area
    if (!is_valid_chain_offset(stripe, candidate.dir_offset)) {
      continue;
    }

    uint64_t doc_offset = stripe->offset + candidate.dir_offset;

    size_t map_size = candidate.approx_size;
    auto mapped = _mapped_file->map_region(doc_offset, map_size,
                                           MappedFile::MapMode::ReadOnly);
    if (!mapped) {
      continue;
    }

    DocumentReader reader(*mapped);
    if (!reader.is_valid()) {
      _mapped_file->unmap_region(*mapped);
      continue;
    }

    // If approx_size underestimates the actual document length, remap with
    // the correct size (same pattern as read_sync).  This can happen because
    // DirEntry encodes sizes in a compact format that caps at ~256KB.
    if (reader.document().len > map_size) {
      if (reader.document().len > stripe->size) {
        _mapped_file->unmap_region(*mapped);
        continue;  // Skip corrupted entry
      }
      _mapped_file->unmap_region(*mapped);
      map_size = reader.document().len;
      mapped = _mapped_file->map_region(doc_offset, map_size,
                                        MappedFile::MapMode::ReadOnly);
      if (!mapped) {
        continue;
      }
      reader = DocumentReader(*mapped);
      if (!reader.is_valid()) {
        _mapped_file->unmap_region(*mapped);
        continue;
      }
    }

    CacheKey stored_key = reader.first_key();
    _mapped_file->unmap_region(*mapped);

    if (stored_key != key) {
      continue;  // Tag collision — not our entry
    }

    // Full key match — remove this specific entry using precise offset match
    if (stripe->remove_entry_at(key, candidate.dir_offset)) {
      // ---- Invalidate the RAM copies this remove orphans (post-removal) --
      //
      // ORDERING (load-bearing).  Both steps run AFTER the directory entry
      // is gone, in this order: bump the stripe's remove generation, then
      // drop the RAM entries.  The mirror-image placement this function used
      // to have — bumping the generation at the TOP, evicting here — left a
      // permanent resurrection hole: a lock-free reader that sampled
      // the generation between that bump and the removal above still probed
      // the LIVE directory entry and copied the document into RAM, its
      // re-check saw no second bump, and our eviction below had already run —
      // so nothing ever withdrew the put.  read_sync serves a RAM hit for
      // (key, Original) without consulting the directory, so the removed
      // document kept being served indefinitely.
      //
      // Placed here, the reader's own resurrection guard closes both sides
      // (see read_alternate_sync, which samples the generation before its
      // probe and re-checks it in its conditional RAM-cache put):
      //
      //   * a reader that sampled BEFORE our bump either lands its put before
      //     our remove_all — we evict it — or after it, and then its put's
      //     predicate sees the bump and the put is dropped at insert time;
      //   * a reader that sampled AFTER our bump synchronizes-with that
      //     bump, and therefore with the removal sequenced before it, so its
      //     probe can no longer see the entry and it never puts the removed
      //     document.
      //
      // So no copy of the removed document SURVIVES in RAM: every raced put
      // is either evicted by us or rejected by its own author (a bump that
      // lands inside the put's own critical section is withdrawn by the
      // reader's post-put re-check, or by our eviction).  This is the same
      // post-publish ordering the commit paths adopted with the ordering fix;
      // the full argument lives in commit_alternate_write.  Sited after every
      // early return, so a remove that found nothing never bumps or evicts.
      if (_ram_cache) {
        stripe->remove_epoch.fetch_add(1, std::memory_order_acq_rel);
        _ram_cache->remove_all(key);
      }
      return {};
    }
  }

  return make_unexpected(CacheError::NotFound);
}

std::expected<bool, CacheError> Volume::exists_sync(const CacheKey& key) {
  Stripe* stripe = select_stripe(key);
  if (stripe == nullptr) {
    return make_unexpected(CacheError::NotInitialized);
  }

  // Lock-free read — pure directory probe (no borrow escapes), covered by
  // the directory's own synchronization; see the note in read_sync().
  bool found = false;
  stripe->probe_each(key, [&](const DirEntry&) {
    found = true;
    return false;  // Stop iteration
  });
  return found;
}

namespace {

// Monotonic timestamp for wrap-cadence telemetry. Never returns 0 so that
// 0 can serve as the "never wrapped" sentinel.
uint64_t steady_now_ns() noexcept {
  auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();
  return now_ns > 0 ? static_cast<uint64_t>(now_ns) : 1;
}

}  // namespace

void Volume::record_wrap(Stripe* stripe) noexcept {
  uint64_t now_ns = steady_now_ns();

  if (stripe->use_mmap_directory && stripe->mmap_directory) {
    // Cross-process visibility: count the wrap in the shared per-stripe
    // directory header (we are under the stripe's write_lock here, which
    // serializes the update with other writer processes).
    stripe->mmap_directory->record_shared_wrap(now_ns);
  } else {
    _wrap_count.fetch_add(1, std::memory_order_relaxed);
    // Per-stripe epoch for the stamp-then-revalidate protocol
    // (the non-mmap counterpart of shared_wrap_count).
    stripe->local_wrap_count.fetch_add(1, std::memory_order_seq_cst);
  }

  // Process-local interval tracking: covers only wraps performed by this
  // process, volume-wide across stripes (see VolumeStats for semantics).
  uint64_t prev_ns =
      _last_wrap_time_ns.exchange(now_ns, std::memory_order_acq_rel);
  if (prev_ns == 0 || now_ns <= prev_ns) {
    // First wrap ever, or a concurrent wrap on another stripe published a
    // later timestamp first — no meaningful interval to record.
    return;
  }

  uint64_t interval = now_ns - prev_ns;
  _last_wrap_interval_ns.store(interval, std::memory_order_relaxed);

  uint64_t cur_min = _min_wrap_interval_ns.load(std::memory_order_relaxed);
  while ((cur_min == 0 || interval < cur_min) &&
         !_min_wrap_interval_ns.compare_exchange_weak(
             cur_min, interval, std::memory_order_relaxed)) {
  }
}

void Volume::repair_wrap_state(Stripe* stripe) {
  if (!(stripe->use_mmap_directory && stripe->mmap_directory)) {
    return;  // Process-local state dies with the process: nothing to repair.
  }
  MmapDirectory& dir = *stripe->mmap_directory;
  // Phase FIRST, intent LAST: a reader that observes the cleared intent must
  // also observe the re-derived phase (both seq_cst, program order).  The
  // pass count is the authoritative epoch; the phase is only ever its low
  // bit.  A writer that died between the phase toggle and the pass-count
  // bump left the two disagreeing (every current-pass entry then reads as
  // previous-pass and vice versa); re-deriving closes that drift.  Entries
  // of the "wrong" phase that become visible again are exactly the
  // phase-ABA survivors invariants 8 and 9 already reject by key and
  // position, so this can only ever turn serves into misses.
  dir.set_current_phase((dir.shared_wrap_count() & 1U) != 0);
  dir.set_wrap_intent(false);
}

void Volume::repair_after_forced_release(
    Stripe* stripe, const MmapDirectory::WriteLockToken& token) {
  // Proven dead ONLY.  An escalated takeover usurps a holder we could not
  // prove dead: it may still be alive and inside its wrap window, and
  // clearing its intent would let a reader borrow bytes that holder is
  // about to publish over.  Leave the flag to that holder (or to the next
  // proven-dead recovery / wrap).
  if (token.acquired && token.forced_release && !token.escalated_takeover) {
    repair_wrap_state(stripe);
  }
}

// --- Lease-based region pinning -------------------------------------------
// --- + borrow-scoped wrap gating (the write-starvation fix) ---------------
//
// Reader protocol (mmap-borrow paths ONLY — read_sync and
// read_alternate_sync disk hits; RAM-cache hits and misses never stamp or
// count): after locating the entry, register the borrow in the stripe's
// outstanding-borrow accounting (acquire_borrow — a seq_cst CAS count+1 on
// THIS THREAD's borrow shard for non-mmap stripes, on the single shared
// slot for mmap stripes) and stamp
// the stripe lease (seq_cst CAS-max now + T, with the write-avoidance
// guard) BEFORE the borrow escapes, then revalidate via
// borrow_still_valid(): the writer's wrap-intent flag must be clear
// (loaded FIRST) and the wrap epoch (shared_wrap_count + phase captured at
// probe start) unchanged.  If either check fails, a wrap raced (or is
// racing) the read between probe and stamp: release the borrow, discard,
// and retry/miss.  The revalidation is deliberately NOT routed through the
// checksum-validation cache.  The count is dropped on ReadHandle
// close/destruction (release_borrow) — closing the handle is what returns
// write capacity to the stripe.
//
// Writer protocol: inside allocate_write_slot (the single shared
// allocation helper used by BOTH wrap sites), the writer sets the shared
// wrap-intent flag (seq_cst) BEFORE the gate loads; the gate
// (lease_permits_wrap) runs BEFORE evict_if_needed()/toggle_phase() and
// defers the wrap only while BOTH hold:
//   - the stripe's outstanding-borrow count is nonzero (a live ReadHandle
//     still aliases the region), AND
//   - the read lease is live (the liveness bound: holders must renew at a
//     cadence <= 3T/4, so a crashed/leaked holder stops blocking within T;
//     an open handle whose lease lapsed is unprotected, as before).
// A count leaked past its lease (crashed holder + steady fresh reads
// re-stamping) is cleared by the anti-starvation ceiling: the forced wrap
// resets the slot (generation+1, count 0), so leaked state costs at most
// one lease_wrap_ceiling episode.  Before the borrow gate the lease alone
// gated wraps,
// so ANY stripe read more often than once per T sat write-starved at
// capacity (~one forced admission per ceiling) — a promptly-closed read
// now costs writers nothing beyond its open window.
// A deferred wrap has zero side effects — no phase toggle, no directory
// invalidation, no write_pos mutation; only counters — the intent flag is
// cleared and the fill dropped (cache writes are best-effort).  On a
// permitted wrap the intent flag stays set across the entire decision +
// publish window (toggle/reset/record_shared_wrap) and is cleared after.
// The Dekker closure proof lives at the intent-set site in
// allocate_write_slot.

void Volume::stamp_read_lease(Stripe* stripe) const {
  if (_lease_t_ns == 0 || stripe == nullptr) {
    return;  // Leases disabled.
  }
  uint64_t now = steady_now_ns();
  uint64_t new_expiry = now + _lease_t_ns;
  // Write-avoidance guard: skip the CAS when the current expiry already
  // covers now + T - slack (slack = T/4).  Protection floor is 3T/4.
  uint64_t skip_if_at_least = now + _lease_t_ns - _lease_t_ns / 4;
  if (stripe->use_mmap_directory && stripe->mmap_directory) {
    stripe->mmap_directory->stamp_lease_expiry(new_expiry, skip_if_at_least);
  } else {
    cas_max_lease(stripe->local_lease_expiry_ns, new_expiry, skip_if_at_least);
  }
}

BorrowToken Volume::acquire_borrow(Stripe* stripe) {
  if (_lease_t_ns == 0 || stripe == nullptr) {
    return {};  // Leases disabled: the wrap gate is off, nothing to count.
  }
  borrow_slot::Acquired acquired;
  uint8_t shard = 0;
  if (stripe->use_mmap_directory && stripe->mmap_directory) {
    acquired = stripe->mmap_directory->borrow_acquire();
  } else {
    // THIS THREAD's shard: the CAS stays on a thread-local
    // cache line instead of bouncing one stripe-global slot line across
    // every reading core.  The token records the shard so the release
    // lands on the same slot.
    shard = static_cast<uint8_t>(thread_shard_index(Stripe::kBorrowShards));
    acquired = borrow_slot::acquire(stripe->local_borrow_shards[shard].slot);
  }
  return {acquired.generation, acquired.counted, true, shard};
}

void Volume::release_borrow(Stripe* stripe, BorrowToken token) {
  if (!token.active || !token.counted || stripe == nullptr) {
    return;  // RAM hit, leases disabled, or saturated ride-along.
  }
  if (_teardown.load(std::memory_order_seq_cst)) {
    return;  // close() freed the stripes: leaked-count case, documented.
  }
  if (stripe->use_mmap_directory && stripe->mmap_directory) {
    stripe->mmap_directory->borrow_release(token.generation);
  } else {
    borrow_slot::release(stripe->local_borrow_shards[token.shard].slot,
                         token.generation);
  }
}

std::vector<std::shared_ptr<VolumeReadAnchor>> Volume::make_read_anchors() {
  // One anchor per thread shard, each with its OWN control block — that
  // separation is the point (see VolumeReadAnchor in volume.hpp).  All of
  // them pin the same Volume and the MappedFile of the CURRENT open;
  // callers must rebuild after a close()/open() cycle (Cache::start()
  // does).  Requires this Volume to be shared_ptr-owned.
  std::vector<std::shared_ptr<VolumeReadAnchor>> anchors;
  if (!_mapped_file) {
    return anchors;  // Not open: no mapping to pin.
  }
  anchors.reserve(kReadAnchorShards);
  for (size_t i = 0; i < kReadAnchorShards; ++i) {
    auto anchor = std::make_shared<VolumeReadAnchor>();
    anchor->volume = shared_from_this();
    anchor->mapped_file = _mapped_file;
    anchors.push_back(std::move(anchor));
  }
  return anchors;
}

void Volume::set_read_anchors(const std::shared_ptr<VolumeReadAnchor>* slots,
                              size_t count) {
  // Caller must exclude concurrent reads (Cache installs/detaches under
  // its exclusive gate).  thread_shard_index requires a power of two.
  if (slots == nullptr || count == 0 || (count & (count - 1)) != 0) {
    _read_anchor_slots = nullptr;
    _read_anchor_count = 0;
    return;
  }
  _read_anchor_slots = slots;
  _read_anchor_count = count;
}

std::pair<uint64_t, bool> Volume::wrap_epoch(const Stripe* stripe) const {
  if (stripe->use_mmap_directory && stripe->mmap_directory) {
    return stripe->mmap_directory->wrap_epoch();
  }
  bool phase = stripe->directory ? stripe->directory->current_phase() : false;
  return {stripe->local_wrap_count.load(std::memory_order_seq_cst), phase};
}

void Volume::set_wrap_intent(Stripe* stripe, bool active) {
  if (stripe->use_mmap_directory && stripe->mmap_directory) {
    stripe->mmap_directory->set_wrap_intent(active);
  } else {
    stripe->local_wrap_intent.store(active ? 1 : 0, std::memory_order_seq_cst);
  }
}

bool Volume::wrap_intent_set(const Stripe* stripe) const {
  if (stripe->use_mmap_directory && stripe->mmap_directory) {
    return stripe->mmap_directory->wrap_intent();
  }
  return stripe->local_wrap_intent.load(std::memory_order_seq_cst) != 0;
}

bool Volume::borrow_still_valid(
    const Stripe* stripe, const std::pair<uint64_t, bool>& epoch_start) const {
  // Order is load-bearing (see the Dekker proof in allocate_write_slot):
  // the intent flag is loaded FIRST.  If intent == 0 was read from a
  // completed wrap's clear, that clear is seq_cst-after the wrap-count
  // publish, so the epoch loads below (later in the seq_cst total order)
  // must observe the changed epoch.  If intent == 0 predates a concurrent
  // writer's intent-store, that writer's later lease load is
  // seq_cst-after our stamp and defers.
  if (wrap_intent_set(stripe)) {
    return false;  // A wrap decision is in flight — discard and retry.
  }
  return wrap_epoch(stripe) == epoch_start;
}

bool Volume::renew_read_lease(Stripe* stripe,
                              const std::pair<uint64_t, bool>& epoch_start) {
  if (_lease_t_ns == 0) {
    // Leases disabled: no protection to extend.  The embedder treats a
    // false renew as "cannot prove this borrow still safe" and copies.
    return false;
  }
  if (_teardown.load(std::memory_order_seq_cst)) {
    return false;  // close() freed the stripes: nothing left to renew.
  }
  // Read-side acquire fence: order the caller's PRECEDING data reads (the
  // embedder's memcpy of the borrowed mmap span) before the epoch verdict
  // below.  This is the directory seqlock reader's fence (mmap_directory:
  // the acquire fence between the entry copy and the version re-check),
  // applied to the copy-then-verify contract.  As a LoadLoad barrier it
  // pins the memcpy loads ahead of the epoch load; without it a seq_cst
  // LOAD is only an acquire on itself and does NOT keep an earlier plain
  // load from sinking below it, so on a weakly-ordered ISA a memcpy load
  // could bind AFTER the epoch load and copy bytes a wrap already
  // overwrote while we still read the pre-wrap epoch.  No-op on x86 (loads
  // are already acquire; the initial-borrow path is fence-correct there
  // only because seq_cst RMWs happen to interpose).  Like the directory
  // seqlock reader it mirrors, the plain-load-then-acquire-fence idiom is
  // ISA-correct but technically UB under strict ISO C++ (the plain loads
  // race) — a pre-existing, deliberate trade shared with mmap_directory.
  std::atomic_thread_fence(std::memory_order_acquire);
  // Epoch-ONLY, check-before-stamp (2026-07-07 lease amendment).  If the
  // stripe's wrap epoch moved since the borrow was taken, a committed or
  // ceiling-forced wrap may have overwritten the borrowed region in place:
  // fail so the embedder copies/aborts, and do NOT re-stamp — extending a
  // lease on a doomed borrow only marches the anti-starvation deferral
  // clock and needlessly starves writers.  A bare wrap_intent is
  // deliberately NOT a failure here (unlike the initial borrow's
  // borrow_still_valid): a deferred wrap has zero side effects, so failing
  // on transient intent would spuriously abort valid in-flight serves on a
  // hot leased stripe (every writer attempt sets-then-clears intent).  An
  // in-flight wrap that actually commits moves the epoch and is caught on
  // the next renew or by the embedder's pre-write revalidation.
  if (wrap_epoch(stripe) != epoch_start) {
    return false;
  }
  stamp_read_lease(stripe);  // Still valid — extend the lease.
  return true;
}

LeaseRenewal Volume::renew_read_lease_strict(
    Stripe* stripe, const std::pair<uint64_t, bool>& epoch_start) {
  // Lease amendment (2026-07-07): the ALIASED zero-copy path's per-send
  // validation.  Unlike renew_read_lease (epoch-only, safe only for
  // copy-then-verify where the read is observable), an aliased writev's read
  // is an unobservable socket send: it needs the SAME Dekker-ordered protocol
  // as the initial borrow — stamp the lease FIRST, then load wrap_intent
  // before epoch — so a normal wrap racing at the lease-lapse boundary
  // observes our fresh stamp (seq_cst-after its lease load) and defers
  // instead of overwriting the region mid-send.  The stamp+revalidate and
  // the caller's writev run in the same synchronous call stack (µs apart),
  // so the fresh T-lease covers the whole sendfile_chain writev burst.
  if (_lease_t_ns == 0) {
    // Leases disabled: no wrap-overwrite protection exists (legacy
    // no-lease semantics).  Tell the embedder to copy but NOT abort — the
    // copy is a plain immediate snapshot as it was before leases existed.
    return LeaseRenewal::kLeasesOff;
  }
  // Read-side acquire fence (same rationale as renew_read_lease, and the
  // directory seqlock reader's fence): order the caller's PRECEDING data
  // reads before the epoch/teardown verdict.  Load-bearing here beyond the
  // Dekker stamp: the stamp can degrade to a pure load via the
  // write-avoidance skip guard (stamp_read_lease), so it is not guaranteed
  // to be the seq_cst RMW that would otherwise fence the epoch load — this
  // LoadLoad barrier pins the preceding reads ahead of the epoch verdict
  // regardless.  No-op on x86.
  std::atomic_thread_fence(std::memory_order_acquire);
  if (_teardown.load(std::memory_order_seq_cst)) {
    return LeaseRenewal::kTorn;  // Cache stopping mid-drain: do not alias.
  }
  stamp_read_lease(stripe);  // Stamp FIRST — Dekker (matches the borrow).
  // borrow_still_valid LOAD order: wrap_intent loaded FIRST, then epoch (the
  // seq_cst ordering the Dekker proof needs).  But the returned VERDICT gives
  // epoch-move PRECEDENCE over intent: any overwrite of a borrowed region is a
  // wrap, and a wrap always calls record_wrap (epoch++) BEFORE its pwrite, so
  // a moved epoch means the region may already be (or is being) overwritten
  // and MUST be kTorn -- even if a (possibly different, pipelined) wrap also
  // holds intent right now.  A bare intent with an UNMOVED epoch is a wrap
  // still in its pre-record_wrap decision window: the region is intact but a
  // commit may land during the aliased send, so de-alias by copying.
  const bool intent = wrap_intent_set(stripe);  // load intent FIRST
  const bool epoch_moved = wrap_epoch(stripe) != epoch_start;  // then epoch
  // PREMISE (restored): "any overwrite of a borrowed region is a wrap" holds
  // for every borrow this protocol can hand out, because a borrow is only
  // ever taken through a node BEHIND the write cursor — the phase-ABA
  // positional guard rejects at/ahead-of-cursor nodes on BOTH paths that can
  // yield a borrow target: directory probes (Stripe::probe_each) and
  // alternate-chain hops (is_valid_chain_offset).  A before-the-cursor
  // region is only ever revisited by WRAPPING back to it, and a wrap always
  // record_wrap()s (epoch++) before its pwrite; the ORDINARY forward fill
  // only advances INTO not-yet-borrowable ahead-of-cursor space, so it can
  // never cross a live borrow.  This closes both forward-fill tears that
  // formerly voided this premise: the two-wrap trailing-gap directory
  // survivor, and the one-wrap dark chain tail (a head committed across a
  // wrap keeps next pointing at the pre-wrap, now ahead-of-cursor old head).
  if (epoch_moved) {
    return LeaseRenewal::kTorn;
  }
  if (intent) {
    return LeaseRenewal::kCopyNow;
  }
  return LeaseRenewal::kOk;
}

bool Volume::lease_permits_wrap(Stripe* stripe) {
  if (_lease_t_ns == 0) {
    return true;  // Leases disabled — pre-lease behavior.
  }

  // Both gate loads are seq_cst and run AFTER the caller's wrap-intent
  // store (Dekker pairing with the reader's acquire/stamp RMWs — proof at
  // allocate_write_slot).
  uint64_t expiry;
  uint64_t borrow_count;
  if (stripe->use_mmap_directory && stripe->mmap_directory) {
    expiry = stripe->mmap_directory->lease_expiry_ns();
    borrow_count =
        borrow_slot::count(stripe->mmap_directory->borrow_slot_raw());
  } else {
    expiry = stripe->local_lease_expiry_ns.load(std::memory_order_seq_cst);
    // Sum every borrow shard.  Each seq_cst load pairs with
    // that shard's acquire CAS: for any given reader, either its CAS
    // precedes this load in the seq_cst total order (the sum counts it
    // and the wrap defers), or this gate's earlier intent-store precedes
    // the reader's intent-load (the reader discards its borrow) — the
    // single-slot Dekker argument, quantified per shard.
    borrow_count = 0;
    for (const auto& shard : stripe->local_borrow_shards) {
      borrow_count +=
          borrow_slot::count(shard.slot.load(std::memory_order_seq_cst));
    }
  }

  uint64_t now = steady_now_ns();
  // Reboot/clock staleness clamp: CLOCK_MONOTONIC restarts at boot, so a
  // persisted expiry from a prior boot can read as unexpired for days.
  // Any expiry further out than now + T + ceiling is stale — ignore it.
  bool lease_active = expiry != 0 && expiry > now &&
                      expiry <= now + _lease_t_ns + _lease_ceiling_ns;
  // a wrap endangers only bytes a live borrow still aliases.
  // Gate on the outstanding-borrow count AND the lease: no open handles
  // (the common promptly-closed read) means the residual lease timestamp
  // must not block the wrap; open handles whose lease lapsed (holder
  // stopped renewing, or crashed) are unprotected exactly as before.
  bool borrows_outstanding = borrow_count != 0;
  if (!lease_active || !borrows_outstanding) {
    // Nothing to protect: reset the continuous-deferral episode and proceed.
    stripe->wrap_deferred_since_ns.store(0, std::memory_order_relaxed);
    publish_wrap_deferred_deadline(stripe, 0);  // STEP-3: no force pending
    return true;
  }

  // Anti-starvation ceiling: the backstop for counted-but-dead borrows — a
  // crashed holder's leaked count combined with fresh reads re-stamping the
  // lease would otherwise starve writers forever.
  uint64_t since =
      stripe->wrap_deferred_since_ns.load(std::memory_order_relaxed);
  if (since != 0 && now - since > _lease_ceiling_ns) {
    _wraps_forced_past_lease.fetch_add(1, std::memory_order_relaxed);
    stripe->wrap_deferred_since_ns.store(0, std::memory_order_relaxed);
    publish_wrap_deferred_deadline(stripe, 0);  // STEP-3: force consumed
    // clear the outstanding-borrow slot (generation+1, count 0)
    // so leaked state costs at most this one ceiling episode.  Live holders
    // invalidated here find out via the moved epoch on their next
    // renew/strict-renew (kTorn), exactly as forced wraps always worked;
    // their late releases are dropped by the generation check.
    if (stripe->use_mmap_directory && stripe->mmap_directory) {
      stripe->mmap_directory->borrow_force_reset();
    } else {
      for (auto& shard : stripe->local_borrow_shards) {
        borrow_slot::force_reset(shard.slot);
      }
    }
    return true;  // Forced wrap: >ceiling holds are unprotected (documented).
  }
  if (since == 0) {
    stripe->wrap_deferred_since_ns.store(now, std::memory_order_relaxed);
    // STEP-3: publish the instant a forced wrap becomes reachable so a
    // reader serving a zero-copy borrow can copy-out before it fires.
    publish_wrap_deferred_deadline(stripe, now + _lease_ceiling_ns);
  }
  return false;  // Defer the wrap; caller drops the fill.
}

void Volume::publish_wrap_deferred_deadline(Stripe* stripe,
                                            uint64_t deadline_ns) {
  // 0 = no deferral pending.  Otherwise coarsen to ms; guarantee a
  // nonzero value so the ms==0 sentinel is never confused with a live
  // deadline that truncates to 0 at boot.
  uint32_t deadline_ms = 0;
  if (deadline_ns != 0) {
    deadline_ms = static_cast<uint32_t>(deadline_ns / 1000000);
    if (deadline_ms == 0) deadline_ms = 1;
  }
  if (stripe->use_mmap_directory && stripe->mmap_directory) {
    stripe->mmap_directory->set_wrap_deferred_deadline_ms(deadline_ms);
  } else {
    stripe->local_wrap_deferred_deadline_ms.store(deadline_ms,
                                                  std::memory_order_seq_cst);
  }
}

uint64_t Volume::ns_until_forced_wrap(const Stripe* stripe) const {
  if (_lease_t_ns == 0 || stripe == nullptr ||
      _teardown.load(std::memory_order_seq_cst)) {
    return UINT64_MAX;  // Leases disabled / closing: no ceiling force.
  }
  uint32_t deadline_ms;
  if (stripe->use_mmap_directory && stripe->mmap_directory) {
    deadline_ms = stripe->mmap_directory->wrap_deferred_deadline_ms();
  } else {
    deadline_ms =
        stripe->local_wrap_deferred_deadline_ms.load(std::memory_order_seq_cst);
  }
  if (deadline_ms == 0) {
    return UINT64_MAX;  // No wrap currently deferred on this stripe.
  }
  uint32_t now_ms = static_cast<uint32_t>(steady_now_ns() / 1000000);
  uint32_t remaining_ms = deadline_ms - now_ms;  // unsigned, wrap-safe
  // A remaining value larger than the ceiling means the deadline already
  // passed (unsigned underflow) or a stale/rebooted publisher: treat as
  // imminent (0) so the embedder copies -- fail-safe.
  uint64_t ceiling_ms = _lease_ceiling_ns / 1000000;
  if (remaining_ms > ceiling_ms) {
    return 0;
  }
  return static_cast<uint64_t>(remaining_ms) * 1000000;
}

std::expected<Volume::WriteSlot, CacheError> Volume::allocate_write_slot(
    Stripe* stripe, size_t doc_size) {
  // v6: round the reservation up to 8 bytes so every document starts on an
  // 8-byte boundary.  The in-place header RMW sites store hit_count /
  // next_alternate_offset / last_access through std::atomic_ref on the shared
  // mapping, which is UB on ARM64 unless those fields are naturally aligned;
  // together with v6's aligned wire offsets, an 8-aligned doc start
  // guarantees it.  The 0-7 pad bytes are dead space the directory never maps
  // (reads are directory-driven, not a sequential scan).
  doc_size = (doc_size + 7u) & ~static_cast<size_t>(7u);
  // Acquire the cross-process write lock for write_pos allocation AND hold it
  // across the caller's pwrite (F6).  The original rationale for releasing the
  // lock here -- "holding it through pwrite() could exceed the spin timeout and
  // get force-released, causing overlapping writes" -- is obsolete since the
  // write-lock lifetime fix: a force-release now PROVES the holder dead
  // (kill(pid,0)) before recovering it, so a live process mid-pwrite is never
  // usurped by routine recovery. (The multi-second last-resort escalation
  // remains the bounded W2-class residual -- see the honesty note on WriteSlot
  // in volume.hpp and the commit_write_slot usurp comment below.)  Releasing
  // the lock BEFORE the pwrite instead opened the reservation-to-pwrite tear
  // window this change closes: the guard-visible write cursor was advanced past
  // bytes that the pwrite had not yet filled, so a phase-ABA survivor inside
  // the reserved span passed the positional guard, handed out a zero-copy
  // borrow, and was then torn by the pwrite.  We now reserve here, RETURN WITH
  // THE LOCK HELD, and let commit_write_slot() advance the cursor + release
  // only after the fill is durable.
  bool has_write_lock = false;
  MmapDirectory::WriteLockToken write_token;
  if (stripe->use_mmap_directory && stripe->mmap_directory) {
    write_token = stripe->mmap_directory->acquire_write_lock();
    has_write_lock = true;

    // Write-lock recovery telemetry (see MmapDirectory::acquire_write_lock).
    // forced_release counts routine crash recovery (holder PROVEN dead);
    // escalated_takeover counts the last-resort takeover of a holder we
    // could NOT prove dead — THE alertable event.
    if (write_token.forced_release) {
      _write_lock_force_releases.fetch_add(1, std::memory_order_relaxed);
    }
    if (write_token.escalated_takeover) {
      _write_lock_escalation_takeovers.fetch_add(1, std::memory_order_relaxed);
    }
    if (write_token.live_waits != 0) {
      _write_lock_live_holder_waits.fetch_add(write_token.live_waits,
                                              std::memory_order_relaxed);
    }

    // HOISTED usurpation gate: abort BEFORE any shared side effect (wrap
    // intent, phase toggle, eviction, wrap record, write_pos publish) when
    // a force-release has already invalidated this acquisition — the
    // acquire-tail stall (escalated between the winning CAS and here) and
    // the sample-race stale token.  Without this gate a resumed usurped
    // holder would commit the wrap branch's shared mutations before the
    // publish-time gate below caught it.  Do NOT release on failure: the
    // lock is either the usurper's now, or (stale-token race) still ours
    // and recovered by the next waiter's escalation — an availability
    // blip, never an overlap.
    if (!stripe->mmap_directory->revalidate_write_lock(write_token)) {
      _write_lock_usurp_aborts.fetch_add(1, std::memory_order_relaxed);
      return make_unexpected(CacheError::Busy);
    }

    // Stuck-intent recovery: a holder PROVEN dead may have died inside its
    // wrap window (see repair_wrap_state).  We hold the lock, so no writer
    // is in that window now.
    repair_after_forced_release(stripe, write_token);

    // Adopt the shared cursor UNCONDITIONALLY whenever it is a valid in-bounds
    // position -- in EITHER direction.  It is loaded under the cross-process
    // write lock, so it is authoritative: by construction it is the end of the
    // last reservation made by ANY process (or, after a wrap, the low position
    // that peer wrapped to).
    //
    // Adopting only a HIGHER value (the pre-fix `shared_pos > write_pos`) is
    // unsound, because a WRAP legitimately drives the shared cursor DOWN.  A
    // peer whose process-local write_pos was left stale-HIGH would refuse the
    // post-wrap value, compute `available` from its stale cursor, conclude it
    // must wrap too, and reserve [data_area_start, ...) -- ALIASING the range
    // the wrapping peer just reserved.  Two writers then pwrite the same bytes,
    // each holding the write lock legitimately in turn and passing
    // revalidate_write_lock(): an overlap produced UNDER the lock, which the
    // usurpation machinery is blind to.  It also toggles the phase once per
    // process per wrap round (phase ABA).  Regression test:
    // tests/integration/test_multiprocess_writers.cpp.
    //
    // The bounds mirror the recovery guard in init_stripes(): a fresh volume
    // has shared_write_pos == 0, which is below data_offset and correctly
    // rejected, leaving the process-local cursor untouched.
    uint64_t shared_pos = stripe->mmap_directory->get_shared_write_pos();
    if (shared_pos >= stripe->data_offset &&
        shared_pos <= stripe->offset + stripe->size) {
      stripe->write_pos = shared_pos;
    }
  }

  uint64_t data_area_start = stripe->data_offset;
  uint64_t data_area_end = stripe->offset + stripe->size;

  // Reject documents that can never fit in the data area
  if (doc_size > data_area_end - data_area_start) {
    if (has_write_lock) stripe->mmap_directory->release_write_lock(write_token);
    return make_unexpected(CacheError::NoSpace);
  }

  uint64_t available = data_area_end - stripe->write_pos;

  if (doc_size > available) {
    // Need to wrap around.  Wrap-intent flag + lease gate.
    //
    // The intent flag is stored (seq_cst) BEFORE the gate loads (lease
    // AND the outstanding-borrow count — a
    // per-shard sum on non-mmap stripes, so "gate-load" below means the
    // gate's load of the READER'S OWN shard; the pairing quantifies per
    // shard), making the
    // writer store-then-load against the reader's store-then-load
    // (borrow-count CAS + lease stamp, then intent/epoch loads) — the
    // shape that closes the Dekker window a bare load-then-store gate
    // leaves open.  Proof, in the seq_cst total order S (identical for
    // the lease stamp and the count RMW; "stamp" below stands for
    // either): if the writer's gate load missed the reader's stamp, then
    // (program order)
    // intent-store <S gate-load <S reader-stamp <S reader-intent-load,
    // so the reader observes intent == 1 and discards the borrow; if the
    // reader instead read intent == 0 from this wrap's CLEAR, the clear
    // is <S-after the wrap-count publish, so the reader's later epoch
    // load sees the changed epoch and discards; and if the reader's
    // intent-load predates this intent-store, its stamp predates our
    // gate load, which therefore sees it and defers.  Either the writer
    // defers or the reader retries — no interleaving lets a borrow escape
    // into a wrapping region.
    //
    // The gate itself runs BEFORE evict_if_needed()/toggle_phase().  A
    // deferred wrap has ZERO side effects (no phase toggle, no directory
    // invalidation, no write_pos mutation; counters only) — the intent
    // flag is cleared and the fill dropped.
    set_wrap_intent(stripe, true);
    writer_seam(WriterSeam::kAfterIntentSet);
    if (!lease_permits_wrap(stripe)) {
      set_wrap_intent(stripe, false);
      _wraps_deferred_by_lease.fetch_add(1, std::memory_order_relaxed);
      _writes_dropped_by_lease.fetch_add(1, std::memory_order_relaxed);
      if (has_write_lock) {
        stripe->mmap_directory->release_write_lock(write_token);
      }
      return make_unexpected(CacheError::NoSpace);
    }

    // Usurpation composition note (write-lock hardening): the hoisted gate
    // above ran BEFORE set_wrap_intent, so a holder escalated earlier
    // commits nothing here.  A holder escalated AFTER that gate (a second
    // multi-second escalation landing inside this short section) can still
    // commit these wrap mutations alongside the usurper's own wrap.  The
    // exposure is bounded: intent-flag and epoch/wrap-count perturbations
    // are reader-detectable (retry/miss), and the worst case — BOTH wraps
    // toggle the phase, netting the phase back and "resurrecting" prior-
    // phase entries — is caught per-read by the full-key verify + CRC
    // envelope and the phase-ABA positional guard at both read choke
    // points (probe + hop).  Detectable degradation, not silent overlap;
    // the undetectable channel stays gated at publish/pwrite below.
    writer_seam(WriterSeam::kAfterGatePassed);
    evict_if_needed(stripe, doc_size);

    // Check again after eviction
    available = data_area_end - stripe->write_pos;
    if (doc_size > available) {
      // Wrap to beginning of data area
      stripe->write_pos = data_area_start;
      // Multi-process: lower the SHARED guard cursor to the wrap target NOW,
      // inside the intent window, not at commit_write_slot.  Readers' phase-
      // ABA positional guard reads shared_write_pos; the wrap below makes
      // every two-wrap survivor in [data_area_start, old cursor) current-phase
      // again, and this wrap's own reservation starts at data_area_start.
      // Left HIGH until commit, the guard would admit such a survivor for the
      // whole reservation->pwrite window, and its borrow revalidates cleanly
      // (intent already cleared, epoch already bumped) -- a torn live borrow
      // once the pwrite lands.  It would also make a wrap whose first fill
      // never commits wrap AGAIN on the next write (the next writer adopts
      // the stale high cursor above), a second phase toggle with no pass in
      // between.
      //
      // Ordering proof (in the style of the wrap-intent Dekker proof above).
      // This release store is program-order BEFORE record_wrap's seq_cst
      // epoch RMW.  A reader captures epoch_start (seq_cst load) before it
      // probes, and the probe loads the cursor (acquire) after that:
      //  - epoch_start reads the post-wrap count: it synchronizes with the
      //    RMW, so the cursor load sees this store or a later one.  Later
      //    stores are commit_write_slot's, published only after the fill is
      //    durable, so the guard never covers unfilled bytes (F6 holds).
      //    Everything at/after data_area_start is rejected until then.
      //  - epoch_start reads the pre-wrap count: borrow_still_valid's later
      //    epoch load (or its intent load) is where the reader catches this
      //    wrap, exactly as before; the cursor value is irrelevant there.
      // Lowering the cursor only ever REJECTS more (a miss), never exposes
      // unfilled bytes.  Revalidate the lock first, like commit_write_slot: a
      // usurped holder must not move the usurper's cursor (its
      // commit_write_slot then fails the same check and skips the insert).
      if (has_write_lock &&
          stripe->mmap_directory->revalidate_write_lock(write_token)) {
        stripe->mmap_directory->set_shared_write_pos(data_area_start);
      }
      writer_seam(WriterSeam::kWrapAfterCursor);
      record_wrap(stripe);
      writer_seam(WriterSeam::kAfterEpochStore);
    }

    // Wrap decision + publish complete (toggle/reset/record done) — only
    // now release the intent flag.  ORDER IS LOAD-BEARING: the reader-side
    // Dekker leg "intent read as 0 from this clear => the epoch load sees
    // the change" holds only because the phase toggle (evict_if_needed)
    // and record_wrap's epoch bump stay program-order BEFORE this store.
    set_wrap_intent(stripe, false);
  }

  // Reserve the write offset.  F6: do NOT advance the guard-visible write
  // cursor here.  stripe->write_pos (the single-process guard cursor) is left
  // at the reservation start, and shared_write_pos (the multi-process guard
  // cursor) is NOT published, until commit_write_slot() runs AFTER the caller's
  // pwrite.  The cross-process write lock is likewise held across the pwrite
  // (see the rationale at the top of this function), so no peer can allocate an
  // overlapping range in the meantime.  The reservation therefore reserves the
  // span WITHOUT ever making it guard-visible before its bytes are durable --
  // closing the reservation-to-pwrite tear window.
  uint64_t write_offset = stripe->write_pos.load(std::memory_order_relaxed);

  WriteSlot slot;
  slot.write_offset = write_offset;
  slot.new_write_pos = write_offset + doc_size;
  slot.write_token = write_token;
  slot.holds_write_lock = has_write_lock;
  slot.deferred_publish = true;

  // Pre-F6 demonstrator seam (TEST ONLY): revert to advancing the guard cursor
  // + releasing the lock AT RESERVATION, before the caller's pwrite --
  // reopening the tear window so the regression tests can exhibit the torn
  // read.
  if (s_advance_cursor_at_reservation_for_test.load(
          std::memory_order_relaxed)) {
    stripe->write_pos.store(slot.new_write_pos, std::memory_order_release);
    if (has_write_lock) {
      // Mirror the original pre-pwrite usurpation gate (old :2036): a peer
      // that force-released us mid-section must not have its position
      // clobbered.
      if (!stripe->mmap_directory->revalidate_write_lock(write_token)) {
        _write_lock_usurp_aborts.fetch_add(1, std::memory_order_relaxed);
        return make_unexpected(CacheError::Busy);
      }
      stripe->mmap_directory->set_shared_write_pos(slot.new_write_pos);
      stripe->mmap_directory->release_write_lock(write_token);
    }
    slot.holds_write_lock = false;
    slot.deferred_publish = false;
  }

  return slot;
}

std::expected<void, CacheError> Volume::commit_write_slot(Stripe* stripe,
                                                          const WriteSlot& slot,
                                                          bool fill_ok) {
  // F6: advance the guard-visible write cursor to cover the now-durable fill,
  // then release the cross-process write lock.  Runs AFTER the caller's pwrite
  // (+ optional sync_on_write fsync), so the cursor never covers un-filled
  // bytes.  On fill failure NOTHING is advanced (invariant: the guard/shared
  // cursor never exceeds the durably-written frontier) -- the reserved span is
  // simply left un-advanced and reused by the next writer, which holds
  // stripe->mutex and (multi-process) re-syncs its local cursor from the
  // unchanged shared cursor.
  if (!slot.deferred_publish) {
    // Pre-F6 demonstrator path already advanced + released at reservation.
    return {};
  }

  if (!slot.holds_write_lock) {
    // Single-process (non-mmap) stripe: stripe->write_pos IS the guard cursor.
    // Publish the advance only now, after the fill is durable.
    if (fill_ok) {
      stripe->write_pos.store(slot.new_write_pos, std::memory_order_release);
    }
    return {};
  }

  // Multi-process: the write lock has been held since allocate_write_slot.
  // Re-validate we still own it -- a multi-second last-resort escalation could
  // have usurped us mid-pwrite -- BEFORE mutating the shared cursor.  On usurp,
  // publish nothing and release nothing (the lock is the usurper's now); the
  // caller then skips its directory insert and OUR fill self-heals as a miss.
  // Honesty note (bounded residual, not closed): this gate cannot recall a
  // pwrite already in flight.  A stalled holder's late pwrite can land over
  // the span the usurper reserved at the same un-advanced cursor and already
  // published, tearing the usurper's bytes under a live entry.  The tear is
  // detectable (CRC + full-key resolve it to Corrupted/miss, never a wrong
  // serve -- pinned by F6-F) and only the escalation deadline bounds it.
  if (!stripe->mmap_directory->revalidate_write_lock(slot.write_token)) {
    _write_lock_usurp_aborts.fetch_add(1, std::memory_order_relaxed);
    return make_unexpected(CacheError::Busy);
  }
  if (fill_ok) {
    // Keep the process-local cursor coherent with the shared one for the next
    // in-process writer, then publish the shared (guard-visible) cursor.
    stripe->write_pos.store(slot.new_write_pos, std::memory_order_release);
    stripe->mmap_directory->set_shared_write_pos(slot.new_write_pos);
  }
  stripe->mmap_directory->release_write_lock(slot.write_token);
  return {};
}

VolumeStats Volume::stats() const {
  VolumeStats result;
  result.bytes_capacity = _config.size;
  result.stripe_count = _stripes.size();
  result.reset_gate_degraded =
      _reset_gate_degraded.load(std::memory_order_relaxed) ? 1 : 0;
  result.resets_under_degraded_gate =
      _resets_under_degraded_gate.load(std::memory_order_relaxed);
  result.resets_gate_verified =
      _resets_gate_verified.load(std::memory_order_relaxed);
  result.reads = reads_total();
  result.writes = _writes.load();
  result.evictions = _evictions.load();
  result.directory_syncs = _directory_syncs.load();
  result.fsyncs = _fsyncs.load();
  result.readahead_hints_issued = _readahead_hints.load();

  // Wrap-cadence telemetry. Count: process-local counter (non-mmap
  // stripes) plus the shared per-stripe counters (mmap stripes), so in
  // multi-process mode wraps by other processes are included. Age: most
  // recent wrap seen locally or published by any process. Intervals:
  // process-local by design (see VolumeStats).
  result.wrap_count = _wrap_count.load(std::memory_order_relaxed);
  result.last_wrap_interval_ns =
      _last_wrap_interval_ns.load(std::memory_order_relaxed);
  result.min_wrap_interval_ns =
      _min_wrap_interval_ns.load(std::memory_order_relaxed);

  // Read-lease counters (process-local; see VolumeStats).
  result.wraps_deferred_by_lease =
      _wraps_deferred_by_lease.load(std::memory_order_relaxed);
  result.writes_dropped_by_lease =
      _writes_dropped_by_lease.load(std::memory_order_relaxed);
  result.wraps_forced_past_lease =
      _wraps_forced_past_lease.load(std::memory_order_relaxed);
  result.tag_collision_evictions =
      _tag_collision_evictions.load(std::memory_order_relaxed);
  result.bucket_full_evictions =
      _bucket_full_evictions.load(std::memory_order_relaxed);
  result.alternate_shadows_unlinked =
      _alternate_shadows_unlinked.load(std::memory_order_relaxed);
  result.alternate_splice_deferred =
      _alternate_splice_deferred.load(std::memory_order_relaxed);
  result.alternate_chain_resets =
      _alternate_chain_resets.load(std::memory_order_relaxed);
  result.alternate_max_chain_depth =
      _alternate_max_chain_depth.load(std::memory_order_relaxed);
  result.alternate_wrap_refusals =
      _alternate_wrap_refusals.load(std::memory_order_relaxed);
  result.write_lock_force_releases =
      _write_lock_force_releases.load(std::memory_order_relaxed);
  result.write_lock_escalation_takeovers =
      _write_lock_escalation_takeovers.load(std::memory_order_relaxed);
  result.write_lock_live_holder_waits =
      _write_lock_live_holder_waits.load(std::memory_order_relaxed);
  result.write_lock_usurp_aborts =
      _write_lock_usurp_aborts.load(std::memory_order_relaxed);
  result.ram_coherence_rejections =
      _ram_coherence_rejections.load(std::memory_order_relaxed);
  result.ram_coherence_put_rejections =
      _ram_coherence_put_rejections.load(std::memory_order_relaxed);
  uint64_t last_wrap_ns = _last_wrap_time_ns.load(std::memory_order_acquire);

  for (const auto& stripe : _stripes) {
    result.entry_count += stripe->entry_count();
    result.bytes_used += stripe->write_pos - stripe->offset;
    result.stripe_bytes += stripe->size;
    if (stripe->use_mmap_directory && stripe->mmap_directory) {
      result.wrap_count += stripe->mmap_directory->shared_wrap_count();
      last_wrap_ns = std::max(
          last_wrap_ns, stripe->mmap_directory->shared_last_wrap_time_ns());
      // Borrow gauge: live borrows registered in the SHARED slot
      // (borrows held by all processes; saturates at 255 per stripe).
      result.borrows_outstanding +=
          borrow_slot::count(stripe->mmap_directory->borrow_slot_raw());
    } else {
      for (const auto& shard : stripe->local_borrow_shards) {
        result.borrows_outstanding +=
            borrow_slot::count(shard.slot.load(std::memory_order_relaxed));
      }
    }
  }

  if (last_wrap_ns != 0) {
    uint64_t now_ns = steady_now_ns();
    result.last_wrap_age_ns = now_ns > last_wrap_ns ? now_ns - last_wrap_ns : 0;
  }

  return result;
}

uint64_t Volume::bytes_used() const {
  uint64_t used = 0;
  for (const auto& stripe : _stripes) {
    used += stripe->write_pos - stripe->offset;
  }
  return used;
}

void Volume::set_ram_cache(std::shared_ptr<RamCache> cache) {
  _ram_cache = std::move(cache);
}

void Volume::evict_from_ram_cache(const CacheKey& key, AlternateId id) {
  if (_ram_cache) {
    _ram_cache->remove(key, id);
  }
}

void Volume::set_hit_tracker(std::shared_ptr<HitTracker> tracker) {
  _hit_tracker = std::move(tracker);
}

namespace {
// F6 scope guard for the held-lock window: between allocate_write_slot
// (which returns with the cross-process write lock HELD in multi-process
// mode) and commit_write_slot (which advances the cursor and releases), any
// unexpected unwind -- a future early return added between them, or an
// exception thrown by embedder/plugin code -- would otherwise leak the lock
// until a waiter's multi-second last-resort escalation recovered it,
// wedging the stripe's writers for that long.  On destruction while still
// armed it releases the still-held lock WITHOUT advancing any cursor (the
// invariant that only commit_write_slot advances the guard-visible cursor
// is preserved; the reserved span becomes a gap reused by the next writer).
// release_write_lock is ownership-checked (it revalidates internally), so a
// lock usurped mid-window is left untouched.  The normal path disarms right
// after commit_write_slot, which has then either released the lock or
// determined it is the usurper's.
class HeldWriteSlotReleaser {
 public:
  HeldWriteSlotReleaser(Stripe* stripe, const Volume::WriteSlot& slot)
      : _stripe(stripe), _slot(slot) {}
  HeldWriteSlotReleaser(const HeldWriteSlotReleaser&) = delete;
  HeldWriteSlotReleaser& operator=(const HeldWriteSlotReleaser&) = delete;
  ~HeldWriteSlotReleaser() {
    if (_armed && _slot.holds_write_lock && _stripe->mmap_directory) {
      _stripe->mmap_directory->release_write_lock(_slot.write_token);
    }
  }
  void disarm() { _armed = false; }

 private:
  Stripe* _stripe;
  const Volume::WriteSlot _slot;
  bool _armed = true;
};
}  // namespace

std::expected<void, CacheError> Volume::commit_write(
    Stripe* stripe, const CacheKey& key, std::span<const std::byte> header,
    std::span<const std::byte> content) {
  if (stripe == nullptr) {
    return make_unexpected(CacheError::NotInitialized);
  }

  // Multi-process: reject writes to non-owned stripes
  if (!stripe->owned) {
    return make_unexpected(CacheError::NotOwned);
  }

  // Build the document
  auto doc_data = DocumentBuilder()
                      .set_key(key)
                      .set_header(header)
                      .set_content(content)
                      .set_type(Document::Type::SingleFrag)
                      .enable_checksum(true)
                      .build();

  if (doc_data.empty()) {
    return make_unexpected(CacheError::InvalidArgument);
  }

  size_t doc_size = doc_data.size();

  std::unique_lock lock(stripe->mutex);

  // Allocate the write slot via the shared lease-gated helper
  // (write-lock handling, capacity checks, lease gate, eviction/wrap,
  // offset reservation — identical at both commit sites by construction).
  auto slot_res = allocate_write_slot(stripe, doc_size);
  if (!slot_res) {
    return make_unexpected(slot_res.error());
  }
  const WriteSlot slot = *slot_res;
  uint64_t write_offset = slot.write_offset;
  // Release the held lock on any unexpected unwind before commit_write_slot
  // (see HeldWriteSlotReleaser above).
  HeldWriteSlotReleaser slot_releaser(stripe, slot);

  // F6 TEST SEAM: freeze the writer inside the reservation-to-pwrite tear
  // window -- the byte range is reserved (multi-process: the write lock is
  // STILL HELD) but the guard-visible cursor has NOT advanced, so a concurrent
  // reader probing a phase-ABA survivor in the reserved span must miss.
  if (s_write_tear_gate_for_test) {
    s_write_tear_gate_for_test(slot.write_offset, slot.new_write_pos);
  }

  // Write data.  F6: the cross-process write lock is STILL HELD across this
  // pwrite (released by commit_write_slot below, after the fill is durable) --
  // no peer can allocate an overlapping range, and the guard-visible cursor is
  // advanced only once the bytes exist.
#ifdef _WIN32
  auto map_result = _mapped_file->map_region(write_offset, doc_size,
                                             MappedFile::MapMode::ReadWrite);
  bool fill_ok = map_result.has_value();
  if (fill_ok) {
    std::memcpy(map_result->data(), doc_data.data(), doc_size);
    if (_config.sync_on_write) {
      _mapped_file->sync(*map_result, MappedFile::SyncMode::Sync);
    }
    _mapped_file->unmap_region(*map_result);
  }
#else
  ssize_t written = pwrite(_fd, doc_data.data(), doc_size, write_offset);
  bool fill_ok = !(written < 0 || static_cast<size_t>(written) != doc_size);
#endif

  // Sync to ensure data is visible to mmap readers (if configured).
  // ORDERING INVARIANT (power loss): this data sync MUST happen before the
  // cursor advance / directory insert below.  A directory entry may only ever
  // be published after the document bytes it points at are durable, so an entry
  // that survives a power loss always points at durable data.  Do not reorder.
  // (On Windows the data region was already flushed via MappedFile::sync right
  // after the memcpy above.)  See the durability contract on sync_directory().
  if (fill_ok && _config.sync_on_write) {
#ifndef _WIN32
    fsync_fd();
#endif
  }

  // F6: advance the guard-visible write cursor to cover the now-durable fill
  // and release the cross-process write lock.  On a mid-pwrite usurp this
  // returns Busy (lock is the usurper's) and we skip the insert -- though our
  // already-landed pwrite may have torn the usurper's span (detectable-only
  // residual; see commit_write_slot); on fill failure it advances nothing and
  // we surface IoError below.
  auto committed = commit_write_slot(stripe, slot, fill_ok);
  slot_releaser.disarm();  // lock released (or usurped) by commit_write_slot
  if (!committed) {
    return make_unexpected(committed.error());
  }
  if (!fill_ok) {
    return make_unexpected(CacheError::IoError);
  }

  // Calculate offset relative to stripe start for directory entry
  if (write_offset < stripe->offset) {
    return make_unexpected(CacheError::InternalError);
  }
  uint64_t relative_offset = write_offset - stripe->offset;
  if (relative_offset + doc_size > stripe->size) {
    return make_unexpected(CacheError::InternalError);
  }

  // Resolve the in-place-update election by FULL key before publishing.
  // A DirEntry only holds a 12-bit tag, so a same-tag entry may belong to a
  // DIFFERENT key colliding on (bucket, tag) — blindly updating it in place
  // silently destroys the victim's entry (its reads then key-verify-fail
  // into a clean NotFound).  Map each candidate's document header and
  // compare the stored first_key; only an entry proven to hold this key is
  // eligible for in-place update.  Runs under stripe->mutex AFTER
  // allocate_write_slot (no phase toggle can race the probe), costs one
  // 132-byte header map per same-tag candidate on the write path only —
  // the read path was already full-key-verified.
  uint64_t verified_offset = Directory::kNoVerifiedEntry;
  stripe->probe_each(key, [&](const DirEntry& dir_entry) {
    if (!is_valid_chain_offset(stripe, dir_entry.offset())) {
      return true;  // Continue to next candidate
    }
    auto mdoc = map_document(*_mapped_file, stripe->offset + dir_entry.offset(),
                             Document::kHeaderSize, stripe->size, false);
    if (!mdoc) {
      return true;  // Unreadable candidate — treat as foreign, keep looking
    }
    CacheKey stored_key = mdoc->reader.first_key();
    _mapped_file->unmap_region(mdoc->region);
    if (stored_key != key) {
      return true;  // Tag collision — different key, keep looking
    }
    verified_offset = dir_entry.offset();
    return false;  // Verified our entry — stop
  });

  // Update directory — publishes the entry.  MUST stay after the data sync
  // above; see the ordering-invariant comment there.
  bool collision_evicted = false;
  bool bucket_full_evicted = false;
  if (!stripe->insert(key, relative_offset, doc_size, verified_offset,
                      &collision_evicted, &bucket_full_evicted)) {
    return make_unexpected(CacheError::InternalError);
  }
  if (collision_evicted) {
    _tag_collision_evictions.fetch_add(1, std::memory_order_relaxed);
  }
  if (bucket_full_evicted) {
    _bucket_full_evictions.fetch_add(1, std::memory_order_relaxed);
  }

#ifdef _WIN32
  // Windows: the data region was flushed above, but dirty mapped-view pages
  // (the directory) are not covered by FlushFileBuffers alone — flush the
  // directory view too so sync_on_write covers the entry it just published.
  // Cost: a second FlushFileBuffers per put, and FlushViewOfFile spans the
  // whole directory region — acceptable because sync_on_write is already the
  // slow/durable path.  Best-effort: a failed flush leaves the entry to the
  // periodic sync_directory() pass.
  if (_config.sync_on_write && stripe->use_mmap_directory &&
      stripe->mmap_directory) {
    (void)_mapped_file->sync(stripe->mmap_directory->region(),
                             MappedFile::SyncMode::Sync);
  }
#endif

  // Update stats
  ++_writes;

  // Write-around: do NOT populate RAM cache on writes.
  // In multi-process mode, only the writing process would benefit;
  // other processes (e.g. nginx) would still hold stale RAM entries.
  // Reads populate the RAM cache on miss (see read_alternate_sync).
  //
  // Write-around still has to INVALIDATE, though.  This path publishes a head
  // carrying the Original id (DocumentBuilder's default), which is exactly the
  // (key, id) the RAM cache is keyed by — so a copy an earlier read left in
  // RAM now describes a superseded document, and both read paths would serve
  // it.  Same eviction, same post-publish ordering and the same reasoning as
  // in commit_alternate_write; see the full argument there.
  if (_ram_cache) {
    stripe->remove_epoch.fetch_add(1, std::memory_order_acq_rel);
    _ram_cache->remove(key, AlternateId::Original);
  }

  return {};
}

void Volume::evict_if_needed(Stripe* stripe, size_t required_space) {
  (void)required_space;

  // Toggle the GC phase. All directory entries from the previous phase
  // become stale: insert() can overwrite them, and probe/probe_each skip
  // them. This implements ATS-inspired phase-based eviction — when the
  // write position wraps, old entries pointing to now-overwritten disk
  // regions are logically invalidated in a single O(1) operation.
  //
  // INVARIANT (2-wrap phase ABA): phase() is a single bit, so an entry that
  // SURVIVES its bucket through TWO wraps of this stripe reads as
  // current-phase again and is no longer skipped as stale.  Two cases:
  //
  // - Offset REUSED by a later pass: the entry aliases a DIFFERENT
  //   document.  Under SINGLE-THREADED construction this cannot become a
  //   wrong serve — the read path re-verifies the stored 256-bit first_key
  //   (a DirEntry carries only a 12-bit tag), so the aliased entry
  //   resolves to a clean miss or the correct live document; and any
  //   directory-reachable document is fully written and synced before its
  //   entry publishes (commit_write's ordering invariant), so the
  //   never-invalidated checksum-validation cache cannot turn the reuse
  //   into a torn serve.  Pinned by test_wrap_phase_aba.cpp (A)/(B).  Do
  //   not weaken the read-path first_key check on the assumption that the
  //   phase bit alone evicts stale entries.
  //
  // - Offset NOT reused (trailing-gap survivor, variable doc sizes): the
  //   entry points at INTACT bytes sitting AHEAD of write_pos.  A borrow
  //   taken through it passes every gauntlet leg legitimately, yet the
  //   region is later overwritten by the plain in-pass FORWARD fill — no
  //   wrap event, so neither lease_permits_wrap nor the intent/epoch
  //   revalidation runs (both live in allocate_write_slot's wrap branch
  //   only).  This VOIDS the renew_lease premise that "any overwrite of a
  //   borrowed region is a wrap" (see the verdict comment there) and
  //   tears bytes under a live, in-term lease.  KNOWN GAP, tracked
  //   separately; demonstrated deterministically by the [!mayfail]
  //   forward-fill test in test_wrap_phase_aba.cpp (C).
  if (stripe->use_mmap_directory && stripe->mmap_directory) {
    stripe->mmap_directory->toggle_phase();
  } else if (stripe->directory) {
    stripe->directory->toggle_phase();
  }

  ++_evictions;
}

std::expected<WriteHandle, CacheError> Volume::write_alternate_sync(
    const CacheKey& key, AlternateId alternate_id, uint64_t content_length) {
  Stripe* stripe = select_stripe(key);
  if (stripe == nullptr) {
    return make_unexpected(CacheError::NotInitialized);
  }

  // Multi-process: reject writes to non-owned stripes
  if (!stripe->owned) {
    return make_unexpected(CacheError::NotOwned);
  }

  // Same declared-length early reject as write_sync.
  if (_config.max_object_size > 0 && content_length > _config.max_object_size) {
    return make_unexpected(CacheError::ObjectTooLarge);
  }

  auto impl = std::make_shared<VolumeAlternateWriteHandleImpl>();
  impl->volume_raw = this;
  impl->volume_weak = weak_from_this();
  // Empty weak_ptr => no shared owner (constructed directly, not via
  // Cache::add_volume): nothing can free it behind our back, so the raw
  // pointer stands.
  impl->guarded = (impl->volume_weak.use_count() > 0);
  impl->stripe = stripe;
  impl->key = key;
  impl->alternate_id = alternate_id;
  impl->expected_length = content_length;

  return WriteHandle(impl);
}

std::expected<void, CacheError> Volume::commit_alternate_write(
    Stripe* stripe, const CacheKey& key, AlternateId alternate_id,
    std::span<const std::byte> header, std::span<const std::byte> content) {
  if (stripe == nullptr) {
    return make_unexpected(CacheError::NotInitialized);
  }

  // Multi-process: reject writes to non-owned stripes
  if (!stripe->owned) {
    return make_unexpected(CacheError::NotOwned);
  }

  std::unique_lock lock(stripe->mutex);

  // FENCE DISCIPLINE (load-bearing).  ONE wrap-epoch sample, taken here --
  // before the probe below resolves ANY offset.  commit_header_rmw's contract
  // requires the sample to predate the resolution of every offset it is later
  // asked to store into, and this function resolves the whole chain during
  // the probe walk, i.e. BEFORE allocate_write_slot.  A fresh sample taken
  // after the allocation would fence out a wrap that happened inside our OWN
  // allocation, and the splice below could then store into freshly recycled
  // bytes.  The same sample fences the wrap-frontier link refusal further
  // down.  Never re-sample.
  const auto epoch_start = wrap_epoch(stripe);

  // Find the current head document offset
  uint64_t head_relative_offset = 0;
  std::bitset<256> unique_alternate_ids;
  bool key_exists = false;
  bool chain_truncated = false;
  bool cycle_detected = false;
  // True iff the walk saw the WHOLE chain, i.e. it ended on next == 0.  When
  // false the tail beyond the last visited node is not visible to us, so the
  // splice must not drop it.
  bool chain_fully_walked = false;

  // One entry per visited chain node, in walk order (head first).  This is
  // also the cycle-detection visited set: the walk stops on a repeated
  // offset, so it terminates on a cyclic chain without relying on the depth
  // cap.  The walk already loads exactly these three fields for the unique-id
  // count, so tracking them costs no extra I/O at any depth.  Deliberately
  // NOT zero-initialised: only slots [0, node_count) are ever read,
  // and every such slot was written by the walk below.
  struct ChainNode {
    uint64_t rel_offset;
    uint64_t next_rel;
    uint8_t id;
  };
  std::array<ChainNode, Document::kMaxChainTraversalDepth> nodes;
  size_t node_count = 0;

  stripe->probe_each(key, [&](const DirEntry& dir_entry) {
    // Directory::probe_each may re-invoke a callback for the same entry when
    // a version change retries the scan, so every piece of state this
    // callback accumulates is reset on entry.  Carrying a stale predecessor
    // across a retried scan would splice against a different chain snapshot.
    head_relative_offset = 0;
    unique_alternate_ids.reset();
    key_exists = false;
    chain_truncated = false;
    cycle_detected = false;
    chain_fully_walked = false;
    node_count = 0;

    uint64_t doc_offset = stripe->offset + dir_entry.offset();

    auto mapped = _mapped_file->map_region(doc_offset, Document::kHeaderSize,
                                           MappedFile::MapMode::ReadOnly);
    if (!mapped) {
      return true;
    }

    DocumentReader reader(*mapped);
    if (!reader.is_valid()) {
      _mapped_file->unmap_region(*mapped);
      return true;
    }

    CacheKey stored_key = reader.first_key();
    if (stored_key != key) {
      _mapped_file->unmap_region(*mapped);
      return true;
    }

    // Found the head - count alternates in chain
    key_exists = true;
    head_relative_offset = dir_entry.offset();

    // Count unique alternate IDs in the chain (duplicates don't count
    // toward the limit — they arise from concurrent nginx writes).
    uint64_t current_offset = doc_offset;
    uint64_t current_rel = dir_entry.offset();
    while (current_offset != 0 &&
           node_count < Document::kMaxChainTraversalDepth) {
      // Cycle guard.  O(d^2) compares with d <= 128 (1-8 in steady state) on
      // an array already in cache — nanoseconds, and it is what makes the
      // splice's "the successor is a descendant in an ACYCLIC walk" argument
      // hold.
      bool repeat = false;
      for (size_t i = 0; i < node_count; ++i) {
        if (nodes[i].rel_offset == current_rel) {
          repeat = true;
          break;
        }
      }
      if (repeat) {
        cycle_detected = true;
        break;
      }

      auto doc = map_document(*_mapped_file, current_offset,
                              Document::kHeaderSize, stripe->size, false);
      if (!doc) break;

      unique_alternate_ids.set(doc->reader.document().alternate_id);
      uint64_t next_offset = doc->reader.document().next_alternate_offset;
      nodes[node_count] =
          ChainNode{current_rel, next_offset,
                    static_cast<uint8_t>(doc->reader.document().alternate_id)};
      ++node_count;
      _mapped_file->unmap_region(doc->region);

      if (next_offset == 0) {
        current_offset = 0;
        chain_fully_walked = true;
        break;
      }
      if (!is_valid_chain_offset(stripe, next_offset)) break;
      current_rel = next_offset;
      current_offset = stripe->offset + next_offset;
    }

    // Detect truncated traversal: the loop exited because node_count hit the
    // limit while there were still more nodes to visit.  A detected cycle
    // counts as truncated too — before the cycle guard existed such a chain
    // spun to the cap and landed here, and the heal below is what breaks it.
    if ((current_offset != 0 &&
         node_count >= Document::kMaxChainTraversalDepth) ||
        cycle_detected) {
      chain_truncated = true;
    }

    _mapped_file->unmap_region(*mapped);
    return false;
  });

  // Depth high-water mark: recorded for EVERY alternate write, including the
  // ones rejected below — a rejected write is exactly when the operator most
  // needs the number.
  cas_max_counter(_alternate_max_chain_depth, node_count);

  const auto write_id = static_cast<uint8_t>(alternate_id);

  // Chain reset at the traversal boundary (the backstop).  When the walk
  // could not see the whole chain AND every node it did see carries the id we
  // are about to write, all of them are superseded copies of this write, so
  // starting a fresh chain loses nothing a reader could reach: nodes past the
  // cap are already invisible to every walk (readers use the same cap), and a
  // cycle's nodes are all accounted for.  Without this, a key whose splices
  // were deferred often enough becomes permanently unwritable.  Deliberately
  // NOT extended to multi-id chains: those may hide reachable distinct
  // alternates behind the cap, and dropping them is a data loss the caller
  // never asked for -- they still get TooManyAlternates.
  //
  // Independent of the unlink kill switch: this is a safety valve, not the
  // optimization.
  bool chain_reset = false;
  if (chain_truncated) {
    if (unique_alternate_ids.count() == 1 &&
        unique_alternate_ids.test(write_id)) {
      chain_reset = true;
      chain_truncated = false;
    }
  }

  // Reject if the chain was too long to fully traverse
  if (chain_truncated) {
    return make_unexpected(CacheError::TooManyAlternates);
  }

  // Check unique alternate count limit
  if (unique_alternate_ids.count() >= Document::kMaxAlternates) {
    return make_unexpected(CacheError::TooManyAlternates);
  }

  // ---- Plan the unlink of superseded same-id nodes -----------------------
  //
  // Re-recording an alternate id prepends a new document; without this the
  // superseded copy stays linked forever and the physical chain grows with
  // every refresh until the traversal cap wedges the key.  The plan is the
  // standard delete-all-matching pass over a singly linked list, computed
  // from the walk above (no extra I/O): the new head links to the first
  // surviving node, and each surviving node that is followed by a run of
  // superseded ones is repointed past that run.
  //
  // Two cases, and only the second one costs anything:
  //   * the superseded node is the current head (always, for the dominant
  //     "one id, re-recorded" shape) — handled entirely at BUILD time by
  //     stamping a different next offset into the new document.  Zero extra
  //     I/O, zero extra stores, no new failure mode.
  //   * the superseded node sits mid-chain (multi-id keys) — needs the
  //     in-place repoint, which happens AFTER the new head is published.
  //
  // Ordering is load-bearing: publish first, splice second.  Before the
  // splice a reader sees [new, .., P, S, ..]; after it, [new, .., P, S.next,
  // ..].  Both are well-formed.  The reverse order opens a window in which
  // the id has no reachable version at all.  Deliberately NOT
  // zero-initialised: only slots [0, repoint_count) are ever read,
  // and every such slot was written by the planner below.
  struct PendingRepoint {
    uint64_t pred_rel;      // predecessor, relative to the stripe
    uint64_t new_next;      // what its next_alternate_offset must become
    uint8_t pred_id;        // identity check for commit_header_rmw
    uint32_t shadow_count;  // superseded nodes this store removes
  };
  std::array<PendingRepoint, Document::kMaxChainTraversalDepth> repoints;
  size_t repoint_count = 0;
  uint64_t shadows_unlinked_at_publish = 0;
  uint64_t shadows_deferred = 0;

  uint64_t new_next_offset = key_exists ? head_relative_offset : 0;

  if (chain_reset) {
    new_next_offset = 0;
  } else if (_config.unlink_superseded_alternates && key_exists &&
             node_count > 0) {
    // Index of the first surviving (non-superseded) node, and the shadows
    // ahead of it that the build-time splice removes for free.
    size_t first_keeper = node_count;
    for (size_t i = 0; i < node_count; ++i) {
      if (nodes[i].id != write_id) {
        first_keeper = i;
        break;
      }
    }

    if (first_keeper == node_count) {
      // Every visible node is superseded.  Dropping them all is only safe
      // when the walk reached the real end of the chain; otherwise the
      // invisible tail would go with them.
      if (chain_fully_walked) {
        new_next_offset = 0;
        shadows_unlinked_at_publish = node_count;
      } else {
        shadows_deferred += node_count;
      }
    } else {
      if (first_keeper > 0) {
        // The head (and possibly a run behind it) is superseded: link the new
        // document straight past it.  Re-validate the hop the same way the
        // walk did; it passed moments ago under this lock and the cursor has
        // not moved, so a failure here means something is wrong and we fall
        // back to today's plain prepend.
        if (is_valid_chain_offset(stripe, nodes[first_keeper].rel_offset)) {
          new_next_offset = nodes[first_keeper].rel_offset;
          shadows_unlinked_at_publish = first_keeper;
        } else {
          shadows_deferred += first_keeper;
        }
      }

      // Mid-chain runs: repoint each surviving node past the superseded nodes
      // that follow it.  Built front-to-back, and executed in that order (see
      // repoint_chain_link's ordering invariant).
      size_t prev_keeper = first_keeper;
      for (size_t i = first_keeper + 1; i <= node_count; ++i) {
        const bool is_keeper = (i < node_count && nodes[i].id != write_id);
        if (!is_keeper && i < node_count) {
          continue;  // still inside a run of superseded nodes
        }
        const size_t run = i - prev_keeper - 1;  // shadows since prev_keeper
        if (run > 0) {
          if (i < node_count) {
            repoints[repoint_count++] = PendingRepoint{
                nodes[prev_keeper].rel_offset, nodes[i].rel_offset,
                nodes[prev_keeper].id, static_cast<uint32_t>(run)};
          } else if (chain_fully_walked) {
            // Trailing run: terminate the chain at the last survivor.  Only
            // safe when the walk saw the end of the chain.
            repoints[repoint_count++] = PendingRepoint{
                nodes[prev_keeper].rel_offset, 0, nodes[prev_keeper].id,
                static_cast<uint32_t>(run)};
          } else {
            shadows_deferred += run;
          }
        }
        prev_keeper = i;
      }
    }

    // A cycle means our notion of "predecessor" is unreliable, so no in-place
    // store may run.  (Unreachable today: a detected cycle is treated as a
    // truncated walk above, which either heals or rejects before we get here.
    // Kept because the splice's no-cycle-created proof depends on it.)
    if (cycle_detected) {
      for (size_t i = 0; i < repoint_count; ++i) {
        shadows_deferred += repoints[i].shadow_count;
      }
      repoint_count = 0;
    }
  }

  // Get current time for last_access
  auto now = std::chrono::system_clock::now();
  auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now.time_since_epoch())
                    .count();

  // Build the document with alternate chain fields
  auto doc_data = DocumentBuilder()
                      .set_key(key)
                      .set_header(header)
                      .set_content(content)
                      .set_type(Document::Type::SingleFrag)
                      .set_alternate_id(static_cast<uint8_t>(alternate_id))
                      .set_next_alternate_offset(new_next_offset)
                      .set_hit_count(0)
                      .set_last_access(now_ms)
                      .enable_checksum(true)
                      .build();

  if (doc_data.empty()) {
    return make_unexpected(CacheError::InvalidArgument);
  }

  size_t doc_size = doc_data.size();

  // Allocate the write slot via the shared lease-gated helper
  // (write-lock handling, capacity checks, lease gate, eviction/wrap,
  // offset reservation — identical at both commit sites by construction).
  auto slot_res = allocate_write_slot(stripe, doc_size);
  if (!slot_res) {
    return make_unexpected(slot_res.error());
  }
  const WriteSlot slot = *slot_res;
  uint64_t write_offset = slot.write_offset;
  // Release the held lock on any unexpected unwind before commit_write_slot
  // (see HeldWriteSlotReleaser at commit_write).
  HeldWriteSlotReleaser slot_releaser(stripe, slot);

  // WRAP-FRONTIER LINK REFUSAL.  The offset we stamped as this document's
  // next_alternate_offset was resolved before the allocation.  Allocating can
  // WRAP the stripe (its own or a peer's), and a wrap is the only way
  // behind-cursor bytes get reused — so on a moved epoch that offset may name
  // bytes the ring is refilling right now.  Linking to it would publish a
  // pointer into a document that is about to become someone else's: the
  // positional hop guard rejects it only until the cursor passes it again,
  // after which walks follow it into foreign or cyclic territory.
  //
  // Refuse the link instead: zero the field in the buffer before the fill, so
  // this write starts a FRESH chain.  The pre-wrap nodes are orphaned, which
  // is the honest outcome — the ring has already begun overwriting them.  The
  // epoch compare is exactly equivalent to "a wrap happened" in both
  // directions (only allocate_write_slot's wrap branch moves the count or the
  // phase), so it cannot spuriously discard a healthy chain.
  //
  // NOT gated by the unlink kill switch: this is a safety guard, and the
  // unlink makes its precondition MORE likely (a spliced head points further
  // back, i.e. at the bytes a wrap recycles first).  The two ship together.
  const bool wrap_raced_write = (wrap_epoch(stripe) != epoch_start);
  // A refusal is only COUNTED when the stamped link was actually live
  //: new_next_offset is still the value the planner stamped into the
  // document, and it is already 0 when there was no pre-wrap chain to orphan
  // -- the first write of a key, a chain reset, or the planner itself
  // choosing a fresh chain.  Zeroing an already-zero field refuses nothing,
  // so those wrap-raced writes are ordinary wraps, not wrap-race events.
  const bool link_refused = wrap_raced_write && new_next_offset != 0;
  if (wrap_raced_write) {
    std::memset(doc_data.data() + Document::kNextAlternateOffsetPos, 0,
                sizeof(uint64_t));
    // Every offset resolved during the walk is suspect for the same reason,
    // so no in-place splice may run either.  (repoint_chain_link's fence
    // would reject them anyway; dropping the plan here saves the syscalls.)
    // Neither shadow counter moves, including any deferral the planner had
    // already recorded: the refusal detaches the WHOLE old chain, so nothing
    // is left linked (not a deferral) and nothing was spliced (not the
    // mechanism working).
    repoint_count = 0;
    shadows_unlinked_at_publish = 0;
    shadows_deferred = 0;
  }

  // F6 TEST SEAM: freeze inside the reservation-to-pwrite tear window (see
  // commit_write for the full note).
  if (s_write_tear_gate_for_test) {
    s_write_tear_gate_for_test(slot.write_offset, slot.new_write_pos);
  }

  // Write data.  F6: the cross-process write lock is STILL HELD across this
  // pwrite (released by commit_write_slot below, after the fill is durable).
  // If the fill fails, the reserved region becomes a harmless "gap" (the cursor
  // is NOT advanced past it), reclaimed at the next wrap-around.
#ifdef _WIN32
  auto map_result = _mapped_file->map_region(write_offset, doc_size,
                                             MappedFile::MapMode::ReadWrite);
  bool fill_ok = map_result.has_value();
  if (fill_ok) {
    std::memcpy(map_result->data(), doc_data.data(), doc_size);
    if (_config.sync_on_write) {
      _mapped_file->sync(*map_result, MappedFile::SyncMode::Sync);
    }
    _mapped_file->unmap_region(*map_result);
  }
#else
  ssize_t written = pwrite(_fd, doc_data.data(), doc_size, write_offset);
  bool fill_ok = !(written < 0 || static_cast<size_t>(written) != doc_size);
#endif

  // Sync if configured.
  // ORDERING INVARIANT (power loss): this data sync MUST happen before the
  // cursor advance / directory insert below -- an entry that survives a power
  // loss must always point at durable data.  Do not reorder.  See
  // commit_write() and the durability contract on sync_directory().
  if (fill_ok && _config.sync_on_write) {
#ifndef _WIN32
    fsync_fd();
#endif
  }

  // F6: advance the guard-visible cursor + release the write lock now that the
  // fill is durable (Busy on a mid-pwrite usurp -> skip the insert; IoError on
  // fill failure).
  auto committed = commit_write_slot(stripe, slot, fill_ok);
  slot_releaser.disarm();  // lock released (or usurped) by commit_write_slot
  if (!committed) {
    return make_unexpected(committed.error());
  }
  if (!fill_ok) {
    return make_unexpected(CacheError::IoError);
  }

  // Calculate offset relative to stripe start
  if (write_offset < stripe->offset) {
    return make_unexpected(CacheError::InternalError);
  }
  uint64_t relative_offset = write_offset - stripe->offset;
  if (relative_offset + doc_size > stripe->size) {
    return make_unexpected(CacheError::InternalError);
  }

  // Update directory to point to new head — publishes the entry.  MUST stay
  // after the data sync above; see the ordering-invariant comment there.
  // insert() uses its own per-bucket seqlock for synchronization.
  // head_relative_offset was verified by full first_key comparison in the
  // probe above, so only that entry may be updated in place — a same-tag
  // entry holding a colliding foreign key is preserved (or, full bucket,
  // evicted and counted).
  bool collision_evicted = false;
  bool bucket_full_evicted = false;
  if (!stripe->insert(key, relative_offset, doc_size,
                      (key_exists && !wrap_raced_write)
                          ? head_relative_offset
                          : Directory::kNoVerifiedEntry,
                      &collision_evicted, &bucket_full_evicted)) {
    return make_unexpected(CacheError::InternalError);
  }
  if (collision_evicted) {
    _tag_collision_evictions.fetch_add(1, std::memory_order_relaxed);
  }
  if (bucket_full_evicted) {
    _bucket_full_evictions.fetch_add(1, std::memory_order_relaxed);
  }

#ifdef _WIN32
  // Windows: flush the directory view so sync_on_write covers the entry it
  // just published (mirrors commit_write; see the cost note there).
  if (_config.sync_on_write && stripe->use_mmap_directory &&
      stripe->mmap_directory) {
    (void)_mapped_file->sync(stripe->mmap_directory->region(),
                             MappedFile::SyncMode::Sync);
  }
#endif

  ++_writes;

  // ---- Splice the superseded mid-chain nodes out (post-publish) ----------
  //
  // Best-effort by contract: the caller's document is already written and
  // published, so a failure here must never fail the write.  A store that is
  // fenced out (Busy) or a predecessor we cannot map simply leaves the
  // superseded node linked — which is exactly the behaviour of a binary
  // without this mechanism — and the next write retries the splice.  Count it
  // instead, so the degradation is visible rather than silent.
  //
  // Front-to-back (see repoint_chain_link): after each store the chain is a
  // valid, merely longer, list, so a crash or an abandoned pass anywhere in
  // here leaves a chain every reader can walk.
  for (size_t i = 0; i < repoint_count; ++i) {
    const PendingRepoint& r = repoints[i];
    auto res = repoint_chain_link(stripe, stripe->offset + r.pred_rel,
                                  static_cast<AlternateId>(r.pred_id), key,
                                  r.new_next, epoch_start, /*blocking=*/false);
    if (res.has_value()) {
      shadows_unlinked_at_publish += r.shadow_count;
    } else {
      shadows_deferred += r.shadow_count;
    }
  }
  if (shadows_unlinked_at_publish != 0) {
    _alternate_shadows_unlinked.fetch_add(shadows_unlinked_at_publish,
                                          std::memory_order_relaxed);
  }
  if (shadows_deferred != 0) {
    _alternate_splice_deferred.fetch_add(shadows_deferred,
                                         std::memory_order_relaxed);
  }
  if (chain_reset) {
    _alternate_chain_resets.fetch_add(1, std::memory_order_relaxed);
  }
  if (link_refused) {
    // The wrap-frontier refusal: counted here, post-publish, so a
    // write that failed after zeroing the link never moves it -- the counter
    // means "a fresh chain was started because a wrap raced the write, and a
    // live pre-wrap chain was orphaned by it".
    _alternate_wrap_refusals.fetch_add(1, std::memory_order_relaxed);
  }

  // ---- Invalidate the RAM copy this write supersedes (post-publish) ------
  //
  // Writes stay write-around: this does NOT populate the RAM cache (see the
  // rationale in commit_write).  But the version we just superseded may
  // already be IN it, put there by an earlier read — and read_alternate_sync
  // consults RAM after the selector picks, keyed by (key, selected id).  With
  // no eviction here, every later read of this id is a RAM hit returning the
  // superseded bytes: pinned, silent, and entirely unaffected by the chain
  // being correct on disk.
  //
  // ONE remove covers the whole write.  Every node this commit detached
  // carries OUR id by construction — the unlink planner only ever drops nodes
  // whose id equals write_id, and the single-id chain reset fires only when
  // every visible node is that same id — so they all share the (key, id) RAM
  // key with the version being superseded.
  //
  // ORDERING (load-bearing).  Both steps run AFTER the directory insert
  // published the new head, in this order: bump the stripe's remove
  // generation, then drop the entry.
  //
  // A lock-free reader that missed RAM copies the OLD document into RAM with
  // no stripe lock held, so its put may land at any point relative to us.  It
  // samples the remove generation before its probe and re-checks it in its
  // conditional put's predicate — evaluated under the same RAM-cache
  // write lock our remove takes — dropping the put at insert time when the
  // generation moved (the resurrection guard remove_sync and
  // remove_alternate_sync already rely on).  That closes both sides:
  //
  //   * a reader that sampled the generation BEFORE our bump either lands its
  //     put before our remove — we evict it — or after it, and then its put's
  //     predicate sees the bump and the put is never inserted;
  //   * a reader that sampled it AFTER our bump synchronizes-with that bump,
  //     and therefore with the publish sequenced before it, so its probe sees
  //     the NEW head and caches the NEW bytes.
  //
  // So no superseded copy SURVIVES: every stale put is either evicted by us
  // or rejected by its own author.  The mirror-image placement —
  // invalidating at the TOP of the function, where the remove paths put
  // their bump before the remove-ordering fix moved it after the mutation —
  // would instead leave a permanent one: a reader sampling between that bump
  // and the publish still walks the OLD chain, and nothing would ever reject
  // its stale put.  So this mirrors the removes' MECHANISM, not their position,
  // and follows the same "publish first, fix up second" rule as the splice
  // above.
  //
  // The reader-side half of this protocol is the conditional put:
  // the read path's predicate re-checks this generation while the RAM
  // cache holds the write lock that serializes our eviction, so a raced
  // put that lands after our eviction is rejected at insert time instead
  // of landing and then being withdrawn — and one that lands before it is
  // removed by that eviction.  No superseded copy OUTLIVES the
  // invalidation.  (Before the conditional put the guard was put-THEN-undo: the
  // raced put was withdrawn only after landing, so for the few instructions in
  // between the older bytes could be served to a third reader — a
  // transient, self-healing version dip.)  A generation bump that lands
  // inside the put's own critical section is still withdrawn, by the
  // reader's post-put re-check or by this eviction, whichever comes
  // first.  The concurrency case in
  // tests/integration/test_ram_alternate_invalidation.cpp now asserts the
  // per-reader monotonicity this buys.
  //
  // SCOPE: process-local, like the remove paths this follows.  The RAM cache
  // lives in this process's heap and remove_epoch is a process-local counter,
  // so a peer process that has the superseded version in ITS RAM cache is not
  // reached from here and keeps serving it.  That exposure is inherent to a
  // per-process RAM cache in front of a shared volume — it needs a shared
  // invalidation signal, not a commit-side eviction — and is unchanged by this
  // fix, which closes the single-process case completely.
  //
  // Sited after every early return, so a write that failed to publish never
  // evicts a RAM entry that is still the correct answer.
  if (_ram_cache) {
    stripe->remove_epoch.fetch_add(1, std::memory_order_acq_rel);
    _ram_cache->remove(key, alternate_id);
  }

  return {};
}

std::expected<std::vector<AlternateInfo>, CacheError>
Volume::list_alternates_sync(const CacheKey& key) {
  Stripe* stripe = select_stripe(key);
  if (stripe == nullptr) {
    return make_unexpected(CacheError::NotInitialized);
  }

  // Retry loop for torn/raced read handling.  Retries were historically
  // multi-process-only (only remote writers could race a read); readers now
  // take no stripe lock in ANY mode, so a local writer's wrap can race a
  // read the same way a remote one always could — checksum validation and
  // the wrap-epoch revalidation catch it and land here for another
  // attempt.
  uint32_t max_attempts = _mp_config.max_read_retries + 1;

  for (uint32_t attempt = 0; attempt < max_attempts; ++attempt) {
    if (attempt > 0) {
      std::this_thread::yield();
    }

    // Lock-free read — readers take no stripe lock in any mode; see the
    // correctness note in read_sync().

    // Capture the wrap epoch so a wrap racing the walk is
    // detected.  This walk takes no borrow/lease (no handle escapes), so
    // without the check a mid-walk overwrite could yield AlternateInfo
    // built from freshly overwritten regions.
    const auto epoch_start = wrap_epoch(stripe);

    std::vector<AlternateInfo> alternates;
    std::expected<std::vector<AlternateInfo>, CacheError> result =
        make_unexpected(CacheError::NotFound);
    bool found_head = false;
    bool checksum_failed = false;
    bool chain_incomplete = false;

    // First, find the head document via directory probe
    stripe->probe_each(key, [&](const DirEntry& dir_entry) {
      if (found_head) {
        return false;  // Already processing, stop iteration
      }

      uint64_t doc_offset = stripe->offset + dir_entry.offset();

      size_t alt_map_size = dir_entry.approx_size();
      auto mapped = _mapped_file->map_region(doc_offset, alt_map_size,
                                             MappedFile::MapMode::ReadOnly);
      if (!mapped) {
        return true;  // Continue to next candidate
      }

      DocumentReader reader(*mapped);
      if (!reader.is_valid()) {
        _mapped_file->unmap_region(*mapped);
        return true;  // Continue to next candidate
      }

      // Remap if approx_size underestimates actual document length
      if (reader.document().len > alt_map_size) {
        if (reader.document().len > stripe->size) {
          _mapped_file->unmap_region(*mapped);
          return true;  // Skip corrupted entry
        }
        _mapped_file->unmap_region(*mapped);
        alt_map_size = reader.document().len;
        mapped = _mapped_file->map_region(doc_offset, alt_map_size,
                                          MappedFile::MapMode::ReadOnly);
        if (!mapped) {
          return true;
        }
        reader = DocumentReader(*mapped);
        if (!reader.is_valid()) {
          _mapped_file->unmap_region(*mapped);
          return true;
        }
      }

      // Verify the key matches
      CacheKey stored_key = reader.first_key();
      if (stored_key != key) {
        _mapped_file->unmap_region(*mapped);
        return true;  // Continue to next candidate
      }

      // Skip checksum during chain enumeration — only header fields
      // are needed for AlternateInfo.

      // Found the head document - now traverse the chain
      found_head = true;
      uint64_t current_offset = doc_offset;
      size_t chain_depth = 0;

      while (current_offset != 0 &&
             chain_depth < Document::kMaxChainTraversalDepth) {
        constexpr size_t kInitialMapSize =
            Document::kHeaderSize + static_cast<size_t>(64 * 1024);
        auto doc = map_document(*_mapped_file, current_offset, kInitialMapSize,
                                stripe->size, true);
        if (!doc) {
          chain_incomplete = true;
          break;
        }

        // Cross-key escape guard — see the identical check in
        // read_alternate_sync's walk.
        if (doc->reader.first_key() != key) {
          _mapped_file->unmap_region(doc->region);
          chain_incomplete = true;
          break;
        }

        alternates.push_back(build_alternate_info(doc->reader, current_offset));

        uint64_t next_offset = doc->reader.document().next_alternate_offset;
        _mapped_file->unmap_region(doc->region);

        if (next_offset == 0) break;
        if (!is_valid_chain_offset(stripe, next_offset)) break;
        ++chain_depth;
        current_offset = stripe->offset + next_offset;
      }

      // Check for potential chain corruption (cycle detection)
      if (chain_depth >= Document::kMaxChainTraversalDepth) {
        // Possible cycle — keep any alternates collected so far
        checksum_failed = true;  // Trigger retry if alternates empty
      }

      // Graceful degradation: if we collected valid alternates before
      // encountering corruption or chain incompleteness, return them.
      // Only fail if zero alternates were collected.
      if (!alternates.empty()) {
        result = std::move(alternates);
      } else if (chain_incomplete || checksum_failed) {
        // No alternates at all — mark for retry
        checksum_failed = true;
      }
      return false;  // Stop iteration
    });

    // Discard and retry if a wrap raced the walk — the collected
    // AlternateInfo may describe overwritten regions.
    // Read-side acquire fence (see renew_read_lease): the walk copied
    // header bytes out of the live mmap with plain loads
    // (build_alternate_info; checksum is skipped during enumeration) and
    // takes no borrow, so no seq_cst RMW interposes between those copies
    // and this verdict — pin the copy loads ahead of the epoch load.
    std::atomic_thread_fence(std::memory_order_acquire);
    if (result.has_value() && wrap_epoch(stripe) != epoch_start) {
      result = make_unexpected(CacheError::NotFound);
      checksum_failed = true;
    }

    if (result.has_value()) {
      return result;  // Got alternates (possibly partial), return them
    }
    if (!checksum_failed) {
      return result;  // NotFound or other non-retriable error
    }
    // Checksum failed with no usable alternates, retry if we have attempts left
  }

  // All retries exhausted with checksum failures — report corruption
  // rather than a misleading NotFound.
  return make_unexpected(CacheError::Corrupted);
}

std::expected<ReadHandle, CacheError> Volume::read_alternate_sync(
    const CacheKey& key, const StorageAlternateSelector& selector,
    const AlternateSelectionContext& ctx) {
  Stripe* stripe = select_stripe(key);
  if (stripe == nullptr) {
    return make_unexpected(CacheError::NotInitialized);
  }

  // Retry loop for torn/raced read handling.  Retries were historically
  // multi-process-only (only remote writers could race a read); readers now
  // take no stripe lock in ANY mode, so a local writer's wrap can race a
  // read the same way a remote one always could — checksum validation and
  // the wrap-epoch revalidation catch it and land here for another
  // attempt.
  uint32_t max_attempts = _mp_config.max_read_retries + 1;

  bool saw_checksum_failure = false;

  for (uint32_t attempt = 0; attempt < max_attempts; ++attempt) {
    if (attempt > 0) {
      std::this_thread::yield();
    }

    // Track hit recording info to defer until after the probe section
    // (record_hit() can trigger a synchronous HitTracker flush →
    // update_hit_count_sync → exclusive stripe->mutex; deferring keeps that
    // heavyweight path out of the hot probe window).
    bool should_record_hit = false;
    AlternateId hit_alt_id{};

    // Lock-free read — readers take no stripe lock in any mode; see the
    // correctness note in read_sync().

    // NOTE: RAM cache check moved to after selector picks the best
    // alternate (keyed by CacheKey + AlternateId).

    // Collect all alternates for selection
    std::vector<AlternateInfo> alternates;
    std::expected<ReadHandle, CacheError> result =
        make_unexpected(CacheError::NotFound);
    bool found_head = false;
    bool checksum_failed = false;
    bool chain_incomplete = false;
    bool epoch_changed = false;  // wrap raced the read

    // Capture the wrap epoch at probe start for the reader's
    // stamp-then-revalidate protocol (disk borrows only).
    const auto epoch_start = wrap_epoch(stripe);
    // Resurrection guard (see Stripe::remove_epoch): captured before the
    // probe, re-checked in the RAM-cache put's predicate below and
    // once more after a put that inserted.
    const uint64_t remove_epoch_start =
        stripe->remove_epoch.load(std::memory_order_acquire);
    // RAM-coherence gate (a): the CROSS-PROCESS analogue of the process-local
    // epoch above, and sampled in the same place and for the same reason —
    // BEFORE the probe.  The bucket version only ever advances, so a stamp
    // taken pre-probe means "nothing in this bucket has moved since we started
    // looking"; once anything moves, the mismatch is PERMANENT and no window
    // exists in which the entry could be served stale.  Taking a fresh
    // reading at put time instead would reintroduce exactly that window.
    const bool ram_coherence = ram_coherence_active(stripe);
    const uint32_t bucket_version_start =
        ram_coherence ? stripe->bucket_version(key) : 0;

    stripe->probe_each(key, [&](const DirEntry& dir_entry) {
      if (found_head) {
        return false;
      }

      uint64_t doc_offset = stripe->offset + dir_entry.offset();

      auto mapped = _mapped_file->map_region(
          doc_offset, dir_entry.approx_size(), MappedFile::MapMode::ReadOnly);
      if (!mapped) {
        return true;
      }

      DocumentReader reader(*mapped);
      if (!reader.is_valid()) {
        _mapped_file->unmap_region(*mapped);
        return true;
      }

      // If the directory's approx_size underestimates the actual document
      // length, remap with the correct size.  This can happen because
      // DirEntry encodes sizes in a compact format that caps at ~256KB.
      if (reader.document().len > dir_entry.approx_size()) {
        if (reader.document().len > stripe->size) {
          _mapped_file->unmap_region(*mapped);
          return true;  // Skip corrupted entry
        }
        _mapped_file->unmap_region(*mapped);
        mapped = _mapped_file->map_region(doc_offset, reader.document().len,
                                          MappedFile::MapMode::ReadOnly);
        if (!mapped) {
          return true;
        }
        reader = DocumentReader(*mapped);
        if (!reader.is_valid()) {
          _mapped_file->unmap_region(*mapped);
          return true;
        }
      }

      CacheKey stored_key = reader.first_key();
      if (stored_key != key) {
        _mapped_file->unmap_region(*mapped);
        return true;
      }

      // Skip checksum during chain enumeration — only the header fields
      // are needed to build AlternateInfo.  The selected alternate's
      // checksum is verified after selector picks it.

      // Found the head - traverse chain to collect all alternates
      found_head = true;
      uint64_t current_offset = doc_offset;
      size_t chain_depth = 0;

      // Store offsets alongside AlternateInfo for later retrieval
      std::vector<uint64_t> alternate_offsets;

      while (current_offset != 0 &&
             chain_depth < Document::kMaxChainTraversalDepth) {
        constexpr size_t kInitialMapSize =
            Document::kHeaderSize + static_cast<size_t>(64 * 1024);
        auto doc = map_document(*_mapped_file, current_offset, kInitialMapSize,
                                stripe->size, true);
        if (!doc) {
          chain_incomplete = true;
          break;
        }

        // Cross-key escape guard: the walk holds no lock.  The in-place
        // chain repoint (remove_alternate_sync) now stores
        // next_alternate_offset atomically under the write lock and this hop
        // reads it atomically (v6), so the pointer is never read TORN -- but a
        // lock-free walk can still follow a STALE pointer to a node a peer has
        // since wrapped over, landing on an in-bounds document of a DIFFERENT
        // key.  Never accept a node that does not belong to this key — without
        // this check such a walk could enumerate (and later serve) another
        // key's content.
        if (doc->reader.first_key() != key) {
          _mapped_file->unmap_region(doc->region);
          chain_incomplete = true;
          break;
        }

        alternates.push_back(build_alternate_info(doc->reader, current_offset));
        alternate_offsets.push_back(current_offset);

        uint64_t next_offset = doc->reader.document().next_alternate_offset;
        _mapped_file->unmap_region(doc->region);

        if (next_offset == 0) break;
        if (!is_valid_chain_offset(stripe, next_offset)) break;
        ++chain_depth;
        current_offset = stripe->offset + next_offset;
      }

      // Graceful degradation: if we collected valid alternates before
      // encountering chain_incomplete, checksum failure, or cycle detection,
      // proceed to selector with what we have. Only fail if zero alternates.
      if (alternates.empty()) {
        if (chain_incomplete || checksum_failed) {
          checksum_failed = true;  // Trigger retry
        }
        _mapped_file->unmap_region(*mapped);
        return false;
      }

      // Use selector to choose the best alternate
      auto selected_idx = selector.select(alternates, ctx);
      if (!selected_idx.has_value()) {
        result = make_unexpected(CacheError::AlternateNotFound);
        _mapped_file->unmap_region(*mapped);
        return false;
      }

      // Bounds check on selected_idx (defensive programming)
      if (*selected_idx >= alternates.size() ||
          *selected_idx >= alternate_offsets.size()) {
        result = make_unexpected(CacheError::Corrupted);
        _mapped_file->unmap_region(*mapped);
        return false;
      }

      // Check RAM cache for the selected alternate.
      // When persistent mmap is active, RAM cache hits lose the file
      // offset (forcing writev instead of kernel sendfile).  Use a
      // size threshold: small entries benefit from RAM cache's fast
      // hashmap lookup; large entries benefit from sendfile zero-copy.
      // 32KB threshold: memcpy of <=32KB is ~2µs (negligible vs per-request
      // overhead), while memcpy of 1MB is ~100µs (dominates).
      static constexpr uint64_t kSendfileThreshold =
          static_cast<const uint64_t>(32 * 1024);
      AlternateId selected_alt_id = alternates[*selected_idx].id;
      uint64_t selected_content_len = alternates[*selected_idx].content_length;
      bool use_ram_cache =
          _ram_cache && (!_mapped_file->persistent_base() ||
                         selected_content_len <= kSendfileThreshold);
      if (use_ram_cache) {
        // RAM-coherence gate (c): same validation as read_sync's gate (d).  The
        // explicit remove is redundant on the success path below (the put_if
        // re-stamps the entry anyway), but the disk read can still fail into
        // a retry, and leaving a known-stale entry behind for that retry to
        // find is exactly the bug.
        uint32_t stamp = 0;
        auto ram_data = _ram_cache->get(key, selected_alt_id,
                                        ram_coherence ? &stamp : nullptr);
        if (ram_data && ram_coherence && stamp != stripe->bucket_version(key)) {
          _ram_cache->remove(key, selected_alt_id);
          _ram_coherence_rejections.fetch_add(1, std::memory_order_relaxed);
          ram_data.reset();
        }
        if (ram_data) {
          auto impl = std::make_shared<VolumeReadHandleImpl>();
          impl->ram_buffer = std::move(*ram_data);
          DocumentReader ram_reader(impl->ram_buffer);
          if (ram_reader.is_valid()) {
            bump_reads();
            should_record_hit = true;
            hit_alt_id = selected_alt_id;
            impl->is_ram_hit = true;
            impl->doc = ram_reader.document();
            impl->header_data = ram_reader.header();
            impl->content_data = ram_reader.content();
            result = ReadHandle(impl);
            _mapped_file->unmap_region(*mapped);
            return false;
          }
        }
      }

      // Read the selected alternate from disk
      uint64_t selected_offset = alternate_offsets[*selected_idx];

      // Check for integer overflow in map size calculation
      uint64_t content_len = alternates[*selected_idx].content_length;
      constexpr uint64_t kMapOverhead =
          Document::kHeaderSize + static_cast<size_t>(64 * 1024);
      if (content_len > std::numeric_limits<uint64_t>::max() - kMapOverhead) {
        result = make_unexpected(CacheError::Corrupted);
        _mapped_file->unmap_region(*mapped);
        return false;
      }
      uint64_t map_size = content_len + kMapOverhead;

      auto selected_mapped = _mapped_file->map_region(
          selected_offset, map_size, MappedFile::MapMode::ReadOnly);
      if (!selected_mapped) {
        result = make_unexpected(CacheError::IoError);
        _mapped_file->unmap_region(*mapped);
        return false;
      }

      DocumentReader selected_reader(*selected_mapped);
      if (!selected_reader.is_valid()) {
        _mapped_file->unmap_region(*selected_mapped);
        _mapped_file->unmap_region(*mapped);
        result = make_unexpected(CacheError::Corrupted);
        return false;
      }

      // Cross-key escape guard: the selected offset came from the
      // lock-free chain walk above, which a concurrent chain repoint may
      // have raced — never SERVE a document that does not belong to this
      // key.  Retry the whole attempt (the chain has changed under us).
      if (selected_reader.first_key() != key) {
        _mapped_file->unmap_region(*selected_mapped);
        _mapped_file->unmap_region(*mapped);
        checksum_failed = true;
        return false;
      }

      // Large-document readahead for the selected alternate — same
      // placement rule as read_sync(): the key has been re-verified, the
      // byte range is known, and the CRC pass below is the first content
      // touch.  See maybe_advise_readahead().
      maybe_advise_readahead(
          selected_offset,
          selected_mapped->first(std::min<size_t>(
              selected_mapped->size(), selected_reader.document().len)));

      // Verify checksum of selected alternate.
      // Skip if this {offset, checksum} pair was already verified.
      if (_config.verify_checksum_on_read &&
          selected_reader.document().checksum != 0 &&
          !is_checksum_validated(selected_offset,
                                 selected_reader.document().checksum)) {
        if (!selected_reader.document().verify_checksum(
                selected_reader.payload())) {
          _mapped_file->unmap_region(*selected_mapped);
          _mapped_file->unmap_region(*mapped);
          checksum_failed = true;
          return false;
        }
        mark_checksum_validated(selected_offset,
                                selected_reader.document().checksum);
      }

      // Lease protocol + register the borrow (count+1) and stamp the
      // read lease (seq_cst CAS-max now + T, with the write-avoidance
      // guard) BEFORE the borrow escapes, then revalidate: wrap-intent
      // flag clear AND wrap epoch (captured at probe start) unchanged
      // (Dekker closure — see allocate_write_slot).  A failure means a
      // wrap raced (or is racing) this read — release, discard and
      // retry/miss.  Runs before the RAM-cache put below so possibly-
      // overwritten bytes are never cached, and is deliberately
      // independent of the checksum-validation cache above.
      BorrowToken borrow = acquire_borrow(stripe);
      stamp_read_lease(stripe);
      // Read-side acquire fence (see renew_read_lease): the chain walk
      // above copied header bytes into owned AlternateInfo with plain
      // loads (checksum skipped during enumeration), and the checksum
      // pass read the selected payload.  Do NOT rely on the RMWs just
      // above to order them — the stamp can degrade to a pure load via
      // the write-avoidance skip guard, and with leases disabled both
      // degrade to nothing.  Pin all preceding reads ahead of the epoch
      // verdict below.
      std::atomic_thread_fence(std::memory_order_acquire);
      if (!borrow_still_valid(stripe, epoch_start)) {
        release_borrow(stripe, borrow);
        _mapped_file->unmap_region(*selected_mapped);
        _mapped_file->unmap_region(*mapped);
        epoch_changed = true;
        return false;  // Stop iteration; retry the whole attempt
      }

      // Cache the selected alternate in RAM for future reads — conditionally
      //.  The predicate re-checks the stripe's remove generation
      // while the RAM cache holds the write lock that also serializes the
      // invalidation's eviction, so the insert is atomic with respect to the
      // invalidation's bump+evict: a predicate evaluated after the eviction
      // critical section happens-after the bump (sequenced before that
      // section) and must observe it, so a put made against a superseded
      // generation is dropped at insert time and NEVER becomes visible to
      // another reader; one evaluated before it inserts an entry that
      // stays visible until the eviction itself removes it.  Either way no
      // superseded copy outlives the invalidation — closing the
      // put-then-undo window where the older bytes were briefly served
      // before the re-check below withdrew them.
      //
      // The post-put re-check below is still required, for two races no
      // insert-time predicate can exclude:
      //  - a wrap racing the copy (possible with leases disabled, or via a
      //    ceiling-forced wrap): a wrap takes NO RAM-cache lock, so it can
      //    tear the copied bytes after the predicate passed — RAM hits are
      //    served WITHOUT checksum verification, so a torn entry would be
      //    served as authoritative until evicted.  Only a revalidation
      //    AFTER the copy completes catches this;
      //  - a remove whose generation bump lands inside the put's own
      //    critical section, after the predicate passed — the invalidation's
      //    eviction would get the entry anyway; this undo just gets it
      //    first.
      // On either race, undo the put.  Only the RAM copy is discarded: the
      // handle's own borrow was revalidated above and remains lease-
      // protected (and removes never overwrite document bytes).
      if (use_ram_cache) {
        size_t doc_len = selected_reader.document().len;
        if (doc_len <= selected_mapped->size()) {
          const bool inserted = _ram_cache->put_if(
              key, selected_alt_id, selected_mapped->subspan(0, doc_len),
              [&] {
                if (stripe->remove_epoch.load(std::memory_order_acquire) !=
                    remove_epoch_start) {
                  return false;
                }
                if (!ram_coherence) {
                  return true;
                }
                // RAM-coherence gate (b): the same guard the predicate already
                // applies process-locally, extended cross-process and evaluated
                // under the same RAM-cache write lock.  An ODD sample means a
                // peer writer held the bucket when we sampled, so the stamp is
                // guaranteed to mismatch on the entry's very first hit -- skip
                // the doomed insert.  Both rejections are pure savings: the
                // entry could never have been served.
                if ((bucket_version_start & 1U) != 0U ||
                    stripe->bucket_version(key) != bucket_version_start) {
                  _ram_coherence_put_rejections.fetch_add(
                      1, std::memory_order_relaxed);
                  return false;
                }
                return true;
              },
              /*stamp=*/bucket_version_start);
          if (inserted) {
            // Read-side acquire fence (see renew_read_lease): the put above
            // memcpys the mmap bytes with plain loads, and NO seq_cst RMW
            // interposes between that copy and this verdict — the
            // acquire_borrow/stamp RMWs ran BEFORE the copy, and the RAM
            // cache's lock release is a one-way barrier a later load may
            // hoist above.  Pin the copy's loads ahead of the epoch and
            // remove-epoch loads: RAM hits are served WITHOUT checksum
            // verification, so this revalidation is the only guard against
            // caching (and then serving) torn bytes.
            std::atomic_thread_fence(std::memory_order_acquire);
            if (!borrow_still_valid(stripe, epoch_start) ||
                stripe->remove_epoch.load(std::memory_order_acquire) !=
                    remove_epoch_start) {
              _ram_cache->remove(key, selected_alt_id);
            }
          }
        }
      }

      bump_reads();
      should_record_hit = true;
      hit_alt_id = selected_alt_id;

      auto impl = std::make_shared<VolumeReadHandleImpl>();
      // same thread-shard anchor pin as read_sync.
      impl->anchor = read_anchor();
      if (!impl->anchor) {
        impl->mapped_file_weak = _mapped_file;
        impl->volume_weak = weak_from_this();  // pin for lease renew
      }
      impl->mapping = *selected_mapped;
      impl->doc = selected_reader.document();
      impl->header_data = selected_reader.header();
      impl->content_data = selected_reader.content();
      impl->stripe = stripe;
      impl->epoch_start = epoch_start;  // checked-renew snapshot
      impl->borrow = borrow;            // released on handle destruction

      result = ReadHandle(impl);
      _mapped_file->unmap_region(*mapped);
      return false;
    });

    if (_hit_tracker && should_record_hit) {
      _hit_tracker->record_hit(key, hit_alt_id);
    }

    if (found_head && !checksum_failed && !epoch_changed) {
      return result;
    }
    if (!checksum_failed && !epoch_changed) {
      return result;  // NotFound or other error, don't retry
    }
    if (checksum_failed) {
      saw_checksum_failure = true;
    }
    // Checksum failed or a wrap raced the read (wrap epoch
    // revalidation) — retry if we have attempts left
  }

  // All retries exhausted.  Checksum failures indicate corruption; pure
  // epoch churn means the entry kept being evicted mid-read — a miss.
  return make_unexpected(saw_checksum_failure ? CacheError::Corrupted
                                              : CacheError::NotFound);
}

std::expected<void, CacheError> Volume::commit_header_rmw(
    Stripe* stripe, std::byte* header, const CacheKey& expected_key,
    AlternateId expected_alt, std::pair<uint64_t, bool> epoch_start,
    bool blocking, const std::function<void()>& apply) {
  // Cross-process-safe in-place mutation of a LIVE published document's fixed
  // header.  The two RMW sites -- the hit-count bump and the middle/tail chain
  // repoint -- are the only writers that mutate a published document's fixed
  // header in place, and that header sits OUTSIDE the document checksum, so a
  // stray store into bytes a peer has since wrapped over is not CRC-detectable
  // and the checksum-validation cache turns it into a silent wrong-serve.
  //
  // One fenced attempt.  The caller samples `epoch_start` BEFORE it resolves
  // the target offset, maps `header` read-write BEFORE calling (so the
  // critical section is pure atomics -- never a map/unmap syscall under the
  // write lock), and unmaps AFTER.  We take the stripe write lock (blocking
  // for the control path, try-once for the hit path), then require:
  //   (a) revalidate -- no force-release took the lock over since we acquired;
  //   (b) the wrap epoch is unchanged since `epoch_start` -- a wrap is the
  //       ONLY way behind-cursor bytes are reused, and every wrap bumps the
  //       epoch under this same lock BEFORE its fill, so an unchanged epoch
  //       proves the target's bytes were not recycled;
  //   (c) the header still carries our key + alternate.  This is NOT
  //       redundant with (b): (b) fences a wrap of behind-cursor bytes, but a
  //       forward fill's dark-node overwrite, or a peer recycling the offset
  //       in the resolve->acquire window, is caught here (backed by the
  //       positional-guard positional guard that keeps a resolved offset behind
  //       the cursor).
  // Only then does `apply` run.  On any failure we release and report Busy;
  // the hit path drops the delta (best-effort by contract), the control path
  // re-resolves and retries.
  //
  // RESIDUAL (not closed here -- honest): a peer completing a last-resort
  // escalated write-lock takeover in the few instructions between our checks
  // and `apply` could wrap and let `apply` store into a freshly-recycled
  // foreign document; only a same-key+same-alternate collision at that offset
  // makes it undetectable (~2^-16 via the checksum-validation cache).  This is
  // the same bounded residual acquire_write_lock documents for the write path;
  // closing it fully means keying the checksum-validation cache by wrap epoch
  // (filed separately).  This fix reduces the header-RMW silent channel from
  // deterministic-on-any-wrap-racing-a-resolve down to that residual.
  auto identity_ok = [&]() {
    DocumentReader reader(
        std::span<const std::byte>(header, Document::kHeaderSize));
    return reader.is_valid() && reader.first_key() == expected_key &&
           static_cast<AlternateId>(reader.document().alternate_id) ==
               expected_alt;
  };

  // Fail closed if `header` is not 8-aligned: `apply` stores through
  // std::atomic_ref, which is UB (SIGBUS on ARM64) on an unaligned address.
  // Every legitimately-resolved document starts 8-aligned (v6 slot padding),
  // so this only trips on a corrupt/misaligned resolved offset -- report Busy
  // rather than crash.
  if (reinterpret_cast<std::uintptr_t>(header) % 8 != 0) {
    return make_unexpected(CacheError::Busy);
  }

  if (!(stripe->use_mmap_directory && stripe->mmap_directory)) {
    // In-memory directory => single process; the caller's stripe->mutex
    // already serializes every writer, so no cross-process lock exists or is
    // needed.  Honor the fence + identity for symmetry with the mmap path.
    if (wrap_epoch(stripe) != epoch_start || !identity_ok()) {
      return make_unexpected(CacheError::Busy);
    }
    apply();
    return {};
  }

  MmapDirectory& dir = *stripe->mmap_directory;
  auto token =
      blocking ? dir.acquire_write_lock() : dir.try_acquire_write_lock();
  if (!token.acquired) {
    // try-once mode only: a peer holds the lock mid-allocation.  Never wait.
    return make_unexpected(CacheError::Busy);
  }
  if (dir.revalidate_write_lock(token)) {
    repair_after_forced_release(stripe, token);  // see allocate_write_slot
  }
  const bool ok = dir.revalidate_write_lock(token) &&
                  wrap_epoch(stripe) == epoch_start && identity_ok();
  if (ok) {
    apply();  // pure std::atomic_ref stores -- no syscalls under the lock
  }
  dir.release_write_lock(token);  // release BEFORE the caller unmaps (M1)
  if (!ok) {
    return make_unexpected(CacheError::Busy);
  }
  return {};
}

std::expected<void, CacheError> Volume::repoint_chain_link(
    Stripe* stripe, uint64_t pred_absolute_offset,
    AlternateId pred_alternate_id, const CacheKey& key,
    uint64_t new_next_offset, std::pair<uint64_t, bool> epoch_start,
    bool blocking) {
  // The fixed header is outside the document checksum, so this store must run
  // under the write lock + the wrap-epoch fence (commit_header_rmw) or a
  // peer's wrap could turn it into a silent chain corruption / wrong-variant
  // serve.  Map read-write BEFORE the lock and unmap AFTER (no map/unmap
  // syscall under the lock).  The multi-store ordering invariant that binds
  // every caller is on the declaration in volume.hpp.
  auto region =
      _mapped_file->map_region(pred_absolute_offset, Document::kHeaderSize,
                               MappedFile::MapMode::ReadWrite);
  if (!region) {
    return make_unexpected(CacheError::IoError);
  }
  std::byte* header = region->data();
  auto apply = [&]() {
    auto& next_ref = *reinterpret_cast<uint64_t*>(
        header + Document::kNextAlternateOffsetPos);
    std::atomic_ref<uint64_t>(next_ref).store(new_next_offset,
                                              std::memory_order_release);
  };

  auto res = commit_header_rmw(stripe, header, key, pred_alternate_id,
                               epoch_start, blocking, apply);
  if (res.has_value() && _config.sync_on_write) {
    _mapped_file->sync(*region, MappedFile::SyncMode::Sync);
  }
  _mapped_file->unmap_region(*region);  // AFTER the lock is released (M1)
  return res;
}

std::expected<void, CacheError> Volume::remove_alternate_sync(
    const CacheKey& key, AlternateId alternate_id) {
  Stripe* stripe = select_stripe(key);
  if (stripe == nullptr) {
    return make_unexpected(CacheError::NotInitialized);
  }

  // Multi-process: reject writes to non-owned stripes
  if (!stripe->owned) {
    return make_unexpected(CacheError::NotOwned);
  }

  std::unique_lock lock(stripe->mutex);

  // A middle/tail removal repoints a LIVE predecessor's next_alternate_offset
  // in place, which is only safe under the write lock + wrap-epoch fence
  // (commit_header_rmw).  A peer's wrap between our resolve and our store
  // invalidates the resolved predecessor offset, so on a fenced-out attempt
  // we re-resolve and retry a bounded number of times, then report Busy --
  // never a silent drop, which would RESURRECT the removed alternate.
  constexpr int kMaxRepointRetries = 8;
  for (int attempt = 0; attempt < kMaxRepointRetries; ++attempt) {
    // Sample the wrap epoch BEFORE resolving (the fence needs it unchanged).
    const auto epoch_start = wrap_epoch(stripe);

    // Find the head document
    uint64_t head_relative_offset = 0;
    bool found_head = false;

    stripe->probe_each(key, [&](const DirEntry& dir_entry) {
      if (found_head) {
        return false;
      }

      uint64_t doc_offset = stripe->offset + dir_entry.offset();

      auto mapped = _mapped_file->map_region(doc_offset, Document::kHeaderSize,
                                             MappedFile::MapMode::ReadOnly);
      if (!mapped) {
        return true;
      }

      DocumentReader reader(*mapped);
      if (!reader.is_valid()) {
        _mapped_file->unmap_region(*mapped);
        return true;
      }

      CacheKey stored_key = reader.first_key();
      if (stored_key != key) {
        _mapped_file->unmap_region(*mapped);
        return true;
      }

      head_relative_offset = dir_entry.offset();
      found_head = true;
      _mapped_file->unmap_region(*mapped);
      return false;
    });

    if (!found_head) {
      return make_unexpected(CacheError::NotFound);
    }

    // Traverse chain to find target alternate and its predecessor
    uint64_t prev_absolute_offset = 0;
    AlternateId prev_alternate_id = static_cast<AlternateId>(0);
    uint64_t target_absolute_offset = 0;
    uint64_t target_next_offset = 0;
    size_t chain_depth = 0;
    bool target_is_head = false;

    uint64_t current_offset = stripe->offset + head_relative_offset;

    while (current_offset != 0 &&
           chain_depth < Document::kMaxChainTraversalDepth) {
      auto mdoc = map_document(*_mapped_file, current_offset,
                               Document::kHeaderSize, stripe->size, false);
      if (!mdoc) break;

      const Document& doc = mdoc->reader.document();
      auto current_id = static_cast<AlternateId>(doc.alternate_id);

      if (current_id == alternate_id) {
        target_absolute_offset = current_offset;
        target_next_offset = doc.next_alternate_offset;
        target_is_head = (chain_depth == 0);
        _mapped_file->unmap_region(mdoc->region);
        break;
      }

      prev_absolute_offset = current_offset;
      prev_alternate_id = current_id;
      uint64_t next_offset = doc.next_alternate_offset;
      _mapped_file->unmap_region(mdoc->region);

      if (next_offset == 0) {
        break;
      }

      // Validate offset before following (security)
      if (!is_valid_chain_offset(stripe, next_offset)) {
        return make_unexpected(CacheError::ChainCorrupted);
      }

      current_offset = stripe->offset + next_offset;
      ++chain_depth;
    }

    if (target_absolute_offset == 0) {
      return make_unexpected(CacheError::AlternateNotFound);
    }

    if (target_is_head) {
      // Removing the head - update directory (protected by the directory's own
      // writer/phase locks; no in-place document mutation, so no fence).
      if (target_next_offset == 0) {
        // Only alternate - remove from directory entirely.
        // Use remove_entry_at for precise offset-based removal
        // (collision-safe).
        stripe->remove_entry_at(key, head_relative_offset);
      } else {
        // Validate target_next_offset before following (security)
        if (!is_valid_chain_offset(stripe, target_next_offset)) {
          return make_unexpected(CacheError::ChainCorrupted);
        }

        // Update directory to point to next alternate
        // Calculate approximate size of the next document
        auto next_mapped = _mapped_file->map_region(
            stripe->offset + target_next_offset, Document::kHeaderSize,
            MappedFile::MapMode::ReadOnly);
        if (!next_mapped) {
          return make_unexpected(CacheError::IoError);
        }
        DocumentReader next_reader(*next_mapped);
        if (!next_reader.is_valid()) {
          _mapped_file->unmap_region(*next_mapped);
          return make_unexpected(CacheError::Corrupted);
        }
        size_t next_doc_size = next_reader.document().len;
        _mapped_file->unmap_region(*next_mapped);

        // head_relative_offset was verified by full first_key comparison
        // above — repoint exactly that entry, never a same-tag collider.
        stripe->insert(key, target_next_offset, next_doc_size,
                       head_relative_offset);
      }

      // Head was removed or changed - invalidate RAM cache.
      // Remove the specific alternate that was removed, and do it in the
      // load-bearing post-removal order: bump the stripe's remove generation
      // THEN evict, BOTH after the directory mutation above (entry removal or
      // head repoint).  Bumping at the top of the operation instead — where
      // this function's bump used to sit — left a permanent resurrection
      // hole: a lock-free reader sampling the generation between that
      // bump and the mutation still walked the LIVE chain, and neither our
      // eviction (already run) nor its own re-check (no second bump) ever
      // withdrew its put.  For a removed Original head that is not merely
      // unreachable: read_sync serves a RAM hit for (key, Original) without
      // consulting the directory, so the removed head kept being served
      // indefinitely.  See remove_sync for the full two-sided argument.
      if (_ram_cache) {
        stripe->remove_epoch.fetch_add(1, std::memory_order_acq_rel);
        _ram_cache->remove(key, alternate_id);
      }
      return {};
    }

    // Removing from middle or tail: repoint the predecessor's
    // next_alternate_offset in place (shared helper; see repoint_chain_link).
    auto res =
        repoint_chain_link(stripe, prev_absolute_offset, prev_alternate_id, key,
                           target_next_offset, epoch_start, /*blocking=*/true);

    if (res.has_value()) {
      if (_config.sync_on_write) {
        fsync_fd();
      }
      // RAM coherence: publish the unlink to PEER PROCESSES.  Every other
      // invalidating path mutates a DirEntry and so advances the bucket's
      // seqlock version for free; this one rewrites the PREDECESSOR DOCUMENT's
      // next_alternate_offset in place under the global directory write lock,
      // leaving the head entry -- and the bucket version -- untouched.  A peer
      // validating its RAM tier against that version would keep serving the
      // alternate we just unlinked.  touch_bucket() closes that with an empty
      // seqlock writer bracket (+2, fenced, parity preserved).
      //
      // UNCONDITIONAL, on purpose, and OUTSIDE the RAM-cache block below.
      // Gating the PUBLISH on this process's own coherence setting would make
      // a coherent reader depend on every peer WRITER also opting in -- the
      // cross-process configuration coupling this design exists to avoid.
      // Gating it on _ram_cache would be worse still: the "one writer, many
      // readers" deployment this protects is precisely the one whose writer
      // runs with no RAM tier of its own, so the bump would never fire where
      // it matters most.  Ordering is already correct: repoint_chain_link has
      // released the directory write lock, and its release store is ordered
      // before acquire_writer's seq_cst fence.  Cost lands on a cold path.
      if (stripe->use_mmap_directory && stripe->mmap_directory) {
        stripe->mmap_directory->touch_bucket(key);
      }
      // Same post-mutation invalidation as the head-removal path above, in
      // the same order: the repoint just unlinked the target from the
      // live chain, so bump the remove generation THEN drop its RAM entry,
      // both after the mutation.  Without this a reader racing the repoint
      // could leave the removed alternate's copy in RAM with nothing ever
      // withdrawing it.
      if (_ram_cache) {
        stripe->remove_epoch.fetch_add(1, std::memory_order_acq_rel);
        _ram_cache->remove(key, alternate_id);
      }
      return {};
    }
    // Busy: a wrap raced our resolve (or a rare lock takeover).  Re-resolve
    // and retry; a bounded exhaustion returns Busy rather than dropping.
  }

  return make_unexpected(CacheError::Busy);
}

std::expected<void, CacheError> Volume::update_hit_count_sync(
    const CacheKey& key, AlternateId alternate_id, uint32_t hit_delta,
    int64_t last_access_ms) {
  Stripe* stripe = select_stripe(key);
  if (stripe == nullptr) {
    return make_unexpected(CacheError::NotInitialized);
  }

  // Multi-process: reject writes to non-owned stripes
  if (!stripe->owned) {
    return make_unexpected(CacheError::NotOwned);
  }

  std::unique_lock<std::shared_mutex> lock(stripe->mutex);

  // Sample the wrap epoch BEFORE resolving the target offset -- the fence in
  // commit_header_rmw requires it unchanged at store time (see there).
  const auto epoch_start = wrap_epoch(stripe);

  // Find the document with the matching alternate_id
  uint64_t target_offset = 0;

  stripe->probe_each(key, [&](const DirEntry& dir_entry) {
    uint64_t doc_offset = stripe->offset + dir_entry.offset();

    // Validate initial offset from directory entry
    if (!is_valid_chain_offset(stripe, dir_entry.offset())) {
      return true;  // Continue to next candidate
    }

    auto mapped = _mapped_file->map_region(doc_offset, dir_entry.approx_size(),
                                           MappedFile::MapMode::ReadOnly);
    if (!mapped) {
      return true;  // Continue to next candidate
    }

    DocumentReader reader(*mapped);
    if (!reader.is_valid()) {
      _mapped_file->unmap_region(*mapped);
      return true;  // Continue to next candidate
    }

    CacheKey stored_key = reader.first_key();
    if (stored_key != key) {
      _mapped_file->unmap_region(*mapped);
      return true;  // Continue to next candidate
    }

    _mapped_file->unmap_region(*mapped);

    // Found head - traverse chain to find the right alternate
    uint64_t current_offset = doc_offset;
    size_t chain_depth = 0;

    while (current_offset != 0 &&
           chain_depth < Document::kMaxChainTraversalDepth) {
      auto mdoc = map_document(*_mapped_file, current_offset,
                               Document::kHeaderSize, stripe->size, false);
      if (!mdoc) break;

      const Document& doc = mdoc->reader.document();
      auto current_id = static_cast<AlternateId>(doc.alternate_id);

      if (current_id == alternate_id) {
        target_offset = current_offset;
        _mapped_file->unmap_region(mdoc->region);
        break;
      }

      uint64_t next_offset = doc.next_alternate_offset;
      _mapped_file->unmap_region(mdoc->region);

      if (next_offset == 0) break;
      if (!is_valid_chain_offset(stripe, next_offset)) break;
      current_offset = stripe->offset + next_offset;
      ++chain_depth;
    }

    return false;  // Stop iteration
  });

  if (target_offset == 0) {
    return make_unexpected(CacheError::AlternateNotFound);
  }

  // Map the target header read-write BEFORE taking the write lock, and store
  // hit_count + last_access via std::atomic_ref under it (v6 lays both out
  // naturally aligned; the store races only a reader's atomic load in
  // Document::deserialize, never a torn read).  Hit counts are best-effort by
  // contract, so this is the try-once path: on contention or a raced wrap the
  // delta is DROPPED (reported as Busy) rather than blocking the flush thread
  // on the cross-process write lock behind a peer's pwrite, and never risking
  // a stray write into wrapped bytes.  (A concurrent remove_alternate_sync on
  // the same stripe can still park the flush thread on stripe->mutex, but
  // removes are rare.)
  auto region = _mapped_file->map_region(target_offset, Document::kHeaderSize,
                                         MappedFile::MapMode::ReadWrite);
  if (!region) {
    return make_unexpected(CacheError::IoError);
  }
  std::byte* header = region->data();

  auto apply = [&]() {
    auto& hit_ref =
        *reinterpret_cast<uint32_t*>(header + Document::kHitCountOffset);
    const uint32_t current =
        std::atomic_ref<uint32_t>(hit_ref).load(std::memory_order_relaxed);
    uint32_t updated = current;
    if (hit_delta <= std::numeric_limits<uint32_t>::max() - current) {
      updated = current + hit_delta;
    } else {
      updated = std::numeric_limits<uint32_t>::max();
    }
    std::atomic_ref<uint32_t>(hit_ref).store(updated,
                                             std::memory_order_release);
    auto& access_ref =
        *reinterpret_cast<int64_t*>(header + Document::kLastAccessOffset);
    std::atomic_ref<int64_t>(access_ref)
        .store(last_access_ms, std::memory_order_release);
  };

  auto result = commit_header_rmw(stripe, header, key, alternate_id,
                                  epoch_start, /*blocking=*/false, apply);
  _mapped_file->unmap_region(*region);  // AFTER the lock is released (M1)

  // Note: no fsync here (write amplification).  Hit counts are best-effort and
  // reach peers via the once-per-flush-cycle fsync_volume(); a dropped delta
  // (Busy) is surfaced to the caller for the contention counter.
  return result;
}

int Volume::fsync_fd() const {
  _fsyncs.fetch_add(1, std::memory_order_relaxed);
  return CYCLONE_FSYNC(_fd);
}

std::expected<void, CacheError> Volume::fsync_volume() const {
  if (_fd < 0) {
    return make_unexpected(CacheError::NotInitialized);
  }
  if (fsync_fd() != 0) {
    return make_unexpected(CacheError::IoError);
  }
  return {};
}

// Durability contract for the persistent (mmap'd) directory
// ----------------------------------------------------------
// A volume file holds, per stripe, [mmap'd directory][pad][data region], and
// is reached through two handles onto the same file: _fd (pwrite/fsync of
// document data) and _mapped_file (MAP_SHARED mapping through which DirEntry
// stores land).
//
// Ordering invariant: document data is made durable BEFORE the directory
// entries that point at it.  In multi-process mode this is the job of THIS
// periodic sync — it fsync's the data fd first, then msync's the directory
// regions (see the ordered body below) — because per-write fsync is
// deliberately disabled to avoid an fsync convoy on the shared inode
//.  A caller may still opt into per-write fsync via
// VolumeConfig::sync_on_write, which makes each commit's data durable at
// commit time.  Otherwise neither the data nor its entry is synchronously
// durable on return from a write; both reach disk via this periodic sync (or
// OS writeback).
//
// Worst case after power loss, bounded by one sync interval:
//   * A fresh append published since the last sync may be MISSING, or its
//     entry may momentarily point at a not-yet-durable / torn offset — the
//     read gauntlet (magic → bounds → key → CRC32, verify_checksum_on_read
//     forced on in persistent mode) degrades that to a cache MISS.  The
//     fsync-before-msync order below guarantees the entry cannot outrace its
//     own data on the periodic path, so this stays miss-only rather than a
//     dangling entry.
//   * An in-place OVERWRITE of an existing key (commit_write / _alternate
//     reuse the prior offset) whose new data was not yet synced degrades to a
//     STALE HIT: the offset still holds the previous, fully-durable value,
//     which passes magic+key+CRC, so the pre-overwrite value is served.  This
//     is acknowledged-write loss bounded by one sync interval, NOT a miss.
//     Acceptable here because the cache is non-authoritative and
//     reconstructible (a stale metadata decision is revalidated / re-derived
//     upstream).  A caller needing overwrite durability must set
//     sync_on_write.
//
// Platform notes:
// - POSIX: the data fd is fsync'd FIRST, then each directory region gets an
//   explicit msync(MS_SYNC) (data-before-directory; see the ordering
//   invariant above).  On Linux the fsync alone would already cover the
//   mmap'd pages (unified dirty tracking of the inode; msync(MS_ASYNC) is
//   documented as a no-op) — but that is Linux behavior, NOT a POSIX
//   guarantee, so the msync makes the directory guarantee explicit on every
//   POSIX system instead of re-inferring it for macOS/BSD.
// - macOS: fsync()/msync() do not flush the drive's volatile write cache
//   (F_FULLFSYNC would).  We accept the weaker guarantee: the bounded-loss
//   window widens by whatever the drive cache holds.
// - Windows: FlushFileBuffers on the file handle alone does NOT flush dirty
//   mapped-view pages; each directory view needs FlushViewOfFile followed by
//   FlushFileBuffers (MappedFile::sync(SyncMode::Sync) does both).
std::expected<void, CacheError> Volume::sync_directory() const {
  if (_fd < 0) {
    return make_unexpected(CacheError::NotInitialized);
  }

  bool failed = false;

#ifndef _WIN32
  // Data-before-directory: fsync the data fd FIRST so pwrite'd document data
  // (and, on Linux, any mmap-dirtied inode pages) is durable before we publish
  // the directory entries that point at it below.  With per-write fsync gone
  // this ordering is what keeps a crash-surviving append entry
  // pointing at durable data, so a torn/unsynced append degrades to a miss
  // rather than a dangling entry.  (Windows flushes data as part of each
  // region's FlushFileBuffers, so no separate step is needed there.)
  if (!fsync_volume().has_value()) {
    failed = true;
  }
#endif

  // Best-effort across stripes: attempt every region each cycle, remember
  // the first failure, report it at the end.  The next interval retries.
  for (const auto& stripe : _stripes) {
    if (!stripe->use_mmap_directory || !stripe->mmap_directory) {
      continue;
    }
    auto region = stripe->mmap_directory->region();
    if (region.empty()) {
      continue;
    }
    // POSIX: msync(MS_SYNC); Windows: FlushViewOfFile + FlushFileBuffers.
    if (!_mapped_file ||
        _mapped_file->sync(region, MappedFile::SyncMode::Sync)) {
      failed = true;
    }
  }

  if (failed) {
    return make_unexpected(CacheError::IoError);
  }
  _directory_syncs.fetch_add(1, std::memory_order_relaxed);
  return {};
}

}  // namespace cyclone
