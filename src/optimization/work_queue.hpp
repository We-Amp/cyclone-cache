// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <shared_mutex>
#include <unordered_set>

#include "../core/thread_shard.hpp"
#include "cyclone/alternate.hpp"
#include "cyclone/config.hpp"
#include "cyclone/error.hpp"
#include "cyclone/key.hpp"

namespace cyclone {

// Unique identifier for a work item
struct WorkItemId {
  CacheKey key;
  AlternateId target_alternate;

  bool operator==(const WorkItemId &other) const {
    return key == other.key && target_alternate == other.target_alternate;
  }
};

}  // namespace cyclone

// Hash function for WorkItemId
template <>
struct std::hash<cyclone::WorkItemId> {
  size_t operator()(const cyclone::WorkItemId &id) const noexcept {
    // Combine key hash with alternate id
    size_t h1 = std::hash<cyclone::CacheKey>{}(id.key);
    size_t h2 = std::hash<uint8_t>{}(static_cast<uint8_t>(id.target_alternate));
    return h1 ^ (h2 << 1);
  }
};

namespace cyclone {

// Work item representing an optimization task
struct WorkItem {
  WorkItemId id;
  uint32_t plugin_id = 0;  // ID of the plugin that requested this work
  uint32_t priority = 0;   // Higher = more important
  uint32_t hit_count = 0;
  uint64_t content_length = 0;
  size_t estimated_memory = 0;
  bool deferrable = true;  // Can be cancelled during load shedding
  std::chrono::steady_clock::time_point enqueue_time;

  // Combined priority for queue ordering (higher = dequeue first)
  [[nodiscard]] uint64_t effective_priority() const {
    return (static_cast<uint64_t>(priority) << 32) | hit_count;
  }

  // Heap ordering key set by WorkQueue::enqueue() (larger dequeues first).
  // With aging it is priority * interval - enqueue_time: every item gains
  // one priority level per interval, so the difference of two items' aged
  // priorities never changes and a fixed key gives the aged order.
  double order_key = 0.0;

  // Comparison for priority queue (max heap by effective priority)
  bool operator<(const WorkItem &other) const {
    return effective_priority() < other.effective_priority();
  }
};

// Statistics about the work queue
struct WorkQueueStats {
  size_t current_size = 0;
  size_t current_memory = 0;
  uint64_t total_enqueued = 0;
  uint64_t total_dequeued = 0;
  uint64_t total_cancelled = 0;
  uint64_t rejected_full = 0;
  uint64_t rejected_memory = 0;
  uint64_t duplicates = 0;
};

class WorkQueue {
 public:
  explicit WorkQueue(const OptimizationConfig &config);
  ~WorkQueue();

  WorkQueue(const WorkQueue &) = delete;
  WorkQueue &operator=(const WorkQueue &) = delete;

  // Enqueue a work item. Returns error if queue is full or memory limit
  // exceeded.
  std::expected<void, CacheError> enqueue(WorkItem item);

  // Dequeue the highest priority item. Blocks until an item is available or
  // stop() is called. Returns nullopt if the queue is stopped.
  std::optional<WorkItem> dequeue();

  // Try to dequeue without blocking. Returns nullopt if queue is empty.
  std::optional<WorkItem> try_dequeue();

  // Cancel a specific work item by ID
  bool cancel(const WorkItemId &id);

  // Cancel all work items for a key
  size_t cancel_by_key(const CacheKey &key);

  // Cancel all deferrable work items (for load shedding)
  size_t cancel_deferrable();

  // Check if a work item with this ID is already queued
  bool contains(const WorkItemId &id) const;

  // Get current queue size
  size_t size() const;

  // Get current memory usage estimate
  size_t memory_usage() const noexcept;

  // Check if queue is empty
  bool empty() const;

  // Stop the queue (unblocks all waiting dequeue calls)
  void stop();

  // Start the queue after a stop(): clears the stopped flag so enqueue and
  // dequeue operate again.  Pairs with stop(); a freshly constructed queue
  // is already started.  Any items still queued at stop() stay queued.
  void start();

  // Check if the queue has been stopped
  bool is_stopped() const noexcept;

  // Get statistics.
  // Not a consistent snapshot: each counter is read on its own.  End counters
  // (dequeued) are read before start counters, so an
  // end never appears to exceed its start.
  WorkQueueStats stats() const;

 private:
  void rebuild_heap();
  // Heap order: larger order_key first, ties by effective priority.
  static bool heap_less(const WorkItem &a, const WorkItem &b) {
    if (a.order_key != b.order_key) {
      return a.order_key < b.order_key;
    }
    return a.effective_priority() < b.effective_priority();
  }

  OptimizationConfig _config;

  // Priority queue for work items (max heap by priority)
  std::vector<WorkItem> _heap;

  // Set of currently queued item IDs for deduplication
  std::unordered_set<WorkItemId> _queued_ids;

  // Synchronization
  mutable std::shared_mutex _mutex;
  std::condition_variable_any _cv;
  std::atomic<bool> _stopped{false};

  // Statistics
  std::atomic<size_t> _memory_usage{0};
  // Counters incremented per queue operation, so relaxed atomics read field
  // by field in stats(), never under a mutex.  The current size and memory
  // gauges are derived on read (from _heap under a shared lock and
  // _memory_usage).
  struct alignas(kShardPad) HotCounters {
    std::atomic<uint64_t> total_enqueued{0};
    std::atomic<uint64_t> total_dequeued{0};
    std::atomic<uint64_t> total_cancelled{0};
    std::atomic<uint64_t> rejected_full{0};
    std::atomic<uint64_t> rejected_memory{0};
    std::atomic<uint64_t> duplicates{0};
  };
  static_assert(alignof(HotCounters) == kShardPad &&
                sizeof(HotCounters) % kShardPad == 0);
  HotCounters _stat;
};

}  // namespace cyclone
