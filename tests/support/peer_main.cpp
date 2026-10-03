// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// cyclone-test-peer: helper process for the spawn-based reset-gate tests
// (tests/integration/test_reset_gate_spawn.cpp).
//
// A SEPARATE image, deliberately NOT a re-exec of cyclone-tests:
// Catch2WithMain owns that binary's main(), catch_discover_tests runs
// `--list-tests` at BUILD time (so an argv-sniffing main can hang the build),
// and AppVerifier IFEO configuration on the Windows CI lane is keyed on the
// image name.
//
// Protocol (line-based, over the anonymous stdin/stdout pipes the parent
// spawned us with -- pipes give bidirectional death detection for free and
// take no lock on the volume, so the handshake cannot perturb the test):
//   child -> parent:  "READY\n"        the requested state is established
//                     "ERR <code>\n"   it is not; <code> is the CacheError
//                                      (open mode) or OS error (hold mode)
//   parent -> child:  "EXIT\n" or EOF  release everything and exit 0
// A parent crash closes our stdin, the blocking read returns EOF and we exit:
// no orphaned peers on a persistent CI runner.
//
// Modes:
//   open <raw-path> <size-bytes>  open a Cache via the public C++ API -- the
//                                 full live-peer shape: volume mapped, shared
//                                 lifetime lock held for the process lifetime
//   hold <volume-file-path>       take ONLY the shared lifetime byte-range
//                                 lock on an existing volume FILE (no Cache,
//                                 no mapping).  The W2 stand-in peer; the
//                                 test explains why W2 cannot use a fully
//                                 mapped peer.  The path here is the actual
//                                 on-disk (fingerprinted) file, not the raw
//                                 add_volume path.
//   seam <raw-path> <size-bytes> <seam> <crash|hang> <content-bytes> [wrap]
//                                 open a Cache (multi-process, leases off)
//                                 and write <content-bytes> documents until
//                                 the writer reaches Volume::WriterSeam
//                                 number <seam> for the first time; there it
//                                 either _exit(kSeamCrashExit)s (a writer
//                                 killed inside its wrap window, holding the
//                                 cross-process write lock) or says READY and
//                                 blocks until released, then _exit()s
//                                 WITHOUT leaving the window (a live holder
//                                 stalled inside it).  Exit 1 if the seam was
//                                 never reached.  With `wrap`, only the
//                                 write that WRAPS arms the seam (not an
//                                 earlier frontier advance).
//   carry <raw-path> <size-bytes> <step> <crash|hang> <key> <content-bytes>
//                                 open a Cache (multi-process, leases off,
//                                 wrap retention ON) and write ONE AVIF
//                                 alternate of <content-bytes> 0x5A bytes to
//                                 <key>, whose chain head the parent made
//                                 RETAINED, so the write carries the chain
//                                 forward.  At <step> it _exit(42)s (crash)
//                                 or says READY and blocks until released,
//                                 then _exit(0)s (hang).  Steps, in the
//                                 order the write reaches them:
//                                   copied  Volume::CarrySeam::kSourcesCopied
//                                   intent  WriterSeam::kAfterIntentSet
//                                   gate    WriterSeam::kAfterGatePassed
//                                   epoch   WriterSeam::kAfterEpochStore
//                                           (the three writer seams fire in
//                                           the frontier advance the carry's
//                                           allocation needs)
//                                   tear    the F6 reservation->pwrite gate
//                                   filled  Volume::CarrySeam::kFilled
//                                 Exit 1 if the step was never reached.
//   borrow <raw-path> <size-bytes> <key>...
//                                 open a Cache (multi-process, 600 s lease),
//                                 take a disk borrow of every key, say READY
//                                 and HOLD the handles until released or
//                                 killed (a zero-copy reader; SIGKILL leaks
//                                 its borrow counts).
//   exitopen <dir> <size-bytes> <shape> <return|exit>
//                                 open one or two caches under <dir> (see
//                                 run_exitopen for the shapes), work them,
//                                 say READY, and on release leave the
//                                 process WITHOUT stopping or destroying
//                                 them: background threads alive through
//                                 static destruction (issue
//                                 oschaaf/modpagespeed-2#1761).  Exit 0 is
//                                 the contract; a sanitizer report, signal
//                                 or hang is the finding.

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "core/volume.hpp"
#include "cyclone/cache.hpp"
#include "cyclone/config.hpp"
#include "cyclone/cyclone_c.h"
#include "cyclone/key.hpp"
#include "cyclone/plugin/optimization.hpp"
#include "optimization/optimization_engine.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX  // windows.h min/max macros would break std::min/std::max
#endif
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

// Sanitizer hooks for the exitopen mode (see run_exitopen).  A process that
// exits with its Cache threads alive has, by construction, threads that are
// never joined; TSan would report every one as a "thread leak" and exit 66,
// masking the real question (does any thread race a destroyed static).  The
// hook only sets a default: a TSAN_OPTIONS in the environment still wins.
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
extern "C" const char* __tsan_default_options() {
  return "report_thread_leaks=0";
}
#endif
#if __has_feature(address_sanitizer)
#include <sanitizer/lsan_interface.h>
#define CYCLONE_PEER_HAS_LSAN 1
#endif
#endif
#if defined(__SANITIZE_THREAD__) && !defined(__has_feature)
extern "C" const char* __tsan_default_options() {
  return "report_thread_leaks=0";
}
#endif
#if defined(__SANITIZE_ADDRESS__) && !defined(_MSC_VER) && \
    !defined(CYCLONE_PEER_HAS_LSAN)
#include <sanitizer/lsan_interface.h>  // GCC ASan; MSVC ASan has no LSan
#define CYCLONE_PEER_HAS_LSAN 1
#endif
#ifndef CYCLONE_PEER_HAS_LSAN
#define CYCLONE_PEER_HAS_LSAN 0
#endif

namespace {

// Mirrors kLifetimeLockByte in src/core/volume.cpp: the past-EOF byte every
// open volume holds a SHARED lock on for its whole life.  Same replication
// precedent as the raw holder child in test_stabilization.cpp ("a refused
// open under a live peer writes nothing to disk").
constexpr unsigned long long kLifetimeLockByte = 0x7FFFFFFFFFFFFFFDULL;

void say(const std::string& line) {
  std::fputs(line.c_str(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);  // stdout is a pipe (fully buffered): flush or the
                        // parent's wait_ready deadline expires
}

// Block until the parent says EXIT or dies (EOF on stdin).  Any line releases
// us; the pipe closing is the load-bearing orphan-prevention signal.
void wait_for_release() {
  char buf[64];
  while (std::fgets(buf, sizeof buf, stdin) != nullptr) {
    if (std::strncmp(buf, "EXIT", 4) == 0) {
      return;
    }
  }
}

int run_open(const char* path, unsigned long long size) {
  cyclone::CacheConfig config;
  config.set_multi_process(0, 1);
  config.set_ram_cache_size(0);

  auto cache = cyclone::Cache::create(config);
  if (!cache.has_value()) {
    say("ERR " + std::to_string(static_cast<int>(cache.error())));
    return 1;
  }
  if (auto added = (*cache)->add_volume(path, static_cast<size_t>(size));
      !added.has_value()) {
    say("ERR " + std::to_string(static_cast<int>(added.error())));
    return 1;
  }
  if (auto started = (*cache)->start(); !started.has_value()) {
    say("ERR " + std::to_string(static_cast<int>(started.error())));
    return 1;
  }

  say("READY");
  wait_for_release();

  (*cache)->stop();
  // The Cache destructor closes the volume, which is the ONLY thing that
  // releases the lifetime lock (never an explicit unlock; see volume.cpp).
  return 0;
}

int run_hold(const char* file_path) {
#ifdef _WIN32
  const int fd = _open(file_path, _O_RDWR | _O_BINARY);
  if (fd < 0) {
    say("ERR " + std::to_string(errno));
    return 1;
  }
  HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
  OVERLAPPED ov{};
  ov.Offset = static_cast<DWORD>(kLifetimeLockByte & 0xFFFFFFFFULL);
  ov.OffsetHigh = static_cast<DWORD>(kLifetimeLockByte >> 32);
  // SHARED (no LOCKFILE_EXCLUSIVE_LOCK) + FAIL_IMMEDIATELY: exactly the
  // acquisition an open volume performs for its lifetime lock.
  if (!LockFileEx(handle, LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov)) {
    say("ERR " + std::to_string(GetLastError()));
    _close(fd);
    return 1;
  }
  say("READY");
  wait_for_release();
  _close(fd);  // releases the byte-range lock
#else
  const int fd = ::open(file_path, O_RDWR);
  if (fd < 0) {
    say("ERR " + std::to_string(errno));
    return 1;
  }
  struct flock fl{};
  fl.l_type = F_RDLCK;
  fl.l_whence = SEEK_SET;
  fl.l_start = static_cast<off_t>(kLifetimeLockByte);
  fl.l_len = 1;
  if (::fcntl(fd, F_OFD_SETLK, &fl) != 0) {
    say("ERR " + std::to_string(errno));
    ::close(fd);
    return 1;
  }
  say("READY");
  wait_for_release();
  ::close(fd);  // last close of the OFD releases the lock
#endif
  return 0;
}

// Distinct exit code for "died at the seam" so the parent can tell a seam
// crash from any other failure.
constexpr int kSeamCrashExit = 42;

int run_seam(const char* path, unsigned long long size, int seam, bool crash,
             unsigned long long content_bytes, bool wrap_only) {
  cyclone::CacheConfig config;
  config.set_multi_process(0, 1);
  config.set_ram_cache_size(0);
  config.read_lease_duration = std::chrono::milliseconds(0);
  config.lease_wrap_ceiling = std::chrono::milliseconds(0);

  auto cache = cyclone::Cache::create(config);
  if (!cache.has_value()) {
    say("ERR " + std::to_string(static_cast<int>(cache.error())));
    return 1;
  }
  if (auto added = (*cache)->add_volume(path, static_cast<size_t>(size));
      !added.has_value()) {
    say("ERR " + std::to_string(static_cast<int>(added.error())));
    return 1;
  }
  if (auto started = (*cache)->start(); !started.has_value()) {
    say("ERR " + std::to_string(static_cast<int>(started.error())));
    return 1;
  }

  const auto target = static_cast<cyclone::Volume::WriterSeam>(seam);
  // wrap_only: fire only inside the write that WRAPS (armed just before it),
  // not at an earlier frontier advance.
  static std::atomic<bool> armed{false};
  armed.store(!wrap_only);
  cyclone::Volume::s_writer_seam_for_test =
      [target, crash](cyclone::Volume::WriterSeam at) {
        if (at != target || !armed.load()) {
          return;
        }
        if (crash) {
          std::_Exit(kSeamCrashExit);  // no teardown: lock + intent leak
        }
        say("READY");
        wait_for_release();
        std::_Exit(0);  // never leaves the window
      };

  std::vector<std::byte> content(static_cast<size_t>(content_bytes),
                                 std::byte{0x5A});
  const size_t max_writes = static_cast<size_t>(size / content_bytes) * 4 + 8;
  // On-disk size of one document (132-byte header, 8-byte padding).
  const uint64_t doc_bytes = (content_bytes + 132 + 7) & ~uint64_t{7};
  for (size_t i = 0; i < max_writes; ++i) {
    if (wrap_only) {
      // Single stripe: current_bytes is the cursor's stripe-relative offset.
      const auto st = (*cache)->stats();
      armed.store(st.current_bytes + doc_bytes > st.stripe_bytes);
    }
    const cyclone::CacheKey key("seam-peer-" + std::to_string(i));
    auto wh = (*cache)->write_sync(key, content.size());
    if (!wh.has_value()) {
      continue;
    }
    (void)wh->write_sync(content);
    (void)wh->close_sync();
  }
  cyclone::Volume::s_writer_seam_for_test = nullptr;
  say("ERR seam never reached");
  return 1;
}

int run_carry(const char* path, unsigned long long size, const char* step,
              bool crash, const char* key, unsigned long long content_bytes) {
  cyclone::CacheConfig config;
  config.set_multi_process(0, 1);
  config.set_ram_cache_size(0);
  config.set_wrap_retention(true);
  config.read_lease_duration = std::chrono::milliseconds(0);
  config.lease_wrap_ceiling = std::chrono::milliseconds(0);

  auto cache = cyclone::Cache::create(config);
  if (!cache.has_value()) {
    say("ERR " + std::to_string(static_cast<int>(cache.error())));
    return 1;
  }
  if (auto added = (*cache)->add_volume(path, static_cast<size_t>(size));
      !added.has_value()) {
    say("ERR " + std::to_string(static_cast<int>(added.error())));
    return 1;
  }
  if (auto started = (*cache)->start(); !started.has_value()) {
    say("ERR " + std::to_string(static_cast<int>(started.error())));
    return 1;
  }

  static bool s_crash = false;
  s_crash = crash;
  auto at_step = [] {
    if (s_crash) {
      std::_Exit(kSeamCrashExit);  // no teardown: whatever is held leaks
    }
    say("READY");
    wait_for_release();
    std::_Exit(0);  // never finishes the write
  };
  using cyclone::Volume;
  const std::string target(step);
  if (target == "copied" || target == "filled") {
    const auto want = target == "copied" ? Volume::CarrySeam::kSourcesCopied
                                         : Volume::CarrySeam::kFilled;
    Volume::s_carry_seam_for_test = [want, at_step](Volume::CarrySeam at) {
      if (at == want) {
        at_step();
      }
    };
  } else if (target == "tear") {
    Volume::s_write_tear_gate_for_test = [at_step](uint64_t, uint64_t) {
      at_step();
    };
  } else {
    Volume::WriterSeam want = Volume::WriterSeam::kAfterIntentSet;
    if (target == "gate") {
      want = Volume::WriterSeam::kAfterGatePassed;
    } else if (target == "epoch") {
      want = Volume::WriterSeam::kAfterEpochStore;
    } else if (target != "intent") {
      say("ERR unknown step " + target);
      return 2;
    }
    Volume::s_writer_seam_for_test = [want, at_step](Volume::WriterSeam at) {
      if (at == want) {
        at_step();
      }
    };
  }

  std::vector<std::byte> content(static_cast<size_t>(content_bytes),
                                 std::byte{0x5A});
  auto wh = (*cache)->write_alternate_sync(
      cyclone::CacheKey(key), cyclone::AlternateId::AVIF, content.size());
  if (wh.has_value()) {
    [[maybe_unused]] const auto wrote = wh->write_sync(content);
    [[maybe_unused]] const auto closed = wh->close_sync();
  }
  Volume::s_carry_seam_for_test = nullptr;
  Volume::s_write_tear_gate_for_test = nullptr;
  Volume::s_writer_seam_for_test = nullptr;
  say("ERR step never reached");
  return 1;
}

// Open a Cache with a long read lease, take a disk borrow of every listed
// key and HOLD the handles: the stand-in for a zero-copy reader.  The
// parent SIGKILLs it to leak the counts (test 7) or releases it.
int run_borrow(const char* path, unsigned long long size, int nkeys,
               char** keys) {
  cyclone::CacheConfig config;
  config.set_multi_process(0, 1);
  config.set_ram_cache_size(0);
  config.read_lease_duration = std::chrono::milliseconds(600000);
  config.lease_wrap_ceiling = std::chrono::milliseconds(600000);

  auto cache = cyclone::Cache::create(config);
  if (!cache.has_value()) {
    say("ERR " + std::to_string(static_cast<int>(cache.error())));
    return 1;
  }
  if (auto added = (*cache)->add_volume(path, static_cast<size_t>(size));
      !added.has_value()) {
    say("ERR " + std::to_string(static_cast<int>(added.error())));
    return 1;
  }
  if (auto started = (*cache)->start(); !started.has_value()) {
    say("ERR " + std::to_string(static_cast<int>(started.error())));
    return 1;
  }
  std::vector<cyclone::ReadHandle> held;
  for (int i = 0; i < nkeys; ++i) {
    auto rh = (*cache)->read_sync(cyclone::CacheKey(keys[i]));
    if (!rh.has_value()) {
      say(std::string("ERR miss ") + keys[i]);
      return 1;
    }
    held.push_back(std::move(*rh));
  }
  say("READY");
  wait_for_release();
  return 0;
}

// ---------------------------------------------------------------------------
// exitopen: leave the process with Cache objects still OPEN.
//
// The host shape behind PageSpeed 2.x issue oschaaf/modpagespeed-2#1761: a
// .NET host that never stop()s / destroys the caches its ps_cache_open
// created, and exits while their DirectorySyncer, HitTracker and
// OptimizationEngine threads are alive.  Everything below is heap-allocated
// and deliberately leaked; the only teardown that runs is the process's own
// (static destructors, atexit, then the OS reaping the threads).
// ---------------------------------------------------------------------------

// A plugin that turns every original write into one alternate write, so the
// optimization workers are writing into the cache while the process exits.
class EchoOptimizationPlugin : public cyclone::OptimizationPlugin {
 public:
  [[nodiscard]] cyclone::PluginInfo info() const override {
    return {"exitopen-echo", "1.0.0", 7161};
  }
  cyclone::OptimizationPlan plan_optimization(
      const cyclone::CacheKey& /*key*/, std::span<const std::byte> /*header*/,
      uint64_t content_length, cyclone::AlternateId written_alternate,
      uint32_t /*hit_count*/) override {
    cyclone::OptimizationPlan plan;
    if (written_alternate == cyclone::AlternateId::Original) {
      plan.add(cyclone::AlternateId::Gzip, 10, true, content_length);
    }
    return plan;
  }
  std::expected<cyclone::TransformResult, cyclone::CacheError> transform(
      cyclone::AlternateId target,
      const cyclone::OptimizationContext& ctx) override {
    if (ctx.is_cancelled()) {
      return std::unexpected(cyclone::CacheError::OptimizationCancelled);
    }
    cyclone::TransformResult result;
    result.alternate_id = target;
    result.header.assign(ctx.source_header().begin(),
                         ctx.source_header().end());
    result.content.assign(ctx.source_content().begin(),
                          ctx.source_content().end());
    return result;
  }
};

// Shared load state for the exitopen writer threads.  Heap-allocated and
// leaked like the caches, so a thread still running at exit never touches a
// destroyed object of ours (the subject under test is Cyclone's teardown,
// not this file's).
struct ExitOpenLoad {
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> writes{0};
  std::vector<std::thread> threads;
};

constexpr size_t kExitOpenDocBytes = 2048;

void exit_open_write_loop(CycloneCacheHandle* c, int lane, ExitOpenLoad* load) {
  const std::string payload(kExitOpenDocBytes, static_cast<char>('A' + lane));
  for (uint64_t i = 0; !load->stop.load(std::memory_order_relaxed); ++i) {
    const std::string key =
        "exitopen-" + std::to_string(lane) + "-" + std::to_string(i % 512);
    (void)cyclone_cache_write(c, key.data(), key.size(), payload.data(),
                              payload.size());
    CycloneReadHandle* rh = nullptr;
    if (cyclone_cache_read(c, key.data(), key.size(), &rh) == CYCLONE_OK) {
      cyclone_cache_read_close(rh);
    }
    load->writes.fetch_add(1, std::memory_order_relaxed);
  }
}

CycloneCacheHandle* exit_open_capi(const std::string& path,
                                   unsigned long long size) {
  CycloneCacheConfig cfg{};
  cfg.cache_path = path.c_str();
  cfg.cache_size_bytes = size;
  cfg.ram_cache_size_bytes = 16ULL * 1024 * 1024;
  cfg.enable_checksum = 1;
  cfg.num_segments = 4;
  cfg.enable_mmap_directory = 1;  // PageSpeed's cross-process shape
  CycloneCacheHandle* h = nullptr;
  if (cyclone_cache_create(&cfg, &h) != CYCLONE_OK) {
    return nullptr;
  }
  return h;
}

// Modes (see main):
//   capi1     one C-API cache, a burst of writes + reads, then exit
//   capi2     cache A under write load; cache B (its own path) opened and
//             loaded while A is being written; load threads joined; exit
//   capi2same like capi2 but B opens the SAME volume file as A
//   capi2live like capi2 but the load threads are still writing at exit
//   cppbusy   C++ API, every background thread made busy: 1 ms directory
//             sync, 1 ms hit flush, optimization engine with two workers
//             and the echo plugin writing alternates; load threads joined
//   lockwait  two C++ caches on ONE volume file; cache A's writer is parked
//             inside the tear gate holding the cross-process write lock,
//             cache B's writer waits on that lock and, past 50 ms, probes
//             the holder's liveness through the process-wide liveness
//             mutex after every sleep.  An atexit handler registered
//             BEFORE the caches sleeps 500 ms, so static destructors
//             (registered later, so run earlier) have run while B is still
//             probing: a host whose own exit handlers take a while.  With a
//             destructible liveness mutex this aborted on Apple's libc++
//             (EINVAL -> std::system_error -> std::terminate); the mutex is
//             immortal now.
// Exit style: "return" returns from main (static destructors + atexit run
// with the Cache threads alive, then the OS reaps them); "exit" calls
// std::exit(0) from inside this function so main's locals never unwind.
int run_exitopen(const char* raw_path, unsigned long long size,
                 const std::string& shape, const std::string& how) {
#if CYCLONE_PEER_HAS_LSAN
  // Leaking the caches is the premise of this mode, not a finding: keep
  // LeakSanitizer from turning the deliberate leak into a non-zero exit.
  // Everything a background thread allocates hangs off these blocks, which
  // LSan scans as roots once they are ignored.
  __lsan_disable();
#endif
  const std::string path_a = std::string(raw_path) + "/a/cyclone.dat";
  const std::string path_b = std::string(raw_path) + "/b/cyclone.dat";
  std::error_code ec;
  std::filesystem::create_directories(std::string(raw_path) + "/a", ec);
  std::filesystem::create_directories(std::string(raw_path) + "/b", ec);
  auto* load = new ExitOpenLoad();

  if (shape == "lockwait") {
    // Registered first, so it runs LAST at exit: everything constructed
    // after it (the liveness mutex is constructed at the first attach) is
    // destroyed before it sleeps.
    std::atexit(
        [] { std::this_thread::sleep_for(std::chrono::milliseconds(500)); });
    auto open_same = [&]() -> cyclone::Cache* {
      cyclone::CacheConfig config;
      config.set_multi_process(0, 1);
      config.set_ram_cache_size(0);
      auto created = cyclone::Cache::create(config);
      if (!created.has_value()) {
        return nullptr;
      }
      cyclone::Cache* cache = created->release();  // never destroyed
      if (!cache->add_volume(path_a, static_cast<size_t>(size)).has_value() ||
          !cache->start().has_value()) {
        return nullptr;
      }
      return cache;
    };
    cyclone::Cache* a = open_same();
    cyclone::Cache* b = open_same();
    if (a == nullptr || b == nullptr) {
      say("ERR open");
      return 1;
    }
    // The first writer to reach the tear gate (A's) parks there for the
    // rest of the process, holding the cross-process write lock.
    static std::atomic<bool> parked{false};
    cyclone::Volume::s_write_tear_gate_for_test = [](uint64_t, uint64_t) {
      if (!parked.exchange(true)) {
        for (;;) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
      }
    };
    const std::vector<std::byte> content(kExitOpenDocBytes, std::byte{0x5A});
    load->threads.emplace_back([a, &content] {
      if (auto wh =
              a->write_sync(cyclone::CacheKey("lockwait-a"), content.size());
          wh.has_value()) {
        [[maybe_unused]] const auto wrote = wh->write_sync(content);
        [[maybe_unused]] const auto closed = wh->close_sync();
      }
    });
    while (!parked.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    load->threads.emplace_back([b, load, &content] {
      for (uint64_t i = 0; !load->stop.load(std::memory_order_relaxed); ++i) {
        if (auto wh = b->write_sync(
                cyclone::CacheKey("lockwait-b-" + std::to_string(i)),
                content.size());
            wh.has_value()) {
          [[maybe_unused]] const auto wrote = wh->write_sync(content);
          [[maybe_unused]] const auto closed = wh->close_sync();
        }
        load->writes.fetch_add(1, std::memory_order_relaxed);
      }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    std::fprintf(stderr, "exitopen: lockwait b_writes=%llu\n",
                 static_cast<unsigned long long>(load->writes.load()));
    say("READY");
    wait_for_release();
    for (auto& t : load->threads) {
      t.detach();  // A is parked for good; B keeps waiting on the lock
    }
    if (how == "exit") {
      std::exit(0);
    }
    return 0;
  }

  if (shape == "cppbusy") {
    cyclone::CacheConfig config;
    config.set_multi_process(0, 1);
    config.set_ram_cache_size(16ULL * 1024 * 1024);
    config.set_directory_sync_interval(std::chrono::milliseconds(1));
    config.hit_flush_interval = std::chrono::milliseconds(1);
    config.optimization_config.set_enabled(true).set_min_threads(2);
    config.optimization_config.min_hits_before_optimize = 0;
    config.optimization_config.scale_check_interval =
        std::chrono::milliseconds(1);
    auto created = cyclone::Cache::create(config);
    if (!created.has_value()) {
      say("ERR " + std::to_string(static_cast<int>(created.error())));
      return 1;
    }
    cyclone::Cache* cache = created->release();  // never destroyed
    if (auto added = cache->add_volume(path_a, static_cast<size_t>(size));
        !added.has_value()) {
      say("ERR " + std::to_string(static_cast<int>(added.error())));
      return 1;
    }
    if (auto started = cache->start(); !started.has_value()) {
      say("ERR " + std::to_string(static_cast<int>(started.error())));
      return 1;
    }
    cache->optimization_engine()->register_plugin(
        std::make_shared<EchoOptimizationPlugin>());
    const std::vector<std::byte> content(kExitOpenDocBytes, std::byte{0x5A});
    for (int lane = 0; lane < 2; ++lane) {
      load->threads.emplace_back([cache, lane, load, &content] {
        for (uint64_t i = 0; !load->stop.load(std::memory_order_relaxed); ++i) {
          cyclone::CacheKey k("exitopen-" + std::to_string(lane) + "-" +
                              std::to_string(i % 512));
          if (auto wh = cache->write_sync(k, content.size()); wh.has_value()) {
            [[maybe_unused]] const auto wrote = wh->write_sync(content);
            [[maybe_unused]] const auto closed = wh->close_sync();
          }
          // The write path does not feed the engine itself (see audit in
          // test_exit_with_open_cache.cpp); hand it every write so its
          // workers are writing alternates while the process exits.
          cache->optimization_engine()->on_write_complete(
              k, {}, content.size(), cyclone::AlternateId::Original, 5);
          if (auto rh = cache->read_sync(k); rh.has_value()) {
            (void)rh->content().size();
          }
          load->writes.fetch_add(1, std::memory_order_relaxed);
        }
      });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const auto st = cache->stats();
    std::fprintf(stderr,
                 "exitopen: cppbusy writes=%llu entries=%llu dir_syncs=%llu "
                 "opt_tasks=%llu\n",
                 static_cast<unsigned long long>(load->writes.load()),
                 static_cast<unsigned long long>(st.current_entries),
                 static_cast<unsigned long long>(st.directory_syncs),
                 static_cast<unsigned long long>(
                     cache->optimization_engine()->stats().completed));
    say("READY");
    wait_for_release();
    load->stop.store(true);
    for (auto& t : load->threads) {
      t.join();
    }
    // Leave the workers something to do at the very end.
    for (int i = 0; i < 64; ++i) {
      cyclone::CacheKey k("exitopen-tail-" + std::to_string(i));
      if (auto wh = cache->write_sync(k, content.size()); wh.has_value()) {
        [[maybe_unused]] const auto wrote = wh->write_sync(content);
        [[maybe_unused]] const auto closed = wh->close_sync();
      }
      cache->optimization_engine()->on_write_complete(
          k, {}, content.size(), cyclone::AlternateId::Original, 5);
    }
    if (how == "exit") {
      std::exit(0);
    }
    return 0;
  }

  CycloneCacheHandle* a = exit_open_capi(path_a, size);
  if (a == nullptr) {
    say("ERR create a");
    return 1;
  }
  if (shape == "capi1") {
    const std::string payload(kExitOpenDocBytes, 'x');
    for (int i = 0; i < 256; ++i) {
      const std::string key = "exitopen-" + std::to_string(i);
      (void)cyclone_cache_write(a, key.data(), key.size(), payload.data(),
                                payload.size());
      CycloneReadHandle* rh = nullptr;
      if (cyclone_cache_read(a, key.data(), key.size(), &rh) == CYCLONE_OK) {
        cyclone_cache_read_close(rh);
      }
    }
    say("READY");
    wait_for_release();
    if (how == "exit") {
      std::exit(0);
    }
    return 0;
  }

  // capi2 / capi2same / capi2live: A under load from two threads while B
  // opens.
  for (int lane = 0; lane < 2; ++lane) {
    load->threads.emplace_back(exit_open_write_loop, a, lane, load);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CycloneCacheHandle* b =
      exit_open_capi(shape == "capi2same" ? path_a : path_b, size);
  if (b == nullptr) {
    say("ERR create b");
    return 1;
  }
  for (int lane = 2; lane < 4; ++lane) {
    load->threads.emplace_back(exit_open_write_loop, b, lane, load);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  std::fprintf(stderr, "exitopen: %s writes=%llu\n", shape.c_str(),
               static_cast<unsigned long long>(load->writes.load()));
  say("READY");
  wait_for_release();
  if (shape == "capi2live") {
    for (auto& t : load->threads) {
      t.detach();  // still writing into A and B while the process exits
    }
  } else {
    load->stop.store(true);
    for (auto& t : load->threads) {
      t.join();
    }
  }
  if (how == "exit") {
    std::exit(0);
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 5 && std::strcmp(argv[1], "borrow") == 0) {
    return run_borrow(argv[2], std::strtoull(argv[3], nullptr, 10), argc - 4,
                      argv + 4);
  }
  if (argc >= 7 && std::strcmp(argv[1], "seam") == 0) {
    return run_seam(argv[2], std::strtoull(argv[3], nullptr, 10),
                    std::atoi(argv[4]), std::strcmp(argv[5], "crash") == 0,
                    std::strtoull(argv[6], nullptr, 10),
                    argc >= 8 && std::strcmp(argv[7], "wrap") == 0);
  }
  if (argc >= 8 && std::strcmp(argv[1], "carry") == 0) {
    return run_carry(argv[2], std::strtoull(argv[3], nullptr, 10), argv[4],
                     std::strcmp(argv[5], "crash") == 0, argv[6],
                     std::strtoull(argv[7], nullptr, 10));
  }
  if (argc >= 6 && std::strcmp(argv[1], "exitopen") == 0) {
    return run_exitopen(argv[2], std::strtoull(argv[3], nullptr, 10), argv[4],
                        argv[5]);
  }
  if (argc >= 4 && std::strcmp(argv[1], "open") == 0) {
    return run_open(argv[2], std::strtoull(argv[3], nullptr, 10));
  }
  if (argc >= 3 && std::strcmp(argv[1], "hold") == 0) {
    return run_hold(argv[2]);
  }
  say("ERR usage: cyclone-test-peer open <path> <size> | hold <file> | seam "
      "<path> <size> <seam> <crash|hang> <content-bytes> | carry <path> "
      "<size> <step> <crash|hang> <key> <content-bytes> | exitopen <dir> "
      "<size> <capi1|capi2|capi2same|capi2live|cppbusy|lockwait> "
      "<return|exit>");
  return 2;
}
