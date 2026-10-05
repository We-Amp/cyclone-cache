// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// signal_child() must refuse every pid that is not a single other process.
// Only signal 0 (an existence probe, which delivers nothing) is used here,
// so the case is harmless even if the helper were broken.

#ifndef _WIN32

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>
#include <cerrno>

#include "support/signal_child.hpp"

TEST_CASE("signal_child refuses pids that are not a single other process",
          "[support][signal]") {
  for (const pid_t pid : {pid_t{0}, pid_t{-1}, pid_t{-2}, ::getpid()}) {
    CAPTURE(pid);
    CHECK_FALSE(is_child_pid(pid));
    errno = 0;
    CHECK(signal_child(pid, 0) == -1);
    CHECK(errno == EINVAL);
  }
}

TEST_CASE("probe_pid and signal_grandchild refuse what they must",
          "[support][signal]") {
  for (const pid_t pid : {pid_t{0}, pid_t{-1}, pid_t{-2}}) {
    CAPTURE(pid);
    errno = 0;
    CHECK(probe_pid(pid) == -1);
    CHECK(errno == EINVAL);
  }
  CHECK(probe_pid(::getpid()) == 0);  // Signal 0: exists, nothing sent
  // No such middle process of ours: refused, whatever the grandchild pid.
  errno = 0;
  CHECK(signal_grandchild(::getpid(), 12345, 0) == -1);
  CHECK(errno == EINVAL);
}

TEST_CASE("SIGCHLD has its default disposition in the test binary",
          "[support][signal]") {
  struct sigaction current;
  REQUIRE(::sigaction(SIGCHLD, nullptr, &current) == 0);
  CHECK(current.sa_handler == SIG_DFL);
  CHECK((current.sa_flags & SA_NOCLDWAIT) == 0);
}

TEST_CASE("signal_child reaches a real child", "[support][signal]") {
  const pid_t pid = ::fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    ::_exit(0);
  }
  CHECK(is_child_pid(pid));
  // The child may already have exited; until it is reaped it still exists.
  CHECK(signal_child(pid, 0) == 0);
  int status = 0;
  REQUIRE(::waitpid(pid, &status, 0) == pid);
  // Reaped: the pid is no longer ours and may be reused, so it is refused.
  errno = 0;
  CHECK_FALSE(is_child_pid(pid));
  CHECK(signal_child(pid, 0) == -1);
  CHECK(errno == EINVAL);
}

#endif  // !_WIN32
