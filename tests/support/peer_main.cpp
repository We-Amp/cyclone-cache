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
//   seam <raw-path> <size-bytes> <seam> <crash|hang> <content-bytes>
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
//                                 never reached.
//   borrow <raw-path> <size-bytes> <key>...
//                                 open a Cache (multi-process, 600 s lease),
//                                 take a disk borrow of every key, say READY
//                                 and HOLD the handles until released or
//                                 killed (a zero-copy reader; SIGKILL leaks
//                                 its borrow counts).

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/volume.hpp"
#include "cyclone/cache.hpp"
#include "cyclone/config.hpp"
#include "cyclone/key.hpp"

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
             unsigned long long content_bytes) {
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
  cyclone::Volume::s_writer_seam_for_test =
      [target, crash](cyclone::Volume::WriterSeam at) {
        if (at != target) {
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
  for (size_t i = 0; i < max_writes; ++i) {
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

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 5 && std::strcmp(argv[1], "borrow") == 0) {
    return run_borrow(argv[2], std::strtoull(argv[3], nullptr, 10), argc - 4,
                      argv + 4);
  }
  if (argc >= 7 && std::strcmp(argv[1], "seam") == 0) {
    return run_seam(argv[2], std::strtoull(argv[3], nullptr, 10),
                    std::atoi(argv[4]), std::strcmp(argv[5], "crash") == 0,
                    std::strtoull(argv[6], nullptr, 10));
  }
  if (argc >= 4 && std::strcmp(argv[1], "open") == 0) {
    return run_open(argv[2], std::strtoull(argv[3], nullptr, 10));
  }
  if (argc >= 3 && std::strcmp(argv[1], "hold") == 0) {
    return run_hold(argv[2]);
  }
  say("ERR usage: cyclone-test-peer open <path> <size> | hold <file> | seam "
      "<path> <size> <seam> <crash|hang> <content-bytes>");
  return 2;
}
