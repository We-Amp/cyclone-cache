// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#pragma once

#include <atomic>
#include <cstddef>

namespace cyclone {

// Cyclone's read path replaces shared-cache-line RMWs with per-thread
// sharded state: statistics counters, borrow-slot shards and
// read-handle anchors all pick "this thread's shard" through this one
// index so a given thread touches the SAME shard line everywhere.
//
// Assignment is round-robin at first use, not a thread::id hash: a hash
// gives random PERMANENT assignments, and by birthday statistics a
// handful of long-lived server threads would likely share a shard for the
// process lifetime.  Round-robin guarantees the spread at identical
// per-access cost.  (Same rationale as Cache's reader-gate shard.)
//
// `mod` must be a power of two so callers with different shard counts
// still map one thread to one line per array.
inline size_t thread_shard_index(size_t mod) {
  static std::atomic<size_t> next{0};
  static thread_local const size_t idx =
      next.fetch_add(1, std::memory_order_relaxed);
  return idx & (mod - 1);
}

// Destructive-interference padding for shard arrays: 128 bytes covers the
// adjacent-line prefetcher on x86 (same sizing as Cache's GateShard).
inline constexpr size_t kShardPad = 128;

}  // namespace cyclone
