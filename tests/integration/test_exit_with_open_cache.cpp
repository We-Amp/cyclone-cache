// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// A process may exit with Cache objects still open.
//
// The embedder shape behind a downstream package smoke test:
// a .NET host creates caches through the C API (ps_cache_open ->
// cyclone_cache_create), never calls cyclone_cache_destroy, and returns from
// main (or calls exit) while the caches' background threads are alive.  On
// POSIX, exit() runs static destructors and atexit handlers with those
// threads still running, then the OS reaps them.  On Windows it depends on
// how Cyclone is linked: in a DLL, ExitProcess terminates the other threads
// before DLL_PROCESS_DETACH runs the DLL's static destructors; in a
// statically linked executable (this test's peer, a native embedder) the
// UCRT's exit() runs the executable's static destructors with the other
// threads alive, as on POSIX, and the safety of a destroyed std::mutex there
// rests on MSVC's ~mutex being a no-op in release builds.  The contract
// pinned here: that process exits 0, with no fault, no sanitizer report and
// no hang.  It covers Cyclone's own statics only, not embedder-owned objects
// a Cache thread can reach that the embedder destroys at exit.
//
// Why this is safe (audit at the time of writing; keep it so):
//
//   Threads a started Cache owns, all cooperative loops owned by heap objects
//   that are never destroyed in this scenario, so no join and no destructor
//   runs under them:
//     DirectorySyncer::thread_func      (cache.cpp; mmap directory only)
//     HitTracker::flush_thread_func     (hit_tracker.cpp; enable_hit_tracking)
//     OptimizationEngine::monitoring_loop and
//     AdaptiveThreadPool::worker_loop   (optimization_config.enabled, off by
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
//   State of a dependency that its own exit handler tears down:
//     the key hash                      (key.cpp) -- every cache call of an
//                                        embedder thread starts by hashing
//                                        its key.  With the OpenSSL backend
//                                        that went through EVP, which looks
//                                        the digest up in OpenSSL 3's default
//                                        library context; OPENSSL_cleanup, an
//                                        exit handler, frees that context.  A
//                                        thread still writing at exit then
//                                        faulted inside the lookup (SIGSEGV,
//                                        or an abort from the allocator), or,
//                                        once the cleanup was over, got a
//                                        failed hash and an all-zero key.
//                                        The backend now uses the SHA256_*
//                                        functions on a stack context, which
//                                        reach none of that state.  The
//                                        bundled backend never did.  Only the
//                                        capi2live case below has embedder
//                                        threads in cache calls at exit, and
//                                        only a build with the OpenSSL
//                                        backend can show this; CI has a job
//                                        for it.
//
// Each case spawns the helper in a shape of that scenario and requires exit
// code 0 before a deadline.  The spawn transport is the same anonymous-pipe
// protocol as test_reset_gate_spawn.cpp; the helper says READY when its
// caches are open and loaded, and leaves the process on EXIT.  Leaking the
// caches is the premise, so for this mode only the leak reports are off:
// the helper calls __lsan_disable() in the exitopen mode (a runtime call, so
// the other peer modes, which stop their caches, keep LeakSanitizer), and
// this test spawns the helper with the lane's TSAN_OPTIONS extended by
// report_thread_leaks=0 (SpawnedPeer::spawn's extra_env REPLACES the
// inherited variable; a process-wide __tsan_default_options in the helper
// would have hidden real thread leaks in the other modes).  That flag is
// belt and braces: TSan reports a thread leak only for a thread that has
// FINISHED without being joined or detached, and the Cache threads are
// still running when the process exits while the helper joins or detaches
// its own, so the mode is clean with the flag off as well.  A non-zero exit
// is then a real use-after-free, a data race on a destroyed object, or a
// crash.
//
// The READY line echoes the probe variable and TSAN_OPTIONS the helper saw,
// and every case asserts the override took effect: this process sets
// CYCLONE_PEER_PROBE to one value in its own environment and passes another
// in extra_env, so the check proves replacement (getenv and the sanitizer
// runtimes take the FIRST match of a name; an appended duplicate would not
// have worked), on all three platforms, including the Windows environment
// block.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#ifndef _WIN32
#include <stdlib.h>  // setenv
#endif

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

// The lane's own TSAN_OPTIONS (its suppressions file), or empty.
std::string lane_tsan_options() {
  const char* current = std::getenv("TSAN_OPTIONS");
  return current == nullptr ? std::string() : std::string(current);
}

// TSAN_OPTIONS for the helper: the lane's value with report_thread_leaks=0
// appended.  Harmless in a non-TSan build: an unused environment variable.
std::string peer_tsan_options() {
  std::string opts = lane_tsan_options();
  if (!opts.empty()) {
    opts += ":";
  }
  return "TSAN_OPTIONS=" + opts + "report_thread_leaks=0";
}

// Set CYCLONE_PEER_PROBE in THIS process so the helper's copy must have
// replaced an inherited value, not filled an absent one.  Catch2 runs the
// cases on one thread and every earlier case has joined its threads, so the
// write cannot race a getenv.
void set_parent_probe() {
#ifdef _WIN32
  _putenv_s("CYCLONE_PEER_PROBE", "parent");
#else
  ::setenv("CYCLONE_PEER_PROBE", "parent", 1);
#endif
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
  set_parent_probe();
  REQUIRE(peer.spawn(
      exe,
      {"exitopen", dir.dir().string(), std::to_string(kVolSize), shape, how},
      {peer_tsan_options(), "CYCLONE_PEER_PROBE=child"}));
  auto ready = peer.wait_ready(kDeadline);
  REQUIRE(ready.has_value());
  INFO("ready line: " << *ready);
  // "READY probe=<CYCLONE_PEER_PROBE> tsan=<TSAN_OPTIONS>" as the helper saw
  // them: the extras replaced the inherited values.
  const std::string prefix = "READY probe=child tsan=";
  REQUIRE(ready->starts_with(prefix));
  const std::string seen_tsan = ready->substr(prefix.size());
  REQUIRE(seen_tsan.find("report_thread_leaks=0") != std::string::npos);
  if (const std::string lane = lane_tsan_options(); !lane.empty()) {
    REQUIRE(seen_tsan.find(lane) != std::string::npos);  // suppressions kept
  }
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
  // The writers keep going for 300 ms after every other exit handler has run
  // and every later static is destroyed (the helper registers a sleeping
  // handler first, as lockwait does), and the handler then checks the key
  // hash against a known answer: exit code 3 if it is wrong.  With the
  // EVP-based OpenSSL key hash this failed every run, as a fault in the
  // digest lookup or as the wrong hash; without the sleep and the check it
  // failed about one run in two, whether the case ran alone or in the whole
  // binary.
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
