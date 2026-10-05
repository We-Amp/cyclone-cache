// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#pragma once

#include <atomic>
#include <cstdint>

namespace cyclone {

// Fork gate: keeps the library's own background threads out of the way of
// fork().
//
// A process that opens a cache runs library threads (hit-count flush,
// periodic directory sync, optimization workers).  fork() copies the address
// space as it is at that instant and only the forking thread survives in the
// child.  A lock one of those threads held at that instant is therefore
// locked in the child for good: nothing there will ever release it, and the
// child blocks on its first use of it (a read that hashes to a hit-tracker
// stripe the flush thread was sweeping; a teardown that takes the cache gate
// the sync thread held shared).
//
// The rule that prevents it: a library thread touches cache state only
// inside a ForkGatedPass, and fork() waits for every pass in flight and
// holds off new ones until it has returned.  The gate itself is two atomic
// counters, no mutex and no condition variable, so there is nothing of its
// own a child could inherit locked and nothing the child handler does beyond
// plain stores.
//
// What the gate does NOT cover: threads of the application.  A lock an
// application thread holds inside a cache call at the moment another thread
// forks is inherited locked, as with any other library; see the fork
// contract in include/cyclone/cache.hpp.
//
// Windows has no fork: everything here is inert there.

// Registers the pthread_atfork handlers (once per process; later calls are
// no-ops).  Call it before starting a thread that uses ForkGatedPass.
void install_fork_handlers() noexcept;

// Bumped in every forked child, so state can tell that it now lives in
// another process than the one that set it up, whatever the PIDs say.
// Counts only forks made after install_fork_handlers(); 0 on Windows.
[[nodiscard]] uint32_t fork_epoch() noexcept;

#ifdef CYCLONE_TEST_SEAMS
// Test seam: the number of fork() calls that are between their prepare and
// parent handlers right now (always 0 on Windows).
[[nodiscard]] uint32_t forks_pending_for_test() noexcept;
#endif

// One unit of background work that must not be cut in half by fork().
//
//   ForkGatedPass pass(_running);
//   if (!pass) break;   // the owner asked us to stop while a fork was pending
//   ... take locks, do the work, release them ...
//
// The constructor waits while a fork() is between its prepare and
// parent handlers; fork() in turn waits (in prepare) for every pass that
// already started.  `keep_running` is the owning thread's stop flag: when
// it turns false during that wait the pass is not entered, so a stop() that
// joins this thread cannot be held up by a fork that is itself waiting on
// another pass.  Passes nest; a nested pass on the same thread never waits.
//
// A pass should be short and must not block on anything fork() could be
// holding: fork() does not return until the pass ends.
class ForkGatedPass {
 public:
  explicit ForkGatedPass(const std::atomic<bool>& keep_running) noexcept;
  ~ForkGatedPass();

  ForkGatedPass(const ForkGatedPass&) = delete;
  ForkGatedPass& operator=(const ForkGatedPass&) = delete;

  [[nodiscard]] explicit operator bool() const noexcept { return _entered; }

 private:
  bool _entered = false;
};

}  // namespace cyclone
