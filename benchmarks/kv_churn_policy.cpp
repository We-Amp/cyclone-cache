// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Eviction-policy-only replay of the churn v1 streams
// (doc/kv-cache-benchmark/kv-churn-spec.md): no store, no I/O, just which
// gets would hit under each policy at the same capacity in blocks.  It
// separates what a store's eviction POLICY costs in hit ratio from what its
// implementation costs in time.
//
//   lru         what the LMDB / filedir harness implements
//   fifo        one global FIFO
//   stripe fifo FIFO per stripe, keys routed as Volume::select_stripe does
//               (segment_hash() % stripe count; count from the auto geometry)
//   wrap flush  Cyclone today: per stripe, a wrap toggles the directory
//               phase and every entry of the previous pass stops resolving at
//               once (Volume::evict_if_needed), so the stripe restarts empty
//
// Usage: kv_churn_policy [block_size] [capacity] [universe] [pattern]
// (defaults 2 MiB, 16 GiB, 3 x capacity / block_size, both patterns).
// Thread 0's stream, warm-up of 2 x capacity inserts, then 400 000 gets.

#include <array>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <list>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "cyclone/key.hpp"
#include "kv_workload.hpp"

using namespace cyclone;
using namespace cyclone::kv_workload;

namespace {

constexpr uint64_t kMeasuredGets = 400000;
// Per-document overhead on the Cyclone side (document header + 64 B
// metadata), rounded up.
constexpr size_t kCycloneDocOverhead = 264;

class Lru {
 public:
  explicit Lru(size_t cap) : _cap(cap) {}
  bool access(uint64_t k) {
    auto it = _map.find(k);
    if (it != _map.end()) {
      _list.splice(_list.begin(), _list, it->second);
      return true;
    }
    _list.push_front(k);
    _map[k] = _list.begin();
    if (_list.size() > _cap) {
      _map.erase(_list.back());
      _list.pop_back();
    }
    return false;
  }

 private:
  size_t _cap;
  std::list<uint64_t> _list;
  std::unordered_map<uint64_t, std::list<uint64_t>::iterator> _map;
};

class Fifo {
 public:
  explicit Fifo(size_t cap) : _cap(cap) {}
  bool access(uint64_t k) {
    if (_set.count(k) != 0) return true;
    _queue.push_back(k);
    _set.insert(k);
    if (_queue.size() > _cap) {
      _set.erase(_queue.front());
      _queue.pop_front();
    }
    return false;
  }

 private:
  size_t _cap;
  std::deque<uint64_t> _queue;
  std::unordered_set<uint64_t> _set;
};

class WrapFlush {
 public:
  explicit WrapFlush(size_t cap) : _cap(cap) {}
  bool access(uint64_t k) {
    if (_set.count(k) != 0) return true;
    if (_set.size() == _cap) _set.clear();  // the wrap
    _set.insert(k);
    return false;
  }

 private:
  size_t _cap;
  std::unordered_set<uint64_t> _set;
};

void run(size_t block, size_t capacity, size_t universe, bool scan) {
  // kv_churn sizes the volume so the stripes' data areas sum to `capacity`;
  // the mmap directories and volume header it adds on top are small next to
  // the 32 MiB rounding step, so the count is taken from `capacity` (kv_churn
  // prints the count its volume actually got).
  const size_t stripes = churn_stripe_count(capacity);
  const size_t lru_blocks = capacity / (block + kMetaSize);
  const size_t stripe_blocks =
      capacity / stripes / (block + kCycloneDocOverhead);
  const auto perm = churn_permutation(universe);
  ChurnStream stream(perm, 0, scan);
  Lru lru(lru_blocks);
  Fifo fifo(lru_blocks);
  std::vector<Fifo> sfifo(stripes, Fifo(stripe_blocks));
  std::vector<WrapFlush> flush(stripes, WrapFlush(stripe_blocks));
  const uint64_t warm_inserts = 2 * capacity / block;
  uint64_t inserts = 0;
  uint64_t gets = 0;
  std::array<uint64_t, 4> hits{};
  while (gets < kMeasuredGets) {
    const uint64_t k = stream.next();
    const bool measure = inserts >= warm_inserts;
    const size_t s = churn_stripe_of(k, stripes);
    const std::array<bool, 4> hit = {lru.access(k), fifo.access(k),
                                     sfifo[s].access(k), flush[s].access(k)};
    if (!hit[0]) ++inserts;
    if (!measure) continue;
    ++gets;
    for (size_t i = 0; i < hit.size(); ++i) hits[i] += hit[i] ? 1 : 0;
  }
  auto ratio = [&](size_t i) {
    return static_cast<double>(hits[i]) / static_cast<double>(gets);
  };
  std::printf(
      "%-9s block=%zu U=%zu capacity=%zu blocks (%zu/stripe x %zu): lru %.4f "
      "fifo %.4f stripe-fifo %.4f wrap-flush %.4f\n",
      scan ? "zipf+scan" : "zipf", block, universe, lru_blocks, stripe_blocks,
      stripes, ratio(0), ratio(1), ratio(2), ratio(3));
}

}  // namespace

int main(int argc, char* argv[]) {
  const size_t block = argc > 1 ? std::stoull(argv[1]) : 2097152;
  const size_t capacity =
      argc > 2 ? std::stoull(argv[2]) : size_t{16} * 1024 * 1024 * 1024;
  const size_t universe = argc > 3 && std::stoull(argv[3]) > 0
                              ? std::stoull(argv[3])
                              : 3 * capacity / block;
  const std::string pattern = argc > 4 ? argv[4] : "";
  for (const bool scan : {false, true}) {
    if (!pattern.empty() && pattern != (scan ? "zipf+scan" : "zipf")) {
      continue;
    }
    run(block, capacity, universe, scan);
  }
  return 0;
}
