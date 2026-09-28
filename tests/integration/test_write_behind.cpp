// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Write-behind (issue #43; CacheConfig::write_behind,
// doc/design/writer-admission-control.md).  With it on, every document above
// 64 KiB has its write-back started right after its commit, over exactly the
// range its fill wrote, with no stripe mutex or write lock held.  The kernel
// call is Linux-only; Volume::s_write_behind_for_test fires on every platform
// just before it, so these checks run everywhere.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <future>
#include <string>
#include <utility>
#include <vector>

#include "core/volume.hpp"
#include "cyclone/cache.hpp"
#include "cyclone/key.hpp"
#include "support/temp_cache.hpp"

using namespace cyclone;

namespace {

constexpr size_t kKiB = 1024;
constexpr size_t kMiB = 1024 * kKiB;
constexpr uint64_t kPage = 4096;

uint64_t round_up_page(uint64_t v) { return (v + kPage - 1) & ~(kPage - 1); }

std::vector<std::byte> body(size_t size, int seed) {
  std::vector<std::byte> b(size);
  for (size_t i = 0; i < size; ++i) {
    b[i] = static_cast<std::byte>((i * 131 + static_cast<size_t>(seed)) & 0xFF);
  }
  return b;
}

bool put(Cache& cache, const std::string& key,
         const std::vector<std::byte>& content) {
  auto wh = cache.write_sync(CacheKey(key), content.size());
  if (!wh.has_value() ||
      !wh->write_sync(std::span<const std::byte>(content)).has_value()) {
    return false;
  }
  return wh->close_sync().has_value();
}

bool reads_back(Cache& cache, const std::string& key,
                const std::vector<std::byte>& content) {
  auto rh = cache.read_sync(CacheKey(key));
  if (!rh.has_value()) {
    return false;
  }
  const auto got = rh->content();
  return got.size() == content.size() &&
         std::equal(got.begin(), got.end(), content.begin());
}

std::unique_ptr<Cache> open_cache(const TempCacheDir& dir, bool mmap_dir,
                                  bool fill, bool write_behind) {
  CacheConfig cfg;
  cfg.set_ram_cache_size(0);
  cfg.set_fill_large_document_tail(fill);
  cfg.set_write_behind(write_behind);
  if (mmap_dir) {
    cfg.set_multi_process(0, 1);
  }
  auto created = Cache::create(cfg);
  REQUIRE(created.has_value());
  auto cache = std::move(*created);
  REQUIRE(cache->add_volume(dir.path(), 32 * kMiB).has_value());
  REQUIRE(cache->start().has_value());
  REQUIRE(cache->stats().stripe_count == 1);
  return cache;
}

// Records every write's reservation and every write-behind range.
struct Recorder {
  std::vector<std::pair<uint64_t, uint64_t>> slots;   // (offset, new cursor)
  std::vector<std::pair<uint64_t, uint64_t>> ranges;  // (offset, length)
  Recorder() {
    Volume::s_write_tear_gate_for_test = [this](uint64_t wo, uint64_t np) {
      slots.emplace_back(wo, np);
    };
    Volume::s_write_behind_for_test = [this](uint64_t off, uint64_t len) {
      ranges.emplace_back(off, len);
    };
  }
  ~Recorder() {
    Volume::s_write_tear_gate_for_test = {};
    Volume::s_write_behind_for_test = {};
  }
  Recorder(const Recorder&) = delete;
  Recorder& operator=(const Recorder&) = delete;
};

}  // namespace

TEST_CASE("Write-behind: off by default, no range is started",
          "[write_behind][write]") {
  REQUIRE_FALSE(CacheConfig{}.write_behind);
  TempCacheDir tmp("write_behind_off");
  auto cache = open_cache(tmp, /*mmap_dir=*/false, /*fill=*/true,
                          /*write_behind=*/false);
  Recorder rec;
  const auto big = body(300 * kKiB, 1);
  REQUIRE(put(*cache, "big", big));
  REQUIRE(reads_back(*cache, "big", big));
  REQUIRE(rec.slots.size() == 1);
  REQUIRE(rec.ranges.empty());
  REQUIRE(cache->stats().write_behind_ranges == 0);
  cache->stop();
}

TEST_CASE("Write-behind: large documents only, over exactly the filled range",
          "[write_behind][write]") {
  for (const bool mmap_dir : {false, true}) {
    for (const bool fill : {false, true}) {
      CAPTURE(mmap_dir, fill);
      TempCacheDir tmp("write_behind_on");
      auto cache = open_cache(tmp, mmap_dir, fill, /*write_behind=*/true);
      Recorder rec;
      size_t large = 0;
      for (const size_t size :
           {size_t{1000}, size_t{64} * kKiB - Document::kHeaderSize,
            size_t{64} * kKiB, size_t{300} * kKiB + 13, size_t{2} * kMiB + 5,
            size_t{4096}}) {
        CAPTURE(size);
        const auto content = body(size, static_cast<int>(size & 0xFF));
        const std::string key = "k" + std::to_string(size);
        const size_t before = rec.ranges.size();
        REQUIRE(put(*cache, key, content));
        REQUIRE(reads_back(*cache, key, content));
        const auto [wo, new_pos] = rec.slots.back();
        const uint64_t doc_len = new_pos - wo;  // 8-rounded document size
        if (doc_len <= 64 * kKiB) {
          REQUIRE(rec.ranges.size() == before);  // small: never started
          continue;
        }
        ++large;
        REQUIRE(rec.ranges.size() == before + 1);
        const auto [off, len] = rec.ranges.back();
        REQUIRE(off == wo);
        if (fill) {
          // Runs through the tail fill: past the cursor, up to the next page
          // boundary of the file (less if the frontier clamps the fill).
          REQUIRE(off + len >= new_pos);
          REQUIRE(off + len <= round_up_page(new_pos));
        } else {
          // Exactly the document's bytes (its unrounded size).
          REQUIRE(len > doc_len - 8);
          REQUIRE(len <= doc_len);
        }
      }
      REQUIRE(large == 3);
#ifdef __linux__
      REQUIRE(cache->stats().write_behind_ranges == large);
#else
      REQUIRE(cache->stats().write_behind_ranges == 0);  // no kernel call
#endif
      cache->stop();
    }
  }
}

TEST_CASE("Write-behind: runs with no stripe mutex or write lock held",
          "[write_behind][write][concurrent]") {
  for (const bool mmap_dir : {false, true}) {
    CAPTURE(mmap_dir);
    TempCacheDir tmp("write_behind_unlocked");
    auto cache = open_cache(tmp, mmap_dir, /*fill=*/true,
                            /*write_behind=*/true);
    const auto big = body(200 * kKiB, 7);
    const auto small = body(2000, 8);
    // Inside the first write-behind, a second writer commits to the same
    // (only) stripe and a reader reads.  Were the stripe mutex or the
    // cross-process write lock still held, the writer would block and the
    // wait below would time out.
    bool nested_done = false;
    bool first = true;
    Volume::s_write_behind_for_test = [&](uint64_t, uint64_t) {
      if (!first) {
        return;
      }
      first = false;
      auto other = std::async(std::launch::async, [&] {
        return put(*cache, "other", small) && reads_back(*cache, "big", big);
      });
      nested_done = other.wait_for(std::chrono::seconds(10)) ==
                        std::future_status::ready &&
                    other.get();
    };
    REQUIRE(put(*cache, "big", big));
    Volume::s_write_behind_for_test = {};
    REQUIRE(nested_done);
    REQUIRE(reads_back(*cache, "other", small));
    cache->stop();
  }
}

TEST_CASE("Write-behind: the alternate commit path starts it too",
          "[write_behind][write][alternate]") {
  TempCacheDir tmp("write_behind_alt");
  auto cache = open_cache(tmp, /*mmap_dir=*/true, /*fill=*/true,
                          /*write_behind=*/true);
  Recorder rec;
  const auto big = body(256 * kKiB, 3);
  const CacheKey key("alt");
  auto wh = cache->write_alternate_sync(key, AlternateId::Original, big.size());
  REQUIRE(wh.has_value());
  REQUIRE(wh->write_sync(std::span<const std::byte>(big)).has_value());
  REQUIRE(wh->close_sync().has_value());
  REQUIRE(rec.ranges.size() == 1);
  const auto [off, len] = rec.ranges.back();
  const auto [wo, new_pos] = rec.slots.back();
  REQUIRE(off == wo);
  REQUIRE(off + len >= new_pos);
  REQUIRE(off + len <= round_up_page(new_pos));
  cache->stop();
}
