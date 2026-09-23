// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Does Cyclone's zero-copy read pay off for a GPU transfer?  (CUDA, discrete
// GPU over PCIe.)
//
// The CUDA counterpart of benchmarks/kv_gpu_metal.mm, asking the same
// question on the hardware the Metal run could not answer it on: a block that
// is ALREADY in the store -- written, read once, every page touched -- how
// fast does it reach device memory, and does sourcing the transfer from
// Cyclone's mapping beat staging it through a pinned buffer?  Unlike Apple
// silicon there IS a bus here, so this is a real host-to-device upload and
// the staged path really does pay a copy the mapping-sourced path does not.
//
// The paths, all moving the same bytes to the same device buffer:
//
//   pageable             cudaMemcpy(H2D) straight from content().  The naive
//                        path: the driver stages it through its own internal
//                        pinned buffers, synchronously.
//   zerocopy             cudaHostRegister the page-aligned range containing
//                        content(), cudaMemcpyAsync, sync, cudaHostUnregister
//                        -- for each block of the batch.  "Pin whatever span
//                        the read returned."  Batched, the spans are sorted
//                        and coalesced first: records are packed back to back
//                        so they overlap, and CUDA refuses an overlapping
//                        registration (see unit_zerocopy).
//   zerocopy-persistent  cudaHostRegister the store's WHOLE mapping ONCE, then
//                        per block just cudaMemcpyAsync from the borrowed
//                        pointer (which now lands inside a registered range,
//                        so the driver DMAs from the page cache directly).
//                        The decisive variant.
//   staged               memcpy content() into a cudaMallocHost buffer, then
//                        cudaMemcpyAsync.  The conventional path.
//   memcpy               the copy alone, no GPU at all -- kv_bench's `copy`
//                        access mode, as a reference cost.
//
// Plus the fixed-cost and ceiling references, none of which touch a store:
//   submit-only-0B       a 0-byte cudaMemcpyAsync + cudaStreamSynchronize.
//   h2d-4KiB             4 KiB from the pinned buffer.
//   pinned-ceiling       a full block from the pinned buffer: the PCIe H2D
//                        bandwidth ceiling this machine can reach at all.
//
// zerocopy, zerocopy-persistent and staged also run batched: kBatchBlocks
// cudaMemcpyAsync on one stream, one cudaStreamSynchronize, reported per-block
// amortized.  `pageable` cannot batch -- cudaMemcpy is synchronous by
// definition -- and `memcpy` has no submission to amortize, so both are batch
// 1 only.
//
// Stores: cyclone, lmdb (the other zero-copy store, optional at build time)
// and filedir-pread (the no-borrowed-buffer baseline: staged and memcpy only).
//
// The experiment isolates the TRANSFER, not first touch.  Every block is
// written, then read once and page-touched, before any timing starts -- CRC
// verification, directory faults and page faults are all paid up front.
// Blocks come from benchmarks/kv_workload.hpp, so they are bit-identical to
// kv_bench's and to the Metal experiment's: keys are SHA-256("prefix-<i>"),
// values are xorshift64* seeded with the block index, metadata is the 64-byte
// "digest twice" header.
//
// The load-bearing unknown is whether cudaHostRegister accepts a file-backed
// MAP_SHARED mapping at all.  If it refuses, `zerocopy` and
// `zerocopy-persistent` are simply not available to an embedder and the
// answer to the headline question is "no, stage it" -- so the probe block at
// startup reports the exact cudaError for every surface, with a private
// anonymous mapping as the control.
//
// Run it:
//   ./build/kv_gpu_cuda --block-size 2097152 --seconds 10

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cyclone/cache.hpp"
#include "cyclone/config.hpp"
#include "cyclone/error.hpp"
#include "cyclone/handle.hpp"
#include "cyclone/key.hpp"
#include "kv_gpu_cuda.h"
#include "kv_workload.hpp"

#ifdef CYCLONE_KV_GPU_HAVE_LMDB
#include <lmdb.h>
#endif

using cyclone::kv_workload::build_dataset;
using cyclone::kv_workload::compute_percentiles;
using cyclone::kv_workload::Dataset;
using cyclone::kv_workload::fill_block;
using cyclone::kv_workload::kMetaSize;
using cyclone::kv_workload::kPageSize;

namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t kBatchBlocks = 16;                 // blocks per stream batch
constexpr size_t kBlocks = 512;                     // N
constexpr size_t kVolumeBytes = size_t{8} << 30;    // 8 GiB
constexpr size_t kLmdbMapBytes = size_t{16} << 30;  // 16 GiB
constexpr double kWarmupSeconds = 2.0;
constexpr double kDeviceWarmupSeconds = 1.0;
constexpr size_t kRegisterWindow = size_t{1} << 30;  // 1 GiB fallback windows
constexpr const char *kVolumeStem = "kv_gpu_cuda_cyclone";

// The HOST page size, from getpagesize() at startup.  cudaHostRegister wants
// a page-aligned base, so this must never be hardcoded.  Distinct from the
// workload's kPageSize (4096), which is only the `view` touch stride.
size_t g_page_size = 4096;

double secs_since(Clock::time_point t) {
  return std::chrono::duration<double>(Clock::now() - t).count();
}

uint64_t fnv1a(const uint8_t *p, size_t n) {
  uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 1099511628211ULL;
  }
  return h;
}

size_t round_up(size_t v, size_t a) { return (v + a - 1) & ~(a - 1); }

uintptr_t page_base(uintptr_t p) {
  return p & ~static_cast<uintptr_t>(g_page_size - 1);
}

std::string cuda_err(int rc) { return cygpu_error_string(rc); }

// Why the last measured unit gave up.  A path that fails mid-run reports a
// bare "transfer failed" otherwise, and the whole point of the experiment is
// which CUDA call refuses what -- so the units record the exact
// cudaGetErrorString here and run_path() puts it in the row's note.
std::string g_unit_error;

// What cudaHostRegister was asked for, and what it made of it.
enum class RegOutcome { kDefault, kReadOnly, kAlreadyRegistered, kRefused };

// cudaHostRegisterDefault is refused ("invalid argument") on a PROT_READ file
// mapping -- which is exactly what LMDB's map is, and what a consumer mapping
// a Cyclone volume file for reading would create.  cudaHostRegisterReadOnly
// takes those.  So: try Default (what a read-write mapping wants), fall back
// to ReadOnly, and report an overlap with an existing registration as its own
// outcome, because the range is page-locked either way and refusing to
// measure it would be wrong.
RegOutcome host_register_any(const void *base, size_t len, std::string *err) {
  const int rc = cygpu_host_register(base, len, 0);
  if (rc == 0) return RegOutcome::kDefault;
  if (rc == cygpu_error_already_registered()) {
    return RegOutcome::kAlreadyRegistered;
  }
  const int rc_ro = cygpu_host_register(base, len, 1);
  if (rc_ro == 0) return RegOutcome::kReadOnly;
  if (rc_ro == cygpu_error_already_registered()) {
    return RegOutcome::kAlreadyRegistered;
  }
  if (err != nullptr) {
    *err = "RegisterDefault -> " + cuda_err(rc) + "; RegisterReadOnly -> " +
           cuda_err(rc_ro);
  }
  return RegOutcome::kRefused;
}

// ------------------------------------------------------------ machine info

// The value side of the first `key: value` (/proc) or `key=value`
// (/etc/os-release) line whose key contains `needle`.
std::string first_line_matching(const char *path, const char *needle) {
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    if (line.find(needle) != std::string::npos) {
      const auto sep = line.find_first_of(":=");
      if (sep == std::string::npos) return line;
      auto v = line.substr(sep + 1);
      while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) {
        v.erase(v.begin());
      }
      if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
        v = v.substr(1, v.size() - 2);
      }
      return v;
    }
  }
  return "?";
}

// `nvidia-smi --query-gpu=...`: driver version and the PCIe link the transfer
// actually runs over, which nothing in the CUDA runtime API reports.
std::string nvidia_smi_query(const char *fields) {
  std::string cmd = "nvidia-smi --query-gpu=";
  cmd += fields;
  cmd += " --format=csv,noheader 2>/dev/null";
  FILE *fp = ::popen(cmd.c_str(), "r");
  if (fp == nullptr) return "?";
  std::string out;
  char buf[512];
  while (std::fgets(buf, sizeof(buf), fp) != nullptr) out += buf;
  ::pclose(fp);
  while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) {
    out.pop_back();
  }
  return out.empty() ? std::string("?") : out;
}

size_t count_cpus() {
  std::ifstream in("/proc/cpuinfo");
  std::string line;
  size_t n = 0;
  while (std::getline(in, line)) {
    if (line.rfind("processor", 0) == 0) ++n;
  }
  return n;
}

// -------------------------------------------------------------- the stores

// One block source.  `acquire` hands out a borrowed pointer that stays valid
// until the matching `release(slot)`; `read_into` copies the block into the
// caller's destination (the pinned buffer, or a heap buffer).  A store with
// no borrowed-read API (filedir-pread) implements read_into only.
class GpuStore {
 public:
  virtual ~GpuStore() = default;
  [[nodiscard]] virtual std::string name() const = 0;
  [[nodiscard]] virtual std::string tuning() const = 0;
  [[nodiscard]] virtual bool has_borrowed_read() const = 0;
  virtual bool populate(const Dataset &ds, size_t bs) = 0;
  virtual bool acquire(size_t slot, size_t i, const uint8_t *&data,
                       size_t &len) = 0;
  virtual void release(size_t slot) = 0;
  virtual bool read_into(size_t i, uint8_t *dst, size_t len) {
    const uint8_t *d = nullptr;
    size_t l = 0;
    if (!acquire(0, i, d, l) || l != len) {
      release(0);
      return false;
    }
    std::memcpy(dst, d, len);
    release(0);
    return true;
  }
};

// ------------------------------------------------------------------ cyclone

class CycloneStore : public GpuStore {
 public:
  explicit CycloneStore(std::string dir) : _dir(std::move(dir)) {
    _slots.resize(kBatchBlocks);
  }
  ~CycloneStore() override {
    _probe_handle.reset();
    for (auto &s : _slots) s.reset();
    if (_cache) _cache->stop();
  }

  [[nodiscard]] std::string name() const override { return "cyclone"; }
  [[nodiscard]] std::string tuning() const override {
    return "8 GiB volume, max_object_size=0, ram_cache_size=0 (disk hits give "
           "a borrowed mmap span), enable_checksum=true, "
           "verify_checksum_on_read=true, set_multi_process(0,1) (shared mmap "
           "directory), stripe_size=auto (16 x ~512 MiB), sync_on_write=false, "
           "hit tracking / optimization / directory syncer off -- identical to "
           "benchmarks/kv_bench.cpp and benchmarks/kv_gpu_metal.mm";
  }
  [[nodiscard]] bool has_borrowed_read() const override { return true; }

  bool populate(const Dataset &ds, size_t bs) override {
    std::error_code ec;
    std::filesystem::create_directories(_dir, ec);
    for (const auto &e : std::filesystem::directory_iterator(_dir, ec)) {
      if (ec) break;
      if (e.path().filename().string().rfind(kVolumeStem, 0) == 0) {
        std::filesystem::remove(e.path(), ec);
      }
    }

    cyclone::CacheConfig config;
    config.ram_cache_size = 0;
    config.max_object_size = 0;
    config.enable_checksum = true;
    config.verify_checksum_on_read = true;
    config.enable_hit_tracking = false;
    config.optimization_config.enabled = false;
    config.directory_sync_interval = std::chrono::milliseconds{0};
    config.set_multi_process(0, 1);

    auto created = cyclone::Cache::create(config);
    if (!created) {
      std::cerr << "  cyclone: Cache::create failed\n";
      return false;
    }
    _cache = std::move(*created);

    cyclone::VolumeConfig vol;
    vol.path =
        (std::filesystem::path(_dir) / (std::string(kVolumeStem) + ".dat"))
            .string();
    vol.size = kVolumeBytes;
    vol.stripe_size = 0;  // auto: 16 stripes of ~512 MiB
    vol.sync_on_write = false;
    vol.max_object_size = 0;
    if (!_cache->add_volume(vol)) {
      std::cerr << "  cyclone: add_volume failed\n";
      return false;
    }
    if (!_cache->start()) {
      std::cerr << "  cyclone: start failed\n";
      return false;
    }
    // The fingerprinted file name differs from the configured path, and the
    // A3 probe mmaps the real file, so take it from the cache.
    for (const auto &f : _cache->volume_files()) _volume_file = f.file_path;

    _keys = ds.keys;
    std::vector<std::byte> block(bs);
    for (size_t i = 0; i < _keys.size(); ++i) {
      fill_block(block, i);
      auto wh = _cache->write_sync(_keys[i], bs);
      if (!wh) return false;
      wh->set_header(std::span<const std::byte>(ds.metadata[i]));
      auto w = wh->write_sync(std::span<const std::byte>(block));
      auto c = wh->close_sync();
      if (!w || !c) return false;
    }
    return true;
  }

  bool acquire(size_t slot, size_t i, const uint8_t *&data,
               size_t &len) override {
    auto rh = _cache->read_sync(_keys[i]);
    if (!rh) return false;
    const auto c = rh->content();
    data = reinterpret_cast<const uint8_t *>(c.data());
    len = c.size();
    _slots[slot] = std::move(*rh);
    return true;
  }

  void release(size_t slot) override { _slots[slot].reset(); }

  // Extra registration-probe surfaces (A2 / A3), used once at startup.
  std::optional<std::span<const std::byte>> mapped_view_of(size_t i) {
    auto rh = _cache->read_sync(_keys[i]);
    if (!rh) return std::nullopt;
    auto mv = rh->mapped_view();
    _probe_handle = std::move(*rh);
    return mv;
  }
  uint64_t content_file_offset_of(size_t i) {
    auto rh = _cache->read_sync(_keys[i]);
    if (!rh) return cyclone::ReadHandle::kNoFileOffset;
    const uint64_t off = rh->content_file_offset();
    _probe_handle = std::move(*rh);
    return off;
  }
  void drop_probe_handle() { _probe_handle.reset(); }
  [[nodiscard]] const std::string &volume_file() const { return _volume_file; }

 private:
  std::string _dir;
  std::unique_ptr<cyclone::Cache> _cache;
  std::vector<cyclone::CacheKey> _keys;
  std::vector<std::optional<cyclone::ReadHandle>> _slots;
  std::optional<cyclone::ReadHandle> _probe_handle;
  std::string _volume_file;
};

// --------------------------------------------------------------------- lmdb

#ifdef CYCLONE_KV_GPU_HAVE_LMDB
class LmdbStore : public GpuStore {
 public:
  explicit LmdbStore(std::string dir) : _dir(std::move(dir)) {
    _txns.assign(kBatchBlocks, nullptr);
  }
  ~LmdbStore() override {
    for (size_t s = 0; s < _txns.size(); ++s) release(s);
    if (_env) mdb_env_close(_env);
  }

  [[nodiscard]] std::string name() const override { return "lmdb"; }
  [[nodiscard]] std::string tuning() const override {
    int a = 0, b = 0, c = 0;
    const char *v = mdb_version(&a, &b, &c);
    return std::string(v ? v : "LMDB") +
           "; map size 16 GiB, env flags MDB_NOTLS (durable default "
           "otherwise); key = raw 32-byte digest, value = 64 B header + "
           "block; put = one write txn with mdb_put(MDB_RESERVE); get = one "
           "MDB_RDONLY txn, MDB_val points into the map (genuinely "
           "zero-copy).  MDB_NOTLS is a DEVIATION from the peer harness's "
           "flags=0: without it LMDB pins a read txn to a thread slot and a "
           "batch of 16 simultaneous borrows from one thread cannot be "
           "opened at all.  It changes reader-slot bookkeeping only, not "
           "durability.";
  }
  [[nodiscard]] bool has_borrowed_read() const override { return true; }

  bool populate(const Dataset &ds, size_t bs) override {
    std::error_code ec;
    std::filesystem::remove_all(_dir, ec);
    std::filesystem::create_directories(_dir, ec);
    if (mdb_env_create(&_env) != 0) return false;
    mdb_env_set_mapsize(_env, kLmdbMapBytes);
    mdb_env_set_maxreaders(_env, 256);
    // MDB_NOTLS: allow more than one MDB_RDONLY txn per thread, which the
    // 16-block batch needs (16 simultaneous borrows from one thread).
    if (mdb_env_open(_env, _dir.c_str(), MDB_NOTLS, 0664) != 0) return false;
    MDB_txn *txn = nullptr;
    if (mdb_txn_begin(_env, nullptr, 0, &txn) != 0) return false;
    if (mdb_dbi_open(txn, nullptr, 0, &_dbi) != 0) return false;
    if (mdb_txn_commit(txn) != 0) return false;

    _keys = ds.keys;
    std::vector<std::byte> block(bs);
    for (size_t i = 0; i < _keys.size(); ++i) {
      fill_block(block, i);
      MDB_txn *wt = nullptr;
      if (mdb_txn_begin(_env, nullptr, 0, &wt) != 0) return false;
      MDB_val k = key_val(i);
      MDB_val v{kMetaSize + bs, nullptr};
      if (mdb_put(wt, _dbi, &k, &v, MDB_RESERVE) != 0) {
        mdb_txn_abort(wt);
        return false;
      }
      auto *dst = static_cast<uint8_t *>(v.mv_data);
      std::memcpy(dst, ds.metadata[i].data(), kMetaSize);
      std::memcpy(dst + kMetaSize, block.data(), bs);
      if (mdb_txn_commit(wt) != 0) return false;
    }
    return true;
  }

  bool acquire(size_t slot, size_t i, const uint8_t *&data,
               size_t &len) override {
    MDB_txn *txn = nullptr;
    if (mdb_txn_begin(_env, nullptr, MDB_RDONLY, &txn) != 0) return false;
    MDB_val k = key_val(i);
    MDB_val v{};
    if (mdb_get(txn, _dbi, &k, &v) != 0 || v.mv_size < kMetaSize) {
      mdb_txn_abort(txn);
      return false;
    }
    _txns[slot] = txn;
    data = static_cast<const uint8_t *>(v.mv_data) + kMetaSize;
    len = v.mv_size - kMetaSize;
    return true;
  }

  void release(size_t slot) override {
    if (_txns[slot]) {
      mdb_txn_abort(_txns[slot]);
      _txns[slot] = nullptr;
    }
  }

 private:
  // The raw 32-byte SHA-256 digest, pointing into the retained key.
  MDB_val key_val(size_t i) const {
    auto digest = _keys[i].digest();
    return MDB_val{
        digest.size(),
        const_cast<void *>(static_cast<const void *>(digest.data()))};
  }

  std::string _dir;
  MDB_env *_env = nullptr;
  MDB_dbi _dbi = 0;
  std::vector<cyclone::CacheKey> _keys;
  std::vector<MDB_txn *> _txns;
};
#endif  // CYCLONE_KV_GPU_HAVE_LMDB

// ------------------------------------------------------------------ filedir

// One file per block, read with a single pread straight into the destination.
// No borrowed-buffer API, so there is no zero-copy path to compare -- which is
// precisely the baseline the zero-copy paths are measured against.
class FiledirStore : public GpuStore {
 public:
  explicit FiledirStore(std::string dir) : _dir(std::move(dir)) {}

  [[nodiscard]] std::string name() const override { return "filedir-pread"; }
  [[nodiscard]] std::string tuning() const override {
    return "one file per block named by the 64-char hex digest; record = 64 B "
           "header + block; put = open/write/close, no fsync; get = open + "
           "pread of the block straight into the destination + close";
  }
  [[nodiscard]] bool has_borrowed_read() const override { return false; }

  bool populate(const Dataset &ds, size_t bs) override {
    std::error_code ec;
    std::filesystem::remove_all(_dir, ec);
    std::filesystem::create_directories(_dir, ec);
    _keys = ds.keys;
    std::vector<std::byte> block(bs);
    for (size_t i = 0; i < _keys.size(); ++i) {
      fill_block(block, i);
      const std::string p = path_for(i);
      const int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (fd < 0) return false;
      const bool ok = ::write(fd, ds.metadata[i].data(), kMetaSize) ==
                          static_cast<ssize_t>(kMetaSize) &&
                      ::write(fd, block.data(), bs) == static_cast<ssize_t>(bs);
      ::close(fd);
      if (!ok) return false;
    }
    return true;
  }

  bool acquire(size_t, size_t, const uint8_t *&, size_t &) override {
    return false;  // no borrowed read path
  }
  void release(size_t) override {}

  bool read_into(size_t i, uint8_t *dst, size_t len) override {
    const std::string p = path_for(i);
    const int fd = ::open(p.c_str(), O_RDONLY);
    if (fd < 0) return false;
    const ssize_t n = ::pread(fd, dst, len, static_cast<off_t>(kMetaSize));
    ::close(fd);
    return n == static_cast<ssize_t>(len);
  }

 private:
  [[nodiscard]] std::string path_for(size_t i) const {
    return (std::filesystem::path(_dir) / (_keys[i].to_hex() + ".blk"))
        .string();
  }
  std::string _dir;
  std::vector<cyclone::CacheKey> _keys;
};

// -------------------------------------------------------------- measurement

struct RunOut {
  double ops_per_s = 0.0;
  double gb_per_s = 0.0;
  double p50_us = 0.0;
  double p99_us = 0.0;
  bool correct = false;
  bool supported = true;
  std::string note;
};

// One unit of work: transfer `count` blocks starting at index `start`, into
// destination slots 0..count-1.  Returns false if any block failed.
using UnitFn = std::function<bool(size_t start, size_t count)>;
// Checksum of destination slot 0 after a unit(idx, 1).
using ChecksumFn = std::function<uint64_t()>;

RunOut run_path(size_t n, size_t bs, size_t batch, double seconds,
                const UnitFn &unit, const ChecksumFn &checksum) {
  RunOut out;
  const double warmup = std::min(kWarmupSeconds, seconds);
  size_t i = 0;
  g_unit_error.clear();

  auto t0 = Clock::now();
  while (secs_since(t0) < warmup) {
    if (!unit(i, batch)) {
      out.supported = false;
      out.note = "failed during warm-up" +
                 (g_unit_error.empty() ? std::string() : ": " + g_unit_error);
      return out;
    }
    i = (i + batch) % n;
  }

  std::vector<double> lat;
  lat.reserve(static_cast<size_t>(seconds * 20000));
  uint64_t blocks = 0;
  t0 = Clock::now();
  double elapsed = 0.0;
  for (;;) {
    const auto a = Clock::now();
    if (!unit(i, batch)) {
      out.supported = false;
      out.note = "failed during measurement" +
                 (g_unit_error.empty() ? std::string() : ": " + g_unit_error);
      return out;
    }
    const auto b = Clock::now();
    // Per-block amortized latency: a batched unit's wall time / batch, so the
    // percentile columns stay comparable across the batch column.
    lat.push_back(std::chrono::duration<double, std::micro>(b - a).count() /
                  static_cast<double>(batch));
    blocks += batch;
    i = (i + batch) % n;
    elapsed = std::chrono::duration<double>(b - t0).count();
    if (elapsed >= seconds) break;
  }

  out.ops_per_s = static_cast<double>(blocks) / elapsed;
  // Decimal GB, excluding the 64-byte header, as in kv_bench.
  out.gb_per_s =
      static_cast<double>(blocks) * static_cast<double>(bs) / elapsed / 1e9;
  const auto pct = compute_percentiles(lat);
  out.p50_us = pct.p50;
  out.p99_us = pct.p99;

  // Correctness: one more single-block transfer of a known block, then read
  // the destination back and compare a 64-bit checksum against the source.
  const size_t check = n / 2;
  std::vector<uint8_t> expect(bs);
  fill_block(expect.data(), bs, check);
  const uint64_t want = fnv1a(expect.data(), bs);
  out.correct = unit(check, 1) && checksum() == want;
  return out;
}

// Copy device slot 0 back to the host and checksum it: the CPU cannot read
// device memory, so the check has to round-trip.
uint64_t checksum_device_slot0(size_t bs, std::vector<uint8_t> *readback) {
  if (cygpu_memcpy_d2h(readback->data(), 0, bs) != 0) return 0;
  return fnv1a(readback->data(), bs);
}

// ------------------------------------------------------------------- report

struct Row {
  std::string store;
  std::string path;
  size_t batched = 1;
  size_t block_size = 0;
  RunOut r;
};

// Number of rows that ran (supported) but failed their correctness check;
// main() turns a non-zero count into a failing exit status.
size_t count_failed(const std::vector<Row> &rows) {
  return static_cast<size_t>(std::count_if(
      rows.begin(), rows.end(),
      [](const Row &row) { return row.r.supported && !row.r.correct; }));
}

void print_table(const std::vector<Row> &rows) {
  std::printf(
      "\n| store | path | batch | blocks/s | GB/s | p50 us | p99 us | "
      "correct |\n");
  std::printf("|---|---|---:|---:|---:|---:|---:|---|\n");
  for (const auto &row : rows) {
    if (!row.r.supported) {
      std::printf("| %s | %s | %zu | - | - | - | - | %s |\n", row.store.c_str(),
                  row.path.c_str(), row.batched, row.r.note.c_str());
      continue;
    }
    std::printf("| %s | %s | %zu | %.0f | %.2f | %.1f | %.1f | %s |\n",
                row.store.c_str(), row.path.c_str(), row.batched,
                row.r.ops_per_s, row.r.gb_per_s, row.r.p50_us, row.r.p99_us,
                row.r.correct ? "ok" : "**FAIL**");
  }
}

void emit_json(const std::vector<Row> &rows, std::ostream &os) {
  for (const auto &row : rows) {
    if (!row.r.supported) continue;
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "{\"store\":\"%s\",\"path\":\"%s\",\"batched\":%zu,"
                  "\"block_size\":%zu,\"ops_per_s\":%.1f,\"gb_per_s\":%.4f,"
                  "\"p50_us\":%.2f,\"p99_us\":%.2f,\"correct\":\"%s\"}",
                  row.store.c_str(), row.path.c_str(), row.batched,
                  row.block_size, row.r.ops_per_s, row.r.gb_per_s, row.r.p50_us,
                  row.r.p99_us, row.r.correct ? "ok" : "fail");
    os << buf << "\n";
  }
  os.flush();
}

// ------------------------------------------------- cudaHostRegister probes

// Answers the load-bearing question: will CUDA page-lock a file-backed
// MAP_SHARED mapping?  "accepted" alone is not enough -- a transfer out of
// the registered range must also deliver the right bytes, so the probe copies
// and compares.
struct RegProbe {
  bool accepted = false;
  bool transfer_ok = false;
  bool bytes_match = false;
  std::string detail;
};

RegProbe probe_register(const void *base, size_t len, size_t src_off,
                        size_t copy_len, bool read_only,
                        std::vector<uint8_t> *readback) {
  RegProbe p;
  const int rc = cygpu_host_register(base, len, read_only ? 1 : 0);
  if (rc != 0) {
    p.detail = "REFUSED: cudaHostRegister -> " + cuda_err(rc);
    return p;
  }
  p.accepted = true;
  const auto *src = static_cast<const uint8_t *>(base) + src_off;
  const int c1 = cygpu_memcpy_h2d_async(src, 0, copy_len);
  const int c2 = cygpu_stream_sync();
  if (c1 != 0 || c2 != 0) {
    p.detail = "accepted; but transfer FAILED: " + cuda_err(c1 != 0 ? c1 : c2);
  } else {
    p.transfer_ok = true;
    // A2 hands over a document view that is LONGER than the readback buffer
    // (it carries the 132-byte header), so the comparison is taken over as
    // much as fits -- enough to catch a transfer that moved the wrong bytes.
    const size_t verify = std::min(copy_len, readback->size());
    const int c3 = cygpu_memcpy_d2h(readback->data(), 0, verify);
    p.bytes_match = c3 == 0 && std::memcmp(readback->data(), src, verify) == 0;
    p.detail = std::string("accepted; transfer completed; device bytes ") +
               (p.bytes_match ? "== source" : "!= SOURCE");
  }
  const int urc = cygpu_host_unregister(base);
  if (urc != 0) p.detail += "; unregister -> " + cuda_err(urc);
  return p;
}

// Self-contained probe, independent of Cyclone: our own MAP_SHARED file
// mapping read-write and read-only, plus a private anonymous mapping as the
// control (which must always be accepted -- if it is not, the failure is in
// the driver or the memlock limit, not in the file backing).
void probe_synthetic(const std::string &dir, size_t bs,
                     std::vector<uint8_t> *readback) {
  const std::string p = (std::filesystem::path(dir) / "reg_probe.bin").string();
  const int fd = ::open(p.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    std::printf("  synthetic probe: cannot create %s\n", p.c_str());
    return;
  }
  if (::ftruncate(fd, static_cast<off_t>(bs)) != 0) {
    ::close(fd);
    return;
  }

  void *rw = ::mmap(nullptr, bs, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (rw != MAP_FAILED) {
    std::memset(rw, 0xCD, bs);
    auto r = probe_register(rw, bs, 0, bs, false, readback);
    std::printf("  mmap(MAP_SHARED, RW)  RegisterDefault  : %s\n",
                r.detail.c_str());
    r = probe_register(rw, bs, 0, bs, true, readback);
    std::printf("  mmap(MAP_SHARED, RW)  RegisterReadOnly : %s\n",
                r.detail.c_str());
    ::munmap(rw, bs);
  }

  void *ro = ::mmap(nullptr, bs, PROT_READ, MAP_SHARED, fd, 0);
  if (ro != MAP_FAILED) {
    auto r = probe_register(ro, bs, 0, bs, false, readback);
    std::printf("  mmap(MAP_SHARED, RO)  RegisterDefault  : %s\n",
                r.detail.c_str());
    r = probe_register(ro, bs, 0, bs, true, readback);
    std::printf("  mmap(MAP_SHARED, RO)  RegisterReadOnly : %s\n",
                r.detail.c_str());
    ::munmap(ro, bs);
  }

  // Control: a private anonymous mapping, which cudaHostRegister is
  // documented to accept.  Anything else failing while this succeeds isolates
  // the failure to the file backing.
  void *anon = ::mmap(nullptr, bs, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (anon != MAP_FAILED) {
    std::memset(anon, 0xCD, bs);
    const auto r = probe_register(anon, bs, 0, bs, false, readback);
    std::printf("  MAP_PRIVATE|ANONYMOUS (control)        : %s\n",
                r.detail.c_str());
    ::munmap(anon, bs);
  }

  ::close(fd);
  std::error_code ec;
  std::filesystem::remove(p, ec);
}

// The three Cyclone surfaces an embedder could hand to CUDA.  A1 is the one
// the `zerocopy` path uses; A2 and A3 are the persistent-mapping surfaces,
// A3 being what a GPUDirect-style consumer would use.
void probe_cyclone_surfaces(CycloneStore *cyc, size_t bs,
                            std::vector<uint8_t> *readback) {
  std::printf("  cudaHostRegister surfaces for cyclone:\n");

  // A1: the borrowed content() span, registered from the page below it.
  const uint8_t *d = nullptr;
  size_t l = 0;
  if (cyc->acquire(0, 0, d, l)) {
    const uintptr_t ptr = reinterpret_cast<uintptr_t>(d);
    const uintptr_t base = page_base(ptr);
    const size_t off = static_cast<size_t>(ptr - base);
    const auto r =
        probe_register(reinterpret_cast<void *>(base),
                       round_up(off + l, g_page_size), off, l, false, readback);
    // content() sits behind the 132-byte document header, so it is never page
    // aligned; the registration is taken at the page below it and the copy
    // sources from content() itself, which is inside the registered range.
    std::printf(
        "    A1 content() page-aligned range      : %s\n"
        "       (content ptr %% page = %zu)\n",
        r.detail.c_str(), off);
    cyc->release(0);
  }

  // A2: the whole document view, header included.
  if (auto mv = cyc->mapped_view_of(0); mv.has_value()) {
    const uintptr_t ptr = reinterpret_cast<uintptr_t>(mv->data());
    const uintptr_t base = page_base(ptr);
    const size_t off = static_cast<size_t>(ptr - base);
    const auto r = probe_register(reinterpret_cast<void *>(base),
                                  round_up(off + mv->size(), g_page_size), off,
                                  mv->size(), false, readback);
    std::printf(
        "    A2 mapped_view() page-aligned range  : %s\n"
        "       (document view %zu B incl. its header)\n",
        r.detail.c_str(), mv->size());
  } else {
    std::printf("    A2 mapped_view()                     : none\n");
  }
  cyc->drop_probe_handle();

  // A3: our own mmap of the volume file at content_file_offset() -- the
  // surface a GPUDirect-style consumer would use.
  const uint64_t fo = cyc->content_file_offset_of(0);
  if (fo != cyclone::ReadHandle::kNoFileOffset && !cyc->volume_file().empty()) {
    const int fd = ::open(cyc->volume_file().c_str(), O_RDONLY);
    if (fd >= 0) {
      const uint64_t abase = fo & ~static_cast<uint64_t>(g_page_size - 1);
      const size_t aoff = static_cast<size_t>(fo - abase);
      const size_t mlen = round_up(aoff + bs, g_page_size);
      void *m = ::mmap(nullptr, mlen, PROT_READ, MAP_SHARED, fd,
                       static_cast<off_t>(abase));
      if (m != MAP_FAILED) {
        // Both flags: a PROT_READ mapping is exactly the case
        // cudaHostRegisterDefault refuses, so reporting only Default would
        // read as "CUDA cannot take this surface" when it can.
        auto r = probe_register(m, mlen, aoff, bs, false, readback);
        std::printf(
            "    A3 own mmap(PROT_READ) Default       : %s\n"
            "       (volume file %s, content_file_offset %llu)\n",
            r.detail.c_str(), cyc->volume_file().c_str(),
            static_cast<unsigned long long>(fo));
        r = probe_register(m, mlen, aoff, bs, true, readback);
        std::printf("    A3 own mmap(PROT_READ) ReadOnly      : %s\n",
                    r.detail.c_str());
        ::munmap(m, mlen);
      }
      ::close(fd);
    }
  } else {
    std::printf(
        "    A3 own mmap                          : content_file_offset "
        "unavailable\n");
  }
  cyc->drop_probe_handle();
  std::fflush(stdout);
}

// ------------------------------------------ persistent registration windows

// The `zerocopy-persistent` setup: page-lock the store's whole mapping once.
// A single cudaHostRegister over several GiB is the ideal; when the driver or
// RLIMIT_MEMLOCK refuses it, the same span is covered by kRegisterWindow
// windows instead, which is functionally identical for the transfer (a
// borrowed pointer lands inside exactly one of them) and only differs in
// bookkeeping.
class SpanRegistration {
 public:
  ~SpanRegistration() { release(); }

  bool acquire(uintptr_t base, size_t len) {
    _base = base;
    _len = len;
    if (try_windows(len)) {
      _windows = 1;
      return true;
    }
    const std::string whole = _error;
    if (try_windows(kRegisterWindow)) {
      _windows = (len + kRegisterWindow - 1) / kRegisterWindow;
      _error = "single " + std::to_string(len >> 30) + " GiB registration " +
               whole + "; fell back to 1 GiB windows";
      return true;
    }
    _error = "single registration " + whole + "; 1 GiB windows " + _error;
    return false;
  }

  void release() {
    for (auto p : _registered) cygpu_host_unregister(p);
    _registered.clear();
  }

  [[nodiscard]] bool contains(uintptr_t p, size_t len) const {
    return p >= _base && (p - _base) + len <= _len;
  }
  [[nodiscard]] size_t windows() const { return _windows; }
  [[nodiscard]] const std::string &error() const { return _error; }
  // Which cudaHostRegister flag the mapping was actually taken with -- the
  // read-only fallback is the whole reason LMDB's map can be registered.
  [[nodiscard]] const char *flag() const { return _flag; }

 private:
  bool try_windows(size_t window) {
    release();
    for (size_t off = 0; off < _len; off += window) {
      const size_t n = std::min(window, _len - off);
      auto *p = reinterpret_cast<void *>(_base + off);
      std::string err;
      switch (host_register_any(p, n, &err)) {
        case RegOutcome::kDefault:
          _flag = "cudaHostRegisterDefault";
          break;
        case RegOutcome::kReadOnly:
          _flag = "cudaHostRegisterReadOnly";
          break;
        case RegOutcome::kAlreadyRegistered:
          _flag = "already registered";
          continue;  // nothing of ours to unregister
        case RegOutcome::kRefused:
          _error = "-> " + err;
          release();
          return false;
      }
      _registered.push_back(p);
    }
    _error.clear();
    return true;
  }

  uintptr_t _base = 0;
  size_t _len = 0;
  size_t _windows = 0;
  const char *_flag = "?";
  std::vector<void *> _registered;
  std::string _error;
};

// --------------------------------------------------------------------- CLI

struct Params {
  std::vector<size_t> block_sizes;
  double seconds = 10.0;
  std::string path = "./gpudata";
  std::string output;
};

void usage() {
  std::printf(
      "kv_gpu_cuda -- does Cyclone's zero-copy read pay off for a CUDA "
      "host-to-device transfer?\n\n"
      "  --block-size N   value size in bytes (repeatable; default 2097152)\n"
      "  --seconds S      measured window per path (default 10)\n"
      "  --path DIR       store data directory (default ./gpudata)\n"
      "  --output FILE    write the JSON lines to FILE as well as stdout\n"
      "  --help\n");
}

void print_machine_info(const Params &p, const CyGpuDeviceInfo &gpu) {
  utsname u{};
  ::uname(&u);
  std::printf("=== machine ===\n");
  std::printf("cpu          : %s (%zu logical)\n",
              first_line_matching("/proc/cpuinfo", "model name").c_str(),
              count_cpus());
  std::printf("memory       : %s\n",
              first_line_matching("/proc/meminfo", "MemTotal").c_str());
  std::printf("os           : %s (%s %s)\n",
              first_line_matching("/etc/os-release", "PRETTY_NAME").c_str(),
              u.sysname, u.release);
  std::printf(
      "gpu          : %s (compute capability %d.%d, %d SMs, "
      "%.1f GiB)\n",
      gpu.name, gpu.cc_major, gpu.cc_minor, gpu.multi_processor_count,
      static_cast<double>(gpu.total_global_mem) / (1024.0 * 1024 * 1024));
  std::printf(
      "pci          : %04x:%02x:%02x, unified addressing %s, "
      "can map host memory %s\n",
      gpu.pci_domain_id, gpu.pci_bus_id, gpu.pci_device_id,
      gpu.unified_addressing ? "yes" : "no",
      gpu.can_map_host_memory ? "yes" : "no");
  std::printf("cuda         : driver %d.%d, runtime %d.%d\n",
              gpu.driver_version / 1000, (gpu.driver_version % 1000) / 10,
              gpu.runtime_version / 1000, (gpu.runtime_version % 1000) / 10);
  std::printf("nvidia-smi   : %s\n",
              nvidia_smi_query("driver_version,pcie.link.gen.current,"
                               "pcie.link.gen.max,pcie.link.width.current,"
                               "pcie.link.width.max")
                  .c_str());
  std::printf("              (driver, PCIe gen cur/max, width cur/max)\n");
  std::printf("page size    : %zu bytes (getpagesize)\n", g_page_size);
  std::printf("data path    : %s\n", p.path.c_str());
#ifdef CYCLONE_KV_GPU_HAVE_LMDB
  std::printf("lmdb         : built in\n");
#else
  std::printf("lmdb         : not built\n");
#endif
  std::printf(
      "\nNOTE: this experiment isolates the TRANSFER, not first touch.\n"
      "Every block is written, then read once and page-touched, before any\n"
      "timing starts -- CRC verification, directory faults and page faults\n"
      "are all paid up front.\n");
}

// The fixed cost of a CUDA submission and the bandwidth ceiling of the bus,
// neither of which involves a store.  Together they bound every row below:
// nothing can be faster than pinned-ceiling, and at batch 1 nothing can be
// faster than submit-only-0B.
void measure_cuda_references(double seconds, size_t bs, std::vector<Row> *rows,
                             std::vector<uint8_t> *readback) {
  auto *pinned = static_cast<uint8_t *>(cygpu_pinned_ptr());

  // Bring the GPU up to clock first; a cold device idles at a low pstate and
  // reports a bus bandwidth that swings by more than the effects measured.
  const auto t_warm = Clock::now();
  while (secs_since(t_warm) < kDeviceWarmupSeconds) {
    cygpu_memcpy_h2d_async(pinned, 0, bs);
    cygpu_stream_sync();
  }

  UnitFn submit_only = [](size_t, size_t) {
    return cygpu_memcpy_h2d_async(cygpu_pinned_ptr(), 0, 0) == 0 &&
           cygpu_stream_sync() == 0;
  };
  UnitFn h2d_4k = [pinned](size_t, size_t) {
    return cygpu_memcpy_h2d_async(pinned, 0, 4096) == 0 &&
           cygpu_stream_sync() == 0;
  };
  UnitFn ceiling = [pinned, bs](size_t, size_t count) {
    for (size_t k = 0; k < count; ++k) {
      if (cygpu_memcpy_h2d_async(pinned + k * bs, k * bs, bs) != 0) {
        return false;
      }
    }
    return cygpu_stream_sync() == 0;
  };
  ChecksumFn always_ok = []() { return uint64_t{0}; };

  auto a = run_path(kBlocks, 0, 1, seconds, submit_only, always_ok);
  auto b = run_path(kBlocks, 4096, 1, seconds, h2d_4k, always_ok);
  a.correct = true;
  b.correct = true;
  rows->push_back(Row{"cuda", "submit-only-0B", 1, 0, a});
  rows->push_back(Row{"cuda", "h2d-4KiB", 1, 4096, b});

  // The pinned-buffer ceiling IS checked for correctness: the pinned buffer is
  // filled with the check block so the same round trip as every other path
  // applies.
  std::vector<uint8_t> expect(bs);
  fill_block(expect.data(), bs, kBlocks / 2);
  for (size_t k = 0; k < kBatchBlocks; ++k) {
    std::memcpy(pinned + k * bs, expect.data(), bs);
  }
  ChecksumFn dev_checksum = [bs, readback]() {
    return checksum_device_slot0(bs, readback);
  };
  std::vector<Row> ceil_rows;
  for (size_t batch : {size_t{1}, kBatchBlocks}) {
    auto c = run_path(kBlocks, bs, batch, seconds, ceiling, dev_checksum);
    ceil_rows.push_back(Row{"cuda", "pinned-ceiling", batch, bs, c});
  }

  std::printf(
      "\n--- CUDA fixed cost and PCIe ceiling (no store involved) ---\n"
      "  0-byte H2D + stream sync    : %.1f us p50, %.1f us p99, %.0f/s\n"
      "  4 KiB H2D from pinned       : %.1f us p50, %.1f us p99, %.0f/s\n",
      a.p50_us, a.p99_us, a.ops_per_s, b.p50_us, b.p99_us, b.ops_per_s);
  for (const auto &row : ceil_rows) {
    std::printf(
        "  %.1f MiB H2D from pinned, batch %2zu : %6.2f GB/s, %.1f us p50\n",
        static_cast<double>(bs) / (1024.0 * 1024), row.batched, row.r.gb_per_s,
        row.r.p50_us);
  }
  rows->insert(rows->end(), ceil_rows.begin(), ceil_rows.end());
  std::fflush(stdout);
}

// Everything measured for one store at one block size.
void run_store(GpuStore *store, const Dataset &ds, size_t bs, double seconds,
               std::vector<uint8_t> *heap, std::vector<uint8_t> *readback,
               std::vector<Row> *all_rows) {
  std::printf("\n--- store: %s ---\n  tuning: %s\n", store->name().c_str(),
              store->tuning().c_str());
  std::fflush(stdout);

  const auto t_pop = Clock::now();
  if (!store->populate(ds, bs)) {
    std::printf("  populate FAILED -- skipping this store\n");
    return;
  }
  std::printf("  populate: %zu blocks in %.1f s\n", ds.keys.size(),
              secs_since(t_pop));

  // First touch: read every block once and touch every page, so CRC
  // verification and page faults are paid before any timing.  `lo`/`hi` bound
  // the span of the store's mapping the dataset actually occupies, which the
  // persistent-registration variant needs.
  const auto t_touch = Clock::now();
  uint64_t sink = 0;
  bool touch_ok = true;
  uintptr_t lo = UINTPTR_MAX;
  uintptr_t hi = 0;
  for (size_t i = 0; i < ds.keys.size(); ++i) {
    if (store->has_borrowed_read()) {
      const uint8_t *d = nullptr;
      size_t l = 0;
      if (!store->acquire(0, i, d, l) || l != bs) {
        touch_ok = false;
        store->release(0);
        break;
      }
      const auto ptr = reinterpret_cast<uintptr_t>(d);
      lo = std::min(lo, ptr);
      hi = std::max(hi, ptr + l);
      for (size_t off = 0; off < l; off += kPageSize) sink += d[off];
      store->release(0);
    } else {
      if (!store->read_into(i, heap->data(), bs)) {
        touch_ok = false;
        break;
      }
      for (size_t off = 0; off < bs; off += kPageSize) sink += (*heap)[off];
    }
  }
  if (!touch_ok) {
    std::printf("  first touch FAILED -- skipping this store\n");
    return;
  }
  std::printf("  first touch: %zu blocks in %.1f s (sink %llu)\n",
              ds.keys.size(), secs_since(t_touch),
              static_cast<unsigned long long>(sink));
  std::fflush(stdout);

  if (auto *cyc = dynamic_cast<CycloneStore *>(store)) {
    probe_cyclone_surfaces(cyc, bs, readback);
  }

  const size_t n = ds.keys.size();
  auto *pinned = static_cast<uint8_t *>(cygpu_pinned_ptr());

  // A: cudaMemcpy straight from the borrowed pointer.  Synchronous by
  // definition, so it is batch 1 only.
  UnitFn unit_pageable = [&](size_t start, size_t count) {
    for (size_t k = 0; k < count; ++k) {
      const uint8_t *d = nullptr;
      size_t l = 0;
      if (!store->acquire(k, (start + k) % n, d, l) || l != bs) {
        g_unit_error = "store read failed";
        store->release(k);
        return false;
      }
      const int rc = cygpu_memcpy_h2d_sync(d, k * bs, l);
      store->release(k);
      if (rc != 0) {
        g_unit_error = "cudaMemcpy -> " + cuda_err(rc);
        return false;
      }
    }
    return true;
  };

  // B: register the page-aligned range containing each block, transfer,
  // unregister -- once per batch.
  //
  // Two mechanics the naive shape gets wrong, both found by running it.
  // Records are packed back to back, so two blocks of one batch routinely
  // share a page: CUDA refuses a range overlapping one already pinned, and a
  // copy whose source straddles the END of a registration comes back
  // "invalid argument" even though every byte is resident.  So the spans are
  // sorted and coalesced first, which is what an embedder pinning borrowed
  // spans would have to do anyway.  And ALL registration happens before any
  // copy is enqueued, unregistration only after the sync -- unregistering
  // host memory the DMA engine is still reading would be a use-after-free.
  std::vector<std::pair<const uint8_t *, size_t>> zc_blocks;
  std::vector<std::pair<uintptr_t, uintptr_t>> zc_spans;
  std::vector<std::pair<uintptr_t, uintptr_t>> zc_merged;
  std::vector<void *> zc_regs;
  zc_blocks.reserve(kBatchBlocks);
  zc_spans.reserve(kBatchBlocks);
  zc_merged.reserve(kBatchBlocks);
  zc_regs.reserve(kBatchBlocks);
  UnitFn unit_zerocopy = [&](size_t start, size_t count) {
    zc_blocks.clear();
    zc_spans.clear();
    zc_merged.clear();
    zc_regs.clear();
    bool ok = true;
    size_t got = 0;
    for (size_t k = 0; k < count; ++k) {
      const uint8_t *d = nullptr;
      size_t l = 0;
      if (!store->acquire(k, (start + k) % n, d, l) || l != bs) {
        g_unit_error = "store read failed";
        store->release(k);
        ok = false;
        break;
      }
      got = k + 1;
      zc_blocks.emplace_back(d, l);
      const uintptr_t ptr = reinterpret_cast<uintptr_t>(d);
      const uintptr_t base = page_base(ptr);
      const size_t off = static_cast<size_t>(ptr - base);
      zc_spans.emplace_back(base, base + round_up(off + l, g_page_size));
    }

    if (ok) {
      std::sort(zc_spans.begin(), zc_spans.end());
      for (const auto &s : zc_spans) {
        if (!zc_merged.empty() && s.first <= zc_merged.back().second) {
          zc_merged.back().second = std::max(zc_merged.back().second, s.second);
        } else {
          zc_merged.push_back(s);
        }
      }
      for (const auto &s : zc_merged) {
        auto *p = reinterpret_cast<void *>(s.first);
        std::string err;
        const auto outcome = host_register_any(p, s.second - s.first, &err);
        if (outcome == RegOutcome::kRefused) {
          g_unit_error = "cudaHostRegister on the borrowed span: " + err;
          ok = false;
          break;
        }
        // Registered by something else (the store's own mapping, say): the
        // bytes are page-locked either way, but do not unregister a range
        // this batch did not take.
        if (outcome != RegOutcome::kAlreadyRegistered) zc_regs.push_back(p);
      }
    }

    if (ok) {
      for (size_t k = 0; k < zc_blocks.size(); ++k) {
        const int rc = cygpu_memcpy_h2d_async(zc_blocks[k].first, k * bs,
                                              zc_blocks[k].second);
        if (rc != 0) {
          g_unit_error = "cudaMemcpyAsync -> " + cuda_err(rc);
          ok = false;
          break;
        }
      }
    }
    if (const int rc = cygpu_stream_sync(); rc != 0) {
      g_unit_error = "cudaStreamSynchronize -> " + cuda_err(rc);
      ok = false;
    }
    for (auto *r : zc_regs) cygpu_host_unregister(r);
    for (size_t k = 0; k < got; ++k) store->release(k);
    return ok;
  };

  // C: the whole mapping is already registered, so a block is nothing but its
  // borrowed pointer -- the driver DMAs out of the page cache in place.  The
  // containment check is not free bookkeeping: if a block ever landed outside
  // the registered span the transfer would silently fall back to the pageable
  // staging path and the row would be measuring D, not C.
  SpanRegistration span;
  UnitFn unit_zerocopy_persist = [&](size_t start, size_t count) {
    bool ok = true;
    size_t got = 0;
    for (size_t k = 0; k < count; ++k) {
      const uint8_t *d = nullptr;
      size_t l = 0;
      if (!store->acquire(k, (start + k) % n, d, l) || l != bs) {
        g_unit_error = "store read failed";
        store->release(k);
        ok = false;
        break;
      }
      got = k + 1;
      if (!span.contains(reinterpret_cast<uintptr_t>(d), l)) {
        g_unit_error = "borrowed pointer fell outside the registered span";
        ok = false;
        break;
      }
      const int rc = cygpu_memcpy_h2d_async(d, k * bs, l);
      if (rc != 0) {
        g_unit_error = "cudaMemcpyAsync -> " + cuda_err(rc);
        ok = false;
        break;
      }
    }
    if (const int rc = cygpu_stream_sync(); rc != 0) {
      g_unit_error = "cudaStreamSynchronize -> " + cuda_err(rc);
      ok = false;
    }
    for (size_t k = 0; k < got; ++k) store->release(k);
    return ok;
  };

  // D: memcpy into the pinned buffer, then transfer out of it.
  UnitFn unit_staged = [&](size_t start, size_t count) {
    bool ok = true;
    size_t got = 0;
    for (size_t k = 0; k < count; ++k) {
      if (!store->read_into((start + k) % n, pinned + k * bs, bs)) {
        g_unit_error = "store read failed";
        ok = false;
        break;
      }
      got = k + 1;
    }
    for (size_t k = 0; k < got; ++k) {
      const int rc = cygpu_memcpy_h2d_async(pinned + k * bs, k * bs, bs);
      if (rc != 0) {
        g_unit_error = "cudaMemcpyAsync -> " + cuda_err(rc);
        ok = false;
        break;
      }
    }
    if (const int rc = cygpu_stream_sync(); rc != 0) {
      g_unit_error = "cudaStreamSynchronize -> " + cuda_err(rc);
      ok = false;
    }
    return ok;
  };

  // E: the host copy alone, no GPU at all.
  UnitFn unit_memcpy = [&](size_t start, size_t count) {
    for (size_t k = 0; k < count; ++k) {
      if (!store->read_into((start + k) % n, heap->data(), bs)) {
        g_unit_error = "store read failed";
        return false;
      }
    }
    return true;
  };

  ChecksumFn dev_checksum = [bs, readback]() {
    return checksum_device_slot0(bs, readback);
  };
  ChecksumFn heap_checksum = [&]() { return fnv1a(heap->data(), bs); };

  std::vector<Row> rows;
  const std::string sname = store->name();
  const bool borrowed = store->has_borrowed_read();

  // Path-major, not batch-major as in the Metal experiment: the persistent
  // registration must NOT be in place while `pageable` or `zerocopy` run (a
  // pointer inside an already-registered range is no longer pageable, and
  // cudaHostRegister refuses a range that overlaps one), so each path owns
  // its setup for the whole time it is measured.
  if (borrowed) {
    rows.push_back({sname, "pageable", 1, bs,
                    run_path(n, bs, 1, seconds, unit_pageable, dev_checksum)});
    for (size_t batch : {size_t{1}, kBatchBlocks}) {
      rows.push_back(
          {sname, "zerocopy", batch, bs,
           run_path(n, bs, batch, seconds, unit_zerocopy, dev_checksum)});
    }

    const uintptr_t sbase = page_base(lo);
    const size_t slen = round_up(static_cast<size_t>(hi - sbase), g_page_size);
    const auto t_reg = Clock::now();
    if (span.acquire(sbase, slen)) {
      std::printf(
          "  persistent registration: accepted with %s (%.2f GiB of the "
          "store's mapping, %zu window(s), %.1f s)%s%s\n",
          span.flag(), static_cast<double>(slen) / (1024.0 * 1024 * 1024),
          span.windows(), secs_since(t_reg), span.error().empty() ? "" : " -- ",
          span.error().c_str());
      for (size_t batch : {size_t{1}, kBatchBlocks}) {
        rows.push_back({sname, "zerocopy-persistent", batch, bs,
                        run_path(n, bs, batch, seconds, unit_zerocopy_persist,
                                 dev_checksum)});
      }
    } else {
      std::printf("  persistent registration: REFUSED (%.2f GiB) -- %s\n",
                  static_cast<double>(slen) / (1024.0 * 1024 * 1024),
                  span.error().c_str());
      RunOut bad;
      bad.supported = false;
      bad.note = "cudaHostRegister refused the mapping";
      for (size_t batch : {size_t{1}, kBatchBlocks}) {
        rows.push_back({sname, "zerocopy-persistent", batch, bs, bad});
      }
    }
    span.release();
  }

  for (size_t batch : {size_t{1}, kBatchBlocks}) {
    rows.push_back(
        {sname, "staged", batch, bs,
         run_path(n, bs, batch, seconds, unit_staged, dev_checksum)});
  }
  // memcpy has no GPU submission to amortize, so batching it would measure
  // nothing new.
  rows.push_back({sname, "memcpy", 1, bs,
                  run_path(n, bs, 1, seconds, unit_memcpy, heap_checksum)});

  for (const auto &row : rows) {
    std::printf(
        "  %-20s batch %2zu : %8.0f blocks/s  %6.2f GB/s  p50 %8.1f us  "
        "p99 %8.1f us  %s\n",
        row.path.c_str(), row.batched, row.r.ops_per_s, row.r.gb_per_s,
        row.r.p50_us, row.r.p99_us,
        row.r.supported ? (row.r.correct ? "correct = ok" : "correct = FAIL")
                        : row.r.note.c_str());
    std::fflush(stdout);
  }
  all_rows->insert(all_rows->end(), rows.begin(), rows.end());
}

}  // namespace

int main(int argc, char **argv) {
  Params p;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string {
      return (i + 1 < argc) ? argv[++i] : std::string();
    };
    if (a == "--block-size") {
      p.block_sizes.push_back(std::stoull(next()));
    } else if (a == "--seconds") {
      p.seconds = std::stod(next());
    } else if (a == "--path") {
      p.path = next();
    } else if (a == "--output") {
      p.output = next();
    } else if (a == "--help" || a == "-h") {
      usage();
      return 0;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
      usage();
      return 2;
    }
  }
  if (p.block_sizes.empty()) p.block_sizes.push_back(2 * 1024 * 1024);

  g_page_size = static_cast<size_t>(::getpagesize());
  std::error_code ec;
  std::filesystem::create_directories(p.path, ec);

  if (const int rc = cygpu_init(); rc != 0) {
    std::fprintf(stderr, "CUDA init failed: %s\n", cuda_err(rc).c_str());
    return 1;
  }
  CyGpuDeviceInfo gpu{};
  if (const int rc = cygpu_device_info(&gpu); rc != 0) {
    std::fprintf(stderr, "cudaGetDeviceProperties failed: %s\n",
                 cuda_err(rc).c_str());
    return 1;
  }
  print_machine_info(p, gpu);

  const Dataset ds = build_dataset(kBlocks);
  std::vector<Row> all_rows;

  for (size_t bs : p.block_sizes) {
    std::printf("\n=== block size %zu bytes (%.1f MiB), N = %zu ===\n", bs,
                static_cast<double>(bs) / (1024.0 * 1024), kBlocks);

    if (const int rc =
            cygpu_alloc_buffers(kBatchBlocks * bs, kBatchBlocks * bs);
        rc != 0) {
      std::fprintf(stderr, "device/pinned allocation failed: %s\n",
                   cuda_err(rc).c_str());
      return 1;
    }
    std::vector<uint8_t> readback(bs);
    std::vector<uint8_t> heap(bs);

    std::printf("\n--- cudaHostRegister on a file-backed mapping ---\n");
    probe_synthetic(p.path, bs, &readback);

    measure_cuda_references(p.seconds, bs, &all_rows, &readback);

    std::vector<std::unique_ptr<GpuStore>> stores;
    stores.push_back(std::make_unique<CycloneStore>(
        (std::filesystem::path(p.path) / "cyclone").string()));
#ifdef CYCLONE_KV_GPU_HAVE_LMDB
    stores.push_back(std::make_unique<LmdbStore>(
        (std::filesystem::path(p.path) / "lmdb").string()));
#endif
    stores.push_back(std::make_unique<FiledirStore>(
        (std::filesystem::path(p.path) / "filedir").string()));

    for (auto &st : stores) {
      run_store(st.get(), ds, bs, p.seconds, &heap, &readback, &all_rows);
    }

    stores.clear();
    cygpu_free_buffers();
  }

  print_table(all_rows);
  std::printf("\n--- JSON lines ---\n");
  emit_json(all_rows, std::cout);
  if (!p.output.empty()) {
    std::ofstream out(p.output, std::ios::trunc);
    emit_json(all_rows, out);
    std::printf("\nJSON lines written to %s\n", p.output.c_str());
  }
  cygpu_shutdown();
  if (const size_t failed = count_failed(all_rows); failed != 0) {
    std::fprintf(stderr, "%zu row(s) failed the correctness check\n", failed);
    return 1;
  }
  return 0;
}
