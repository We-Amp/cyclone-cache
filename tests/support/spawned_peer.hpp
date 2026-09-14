// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Parent-side wrapper around one spawned cyclone-test-peer process (see
// tests/support/peer_main.cpp for the protocol).
//
// Handshake transport is a pair of anonymous pipes on the child's
// stdin/stdout.  This is the only mechanism with free BIDIRECTIONAL death
// detection: the child dying makes the parent's read hit EOF (no hang), and
// the parent dying makes the child's blocking stdin read hit EOF so the child
// exits (no orphaned peers on a persistent CI runner).  It also takes no lock
// on the volume, so the handshake cannot perturb what the tests measure.
//
// POSIX spawning is posix_spawn, NOT fork+exec: fork in the multi-threaded
// test binary followed by anything nontrivial is TSan-illegal, and
// posix_spawn keeps these tests runnable under every sanitizer lane.
//
// EVERY blocking operation takes an explicit deadline.  No CI lane runs
// ctest, so no per-test timeout exists anywhere else; a deadline expiry here
// must become a red test, never a hung job.

#ifndef CYCLONE_TESTS_SUPPORT_SPAWNED_PEER_HPP
#define CYCLONE_TESTS_SUPPORT_SPAWNED_PEER_HPP

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX  // windows.h min/max macros would break std::min/std::max
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>

extern char** environ;
#endif

class SpawnedPeer {
 public:
  SpawnedPeer() = default;
  ~SpawnedPeer() { kill(); }

  SpawnedPeer(const SpawnedPeer&) = delete;
  SpawnedPeer& operator=(const SpawnedPeer&) = delete;
  SpawnedPeer(SpawnedPeer&&) = delete;
  SpawnedPeer& operator=(SpawnedPeer&&) = delete;

  // Launch `exe` with `args` (argv[1..]); stdin/stdout wired to our pipes,
  // stderr inherited so peer diagnostics land in the test log.
  bool spawn(const std::string& exe, const std::vector<std::string>& args);

  // Read one status line ("READY" / "ERR ...") from the child's stdout.
  // Robust to partial writes: accumulates until '\n' (a trailing '\r' from a
  // Windows text-mode child is stripped).  On child death with a partial
  // line buffered, returns that partial line.  nullopt = deadline expired or
  // the child died without saying anything.
  std::optional<std::string> wait_ready(std::chrono::milliseconds deadline);

  // Ask the child to exit: write "EXIT\n", then close its stdin so EOF backs
  // the request up even if the line is missed.
  void request_exit();

  // Reap the child.  Returns its exit code (POSIX: -signo if killed by a
  // signal); nullopt = still running at the deadline.
  std::optional<int> wait_exit(std::chrono::milliseconds deadline);

  // Hard teardown: terminate if still running, reap, close every handle.
  // Idempotent; the destructor calls it.
  void kill();

 private:
  std::string buf_;
  bool reaped_ = false;
  int exit_code_ = -1;

#ifdef _WIN32
  HANDLE proc_ = nullptr;
  HANDLE stdin_w_ = INVALID_HANDLE_VALUE;   // parent writes the child's stdin
  HANDLE stdout_r_ = INVALID_HANDLE_VALUE;  // parent reads the child's stdout
#else
  pid_t pid_ = -1;
  int stdin_w_ = -1;
  int stdout_r_ = -1;
#endif
};

#ifdef _WIN32

inline bool SpawnedPeer::spawn(const std::string& exe,
                               const std::vector<std::string>& args) {
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE in_r = INVALID_HANDLE_VALUE;
  HANDLE in_w = INVALID_HANDLE_VALUE;
  HANDLE out_r = INVALID_HANDLE_VALUE;
  HANDLE out_w = INVALID_HANDLE_VALUE;
  if (!CreatePipe(&in_r, &in_w, &sa, 0)) {
    return false;
  }
  if (!CreatePipe(&out_r, &out_w, &sa, 0)) {
    CloseHandle(in_r);
    CloseHandle(in_w);
    return false;
  }
  // The PARENT ends must not be inherited, or the child itself holds a write
  // end of its own stdout pipe and the parent's reads never see EOF.
  SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOA si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = in_r;
  si.hStdOutput = out_w;
  si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

  // Quote every token.  Sufficient here: the arguments are temp paths and
  // integers, never containing embedded quotes or trailing backslashes.
  std::string cmd = "\"" + exe + "\"";
  for (const auto& a : args) {
    cmd += " \"" + a + "\"";
  }
  std::vector<char> cmdbuf(cmd.begin(), cmd.end());
  cmdbuf.push_back('\0');

  PROCESS_INFORMATION pi{};
  const BOOL ok =
      CreateProcessA(exe.c_str(), cmdbuf.data(), nullptr, nullptr,
                     /*bInheritHandles=*/TRUE, 0, nullptr, nullptr, &si, &pi);
  // Close the CHILD-side ends in the parent regardless of outcome; keeping
  // out_w open here would mean reads on out_r never return EOF.
  CloseHandle(in_r);
  CloseHandle(out_w);
  if (!ok) {
    CloseHandle(in_w);
    CloseHandle(out_r);
    return false;
  }
  CloseHandle(pi.hThread);
  proc_ = pi.hProcess;
  stdin_w_ = in_w;
  stdout_r_ = out_r;
  return true;
}

inline std::optional<std::string> SpawnedPeer::wait_ready(
    std::chrono::milliseconds deadline) {
  const auto expiry = std::chrono::steady_clock::now() + deadline;
  auto take_line = [this]() -> std::optional<std::string> {
    const auto nl = buf_.find('\n');
    if (nl == std::string::npos) {
      return std::nullopt;
    }
    std::string line = buf_.substr(0, nl);
    buf_.erase(0, nl + 1);
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();  // text-mode child stdout writes \r\n
    }
    return line;
  };

  for (;;) {
    if (auto line = take_line()) {
      return line;
    }
    // Checked at the top so EVERY iteration is deadline-bounded — including
    // the read-and-continue path, where a child streaming bytes with no '\n'
    // would otherwise spin here forever (matches the POSIX twin).
    if (std::chrono::steady_clock::now() >= expiry) {
      return std::nullopt;
    }
    // Anonymous pipes do not support overlapped reads, so the deadline is
    // enforced by polling PeekNamedPipe (non-blocking) instead of ReadFile.
    DWORD avail = 0;
    if (!PeekNamedPipe(stdout_r_, nullptr, 0, nullptr, &avail, nullptr)) {
      // Broken pipe: the child is gone.  Surface any partial line.
      if (!buf_.empty()) {
        std::string line = buf_;
        buf_.clear();
        if (!line.empty() && line.back() == '\r') {
          line.pop_back();
        }
        return line;
      }
      return std::nullopt;
    }
    if (avail > 0) {
      char tmp[256];
      DWORD got = 0;
      const DWORD want =
          avail < sizeof(tmp) ? avail : static_cast<DWORD>(sizeof(tmp));
      if (ReadFile(stdout_r_, tmp, want, &got, nullptr) && got > 0) {
        buf_.append(tmp, got);
      }
      continue;
    }
    if (WaitForSingleObject(proc_, 0) == WAIT_OBJECT_0) {
      // Child exited; one final peek closes the exited-after-write race.
      DWORD late = 0;
      if (PeekNamedPipe(stdout_r_, nullptr, 0, nullptr, &late, nullptr) &&
          late > 0) {
        continue;
      }
      if (!buf_.empty()) {
        std::string line = buf_;
        buf_.clear();
        if (!line.empty() && line.back() == '\r') {
          line.pop_back();
        }
        return line;
      }
      return std::nullopt;
    }
    Sleep(10);
  }
}

inline void SpawnedPeer::request_exit() {
  if (stdin_w_ != INVALID_HANDLE_VALUE) {
    DWORD written = 0;
    WriteFile(stdin_w_, "EXIT\n", 5, &written, nullptr);
    CloseHandle(stdin_w_);  // EOF backstop if the line is missed
    stdin_w_ = INVALID_HANDLE_VALUE;
  }
}

inline std::optional<int> SpawnedPeer::wait_exit(
    std::chrono::milliseconds deadline) {
  if (reaped_) {
    return exit_code_;
  }
  if (proc_ == nullptr) {
    return std::nullopt;
  }
  const DWORD ms =
      deadline.count() < 0 ? 0 : static_cast<DWORD>(deadline.count());
  if (WaitForSingleObject(proc_, ms) != WAIT_OBJECT_0) {
    return std::nullopt;
  }
  DWORD code = 0;
  if (!GetExitCodeProcess(proc_, &code)) {
    return std::nullopt;
  }
  reaped_ = true;
  exit_code_ = static_cast<int>(code);
  return exit_code_;
}

inline void SpawnedPeer::kill() {
  if (proc_ != nullptr && !reaped_) {
    if (WaitForSingleObject(proc_, 0) != WAIT_OBJECT_0) {
      TerminateProcess(proc_, 1);
    }
    if (WaitForSingleObject(proc_, 10000) == WAIT_OBJECT_0) {
      DWORD code = 0;
      if (GetExitCodeProcess(proc_, &code)) {
        exit_code_ = static_cast<int>(code);
      }
      reaped_ = true;
    }
    // else: the child is wedged past TerminateProcess — beyond saving.  Do
    // NOT record STILL_ACTIVE (259) as an exit code; leaving exit_code_ = -1
    // keeps wait_exit honest.  Handles are still closed below: the test is
    // ending either way.
  }
  if (proc_ != nullptr) {
    CloseHandle(proc_);
    proc_ = nullptr;
  }
  if (stdin_w_ != INVALID_HANDLE_VALUE) {
    CloseHandle(stdin_w_);
    stdin_w_ = INVALID_HANDLE_VALUE;
  }
  if (stdout_r_ != INVALID_HANDLE_VALUE) {
    CloseHandle(stdout_r_);
    stdout_r_ = INVALID_HANDLE_VALUE;
  }
}

#else  // POSIX

inline bool SpawnedPeer::spawn(const std::string& exe,
                               const std::vector<std::string>& args) {
  int in_pipe[2];   // parent -> child stdin
  int out_pipe[2];  // child stdout -> parent
  if (::pipe(in_pipe) != 0) {
    return false;
  }
  if (::pipe(out_pipe) != 0) {
    ::close(in_pipe[0]);
    ::close(in_pipe[1]);
    return false;
  }
  // The parent-kept ends must not leak into other spawned children: in a
  // two-peer test, peer B inheriting peer A's parent-side ends would defer
  // A's EOF backstop for as long as B lives.
  ::fcntl(in_pipe[1], F_SETFD, FD_CLOEXEC);
  ::fcntl(out_pipe[0], F_SETFD, FD_CLOEXEC);

  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_adddup2(&fa, in_pipe[0], 0);
  posix_spawn_file_actions_adddup2(&fa, out_pipe[1], 1);
  posix_spawn_file_actions_addclose(&fa, in_pipe[0]);
  posix_spawn_file_actions_addclose(&fa, in_pipe[1]);
  posix_spawn_file_actions_addclose(&fa, out_pipe[0]);
  posix_spawn_file_actions_addclose(&fa, out_pipe[1]);

  std::vector<char*> argv;
  argv.push_back(const_cast<char*>(exe.c_str()));
  for (const auto& a : args) {
    argv.push_back(const_cast<char*>(a.c_str()));
  }
  argv.push_back(nullptr);

  pid_t pid = -1;
  const int rc =
      ::posix_spawn(&pid, exe.c_str(), &fa, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  ::close(in_pipe[0]);
  ::close(out_pipe[1]);
  if (rc != 0) {
    ::close(in_pipe[1]);
    ::close(out_pipe[0]);
    return false;
  }
  pid_ = pid;
  stdin_w_ = in_pipe[1];
  stdout_r_ = out_pipe[0];
  // Non-blocking so the deadline governs even a pathological read.
  const int flags = ::fcntl(stdout_r_, F_GETFL);
  if (flags >= 0) {
    ::fcntl(stdout_r_, F_SETFL, flags | O_NONBLOCK);
  }
  return true;
}

inline std::optional<std::string> SpawnedPeer::wait_ready(
    std::chrono::milliseconds deadline) {
  const auto expiry = std::chrono::steady_clock::now() + deadline;
  for (;;) {
    const auto nl = buf_.find('\n');
    if (nl != std::string::npos) {
      std::string line = buf_.substr(0, nl);
      buf_.erase(0, nl + 1);
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      return line;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= expiry) {
      return std::nullopt;
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(expiry - now)
            .count();
    struct pollfd p{};
    p.fd = stdout_r_;
    p.events = POLLIN;
    const int prc =
        ::poll(&p, 1, static_cast<int>(remaining < 100 ? remaining : 100));
    if (prc <= 0) {
      continue;  // timeout slice or EINTR; the deadline check above governs
    }
    char tmp[256];
    const ssize_t n = ::read(stdout_r_, tmp, sizeof(tmp));
    if (n > 0) {
      buf_.append(tmp, static_cast<size_t>(n));
      continue;
    }
    if (n < 0) {
      if (errno == EAGAIN || errno == EINTR) {
        continue;
      }
      return std::nullopt;
    }
    // EOF (n == 0): the child died.  Surface any partial line.
    if (buf_.empty()) {
      return std::nullopt;
    }
    std::string line = buf_;
    buf_.clear();
    return line;
  }
}

inline void SpawnedPeer::request_exit() {
  if (stdin_w_ >= 0) {
    const ssize_t wrote = ::write(stdin_w_, "EXIT\n", 5);
    (void)wrote;        // best-effort: the close below is the EOF backstop
    ::close(stdin_w_);  // EOF backstop if the line is missed
    stdin_w_ = -1;
  }
}

inline std::optional<int> SpawnedPeer::wait_exit(
    std::chrono::milliseconds deadline) {
  if (reaped_) {
    return exit_code_;
  }
  if (pid_ <= 0) {
    return std::nullopt;
  }
  const auto expiry = std::chrono::steady_clock::now() + deadline;
  for (;;) {
    int status = 0;
    const pid_t got = ::waitpid(pid_, &status, WNOHANG);
    if (got == pid_) {
      reaped_ = true;
      exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status)
                                     : -static_cast<int>(WTERMSIG(status));
      return exit_code_;
    }
    if (got < 0 && errno != EINTR) {
      return std::nullopt;
    }
    if (std::chrono::steady_clock::now() >= expiry) {
      return std::nullopt;
    }
    struct timespec ts{0, 10'000'000L};  // 10ms
    ::nanosleep(&ts, nullptr);
  }
}

inline void SpawnedPeer::kill() {
  if (pid_ > 0 && !reaped_) {
    ::kill(pid_, SIGKILL);
    int status = 0;
    while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
    }
    reaped_ = true;
    exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status)
                                   : -static_cast<int>(WTERMSIG(status));
  }
  if (stdin_w_ >= 0) {
    ::close(stdin_w_);
    stdin_w_ = -1;
  }
  if (stdout_r_ >= 0) {
    ::close(stdout_r_);
    stdout_r_ = -1;
  }
}

#endif  // _WIN32

#endif  // CYCLONE_TESTS_SUPPORT_SPAWNED_PEER_HPP
