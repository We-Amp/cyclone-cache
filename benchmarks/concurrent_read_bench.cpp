// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Concurrent read scaling microbenchmark.
// Populates a fully RAM-resident cache, then measures aggregate random-read
// throughput as thread count rises.  The working set is page-cache-resident
// via the persistent mmap (note: Cache::read_sync never populates the CLFUS
// RAM tier — only the alternate read path puts — so even with the RAM tier
// on, every read exercises the miss side of CLFUS plus the volume path).
// No I/O in the hot path, so flat or collapsing throughput => read-path
// contention, not storage.  Compare the per-binary scaling%% column across
// builds — and quote 1T alongside: single-thread wins (e.g. the CRC
// validation-cache sizing) and multi-thread scaling wins are separate
// effects.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "cyclone/cache.hpp"
#include "cyclone/key.hpp"

using namespace cyclone;

namespace {
std::string make_temp_volume(size_t size_mb) {
  std::string path =
      (std::filesystem::temp_directory_path() / "cyclone_concur_bench.dat")
          .string();
  std::remove(path.c_str());
  FILE *f = std::fopen(path.c_str(), "wb");
  if (f) {
    std::fseek(f, static_cast<long>(size_mb * 1024 * 1024 - 1), SEEK_SET);
    std::fputc(0, f);
    std::fclose(f);
  }
  return path;
}
}  // namespace

int main(int argc, char *argv[]) {
  size_t num_entries = 20000;
  size_t content_size = 512;
  double seconds_per_point = 3.0;
  size_t cache_size_mb = 512;
  if (argc > 1) num_entries = std::stoul(argv[1]);
  if (argc > 2) content_size = std::stoul(argv[2]);
  if (argc > 3) seconds_per_point = std::stod(argv[3]);
  int fixed_threads = (argc > 4) ? std::stoi(argv[4]) : 0;
  if (argc > 5) cache_size_mb = std::stoul(argv[5]);
  bool ram_off = (argc > 6) && std::string(argv[6]) == "ramoff";

  std::cout << "Concurrent read scaling benchmark\n"
            << "  entries=" << num_entries << " content=" << content_size
            << "B (working set ~"
            << (num_entries * content_size) / (size_t{1024} * 1024)
            << " MB, RAM-resident), seconds/point=" << seconds_per_point
            << "\n\n";

  std::cout << "  volume=" << cache_size_mb << " MB (stripe floor 128MB => ~"
            << (cache_size_mb / 128 > 0 ? cache_size_mb / 128 : 1)
            << " stripes)\n\n";
  std::string cache_path = make_temp_volume(cache_size_mb);
  CacheConfig config;  // default: 256 MB CLFUS RAM cache, num_segments=4
  if (ram_off) {
    config.ram_cache_size = 0;
    std::cout << "  RAM cache DISABLED (volume-only path, matches mps)\n";
  }
  auto cache_result = Cache::create(config);
  if (!cache_result.has_value()) {
    std::cerr << "create failed\n";
    return 1;
  }
  auto &cache = *cache_result;
  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = cache_size_mb * 1024 * 1024;
  if (!cache->add_volume(vol_config).has_value()) {
    std::cerr << "add_volume failed\n";
    return 1;
  }
  if (!cache->start().has_value()) {
    std::cerr << "start failed\n";
    return 1;
  }

  std::vector<std::byte> content(content_size, std::byte{0xAB});
  for (size_t i = 0; i < num_entries; ++i) {
    CacheKey key("bench-key-" + std::to_string(i));
    auto wh = cache->write_sync(key, content.size());
    if (wh.has_value()) {
      wh->write_sync(std::span<const std::byte>(content));
      wh->close_sync();
    }
  }
  for (size_t i = 0; i < num_entries; ++i) {
    CacheKey key("bench-key-" + std::to_string(i));
    auto rh = cache->read_sync(key);
    if (rh.has_value()) {
      [[maybe_unused]] auto c = rh->content();
    }
  }

  // Precompute the keys: building a CacheKey from a std::string
  // concatenation inside the hot loop allocates per read, and that
  // allocator traffic flattens every configuration equally (it hid the
  // real scaling signal entirely on macOS's xzone allocator).  The hot
  // loop below must stay zero-allocation.
  std::vector<CacheKey> keys;
  keys.reserve(num_entries);
  for (size_t i = 0; i < num_entries; ++i) {
    keys.emplace_back("bench-key-" + std::to_string(i));
  }

  auto run_point = [&](int nthreads) -> double {
    std::atomic<bool> go{false}, stop{false};
    std::vector<uint64_t> counts(nthreads, 0);
    std::vector<std::thread> threads;
    threads.reserve(nthreads);
    for (int t = 0; t < nthreads; ++t) {
      threads.emplace_back([&, t]() {
        std::mt19937 rng(1000 + t);
        std::uniform_int_distribution<size_t> dist(0, num_entries - 1);
        while (!go.load(std::memory_order_acquire)) {
        }
        uint64_t local = 0;
        while (!stop.load(std::memory_order_relaxed)) {
          const CacheKey &key = keys[dist(rng)];
          auto rh = cache->read_sync(key);
          if (rh.has_value()) {
            [[maybe_unused]] auto c = rh->content();
            ++local;
          }
        }
        counts[t] = local;
      });
    }
    auto start = std::chrono::steady_clock::now();
    go.store(true, std::memory_order_release);
    std::this_thread::sleep_for(
        std::chrono::duration<double>(seconds_per_point));
    stop.store(true, std::memory_order_relaxed);
    for (auto &th : threads) th.join();
    double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
            .count();
    uint64_t total = 0;
    for (auto c : counts) total += c;
    return total / secs;
  };

  if (fixed_threads > 0) {
    std::printf("fixed %d threads: %.0f reads/sec\n", fixed_threads,
                run_point(fixed_threads));
    return 0;
  }
  std::printf("%8s %16s %12s %10s\n", "threads", "reads/sec", "vs 1T",
              "scaling%");
  double base = 0.0;
  for (int nt : {1, 2, 4, 8, 16, 32, 64}) {
    if (nt > 2 * (int)std::thread::hardware_concurrency()) break;
    double rps = run_point(nt);
    if (nt == 1) base = rps;
    double speedup = rps / base;
    std::printf("%8d %16.0f %11.2fx %9.0f%%\n", nt, rps, speedup,
                100.0 * speedup / nt);
  }
  std::cout << "\n(scaling% ~100 = linear scaling; collapsing scaling% = lock "
               "contention)\n";
  return 0;
}
