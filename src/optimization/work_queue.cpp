// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "work_queue.hpp"

#include <algorithm>

namespace cyclone {

WorkQueue::WorkQueue(const OptimizationConfig &config) : _config(config) {
  _heap.reserve(std::min(_config.max_queue_size, size_t{1024}));
}

WorkQueue::~WorkQueue() { stop(); }

std::expected<void, CacheError> WorkQueue::enqueue(WorkItem item) {
  if (_stopped.load(std::memory_order_acquire)) {
    return std::unexpected(CacheError::Closed);
  }

  item.enqueue_time = std::chrono::steady_clock::now();

  std::unique_lock lock(_mutex);

  // Check for duplicates
  if (_queued_ids.contains(item.id)) {
    std::lock_guard stats_lock(_stats_mutex);
    ++_stats.duplicates;
    return {};  // Silently succeed for duplicates
  }

  // Check queue size limit
  if (_heap.size() >= _config.max_queue_size) {
    std::lock_guard stats_lock(_stats_mutex);
    ++_stats.rejected_full;
    return std::unexpected(CacheError::OptimizationQueueFull);
  }

  // Check memory limit (with overflow protection)
  size_t current_memory = _memory_usage.load(std::memory_order_relaxed);
  size_t item_memory = item.estimated_memory;
  if (item_memory > _config.max_memory_bytes - current_memory) {
    // Would overflow or exceed limit
    std::lock_guard stats_lock(_stats_mutex);
    ++_stats.rejected_memory;
    return std::unexpected(CacheError::OptimizationQueueFull);
  }

  // Add to queue (save id and memory before move for exception safety)
  auto item_id = item.id;
  _queued_ids.insert(item_id);
  try {
    _heap.push_back(item);
  } catch (...) {
    _queued_ids.erase(item_id);
    throw;
  }
  std::push_heap(_heap.begin(), _heap.end());

  _memory_usage.fetch_add(item_memory, std::memory_order_relaxed);

  {
    std::lock_guard stats_lock(_stats_mutex);
    ++_stats.total_enqueued;
    _stats.current_size = _heap.size();
    _stats.current_memory = _memory_usage.load(std::memory_order_relaxed);
  }

  lock.unlock();
  _cv.notify_one();

  return {};
}

std::optional<WorkItem> WorkQueue::dequeue() {
  std::unique_lock lock(_mutex);

  _cv.wait(lock, [this] {
    return !_heap.empty() || _stopped.load(std::memory_order_acquire);
  });

  if (_stopped.load(std::memory_order_acquire) && _heap.empty()) {
    return std::nullopt;
  }

  if (_heap.empty()) {
    return std::nullopt;
  }

  std::pop_heap(_heap.begin(), _heap.end());
  WorkItem item = _heap.back();
  _heap.pop_back();

  _queued_ids.erase(item.id);
  _memory_usage.fetch_sub(item.estimated_memory, std::memory_order_relaxed);

  {
    std::lock_guard stats_lock(_stats_mutex);
    ++_stats.total_dequeued;
    _stats.current_size = _heap.size();
    _stats.current_memory = _memory_usage.load(std::memory_order_relaxed);
  }

  return item;
}

std::optional<WorkItem> WorkQueue::try_dequeue() {
  std::unique_lock lock(_mutex);

  if (_heap.empty()) {
    return std::nullopt;
  }

  std::pop_heap(_heap.begin(), _heap.end());
  WorkItem item = _heap.back();
  _heap.pop_back();

  _queued_ids.erase(item.id);
  _memory_usage.fetch_sub(item.estimated_memory, std::memory_order_relaxed);

  {
    std::lock_guard stats_lock(_stats_mutex);
    ++_stats.total_dequeued;
    _stats.current_size = _heap.size();
    _stats.current_memory = _memory_usage.load(std::memory_order_relaxed);
  }

  return item;
}

bool WorkQueue::cancel(const WorkItemId &id) {
  std::unique_lock lock(_mutex);

  if (!_queued_ids.contains(id)) {
    return false;
  }

  auto it = std::find_if(_heap.begin(), _heap.end(),
                         [&id](const WorkItem &item) { return item.id == id; });

  if (it == _heap.end()) {
    // Inconsistent state, should not happen
    _queued_ids.erase(id);
    return false;
  }

  size_t mem = it->estimated_memory;
  _heap.erase(it);
  _queued_ids.erase(id);
  _memory_usage.fetch_sub(mem, std::memory_order_relaxed);

  rebuild_heap();

  {
    std::lock_guard stats_lock(_stats_mutex);
    ++_stats.total_cancelled;
    _stats.current_size = _heap.size();
    _stats.current_memory = _memory_usage.load(std::memory_order_relaxed);
  }

  return true;
}

size_t WorkQueue::cancel_by_key(const CacheKey &key) {
  std::unique_lock lock(_mutex);

  size_t cancelled = 0;
  size_t mem_freed = 0;

  auto it = _heap.begin();
  while (it != _heap.end()) {
    if (it->id.key == key) {
      mem_freed += it->estimated_memory;
      _queued_ids.erase(it->id);
      it = _heap.erase(it);
      ++cancelled;
    } else {
      ++it;
    }
  }

  if (cancelled > 0) {
    _memory_usage.fetch_sub(mem_freed, std::memory_order_relaxed);
    rebuild_heap();

    std::lock_guard stats_lock(_stats_mutex);
    _stats.total_cancelled += cancelled;
    _stats.current_size = _heap.size();
    _stats.current_memory = _memory_usage.load(std::memory_order_relaxed);
  }

  return cancelled;
}

size_t WorkQueue::cancel_deferrable() {
  std::unique_lock lock(_mutex);

  size_t cancelled = 0;
  size_t mem_freed = 0;

  auto it = _heap.begin();
  while (it != _heap.end()) {
    if (it->deferrable) {
      mem_freed += it->estimated_memory;
      _queued_ids.erase(it->id);
      it = _heap.erase(it);
      ++cancelled;
    } else {
      ++it;
    }
  }

  if (cancelled > 0) {
    _memory_usage.fetch_sub(mem_freed, std::memory_order_relaxed);
    rebuild_heap();

    std::lock_guard stats_lock(_stats_mutex);
    _stats.total_cancelled += cancelled;
    _stats.current_size = _heap.size();
    _stats.current_memory = _memory_usage.load(std::memory_order_relaxed);
  }

  return cancelled;
}

bool WorkQueue::contains(const WorkItemId &id) const {
  std::shared_lock lock(_mutex);
  return _queued_ids.contains(id);
}

size_t WorkQueue::size() const {
  std::shared_lock lock(_mutex);
  return _heap.size();
}

size_t WorkQueue::memory_usage() const noexcept {
  return _memory_usage.load(std::memory_order_relaxed);
}

bool WorkQueue::empty() const {
  std::shared_lock lock(_mutex);
  return _heap.empty();
}

void WorkQueue::stop() {
  _stopped.store(true, std::memory_order_release);
  _cv.notify_all();
}

bool WorkQueue::is_stopped() const noexcept {
  return _stopped.load(std::memory_order_acquire);
}

WorkQueueStats WorkQueue::stats() const {
  std::lock_guard lock(_stats_mutex);
  return _stats;
}

void WorkQueue::rebuild_heap() { std::make_heap(_heap.begin(), _heap.end()); }

}  // namespace cyclone
