// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "cyclone/cache.hpp"
#include "cyclone/key.hpp"

using namespace cyclone;

namespace {

std::string create_temp_file(size_t size_mb) {
  std::string path =
      (std::filesystem::temp_directory_path() / "cyclone_benchmark_cache.dat")
          .string();
  std::remove(path.c_str());

  FILE *f = std::fopen(path.c_str(), "wb");
  if (f != nullptr) {
    std::fseek(f, size_mb * 1024 * 1024 - 1, SEEK_SET);
    std::fputc(0, f);
    std::fclose(f);
  }
  return path;
}

void cleanup_temp_file(const std::string &path) { std::remove(path.c_str()); }

struct BenchmarkResult {
  std::string name;
  size_t operations;
  double elapsed_seconds;
  double ops_per_second;
  double avg_latency_us;
};

void print_result(const BenchmarkResult &result) {
  std::printf("%-40s %10zu ops  %8.3f sec  %12.0f ops/sec  %8.2f us/op\n",
              result.name.c_str(), result.operations, result.elapsed_seconds,
              result.ops_per_second, result.avg_latency_us);
}

BenchmarkResult benchmark_write(Cache &cache, size_t num_entries,
                                size_t content_size) {
  std::vector<std::byte> content(content_size);
  std::mt19937 rng(42);
  for (auto &b : content) {
    b = static_cast<std::byte>(rng() & 0xFF);
  }

  auto start = std::chrono::high_resolution_clock::now();

  for (size_t i = 0; i < num_entries; ++i) {
    CacheKey key("benchmark-key-" + std::to_string(i));
    auto wh_result = cache.write_sync(key, content.size());
    if (wh_result.has_value()) {
      wh_result->write_sync(std::span<const std::byte>(content));
      wh_result->close_sync();
    }
  }

  auto end = std::chrono::high_resolution_clock::now();
  double secs = std::chrono::duration<double>(end - start).count();

  return {"Write " + std::to_string(content_size) + "B entries", num_entries,
          secs, num_entries / secs, (secs * 1e6) / num_entries};
}

BenchmarkResult benchmark_read(Cache &cache, size_t num_entries) {
  auto start = std::chrono::high_resolution_clock::now();

  size_t successful_reads = 0;
  for (size_t i = 0; i < num_entries; ++i) {
    CacheKey key("benchmark-key-" + std::to_string(i));
    auto rh_result = cache.read_sync(key);
    if (rh_result.has_value()) {
      [[maybe_unused]] auto content = rh_result->content();
      ++successful_reads;
    }
  }

  auto end = std::chrono::high_resolution_clock::now();
  double secs = std::chrono::duration<double>(end - start).count();

  return {"Read (sequential)", successful_reads, secs, successful_reads / secs,
          (secs * 1e6) / successful_reads};
}

BenchmarkResult benchmark_read_random(Cache &cache, size_t num_entries,
                                      size_t num_ops) {
  std::mt19937 rng(12345);
  std::uniform_int_distribution<size_t> dist(0, num_entries - 1);

  auto start = std::chrono::high_resolution_clock::now();

  size_t successful_reads = 0;
  for (size_t i = 0; i < num_ops; ++i) {
    size_t idx = dist(rng);
    CacheKey key("benchmark-key-" + std::to_string(idx));
    auto rh_result = cache.read_sync(key);
    if (rh_result.has_value()) {
      [[maybe_unused]] auto content = rh_result->content();
      ++successful_reads;
    }
  }

  auto end = std::chrono::high_resolution_clock::now();
  double secs = std::chrono::duration<double>(end - start).count();

  return {"Read (random)", num_ops, secs, num_ops / secs,
          (secs * 1e6) / num_ops};
}

BenchmarkResult benchmark_exists(Cache &cache, size_t num_entries) {
  auto start = std::chrono::high_resolution_clock::now();

  size_t found = 0;
  for (size_t i = 0; i < num_entries; ++i) {
    CacheKey key("benchmark-key-" + std::to_string(i));
    auto exists_result = cache.exists_sync(key);
    if (exists_result.has_value() && *exists_result) {
      ++found;
    }
  }

  auto end = std::chrono::high_resolution_clock::now();
  double secs = std::chrono::duration<double>(end - start).count();

  return {"Exists check", num_entries, secs, num_entries / secs,
          (secs * 1e6) / num_entries};
}

BenchmarkResult benchmark_miss(Cache &cache, size_t num_ops) {
  auto start = std::chrono::high_resolution_clock::now();

  for (size_t i = 0; i < num_ops; ++i) {
    CacheKey key("nonexistent-key-" + std::to_string(i));
    auto rh_result = cache.read_sync(key);
    (void)rh_result;
  }

  auto end = std::chrono::high_resolution_clock::now();
  double secs = std::chrono::duration<double>(end - start).count();

  return {"Read miss", num_ops, secs, num_ops / secs, (secs * 1e6) / num_ops};
}

BenchmarkResult benchmark_key_generation(size_t num_ops) {
  auto start = std::chrono::high_resolution_clock::now();

  for (size_t i = 0; i < num_ops; ++i) {
    CacheKey key("http://example.com/path/" + std::to_string(i) +
                 "?query=value");
    [[maybe_unused]] auto hash = key.segment_hash();
  }

  auto end = std::chrono::high_resolution_clock::now();
  double secs = std::chrono::duration<double>(end - start).count();

  return {"Key generation (SHA-256)", num_ops, secs, num_ops / secs,
          (secs * 1e6) / num_ops};
}

}  // namespace

int main(int argc, char *argv[]) {
  size_t cache_size_mb = 100;
  size_t num_entries = 1000;
  size_t content_size = 4096;

  if (argc > 1) {
    cache_size_mb = std::stoul(argv[1]);
  }
  if (argc > 2) {
    num_entries = std::stoul(argv[2]);
  }
  if (argc > 3) {
    content_size = std::stoul(argv[3]);
  }

  std::cout << "Cyclone Cache Benchmark\n";
  std::cout << "=======================\n";
  std::cout << "Cache size: " << cache_size_mb << " MB\n";
  std::cout << "Entries: " << num_entries << "\n";
  std::cout << "Content size: " << content_size << " bytes\n";
  std::cout << "\n";

  std::string cache_path = create_temp_file(cache_size_mb);

  CacheConfig config;
  auto cache_result = Cache::create(config);
  if (!cache_result.has_value()) {
    std::cerr << "Failed to create cache\n";
    return 1;
  }
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = cache_size_mb * 1024 * 1024;

  if (!cache->add_volume(vol_config).has_value()) {
    std::cerr << "Failed to add volume\n";
    cleanup_temp_file(cache_path);
    return 1;
  }

  if (!cache->start().has_value()) {
    std::cerr << "Failed to start cache\n";
    cleanup_temp_file(cache_path);
    return 1;
  }

  std::vector<BenchmarkResult> results;

  std::cout << "Running benchmarks...\n\n";

  results.push_back(benchmark_key_generation(100000));

  results.push_back(benchmark_write(*cache, num_entries, content_size));

  results.push_back(benchmark_read(*cache, num_entries));

  results.push_back(
      benchmark_read_random(*cache, num_entries, num_entries * 2));

  results.push_back(benchmark_exists(*cache, num_entries));

  results.push_back(benchmark_miss(*cache, num_entries));

  std::cout << "Results:\n";
  std::cout << std::string(100, '-') << "\n";
  for (const auto &result : results) {
    print_result(result);
  }
  std::cout << std::string(100, '-') << "\n";

  auto stats = cache->stats();
  std::cout << "\nCache Statistics:\n";
  std::cout << "  Entries: " << stats.current_entries << "\n";
  std::cout << "  Bytes used: " << stats.current_bytes << "\n";
  std::cout << "  Bytes read: " << stats.bytes_read << "\n";
  std::cout << "  Bytes written: " << stats.bytes_written << "\n";
  std::cout << "  RAM cache hits: " << stats.ram_cache_hits << "\n";
  std::cout << "  RAM cache misses: " << stats.ram_cache_misses << "\n";
  std::cout << "  Disk cache hits: " << stats.disk_cache_hits << "\n";
  std::cout << "  Disk cache misses: " << stats.disk_cache_misses << "\n";
  std::cout << "  Evictions: " << stats.evictions << "\n";

  cache->stop();
  cleanup_temp_file(cache_path);

  return 0;
}
