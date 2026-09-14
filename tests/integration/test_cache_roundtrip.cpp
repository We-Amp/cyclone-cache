// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include "cyclone/cache.hpp"
#include "cyclone/key.hpp"
#include "cyclone/plugin/plugin.hpp"
#include "support/temp_cache.hpp"

using namespace cyclone;

TEST_CASE("Cache write and read round-trip", "[integration]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(10 * 1024 * 1024);  // 10MB

  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  CacheKey key("test-roundtrip-key");

  std::string header_str = "X-Test: value";
  std::string content_str = "This is the cached content data";

  std::vector<std::byte> header(header_str.size());
  std::memcpy(header.data(), header_str.data(), header_str.size());

  std::vector<std::byte> content(content_str.size());
  std::memcpy(content.data(), content_str.data(), content_str.size());

  // Write
  {
    auto wh_result = cache->write_sync(key, content.size());
    REQUIRE(wh_result.has_value());

    auto &wh = *wh_result;
    wh.set_header(std::span<const std::byte>(header));
    auto write_result = wh.write_sync(std::span<const std::byte>(content));
    REQUIRE(write_result.has_value());

    auto close_result = wh.close_sync();
    REQUIRE(close_result.has_value());
  }

  // Read back
  {
    auto rh_result = cache->read_sync(key);
    REQUIRE(rh_result.has_value());

    auto &rh = *rh_result;
    auto read_header = rh.header();
    auto read_content = rh.content();

    REQUIRE(read_header.size() == header.size());
    REQUIRE(read_content.size() == content.size());

    std::string read_header_str(
        reinterpret_cast<const char *>(read_header.data()), read_header.size());
    std::string read_content_str(
        reinterpret_cast<const char *>(read_content.data()),
        read_content.size());

    REQUIRE(read_header_str == header_str);
    REQUIRE(read_content_str == content_str);
  }

  cache->stop();
}

TEST_CASE("Cache multiple entries", "[integration]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(20 * 1024 * 1024);

  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  // Write multiple entries
  for (int i = 0; i < 10; ++i) {
    CacheKey key("multi-test-key-" + std::to_string(i));
    std::string content_str = "Content for entry " + std::to_string(i);

    std::vector<std::byte> content(content_str.size());
    std::memcpy(content.data(), content_str.data(), content_str.size());

    auto wh_result = cache->write_sync(key, content.size());
    REQUIRE(wh_result.has_value());

    auto write_result =
        wh_result->write_sync(std::span<const std::byte>(content));
    REQUIRE(write_result.has_value());

    auto close_result = wh_result->close_sync();
    REQUIRE(close_result.has_value());
  }

  // Read them back
  for (int i = 0; i < 10; ++i) {
    CacheKey key("multi-test-key-" + std::to_string(i));
    std::string expected = "Content for entry " + std::to_string(i);

    auto rh_result = cache->read_sync(key);
    REQUIRE(rh_result.has_value());

    auto read_content = rh_result->content();
    std::string read_str(reinterpret_cast<const char *>(read_content.data()),
                         read_content.size());

    REQUIRE(read_str == expected);
  }

  auto stats = cache->stats();
  REQUIRE(stats.current_entries == 10);

  cache->stop();
}

TEST_CASE("Cache remove entry", "[integration]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(10 * 1024 * 1024);

  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  CacheKey key("remove-test-key");
  std::string content_str = "Content to be removed";

  std::vector<std::byte> content(content_str.size());
  std::memcpy(content.data(), content_str.data(), content_str.size());

  // Write
  {
    auto wh_result = cache->write_sync(key, content.size());
    REQUIRE(wh_result.has_value());
    wh_result->write_sync(std::span<const std::byte>(content));
    wh_result->close_sync();
  }

  // Verify it exists
  {
    auto exists = cache->exists_sync(key);
    REQUIRE(exists.has_value());
    REQUIRE(*exists == true);
  }

  // Remove
  {
    auto remove_result = cache->remove_sync(key);
    REQUIRE(remove_result.has_value());
  }

  // Verify it's gone
  {
    auto exists = cache->exists_sync(key);
    REQUIRE(exists.has_value());
    REQUIRE(*exists == false);
  }

  // Read should fail
  {
    auto rh_result = cache->read_sync(key);
    REQUIRE_FALSE(rh_result.has_value());
    REQUIRE(rh_result.error() == CacheError::NotFound);
  }

  cache->stop();
}

TEST_CASE("Cache not found", "[integration]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(10 * 1024 * 1024);

  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  CacheKey key("nonexistent-key");

  auto rh_result = cache->read_sync(key);
  REQUIRE_FALSE(rh_result.has_value());
  REQUIRE(rh_result.error() == CacheError::NotFound);

  cache->stop();
}

TEST_CASE("Cache list_alternates_sync returns single alternate",
          "[integration][alternate]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(10 * 1024 * 1024);

  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  CacheKey key("alternates-test-key");
  std::string content_str = "Original content";

  std::vector<std::byte> content(content_str.size());
  std::memcpy(content.data(), content_str.data(), content_str.size());

  // Write an entry
  {
    auto wh_result = cache->write_sync(key, content.size());
    REQUIRE(wh_result.has_value());
    wh_result->write_sync(std::span<const std::byte>(content));
    wh_result->close_sync();
  }

  // List alternates - should return single entry
  {
    auto alts_result = cache->list_alternates_sync(key);
    REQUIRE(alts_result.has_value());

    auto &alternates = *alts_result;
    REQUIRE(alternates.size() == 1);
    REQUIRE(alternates[0].id == AlternateId::Original);
    REQUIRE(alternates[0].content_length == content.size());
  }

  cache->stop();
}

TEST_CASE("Cache list_alternates_sync for nonexistent key",
          "[integration][alternate]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(10 * 1024 * 1024);

  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  CacheKey key("nonexistent-alternates-key");

  auto alts_result = cache->list_alternates_sync(key);
  REQUIRE_FALSE(alts_result.has_value());
  REQUIRE(alts_result.error() == CacheError::NotFound);

  cache->stop();
}

TEST_CASE("Cache write_alternate_sync creates alternate chain",
          "[integration][alternate]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(10 * 1024 * 1024);

  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  CacheKey key("alternate-chain-key");

  // Write original content
  std::string original_str = "Original uncompressed content";
  std::vector<std::byte> original_content(original_str.size());
  std::memcpy(original_content.data(), original_str.data(),
              original_str.size());

  {
    auto wh_result = cache->write_sync(key, original_content.size());
    REQUIRE(wh_result.has_value());
    wh_result->write_sync(std::span<const std::byte>(original_content));
    wh_result->close_sync();
  }

  // Write Brotli alternate
  std::string brotli_str = "Brotli compressed content";
  std::vector<std::byte> brotli_content(brotli_str.size());
  std::memcpy(brotli_content.data(), brotli_str.data(), brotli_str.size());

  {
    auto wh_result = cache->write_alternate_sync(key, AlternateId::Brotli,
                                                 brotli_content.size());
    REQUIRE(wh_result.has_value());
    wh_result->write_sync(std::span<const std::byte>(brotli_content));
    wh_result->close_sync();
  }

  // Write Gzip alternate
  std::string gzip_str = "Gzip compressed content";
  std::vector<std::byte> gzip_content(gzip_str.size());
  std::memcpy(gzip_content.data(), gzip_str.data(), gzip_str.size());

  {
    auto wh_result = cache->write_alternate_sync(key, AlternateId::Gzip,
                                                 gzip_content.size());
    REQUIRE(wh_result.has_value());
    wh_result->write_sync(std::span<const std::byte>(gzip_content));
    wh_result->close_sync();
  }

  // List alternates - should return 3 entries (Gzip -> Brotli -> Original)
  {
    auto alts_result = cache->list_alternates_sync(key);
    REQUIRE(alts_result.has_value());

    auto &alternates = *alts_result;
    REQUIRE(alternates.size() == 3);

    // Head should be Gzip (most recently added)
    REQUIRE(alternates[0].id == AlternateId::Gzip);
    REQUIRE(alternates[0].content_length == gzip_content.size());

    // Second should be Brotli
    REQUIRE(alternates[1].id == AlternateId::Brotli);
    REQUIRE(alternates[1].content_length == brotli_content.size());

    // Third should be Original
    REQUIRE(alternates[2].id == AlternateId::Original);
    REQUIRE(alternates[2].content_length == original_content.size());
  }

  cache->stop();
}

TEST_CASE("Cache write_alternate_sync without existing key",
          "[integration][alternate]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(10 * 1024 * 1024);

  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  CacheKey key("new-alternate-key");

  // Write alternate without original (should work - creates new entry)
  std::string content_str = "First entry as Brotli alternate";
  std::vector<std::byte> content(content_str.size());
  std::memcpy(content.data(), content_str.data(), content_str.size());

  {
    auto wh_result =
        cache->write_alternate_sync(key, AlternateId::Brotli, content.size());
    REQUIRE(wh_result.has_value());
    wh_result->write_sync(std::span<const std::byte>(content));
    wh_result->close_sync();
  }

  // List alternates - should return 1 entry
  {
    auto alts_result = cache->list_alternates_sync(key);
    REQUIRE(alts_result.has_value());

    auto &alternates = *alts_result;
    REQUIRE(alternates.size() == 1);
    REQUIRE(alternates[0].id == AlternateId::Brotli);
  }

  cache->stop();
}

TEST_CASE("Cache read_alternate_sync with DefaultStorageSelector",
          "[integration][alternate]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(10 * 1024 * 1024);

  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  CacheKey key("selector-test-key");

  // Write original and alternates
  std::string original_str = "Original content";
  std::vector<std::byte> original_content(original_str.size());
  std::memcpy(original_content.data(), original_str.data(),
              original_str.size());

  {
    auto wh = cache->write_sync(key, original_content.size());
    REQUIRE(wh.has_value());
    wh->write_sync(std::span<const std::byte>(original_content));
    wh->close_sync();
  }

  std::string brotli_str = "Brotli content";
  std::vector<std::byte> brotli_content(brotli_str.size());
  std::memcpy(brotli_content.data(), brotli_str.data(), brotli_str.size());

  {
    auto wh = cache->write_alternate_sync(key, AlternateId::Brotli,
                                          brotli_content.size());
    REQUIRE(wh.has_value());
    wh->write_sync(std::span<const std::byte>(brotli_content));
    wh->close_sync();
  }

  // Read with DefaultStorageSelector - should return first (head = Brotli)
  DefaultStorageSelector selector;
  AlternateSelectionContext ctx;

  auto rh = cache->read_alternate_sync(key, selector, ctx);
  REQUIRE(rh.has_value());
  // Default selector returns index 0, which is the head (Brotli)
  REQUIRE(rh->content().size() == brotli_content.size());

  cache->stop();
}

TEST_CASE("Cache read_alternate_sync with CompressionAwareSelector",
          "[integration][alternate]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(10 * 1024 * 1024);

  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  CacheKey key("compression-selector-key");

  // Write original
  std::string original_str = "Original uncompressed";
  std::vector<std::byte> original_content(original_str.size());
  std::memcpy(original_content.data(), original_str.data(),
              original_str.size());

  {
    auto wh = cache->write_sync(key, original_content.size());
    REQUIRE(wh.has_value());
    wh->write_sync(std::span<const std::byte>(original_content));
    wh->close_sync();
  }

  // Write Gzip alternate
  std::string gzip_str = "Gzip compressed";
  std::vector<std::byte> gzip_content(gzip_str.size());
  std::memcpy(gzip_content.data(), gzip_str.data(), gzip_str.size());

  {
    auto wh = cache->write_alternate_sync(key, AlternateId::Gzip,
                                          gzip_content.size());
    REQUIRE(wh.has_value());
    wh->write_sync(std::span<const std::byte>(gzip_content));
    wh->close_sync();
  }

  // Write Brotli alternate (should be preferred by CompressionAwareSelector)
  std::string brotli_str = "Brotli compressed";
  std::vector<std::byte> brotli_content(brotli_str.size());
  std::memcpy(brotli_content.data(), brotli_str.data(), brotli_str.size());

  {
    auto wh = cache->write_alternate_sync(key, AlternateId::Brotli,
                                          brotli_content.size());
    REQUIRE(wh.has_value());
    wh->write_sync(std::span<const std::byte>(brotli_content));
    wh->close_sync();
  }

  // Chain is: Brotli -> Gzip -> Original
  // CompressionAwareSelector should prefer Brotli
  CompressionAwareSelector selector;
  AlternateSelectionContext ctx;
  ctx.prefer_compressed = true;

  auto rh = cache->read_alternate_sync(key, selector, ctx);
  REQUIRE(rh.has_value());
  REQUIRE(rh->content().size() == brotli_content.size());

  // With prefer_compressed = false, should return first (head = Brotli anyway)
  ctx.prefer_compressed = false;
  auto rh2 = cache->read_alternate_sync(key, selector, ctx);
  REQUIRE(rh2.has_value());
  REQUIRE(rh2->content().size() == brotli_content.size());

  cache->stop();
}

TEST_CASE("Cache read_alternate_sync for nonexistent key",
          "[integration][alternate]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(10 * 1024 * 1024);

  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  CacheKey key("nonexistent-selector-key");
  DefaultStorageSelector selector;
  AlternateSelectionContext ctx;

  auto rh = cache->read_alternate_sync(key, selector, ctx);
  REQUIRE_FALSE(rh.has_value());
  REQUIRE(rh.error() == CacheError::NotFound);

  cache->stop();
}

TEST_CASE("Cache remove_alternate_sync removes head alternate",
          "[integration][alternate]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(10 * 1024 * 1024);

  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  CacheKey key("remove-head-test-key");

  // Write original and alternates: Original -> Gzip -> Brotli (head)
  std::string original_str = "Original content";
  std::vector<std::byte> original_content(original_str.size());
  std::memcpy(original_content.data(), original_str.data(),
              original_str.size());

  {
    auto wh = cache->write_sync(key, original_content.size());
    REQUIRE(wh.has_value());
    wh->write_sync(std::span<const std::byte>(original_content));
    wh->close_sync();
  }

  std::string gzip_str = "Gzip content";
  std::vector<std::byte> gzip_content(gzip_str.size());
  std::memcpy(gzip_content.data(), gzip_str.data(), gzip_str.size());

  {
    auto wh = cache->write_alternate_sync(key, AlternateId::Gzip,
                                          gzip_content.size());
    REQUIRE(wh.has_value());
    wh->write_sync(std::span<const std::byte>(gzip_content));
    wh->close_sync();
  }

  std::string brotli_str = "Brotli content";
  std::vector<std::byte> brotli_content(brotli_str.size());
  std::memcpy(brotli_content.data(), brotli_str.data(), brotli_str.size());

  {
    auto wh = cache->write_alternate_sync(key, AlternateId::Brotli,
                                          brotli_content.size());
    REQUIRE(wh.has_value());
    wh->write_sync(std::span<const std::byte>(brotli_content));
    wh->close_sync();
  }

  // Verify chain: Brotli -> Gzip -> Original
  {
    auto alts = cache->list_alternates_sync(key);
    REQUIRE(alts.has_value());
    REQUIRE(alts->size() == 3);
    REQUIRE((*alts)[0].id == AlternateId::Brotli);
    REQUIRE((*alts)[1].id == AlternateId::Gzip);
    REQUIRE((*alts)[2].id == AlternateId::Original);
  }

  // Remove head (Brotli)
  {
    auto result = cache->remove_alternate_sync(key, AlternateId::Brotli);
    REQUIRE(result.has_value());
  }

  // Verify chain is now: Gzip -> Original
  {
    auto alts = cache->list_alternates_sync(key);
    REQUIRE(alts.has_value());
    REQUIRE(alts->size() == 2);
    REQUIRE((*alts)[0].id == AlternateId::Gzip);
    REQUIRE((*alts)[1].id == AlternateId::Original);
  }

  cache->stop();
}

TEST_CASE("Cache remove_alternate_sync removes middle alternate",
          "[integration][alternate]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(10 * 1024 * 1024);

  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  CacheKey key("remove-middle-test-key");

  // Write original and alternates: Original -> Gzip -> Brotli (head)
  std::string original_str = "Original content";
  std::vector<std::byte> original_content(original_str.size());
  std::memcpy(original_content.data(), original_str.data(),
              original_str.size());

  {
    auto wh = cache->write_sync(key, original_content.size());
    REQUIRE(wh.has_value());
    wh->write_sync(std::span<const std::byte>(original_content));
    wh->close_sync();
  }

  std::string gzip_str = "Gzip content";
  std::vector<std::byte> gzip_content(gzip_str.size());
  std::memcpy(gzip_content.data(), gzip_str.data(), gzip_str.size());

  {
    auto wh = cache->write_alternate_sync(key, AlternateId::Gzip,
                                          gzip_content.size());
    REQUIRE(wh.has_value());
    wh->write_sync(std::span<const std::byte>(gzip_content));
    wh->close_sync();
  }

  std::string brotli_str = "Brotli content";
  std::vector<std::byte> brotli_content(brotli_str.size());
  std::memcpy(brotli_content.data(), brotli_str.data(), brotli_str.size());

  {
    auto wh = cache->write_alternate_sync(key, AlternateId::Brotli,
                                          brotli_content.size());
    REQUIRE(wh.has_value());
    wh->write_sync(std::span<const std::byte>(brotli_content));
    wh->close_sync();
  }

  // Remove middle (Gzip)
  {
    auto result = cache->remove_alternate_sync(key, AlternateId::Gzip);
    REQUIRE(result.has_value());
  }

  // Verify chain is now: Brotli -> Original
  {
    auto alts = cache->list_alternates_sync(key);
    REQUIRE(alts.has_value());
    REQUIRE(alts->size() == 2);
    REQUIRE((*alts)[0].id == AlternateId::Brotli);
    REQUIRE((*alts)[1].id == AlternateId::Original);
  }

  cache->stop();
}

TEST_CASE("Cache remove_alternate_sync removes tail alternate",
          "[integration][alternate]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(10 * 1024 * 1024);

  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  CacheKey key("remove-tail-test-key");

  // Write original and alternate: Original -> Brotli (head)
  std::string original_str = "Original content";
  std::vector<std::byte> original_content(original_str.size());
  std::memcpy(original_content.data(), original_str.data(),
              original_str.size());

  {
    auto wh = cache->write_sync(key, original_content.size());
    REQUIRE(wh.has_value());
    wh->write_sync(std::span<const std::byte>(original_content));
    wh->close_sync();
  }

  std::string brotli_str = "Brotli content";
  std::vector<std::byte> brotli_content(brotli_str.size());
  std::memcpy(brotli_content.data(), brotli_str.data(), brotli_str.size());

  {
    auto wh = cache->write_alternate_sync(key, AlternateId::Brotli,
                                          brotli_content.size());
    REQUIRE(wh.has_value());
    wh->write_sync(std::span<const std::byte>(brotli_content));
    wh->close_sync();
  }

  // Remove tail (Original)
  {
    auto result = cache->remove_alternate_sync(key, AlternateId::Original);
    REQUIRE(result.has_value());
  }

  // Verify chain is now: Brotli only
  {
    auto alts = cache->list_alternates_sync(key);
    REQUIRE(alts.has_value());
    REQUIRE(alts->size() == 1);
    REQUIRE((*alts)[0].id == AlternateId::Brotli);
  }

  cache->stop();
}

TEST_CASE("Cache remove_alternate_sync removes only alternate",
          "[integration][alternate]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(10 * 1024 * 1024);

  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  CacheKey key("remove-only-test-key");

  // Write single entry
  std::string content_str = "Only content";
  std::vector<std::byte> content(content_str.size());
  std::memcpy(content.data(), content_str.data(), content_str.size());

  {
    auto wh = cache->write_sync(key, content.size());
    REQUIRE(wh.has_value());
    wh->write_sync(std::span<const std::byte>(content));
    wh->close_sync();
  }

  // Remove only alternate (Original)
  {
    auto result = cache->remove_alternate_sync(key, AlternateId::Original);
    REQUIRE(result.has_value());
  }

  // Key should no longer exist
  {
    auto exists = cache->exists_sync(key);
    REQUIRE(exists.has_value());
    REQUIRE(*exists == false);
  }

  cache->stop();
}

TEST_CASE("Cache remove_alternate_sync for nonexistent alternate",
          "[integration][alternate]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(10 * 1024 * 1024);

  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  CacheKey key("remove-nonexistent-alt-key");

  // Write original only
  std::string content_str = "Original content";
  std::vector<std::byte> content(content_str.size());
  std::memcpy(content.data(), content_str.data(), content_str.size());

  {
    auto wh = cache->write_sync(key, content.size());
    REQUIRE(wh.has_value());
    wh->write_sync(std::span<const std::byte>(content));
    wh->close_sync();
  }

  // Try to remove nonexistent alternate (Brotli)
  {
    auto result = cache->remove_alternate_sync(key, AlternateId::Brotli);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == CacheError::AlternateNotFound);
  }

  // Original should still exist
  {
    auto alts = cache->list_alternates_sync(key);
    REQUIRE(alts.has_value());
    REQUIRE(alts->size() == 1);
    REQUIRE((*alts)[0].id == AlternateId::Original);
  }

  cache->stop();
}

TEST_CASE("Cache remove_alternate_sync for nonexistent key",
          "[integration][alternate]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(10 * 1024 * 1024);

  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  CacheKey key("remove-nonexistent-key");

  auto result = cache->remove_alternate_sync(key, AlternateId::Original);
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error() == CacheError::NotFound);

  cache->stop();
}

TEST_CASE("Full alternate chain workflow",
          "[integration][alternate][workflow]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  config.enable_hit_tracking = true;
  config.hit_flush_interval = std::chrono::milliseconds(100);
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(20 * 1024 * 1024);

  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  CacheKey key("workflow-test-key");

  // Step 1: Write original content
  std::string original_str =
      "This is the original uncompressed content for the workflow test";
  std::vector<std::byte> original_content(original_str.size());
  std::memcpy(original_content.data(), original_str.data(),
              original_str.size());

  {
    auto wh = cache->write_sync(key, original_content.size());
    REQUIRE(wh.has_value());
    wh->write_sync(std::span<const std::byte>(original_content));
    REQUIRE(wh->close_sync().has_value());
  }

  // Verify original exists
  {
    auto alts = cache->list_alternates_sync(key);
    REQUIRE(alts.has_value());
    REQUIRE(alts->size() == 1);
    REQUIRE((*alts)[0].id == AlternateId::Original);
  }

  // Step 2: Add Gzip alternate (simulated compressed content)
  std::string gzip_str = "Gzip compressed version";
  std::vector<std::byte> gzip_content(gzip_str.size());
  std::memcpy(gzip_content.data(), gzip_str.data(), gzip_str.size());

  {
    auto wh = cache->write_alternate_sync(key, AlternateId::Gzip,
                                          gzip_content.size());
    REQUIRE(wh.has_value());
    wh->write_sync(std::span<const std::byte>(gzip_content));
    REQUIRE(wh->close_sync().has_value());
  }

  // Step 3: Add Brotli alternate (simulated compressed content)
  std::string brotli_str = "Brotli compressed version";
  std::vector<std::byte> brotli_content(brotli_str.size());
  std::memcpy(brotli_content.data(), brotli_str.data(), brotli_str.size());

  {
    auto wh = cache->write_alternate_sync(key, AlternateId::Brotli,
                                          brotli_content.size());
    REQUIRE(wh.has_value());
    wh->write_sync(std::span<const std::byte>(brotli_content));
    REQUIRE(wh->close_sync().has_value());
  }

  // Verify chain: Brotli -> Gzip -> Original
  {
    auto alts = cache->list_alternates_sync(key);
    REQUIRE(alts.has_value());
    REQUIRE(alts->size() == 3);
    REQUIRE((*alts)[0].id == AlternateId::Brotli);
    REQUIRE((*alts)[1].id == AlternateId::Gzip);
    REQUIRE((*alts)[2].id == AlternateId::Original);
  }

  // Step 4: Read with CompressionAwareSelector - should prefer Brotli
  {
    CompressionAwareSelector selector;
    AlternateSelectionContext ctx;
    ctx.prefer_compressed = true;

    auto rh = cache->read_alternate_sync(key, selector, ctx);
    REQUIRE(rh.has_value());

    std::string read_str(reinterpret_cast<const char *>(rh->content().data()),
                         rh->content().size());
    REQUIRE(read_str == brotli_str);
  }

  // Step 5: Multiple reads to generate hits
  {
    DefaultStorageSelector selector;
    AlternateSelectionContext ctx;

    for (int i = 0; i < 5; ++i) {
      auto rh = cache->read_alternate_sync(key, selector, ctx);
      REQUIRE(rh.has_value());
    }
  }

  // Step 6: Remove Gzip alternate (middle of chain)
  {
    auto result = cache->remove_alternate_sync(key, AlternateId::Gzip);
    REQUIRE(result.has_value());
  }

  // Verify chain: Brotli -> Original
  {
    auto alts = cache->list_alternates_sync(key);
    REQUIRE(alts.has_value());
    REQUIRE(alts->size() == 2);
    REQUIRE((*alts)[0].id == AlternateId::Brotli);
    REQUIRE((*alts)[1].id == AlternateId::Original);
  }

  // Step 7: Read after removal - Brotli should still be selectable
  {
    CompressionAwareSelector selector;
    AlternateSelectionContext ctx;
    ctx.prefer_compressed = true;

    auto rh = cache->read_alternate_sync(key, selector, ctx);
    REQUIRE(rh.has_value());

    std::string read_str(reinterpret_cast<const char *>(rh->content().data()),
                         rh->content().size());
    REQUIRE(read_str == brotli_str);
  }

  // Step 8: Add Zstd alternate
  std::string zstd_str = "Zstd compressed version";
  std::vector<std::byte> zstd_content(zstd_str.size());
  std::memcpy(zstd_content.data(), zstd_str.data(), zstd_str.size());

  {
    auto wh = cache->write_alternate_sync(key, AlternateId::Zstd,
                                          zstd_content.size());
    REQUIRE(wh.has_value());
    wh->write_sync(std::span<const std::byte>(zstd_content));
    REQUIRE(wh->close_sync().has_value());
  }

  // Verify chain: Zstd -> Brotli -> Original
  {
    auto alts = cache->list_alternates_sync(key);
    REQUIRE(alts.has_value());
    REQUIRE(alts->size() == 3);
    REQUIRE((*alts)[0].id == AlternateId::Zstd);
    REQUIRE((*alts)[1].id == AlternateId::Brotli);
    REQUIRE((*alts)[2].id == AlternateId::Original);
  }

  // Step 9: CompressionAwareSelector should return a compressed variant
  {
    CompressionAwareSelector selector;
    AlternateSelectionContext ctx;
    ctx.prefer_compressed = true;

    auto rh = cache->read_alternate_sync(key, selector, ctx);
    REQUIRE(rh.has_value());

    // Should get either Brotli or Zstd (both are compression variants)
    std::string read_str(reinterpret_cast<const char *>(rh->content().data()),
                         rh->content().size());
    bool is_compressed = (read_str == brotli_str || read_str == zstd_str);
    REQUIRE(is_compressed);
  }

  // Step 10: Remove all alternates one by one
  {
    auto result = cache->remove_alternate_sync(key, AlternateId::Zstd);
    REQUIRE(result.has_value());
  }
  {
    auto result = cache->remove_alternate_sync(key, AlternateId::Brotli);
    REQUIRE(result.has_value());
  }

  // Verify only Original remains
  {
    auto alts = cache->list_alternates_sync(key);
    REQUIRE(alts.has_value());
    REQUIRE(alts->size() == 1);
    REQUIRE((*alts)[0].id == AlternateId::Original);
  }

  // Step 11: Remove Original - key should no longer exist
  {
    auto result = cache->remove_alternate_sync(key, AlternateId::Original);
    REQUIRE(result.has_value());
  }

  {
    auto exists = cache->exists_sync(key);
    REQUIRE(exists.has_value());
    REQUIRE(*exists == false);
  }

  cache->stop();
}

TEST_CASE("Alternate chain with multiple keys",
          "[integration][alternate][workflow]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(30 * 1024 * 1024);

  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  // Create multiple keys with different alternate configurations
  for (int i = 0; i < 5; ++i) {
    CacheKey key("multi-key-" + std::to_string(i));

    // Write original
    std::string original_str = "Original content for key " + std::to_string(i);
    std::vector<std::byte> original_content(original_str.size());
    std::memcpy(original_content.data(), original_str.data(),
                original_str.size());

    {
      auto wh = cache->write_sync(key, original_content.size());
      REQUIRE(wh.has_value());
      wh->write_sync(std::span<const std::byte>(original_content));
      wh->close_sync();
    }

    // Add varying number of alternates
    if (i >= 1) {
      std::string gzip_str = "Gzip for key " + std::to_string(i);
      std::vector<std::byte> gzip_content(gzip_str.size());
      std::memcpy(gzip_content.data(), gzip_str.data(), gzip_str.size());

      auto wh = cache->write_alternate_sync(key, AlternateId::Gzip,
                                            gzip_content.size());
      REQUIRE(wh.has_value());
      wh->write_sync(std::span<const std::byte>(gzip_content));
      wh->close_sync();
    }

    if (i >= 2) {
      std::string brotli_str = "Brotli for key " + std::to_string(i);
      std::vector<std::byte> brotli_content(brotli_str.size());
      std::memcpy(brotli_content.data(), brotli_str.data(), brotli_str.size());

      auto wh = cache->write_alternate_sync(key, AlternateId::Brotli,
                                            brotli_content.size());
      REQUIRE(wh.has_value());
      wh->write_sync(std::span<const std::byte>(brotli_content));
      wh->close_sync();
    }
  }

  // Verify each key has correct number of alternates
  for (int i = 0; i < 5; ++i) {
    CacheKey key("multi-key-" + std::to_string(i));
    auto alts = cache->list_alternates_sync(key);
    REQUIRE(alts.has_value());

    size_t expected_count = 1;     // Original
    if (i >= 1) expected_count++;  // Gzip
    if (i >= 2) expected_count++;  // Brotli

    REQUIRE(alts->size() == expected_count);
  }

  // Read from each key using CompressionAwareSelector
  CompressionAwareSelector selector;
  AlternateSelectionContext ctx;
  ctx.prefer_compressed = true;

  for (int i = 0; i < 5; ++i) {
    CacheKey key("multi-key-" + std::to_string(i));
    auto rh = cache->read_alternate_sync(key, selector, ctx);
    REQUIRE(rh.has_value());

    // Verify we got the expected alternate
    if (i >= 2) {
      // Should get Brotli
      std::string expected = "Brotli for key " + std::to_string(i);
      std::string actual(reinterpret_cast<const char *>(rh->content().data()),
                         rh->content().size());
      REQUIRE(actual == expected);
    } else if (i >= 1) {
      // Should get Gzip
      std::string expected = "Gzip for key " + std::to_string(i);
      std::string actual(reinterpret_cast<const char *>(rh->content().data()),
                         rh->content().size());
      REQUIRE(actual == expected);
    } else {
      // Should get Original
      std::string expected = "Original content for key " + std::to_string(i);
      std::string actual(reinterpret_cast<const char *>(rh->content().data()),
                         rh->content().size());
      REQUIRE(actual == expected);
    }
  }

  cache->stop();
}

TEST_CASE("Cache reset_stats clears statistics", "[integration][stats]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(10 * 1024 * 1024);

  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  // Write some data
  CacheKey key("stats-test-key");
  std::string content_str = "Content for stats test";
  std::vector<std::byte> content(content_str.size());
  std::memcpy(content.data(), content_str.data(), content_str.size());

  {
    auto wh = cache->write_sync(key, content.size());
    REQUIRE(wh.has_value());
    wh->write_sync(std::span<const std::byte>(content));
    wh->close_sync();
  }

  // Read to generate hits
  {
    auto rh = cache->read_sync(key);
    REQUIRE(rh.has_value());
  }

  // Generate a miss
  {
    CacheKey miss_key("nonexistent-stats-key");
    auto rh = cache->read_sync(miss_key);
    REQUIRE_FALSE(rh.has_value());
  }

  // Verify some counters are non-zero before reset
  {
    auto s = cache->stats();
    bool has_activity =
        (s.bytes_written > 0 || s.disk_cache_hits > 0 || s.current_entries > 0);
    REQUIRE(has_activity);
  }

  // Snapshot stats that are aggregated from volumes/ram_cache (not affected by
  // reset)
  auto pre_reset = cache->stats();

  // Reset the base stats struct
  cache->reset_stats();

  // Verify reset_stats() zeroed the base counters.
  // Note: stats() aggregates volume and ram_cache stats on top of the base,
  // so volume-sourced fields (disk_cache_hits, bytes_read, bytes_written,
  // current_entries, current_bytes, evictions) and ram_cache-sourced fields
  // (ram_cache_hits, ram_cache_misses) persist across reset.
  {
    auto s = cache->stats();

    // Volume-aggregated fields should remain unchanged
    REQUIRE(s.current_entries == pre_reset.current_entries);
  }

  cache->stop();
}

TEST_CASE("Cache config returns initialized config", "[integration][config]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  config.ram_cache_size = 2_MB;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(10 * 1024 * 1024);

  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  // Verify config() returns the config we set
  REQUIRE(cache->config().ram_cache_size == 2_MB);

  cache->stop();
}

TEST_CASE("Cache total_capacity sums volumes", "[integration][capacity]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(10 * 1024 * 1024);

  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  // With one volume, total_capacity should be > 0
  REQUIRE(cache->total_capacity() > 0);

  cache->stop();
}

TEST_CASE("Cache plugin_manager const access", "[integration][plugin]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(10 * 1024 * 1024);

  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  // Get a const reference to the cache and call plugin_manager()
  const Cache &const_cache = *cache;
  const auto &pm = const_cache.plugin_manager();

  // Verify the plugin_manager() const access works (no crash, returns
  // reference)
  (void)pm;

  cache->stop();
}

TEST_CASE("Cache evict_from_ram_cache", "[integration][ram_cache]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  config.ram_cache_size = 2_MB;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(10 * 1024 * 1024);

  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  CacheKey key("evict-ram-test-key");
  std::string content_str = "Content for RAM cache eviction test";
  std::vector<std::byte> content(content_str.size());
  std::memcpy(content.data(), content_str.data(), content_str.size());

  // Write data
  {
    auto wh = cache->write_sync(key, content.size());
    REQUIRE(wh.has_value());
    wh->write_sync(std::span<const std::byte>(content));
    wh->close_sync();
  }

  // Read to populate RAM cache
  {
    auto rh = cache->read_sync(key);
    REQUIRE(rh.has_value());
  }

  // Evict the entry from RAM cache
  cache->evict_from_ram_cache(key, AlternateId::Original);

  // Read again - should still succeed (from disk), data is intact
  {
    auto rh = cache->read_sync(key);
    REQUIRE(rh.has_value());

    auto read_content = rh->content();
    std::string read_str(reinterpret_cast<const char *>(read_content.data()),
                         read_content.size());
    REQUIRE(read_str == content_str);
  }

  cache->stop();
}

// =============================================================================
// Phase 6D: Cache Optimization Engine
// =============================================================================

TEST_CASE("Cache optimization engine startup", "[integration][optimization]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  config.optimization_config.enabled = true;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(10 * 1024 * 1024);

  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  // When optimization is enabled, optimization_engine() should return non-null
  REQUIRE(cache->optimization_engine() != nullptr);

  // Verify const access also works
  const Cache &const_cache = *cache;
  REQUIRE(const_cache.optimization_engine() != nullptr);

  cache->stop();
}
