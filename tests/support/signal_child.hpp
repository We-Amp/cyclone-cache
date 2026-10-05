// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The one way tests send a signal to another process.
//
// kill() with a pid that is not a real child does not fail, it widens:
//   pid ==  0  signals every process in the caller's process group,
//   pid == -1  signals every process the caller may signal, which for an
//              ordinary user is everything that user runs (shells, editors,
//              the CI runner, the test harness itself),
//   pid <  -1  signals the process group -pid.
// A test reaches those values by copy-paste more easily than it seems:
// fork() returns -1 when it fails, a pid variable is often initialised to
// -1 or 0, and a waitpid(-1, ...) that "reaps" the wrong child leaves the
// variable unchanged.  So every signal a test sends goes through
// signal_child(), which refuses anything but a single other process, and
// every wait for a specific child goes through is_child_pid() first
// (waitpid(-1 or 0) would reap ANY child, which is wrong as well).
//
// Rule for new tests: check the result of fork() (REQUIRE(pid >= 0))
// before anything else in the parent, and never call ::kill() directly.

#ifndef CYCLONE_TESTS_SUPPORT_SIGNAL_CHILD_HPP
#define CYCLONE_TESTS_SUPPORT_SIGNAL_CHILD_HPP

#ifndef _WIN32

#include <signal.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>

// True when `pid` names one process other than this one: what fork()
// returns to the parent on success.
[[nodiscard]] inline bool is_child_pid(pid_t pid) noexcept {
  return pid > 0 && pid != ::getpid();
}

// kill(pid, sig) for exactly one other process; sig 0 probes it.  Refuses
// pid <= 0 and this process's own pid: returns -1 with errno EINVAL and
// sends nothing, so a caller that checks the result fails its test and one
// that does not check it does no harm.
inline int signal_child(pid_t pid, int sig) noexcept {
  if (!is_child_pid(pid)) {
    std::fprintf(stderr,
                 "signal_child: refused to send signal %d to pid %ld (not a "
                 "single other process)\n",
                 sig, static_cast<long>(pid));
    errno = EINVAL;
    return -1;
  }
  return ::kill(pid, sig);
}

#endif  // !_WIN32

#endif  // CYCLONE_TESTS_SUPPORT_SIGNAL_CHILD_HPP
