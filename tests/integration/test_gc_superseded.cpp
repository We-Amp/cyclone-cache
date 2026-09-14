// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Opt-in startup GC of SUPERSEDED structural-fingerprint cache files
// (gc_superseded_volumes; CacheConfig::gc_superseded_on_start).
//
// This is disk hygiene, but it is safety-critical: a bug can unlink a LIVE
// cache file and cause data loss.  Every test here pins one of the safety
// properties the GC must never violate -- it must delete ONLY a superseded
// fingerprinted file that no live peer holds, and never a live/locked file, a
// legacy un-fingerprinted cyclone.dat, or a foreign look-alike.
//
// The whole translation unit is POSIX-only because GC itself is compiled out
// on Windows: its safety argument is not yet established there (see the
// gc_superseded_on_start rationale in cyclone/config.hpp) -- NOT because live
// peers cannot exist; the lifetime lock IS implemented on Windows.
// Same-process OFD locks conflict across fds, so the "live peer protected"
// case is exercisable in a single process.
//
// COVERAGE NOTE (inodes_match recheck): the pre-unlink name->inode TOCTOU
// recheck (inodes_match on the locked fd) is present and correct but is NOT
// directly exercised here -- a deterministic in-process race to swap the inode
// between GC's stat/open and its unlink would need a production test-seam we
// deliberately do not add.  Its correctness rides on (a) inodes_match itself,
// unit-tested in test_inode_revalidation.cpp, and (b) the ordering proof that
// the recheck runs while GC holds the EXCLUSIVE lifetime lock, so no peer can
// re-open+lock the name in the gap (see gc_superseded_volumes invariants
// I1/I3).

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "core/volume.hpp"

#ifndef _WIN32

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstddef>
#include <ctime>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "cyclone/cache.hpp"
#include "cyclone/config.hpp"
#include "cyclone/key.hpp"
#include "support/temp_cache.hpp"

using namespace cyclone;
namespace fs = std::filesystem;

namespace {

constexpr size_t kMB = static_cast<size_t>(1024) * 1024;

// A modest live-volume size (auto stripes) and a DIFFERENT-geometry size used
// only to derive a distinct superseded fingerprint NAME (40MB vs 512MB fall on
// opposite sides of an auto-stripe count boundary, so they hash differently --
// see test_fingerprint_filenames.cpp).
constexpr size_t kLiveSize = 40 * kMB;
constexpr size_t kSupersededGeomSize = 512 * kMB;

// The live volume's real fingerprinted path for a given base (single-process,
// mmap_directory=false: what a directly-constructed single-process Volume, and
// a default single-process Cache, resolve to).
std::string live_name(const std::string& base) {
  return fingerprint_cache_path(base, kLiveSize, 0, /*mmap_directory=*/false);
}

// A fingerprint-shaped name that differs from live_name but shares its base
// stem ("cyclone") -- i.e. exactly what an earlier format/geometry left behind.
std::string superseded_name(const std::string& base) {
  return fingerprint_cache_path(base, kSupersededGeomSize, 0,
                                /*mmap_directory=*/false);
}

// Write a minimal but VALID volume file: a correct 64-byte VolumeHeader (magic
// matches, so VolumeHeader::is_valid() passes) and nothing else.  Enough for
// the GC's magic check; far cheaper than ftruncating a multi-hundred-MB file.
void write_valid_header_file(const std::string& path) {
  VolumeHeader h;  // defaults: magic == kMagic, current format version
  std::byte buf[VolumeHeader::kSize];
  h.serialize(buf);
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
  REQUIRE(fd >= 0);
  REQUIRE(::write(fd, buf, VolumeHeader::kSize) ==
          static_cast<ssize_t>(VolumeHeader::kSize));
  ::close(fd);
}

// Write a fingerprint-shaped file whose contents are NOT a valid volume header.
void write_garbage_file(const std::string& path) {
  std::byte junk[128];
  for (auto& b : junk) {
    b = std::byte{0xFF};
  }
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
  REQUIRE(fd >= 0);
  REQUIRE(::write(fd, junk, sizeof junk) == static_cast<ssize_t>(sizeof junk));
  ::close(fd);
}

// Push a file's mtime `seconds_ago` into the past so it clears the GC age
// floor.
void backdate(const std::string& path, int seconds_ago) {
  const auto t = std::time(nullptr) - seconds_ago;
  struct timespec times[2];
  times[0].tv_sec = t;
  times[0].tv_nsec = 0;  // atime
  times[1].tv_sec = t;
  times[1].tv_nsec = 0;  // mtime
  REQUIRE(::utimensat(AT_FDCWD, path.c_str(), times, 0) == 0);
}

// Open a real, single-process Volume directly on `path` at kLiveSize and keep
// it open (so it holds an fd -> backing_identity(), and the SHARED lifetime
// lock).  Returned by value: Volume is move-... actually non-copyable; hand
// back via unique_ptr so the caller controls lifetime.
std::unique_ptr<Volume> open_live_volume(const std::string& path,
                                         size_t size = kLiveSize) {
  VolumeConfig vc;
  vc.path = path;
  vc.size = size;
  auto v = std::make_unique<Volume>(vc);
  REQUIRE(v->open().has_value());
  return v;
}

}  // namespace

TEST_CASE("GC reclaims a superseded fingerprint file", "[gc][superseded]") {
  TempCacheDir dir;
  const std::string base = dir.path();
  const std::string fa = live_name(base);
  const std::string fb = superseded_name(base);
  REQUIRE(fa != fb);  // distinct geometry -> distinct name

  auto live = open_live_volume(fa);
  write_valid_header_file(fb);  // a valid, but NOT-live, superseded file

  REQUIRE(fs::exists(fa));
  REQUIRE(fs::exists(fb));

  // age_floor 0 makes the reclamation deterministic regardless of mtime.
  gc_superseded_volumes({live.get()}, std::chrono::seconds(0));

  REQUIRE(fs::exists(fa));        // I5: the live file survives
  REQUIRE_FALSE(fs::exists(fb));  // the superseded file is reclaimed
  live->close();
}

TEST_CASE(
    "GC scopes base-stem matching per directory (no cross-dir over-reach)",
    "[gc][superseded]") {
  TempCacheDir dirA;
  TempCacheDir dirB;

  // A live volume in dir A with base stem "cyclone".
  const std::string liveA = live_name(dirA.path("cyclone.dat"));
  auto lva = open_live_volume(liveA);

  // A live volume in dir B with a DIFFERENT base stem "other" -- so dir B is
  // actually scanned by GC (a directory with no live volume is never visited,
  // which alone would mask the bug this test targets).
  const std::string liveB = live_name(dirB.path("other.dat"));
  auto lvb = open_live_volume(liveB);

  // A FOREIGN, unheld, valid-header, aged fingerprint file in dir B whose base
  // stem is "cyclone" -- matching the live volume in dir A, but NO live volume
  // rooted in dir B.  This is the over-reach case: with the old two-global-sets
  // logic its base stem is in the global set and it would be UNLINKED; with
  // per-directory scoping it is matched only against dir B's live base stems
  // ({"other"}) and skipped.
  const std::string foreignB = superseded_name(dirB.path("cyclone.dat"));
  write_valid_header_file(foreignB);
  backdate(foreignB, 120);

  // A genuinely-superseded, unheld file in dir B whose base stem IS "other"
  // (matches dir B's own live volume) -- it SHOULD be reclaimed.  This proves
  // GC really scans dir B, isolating the fix's effect to the cross-dir base
  // stem rather than to "dir B was skipped entirely".
  const std::string supersededB = superseded_name(dirB.path("other.dat"));
  REQUIRE(supersededB != liveB);  // distinct geometry -> not the live file
  write_valid_header_file(supersededB);
  backdate(supersededB, 120);

  gc_superseded_volumes({lva.get(), lvb.get()});  // default age floor (60s)

  // Per-directory scoping: the cross-dir base-stem coincidence is NOT deleted.
  REQUIRE(fs::exists(foreignB));
  // dir B's own superseded file IS reclaimed (proves GC scanned dir B).
  REQUIRE_FALSE(fs::exists(supersededB));
  // Both live files survive.
  REQUIRE(fs::exists(liveA));
  REQUIRE(fs::exists(liveB));

  lva->close();
  lvb->close();
}

TEST_CASE("GC never deletes a file a live peer holds open",
          "[gc][superseded]") {
  TempCacheDir dir;
  const std::string base = dir.path();
  const std::string fa = live_name(base);
  const std::string fb = superseded_name(base);
  REQUIRE(fa != fb);

  auto live = open_live_volume(fa);
  // A live PEER holds the superseded-named file open -> it holds the SHARED
  // lifetime lock on that inode.  It is deliberately NOT in the live set handed
  // to GC, so GC classifies it as a candidate and must be stopped by the
  // exclusive-lock probe alone (I1/I2), not by the keep-set.
  auto peer = open_live_volume(fb);

  REQUIRE(fs::exists(fb));
  gc_superseded_volumes({live.get()}, std::chrono::seconds(0));
  REQUIRE(fs::exists(fb));  // exclusive probe Conflicts -> skip

  peer->close();
  live->close();
}

TEST_CASE("GC never deletes a foreign look-alike (bad header)",
          "[gc][superseded]") {
  TempCacheDir dir;
  const std::string base = dir.path();
  const std::string fa = live_name(base);
  const std::string foreign = superseded_name(base);
  REQUIRE(fa != foreign);

  auto live = open_live_volume(fa);
  write_garbage_file(foreign);  // fingerprint-shaped NAME, invalid header

  gc_superseded_volumes({live.get()}, std::chrono::seconds(0));
  REQUIRE(fs::exists(foreign));  // header magic check fails -> skip
  live->close();
}

TEST_CASE("GC never deletes a legacy un-fingerprinted cyclone.dat",
          "[gc][superseded]") {
  TempCacheDir dir;
  const std::string base = dir.path();  // ".../cyclone.dat" (no fingerprint)
  const std::string fa = live_name(base);

  auto live = open_live_volume(fa);
  // Even a perfectly valid volume header at the legacy name must be spared: its
  // stem is not fingerprint-shaped, so it is never a candidate (I4).
  write_valid_header_file(base);
  REQUIRE(fs::exists(base));

  gc_superseded_volumes({live.get()}, std::chrono::seconds(0));
  REQUIRE(fs::exists(base));  // I4: legacy name skipped before any fd work
  live->close();
}

TEST_CASE("GC age floor spares fresh files, reclaims aged ones",
          "[gc][superseded]") {
  TempCacheDir dir;
  const std::string base = dir.path();
  const std::string fa = live_name(base);
  const std::string fb = superseded_name(base);

  auto live = open_live_volume(fa);
  write_valid_header_file(fb);  // mtime == now

  // Default 60s floor: a just-created superseded file is spared (race-reducer).
  gc_superseded_volumes({live.get()});
  REQUIRE(fs::exists(fb));

  // Backdate past the floor: now it is reclaimed.
  backdate(fb, 120);
  gc_superseded_volumes({live.get()});
  REQUIRE_FALSE(fs::exists(fb));
  live->close();
}

TEST_CASE("GC does nothing without a live keep-set (fail closed)",
          "[gc][superseded]") {
  TempCacheDir dir;
  const std::string base = dir.path();
  const std::string fb = superseded_name(base);
  write_valid_header_file(fb);
  backdate(fb, 120);

  // Empty live set -> empty keep-set / base-stem set -> GC refuses to delete
  // anything (it cannot prove a candidate is not one of the live files).  This
  // is the same fail-closed direction the Unsupported lock result takes below.
  gc_superseded_volumes({}, std::chrono::seconds(0));
  REQUIRE(fs::exists(fb));
}

// The upgrade path a format-major bump actually takes.  Asserted here because
// the bump's whole justification is that discard-and-recreate is EXISTING
// behaviour reached by filename divergence -- if that were wrong, a bump would
// instead reset a volume in place, or refuse to open under a live peer.
TEST_CASE("A format-major upgrade opens a fresh volume beside the old file",
          "[gc][superseded][cache][migration]") {
  // The file a previous-format binary would have been using: same base stem
  // and geometry, previous format major in BOTH the name and the header.
  auto previous_format_name = [](const std::string& base) {
    const std::string n = live_name(base);
    const std::string cur =
        "-" + std::to_string(VolumeHeader::kFormatVersionMajor) + "-";
    const std::string prev =
        "-" + std::to_string(VolumeHeader::kFormatVersionMajor - 1) + "-";
    const size_t at = n.rfind(cur);
    REQUIRE(at != std::string::npos);
    return n.substr(0, at) + prev + n.substr(at + cur.size());
  };
  auto write_previous_format_file = [](const std::string& path) {
    VolumeHeader h;
    h.format_version_major = VolumeHeader::kFormatVersionMajor - 1;
    REQUIRE(h.is_valid());
    REQUIRE_FALSE(h.is_compatible());
    std::byte buf[VolumeHeader::kSize];
    h.serialize(buf);
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
    REQUIRE(fd >= 0);
    REQUIRE(::write(fd, buf, VolumeHeader::kSize) ==
            static_cast<ssize_t>(VolumeHeader::kSize));
    ::close(fd);
  };

  const bool gc_on = GENERATE(false, true);
  TempCacheDir dir;
  const std::string base = dir.path();
  const std::string old_file = previous_format_name(base);
  write_previous_format_file(old_file);
  // Backdate rather than wait: the GC age floor start() applies defaults to
  // 60s, which a fixture created moments ago would otherwise never clear.
  backdate(old_file, 120);

  CacheConfig cfg;
  cfg.set_ram_cache_size(0);
  cfg.set_gc_superseded_on_start(gc_on);
  auto c = Cache::create(cfg);
  REQUIRE(c.has_value());
  VolumeConfig vc;
  vc.path = base;
  vc.size = kLiveSize;
  REQUIRE((*c)->add_volume(vc).has_value());
  // Never IncompatibleVersion, never ResetRefusedLivePeer: the current-format
  // binary resolves to a DIFFERENT filename and never consults the old file's
  // compatibility at all.
  REQUIRE((*c)->start().has_value());

  const auto files = (*c)->volume_files();
  REQUIRE(files.size() == 1);
  REQUIRE(files[0].file_path == live_name(base));
  REQUIRE(files[0].file_path != old_file);
  REQUIRE(fs::exists(files[0].file_path));

  // Cold cache, and -- the load-bearing part -- cold by CREATION, not by an
  // in-place wipe: a freshly created inode counts as neither a gate-verified
  // nor a degraded-gate reset, so both provenance counters staying at zero is
  // proof no volume was reset under anything.
  auto st = (*c)->stats();
  REQUIRE(st.current_entries == 0);
  REQUIRE(st.resets_gate_verified == 0);
  REQUIRE(st.resets_under_degraded_gate == 0);
  REQUIRE_FALSE((*c)->read_sync(CacheKey("migration-probe")).has_value());

  // The old file is left alone unless GC is explicitly enabled.
  REQUIRE(fs::exists(old_file) == !gc_on);
  (*c)->stop();
}

TEST_CASE("start() runs GC iff gc_superseded_on_start (wiring)",
          "[gc][superseded][cache]") {
  SECTION("enabled -> superseded file reclaimed on start") {
    TempCacheDir dir;
    const std::string base = dir.path();
    const std::string super = superseded_name(base);
    write_valid_header_file(super);
    backdate(super, 120);  // clear the default age floor start() uses

    CacheConfig cfg;
    cfg.set_ram_cache_size(0);
    cfg.set_gc_superseded_on_start(true);
    auto c = Cache::create(cfg);
    REQUIRE(c.has_value());
    VolumeConfig vc;
    vc.path = base;
    vc.size = kLiveSize;
    REQUIRE((*c)->add_volume(vc).has_value());
    REQUIRE((*c)->start().has_value());

    REQUIRE(fs::exists(live_name(base)));  // the cache's own file survives
    REQUIRE_FALSE(fs::exists(super));      // superseded reclaimed by start()
    (*c)->stop();
  }

  SECTION("disabled (default) -> nothing deleted") {
    TempCacheDir dir;
    const std::string base = dir.path();
    const std::string super = superseded_name(base);
    write_valid_header_file(super);
    backdate(super, 120);

    CacheConfig cfg;  // gc_superseded_on_start defaults to false
    cfg.set_ram_cache_size(0);
    REQUIRE(cfg.gc_superseded_on_start == false);
    auto c = Cache::create(cfg);
    REQUIRE(c.has_value());
    VolumeConfig vc;
    vc.path = base;
    vc.size = kLiveSize;
    REQUIRE((*c)->add_volume(vc).has_value());
    REQUIRE((*c)->start().has_value());

    REQUIRE(fs::exists(super));  // opt-out: untouched
    (*c)->stop();
  }
}

// Unsupported-polarity guard.
//
// The lock classifier (try_lock_lifetime_exclusive) has internal linkage in
// volume.cpp and cannot be called from here, and a filesystem with NO OFD
// byte-range locking (the only way to make the probe return Unsupported) is not
// something a test can synthesize deterministically on a normal CI filesystem.
// So this property is pinned by CODE STRUCTURE + comment rather than execution:
// GC's probe handles ONLY LockResult::Acquired as "safe to delete"; Conflict,
// Unsupported, and any other outcome all fall through to a SKIP.  This is the
// exact OPPOSITE polarity of the reset gate in Volume::open_locked, which
// treats Unsupported as "degrade -> allow reset".  The "fail closed without a
// keep-set" case above exercises the same skip-on-uncertainty direction that we
// rely on here.  If this ever regresses to deleting on a non-Acquired result,
// the live-peer and foreign-look-alike cases above would also start failing.
TEST_CASE("GC treats a non-Acquired lock probe as skip (documented)",
          "[gc][superseded]") {
  SUCCEED(
      "Unsupported/Conflict -> skip is structural; see comment + "
      "live-peer / fail-closed cases");
}

// F2 (.small tier): the small-tier sibling is a first-class live entry with its
// OWN base stem ("cyclone.dat", from "cyclone.dat.small") -- distinct from the
// main tier's base stem ("cyclone").  Pin all four sibling-shape properties:
// live .small survival, superseded .small reclamation, legacy
// "cyclone.dat.small" survival, and the hostile shape
// "cyclone-<fmt>-<hex>.dat.small" (a main-tier FINGERPRINTED name with a
// literal ".small" appended -- its stem ends ".dat", which is not a
// fingerprint tail) never classifying as a candidate.
TEST_CASE(
    "GC handles the .small tier: reclaims superseded, spares legacy and "
    "hostile shapes",
    "[gc][superseded]") {
  TempCacheDir dir;
  const std::string small_base = dir.path("cyclone.dat.small");

  // Live small-tier volume at its real fingerprinted name.
  const std::string live_small =
      fingerprint_cache_path(small_base, kLiveSize, 0,
                             /*mmap_directory=*/false);
  auto live = open_live_volume(live_small);

  // A superseded small-tier sibling (different geometry, same base stem).
  const std::string superseded_small =
      fingerprint_cache_path(small_base, kSupersededGeomSize, 0,
                             /*mmap_directory=*/false);
  REQUIRE(superseded_small != live_small);
  write_valid_header_file(superseded_small);

  // A legacy un-fingerprinted small tier: never a candidate (I4).
  write_valid_header_file(small_base);

  // Hostile shape: a fingerprinted MAIN-tier name with ".small" appended.  Its
  // stem ("cyclone-<fmt>-<hex>.dat") ends in ".dat", so it must never
  // classify -- even though it contains a fingerprint-looking run.
  const std::string hostile = live_name(dir.path("cyclone.dat")) + ".small";
  write_valid_header_file(hostile);

  gc_superseded_volumes({live.get()}, std::chrono::seconds(0));

  REQUIRE(fs::exists(live_small));              // I5: live sibling survives
  REQUIRE_FALSE(fs::exists(superseded_small));  // superseded sibling reclaimed
  REQUIRE(fs::exists(small_base));              // I4: legacy .small survives
  REQUIRE(fs::exists(hostile));                 // shape mismatch never a
                                                // candidate
  live->close();
}

// F4 (v5 header): the GC population is BY DEFINITION old-format files, so the
// magic check must accept an old format major (is_valid() is magic-only;
// is_compatible() is the strict one).  A regression that tightened the GC
// header check to is_compatible() would silently make GC inert for exactly the
// files it exists to reclaim.
TEST_CASE("GC reclaims an old-format-major superseded file (magic-only check)",
          "[gc][superseded]") {
  TempCacheDir dir;
  const std::string base = dir.path();
  auto live = open_live_volume(live_name(base));

  const std::string old_fmt = superseded_name(base);
  VolumeHeader h;
  h.format_version_major = 5;  // pre-current format: is_valid, NOT compatible
  REQUIRE(h.is_valid());
  REQUIRE_FALSE(h.is_compatible());
  std::byte buf[VolumeHeader::kSize];
  h.serialize(buf);
  const int fd = ::open(old_fmt.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
  REQUIRE(fd >= 0);
  REQUIRE(::write(fd, buf, VolumeHeader::kSize) ==
          static_cast<ssize_t>(VolumeHeader::kSize));
  ::close(fd);

  gc_superseded_volumes({live.get()}, std::chrono::seconds(0));

  REQUIRE_FALSE(fs::exists(old_fmt));  // the actual GC population is reclaimed
  live->close();
}

#endif  // !_WIN32
