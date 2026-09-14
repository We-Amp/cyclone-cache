// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Production readiness tests: exercises critical edge cases for multi-process
// cache usage including data integrity, crash recovery, and resource handling.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#include "core/document.hpp"
#include "core/mmap_directory.hpp"
#include "core/volume.hpp"  // fingerprint_cache_path
#include "cyclone/cache.hpp"
#include "cyclone/config.hpp"
#include "cyclone/key.hpp"

#ifdef _WIN32
#include <process.h>
#define GETPID _getpid
#else
#include <unistd.h>
#define GETPID getpid
#endif

using namespace cyclone;
namespace fs = std::filesystem;

namespace {

std::string get_temp_path(const std::string &name) {
  auto path = fs::temp_directory_path() /
              ("cyclone_prod_" + name + "_" + std::to_string(GETPID()));
  return path.string();
}

// Resolve the structural-fingerprint filename Cache::add_volume() opens, so raw
// corruption below targets the file the cache actually created (see
// fingerprint_cache_path).  Single config per test, so fingerprinting the path
// is exact.
std::string fp(const std::string &path, size_t size, bool mp) {
  return fingerprint_cache_path(path, size, /*stripe_size=*/0, mp);
}

void cleanup_temp_file(const std::string &path) {
  std::error_code ec;
  fs::remove(path, ec);
  // Also remove the structural-fingerprint sibling(s) Cache::add_volume opened
  // ("<stem>-<fmt>-<hash><ext>"), so fingerprinted files do not accumulate on a
  // persistent /tmp.  Test-only directory iteration.
  const fs::path p(path);
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

}  // namespace

// =============================================================================
// PR-1: Checksum verification in single-process mode
// =============================================================================
// Checksums must be verified on reads even without multi-process enabled,
// so that on-disk corruption is detected rather than silently returned.

TEST_CASE("Single-process checksum detects corruption",
          "[production][integrity]") {
  std::string path = get_temp_path("sp_checksum");
  CacheKey key("sp-checksum-key");
  std::vector<std::byte> data(1024, std::byte{0xAA});

  // Write entry in single-process mode (no multi-process config)
  {
    CacheConfig config;
    config.enable_hit_tracking = false;

    auto cache = Cache::create(config);
    REQUIRE(cache.has_value());
    (*cache)->add_volume(path, static_cast<size_t>(10 * 1024 * 1024));
    REQUIRE((*cache)->start().has_value());

    auto wh = (*cache)->write_sync(key, data.size());
    REQUIRE(wh.has_value());
    REQUIRE(wh->write_sync(data).has_value());
    REQUIRE(wh->close_sync().has_value());

    // Verify readable
    auto rh = (*cache)->read_sync(key);
    REQUIRE(rh.has_value());

    (*cache)->stop();
  }

  // Corrupt data region
  {
    std::fstream file(fp(path, static_cast<size_t>(10 * 1024 * 1024), false),
                      std::ios::in | std::ios::out | std::ios::binary);
    REQUIRE(file.is_open());

    file.seekg(0, std::ios::end);
    auto file_size = file.tellg();

    // Corrupt the last 30% of the file
    auto corrupt_start =
        static_cast<std::streamoff>(static_cast<double>(file_size) * 0.7);
    std::vector<char> garbage(256, 'Z');
    for (auto pos = corrupt_start; pos < file_size; pos += 512) {
      file.seekp(pos);
      std::streamsize to_write =
          std::min(static_cast<std::streamsize>(garbage.size()),
                   static_cast<std::streamsize>(file_size - pos));
      file.write(garbage.data(), to_write);
    }
    file.close();
  }

  // Reopen in single-process mode and read — should detect corruption
  {
    CacheConfig config;
    config.enable_hit_tracking = false;

    auto cache = Cache::create(config);
    REQUIRE(cache.has_value());
    (*cache)->add_volume(path, static_cast<size_t>(10 * 1024 * 1024));
    REQUIRE((*cache)->start().has_value());

    auto rh = (*cache)->read_sync(key);
    if (rh.has_value()) {
      // If the read succeeded, the corruption didn't hit our document
      // — verify the content is actually correct
      auto content = rh->content();
      bool content_matches = (content.size() == data.size());
      if (content_matches) {
        content_matches =
            (std::memcmp(content.data(), data.data(), data.size()) == 0);
      }
      REQUIRE(content_matches);
    } else {
      // Should be Corrupted or NotFound (if directory was also corrupted)
      REQUIRE((rh.error() == CacheError::Corrupted ||
               rh.error() == CacheError::NotFound));
    }

    (*cache)->stop();
  }

  cleanup_temp_file(path);
}

// =============================================================================
// PR-2: Checksum verification with non-empty headers
// =============================================================================
// Exercises the fixed checksum computation path that includes header_data +
// content.

TEST_CASE("Checksum covers header data and content",
          "[production][integrity]") {
  std::string path = get_temp_path("header_checksum");

  CacheConfig config;
  config.enable_hit_tracking = false;
  config.set_multi_process(0, 1);
  config.set_enable_checksum(true);

  auto cache = Cache::create(config);
  REQUIRE(cache.has_value());
  (*cache)->add_volume(path, static_cast<size_t>(10 * 1024 * 1024));
  REQUIRE((*cache)->start().has_value());

  CacheKey key("header-cksum-key");

  // Write with both header and content
  std::string header_str = "Content-Type: text/html\r\nX-Custom: value";
  std::vector<std::byte> header(header_str.size());
  std::memcpy(header.data(), header_str.data(), header_str.size());

  std::vector<std::byte> content(2048, std::byte{0xBB});

  {
    auto wh = (*cache)->write_sync(key, content.size());
    REQUIRE(wh.has_value());
    wh->set_header(std::span<const std::byte>(header));
    REQUIRE(wh->write_sync(content).has_value());
    REQUIRE(wh->close_sync().has_value());
  }

  // Read back and verify both header and content
  {
    auto rh = (*cache)->read_sync(key);
    REQUIRE(rh.has_value());

    auto read_header = rh->header();
    REQUIRE(read_header.size() == header.size());
    REQUIRE(std::memcmp(read_header.data(), header.data(), header.size()) == 0);

    auto read_content = rh->content();
    REQUIRE(read_content.size() == content.size());
    REQUIRE(std::memcmp(read_content.data(), content.data(), content.size()) ==
            0);
  }

  (*cache)->stop();
  cleanup_temp_file(path);
}

// =============================================================================
// PR-3: Zero-length content with header in multi-process mode
// =============================================================================

TEST_CASE("Zero content with header round-trips in multi-process mode",
          "[production][integrity]") {
  std::string path = get_temp_path("zero_content_mp");

  CacheConfig config;
  config.enable_hit_tracking = false;
  config.set_multi_process(0, 1);
  config.set_enable_checksum(true);

  auto cache = Cache::create(config);
  REQUIRE(cache.has_value());
  (*cache)->add_volume(path, static_cast<size_t>(10 * 1024 * 1024));
  REQUIRE((*cache)->start().has_value());

  CacheKey key("zero-content-mp-key");
  std::string header_str = "X-Empty: true";
  std::vector<std::byte> header(header_str.size());
  std::memcpy(header.data(), header_str.data(), header_str.size());

  {
    auto wh = (*cache)->write_sync(key, 0);
    REQUIRE(wh.has_value());
    wh->set_header(std::span<const std::byte>(header));
    wh->write_sync(std::span<const std::byte>{});
    REQUIRE(wh->close_sync().has_value());
  }

  {
    auto rh = (*cache)->read_sync(key);
    REQUIRE(rh.has_value());
    REQUIRE(rh->content().empty());
    auto rh_header = rh->header();
    REQUIRE(rh_header.size() == header.size());
  }

  (*cache)->stop();
  cleanup_temp_file(path);
}

// =============================================================================
// PR-4: Document size exceeding stripe is rejected
// =============================================================================

TEST_CASE("Write exceeding stripe size returns NoSpace", "[production][edge]") {
  std::string path = get_temp_path("oversize_doc");

  CacheConfig config;
  config.enable_hit_tracking = false;
  config.ram_cache_size = 0;
  // This case is about STRIPE GEOMETRY, not the per-object bound:
  // the 128MB payload is over the 64MB default max_object_size, which
  // would now reject it at open before geometry is ever consulted.
  config.max_object_size = 0;

  auto cache = Cache::create(config);
  REQUIRE(cache.has_value());

  // Use minimum viable volume size
  size_t vol_size = static_cast<size_t>(128 * 1024 * 1024);  // 128MB = 1 stripe
  (*cache)->add_volume(path, vol_size);
  REQUIRE((*cache)->start().has_value());

  CacheKey key("oversize-key");
  // Create data larger than the stripe data area
  // The data area is stripe_size minus directory overhead
  std::vector<std::byte> huge_data(vol_size, std::byte{0xFF});

  auto wh = (*cache)->write_sync(key, huge_data.size());
  REQUIRE(wh.has_value());
  REQUIRE(wh->write_sync(huge_data).has_value());

  auto close_result = wh->close_sync();
  // Should fail with NoSpace (document can never fit)
  REQUIRE_FALSE(close_result.has_value());
  REQUIRE((close_result.error() == CacheError::NoSpace ||
           close_result.error() == CacheError::InternalError));

  (*cache)->stop();
  cleanup_temp_file(path);
}

// =============================================================================
// PR-5: Concurrent reads and writes with hit tracking
// =============================================================================
// Verifies that the atomic flush stats don't race with stats() reads.

TEST_CASE("Stats are thread-safe with concurrent hit tracking",
          "[production][concurrent]") {
  std::string path = get_temp_path("stats_race");

  CacheConfig config;
  config.enable_hit_tracking = true;
  config.hit_flush_interval = std::chrono::milliseconds(50);  // Fast flush
  config.ram_cache_size = 0;

  auto cache = Cache::create(config);
  REQUIRE(cache.has_value());
  (*cache)->add_volume(path, static_cast<size_t>(10 * 1024 * 1024));
  REQUIRE((*cache)->start().has_value());

  // Write some entries
  for (int i = 0; i < 20; ++i) {
    CacheKey key("stats-key-" + std::to_string(i));
    std::vector<std::byte> data(256, std::byte{static_cast<uint8_t>(i)});
    auto wh = (*cache)->write_sync(key, data.size());
    if (wh.has_value()) {
      wh->write_sync(data);
      wh->close_sync();
    }
  }

  std::atomic<bool> stop{false};
  std::atomic<int> stats_reads{0};

  // Thread 1: Continuously read entries (triggers hit tracking)
  auto reader_fn = [&]() {
    while (!stop.load(std::memory_order_relaxed)) {
      for (int i = 0; i < 20; ++i) {
        CacheKey key("stats-key-" + std::to_string(i));
        auto rh = (*cache)->read_sync(key);
        (void)rh;
      }
    }
  };

  // Thread 2: Continuously read stats
  auto stats_fn = [&]() {
    while (!stop.load(std::memory_order_relaxed)) {
      auto s = (*cache)->stats();
      // Just verify no crash/corruption in reading stats
      (void)s.hit_flush_successes;
      (void)s.hit_flush_failures;
      (void)s.hit_flush_total_delta;
      (void)s.hit_flush_fsyncs;
      stats_reads.fetch_add(1, std::memory_order_relaxed);
    }
  };

  std::thread reader(reader_fn);
  std::thread stats_reader(stats_fn);

  // Let them run for a bit (long enough for several flush cycles)
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  stop.store(true, std::memory_order_relaxed);
  reader.join();
  stats_reader.join();

  REQUIRE(stats_reads.load() > 0);

  (*cache)->stop();
  cleanup_temp_file(path);
}

// =============================================================================
// PR-6: MmapDirectory clear() is safe during concurrent reads
// =============================================================================

TEST_CASE("MmapDirectory clear is safe with concurrent probe",
          "[production][concurrent]") {
  // Allocate shared memory for MmapDirectory
  size_t num_buckets = 256;
  size_t required = MmapDirectory::required_size(num_buckets);
  std::vector<std::byte> region(required, std::byte{0});

  auto dir = MmapDirectory::init(region, num_buckets);
  REQUIRE(dir.has_value());

  // Insert some entries
  for (int i = 0; i < 100; ++i) {
    CacheKey key("clear-test-" + std::to_string(i));
    dir->insert(key, static_cast<uint64_t>(i) * 1024, 512);
  }
  REQUIRE(dir->count() > 0);

  std::atomic<bool> stop{false};
  std::atomic<int> reads_completed{0};
  std::atomic<int> clears_completed{0};

  // Reader thread: continuously probe entries
  auto reader_fn = [&]() {
    while (!stop.load(std::memory_order_relaxed)) {
      for (int i = 0; i < 100; ++i) {
        CacheKey key("clear-test-" + std::to_string(i));
        auto result = dir->probe(key);
        (void)result;  // May or may not find it during clear
        reads_completed.fetch_add(1, std::memory_order_relaxed);
      }
    }
  };

  // Writer thread: periodically clear and repopulate
  auto writer_fn = [&]() {
    while (!stop.load(std::memory_order_relaxed)) {
      dir->clear();
      clears_completed.fetch_add(1, std::memory_order_relaxed);

      for (int i = 0; i < 100; ++i) {
        CacheKey key("clear-test-" + std::to_string(i));
        dir->insert(key, static_cast<uint64_t>(i) * 1024, 512);
      }
    }
  };

  std::thread reader(reader_fn);
  std::thread writer(writer_fn);

  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  stop.store(true, std::memory_order_relaxed);
  reader.join();
  writer.join();

  REQUIRE(reads_completed.load() > 0);
  REQUIRE(clears_completed.load() > 0);
}

// =============================================================================
// PR-7: Cache survives rapid open/close cycles
// =============================================================================

TEST_CASE("Cache survives rapid open/close cycles", "[production][lifecycle]") {
  std::string path = get_temp_path("rapid_lifecycle");

  for (int cycle = 0; cycle < 10; ++cycle) {
    CacheConfig config;
    config.enable_hit_tracking = false;
    config.ram_cache_size = 0;

    auto cache = Cache::create(config);
    REQUIRE(cache.has_value());
    (*cache)->add_volume(path, static_cast<size_t>(10 * 1024 * 1024));
    REQUIRE((*cache)->start().has_value());

    // Write an entry each cycle
    CacheKey key("lifecycle-" + std::to_string(cycle));
    std::vector<std::byte> data(128, std::byte{static_cast<uint8_t>(cycle)});
    auto wh = (*cache)->write_sync(key, data.size());
    REQUIRE(wh.has_value());
    REQUIRE(wh->write_sync(data).has_value());
    REQUIRE(wh->close_sync().has_value());

    // Read back entries from this and previous cycles
    for (int i = 0; i <= cycle; ++i) {
      CacheKey read_key("lifecycle-" + std::to_string(i));
      auto rh = (*cache)->read_sync(read_key);
      // May not find entries from before a wrap/eviction, but shouldn't crash
      if (rh.has_value()) {
        REQUIRE(rh->content().size() == 128);
      }
    }

    (*cache)->stop();
  }

  cleanup_temp_file(path);
}

// =============================================================================
// PR-8: Multiple concurrent writers on different keys
// =============================================================================

TEST_CASE("Concurrent writers on different keys", "[production][concurrent]") {
  std::string path = get_temp_path("concurrent_writers");

  CacheConfig config;
  config.enable_hit_tracking = false;
  config.ram_cache_size = 0;

  auto cache = Cache::create(config);
  REQUIRE(cache.has_value());
  (*cache)->add_volume(path, static_cast<size_t>(50 * 1024 * 1024));
  REQUIRE((*cache)->start().has_value());

  constexpr int kThreads = 4;
  constexpr int kWritesPerThread = 50;

  std::atomic<int> total_writes{0};
  std::atomic<int> total_errors{0};

  auto writer_fn = [&](int thread_id) {
    for (int i = 0; i < kWritesPerThread; ++i) {
      CacheKey key("t" + std::to_string(thread_id) + "-k" + std::to_string(i));
      std::vector<std::byte> data(512,
                                  std::byte{static_cast<uint8_t>(thread_id)});

      auto wh = (*cache)->write_sync(key, data.size());
      if (wh.has_value()) {
        auto wr = wh->write_sync(data);
        if (wr.has_value()) {
          auto cl = wh->close_sync();
          if (cl.has_value()) {
            total_writes.fetch_add(1, std::memory_order_relaxed);
            continue;
          }
        }
      }
      total_errors.fetch_add(1, std::memory_order_relaxed);
    }
  };

  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back(writer_fn, t);
  }
  for (auto &t : threads) {
    t.join();
  }

  // Most writes should succeed
  REQUIRE(total_writes.load() > kThreads * kWritesPerThread / 2);

  // Verify reads after concurrent writes
  int readable = 0;
  for (int t = 0; t < kThreads; ++t) {
    for (int i = 0; i < kWritesPerThread; ++i) {
      CacheKey key("t" + std::to_string(t) + "-k" + std::to_string(i));
      auto rh = (*cache)->read_sync(key);
      if (rh.has_value()) {
        // Verify content integrity
        auto content = rh->content();
        REQUIRE(content.size() == 512);
        bool all_correct = true;
        for (auto b : content) {
          if (b != std::byte{static_cast<uint8_t>(t)}) {
            all_correct = false;
            break;
          }
        }
        REQUIRE(all_correct);
        ++readable;
      }
    }
  }
  REQUIRE(readable > 0);

  (*cache)->stop();
  cleanup_temp_file(path);
}

// =============================================================================
// PR-9: Alternates with headers survive checksum verification
// =============================================================================

TEST_CASE("Alternate chain with headers passes checksum",
          "[production][integrity]") {
  std::string path = get_temp_path("alt_header_cksum");

  CacheConfig config;
  config.enable_hit_tracking = false;
  config.set_multi_process(0, 1);
  config.set_enable_checksum(true);

  auto cache = Cache::create(config);
  REQUIRE(cache.has_value());
  (*cache)->add_volume(path, static_cast<size_t>(10 * 1024 * 1024));
  REQUIRE((*cache)->start().has_value());

  CacheKey key("alt-header-key");

  // Write 3 alternates, each with different headers and content
  for (int i = 1; i <= 3; ++i) {
    auto alt_id = static_cast<AlternateId>(i);
    std::string hdr = "X-Alt: " + std::to_string(i);
    std::vector<std::byte> header(hdr.size());
    std::memcpy(header.data(), hdr.data(), hdr.size());

    std::vector<std::byte> content(static_cast<size_t>(256 * i),
                                   std::byte{static_cast<uint8_t>(0x30 + i)});

    auto wh = (*cache)->write_alternate_sync(key, alt_id, content.size());
    REQUIRE(wh.has_value());
    wh->set_header(std::span<const std::byte>(header));
    REQUIRE(wh->write_sync(content).has_value());
    REQUIRE(wh->close_sync().has_value());
  }

  // List alternates — should pass checksum verification for all
  auto alts = (*cache)->list_alternates_sync(key);
  REQUIRE(alts.has_value());
  REQUIRE(alts->size() == 3);

  // Verify each alternate has correct header data
  for (const auto &alt : *alts) {
    REQUIRE(!alt.header.empty());
    std::string hdr(reinterpret_cast<const char *>(alt.header.data()),
                    alt.header.size());
    REQUIRE(hdr.substr(0, 7) == "X-Alt: ");
  }

  (*cache)->stop();
  cleanup_temp_file(path);
}

// =============================================================================
// PR-10: Corrupted document len field doesn't cause OOB mapping
// =============================================================================

TEST_CASE("Corrupted document len field is handled safely",
          "[production][security]") {
  std::string path = get_temp_path("corrupt_len");
  CacheKey key("corrupt-len-key");
  std::vector<std::byte> data(1024, std::byte{0xCC});

  // Write a valid entry
  {
    CacheConfig config;
    config.enable_hit_tracking = false;
    config.set_multi_process(0, 1);
    config.set_enable_checksum(true);

    auto cache = Cache::create(config);
    REQUIRE(cache.has_value());
    (*cache)->add_volume(path, static_cast<size_t>(10 * 1024 * 1024));
    REQUIRE((*cache)->start().has_value());

    auto wh = (*cache)->write_sync(key, data.size());
    REQUIRE(wh.has_value());
    REQUIRE(wh->write_sync(data).has_value());
    REQUIRE(wh->close_sync().has_value());

    (*cache)->stop();
  }

  // Corrupt the len field to an absurdly large value
  {
    std::fstream file(fp(path, static_cast<size_t>(10 * 1024 * 1024), true),
                      std::ios::in | std::ios::out | std::ios::binary);
    REQUIRE(file.is_open());

    file.seekg(0, std::ios::end);
    auto file_size = file.tellg();

    // Find the document in the data area (after ~50% of file)
    // The document header starts with the document magic, followed by len
    // (uint32_t).  Use the live constant, not a literal -- a format bump that
    // changes kMagic would otherwise make this scan match nothing and the
    // corrupted-len security check pass vacuously.
    uint32_t magic = Document::kMagic;
    std::vector<char> buf(4);

    for (auto pos =
             static_cast<std::streamoff>(static_cast<double>(file_size) * 0.5);
         pos < static_cast<std::streamoff>(file_size) - 8; ++pos) {
      file.seekg(pos);
      file.read(buf.data(), 4);
      uint32_t read_magic;
      std::memcpy(&read_magic, buf.data(), 4);
      if (read_magic == magic) {
        // Found document header, corrupt the len field (next 4 bytes)
        uint32_t bad_len = 0xFFFFFFFF;  // ~4GB
        file.seekp(pos + 4);
        file.write(reinterpret_cast<const char *>(&bad_len), 4);
        break;
      }
    }
    file.close();
  }

  // Reopen and read — should handle gracefully (no crash, no OOB)
  {
    CacheConfig config;
    config.enable_hit_tracking = false;
    config.set_multi_process(0, 1);
    config.set_enable_checksum(true);
    config.multi_process_config.set_max_read_retries(1);

    auto cache = Cache::create(config);
    REQUIRE(cache.has_value());
    (*cache)->add_volume(path, static_cast<size_t>(10 * 1024 * 1024));
    REQUIRE((*cache)->start().has_value());

    // Should not crash - may return NotFound, Corrupted, or a valid handle
    // (if the corrupted entry isn't the one we're looking for)
    auto rh = (*cache)->read_sync(key);
    if (rh.has_value()) {
      // If somehow found, content should still be valid
      (void)rh->content();
    }
    // Key point: no crash or segfault occurred

    (*cache)->stop();
  }

  cleanup_temp_file(path);
}

// =============================================================================
// PR-11: Reset stats is safe during concurrent access
// =============================================================================

TEST_CASE("Reset stats during concurrent reads", "[production][concurrent]") {
  std::string path = get_temp_path("reset_stats");

  CacheConfig config;
  config.enable_hit_tracking = false;
  config.ram_cache_size = 0;

  auto cache = Cache::create(config);
  REQUIRE(cache.has_value());
  (*cache)->add_volume(path, static_cast<size_t>(10 * 1024 * 1024));
  REQUIRE((*cache)->start().has_value());

  // Write entries
  for (int i = 0; i < 10; ++i) {
    CacheKey key("reset-stats-" + std::to_string(i));
    std::vector<std::byte> data(128, std::byte{0});
    auto wh = (*cache)->write_sync(key, data.size());
    if (wh.has_value()) {
      wh->write_sync(data);
      wh->close_sync();
    }
  }

  std::atomic<bool> stop{false};

  auto reader_fn = [&]() {
    while (!stop.load(std::memory_order_relaxed)) {
      auto s = (*cache)->stats();
      (void)s;
      (*cache)->reset_stats();
    }
  };

  std::thread t1(reader_fn);
  std::thread t2(reader_fn);

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  stop.store(true);
  t1.join();
  t2.join();

  // No crash = pass
  (*cache)->stop();
  cleanup_temp_file(path);
}

// =============================================================================
// PR-12: Eviction under pressure doesn't corrupt remaining entries
// =============================================================================

TEST_CASE("Eviction preserves integrity of surviving entries",
          "[production][integrity]") {
  std::string path = get_temp_path("eviction_integrity");

  CacheConfig config;
  config.enable_hit_tracking = false;
  config.ram_cache_size = 0;

  auto cache = Cache::create(config);
  REQUIRE(cache.has_value());
  // Small volume to force eviction quickly
  (*cache)->add_volume(path, static_cast<size_t>(128 * 1024 * 1024));
  REQUIRE((*cache)->start().has_value());

  // Write enough entries to fill the cache and trigger eviction
  int total_written = 0;
  for (int i = 0; i < 500; ++i) {
    CacheKey key("evict-" + std::to_string(i));
    // Large enough to fill cache quickly
    std::vector<std::byte> data(static_cast<size_t>(64 * 1024),
                                std::byte{static_cast<uint8_t>(i & 0xFF)});

    auto wh = (*cache)->write_sync(key, data.size());
    if (wh.has_value()) {
      auto wr = wh->write_sync(data);
      if (wr.has_value()) {
        auto cl = wh->close_sync();
        if (cl.has_value()) {
          ++total_written;
        }
      }
    }
  }
  REQUIRE(total_written > 0);

  // Read back whatever survived eviction — verify integrity
  int readable = 0;
  int corrupted = 0;
  for (int i = 0; i < 500; ++i) {
    CacheKey key("evict-" + std::to_string(i));
    auto rh = (*cache)->read_sync(key);
    if (rh.has_value()) {
      auto content = rh->content();
      REQUIRE(content.size() == static_cast<size_t>(64 * 1024));
      // Verify content is correct
      auto expected = std::byte{static_cast<uint8_t>(i & 0xFF)};
      bool ok = true;
      for (auto b : content) {
        if (b != expected) {
          ok = false;
          break;
        }
      }
      if (ok) {
        ++readable;
      } else {
        ++corrupted;
      }
    }
  }
  REQUIRE(readable > 0);
  REQUIRE(corrupted == 0);

  (*cache)->stop();
  cleanup_temp_file(path);
}

// =============================================================================
// PR-13: Concurrent volume initialization (TOCTOU)
// =============================================================================
// Two threads simultaneously opening the same volume file must not corrupt
// the cache.  The O_EXCL fix ensures only one process creates/initialises
// the file while the other opens the existing file.

TEST_CASE("Concurrent volume initialization is safe",
          "[production][concurrent]") {
  std::string path = get_temp_path("concurrent_init");

  // Make sure the file does NOT exist before the test
  cleanup_temp_file(path);

  constexpr int kThreads = 4;
  std::atomic<int> successes{0};
  std::atomic<int> failures{0};

  auto open_fn = [&](int thread_id) {
    CacheConfig config;
    config.enable_hit_tracking = false;
    config.ram_cache_size = 0;

    auto cache = Cache::create(config);
    if (!cache.has_value()) {
      failures.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    (*cache)->add_volume(path, static_cast<size_t>(10 * 1024 * 1024));
    auto start_result = (*cache)->start();
    if (!start_result.has_value()) {
      failures.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    // Write an entry to verify the cache is functional
    CacheKey key("init-thread-" + std::to_string(thread_id));
    std::vector<std::byte> data(256,
                                std::byte{static_cast<uint8_t>(thread_id)});
    auto wh = (*cache)->write_sync(key, data.size());
    if (wh.has_value()) {
      auto wr = wh->write_sync(data);
      if (wr.has_value()) {
        auto cl = wh->close_sync();
        if (cl.has_value()) {
          successes.fetch_add(1, std::memory_order_relaxed);
        } else {
          failures.fetch_add(1, std::memory_order_relaxed);
        }
      } else {
        failures.fetch_add(1, std::memory_order_relaxed);
      }
    } else {
      failures.fetch_add(1, std::memory_order_relaxed);
    }

    (*cache)->stop();
  };

  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back(open_fn, t);
  }
  for (auto &t : threads) {
    t.join();
  }

  // At least one thread should succeed; no crashes or corrupted state
  REQUIRE(successes.load() > 0);

  // Reopen and verify cache is usable after concurrent init
  {
    CacheConfig config;
    config.enable_hit_tracking = false;
    config.ram_cache_size = 0;

    auto cache = Cache::create(config);
    REQUIRE(cache.has_value());
    (*cache)->add_volume(path, static_cast<size_t>(10 * 1024 * 1024));
    REQUIRE((*cache)->start().has_value());

    // Write and read a fresh entry to confirm integrity
    CacheKey key("post-init-verify");
    std::vector<std::byte> data(128, std::byte{0xAA});
    auto wh = (*cache)->write_sync(key, data.size());
    REQUIRE(wh.has_value());
    REQUIRE(wh->write_sync(data).has_value());
    REQUIRE(wh->close_sync().has_value());

    auto rh = (*cache)->read_sync(key);
    REQUIRE(rh.has_value());
    REQUIRE(rh->content().size() == 128);

    (*cache)->stop();
  }

  cleanup_temp_file(path);
}
