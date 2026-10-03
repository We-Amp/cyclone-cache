// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// A process may exit with Cache objects still open.
//
// The embedder shape behind a downstream package smoke test:
// a .NET host creates caches through the C API (ps_cache_open ->
// cyclone_cache_create), never calls cyclone_cache_destroy, and returns from
// main (or calls exit) while the caches' background threads are alive.  On
// POSIX, exit() runs static destructors and atexit handlers with those
// threads still running, then the OS reaps them; on Windows, ExitProcess
// terminates the other threads first and then runs DLL_PROCESS_DETACH.  The
// contract pinned here: that process exits 0, with no fault, no sanitizer
// report and no hang.
//
// Why this is safe (audit at the time of writing; keep it so):
//
//   Threads a started Cache owns, all cooperative loops owned by heap objects
//   that are never destroyed in this scenario, so no join and no destructor
//   runs under them:
//     DirectorySyncer::thread_func      (cache.cpp; mmap directory only)
//     HitTracker::flush_thread_func     (hit_tracker.cpp; enable_hit_tracking)
//     OptimizationEngine::monitoring_loop and
//     AdaptiveThreadPool::worker_loop   (optimization_config.enabled, the
//                                        default; min_threads workers)
//   Executor::global() has no caller, so its leak_thread_on_shutdown path
//   (thread_util.hpp) never runs.  Nothing is detach()ed.
//
//   Function-local / class statics with a non-trivial destructor that a
//   thread could touch after it is destroyed:
//     liveness_mutex()                  (mmap_directory.cpp) -- taken on
//                                        attach/detach (open/close) and, by
//                                        ANY writer waiting on the
//                                        cross-process write lock behind a
//                                        holder stalled past 50 ms, after
//                                        every sleep.  That is reachable at
//                                        exit (the lockwait case below
//                                        aborted on Apple's libc++ before
//                                        the fix), so the mutex is immortal:
//                                        allocated once, never destroyed.
//     cache_error_category()            (error.hpp) -- only error formatting
//     Volume::s_*_for_test std::function hooks (volume.hpp) -- empty in
//                                        production; a destroyed empty
//                                        std::function still tests false
//   Every other static is a trivially destructible std::atomic or a
//   thread_local of trivially destructible slots (HighResolutionTimer owns a
//   Win32 handle and is destroyed per thread, never from another thread).
//
// Each case spawns the helper in a shape of that scenario and requires exit
// code 0 before a deadline.  The spawn transport is the same anonymous-pipe
// protocol as test_reset_gate_spawn.cpp; the helper says READY when its
// caches are open and loaded, and leaves the process on EXIT.  Under ASan
// and TSan the helper disables leak and thread-leak reporting for this one
// mode (the leak is the premise), so a non-zero exit there is a real
// use-after-free, a data race on a destroyed object, or a crash.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include "support/spawned_peer.hpp"
#include "support/temp_cache.hpp"

namespace {

// Generous: a sanitizer build loads two caches from four threads for 300 ms
// and exits through the runtime's teardown; a hang is the finding, so the
// deadline must be far from the normal duration (well under a second).
constexpr std::chrono::milliseconds kDeadline{60000};
constexpr unsigned long long kVolSize = 32ULL * 1024 * 1024;

// REQUIRE, never SKIP: a missing helper exe must be a red test, not silently
// reduced coverage that still looks green.
std::string peer_exe() {
  const std::string exe = CYCLONE_TEST_PEER_EXE;
  REQUIRE(std::filesystem::exists(exe));
  return exe;
}

// Spawn `shape` leaving the process by `how` ("return" from main or
// std::exit(0)) and require a clean exit.  The test directory outlives the
// peer (declared first by the caller), so the peer's mapping and handles are
// closed before the directory is removed.
void require_clean_exit(const TempCacheDir& dir, const std::string& shape,
                        const std::string& how) {
  SpawnedPeer peer;
  const std::string exe = peer_exe();
  INFO("shape=" << shape << " how=" << how);
  REQUIRE(peer.spawn(exe, {"exitopen", dir.dir().string(),
                           std::to_string(kVolSize), shape, how}));
  auto ready = peer.wait_ready(kDeadline);
  REQUIRE(ready.has_value());
  REQUIRE(*ready == "READY");
  peer.request_exit();
  auto code = peer.wait_exit(kDeadline);
  REQUIRE(code.has_value());  // nullopt = still alive at the deadline: a hang
  REQUIRE(*code == 0);        // negative = signal (POSIX); >0 = sanitizer
}

}  // namespace

TEST_CASE("exit with one open C-API cache is clean",
          "[lifecycle][exit][spawn]") {
  TempCacheDir dir("exit1");
  for (const char* how : {"return", "exit"}) {
    require_clean_exit(dir, "capi1", how);
  }
}

TEST_CASE("exit with two open C-API caches, the second opened under load",
          "[lifecycle][exit][spawn]") {
  TempCacheDir dir("exit2");
  for (const char* how : {"return", "exit"}) {
    require_clean_exit(dir, "capi2", how);
  }
}

TEST_CASE("exit with two in-process opens of the same volume file",
          "[lifecycle][exit][spawn]") {
  TempCacheDir dir("exit2s");
  for (const char* how : {"return", "exit"}) {
    require_clean_exit(dir, "capi2same", how);
  }
}

TEST_CASE("exit while embedder threads are still writing to open caches",
          "[lifecycle][exit][spawn]") {
  TempCacheDir dir("exit2l");
  for (const char* how : {"return", "exit"}) {
    require_clean_exit(dir, "capi2live", how);
  }
}

TEST_CASE("exit while a writer waits on the cross-process write lock",
          "[lifecycle][exit][spawn]") {
  // Two caches on one volume file: A's writer parked holding the write lock,
  // B's writer probing the holder's liveness through the process-wide
  // liveness mutex after every sleep, and a 500 ms atexit handler so static
  // destruction is over while B still probes.  Before the liveness mutex
  // became immortal this aborted on Apple's libc++ (lock of a destroyed
  // pthread mutex: EINVAL -> std::system_error -> std::terminate, exit -6);
  // glibc and MSVC have a trivial ~mutex, so the lock there was a silent
  // no-op on a dead object.
  TempCacheDir dir("exitlock");
  for (const char* how : {"return", "exit"}) {
    require_clean_exit(dir, "lockwait", how);
  }
}

TEST_CASE("exit with every Cache background thread busy",
          "[lifecycle][exit][spawn]") {
  // 1 ms directory sync, 1 ms hit flush, two optimization workers writing
  // alternates: every thread an open Cache owns is mid-work at exit.
  TempCacheDir dir("exitbusy");
  for (const char* how : {"return", "exit"}) {
    require_clean_exit(dir, "cppbusy", how);
  }
}
