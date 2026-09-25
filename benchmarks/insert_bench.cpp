// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Insert-path micro-benchmark (issue #16): the cost of one put, split into
// its API steps, at the block sizes the KV benchmark uses.
//
// Each insert is timed as four steps around the public API:
//   key     CacheKey::from_url (SHA-256 of the key string)
//   open    Cache::write_sync(key, length)          -> WriteHandle
//   write   WriteHandle::write_sync(block)          (one call, whole block)
//   commit  WriteHandle::close_sync()               (build, pwrite, publish)
// With --reserve, `write` is WriteHandle::reserve() instead, and the block is
// generated straight into the returned span (untimed, as the generation of
// the write_sync() block is), so the copy write_sync() makes is gone.
//
// Two phases over a fresh volume of --capacity bytes:
//   fill    the first lap: every insert lands on file space never written.
//   steady  --laps further laps: every insert evicts, as in kv_churn.
// Page faults (minor / major) and context switches per insert are read from
// getrusage() around each phase, so first-touch and allocator faults show
// up without a profiler.
//
// The store is configured as kv_churn configures it: mmap directory on,
// checksum on, no RAM tier, no fsync, library-default wrap retention.
//
//   insert_bench [--block-size B] [--capacity C] [--laps N] [--path DIR]
//                [--wrap-retention on|off] [--no-mmap-dir] [--reserve]
//                [--output FILE.jsonl]

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "cyclone/cache.hpp"
#include "cyclone/config.hpp"
#include "cyclone/error.hpp"
#include "cyclone/key.hpp"

#ifndef _WIN32
#include <sys/resource.h>
#endif

using namespace cyclone;

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
  size_t block_size = size_t{2} * 1024 * 1024;
  size_t capacity = size_t{1} * 1024 * 1024 * 1024;
  double laps = 2.0;
  std::string path;
  std::string output;
  std::optional<bool> wrap_retention;
  bool mmap_dir = true;
  bool reserve = false;
};

struct Usage {
  long minflt = 0;
  long majflt = 0;
  long nivcsw = 0;
  long nvcsw = 0;
};

Usage usage_now() {
  Usage u;
#ifndef _WIN32
  rusage r{};
  getrusage(RUSAGE_SELF, &r);
  u.minflt = r.ru_minflt;
  u.majflt = r.ru_majflt;
  u.nivcsw = r.ru_nivcsw;
  u.nvcsw = r.ru_nvcsw;
#endif
  return u;
}

double micros(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double, std::micro>(b - a).count();
}

struct Series {
  std::vector<double> v;
  void add(double x) { v.push_back(x); }
  [[nodiscard]] double pct(double p) const {
    if (v.empty()) return 0.0;
    std::vector<double> s = v;
    const auto k = static_cast<size_t>(p * static_cast<double>(s.size() - 1));
    std::nth_element(s.begin(), s.begin() + static_cast<ptrdiff_t>(k), s.end());
    return s[k];
  }
  [[nodiscard]] double mean() const {
    if (v.empty()) return 0.0;
    double t = 0.0;
    for (const double x : v) t += x;
    return t / static_cast<double>(v.size());
  }
};

struct PhaseStats {
  Series key, open, write, commit, total;
  Usage before, after;
  double seconds = 0.0;
  uint64_t failures = 0;
};

// Incompressible, cheap, distinct per insert (xorshift64*).
void fill_span(std::span<std::byte> buf, uint64_t seed) {
  uint64_t x = seed * 0x9E3779B97F4A7C15ULL + 1;
  auto* p = reinterpret_cast<uint64_t*>(buf.data());
  const size_t n = buf.size() / sizeof(uint64_t);
  for (size_t i = 0; i < n; ++i) {
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    p[i] = x * 0x2545F4914F6CDD1DULL;
  }
}

std::unique_ptr<Cache> open_store(const Options& opts) {
  CacheConfig config;
  config.ram_cache_size = 0;
  config.max_object_size = 0;
  config.enable_checksum = true;
  config.verify_checksum_on_read = true;
  config.enable_hit_tracking = false;
  config.optimization_config.enabled = false;
  config.directory_sync_interval = std::chrono::milliseconds{0};
  if (opts.mmap_dir) config.set_multi_process(0, 1);
  if (opts.wrap_retention) config.wrap_retention = *opts.wrap_retention;
  auto created = Cache::create(config);
  if (!created) return nullptr;
  auto cache = std::move(*created);
  VolumeConfig vol;
  vol.path =
      (std::filesystem::path(opts.path) / "cyclone_insert_bench.dat").string();
  vol.size = opts.capacity;
  vol.sync_on_write = false;
  vol.max_object_size = 0;
  if (!cache->add_volume(vol)) return nullptr;
  if (!cache->start()) return nullptr;
  return cache;
}

// WriteHandle::reserve() where the library has it (so this file also builds
// against trees that predate it, for before/after runs).
// The block is generated straight into the reserved span, as a producer
// using reserve() would; that generation is the producer's work, so its
// time is returned in `gen_us` for the caller to leave out.
template <class Handle>
bool reserve_and_fill(Handle& wh, size_t length, uint64_t seed,
                      double& gen_us) {
  if constexpr (requires(Handle& h) { h.reserve(size_t{}); }) {
    auto dst = wh.reserve(length);
    if (!dst) return false;
    const auto g0 = Clock::now();
    fill_span(*dst, seed);
    gen_us = micros(g0, Clock::now());
    return true;
  } else {
    std::cerr << "this build has no WriteHandle::reserve()\n";
    std::exit(2);
  }
}

void run_phase(Cache& cache, const Options& opts, uint64_t first,
               uint64_t count, std::vector<std::byte>& block,
               std::array<std::byte, 64>& meta, PhaseStats& st) {
  st.before = usage_now();
  const auto p0 = Clock::now();
  for (uint64_t i = first; i < first + count; ++i) {
    if (!opts.reserve) {
      fill_span(block, i);  // the producer's work: not timed
    }
    double gen_us = 0.0;  // --reserve: generation inside the write step
    const std::string name = "insert-" + std::to_string(i);
    const auto t0 = Clock::now();
    const CacheKey key = CacheKey::from_url(name);
    const auto t1 = Clock::now();
    auto wh = cache.write_sync(key, opts.block_size);
    const auto t2 = Clock::now();
    bool ok = wh.has_value();
    Clock::time_point t3 = t2;
    if (ok) {
      wh->set_header(std::span<const std::byte>(meta));
      if (opts.reserve) {
        ok = reserve_and_fill(*wh, opts.block_size, i, gen_us);
      } else {
        ok = wh->write_sync(std::span<const std::byte>(block)).has_value();
      }
      t3 = Clock::now();
      ok = wh->close_sync().has_value() && ok;
    }
    const auto t4 = Clock::now();
    if (!ok) {
      ++st.failures;
      continue;
    }
    st.key.add(micros(t0, t1));
    st.open.add(micros(t1, t2));
    st.write.add(micros(t2, t3) - gen_us);
    st.commit.add(micros(t3, t4));
    st.total.add(micros(t1, t4) - gen_us);
  }
  st.seconds = std::chrono::duration<double>(Clock::now() - p0).count();
  st.after = usage_now();
}

void print_phase(const char* name, const PhaseStats& st, const Options& opts,
                 std::ostream* json) {
  const auto n = static_cast<double>(std::max<size_t>(1, st.total.v.size()));
  const double gbps = static_cast<double>(st.total.v.size()) *
                      static_cast<double>(opts.block_size) / st.seconds / 1e9;
  std::cout << std::fixed << std::setprecision(1);
  std::cout << "\n"
            << name << ": " << st.total.v.size() << " inserts, "
            << std::setprecision(2) << gbps << " GB/s wall (incl. fill), "
            << st.failures << " failures\n"
            << std::setprecision(1);
  std::cout << "  step      p50 us    p99 us   mean us\n";
  auto row = [&](const char* s, const Series& x) {
    std::cout << "  " << std::left << std::setw(7) << s << std::right
              << std::setw(9) << x.pct(0.50) << std::setw(10) << x.pct(0.99)
              << std::setw(10) << x.mean() << "\n";
  };
  row("key", st.key);
  row("open", st.open);
  row(opts.reserve ? "reserve" : "write", st.write);
  row("commit", st.commit);
  row("total", st.total);
  std::cout << std::setprecision(1) << "  per insert: minor faults "
            << static_cast<double>(st.after.minflt - st.before.minflt) / n
            << ", major faults "
            << static_cast<double>(st.after.majflt - st.before.majflt) / n
            << ", vol/invol ctx switches " << std::setprecision(2)
            << static_cast<double>(st.after.nvcsw - st.before.nvcsw) / n << "/"
            << static_cast<double>(st.after.nivcsw - st.before.nivcsw) / n
            << "\n";
  if (json != nullptr) {
    *json << std::setprecision(2) << "{\"bench\":\"insert\",\"phase\":\""
          << name << "\",\"block_size\":" << opts.block_size
          << ",\"capacity\":" << opts.capacity
          << ",\"reserve\":" << (opts.reserve ? "true" : "false")
          << ",\"inserts\":" << st.total.v.size() << ",\"gb_per_s\":" << gbps
          << ",\"total_p50_us\":" << st.total.pct(0.5)
          << ",\"total_p99_us\":" << st.total.pct(0.99)
          << ",\"total_mean_us\":" << st.total.mean()
          << ",\"write_p50_us\":" << st.write.pct(0.5)
          << ",\"commit_p50_us\":" << st.commit.pct(0.5)
          << ",\"minflt_per_insert\":"
          << static_cast<double>(st.after.minflt - st.before.minflt) / n
          << ",\"majflt_per_insert\":"
          << static_cast<double>(st.after.majflt - st.before.majflt) / n
          << "}\n";
  }
}

bool parse(int argc, char** argv, Options& o) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const bool has = i + 1 < argc;
    if (a == "--block-size" && has) {
      o.block_size = std::strtoull(argv[++i], nullptr, 10);
    } else if (a == "--capacity" && has) {
      o.capacity = std::strtoull(argv[++i], nullptr, 10);
    } else if (a == "--laps" && has) {
      o.laps = std::strtod(argv[++i], nullptr);
    } else if (a == "--path" && has) {
      o.path = argv[++i];
    } else if (a == "--output" && has) {
      o.output = argv[++i];
    } else if (a == "--wrap-retention" && has) {
      o.wrap_retention = std::string(argv[++i]) == "on";
    } else if (a == "--no-mmap-dir") {
      o.mmap_dir = false;
    } else if (a == "--reserve") {
      o.reserve = true;
    } else {
      std::cerr << "usage: insert_bench [--block-size B] [--capacity C] "
                   "[--laps N] [--path DIR] [--wrap-retention on|off] "
                   "[--no-mmap-dir] [--reserve] [--output FILE]\n";
      return false;
    }
  }
  if (o.path.empty()) {
    o.path = (std::filesystem::temp_directory_path() / "cyclone_insert_bench")
                 .string();
  }
  return o.block_size >= 8 && o.capacity > o.block_size * 8;
}

int run(int argc, char** argv) {
  Options opts;
  if (!parse(argc, argv, opts)) return 2;
  std::filesystem::create_directories(opts.path);
  std::filesystem::remove(std::filesystem::path(opts.path) /
                          "cyclone_insert_bench.dat");
  auto cache = open_store(opts);
  if (!cache) {
    std::cerr << "failed to open the store\n";
    return 1;
  }
  std::cout << "insert_bench: block " << opts.block_size << " B, capacity "
            << opts.capacity << " B, laps " << opts.laps << ", mmap dir "
            << (opts.mmap_dir ? "on" : "off") << ", wrap retention "
            << (opts.wrap_retention ? (*opts.wrap_retention ? "on" : "off")
                                    : "default")
            << ", " << (opts.reserve ? "reserve()" : "write_sync()") << "\n";

  std::vector<std::byte> block(opts.block_size);
  std::array<std::byte, 64> meta{};
  const uint64_t per_lap = opts.capacity / (opts.block_size + 512);
  PhaseStats fill;
  PhaseStats steady;
  run_phase(*cache, opts, 0, per_lap, block, meta, fill);
  run_phase(*cache, opts, per_lap,
            static_cast<uint64_t>(static_cast<double>(per_lap) * opts.laps),
            block, meta, steady);

  std::unique_ptr<std::ofstream> json;
  if (!opts.output.empty()) {
    json = std::make_unique<std::ofstream>(opts.output, std::ios::app);
  }
  print_phase("fill", fill, opts, json.get());
  print_phase("steady", steady, opts, json.get());
  cache.reset();
  std::filesystem::remove(std::filesystem::path(opts.path) /
                          "cyclone_insert_bench.dat");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception& e) {
    std::cerr << "insert_bench: " << e.what() << "\n";
  } catch (...) {
    std::cerr << "insert_bench: unknown exception\n";
  }
  return 1;
}
