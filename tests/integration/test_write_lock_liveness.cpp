// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Write-lock holder liveness without trusting PIDs (issue #32).
//
// A write-lock waiter used to take the lock over from a holder whose
// published PID kill(pid, 0) reported gone.  Across PID namespaces (two
// containers sharing a volume) that PID means nothing to the waiter: a live
// holder reads as dead, or its PID aliases an unrelated live process.  Each
// process now holds a liveness slot (WriterLiveness): a byte-range lock on
// the volume file that the kernel drops when, and only when, it dies.  A
// holder encodes its slot in its lock token.
//
// A real second PID namespace needs privileges CI does not have, so the
// portable cases make a forked holder PUBLISH a PID that reads dead, or one
// that aliases a live process, through the current_pid() test seam.  That
// is exactly what the waiter sees of a holder in another namespace.  On
// Linux, a last case runs the real thing with unshare(CLONE_NEWPID) where
// the kernel allows it, and is skipped otherwise.
//
// The fork cases are POSIX only.  On Windows the first cases below and the
// Volume-level crash cases (test_wrap_retention.cpp, "Retention 13", whose
// writer peer dies holding the write lock) cover the LockFileEx liveness.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <string>

#include "core/mmap_directory.hpp"
#include "core/volume.hpp"
#include "cyclone/config.hpp"
#include "support/temp_cache.hpp"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

using cyclone::MmapDirectory;
using cyclone::WriterLiveness;

TEST_CASE(
    "Write-lock liveness: a slot token decodes to its slot and never to "
    "the plain token of its generation, 0, 1 or a recovery claim",
    "[writelock][liveness]") {
  for (uint32_t g = 0; g <= 0xFFFF; ++g) {
    const auto gen = static_cast<uint16_t>(g);
    const uint8_t plain = MmapDirectory::write_lock_token_for_test(gen, -1);
    for (int slot = 0; slot < static_cast<int>(WriterLiveness::kSlots);
         ++slot) {
      const uint8_t v = MmapDirectory::write_lock_token_for_test(gen, slot);
      if (v < 2 || v > 253 || v == plain ||
          MmapDirectory::write_lock_slot_for_test(v, gen) != slot) {
        FAIL("generation " << g << " slot " << slot << " token " << int{v});
      }
    }
    for (const int v : {0, 1, 254, 255, int{plain}}) {
      if (MmapDirectory::write_lock_slot_for_test(
              static_cast<uint8_t>(v), gen) != WriterLiveness::kNoSlot) {
        FAIL("generation " << g << " value " << v << " decodes to a slot");
      }
    }
  }
}

namespace {

// Open (creating it if needed) a file read-write.
int open_rw(const std::string &path) {
#ifdef _WIN32
  return ::_open(path.c_str(), _O_RDWR | _O_CREAT | _O_BINARY,
                 _S_IREAD | _S_IWRITE);
#else
  return ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
#endif
}

void close_fd(int fd) {
#ifdef _WIN32
  ::_close(fd);
#else
  ::close(fd);
#endif
}

// Slots of `observer`'s file some process holds.
int live_slots(const WriterLiveness &observer) {
  int live = 0;
  for (unsigned slot = 0; slot < WriterLiveness::kSlots; ++slot) {
    live += observer.probe(slot) == WriterLiveness::Verdict::kAlive ? 1 : 0;
  }
  return live;
}

}  // namespace

TEST_CASE(
    "Write-lock liveness: a multi-process Volume holds a slot on the volume "
    "file while open, and releases it on close",
    "[writelock][multiprocess][liveness]") {
  TempCacheDir tmp("wll_vol");
  const std::string path = tmp.path();
  cyclone::VolumeConfig vc;
  vc.path = path;
  vc.size = size_t{16} << 20;
  cyclone::MultiProcessConfig mp;
  mp.set_enabled(true).set_process_index(0).set_total_processes(1);
  auto volume = std::make_unique<cyclone::Volume>(vc, mp);
  REQUIRE(volume->open().has_value());
  CHECK(volume->stats().write_lock_liveness_unregistered == 0);

  // An observer on the same file, as a peer process sees it.  It claims no
  // slot itself, so only the Volume's lock can answer.
  const int fd = open_rw(path);
  REQUIRE(fd >= 0);
  WriterLiveness::s_claims_fail_for_test.store(true);
  WriterLiveness observer;
  observer.attach(fd, path);
  WriterLiveness::s_claims_fail_for_test.store(false);
  CHECK(observer.slot_of() == WriterLiveness::kNoSlot);
  CHECK(live_slots(observer) == 1);

  // A second Volume of the same file (another process, as far as the locks
  // go) claims another slot.
  auto second = std::make_unique<cyclone::Volume>(vc, mp);
  REQUIRE(second->open().has_value());
  CHECK(live_slots(observer) == 2);
  second->close();
  CHECK(live_slots(observer) == 1);

  volume->close();
  CHECK(live_slots(observer) == 0);
  observer.detach();
  close_fd(fd);
}

TEST_CASE(
    "Write-lock liveness: slots are exclusive and bounded; with none left a "
    "process holds none",
    "[writelock][liveness]") {
  TempCacheDir tmp("wll_slots");
  const std::string path = tmp.path("slots.dat");
  const int fd = open_rw(path);
  REQUIRE(fd >= 0);
  WriterLiveness::s_slots_for_test.store(2);
  {
    WriterLiveness a;
    WriterLiveness b;
    WriterLiveness c;
    a.attach(fd, path);
    b.attach(fd, path);
    c.attach(fd, path);
    CHECK(a.slot_of() == 0);
    CHECK(b.slot_of() == 1);
    CHECK(c.slot_of() == WriterLiveness::kNoSlot);  // exhausted
    a.detach();                                     // frees slot 0
    c.attach(fd, path);
    CHECK(c.slot_of() == 0);  // reused
  }
  WriterLiveness::s_slots_for_test.store(0);
  close_fd(fd);
}

#ifndef _WIN32

#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <span>
#include <thread>
#include <utility>

#include "support/liveness_file.hpp"

#if defined(__linux__)
#include <sched.h>
#endif

using Clock = std::chrono::steady_clock;
using Ms = std::chrono::milliseconds;

namespace {

constexpr size_t kBuckets = 64;
constexpr uint64_t kBase = 0x4000;

// A PID no process has on any supported platform (Linux caps pid_max at
// 2^22, macOS and the BSDs at 99999): kill(pid, 0) reports ESRCH for it, as
// it does for a live holder in another PID namespace.
constexpr uint32_t kPidReadsDead = 0x3FFFFF0;

// How long a waiter must leave a live holder alone: far past the 50 ms
// probe threshold, far below the 5 s escalation.
constexpr Ms kLiveWindow{400};
// How soon a proven-dead holder must be recovered: the probe runs after
// every sleep (at most 1 ms) once the holder held 50 ms.
constexpr Ms kPromptRecovery{1000};

bool kill_reports_dead(uint32_t pid) {
  return ::kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH;
}

// Limit the claimable slots for one test.
class SlotLimit {
 public:
  explicit SlotLimit(unsigned n) { WriterLiveness::s_slots_for_test.store(n); }
  ~SlotLimit() { WriterLiveness::s_slots_for_test.store(0); }
  SlotLimit(const SlotLimit &) = delete;
  SlotLimit &operator=(const SlotLimit &) = delete;
};

// A directory in shared anonymous memory (parent and forked children share
// its lock words) whose processes claim liveness slots on a file.
struct SharedDir {
  SharedDir() {
    size = MmapDirectory::required_size(kBuckets);
    base = ::mmap(nullptr, size, PROT_READ | PROT_WRITE,
                  MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    REQUIRE(base != MAP_FAILED);
    auto opt = MmapDirectory::init(
        std::span<std::byte>(static_cast<std::byte *>(base), size), kBuckets);
    REQUIRE(opt.has_value());
    dir = std::move(*opt);
    dir.set_shared_write_pos(kBase);
    liveness = std::make_unique<LivenessFile>(dir);
    REQUIRE(liveness->ok());
    MmapDirectory::s_write_lock_presume_dead_for_test.store(false);
    MmapDirectory::s_write_lock_max_live_waits_for_test.store(0);
  }
  ~SharedDir() {
    liveness.reset();
    ::munmap(base, size);
  }
  SharedDir(const SharedDir &) = delete;
  SharedDir &operator=(const SharedDir &) = delete;

  [[nodiscard]] WriterLiveness &live() { return liveness->liveness(); }

  void *base = nullptr;
  size_t size = 0;
  MmapDirectory dir;
  std::unique_ptr<LivenessFile> liveness;
};

enum class Then : uint8_t { kStayAlive, kDie };

struct Report {
  int32_t slot = WriterLiveness::kNoSlot;
  uint32_t published = 0;
};

struct HolderOptions {
  uint32_t pid = 0;  // the PID to publish; 0: its own
  bool fail_claim = false;
  bool detach_liveness = false;  // a build from before #32
};

// Fork a holder that takes the write lock, reports {slot, published pid} on
// a pipe and then either blocks (alive, holding the lock) or _exits holding
// it.
pid_t fork_holder(SharedDir &d, Then then, Report &report,
                  HolderOptions options = {}) {
  int pfd[2];
  REQUIRE(::pipe(pfd) == 0);
  const pid_t child = ::fork();
  REQUIRE(child >= 0);
  if (child == 0) {
    ::close(pfd[0]);
    if (options.pid != 0) {
      WriterLiveness::s_pid_override_for_test.store(options.pid);
    }
    WriterLiveness::s_claims_fail_for_test.store(options.fail_claim);
    if (options.detach_liveness) {
      d.dir.set_liveness(nullptr);
    }
    const auto token = d.dir.acquire_write_lock(/*capped=*/false);
    Report r;
    r.slot = token.liveness_slot;
    r.published = WriterLiveness::current_pid();
    (void)!::write(pfd[1], &r, sizeof(r));
    if (!token.acquired) {
      _exit(2);
    }
    if (then == Then::kDie) {
      _exit(137);  // dies HOLDING the write lock
    }
    for (;;) {
      ::pause();  // alive, holding the lock, until SIGKILLed
    }
  }
  ::close(pfd[1]);
  REQUIRE(::read(pfd[0], &report, sizeof(report)) ==
          static_cast<ssize_t>(sizeof(report)));
  ::close(pfd[0]);
  return child;
}

void kill_and_reap(pid_t pid) {
  ::kill(pid, SIGKILL);
  int status = 0;
  ::waitpid(pid, &status, 0);
}

void reap(pid_t pid) {
  int status = 0;
  REQUIRE(::waitpid(pid, &status, 0) == pid);
}

// A waiter thread on the write lock (uncapped, as a wrap would wait).
struct Waiter {
  explicit Waiter(MmapDirectory &dir) {
    thread = std::thread([this, &dir] {
      token = dir.acquire_write_lock(/*capped=*/false);
      done_at = Clock::now();
      done.store(true, std::memory_order_release);
    });
  }
  ~Waiter() {
    if (thread.joinable()) {
      thread.join();
    }
  }
  Waiter(const Waiter &) = delete;
  Waiter &operator=(const Waiter &) = delete;

  std::thread thread;
  std::atomic<bool> done{false};
  Clock::time_point done_at{};
  MmapDirectory::WriteLockToken token;
};

// Wait out a live holder for kLiveWindow (it must not be taken over), then
// kill it: the waiter must then recover the lock promptly, as a proven-dead
// holder.
void expect_left_alone_then_recovered(SharedDir &d, pid_t holder) {
  Waiter waiter(d.dir);
  std::this_thread::sleep_for(kLiveWindow);
  CHECK_FALSE(waiter.done.load(std::memory_order_acquire));
  CHECK(d.dir.get_shared_write_pos() == kBase);  // no second reservation

  const auto killed_at = Clock::now();
  kill_and_reap(holder);
  waiter.thread.join();
  REQUIRE(waiter.token.acquired);
  CHECK(waiter.token.forced_release);  // proven dead: routine recovery
  CHECK_FALSE(waiter.token.escalated_takeover);
  CHECK(waiter.done_at - killed_at < kPromptRecovery);
  d.dir.release_write_lock(waiter.token);
}

}  // namespace

TEST_CASE(
    "Write-lock liveness: a live holder whose PID reads dead is not taken "
    "over, and is recovered promptly once it really dies",
    "[writelock][multiprocess][liveness]") {
  SharedDir d;
  REQUIRE(kill_reports_dead(kPidReadsDead));  // kill(pid, 0) would usurp it

  Report report;
  const pid_t holder =
      fork_holder(d, Then::kStayAlive, report, {.pid = kPidReadsDead});
  CHECK(report.slot >= 0);
  CHECK(report.published == kPidReadsDead);
  expect_left_alone_then_recovered(d, holder);
}

TEST_CASE(
    "Write-lock liveness: a dead holder whose PID aliases a live process is "
    "recovered promptly, not after the escalation",
    "[writelock][multiprocess][liveness]") {
  SharedDir d;
  // A live process the dead holder's PID will name (as a PID from another
  // namespace can name any local process).
  const pid_t alias = ::fork();
  REQUIRE(alias >= 0);
  if (alias == 0) {
    for (;;) {
      ::pause();
    }
  }
  REQUIRE_FALSE(kill_reports_dead(static_cast<uint32_t>(alias)));

  Report report;
  const pid_t holder =
      fork_holder(d, Then::kDie, report, {.pid = static_cast<uint32_t>(alias)});
  CHECK(report.slot >= 0);
  reap(holder);

  const auto t0 = Clock::now();
  const auto token = d.dir.acquire_write_lock(/*capped=*/false);
  const auto waited = Clock::now() - t0;
  REQUIRE(token.acquired);
  CHECK(token.forced_release);
  CHECK_FALSE(token.escalated_takeover);
  CHECK(waited < kPromptRecovery);
  d.dir.release_write_lock(token);
  kill_and_reap(alias);
}

TEST_CASE(
    "Write-lock liveness: a dead holder's slot is reusable, and a new live "
    "holder in it is honoured",
    "[writelock][multiprocess][liveness]") {
  // Two slots: the parent takes slot 0, every child slot 1.
  SlotLimit limit(2);
  SharedDir d;
  REQUIRE(d.live().slot_of() == 0);

  Report first;
  const pid_t dead = fork_holder(d, Then::kDie, first);
  CHECK(first.slot == 1);
  reap(dead);
  CHECK(d.live().probe(1) == WriterLiveness::Verdict::kDead);
  auto token = d.dir.acquire_write_lock(/*capped=*/false);
  REQUIRE(token.acquired);
  CHECK(token.forced_release);
  d.dir.release_write_lock(token);

  Report second;
  const pid_t live = fork_holder(d, Then::kStayAlive, second);
  CHECK(second.slot == 1);  // the same slot, re-claimed
  CHECK(d.live().probe(1) == WriterLiveness::Verdict::kAlive);
  expect_left_alone_then_recovered(d, live);
  CHECK(d.live().probe(1) == WriterLiveness::Verdict::kDead);
}

TEST_CASE(
    "Write-lock liveness: a holder without a slot is never proven dead; "
    "only the time-based escalation recovers it",
    "[writelock][multiprocess][liveness]") {
  // No slot because the claim failed (no byte-range locks, the file replaced
  // by name), because every slot is taken, or because the holder predates
  // #32.  Its token encodes no slot, so the missing lock proves nothing: it
  // is recovered only by the escalation, never by its PID -- even when that
  // PID reads dead, as a peer's in another namespace would.
  constexpr auto kEscalation = Ms{300};
  enum class Cause : uint8_t { kClaimFails, kSlotsExhausted, kOlderBuild };
  for (const Cause cause :
       {Cause::kClaimFails, Cause::kSlotsExhausted, Cause::kOlderBuild}) {
    CAPTURE(static_cast<int>(cause));
    // With one slot, the parent's claim takes it and the child finds none.
    SlotLimit limit(cause == Cause::kSlotsExhausted ? 1 : 0);
    SharedDir d;
    Report report;
    const pid_t holder =
        fork_holder(d, Then::kDie, report,
                    {.pid = kPidReadsDead - 1,
                     .fail_claim = cause == Cause::kClaimFails,
                     .detach_liveness = cause == Cause::kOlderBuild});
    CHECK(report.slot == WriterLiveness::kNoSlot);
    reap(holder);
    REQUIRE(kill_reports_dead(report.published));

    MmapDirectory::s_write_lock_escalation_us_for_test.store(
        std::chrono::duration_cast<std::chrono::microseconds>(kEscalation)
            .count());
    const auto t0 = Clock::now();
    const auto token = d.dir.acquire_write_lock(/*capped=*/false);
    const auto waited = Clock::now() - t0;
    MmapDirectory::s_write_lock_escalation_us_for_test.store(0);
    REQUIRE(token.acquired);
    CHECK_FALSE(token.forced_release);
    CHECK(token.escalated_takeover);
    CHECK(waited >= kEscalation);
    d.dir.release_write_lock(token);
  }
}

TEST_CASE(
    "Write-lock liveness: a forked child claims its own slot, whose death is "
    "visible while the parent lives",
    "[writelock][multiprocess][liveness]") {
  SharedDir d;
  const int parent_slot = d.live().slot_of();
  REQUIRE(parent_slot >= 0);

  // A real fork, no PID seam: the child claims a slot on its own
  // descriptor, so its lock dies with it although the parent keeps the
  // descriptors the child inherited open.
  Report report;
  const pid_t child = fork_holder(d, Then::kDie, report);
  CHECK(report.published == static_cast<uint32_t>(child));
  REQUIRE(report.slot >= 0);
  CHECK(report.slot != parent_slot);
  reap(child);
  CHECK(d.live().probe(static_cast<unsigned>(report.slot)) ==
        WriterLiveness::Verdict::kDead);
  const auto token = d.dir.acquire_write_lock(/*capped=*/false);
  REQUIRE(token.acquired);
  CHECK(token.forced_release);
  d.dir.release_write_lock(token);

  // The parent's slot survived the child's claim, which closed the child's
  // inherited copy of the parent's claim descriptor.
  CHECK(d.live().slot_of() == parent_slot);
  CHECK(d.live().probe(static_cast<unsigned>(parent_slot)) ==
        WriterLiveness::Verdict::kAlive);
}

#if defined(__linux__)
namespace {

// Map our uid/gid to root in a fresh user namespace (unprivileged unshare).
bool write_file(const char *path, const std::string &text) {
  const int fd = ::open(path, O_WRONLY | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }
  const bool ok = ::write(fd, text.data(), text.size()) ==
                  static_cast<ssize_t>(text.size());
  ::close(fd);
  return ok;
}

int pid_namespace_flags() {
  return ::geteuid() == 0 ? CLONE_NEWPID : (CLONE_NEWUSER | CLONE_NEWPID);
}

}  // namespace

TEST_CASE(
    "Write-lock liveness: a holder in another PID namespace (unshare) is "
    "left alone while alive and recovered promptly once dead",
    "[writelock][multiprocess][liveness][pidns]") {
  SharedDir d;

  // Whether this kernel lets us create a PID namespace at all (root, or
  // unprivileged user namespaces); skip otherwise.
  {
    const pid_t probe = ::fork();
    REQUIRE(probe >= 0);
    if (probe == 0) {
      _exit(::unshare(pid_namespace_flags()) == 0 ? 0 : 1);
    }
    int status = 0;
    ::waitpid(probe, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      SKIP("unshare(CLONE_NEWPID) is not permitted here");
    }
  }

  // The holder is the first process (pid 1) of a new PID namespace; the
  // middle process forks it and tells us its pid as we see it.
  struct NsReport {
    uint32_t from_middle = 0;
    uint32_t ns_pid = 0;
    uint32_t real_pid = 0;
    int32_t slot = WriterLiveness::kNoSlot;
  };
  int pfd[2];
  REQUIRE(::pipe(pfd) == 0);
  const pid_t middle = ::fork();
  REQUIRE(middle >= 0);
  if (middle == 0) {
    ::close(pfd[0]);
    const uid_t uid = ::geteuid();
    const gid_t gid = ::getegid();
    if (::unshare(pid_namespace_flags()) != 0) {
      _exit(3);
    }
    if (uid != 0) {
      (void)write_file("/proc/self/setgroups", "deny");
      (void)write_file("/proc/self/uid_map", "0 " + std::to_string(uid) + " 1");
      (void)write_file("/proc/self/gid_map", "0 " + std::to_string(gid) + " 1");
    }
    const pid_t holder = ::fork();
    if (holder < 0) {
      _exit(4);
    }
    if (holder == 0) {
      const auto token = d.dir.acquire_write_lock(/*capped=*/false);
      NsReport r;
      r.ns_pid = static_cast<uint32_t>(::getpid());
      r.slot = token.liveness_slot;
      (void)!::write(pfd[1], &r, sizeof(r));
      for (;;) {
        ::pause();
      }
    }
    NsReport r;
    r.from_middle = 1;
    r.real_pid = static_cast<uint32_t>(holder);
    (void)!::write(pfd[1], &r, sizeof(r));
    int status = 0;
    ::waitpid(holder, &status, 0);
    _exit(0);
  }
  ::close(pfd[1]);
  NsReport a;
  NsReport b;
  REQUIRE(::read(pfd[0], &a, sizeof(a)) == static_cast<ssize_t>(sizeof(a)));
  REQUIRE(::read(pfd[0], &b, sizeof(b)) == static_cast<ssize_t>(sizeof(b)));
  ::close(pfd[0]);
  const NsReport &from_holder = a.from_middle != 0 ? b : a;
  const NsReport &from_middle = a.from_middle != 0 ? a : b;
  const uint32_t real_pid = from_middle.real_pid;
  REQUIRE(real_pid != 0);
  CHECK(from_holder.slot >= 0);
  CHECK(from_holder.ns_pid == 1);  // what the holder published
  CHECK(from_holder.ns_pid != real_pid);

  Waiter waiter(d.dir);
  std::this_thread::sleep_for(kLiveWindow);
  CHECK_FALSE(waiter.done.load(std::memory_order_acquire));
  CHECK(d.dir.get_shared_write_pos() == kBase);

  const auto killed_at = Clock::now();
  ::kill(static_cast<pid_t>(real_pid), SIGKILL);
  int status = 0;
  ::waitpid(middle, &status, 0);  // the middle reaps the holder first
  waiter.thread.join();
  REQUIRE(waiter.token.acquired);
  CHECK(waiter.token.forced_release);
  CHECK_FALSE(waiter.token.escalated_takeover);
  CHECK(waiter.done_at - killed_at < kPromptRecovery);
  d.dir.release_write_lock(waiter.token);
}
#endif  // __linux__

#endif  // !_WIN32
