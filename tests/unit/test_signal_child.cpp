// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// signal_child() must refuse every pid that is not a single other process.
// Only signal 0 (an existence probe, which delivers nothing) is used here,
// so the case is harmless even if the helper were broken.

#ifndef _WIN32

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
}

#endif  // !_WIN32
