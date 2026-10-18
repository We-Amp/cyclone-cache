// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <latch>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#endif

#include "c_api/c_error_map.hpp"
#include "cyclone/config.hpp"
#include "cyclone/cyclone_c.h"
#include "support/temp_cache.hpp"

// =============================================================================
// Basic C API Tests
// =============================================================================

TEST_CASE("C API: Create and destroy cache", "[c_api]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();
  CycloneCacheConfig config{};
  config.cache_path = cache_path.c_str();
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);     // 10MB
  config.ram_cache_size_bytes = static_cast<uint64_t>(1 * 1024 * 1024);  // 1MB
  config.enable_checksum = 1;
  config.num_segments = 2;

  CycloneCacheHandle *cache = nullptr;
  CycloneError err = cyclone_cache_create(&config, &cache);
  REQUIRE(err == CYCLONE_OK);
  REQUIRE(cache != nullptr);

  cyclone_cache_destroy(cache);
}

TEST_CASE("C API: Write and read", "[c_api]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();
  CycloneCacheConfig config{};
  config.cache_path = cache_path.c_str();
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.ram_cache_size_bytes = static_cast<uint64_t>(1 * 1024 * 1024);
  config.enable_checksum = 1;
  config.num_segments = 2;

  CycloneCacheHandle *cache = nullptr;
  REQUIRE(cyclone_cache_create(&config, &cache) == CYCLONE_OK);

  const char *key = "test_key";
  const char *data = "Hello, World!";
  REQUIRE(cyclone_cache_write(cache, key, strlen(key), data, strlen(data)) ==
          CYCLONE_OK);

  CycloneReadHandle *rh = nullptr;
  REQUIRE(cyclone_cache_read(cache, key, strlen(key), &rh) == CYCLONE_OK);
  REQUIRE(rh != nullptr);

  const char *read_data = nullptr;
  size_t read_len = 0;
  REQUIRE(cyclone_cache_read_data(rh, &read_data, &read_len) == CYCLONE_OK);
  REQUIRE(read_len == strlen(data));
  REQUIRE(std::memcmp(read_data, data, read_len) == 0);

  cyclone_cache_read_close(rh);
  cyclone_cache_destroy(cache);
}

TEST_CASE("C API: Exists and delete", "[c_api]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();
  CycloneCacheConfig config{};
  config.cache_path = cache_path.c_str();
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.ram_cache_size_bytes = static_cast<uint64_t>(1 * 1024 * 1024);
  config.enable_checksum = 1;
  config.num_segments = 2;

  CycloneCacheHandle *cache = nullptr;
  REQUIRE(cyclone_cache_create(&config, &cache) == CYCLONE_OK);

  const char *key = "exist_key";
  const char *data = "data";

  REQUIRE(cyclone_cache_exists(cache, key, strlen(key)) == CYCLONE_NOT_FOUND);
  REQUIRE(cyclone_cache_write(cache, key, strlen(key), data, strlen(data)) ==
          CYCLONE_OK);
  REQUIRE(cyclone_cache_exists(cache, key, strlen(key)) == CYCLONE_OK);
  REQUIRE(cyclone_cache_delete(cache, key, strlen(key)) == CYCLONE_OK);
  REQUIRE(cyclone_cache_exists(cache, key, strlen(key)) == CYCLONE_NOT_FOUND);

  cyclone_cache_destroy(cache);
}

// =============================================================================
// Miss Callback Tests
// =============================================================================

TEST_CASE("C API: read_async without miss handler returns NOT_FOUND",
          "[c_api][miss_callback]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();
  CycloneCacheConfig config{};
  config.cache_path = cache_path.c_str();
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.ram_cache_size_bytes = static_cast<uint64_t>(1 * 1024 * 1024);
  config.enable_checksum = 1;
  config.num_segments = 2;

  CycloneCacheHandle *cache = nullptr;
  REQUIRE(cyclone_cache_create(&config, &cache) == CYCLONE_OK);

  const char *key = "missing_key";

  struct Context {
    std::atomic<bool> called{false};
    CycloneError err = CYCLONE_OK;
  } ctx;

  auto callback = [](void *ud, const char *, size_t, CycloneError err) {
    auto *c = static_cast<Context *>(ud);
    c->err = err;
    c->called.store(true);
  };

  REQUIRE(cyclone_cache_read_async(cache, key, strlen(key), callback, &ctx) ==
          CYCLONE_OK);
  REQUIRE(ctx.called.load());
  REQUIRE(ctx.err == CYCLONE_NOT_FOUND);

  cyclone_cache_destroy(cache);
}

TEST_CASE("C API: read_async cache hit skips miss handler",
          "[c_api][miss_callback]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();
  CycloneCacheConfig config{};
  config.cache_path = cache_path.c_str();
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.ram_cache_size_bytes = static_cast<uint64_t>(1 * 1024 * 1024);
  config.enable_checksum = 1;
  config.num_segments = 2;

  CycloneCacheHandle *cache = nullptr;
  REQUIRE(cyclone_cache_create(&config, &cache) == CYCLONE_OK);

  // Write data first
  const char *key = "cached_key";
  const char *data = "cached_data";
  REQUIRE(cyclone_cache_write(cache, key, strlen(key), data, strlen(data)) ==
          CYCLONE_OK);

  // Set up a miss handler that should NOT be called
  std::atomic<int> miss_handler_calls{0};
  auto miss_handler = [](const char *, size_t, void *ud,
                         CycloneMissDoneCallback, void *) {
    auto *calls = static_cast<std::atomic<int> *>(ud);
    calls->fetch_add(1);
  };
  REQUIRE(cyclone_cache_set_miss_handler(cache, miss_handler,
                                         &miss_handler_calls) == CYCLONE_OK);

  // Read should hit cache, not invoke miss handler
  struct Context {
    std::atomic<bool> called{false};
    CycloneError err = CYCLONE_OK;
    std::string data;
  } ctx;

  auto callback = [](void *ud, const char *d, size_t len, CycloneError err) {
    auto *c = static_cast<Context *>(ud);
    c->err = err;
    if (d && len > 0) c->data.assign(d, len);
    c->called.store(true);
  };

  REQUIRE(cyclone_cache_read_async(cache, key, strlen(key), callback, &ctx) ==
          CYCLONE_OK);
  REQUIRE(ctx.called.load());
  REQUIRE(ctx.err == CYCLONE_OK);
  REQUIRE(ctx.data == "cached_data");
  REQUIRE(miss_handler_calls.load() == 0);

  cyclone_cache_destroy(cache);
}

TEST_CASE("C API: Basic miss handler invocation", "[c_api][miss_callback]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();
  CycloneCacheConfig config{};
  config.cache_path = cache_path.c_str();
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.ram_cache_size_bytes = static_cast<uint64_t>(1 * 1024 * 1024);
  config.enable_checksum = 1;
  config.num_segments = 2;

  CycloneCacheHandle *cache = nullptr;
  REQUIRE(cyclone_cache_create(&config, &cache) == CYCLONE_OK);

  // Miss handler that returns fetched data synchronously
  auto miss_handler = [](const char *key, size_t key_len, void *,
                         CycloneMissDoneCallback done_cb, void *done_ud) {
    std::string fetched = "fetched_for_" + std::string(key, key_len);
    done_cb(done_ud, fetched.data(), fetched.size(), CYCLONE_OK);
  };
  REQUIRE(cyclone_cache_set_miss_handler(cache, miss_handler, nullptr) ==
          CYCLONE_OK);

  const char *key = "miss_key";
  struct Context {
    std::atomic<bool> called{false};
    CycloneError err = CYCLONE_OK;
    std::string data;
  } ctx;

  auto callback = [](void *ud, const char *d, size_t len, CycloneError err) {
    auto *c = static_cast<Context *>(ud);
    c->err = err;
    if (d && len > 0) c->data.assign(d, len);
    c->called.store(true);
  };

  REQUIRE(cyclone_cache_read_async(cache, key, strlen(key), callback, &ctx) ==
          CYCLONE_OK);
  REQUIRE(ctx.called.load());
  REQUIRE(ctx.err == CYCLONE_OK);
  REQUIRE(ctx.data == "fetched_for_miss_key");

  // Verify data was stored in cache
  REQUIRE(cyclone_cache_exists(cache, key, strlen(key)) == CYCLONE_OK);

  cyclone_cache_destroy(cache);
}

TEST_CASE("C API: A waiter observes the fetched value already stored",
          "[c_api][miss_callback]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();
  CycloneCacheConfig config{};
  config.cache_path = cache_path.c_str();
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.ram_cache_size_bytes = static_cast<uint64_t>(1 * 1024 * 1024);
  config.enable_checksum = 1;
  config.num_segments = 2;

  CycloneCacheHandle *cache = nullptr;
  REQUIRE(cyclone_cache_create(&config, &cache) == CYCLONE_OK);

  auto miss_handler = [](const char *, size_t, void *,
                         CycloneMissDoneCallback done_cb, void *done_ud) {
    const char value[] = "fetched";
    done_cb(done_ud, value, sizeof(value) - 1, CYCLONE_OK);
  };
  REQUIRE(cyclone_cache_set_miss_handler(cache, miss_handler, nullptr) ==
          CYCLONE_OK);

  const char *key = "store_before_notify";
  struct Context {
    CycloneCacheHandle *cache;
    const char *key;
    CycloneError exists_in_callback = CYCLONE_NOT_FOUND;
    bool called = false;
  } ctx{cache, key};

  auto callback = [](void *ud, const char *, size_t, CycloneError) {
    auto *c = static_cast<Context *>(ud);
    c->exists_in_callback =
        cyclone_cache_exists(c->cache, c->key, strlen(c->key));
    c->called = true;
  };

  REQUIRE(cyclone_cache_read_async(cache, key, strlen(key), callback, &ctx) ==
          CYCLONE_OK);
  REQUIRE(ctx.called);
  REQUIRE(ctx.exists_in_callback == CYCLONE_OK);

  cyclone_cache_destroy(cache);
}

size_t cyclone_test_completed_context_count(CycloneCacheHandle *cache);

TEST_CASE("C API: Completed miss contexts stay bounded and are reclaimed",
          "[c_api][miss_callback]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();
  CycloneCacheConfig config{};
  config.cache_path = cache_path.c_str();
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.ram_cache_size_bytes = static_cast<uint64_t>(1 * 1024 * 1024);
  config.enable_checksum = 1;
  config.num_segments = 2;

  CycloneCacheHandle *cache = nullptr;
  REQUIRE(cyclone_cache_create(&config, &cache) == CYCLONE_OK);

  struct Saved {
    CycloneMissDoneCallback first_cb = nullptr;
    void *first_ud = nullptr;
    CycloneMissDoneCallback held_cb = nullptr;
    void *held_ud = nullptr;
  } saved;

  auto miss_handler = [](const char *key, size_t key_len, void *ud,
                         CycloneMissDoneCallback done_cb, void *done_ud) {
    auto *s = static_cast<Saved *>(ud);
    std::string k(key, key_len);
    if (k == "held") {
      // Left outstanding on purpose.
      s->held_cb = done_cb;
      s->held_ud = done_ud;
      return;
    }
    if (k == "stray") {
      s->first_cb = done_cb;
      s->first_ud = done_ud;
    }
    const char value[] = "v";
    done_cb(done_ud, value, 1, CYCLONE_OK);
  };
  REQUIRE(cyclone_cache_set_miss_handler(cache, miss_handler, &saved) ==
          CYCLONE_OK);

  auto noop = [](void *, const char *, size_t, CycloneError) {};
  auto miss = [&](const std::string &key) {
    REQUIRE(cyclone_cache_read_async(cache, key.data(), key.size(), noop,
                                     nullptr) == CYCLONE_OK);
  };

  // A completed miss is remembered, so a stray second completion of it
  // lands on live memory and is ignored; the number remembered is capped.
  miss("held");
  miss("stray");
  const char again[] = "again";
  for (int i = 0; i < 3000; ++i) {
    miss("many_misses_" + std::to_string(i));
    REQUIRE(cyclone_test_completed_context_count(cache) <= 1024);
    if (i == 100) {
      // Still within the ring: ignored.
      saved.first_cb(saved.first_ud, again, sizeof(again) - 1, CYCLONE_OK);
    }
  }
  REQUIRE(cyclone_test_completed_context_count(cache) == 1024);

  // The last outstanding fetch completes; draining releases the list.
  const char value[] = "v";
  saved.held_cb(saved.held_ud, value, 1, CYCLONE_OK);
  REQUIRE(cyclone_cache_drain_pending(cache, 1000) == CYCLONE_OK);
  REQUIRE(cyclone_test_completed_context_count(cache) == 0);

  cyclone_cache_destroy(cache);
}

TEST_CASE("C API: Miss handler error propagation", "[c_api][miss_callback]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();
  CycloneCacheConfig config{};
  config.cache_path = cache_path.c_str();
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.ram_cache_size_bytes = static_cast<uint64_t>(1 * 1024 * 1024);
  config.enable_checksum = 1;
  config.num_segments = 2;

  CycloneCacheHandle *cache = nullptr;
  REQUIRE(cyclone_cache_create(&config, &cache) == CYCLONE_OK);

  // Miss handler that returns an error
  auto miss_handler = [](const char *, size_t, void *,
                         CycloneMissDoneCallback done_cb, void *done_ud) {
    done_cb(done_ud, nullptr, 0, CYCLONE_IO_ERROR);
  };
  REQUIRE(cyclone_cache_set_miss_handler(cache, miss_handler, nullptr) ==
          CYCLONE_OK);

  const char *key = "error_key";
  struct Context {
    std::atomic<bool> called{false};
    CycloneError err = CYCLONE_OK;
  } ctx;

  auto callback = [](void *ud, const char *, size_t, CycloneError err) {
    auto *c = static_cast<Context *>(ud);
    c->err = err;
    c->called.store(true);
  };

  REQUIRE(cyclone_cache_read_async(cache, key, strlen(key), callback, &ctx) ==
          CYCLONE_OK);
  REQUIRE(ctx.called.load());
  REQUIRE(ctx.err == CYCLONE_IO_ERROR);

  // Error should not be cached - subsequent read triggers miss handler again
  std::atomic<int> miss_count{0};
  auto counting_handler = [](const char *, size_t, void *ud,
                             CycloneMissDoneCallback done_cb, void *done_ud) {
    auto *count = static_cast<std::atomic<int> *>(ud);
    count->fetch_add(1);
    done_cb(done_ud, nullptr, 0, CYCLONE_IO_ERROR);
  };
  REQUIRE(cyclone_cache_set_miss_handler(cache, counting_handler,
                                         &miss_count) == CYCLONE_OK);

  Context ctx2;
  REQUIRE(cyclone_cache_read_async(cache, key, strlen(key), callback, &ctx2) ==
          CYCLONE_OK);
  REQUIRE(miss_count.load() == 1);  // Handler called again

  cyclone_cache_destroy(cache);
}

TEST_CASE("C API: Request coalescing - N concurrent reads trigger 1 fetch",
          "[c_api][miss_callback][coalescing]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();
  CycloneCacheConfig config{};
  config.cache_path = cache_path.c_str();
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.ram_cache_size_bytes = static_cast<uint64_t>(1 * 1024 * 1024);
  config.enable_checksum = 1;
  config.num_segments = 2;

  CycloneCacheHandle *cache = nullptr;
  REQUIRE(cyclone_cache_create(&config, &cache) == CYCLONE_OK);

  std::atomic<int> miss_handler_calls{0};
  std::latch start_latch(1);  // Hold miss handler until all reads are queued
  CycloneMissDoneCallback stored_done_cb = nullptr;
  void *stored_done_ud = nullptr;

  auto miss_handler = [](const char *key, size_t key_len, void *ud,
                         CycloneMissDoneCallback done_cb, void *done_ud) {
    auto *ctx =
        static_cast<std::tuple<std::atomic<int> *, std::latch *,
                               CycloneMissDoneCallback *, void **> *>(ud);
    std::get<0>(*ctx)->fetch_add(1);
    // Store callback for later
    *std::get<2>(*ctx) = done_cb;
    *std::get<3>(*ctx) = done_ud;
    // Wait for signal
    std::get<1>(*ctx)->wait();
    // Now complete
    std::string data = "coalesced_data";
    done_cb(done_ud, data.data(), data.size(), CYCLONE_OK);
  };

  auto handler_ctx = std::make_tuple(&miss_handler_calls, &start_latch,
                                     &stored_done_cb, &stored_done_ud);
  REQUIRE(cyclone_cache_set_miss_handler(cache, miss_handler, &handler_ctx) ==
          CYCLONE_OK);

  const char *key = "coalesce_key";
  constexpr int N = 5;
  std::atomic<int> callbacks_received{0};
  std::vector<std::string> received_data(N);
  std::vector<CycloneError> received_errors(N);

  struct CallbackCtx {
    std::atomic<int> *counter;
    std::string *data;
    CycloneError *err;
    int index;
  };
  std::vector<CallbackCtx> cb_contexts(N);

  auto callback = [](void *ud, const char *d, size_t len, CycloneError err) {
    auto *ctx = static_cast<CallbackCtx *>(ud);
    *ctx->err = err;
    if (d && len > 0) ctx->data->assign(d, len);
    ctx->counter->fetch_add(1);
  };

  // Launch N concurrent reads in separate threads
  std::vector<std::thread> threads;
  for (int i = 0; i < N; ++i) {
    cb_contexts[i] = {&callbacks_received, &received_data[i],
                      &received_errors[i], i};
    threads.emplace_back([&, i]() {
      cyclone_cache_read_async(cache, key, strlen(key), callback,
                               &cb_contexts[i]);
    });
  }

  // Wait for threads to call read_async
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Release the miss handler
  start_latch.count_down();

  // Wait for all threads
  for (auto &t : threads) t.join();

  // Wait for all callbacks
  while (callbacks_received.load() < N) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  // Verify: only 1 miss handler call, all N callbacks received same data
  REQUIRE(miss_handler_calls.load() == 1);
  REQUIRE(callbacks_received.load() == N);
  for (int i = 0; i < N; ++i) {
    REQUIRE(received_errors[i] == CYCLONE_OK);
    REQUIRE(received_data[i] == "coalesced_data");
  }

  cyclone_cache_destroy(cache);
}

TEST_CASE("C API: Concurrent different keys fire independently",
          "[c_api][miss_callback]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();
  CycloneCacheConfig config{};
  config.cache_path = cache_path.c_str();
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.ram_cache_size_bytes = static_cast<uint64_t>(1 * 1024 * 1024);
  config.enable_checksum = 1;
  config.num_segments = 2;

  CycloneCacheHandle *cache = nullptr;
  REQUIRE(cyclone_cache_create(&config, &cache) == CYCLONE_OK);

  std::atomic<int> miss_handler_calls{0};

  auto miss_handler = [](const char *key, size_t key_len, void *ud,
                         CycloneMissDoneCallback done_cb, void *done_ud) {
    auto *count = static_cast<std::atomic<int> *>(ud);
    count->fetch_add(1);
    std::string data = "data_for_" + std::string(key, key_len);
    done_cb(done_ud, data.data(), data.size(), CYCLONE_OK);
  };
  REQUIRE(cyclone_cache_set_miss_handler(cache, miss_handler,
                                         &miss_handler_calls) == CYCLONE_OK);

  std::vector<std::string> keys = {"key_a", "key_b", "key_c"};
  std::atomic<int> callbacks_received{0};

  auto callback = [](void *ud, const char *, size_t, CycloneError) {
    auto *count = static_cast<std::atomic<int> *>(ud);
    count->fetch_add(1);
  };

  for (const auto &k : keys) {
    cyclone_cache_read_async(cache, k.c_str(), k.size(), callback,
                             &callbacks_received);
  }

  // Each key should trigger its own miss handler
  REQUIRE(miss_handler_calls.load() == 3);
  REQUIRE(callbacks_received.load() == 3);

  cyclone_cache_destroy(cache);
}

TEST_CASE("C API: Async done callback from different thread",
          "[c_api][miss_callback][threading]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();
  CycloneCacheConfig config{};
  config.cache_path = cache_path.c_str();
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.ram_cache_size_bytes = static_cast<uint64_t>(1 * 1024 * 1024);
  config.enable_checksum = 1;
  config.num_segments = 2;

  CycloneCacheHandle *cache = nullptr;
  REQUIRE(cyclone_cache_create(&config, &cache) == CYCLONE_OK);

  // Miss handler that spawns a thread to call done_cb
  auto miss_handler = [](const char *key, size_t key_len, void *,
                         CycloneMissDoneCallback done_cb, void *done_ud) {
    std::thread([=]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      std::string data = "async_data";
      done_cb(done_ud, data.data(), data.size(), CYCLONE_OK);
    }).detach();
  };
  REQUIRE(cyclone_cache_set_miss_handler(cache, miss_handler, nullptr) ==
          CYCLONE_OK);

  const char *key = "async_key";
  struct Context {
    std::atomic<bool> called{false};
    CycloneError err = CYCLONE_OK;
    std::string data;
  } ctx;

  auto callback = [](void *ud, const char *d, size_t len, CycloneError err) {
    auto *c = static_cast<Context *>(ud);
    c->err = err;
    if (d && len > 0) c->data.assign(d, len);
    c->called.store(true);
  };

  REQUIRE(cyclone_cache_read_async(cache, key, strlen(key), callback, &ctx) ==
          CYCLONE_OK);

  // Wait for async completion
  for (int i = 0; i < 100 && !ctx.called.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  REQUIRE(ctx.called.load());
  REQUIRE(ctx.err == CYCLONE_OK);
  REQUIRE(ctx.data == "async_data");

  // Small delay to let detached thread finish before destroying cache
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  cyclone_cache_destroy(cache);
}

TEST_CASE("C API: Shutdown rejects new reads",
          "[c_api][miss_callback][shutdown]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();
  CycloneCacheConfig config{};
  config.cache_path = cache_path.c_str();
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.ram_cache_size_bytes = static_cast<uint64_t>(1 * 1024 * 1024);
  config.enable_checksum = 1;
  config.num_segments = 2;

  CycloneCacheHandle *cache = nullptr;
  REQUIRE(cyclone_cache_create(&config, &cache) == CYCLONE_OK);

  std::atomic<int> miss_calls{0};
  auto miss_handler = [](const char *, size_t, void *ud,
                         CycloneMissDoneCallback done_cb, void *done_ud) {
    auto *count = static_cast<std::atomic<int> *>(ud);
    count->fetch_add(1);
    done_cb(done_ud, "data", 4, CYCLONE_OK);
  };
  REQUIRE(cyclone_cache_set_miss_handler(cache, miss_handler, &miss_calls) ==
          CYCLONE_OK);

  // Initiate shutdown
  REQUIRE(cyclone_cache_drain_pending(cache, 1000) == CYCLONE_OK);

  // New reads after shutdown should return NOT_INITIALIZED
  struct Context {
    std::atomic<bool> called{false};
    CycloneError err = CYCLONE_OK;
  } ctx;

  auto callback = [](void *ud, const char *, size_t, CycloneError err) {
    auto *c = static_cast<Context *>(ud);
    c->err = err;
    c->called.store(true);
  };

  const char *key = "post_shutdown_key";
  REQUIRE(cyclone_cache_read_async(cache, key, strlen(key), callback, &ctx) ==
          CYCLONE_OK);
  REQUIRE(ctx.called.load());
  REQUIRE(ctx.err == CYCLONE_NOT_INITIALIZED);
  REQUIRE(miss_calls.load() == 0);  // Handler was not called

  cyclone_cache_destroy(cache);
}

TEST_CASE("C API: Shutdown drains in-flight fetches",
          "[c_api][miss_callback][shutdown]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();
  CycloneCacheConfig config{};
  config.cache_path = cache_path.c_str();
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.ram_cache_size_bytes = static_cast<uint64_t>(1 * 1024 * 1024);
  config.enable_checksum = 1;
  config.num_segments = 2;

  CycloneCacheHandle *cache = nullptr;
  REQUIRE(cyclone_cache_create(&config, &cache) == CYCLONE_OK);

  std::latch block_handler(1);
  std::atomic<bool> handler_started{false};

  auto miss_handler = [](const char *, size_t, void *ud,
                         CycloneMissDoneCallback done_cb, void *done_ud) {
    auto *ctx = static_cast<std::pair<std::latch *, std::atomic<bool> *> *>(ud);
    ctx->second->store(true);
    ctx->first->wait();  // Block until signaled
    done_cb(done_ud, "delayed_data", 12, CYCLONE_OK);
  };

  auto handler_ctx = std::make_pair(&block_handler, &handler_started);
  REQUIRE(cyclone_cache_set_miss_handler(cache, miss_handler, &handler_ctx) ==
          CYCLONE_OK);

  struct Context {
    std::atomic<bool> called{false};
    CycloneError err = CYCLONE_OK;
    std::string data;
  } ctx;

  auto callback = [](void *ud, const char *d, size_t len, CycloneError err) {
    auto *c = static_cast<Context *>(ud);
    c->err = err;
    if (d && len > 0) c->data.assign(d, len);
    c->called.store(true);
  };

  // Start async read that will block in miss handler
  std::thread read_thread([&]() {
    cyclone_cache_read_async(cache, "blocked_key", 11, callback, &ctx);
  });

  // Wait for handler to start
  while (!handler_started.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  // Start drain in another thread
  std::atomic<bool> drain_complete{false};
  std::thread drain_thread([&]() {
    cyclone_cache_drain_pending(cache, 5000);
    drain_complete.store(true);
  });

  // Give drain a moment to start waiting
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  REQUIRE(!drain_complete.load());  // Should still be waiting

  // Release the handler
  block_handler.count_down();

  // Wait for everything to complete
  read_thread.join();
  drain_thread.join();

  REQUIRE(drain_complete.load());
  REQUIRE(ctx.called.load());
  REQUIRE(ctx.err == CYCLONE_OK);
  REQUIRE(ctx.data == "delayed_data");

  cyclone_cache_destroy(cache);
}

// =============================================================================
// Stats Test
// =============================================================================

TEST_CASE("C API: Stats retrieval", "[c_api]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();
  CycloneCacheConfig config{};
  config.cache_path = cache_path.c_str();
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.ram_cache_size_bytes = static_cast<uint64_t>(1 * 1024 * 1024);
  config.enable_checksum = 1;
  config.num_segments = 2;

  CycloneCacheHandle *cache = nullptr;
  REQUIRE(cyclone_cache_create(&config, &cache) == CYCLONE_OK);

  // Write some data
  const char *key = "stats_key";
  const char *data = "stats_data";
  REQUIRE(cyclone_cache_write(cache, key, strlen(key), data, strlen(data)) ==
          CYCLONE_OK);

  CycloneCacheStats stats{};
  REQUIRE(cyclone_cache_stats(cache, &stats) == CYCLONE_OK);
  // bytes_written is not currently tracked per-operation
  // so verify current_entries instead which reflects actual stored data.
  REQUIRE(stats.current_entries > 0);

  cyclone_cache_destroy(cache);
}

TEST_CASE(
    "C API: disable_wrap_retention selects flush mode; zero keeps "
    "the default",
    "[c_api][retention]") {
  // Observable through the retention counters: with wrap retention the very
  // first write already advances the clean frontier through empty space.
  auto frontier_advances_after_one_write = [](int disable) {
    TempCacheDir tmp;
    std::string cache_path = tmp.path();
    CycloneCacheConfig config{};
    config.cache_path = cache_path.c_str();
    config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
    config.enable_checksum = 1;
    config.disable_wrap_retention = disable;
    CycloneCacheHandle *cache = nullptr;
    REQUIRE(cyclone_cache_create(&config, &cache) == CYCLONE_OK);
    const char *key = "mode_key";
    const char *data = "mode_data";
    REQUIRE(cyclone_cache_write(cache, key, strlen(key), data, strlen(data)) ==
            CYCLONE_OK);
    CycloneCacheStats stats{};
    REQUIRE(cyclone_cache_stats(cache, &stats) == CYCLONE_OK);
    cyclone_cache_destroy(cache);
    return stats.frontier_advances;
  };
  REQUIRE(frontier_advances_after_one_write(1) == 0);
  // Zero-initialised: the library default (CacheConfig::wrap_retention),
  // which is retention unless the test-seam override forces flush.
  const bool default_retains = cyclone::CacheConfig{}.wrap_retention;
  // NOLINTNEXTLINE(concurrency-mt-unsafe): read-only
  const char *forced = std::getenv("CYCLONE_TEST_WRAP_RETENTION");
  REQUIRE(default_retains == (forced == nullptr || forced[0] == '1'));
  REQUIRE((frontier_advances_after_one_write(0) > 0) == default_retains);
}

// =============================================================================
// Edge Case and Validation Tests
// =============================================================================

TEST_CASE("C API: Zero-length key rejected", "[c_api][edge]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();
  CycloneCacheConfig config{};
  config.cache_path = cache_path.c_str();
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.ram_cache_size_bytes = static_cast<uint64_t>(1 * 1024 * 1024);
  config.enable_checksum = 1;
  config.num_segments = 2;

  CycloneCacheHandle *cache = nullptr;
  REQUIRE(cyclone_cache_create(&config, &cache) == CYCLONE_OK);

  const char *data = "test_data";

  // All key-based functions should reject zero-length keys
  REQUIRE(cyclone_cache_write(cache, "x", 0, data, strlen(data)) ==
          CYCLONE_INVALID_ARGUMENT);
  REQUIRE(cyclone_cache_exists(cache, "x", 0) == CYCLONE_INVALID_ARGUMENT);
  REQUIRE(cyclone_cache_delete(cache, "x", 0) == CYCLONE_INVALID_ARGUMENT);

  CycloneReadHandle *rh = nullptr;
  REQUIRE(cyclone_cache_read(cache, "x", 0, &rh) == CYCLONE_INVALID_ARGUMENT);

  std::atomic<bool> cb_called{false};
  auto callback = [](void *ud, const char *, size_t, CycloneError) {
    static_cast<std::atomic<bool> *>(ud)->store(true);
  };
  REQUIRE(cyclone_cache_read_async(cache, "x", 0, callback, &cb_called) ==
          CYCLONE_INVALID_ARGUMENT);
  REQUIRE(!cb_called.load());  // Callback not called on validation error

  cyclone_cache_destroy(cache);
}

TEST_CASE("C API: NULL data with non-zero length rejected", "[c_api][edge]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();
  CycloneCacheConfig config{};
  config.cache_path = cache_path.c_str();
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.ram_cache_size_bytes = static_cast<uint64_t>(1 * 1024 * 1024);
  config.enable_checksum = 1;
  config.num_segments = 2;

  CycloneCacheHandle *cache = nullptr;
  REQUIRE(cyclone_cache_create(&config, &cache) == CYCLONE_OK);

  // NULL data with non-zero length should be rejected
  REQUIRE(cyclone_cache_write(cache, "key", 3, nullptr, 100) ==
          CYCLONE_INVALID_ARGUMENT);

  // NULL data with zero length should be OK (empty value)
  REQUIRE(cyclone_cache_write(cache, "empty_key", 9, nullptr, 0) == CYCLONE_OK);

  cyclone_cache_destroy(cache);
}

TEST_CASE("C API: done_cb called twice is safely ignored",
          "[c_api][miss_callback][edge]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();
  CycloneCacheConfig config{};
  config.cache_path = cache_path.c_str();
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.ram_cache_size_bytes = static_cast<uint64_t>(1 * 1024 * 1024);
  config.enable_checksum = 1;
  config.num_segments = 2;

  CycloneCacheHandle *cache = nullptr;
  REQUIRE(cyclone_cache_create(&config, &cache) == CYCLONE_OK);

  std::atomic<int> handler_calls{0};

  // Miss handler that calls done_cb TWICE
  auto miss_handler = [](const char *, size_t, void *ud,
                         CycloneMissDoneCallback done_cb, void *done_ud) {
    auto *calls = static_cast<std::atomic<int> *>(ud);
    calls->fetch_add(1);
    // First call
    done_cb(done_ud, "first_data", 10, CYCLONE_OK);
    // Second call (should be ignored)
    done_cb(done_ud, "second_data", 11, CYCLONE_OK);
  };
  REQUIRE(cyclone_cache_set_miss_handler(cache, miss_handler, &handler_calls) ==
          CYCLONE_OK);

  std::atomic<int> callbacks_received{0};
  std::string received_data;

  auto callback = [](void *ud, const char *d, size_t len, CycloneError err) {
    auto *ctx = static_cast<std::pair<std::atomic<int> *, std::string *> *>(ud);
    ctx->first->fetch_add(1);
    if (d && len > 0 && ctx->second->empty()) {
      ctx->second->assign(d, len);
    }
  };

  auto cb_ctx = std::make_pair(&callbacks_received, &received_data);
  REQUIRE(cyclone_cache_read_async(cache, "double_done_key", 15, callback,
                                   &cb_ctx) == CYCLONE_OK);

  // Wait a bit for any additional callbacks
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  REQUIRE(handler_calls.load() == 1);
  REQUIRE(callbacks_received.load() ==
          1);  // Only one callback despite two done_cb calls
  REQUIRE(received_data == "first_data");  // Got first data, not second

  cyclone_cache_destroy(cache);
}

TEST_CASE("C API: Max in-flight limit enforced", "[c_api][miss_callback]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();
  CycloneCacheConfig config{};
  config.cache_path = cache_path.c_str();
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.ram_cache_size_bytes = static_cast<uint64_t>(1 * 1024 * 1024);
  config.enable_checksum = 1;
  config.num_segments = 2;
  config.max_in_flight_requests = 3;  // Small limit for testing

  CycloneCacheHandle *cache = nullptr;
  REQUIRE(cyclone_cache_create(&config, &cache) == CYCLONE_OK);

  std::latch block_handlers(1);
  std::atomic<int> handler_calls{0};

  // Handler that blocks until signaled
  auto miss_handler = [](const char *, size_t, void *ud,
                         CycloneMissDoneCallback done_cb, void *done_ud) {
    auto *ctx = static_cast<std::pair<std::latch *, std::atomic<int> *> *>(ud);
    ctx->second->fetch_add(1);
    ctx->first->wait();
    done_cb(done_ud, "data", 4, CYCLONE_OK);
  };

  auto handler_ctx = std::make_pair(&block_handlers, &handler_calls);
  REQUIRE(cyclone_cache_set_miss_handler(cache, miss_handler, &handler_ctx) ==
          CYCLONE_OK);

  std::atomic<int> ok_callbacks{0};
  std::atomic<int> no_space_callbacks{0};

  auto callback = [](void *ud, const char *, size_t, CycloneError err) {
    auto *ctx =
        static_cast<std::pair<std::atomic<int> *, std::atomic<int> *> *>(ud);
    if (err == CYCLONE_OK) {
      ctx->first->fetch_add(1);
    } else if (err == CYCLONE_NO_SPACE) {
      ctx->second->fetch_add(1);
    }
  };

  auto cb_ctx = std::make_pair(&ok_callbacks, &no_space_callbacks);

  // Start 3 async reads (should all succeed, hitting limit)
  std::vector<std::thread> threads;
  for (int i = 0; i < 3; ++i) {
    std::string key = "key_" + std::to_string(i);
    threads.emplace_back([&, key]() {
      cyclone_cache_read_async(cache, key.c_str(), key.size(), callback,
                               &cb_ctx);
    });
  }

  // Wait for handlers to start
  while (handler_calls.load() < 3) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  // 4th request with different key should get NO_SPACE
  cyclone_cache_read_async(cache, "key_overflow", 12, callback, &cb_ctx);

  // But coalescing to existing key should still work
  cyclone_cache_read_async(cache, "key_0", 5, callback, &cb_ctx);

  // Release handlers
  block_handlers.count_down();

  for (auto &t : threads) t.join();

  // Wait for all callbacks
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  REQUIRE(handler_calls.load() == 3);  // Only 3 unique keys triggered handlers
  REQUIRE(no_space_callbacks.load() == 1);  // 1 rejected due to limit
  REQUIRE(ok_callbacks.load() == 4);        // 3 original + 1 coalesced

  cyclone_cache_destroy(cache);
}

TEST_CASE("C API: set_miss_handler while reads in flight",
          "[c_api][miss_callback][concurrent]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();
  CycloneCacheConfig config{};
  config.cache_path = cache_path.c_str();
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.ram_cache_size_bytes = static_cast<uint64_t>(1 * 1024 * 1024);
  config.enable_checksum = 1;
  config.num_segments = 2;

  CycloneCacheHandle *cache = nullptr;
  REQUIRE(cyclone_cache_create(&config, &cache) == CYCLONE_OK);

  std::latch block_handler(1);
  std::atomic<bool> handler1_started{false};
  std::atomic<int> handler1_calls{0};
  std::atomic<int> handler2_calls{0};

  // First handler that blocks
  auto handler1 = [](const char *, size_t, void *ud,
                     CycloneMissDoneCallback done_cb, void *done_ud) {
    auto *ctx = static_cast<
        std::tuple<std::latch *, std::atomic<bool> *, std::atomic<int> *> *>(
        ud);
    std::get<2>(*ctx)->fetch_add(1);
    std::get<1>(*ctx)->store(true);
    std::get<0>(*ctx)->wait();
    done_cb(done_ud, "handler1_data", 13, CYCLONE_OK);
  };

  // Second handler (replacement)
  auto handler2 = [](const char *, size_t, void *ud,
                     CycloneMissDoneCallback done_cb, void *done_ud) {
    auto *count = static_cast<std::atomic<int> *>(ud);
    count->fetch_add(1);
    done_cb(done_ud, "handler2_data", 13, CYCLONE_OK);
  };

  auto handler1_ctx =
      std::make_tuple(&block_handler, &handler1_started, &handler1_calls);
  REQUIRE(cyclone_cache_set_miss_handler(cache, handler1, &handler1_ctx) ==
          CYCLONE_OK);

  struct CallbackCtx {
    std::atomic<bool> called{false};
    std::string data;
  };
  CallbackCtx ctx1, ctx2;

  auto callback = [](void *ud, const char *d, size_t len, CycloneError) {
    auto *ctx = static_cast<CallbackCtx *>(ud);
    if (d && len > 0) ctx->data.assign(d, len);
    ctx->called.store(true);
  };

  // Start first async read (will block in handler1)
  std::thread read1(
      [&]() { cyclone_cache_read_async(cache, "key1", 4, callback, &ctx1); });

  // Wait for handler1 to start
  while (!handler1_started.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  // Change handler while first read is in flight
  REQUIRE(cyclone_cache_set_miss_handler(cache, handler2, &handler2_calls) ==
          CYCLONE_OK);

  // Start second async read (should use handler2)
  cyclone_cache_read_async(cache, "key2", 4, callback, &ctx2);

  // Release handler1
  block_handler.count_down();
  read1.join();

  // Wait for callbacks
  while (!ctx1.called.load() || !ctx2.called.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  REQUIRE(handler1_calls.load() == 1);
  REQUIRE(handler2_calls.load() == 1);
  REQUIRE(ctx1.data == "handler1_data");  // First read used original handler
  REQUIRE(ctx2.data == "handler2_data");  // Second read used new handler

  cyclone_cache_destroy(cache);
}

TEST_CASE("C API: Large key handling", "[c_api][edge]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();
  CycloneCacheConfig config{};
  config.cache_path = cache_path.c_str();
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.ram_cache_size_bytes = static_cast<uint64_t>(1 * 1024 * 1024);
  config.enable_checksum = 1;
  config.num_segments = 2;

  CycloneCacheHandle *cache = nullptr;
  REQUIRE(cyclone_cache_create(&config, &cache) == CYCLONE_OK);

  // 64KB key
  std::string large_key(static_cast<size_t>(64 * 1024), 'x');
  const char *data = "data_for_large_key";

  // Should work (key gets hashed to fixed size internally)
  REQUIRE(cyclone_cache_write(cache, large_key.c_str(), large_key.size(), data,
                              strlen(data)) == CYCLONE_OK);
  REQUIRE(cyclone_cache_exists(cache, large_key.c_str(), large_key.size()) ==
          CYCLONE_OK);

  CycloneReadHandle *rh = nullptr;
  REQUIRE(cyclone_cache_read(cache, large_key.c_str(), large_key.size(), &rh) ==
          CYCLONE_OK);

  const char *read_data = nullptr;
  size_t read_len = 0;
  REQUIRE(cyclone_cache_read_data(rh, &read_data, &read_len) == CYCLONE_OK);
  REQUIRE(std::string(read_data, read_len) == data);

  cyclone_cache_read_close(rh);
  cyclone_cache_destroy(cache);
}

TEST_CASE("C API: Large data handling", "[c_api][edge]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();
  CycloneCacheConfig config{};
  config.cache_path = cache_path.c_str();
  config.cache_size_bytes =
      static_cast<uint64_t>(50 * 1024 * 1024);  // 50MB cache
  config.ram_cache_size_bytes = static_cast<uint64_t>(1 * 1024 * 1024);
  config.enable_checksum = 1;
  config.num_segments = 2;

  CycloneCacheHandle *cache = nullptr;
  REQUIRE(cyclone_cache_create(&config, &cache) == CYCLONE_OK);

  const char *key = "large_data_key";
  // 100KB data (comfortably fits within write buffer limits)
  std::vector<char> large_data(static_cast<size_t>(100 * 1024));
  for (size_t i = 0; i < large_data.size(); ++i) {
    large_data[i] = static_cast<char>('A' + (i % 26));
  }

  REQUIRE(cyclone_cache_write(cache, key, strlen(key), large_data.data(),
                              large_data.size()) == CYCLONE_OK);

  CycloneReadHandle *rh = nullptr;
  REQUIRE(cyclone_cache_read(cache, key, strlen(key), &rh) == CYCLONE_OK);

  const char *read_data = nullptr;
  size_t read_len = 0;
  REQUIRE(cyclone_cache_read_data(rh, &read_data, &read_len) == CYCLONE_OK);
  REQUIRE(read_len == large_data.size());
  REQUIRE(std::memcmp(read_data, large_data.data(), read_len) == 0);

  cyclone_cache_read_close(rh);
  cyclone_cache_destroy(cache);
}

TEST_CASE("C API: Coalescing stress test", "[c_api][stress]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();
  CycloneCacheConfig config{};
  config.cache_path = cache_path.c_str();
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.ram_cache_size_bytes = static_cast<uint64_t>(1 * 1024 * 1024);
  config.enable_checksum = 1;
  config.num_segments = 2;

  CycloneCacheHandle *cache = nullptr;
  REQUIRE(cyclone_cache_create(&config, &cache) == CYCLONE_OK);

  std::atomic<int> handler_calls{0};
  std::latch start_latch(1);

  auto miss_handler = [](const char *, size_t, void *ud,
                         CycloneMissDoneCallback done_cb, void *done_ud) {
    auto *ctx = static_cast<std::pair<std::atomic<int> *, std::latch *> *>(ud);
    ctx->first->fetch_add(1);
    ctx->second->wait();
    done_cb(done_ud, "stress_data", 11, CYCLONE_OK);
  };

  auto handler_ctx = std::make_pair(&handler_calls, &start_latch);
  REQUIRE(cyclone_cache_set_miss_handler(cache, miss_handler, &handler_ctx) ==
          CYCLONE_OK);

  constexpr int N = 100;  // 100 concurrent readers
  std::atomic<int> ok_callbacks{0};
  std::atomic<int> error_callbacks{0};

  auto callback = [](void *ud, const char *d, size_t len, CycloneError err) {
    auto *ctx =
        static_cast<std::pair<std::atomic<int> *, std::atomic<int> *> *>(ud);
    if (err == CYCLONE_OK && d && len == 11) {
      ctx->first->fetch_add(1);
    } else {
      ctx->second->fetch_add(1);
    }
  };

  auto cb_ctx = std::make_pair(&ok_callbacks, &error_callbacks);

  const char *key = "stress_key";
  std::vector<std::thread> threads;
  threads.reserve(N);

  for (int i = 0; i < N; ++i) {
    threads.emplace_back([&]() {
      cyclone_cache_read_async(cache, key, strlen(key), callback, &cb_ctx);
    });
  }

  // Wait for all threads to have called read_async
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Release the handler
  start_latch.count_down();

  for (auto &t : threads) t.join();

  // Wait for all callbacks
  while (ok_callbacks.load() + error_callbacks.load() < N) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  REQUIRE(handler_calls.load() == 1);  // Only 1 handler call despite N readers
  REQUIRE(ok_callbacks.load() == N);   // All N readers got data
  REQUIRE(error_callbacks.load() == 0);

  cyclone_cache_destroy(cache);
}

// =============================================================================
// Phase 6C: C API Null Handle and Error Code Tests
// =============================================================================

TEST_CASE("C API null handle errors", "[c_api][error]") {
  // Call cyclone_cache_read() with NULL cache handle
  {
    CycloneReadHandle *rh = nullptr;
    CycloneError err = cyclone_cache_read(nullptr, "key", 3, &rh);
    REQUIRE(err == CYCLONE_INVALID_ARGUMENT);
    REQUIRE(rh == nullptr);
  }

  // Call cyclone_cache_write() with NULL cache handle
  {
    CycloneError err = cyclone_cache_write(nullptr, "key", 3, "data", 4);
    REQUIRE(err == CYCLONE_INVALID_ARGUMENT);
  }

  // Call cyclone_cache_delete() with NULL cache handle
  {
    CycloneError err = cyclone_cache_delete(nullptr, "key", 3);
    REQUIRE(err == CYCLONE_INVALID_ARGUMENT);
  }
}

TEST_CASE("C API error code mapping completeness", "[c_api][error]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();
  CycloneCacheConfig config{};
  config.cache_path = cache_path.c_str();
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.ram_cache_size_bytes = static_cast<uint64_t>(1 * 1024 * 1024);
  config.enable_checksum = 1;
  config.num_segments = 2;

  CycloneCacheHandle *cache = nullptr;
  REQUIRE(cyclone_cache_create(&config, &cache) == CYCLONE_OK);

  // Read a key that doesn't exist -> CYCLONE_NOT_FOUND
  {
    CycloneReadHandle *rh = nullptr;
    CycloneError err = cyclone_cache_read(cache, "missing_key", 11, &rh);
    REQUIRE(err == CYCLONE_NOT_FOUND);
    REQUIRE(rh == nullptr);
  }

  // exists() on a key that doesn't exist -> CYCLONE_NOT_FOUND
  {
    CycloneError err = cyclone_cache_exists(cache, "missing_key", 11);
    REQUIRE(err == CYCLONE_NOT_FOUND);
  }

  // Write a key, then verify it returns CYCLONE_OK
  {
    const char *key = "error_test_key";
    const char *data = "error_test_data";
    CycloneError err =
        cyclone_cache_write(cache, key, strlen(key), data, strlen(data));
    REQUIRE(err == CYCLONE_OK);
  }

  // Verify exists() returns CYCLONE_OK for a present key
  {
    CycloneError err = cyclone_cache_exists(cache, "error_test_key", 14);
    REQUIRE(err == CYCLONE_OK);
  }

  // Delete a key that doesn't exist -> CYCLONE_NOT_FOUND
  {
    CycloneError err = cyclone_cache_delete(cache, "nonexistent_delete", 18);
    REQUIRE(err == CYCLONE_NOT_FOUND);
  }

  cyclone_cache_destroy(cache);
}

// =============================================================================
// Small-Object Tier Tests
// =============================================================================

TEST_CASE("C API: Tier-tagged operations route to the small tier",
          "[c_api][smalltier]") {
  // 320MB total with a 40% carve-out enables the small tier (the small
  // volume is floored at one 128MB stripe).
  TempCacheDir tmp;
  std::string cache_path = tmp.path();
  CycloneCacheConfig config{};
  config.cache_path = cache_path.c_str();
  config.cache_size_bytes = static_cast<uint64_t>(320) * 1024 * 1024;
  config.ram_cache_size_bytes = 0;
  config.enable_checksum = 1;
  config.num_segments = 2;
  config.small_tier_percent = 40;

  CycloneCacheHandle *cache = nullptr;
  REQUIRE(cyclone_cache_create(&config, &cache) == CYCLONE_OK);
  REQUIRE(cyclone_cache_small_tier_active(cache) == 1);
  REQUIRE(cyclone_cache_small_tier_active(nullptr) == 0);

  const char *key = "tier_key";
  const char *data = "small tier payload";

  REQUIRE(cyclone_cache_write_tier(cache, key, strlen(key), data, strlen(data),
                                   CYCLONE_TIER_SMALL) == CYCLONE_OK);

  // Visible through the small tier...
  REQUIRE(cyclone_cache_exists_tier(cache, key, strlen(key),
                                    CYCLONE_TIER_SMALL) == CYCLONE_OK);
  {
    CycloneReadHandle *rh = nullptr;
    REQUIRE(cyclone_cache_read_tier(cache, key, strlen(key), CYCLONE_TIER_SMALL,
                                    &rh) == CYCLONE_OK);
    const char *read_data = nullptr;
    size_t read_len = 0;
    REQUIRE(cyclone_cache_read_data(rh, &read_data, &read_len) == CYCLONE_OK);
    REQUIRE(read_len == strlen(data));
    REQUIRE(std::memcmp(read_data, data, read_len) == 0);
    cyclone_cache_read_close(rh);
  }

  // ...but NOT through the default tier (separate keyspaces).
  REQUIRE(cyclone_cache_exists(cache, key, strlen(key)) == CYCLONE_NOT_FOUND);
  {
    CycloneReadHandle *rh = nullptr;
    REQUIRE(cyclone_cache_read(cache, key, strlen(key), &rh) ==
            CYCLONE_NOT_FOUND);
  }
  // CYCLONE_TIER_DEFAULT behaves exactly like the tier-less functions.
  REQUIRE(cyclone_cache_exists_tier(cache, key, strlen(key),
                                    CYCLONE_TIER_DEFAULT) == CYCLONE_NOT_FOUND);

  // Delete through the small tier.
  REQUIRE(cyclone_cache_delete_tier(cache, key, strlen(key),
                                    CYCLONE_TIER_SMALL) == CYCLONE_OK);
  REQUIRE(cyclone_cache_exists_tier(cache, key, strlen(key),
                                    CYCLONE_TIER_SMALL) == CYCLONE_NOT_FOUND);

  cyclone_cache_destroy(cache);
}

TEST_CASE("C API: Small tier falls back to default when disabled",
          "[c_api][smalltier]") {
  // small_tier_percent defaults to 0 in the zero-initialized config: tier
  // disabled, CYCLONE_TIER_SMALL operations behave like the default tier.
  TempCacheDir tmp;
  std::string cache_path = tmp.path();
  CycloneCacheConfig config{};
  config.cache_path = cache_path.c_str();
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.ram_cache_size_bytes = 0;
  config.enable_checksum = 1;
  config.num_segments = 2;

  CycloneCacheHandle *cache = nullptr;
  REQUIRE(cyclone_cache_create(&config, &cache) == CYCLONE_OK);
  REQUIRE(cyclone_cache_small_tier_active(cache) == 0);

  const char *key = "fallback_key";
  const char *data = "fallback payload";
  REQUIRE(cyclone_cache_write_tier(cache, key, strlen(key), data, strlen(data),
                                   CYCLONE_TIER_SMALL) == CYCLONE_OK);

  // One shared keyspace: readable through both tier tags.
  REQUIRE(cyclone_cache_exists(cache, key, strlen(key)) == CYCLONE_OK);
  REQUIRE(cyclone_cache_exists_tier(cache, key, strlen(key),
                                    CYCLONE_TIER_SMALL) == CYCLONE_OK);
  {
    CycloneReadHandle *rh = nullptr;
    REQUIRE(cyclone_cache_read(cache, key, strlen(key), &rh) == CYCLONE_OK);
    const char *read_data = nullptr;
    size_t read_len = 0;
    REQUIRE(cyclone_cache_read_data(rh, &read_data, &read_len) == CYCLONE_OK);
    REQUIRE(read_len == strlen(data));
    REQUIRE(std::memcmp(read_data, data, read_len) == 0);
    cyclone_cache_read_close(rh);
  }

  cyclone_cache_destroy(cache);
}

// =============================================================================
// max_object_size Knob Tests
// =============================================================================
//
// The C config's max_object_size uses sentinel semantics so a zero-initialised
// config lands on the library default (64MB), NOT on "disabled": 0 = default,
// UINT64_MAX = disabled, any other N = bound.  A stripe must hold a whole
// object and auto striping only reaches >=64MB stripes at a 2GB volume, so
// the two tests that must ADMIT a 64MB object use one.  Payload sizing as in
// tests/integration/test_max_object_size.cpp: an over-bound write is rejected
// at open on its DECLARED length, so the big buffer is allocated but never
// streamed.

TEST_CASE("C API: zero-initialised max_object_size keeps the 64MB default",
          "[c_api][object_size]") {
  // The 0 sentinel covers an explicit 0 and a memset struct alike (same
  // value): both must behave as the untouched C++ default.
  TempCacheDir tmp;
  std::string cache_path = tmp.path();
  CycloneCacheConfig config{};
  config.cache_path = cache_path.c_str();
  config.cache_size_bytes = static_cast<uint64_t>(2) * 1024 * 1024 * 1024;
  config.ram_cache_size_bytes = 0;
  config.enable_checksum = 1;
  config.num_segments = 2;

  CycloneCacheHandle *cache = nullptr;
  REQUIRE(cyclone_cache_create(&config, &cache) == CYCLONE_OK);

  const char *key = "zero_init_bound";
  constexpr size_t kMB = static_cast<size_t>(1024) * 1024;
  std::vector<char> big(64 * kMB + 1, 'x');

  // 64MB + 1 is over the default bound: rejected, nothing stored.
  REQUIRE(cyclone_cache_write(cache, key, strlen(key), big.data(),
                              big.size()) == CYCLONE_OBJECT_TOO_LARGE);
  REQUIRE(cyclone_cache_exists(cache, key, strlen(key)) == CYCLONE_NOT_FOUND);

  // Exactly 64MB is at the default bound: admitted.
  REQUIRE(cyclone_cache_write(cache, key, strlen(key), big.data(), 64 * kMB) ==
          CYCLONE_OK);
  REQUIRE(cyclone_cache_exists(cache, key, strlen(key)) == CYCLONE_OK);

  cyclone_cache_destroy(cache);
}

TEST_CASE("C API: explicit max_object_size bound enforced on plain writes",
          "[c_api][object_size]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();
  CycloneCacheConfig config{};
  config.cache_path = cache_path.c_str();
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.ram_cache_size_bytes = 0;
  config.enable_checksum = 1;
  config.num_segments = 2;
  config.max_object_size = 1024;

  CycloneCacheHandle *cache = nullptr;
  REQUIRE(cyclone_cache_create(&config, &cache) == CYCLONE_OK);

  const char *key = "small_bound_key";
  std::vector<char> over(1025, 'o');
  std::vector<char> at(1024, 'a');

  // Over the bound: rejected, nothing stored.
  REQUIRE(cyclone_cache_write(cache, key, strlen(key), over.data(),
                              over.size()) == CYCLONE_OBJECT_TOO_LARGE);
  REQUIRE(cyclone_cache_exists(cache, key, strlen(key)) == CYCLONE_NOT_FOUND);

  // At exactly the bound: admitted and served.
  REQUIRE(cyclone_cache_write(cache, key, strlen(key), at.data(), at.size()) ==
          CYCLONE_OK);
  CycloneReadHandle *rh = nullptr;
  REQUIRE(cyclone_cache_read(cache, key, strlen(key), &rh) == CYCLONE_OK);
  const char *read_data = nullptr;
  size_t read_len = 0;
  REQUIRE(cyclone_cache_read_data(rh, &read_data, &read_len) == CYCLONE_OK);
  REQUIRE(read_len == at.size());
  REQUIRE(std::memcmp(read_data, at.data(), read_len) == 0);
  cyclone_cache_read_close(rh);

  // Under the bound: admitted.
  const char *small = "small enough";
  REQUIRE(cyclone_cache_write(cache, "small_key", strlen("small_key"), small,
                              strlen(small)) == CYCLONE_OK);

  cyclone_cache_destroy(cache);
}

TEST_CASE("C API: miss handler write-back over max_object_size is rejected",
          "[c_api][object_size][miss_callback]") {
  // internal_miss_done (cyclone_c.cpp) writes the fetched response back via
  // cyclone_cache_write, which routes through the same open-time bound check
  // as any plain write.  An over-bound upstream response must NOT be
  // stored; the waiter still gets the data (write-back failure is non-fatal
  // by design).
  TempCacheDir tmp;
  std::string cache_path = tmp.path();
  CycloneCacheConfig config{};
  config.cache_path = cache_path.c_str();
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.ram_cache_size_bytes = 0;
  config.enable_checksum = 1;
  config.num_segments = 2;
  config.max_object_size = 1024;

  CycloneCacheHandle *cache = nullptr;
  REQUIRE(cyclone_cache_create(&config, &cache) == CYCLONE_OK);

  auto miss_handler = [](const char *, size_t, void *,
                         CycloneMissDoneCallback done_cb, void *done_ud) {
    std::vector<char> big(2048, 'b');
    done_cb(done_ud, big.data(), big.size(), CYCLONE_OK);
  };
  REQUIRE(cyclone_cache_set_miss_handler(cache, miss_handler, nullptr) ==
          CYCLONE_OK);

  const char *key = "oversized_miss";
  struct Context {
    std::atomic<bool> called{false};
    CycloneError err = CYCLONE_OK;
    size_t len = 0;
  } ctx;

  auto callback = [](void *ud, const char *, size_t len, CycloneError err) {
    auto *c = static_cast<Context *>(ud);
    c->err = err;
    c->len = len;
    c->called.store(true);
  };

  REQUIRE(cyclone_cache_read_async(cache, key, strlen(key), callback, &ctx) ==
          CYCLONE_OK);
  // The waiter still receives the upstream response...
  REQUIRE(ctx.called.load());
  REQUIRE(ctx.err == CYCLONE_OK);
  REQUIRE(ctx.len == 2048);
  // ...but the over-bound write-back is rejected: nothing was stored.
  REQUIRE(cyclone_cache_exists(cache, key, strlen(key)) == CYCLONE_NOT_FOUND);

  cyclone_cache_destroy(cache);
}

TEST_CASE("C API: max_object_size UINT64_MAX disables the bound",
          "[c_api][object_size]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();
  CycloneCacheConfig config{};
  config.cache_path = cache_path.c_str();
  config.cache_size_bytes = static_cast<uint64_t>(2) * 1024 * 1024 * 1024;
  config.ram_cache_size_bytes = 0;
  config.enable_checksum = 1;
  config.num_segments = 2;
  config.max_object_size = UINT64_MAX;

  CycloneCacheHandle *cache = nullptr;
  REQUIRE(cyclone_cache_create(&config, &cache) == CYCLONE_OK);

  const char *key = "disabled_bound_key";
  std::vector<char> big(static_cast<size_t>(64) * 1024 * 1024 + 1, 'x');

  // Over the 64MB default: admitted because the bound is disabled.
  REQUIRE(cyclone_cache_write(cache, key, strlen(key), big.data(),
                              big.size()) == CYCLONE_OK);
  REQUIRE(cyclone_cache_exists(cache, key, strlen(key)) == CYCLONE_OK);

  cyclone_cache_destroy(cache);
}

// =============================================================================
// Cross-process RAM coherence knob
// =============================================================================
//
// Positive logic at the C boundary, because the C++ default is already the
// safe one: a zero-initialised config lands on OFF, which is the historical
// behaviour.  The four cases below are the same shape the earlier knob tests
// established for
// max_object_size -- the default, the knob alone, each missing precondition,
// and the fully-enabled combination -- because the failure mode this guards is
// a consumer BELIEVING it enabled a correctness guarantee that is in fact
// inert.

namespace {

// One cache, created from `config`, reporting whether coherence took effect.
// Every configuration here must CREATE successfully: an inert knob is inert,
// never an error.
int coherence_active_for(CycloneCacheConfig config) {
  CycloneCacheHandle *cache = nullptr;
  REQUIRE(cyclone_cache_create(&config, &cache) == CYCLONE_OK);
  const int active = cyclone_cache_cross_process_ram_coherence_active(cache);
  cyclone_cache_destroy(cache);
  return active;
}

CycloneCacheConfig coherence_base_config(const std::string &path) {
  CycloneCacheConfig config{};
  config.cache_path = path.c_str();
  config.cache_size_bytes = static_cast<uint64_t>(64) * 1024 * 1024;
  config.ram_cache_size_bytes = static_cast<uint64_t>(8) * 1024 * 1024;
  config.enable_checksum = 1;
  config.num_segments = 2;
  return config;
}

}  // namespace

TEST_CASE("C API: zero-initialised config leaves RAM coherence OFF",
          "[c_api][coherence]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();
  CycloneCacheConfig config{};
  config.cache_path = cache_path.c_str();
  config.cache_size_bytes = static_cast<uint64_t>(64) * 1024 * 1024;
  config.ram_cache_size_bytes = static_cast<uint64_t>(8) * 1024 * 1024;
  config.enable_checksum = 1;
  config.num_segments = 2;
  // enable_cross_process_ram_coherence deliberately untouched: a memset
  // struct and an explicit 0 are the same value and must behave alike.
  REQUIRE(config.enable_cross_process_ram_coherence == 0);

  CycloneCacheHandle *cache = nullptr;
  REQUIRE(cyclone_cache_create(&config, &cache) == CYCLONE_OK);
  REQUIRE(cyclone_cache_cross_process_ram_coherence_active(cache) == 0);

  // The cache still works, and the counters exist and read zero.
  const char *key = "coherence_zero_init";
  const char *value = "payload";
  REQUIRE(cyclone_cache_write(cache, key, strlen(key), value, strlen(value)) ==
          CYCLONE_OK);
  REQUIRE(cyclone_cache_exists(cache, key, strlen(key)) == CYCLONE_OK);

  CycloneCacheStats stats{};
  REQUIRE(cyclone_cache_stats(cache, &stats) == CYCLONE_OK);
  REQUIRE(stats.ram_coherence_rejections == 0);
  REQUIRE(stats.ram_coherence_put_rejections == 0);

  cyclone_cache_destroy(cache);
}

TEST_CASE("C API: RAM coherence needs the mmap directory and a RAM tier",
          "[c_api][coherence]") {
  TempCacheDir tmp_a;
  TempCacheDir tmp_b;
  TempCacheDir tmp_c;
  TempCacheDir tmp_d;
  std::string path_a = tmp_a.path();
  std::string path_b = tmp_b.path();
  std::string path_c = tmp_c.path();
  std::string path_d = tmp_d.path();

  // (1) Knob alone, no mmap directory: inert.  There is no shared version
  // array to validate against, so the knob must not pretend otherwise.
  CycloneCacheConfig knob_only = coherence_base_config(path_a);
  knob_only.enable_cross_process_ram_coherence = 1;
  REQUIRE(coherence_active_for(knob_only) == 0);

  // (2) mmap directory but knob off: inert (this is the default posture of
  // every multi-process consumer today).
  CycloneCacheConfig mmap_only = coherence_base_config(path_b);
  mmap_only.enable_mmap_directory = 1;
  REQUIRE(coherence_active_for(mmap_only) == 0);

  // (3) Knob + mmap directory but NO RAM tier: inert, nothing to validate.
  CycloneCacheConfig no_ram = coherence_base_config(path_c);
  no_ram.enable_mmap_directory = 1;
  no_ram.enable_cross_process_ram_coherence = 1;
  no_ram.ram_cache_size_bytes = 0;
  REQUIRE(coherence_active_for(no_ram) == 0);

  // (4) All three: ACTIVE.
  CycloneCacheConfig full = coherence_base_config(path_d);
  full.enable_mmap_directory = 1;
  full.enable_cross_process_ram_coherence = 1;
  REQUIRE(coherence_active_for(full) == 1);
}

// =============================================================================
// Read handle lifetime
// =============================================================================

TEST_CASE("C API: an open read handle does not hold a disk borrow",
          "[c_api][read]") {
  // cyclone_cache_read() copies the entry into the handle, and the copy is
  // what cyclone_cache_read_data() serves.  The handle must not also keep
  // the disk borrow open: a borrow pins the stripe and defers wraps for as
  // long as the application keeps the handle.
  TempCacheDir tmp;
  std::string cache_path = tmp.path();
  CycloneCacheConfig config{};
  config.cache_path = cache_path.c_str();
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.ram_cache_size_bytes = 0;  // disk only: every hit is a disk hit
  config.enable_checksum = 1;
  config.num_segments = 2;

  CycloneCacheHandle *cache = nullptr;
  REQUIRE(cyclone_cache_create(&config, &cache) == CYCLONE_OK);

  const std::string key = "borrow_key";
  const std::string data(4096, 'b');
  REQUIRE(cyclone_cache_write(cache, key.data(), key.size(), data.data(),
                              data.size()) == CYCLONE_OK);

  CycloneReadHandle *rh = nullptr;
  REQUIRE(cyclone_cache_read(cache, key.data(), key.size(), &rh) == CYCLONE_OK);
  REQUIRE(rh != nullptr);

  CycloneCacheStats stats{};
  REQUIRE(cyclone_cache_stats(cache, &stats) == CYCLONE_OK);
  REQUIRE(stats.disk_cache_hits >= 1);
  CHECK(stats.borrows_outstanding == 0);

  // The data stays readable for the life of the handle.
  const char *read_data = nullptr;
  size_t read_len = 0;
  REQUIRE(cyclone_cache_read_data(rh, &read_data, &read_len) == CYCLONE_OK);
  REQUIRE(std::string(read_data, read_len) == data);

  cyclone_cache_read_close(rh);
  REQUIRE(cyclone_cache_stats(cache, &stats) == CYCLONE_OK);
  REQUIRE(stats.borrows_outstanding == 0);
  cyclone_cache_destroy(cache);
}

// =============================================================================
// Error mapping
// =============================================================================

TEST_CASE("C API: library errors map to distinct C codes", "[c_api][error]") {
  using cyclone::CacheError;
  using cyclone::c_api_detail::to_c_error;
  // Routine outcomes must not surface as CYCLONE_INTERNAL_ERROR, which the
  // header reserves for conditions that should not occur in normal use.
  CHECK(to_c_error(CacheError::Success) == CYCLONE_OK);
  CHECK(to_c_error(CacheError::NotFound) == CYCLONE_NOT_FOUND);
  CHECK(to_c_error(CacheError::Exists) == CYCLONE_EXISTS);
  CHECK(to_c_error(CacheError::NoSpace) == CYCLONE_NO_SPACE);
  CHECK(to_c_error(CacheError::IoError) == CYCLONE_IO_ERROR);
  CHECK(to_c_error(CacheError::Corrupted) == CYCLONE_CORRUPTED);
  CHECK(to_c_error(CacheError::ChainCorrupted) == CYCLONE_CORRUPTED);
  CHECK(to_c_error(CacheError::InvalidKey) == CYCLONE_INVALID_KEY);
  CHECK(to_c_error(CacheError::InvalidArgument) == CYCLONE_INVALID_ARGUMENT);
  CHECK(to_c_error(CacheError::InvalidConfiguration) ==
        CYCLONE_INVALID_ARGUMENT);
  CHECK(to_c_error(CacheError::NotInitialized) == CYCLONE_NOT_INITIALIZED);
  CHECK(to_c_error(CacheError::Closed) == CYCLONE_NOT_INITIALIZED);
  CHECK(to_c_error(CacheError::Busy) == CYCLONE_BUSY);
  CHECK(to_c_error(CacheError::Timeout) == CYCLONE_TIMEOUT);
  CHECK(to_c_error(CacheError::NotOwned) == CYCLONE_NOT_OWNED);
  CHECK(to_c_error(CacheError::IncompatibleVersion) ==
        CYCLONE_INCOMPATIBLE_VERSION);
  CHECK(to_c_error(CacheError::AlternateNotFound) == CYCLONE_NOT_FOUND);
  CHECK(to_c_error(CacheError::ResetRefusedLivePeer) ==
        CYCLONE_RESET_REFUSED_LIVE_PEER);
  CHECK(to_c_error(CacheError::ObjectTooLarge) == CYCLONE_OBJECT_TOO_LARGE);
  CHECK(to_c_error(CacheError::InternalError) == CYCLONE_INTERNAL_ERROR);
  // A value outside the enumeration still maps to the catch-all.
  CHECK(to_c_error(static_cast<CacheError>(0xFF)) == CYCLONE_INTERNAL_ERROR);

  // The appended codes keep the existing values and extend the tail.
  STATIC_REQUIRE(CYCLONE_BUSY == 12);
  STATIC_REQUIRE(CYCLONE_TIMEOUT == CYCLONE_BUSY + 1);
  STATIC_REQUIRE(CYCLONE_NOT_OWNED == CYCLONE_TIMEOUT + 1);
  STATIC_REQUIRE(CYCLONE_INCOMPATIBLE_VERSION == CYCLONE_NOT_OWNED + 1);
}

TEST_CASE("C API: read_async without a miss handler reports corruption",
          "[c_api][miss_callback][error]") {
  // A disk failure or a corrupted entry is not a clean miss: without a miss
  // handler, read_async must report it as such, as the synchronous read
  // does, rather than as CYCLONE_NOT_FOUND.
  TempCacheDir tmp;
  std::string cache_path = tmp.path();
  CycloneCacheConfig config{};
  config.cache_path = cache_path.c_str();
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.ram_cache_size_bytes = 0;  // disk only
  config.enable_checksum = 1;
  config.num_segments = 2;

  CycloneCacheHandle *cache = nullptr;
  REQUIRE(cyclone_cache_create(&config, &cache) == CYCLONE_OK);

  const std::string key = "corrupt_me";
  std::string data(8192, '\0');
  for (size_t i = 0; i < data.size(); ++i) {
    data[i] = static_cast<char>('a' + (i * 7 + i / 13) % 26);
  }
  REQUIRE(cyclone_cache_write(cache, key.data(), key.size(), data.data(),
                              data.size()) == CYCLONE_OK);

  // Flip one byte of the stored content in place.  The volume is a shared
  // mapping of the file, so the change is visible to the next read.
  bool flipped = false;
  for (const auto &entry : std::filesystem::directory_iterator(tmp.dir())) {
    if (!entry.is_regular_file()) continue;
    std::FILE *f = std::fopen(entry.path().string().c_str(), "r+b");
    REQUIRE(f != nullptr);
    std::vector<char> file(static_cast<size_t>(entry.file_size()));
    REQUIRE(std::fread(file.data(), 1, file.size(), f) == file.size());
    auto it = std::search(file.begin(), file.end(), data.begin(), data.end());
    if (it != file.end()) {
      auto pos = static_cast<long>(it - file.begin()) + 100;
      REQUIRE(std::fseek(f, pos, SEEK_SET) == 0);
      char flipped_byte = static_cast<char>(data[100] ^ 0x5A);
      REQUIRE(std::fwrite(&flipped_byte, 1, 1, f) == 1);
      flipped = true;
    }
    std::fclose(f);
    if (flipped) break;
  }
  REQUIRE(flipped);

  CycloneReadHandle *rh = nullptr;
  REQUIRE(cyclone_cache_read(cache, key.data(), key.size(), &rh) ==
          CYCLONE_CORRUPTED);

  struct Context {
    std::atomic<bool> called{false};
    CycloneError err = CYCLONE_OK;
  } ctx;
  auto callback = [](void *ud, const char *, size_t, CycloneError err) {
    auto *c = static_cast<Context *>(ud);
    c->err = err;
    c->called.store(true);
  };
  REQUIRE(cyclone_cache_read_async(cache, key.data(), key.size(), callback,
                                   &ctx) == CYCLONE_OK);
  REQUIRE(ctx.called.load());
  CHECK(ctx.err == CYCLONE_CORRUPTED);

  cyclone_cache_destroy(cache);
}

// =============================================================================
// Callback re-entrancy
// =============================================================================

TEST_CASE("C API: read_cb rejected by the in-flight limit may re-enter",
          "[c_api][miss_callback]") {
  // read_cb runs on the caller's stack.  Every path that calls it must do so
  // with no bridge lock held, so a callback can call back into the same
  // cache (chain another read, change the miss handler, drain).  The
  // rejection at the in-flight limit is one such path.
  TempCacheDir tmp;
  std::string cache_path = tmp.path();
  CycloneCacheConfig config{};
  config.cache_path = cache_path.c_str();
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.ram_cache_size_bytes = static_cast<uint64_t>(1 * 1024 * 1024);
  config.enable_checksum = 1;
  config.num_segments = 2;
  config.max_in_flight_requests = 1;

  CycloneCacheHandle *cache = nullptr;
  REQUIRE(cyclone_cache_create(&config, &cache) == CYCLONE_OK);

  // A handler that parks the fetch, so the first key holds the only slot.
  struct Parked {
    CycloneMissDoneCallback done_cb = nullptr;
    void *done_ud = nullptr;
  } parked;
  auto park = [](const char *, size_t, void *ud, CycloneMissDoneCallback cb,
                 void *done_ud) {
    auto *p = static_cast<Parked *>(ud);
    p->done_cb = cb;
    p->done_ud = done_ud;
  };
  REQUIRE(cyclone_cache_set_miss_handler(cache, park, &parked) == CYCLONE_OK);

  auto ignore = [](void *, const char *, size_t, CycloneError) {};
  REQUIRE(cyclone_cache_read_async(cache, "first", 5, ignore, nullptr) ==
          CYCLONE_OK);
  REQUIRE(parked.done_cb != nullptr);

  struct Reentry {
    CycloneCacheHandle *cache;
    CycloneError err = CYCLONE_OK;
    CycloneError nested = CYCLONE_INTERNAL_ERROR;
  } reentry{cache};
  auto reenter = [](void *ud, const char *, size_t, CycloneError err) {
    auto *r = static_cast<Reentry *>(ud);
    r->err = err;
    // Takes the bridge lock: deadlocks if read_cb runs under it.
    r->nested = cyclone_cache_set_miss_handler(r->cache, nullptr, nullptr);
  };

  std::atomic<bool> returned{false};
  std::thread t([&] {
    cyclone_cache_read_async(cache, "second", 6, reenter, &reentry);
    returned.store(true);
  });
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!returned.load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (!returned.load()) {
    // Deadlocked: the thread cannot be joined and the cache cannot be
    // destroyed.  Leave both behind and fail.
    t.detach();
    FAIL("read_cb ran under the bridge lock: re-entry deadlocked");
  }
  t.join();
  CHECK(reentry.err == CYCLONE_NO_SPACE);
  CHECK(reentry.nested == CYCLONE_OK);

  parked.done_cb(parked.done_ud, "v", 1, CYCLONE_OK);
  cyclone_cache_destroy(cache);
}

// =============================================================================
// Drain timeout and destroy with fetches outstanding
// =============================================================================

namespace {
struct ParkedFetch {
  std::atomic<CycloneMissDoneCallback> done_cb{nullptr};
  std::atomic<void *> done_ud{nullptr};
};

void park_fetch(const char *, size_t, void *ud, CycloneMissDoneCallback cb,
                void *done_ud) {
  auto *p = static_cast<ParkedFetch *>(ud);
  p->done_ud.store(done_ud);
  p->done_cb.store(cb);
}

struct CountingWaiter {
  std::atomic<int> calls{0};
  std::atomic<CycloneError> err{CYCLONE_INTERNAL_ERROR};
};

void count_read(void *ud, const char *, size_t, CycloneError err) {
  auto *w = static_cast<CountingWaiter *>(ud);
  w->err.store(err);
  w->calls.fetch_add(1);
}

CycloneCacheHandle *make_async_cache(const std::string &path) {
  CycloneCacheConfig config{};
  config.cache_path = path.c_str();
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.ram_cache_size_bytes = static_cast<uint64_t>(1 * 1024 * 1024);
  config.enable_checksum = 1;
  config.num_segments = 2;
  CycloneCacheHandle *cache = nullptr;
  REQUIRE(cyclone_cache_create(&config, &cache) == CYCLONE_OK);
  return cache;
}
}  // namespace

TEST_CASE("C API: drain timeout is reported and a late fetch is safe",
          "[c_api][miss_callback][shutdown]") {
  // A fetch that outlives drain_pending() must not be reported as drained,
  // and its done callback may still arrive after cyclone_cache_destroy():
  // the bridge must keep what that callback touches alive.
  TempCacheDir tmp;
  CycloneCacheHandle *cache = make_async_cache(tmp.path());
  ParkedFetch parked;
  REQUIRE(cyclone_cache_set_miss_handler(cache, park_fetch, &parked) ==
          CYCLONE_OK);

  CountingWaiter waiter;
  REQUIRE(cyclone_cache_read_async(cache, "late", 4, count_read, &waiter) ==
          CYCLONE_OK);
  REQUIRE(parked.done_cb.load() != nullptr);

  CHECK(cyclone_cache_drain_pending(cache, 20) == CYCLONE_TIMEOUT);
  // The waiter was failed by the drain.
  CHECK(waiter.calls.load() == 1);
  CHECK(waiter.err.load() == CYCLONE_NOT_INITIALIZED);

  cyclone_cache_destroy(cache);

  // The fetch completes after the destroy.  It must neither touch freed
  // memory nor answer the waiter a second time.
  parked.done_cb.load()(parked.done_ud.load(), "value", 5, CYCLONE_OK);
  CHECK(waiter.calls.load() == 1);
}

TEST_CASE("C API: destroy waits for a write-back that is under way",
          "[c_api][miss_callback][shutdown]") {
  // The done callback answers the waiters first and then writes the value
  // back into the cache.  A destroy that runs while that write-back is
  // pending must wait for it rather than free the cache under it.
  TempCacheDir tmp;
  CycloneCacheHandle *cache = make_async_cache(tmp.path());
  ParkedFetch parked;
  REQUIRE(cyclone_cache_set_miss_handler(cache, park_fetch, &parked) ==
          CYCLONE_OK);

  // A waiter that blocks inside read_cb, which runs after the write-back is
  // registered and before it starts.
  struct Blocking {
    std::latch entered{1};
    std::latch release{1};
  } blocking;
  auto block_read = [](void *ud, const char *, size_t, CycloneError) {
    auto *b = static_cast<Blocking *>(ud);
    b->entered.count_down();
    b->release.wait();
  };
  REQUIRE(cyclone_cache_read_async(cache, "slow", 4, block_read, &blocking) ==
          CYCLONE_OK);
  REQUIRE(parked.done_cb.load() != nullptr);

  std::thread fetcher([&] {
    parked.done_cb.load()(parked.done_ud.load(), "value", 5, CYCLONE_OK);
  });
  blocking.entered.wait();

  // The write-back is pending, so the drain times out.
  CHECK(cyclone_cache_drain_pending(cache, 20) == CYCLONE_TIMEOUT);

  std::atomic<bool> destroyed{false};
  std::thread destroyer([&] {
    cyclone_cache_destroy(cache);
    destroyed.store(true);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  CHECK_FALSE(destroyed.load());

  blocking.release.count_down();
  fetcher.join();
  destroyer.join();
  CHECK(destroyed.load());
}

TEST_CASE("C API: a throwing read_cb does not leave a write-back pending",
          "[c_api][miss_callback][shutdown]") {
  // Callbacks must not throw (see cyclone_c.h), but one written in C++ might.
  // The bridge contains the exception; it must not also leave its
  // write-back accounting behind, or destroy waits for it forever.
  TempCacheDir tmp;
  CycloneCacheHandle *cache = make_async_cache(tmp.path());
  ParkedFetch parked;
  REQUIRE(cyclone_cache_set_miss_handler(cache, park_fetch, &parked) ==
          CYCLONE_OK);

  auto throwing_read = [](void *, const char *, size_t, CycloneError) {
    throw std::runtime_error("read_cb failed");
  };
  REQUIRE(cyclone_cache_read_async(cache, "throws", 6, throwing_read,
                                   nullptr) == CYCLONE_OK);
  REQUIRE(parked.done_cb.load() != nullptr);
  parked.done_cb.load()(parked.done_ud.load(), "value", 5, CYCLONE_OK);

  std::atomic<bool> destroyed{false};
  std::thread destroyer([&] {
    cyclone_cache_destroy(cache);
    destroyed.store(true);
  });
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!destroyed.load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (!destroyed.load()) {
    destroyer.detach();
    FAIL("destroy is waiting for a write-back that will never finish");
  }
  destroyer.join();
}

TEST_CASE(
    "C API: a throwing miss handler leaves no read registered, so its "
    "read_cb is never called",
    "[c_api][miss_callback][shutdown]") {
  // The miss handler must not throw (see cyclone_c.h), but one written in
  // C++ might.  The read then returns an error, which promises that read_cb
  // will not be called: the read must not stay registered and be answered
  // later (at destroy, perhaps after the caller freed read_user_data).
  TempCacheDir tmp;
  CycloneCacheHandle *cache = make_async_cache(tmp.path());
  auto throwing_handler = [](const char *, size_t, void *,
                             CycloneMissDoneCallback, void *) {
    throw std::runtime_error("miss handler failed");
  };
  REQUIRE(cyclone_cache_set_miss_handler(cache, throwing_handler, nullptr) ==
          CYCLONE_OK);

  std::atomic<int> calls{0};
  auto counting_read = [](void *ud, const char *, size_t, CycloneError) {
    static_cast<std::atomic<int> *>(ud)->fetch_add(1);
  };
  CHECK(cyclone_cache_read_async(cache, "handler-throws", 14, counting_read,
                                 &calls) == CYCLONE_INTERNAL_ERROR);
  CHECK(calls.load() == 0);

  // Nothing is in flight: a drain is immediate, and destroy answers no one.
  CHECK(cyclone_cache_drain_pending(cache, 1000) == CYCLONE_OK);
  cyclone_cache_destroy(cache);
  CHECK(calls.load() == 0);
}

#if defined(__GLIBC__) && defined(__GLIBCXX__)
TEST_CASE("C API: a thread cancelled inside drain_pending unwinds cleanly",
          "[c_api][shutdown]") {
  // glibc implements thread cancellation as a forced unwind.  The bridge's
  // exception barrier must let it pass: swallowing it aborts the process.
  // drain_pending() waits on a condition variable, a cancellation point.
  TempCacheDir tmp;
  CycloneCacheHandle *cache = make_async_cache(tmp.path());
  ParkedFetch parked;
  REQUIRE(cyclone_cache_set_miss_handler(cache, park_fetch, &parked) ==
          CYCLONE_OK);
  CountingWaiter waiter;
  REQUIRE(cyclone_cache_read_async(cache, "held", 4, count_read, &waiter) ==
          CYCLONE_OK);
  REQUIRE(parked.done_cb.load() != nullptr);

  std::atomic<bool> returned{false};
  std::thread drainer([&] {
    cyclone_cache_drain_pending(cache, 60000);
    returned.store(true);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  REQUIRE(pthread_cancel(drainer.native_handle()) == 0);
  drainer.join();
  CHECK_FALSE(returned.load());

  // The bridge lock was released by the unwind: the fetch completes and the
  // cache shuts down normally.
  parked.done_cb.load()(parked.done_ud.load(), "value", 5, CYCLONE_OK);
  CHECK(waiter.calls.load() == 1);
  cyclone_cache_destroy(cache);
}
#endif
