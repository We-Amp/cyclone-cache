// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Performance Baseline Tool
// Generates reproducible benchmarks and outputs JSON for tracking over time.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <vector>

#include "cyclone/cache.hpp"
#include "cyclone/key.hpp"

using namespace cyclone;

namespace {

struct LatencyStats {
  double min_us;
  double max_us;
  double avg_us;
  double p50_us;
  double p90_us;
  double p99_us;
  double p999_us;
  size_t count;
};

struct BenchmarkResult {
  std::string name;
  size_t operations;
  double elapsed_seconds;
  double ops_per_second;
  LatencyStats latency;
};

LatencyStats calculate_latency_stats(std::vector<double> &latencies_us) {
  LatencyStats stats{};
  if (latencies_us.empty()) {
    return stats;
  }

  stats.count = latencies_us.size();
  std::sort(latencies_us.begin(), latencies_us.end());

  stats.min_us = latencies_us.front();
  stats.max_us = latencies_us.back();

  double sum = 0;
  for (double l : latencies_us) {
    sum += l;
  }
  stats.avg_us = sum / latencies_us.size();

  auto percentile = [&](double p) {
    auto idx = static_cast<size_t>(p * (latencies_us.size() - 1));
    return latencies_us[idx];
  };

  stats.p50_us = percentile(0.50);
  stats.p90_us = percentile(0.90);
  stats.p99_us = percentile(0.99);
  stats.p999_us = percentile(0.999);

  return stats;
}

std::string create_temp_file(size_t size_mb) {
  std::string path =
      (std::filesystem::temp_directory_path() / "cyclone_baseline_cache.dat")
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

BenchmarkResult benchmark_key_generation(size_t num_ops) {
  std::vector<double> latencies;
  latencies.reserve(num_ops);

  auto start = std::chrono::high_resolution_clock::now();

  for (size_t i = 0; i < num_ops; ++i) {
    auto op_start = std::chrono::high_resolution_clock::now();

    CacheKey key("http://example.com/path/" + std::to_string(i) +
                 "?query=value");
    [[maybe_unused]] auto hash = key.segment_hash();

    auto op_end = std::chrono::high_resolution_clock::now();
    latencies.push_back(
        std::chrono::duration<double, std::micro>(op_end - op_start).count());
  }

  auto end = std::chrono::high_resolution_clock::now();
  double secs = std::chrono::duration<double>(end - start).count();

  return {"key_generation", num_ops, secs, num_ops / secs,
          calculate_latency_stats(latencies)};
}

BenchmarkResult benchmark_sequential_write(Cache &cache, size_t num_entries,
                                           size_t content_size) {
  std::vector<std::byte> content(content_size);
  std::mt19937 rng(42);
  for (auto &b : content) {
    b = static_cast<std::byte>(rng() & 0xFF);
  }

  std::vector<double> latencies;
  latencies.reserve(num_entries);

  auto start = std::chrono::high_resolution_clock::now();

  for (size_t i = 0; i < num_entries; ++i) {
    auto op_start = std::chrono::high_resolution_clock::now();

    CacheKey key("baseline-write-" + std::to_string(i));
    auto wh_result = cache.write_sync(key, content.size());
    if (wh_result.has_value()) {
      wh_result->write_sync(std::span<const std::byte>(content));
      wh_result->close_sync();
    }

    auto op_end = std::chrono::high_resolution_clock::now();
    latencies.push_back(
        std::chrono::duration<double, std::micro>(op_end - op_start).count());
  }

  auto end = std::chrono::high_resolution_clock::now();
  double secs = std::chrono::duration<double>(end - start).count();

  return {"sequential_write_" + std::to_string(content_size) + "B", num_entries,
          secs, num_entries / secs, calculate_latency_stats(latencies)};
}

BenchmarkResult benchmark_sequential_read(Cache &cache, size_t num_entries) {
  std::vector<double> latencies;
  latencies.reserve(num_entries);
  size_t successful = 0;

  auto start = std::chrono::high_resolution_clock::now();

  for (size_t i = 0; i < num_entries; ++i) {
    auto op_start = std::chrono::high_resolution_clock::now();

    CacheKey key("baseline-write-" + std::to_string(i));
    auto rh_result = cache.read_sync(key);
    if (rh_result.has_value()) {
      [[maybe_unused]] auto content = rh_result->content();
      ++successful;
    }

    auto op_end = std::chrono::high_resolution_clock::now();
    latencies.push_back(
        std::chrono::duration<double, std::micro>(op_end - op_start).count());
  }

  auto end = std::chrono::high_resolution_clock::now();
  double secs = std::chrono::duration<double>(end - start).count();

  return {"sequential_read", successful, secs, successful / secs,
          calculate_latency_stats(latencies)};
}

BenchmarkResult benchmark_random_read(Cache &cache, size_t num_entries,
                                      size_t num_ops) {
  std::vector<double> latencies;
  latencies.reserve(num_ops);
  std::mt19937 rng(12345);
  std::uniform_int_distribution<size_t> dist(0, num_entries - 1);
  size_t successful = 0;

  auto start = std::chrono::high_resolution_clock::now();

  for (size_t i = 0; i < num_ops; ++i) {
    auto op_start = std::chrono::high_resolution_clock::now();

    size_t idx = dist(rng);
    CacheKey key("baseline-write-" + std::to_string(idx));
    auto rh_result = cache.read_sync(key);
    if (rh_result.has_value()) {
      [[maybe_unused]] auto content = rh_result->content();
      ++successful;
    }

    auto op_end = std::chrono::high_resolution_clock::now();
    latencies.push_back(
        std::chrono::duration<double, std::micro>(op_end - op_start).count());
  }

  auto end = std::chrono::high_resolution_clock::now();
  double secs = std::chrono::duration<double>(end - start).count();

  return {"random_read", num_ops, secs, num_ops / secs,
          calculate_latency_stats(latencies)};
}

BenchmarkResult benchmark_exists_check(Cache &cache, size_t num_entries) {
  std::vector<double> latencies;
  latencies.reserve(num_entries);

  auto start = std::chrono::high_resolution_clock::now();

  for (size_t i = 0; i < num_entries; ++i) {
    auto op_start = std::chrono::high_resolution_clock::now();

    CacheKey key("baseline-write-" + std::to_string(i));
    [[maybe_unused]] auto exists = cache.exists_sync(key);

    auto op_end = std::chrono::high_resolution_clock::now();
    latencies.push_back(
        std::chrono::duration<double, std::micro>(op_end - op_start).count());
  }

  auto end = std::chrono::high_resolution_clock::now();
  double secs = std::chrono::duration<double>(end - start).count();

  return {"exists_check", num_entries, secs, num_entries / secs,
          calculate_latency_stats(latencies)};
}

BenchmarkResult benchmark_cache_miss(Cache &cache, size_t num_ops) {
  std::vector<double> latencies;
  latencies.reserve(num_ops);

  auto start = std::chrono::high_resolution_clock::now();

  for (size_t i = 0; i < num_ops; ++i) {
    auto op_start = std::chrono::high_resolution_clock::now();

    CacheKey key("nonexistent-key-" + std::to_string(i));
    [[maybe_unused]] auto rh_result = cache.read_sync(key);

    auto op_end = std::chrono::high_resolution_clock::now();
    latencies.push_back(
        std::chrono::duration<double, std::micro>(op_end - op_start).count());
  }

  auto end = std::chrono::high_resolution_clock::now();
  double secs = std::chrono::duration<double>(end - start).count();

  return {"cache_miss", num_ops, secs, num_ops / secs,
          calculate_latency_stats(latencies)};
}

BenchmarkResult benchmark_mixed_workload(Cache &cache, size_t num_entries,
                                         size_t num_ops) {
  std::vector<double> latencies;
  latencies.reserve(num_ops);
  std::mt19937 rng(54321);
  std::uniform_int_distribution<size_t> key_dist(0, num_entries - 1);
  std::uniform_int_distribution<int> op_dist(0, 9);

  std::vector<std::byte> content(1024);
  for (auto &b : content) {
    b = static_cast<std::byte>(rng() & 0xFF);
  }

  auto start = std::chrono::high_resolution_clock::now();

  for (size_t i = 0; i < num_ops; ++i) {
    auto op_start = std::chrono::high_resolution_clock::now();

    size_t idx = key_dist(rng);
    CacheKey key("baseline-write-" + std::to_string(idx));

    int op = op_dist(rng);
    if (op < 7) {
      auto rh_result = cache.read_sync(key);
      (void)rh_result;
    } else if (op < 9) {
      auto exists = cache.exists_sync(key);
      (void)exists;
    } else {
      auto wh_result = cache.write_sync(key, content.size());
      if (wh_result.has_value()) {
        wh_result->write_sync(std::span<const std::byte>(content));
        wh_result->close_sync();
      }
    }

    auto op_end = std::chrono::high_resolution_clock::now();
    latencies.push_back(
        std::chrono::duration<double, std::micro>(op_end - op_start).count());
  }

  auto end = std::chrono::high_resolution_clock::now();
  double secs = std::chrono::duration<double>(end - start).count();

  return {"mixed_workload_70r_20e_10w", num_ops, secs, num_ops / secs,
          calculate_latency_stats(latencies)};
}

void output_json(std::ostream &out, const std::vector<BenchmarkResult> &results,
                 const CacheStats &stats, size_t cache_size_mb,
                 size_t num_entries, size_t content_size) {
  auto now = std::chrono::system_clock::now();
  auto now_time = std::chrono::system_clock::to_time_t(now);
  std::tm now_tm = *std::localtime(&now_time);

  out << "{\n";
  out << "  \"timestamp\": \"" << std::put_time(&now_tm, "%Y-%m-%dT%H:%M:%S")
      << "\",\n";
  out << "  \"version\": \"0.1.0\",\n";
  out << "  \"config\": {\n";
  out << "    \"cache_size_mb\": " << cache_size_mb << ",\n";
  out << "    \"num_entries\": " << num_entries << ",\n";
  out << "    \"content_size_bytes\": " << content_size << "\n";
  out << "  },\n";
  out << "  \"benchmarks\": [\n";

  for (size_t i = 0; i < results.size(); ++i) {
    const auto &r = results[i];
    out << "    {\n";
    out << "      \"name\": \"" << r.name << "\",\n";
    out << "      \"operations\": " << r.operations << ",\n";
    out << "      \"elapsed_seconds\": " << std::fixed << std::setprecision(6)
        << r.elapsed_seconds << ",\n";
    out << "      \"ops_per_second\": " << std::fixed << std::setprecision(2)
        << r.ops_per_second << ",\n";
    out << "      \"latency_us\": {\n";
    out << "        \"min\": " << std::fixed << std::setprecision(3)
        << r.latency.min_us << ",\n";
    out << "        \"max\": " << std::fixed << std::setprecision(3)
        << r.latency.max_us << ",\n";
    out << "        \"avg\": " << std::fixed << std::setprecision(3)
        << r.latency.avg_us << ",\n";
    out << "        \"p50\": " << std::fixed << std::setprecision(3)
        << r.latency.p50_us << ",\n";
    out << "        \"p90\": " << std::fixed << std::setprecision(3)
        << r.latency.p90_us << ",\n";
    out << "        \"p99\": " << std::fixed << std::setprecision(3)
        << r.latency.p99_us << ",\n";
    out << "        \"p999\": " << std::fixed << std::setprecision(3)
        << r.latency.p999_us << "\n";
    out << "      }\n";
    out << "    }" << (i + 1 < results.size() ? "," : "") << "\n";
  }

  out << "  ],\n";
  out << "  \"cache_stats\": {\n";
  out << "    \"current_entries\": " << stats.current_entries << ",\n";
  out << "    \"current_bytes\": " << stats.current_bytes << ",\n";
  out << "    \"ram_cache_hits\": " << stats.ram_cache_hits << ",\n";
  out << "    \"ram_cache_misses\": " << stats.ram_cache_misses << ",\n";
  out << "    \"disk_cache_hits\": " << stats.disk_cache_hits << ",\n";
  out << "    \"disk_cache_misses\": " << stats.disk_cache_misses << ",\n";
  out << "    \"bytes_read\": " << stats.bytes_read << ",\n";
  out << "    \"bytes_written\": " << stats.bytes_written << ",\n";
  out << "    \"evictions\": " << stats.evictions << "\n";
  out << "  }\n";
  out << "}\n";
}

void print_summary(const std::vector<BenchmarkResult> &results) {
  std::cout << "\n";
  std::cout << std::setw(35) << std::left << "Benchmark" << std::setw(12)
            << std::right << "Ops/sec" << std::setw(10) << "Avg(us)"
            << std::setw(10) << "P50(us)" << std::setw(10) << "P99(us)"
            << std::setw(12) << "P99.9(us)"
            << "\n";
  std::cout << std::string(89, '-') << "\n";

  for (const auto &r : results) {
    std::cout << std::setw(35) << std::left << r.name << std::setw(12)
              << std::right << std::fixed << std::setprecision(0)
              << r.ops_per_second << std::setw(10) << std::setprecision(2)
              << r.latency.avg_us << std::setw(10) << r.latency.p50_us
              << std::setw(10) << r.latency.p99_us << std::setw(12)
              << r.latency.p999_us << "\n";
  }
  std::cout << std::string(89, '-') << "\n";
}

}  // namespace

int main(int argc, char *argv[]) {
  size_t cache_size_mb = 100;
  size_t num_entries = 1000;
  size_t content_size = 4096;
  std::string output_file;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--cache-size" && i + 1 < argc) {
      cache_size_mb = std::stoul(argv[++i]);
    } else if (arg == "--entries" && i + 1 < argc) {
      num_entries = std::stoul(argv[++i]);
    } else if (arg == "--content-size" && i + 1 < argc) {
      content_size = std::stoul(argv[++i]);
    } else if (arg == "--output" && i + 1 < argc) {
      output_file = argv[++i];
    } else if (arg == "--help" || arg == "-h") {
      std::cout << "Usage: " << argv[0] << " [options]\n";
      std::cout << "Options:\n";
      std::cout
          << "  --cache-size MB     Cache size in megabytes (default: 100)\n";
      std::cout << "  --entries N         Number of entries to test (default: "
                   "1000)\n";
      std::cout
          << "  --content-size B    Content size in bytes (default: 4096)\n";
      std::cout << "  --output FILE       Write JSON results to file\n";
      std::cout << "  --help, -h          Show this help\n";
      return 0;
    }
  }

  std::cout << "Cyclone Cache Performance Baseline\n";
  std::cout << "===================================\n";
  std::cout << "Cache size: " << cache_size_mb << " MB\n";
  std::cout << "Entries: " << num_entries << "\n";
  std::cout << "Content size: " << content_size << " bytes\n";
  if (!output_file.empty()) {
    std::cout << "Output: " << output_file << "\n";
  }
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

  std::cout << "Running benchmarks...\n";

  std::cout << "  [1/7] Key generation...\n";
  results.push_back(benchmark_key_generation(100000));

  std::cout << "  [2/7] Sequential write...\n";
  results.push_back(
      benchmark_sequential_write(*cache, num_entries, content_size));

  std::cout << "  [3/7] Sequential read...\n";
  results.push_back(benchmark_sequential_read(*cache, num_entries));

  std::cout << "  [4/7] Random read...\n";
  results.push_back(
      benchmark_random_read(*cache, num_entries, num_entries * 2));

  std::cout << "  [5/7] Exists check...\n";
  results.push_back(benchmark_exists_check(*cache, num_entries));

  std::cout << "  [6/7] Cache miss...\n";
  results.push_back(benchmark_cache_miss(*cache, num_entries));

  std::cout << "  [7/7] Mixed workload...\n";
  results.push_back(
      benchmark_mixed_workload(*cache, num_entries, num_entries * 5));

  auto stats = cache->stats();

  print_summary(results);

  std::cout << "\nCache Statistics:\n";
  std::cout << "  Entries: " << stats.current_entries << "\n";
  std::cout << "  Bytes used: " << stats.current_bytes << "\n";
  std::cout << "  RAM cache hits: " << stats.ram_cache_hits << "\n";
  std::cout << "  Disk cache hits: " << stats.disk_cache_hits << "\n";
  std::cout << "  Evictions: " << stats.evictions << "\n";

  if (!output_file.empty()) {
    std::ofstream out(output_file);
    if (out.is_open()) {
      output_json(out, results, stats, cache_size_mb, num_entries,
                  content_size);
      std::cout << "\nResults written to: " << output_file << "\n";
    } else {
      std::cerr << "Failed to open output file: " << output_file << "\n";
    }
  }

  cache->stop();
  cleanup_temp_file(cache_path);

  return 0;
}
