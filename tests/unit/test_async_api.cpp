// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "cyclone/cache.hpp"
#include "cyclone/key.hpp"
#include "cyclone/task.hpp"
#include "support/temp_cache.hpp"

using namespace cyclone;

// ---------------------------------------------------------------------------
// Helpers for integration tests
// ---------------------------------------------------------------------------

// ===========================================================================
// Coroutine framework tests (pure unit tests, no cache needed)
// ===========================================================================

TEST_CASE("Task simple co_return value", "[task][coroutine]") {
  auto coro = []() -> Task<int> { co_return 42; };

  auto task = coro();
  int result = task.sync_wait();
  REQUIRE(result == 42);
}

TEST_CASE("Task void coroutine", "[task][coroutine]") {
  bool executed = false;

  auto coro = [&]() -> Task<void> {
    executed = true;
    co_return;
  };

  auto task = coro();
  task.sync_wait();
  REQUIRE(executed);
}

TEST_CASE("Task exception propagation", "[task][coroutine]") {
  auto coro = []() -> Task<int> {
    throw std::runtime_error("coroutine error");
    co_return 0;  // unreachable, but needed to make this a coroutine
  };

  auto task = coro();
  REQUIRE_THROWS_AS(task.sync_wait(), std::runtime_error);
}

TEST_CASE("Task move semantics", "[task][coroutine]") {
  auto coro = []() -> Task<int> { co_return 99; };

  auto task1 = coro();
  REQUIRE(task1.handle() != nullptr);

  // Move task1 into task2
  auto task2 = std::move(task1);
  REQUIRE(task1.handle() == nullptr);  // NOLINT(bugprone-use-after-move)
  REQUIRE(task2.handle() != nullptr);

  int result = task2.sync_wait();
  REQUIRE(result == 99);
}

TEST_CASE("Task done and resume", "[task][coroutine]") {
  auto coro = []() -> Task<int> { co_return 7; };

  auto task = coro();

  // Before resume, the task is not done (initial_suspend is suspend_always)
  REQUIRE_FALSE(task.done());

  // Resume drives the coroutine to completion
  task.resume();
  REQUIRE(task.done());
}

TEST_CASE("Task continuation chaining", "[task][coroutine]") {
  auto inner = []() -> Task<int> { co_return 10; };

  auto outer = [&]() -> Task<int> {
    int val = co_await inner();
    co_return val + 5;
  };

  auto task = outer();
  int result = task.sync_wait();
  REQUIRE(result == 15);
}

TEST_CASE("ReadyAwaiter make_ready", "[task][coroutine]") {
  auto coro = []() -> Task<std::string> {
    std::string val = co_await make_ready(std::string("hello"));
    co_return val;
  };

  auto task = coro();
  std::string result = task.sync_wait();
  REQUIRE(result == "hello");
}

// ===========================================================================
// Async Cache API tests (integration, need real cache)
// ===========================================================================

TEST_CASE("Cache async open_write and open_read", "[async][integration]") {
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

  CacheKey key("async-roundtrip-key");

  std::string header_str = "X-Async: true";
  std::string content_str = "Async content payload";

  std::vector<std::byte> header(header_str.size());
  std::memcpy(header.data(), header_str.data(), header_str.size());

  std::vector<std::byte> content(content_str.size());
  std::memcpy(content.data(), content_str.data(), content_str.size());

  // Async write via open_write().sync_wait()
  {
    auto wh_task = cache->open_write(key, content.size());
    auto wh_result = wh_task.sync_wait();
    REQUIRE(wh_result.has_value());

    auto &wh = *wh_result;
    wh.set_header(std::span<const std::byte>(header));

    auto write_task = wh.write(std::span<const std::byte>(content));
    auto write_result = write_task.sync_wait();
    REQUIRE(write_result.has_value());
    REQUIRE(*write_result == content.size());

    auto close_task = wh.close();
    auto close_result = close_task.sync_wait();
    REQUIRE(close_result.has_value());
  }

  // Async read via open_read().sync_wait()
  {
    auto rh_task = cache->open_read(key);
    auto rh_result = rh_task.sync_wait();
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

TEST_CASE("Cache async exists", "[async][integration]") {
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

  CacheKey key("async-exists-key");

  // Key should not exist yet
  {
    auto exists_task = cache->exists(key);
    auto exists_result = exists_task.sync_wait();
    REQUIRE(exists_result.has_value());
    REQUIRE(*exists_result == false);
  }

  // Write something via sync API for simplicity
  {
    std::string content_str = "exists test content";
    std::vector<std::byte> content(content_str.size());
    std::memcpy(content.data(), content_str.data(), content_str.size());

    auto wh_result = cache->write_sync(key, content.size());
    REQUIRE(wh_result.has_value());
    wh_result->write_sync(std::span<const std::byte>(content));
    wh_result->close_sync();
  }

  // Key should now exist via async API
  {
    auto exists_task = cache->exists(key);
    auto exists_result = exists_task.sync_wait();
    REQUIRE(exists_result.has_value());
    REQUIRE(*exists_result == true);
  }

  cache->stop();
}

TEST_CASE("Cache async remove", "[async][integration]") {
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

  CacheKey key("async-remove-key");

  // Write something
  {
    std::string content_str = "content to be removed async";
    std::vector<std::byte> content(content_str.size());
    std::memcpy(content.data(), content_str.data(), content_str.size());

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

  // Remove via async API
  {
    auto remove_task = cache->remove(key);
    auto remove_result = remove_task.sync_wait();
    REQUIRE(remove_result.has_value());
  }

  // Verify it is gone
  {
    auto exists = cache->exists_sync(key);
    REQUIRE(exists.has_value());
    REQUIRE(*exists == false);
  }

  // Async read should fail with NotFound
  {
    auto rh_task = cache->open_read(key);
    auto rh_result = rh_task.sync_wait();
    REQUIRE_FALSE(rh_result.has_value());
    REQUIRE(rh_result.error() == CacheError::NotFound);
  }

  cache->stop();
}
