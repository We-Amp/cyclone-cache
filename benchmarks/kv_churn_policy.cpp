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
//   stripe fifo FIFO per stripe, 16 stripes routed by key hash
//   wrap flush  Cyclone today: per stripe, a wrap toggles the directory
//               phase and every entry of the previous pass stops resolving at
//               once (Volume::evict_if_needed), so the stripe restarts empty
//   retain/N    the wrap-retention proposal (doc/design/wrap-retention.md):
//               per stripe, the previous pass stays readable until a clean
//               frontier that advances in N fixed chunks per stripe reaches
//               it.  The writer advances one chunk early, as soon as less
//               than half a chunk of cleaned runway is left, so on average
//               about one chunk per stripe is dead (cleaned, not yet
//               refilled).  One chunk per stripe degenerates to wrap
//               flush; one-block chunks approach stripe FIFO.
//
// Usage: kv_churn_policy [block_size] [capacity] [universe] [pattern]
// (defaults 2 MiB, 16 GiB, 3 x capacity / block_size, both patterns).
// Thread 0's stream, warm-up of 2 x capacity inserts, then 400 000 gets.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

constexpr size_t kStripes = 16;
constexpr uint64_t kMeasuredGets = 400000;
// Per-document overhead on the Cyclone side (document header + 64 B
// metadata), rounded up.
constexpr size_t kCycloneDocOverhead = 264;

size_t stripe_of(uint64_t index) {
  const auto d = churn_key(index).digest();
  uint32_t h = 0;
  std::memcpy(&h, d.data() + 8, sizeof(h));
  return h % kStripes;
}

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

// The wrap-retention proposal, on a ring of `slots` equal-sized blocks.  W is
// the write cursor, F the clean frontier (W <= F): [0, W) holds this pass,
// [W, F) is cleaned runway nothing resolves into, [F, slots) still holds the
// previous pass and stays readable.  Advancing F by one chunk evicts the
// previous-pass blocks in that chunk; a wrap restarts both at 0, turning
// this pass into the previous one.
class FrontierRetain {
 public:
  FrontierRetain(size_t slots, size_t chunks)
      : _slots(slots),
        _chunk((slots + chunks - 1) / chunks),
        _ring(slots, kEmpty) {}
  bool access(uint64_t k) {
    if (_resident.count(k) != 0) return true;
    if (_w == _slots) {  // wrap: W == slots implies F == slots
      _w = 0;
      _f = 0;
    }
    if (_w == _f) advance();
    _ring[_w++] = k;
    _resident.insert(k);
    // Early advance: keep at least half a chunk of cleaned runway, so a
    // writer rarely has to run the reader-exclusion gate for the very
    // document it is placing.
    if (_f < _slots && _f - _w < _chunk / 2 + 1) advance();
    return false;
  }

 private:
  static constexpr uint64_t kEmpty = UINT64_MAX;
  void advance() {
    const size_t next = std::min(_f + _chunk, _slots);
    for (size_t i = _f; i < next; ++i) {
      if (_ring[i] != kEmpty) _resident.erase(_ring[i]);
      _ring[i] = kEmpty;
    }
    _f = next;
  }
  size_t _slots;
  size_t _chunk;
  std::vector<uint64_t> _ring;
  std::unordered_set<uint64_t> _resident;
  size_t _w = 0;
  size_t _f = 0;
};

constexpr std::array<size_t, 3> kRetainChunks = {16, 64, 256};

void run(size_t block, size_t capacity, size_t universe, bool scan) {
  const size_t lru_blocks = capacity / (block + kMetaSize);
  const size_t stripe_blocks =
      capacity / kStripes / (block + kCycloneDocOverhead);
  const auto perm = churn_permutation(universe);
  ChurnStream stream(perm, 0, scan);
  Lru lru(lru_blocks);
  Fifo fifo(lru_blocks);
  std::vector<Fifo> sfifo(kStripes, Fifo(stripe_blocks));
  std::vector<WrapFlush> flush(kStripes, WrapFlush(stripe_blocks));
  std::array<std::vector<FrontierRetain>, kRetainChunks.size()> retain;
  for (size_t j = 0; j < kRetainChunks.size(); ++j) {
    retain[j].assign(kStripes, FrontierRetain(stripe_blocks, kRetainChunks[j]));
  }
  const uint64_t warm_inserts = 2 * capacity / block;
  uint64_t inserts = 0;
  uint64_t gets = 0;
  std::array<uint64_t, 4 + kRetainChunks.size()> hits{};
  while (gets < kMeasuredGets) {
    const uint64_t k = stream.next();
    const bool measure = inserts >= warm_inserts;
    const size_t s = stripe_of(k);
    std::array<bool, 4 + kRetainChunks.size()> hit = {
        lru.access(k), fifo.access(k), sfifo[s].access(k), flush[s].access(k)};
    for (size_t j = 0; j < kRetainChunks.size(); ++j) {
      hit[4 + j] = retain[j][s].access(k);
    }
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
      "fifo %.4f stripe-fifo %.4f wrap-flush %.4f",
      scan ? "zipf+scan" : "zipf", block, universe, lru_blocks, stripe_blocks,
      kStripes, ratio(0), ratio(1), ratio(2), ratio(3));
  for (size_t j = 0; j < kRetainChunks.size(); ++j) {
    std::printf(" retain/%zu %.4f", kRetainChunks[j], ratio(4 + j));
  }
  std::printf("\n");
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
