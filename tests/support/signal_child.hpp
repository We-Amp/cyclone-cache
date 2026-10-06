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
// variable unchanged.  And a pid that WAS our child may belong to another
// process by now, if the child was reaped (with SIGCHLD ignored the
// kernel reaps it on its own).  So every signal a test sends goes through
// signal_child(), which signals only an unreaped child of this process,
// and every wait for a specific child goes through is_child_pid() first
// (waitpid(-1 or 0) would reap ANY child, which is wrong as well).  The
// two narrow exceptions, signal_grandchild() and probe_pid(), say at
// their definitions why they are safe.
//
// Rule for new tests: check the result of fork() (REQUIRE(pid >= 0))
// before anything else in the parent, never call ::kill() directly, and
// signal a child only before it has been reaped.

#ifndef CYCLONE_TESTS_SUPPORT_SIGNAL_CHILD_HPP
#define CYCLONE_TESTS_SUPPORT_SIGNAL_CHILD_HPP

#ifndef _WIN32

#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>

// True when `pid` is a child of this process that has not been reaped yet
// (running, stopped, or exited and waiting to be reaped).  While a child
// is unreaped its pid cannot be given to another process, so a signal sent
// right after this check reaches that child and nothing else.  Checked
// with a waitid() that neither blocks nor reaps (WNOHANG | WNOWAIT): it
// fails with ECHILD for any pid that is not our unreaped child, including
// one that was already reaped -- by us, or automatically because SIGCHLD
// was ignored (see sigchld_default.cpp) -- and may by now belong to an
// unrelated process.
[[nodiscard]] inline bool is_child_pid(pid_t pid) noexcept {
  if (pid <= 0 || pid == ::getpid()) {
    return false;
  }
  siginfo_t info{};
  for (;;) {
    if (::waitid(P_PID, static_cast<id_t>(pid), &info,
                 WEXITED | WSTOPPED | WCONTINUED | WNOHANG | WNOWAIT) == 0) {
      return true;
    }
    if (errno != EINTR) {
      return false;
    }
  }
}

namespace signal_child_detail {
inline int refuse(const char *what, pid_t pid, int sig) noexcept {
  std::fprintf(stderr, "%s: refused to send signal %d to pid %ld\n", what, sig,
               static_cast<long>(pid));
  errno = EINVAL;
  return -1;
}
}  // namespace signal_child_detail

// kill(pid, sig) for exactly one unreaped child of this process; sig 0
// probes it.  Refuses anything else (see is_child_pid): returns -1 with
// errno EINVAL and sends nothing, so a caller that checks the result fails
// its test and one that does not check it does no harm.
inline int signal_child(pid_t pid, int sig) noexcept {
  if (!is_child_pid(pid)) {
    return signal_child_detail::refuse("signal_child", pid, sig);
  }
  return ::kill(pid, sig);
}

// The one case where a test must signal a process that is not its own
// child: a grandchild whose parent ("middle") is our child and reaps it.
// Safe while `middle` is our unreaped child that has NOT exited: the
// middle process waits for the grandchild and exits right after reaping
// it, so while the middle is still running the grandchild has not been
// reaped and its pid cannot have been reused.  (The remaining window --
// the middle reaped the grandchild but has not exited yet -- requires the
// grandchild to have died on its own; the callers' grandchildren only end
// by this signal.)
inline int signal_grandchild(pid_t middle, pid_t grandchild, int sig) noexcept {
  if (!is_child_pid(middle) || grandchild <= 0 || grandchild == ::getpid() ||
      grandchild == middle) {
    return signal_child_detail::refuse("signal_grandchild", grandchild, sig);
  }
  siginfo_t info{};
  if (::waitid(P_PID, static_cast<id_t>(middle), &info,
               WEXITED | WNOHANG | WNOWAIT) != 0 ||
      info.si_pid != 0) {
    // The middle process has exited (or cannot be checked): it may have
    // reaped the grandchild, whose pid may be reused.
    return signal_child_detail::refuse("signal_grandchild", grandchild, sig);
  }
  return ::kill(grandchild, sig);
}

// Does a process with this pid exist?  kill(pid, 0) for any single pid,
// child or not: signal 0 only checks and delivers nothing, so it cannot
// harm whichever process holds the pid.  Refuses pid <= 0 (a process group
// or every process) like the helpers above.
inline int probe_pid(pid_t pid) noexcept {
  if (pid <= 0) {
    return signal_child_detail::refuse("probe_pid", pid, 0);
  }
  return ::kill(pid, 0);
}

#endif  // !_WIN32

#endif  // CYCLONE_TESTS_SUPPORT_SIGNAL_CHILD_HPP
