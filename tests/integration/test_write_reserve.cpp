// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// WriteHandle::reserve() (issue #16): the zero-copy form of write_sync().
// The caller fills the returned span in place; the commit stores the buffer
// as it stands at close, checksum included.
//
// What each case pins:
//   * a reserved-and-filled object commits and reads back byte for byte, in
//     both directory modes (in-memory, and the mmap directory the KV tier
//     uses), with the checksum verified on read;
//   * reserve() and write_sync() mix, in call order;
//   * bytes written into the span AFTER reserve() returns, up to close, are
//     what is stored (the checksum is taken at close);
//   * abort, or destroying the handle unclosed, after a partial fill
//     publishes nothing;
//   * the write_sync() limits apply, and a refused reserve() appends
//     nothing;
//   * the alternate write path has it too.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "cyclone/alternate.hpp"
#include "cyclone/cache.hpp"
#include "cyclone/config.hpp"
#include "cyclone/key.hpp"
#include "support/temp_cache.hpp"

using namespace cyclone;

namespace {

constexpr size_t kMB = static_cast<size_t>(1024) * 1024;

std::unique_ptr<Cache> make_cache(const TempCacheDir& dir, bool mmap_dir,
                                  size_t max_object_size = 0) {
  CacheConfig cfg;
  cfg.ram_cache_size = 0;  // read back from the committed disk bytes
  cfg.max_object_size = max_object_size;
  cfg.enable_checksum = true;
  cfg.verify_checksum_on_read = true;
  if (mmap_dir) {
    cfg.set_multi_process(0, 1);
  }
  auto created = Cache::create(cfg);
  REQUIRE(created.has_value());
  auto cache = std::move(*created);
  VolumeConfig vc;
  vc.path = dir.path();
  vc.size = 64 * kMB;
  REQUIRE(cache->add_volume(vc).has_value());
  REQUIRE(cache->start().has_value());
  return cache;
}

std::byte pattern_at(size_t i) {
  return static_cast<std::byte>((i * 131 + 17) & 0xFF);
}

void fill_pattern(std::span<std::byte> dst, size_t first_index) {
  for (size_t i = 0; i < dst.size(); ++i) {
    dst[i] = pattern_at(first_index + i);
  }
}

class ExactIdSelector : public StorageAlternateSelector {
 public:
  explicit ExactIdSelector(AlternateId target) : _target(target) {}
  [[nodiscard]] std::optional<size_t> select(
      std::span<const AlternateInfo> alternates,
      const AlternateSelectionContext& /*ctx*/) const override {
    for (size_t i = 0; i < alternates.size(); ++i) {
      if (alternates[i].id == _target) {
        return i;
      }
    }
    return std::nullopt;
  }

 private:
  AlternateId _target;
};

bool matches_pattern(std::span<const std::byte> got) {
  for (size_t i = 0; i < got.size(); ++i) {
    if (got[i] != pattern_at(i)) {
      return false;
    }
  }
  return true;
}

}  // namespace

TEST_CASE("WriteHandle::reserve commits the filled span",
          "[write_reserve][write]") {
  for (const bool mmap_dir : {false, true}) {
    TempCacheDir tmp(mmap_dir ? "reserve_mmap" : "reserve_mem");
    auto cache = make_cache(tmp, mmap_dir);
    const CacheKey key("reserve-basic");
    const size_t size = 2 * kMB + 13;  // not a multiple of the page or of 8
    const std::vector<std::byte> meta(64, std::byte{0x5A});

    auto wh = cache->write_sync(key, size);
    REQUIRE(wh.has_value());
    wh->set_header(meta);
    auto span = wh->reserve(size);
    REQUIRE(span.has_value());
    REQUIRE(span->size() == size);
    REQUIRE(wh->bytes_written() == size);
    fill_pattern(*span, 0);
    REQUIRE(wh->close_sync().has_value());

    auto rh = cache->read_sync(key);
    REQUIRE(rh.has_value());
    REQUIRE(rh->content().size() == size);
    REQUIRE(matches_pattern(rh->content()));
    REQUIRE(rh->header().size() == meta.size());
    REQUIRE(std::memcmp(rh->header().data(), meta.data(), meta.size()) == 0);
    cache->stop();
  }
}

TEST_CASE("WriteHandle::reserve mixes with write_sync in call order",
          "[write_reserve][write]") {
  TempCacheDir tmp("reserve_mix");
  auto cache = make_cache(tmp, /*mmap_dir=*/true);
  const CacheKey key("reserve-mix");
  const size_t a = 1000;
  const size_t b = 70000;
  const size_t c = 333;

  std::vector<std::byte> first(a);
  fill_pattern(first, 0);
  std::vector<std::byte> last(c);
  fill_pattern(last, a + b);

  auto wh = cache->write_sync(key);  // unsized: the buffer grows as it goes
  REQUIRE(wh.has_value());
  REQUIRE(wh->write_sync(std::span<const std::byte>(first)).has_value());
  auto mid = wh->reserve(b);
  REQUIRE(mid.has_value());
  fill_pattern(*mid, a);
  REQUIRE(wh->write_sync(std::span<const std::byte>(last)).has_value());
  REQUIRE(wh->bytes_written() == a + b + c);
  REQUIRE(wh->close_sync().has_value());

  auto rh = cache->read_sync(key);
  REQUIRE(rh.has_value());
  REQUIRE(rh->content().size() == a + b + c);
  REQUIRE(matches_pattern(rh->content()));
  cache->stop();
}

TEST_CASE("WriteHandle::reserve stores what the span holds at close",
          "[write_reserve][write]") {
  TempCacheDir tmp("reserve_late");
  auto cache = make_cache(tmp, /*mmap_dir=*/true);
  const CacheKey key("reserve-late-fill");
  const size_t size = 4096;

  auto wh = cache->write_sync(key, size);
  REQUIRE(wh.has_value());
  auto span = wh->reserve(size);
  REQUIRE(span.has_value());
  std::memset(span->data(), 0xEE, span->size());  // an early draft...
  fill_pattern(*span, 0);                         // ...overwritten before close
  REQUIRE(wh->close_sync().has_value());

  // verify_checksum_on_read: a checksum taken before the second fill would
  // fail here as Corrupted rather than serve.
  auto rh = cache->read_sync(key);
  REQUIRE(rh.has_value());
  REQUIRE(matches_pattern(rh->content()));
  cache->stop();
}

TEST_CASE("WriteHandle::reserve then abort publishes nothing",
          "[write_reserve][write]") {
  TempCacheDir tmp("reserve_abort");
  auto cache = make_cache(tmp, /*mmap_dir=*/true);

  SECTION("explicit abort after a partial fill") {
    const CacheKey key("reserve-abort");
    auto wh = cache->write_sync(key, kMB);
    REQUIRE(wh.has_value());
    auto span = wh->reserve(kMB);
    REQUIRE(span.has_value());
    fill_pattern(span->first(kMB / 2), 0);
    wh->abort();
    REQUIRE_FALSE(wh->reserve(16).has_value());
    auto exists = cache->exists_sync(key);
    REQUIRE(exists.has_value());
    REQUIRE_FALSE(*exists);
  }

  SECTION("handle destroyed unclosed") {
    const CacheKey key("reserve-drop");
    {
      auto wh = cache->write_sync(key, kMB);
      REQUIRE(wh.has_value());
      auto span = wh->reserve(kMB);
      REQUIRE(span.has_value());
      fill_pattern(*span, 0);
    }
    auto exists = cache->exists_sync(key);
    REQUIRE(exists.has_value());
    REQUIRE_FALSE(*exists);
  }

  SECTION("reserve after close is refused") {
    const CacheKey key("reserve-after-close");
    auto wh = cache->write_sync(key, 8);
    REQUIRE(wh.has_value());
    auto span = wh->reserve(8);
    REQUIRE(span.has_value());
    fill_pattern(*span, 0);
    REQUIRE(wh->close_sync().has_value());
    auto again = wh->reserve(8);
    REQUIRE_FALSE(again.has_value());
    REQUIRE(again.error() == CacheError::Closed);
  }
  cache->stop();
}

TEST_CASE("WriteHandle::reserve honours max_object_size",
          "[write_reserve][write][object_size]") {
  TempCacheDir tmp("reserve_bound");
  auto cache = make_cache(tmp, /*mmap_dir=*/false, /*max_object_size=*/kMB);
  const CacheKey key("reserve-bound");

  auto wh = cache->write_sync(key);
  REQUIRE(wh.has_value());
  auto over = wh->reserve(kMB + 1);
  REQUIRE_FALSE(over.has_value());
  REQUIRE(over.error() == CacheError::ObjectTooLarge);
  REQUIRE(wh->bytes_written() == 0);  // a refused reserve appends nothing

  auto at = wh->reserve(kMB);
  REQUIRE(at.has_value());
  fill_pattern(*at, 0);
  auto one_more = wh->reserve(1);
  REQUIRE_FALSE(one_more.has_value());
  REQUIRE(one_more.error() == CacheError::ObjectTooLarge);
  REQUIRE(wh->close_sync().has_value());

  auto rh = cache->read_sync(key);
  REQUIRE(rh.has_value());
  REQUIRE(rh->content().size() == kMB);
  REQUIRE(matches_pattern(rh->content()));

  // The 4 GiB document limit, without allocating anything.
  auto wh2 = cache->write_sync(CacheKey("reserve-huge"));
  REQUIRE(wh2.has_value());
  auto huge = wh2->reserve(SIZE_MAX);
  REQUIRE_FALSE(huge.has_value());
  cache->stop();
}

TEST_CASE("WriteHandle::reserve on the alternate write path",
          "[write_reserve][write][alternate]") {
  TempCacheDir tmp("reserve_alt");
  auto cache = make_cache(tmp, /*mmap_dir=*/true);
  const CacheKey key("reserve-alt");
  const size_t size = 50000;

  auto wh = cache->write_alternate_sync(key, AlternateId::Gzip, size);
  REQUIRE(wh.has_value());
  auto span = wh->reserve(size);
  REQUIRE(span.has_value());
  fill_pattern(*span, 0);
  REQUIRE(wh->close_sync().has_value());

  const ExactIdSelector selector(AlternateId::Gzip);
  const AlternateSelectionContext ctx{};
  auto rh = cache->read_alternate_sync(key, selector, ctx);
  REQUIRE(rh.has_value());
  REQUIRE(rh->content().size() == size);
  REQUIRE(matches_pattern(rh->content()));
  cache->stop();
}
