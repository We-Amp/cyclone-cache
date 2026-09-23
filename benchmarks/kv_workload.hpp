// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The KV-cache storage-tier workload, in one place.
//
// This header IS the "KV-cache storage-tier workload spec (v1)" as Cyclone
// implements it: key derivation, value generator, metadata header, Zipf access
// distribution, dataset sizing, access modes and the latency percentile
// idiom.  Every benchmark in this repository that claims to run "the KV
// workload" includes it, so two harnesses in the same tree cannot drift apart
// silently -- which is the whole point of the cross-harness reference vectors
// printed by `kv_bench --print-vectors`.
//
// Consumers today:
//   benchmarks/kv_bench.cpp       the five-phase storage-tier sweep
//   benchmarks/kv_gpu_metal.mm    the Apple-silicon GPU transfer experiment
//   benchmarks/kv_churn.cpp       bounded-capacity churn (kv-churn-spec.md)
//
// Header-only and deliberately free of Cyclone internals: it uses the public
// CacheKey only, so a peer harness can be diffed against it line by line.
//
// If you change ANYTHING here, `kv_bench --print-vectors` changes with it and
// the numbers stop being comparable to previously published runs.  The vectors
// it prints are the tripwire:
//
//   key prefix-0     = d0f3364b...
//   value[0][0..15]  = 76 3b 8d 94 ...
//   zipf(2048, 42)   = 8 12 44 22 6 0 0 38 28 389

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include "cyclone/key.hpp"

namespace cyclone::kv_workload {

// Stride of the `view` access mode: one byte per 4 KiB page of the returned
// span.  This is a WORKLOAD constant, not a property of the host -- it stays
// 4096 even where the OS page is larger (Apple silicon: 16 KiB), because the
// peer harnesses touch the same stride and the count of touched bytes has to
// match across implementations.
inline constexpr size_t kPageSize = 4096;

// Spec metadata header: the 32-byte digest, twice.
inline constexpr size_t kMetaSize = 64;

// Dataset sizing: N = min(4096, 4 GiB / block_size).
inline constexpr size_t kMaxBlocks = 4096;
inline constexpr size_t kDatasetBudget = size_t{4} * 1024 * 1024 * 1024;

// Zipf skew (Gray et al. / YCSB), fixed by the spec.
inline constexpr double kZipfTheta = 0.99;

// ---------------------------------------------------------------------------
// Value generator: xorshift64* seeded with the block index (spec).
//
// SPEC RESOLUTION: "seed = i" is degenerate for i = 0 (xorshift64* is stuck at
// zero), so a zero seed -- and only a zero seed -- is replaced by the golden
// ratio constant below.  Every other block is exactly "seed = i".
// ---------------------------------------------------------------------------
inline constexpr uint64_t kZeroSeedSubstitute = 0x9E3779B97F4A7C15ULL;
inline constexpr uint64_t kXorshiftMultiplier = 0x2545F4914F6CD1DBULL;

inline uint64_t xorshift64star(uint64_t &state) {
  state ^= state >> 12;
  state ^= state << 25;
  state ^= state >> 27;
  return state * kXorshiftMultiplier;
}

inline uint64_t seed_state(uint64_t seed) {
  return seed == 0 ? kZeroSeedSubstitute : seed;
}

inline void fill_block(std::span<std::byte> out, uint64_t seed) {
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

// Byte-pointer convenience for adapters that hold raw buffers (the Metal
// experiment's store adapters).  Identical bytes: it forwards.
inline void fill_block(uint8_t *out, size_t len, uint64_t seed) {
  fill_block(std::span<std::byte>(reinterpret_cast<std::byte *>(out), len),
             seed);
}

// First 8 bytes of block `seed`, for a cheap content spot-check.
inline uint64_t first_word_of_block(uint64_t seed) {
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
// Dataset: keys and the 64-byte metadata header.
// ---------------------------------------------------------------------------
struct Dataset {
  std::vector<CacheKey> keys;
  std::vector<std::array<std::byte, kMetaSize>> metadata;
};

// Key = SHA-256("prefix-<i>").  CacheKey's string constructor IS that SHA-256,
// so the digest it produces is byte-identical to what a peer harness hashes;
// it is routed through from_digest() to make the contract explicit.
inline Dataset build_dataset(size_t n) {
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

// N = min(4096, 4 GiB / block_size), so the dataset never outgrows the budget.
inline size_t dataset_blocks(size_t block_size) {
  if (block_size == 0) return 0;
  return std::min(kMaxBlocks, kDatasetBudget / block_size);
}

// ---------------------------------------------------------------------------
// Access modes
// ---------------------------------------------------------------------------

// `view`: one byte per 4 KiB page of the borrowed span, summed.  The sum is
// returned so the caller can sink it and keep the optimizer from dropping the
// loads.
inline uint64_t touch_pages(std::span<const std::byte> data) {
  uint64_t sink = 0;
  for (size_t off = 0; off < data.size(); off += kPageSize) {
    sink += static_cast<uint64_t>(std::to_integer<uint8_t>(data[off]));
  }
  return sink;
}

// `copy`: memcpy the value into the caller's preallocated staging buffer.
inline uint64_t copy_out(std::span<const std::byte> data,
                         std::vector<std::byte> &buffer) {
  const size_t len = std::min(data.size(), buffer.size());
  std::memcpy(buffer.data(), data.data(), len);
  return len == 0 ? 0
                  : static_cast<uint64_t>(std::to_integer<uint8_t>(buffer[0]));
}

// ---------------------------------------------------------------------------
// Churn v1 (doc/kv-cache-benchmark/kv-churn-spec.md): a bounded-capacity
// tier under get-or-insert.  Key index i still means key SHA-256("prefix-<i>")
// and value xorshift64*(state i); what is new is WHICH indices are drawn.
// ---------------------------------------------------------------------------
inline constexpr uint64_t kChurnPermSeed = 0x243F6A8885A308D3ULL;
inline constexpr uint64_t kChurnChoiceSeedBase = 1000003;
inline constexpr uint64_t kChurnZipfSeedBase = 42;
inline constexpr double kChurnScanFraction = 0.1;
inline constexpr uint64_t kChurnScanBase = uint64_t{1} << 40;
inline constexpr uint64_t kChurnScanThreadStride = uint64_t{1} << 32;

// One fixed permutation of [0, u): Zipf rank r -> key index perm[r], so the
// popular keys are not numerically adjacent.
inline std::vector<uint64_t> churn_permutation(size_t u) {
  std::vector<uint64_t> perm(u);
  for (size_t i = 0; i < u; ++i) perm[i] = i;
  uint64_t state = kChurnPermSeed;
  for (size_t i = u; i > 1; --i) {
    const size_t j = static_cast<size_t>(xorshift64star(state) % i);
    std::swap(perm[i - 1], perm[j]);
  }
  return perm;
}

// Per-thread key-index stream for pattern "zipf" (scan = false) or
// "zipf+scan" (scan = true).
class ChurnStream {
 public:
  ChurnStream(const std::vector<uint64_t> &perm, uint32_t thread_id, bool scan)
      : _perm(perm),
        _zipf(perm.size(), kZipfTheta, kChurnZipfSeedBase + thread_id),
        _choice(kChurnChoiceSeedBase + thread_id),
        _scan(scan),
        _scan_base(kChurnScanBase + thread_id * kChurnScanThreadStride) {}

  uint64_t next() {
    if (_scan) {
      const double u = static_cast<double>(xorshift64star(_choice) >> 11) *
                       (1.0 / 9007199254740992.0);
      if (u < kChurnScanFraction) return _scan_base + _scan_next++;
    }
    return _perm[_zipf.next()];
  }

 private:
  const std::vector<uint64_t> &_perm;
  ZipfGenerator _zipf;
  uint64_t _choice;
  bool _scan;
  uint64_t _scan_base;
  uint64_t _scan_next = 0;
};

inline CacheKey churn_key(uint64_t index) {
  const CacheKey hashed("prefix-" + std::to_string(index));
  return CacheKey::from_digest(hashed.digest());
}

// Copies of the auto stripe-geometry constants (kAutoStripeGranularity,
// kAutoStripeTarget in src/core/volume.hpp); kv_churn static_asserts them
// against the originals and checks the stripe count its volume gets.
inline constexpr size_t kChurnAutoStripeGranularity = size_t{32} * 1024 * 1024;
inline constexpr size_t kChurnAutoStripeTarget = 16;

// Stripe count of an auto-geometry volume with `usable` bytes after the
// volume header: compute_stripe_geometry() in src/core/volume.cpp,
// clamp(round(usable / granularity), 1, target).
inline size_t churn_stripe_count(size_t usable) {
  const size_t n =
      (usable + kChurnAutoStripeGranularity / 2) / kChurnAutoStripeGranularity;
  return std::clamp<size_t>(n, 1, kChurnAutoStripeTarget);
}

// The stripe a key index lands in: Volume::select_stripe() routes by
// key.segment_hash() % stripe count.
inline size_t churn_stripe_of(uint64_t index, size_t stripes) {
  return churn_key(index).segment_hash() % stripes;
}

// ---------------------------------------------------------------------------
// Latency percentiles: sort, index = p * (n - 1) -- the same idiom as
// benchmarks/performance_baseline.cpp, so percentile columns are comparable
// across every benchmark in the tree.  Sorts `latencies_us` in place.
// ---------------------------------------------------------------------------
struct Percentiles {
  double p50 = 0.0;
  double p99 = 0.0;
  double p999 = 0.0;
};

inline Percentiles compute_percentiles(std::vector<double> &latencies_us) {
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

}  // namespace cyclone::kv_workload
