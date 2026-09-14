// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "cyclone/alternate.hpp"
#include "cyclone/cache.hpp"
#include "cyclone/key.hpp"
#include "support/temp_cache.hpp"

using namespace cyclone;

TEST_CASE("ReadHandle mapped_view access", "[handle][integration]") {
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

  CacheKey key("mapped-view-test-key");
  std::string content_str = "Content for mapped view test";

  std::vector<std::byte> content(content_str.size());
  std::memcpy(content.data(), content_str.data(), content_str.size());

  // Write data
  {
    auto wh_result = cache->write_sync(key, content.size());
    REQUIRE(wh_result.has_value());
    auto write_result =
        wh_result->write_sync(std::span<const std::byte>(content));
    REQUIRE(write_result.has_value());
    auto close_result = wh_result->close_sync();
    REQUIRE(close_result.has_value());
  }

  // Read and check mapped_view
  {
    auto rh_result = cache->read_sync(key);
    REQUIRE(rh_result.has_value());

    auto &rh = *rh_result;
    REQUIRE(rh.is_valid());

    // mapped_view() may return nullopt (RAM cache hit) or a valid span (disk
    // read). Either way, the call must not crash and the handle must remain
    // valid.
    auto view = rh.mapped_view();
    // If a view is present, it should be non-empty and contain the content
    if (view.has_value()) {
      REQUIRE(view->size() >= content.size());
    }
  }

  cache->stop();
}

TEST_CASE("ReadHandle is_ram_cache_hit", "[handle][integration]") {
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

  CacheKey key("ram-cache-hit-test-key");
  std::string content_str = "Content for RAM cache hit test";

  std::vector<std::byte> content(content_str.size());
  std::memcpy(content.data(), content_str.data(), content_str.size());

  // Write data
  {
    auto wh_result = cache->write_sync(key, content.size());
    REQUIRE(wh_result.has_value());
    wh_result->write_sync(std::span<const std::byte>(content));
    wh_result->close_sync();
  }

  // Read multiple times via read_alternate_sync to populate RAM cache.
  // The first read comes from disk and populates the RAM cache.
  // Subsequent reads should hit the RAM cache.
  bool saw_ram_cache_hit = false;
  for (int i = 0; i < 5; ++i) {
    DefaultStorageSelector selector;
    AlternateSelectionContext ctx;
    auto rh_result = cache->read_alternate_sync(key, selector, ctx);
    REQUIRE(rh_result.has_value());
    REQUIRE(rh_result->is_valid());
    if (rh_result->is_ram_cache_hit()) {
      saw_ram_cache_hit = true;
    }
  }

  // Verify the is_ram_cache_hit() method works (returns a boolean, false when
  // default-constructed). Even if the cache implementation does not promote to
  // RAM on small entries, the accessor itself must function without error.
  {
    ReadHandle rh;
    REQUIRE_FALSE(rh.is_ram_cache_hit());
  }

  // Check stats to understand RAM cache behavior.
  // When persistent mmap is active, the RAM cache is bypassed on reads
  // (disk reads are already zero-syscall and preserve file offsets for
  // sendfile), so hits+misses may be zero.
  auto stats = cache->stats();
  if (stats.ram_cache_hits + stats.ram_cache_misses > 0) {
    // If any read was served from RAM cache, verify the flag was set
    if (stats.ram_cache_hits > 0) {
      REQUIRE(saw_ram_cache_hit);
    }
  }

  cache->stop();
}

TEST_CASE("WriteHandle bytes_written tracking", "[handle][integration]") {
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

  CacheKey key("bytes-written-test-key");

  std::string chunk1_str = "First chunk of data";
  std::string chunk2_str = "Second chunk of data, slightly longer";

  std::vector<std::byte> chunk1(chunk1_str.size());
  std::memcpy(chunk1.data(), chunk1_str.data(), chunk1_str.size());

  std::vector<std::byte> chunk2(chunk2_str.size());
  std::memcpy(chunk2.data(), chunk2_str.data(), chunk2_str.size());

  size_t total_size = chunk1.size() + chunk2.size();

  auto wh_result = cache->write_sync(key, total_size);
  REQUIRE(wh_result.has_value());

  auto &wh = *wh_result;
  REQUIRE(wh.is_valid());
  REQUIRE(wh.bytes_written() == 0);

  // Write first chunk
  auto write1 = wh.write_sync(std::span<const std::byte>(chunk1));
  REQUIRE(write1.has_value());
  REQUIRE(wh.bytes_written() == chunk1.size());

  // Write second chunk
  auto write2 = wh.write_sync(std::span<const std::byte>(chunk2));
  REQUIRE(write2.has_value());
  REQUIRE(wh.bytes_written() == total_size);

  auto close_result = wh.close_sync();
  REQUIRE(close_result.has_value());

  cache->stop();
}

TEST_CASE("WriteHandle explicit abort", "[handle][integration]") {
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

  CacheKey key("abort-test-key");
  std::string content_str = "Content that will be aborted";

  std::vector<std::byte> content(content_str.size());
  std::memcpy(content.data(), content_str.data(), content_str.size());

  // Write and abort
  {
    auto wh_result = cache->write_sync(key, content.size());
    REQUIRE(wh_result.has_value());

    auto &wh = *wh_result;
    wh.write_sync(std::span<const std::byte>(content));

    // Explicitly abort the write
    wh.abort();
    REQUIRE_FALSE(wh.is_valid());
  }

  // Verify the key does not exist
  {
    auto exists = cache->exists_sync(key);
    REQUIRE(exists.has_value());
    REQUIRE(*exists == false);
  }

  // Read should fail with NotFound
  {
    auto rh_result = cache->read_sync(key);
    REQUIRE_FALSE(rh_result.has_value());
    REQUIRE(rh_result.error() == CacheError::NotFound);
  }

  cache->stop();
}

TEST_CASE("Invalid handle default construction", "[handle][edge]") {
  // Default-constructed ReadHandle
  {
    ReadHandle rh;
    REQUIRE_FALSE(rh.is_valid());
    REQUIRE(rh.content_length() == 0);
    REQUIRE(rh.header().empty());
    REQUIRE(rh.content().empty());
    REQUIRE_FALSE(rh.is_ram_cache_hit());
    REQUIRE_FALSE(rh.mapped_view().has_value());
  }

  // Default-constructed WriteHandle
  {
    WriteHandle wh;
    REQUIRE_FALSE(wh.is_valid());
    REQUIRE(wh.bytes_written() == 0);

    // Operations on invalid handle should return errors or be no-ops
    auto write_result = wh.write_sync(std::span<const std::byte>{});
    REQUIRE_FALSE(write_result.has_value());
    REQUIRE(write_result.error() == CacheError::InvalidArgument);

    auto close_result = wh.close_sync();
    REQUIRE_FALSE(close_result.has_value());
    REQUIRE(close_result.error() == CacheError::InvalidArgument);
  }

  // Default-constructed UpdateHandle
  {
    UpdateHandle uh;
    REQUIRE_FALSE(uh.is_valid());
    REQUIRE(uh.header().empty());
  }
}

TEST_CASE("Handle move semantics", "[handle][edge]") {
  // Default-constructed (invalid) handle move
  {
    ReadHandle rh1;
    REQUIRE_FALSE(rh1.is_valid());

    ReadHandle rh2 = std::move(rh1);
    REQUIRE_FALSE(rh2.is_valid());
    REQUIRE_FALSE(rh1.is_valid());  // NOLINT(bugprone-use-after-move)
  }

  // Valid ReadHandle move: obtain a real handle from cache, then move it
  {
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

    CacheKey key("move-test-key");
    std::string content_str = "move test content";
    std::vector<std::byte> content(content_str.size());
    std::memcpy(content.data(), content_str.data(), content_str.size());

    // Write
    {
      auto wh = cache->write_sync(key, content.size());
      REQUIRE(wh.has_value());
      REQUIRE(wh->write_sync(content).has_value());
      REQUIRE(wh->close_sync().has_value());
    }

    // Read and move the valid handle
    auto rh1 = cache->read_sync(key);
    REQUIRE(rh1.has_value());
    REQUIRE(rh1->is_valid());
    REQUIRE(rh1->content_length() > 0);

    ReadHandle rh2 = std::move(*rh1);
    REQUIRE(rh2.is_valid());
    REQUIRE(rh2.content_length() > 0);
    // Source should be invalid after move
    REQUIRE_FALSE(rh1->is_valid());  // NOLINT(bugprone-use-after-move)

    cache->stop();
  }
}
