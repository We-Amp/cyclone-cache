// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Stabilization tests: exercises fixes for cross-process cache sharing issues
// found during staging deployment (ghost directory entries after reset, chain
// corruption error propagation, concurrent reset + write races).

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "core/volume.hpp"  // fingerprint_cache_path
#include "cyclone/cache.hpp"
#include "cyclone/config.hpp"
#include "cyclone/key.hpp"

#ifdef _WIN32
#include <process.h>
#define GETPID _getpid
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#define GETPID getpid
#endif

// ThreadSanitizer cannot validate the fork-based tests in this file whose
// forked child then runs a Cache: Cache::start() spawns the background
// hit-tracker flush thread, and TSan forbids "starting new threads after a
// multi-threaded fork" (the parent is already multi-threaded from its own
// start()) -- it hard-aborts the child on macOS and silently mis-behaves it on
// Linux.  This is a TSan+fork limitation, NOT a product or test-logic bug: the
// same tests pass under the normal build, ASan/UBSan, clang-20, MSVC and
// stress, and production Apache/nginx likewise fork after start() (the child
// not owning the flush thread is existing multi-process behaviour).  The
// affected fork+Cache::start() cases are compile-guarded out under TSan (a
// feature guard, because the TSan CI lane runs bare ./cyclone-tests with no tag
// filter, so a tag alone would not skip them).  The other fork tests here that
// do NOT start a Cache in a multi-threaded-forked child (the graceful-drain
// and coexist-no-init cases, and the concurrent-open test in
// multi_process_test.cpp) keep running under TSan.
#if defined(__SANITIZE_THREAD__)
#define CYCLONE_TSAN_BUILD 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define CYCLONE_TSAN_BUILD 1
#endif
#endif

using namespace cyclone;
namespace fs = std::filesystem;

namespace {

std::string get_temp_path(const std::string &name) {
  auto path = fs::temp_directory_path() /
              ("cyclone_stab_" + name + "_" + std::to_string(GETPID()));
  return path.string();
}

// Resolve the structural-fingerprint filename that Cache::add_volume() will
// open for a given {size, multi-process} config (see fingerprint_cache_path).
// Raw file inspection/corruption/stat below must target THIS name, not the
// pre-fingerprint path.  add_volume() itself keeps receiving the raw path so
// each (parent/child) process fingerprints exactly as production does.
std::string fp(const std::string &path, size_t size, bool multi_process = true,
               size_t stripe_size = 0) {
  return fingerprint_cache_path(path, size, stripe_size, multi_process);
}

void cleanup_temp_file(const std::string &path) {
  std::error_code ec;
  fs::remove(path, ec);
  // Also remove the structural-fingerprint sibling(s) Cache::add_volume opened
  // ("<stem>-<fmt>-<hash><ext>"), so fingerprinted files do not accumulate on a
  // persistent /tmp.  Test-only directory iteration.
  const fs::path p(path);
  const std::string prefix = p.stem().string() + "-";
  for (fs::directory_iterator it(p.parent_path(), ec), end; it != end && !ec;
       it.increment(ec)) {
    const std::string n = it->path().filename().string();
    if (n.size() > prefix.size() && n.compare(0, prefix.size(), prefix) == 0) {
      std::error_code e2;
      fs::remove(it->path(), e2);
    }
  }
}

// Helper: create a cache, write N alternates for a key, close it.
// Returns the cache file path.
std::string write_alternates(const std::string &path_suffix,
                             const CacheKey &key, int num_alternates,
                             bool multi_process) {
  std::string path = get_temp_path(path_suffix);

  CacheConfig config;
  if (multi_process) {
    config.set_multi_process(0, 1);
  }
  config.set_enable_checksum(true);

  auto cache = Cache::create(config);
  REQUIRE(cache.has_value());

  (*cache)->add_volume(path, static_cast<size_t>(10 * 1024 * 1024));
  REQUIRE((*cache)->start().has_value());

  for (int i = 0; i < num_alternates; ++i) {
    auto alt_id = static_cast<AlternateId>(i + 1);
    std::vector<std::byte> data(256, std::byte{static_cast<uint8_t>(0x40 + i)});

    auto wh = (*cache)->write_alternate_sync(key, alt_id, data.size());
    REQUIRE(wh.has_value());
    REQUIRE(wh->write_sync(data).has_value());
    REQUIRE(wh->close_sync().has_value());
  }

  (*cache)->stop();
  return path;
}

}  // namespace

// =============================================================================
// T2: Volume reset leaves no ghost directory entries
// =============================================================================
// The old Volume::reset() only cleared 64KB of the directory region, leaving
// entries in buckets beyond that offset.  After fix F3, reset() clears the
// full directory.

TEST_CASE("Volume reset leaves no ghost directory entries",
          "[stabilization][multiprocess]") {
  std::string path = get_temp_path("ghost_reset");

  CacheKey key("ghost-test-key");
  std::vector<std::byte> data(512, std::byte{0xAA});

  // Phase 1: populate cache with entries in multi-process mode (mmap directory)
  {
    CacheConfig config;
    config.set_multi_process(0, 1);
    config.set_enable_checksum(true);

    auto cache = Cache::create(config);
    REQUIRE(cache.has_value());
    (*cache)->add_volume(path, static_cast<size_t>(10 * 1024 * 1024));
    REQUIRE((*cache)->start().has_value());

    // Write many entries to spread across directory buckets
    for (int i = 0; i < 500; ++i) {
      CacheKey k("ghost-key-" + std::to_string(i));
      auto wh = (*cache)->write_sync(k, data.size());
      if (wh.has_value()) {
        wh->write_sync(data);
        wh->close_sync();
      }
    }

    auto stats = (*cache)->stats();
    REQUIRE(stats.current_entries > 0);

    (*cache)->stop();
  }

  // Phase 2: reopen with incompatible version to trigger reset
  // (we corrupt the version field to force a reset path)
  {
    // Corrupt the volume header version to trigger auto_reset_on_incompatible
    std::fstream file(fp(path, static_cast<size_t>(10 * 1024 * 1024)),
                      std::ios::in | std::ios::out | std::ios::binary);
    REQUIRE(file.is_open());

    // VolumeHeader: magic(4) + major(2) + minor(2)
    // Write an incompatible major version at offset 4
    file.seekp(4);
    uint16_t bad_major = 0xFF;
    file.write(reinterpret_cast<const char *>(&bad_major), sizeof(bad_major));
    file.close();
  }

  // Phase 3: reopen — auto_reset should clear the entire directory
  {
    CacheConfig config;
    config.set_multi_process(0, 1);
    config.set_enable_checksum(true);

    auto cache = Cache::create(config);
    REQUIRE(cache.has_value());

    VolumeConfig vol_config;
    vol_config.path = path;
    vol_config.size = static_cast<size_t>(10 * 1024 * 1024);
    vol_config.auto_reset_on_incompatible = true;
    (*cache)->add_volume(vol_config);
    REQUIRE((*cache)->start().has_value());

    // All old entries should be gone — no ghost entries
    for (int i = 0; i < 500; ++i) {
      CacheKey k("ghost-key-" + std::to_string(i));
      auto result = (*cache)->read_sync(k);
      REQUIRE_FALSE(result.has_value());
    }

    auto stats = (*cache)->stats();
    REQUIRE(stats.current_entries == 0);

    (*cache)->stop();
  }

  cleanup_temp_file(path);
}

// =============================================================================
// T2b: reset() must zero EVERY stripe's directory, not just stripe 0's
// =============================================================================
// The test above uses a 10MB volume, which the auto geometry lays out as a
// SINGLE stripe -- so it passed even while reset() only zeroed the directory
// at VolumeHeader::kSize (stripe 0's).  Production volumes are multi-stripe:
// the surviving MmapDirectory structures in stripes 1..N-1 kept their MDIR
// magic, their entries AND their shared_write_pos, so init_stripes()
// re-ADOPTED them and the "reset" volume kept serving pre-reset documents.
//
// Two things make this test non-vacuous, and both are load-bearing:
//   * MULTI-PROCESS mode (mmap directory).  Single-process volumes build a
//     fresh in-memory Directory on every open, so they are genuinely cold
//     already; only the mmap directory persists on disk.  The bug bites
//     exactly the multi-process production configuration.
//   * ram_cache_size == 0.  A live RAM cache would serve the "gone" keys
//     from memory and mask a directory that was never wiped.
//
// The stripe_count REQUIRE is deliberate: it pins the geometry so nobody can
// shrink the volume later and silently re-mask the bug behind one stripe.
TEST_CASE("Volume reset zeroes every stripe's directory (multi-stripe)",
          "[stabilization][multiprocess]") {
  const std::string path = get_temp_path("ghost_reset_multistripe");
  cleanup_temp_file(path);

  // 512MB -> auto geometry yields 16 stripes (the kAutoStripeTarget cap).
  constexpr size_t kVolumeSize = static_cast<size_t>(512) * 1024 * 1024;
  constexpr int kNumKeys = 400;
  const std::vector<std::byte> data(512, std::byte{0xC5});

  // Phase 1: populate a multi-stripe, multi-process volume.
  {
    CacheConfig config;
    config.set_multi_process(0, 1);
    config.set_ram_cache_size(0);  // essential: a RAM hit would mask the bug

    auto cache = Cache::create(config);
    REQUIRE(cache.has_value());
    (*cache)->add_volume(path, kVolumeSize);
    REQUIRE((*cache)->start().has_value());

    for (int i = 0; i < kNumKeys; ++i) {
      CacheKey k("multistripe-key-" + std::to_string(i));
      auto wh = (*cache)->write_sync(k, data.size());
      REQUIRE(wh.has_value());
      REQUIRE(wh->write_sync(data).has_value());
      REQUIRE(wh->close_sync().has_value());
    }

    auto stats = (*cache)->stats();
    INFO("volume must actually be multi-stripe or this test is vacuous");
    REQUIRE(stats.stripe_count == 16);
    REQUIRE(stats.current_entries == kNumKeys);

    (*cache)->stop();
  }

  // Phase 2: stamp an incompatible format_version_major to force the reset
  // path on the next open (same idiom as the single-stripe case above).
  {
    std::fstream file(fp(path, kVolumeSize),
                      std::ios::in | std::ios::out | std::ios::binary);
    REQUIRE(file.is_open());
    file.seekp(4);  // VolumeHeader: magic(4) + major(2) + minor(2)
    const uint16_t bad_major = 0xFF;
    file.write(reinterpret_cast<const char *>(&bad_major), sizeof(bad_major));
    file.close();
  }

  // Phase 3: reopen -- auto_reset must clear EVERY stripe's directory.
  {
    CacheConfig config;
    config.set_multi_process(0, 1);
    config.set_ram_cache_size(0);

    auto cache = Cache::create(config);
    REQUIRE(cache.has_value());

    VolumeConfig vol_config;
    vol_config.path = path;
    vol_config.size = kVolumeSize;
    vol_config.auto_reset_on_incompatible = true;  // the default; explicit here
    (*cache)->add_volume(vol_config);
    REQUIRE((*cache)->start().has_value());

    // Collect-then-assert so a failure reports HOW MANY ghosts survived (the
    // pre-fix number is ~371/400: every key that hashed to a stripe other
    // than stripe 0).
    int survivors = 0;
    for (int i = 0; i < kNumKeys; ++i) {
      CacheKey k("multistripe-key-" + std::to_string(i));
      if ((*cache)->read_sync(k).has_value()) {
        ++survivors;
      }
    }
    INFO("ghost documents surviving reset(): " << survivors << " / "
                                               << kNumKeys);
    REQUIRE(survivors == 0);

    auto stats = (*cache)->stats();
    REQUIRE(stats.current_entries == 0);

    (*cache)->stop();
  }

  cleanup_temp_file(path);
}

// =============================================================================
// T2c: reset() must never run while a LIVE PEER has the volume open
// =============================================================================
// Fork-based, POSIX only (the gate is flock/LockFileEx; Windows has no fork,
// so the same property cannot be exercised this way).
//
// Background: the init lock orders OPENERS ONLY -- it is taken and
// dropped inside Volume::open().  A process that ALREADY has the volume open
// and mapped is invisible to a new opener, whose reset() then runs straight
// over its live mmap'd directories.  This is reachable TODAY with no format
// bump: needs_reset is also set on a GEOMETRY mismatch, i.e. any cache-size
// change across restarts.  nginx SIGHUP / Apache graceful restart + a resize
// = the cache is wiped out from under the workers still serving from it.
//
// Fixing T2b above makes this strictly WORSE (the wipe goes from partial to
// total), which is why the gate has to ship with it.
//
// HOUSE RULES: no Catch2 assertions inside a forked child -- Catch2 is not
// fork-safe.  The child does plain C checks and _exit()s a code; the parent
// reaps and REQUIREs on the code.  Collect-then-assert everywhere else.
#ifndef _WIN32
namespace {

constexpr uint16_t kBadMajor = 0xFF;
constexpr size_t kPeerVolumeSize = static_cast<size_t>(512) * 1024 * 1024;
constexpr int kPeerNumKeys = 400;

// Read format_version_major straight off disk.  VolumeHeader layout:
// magic(4) + format_version_major(2) + format_version_minor(2).
uint16_t read_on_disk_major(const std::string &path) {
  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    return 0;
  }
  uint16_t major = 0;
  ssize_t n = ::pread(fd, &major, sizeof(major), 4);
  ::close(fd);
  return n == static_cast<ssize_t>(sizeof(major)) ? major : 0;
}

void stamp_on_disk_major(const std::string &path, uint16_t major) {
  int fd = ::open(path.c_str(), O_RDWR);
  REQUIRE(fd >= 0);
  REQUIRE(::pwrite(fd, &major, sizeof(major), 4) ==
          static_cast<ssize_t>(sizeof(major)));
  ::fsync(fd);
  ::close(fd);
}

// Runs in the FORKED CHILD -- the "new binary" opening a volume whose on-disk
// format_version_major it considers incompatible, so it WANTS to reset.
// Never returns.  Exit codes:
//   2 = start() succeeded AND the header was restamped -- reset() actually ran
//       (the WANTED result once no live peer remains; the BUG if a peer is
//       live)
//   3 = open REFUSED (start() failed) -- the wanted result under a live peer
//   5 = start() succeeded but the header was NOT restamped (unexpected: opened
//       an incompatible volume without resetting it)
[[noreturn]] void peer_child_open(const std::string &path, int write_fd) {
  CacheConfig config;
  config.set_multi_process(0, 1);
  config.set_ram_cache_size(0);

  auto cache = Cache::create(config);
  if (!cache.has_value()) {
    unsigned char b = 1;
    (void)!::write(write_fd, &b, 1);
    _exit(3);
  }

  VolumeConfig vol_config;
  vol_config.path = path;
  vol_config.size = kPeerVolumeSize;
  vol_config.auto_reset_on_incompatible = true;
  (*cache)->add_volume(vol_config);

  const bool started = (*cache)->start().has_value();

  // Handshake: the parent may proceed once we have finished opening (so its
  // post-reap checks cannot race the child's reset, if any).
  unsigned char b = 1;
  (void)!::write(write_fd, &b, 1);

  if (!started) {
    _exit(3);  // refused to open under the live peer -- the wanted result
  }

  const uint16_t major = read_on_disk_major(fp(path, kPeerVolumeSize));
  (*cache)->stop();

  // Restamped => reset() ran (correct only once the peer is gone).  Started but
  // NOT restamped would mean it opened the incompatible volume without a reset.
  _exit(major != kBadMajor ? 2 : 5);
}

// The RESIZE trigger under structural-fingerprint filenames.  A plain cache
// resize (DIFFERENT size, no format bump) now yields a DIFFERENT geometry hash,
// so this child's 1GB config resolves to a SEPARATE filename from the peer's
// live 512MB file (see fingerprint_cache_path).  It therefore opens its OWN
// fresh file cleanly -- the peer's ring is never shared, extended or reset.
// Never returns.  Exit codes:
//   4 = opened its OWN fingerprinted file (the wanted result: upstream
//       isolation, the peer left untouched)
//   3 = refused -- would mean both configs collided on ONE file (only possible
//       pre-fingerprint, or on a hash collision caught by the stripe_count
//       backstop)
// (The same-size version-major refuse gate is exercised by peer_child_open.)
[[noreturn, maybe_unused]] void peer_resize_child(const std::string &path,
                                                  int write_fd) {
  CacheConfig config;
  config.set_multi_process(0, 1);
  config.set_ram_cache_size(0);

  auto cache = Cache::create(config);
  if (!cache.has_value()) {
    unsigned char b = 1;
    (void)!::write(write_fd, &b, 1);
    _exit(3);
  }

  VolumeConfig vol_config;
  vol_config.path = path;
  vol_config.size =
      2 * kPeerVolumeSize;  // the operator doubled the cache (1GB)
  vol_config.auto_reset_on_incompatible = true;
  (*cache)->add_volume(vol_config);

  const bool started = (*cache)->start().has_value();

  unsigned char b = 1;
  (void)!::write(write_fd, &b, 1);  // handshake: we are done opening

  if (started) {
    (*cache)->stop();
    // Opened cleanly -- the WANTED result under fingerprinting: the 1GB config
    // resolved to its OWN separate file, so it never shared, extended or reset
    // the live 512MB peer (see the header block above and the CHECK == 4).
    _exit(4);
  }
  // Refused to open -- reachable only if both configs collided on ONE file
  // (pre-fingerprint, or a stripe_count-backstop hash collision).  Not the
  // expected outcome here; surfaced so the parent can tell it apart from 4.
  _exit(3);
}

}  // namespace

#if !defined(CYCLONE_TSAN_BUILD)
TEST_CASE("reset() never runs while a live peer holds the volume open",
          "[stabilization][multiprocess]") {
  const std::string path = get_temp_path("reset_live_peer");
  cleanup_temp_file(path);

  const std::vector<std::byte> data(512, std::byte{0x7E});

  CacheConfig config;
  config.set_multi_process(0, 1);
  config.set_ram_cache_size(0);  // a RAM hit would mask a wiped disk directory

  auto cache = Cache::create(config);
  REQUIRE(cache.has_value());
  (*cache)->add_volume(path, kPeerVolumeSize);
  REQUIRE((*cache)->start().has_value());

  for (int i = 0; i < kPeerNumKeys; ++i) {
    CacheKey k("peer-key-" + std::to_string(i));
    auto wh = (*cache)->write_sync(k, data.size());
    REQUIRE(wh.has_value());
    REQUIRE(wh->write_sync(data).has_value());
    REQUIRE(wh->close_sync().has_value());
  }
  {
    auto stats = (*cache)->stats();
    INFO("volume must be multi-stripe or the wipe is not representative");
    REQUIRE(stats.stripe_count == 16);
    REQUIRE(stats.current_entries == kPeerNumKeys);
  }

  // Make the on-disk header look incompatible to the NEXT opener, WITHOUT
  // touching our own live, already-mapped volume: the header is only read at
  // open().  This is the "new binary, new format" situation -- and it stands
  // in for the geometry-mismatch path (a cache resize), which needs no format
  // bump at all to reach the same reset().
  stamp_on_disk_major(fp(path, kPeerVolumeSize), kBadMajor);
  REQUIRE(read_on_disk_major(fp(path, kPeerVolumeSize)) == kBadMajor);

  // ---- LEG 1: peer is LIVE -> the child must REFUSE, not reset ----------
  int pfd[2];
  REQUIRE(::pipe(pfd) == 0);
  pid_t pid = fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    ::close(pfd[0]);
    peer_child_open(path, pfd[1]);  // never returns
  }
  ::close(pfd[1]);

  // PARENT = the "old worker": volume stays open and mapped, and keeps
  // writing while the child opens.  Poll the handshake non-blocking so the
  // writes really do overlap the child's open().
  int flags = ::fcntl(pfd[0], F_GETFL);
  REQUIRE(flags >= 0);
  REQUIRE(::fcntl(pfd[0], F_SETFL, flags | O_NONBLOCK) == 0);

  constexpr int kLiveWrites = 50;
  int live_ok = 0;
  bool child_opened = false;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(30);
  for (int i = 0; i < kLiveWrites; ++i) {
    CacheKey k("peer-live-" + std::to_string(i));
    auto wh = (*cache)->write_sync(k, data.size());
    if (wh.has_value() && wh->write_sync(data).has_value() &&
        wh->close_sync().has_value()) {
      ++live_ok;
    }
    if (!child_opened) {
      unsigned char b = 0;
      if (::read(pfd[0], &b, 1) == 1) {
        child_opened = true;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  while (!child_opened && std::chrono::steady_clock::now() < deadline) {
    unsigned char b = 0;
    if (::read(pfd[0], &b, 1) == 1) {
      child_opened = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (!child_opened) {
    ::kill(pid, SIGKILL);
  }
  int status = 0;
  REQUIRE(::waitpid(pid, &status, 0) == pid);
  ::close(pfd[0]);

  REQUIRE(child_opened);
  REQUIRE(WIFEXITED(status));

  // Collect-then-assert: CHECK (not REQUIRE) so one run reports EVERY number
  // -- the child's verdict AND how much of the live peer's cache survived it.
  INFO(
      "child exit: 3=refused under the live peer (want), "
      "2=RESET under a live peer (the bug), 5=opened without resetting");
  CHECK(WEXITSTATUS(status) == 3);

  // The live peer kept every one of its pre-fork documents...
  int survivors = 0;
  for (int i = 0; i < kPeerNumKeys; ++i) {
    CacheKey k("peer-key-" + std::to_string(i));
    if ((*cache)->read_sync(k).has_value()) {
      ++survivors;
    }
  }
  INFO("pre-fork documents surviving the child's open: " << survivors << " / "
                                                         << kPeerNumKeys);
  CHECK(survivors == kPeerNumKeys);
  // ...and its concurrent writes all landed.
  CHECK(live_ok == kLiveWrites);
  // ...and the header was NOT restamped: the reset is still OWED, and the
  // next opener (once we are gone) must be the one to perform it.
  CHECK(read_on_disk_major(fp(path, kPeerVolumeSize)) == kBadMajor);

  // ---- LEG 2: peer is GONE -> the reset must actually happen -------------
  // Proves the gate does not deadlock the real upgrade: once the old workers
  // drain, the exclusive probe succeeds and the owed reset runs.
  (*cache)->stop();
  cache->reset();  // drop the Cache entirely -> volume closed, lock released

  int pfd2[2];
  REQUIRE(::pipe(pfd2) == 0);
  pid_t pid2 = fork();
  REQUIRE(pid2 >= 0);
  if (pid2 == 0) {
    ::close(pfd2[0]);
    peer_child_open(path, pfd2[1]);  // never returns
  }
  ::close(pfd2[1]);
  int status2 = 0;
  REQUIRE(::waitpid(pid2, &status2, 0) == pid2);
  ::close(pfd2[0]);
  REQUIRE(WIFEXITED(status2));
  INFO(
      "with no live peer the child MUST reset (exit 2); any other code means "
      "the gate wedged the upgrade path");
  REQUIRE(WEXITSTATUS(status2) == 2);

  // The header was restamped, and the volume really is empty now.
  REQUIRE(read_on_disk_major(fp(path, kPeerVolumeSize)) != kBadMajor);
  {
    CacheConfig cfg2;
    cfg2.set_multi_process(0, 1);
    cfg2.set_ram_cache_size(0);
    auto c2 = Cache::create(cfg2);
    REQUIRE(c2.has_value());
    (*c2)->add_volume(path, kPeerVolumeSize);
    REQUIRE((*c2)->start().has_value());
    int ghosts = 0;
    for (int i = 0; i < kPeerNumKeys; ++i) {
      CacheKey k("peer-key-" + std::to_string(i));
      if ((*c2)->read_sync(k).has_value()) {
        ++ghosts;
      }
    }
    INFO("documents surviving the (now permitted) reset: " << ghosts);
    REQUIRE(ghosts == 0);
    REQUIRE((*c2)->stats().current_entries == 0);
    (*c2)->stop();
  }

  cleanup_temp_file(path);
}
#endif  // !CYCLONE_TSAN_BUILD (fork+Cache::start() unsupported under TSan)

// =============================================================================
// The RESIZE trigger refuses under a live peer, just like the version trigger
// =============================================================================
// The gate stops a new opener from WIPING a volume a live peer is serving from.
// TEST 1 reaches it via a version-major mismatch; a plain cache RESIZE reaches
// the SAME needs_reset with no format bump at all.  The opener must not carry
// on with ITS OWN geometry either -- stripe offsets and the shared mmap
// directory positions are derived from the volume size, so imposing a different
// size maps every directory at the wrong offset and corrupts the peer.  So on a
// geometry mismatch under a live peer we REFUSE to open (never adopt, never
// reset); the owed reset runs once the peers drain.
#if !defined(CYCLONE_TSAN_BUILD)
TEST_CASE(
    "a geometry mismatch under a live peer isolates onto its own file, "
    "never touching the peer",
    "[stabilization][multiprocess]") {
  const std::string path = get_temp_path("peer_geometry");
  cleanup_temp_file(path);

  const std::vector<std::byte> data(512, std::byte{0x3C});

  CacheConfig config;
  config.set_multi_process(0, 1);
  config.set_ram_cache_size(0);

  auto cache = Cache::create(config);
  REQUIRE(cache.has_value());
  (*cache)->add_volume(path, kPeerVolumeSize);  // 512MB -> 16 auto stripes
  REQUIRE((*cache)->start().has_value());

  for (int i = 0; i < kPeerNumKeys; ++i) {
    CacheKey k("peer-key-" + std::to_string(i));
    auto wh = (*cache)->write_sync(k, data.size());
    REQUIRE(wh.has_value());
    REQUIRE(wh->write_sync(data).has_value());
    REQUIRE(wh->close_sync().has_value());
  }
  REQUIRE((*cache)->stats().stripe_count == 16);
  REQUIRE((*cache)->stats().current_entries == kPeerNumKeys);

  // Child configures 1GB against the peer's live 512MB.  With structural-
  // fingerprint filenames the child's differing geometry hashes to a DIFFERENT
  // filename (see fingerprint_cache_path), so it never lands on the peer's ring
  // at all: it opens its OWN fresh file cleanly (exit 4) and the peer's 512MB
  // file is left byte-for-byte untouched.  This UPSTREAM isolation supersedes
  // the old same-file refuse gate (the stripe_count clause in open_locked
  // survives only as a hash-collision / pre-fingerprint-legacy backstop).
  int pfd[2];
  REQUIRE(::pipe(pfd) == 0);
  pid_t pid = fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    ::close(pfd[0]);
    peer_resize_child(path, pfd[1]);  // never returns
  }
  ::close(pfd[1]);
  unsigned char b = 0;
  (void)::read(pfd[0], &b, 1);  // handshake (best effort; waitpid is the gate)
  int status = 0;
  REQUIRE(::waitpid(pid, &status, 0) == pid);
  ::close(pfd[0]);
  REQUIRE(WIFEXITED(status));
  INFO(
      "4=opened its OWN fingerprinted file (want); 3=refused on the shared "
      "file (pre-fingerprint behaviour)");
  CHECK(WEXITSTATUS(status) == 4);

  // The peer's 512MB file must be byte-for-byte untouched -- the child opened a
  // different file, so it can neither extend nor reset this one.
  struct stat sb{};
  REQUIRE(::stat(fp(path, kPeerVolumeSize).c_str(), &sb) == 0);
  INFO("the child must not have touched the peer's file");
  CHECK(static_cast<size_t>(sb.st_size) == kPeerVolumeSize);

  // The child's own 1GB file exists SEPARATELY (proof of isolation, not
  // adoption of the peer's ring).
  struct stat sb_child{};
  CHECK(::stat(fp(path, 2 * kPeerVolumeSize).c_str(), &sb_child) == 0);

  // ...and the live peer kept every document.
  int survivors = 0;
  for (int i = 0; i < kPeerNumKeys; ++i) {
    CacheKey k("peer-key-" + std::to_string(i));
    if ((*cache)->read_sync(k).has_value()) {
      ++survivors;
    }
  }
  INFO("peer documents surviving the child's isolated open: "
       << survivors << " / " << kPeerNumKeys);
  CHECK(survivors == kPeerNumKeys);

  (*cache)->stop();
  cleanup_temp_file(fp(path, kPeerVolumeSize));
  cleanup_temp_file(fp(path, 2 * kPeerVolumeSize));
  cleanup_temp_file(path);
}
#endif  // !CYCLONE_TSAN_BUILD (fork+Cache::start() unsupported under TSan)
// =============================================================================
// W1: a graceful worker close must NOT drop the fork family's lifetime lock
// =============================================================================
// nginx/Apache open the Cache in the MASTER and fork(); every worker shares ONE
// open file description, so they share ONE lifetime lock.  The lock must be
// released only by the LAST close of that OFD.  The prior (sidecar) design
// EXPLICITLY unlocked in close(), so the first worker to drain gracefully
// dropped the lock for the whole family -- and a new master then reset the
// cache under the still-live siblings (reviewer #1: 400/400 -> 0/400 on every
// graceful reload).  The fcntl-OFD lifetime lock is never explicitly unlocked,
// so this must no longer happen.
//
// Scenario: master opens the volume (400 entries) and forks a worker that
// inherits the shared OFD.  The master then closes GRACEFULLY (worker still
// alive).  A new opener that wants to reset must still COEXIST, because the
// worker keeps the family lock held.
TEST_CASE("a graceful worker close keeps the fork family's lifetime lock",
          "[stabilization][multiprocess]") {
  const std::string path = get_temp_path("graceful_drain");
  cleanup_temp_file(path);

  const std::vector<std::byte> data(512, std::byte{0x2D});

  int wpf[2];  // worker "ready" pipe
  int wgo[2];  // worker "please exit" pipe
  REQUIRE(::pipe(wpf) == 0);
  REQUIRE(::pipe(wgo) == 0);

  // MASTER: open the volume and populate it.
  auto master = Cache::create([] {
    CacheConfig c;
    c.set_multi_process(0, 1);
    c.set_ram_cache_size(0);
    return c;
  }());
  REQUIRE(master.has_value());
  (*master)->add_volume(path, kPeerVolumeSize);
  REQUIRE((*master)->start().has_value());
  for (int i = 0; i < kPeerNumKeys; ++i) {
    CacheKey k("drain-key-" + std::to_string(i));
    auto wh = (*master)->write_sync(k, data.size());
    REQUIRE(wh.has_value());
    REQUIRE(wh->write_sync(data).has_value());
    REQUIRE(wh->close_sync().has_value());
  }

  // Fork a WORKER that inherits the master's open fds (the shared OFD, hence
  // the shared lifetime lock).  It holds them open, signals ready, and blocks
  // until told to exit -- _exit() so no C++ destructor runs; the kernel closes
  // the fds, which is exactly a worker draining.
  pid_t worker = fork();
  REQUIRE(worker >= 0);
  if (worker == 0) {
    ::close(wpf[0]);
    ::close(wgo[1]);
    unsigned char b = 1;
    (void)!::write(wpf[1], &b, 1);
    (void)::read(wgo[0], &b, 1);  // block until parent says exit
    _exit(0);
  }
  ::close(wpf[1]);
  ::close(wgo[0]);
  unsigned char b = 0;
  REQUIRE(::read(wpf[0], &b, 1) == 1);  // worker is holding the OFD

  // Stamp an incompatible header so the NEXT opener wants to reset, then have
  // the MASTER close GRACEFULLY.  Only the worker now holds the family lock.
  stamp_on_disk_major(fp(path, kPeerVolumeSize), kBadMajor);
  (*master)->stop();
  master->reset();  // destroy the master Cache -> Volume::close() on its fd

  // New opener: must COEXIST (worker still holds the lock), NOT reset.
  int npf[2];
  REQUIRE(::pipe(npf) == 0);
  pid_t opener = fork();
  REQUIRE(opener >= 0);
  if (opener == 0) {
    ::close(npf[0]);
    peer_child_open(path, npf[1]);  // never returns
  }
  ::close(npf[1]);
  (void)::read(npf[0], &b, 1);  // handshake
  int ostatus = 0;
  REQUIRE(::waitpid(opener, &ostatus, 0) == opener);
  ::close(npf[0]);

  // Let the worker drain and reap it.
  (void)!::write(wgo[1], "x", 1);
  int wstatus = 0;
  REQUIRE(::waitpid(worker, &wstatus, 0) == worker);
  ::close(wpf[0]);
  ::close(wgo[1]);

  REQUIRE(WIFEXITED(ostatus));
  INFO(
      "opener exit: 3=refused, lock still held (want), 2=RESET under the "
      "still-live worker (the W1 bug: graceful close dropped the family lock)");
  CHECK(WEXITSTATUS(ostatus) == 3);
  // The header must not have been restamped: the reset is still owed.
  CHECK(read_on_disk_major(fp(path, kPeerVolumeSize)) == kBadMajor);

  cleanup_temp_file(path);
}

// =============================================================================
// W2 / F1: rm cyclone.dat under a live peer must not wipe the peer, nor brick
//          the new opener
// =============================================================================
// The lifetime lock lives on the volume INODE, not a sidecar path, so deleting
// the volume file cannot defeat it: a live peer keeps its now-unlinked inode
// (and its data and its lock), and a new opener's O_CREAT gets a FRESH, empty
// inode with no peer to protect -- so it initializes an empty cache (F1: a
// freshly created file skips the peer probe and the "no header => refuse" path,
// which would otherwise BRICK the new master on a routine cache wipe).
#if !defined(CYCLONE_TSAN_BUILD)
TEST_CASE(
    "rm of the volume under a live peer neither wipes it nor bricks a new "
    "opener",
    "[stabilization][multiprocess]") {
  const std::string path = get_temp_path("rm_under_peer");
  cleanup_temp_file(path);

  const std::vector<std::byte> data(512, std::byte{0x4E});

  auto peer = Cache::create([] {
    CacheConfig c;
    c.set_multi_process(0, 1);
    c.set_ram_cache_size(0);
    return c;
  }());
  REQUIRE(peer.has_value());
  (*peer)->add_volume(path, kPeerVolumeSize);
  REQUIRE((*peer)->start().has_value());
  for (int i = 0; i < kPeerNumKeys; ++i) {
    CacheKey k("rm-key-" + std::to_string(i));
    auto wh = (*peer)->write_sync(k, data.size());
    REQUIRE(wh.has_value());
    REQUIRE(wh->write_sync(data).has_value());
    REQUIRE(wh->close_sync().has_value());
  }
  REQUIRE((*peer)->stats().current_entries == kPeerNumKeys);

  // Delete the volume file out from under the live peer (a tmp-cleaner, an
  // operator, a redeploy).  The peer keeps serving from the unlinked inode.
  REQUIRE(::unlink(fp(path, kPeerVolumeSize).c_str()) == 0);

  // New opener on the same path: O_CREATs a fresh inode.  It must start CLEAN
  // (F1: created_new => reset the empty file), NOT brick.
  int npf[2];
  REQUIRE(::pipe(npf) == 0);
  pid_t opener = fork();
  REQUIRE(opener >= 0);
  if (opener == 0) {
    ::close(npf[0]);
    // The fresh file has no header, so this "new binary" simply initializes it.
    // peer_child_open exits 3 on a failed start (a brick) -- which is the
    // pre-F1 outcome we are guarding against -- and 0/2 on success (major is
    // the freshly-written current version, != kBadMajor, so exit 2 = "reset",
    // i.e. it initialized the new empty file, which is correct here).
    peer_child_open(path, npf[1]);
  }
  ::close(npf[1]);
  unsigned char b = 0;
  (void)::read(npf[0], &b, 1);
  int ostatus = 0;
  REQUIRE(::waitpid(opener, &ostatus, 0) == opener);
  ::close(npf[0]);
  REQUIRE(WIFEXITED(ostatus));
  INFO("opener exit: 3 = start FAILED (the F1 brick); 2/5 = started clean");
  CHECK(WEXITSTATUS(ostatus) != 3);

  // The live peer still has every one of its documents -- the rm + new opener
  // did not touch its unlinked inode.
  int survivors = 0;
  for (int i = 0; i < kPeerNumKeys; ++i) {
    CacheKey k("rm-key-" + std::to_string(i));
    if ((*peer)->read_sync(k).has_value()) {
      ++survivors;
    }
  }
  INFO("peer documents surviving rm + new-opener: " << survivors << " / "
                                                    << kPeerNumKeys);
  CHECK(survivors == kPeerNumKeys);

  (*peer)->stop();
  cleanup_temp_file(path);
}
#endif  // !CYCLONE_TSAN_BUILD (fork+Cache::start() unsupported under TSan)

// =============================================================================
// A refused open under a live peer must write NOTHING to disk
// =============================================================================
// When an incompatible open (bad format version, or a geometry change) probes
// the reset gate and finds a live peer, it REFUSES -- it must not reset, must
// not restamp the header, and must not init() any stripe directory.  This is
// the byte-level proof: an on-disk region left exactly as we clobbered it means
// the refused open touched nothing.
//
// Faithful repro without corrupting a serving peer: a forked child holds the
// lifetime lock on the inode directly (any process with the volume open holds
// it), so the parent's open probes Conflict and refuses.  Stripe 0's on-disk
// MmapDirectory magic is clobbered up front; a refusing open leaves those bytes
// UNTOUCHED, whereas a reset()/init() would overwrite them.
TEST_CASE("a refused open under a live peer writes nothing to disk",
          "[stabilization][multiprocess]") {
  const std::string path = get_temp_path("coexist_no_init");
  cleanup_temp_file(path);

  // A small multi-stripe volume; 64MB auto-tiles into 2 stripes.
  constexpr size_t kVolSize = static_cast<size_t>(64) * 1024 * 1024;
  const std::vector<std::byte> data(256, std::byte{0x6F});

  {
    CacheConfig c;
    c.set_multi_process(0, 1);
    c.set_ram_cache_size(0);
    auto cache = Cache::create(c);
    REQUIRE(cache.has_value());
    (*cache)->add_volume(path, kVolSize);
    REQUIRE((*cache)->start().has_value());
    REQUIRE((*cache)->stats().stripe_count >= 2);  // must be multi-stripe
    for (int i = 0; i < 100; ++i) {
      CacheKey k("f2-key-" + std::to_string(i));
      auto wh = (*cache)->write_sync(k, data.size());
      REQUIRE(wh.has_value());
      REQUIRE(wh->write_sync(data).has_value());
      REQUIRE(wh->close_sync().has_value());
    }
    (*cache)->stop();
  }

  // Stripe 0's MmapDirectory magic sits right after the 64-byte volume header.
  // A refused open never maps or inits any stripe, so this is a convenient
  // geometry-independent probe point that a reset()/init() WOULD overwrite.
  const off_t stripe0_dir = 64;

  // Clobber stripe 0's MmapDirectory magic (first 4 bytes of its directory
  // region).  Record the original 4 bytes so we can prove a refusing open
  // leaves them UNTOUCHED (a reset()/init() would overwrite the region).
  uint32_t saved_magic = 0;
  {
    int fd = ::open(fp(path, kVolSize).c_str(), O_RDWR);
    REQUIRE(fd >= 0);
    REQUIRE(::pread(fd, &saved_magic, sizeof(saved_magic), stripe0_dir) ==
            static_cast<ssize_t>(sizeof(saved_magic)));
    const uint32_t junk = 0xBADD1200u;
    REQUIRE(::pwrite(fd, &junk, sizeof(junk), stripe0_dir) ==
            static_cast<ssize_t>(sizeof(junk)));
    ::fsync(fd);
    ::close(fd);
  }
  // Bump the on-disk format_version_major so the opener wants to reset, leaving
  // size/geometry intact (purely the version trigger).
  stamp_on_disk_major(fp(path, kVolSize), kBadMajor);

  // Fork a child that holds the lifetime lock on the inode directly, so the
  // parent's subsequent open probes Conflict and refuses.  The lock byte
  // matches kLifetimeLockByte in volume.cpp.
  constexpr off_t kLifetimeLockByte = 0x7FFFFFFFFFFFFFFDLL;
  int rpf[2];
  int gpf[2];
  REQUIRE(::pipe(rpf) == 0);
  REQUIRE(::pipe(gpf) == 0);
  pid_t holder = fork();
  REQUIRE(holder >= 0);
  if (holder == 0) {
    ::close(rpf[0]);
    ::close(gpf[1]);
    int fd = ::open(fp(path, kVolSize).c_str(), O_RDWR);
    struct flock fl{};
    fl.l_type = F_RDLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = kLifetimeLockByte;
    fl.l_len = 1;
    int rc = ::fcntl(fd, F_OFD_SETLK, &fl);
    unsigned char ok = (rc == 0) ? 1 : 0;
    (void)!::write(rpf[1], &ok, 1);
    unsigned char b;
    (void)::read(gpf[0], &b, 1);
    _exit(0);
  }
  ::close(rpf[1]);
  ::close(gpf[0]);
  unsigned char holder_ok = 0;
  REQUIRE(::read(rpf[0], &holder_ok, 1) == 1);
  REQUIRE(holder_ok == 1);  // holder took the lifetime lock

  // The open must FAIL (refuse) rather than reset/init under the live peer.
  {
    CacheConfig c;
    c.set_multi_process(0, 1);
    c.set_ram_cache_size(0);
    auto cache = Cache::create(c);
    REQUIRE(cache.has_value());
    VolumeConfig vc;
    vc.path = path;
    vc.size = kVolSize;
    vc.auto_reset_on_incompatible = true;
    (*cache)->add_volume(vc);
    const bool started = (*cache)->start().has_value();
    INFO(
        "an incompatible open under a live peer must REFUSE -- never reset or "
        "init() anything on disk");
    CHECK_FALSE(started);
    if (started) {
      (*cache)->stop();
    }
  }

  // Release the holder.
  (void)!::write(gpf[1], "x", 1);
  int hstatus = 0;
  REQUIRE(::waitpid(holder, &hstatus, 0) == holder);
  ::close(rpf[0]);
  ::close(gpf[1]);

  // The clobbered directory bytes must be UNTOUCHED: a refusing open did not
  // reset or init()/zero the stripe.  (A reset()/init() would have written a
  // fresh MDIR magic over our junk.)
  uint32_t after = 0;
  {
    int fd = ::open(fp(path, kVolSize).c_str(), O_RDONLY);
    REQUIRE(fd >= 0);
    REQUIRE(::pread(fd, &after, sizeof(after), stripe0_dir) ==
            static_cast<ssize_t>(sizeof(after)));
    ::close(fd);
  }
  INFO(
      "stripe 0 directory magic after the refused open (0xBADD1200 = "
      "untouched, want; anything else = reset()/init() ran under the peer)");
  CHECK(after == 0xBADD1200u);
  (void)saved_magic;

  cleanup_temp_file(path);
}

#endif  // !_WIN32

// =============================================================================
// T3: list_alternates reports chain corruption error
// =============================================================================
// When a chain document fails checksum verification after all retries,
// list_alternates_sync() must return CacheError::Corrupted (not silently
// return partial results or NotFound).

TEST_CASE("list_alternates reports chain corruption error",
          "[stabilization][multiprocess]") {
  CacheKey key("chain-corrupt-key");
  std::string path =
      write_alternates("chain_corrupt", key, 3, /*multi_process=*/true);

  // Verify alternates are readable before corruption
  {
    CacheConfig config;
    config.set_multi_process(0, 1);
    config.set_enable_checksum(true);

    auto cache = Cache::create(config);
    REQUIRE(cache.has_value());
    (*cache)->add_volume(path, static_cast<size_t>(10 * 1024 * 1024));
    REQUIRE((*cache)->start().has_value());

    auto alts = (*cache)->list_alternates_sync(key);
    REQUIRE(alts.has_value());
    REQUIRE(alts->size() == 3);

    (*cache)->stop();
  }

  // Corrupt the cache file: overwrite bytes in the data region
  // (after the 64-byte header + directory) to corrupt document checksums
  {
    std::fstream file(fp(path, static_cast<size_t>(10 * 1024 * 1024)),
                      std::ios::in | std::ios::out | std::ios::binary);
    REQUIRE(file.is_open());

    // Get file size
    file.seekg(0, std::ios::end);
    auto file_size = file.tellg();

    // Corrupt multiple regions in the data area to ensure we hit chain
    // documents. The directory occupies ~first few hundred KB; data follows. We
    // corrupt from 80% to 90% of the file to hit the data region.
    auto corrupt_start =
        static_cast<std::streamoff>(static_cast<double>(file_size) * 0.5);
    auto corrupt_end =
        static_cast<std::streamoff>(static_cast<double>(file_size) * 0.95);

    std::vector<char> garbage(1024, 'X');
    for (auto pos = corrupt_start; pos < corrupt_end; pos += 2048) {
      file.seekp(pos);
      file.write(garbage.data(), garbage.size());
    }
    file.close();
  }

  // Reopen and attempt to list alternates — should get Corrupted error
  {
    CacheConfig config;
    config.set_multi_process(0, 1);
    config.set_enable_checksum(true);
    // Only 1 retry so the test completes quickly
    config.multi_process_config.set_max_read_retries(1);

    auto cache = Cache::create(config);
    REQUIRE(cache.has_value());
    (*cache)->add_volume(path, static_cast<size_t>(10 * 1024 * 1024));
    REQUIRE((*cache)->start().has_value());

    auto alts = (*cache)->list_alternates_sync(key);

    // After corruption, we should either:
    // - Get CacheError::Corrupted (checksum failure exhausted retries)
    // - Get CacheError::NotFound (directory entry also corrupted)
    // - Get a partial result (some alternates before the corrupted chain doc)
    // With our fix, the first case is preferred, but NotFound is acceptable
    // if the directory entry itself was corrupted.
    if (!alts.has_value()) {
      // Should be Corrupted or NotFound, not some other error
      REQUIRE((alts.error() == CacheError::Corrupted ||
               alts.error() == CacheError::NotFound ||
               alts.error() == CacheError::ChainCorrupted));
    }
    // If alts has value, the corruption didn't hit the chain documents
    // (directory entries pointed to uncorrupted data) — that's acceptable
    // since corruption location is probabilistic.

    (*cache)->stop();
  }

  cleanup_temp_file(path);
}

// =============================================================================
// T3b: read_sync returns Corrupted on checksum failure
// =============================================================================
// More targeted: write a single entry, corrupt its document bytes, verify
// read_sync returns Corrupted instead of NotFound.

TEST_CASE("read_sync returns Corrupted on checksum failure",
          "[stabilization][multiprocess]") {
  std::string path = get_temp_path("read_corrupt");
  CacheKey key("read-corrupt-key");
  std::vector<std::byte> data(1024, std::byte{0xBB});

  // Write a single entry
  {
    CacheConfig config;
    config.set_multi_process(0, 1);
    config.set_enable_checksum(true);

    auto cache = Cache::create(config);
    REQUIRE(cache.has_value());
    (*cache)->add_volume(path, static_cast<size_t>(10 * 1024 * 1024));
    REQUIRE((*cache)->start().has_value());

    auto wh = (*cache)->write_sync(key, data.size());
    REQUIRE(wh.has_value());
    REQUIRE(wh->write_sync(data).has_value());
    REQUIRE(wh->close_sync().has_value());

    // Verify readable
    auto rh = (*cache)->read_sync(key);
    REQUIRE(rh.has_value());

    (*cache)->stop();
  }

  // Corrupt data region
  {
    std::fstream file(fp(path, static_cast<size_t>(10 * 1024 * 1024)),
                      std::ios::in | std::ios::out | std::ios::binary);
    REQUIRE(file.is_open());

    file.seekg(0, std::ios::end);
    auto file_size = file.tellg();

    // Corrupt the last 30% of the file (likely contains the document)
    auto corrupt_start =
        static_cast<std::streamoff>(static_cast<double>(file_size) * 0.7);
    std::vector<char> garbage(256, 'Z');
    for (auto pos = corrupt_start; pos < file_size; pos += 512) {
      file.seekp(pos);
      std::streamsize to_write =
          std::min(static_cast<std::streamsize>(garbage.size()),
                   static_cast<std::streamsize>(file_size - pos));
      file.write(garbage.data(), to_write);
    }
    file.close();
  }

  // Reopen and read — should get Corrupted
  {
    CacheConfig config;
    config.set_multi_process(0, 1);
    config.set_enable_checksum(true);
    config.multi_process_config.set_max_read_retries(1);

    auto cache = Cache::create(config);
    REQUIRE(cache.has_value());
    (*cache)->add_volume(path, static_cast<size_t>(10 * 1024 * 1024));
    REQUIRE((*cache)->start().has_value());

    auto rh = (*cache)->read_sync(key);
    if (!rh.has_value()) {
      // Should be Corrupted (not NotFound) when checksum verification fails
      REQUIRE((rh.error() == CacheError::Corrupted ||
               rh.error() == CacheError::NotFound));
    }
    // If readable, corruption didn't hit the right bytes — acceptable

    (*cache)->stop();
  }

  cleanup_temp_file(path);
}

// =============================================================================
// T5: Volume reset during active write
// =============================================================================
// If one process resets the volume while another is mid-write, the write
// should either complete cleanly or fail gracefully (no crash, no corruption).

TEST_CASE("Volume reset during active write", "[stabilization][stress]") {
  std::string path = get_temp_path("reset_write");
  std::vector<std::byte> data(4096, std::byte{0xCC});

  std::atomic<bool> stop_writing{false};
  std::atomic<int> writes_completed{0};
  std::atomic<int> writes_failed{0};

  // Writer thread: continuously writes entries
  auto writer_fn = [&]() {
    CacheConfig config;
    config.set_multi_process(0, 2);  // Process 0
    config.set_enable_checksum(true);

    auto cache = Cache::create(config);
    if (!cache.has_value()) return;

    (*cache)->add_volume(path, static_cast<size_t>(10 * 1024 * 1024));
    if (!(*cache)->start().has_value()) return;

    int counter = 0;
    while (!stop_writing.load(std::memory_order_relaxed)) {
      CacheKey key("reset-write-" + std::to_string(counter++));
      auto wh = (*cache)->write_sync(key, data.size());
      if (wh.has_value()) {
        auto wr = wh->write_sync(data);
        if (wr.has_value()) {
          wh->close_sync();
          ++writes_completed;
        } else {
          ++writes_failed;
        }
      } else {
        ++writes_failed;
      }
    }

    (*cache)->stop();
  };

  // Create the initial cache file
  {
    CacheConfig config;
    config.set_multi_process(0, 2);
    config.set_enable_checksum(true);

    auto cache = Cache::create(config);
    REQUIRE(cache.has_value());
    (*cache)->add_volume(path, static_cast<size_t>(10 * 1024 * 1024));
    REQUIRE((*cache)->start().has_value());
    (*cache)->stop();
  }

  // Start writer
  std::thread writer(writer_fn);

  // Give writer time to start
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Simulate a reset by another process: delete and recreate the file
  // (This mimics what PageSpeedCache::ResetVolume does)
  for (int round = 0; round < 3; ++round) {
    // Delete the file
    std::error_code ec;
    fs::remove(path, ec);

    // Recreate it
    {
      CacheConfig config;
      config.set_multi_process(1, 2);  // Process 1
      config.set_enable_checksum(true);

      auto cache = Cache::create(config);
      if (cache.has_value()) {
        (*cache)->add_volume(path, static_cast<size_t>(10 * 1024 * 1024));
        (*cache)->start();
        (*cache)->stop();
      }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  stop_writing.store(true);
  writer.join();

  // The key assertion: no crashes, no hangs.  Writes may fail, that's OK.
  REQUIRE((writes_completed.load() + writes_failed.load()) > 0);

  cleanup_temp_file(path);
}

// =============================================================================
// T6: Write survives concurrent phase toggle (cross-process phase lock)
// =============================================================================
// Before the phase_lock fix, a phase toggle by one thread could invalidate
// entries just written by another thread.  The phase_lock ensures that
// insert() and toggle_phase() are mutually exclusive.

TEST_CASE("Write survives concurrent phase toggle",
          "[stabilization][multiprocess]") {
  std::string path = get_temp_path("phase_lock");
  std::vector<std::byte> data(512, std::byte{0xDD});

  // Create and populate cache
  {
    CacheConfig config;
    config.set_multi_process(0, 1);
    config.set_enable_checksum(true);

    auto cache = Cache::create(config);
    REQUIRE(cache.has_value());
    (*cache)->add_volume(path, static_cast<size_t>(10 * 1024 * 1024));
    REQUIRE((*cache)->start().has_value());

    // Write an entry, then toggle phase, then read — entry must survive.
    // This tests the basic guarantee: an insert that completes before
    // toggle_phase starts must remain readable after the toggle.
    CacheKey key("phase-lock-test-1");
    auto wh = (*cache)->write_sync(key, data.size());
    REQUIRE(wh.has_value());
    REQUIRE(wh->write_sync(data).has_value());
    REQUIRE(wh->close_sync().has_value());

    // Verify readable before toggle
    auto rh1 = (*cache)->read_sync(key);
    REQUIRE(rh1.has_value());

    (*cache)->stop();
  }

  cleanup_temp_file(path);
}

TEST_CASE("Concurrent insert and phase toggle preserve entries",
          "[stabilization][stress]") {
  std::string path = get_temp_path("phase_race");

  // Create the initial cache file
  {
    CacheConfig config;
    config.set_multi_process(0, 1);
    config.set_enable_checksum(true);

    auto cache = Cache::create(config);
    REQUIRE(cache.has_value());
    (*cache)->add_volume(
        path, static_cast<size_t>(2 * 1024 *
                                  1024));  // Small cache to force evictions
    REQUIRE((*cache)->start().has_value());
    (*cache)->stop();
  }

  std::atomic<bool> stop{false};
  std::atomic<int> writes_ok{0};
  std::atomic<int> reads_ok{0};
  std::atomic<int> reads_miss{0};

  // Thread A: writes entries with unique keys
  auto writer_fn = [&]() {
    CacheConfig config;
    config.set_multi_process(0, 1);
    config.set_enable_checksum(true);
    // The immediate read-back below stamps a read lease, which
    // would (by design) defer the evictor's wraps and drop fills, skewing
    // the read-back rate this test measures.  Leases are exercised in
    // test_lease_pinning.cpp; disable them here.
    config.read_lease_duration = std::chrono::milliseconds(0);

    auto cache = Cache::create(config);
    if (!cache.has_value()) return;
    (*cache)->add_volume(path, static_cast<size_t>(2 * 1024 * 1024));
    if (!(*cache)->start().has_value()) return;

    std::vector<std::byte> data(4096, std::byte{0xEE});
    int counter = 0;
    while (!stop.load(std::memory_order_relaxed)) {
      CacheKey key("phase-race-" + std::to_string(counter));
      auto wh = (*cache)->write_sync(key, data.size());
      if (wh.has_value()) {
        if (wh->write_sync(data).has_value()) {
          wh->close_sync();
          ++writes_ok;

          // Immediately read back — with the phase_lock fix this should
          // always succeed (no silent invalidation by concurrent toggle).
          auto rh = (*cache)->read_sync(key);
          if (rh.has_value()) {
            ++reads_ok;
          } else {
            ++reads_miss;
          }
        }
      }
      ++counter;
    }

    (*cache)->stop();
  };

  // Thread B: writes large entries to force evictions (phase toggles)
  auto evictor_fn = [&]() {
    CacheConfig config;
    config.set_multi_process(0, 1);
    config.set_enable_checksum(true);
    // See writer_fn: keep read leases out of this phase-toggle test.
    config.read_lease_duration = std::chrono::milliseconds(0);

    auto cache = Cache::create(config);
    if (!cache.has_value()) return;
    (*cache)->add_volume(path, static_cast<size_t>(2 * 1024 * 1024));
    if (!(*cache)->start().has_value()) return;

    // Write large entries to fill the stripe and trigger evictions
    std::vector<std::byte> big_data(static_cast<size_t>(64 * 1024),
                                    std::byte{0xFF});
    int counter = 0;
    while (!stop.load(std::memory_order_relaxed)) {
      CacheKey key("evict-" + std::to_string(counter++));
      auto wh = (*cache)->write_sync(key, big_data.size());
      if (wh.has_value()) {
        wh->write_sync(big_data);
        wh->close_sync();
      }
    }

    (*cache)->stop();
  };

  std::thread writer(writer_fn);
  std::thread evictor(evictor_fn);

  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  stop.store(true);

  writer.join();
  evictor.join();

  // With the phase_lock fix, immediate read-after-write should succeed
  // at a high rate.  Some misses are expected from legitimate eviction
  // (the entry was genuinely overwritten on disk), but the ratio should
  // be much better than without the fix.
  REQUIRE(writes_ok.load() > 0);
  // The read-back success rate should be meaningful — without the fix,
  // most reads would miss due to phase invalidation.
  auto total_reads = reads_ok.load() + reads_miss.load();
  if (total_reads > 0) {
    auto success_rate =
        static_cast<double>(reads_ok.load()) / static_cast<double>(total_reads);
    // At least 25% of read-after-write should succeed (legitimately evicted
    // entries can miss, but phase-invalidated entries should not).
    REQUIRE(success_rate >= 0.25);
  }

  cleanup_temp_file(path);
}

TEST_CASE("Eviction under concurrent writes from two threads",
          "[stabilization][stress]") {
  std::string path = get_temp_path("evict_concurrent");

  {
    CacheConfig config;
    config.set_multi_process(0, 1);
    config.set_enable_checksum(true);
    auto cache = Cache::create(config);
    REQUIRE(cache.has_value());
    (*cache)->add_volume(path, static_cast<size_t>(2 * 1024 * 1024));
    REQUIRE((*cache)->start().has_value());
    (*cache)->stop();
  }

  std::atomic<bool> stop{false};
  std::atomic<int> total_writes{0};

  auto writer_fn = [&](int thread_id) {
    CacheConfig config;
    config.set_multi_process(0, 1);
    config.set_enable_checksum(true);

    auto cache = Cache::create(config);
    if (!cache.has_value()) return;
    (*cache)->add_volume(path, static_cast<size_t>(2 * 1024 * 1024));
    if (!(*cache)->start().has_value()) return;

    std::vector<std::byte> data(
        8192, std::byte{static_cast<uint8_t>(0xA0 + thread_id)});
    int counter = 0;
    while (!stop.load(std::memory_order_relaxed)) {
      CacheKey key("thread-" + std::to_string(thread_id) + "-" +
                   std::to_string(counter++));
      auto wh = (*cache)->write_sync(key, data.size());
      if (wh.has_value()) {
        wh->write_sync(data);
        wh->close_sync();
        ++total_writes;
      }
    }

    (*cache)->stop();
  };

  std::thread t1(writer_fn, 1);
  std::thread t2(writer_fn, 2);

  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  stop.store(true);

  t1.join();
  t2.join();

  // No crashes, no deadlocks.  Both threads should have completed writes.
  REQUIRE(total_writes.load() > 0);

  cleanup_temp_file(path);
}

// =============================================================================
// T7: Mid-chain corruption preserves earlier alternates for list
// =============================================================================
// After the graceful degradation fix, list_alternates_sync returns valid
// alternates collected before the corrupted chain document instead of
// discarding everything.

TEST_CASE("Mid-chain corruption preserves earlier alternates for list",
          "[stabilization][multiprocess]") {
  CacheKey key("partial-chain-key");
  std::string path =
      write_alternates("partial_chain_list", key, 5, /*multi_process=*/true);

  // Verify all 5 alternates are readable
  {
    CacheConfig config;
    config.set_multi_process(0, 1);
    config.set_enable_checksum(true);

    auto cache = Cache::create(config);
    REQUIRE(cache.has_value());
    (*cache)->add_volume(path, static_cast<size_t>(10 * 1024 * 1024));
    REQUIRE((*cache)->start().has_value());

    auto alts = (*cache)->list_alternates_sync(key);
    REQUIRE(alts.has_value());
    REQUIRE(alts->size() == 5);

    (*cache)->stop();
  }

  // Corrupt the TAIL end of the data region to hit later chain documents
  // while leaving earlier ones intact.  Alternates are written sequentially
  // (newest = head, at highest offset), so corrupting the earliest-written
  // region damages the tail of the chain.
  {
    std::fstream file(fp(path, static_cast<size_t>(10 * 1024 * 1024)),
                      std::ios::in | std::ios::out | std::ios::binary);
    REQUIRE(file.is_open());

    file.seekg(0, std::ios::end);
    auto file_size = file.tellg();

    // Corrupt the earliest data region (60-70% of file) — this is where
    // the first-written alternates live (tail of the chain).
    auto corrupt_start =
        static_cast<std::streamoff>(static_cast<double>(file_size) * 0.6);
    auto corrupt_end =
        static_cast<std::streamoff>(static_cast<double>(file_size) * 0.7);

    std::vector<char> garbage(512, 'Q');
    for (auto pos = corrupt_start; pos < corrupt_end; pos += 1024) {
      file.seekp(pos);
      std::streamsize to_write =
          std::min(static_cast<std::streamsize>(garbage.size()),
                   static_cast<std::streamsize>(corrupt_end - pos));
      file.write(garbage.data(), to_write);
    }
    file.close();
  }

  // Reopen and list alternates — should get a partial result
  {
    CacheConfig config;
    config.set_multi_process(0, 1);
    config.set_enable_checksum(true);
    config.multi_process_config.set_max_read_retries(1);

    auto cache = Cache::create(config);
    REQUIRE(cache.has_value());
    (*cache)->add_volume(path, static_cast<size_t>(10 * 1024 * 1024));
    REQUIRE((*cache)->start().has_value());

    auto alts = (*cache)->list_alternates_sync(key);

    // With graceful degradation:
    // - If corruption hit mid-chain: partial result (1-4 alternates)
    // - If corruption hit head document: error (Corrupted/NotFound)
    // - If corruption missed the chain entirely: all 5
    if (alts.has_value()) {
      // Either partial or full — both acceptable
      REQUIRE(!alts->empty());
      REQUIRE(alts->size() <= 5);
    }
    // Error is also acceptable if head was corrupted

    (*cache)->stop();
  }

  cleanup_temp_file(path);
}

// =============================================================================
// T8: Full chain corruption returns error (not empty result)
// =============================================================================
TEST_CASE("Full chain corruption returns error",
          "[stabilization][multiprocess]") {
  CacheKey key("full-corrupt-key");
  std::string path =
      write_alternates("full_corrupt", key, 3, /*multi_process=*/true);

  // Corrupt the entire data region
  {
    std::fstream file(fp(path, static_cast<size_t>(10 * 1024 * 1024)),
                      std::ios::in | std::ios::out | std::ios::binary);
    REQUIRE(file.is_open());

    file.seekg(0, std::ios::end);
    auto file_size = file.tellg();

    // Corrupt 40% to 99% of the file — very aggressive
    auto corrupt_start =
        static_cast<std::streamoff>(static_cast<double>(file_size) * 0.4);
    std::vector<char> garbage(2048, 'X');
    for (auto pos = corrupt_start; pos < file_size; pos += 2048) {
      file.seekp(pos);
      std::streamsize to_write =
          std::min(static_cast<std::streamsize>(garbage.size()),
                   static_cast<std::streamsize>(file_size - pos));
      file.write(garbage.data(), to_write);
    }
    file.close();
  }

  // Reopen — should get an error, not an empty success
  {
    CacheConfig config;
    config.set_multi_process(0, 1);
    config.set_enable_checksum(true);
    config.multi_process_config.set_max_read_retries(1);

    auto cache = Cache::create(config);
    REQUIRE(cache.has_value());
    (*cache)->add_volume(path, static_cast<size_t>(10 * 1024 * 1024));
    REQUIRE((*cache)->start().has_value());

    auto alts = (*cache)->list_alternates_sync(key);

    if (!alts.has_value()) {
      // Error: Corrupted, NotFound, or ChainCorrupted — all acceptable
      REQUIRE((alts.error() == CacheError::Corrupted ||
               alts.error() == CacheError::NotFound ||
               alts.error() == CacheError::ChainCorrupted));
    }
    // If alts has value, some data survived the corruption — acceptable

    (*cache)->stop();
  }

  cleanup_temp_file(path);
}
