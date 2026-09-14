// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Nightly randomized-schedule concurrency stress lane.
//
// The practical successor to ad-hoc randomized-scheduler tooling: instead of
// pinning a fixed thread count and iteration budget, these tests randomize the
// worker counts AND inject scheduling jitter (random yields / micro-sleeps at
// every operation boundary) so each nightly run explores a different
// interleaving of the borrow-vs-wrap and write-lock contention
// paths.  The seed is drawn per run and PRINTED, so any failure is replayable:
//   CYCLONE_STRESS_SECONDS=60 CYCLONE_STRESS_SEED=<printed> ./cyclone-tests
//   "[stress]"
//
// Both cases are HIDDEN by default (leading-dot tag) so the normal test run is
// untouched; CI's nightly lane selects them explicitly with a time budget.
//
// Env knobs:
//   CYCLONE_STRESS_SECONDS  wall-clock budget per case (default 2s when unset,
//                           so an explicit selection still does real work).
//   CYCLONE_STRESS_SEED     base RNG seed (default: std::random_device).
//   CYCLONE_STRESS_THREADS  fixed worker base count (default 0 = randomize).
//
// Invariant under test: no crash / no sanitizer error, and every READ HIT must
// return exactly the bytes last written for that key -- a torn read, a
// cross-key chain escape, or a resurrected/garbage document would surface here
// as a content mismatch.  Content is deterministic per key, so a hit is
// verifiable without tracking versions.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "cyclone/cache.hpp"
#include "cyclone/config.hpp"
#include "cyclone/key.hpp"
#include "support/temp_cache.hpp"

using namespace cyclone;

namespace {

struct StressParams {
  std::chrono::seconds budget{2};
  uint64_t seed = 0;
  uint32_t base_threads = 0;  // 0 = randomize
};

uint64_t env_u64(const char *name, uint64_t fallback) {
  const char *v = std::getenv(name);
  if (v == nullptr || *v == '\0') {
    return fallback;
  }
  return std::strtoull(v, nullptr, 10);
}

StressParams read_params() {
  StressParams p;
  p.budget = std::chrono::seconds(env_u64("CYCLONE_STRESS_SECONDS", 2));
  uint64_t seed_env = env_u64("CYCLONE_STRESS_SEED", 0);
  p.seed = (seed_env != 0) ? seed_env : std::random_device{}();
  p.base_threads = static_cast<uint32_t>(env_u64("CYCLONE_STRESS_THREADS", 0));
  return p;
}

// Deterministic content for a key id: size and byte pattern are a pure
// function of the id, and the id is stamped into the first bytes, so a hit for
// key K that returns anything other than content_for(K) is a real fault.
constexpr int kNumKeys = 64;

std::vector<std::byte> content_for(uint32_t id) {
  size_t size = 1024 + size_t{id % 8} * 1024;  // 1..8 KB
  std::vector<std::byte> v(size);
  uint32_t stamp = 0xC0DE0000u ^ id;
  std::memcpy(v.data(), &stamp, sizeof(stamp));
  auto fill = static_cast<std::byte>(0x40 + (id & 0x3F));
  for (size_t i = sizeof(stamp); i < size; ++i) {
    v[i] = fill;
  }
  return v;
}

// Returns false (records a mismatch) if the served bytes are not exactly the
// canonical content for `id`.
bool content_matches(uint32_t id, std::span<const std::byte> got) {
  auto expected = content_for(id);
  if (got.size() != expected.size()) {
    return false;
  }
  return std::memcmp(got.data(), expected.data(), expected.size()) == 0;
}

CacheKey key_for(uint32_t id) {
  return CacheKey("stress-" + std::to_string(id));
}

// Scheduling jitter: at every operation boundary, sometimes yield, sometimes
// sleep a random handful of microseconds.  This is the randomized-schedule
// pressure that shakes loose interleaving-dependent races.
inline void jitter(std::mt19937_64 &rng) {
  auto roll = static_cast<uint32_t>(rng() & 0x7);
  if (roll == 0) {
    std::this_thread::yield();
  } else if (roll == 1) {
    std::this_thread::sleep_for(
        std::chrono::microseconds(static_cast<int>(rng() % 60)));
  }
}

std::unique_ptr<Cache> make_stress_cache(const std::string &path,
                                         bool mmap_directory) {
  CacheConfig cfg;
  cfg.ram_cache_size =
      size_t{2} * 1024 * 1024;  // small RAM tier -> disk path exercised
  cfg.enable_checksum = true;
  cfg.verify_checksum_on_read = true;
  if (mmap_directory) {
    cfg.multi_process_config.enabled = true;
  }
  auto cache = Cache::create(cfg);
  REQUIRE(cache.has_value());

  VolumeConfig vol;
  vol.path = path;
  vol.size = size_t{6} * 1024 * 1024;  // small -> frequent wraps under load
  REQUIRE((*cache)->add_volume(vol).has_value());
  REQUIRE((*cache)->start().has_value());
  return std::move(*cache);
}

bool write_key(Cache &cache, uint32_t id) {
  auto content = content_for(id);
  auto wh = cache.write_sync(key_for(id), content.size());
  if (!wh.has_value()) {
    return false;
  }
  if (!wh->write_sync(std::span<const std::byte>(content)).has_value()) {
    wh->abort();
    return false;
  }
  return wh->close_sync().has_value();
}

}  // namespace

// ---------------------------------------------------------------------------
// Borrow-vs-wrap: readers hold live disk-hit handles (borrows that gate wraps
// via read leases) while writers flood a small volume into repeated wraps.
// ---------------------------------------------------------------------------
TEST_CASE("randomized borrow-vs-wrap stress", "[.stress][nightly][borrow]") {
  StressParams p = read_params();
  std::mt19937_64 seed_rng(p.seed);
  uint32_t readers = p.base_threads != 0
                         ? p.base_threads
                         : static_cast<uint32_t>(2 + seed_rng() % 15);  // 2..16
  uint32_t writers = p.base_threads != 0
                         ? p.base_threads
                         : static_cast<uint32_t>(1 + seed_rng() % 6);  // 1..6

  std::printf(
      "[stress][borrow] seed=%llu budget=%llds readers=%u writers=%u\n"
      "  replay: CYCLONE_STRESS_SECONDS=%lld CYCLONE_STRESS_SEED=%llu "
      "./cyclone-tests \"[borrow]\"\n",
      static_cast<unsigned long long>(p.seed),
      static_cast<long long>(p.budget.count()), readers, writers,
      static_cast<long long>(p.budget.count()),
      static_cast<unsigned long long>(p.seed));
  std::fflush(stdout);

  TempCacheDir tmp;
  std::string path = tmp.path();
  auto cache = make_stress_cache(path, /*mmap_directory=*/false);

  // Seed all keys so reads can hit immediately.
  for (uint32_t id = 0; id < kNumKeys; ++id) {
    (void)write_key(*cache, id);
  }

  std::atomic<bool> stop{false};
  std::atomic<uint64_t> mismatches{0};
  std::atomic<uint64_t> hits{0};
  std::atomic<uint64_t> writes{0};
  std::vector<std::thread> workers;
  workers.reserve(size_t{readers} + writers);

  for (uint32_t t = 0; t < readers; ++t) {
    workers.emplace_back([&, t] {
      std::mt19937_64 rng(p.seed ^ (uint64_t{0x1111} * (t + 1)));
      while (!stop.load(std::memory_order_relaxed)) {
        auto id = static_cast<uint32_t>(rng() % kNumKeys);
        auto rh = cache->read_sync(key_for(id));
        if (rh.has_value()) {
          // A hit must serve EXACTLY the canonical bytes for this key --
          // empty included: canonical content is >= 1 KB, so an empty span
          // on a genuine hit is a torn/partial serve, the very fault this
          // lane exists to catch.
          auto content = rh->content();
          if (!content_matches(id, content)) {
            mismatches.fetch_add(1, std::memory_order_relaxed);
          } else {
            hits.fetch_add(1, std::memory_order_relaxed);
          }
          // Hold the borrow across the jitter window: this is what pins a
          // region against a concurrent wrap.
          jitter(rng);
        }
        jitter(rng);
      }
    });
  }

  for (uint32_t t = 0; t < writers; ++t) {
    workers.emplace_back([&, t] {
      std::mt19937_64 rng(p.seed ^ (uint64_t{0x2222} * (t + 1)));
      while (!stop.load(std::memory_order_relaxed)) {
        auto id = static_cast<uint32_t>(rng() % kNumKeys);
        if (write_key(*cache, id)) {
          writes.fetch_add(1, std::memory_order_relaxed);
        }
        jitter(rng);
      }
    });
  }

  std::this_thread::sleep_for(p.budget);
  stop.store(true, std::memory_order_relaxed);
  for (auto &w : workers) {
    w.join();
  }

  auto stats = cache->stats();
  std::printf(
      "[stress][borrow] done: hits=%llu writes=%llu wraps=%llu "
      "mismatches=%llu\n",
      static_cast<unsigned long long>(hits.load()),
      static_cast<unsigned long long>(writes.load()),
      static_cast<unsigned long long>(stats.write_buffer_wraps),
      static_cast<unsigned long long>(mismatches.load()));
  std::fflush(stdout);

  cache->stop();

  INFO("seed=" << p.seed << " -- replay with CYCLONE_STRESS_SEED to reproduce");
  REQUIRE(mismatches.load() == 0);
}

// ---------------------------------------------------------------------------
// Write-lock contention: many concurrent writers (mmap directory) hammer an
// overlapping key set, driving the seqlock / write-lock takeover path, while
// readers verify no torn or cross-key content is ever served.
// ---------------------------------------------------------------------------
TEST_CASE("randomized write-lock contention stress",
          "[.stress][nightly][writelock]") {
  StressParams p = read_params();
  std::mt19937_64 seed_rng(p.seed ^ 0xABCDEF01u);
  uint32_t writers = p.base_threads != 0
                         ? p.base_threads
                         : static_cast<uint32_t>(3 + seed_rng() % 10);  // 3..12
  uint32_t readers = p.base_threads != 0
                         ? p.base_threads
                         : static_cast<uint32_t>(2 + seed_rng() % 7);  // 2..8

  std::printf(
      "[stress][writelock] seed=%llu budget=%llds writers=%u readers=%u\n"
      "  replay: CYCLONE_STRESS_SECONDS=%lld CYCLONE_STRESS_SEED=%llu "
      "./cyclone-tests \"[writelock]\"\n",
      static_cast<unsigned long long>(p.seed),
      static_cast<long long>(p.budget.count()), writers, readers,
      static_cast<long long>(p.budget.count()),
      static_cast<unsigned long long>(p.seed));
  std::fflush(stdout);

  TempCacheDir tmp;
  std::string path = tmp.path();
  auto cache = make_stress_cache(path, /*mmap_directory=*/true);

  // Contend on a NARROW key set so writers collide on the same buckets/stripes.
  constexpr uint32_t kHotKeys = 8;
  for (uint32_t id = 0; id < kHotKeys; ++id) {
    (void)write_key(*cache, id);
  }

  std::atomic<bool> stop{false};
  std::atomic<uint64_t> mismatches{0};
  std::atomic<uint64_t> ops{0};
  std::vector<std::thread> workers;
  workers.reserve(size_t{writers} + readers);

  for (uint32_t t = 0; t < writers; ++t) {
    workers.emplace_back([&, t] {
      std::mt19937_64 rng(p.seed ^ (uint64_t{0x3333} * (t + 1)));
      while (!stop.load(std::memory_order_relaxed)) {
        auto id = static_cast<uint32_t>(rng() % kHotKeys);
        if ((rng() & 0xF) == 0) {
          (void)cache->remove_sync(key_for(id));
        } else {
          (void)write_key(*cache, id);
        }
        ops.fetch_add(1, std::memory_order_relaxed);
        jitter(rng);
      }
    });
  }

  for (uint32_t t = 0; t < readers; ++t) {
    workers.emplace_back([&, t] {
      std::mt19937_64 rng(p.seed ^ (uint64_t{0x4444} * (t + 1)));
      while (!stop.load(std::memory_order_relaxed)) {
        auto id = static_cast<uint32_t>(rng() % kHotKeys);
        auto rh = cache->read_sync(key_for(id));
        if (rh.has_value()) {
          // Same tightened oracle as the borrow case: any hit (empty span
          // included) must equal the canonical content.
          auto content = rh->content();
          if (!content_matches(id, content)) {
            mismatches.fetch_add(1, std::memory_order_relaxed);
          }
        }
        jitter(rng);
      }
    });
  }

  std::this_thread::sleep_for(p.budget);
  stop.store(true, std::memory_order_relaxed);
  for (auto &w : workers) {
    w.join();
  }

  std::printf("[stress][writelock] done: ops=%llu mismatches=%llu\n",
              static_cast<unsigned long long>(ops.load()),
              static_cast<unsigned long long>(mismatches.load()));
  std::fflush(stdout);

  cache->stop();

  INFO("seed=" << p.seed << " -- replay with CYCLONE_STRESS_SEED to reproduce");
  REQUIRE(mismatches.load() == 0);
}
