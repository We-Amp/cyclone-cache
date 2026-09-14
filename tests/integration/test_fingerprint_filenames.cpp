// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Structural-fingerprint cache filenames (upgrade safety).
//
// fingerprint_cache_path() encodes the on-disk FORMAT (kFormatVersionMajor) and
// GEOMETRY (derived stripe count / base stripe size / remainder) into the cache
// filename, so multi-process peers whose binaries disagree on layout resolve to
// DIFFERENT files and never share one on-disk ring during an overlapping
// upgrade.  These tests pin the naming contract and the M1 reconciliation
// (Volume::open_locked's geometry gate no longer resets two same-geometry peers
// that differ only in raw size).

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "core/volume.hpp"  // fingerprint_cache_path / kMinStripeSize
#include "cyclone/cache.hpp"
#include "cyclone/config.hpp"
#include "cyclone/key.hpp"

#ifdef _WIN32
#include <process.h>
#define FP_GETPID _getpid
#else
#include <unistd.h>
#define FP_GETPID getpid
#endif

using namespace cyclone;
namespace fs = std::filesystem;

namespace {

// A stem is fingerprinted iff it ends with "-<one-or-more-digits>-<16 lowercase
// hex>".  Mirrors the internal idempotency parse; used only for assertions.
bool looks_fingerprinted(const std::string &full_path) {
  const std::string stem = fs::path(full_path).stem().string();
  if (stem.size() < 19) {
    return false;
  }
  const size_t hex_begin = stem.size() - 16;
  for (size_t j = hex_begin; j < stem.size(); ++j) {
    const char c = stem[j];
    const bool is_hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    if (!is_hex) {
      return false;
    }
  }
  if (hex_begin < 1 || stem[hex_begin - 1] != '-') {
    return false;
  }
  size_t d = hex_begin - 1;
  while (d > 0 && stem[d - 1] >= '0' && stem[d - 1] <= '9') {
    --d;
  }
  if (d == hex_begin - 1) {
    return false;
  }
  return d >= 1 && stem[d - 1] == '-';
}

std::string tmp(const std::string &name) {
  return (fs::temp_directory_path() / name).string();
}

// Unique per process AND per call, for tests that create REAL cache files (the
// pure-function naming tests above use tmp() and never touch the filesystem, so
// a fixed name is fine there).
std::string unique_tmp(const std::string &name) {
  static std::atomic<int> counter{0};
  return (fs::temp_directory_path() /
          (name + "_" + std::to_string(FP_GETPID()) + "_" +
           std::to_string(counter.fetch_add(1))))
      .string();
}

constexpr size_t kMB = static_cast<size_t>(1024) * 1024;

}  // namespace

// (b)/(d): identical inputs are STABLE and deterministic (no time/rand).
TEST_CASE("fingerprint is stable and deterministic", "[fingerprint]") {
  const std::string p = tmp("cyclone.dat");
  const std::string a =
      fingerprint_cache_path(p, 256 * kMB, 0, /*mmap_directory=*/true);
  const std::string b =
      fingerprint_cache_path(p, 256 * kMB, 0, /*mmap_directory=*/true);
  REQUIRE(a == b);
  REQUIRE(looks_fingerprinted(a));
  // The format major is embedded verbatim.
  const std::string stem = fs::path(a).stem().string();
  REQUIRE(stem.find("-" + std::to_string(VolumeHeader::kFormatVersionMajor) +
                    "-") != std::string::npos);
}

// (a): DIFFERENT geometry -> DIFFERENT filenames.
TEST_CASE("fingerprint distinguishes differing geometry", "[fingerprint]") {
  const std::string p = tmp("cyclone.dat");

  SECTION("mmap vs non-mmap flips the hash") {
    const std::string mp =
        fingerprint_cache_path(p, 256 * kMB, 0, /*mmap_directory=*/true);
    const std::string sp =
        fingerprint_cache_path(p, 256 * kMB, 0, /*mmap_directory=*/false);
    REQUIRE(mp != sp);
  }

  SECTION("sizes across a stripe-count boundary differ (explicit stripes)") {
    // Explicit 128MB stripes: 160MB -> 1 stripe, 300MB -> 2 stripes.
    const std::string one_stripe = fingerprint_cache_path(
        p, 160 * kMB, /*stripe_size=*/kMinStripeSize, true);
    const std::string two_stripes = fingerprint_cache_path(
        p, 300 * kMB, /*stripe_size=*/kMinStripeSize, true);
    REQUIRE(one_stripe != two_stripes);
  }

  SECTION("auto-stripe sizes across a count boundary differ") {
    // Auto granularity 32MB: 40MB -> 1 stripe, 512MB -> capped multi-stripe.
    const std::string small = fingerprint_cache_path(p, 40 * kMB, 0, true);
    const std::string big = fingerprint_cache_path(p, 512 * kMB, 0, true);
    REQUIRE(small != big);
  }
}

// (b): SAME derived geometry -> SAME filename, even at different raw sizes.
TEST_CASE("fingerprint collapses same-geometry sizes", "[fingerprint]") {
  const std::string p = tmp("cyclone.dat");
  // Both land in the explicit-stripe 128MB single-stripe band [128MB, 256MB).
  const std::string a =
      fingerprint_cache_path(p, 160 * kMB, kMinStripeSize, true);
  const std::string b =
      fingerprint_cache_path(p, 200 * kMB, kMinStripeSize, true);
  REQUIRE(a == b);
}

// (c): the ".small" sibling gets its OWN, distinct geohash.
TEST_CASE("small-tier sibling fingerprints independently", "[fingerprint]") {
  const std::string base = tmp("cyclone.dat");
  // Mirrors Cache::add_volume: the small volume arrives path-suffixed ".small"
  // with its own small size and stripe_size == kMinStripeSize.
  const std::string def = fingerprint_cache_path(base, 900 * kMB, 0, true);
  const std::string small =
      fingerprint_cache_path(base + ".small", 128 * kMB, kMinStripeSize, true);
  REQUIRE(def != small);
  // Neither is simply the other with ".small" appended: they are independently
  // hashed, so the small volume never collides with the default's ring.
  REQUIRE(small != def + ".small");
  REQUIRE(def != small + ".small");
  // The small file still carries a ".small" extension after fingerprinting.
  REQUIRE(fs::path(small).extension().string() == ".small");
}

// (e): idempotency -- fingerprinting an already-fingerprinted path is a no-op.
TEST_CASE("fingerprint is idempotent", "[fingerprint]") {
  const std::string p = tmp("cyclone.dat");
  const std::string once = fingerprint_cache_path(p, 256 * kMB, 0, true);
  // Re-applying with the SAME params, and with DIFFERENT params, both return
  // the path unchanged (the tail already matches the fingerprint pattern).
  REQUIRE(fingerprint_cache_path(once, 256 * kMB, 0, true) == once);
  REQUIRE(fingerprint_cache_path(once, 999 * kMB, kMinStripeSize, false) ==
          once);
  // A ".small" fingerprinted path is likewise stable.
  const std::string small =
      fingerprint_cache_path(p + ".small", 128 * kMB, kMinStripeSize, true);
  REQUIRE(fingerprint_cache_path(small, 128 * kMB, kMinStripeSize, true) ==
          small);
}

// (g): path-transform edge cases (std::filesystem last-dot split).
TEST_CASE("fingerprint path-transform edge cases", "[fingerprint]") {
  const std::string fmt = std::to_string(VolumeHeader::kFormatVersionMajor);

  SECTION("no extension -> suffix appended to the whole name") {
    const std::string in = tmp("cyclone");
    const std::string out = fingerprint_cache_path(in, 256 * kMB, 0, true);
    REQUIRE(fs::path(out).extension().string().empty());
    REQUIRE(fs::path(out).stem().string().rfind("cyclone-" + fmt + "-", 0) ==
            0);
    REQUIRE(looks_fingerprinted(out));
  }

  SECTION("multi-dot name splits on the LAST dot") {
    const std::string in = tmp("x.dat.small");
    const std::string out = fingerprint_cache_path(in, 256 * kMB, 0, true);
    // stem "x.dat" + ext ".small" -> "x.dat-<fmt>-<hash>.small".
    REQUIRE(fs::path(out).extension().string() == ".small");
    const std::string stem = fs::path(out).stem().string();
    REQUIRE(stem.rfind("x.dat-" + fmt + "-", 0) == 0);
    REQUIRE(looks_fingerprinted(out));
  }

  SECTION("dotfile -> whole name is the stem, no extension") {
    const std::string in = tmp(".cache");
    const std::string out = fingerprint_cache_path(in, 256 * kMB, 0, true);
    REQUIRE(fs::path(out).extension().string().empty());
    REQUIRE(fs::path(out).filename().string().rfind(".cache-" + fmt + "-", 0) ==
            0);
  }

  SECTION("parent directory is preserved") {
    const std::string in = tmp("cyclone.dat");
    const std::string out = fingerprint_cache_path(in, 256 * kMB, 0, true);
    REQUIRE(fs::path(out).parent_path() == fs::path(in).parent_path());
  }
}

// (f) M1 regression: two raw sizes in the SAME explicit-stripe 128MB band map
// to the SAME file, and opening the second under a LIVE first peer does NOT
// reset it -- the entry the first peer wrote survives.  This is the reason the
// Volume::open_locked geometry gate drops the raw `volume_size != size` clause.
TEST_CASE("M1: same-band peers share a file without a reset", "[fingerprint]") {
  // 160MB and 200MB both derive {1 stripe, 128MB base} with explicit 128MB
  // stripes, so they fingerprint to the SAME name.
  const size_t kSizeA = 160 * kMB;
  const size_t kSizeB = 200 * kMB;
  const std::string base = unique_tmp("cyclone_m1.dat");

  const std::string fa =
      fingerprint_cache_path(base, kSizeA, kMinStripeSize, true);
  const std::string fb =
      fingerprint_cache_path(base, kSizeB, kMinStripeSize, true);
  REQUIRE(fa == fb);  // same band -> same file
  std::remove(fa.c_str());

  CacheKey key("m1-survivor");
  std::vector<std::byte> data(256, std::byte{0x5A});

  auto make = [&](size_t size) {
    CacheConfig config;
    config.set_multi_process(0, 1);
    config.set_enable_checksum(true);
    config.set_ram_cache_size(0);  // force the read through the shared volume
    auto c = Cache::create(config);
    REQUIRE(c.has_value());
    VolumeConfig vc;
    vc.path = base;
    vc.size = size;
    vc.stripe_size = kMinStripeSize;
    REQUIRE((*c)->add_volume(vc).has_value());
    return std::move(*c);
  };

  // Peer A opens at 160MB, writes an entry, and STAYS OPEN (a live peer).
  auto peer_a = make(kSizeA);
  REQUIRE(peer_a->start().has_value());
  {
    auto wh = peer_a->write_sync(key, data.size());
    REQUIRE(wh.has_value());
    REQUIRE(wh->write_sync(data).has_value());
    REQUIRE(wh->close_sync().has_value());
  }
  REQUIRE(peer_a->read_sync(key).has_value());

  // Peer B opens at 200MB (same band -> same file) UNDER the live peer A.  The
  // pre-fingerprint gate would have reset on the raw size difference; with the
  // dropped clause the derived geometry matches, so B attaches WITHOUT a reset
  // and sees A's entry through the shared mmap directory.
  {
    auto peer_b = make(kSizeB);
    REQUIRE(peer_b->start().has_value());
    REQUIRE(peer_b->read_sync(key).has_value());  // survived: no reset
    peer_b->stop();
  }

  // A's entry is still intact after B came and went.
  REQUIRE(peer_a->read_sync(key).has_value());
  peer_a->stop();

  std::remove(fa.c_str());
}

// resolve_unsized_cache_path(): the size==0 opener must find the file its
// sized current-major peers are using -- they never write the raw path.
TEST_CASE("unsized open resolves to the sized peers' fingerprinted file",
          "[fingerprint]") {
  const std::string kMajor = std::to_string(VolumeHeader::kFormatVersionMajor);
  const auto touch = [](const std::string &path) {
    FILE *f = std::fopen(path.c_str(), "wb");
    REQUIRE(f != nullptr);
    std::fclose(f);
  };

  SECTION("no siblings: raw path unchanged") {
    const std::string base = unique_tmp("cyclone_uz.dat");
    REQUIRE(resolve_unsized_cache_path(base) == base);
  }

  SECTION("current-major sibling wins over a raw legacy file") {
    const std::string base = unique_tmp("cyclone_uz.dat");
    const fs::path p(base);
    const std::string sibling =
        (p.parent_path() / (p.stem().string() + "-" + kMajor +
                            "-00112233aabbccdd" + p.extension().string()))
            .string();
    touch(base);
    touch(sibling);
    REQUIRE(resolve_unsized_cache_path(base) == sibling);
    std::remove(base.c_str());
    std::remove(sibling.c_str());
  }

  SECTION("other-major siblings are never candidates") {
    const std::string base = unique_tmp("cyclone_uz.dat");
    const fs::path p(base);
    const std::string old_major =
        std::to_string(VolumeHeader::kFormatVersionMajor - 1);
    const std::string old_sibling =
        (p.parent_path() / (p.stem().string() + "-" + old_major +
                            "-00112233aabbccdd" + p.extension().string()))
            .string();
    touch(old_sibling);
    REQUIRE(resolve_unsized_cache_path(base) == base);
    std::remove(old_sibling.c_str());
  }

  SECTION("several current-major siblings: newest mtime wins") {
    const std::string base = unique_tmp("cyclone_uz.dat");
    const fs::path p(base);
    const auto sib = [&](const std::string &hex) {
      return (p.parent_path() / (p.stem().string() + "-" + kMajor + "-" + hex +
                                 p.extension().string()))
          .string();
    };
    const std::string older = sib("aaaaaaaaaaaaaaaa");
    const std::string newer = sib("1111111111111111");  // lexically first
    touch(older);
    touch(newer);
    // Force a strictly newer mtime on `newer` regardless of fs granularity.
    fs::last_write_time(newer,
                        fs::last_write_time(older) + std::chrono::seconds(10));
    REQUIRE(resolve_unsized_cache_path(base) == newer);
    std::remove(older.c_str());
    std::remove(newer.c_str());
  }

  SECTION("end to end: a size==0 opener shares the sized peer's volume") {
    const std::string base = unique_tmp("cyclone_uz_e2e.dat");
    const CacheKey key("unsized-shared");
    std::vector<std::byte> data(128, std::byte{0x7C});

    // Sized writer (multi-process mode, like the worker) creates the
    // fingerprinted volume and stays open.
    CacheConfig wcfg;
    wcfg.set_multi_process(0, 1);
    wcfg.set_enable_checksum(true);
    wcfg.set_ram_cache_size(0);
    auto writer = Cache::create(wcfg);
    REQUIRE(writer.has_value());
    REQUIRE((*writer)->add_volume(base, 256 * kMB).has_value());
    REQUIRE((*writer)->start().has_value());
    const auto writer_files = (*writer)->volume_files();
    REQUIRE(writer_files.size() == 1);
    REQUIRE(looks_fingerprinted(writer_files[0].file_path));
    {
      auto wh = (*writer)->write_sync(key, data.size());
      REQUIRE(wh.has_value());
      REQUIRE(wh->write_sync(data).has_value());
      REQUIRE(wh->close_sync().has_value());
    }

    // Unsized reader (like the serving process) resolves to the SAME file
    // and sees the writer's entry.
    CacheConfig rcfg;
    rcfg.set_multi_process(0, 1);
    rcfg.set_enable_checksum(true);
    rcfg.set_ram_cache_size(0);
    auto reader = Cache::create(rcfg);
    REQUIRE(reader.has_value());
    REQUIRE((*reader)->add_volume(base, 0).has_value());
    REQUIRE((*reader)->start().has_value());
    const auto reader_files = (*reader)->volume_files();
    REQUIRE(reader_files.size() == 1);
    REQUIRE(reader_files[0].file_path == writer_files[0].file_path);
    REQUIRE((*reader)->read_sync(key).has_value());

    (*reader)->stop();
    (*writer)->stop();
    std::remove(writer_files[0].file_path.c_str());
  }
}

// volume_files(): the embedder-facing map from configured path to the actual
// (fingerprinted) on-disk file.  Embedders that chmod/delete/open the volume
// file must use this -- re-deriving the name is guesswork (it depends on
// derived geometry and on whether a legacy file was adopted).
TEST_CASE("volume_files reports configured and actual on-disk paths",
          "[fingerprint]") {
  SECTION("fresh cache: fingerprinted, small-tier carve-out included") {
    const std::string base = unique_tmp("cyclone_vf.dat");

    CacheConfig config;
    config.small_tier_percent = 10;
    auto c = Cache::create(config);
    REQUIRE(c.has_value());
    REQUIRE((*c)->add_volume(base, 512 * kMB).has_value());
    REQUIRE((*c)->start().has_value());
    REQUIRE((*c)->small_tier_active());

    const auto files = (*c)->volume_files();
    REQUIRE(files.size() == 2);
    REQUIRE(files[0].configured_path == base);
    REQUIRE(files[0].tier == Tier::kDefault);
    REQUIRE(files[1].configured_path == base + ".small");
    REQUIRE(files[1].tier == Tier::kSmall);
    for (const auto &vf : files) {
      REQUIRE(vf.file_path != vf.configured_path);
      REQUIRE(looks_fingerprinted(vf.file_path));
      REQUIRE(fs::exists(vf.file_path));
    }
    // Nothing was created at the raw configured paths.
    REQUIRE_FALSE(fs::exists(base));
    REQUIRE_FALSE(fs::exists(base + ".small"));

    (*c)->stop();
    for (const auto &vf : files) {
      std::remove(vf.file_path.c_str());
    }
  }

  SECTION("size==0 open-at-on-disk-size mode keeps the raw path") {
    // Fingerprinting is a function of the CONFIGURED geometry; the size==0
    // "resolve from the file at open()" mode has none, so the path is left
    // alone (see fingerprint_cache_path) -- the one mode where an operator's
    // legacy-named file is opened under its own name.  Stage such a file by
    // creating a volume normally and renaming it to the raw legacy name.
    const std::string base = unique_tmp("cyclone_vf_legacy.dat");
    {
      CacheConfig config;
      auto c = Cache::create(config);
      REQUIRE(c.has_value());
      REQUIRE((*c)->add_volume(base, 256 * kMB).has_value());
      REQUIRE((*c)->start().has_value());
      const auto files = (*c)->volume_files();
      REQUIRE(files.size() == 1);
      (*c)->stop();
      fs::rename(files[0].file_path, base);
    }

    CacheConfig config;
    auto c = Cache::create(config);
    REQUIRE(c.has_value());
    REQUIRE((*c)->add_volume(base, 0).has_value());
    REQUIRE((*c)->start().has_value());

    const auto files = (*c)->volume_files();
    REQUIRE(files.size() == 1);
    REQUIRE(files[0].configured_path == base);
    REQUIRE(files[0].file_path == base);  // no geometry -> no rewrite
    REQUIRE(fs::exists(base));

    (*c)->stop();
    std::remove(base.c_str());
  }
}

// GOLDEN VALUES: pin the CROSS-RELEASE naming contract.  Everything else in
// this file checks properties within ONE binary; only exact expected strings
// catch a silent change to the serialization order, field widths, or FNV
// constants -- which would rename every deployment's cache file on the next
// release (cold cache + an orphaned multi-GB file per volume, with GC opt-in
// default-off).  If this test breaks, you have changed the on-disk naming
// contract: that must be a conscious decision, paired with a
// kFormatVersionMajor bump.  (Expected hashes independently recomputed from
// the documented serialization: u16 LE format major, u8 mmap flag, u64 LE
// {num_stripes, base_stripe_size, stripe_remainder}.)
//
// These values moved once, for the 6 -> 7 bump that leaves pre-depth-bound
// alternate chains behind.  They were re-derived from the serialization above
// rather than copied from the binary's output -- the re-derivation was checked
// by reproducing the previous v6 hashes (7ddd9504877110d7 / a43f01449dc2105d)
// with the format major set back to 6.
TEST_CASE("fingerprint golden values pin the naming contract",
          "[fingerprint]") {
  // Auto stripes: 256 MiB, mmap on -> geometry {8, 33550336, 32704}.
  REQUIRE(fingerprint_cache_path("cyclone.dat", 256 * kMB, 0,
                                 /*mmap_directory=*/true) ==
          "cyclone-7-0d9cd9759862d3ec.dat");
  // Explicit stripes: 512 MiB + header, 128 MiB stripes, mmap off ->
  // geometry {4, 134217728, 0}.
  REQUIRE(fingerprint_cache_path(
              "cyclone.dat", 512 * kMB + VolumeHeader::kSize, 128 * kMB,
              /*mmap_directory=*/false) == "cyclone-7-f9973954149ff08a.dat");
  // The .small sibling keeps its full "cyclone.dat" stem as the base.
  REQUIRE(fingerprint_cache_path("cyclone.dat.small",
                                 512 * kMB + VolumeHeader::kSize, 128 * kMB,
                                 /*mmap_directory=*/false) ==
          "cyclone.dat-7-f9973954149ff08a.small");
  // Below the header floor (adopt-existing-file mode, size resolved at
  // open()): the path is returned UNCHANGED -- never a garbage-geometry hash.
  REQUIRE(fingerprint_cache_path("cyclone.dat", 0, 0,
                                 /*mmap_directory=*/true) == "cyclone.dat");
}
