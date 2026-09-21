// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Cyclone as an LLM KV-cache / prefix-cache storage tier.
//
// Implements the shared "KV-cache storage-tier workload spec (v1)" so the
// numbers are directly comparable with the peer harnesses (LMDB, RocksDB,
// file-per-block).  Everything the spec pins down -- key derivation, value
// generator, block sizes, dataset size, store capacity, phase structure,
// Zipf parameters, JSON schema -- is implemented verbatim here; the
// Cyclone-specific tuning is printed at startup (the spec allows
// store-specific tuning provided it is printed).
//
// Phases, per block size, in a fresh store:
//   1. put               single writer, sequential, i = 0..N-1
//   2. get_first_touch   single reader, sequential, touches every 4 KiB page
//   3. get_warm          Zipf(0.99), T in {1,4,8}, modes view + copy
//   4. restart           clean close + reopen, read all N once (view)
//   5. multiprocess_read fork 4 reader processes, each phase-3 at T=1, view
//      and copy
//
// Cyclone tuning, and why:
//   max_object_size = 0        blocks run to 32 MiB; the 64 MiB default would
//                              still pass but the bound is simply not part of
//                              this workload.
//   stripe_size                a document must fit ONE stripe's data area, so
//                              the stripe has to exceed the largest block by a
//                              wide margin: keys hash-route to stripes, and a
//                              stripe that fills wraps (evicts).  See
//                              choose_stripe_size() for the rule.
//   ram_cache_size = 0         the CLFUS RAM tier is irrelevant for multi-MB
//                              blobs: it would copy every block into the heap
//                              to serve a tier the OS page cache already
//                              serves zero-copy via the volume mmap.  Disk
//                              hits give a borrowed mmap span, which is what a
//                              KV offload tier wants.
//   enable_checksum = true     mandatory in multi-process mode.  Note that
//                              mmap-directory mode also FORCES
//                              verify_checksum_on_read = true
//                              (Cache::add_volume_locked), so --no-verify only
//                              takes effect together with --no-mmap-dir; the
//                              effective value is what gets printed/emitted.
//   hit tracking off,          none of the three background subsystems is
//   optimization off,          meaningful for a KV tier, and switching them
//   directory_sync_interval=0  off leaves the process single-threaded at the
//                              phase-5 fork() point.
//   multi-process directory    ON by default: it is what makes the restart
//                              phase meaningful (the default in-memory
//                              directory does not survive a restart -- run
//                              --no-mmap-dir to measure exactly that).
//
// Phase 5 opens the volume from the children as SEPARATE Cache instances on
// the same file, with the SAME configuration the parent used -- including the
// same (process_index = 0, total_processes = 1).  That is safe, and it is the
// deliberate choice:
//   - total_processes is not persisted anywhere and is not part of the
//     structural fingerprint (format major, mmap flag, stripe count, base
//     stripe size -- see fingerprint_cache_path in src/core/volume.cpp), so it
//     can never trigger a geometry reset.  It governs write ownership only
//     (stripe_index % total_processes), and these children never write.
//   - an identical config resolves to the identical fingerprinted file and the
//     identical geometry, so Volume::open() takes no reset branch at all; had
//     it wanted one, the reset gate would refuse it under the parent's live
//     shared lifetime lock (CacheError::ResetRefusedLivePeer) rather than wipe
//     the data.
//   - the exclusive init lock is held only across open_locked(), so concurrent
//     openers serialize briefly instead of deadlocking against the parent.
// The children therefore exercise the real cross-process read path: their own
// mapping of the shared mmap directory, seqlock bucket reads, CRC validation
// and shared borrow/lease state.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "cyclone/cache.hpp"
#include "cyclone/config.hpp"
#include "cyclone/error.hpp"
#include "cyclone/key.hpp"

#ifndef _WIN32
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace cyclone;

namespace {

constexpr const char *kStoreName = "cyclone";
constexpr const char *kStoreVersion = "0.1.0";

constexpr size_t kPageSize = 4096;
constexpr size_t kMetaSize = 64;  // spec: 64-byte header = the digest twice
constexpr size_t kCapacityBytes = size_t{8} * 1024 * 1024 * 1024;  // 8 GiB
constexpr size_t kDatasetBudget = size_t{4} * 1024 * 1024 * 1024;  // 4 GiB
constexpr size_t kMaxBlocks = 4096;
constexpr double kZipfTheta = 0.99;
constexpr double kWarmupSeconds = 2.0;
constexpr uint32_t kReaderProcesses = 4;
constexpr const char *kVolumeStem = "cyclone_kv_bench";

// Keeps the page-touch / copy work observable so the optimizer cannot drop it.
std::atomic<uint64_t> g_sink{0};

// ---------------------------------------------------------------------------
// Value generator: xorshift64* seeded with the block index (spec).
//
// SPEC RESOLUTION: "seed = i" is degenerate for i = 0 (xorshift64* is stuck at
// zero), so a zero seed -- and only a zero seed -- is replaced by the golden
// ratio constant below.  Every other block is exactly "seed = i".
// ---------------------------------------------------------------------------
constexpr uint64_t kZeroSeedSubstitute = 0x9E3779B97F4A7C15ULL;

inline uint64_t xorshift64star(uint64_t &state) {
  state ^= state >> 12;
  state ^= state << 25;
  state ^= state >> 27;
  return state * 0x2545F4914F6CD1DBULL;
}

inline uint64_t seed_state(uint64_t seed) {
  return seed == 0 ? kZeroSeedSubstitute : seed;
}

void fill_block(std::span<std::byte> out, uint64_t seed) {
  uint64_t state = seed_state(seed);
  size_t off = 0;
  while (off + sizeof(uint64_t) <= out.size()) {
    const uint64_t word = xorshift64star(state);
    std::memcpy(out.data() + off, &word, sizeof(word));
    off += sizeof(word);
  }
  if (off < out.size()) {
    const uint64_t word = xorshift64star(state);
    std::memcpy(out.data() + off, &word, out.size() - off);
  }
}

// First 8 bytes of block `seed`, for a cheap content spot-check.
uint64_t first_word_of_block(uint64_t seed) {
  uint64_t state = seed_state(seed);
  return xorshift64star(state);
}

// ---------------------------------------------------------------------------
// Zipf generator: Gray et al. (as used by YCSB), theta = 0.99.
// The uniform source is the same xorshift64* engine (seed = 42 + thread id) so
// a peer harness can reproduce the exact access sequence.
// ---------------------------------------------------------------------------
class ZipfGenerator {
 public:
  ZipfGenerator(size_t n, double theta, uint64_t seed)
      : _n(n), _theta(theta), _state(seed_state(seed)) {
    _zetan = zeta(n, theta);
    const double zeta2 = zeta(2, theta);
    _alpha = 1.0 / (1.0 - theta);
    _eta = (1.0 - std::pow(2.0 / static_cast<double>(n), 1.0 - theta)) /
           (1.0 - zeta2 / _zetan);
  }

  size_t next() {
    const double u = uniform();
    const double uz = u * _zetan;
    if (uz < 1.0) return 0;
    if (uz < 1.0 + std::pow(0.5, _theta)) return 1;
    const auto idx = static_cast<size_t>(
        static_cast<double>(_n) * std::pow(_eta * u - _eta + 1.0, _alpha));
    return idx < _n ? idx : _n - 1;
  }

 private:
  static double zeta(size_t n, double theta) {
    double sum = 0.0;
    for (size_t i = 1; i <= n; ++i) {
      sum += 1.0 / std::pow(static_cast<double>(i), theta);
    }
    return sum;
  }

  double uniform() {
    // 53 random bits mapped into [0, 1).
    return static_cast<double>(xorshift64star(_state) >> 11) *
           (1.0 / 9007199254740992.0);
  }

  size_t _n;
  double _theta;
  uint64_t _state;
  double _zetan = 0.0;
  double _alpha = 0.0;
  double _eta = 0.0;
};

// ---------------------------------------------------------------------------
// Measurement bookkeeping
// ---------------------------------------------------------------------------
struct Percentiles {
  double p50 = 0.0;
  double p99 = 0.0;
  double p999 = 0.0;
};

Percentiles compute_percentiles(std::vector<double> &latencies_us) {
  Percentiles p;
  if (latencies_us.empty()) return p;
  std::sort(latencies_us.begin(), latencies_us.end());
  auto pick = [&](double q) {
    const auto idx =
        static_cast<size_t>(q * static_cast<double>(latencies_us.size() - 1));
    return latencies_us[idx];
  };
  p.p50 = pick(0.50);
  p.p99 = pick(0.99);
  p.p999 = pick(0.999);
  return p;
}

struct Record {
  std::string phase;
  std::string mode;  // empty => omitted
  size_t block_size = 0;
  size_t n = 0;
  uint32_t threads = 1;
  uint32_t processes = 0;  // 0 => omitted
  double ops_per_s = 0.0;
  double gb_per_s = 0.0;
  bool has_latency = false;
  Percentiles latency;
  double hit_fraction = -1.0;  // < 0 => omitted
  bool verify_checksum = true;
  bool mmap_directory = true;
};

// gb_per_s is DECIMAL GB (bytes / 1e9), as is conventional for storage
// throughput.  Block sizes stay binary (KiB/MiB) exactly as the spec lists
// them.
double gb_per_s(uint64_t bytes, double seconds) {
  if (seconds <= 0.0) return 0.0;
  return static_cast<double>(bytes) / 1e9 / seconds;
}

void emit_json(std::ostream &out, const Record &r) {
  out << "{\"store\":\"" << kStoreName << "\",\"block_size\":" << r.block_size
      << ",\"n\":" << r.n << ",\"phase\":\"" << r.phase << "\"";
  if (!r.mode.empty()) {
    out << ",\"mode\":\"" << r.mode << "\"";
  }
  out << ",\"threads\":" << r.threads;
  if (r.processes > 0) {
    out << ",\"processes\":" << r.processes;
  }
  out << std::fixed << ",\"ops_per_s\":" << std::setprecision(2) << r.ops_per_s
      << ",\"gb_per_s\":" << std::setprecision(4) << r.gb_per_s;
  if (r.has_latency) {
    out << std::setprecision(3) << ",\"p50_us\":" << r.latency.p50
        << ",\"p99_us\":" << r.latency.p99 << ",\"p999_us\":" << r.latency.p999;
  }
  if (r.hit_fraction >= 0.0) {
    out << std::setprecision(4) << ",\"hit_fraction\":" << r.hit_fraction;
  }
  out << ",\"verify_checksum\":" << (r.verify_checksum ? "true" : "false")
      << ",\"mmap_directory\":" << (r.mmap_directory ? "true" : "false")
      << "}\n";
  out.flush();
}

std::string format_double(double v, int precision) {
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(precision) << v;
  return oss.str();
}

void print_markdown(std::ostream &out, const std::vector<Record> &records) {
  out << "\n| store | block_size | n | phase | mode | threads | ops_per_s |"
      << " gb_per_s | p50_us | p99_us | p999_us | hit_fraction |\n";
  out << "|---|---:|---:|---|---|---:|---:|---:|---:|---:|---:|---:|\n";
  for (const auto &r : records) {
    out << "| " << kStoreName << " | " << r.block_size << " | " << r.n << " | "
        << r.phase << " | " << (r.mode.empty() ? "-" : r.mode) << " | "
        << r.threads;
    if (r.processes > 0) {
      out << "x" << r.processes << "p";
    }
    out << " | " << format_double(r.ops_per_s, 1) << " | "
        << format_double(r.gb_per_s, 3) << " | "
        << (r.has_latency ? format_double(r.latency.p50, 1) : "-") << " | "
        << (r.has_latency ? format_double(r.latency.p99, 1) : "-") << " | "
        << (r.has_latency ? format_double(r.latency.p999, 1) : "-") << " | "
        << (r.hit_fraction >= 0.0 ? format_double(r.hit_fraction, 3) : "-")
        << " |\n";
  }
  out.flush();
}

// ---------------------------------------------------------------------------
// Machine info (printed once, to stderr, plus one machine_info JSON line)
// ---------------------------------------------------------------------------
std::string run_command(const std::string &cmd) {
#ifdef _WIN32
  (void)cmd;
  return "unknown";
#else
  std::string out;
  FILE *pipe = ::popen(cmd.c_str(), "r");
  if (pipe == nullptr) return "unknown";
  char buf[512];
  while (std::fgets(buf, sizeof(buf), pipe) != nullptr) {
    out += buf;
  }
  ::pclose(pipe);
  while (!out.empty() && (out.back() == '\n' || out.back() == ' ' ||
                          out.back() == '\r' || out.back() == '\t')) {
    out.pop_back();
  }
  size_t begin = out.find_first_not_of(" \t");
  if (begin == std::string::npos) return "unknown";
  out = out.substr(begin);
  std::replace(out.begin(), out.end(), '\n', ' ');
  std::replace(out.begin(), out.end(), '"', '\'');
  return out.empty() ? "unknown" : out;
#endif
}

struct MachineInfo {
  std::string cpu;
  std::string cores;
  std::string os;
  std::string filesystem;
  std::string commit;
};

MachineInfo collect_machine_info(const std::string &dir) {
  MachineInfo info;
#ifdef __APPLE__
  info.cpu = run_command("sysctl -n machdep.cpu.brand_string");
  info.cores = run_command(
      "echo \"$(sysctl -n hw.physicalcpu) physical / "
      "$(sysctl -n hw.logicalcpu) logical\"");
  info.os = run_command(
      "echo \"$(uname -sr) / macOS $(sw_vers "
      "-productVersion)\"");
  info.filesystem = run_command("mount | grep -F \" on $(df -P '" + dir +
                                "' | tail -1 | awk '{print $6}') \" | head -1");
#else
  info.cpu = run_command(
      "awk -F: '/model name/{print $2; exit}' /proc/cpuinfo 2>/dev/null");
  info.cores = run_command("nproc 2>/dev/null");
  info.os = run_command("uname -sr");
  info.filesystem = run_command("stat -f -c %T '" + dir + "' 2>/dev/null");
#endif
  info.commit = run_command("git rev-parse --short HEAD 2>/dev/null");
  if (info.cores == "unknown") {
    info.cores = std::to_string(std::thread::hardware_concurrency());
  }
  return info;
}

// ---------------------------------------------------------------------------
// Store setup
// ---------------------------------------------------------------------------
enum class Mode : uint8_t { kView, kCopy };

const char *mode_name(Mode mode) {
  return mode == Mode::kView ? "view" : "copy";
}

struct Options {
  std::vector<size_t> block_sizes;
  std::vector<uint32_t> threads;
  double seconds = 10.0;
  bool no_verify = false;
  bool no_mmap_dir = false;
  bool skip_multiprocess = false;
  std::string output;
  std::string path;
  std::string drop_caches_cmd;  // run before phases 2 and 4 (e.g. Linux
                                // "sync; echo 3 > /proc/sys/vm/drop_caches")
};

// Optional page-cache drop between phases so first-touch and restart read
// from the device instead of the page cache.  Needs privileges the harness
// does not have itself; the command is the operator's.
void drop_caches(const Options &opts) {
  if (opts.drop_caches_cmd.empty()) return;
  std::cerr << "        dropping caches: " << opts.drop_caches_cmd << "\n";
  const int rc = std::system(opts.drop_caches_cmd.c_str());
  if (rc != 0)
    std::cerr << "        drop-caches command returned " << rc << "\n";
}

size_t dataset_blocks(size_t block_size) {
  return std::min(kMaxBlocks, kDatasetBudget / block_size);
}

// A document must fit one stripe's DATA AREA, and keys hash-route to stripes,
// so a stripe also has to absorb the hash-bin variance of the dataset without
// wrapping (a wrap is an eviction, and phases 1-3 must not evict).  The auto
// geometry gives 16 even-tiled ~512 MiB stripes on an 8 GiB volume, which
// holds >= 64 blocks per stripe up to 8 MiB blocks.  For 32 MiB blocks that
// drops to 16 per stripe against an expected occupancy of 8 -- too close -- so
// the stripe size is pinned to 2 GiB (3 stripes, ~43 expected vs 64 capacity).
size_t choose_stripe_size(size_t block_size) {
  if (block_size <= size_t{8} * 1024 * 1024) {
    return 0;  // auto: 16 stripes of ~512 MiB
  }
  return size_t{2} * 1024 * 1024 * 1024;
}

std::string volume_path(const Options &opts) {
  return (std::filesystem::path(opts.path) /
          (std::string(kVolumeStem) + ".dat"))
      .string();
}

// Remove the configured volume file AND any structural-fingerprint sibling
// ("<stem>-<major>-<geohash>.dat") so every block size starts in a fresh
// store.
void remove_volume_files(const Options &opts) {
  std::error_code ec;
  const std::filesystem::path dir(opts.path);
  for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
    if (ec) break;
    if (!entry.is_regular_file(ec)) continue;
    const std::string name = entry.path().filename().string();
    if (name.rfind(kVolumeStem, 0) == 0) {
      std::filesystem::remove(entry.path(), ec);
    }
  }
}

CacheConfig make_config(const Options &opts) {
  CacheConfig config;
  config.ram_cache_size = 0;  // CLFUS is pointless for multi-MB blocks
  config.max_object_size = 0;
  config.enable_checksum = true;
  config.verify_checksum_on_read = !opts.no_verify;
  config.enable_hit_tracking = false;
  config.optimization_config.enabled = false;
  config.directory_sync_interval = std::chrono::milliseconds{0};
  if (!opts.no_mmap_dir) {
    config.set_multi_process(0, 1);
  }
  return config;
}

std::unique_ptr<Cache> open_store(const Options &opts, size_t block_size) {
  auto cache_result = Cache::create(make_config(opts));
  if (!cache_result.has_value()) {
    std::cerr << "Cache::create failed: "
              << cache_error_category().message(
                     static_cast<int>(cache_result.error()))
              << "\n";
    return nullptr;
  }
  auto cache = std::move(*cache_result);

  VolumeConfig vol;
  vol.path = volume_path(opts);
  vol.size = kCapacityBytes;
  vol.stripe_size = choose_stripe_size(block_size);
  vol.sync_on_write = false;
  vol.max_object_size = 0;
  auto added = cache->add_volume(vol);
  if (!added.has_value()) {
    std::cerr << "add_volume failed: "
              << cache_error_category().message(static_cast<int>(added.error()))
              << "\n";
    return nullptr;
  }
  auto started = cache->start();
  if (!started.has_value()) {
    std::cerr << "Cache::start failed: "
              << cache_error_category().message(
                     static_cast<int>(started.error()))
              << "\n";
    return nullptr;
  }
  return cache;
}

// ---------------------------------------------------------------------------
// Read work
// ---------------------------------------------------------------------------
uint64_t touch_pages(std::span<const std::byte> data) {
  uint64_t sink = 0;
  for (size_t off = 0; off < data.size(); off += kPageSize) {
    sink += static_cast<uint64_t>(std::to_integer<uint8_t>(data[off]));
  }
  return sink;
}

uint64_t copy_out(std::span<const std::byte> data,
                  std::vector<std::byte> &buffer) {
  const size_t len = std::min(data.size(), buffer.size());
  std::memcpy(buffer.data(), data.data(), len);
  return len == 0 ? 0
                  : static_cast<uint64_t>(std::to_integer<uint8_t>(buffer[0]));
}

struct RunResult {
  uint64_t ops = 0;
  uint64_t misses = 0;
  uint64_t bytes = 0;
  double seconds = 0.0;
  std::vector<double> latencies_us;
};

// One Zipf reader: `warmup_s` seconds unrecorded, then `measure_s` seconds
// measured.  Shared by phase 3 and by the phase-5 child processes.
RunResult run_zipf_reader(Cache &cache, const std::vector<CacheKey> &keys,
                          Mode mode, size_t block_size, uint32_t thread_id,
                          double warmup_s, double measure_s,
                          bool record_latency) {
  using clock = std::chrono::steady_clock;
  RunResult result;
  ZipfGenerator zipf(keys.size(), kZipfTheta, 42 + thread_id);
  std::vector<std::byte> copy_buffer;
  if (mode == Mode::kCopy) {
    copy_buffer.assign(block_size, std::byte{0});
  }
  uint64_t sink = 0;
  if (record_latency) result.latencies_us.reserve(size_t{1} << 20);

  auto one_op = [&](bool record) {
    const CacheKey &key = keys[zipf.next()];
    const auto op_start = clock::now();
    auto rh = cache.read_sync(key);
    if (rh.has_value()) {
      const auto content = rh->content();
      sink += (mode == Mode::kView) ? touch_pages(content)
                                    : copy_out(content, copy_buffer);
      result.bytes += content.size();
      ++result.ops;
      rh->close();  // never hold a disk-hit borrow longer than the read
      const auto op_end = clock::now();
      if (record) {
        result.latencies_us.push_back(
            std::chrono::duration<double, std::micro>(op_end - op_start)
                .count());
      }
    } else {
      ++result.misses;
    }
  };

  const auto warm_end =
      clock::now() + std::chrono::duration_cast<clock::duration>(
                         std::chrono::duration<double>(warmup_s));
  while (clock::now() < warm_end) {
    one_op(false);
  }
  const auto start = clock::now();
  const auto measure_end =
      start + std::chrono::duration_cast<clock::duration>(
                  std::chrono::duration<double>(measure_s));
  result.ops = 0;
  result.misses = 0;
  result.bytes = 0;
  while (clock::now() < measure_end) {
    one_op(record_latency);
  }
  result.seconds = std::chrono::duration<double>(clock::now() - start).count();
  g_sink.fetch_add(sink, std::memory_order_relaxed);
  return result;
}

// ---------------------------------------------------------------------------
// Phases
// ---------------------------------------------------------------------------
struct Dataset {
  std::vector<CacheKey> keys;
  std::vector<std::array<std::byte, kMetaSize>> metadata;
};

// Key = SHA-256("prefix-<i>").  CacheKey's string constructor IS that SHA-256,
// so the digest it produces is byte-identical to what a peer harness hashes;
// it is routed through from_digest() to make the contract explicit.
Dataset build_dataset(size_t n) {
  Dataset ds;
  ds.keys.reserve(n);
  ds.metadata.resize(n);
  for (size_t i = 0; i < n; ++i) {
    const CacheKey hashed("prefix-" + std::to_string(i));
    const auto digest = hashed.digest();
    ds.keys.push_back(CacheKey::from_digest(digest));
    std::memcpy(ds.metadata[i].data(), digest.data(), CacheKey::kDigestSize);
    std::memcpy(ds.metadata[i].data() + CacheKey::kDigestSize, digest.data(),
                CacheKey::kDigestSize);
  }
  return ds;
}

// Reference vectors shared with the peer harness (its README carries the
// same table), so two implementations of the spec can prove they generate
// the identical workload: keys, block bytes and the Zipf access sequence.
void print_reference_vectors() {
  const Dataset ds = build_dataset(3);
  for (size_t i = 0; i < 3; ++i) {
    std::cout << "key prefix-" << i << " = " << ds.keys[i].to_hex() << "\n";
  }
  std::array<std::byte, 16> head{};
  for (size_t i = 0; i < 3; ++i) {
    fill_block(head, i);
    std::cout << "value[" << i << "][0..15] =";
    for (auto b : head) {
      std::cout << ' ' << std::hex << std::setw(2) << std::setfill('0')
                << std::to_integer<unsigned>(b);
    }
    std::cout << std::dec << "\n";
  }
  for (uint32_t seed : {42U, 43U}) {
    ZipfGenerator zipf(2048, kZipfTheta, seed);
    std::cout << "zipf(n=2048, seed " << seed << ") =";
    for (int i = 0; i < 10; ++i) std::cout << ' ' << zipf.next();
    std::cout << "\n";
  }
}

std::optional<Record> run_put(Cache &cache, const Dataset &ds,
                              size_t block_size) {
  using clock = std::chrono::steady_clock;
  const size_t n = ds.keys.size();
  std::vector<std::byte> block(block_size);
  std::vector<double> latencies;
  latencies.reserve(n);
  uint64_t ok = 0;
  uint64_t bytes = 0;

  const auto start = clock::now();
  for (size_t i = 0; i < n; ++i) {
    fill_block(block, i);  // generation is harness overhead: not timed
    const auto op_start = clock::now();
    auto wh = cache.write_sync(ds.keys[i], block_size);
    if (wh.has_value()) {
      wh->set_header(std::span<const std::byte>(ds.metadata[i]));
      auto written = wh->write_sync(std::span<const std::byte>(block));
      auto closed = wh->close_sync();
      if (written.has_value() && closed.has_value()) {
        ++ok;
        bytes += block_size;
      } else {
        std::cerr << "  put " << i << " failed\n";
      }
    } else {
      std::cerr << "  write_sync " << i << " failed: "
                << cache_error_category().message(static_cast<int>(wh.error()))
                << "\n";
    }
    latencies.push_back(
        std::chrono::duration<double, std::micro>(clock::now() - op_start)
            .count());
  }
  const double secs =
      std::chrono::duration<double>(clock::now() - start).count();

  Record r;
  r.phase = "put";
  r.block_size = block_size;
  r.n = n;
  r.threads = 1;
  r.ops_per_s = static_cast<double>(ok) / secs;
  r.gb_per_s = gb_per_s(bytes, secs);
  r.has_latency = true;
  r.latency = compute_percentiles(latencies);
  if (ok != n) {
    std::cerr << "  ERROR: only " << ok << " of " << n
              << " puts succeeded; results for this size are invalid\n";
    return std::nullopt;
  }
  return r;
}

// Sequential single-threaded view-mode read of every key, used by both the
// first-touch phase and the restart phase.
// Untimed self-check: the first `count` blocks round-trip byte-exactly
// (header and first value word), so a broken generator or a wrap is caught
// before any timed phase runs.  Mirrors the peer harness's content check.
bool verify_blocks(Cache &cache, const Dataset &ds, size_t block_size,
                   size_t count) {
  for (size_t i = 0; i < std::min(count, ds.keys.size()); ++i) {
    auto rh = cache.read_sync(ds.keys[i]);
    if (!rh.has_value()) {
      std::cerr << "  content verify: block " << i << " missing\n";
      return false;
    }
    const auto content = rh->content();
    const auto header = rh->header();
    uint64_t word = 0;
    if (content.size() == block_size) {
      std::memcpy(&word, content.data(), sizeof(word));
    }
    const bool ok =
        content.size() == block_size && word == first_word_of_block(i) &&
        header.size() == kMetaSize &&
        std::memcmp(header.data(), ds.metadata[i].data(), kMetaSize) == 0;
    rh->close();
    if (!ok) {
      std::cerr << "  content verify: block " << i << " MISMATCH\n";
      return false;
    }
  }
  std::cerr << "  content verify: ok\n";
  return true;
}

Record run_sequential_get(Cache &cache, const Dataset &ds, size_t block_size,
                          const std::string &phase, bool verify_content) {
  using clock = std::chrono::steady_clock;
  const size_t n = ds.keys.size();
  std::vector<double> latencies;
  latencies.reserve(n);
  uint64_t hits = 0;
  uint64_t bytes = 0;
  uint64_t bad = 0;
  uint64_t sink = 0;

  const auto start = clock::now();
  for (size_t i = 0; i < n; ++i) {
    const auto op_start = clock::now();
    auto rh = cache.read_sync(ds.keys[i]);
    if (rh.has_value()) {
      const auto content = rh->content();
      sink += touch_pages(content);
      if (verify_content) {
        uint64_t word = 0;
        if (content.size() != block_size) {
          ++bad;
        } else {
          std::memcpy(&word, content.data(), sizeof(word));
          if (word != first_word_of_block(i)) ++bad;
          const auto header = rh->header();
          if (header.size() != kMetaSize ||
              std::memcmp(header.data(), ds.metadata[i].data(), kMetaSize) !=
                  0) {
            ++bad;
          }
        }
      }
      bytes += content.size();
      ++hits;
      rh->close();
    }
    latencies.push_back(
        std::chrono::duration<double, std::micro>(clock::now() - op_start)
            .count());
  }
  const double secs =
      std::chrono::duration<double>(clock::now() - start).count();
  g_sink.fetch_add(sink, std::memory_order_relaxed);
  if (bad > 0) {
    std::cerr << "  WARNING: " << bad << " content/metadata mismatches\n";
  }

  Record r;
  r.phase = phase;
  r.mode = "view";
  r.block_size = block_size;
  r.n = n;
  r.threads = 1;
  r.ops_per_s = static_cast<double>(hits) / secs;
  r.gb_per_s = gb_per_s(bytes, secs);
  r.has_latency = true;
  r.latency = compute_percentiles(latencies);
  r.hit_fraction = static_cast<double>(hits) / static_cast<double>(n);
  return r;
}

Record run_get_warm(Cache &cache, const Dataset &ds, size_t block_size,
                    uint32_t threads, Mode mode, double seconds) {
  std::vector<RunResult> results(threads);
  std::vector<std::thread> workers;
  workers.reserve(threads);
  for (uint32_t t = 0; t < threads; ++t) {
    workers.emplace_back([&, t]() {
      results[t] =
          run_zipf_reader(cache, ds.keys, mode, block_size, t,
                          std::min(kWarmupSeconds, seconds), seconds, true);
    });
  }
  for (auto &w : workers) w.join();

  uint64_t ops = 0;
  uint64_t bytes = 0;
  uint64_t misses = 0;
  double span = 0.0;
  std::vector<double> latencies;
  for (auto &res : results) {
    ops += res.ops;
    bytes += res.bytes;
    misses += res.misses;
    span = std::max(span, res.seconds);
    latencies.insert(latencies.end(), res.latencies_us.begin(),
                     res.latencies_us.end());
  }
  if (misses > 0) {
    std::cerr << "  WARNING: " << misses << " misses during get_warm\n";
  }

  Record r;
  r.phase = "get_warm";
  r.mode = mode_name(mode);
  r.block_size = block_size;
  r.n = ds.keys.size();
  r.threads = threads;
  r.ops_per_s = static_cast<double>(ops) / span;
  r.gb_per_s = gb_per_s(bytes, span);
  r.has_latency = true;
  r.latency = compute_percentiles(latencies);
  return r;
}

#ifndef _WIN32
struct ChildReport {
  uint64_t ops;
  uint64_t bytes;
  uint64_t misses;
  double seconds;
};

// Phase 5.  See the file header for why the children open their own Cache on
// the same volume with the parent's configuration.
bool run_multiprocess(const Options &opts, const Dataset &ds, size_t block_size,
                      Mode mode, double seconds, Record *out) {
  int fds[2] = {-1, -1};
  if (::pipe(fds) != 0) {
    std::cerr << "  pipe() failed; skipping the multi-process phase\n";
    return false;
  }

  std::vector<pid_t> children;
  children.reserve(kReaderProcesses);
  for (uint32_t c = 0; c < kReaderProcesses; ++c) {
    const pid_t pid = ::fork();
    if (pid < 0) {
      std::cerr << "  fork() failed; skipping the multi-process phase\n";
      break;
    }
    if (pid == 0) {
      // ---- CHILD.  No exceptions may escape, and no destructor of the
      // inherited parent state may ever run: always leave via _exit().
      int rc = 1;
      try {
        ::close(fds[0]);
        auto cache = open_store(opts, block_size);
        if (cache) {
          RunResult res = run_zipf_reader(*cache, ds.keys, mode, block_size, c,
                                          std::min(kWarmupSeconds, seconds),
                                          seconds, false);
          ChildReport rep{res.ops, res.bytes, res.misses, res.seconds};
          rc = (::write(fds[1], &rep, sizeof(rep)) == sizeof(rep)) ? 0 : 2;
          cache->stop();
        } else {
          rc = 3;
        }
      } catch (...) {
        rc = 4;
      }
      ::_exit(rc);
    }
    children.push_back(pid);
  }
  ::close(fds[1]);

  uint64_t ops = 0;
  uint64_t bytes = 0;
  uint64_t misses = 0;
  double elapsed_sum = 0.0;
  uint32_t reported = 0;
  ChildReport rep{};
  while (::read(fds[0], &rep, sizeof(rep)) == sizeof(rep)) {
    ops += rep.ops;
    bytes += rep.bytes;
    misses += rep.misses;
    elapsed_sum += rep.seconds;
    ++reported;
  }
  ::close(fds[0]);
  for (pid_t pid : children) {
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      std::cerr << "  child " << pid << " exited abnormally (status " << status
                << ")\n";
    }
  }
  if (reported == 0 || elapsed_sum <= 0.0) {
    std::cerr << "  no child reported; skipping the multi-process phase\n";
    return false;
  }
  // Aggregate = sum(ops) / mean(child elapsed), matching the peer harness.
  const double span = elapsed_sum / reported;
  if (misses > 0) {
    std::cerr << "  WARNING: " << misses << " misses across reader processes\n";
  }

  out->phase = "multiprocess_read";
  out->mode = mode_name(mode);
  out->block_size = block_size;
  out->n = ds.keys.size();
  out->threads = reported;  // process count, so reports key on it
  out->processes = reported;
  out->ops_per_s = static_cast<double>(ops) / span;
  out->gb_per_s = gb_per_s(bytes, span);
  return true;
}
#endif  // !_WIN32

// ---------------------------------------------------------------------------
// Driver
// ---------------------------------------------------------------------------
void print_usage(const char *argv0) {
  std::cout
      << "Usage: " << argv0 << " [options]\n"
      << "  --block-size BYTES    Block size (repeatable; default 524288,\n"
      << "                        2097152, 8388608, 33554432)\n"
      << "  --seconds N           Seconds per get_warm point (default 10)\n"
      << "  --threads LIST        Comma-separated thread counts "
         "(default 1,4,8)\n"
      << "  --no-verify           verify_checksum_on_read = false (only\n"
      << "                        effective with --no-mmap-dir)\n"
      << "  --no-mmap-dir         Use the in-memory directory (no\n"
      << "                        multi-process mode; restart loses the "
         "index)\n"
      << "  --skip-multiprocess   Skip phase 5\n"
      << "  --print-vectors       Print cross-harness reference vectors "
         "and exit\n"
      << "  --output FILE         Write the JSON lines to FILE\n"
      << "  --path DIR            Directory for the volume file\n"
      << "  --drop-caches-cmd CMD Shell command run before phases 2 and 4\n"
      << "                        (e.g. \"sync; echo 3 > /proc/sys/vm/"
         "drop_caches\")\n"
      << "  --help, -h            Show this help\n";
}

std::vector<uint32_t> parse_thread_list(const std::string &spec) {
  std::vector<uint32_t> out;
  std::stringstream ss(spec);
  std::string item;
  while (std::getline(ss, item, ',')) {
    if (item.empty()) continue;
    out.push_back(static_cast<uint32_t>(std::stoul(item)));
  }
  return out;
}

}  // namespace

int main(int argc, char *argv[]) {
  Options opts;
  opts.path = std::filesystem::temp_directory_path().string();

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--block-size" && i + 1 < argc) {
      opts.block_sizes.push_back(std::stoull(argv[++i]));
    } else if (arg == "--seconds" && i + 1 < argc) {
      opts.seconds = std::stod(argv[++i]);
    } else if (arg == "--threads" && i + 1 < argc) {
      opts.threads = parse_thread_list(argv[++i]);
    } else if (arg == "--no-verify") {
      opts.no_verify = true;
    } else if (arg == "--no-mmap-dir") {
      opts.no_mmap_dir = true;
    } else if (arg == "--skip-multiprocess") {
      opts.skip_multiprocess = true;
    } else if (arg == "--print-vectors") {
      print_reference_vectors();
      return 0;
    } else if (arg == "--output" && i + 1 < argc) {
      opts.output = argv[++i];
    } else if (arg == "--path" && i + 1 < argc) {
      opts.path = argv[++i];
    } else if (arg == "--drop-caches-cmd" && i + 1 < argc) {
      opts.drop_caches_cmd = argv[++i];
    } else if (arg == "--help" || arg == "-h") {
      print_usage(argv[0]);
      return 0;
    } else {
      std::cerr << "Unknown or incomplete option: " << arg << "\n";
      print_usage(argv[0]);
      return 1;
    }
  }
  if (opts.block_sizes.empty()) {
    opts.block_sizes = {524288, 2097152, 8388608, 33554432};
  }
  if (opts.threads.empty()) {
    opts.threads = {1, 4, 8};
  }

  std::error_code ec;
  std::filesystem::create_directories(opts.path, ec);

  std::ofstream json_file;
  std::ostream *json_out = &std::cout;
  if (!opts.output.empty()) {
    json_file.open(opts.output);
    if (!json_file.is_open()) {
      std::cerr << "Failed to open output file: " << opts.output << "\n";
      return 1;
    }
    json_out = &json_file;
  }

  // Effective settings: multi-process mode FORCES read-side CRC verification
  // (Cache::add_volume_locked), so report what the store actually does.
  const bool mmap_dir = !opts.no_mmap_dir;
  const bool verify = mmap_dir ? true : !opts.no_verify;

  const MachineInfo machine = collect_machine_info(opts.path);
  std::cerr << "Cyclone KV-cache storage-tier benchmark (workload spec v1)\n"
            << "  cpu          : " << machine.cpu << "\n"
            << "  cores        : " << machine.cores << " (hw_concurrency "
            << std::thread::hardware_concurrency() << ")\n"
            << "  os           : " << machine.os << "\n"
            << "  filesystem   : " << machine.filesystem << "\n"
            << "  store        : " << kStoreName << " " << kStoreVersion
            << " @ " << machine.commit << "\n"
            << "  volume       : " << volume_path(opts) << " ("
            << (kCapacityBytes / (1024 * 1024)) << " MiB)\n"
            << "\nCyclone tuning (spec allows store-specific tuning if "
               "printed):\n"
            << "  max_object_size         = 0 (unbounded; blocks reach 32 "
               "MiB)\n"
            << "  ram_cache_size          = 0 (the CLFUS RAM tier is "
               "irrelevant for\n"
            << "                            multi-MB blobs; disk hits are "
               "zero-copy\n"
            << "                            mmap spans served from the OS "
               "page cache)\n"
            << "  enable_checksum         = true (mandatory in multi-process "
               "mode)\n"
            << "  verify_checksum_on_read = " << (verify ? "true" : "false");
  if (opts.no_verify && mmap_dir) {
    std::cerr << "  [--no-verify IGNORED: the mmap directory forces it on]";
  }
  std::cerr << "\n"
            << "  mmap directory          = " << (mmap_dir ? "on" : "off")
            << (mmap_dir ? " (multi-process 0 of 1)" : " (in-memory index)")
            << "\n"
            << "  hit tracking            = off\n"
            << "  optimization engine     = off\n"
            << "  directory_sync_interval = 0 (no periodic fsync; the restart\n"
            << "                            phase is a process restart, not a "
               "power-loss test)\n"
            << "  sync_on_write           = false (no per-put fsync)\n"
            << "  metadata                = 64-byte set_header() slot (a "
               "dedicated\n"
            << "                            header, not a value prefix)\n"
            << "  seconds/point           = " << opts.seconds << " (after a "
            << kWarmupSeconds << "s warm-up)\n\n";

  *json_out << "{\"store\":\"" << kStoreName
            << "\",\"phase\":\"machine\",\"cpu\":\"" << machine.cpu
            << "\",\"cores\":\"" << machine.cores << "\",\"os\":\""
            << machine.os << "\",\"filesystem\":\"" << machine.filesystem
            << "\",\"version\":\"" << kStoreVersion << "\",\"commit\":\""
            << machine.commit << "\"}\n";
  json_out->flush();

  std::vector<Record> records;
  int exit_code = 0;

  for (size_t block_size : opts.block_sizes) {
    const size_t n = dataset_blocks(block_size);
    if (n == 0) {
      std::cerr << "block size " << block_size << " exceeds the dataset "
                << "budget; skipping\n";
      continue;
    }
    std::cerr << "=== block_size " << block_size << " (" << (block_size / 1024)
              << " KiB), n = " << n << ", dataset "
              << (static_cast<double>(n) * static_cast<double>(block_size) /
                  (1024.0 * 1024.0 * 1024.0))
              << " GiB, stripe_size = ";
    const size_t stripe = choose_stripe_size(block_size);
    if (stripe == 0) {
      std::cerr << "auto";
    } else {
      std::cerr << (stripe / (1024 * 1024)) << " MiB";
    }
    std::cerr << " ===\n";

    remove_volume_files(opts);
    const Dataset ds = build_dataset(n);

    auto cache = open_store(opts, block_size);
    if (!cache) {
      exit_code = 1;
      break;
    }

    auto record = [&](Record r) {
      r.verify_checksum = verify;
      r.mmap_directory = mmap_dir;
      emit_json(*json_out, r);
      records.push_back(std::move(r));
    };

    std::cerr << "  [1/5] put\n";
    auto put = run_put(*cache, ds, block_size);
    if (!put) {
      exit_code = 1;
      break;
    }
    record(*put);
    if (!verify_blocks(*cache, ds, block_size, 2)) {  // untimed self-check
      exit_code = 1;
      break;
    }

    std::cerr << "  [2/5] get_first_touch\n";
    drop_caches(opts);
    record(
        run_sequential_get(*cache, ds, block_size, "get_first_touch", false));

    std::cerr << "  [3/5] get_warm (Zipf theta=" << kZipfTheta << ")\n";
    for (Mode mode : {Mode::kView, Mode::kCopy}) {
      for (uint32_t threads : opts.threads) {
        std::cerr << "        mode=" << mode_name(mode)
                  << " threads=" << threads << "\n";
        record(
            run_get_warm(*cache, ds, block_size, threads, mode, opts.seconds));
      }
    }

    std::cerr << "  [4/5] restart\n";
    cache->stop();
    cache.reset();
    drop_caches(opts);
    cache = open_store(opts, block_size);
    if (!cache) {
      exit_code = 1;
      break;
    }
    record(run_sequential_get(*cache, ds, block_size, "restart", false));

    if (opts.skip_multiprocess) {
      std::cerr << "  [5/5] multiprocess_read: skipped "
                   "(--skip-multiprocess)\n";
    } else if (!mmap_dir) {
      std::cerr << "  [5/5] multiprocess_read: skipped (needs the mmap "
                   "directory;\n"
                   "        the in-memory index is not shared between "
                   "processes)\n";
    } else {
#ifdef _WIN32
      std::cerr << "  [5/5] multiprocess_read: skipped (fork() is POSIX "
                   "only)\n";
#else
      std::cerr << "  [5/5] multiprocess_read (" << kReaderProcesses
                << " processes)\n";
      for (Mode mode : {Mode::kView, Mode::kCopy}) {
        std::cerr << "        mode=" << mode_name(mode) << "\n";
        Record mp;
        if (run_multiprocess(opts, ds, block_size, mode, opts.seconds, &mp)) {
          record(std::move(mp));
        }
      }
#endif
    }

    cache->stop();
    cache.reset();
    remove_volume_files(opts);
  }

  print_markdown(std::cout, records);
  std::cerr << "\n(sink " << g_sink.load(std::memory_order_relaxed) << ")\n";
  if (!opts.output.empty()) {
    std::cerr << "JSON lines written to " << opts.output << "\n";
  }
  return exit_code;
}
