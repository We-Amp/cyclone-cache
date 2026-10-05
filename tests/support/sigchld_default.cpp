// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Puts SIGCHLD back to its default disposition when the test binary
// starts.
//
// A disposition of "ignore" (or the SA_NOCLDWAIT flag) is inherited across
// exec: a test binary started by a runner or a script that ignores SIGCHLD
// would have its children reaped by the kernel the moment they exit.  The
// tests' "wait for the child, kill it at the deadline" loops would then
// never see the child exit, and its pid could belong to another process by
// the time the deadline passes.  signal_child() refuses such a pid anyway
// (it signals only unreaped children); this makes the waits themselves
// work as written.

#ifndef _WIN32

#include <signal.h>

#include <cstring>

namespace {

const bool g_sigchld_default = [] {
  struct sigaction action;
  std::memset(&action, 0, sizeof(action));
  action.sa_handler = SIG_DFL;
  sigemptyset(&action.sa_mask);
  action.sa_flags = 0;  // Also clears SA_NOCLDWAIT
  return ::sigaction(SIGCHLD, &action, nullptr) == 0;
}();

}  // namespace

#endif  // !_WIN32
