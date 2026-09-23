// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Does Cyclone's zero-copy read pay off for a GPU transfer? (Metal, Apple
// silicon.)
//
// For a KV block that is ALREADY in the store -- written, read once, every
// page touched -- how fast can it reach GPU-private memory?  The paths
// measured, all moving the same bytes to the same destination:
//
//   zerocopy             the borrowed mapping is wrapped per block with
//                        -[MTLDevice newBufferWithBytesNoCopy:...] and
//                        blitted straight to a private MTLBuffer.  Measures
//                        "wrap whatever span the read handed back".
//   zerocopy-persistent  the store's WHOLE MAP_SHARED region is wrapped ONCE
//                        and each block is a sourceOffset into it.  Measures
//                        "the cache file is one persistent GPU-visible
//                        mapping" -- the decisive variant.
//   staged               memcpy the borrowed bytes into a persistent shared
//                        MTLBuffer, then blit.  The conventional path.
//   memcpy               the copy alone, no GPU at all; kv_bench's `copy`
//                        access mode, as a reference cost.
//   submit-only-0B       an empty command buffer, committed and waited on.
//   blit-4KiB            a blit encoder moving 4 KiB.  Together with
//                        submit-only-0B this is the fixed cost of the Metal
//                        path, which at batch 1 dominates everything else.
//
// Each path also runs batched (kBatchBlocks blocks per command buffer, one
// waitUntilCompleted), reported per-block amortized.
//
// Stores: cyclone, lmdb (the other zero-copy store, optional at build time)
// and filedir-pread (the no-borrowed-buffer baseline: staged and memcpy
// only).
//
// The experiment isolates the TRANSFER, not first touch.  Every block is
// written, then read once and page-touched, before any timing starts -- CRC
// verification, directory faults and page faults are all paid up front.
// Blocks come from benchmarks/kv_workload.hpp, so they are bit-identical to
// kv_bench's: keys are SHA-256("prefix-<i>"), values are xorshift64* seeded
// with the block index, metadata is the 64-byte "digest twice" header.
//
// CAVEAT -- what "GPU-private" means on unified memory: MTLStorageModePrivate
// on Apple silicon is STILL system DRAM, not a discrete VRAM aperture, and
// there is no PCIe bus in the path.  A blit to a private buffer therefore
// measures (a) a real DMA-engine copy between two regions of the same
// physical memory and (b) the driver work to make the source page range
// GPU-addressable -- which is exactly the cost newBufferWithBytesNoCopy pays
// per block on the `zerocopy` path and pays once on `zerocopy-persistent`.
// That is the honest cost of "get these bytes into a resource the GPU owns
// and the CPU cannot touch", which is what a KV-cache offload tier has to do
// before a kernel reads them.  It is NOT a host-to-device upload
// measurement and must not be compared to a discrete-GPU PCIe figure.  On a
// discrete GPU the staged path would additionally pay a real bus transfer,
// widening the gap in favour of any path that avoids a host copy.
//
// Run it:
//   ./build/kv_gpu_metal --block-size 2097152 --seconds 10

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
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
#include <vector>

#include "cyclone/cache.hpp"
#include "cyclone/config.hpp"
#include "cyclone/error.hpp"
#include "cyclone/handle.hpp"
#include "cyclone/key.hpp"
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

constexpr size_t kBatchBlocks = 16;                 // blocks per command buffer
constexpr size_t kBlocks = 512;                     // N
constexpr size_t kVolumeBytes = size_t{8} << 30;    // 8 GiB
constexpr size_t kLmdbMapBytes = size_t{16} << 30;  // 16 GiB
constexpr double kWarmupSeconds = 2.0;
constexpr double kDeviceWarmupSeconds = 1.0;
constexpr const char *kVolumeStem = "kv_gpu_metal_cyclone";

// The HOST page size, filled in from getpagesize() at startup.  Apple silicon
// is 16 KiB, not 4 KiB, and newBufferWithBytesNoCopy insists on a page-aligned
// base -- so this must never be hardcoded.  Distinct from the workload's
// kPageSize (4096), which is only the `view` touch stride.
size_t g_page_size = 4096;

id<MTLDevice> g_device = nil;
id<MTLCommandQueue> g_queue = nil;
id<MTLBuffer> g_private = nil;   // kBatchBlocks * bs, StorageModePrivate
id<MTLBuffer> g_stage = nil;     // kBatchBlocks * bs, StorageModeShared
id<MTLBuffer> g_readback = nil;  // bs, StorageModeShared

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

// ------------------------------------------------------------ machine info

std::string sysctl_str(const char *name) {
  size_t len = 0;
  if (sysctlbyname(name, nullptr, &len, nullptr, 0) != 0 || len == 0) {
    return "?";
  }
  std::string v(len, '\0');
  if (sysctlbyname(name, v.data(), &len, nullptr, 0) != 0) return "?";
  while (!v.empty() && (v.back() == '\0' || v.back() == '\n')) v.pop_back();
  return v;
}

uint64_t sysctl_u64(const char *name) {
  uint64_t v = 0;
  size_t len = sizeof(v);
  if (sysctlbyname(name, &v, &len, nullptr, 0) != 0) return 0;
  return v;
}

// -------------------------------------------------------------- the stores

// One block source.  `acquire` hands out a borrowed pointer that stays valid
// until the matching `release(slot)`; `read_into` copies the block into the
// caller's destination (the staging MTLBuffer, or a heap buffer).  A store
// with no borrowed-read API (filedir-pread) implements read_into only.
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
           "benchmarks/kv_bench.cpp";
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

  // Extra wrap-acceptance probe surfaces (A2 / A3), used once at startup.
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

  auto t0 = Clock::now();
  while (secs_since(t0) < warmup) {
    if (!unit(i, batch)) {
      out.supported = false;
      out.note = "transfer failed during warm-up";
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
      out.note = "transfer failed during measurement";
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

// Blit private slot 0 back through the shared readback buffer and checksum it:
// the CPU cannot read a private buffer, so the check has to round-trip.
uint64_t checksum_private_slot0(size_t bs) {
  @autoreleasepool {
    id<MTLCommandBuffer> cb = [g_queue commandBuffer];
    id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
    [bl copyFromBuffer:g_private
             sourceOffset:0
                 toBuffer:g_readback
        destinationOffset:0
                     size:bs];
    [bl endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
  }
  return fnv1a(static_cast<const uint8_t *>(g_readback.contents), bs);
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
  std::printf("\n| store | path | batch | blocks/s | GB/s | p50 us | p99 us | "
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

// ------------------------------------------------------- no-copy wrap probe

// Answers the load-bearing question: will Metal wrap a file-backed MAP_SHARED
// mapping without copying it?  "accepted" alone is not enough -- contents must
// come back EQUAL to the mapping base (otherwise Metal took a copy) and the
// blit must reach MTLCommandBufferStatusCompleted.
struct WrapProbe {
  bool accepted = false;
  bool contents_match = false;
  bool blit_ok = false;
  std::string detail;
};

WrapProbe probe_wrap(const void *base, size_t map_len, size_t src_off,
                     size_t copy_len) {
  WrapProbe p;
  @autoreleasepool {
    id<MTLBuffer> src = nil;
    // newBufferWithBytesNoCopy raises an ObjC exception (it does not return
    // nil) when the pointer is not page-aligned, so the probe has to catch.
    @try {
      src = [g_device newBufferWithBytesNoCopy:const_cast<void *>(base)
                                        length:map_len
                                       options:MTLResourceStorageModeShared
                                   deallocator:nil];
    } @catch (NSException *e) {
      p.detail = std::string("NSException ") + e.name.UTF8String + ": " +
                 e.reason.UTF8String;
      return p;
    }
    if (src == nil) {
      p.detail = "newBufferWithBytesNoCopy returned nil";
      return p;
    }
    p.accepted = true;
    p.contents_match = (src.contents == base);
    id<MTLCommandBuffer> cb = [g_queue commandBuffer];
    id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
    [bl copyFromBuffer:src
             sourceOffset:src_off
                 toBuffer:g_private
        destinationOffset:0
                     size:copy_len];
    [bl endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    p.blit_ok = (cb.status == MTLCommandBufferStatusCompleted);
    p.detail =
        "accepted; contents " + std::string(p.contents_match ? "== " : "!= ") +
        "mapping base; blit status " +
        (p.blit_ok ? "completed"
                   : std::string("FAILED: ") +
                         (cb.error ? cb.error.localizedDescription.UTF8String
                                   : "unknown"));
  }
  return p;
}

// Self-contained probe: our own MAP_SHARED file mapping, PROT_READ|PROT_WRITE
// and PROT_READ, to answer the question independently of Cyclone.  The
// read-ONLY case is the surprising one: MTLResourceStorageModeShared is
// nominally CPU-writable, yet a PROT_READ file mapping is accepted too.
void probe_synthetic(const std::string &dir, size_t bs) {
  const std::string p =
      (std::filesystem::path(dir) / "wrap_probe.bin").string();
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
    const auto r = probe_wrap(rw, bs, 0, bs);
    std::printf("  mmap(MAP_SHARED, PROT_READ|PROT_WRITE) : %s\n",
                r.detail.c_str());
    ::munmap(rw, bs);
  }
  void *ro = ::mmap(nullptr, bs, PROT_READ, MAP_SHARED, fd, 0);
  if (ro != MAP_FAILED) {
    const auto r = probe_wrap(ro, bs, 0, bs);
    std::printf("  mmap(MAP_SHARED, PROT_READ)            : %s\n",
                r.detail.c_str());
    ::munmap(ro, bs);
  }
  ::close(fd);
  std::error_code ec;
  std::filesystem::remove(p, ec);
}

// The three Cyclone surfaces an embedder could hand to Metal.  A1 is the one
// the benchmark paths use; A2 and A3 are kept as a regression check, because
// acceptance is a macOS/driver property that could change.
void probe_cyclone_surfaces(CycloneStore *cyc, size_t bs) {
  std::printf("  zero-copy wrap surfaces for cyclone:\n");

  // A1: the borrowed content() span, wrapped from the page below it.
  const uint8_t *d = nullptr;
  size_t l = 0;
  if (cyc->acquire(0, 0, d, l)) {
    const uintptr_t ptr = reinterpret_cast<uintptr_t>(d);
    const uintptr_t base = page_base(ptr);
    const size_t off = static_cast<size_t>(ptr - base);
    const auto r = probe_wrap(reinterpret_cast<void *>(base),
                              round_up(off + l, g_page_size), off, l);
    // content() sits behind the 132-byte document header, so it is never page
    // aligned; the wrap is taken at the page below and the blit uses
    // sourceOffset, which the encoder needs 4-byte aligned.
    std::printf("    A1 content() page-aligned range      : %s\n"
                "       (content ptr %% page = %zu, %% 4 = %zu)\n",
                r.detail.c_str(), off, off % 4);
    cyc->release(0);
  }

  // A2: the whole document view, header included.
  if (auto mv = cyc->mapped_view_of(0); mv.has_value()) {
    const uintptr_t ptr = reinterpret_cast<uintptr_t>(mv->data());
    const uintptr_t base = page_base(ptr);
    const size_t off = static_cast<size_t>(ptr - base);
    const auto r =
        probe_wrap(reinterpret_cast<void *>(base),
                   round_up(off + mv->size(), g_page_size), off, mv->size());
    std::printf("    A2 mapped_view() page-aligned range  : %s\n"
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
        const auto r = probe_wrap(m, mlen, aoff, bs);
        std::printf("    A3 own mmap(MAP_SHARED, PROT_READ)   : %s\n"
                    "       (volume file %s, content_file_offset %llu)\n",
                    r.detail.c_str(), cyc->volume_file().c_str(),
                    static_cast<unsigned long long>(fo));
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

// --------------------------------------------------------------------- CLI

struct Params {
  std::vector<size_t> block_sizes;
  double seconds = 10.0;
  std::string path = "./gpudata";
  std::string output;
};

void usage() {
  std::printf(
      "kv_gpu_metal -- does Cyclone's zero-copy read pay off for a GPU "
      "transfer?\n\n"
      "  --block-size N   value size in bytes (repeatable; default 2097152)\n"
      "  --seconds S      measured window per path (default 10)\n"
      "  --path DIR       store data directory (default ./gpudata)\n"
      "  --output FILE    write the JSON lines to FILE as well as stdout\n"
      "  --help\n");
}

void print_machine_info(const Params &p) {
  std::printf("=== machine ===\n");
  std::printf("chip         : %s (%llu physical / %llu logical)\n",
              sysctl_str("machdep.cpu.brand_string").c_str(),
              static_cast<unsigned long long>(sysctl_u64("hw.physicalcpu")),
              static_cast<unsigned long long>(sysctl_u64("hw.logicalcpu")));
  std::printf("memory       : %llu GiB\n",
              static_cast<unsigned long long>(sysctl_u64("hw.memsize") >> 30));
  std::printf("os           : macOS %s (%s)\n",
              sysctl_str("kern.osproductversion").c_str(),
              sysctl_str("kern.osversion").c_str());
  std::printf("metal device : %s\n", g_device.name.UTF8String);
  std::printf(
      "unified mem  : %s (max buffer %.1f GiB)\n",
      g_device.hasUnifiedMemory ? "yes" : "no",
      static_cast<double>(g_device.maxBufferLength) / (1024.0 * 1024 * 1024));
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
  std::printf(
      "NOTE: on unified memory MTLStorageModePrivate is still system DRAM.\n"
      "A blit to a private buffer is a real DMA-engine copy between two\n"
      "regions of the same physical memory plus the driver work to make the\n"
      "source range GPU-addressable -- not a PCIe upload.\n");
}

// The fixed cost of the Metal path: an empty command buffer, and a blit
// encoder moving 4 KiB.  At batch 1 this dominates every storage difference,
// which is the first conclusion of the experiment.
void measure_metal_fixed_cost(double seconds, size_t bs,
                              std::vector<Row> *rows) {
  // Bring the GPU up to clock first: a cold device reports a blit fixed cost
  // that swings by 2x between runs.
  const auto t_warm = Clock::now();
  while (secs_since(t_warm) < kDeviceWarmupSeconds) {
    @autoreleasepool {
      id<MTLCommandBuffer> cb = [g_queue commandBuffer];
      id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
      [bl copyFromBuffer:g_stage
               sourceOffset:0
                   toBuffer:g_private
          destinationOffset:0
                       size:bs];
      [bl endEncoding];
      [cb commit];
      [cb waitUntilCompleted];
    }
  }

  UnitFn submit_only = [](size_t, size_t) {
    @autoreleasepool {
      id<MTLCommandBuffer> cb = [g_queue commandBuffer];
      [cb commit];
      [cb waitUntilCompleted];
    }
    return true;
  };
  UnitFn blit_4k = [](size_t, size_t) {
    @autoreleasepool {
      id<MTLCommandBuffer> cb = [g_queue commandBuffer];
      id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
      [bl copyFromBuffer:g_stage
               sourceOffset:0
                   toBuffer:g_private
          destinationOffset:0
                       size:4096];
      [bl endEncoding];
      [cb commit];
      [cb waitUntilCompleted];
    }
    return true;
  };
  ChecksumFn always_ok = []() { return uint64_t{0}; };

  auto a = run_path(kBlocks, 0, 1, seconds, submit_only, always_ok);
  auto b = run_path(kBlocks, 4096, 1, seconds, blit_4k, always_ok);
  std::printf(
      "\n--- fixed cost of the Metal path (submission latency) ---\n"
      "  empty command buffer (0 B)  : %.1f us p50, %.1f us p99, %.0f/s\n"
      "  4 KiB blit shared->private  : %.1f us p50, %.1f us p99, %.0f/s\n",
      a.p50_us, a.p99_us, a.ops_per_s, b.p50_us, b.p99_us, b.ops_per_s);
  a.correct = true;
  b.correct = true;
  rows->push_back(Row{"metal", "submit-only-0B", 1, 0, a});
  rows->push_back(Row{"metal", "blit-4KiB", 1, 4096, b});
}

// Everything measured for one store at one block size.
void run_store(GpuStore *store, const Dataset &ds, size_t bs, double seconds,
               std::vector<uint8_t> *heap, std::vector<Row> *all_rows) {
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
  // persistent-wrap variant needs.
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

  // Persistent wrap of the whole mapping.  The per-block wrapper below pays
  // newBufferWithBytesNoCopy on every transfer; a store whose blocks all live
  // inside ONE MAP_SHARED region can instead wrap that region ONCE and blit
  // with a per-block sourceOffset.  The wrap cost then amortizes to zero and
  // the transfer is genuinely copy-free.
  id<MTLBuffer> span_buf = nil;
  uintptr_t span_base = 0;
  size_t span_len = 0;
  if (store->has_borrowed_read() && lo != UINTPTR_MAX) {
    span_base = page_base(lo);
    span_len = round_up(static_cast<size_t>(hi - span_base), g_page_size);
    if (span_len > static_cast<size_t>(g_device.maxBufferLength)) {
      // A cache larger than maxBufferLength would need the mapping split into
      // several wrapped windows.
      std::printf(
          "  persistent wrap: dataset span %.2f GiB exceeds maxBufferLength "
          "-- not available\n",
          static_cast<double>(span_len) / (1024.0 * 1024 * 1024));
    } else {
      @try {
        span_buf = [g_device
            newBufferWithBytesNoCopy:reinterpret_cast<void *>(span_base)
                              length:span_len
                             options:MTLResourceStorageModeShared
                         deallocator:nil];
      } @catch (NSException *e) {
        std::printf("  persistent wrap: NSException %s: %s\n",
                    e.name.UTF8String, e.reason.UTF8String);
        span_buf = nil;
      }
      std::printf(
          "  persistent wrap: %s (%.2f GiB of the store's mapping wrapped "
          "once)\n",
          span_buf ? "accepted" : "REFUSED",
          static_cast<double>(span_len) / (1024.0 * 1024 * 1024));
    }
  }

  const size_t n = ds.keys.size();

  // Keeps per-block wrapper buffers alive until waitUntilCompleted returns.
  std::vector<id<MTLBuffer>> keep;
  keep.reserve(kBatchBlocks);

  UnitFn unit_zerocopy = [&](size_t start, size_t count) {
    bool ok = true;
    size_t got = 0;
    @autoreleasepool {
      id<MTLCommandBuffer> cb = [g_queue commandBuffer];
      id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
      for (size_t k = 0; k < count; ++k) {
        const uint8_t *d = nullptr;
        size_t l = 0;
        if (!store->acquire(k, (start + k) % n, d, l) || l != bs) {
          store->release(k);
          ok = false;
          break;
        }
        const uintptr_t ptr = reinterpret_cast<uintptr_t>(d);
        const uintptr_t base = page_base(ptr);
        const size_t off = static_cast<size_t>(ptr - base);
        const size_t map_len = round_up(off + l, g_page_size);
        id<MTLBuffer> src =
            [g_device newBufferWithBytesNoCopy:reinterpret_cast<void *>(base)
                                        length:map_len
                                       options:MTLResourceStorageModeShared
                                   deallocator:nil];
        if (src == nil) {
          store->release(k);
          ok = false;
          break;
        }
        keep.push_back(src);
        [bl copyFromBuffer:src
                 sourceOffset:off
                     toBuffer:g_private
            destinationOffset:k * bs
                         size:l];
        got = k + 1;
      }
      [bl endEncoding];
      [cb commit];
      [cb waitUntilCompleted];
      for (size_t k = 0; k < got; ++k) store->release(k);
      keep.clear();
    }
    return ok;
  };

  UnitFn unit_zerocopy_persist = [&](size_t start, size_t count) {
    bool ok = true;
    size_t got = 0;
    @autoreleasepool {
      id<MTLCommandBuffer> cb = [g_queue commandBuffer];
      id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
      for (size_t k = 0; k < count; ++k) {
        const uint8_t *d = nullptr;
        size_t l = 0;
        if (!store->acquire(k, (start + k) % n, d, l) || l != bs) {
          store->release(k);
          ok = false;
          break;
        }
        const uintptr_t ptr = reinterpret_cast<uintptr_t>(d);
        if (ptr < span_base || (ptr - span_base) + l > span_len) {
          store->release(k);  // moved out of the wrapped span
          ok = false;
          break;
        }
        [bl copyFromBuffer:span_buf
                 sourceOffset:static_cast<NSUInteger>(ptr - span_base)
                     toBuffer:g_private
            destinationOffset:k * bs
                         size:l];
        got = k + 1;
      }
      [bl endEncoding];
      [cb commit];
      [cb waitUntilCompleted];
      for (size_t k = 0; k < got; ++k) store->release(k);
    }
    return ok;
  };

  UnitFn unit_staged = [&](size_t start, size_t count) {
    bool ok = true;
    size_t got = 0;
    @autoreleasepool {
      auto *sbase = static_cast<uint8_t *>(g_stage.contents);
      for (size_t k = 0; k < count; ++k) {
        if (!store->read_into((start + k) % n, sbase + k * bs, bs)) {
          ok = false;
          break;
        }
        got = k + 1;
      }
      id<MTLCommandBuffer> cb = [g_queue commandBuffer];
      id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
      for (size_t k = 0; k < got; ++k) {
        [bl copyFromBuffer:g_stage
                 sourceOffset:k * bs
                     toBuffer:g_private
            destinationOffset:k * bs
                         size:bs];
      }
      [bl endEncoding];
      [cb commit];
      [cb waitUntilCompleted];
    }
    return ok;
  };

  UnitFn unit_memcpy = [&](size_t start, size_t count) {
    for (size_t k = 0; k < count; ++k) {
      if (!store->read_into((start + k) % n, heap->data(), bs)) return false;
    }
    return true;
  };

  ChecksumFn gpu_checksum = [bs]() { return checksum_private_slot0(bs); };
  ChecksumFn heap_checksum = [&]() { return fnv1a(heap->data(), bs); };

  if (auto *cyc = dynamic_cast<CycloneStore *>(store)) {
    probe_cyclone_surfaces(cyc, bs);
  }

  std::vector<Row> rows;
  const std::string sname = store->name();
  for (size_t batch : {size_t{1}, kBatchBlocks}) {
    if (store->has_borrowed_read()) {
      rows.push_back(
          {sname, "zerocopy", batch, bs,
           run_path(n, bs, batch, seconds, unit_zerocopy, gpu_checksum)});
    }
    if (span_buf != nil) {
      rows.push_back({sname, "zerocopy-persistent", batch, bs,
                      run_path(n, bs, batch, seconds, unit_zerocopy_persist,
                               gpu_checksum)});
    }
    rows.push_back(
        {sname, "staged", batch, bs,
         run_path(n, bs, batch, seconds, unit_staged, gpu_checksum)});
    // memcpy has no GPU submission to amortize, so batching it would measure
    // nothing new.
    if (batch == 1) {
      rows.push_back(
          {sname, "memcpy", batch, bs,
           run_path(n, bs, batch, seconds, unit_memcpy, heap_checksum)});
    }
  }

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

  @autoreleasepool {
    g_device = MTLCreateSystemDefaultDevice();
    if (g_device == nil) {
      std::fprintf(stderr, "no Metal device\n");
      return 1;
    }
    g_queue = [g_device newCommandQueue];

    print_machine_info(p);

    const Dataset ds = build_dataset(kBlocks);
    std::vector<Row> all_rows;

    for (size_t bs : p.block_sizes) {
      std::printf("\n=== block size %zu bytes (%.1f MiB), N = %zu ===\n", bs,
                  static_cast<double>(bs) / (1024.0 * 1024), kBlocks);

      g_private = [g_device newBufferWithLength:kBatchBlocks * bs
                                        options:MTLResourceStorageModePrivate];
      g_stage = [g_device newBufferWithLength:kBatchBlocks * bs
                                      options:MTLResourceStorageModeShared];
      g_readback = [g_device newBufferWithLength:bs
                                         options:MTLResourceStorageModeShared];
      if (g_private == nil || g_stage == nil || g_readback == nil) {
        std::fprintf(stderr, "MTLBuffer allocation failed\n");
        return 1;
      }

      std::printf(
          "\n--- newBufferWithBytesNoCopy on a file-backed mapping ---\n");
      probe_synthetic(p.path, bs);

      measure_metal_fixed_cost(p.seconds, bs, &all_rows);

      std::vector<uint8_t> heap(bs);
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
        run_store(st.get(), ds, bs, p.seconds, &heap, &all_rows);
      }

      stores.clear();
      g_private = nil;
      g_stage = nil;
      g_readback = nil;
    }

    print_table(all_rows);
    std::printf("\n--- JSON lines ---\n");
    emit_json(all_rows, std::cout);
    if (!p.output.empty()) {
      std::ofstream out(p.output, std::ios::trunc);
      emit_json(all_rows, out);
      std::printf("\nJSON lines written to %s\n", p.output.c_str());
    }
    if (const size_t failed = count_failed(all_rows); failed != 0) {
      std::fprintf(stderr, "%zu row(s) failed the correctness check\n", failed);
      return 1;
    }
  }
  return 0;
}
