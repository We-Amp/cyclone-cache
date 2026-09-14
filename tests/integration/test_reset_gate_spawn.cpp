// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Spawn-based multi-process tests for the cross-process reset gate
// (src/core/volume.cpp: "the reset gate: NEVER reset under a live peer").
//
// Peers are SPAWNED as a separate cyclone-test-peer image and handshaken
// over anonymous stdin/stdout pipes (tests/support/spawned_peer.hpp), so
// these cases run on Windows -- where the gate is LockFileEx byte-range
// locks and every process is an independent lock owner (the IIS overlapped-
// recycle shape) -- and stay sanitizer-clean on POSIX (posix_spawn, no
// fork).  The fork-based corpus in test_stabilization.cpp is NOT superseded:
// fork shares ONE open file description (the nginx/Apache family-lock
// semantics), which a spawned process structurally cannot reproduce.
//
// TEARDOWN RULE (every test here): the TempCacheDir is declared FIRST
// (destroyed LAST) and every SpawnedPeer AFTER it (destroyed FIRST), so a
// peer is dead -- its mapping and handles closed -- before the directory is
// removed.  Windows will not delete a file with a live mapped section, and
// TempCacheDir's destructor swallows deletion errors (temp_cache.hpp), so
// the wrong order is a silently leaked temp dir on a persistent runner, not
// a red test.
//
// Every blocking peer interaction takes an explicit deadline and fails the
// test on expiry.  No CI lane runs ctest, so the deadlines here are the ONLY
// timeout that exists: a blocking LockFileEx/fcntl bug must surface as a red
// test in seconds, not a hung 45-minute job.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "cyclone/cache.hpp"
#include "cyclone/config.hpp"
#include "cyclone/error.hpp"
#include "cyclone/key.hpp"
#include "support/spawned_peer.hpp"
#include "support/temp_cache.hpp"

using namespace cyclone;

namespace {

constexpr std::chrono::milliseconds kDeadline{30000};
constexpr size_t kVolSize = static_cast<size_t>(10) * 1024 * 1024;
constexpr uint16_t kBadMajor = 0xFF;

// REQUIRE, never SKIP: a missing helper exe must be a red test, not silently
// reduced coverage that still looks green.
std::string peer_exe() {
  const std::string exe = CYCLONE_TEST_PEER_EXE;
  REQUIRE(std::filesystem::exists(exe));
  return exe;
}

CacheConfig mp_config() {
  CacheConfig c;
  c.set_multi_process(0, 1);
  c.set_ram_cache_size(0);
  return c;
}

// Create a cache at `path`, write `n` entries, and return the volume's actual
// on-disk (structural-fingerprint) file path via volume_files() -- the
// documented way for anything that touches the file itself.  The Cache is
// destroyed on return: no mapping and no lifetime lock survive this call.
std::string create_populated_volume(const std::string& path, int n) {
  auto cache = Cache::create(mp_config());
  REQUIRE(cache.has_value());
  REQUIRE((*cache)->add_volume(path, kVolSize).has_value());
  REQUIRE((*cache)->start().has_value());
  const std::vector<std::byte> data(256, std::byte{0x5A});
  for (int i = 0; i < n; ++i) {
    CacheKey k("gate-key-" + std::to_string(i));
    auto wh = (*cache)->write_sync(k, data.size());
    REQUIRE(wh.has_value());
    REQUIRE(wh->write_sync(data).has_value());
    REQUIRE(wh->close_sync().has_value());
  }
  auto files = (*cache)->volume_files();
  REQUIRE(files.size() == 1);
  (*cache)->stop();
  return files[0].file_path;
}

// Stamp / read format_version_major on a volume FILE.  VolumeHeader layout:
// magic(4) + format_version_major(2).  An fd-level write is safe ONLY while
// no process has the file mapped -- WriteFile on a range that is also
// memory-mapped is not coherent with the views on one documented Windows
// runner class (see the regression block in tests/unit/test_alternate.cpp)
// -- so every caller here stamps strictly between closes, mirroring
// test_stabilization.cpp's "refused open writes nothing" structure, never
// its stamp-under-a-live-mapped-parent one.
void stamp_major(const std::string& file, uint16_t major) {
  std::fstream f(file, std::ios::in | std::ios::out | std::ios::binary);
  REQUIRE(f.is_open());
  f.seekp(4);
  f.write(reinterpret_cast<const char*>(&major), sizeof(major));
  REQUIRE(f.good());
  f.close();
}

uint16_t read_major(const std::string& file) {
  std::ifstream f(file, std::ios::in | std::ios::binary);
  REQUIRE(f.is_open());
  f.seekg(4);
  uint16_t major = 0;
  f.read(reinterpret_cast<char*>(&major), sizeof(major));
  REQUIRE(f.good());
  return major;
}

}  // namespace

// =============================================================================
// W1: two PROCESSES, same version, one volume -- both must open
// =============================================================================
// The IIS overlapped-recycle shape: an old w3wp still serving while the new
// one starts.  If the shared lifetime acquire blocks (LOCKFILE_FAIL_IMMEDIATELY
// missing), peer B never reports READY and the deadline reddens the test.
//
// Declared BEFORE W0 on purpose (Catch2 runs declaration order): a lifetime
// acquire regressed to exclusive+blocking hangs W0's second start() with no
// deadline possible (same process, no spawn), so this deadline-guarded case
// must run first and put the red diagnosis in the log before W0 can wedge
// the job.
TEST_CASE("reset gate: two spawned processes hold one volume concurrently",
          "[reset-gate-spawn][multiprocess]") {
  TempCacheDir tmp;  // FIRST -- teardown rule
  SpawnedPeer a;     // peers AFTER the dir: destroyed (killed) first
  SpawnedPeer b;
  const std::string path = tmp.path();
  const std::string exe = peer_exe();
  const std::string size_arg = std::to_string(kVolSize);

  REQUIRE(a.spawn(exe, {"open", path, size_arg}));
  auto ra = a.wait_ready(kDeadline);
  REQUIRE(ra.has_value());
  INFO("peer A status: " << *ra);
  REQUIRE(*ra == "READY");

  REQUIRE(b.spawn(exe, {"open", path, size_arg}));
  auto rb = b.wait_ready(kDeadline);
  REQUIRE(rb.has_value());
  INFO(
      "peer B status (an ERR here = the second opener was refused or the "
      "lock is exclusive; a timeout = the acquire blocks)");
  REQUIRE(*rb == "READY");

  a.request_exit();
  auto ea = a.wait_exit(kDeadline);
  REQUIRE(ea.has_value());
  CHECK(*ea == 0);

  b.request_exit();
  auto eb = b.wait_exit(kDeadline);
  REQUIRE(eb.has_value());
  CHECK(*eb == 0);
}

// =============================================================================
// W0: two same-process opens of one volume must BOTH succeed
// =============================================================================
// The cheapest catch of the priority-one regression: a lifetime lock taken
// EXCLUSIVE instead of SHARED.  Win32 byte-range locks conflict between
// handles of the SAME process too (matching the OFD semantics the substrate
// is built on), so no spawn is needed: a wrongly-exclusive lock fails the
// second start() here, on every platform.  (The blocking variant of the bug
// has no in-process deadline; W1 above catches it first, by design.)
TEST_CASE("reset gate: same-process opens stack their shared lifetime locks",
          "[reset-gate-spawn][multiprocess]") {
  TempCacheDir tmp;  // FIRST -- teardown rule in the header comment
  const std::string path = tmp.path();

  auto a = Cache::create(mp_config());
  REQUIRE(a.has_value());
  REQUIRE((*a)->add_volume(path, kVolSize).has_value());
  REQUIRE((*a)->start().has_value());

  auto b = Cache::create(mp_config());
  REQUIRE(b.has_value());
  REQUIRE((*b)->add_volume(path, kVolSize).has_value());
  auto started = (*b)->start();
  INFO(
      "the second same-version open must COEXIST: shared lifetime locks "
      "stack; failure here means the lock is wrongly exclusive");
  REQUIRE(started.has_value());

  (*b)->stop();
  (*a)->stop();
}

// =============================================================================
// W2: an incompatible open under a live peer is REFUSED -- exact error code
// =============================================================================
// Ordering is load-bearing and deliberately mirrors test_stabilization.cpp's
// "a refused open under a live peer writes nothing to disk" test: the
// incompatible major is stamped while NO process has the volume open or
// mapped, and the live peer is a "hold"-mode cyclone-test-peer that takes
// exactly the shared lifetime byte-range lock an open volume holds --
// WITHOUT mapping the file.  A fully API-opened peer is structurally
// impossible in this test: it would have to open either BEFORE the stamp
// (making the stamp an fd write under a live mapping -- the documented
// fd-write-vs-mapped-view incoherence on one Windows runner class) or AFTER
// it (whereupon, alone on the volume, it would perform the reset itself and
// erase the incompatibility).  The raw shared lock preserves both of the
// reference test's guarantees: stamp only with no mapping anywhere, and
// probe the gate against a real cross-process lock holder.  W1 covers the
// fully mapped live-peer shape on the compatible path.
TEST_CASE("reset gate: incompatible open is refused under a live peer",
          "[reset-gate-spawn][multiprocess]") {
  TempCacheDir tmp;    // FIRST -- teardown rule
  SpawnedPeer holder;  // AFTER the dir
  const std::string path = tmp.path();
  const std::string exe = peer_exe();

  const std::string file = create_populated_volume(path, 50);
  stamp_major(file, kBadMajor);  // nothing open, nothing mapped
  REQUIRE(read_major(file) == kBadMajor);

  REQUIRE(holder.spawn(exe, {"hold", file}));
  auto rh = holder.wait_ready(kDeadline);
  REQUIRE(rh.has_value());
  INFO("holder status: " << *rh);
  REQUIRE(*rh == "READY");

  {
    auto cache = Cache::create(mp_config());
    REQUIRE(cache.has_value());
    REQUIRE((*cache)->add_volume(path, kVolSize).has_value());
    auto started = (*cache)->start();
    REQUIRE_FALSE(started.has_value());
    // The EXACT code matters: IncompatibleVersion would misdiagnose a
    // live-peer refusal as a format problem, and the operator cure differs
    // (drain the peer vs. touch the format).  The error string is the only
    // observability channel a refusal has.
    REQUIRE(started.error() == CacheError::ResetRefusedLivePeer);
  }

  // The refused open wrote NOTHING: the reset is still owed to the next
  // opener that probes clean.
  CHECK(read_major(file) == kBadMajor);

  // The peer is unperturbed by the refused open: it still answers the
  // handshake and exits cleanly.
  holder.request_exit();
  auto ec = holder.wait_exit(kDeadline);
  REQUIRE(ec.has_value());
  CHECK(*ec == 0);
}

// =============================================================================
// W3: with NO live peer the reset runs, and counts as gate-verified
// =============================================================================
// End-to-end counter plumbing on a lock-supporting filesystem: the exclusive
// probe succeeds, the owed reset actually wipes, and the provenance lands in
// resets_gate_verified -- never in the degraded-gate alarm.
TEST_CASE("reset gate: reset with no peer runs and counts gate-verified",
          "[reset-gate-spawn][multiprocess]") {
  TempCacheDir tmp;
  const std::string path = tmp.path();

  const std::string file = create_populated_volume(path, 50);
  stamp_major(file, kBadMajor);  // nothing open, nothing mapped

  auto cache = Cache::create(mp_config());
  REQUIRE(cache.has_value());
  REQUIRE((*cache)->add_volume(path, kVolSize).has_value());
  REQUIRE((*cache)->start().has_value());

  auto stats = (*cache)->stats();
  CHECK(stats.resets_gate_verified == 1);
  CHECK(stats.resets_under_degraded_gate == 0);
  CHECK(stats.volumes_with_degraded_reset_gate == 0);
  CHECK(stats.current_entries == 0);  // the reset really wiped
  // The header was restamped to the current format by the reset.
  CHECK(read_major(file) != kBadMajor);

  (*cache)->stop();
}

// =============================================================================
// W4 (F1): rm with no holder -> fresh inode, no refusal, NEITHER counter
// =============================================================================
// A routine `rm cyclone.dat` + reload must never brick the new master (the
// pre-F1 "no header => refuse" path) and must never raise the degraded-gate
// alarm: FreshInode provenance counts as neither verified nor degraded -- an
// inode we just created had no peer to endanger.
TEST_CASE("reset gate: fresh inode after rm opens clean and counts neither",
          "[reset-gate-spawn][multiprocess]") {
  TempCacheDir tmp;
  const std::string path = tmp.path();

  const std::string file = create_populated_volume(path, 10);
  REQUIRE(std::filesystem::remove(file));  // nothing has it open

  auto cache = Cache::create(mp_config());
  REQUIRE(cache.has_value());
  REQUIRE((*cache)->add_volume(path, kVolSize).has_value());
  REQUIRE((*cache)->start().has_value());  // created_new: no refusal

  auto stats = (*cache)->stats();
  CHECK(stats.resets_gate_verified == 0);
  CHECK(stats.resets_under_degraded_gate == 0);
  CHECK(stats.volumes_with_degraded_reset_gate == 0);

  (*cache)->stop();
}

// =============================================================================
// W4b (POSIX only): rm UNDER a live peer -> new inode, F1 applies
// =============================================================================
// POSIX-ONLY, and correctly so -- do NOT "fix" this gap with a Windows
// variant: cyclone's _open never requests FILE_SHARE_DELETE, so on Windows
// the rm ITSELF fails while a live peer holds the volume; the fresh-inode-
// under-live-peer scenario is structurally unreachable there.  (W4 covers
// the reachable Windows shape: rm with no holder.)
#ifndef _WIN32
TEST_CASE(
    "reset gate: rm under a live peer opens a fresh inode without "
    "refusal (F1)",
    "[reset-gate-spawn][multiprocess]") {
  TempCacheDir tmp;  // FIRST -- teardown rule
  SpawnedPeer peer;  // AFTER the dir
  const std::string path = tmp.path();
  const std::string exe = peer_exe();

  const std::string file = create_populated_volume(path, 10);

  REQUIRE(peer.spawn(exe, {"open", path, std::to_string(kVolSize)}));
  auto rp = peer.wait_ready(kDeadline);
  REQUIRE(rp.has_value());
  REQUIRE(*rp == "READY");

  // Delete the file out from under the live peer; the peer keeps serving
  // from its now-unlinked inode (and keeps its lock on it).
  REQUIRE(std::filesystem::remove(file));

  // The parent's reopen O_CREATs a FRESH inode: F1 skips the peer probe (the
  // peer's lock lives on the OLD inode and protects nothing here), so the
  // open must succeed and neither provenance counter may move.
  {
    auto cache = Cache::create(mp_config());
    REQUIRE(cache.has_value());
    REQUIRE((*cache)->add_volume(path, kVolSize).has_value());
    REQUIRE((*cache)->start().has_value());
    auto stats = (*cache)->stats();
    CHECK(stats.resets_gate_verified == 0);
    CHECK(stats.resets_under_degraded_gate == 0);
    CHECK(stats.volumes_with_degraded_reset_gate == 0);
    (*cache)->stop();
  }

  // The peer on the old inode stayed healthy throughout.
  peer.request_exit();
  auto ec = peer.wait_exit(kDeadline);
  REQUIRE(ec.has_value());
  CHECK(*ec == 0);
}
#endif  // !_WIN32

// =============================================================================
// W6: the degraded gauge must not smear across volumes
// =============================================================================
// Forcing a single degraded volume portably is not feasible, so this pins the
// healthy-side invariant: on a normal (lock-supporting) filesystem a
// multi-volume cache reports ZERO degraded volumes, and a verified reset on
// each volume counts per volume without touching the gauge.
TEST_CASE("reset gate: degraded gauge stays zero across multiple volumes",
          "[reset-gate-spawn][multiprocess]") {
  TempCacheDir tmp;
  const std::string path_a = tmp.path("vol-a.dat");
  const std::string path_b = tmp.path("vol-b.dat");

  std::string file_a;
  std::string file_b;
  {
    auto cache = Cache::create(mp_config());
    REQUIRE(cache.has_value());
    REQUIRE((*cache)->add_volume(path_a, kVolSize).has_value());
    REQUIRE((*cache)->add_volume(path_b, kVolSize).has_value());
    REQUIRE((*cache)->start().has_value());

    auto stats = (*cache)->stats();
    CHECK(stats.volumes_with_degraded_reset_gate == 0);
    CHECK(stats.resets_under_degraded_gate == 0);

    auto files = (*cache)->volume_files();
    REQUIRE(files.size() == 2);
    file_a = files[0].file_path;
    file_b = files[1].file_path;
    (*cache)->stop();
  }

  // Stamp BOTH volumes incompatible (nothing open or mapped) and reopen: each
  // volume's verified reset counts exactly once -- the counters sum across
  // volumes -- and the gauge still reports zero degraded.
  stamp_major(file_a, kBadMajor);
  stamp_major(file_b, kBadMajor);
  {
    auto cache = Cache::create(mp_config());
    REQUIRE(cache.has_value());
    REQUIRE((*cache)->add_volume(path_a, kVolSize).has_value());
    REQUIRE((*cache)->add_volume(path_b, kVolSize).has_value());
    REQUIRE((*cache)->start().has_value());

    auto stats = (*cache)->stats();
    CHECK(stats.resets_gate_verified == 2);
    CHECK(stats.resets_under_degraded_gate == 0);
    CHECK(stats.volumes_with_degraded_reset_gate == 0);
    (*cache)->stop();
  }
}
