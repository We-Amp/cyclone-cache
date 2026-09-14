// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Enforcement of CacheConfig::max_object_size.
//
// The field was declared but never read: any object size committed.  The
// bound is now enforced at the volume write entry, for originals AND
// alternates, at the two points where a length is known:
//
//   * open time -- a declared content_length over the bound fails the
//     write_sync/write_alternate_sync call itself (the async open_write
//     delegates to write_sync, and the C API sized write routes through it,
//     so both are covered by the same check);
//   * stream time -- the shared write-handle base fails the first chunk
//     that would push the running total over the bound, BEFORE the chunk
//     is buffered (unsized opens, and sized opens that under-declare).
//
// What each case is for:
//
//   * PLAIN WRITE — declared-over rejects at open (nothing written), at-bound
//     commits and serves its bytes, streamed-over rejects mid-stream,
//     streamed-at-bound commits, and 0 disables the bound.
//   * ALTERNATE WRITE — the same contract on the alternate path.
//   * RAISED BOUND — the bound is config-driven: a limit that rejects an
//     object admits it once raised.
//   * ASYNC — open_write delegates to write_sync, pinned so that stays true.
//   * C API — cyclone_cache_write routes through the same open-time check
//     against the default 64 MB bound a zero-initialised C config keeps
//     (the knob itself is covered in tests/unit/test_c_api.cpp).

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include "cyclone/alternate.hpp"
#include "cyclone/cache.hpp"
#include "cyclone/config.hpp"
#include "cyclone/cyclone_c.h"
#include "cyclone/key.hpp"
#include "support/temp_cache.hpp"

using namespace cyclone;

namespace {

constexpr size_t kMB = static_cast<size_t>(1024) * 1024;
constexpr size_t kVolumeBytes = 256 * kMB;

// RAM cache OFF everywhere in this file: the read-back oracles must measure
// the committed disk write, not a RAM copy admitted before it.
CacheConfig bound_config(size_t max_object_size) {
  CacheConfig c;
  c.ram_cache_size = 0;
  c.max_object_size = max_object_size;
  return c;
}

std::unique_ptr<Cache> make_cache(const TempCacheDir& dir,
                                  const CacheConfig& cfg,
                                  size_t stripe_size = 0) {
  auto created = Cache::create(cfg);
  REQUIRE(created.has_value());
  auto cache = std::move(*created);
  VolumeConfig vc;
  vc.path = dir.path();
  vc.size = kVolumeBytes;
  vc.stripe_size = stripe_size;
  REQUIRE(cache->add_volume(vc).has_value());
  REQUIRE(cache->start().has_value());
  return cache;
}

// Patterned content: byte i is (i * 31 + 7) & 0xFF, so a read-back can be
// verified for identity, not just length.
std::vector<std::byte> patterned(size_t size) {
  std::vector<std::byte> v(size);
  for (size_t i = 0; i < size; ++i) {
    v[i] = static_cast<std::byte>((i * 31 + 7) & 0xFF);
  }
  return v;
}

bool content_matches(std::span<const std::byte> got) {
  for (size_t i = 0; i < got.size(); ++i) {
    if (got[i] != static_cast<std::byte>((i * 31 + 7) & 0xFF)) {
      return false;
    }
  }
  return true;
}

}  // namespace

TEST_CASE("Plain writes enforce max_object_size", "[object_size][regression]") {
  const CacheKey key("object-size-plain");

  SECTION("declared length over the bound fails at open, nothing written") {
    TempCacheDir tmp("obj_plain_over");
    auto cache = make_cache(tmp, bound_config(kMB));

    auto wh = cache->write_sync(key, kMB + 1);
    REQUIRE_FALSE(wh.has_value());
    REQUIRE(wh.error() == CacheError::ObjectTooLarge);

    auto exists = cache->exists_sync(key);
    REQUIRE(exists.has_value());
    REQUIRE_FALSE(*exists);
    cache->stop();
  }

  SECTION("declared length at exactly the bound commits and serves") {
    TempCacheDir tmp("obj_plain_at");
    auto cache = make_cache(tmp, bound_config(kMB));

    const auto content = patterned(kMB);
    auto wh = cache->write_sync(key, content.size());
    REQUIRE(wh.has_value());
    REQUIRE(wh->write_sync(std::span<const std::byte>(content)).has_value());
    REQUIRE(wh->close_sync().has_value());

    auto rh = cache->read_sync(key);
    REQUIRE(rh.has_value());
    REQUIRE(rh->content().size() == kMB);
    REQUIRE(content_matches(rh->content()));
    cache->stop();
  }

  SECTION("streamed write fails at the chunk that crosses the bound") {
    TempCacheDir tmp("obj_plain_stream");
    auto cache = make_cache(tmp, bound_config(kMB));

    const auto chunk = patterned(kMB / 2);
    auto wh = cache->write_sync(key);  // unsized open: bound hits mid-stream
    REQUIRE(wh.has_value());
    REQUIRE(wh->write_sync(std::span<const std::byte>(chunk)).has_value());
    REQUIRE(wh->write_sync(std::span<const std::byte>(chunk)).has_value());
    auto third = wh->write_sync(std::span<const std::byte>(chunk));
    REQUIRE_FALSE(third.has_value());
    REQUIRE(third.error() == CacheError::ObjectTooLarge);
    wh->abort();

    auto exists = cache->exists_sync(key);
    REQUIRE(exists.has_value());
    REQUIRE_FALSE(*exists);
    cache->stop();
  }

  SECTION("streamed write at exactly the bound commits") {
    TempCacheDir tmp("obj_plain_stream_at");
    auto cache = make_cache(tmp, bound_config(kMB));

    const auto chunk = patterned(kMB / 2);
    auto wh = cache->write_sync(key);
    REQUIRE(wh.has_value());
    REQUIRE(wh->write_sync(std::span<const std::byte>(chunk)).has_value());
    REQUIRE(wh->write_sync(std::span<const std::byte>(chunk)).has_value());
    REQUIRE(wh->close_sync().has_value());

    auto rh = cache->read_sync(key);
    REQUIRE(rh.has_value());
    REQUIRE(rh->content().size() == kMB);
    REQUIRE(content_matches(rh->content()));
    cache->stop();
  }

  SECTION("max_object_size = 0 disables the bound") {
    TempCacheDir tmp("obj_plain_unbounded");
    // Single stripe: a document must fit one stripe's data area, so the
    // auto-sized stripes (a fraction of the volume) would reject a 64 MB+
    // object on geometry grounds regardless of the bound under test.
    auto cache = make_cache(tmp, bound_config(0), kVolumeBytes);

    // Past the DEFAULT 64 MB: proves the disable, not just a loose bound.
    const auto content = patterned(64 * kMB + 1);
    auto wh = cache->write_sync(key, content.size());
    REQUIRE(wh.has_value());
    REQUIRE(wh->write_sync(std::span<const std::byte>(content)).has_value());
    REQUIRE(wh->close_sync().has_value());
    cache->stop();
  }
}

TEST_CASE("Alternate writes enforce max_object_size",
          "[object_size][alternate][regression]") {
  const CacheKey key("object-size-alt");

  SECTION("declared length over the bound fails at open") {
    TempCacheDir tmp("obj_alt_over");
    auto cache = make_cache(tmp, bound_config(kMB));

    auto wh = cache->write_alternate_sync(key, AlternateId::Brotli, kMB + 1);
    REQUIRE_FALSE(wh.has_value());
    REQUIRE(wh.error() == CacheError::ObjectTooLarge);
    cache->stop();
  }

  SECTION("declared length at exactly the bound commits") {
    TempCacheDir tmp("obj_alt_at");
    auto cache = make_cache(tmp, bound_config(kMB));

    const auto content = patterned(kMB);
    auto wh =
        cache->write_alternate_sync(key, AlternateId::Brotli, content.size());
    REQUIRE(wh.has_value());
    REQUIRE(wh->write_sync(std::span<const std::byte>(content)).has_value());
    REQUIRE(wh->close_sync().has_value());
    cache->stop();
  }

  SECTION("streamed alternate write fails at the crossing chunk") {
    TempCacheDir tmp("obj_alt_stream");
    auto cache = make_cache(tmp, bound_config(kMB));

    const auto chunk = patterned(kMB / 2);
    auto wh = cache->write_alternate_sync(key, AlternateId::Brotli, 0);
    REQUIRE(wh.has_value());
    REQUIRE(wh->write_sync(std::span<const std::byte>(chunk)).has_value());
    REQUIRE(wh->write_sync(std::span<const std::byte>(chunk)).has_value());
    auto third = wh->write_sync(std::span<const std::byte>(chunk));
    REQUIRE_FALSE(third.has_value());
    REQUIRE(third.error() == CacheError::ObjectTooLarge);
    wh->abort();
    cache->stop();
  }
}

TEST_CASE("A raised max_object_size admits the object a lower bound rejected",
          "[object_size][regression]") {
  const CacheKey key("object-size-raised");
  const auto content = patterned(2 * kMB);

  {
    TempCacheDir tmp("obj_raised_low");
    auto cache = make_cache(tmp, bound_config(kMB));
    auto wh = cache->write_sync(key, content.size());
    REQUIRE_FALSE(wh.has_value());
    REQUIRE(wh.error() == CacheError::ObjectTooLarge);
    cache->stop();
  }

  {
    TempCacheDir tmp("obj_raised_high");
    auto cache = make_cache(tmp, bound_config(4 * kMB));
    auto wh = cache->write_sync(key, content.size());
    REQUIRE(wh.has_value());
    REQUIRE(wh->write_sync(std::span<const std::byte>(content)).has_value());
    REQUIRE(wh->close_sync().has_value());

    auto rh = cache->read_sync(key);
    REQUIRE(rh.has_value());
    REQUIRE(rh->content().size() == 2 * kMB);
    REQUIRE(content_matches(rh->content()));
    cache->stop();
  }
}

TEST_CASE("Async open_write enforces max_object_size",
          "[object_size][async][regression]") {
  // open_write delegates to write_sync; pin that the bound holds on the
  // async entry point too, so a future refactor cannot widen the hole.
  TempCacheDir tmp("obj_async");
  auto cache = make_cache(tmp, bound_config(kMB));
  const CacheKey key("object-size-async");

  auto task = cache->open_write(key, kMB + 1);
  auto wh = task.sync_wait();
  REQUIRE_FALSE(wh.has_value());
  REQUIRE(wh.error() == CacheError::ObjectTooLarge);
  cache->stop();
}

TEST_CASE("C API write over the default max_object_size is rejected",
          "[object_size][c_api][regression]") {
  // A zero-initialised C config leaves max_object_size at its 0 sentinel,
  // which the C sentinel maps to the CacheConfig default (64 MB).  A sized
  // write past it must fail with the mapped error; a small write must still
  // succeed.
  TempCacheDir tmp("obj_c_api");
  std::string cache_path = tmp.path();
  CycloneCacheConfig config{};
  config.cache_path = cache_path.c_str();
  config.cache_size_bytes = static_cast<uint64_t>(kVolumeBytes);
  config.ram_cache_size_bytes = 0;
  config.enable_checksum = 1;
  config.num_segments = 2;

  CycloneCacheHandle* cache = nullptr;
  REQUIRE(cyclone_cache_create(&config, &cache) == CYCLONE_OK);

  const char* key = "c-api-object-size";
  std::vector<char> big(64 * kMB + 1, 'x');
  REQUIRE(cyclone_cache_write(cache, key, std::strlen(key), big.data(),
                              big.size()) == CYCLONE_OBJECT_TOO_LARGE);

  const char* data = "small enough";
  REQUIRE(cyclone_cache_write(cache, key, std::strlen(key), data,
                              std::strlen(data)) == CYCLONE_OK);

  cyclone_cache_destroy(cache);
}
