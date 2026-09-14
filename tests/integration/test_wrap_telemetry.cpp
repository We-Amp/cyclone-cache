// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Wrap-cadence telemetry: verify that circular write-buffer wraps are
// counted and that wrap intervals are recorded. Sizing instrument for the
// eviction-vs-reader race.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "cyclone/cache.hpp"
#include "cyclone/key.hpp"

#ifdef _WIN32
#include <process.h>
#define WRAP_GETPID _getpid
#else
#include <unistd.h>
#define WRAP_GETPID getpid
#endif

using namespace cyclone;

namespace {

// Remove the raw path AND the structural-fingerprint sibling(s)
// ("<stem>-<fmt>-<hash><ext>") that Cache::add_volume() actually opens.  With a
// fingerprinted filename the plain std::remove(raw) no longer clears the file
// the Volume uses, so a leftover from a prior run on a persistent /tmp would be
// reopened with stale contents and skew the exact wrap-count / trailing-gap
// assertions here.  Test-only directory iteration.
void remove_cache_files(const std::string &raw_path) {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::remove(raw_path, ec);
  const fs::path p(raw_path);
  const std::string prefix = p.stem().string() + "-";
  for (fs::directory_iterator it(p.parent_path(), ec), end; it != end && !ec;
       it.increment(ec)) {
    const std::string n = it->path().filename().string();
    if (n.size() > prefix.size() && n.compare(0, prefix.size(), prefix) == 0) {
      std::error_code e2;
      fs::remove(it->path(), e2);
    }
  }
}

std::string create_temp_file(size_t size_mb) {
  // Unique per process (getpid) AND per call (counter): no two runs on a
  // persistent /tmp, and no two cases in one run, ever share a cache file.
  static std::atomic<int> counter{0};
  const std::string path =
      (std::filesystem::temp_directory_path() /
       ("cyclone_wrap_telemetry_" + std::to_string(WRAP_GETPID()) + "_" +
        std::to_string(counter.fetch_add(1)) + ".dat"))
          .string();
  remove_cache_files(path);

  FILE *f = std::fopen(path.c_str(), "wb");
  if (f != nullptr) {
    std::fseek(f, static_cast<long>(size_mb * 1024 * 1024 - 1), SEEK_SET);
    std::fputc(0, f);
    std::fclose(f);
  }
  return path;
}

// Helper: write a key with content of the given size, return true on success
bool write_entry(Cache &cache, const std::string &key_str,
                 size_t content_size) {
  CacheKey key(key_str);
  std::vector<std::byte> content(content_size, std::byte{0x42});

  auto wh = cache.write_sync(key, content.size());
  if (!wh.has_value()) {
    return false;
  }
  auto wr = wh->write_sync(std::span<const std::byte>(content));
  if (!wr.has_value()) {
    return false;
  }
  auto cl = wh->close_sync();
  return cl.has_value();
}

}  // namespace

TEST_CASE("Write buffer wrap cadence is recorded", "[eviction][telemetry]") {
  // Small cache (4 MB, single stripe) so repeated fill passes force the
  // write position to wrap multiple times. RAM cache disabled so every
  // write goes to disk; checksums disabled (overwritten regions are fine).
  constexpr size_t kCacheSizeMB = 4;
  constexpr size_t kCacheSize = kCacheSizeMB * 1024 * 1024;
  constexpr size_t kChunkSize = static_cast<const size_t>(64 * 1024);

  std::string cache_path = create_temp_file(kCacheSizeMB);

  CacheConfig config;
  config.ram_cache_size = 0;
  config.enable_checksum = false;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = kCacheSize;

  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  // No wraps before any writes; interval fields start undefined (0).
  {
    auto stats = cache->stats();
    REQUIRE(stats.write_buffer_wraps == 0);
    REQUIRE(stats.last_wrap_interval_ns == 0);
    REQUIRE(stats.min_wrap_interval_ns == 0);
  }

  // One fill pass writes ~1.5x the cache size: guarantees at least one wrap.
  auto fill_pass = [&](size_t pass) {
    size_t num_entries = (kCacheSize / kChunkSize) * 3 / 2;
    for (size_t i = 0; i < num_entries; ++i) {
      std::string key_str =
          "wrap-key-" + std::to_string(pass) + "-" + std::to_string(i);
      // Ignore individual write failures (bucket full is OK here).
      write_entry(*cache, key_str, kChunkSize);
    }
  };

  fill_pass(0);
  uint64_t wraps_after_first_pass = 0;
  {
    auto stats = cache->stats();
    wraps_after_first_pass = stats.write_buffer_wraps;
    REQUIRE(wraps_after_first_pass >= 1);
  }

  fill_pass(1);
  {
    auto stats = cache->stats();
    // The second pass must have wrapped again, so by now we have at least
    // two wraps and the interval fields are populated.
    REQUIRE(stats.write_buffer_wraps > wraps_after_first_pass);
    REQUIRE(stats.write_buffer_wraps >= 2);
    REQUIRE(stats.last_wrap_interval_ns > 0);
    REQUIRE(stats.min_wrap_interval_ns > 0);
    // min is the minimum over all observed intervals, including the last.
    // NOTE: min <= last only holds because this test is single-threaded;
    // polled concurrently with a wrap it is NOT a guaranteed invariant
    // (last is stored before min is folded in).
    REQUIRE(stats.min_wrap_interval_ns <= stats.last_wrap_interval_ns);
  }

  cache->stop();
  remove_cache_files(cache_path);
}

TEST_CASE("Wrap cadence counters are shared in multi-process mode",
          "[eviction][telemetry][multiprocess]") {
  // Same fill pattern, but with mmap directories (multi-process mode):
  // wrap count and last-wrap time land in the shared per-stripe directory
  // header. A SECOND cache view on the same file must then report wraps it
  // did not perform. Two concurrent Cache objects on one file are not an
  // established pattern in this suite, so this follows the close-then-
  // reopen second-view idiom from multi_process_test.cpp ("Checksum
  // mismatch treated as cache miss"): the reopened cache maps the existing
  // file and reads the shared header the writer left behind.
  // Checksums stay enabled (required by multi-process mode); this test
  // never reads back overwritten entries, so that is harmless.
  constexpr size_t kCacheSizeMB = 4;
  constexpr size_t kCacheSize = kCacheSizeMB * 1024 * 1024;
  constexpr size_t kChunkSize = static_cast<const size_t>(64 * 1024);

  std::string cache_path = create_temp_file(kCacheSizeMB);

  uint64_t writer_wraps = 0;

  // --- Writer view: fill past capacity so the write buffer wraps. ---
  {
    CacheConfig config;
    config.ram_cache_size = 0;
    config.set_multi_process(0, 1);  // Enables mmap'd (shared) directories
    auto cache_result = Cache::create(config);
    REQUIRE(cache_result.has_value());
    auto &cache = *cache_result;

    VolumeConfig vol_config;
    vol_config.path = cache_path;
    vol_config.size = kCacheSize;

    REQUIRE(cache->add_volume(vol_config).has_value());
    REQUIRE(cache->start().has_value());

    REQUIRE(cache->stats().write_buffer_wraps == 0);

    size_t num_entries = (kCacheSize / kChunkSize) * 3;
    for (size_t i = 0; i < num_entries; ++i) {
      write_entry(*cache, "mp-wrap-key-" + std::to_string(i), kChunkSize);
    }

    auto stats = cache->stats();
    // Counted via the shared mmap-directory header (cross-process view).
    writer_wraps = stats.write_buffer_wraps;
    REQUIRE(writer_wraps >= 2);
    // This process performed the wraps, so its process-local interval
    // tracking is populated too.
    REQUIRE(stats.last_wrap_interval_ns > 0);
    REQUIRE(stats.min_wrap_interval_ns > 0);

    cache->stop();
  }

  // --- Second view: fresh Cache on the same file, no writes performed. ---
  {
    CacheConfig config2;
    config2.ram_cache_size = 0;
    config2.set_multi_process(0, 1);
    auto cache2_result = Cache::create(config2);
    REQUIRE(cache2_result.has_value());
    auto &cache2 = *cache2_result;

    VolumeConfig vol_config2;
    vol_config2.path = cache_path;
    vol_config2.size = kCacheSize;

    REQUIRE(cache2->add_volume(vol_config2).has_value());
    REQUIRE(cache2->start().has_value());

    auto stats = cache2->stats();
    // The reopened view performed no wraps itself; the count must come
    // from the shared header written by the writer view. A regression
    // that made the counters process-local again would read 0 here.
    REQUIRE(stats.write_buffer_wraps == writer_wraps);
    // Age is derived from the shared last-wrap timestamp: it must be set
    // and plausible (the writer wrapped moments ago, well under a minute).
    REQUIRE(stats.last_wrap_age_ns > 0);
    REQUIRE(stats.last_wrap_age_ns <
            static_cast<uint64_t>(60) * 1000 * 1000 * 1000);
    // Interval tracking is process-local by design and this view has
    // observed no wraps, so it reads 0 (documented semantics).
    REQUIRE(stats.last_wrap_interval_ns == 0);
    REQUIRE(stats.min_wrap_interval_ns == 0);

    cache2->stop();
  }

  remove_cache_files(cache_path);
}
