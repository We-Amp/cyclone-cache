// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Tail fill (issue #35; Volume::kTailFillPage).  A document larger than
// Volume::kTailFillAboveBytes is written together with zeros up to the next
// 4 KiB boundary of the file, so its write never covers only part of its
// last page (a filesystem such as ext4 would otherwise read that page from
// the device inside the pwrite when it is not cached).
//
// What each case pins:
//   * the zeros run exactly from the document's last byte to the next page
//     boundary of the file, and no further (the page after it keeps its old
//     bytes);
//   * packing is unchanged: the next document starts at the 8-byte-rounded
//     end of this one, not at the page boundary, so the fill costs no space
//     and the cursor, capacity and frontier arithmetic are what they were;
//   * a document below the threshold is written exactly as before (the bytes
//     behind it keep their old contents);
//   * every document reads back byte for byte, in both directory modes (the
//     mmap directory is the multi-process one), in flush and retention mode,
//     on the plain and the alternate write path;
//   * with wrap retention the fill never passes the clean frontier: a
//     retained document just past it, held by a borrow that blocks the
//     frontier, keeps its bytes (the zeros would otherwise land on its head).

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/document.hpp"
#include "core/volume.hpp"
#include "cyclone/alternate.hpp"
#include "cyclone/cache.hpp"
#include "cyclone/config.hpp"
#include "cyclone/key.hpp"
#include "support/temp_cache.hpp"

using namespace cyclone;

namespace {

constexpr size_t kMiB = size_t{1024} * 1024;
constexpr uint64_t kPage = Volume::kTailFillPage;
constexpr std::byte kStale{0xA5};

uint64_t page_ceil(uint64_t off) { return (off + kPage - 1) & ~(kPage - 1); }
uint64_t align8(uint64_t n) { return (n + 7) & ~uint64_t{7}; }

std::vector<std::byte> make_content(size_t seed, size_t size) {
  std::vector<std::byte> c(size);
  for (size_t i = 0; i < size; ++i) {
    c[i] = static_cast<std::byte>((seed * 131 + i * 7 + 1) & 0xFF);
  }
  return c;
}

bool content_equals(std::span<const std::byte> got,
                    std::span<const std::byte> want) {
  return got.size() == want.size() &&
         std::memcmp(got.data(), want.data(), got.size()) == 0;
}

bool put(Cache& cache, const std::string& key,
         std::span<const std::byte> content) {
  auto wh = cache.write_sync(CacheKey(key), content.size());
  if (!wh.has_value() || !wh->write_sync(content).has_value()) {
    return false;
  }
  return wh->close_sync().has_value();
}

// The volume's data file: the largest regular file in the test directory
// (add_volume fingerprints the name and adds a small sibling).
std::filesystem::path data_file(const TempCacheDir& dir) {
  std::filesystem::path best;
  uintmax_t best_size = 0;
  const auto folder = std::filesystem::path(dir.path()).parent_path();
  for (const auto& e : std::filesystem::directory_iterator(folder)) {
    if (e.is_regular_file() && e.file_size() > best_size) {
      best = e.path();
      best_size = e.file_size();
    }
  }
  REQUIRE_FALSE(best.empty());
  return best;
}

std::vector<std::byte> read_file(const std::filesystem::path& p, uint64_t off,
                                 size_t len) {
  std::vector<std::byte> out(len);
  std::ifstream in(p, std::ios::binary);
  REQUIRE(in.good());
  in.seekg(static_cast<std::streamoff>(off));
  in.read(reinterpret_cast<char*>(out.data()),
          static_cast<std::streamsize>(len));
  REQUIRE(in.gcount() == static_cast<std::streamsize>(len));
  return out;
}

bool all_equal(std::span<const std::byte> bytes, std::byte v) {
  return std::all_of(bytes.begin(), bytes.end(),
                     [v](std::byte b) { return b == v; });
}

// Records each write's reservation (the tear-gate seam fires once per fill,
// after the slot is reserved and before the bytes are written).
struct SlotRecorder {
  std::vector<std::pair<uint64_t, uint64_t>> slots;  // (offset, new cursor)
  SlotRecorder() {
    Volume::s_write_tear_gate_for_test = [this](uint64_t wo, uint64_t np) {
      slots.emplace_back(wo, np);
    };
  }
  ~SlotRecorder() { Volume::s_write_tear_gate_for_test = {}; }
  SlotRecorder(const SlotRecorder&) = delete;
  SlotRecorder& operator=(const SlotRecorder&) = delete;
};

std::unique_ptr<Cache> open_cache(const TempCacheDir& dir, bool mmap_dir,
                                  bool retention, size_t size) {
  CacheConfig cfg;
  cfg.set_ram_cache_size(0);
  cfg.set_enable_checksum(true);
  cfg.verify_checksum_on_read = true;
  cfg.set_wrap_retention(retention);
  cfg.read_lease_duration = std::chrono::milliseconds(0);
  cfg.lease_wrap_ceiling = std::chrono::milliseconds(0);
  if (mmap_dir) {
    cfg.set_multi_process(0, 1);
  }
  auto created = Cache::create(cfg);
  REQUIRE(created.has_value());
  auto cache = std::move(*created);
  REQUIRE(cache->add_volume(dir.path(), size).has_value());
  REQUIRE(cache->start().has_value());
  REQUIRE(cache->stats().stripe_count == 1);
  return cache;
}

// Fills one whole pass with small documents (below the threshold, so no
// tail fill of their own) made of kStale bytes, so the next pass writes over
// stale, non-zero bytes whose changes the checks below can see.
void fill_stale_pass(Cache& cache) {
  const std::vector<std::byte> stale(size_t{16} * 1024, kStale);
  for (size_t i = 0; cache.stats().write_buffer_wraps == 0; ++i) {
    REQUIRE(i < 100000);
    REQUIRE(put(cache, "stale-" + std::to_string(i), stale));
  }
}

// What a write changed in the file around its document: a snapshot of
// [from, from + len) taken before the write, compared with the file after.
struct Around {
  uint64_t from = 0;
  std::vector<std::byte> before;
};

Around snapshot_around(const std::filesystem::path& file, uint64_t from,
                       uint64_t len) {
  const auto size = static_cast<uint64_t>(std::filesystem::file_size(file));
  REQUIRE(from + len <= size);
  return Around{from, read_file(file, from, len)};
}

// The bytes of [off, off + len) as they were before the write.
std::span<const std::byte> before_of(const Around& a, uint64_t off,
                                     size_t len) {
  REQUIRE(off >= a.from);
  REQUIRE(off - a.from + len <= a.before.size());
  return std::span<const std::byte>(a.before).subspan(off - a.from, len);
}

}  // namespace

TEST_CASE(
    "Tail fill: a large document's write ends on a page boundary; small "
    "documents and packing are unchanged",
    "[tail_fill][write]") {
  for (const bool mmap_dir : {false, true}) {
    for (const bool retention : {false, true}) {
      CAPTURE(mmap_dir, retention);
      TempCacheDir tmp("tail_fill");
      auto cache = open_cache(tmp, mmap_dir, retention, 32 * kMiB);
      fill_stale_pass(*cache);
      const auto file = data_file(tmp);

      // Content sizes on both sides of the threshold, none a multiple of 8
      // or of the page.
      const size_t kHead = Document::kHeaderSize;
      const std::vector<size_t> sizes = {
          Volume::kTailFillAboveBytes - kHead + 1,  // one byte above it
          Volume::kTailFillAboveBytes - kHead,      // exactly at it
          3000,
          size_t{2} * kMiB + 13,
          size_t{512} * 1024 + 5,
          100001,
          777,
          size_t{64} * 1024 * 3 + 1,
      };
      SlotRecorder rec;
      REQUIRE(put(*cache, "probe", make_content(99, 100)));  // small
      uint64_t cursor = rec.slots.back().second;
      size_t zeroed_stale = 0;  // fills that zeroed bytes that were not zero
      for (size_t k = 0; k < sizes.size(); ++k) {
        CAPTURE(k, sizes[k]);
        const auto content = make_content(k, sizes[k]);
        const uint64_t doc_len = kHead + sizes[k];
        const auto around =
            snapshot_around(file, cursor, align8(doc_len) + 2 * kPage);
        const size_t before = rec.slots.size();
        REQUIRE(put(*cache, "doc-" + std::to_string(k), content));
        REQUIRE(rec.slots.size() == before + 1);
        const auto [wo, np] = rec.slots.back();
        // Packed: this document starts where the previous cursor ended, not
        // at a page boundary, and the cursor advances by the 8-byte-rounded
        // document, as before.
        REQUIRE(wo == cursor);
        REQUIRE(np == wo + align8(doc_len));
        cursor = np;

        const uint64_t doc_end = wo + doc_len;
        const uint64_t boundary = page_ceil(doc_end);
        if (doc_len > Volume::kTailFillAboveBytes) {
          // Zeros from the last byte to the page boundary...
          const size_t n = boundary - doc_end;
          REQUIRE(all_equal(read_file(file, doc_end, n), std::byte{0}));
          if (!all_equal(before_of(around, doc_end, n), std::byte{0})) {
            ++zeroed_stale;
          }
          // ...and not one byte further: the next page is untouched.
          REQUIRE(content_equals(read_file(file, boundary, kPage),
                                 before_of(around, boundary, kPage)));
        } else {
          // Small: nothing behind the document changed.
          REQUIRE(content_equals(read_file(file, doc_end, kPage),
                                 before_of(around, doc_end, kPage)));
        }
      }
      REQUIRE(zeroed_stale >= 3);  // the zeros were written, not inherited
      for (size_t k = 0; k < sizes.size(); ++k) {
        CAPTURE(k);
        auto rh = cache->read_sync(CacheKey("doc-" + std::to_string(k)));
        REQUIRE(rh.has_value());
        REQUIRE(content_equals(rh->content(), make_content(k, sizes[k])));
      }
      cache->stop();
    }
  }
}

TEST_CASE("Tail fill: the alternate write path fills the tail too",
          "[tail_fill][write][alternate]") {
  for (const bool mmap_dir : {false, true}) {
    CAPTURE(mmap_dir);
    TempCacheDir tmp("tail_fill_alt");
    auto cache = open_cache(tmp, mmap_dir, /*retention=*/true, 32 * kMiB);
    fill_stale_pass(*cache);
    const auto file = data_file(tmp);

    SlotRecorder rec;
    REQUIRE(put(*cache, "probe", make_content(99, 100)));  // small
    const uint64_t cursor = rec.slots.back().second;
    const auto content = make_content(7, 200003);
    const auto around =
        snapshot_around(file, cursor, content.size() + 4 * kPage);
    auto wh = cache->write_alternate_sync(CacheKey("alt"), AlternateId{3},
                                          content.size());
    REQUIRE(wh.has_value());
    REQUIRE(wh->write_sync(content).has_value());
    REQUIRE(wh->close_sync().has_value());
    const auto [wo, np] = rec.slots.back();
    REQUIRE(wo == cursor);
    // The alternate document's header carries alternate metadata, so take
    // its end from the cursor: np is the 8-rounded end, and the rounding
    // bytes before it are zeroed too.
    const uint64_t boundary = page_ceil(np);
    REQUIRE(boundary > np);  // this size leaves a partial last page
    REQUIRE(all_equal(read_file(file, np, boundary - np), std::byte{0}));
    REQUIRE(content_equals(read_file(file, boundary, kPage),
                           before_of(around, boundary, kPage)));
    auto rh = cache->read_sync(CacheKey("alt"));
    REQUIRE(rh.has_value());
    REQUIRE(content_equals(rh->content(), content));
    cache->stop();
  }
}

// With wrap retention the fill is clamped to the clean frontier.  Geometry:
// a 12 MiB single-stripe volume has 1 MiB chunks, and 128 KiB documents
// written back to back from the start of the data area S put eight in each
// chunk, so pass-0 document 8 starts exactly at the chunk-1 boundary.  A
// borrow on it blocks every frontier advance into chunk 1.  Pass-1
// documents 0..7 then end exactly at that boundary; the last one's page
// boundary lies inside document 8 whenever S is not page-aligned (it is
// not: the stripe starts behind the 64-byte volume header), so an unclamped
// fill would zero the head of a retained document a reader holds.
TEST_CASE(
    "Tail fill: with wrap retention the fill stops at the clean frontier, "
    "sparing a borrowed retained document",
    "[tail_fill][retention][lease]") {
  for (const bool mmap_dir : {false, true}) {
    CAPTURE(mmap_dir);
    TempCacheDir tmp("tail_fill_ret");
    CacheConfig cfg;
    cfg.set_ram_cache_size(0);
    cfg.set_enable_checksum(true);
    cfg.set_wrap_retention(true);
    cfg.read_lease_duration = std::chrono::milliseconds(600000);
    cfg.lease_wrap_ceiling = std::chrono::milliseconds(600000);
    if (mmap_dir) {
      cfg.set_multi_process(0, 1);
    }
    auto created = Cache::create(cfg);
    REQUIRE(created.has_value());
    auto cache = std::move(*created);
    REQUIRE(cache->add_volume(tmp.path(), size_t{12} * kMiB).has_value());
    REQUIRE(cache->start().has_value());
    const auto st0 = cache->stats();
    REQUIRE(st0.stripe_count == 1);
    const uint64_t area = st0.stripe_bytes - st0.current_bytes;
    const FrontierGeometry geom = retention_geometry(area);
    REQUIRE(geom.chunk_size == kMiB);

    constexpr uint64_t kDoc = uint64_t{128} * 1024;
    static_assert(kDoc > Volume::kTailFillAboveBytes);
    const size_t content_size = kDoc - Document::kHeaderSize;
    const uint64_t per_pass = area / kDoc;
    const uint64_t per_chunk = geom.chunk_size / kDoc;
    auto key = [](uint64_t pass, uint64_t i) {
      return "p" + std::to_string(pass) + "-" + std::to_string(i);
    };
    auto body = [&](uint64_t pass, uint64_t i) {
      return make_content(pass * 100003 + i, content_size);
    };
    for (uint64_t i = 0; i < per_pass; ++i) {
      REQUIRE(put(*cache, key(0, i), body(0, i)));
    }
    REQUIRE(cache->stats().write_buffer_wraps == 0);

    SlotRecorder rec;
    REQUIRE(put(*cache, key(1, 0), body(1, 0)));  // wraps; frontier at 1
    REQUIRE(cache->stats().write_buffer_wraps == 1);
    const uint64_t s_abs = rec.slots.front().first;
    // The case has teeth only if the chunk boundary is not a page boundary.
    REQUIRE((s_abs + geom.chunk_size) % kPage != 0);

    std::optional<ReadHandle> held;
    {
      auto r = cache->read_sync(CacheKey(key(0, per_chunk)));  // chunk 1
      REQUIRE(r.has_value());
      held.emplace(std::move(*r));
    }
    REQUIRE(content_equals(held->content(), body(0, per_chunk)));

    for (uint64_t i = 1; i < per_chunk; ++i) {
      REQUIRE(put(*cache, key(1, i), body(1, i)));
    }
    // The last write ended exactly on the frontier the borrow holds there.
    REQUIRE(rec.slots.back().second == s_abs + geom.chunk_size);
    REQUIRE(cache->stats().early_advances_skipped >= 1);

    // The held document is intact, and so is a fresh read of it (its CRC
    // is verified again: a fresh borrow of a retained document).
    REQUIRE(content_equals(held->content(), body(0, per_chunk)));
    held.reset();
    {
      auto again = cache->read_sync(CacheKey(key(0, per_chunk)));
      REQUIRE(again.has_value());
      REQUIRE(content_equals(again->content(), body(0, per_chunk)));
    }
    for (uint64_t i = 0; i < per_chunk; ++i) {
      auto rh = cache->read_sync(CacheKey(key(1, i)));
      REQUIRE(rh.has_value());
      REQUIRE(content_equals(rh->content(), body(1, i)));
    }
    cache->stop();
  }
}
