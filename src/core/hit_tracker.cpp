// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "hit_tracker.hpp"

#include <mutex>

namespace cyclone {

HitTracker::HitTracker(const Config& config) : _config(config) {}

HitTracker::~HitTracker() { stop(); }

void HitTracker::start(HitFlushCallback callback) {
  if (_running.exchange(true)) {
    return;  // Already running
  }

  _flush_callback = std::move(callback);

  if (_config.enable_background_flush) {
    _flush_thread = std::thread(&HitTracker::flush_thread_func, this);
  }
}

void HitTracker::set_batch_done_callback(HitFlushBatchDoneCallback callback) {
  _batch_done_callback = std::move(callback);
}

void HitTracker::stop() {
  if (!_running.exchange(false)) {
    return;  // Already stopped
  }

  // UNBOUNDED join — deliberately NOT the bounded-join + leak pattern (see
  // thread_util.hpp and DirectorySyncer::stop() in cache.cpp).  The flush
  // callback iterates the Cache's volume list and writes hit counts into
  // stripes; a leaked-but-still-running flush thread abandoned on a timeout
  // then dereferences volumes freed by Cache teardown — the same
  // heap-use-after-free class the DirectorySyncer teardown fix removed.
  // Under I/O pressure (a pwrite/fsync stall) the old 5 s deadline genuinely
  // timed out and abandoned a live thread.  The join always completes: the
  // flush loop re-checks _running at <=100 ms granularity and a flush pass is
  // one finite sweep.  The fork-inherited "thread does not exist in this
  // process" case never reaches here — Cache::stop() leaks the HitTracker
  // shared_ptr on that path so ~HitTracker (and thus stop()) never runs in the
  // child.
  if (_flush_thread.joinable()) {
    _flush_thread.join();
  }

  // Perform final flush (runs on the caller's thread, volumes still open).
  flush_now();
}

void HitTracker::record_hit(const CacheKey& key, AlternateId alternate_id) {
  auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();

  HitTrackingKey tracking_key{key, alternate_id};
  bool should_flush_key = false;
  bool should_flush_all = false;

  {
    Stripe& stripe = (*_stripes)[stripe_index(key)];
    std::lock_guard<std::mutex> lock(stripe.mutex);

    auto [it, inserted] = stripe.pending.try_emplace(tracking_key);
    if (inserted) {
      _pending_entries.fetch_add(1, std::memory_order_relaxed);
    }
    auto& record = it->second;
    record.delta++;
    record.last_access_ms = now_ms;

    if (record.delta >= _config.flush_threshold) {
      should_flush_key = true;
    }
    if (_pending_entries.load(std::memory_order_relaxed) >
        _config.max_pending_entries) {
      should_flush_all = true;
    }
  }

  // Flush outside the lock to avoid AB/BA deadlock with the read path.
  if (should_flush_all) {
    flush_now();
  } else if (should_flush_key) {
    flush_key(key, alternate_id);
  }
}

uint32_t HitTracker::pending_hits(const CacheKey& key,
                                  AlternateId alternate_id) const {
  HitTrackingKey tracking_key{key, alternate_id};
  const Stripe& stripe = (*_stripes)[stripe_index(key)];
  std::lock_guard<std::mutex> lock(stripe.mutex);

  auto it = stripe.pending.find(tracking_key);
  if (it == stripe.pending.end()) {
    return 0;
  }
  return it->second.delta;
}

void HitTracker::flush_now() {
  // Drain each stripe under its own mutex, then invoke callbacks with NO stripe
  // mutex held.  The flush callback (update_hit_count_sync) acquires the volume
  // stripe->mutex, while the read path holds stripe->mutex then calls
  // record_hit which acquires a HitTracker stripe mutex; holding it across a
  // callback would create an AB/BA deadlock.  We hold at most one stripe mutex
  // at a time and never across callbacks.
  bool any_flushed = false;
  for (auto& stripe : *_stripes) {
    std::unordered_map<HitTrackingKey, HitRecord> snapshot;
    {
      std::lock_guard<std::mutex> lock(stripe.mutex);
      snapshot = std::move(stripe.pending);
      stripe.pending.clear();
    }
    if (!snapshot.empty()) {
      _pending_entries.fetch_sub(snapshot.size(), std::memory_order_relaxed);
    }
    if (_flush_callback) {
      for (const auto& [tracking_key, record] : snapshot) {
        if (record.delta > 0) {
          _flush_callback(tracking_key.key, tracking_key.alternate_id,
                          record.delta, record.last_access_ms);
          any_flushed = true;
        }
      }
    }
  }

  // Single post-flush callback (e.g., fsync) — once per cycle, not per-key.
  if (any_flushed && _batch_done_callback) {
    _batch_done_callback();
  }
}

void HitTracker::flush_key(const CacheKey& key, AlternateId alternate_id) {
  HitTrackingKey tracking_key{key, alternate_id};
  HitRecord record;
  bool found = false;

  {
    Stripe& stripe = (*_stripes)[stripe_index(key)];
    std::lock_guard<std::mutex> lock(stripe.mutex);

    auto it = stripe.pending.find(tracking_key);
    if (it != stripe.pending.end()) {
      record = it->second;
      stripe.pending.erase(it);
      _pending_entries.fetch_sub(1, std::memory_order_relaxed);
      found = true;
    }
  }

  if (found && _flush_callback && record.delta > 0) {
    _flush_callback(key, alternate_id, record.delta, record.last_access_ms);
  }
}

void HitTracker::flush_all_alternates(const CacheKey& key) {
  // All alternates of a key share a stripe (stripe_index keys on CacheKey), so
  // a single stripe lock covers them.
  std::vector<std::pair<HitTrackingKey, HitRecord>> to_flush;

  {
    Stripe& stripe = (*_stripes)[stripe_index(key)];
    std::lock_guard<std::mutex> lock(stripe.mutex);

    for (auto it = stripe.pending.begin(); it != stripe.pending.end();) {
      if (it->first.key == key) {
        to_flush.emplace_back(*it);
        it = stripe.pending.erase(it);
        _pending_entries.fetch_sub(1, std::memory_order_relaxed);
      } else {
        ++it;
      }
    }
  }

  // Flush outside the lock
  if (_flush_callback) {
    for (const auto& [tracking_key, record] : to_flush) {
      if (record.delta > 0) {
        _flush_callback(tracking_key.key, tracking_key.alternate_id,
                        record.delta, record.last_access_ms);
      }
    }
  }
}

size_t HitTracker::pending_count() const {
  size_t total = 0;
  for (const auto& stripe : *_stripes) {
    std::lock_guard<std::mutex> lock(stripe.mutex);
    total += stripe.pending.size();
  }
  return total;
}

uint64_t HitTracker::total_pending_hits() const {
  uint64_t total = 0;
  for (const auto& stripe : *_stripes) {
    std::lock_guard<std::mutex> lock(stripe.mutex);
    for (const auto& [_, record] : stripe.pending) {
      total += record.delta;
    }
  }
  return total;
}

void HitTracker::flush_thread_func() {
  while (_running.load()) {
    // Sleep for the flush interval
    auto sleep_time = _config.flush_interval;
    auto sleep_end = std::chrono::steady_clock::now() + sleep_time;

    // Sleep in small increments to check _running flag periodically
    while (_running.load() && std::chrono::steady_clock::now() < sleep_end) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (!_running.load()) {
      break;
    }

    // Perform the flush.  Catch all exceptions to prevent silent thread death
    // (pwrite failures, callback exceptions, etc.).
    try {
      flush_now();
    } catch (...) {  // NOLINT(bugprone-empty-catch)
      // Continue the loop — losing one flush cycle is acceptable.
    }
  }
}

}  // namespace cyclone
