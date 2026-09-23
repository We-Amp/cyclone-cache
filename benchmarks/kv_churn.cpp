// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Cyclone as a BOUNDED-capacity LLM KV-cache tier under churn.
//
// Implements doc/kv-cache-benchmark/kv-churn-spec.md (churn v1) exactly: a
// get-or-insert loop (hit -> memcpy the block into a per-thread staging
// buffer; miss -> generate the block and put it) over a key universe three
// times the tier's capacity, Zipf(0.99) with or without a 10 % stream of
// never-seen keys, until the tier has been filled twice over, then a timed
// steady state and a reopen check.  One invocation = one (pattern, threads)
// point in a fresh store; the sweep script runs each under a 4 GiB memory
// cgroup so the tier is larger than the page cache.
//
// Eviction is Cyclone's own FIFO-by-wrap: the harness keeps no index.  The
// peer harness (LMDB, file-per-block) runs the same streams and has to bring
// its own LRU; `--print-vectors` prints the stream heads both must agree on.
//
// Cyclone tuning (printed at startup):
//   volume size        the usable data area (stripes minus their mmap
//                      directories) is sized to ~= --capacity; the exact
//                      number is printed.
//   mmap directory ON  persistent index: the reopen check is meaningful.
//   checksum ON        and verified on read (mmap-directory mode forces it).
//   max_object_size=0, ram_cache_size=0, sync_on_write=false, hit tracking /
//   optimization / periodic directory sync off -- as in kv_bench.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
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
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "core/mmap_directory.hpp"
#include "cyclone/cache.hpp"
#include "cyclone/config.hpp"
#include "cyclone/error.hpp"
#include "cyclone/key.hpp"
#include "kv_workload.hpp"

#ifndef _WIN32
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif
#ifdef __linux__
#include <sys/sysmacros.h>
#endif

using namespace cyclone;
using namespace cyclone::kv_workload;

namespace {

using Clock = std::chrono::steady_clock;

constexpr const char* kStoreName = "cyclone";
constexpr const char* kVolumeStem = "cyclone_kv_churn";
constexpr size_t kVerifyEvery = 64;
constexpr size_t kReopenGets = 10000;
// Stream heads printed for the cross-harness check (the spec asks for 5;
// 12 also shows the first zipf+scan scan keys).
constexpr int kVectorLength = 12;
// Directory buckets per stripe: kDirectoryEntriesPerSegment in
// src/core/volume.cpp (not exported).  Only used to PRINT and size the data
// area; the store itself never reads this.
constexpr size_t kDirBucketsPerStripe = 16 * 1024;
constexpr size_t kPage = 4096;
// VolumeHeader::kSize (src/core/volume.hpp).
constexpr size_t kVolumeHeaderSize = 64;

double seconds_since(Clock::time_point t0) {
  return std::chrono::duration<double>(Clock::now() - t0).count();
}

double micros(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double, std::micro>(b - a).count();
}

struct Options {
  size_t block_size = size_t{2} * 1024 * 1024;
  size_t capacity = size_t{16} * 1024 * 1024 * 1024;
  std::string pattern = "zipf";
  uint32_t threads = 1;
  double seconds = 120.0;
  double fill_factor = 2.0;
  double max_warmup_seconds = 3600.0;
  std::string path;
  std::string output;
  std::string drop_caches_cmd;
  std::optional<bool> wrap_retention;  // unset = the library default
};

// ---------------------------------------------------------------------------
// Host instrumentation (Linux; zeros elsewhere)
// ---------------------------------------------------------------------------
std::string run_command(const std::string& cmd) {
#ifdef _WIN32
  (void)cmd;
  return "unknown";
#else
  std::string out;
  FILE* pipe = ::popen(cmd.c_str(), "r");
  if (pipe == nullptr) return "unknown";
  char buf[512];
  while (std::fgets(buf, sizeof(buf), pipe) != nullptr) out += buf;
  ::pclose(pipe);
  while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) {
    out.pop_back();
  }
  std::replace(out.begin(), out.end(), '\n', ' ');
  std::replace(out.begin(), out.end(), '"', '\'');
  return out.empty() ? "unknown" : out;
#endif
}

struct DeviceCounters {
  uint64_t read_bytes = 0;
  uint64_t write_bytes = 0;
  bool valid = false;
};

// Sectors read/written by the block device holding `dir`
// (/sys/dev/block/<maj>:<min>/stat; sectors are always 512 bytes there).
DeviceCounters device_counters(const std::string& dir) {
  DeviceCounters c;
#ifdef __linux__
  struct stat st{};
  if (::stat(dir.c_str(), &st) != 0) return c;
  std::ostringstream p;
  p << "/sys/dev/block/" << major(st.st_dev) << ":" << minor(st.st_dev)
    << "/stat";
  std::ifstream in(p.str());
  std::array<uint64_t, 11> f{};
  for (auto& v : f) {
    if (!(in >> v)) return c;
  }
  c.read_bytes = f[2] * 512;
  c.write_bytes = f[6] * 512;
  c.valid = true;
#else
  (void)dir;
#endif
  return c;
}

std::string device_name(const std::string& dir) {
#ifdef __linux__
  struct stat st{};
  if (::stat(dir.c_str(), &st) != 0) return "unknown";
  std::ostringstream p;
  p << "/sys/dev/block/" << major(st.st_dev) << ":" << minor(st.st_dev);
  std::error_code ec;
  const auto target = std::filesystem::read_symlink(p.str(), ec);
  return ec ? p.str() : target.filename().string();
#else
  (void)dir;
  return "unknown";
#endif
}

void sync_filesystem(const std::string& dir) {
#ifdef __linux__
  const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
  if (fd >= 0) {
    ::syncfs(fd);
    ::close(fd);
  }
#elif !defined(_WIN32)
  (void)dir;
  ::sync();
#else
  (void)dir;
#endif
}

// cgroup v2 directory of this process ("" when not on cgroup v2).
std::string cgroup_dir() {
#ifdef __linux__
  std::ifstream in("/proc/self/cgroup");
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("0::", 0) == 0) return "/sys/fs/cgroup" + line.substr(3);
  }
#endif
  return "";
}

uint64_t read_u64_file(const std::string& path) {
  std::ifstream in(path);
  uint64_t v = 0;
  in >> v;
  return v;
}

std::string read_word_file(const std::string& path) {
  std::ifstream in(path);
  std::string v;
  in >> v;
  return v.empty() ? "?" : v;
}

uint64_t memory_stat_field(const std::string& cg, const std::string& key) {
  std::ifstream in(cg + "/memory.stat");
  std::string k;
  uint64_t v = 0;
  while (in >> k >> v) {
    if (k == key) return v;
  }
  return 0;
}

// Samples memory.current every 200 ms; kernel 5.15 has no memory.peak.
class CgroupSampler {
 public:
  explicit CgroupSampler(std::string cg) : _cg(std::move(cg)) {
    if (_cg.empty()) return;
    _thread = std::thread([this] {
      while (!_stop.load(std::memory_order_relaxed)) {
        const uint64_t cur = read_u64_file(_cg + "/memory.current");
        uint64_t prev = _peak.load(std::memory_order_relaxed);
        while (cur > prev && !_peak.compare_exchange_weak(prev, cur)) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
      }
    });
  }
  ~CgroupSampler() { stop(); }
  void stop() {
    _stop.store(true);
    if (_thread.joinable()) _thread.join();
  }
  uint64_t peak() const { return _peak.load(); }

 private:
  std::string _cg;
  std::atomic<bool> _stop{false};
  std::atomic<uint64_t> _peak{0};
  std::thread _thread;
};

struct Footprint {
  uint64_t allocated = 0;
  uint64_t apparent = 0;
};

Footprint footprint(const std::string& dir, const std::string& stem) {
  Footprint fp;
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
    const std::string name = e.path().filename().string();
    if (name.rfind(stem, 0) != 0 || !e.is_regular_file(ec)) continue;
#ifndef _WIN32
    struct stat st{};
    if (::stat(e.path().c_str(), &st) == 0) {
      fp.allocated += static_cast<uint64_t>(st.st_blocks) * 512;
      fp.apparent += static_cast<uint64_t>(st.st_size);
    }
#endif
  }
  return fp;
}

uint64_t peak_rss_kib() {
#ifndef _WIN32
  struct rusage ru{};
  ::getrusage(RUSAGE_SELF, &ru);
#ifdef __APPLE__
  return static_cast<uint64_t>(ru.ru_maxrss) / 1024;  // bytes on Darwin
#else
  return static_cast<uint64_t>(ru.ru_maxrss);
#endif
#else
  return 0;
#endif
}

void drop_caches(const Options& opts) {
  if (opts.drop_caches_cmd.empty()) return;
  std::cerr << "  dropping caches: " << opts.drop_caches_cmd << "\n";
  const int rc = std::system(opts.drop_caches_cmd.c_str());
  if (rc != 0) std::cerr << "  drop-caches command returned " << rc << "\n";
}

// ---------------------------------------------------------------------------
// Store
// ---------------------------------------------------------------------------
size_t dir_bytes_per_stripe() {
  const size_t raw = MmapDirectory::required_size(kDirBucketsPerStripe);
  return (raw + kPage - 1) / kPage * kPage;
}

// Auto geometry: 16 even-tiled stripes once the volume is >= 512 MiB, each
// with a fixed-size mmap directory at its head.  Size the volume so the
// stripes' DATA areas sum to the requested capacity.
size_t volume_size_for(size_t capacity) {
  constexpr size_t kStripes = 16;
  const size_t raw =
      kVolumeHeaderSize + capacity + (kStripes * dir_bytes_per_stripe());
  return (raw + kPage - 1) / kPage * kPage;
}

void remove_volume_files(const Options& opts) {
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator(opts.path, ec)) {
    if (e.path().filename().string().rfind(kVolumeStem, 0) == 0) {
      std::filesystem::remove(e.path(), ec);
    }
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
  config.set_multi_process(0, 1);
  if (opts.wrap_retention) {
    config.wrap_retention = *opts.wrap_retention;
  }
  auto created = Cache::create(config);
  if (!created) {
    std::cerr << "Cache::create failed: "
              << cache_error_category().message(
                     static_cast<int>(created.error()))
              << "\n";
    return nullptr;
  }
  auto cache = std::move(*created);
  VolumeConfig vol;
  vol.path =
      (std::filesystem::path(opts.path) / (std::string(kVolumeStem) + ".dat"))
          .string();
  vol.size = volume_size_for(opts.capacity);
  vol.sync_on_write = false;
  vol.max_object_size = 0;
  auto added = cache->add_volume(vol);
  if (!added) {
    std::cerr << "add_volume failed: "
              << cache_error_category().message(static_cast<int>(added.error()))
              << "\n";
    return nullptr;
  }
  auto started = cache->start();
  if (!started) {
    std::cerr << "Cache::start failed\n";
    return nullptr;
  }
  return cache;
}

// ---------------------------------------------------------------------------
// Workload
// ---------------------------------------------------------------------------
struct ThreadStats {
  uint64_t hits = 0;
  uint64_t misses = 0;
  uint64_t inserts = 0;
  uint64_t put_failures = 0;
  uint64_t read_errors = 0;  // read_sync errors other than NotFound
  uint64_t verified = 0;
  double harness_s = 0.0;
  double elapsed_s = 0.0;
  std::vector<double> hit_us;
  std::vector<double> miss_us;
};

struct Shared {
  std::atomic<uint64_t> inserted_bytes{0};
  std::atomic<uint32_t> warm_done{0};
  std::atomic<bool> go{false};
  std::atomic<bool> failed{false};
};

std::array<std::byte, kMetaSize> meta_for(const CacheKey& key) {
  std::array<std::byte, kMetaSize> m{};
  std::memcpy(m.data(), key.digest().data(), CacheKey::kDigestSize);
  std::memcpy(m.data() + CacheKey::kDigestSize, key.digest().data(),
              CacheKey::kDigestSize);
  return m;
}

class Worker {
 public:
  Worker(Cache& cache, const Options& opts, const std::vector<uint64_t>& perm,
         uint32_t tid, bool scan, Shared& shared)
      : _cache(cache),
        _opts(opts),
        _stream(perm, tid, scan),
        _shared(shared),
        _copy(opts.block_size),
        _gen(opts.block_size) {}

  // One get-or-insert.  Returns false on a content mismatch (hard failure).
  bool step(bool record, bool insert_on_miss = true) {
    const uint64_t idx = _stream.next();
    const CacheKey key = churn_key(idx);  // workload prep: not timed
    const auto t0 = Clock::now();
    auto rh = _cache.read_sync(key);
    if (rh) {
      const auto content = rh->content();
      const bool size_ok = content.size() == _opts.block_size;
      copy_out(content, _copy);
      const bool check = (++_hit_seq % kVerifyEvery) == 0;
      std::array<std::byte, kMetaSize> header{};
      const auto h = rh->header();
      const bool header_size_ok = h.size() == kMetaSize;
      if (check && header_size_ok) std::memcpy(header.data(), h.data(), 64);
      rh->close();
      const auto t1 = Clock::now();
      if (!size_ok || !header_size_ok) {
        std::cerr << "FATAL: hit on index " << idx << " has size "
                  << content.size() << "\n";
        return false;
      }
      if (check) {
        const auto v0 = Clock::now();
        fill_block(_gen, idx);
        const auto meta = meta_for(key);
        const bool ok =
            std::memcmp(_gen.data(), _copy.data(), _opts.block_size) == 0 &&
            std::memcmp(header.data(), meta.data(), kMetaSize) == 0;
        _stats.harness_s += seconds_since(v0);
        ++_stats.verified;
        if (!ok) {
          std::cerr << "FATAL: content mismatch on index " << idx << "\n";
          return false;
        }
      }
      ++_stats.hits;
      if (record) _stats.hit_us.push_back(micros(t0, t1));
      return true;
    }
    const auto t1 = Clock::now();
    ++_stats.misses;
    if (rh.error() != CacheError::NotFound) ++_stats.read_errors;
    if (!insert_on_miss) return true;

    fill_block(_gen, idx);  // "compute" the block: harness work, not timed
    const auto meta = meta_for(key);
    const auto p0 = Clock::now();
    _stats.harness_s += micros(t1, p0) / 1e6;
    bool ok = false;
    auto wh = _cache.write_sync(key, _opts.block_size);
    if (wh) {
      wh->set_header(std::span<const std::byte>(meta));
      auto w = wh->write_sync(std::span<const std::byte>(_gen));
      auto c = wh->close_sync();
      ok = w.has_value() && c.has_value();
    }
    const auto p1 = Clock::now();
    if (ok) {
      ++_stats.inserts;
      _shared.inserted_bytes.fetch_add(_opts.block_size,
                                       std::memory_order_relaxed);
    } else {
      ++_stats.put_failures;
    }
    if (record) _stats.miss_us.push_back(micros(t0, t1) + micros(p0, p1));
    return true;
  }

  void run(uint64_t warm_target) {
    const auto warm_start = Clock::now();
    while (_shared.inserted_bytes.load(std::memory_order_relaxed) <
               warm_target &&
           !_shared.failed.load(std::memory_order_relaxed) &&
           seconds_since(warm_start) < _opts.max_warmup_seconds) {
      if (!step(false)) _shared.failed.store(true);
    }
    _shared.warm_done.fetch_add(1);
    while (!_shared.go.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    _stats = ThreadStats{};
    _stats.hit_us.reserve(size_t{1} << 20);
    _stats.miss_us.reserve(size_t{1} << 20);
    const auto start = Clock::now();
    const auto end = start + std::chrono::duration_cast<Clock::duration>(
                                 std::chrono::duration<double>(_opts.seconds));
    while (Clock::now() < end && !_shared.failed.load()) {
      if (!step(true)) _shared.failed.store(true);
    }
    _stats.elapsed_s = seconds_since(start);
  }

  ThreadStats& stats() { return _stats; }

 private:
  Cache& _cache;
  const Options& _opts;
  ChurnStream _stream;
  Shared& _shared;
  std::vector<std::byte> _copy;
  std::vector<std::byte> _gen;
  uint64_t _hit_seq = 0;
  ThreadStats _stats;
};

void print_vectors(const std::vector<uint64_t>& perm) {
  for (const bool scan : {false, true}) {
    for (uint32_t t = 0; t < 2; ++t) {
      ChurnStream s(perm, t, scan);
      std::cerr << "stream " << (scan ? "zipf+scan" : "zipf") << " t" << t
                << ":";
      for (int i = 0; i < kVectorLength; ++i) {
        const uint64_t idx = s.next();
        std::cerr << ' ' << idx << '/' << churn_key(idx).to_hex().substr(0, 8);
      }
      std::cerr << "\n";
    }
  }
}

void usage(const char* argv0) {
  std::cout << "Usage: " << argv0 << " [options]\n"
            << "  --block-size BYTES      default 2097152\n"
            << "  --capacity BYTES        tier payload capacity C "
               "(default 16 GiB)\n"
            << "  --pattern zipf|zipf+scan\n"
            << "  --threads N             default 1\n"
            << "  --seconds S             measured phase (default 120)\n"
            << "  --fill-factor F         warm-up inserts F x C (default 2)\n"
            << "  --max-warmup-seconds S  safety cap (default 3600)\n"
            << "  --path DIR              store directory\n"
            << "  --output FILE           append the JSON line to FILE\n"
            << "  --drop-caches-cmd CMD   run before the warm-up\n"
            << "  --wrap-retention on|off eviction mode (default: the "
               "library default)\n"
            << "  --print-vectors         print the stream heads and exit\n";
}

std::string fixed(double v, int prec) {
  std::ostringstream o;
  o << std::fixed << std::setprecision(prec) << v;
  return o.str();
}

}  // namespace

int main(int argc, char* argv[]) {
  Options opts;
  opts.path = std::filesystem::temp_directory_path().string();
  bool vectors_only = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const bool has = i + 1 < argc;
    if (a == "--block-size" && has) {
      opts.block_size = std::stoull(argv[++i]);
    } else if (a == "--capacity" && has) {
      opts.capacity = std::stoull(argv[++i]);
    } else if (a == "--pattern" && has) {
      opts.pattern = argv[++i];
    } else if (a == "--threads" && has) {
      opts.threads = static_cast<uint32_t>(std::stoul(argv[++i]));
    } else if (a == "--seconds" && has) {
      opts.seconds = std::stod(argv[++i]);
    } else if (a == "--fill-factor" && has) {
      opts.fill_factor = std::stod(argv[++i]);
    } else if (a == "--max-warmup-seconds" && has) {
      opts.max_warmup_seconds = std::stod(argv[++i]);
    } else if (a == "--path" && has) {
      opts.path = argv[++i];
    } else if (a == "--output" && has) {
      opts.output = argv[++i];
    } else if (a == "--drop-caches-cmd" && has) {
      opts.drop_caches_cmd = argv[++i];
    } else if (a == "--wrap-retention" && has) {
      opts.wrap_retention = std::string(argv[++i]) == "on";
    } else if (a == "--print-vectors") {
      vectors_only = true;
    } else {
      usage(argv[0]);
      return a == "--help" || a == "-h" ? 0 : 1;
    }
  }
  if (opts.pattern != "zipf" && opts.pattern != "zipf+scan") {
    std::cerr << "unknown pattern " << opts.pattern << "\n";
    return 1;
  }
  const bool scan = opts.pattern == "zipf+scan";
  const size_t universe = 3 * opts.capacity / opts.block_size;
  const auto perm = churn_permutation(universe);
  std::cerr << "universe U = " << universe << " keys\n";
  print_vectors(perm);
  if (vectors_only) return 0;

  std::error_code ec;
  std::filesystem::create_directories(opts.path, ec);
  remove_volume_files(opts);

  const std::string cg = cgroup_dir();
  std::cerr
      << "Cyclone KV churn (kv-churn-spec v1)\n"
      << "  cpu        : "
      << run_command(
#ifdef __APPLE__
             "sysctl -n machdep.cpu.brand_string"
#else
             "awk -F: '/model name/{print $2; exit}' /proc/cpuinfo"
#endif
             )
      << "\n  os         : " << run_command("uname -sr") << "\n  commit     : "
      << run_command(
             "echo ${KV_COMMIT:-$(git rev-parse --short HEAD 2>/dev/null)}")
      << "\n  device     : " << device_name(opts.path)
      << "\n  cgroup     : " << (cg.empty() ? "none" : cg)
      << " memory.max=" << read_word_file(cg + "/memory.max")
      << "\n  load       : " << run_command("uptime")
      << "\n  pattern    : " << opts.pattern << ", threads " << opts.threads
      << ", block " << opts.block_size << ", capacity " << opts.capacity
      << ", seconds " << opts.seconds << "\n";

  drop_caches(opts);
  CgroupSampler sampler(cg);
  auto cache = open_store(opts);
  if (!cache) return 1;
  const CacheStats geo = cache->stats();
  const uint64_t data_area =
      geo.stripe_bytes - geo.stripe_count * dir_bytes_per_stripe();
  std::cerr << "Cyclone tuning:\n"
            << "  volume size             = " << volume_size_for(opts.capacity)
            << " B, " << geo.stripe_count << " stripes, "
            << dir_bytes_per_stripe() << " B mmap directory each ("
            << kDirBucketsPerStripe * MmapDirectory::kEntriesPerBucket
            << " entries/stripe)\n"
            << "  usable data area        = " << data_area << " B ("
            << fixed(static_cast<double>(data_area) /
                         static_cast<double>(opts.capacity),
                     4)
            << " x C), ~"
            << data_area / geo.stripe_count / (opts.block_size + 256)
            << " blocks/stripe\n"
            << "  mmap directory ON, checksum ON + verify on read, readahead "
               "default, max_object_size 0, ram_cache_size 0, sync_on_write "
               "false, hit tracking/optimization/dir sync off\n"
            << "  eviction                = FIFO by stripe wrap (no app "
               "index), wrap retention "
            << ((opts.wrap_retention ? *opts.wrap_retention
                                     : kDefaultWrapRetention)
                    ? "ON"
                    : "OFF")
            << "\n";

  // Warm-up + measured phase.
  Shared shared;
  std::vector<std::unique_ptr<Worker>> workers;
  for (uint32_t t = 0; t < opts.threads; ++t) {
    workers.push_back(
        std::make_unique<Worker>(*cache, opts, perm, t, scan, shared));
  }
  const auto warm_target = static_cast<uint64_t>(
      opts.fill_factor * static_cast<double>(opts.capacity));
  const auto warm_start = Clock::now();
  std::vector<std::thread> threads;
  for (auto& w : workers) {
    threads.emplace_back([&w, warm_target] { w->run(warm_target); });
  }
  while (shared.warm_done.load() < opts.threads) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  const double warm_s = seconds_since(warm_start);
  const uint64_t warm_inserted = shared.inserted_bytes.load();
  std::cerr << "  warm-up: " << fixed(warm_s, 1) << " s, inserted "
            << warm_inserted << " B\n";
  const bool warm_capped = warm_inserted < warm_target;
  sync_filesystem(opts.path);
  const DeviceCounters dev0 = device_counters(opts.path);
  const CacheStats st0 = cache->stats();
  shared.go.store(true);
  for (auto& t : threads) t.join();
  sync_filesystem(opts.path);
  const DeviceCounters dev1 = device_counters(opts.path);
  const CacheStats st1 = cache->stats();

  ThreadStats total;
  double ops_rate = 0.0;
  double hit_rate = 0.0;
  double ins_rate = 0.0;
  double span = 0.0;
  for (auto& w : workers) {
    auto& s = w->stats();
    const double active = std::max(1e-9, s.elapsed_s - s.harness_s);
    ops_rate += static_cast<double>(s.hits + s.misses) / active;
    hit_rate += static_cast<double>(s.hits) / active;
    ins_rate += static_cast<double>(s.inserts) / active;
    span = std::max(span, s.elapsed_s);
    total.hits += s.hits;
    total.misses += s.misses;
    total.inserts += s.inserts;
    total.put_failures += s.put_failures;
    total.read_errors += s.read_errors;
    total.verified += s.verified;
    total.harness_s += s.harness_s;
    total.hit_us.insert(total.hit_us.end(), s.hit_us.begin(), s.hit_us.end());
    total.miss_us.insert(total.miss_us.end(), s.miss_us.begin(),
                         s.miss_us.end());
  }
  const Percentiles hit_p = compute_percentiles(total.hit_us);
  const Percentiles miss_p = compute_percentiles(total.miss_us);
  const double gets = static_cast<double>(total.hits + total.misses);
  const double bs = static_cast<double>(opts.block_size);
  const uint64_t inserted = total.inserts * opts.block_size;
  const double wa =
      inserted > 0 && dev0.valid
          ? static_cast<double>(dev1.write_bytes - dev0.write_bytes) /
                static_cast<double>(inserted)
          : 0.0;
  const Footprint fp = footprint(opts.path, kVolumeStem);
  const uint64_t mem_file_end = memory_stat_field(cg, "file");
  const uint64_t mem_anon_end = memory_stat_field(cg, "anon");

  // Reopen: close, reopen, first 10 000 gets of thread 0's zipf stream,
  // get only.
  cache->stop();
  cache.reset();
  const auto ro0 = Clock::now();
  cache = open_store(opts);
  if (!cache) return 1;
  const double reopen_open_s = seconds_since(ro0);
  Shared reopen_shared;
  Worker reopen(*cache, opts, perm, 0, false, reopen_shared);
  bool reopen_ok = true;
  for (size_t i = 0; i < kReopenGets && reopen_ok; ++i) {
    reopen_ok = reopen.step(false, false);
  }
  const double reopen_hit = static_cast<double>(reopen.stats().hits) /
                            static_cast<double>(kReopenGets);
  cache->stop();
  cache.reset();
  sampler.stop();

  const bool failed = shared.failed.load() || !reopen_ok;
  std::ostringstream j;
  j << std::fixed << "{\"store\":\"" << kStoreName
    << "\",\"bench\":\"churn\",\"pattern\":\"" << opts.pattern
    << "\",\"block_size\":" << opts.block_size
    << ",\"capacity\":" << opts.capacity << ",\"universe\":" << universe
    << ",\"threads\":" << opts.threads << std::setprecision(2)
    << ",\"seconds\":" << span << ",\"ops_per_s\":" << ops_rate
    << std::setprecision(4) << ",\"hit_ratio\":"
    << (gets > 0 ? static_cast<double>(total.hits) / gets : 0.0)
    << ",\"served_gb_per_s\":" << hit_rate * bs / 1e9
    << ",\"inserted_gb_per_s\":" << ins_rate * bs / 1e9 << std::setprecision(1)
    << ",\"hit_p50_us\":" << hit_p.p50 << ",\"hit_p99_us\":" << hit_p.p99
    << ",\"hit_p999_us\":" << hit_p.p999 << ",\"miss_p50_us\":" << miss_p.p50
    << ",\"miss_p99_us\":" << miss_p.p99 << ",\"miss_p999_us\":" << miss_p.p999
    << ",\"hits\":" << total.hits << ",\"misses\":" << total.misses
    << ",\"inserts\":" << total.inserts
    << ",\"put_failures\":" << total.put_failures
    << ",\"read_errors\":" << total.read_errors
    << ",\"verified\":" << total.verified << std::setprecision(3)
    << ",\"write_amp\":" << wa
    << ",\"device_write_bytes\":" << (dev1.write_bytes - dev0.write_bytes)
    << ",\"device_read_bytes\":" << (dev1.read_bytes - dev0.read_bytes)
    << ",\"footprint_bytes\":" << fp.allocated
    << ",\"apparent_bytes\":" << fp.apparent
    << ",\"peak_rss_kib\":" << peak_rss_kib()
    << ",\"cgroup_mem_peak\":" << sampler.peak()
    << ",\"cgroup_file_end\":" << mem_file_end
    << ",\"cgroup_anon_end\":" << mem_anon_end << ",\"warmup_s\":" << warm_s
    << ",\"warmup_inserted_bytes\":" << warm_inserted
    << ",\"warmup_capped\":" << (warm_capped ? "true" : "false")
    << ",\"reopen_hit_ratio\":" << reopen_hit
    << ",\"reopen_open_s\":" << reopen_open_s
    << ",\"data_area_bytes\":" << data_area
    << ",\"cy_evictions\":" << (st1.evictions - st0.evictions)
    << ",\"cy_wraps\":" << (st1.write_buffer_wraps - st0.write_buffer_wraps)
    << ",\"cy_writes_dropped_by_lease\":"
    << (st1.writes_dropped_by_lease - st0.writes_dropped_by_lease)
    << ",\"cy_wraps_deferred_by_lease\":"
    << (st1.wraps_deferred_by_lease - st0.wraps_deferred_by_lease)
    << ",\"cy_wrap_retention\":"
    << ((opts.wrap_retention ? *opts.wrap_retention : kDefaultWrapRetention)
            ? "true"
            : "false")
    << ",\"cy_frontier_advances\":"
    << (st1.frontier_advances - st0.frontier_advances)
    << ",\"cy_advances_deferred_by_lease\":"
    << (st1.advances_deferred_by_lease - st0.advances_deferred_by_lease)
    << ",\"cy_retained_hits\":" << (st1.retained_hits - st0.retained_hits)
    << ",\"cy_stamp_rejections\":"
    << (st1.stamp_rejections - st0.stamp_rejections)
    << ",\"cy_tag_collision_evictions\":" << st1.tag_collision_evictions
    << ",\"cy_entries\":" << st1.current_entries
    << ",\"cy_readahead_hints\":" << st1.readahead_hints_issued
    << ",\"failed\":" << (failed ? "true" : "false") << "}";
  std::cout << j.str() << "\n";
  if (!opts.output.empty()) {
    std::ofstream out(opts.output, std::ios::app);
    out << j.str() << "\n";
  }
  remove_volume_files(opts);
  if (failed) {
    std::cerr << "RUN FAILED (content mismatch)\n";
    return 2;
  }
  return 0;
}
