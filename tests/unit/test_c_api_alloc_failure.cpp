// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The C API is called from C frames, which an exception cannot cross.  Every
// exported function, and the done callback the bridge hands to a miss
// handler, must turn a failure inside the library into an error code instead
// of letting an exception escape.  These tests make every allocation on the
// calling thread fail for the duration of one call, which is the most common
// way a C++ exception arises inside the bridge.
//
// The global allocation functions are replaced for this test binary.  The
// replacement forwards to malloc/free and fails only on a thread that armed
// it, so every other test is unaffected.  ThreadSanitizer and
// MemorySanitizer runtimes define the allocation functions themselves, so
// under them nothing is replaced and these tests are skipped.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

#include "cyclone/cyclone_c.h"
#include "support/temp_cache.hpp"

#if defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
#define CYCLONE_TEST_REPLACE_NEW 0
#endif
#endif
#if !defined(CYCLONE_TEST_REPLACE_NEW) && defined(__SANITIZE_THREAD__)
#define CYCLONE_TEST_REPLACE_NEW 0
#endif
#ifndef CYCLONE_TEST_REPLACE_NEW
#define CYCLONE_TEST_REPLACE_NEW 1
#endif

#if CYCLONE_TEST_REPLACE_NEW
namespace {
thread_local bool g_fail_allocations = false;

void *checked_alloc(std::size_t size) {
  if (g_fail_allocations) throw std::bad_alloc();
  void *p = std::malloc(size == 0 ? 1 : size);
  if (p == nullptr) throw std::bad_alloc();
  return p;
}

void *nothrow_alloc(std::size_t size) noexcept {
  if (g_fail_allocations) return nullptr;
  return std::malloc(size == 0 ? 1 : size);
}

// Arms allocation failure on this thread for its lifetime.
class FailAllocations {
 public:
  FailAllocations() { g_fail_allocations = true; }
  ~FailAllocations() { g_fail_allocations = false; }
  FailAllocations(const FailAllocations &) = delete;
  FailAllocations &operator=(const FailAllocations &) = delete;
};
}  // namespace

// NOLINTBEGIN(misc-new-delete-overloads,cert-dcl54-cpp)
void *operator new(std::size_t size) { return checked_alloc(size); }
void *operator new[](std::size_t size) { return checked_alloc(size); }
void *operator new(std::size_t size, const std::nothrow_t &) noexcept {
  return nothrow_alloc(size);
}
void *operator new[](std::size_t size, const std::nothrow_t &) noexcept {
  return nothrow_alloc(size);
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
void operator delete(void *p, const std::nothrow_t &) noexcept { std::free(p); }
void operator delete[](void *p, const std::nothrow_t &) noexcept {
  std::free(p);
}
// NOLINTEND(misc-new-delete-overloads,cert-dcl54-cpp)
#else
namespace {
class FailAllocations {};
}  // namespace
#endif

#define SKIP_WITHOUT_ALLOCATION_FAILURE()                              \
  do {                                                                 \
    if (!CYCLONE_TEST_REPLACE_NEW)                                     \
      SKIP("allocation functions are owned by the sanitizer runtime"); \
  } while (false)

namespace {
CycloneCacheHandle *make_cache(const TempCacheDir &tmp, std::string &path) {
  path = tmp.path();
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

// Calls fn with allocation failure armed and reports whether an exception
// left it.
template <typename Fn>
bool escapes(Fn &&fn) {
  try {
    FailAllocations armed;
    fn();
  } catch (...) {
    return true;
  }
  return false;
}
}  // namespace

TEST_CASE("C API: allocation failure in a synchronous call returns an error",
          "[c_api][alloc_failure]") {
  SKIP_WITHOUT_ALLOCATION_FAILURE();
  TempCacheDir tmp;
  std::string path;
  CycloneCacheHandle *cache = make_cache(tmp, path);

  const std::string key = "a key long enough to need a heap allocation";
  const std::string data(1024, 'd');
  REQUIRE(cyclone_cache_write(cache, key.data(), key.size(), data.data(),
                              data.size()) == CYCLONE_OK);

  CycloneError err = CYCLONE_OK;
  CycloneReadHandle *rh = nullptr;
  CHECK_FALSE(escapes(
      [&] { err = cyclone_cache_read(cache, key.data(), key.size(), &rh); }));
  CHECK(err == CYCLONE_INTERNAL_ERROR);
  CHECK(rh == nullptr);

  err = CYCLONE_OK;
  const std::string other = "another key long enough for the heap";
  CHECK_FALSE(escapes([&] {
    err = cyclone_cache_write(cache, other.data(), other.size(), data.data(),
                              data.size());
  }));
  CHECK(err != CYCLONE_OK);

  // Create allocates before it can fail cleanly.
  TempCacheDir tmp2;
  std::string path2 = tmp2.path();
  CycloneCacheConfig config{};
  config.cache_path = path2.c_str();
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_checksum = 1;
  CycloneCacheHandle *second = nullptr;
  err = CYCLONE_OK;
  CHECK_FALSE(escapes([&] { err = cyclone_cache_create(&config, &second); }));
  CHECK(err == CYCLONE_INTERNAL_ERROR);
  CHECK(second == nullptr);

  // The cache still works once allocations succeed again.
  REQUIRE(cyclone_cache_read(cache, key.data(), key.size(), &rh) == CYCLONE_OK);
  cyclone_cache_read_close(rh);
  cyclone_cache_destroy(cache);
}

namespace {
struct Parked {
  CycloneMissDoneCallback done_cb = nullptr;
  void *done_ud = nullptr;
  std::atomic<int> calls{0};
};

void park(const char *, size_t, void *ud, CycloneMissDoneCallback cb,
          void *done_ud) {
  auto *p = static_cast<Parked *>(ud);
  p->done_cb = cb;
  p->done_ud = done_ud;
  p->calls.fetch_add(1);
}

struct Waiter {
  std::atomic<int> calls{0};
  std::atomic<CycloneError> err{CYCLONE_INTERNAL_ERROR};
  std::atomic<size_t> len{0};
};

void on_read(void *ud, const char *, size_t len, CycloneError err) {
  auto *w = static_cast<Waiter *>(ud);
  w->err.store(err);
  w->len.store(len);
  w->calls.fetch_add(1);
}
}  // namespace

TEST_CASE("C API: allocation failure in read_async leaves no stuck state",
          "[c_api][alloc_failure][miss_callback]") {
  SKIP_WITHOUT_ALLOCATION_FAILURE();
  TempCacheDir tmp;
  std::string path;
  CycloneCacheHandle *cache = make_cache(tmp, path);
  Parked parked;
  REQUIRE(cyclone_cache_set_miss_handler(cache, park, &parked) == CYCLONE_OK);

  // A failed read_async reports an error and does not call read_cb: it
  // neither registers a waiter nor starts a fetch.
  const std::string key = "a missing key long enough to need the heap";
  Waiter failed;
  CycloneError err = CYCLONE_OK;
  CHECK_FALSE(escapes([&] {
    err = cyclone_cache_read_async(cache, key.data(), key.size(), on_read,
                                   &failed);
  }));
  CHECK(err == CYCLONE_INTERNAL_ERROR);
  CHECK(failed.calls.load() == 0);
  CHECK(parked.calls.load() == 0);

  // Nothing was left in flight: drain returns at once.
  REQUIRE(cyclone_cache_drain_pending(cache, 0) == CYCLONE_OK);
  cyclone_cache_destroy(cache);
}

TEST_CASE("C API: allocation failure in the miss done callback is contained",
          "[c_api][alloc_failure][miss_callback]") {
  SKIP_WITHOUT_ALLOCATION_FAILURE();
  TempCacheDir tmp;
  std::string path;
  CycloneCacheHandle *cache = make_cache(tmp, path);
  Parked parked;
  REQUIRE(cyclone_cache_set_miss_handler(cache, park, &parked) == CYCLONE_OK);

  const std::string key = "a missing key long enough to need the heap";
  Waiter waiter;
  REQUIRE(cyclone_cache_read_async(cache, key.data(), key.size(), on_read,
                                   &waiter) == CYCLONE_OK);
  REQUIRE(parked.calls.load() == 1);

  // The done callback runs on the miss handler's stack, which belongs to the
  // embedder.  It must not throw, and the waiters must still be answered.
  const std::string value(2048, 'v');
  CHECK_FALSE(escapes([&] {
    parked.done_cb(parked.done_ud, value.data(), value.size(), CYCLONE_OK);
  }));
  CHECK(waiter.calls.load() == 1);
  CHECK(waiter.err.load() == CYCLONE_OK);
  CHECK(waiter.len.load() == value.size());

  // Nothing is left pending, so drain returns at once.
  CHECK(cyclone_cache_drain_pending(cache, 0) == CYCLONE_OK);
  cyclone_cache_destroy(cache);
}
