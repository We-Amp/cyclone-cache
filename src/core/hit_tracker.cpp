// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "hit_tracker.hpp"

#include <algorithm>
#include <chrono>
#include <mutex>

#include "fork_gate.hpp"

#ifndef _WIN32
#include <unistd.h>  // getpid(): see wake_flush_thread
#endif

namespace cyclone {

HitTracker::HitTracker(const Config& config) : _config(config) {}

HitTracker::~HitTracker() { stop(); }

void HitTracker::start(HitFlushCallback callback) {
  if (_running.exchange(true)) {
    return;  // Already running
  }

  _flush_callback = std::move(callback);

  if (_config.enable_background_flush) {
    // Before the thread exists: its passes are fork-gated (see
    // flush_thread_func), which needs the handlers in place.
    install_fork_handlers();
    _thread_epoch = fork_epoch();
#ifndef _WIN32
    _thread_pid = static_cast<long>(::getpid());
#endif
    _has_flush_thread.store(true, std::memory_order_release);
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

  // UNBOUNDED join: deliberately NOT a bounded-join-then-leak pattern (see
  // DirectorySyncer::stop() in cache.cpp).  The flush
  // callback iterates the Cache's volume list and writes hit counts into
  // stripes; a leaked-but-still-running flush thread abandoned on a timeout
  // then dereferences volumes freed by Cache teardown — the same
  // heap-use-after-free class the DirectorySyncer teardown fix removed.
  // Under I/O pressure (a pwrite/fsync stall) the old 5 s deadline genuinely
  // timed out and abandoned a live thread.  The join always completes: the
  // wake below ends the thread's wait at once and a flush pass is one finite
  // sweep.  The fork-inherited "thread does not exist in this
  // process" case never reaches here — Cache::stop() leaks the HitTracker
  // shared_ptr on that path so ~HitTracker (and thus stop()) never runs in the
  // child.
  //
  // The thread blocks on _wake_cv rather than sleeping, so it has to be
  // woken: _running is already false, and the wake makes its wait return.
  wake_flush_thread();
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
  bool over_limit = false;
  bool became_pending = false;

  {
    Stripe& stripe = (*_stripes)[stripe_index(key)];
    std::lock_guard<std::mutex> lock(stripe.mutex);

    // Past the hard bound, and only once that has been true for longer
    // than the grace window without a sweep draining it, a hit that would
    // need a NEW pending entry is sacrificed, not recorded: the pending
    // set is memory-bounded (per grace window) no matter how fast records
    // arrive while no flush can keep up (a stalled callback, or sustained
    // overload with no flush thread).  A hit on a key that is already
    // pending costs no memory and is always counted.  A burst that merely
    // outruns the next sweep is fully recorded.  Hit statistics are
    // best-effort, like a dropped delta on a Busy flush.
    auto it = stripe.pending.find(tracking_key);
    if (it == stripe.pending.end()) {
      if (_pending_entries.load(std::memory_order_relaxed) >
              hard_pending_cap() &&
          over_cap_past_grace()) {
        _overflow_drops.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      it = stripe.pending.emplace(tracking_key, HitRecord{}).first;
      // The set leaving zero is what ends an idle flush thread's wait.
      became_pending =
          _pending_entries.fetch_add(1, std::memory_order_relaxed) == 0;
    }
    auto& record = it->second;
    record.delta++;
    record.last_access_ms = now_ms;

    if (record.delta >= _config.flush_threshold) {
      should_flush_key = true;
    }
    if (_pending_entries.load(std::memory_order_relaxed) >
        _config.max_pending_entries) {
      over_limit = true;
    }
  }

  // Everything below runs outside the stripe lock (leaf-lock rule).  The
  // over-limit response is a HAND-OFF, never a synchronous full flush on
  // the reading thread (see flush_soon).
  if (became_pending) {
    wake_flush_thread();
  }
  if (over_limit) {
    flush_soon();
  } else if (should_flush_key) {
    flush_key(key, alternate_id);
  }
}

// True when the pending set has been past hard_pending_cap() for longer
// than the grace window without any sweep draining it.  The first sighting
// arms the window; flush_now() clears it after every sweep (each sweep
// drains everything it saw).  Relaxed ordering: the stamp is a heuristic
// bound, never a correctness dependency.
bool HitTracker::over_cap_past_grace() {
  const auto now = std::chrono::steady_clock::now();
  const uint64_t now_ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          now.time_since_epoch())
          .count());
  const uint64_t since = _over_cap_since.load(std::memory_order_relaxed);
  if (since == 0) {
    _over_cap_since.store(now_ns, std::memory_order_relaxed);
    return false;  // The grace window starts now.
  }
  // Another thread may have stored a later stamp than our clock read: never
  // let the unsigned difference wrap into a spurious "past grace".
  return now_ns > since && now_ns - since > kOverCapGraceNs;
}

void HitTracker::flush_soon() {
  if (_running.load(std::memory_order_acquire) &&
      _config.enable_background_flush) {
    // A flush thread exists: hand the sweep to it.  The flag ends its
    // wait at once, and is cleared by the thread BEFORE its sweep, so an
    // overflow arriving during the sweep re-arms the early wake instead of
    // waiting out the flush interval.
    _flush_soon.store(true, std::memory_order_seq_cst);
    wake_flush_thread();
    return;
  }
  // No flush thread: the sweep still has to run, or the bound only re-arms
  // on the next over-limit record -- but at most ONE reader at a time runs
  // it.  Every other over-limit reader proceeds; the gate re-opens when
  // the sweep finishes, and a set still over the limit afterwards is
  // picked up by the next over-limit reader.
  if (!_flush_in_progress.exchange(true, std::memory_order_acq_rel)) {
    flush_now();
    _flush_in_progress.store(false, std::memory_order_release);
  }
}

void HitTracker::wake_flush_thread() {
  if (!_has_flush_thread.load(std::memory_order_acquire) ||
      _thread_epoch != fork_epoch()) {
    return;
  }
#ifndef _WIN32
  // The PID covers a process whose fork handlers could not be registered
  // (the epoch then never moves), as in Cache::stop().
  if (_thread_pid != static_cast<long>(::getpid())) {
    return;
  }
#endif
  // Lock and unlock, then notify: the state change the caller made before
  // this call is then either seen by the thread's predicate check or
  // arrives as a notify while it waits (see _wake_mutex).
  {
    std::lock_guard<std::mutex> lock(_wake_mutex);
  }
  _wake_cv.notify_one();
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
          if (_flush_callback(tracking_key.key, tracking_key.alternate_id,
                              record.delta, record.last_access_ms)) {
            any_flushed = true;
          } else {
            _flush_failures.fetch_add(1, std::memory_order_relaxed);
          }
        }
      }
    }
  }

  // Single post-flush callback (e.g., fsync) — once per cycle, not per-key.
  if (any_flushed && _batch_done_callback) {
    _batch_done_callback();
  }

  // Every sweep drains what it saw, so the pending set is back under the
  // bound (or never reached it): re-arm the overflow grace window for the
  // next sustained overload.
  _over_cap_since.store(0, std::memory_order_relaxed);
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
    if (!_flush_callback(key, alternate_id, record.delta,
                         record.last_access_ms)) {
      _flush_failures.fetch_add(1, std::memory_order_relaxed);
    }
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
        if (!_flush_callback(tracking_key.key, tracking_key.alternate_id,
                             record.delta, record.last_access_ms)) {
          _flush_failures.fetch_add(1, std::memory_order_relaxed);
        }
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
  // Never past max_idle_wait, and never a zero wait (a busy loop).
  const auto idle_wait =
      std::max(_config.max_idle_wait, std::chrono::milliseconds(1));
  while (_running.load()) {
    {
      // Waits only; no other lock is taken while _wake_mutex is held, so
      // HitTracker stays a leaf lock.
      std::unique_lock<std::mutex> lock(_wake_mutex);

      // Idle: nothing pending, so there is nothing to sweep.  Block until
      // the first hit (record_hit wakes us when the set leaves zero), an
      // over-limit hand-off, stop(), or the idle safety-net timeout.
      _wake_cv.wait_for(lock, idle_wait, [this] {
        return !_running.load() ||
               _flush_soon.load(std::memory_order_seq_cst) ||
               _pending_entries.load(std::memory_order_relaxed) > 0;
      });
      _wakeups.fetch_add(1, std::memory_order_relaxed);

      // Something is pending: let hits accumulate for one flush interval,
      // as the periodic sweep did, so a hit is flushed no later than
      // flush_interval after it is recorded.  An over-limit hand-off
      // (_flush_soon) or stop() ends the wait early.
      if (_running.load() && !_flush_soon.load(std::memory_order_seq_cst) &&
          _pending_entries.load(std::memory_order_relaxed) > 0) {
        const auto sleep_end =
            std::chrono::steady_clock::now() + _config.flush_interval;
        _wake_cv.wait_until(lock, sleep_end, [this] {
          return !_running.load() ||
                 _flush_soon.load(std::memory_order_seq_cst);
        });
        _wakeups.fetch_add(1, std::memory_order_relaxed);
      }
    }

    if (!_running.load()) {
      break;
    }

    // Nothing to sweep (an idle timeout, or flush_key drained the set
    // meanwhile): skip the 4096-stripe sweep.  An empty sweep would call no
    // callback; the only state it touches is the overflow grace stamp,
    // which a sweep re-arms, so re-arm it here the same way.
    if (_pending_entries.load(std::memory_order_relaxed) == 0 &&
        !_flush_soon.load(std::memory_order_seq_cst)) {
      _over_cap_since.store(0, std::memory_order_relaxed);
      continue;
    }

    // The whole sweep is ONE fork-gated pass: it locks each of the stripe
    // mutexes in turn and its callbacks take volume stripe locks, and a
    // child forked while this thread held any of them would inherit it
    // locked for good (this thread does not exist there) and block on the
    // first read that hashes to that stripe.  fork() waits for the sweep
    // instead.  A false pass means stop() was called while a fork was
    // pending: skip the sweep, stop() runs the final flush itself.
    ForkGatedPass pass(_running);
    if (!pass) {
      break;
    }

    // Clear the hand-off flag BEFORE the sweep: an overflow arriving during
    // the sweep re-arms it (seq_cst both sides), so the thread loops again
    // instead of sleeping through the flush interval with the set still
    // over the limit.
    _flush_soon.store(false, std::memory_order_seq_cst);

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
