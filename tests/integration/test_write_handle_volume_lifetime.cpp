// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// A write handle can outlive the Volume it writes into. WriteHandle owns its
// own impl (shared_ptr), so an embedder holding an open handle when the Cache
// is reset or destroyed is a reachable shape, not a misuse the API prevents --
// and a cache reset behind an operator-facing purge is exactly how it happens
// outside a test. Everything reached afterwards must fail cleanly.
//
// While the handle held a RAW Volume*, two paths dereferenced storage that
// Cache::Impl's volume vector had already destroyed: write() reads
// Volume::config() for the per-object size bound, and close() -> do_commit()
// calls through to the Volume with a `stripe` pointer into its freed storage.
// Under a sanitizer that is a heap-use-after-free; without one it is silent
// corruption. Both paths now pin the Volume through a weak reference and
// report CacheError::Closed when it is gone.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "cyclone/cache.hpp"
#include "cyclone/error.hpp"
#include "cyclone/key.hpp"
#include "support/temp_cache.hpp"

using namespace cyclone;

TEST_CASE("a write handle outliving its volume fails Closed, never crashes",
          "[write][lifetime]") {
  TempCacheDir tmp;
  const std::string cache_path = tmp.path();

  CacheConfig config;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto cache = std::move(*cache_result);

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(10 * 1024 * 1024);
  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  const CacheKey key("write-handle-outlives-volume");
  const std::vector<std::byte> data(64, std::byte{0xAB});

  SECTION("write() after the volume is gone") {
    auto wh = cache->write_sync(key, data.size());
    REQUIRE(wh.has_value());

    // Destroys Cache::Impl, its vector<shared_ptr<Volume>>, and the Volume --
    // the same teardown a cache reset performs under an open handle.
    cache.reset();

    const auto written = wh->write_sync(std::span<const std::byte>(data));
    REQUIRE_FALSE(written.has_value());
    REQUIRE(written.error() == CacheError::Closed);
  }

  SECTION("close() after the volume is gone") {
    auto wh = cache->write_sync(key, data.size());
    REQUIRE(wh.has_value());
    // Buffer while the volume is still alive, so it is the COMMIT path that
    // meets the freed volume rather than the write path above.
    REQUIRE(wh->write_sync(std::span<const std::byte>(data)).has_value());

    cache.reset();

    const auto closed = wh->close_sync();
    REQUIRE_FALSE(closed.has_value());
    REQUIRE(closed.error() == CacheError::Closed);
  }
}
