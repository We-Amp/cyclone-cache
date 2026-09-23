// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include "core/hit_tracker.hpp"
#include "cyclone/alternate.hpp"
#include "cyclone/key.hpp"

using namespace cyclone;

// Record of a flushed hit for testing
struct FlushedHitRecord {
  CacheKey key;
  AlternateId alternate_id;
  uint32_t hit_delta;
  int64_t last_access_ms;
};

TEST_CASE("HitTracker basic operations", "[hit_tracker]") {
  HitTrackerConfig config;
  config.enable_background_flush = false;

  HitTracker tracker(config);

  SECTION("record_hit increments pending count") {
    CacheKey key("test-key");

    REQUIRE(tracker.pending_hits(key) == 0);

    tracker.record_hit(key);
    REQUIRE(tracker.pending_hits(key) == 1);

    tracker.record_hit(key);
    REQUIRE(tracker.pending_hits(key) == 2);

    tracker.record_hit(key);
    REQUIRE(tracker.pending_hits(key) == 3);
  }

  SECTION("pending_count tracks unique keys") {
    CacheKey key1("test-key-1");
    CacheKey key2("test-key-2");
    CacheKey key3("test-key-3");

    REQUIRE(tracker.pending_count() == 0);

    tracker.record_hit(key1);
    REQUIRE(tracker.pending_count() == 1);

    tracker.record_hit(key1);
    REQUIRE(tracker.pending_count() == 1);  // Same key, no new entry

    tracker.record_hit(key2);
    REQUIRE(tracker.pending_count() == 2);

    tracker.record_hit(key3);
    REQUIRE(tracker.pending_count() == 3);
  }

  SECTION("total_pending_hits sums all hits") {
    CacheKey key1("test-key-1");
    CacheKey key2("test-key-2");

    REQUIRE(tracker.total_pending_hits() == 0);

    tracker.record_hit(key1);
    tracker.record_hit(key1);
    tracker.record_hit(key2);

    REQUIRE(tracker.total_pending_hits() == 3);
  }
}

TEST_CASE("HitTracker per-alternate tracking", "[hit_tracker]") {
  HitTrackerConfig config;
  config.enable_background_flush = false;

  HitTracker tracker(config);

  CacheKey key("test-key");

  SECTION("tracks different alternates separately") {
    REQUIRE(tracker.pending_hits(key, AlternateId::Original) == 0);
    REQUIRE(tracker.pending_hits(key, AlternateId::Brotli) == 0);
    REQUIRE(tracker.pending_hits(key, AlternateId::Gzip) == 0);

    tracker.record_hit(key, AlternateId::Original);
    tracker.record_hit(key, AlternateId::Original);
    tracker.record_hit(key, AlternateId::Brotli);

    REQUIRE(tracker.pending_hits(key, AlternateId::Original) == 2);
    REQUIRE(tracker.pending_hits(key, AlternateId::Brotli) == 1);
    REQUIRE(tracker.pending_hits(key, AlternateId::Gzip) == 0);

    REQUIRE(tracker.pending_count() ==
            2);  // 2 unique (key, alternate_id) pairs
  }

  SECTION("flush_all_alternates flushes all alternates for key") {
    std::vector<FlushedHitRecord> flushed_records;

    tracker.start(
        [&](const CacheKey &k, AlternateId alt, uint32_t delta, int64_t ts) {
          flushed_records.push_back({k, alt, delta, ts});
          return true;
        });

    tracker.record_hit(key, AlternateId::Original);
    tracker.record_hit(key, AlternateId::Brotli);

    CacheKey other_key("other-key");
    tracker.record_hit(other_key, AlternateId::Original);

    REQUIRE(tracker.pending_count() == 3);

    tracker.flush_all_alternates(key);

    REQUIRE(tracker.pending_count() == 1);  // Only other_key remains
    REQUIRE(flushed_records.size() == 2);   // Both alternates of key flushed

    // Stop tracker before flushed_records goes out of scope
    // (destructor would otherwise call callback with invalid reference)
    tracker.stop();
  }
}

TEST_CASE("HitTracker flush operations", "[hit_tracker]") {
  HitTrackerConfig config;
  config.enable_background_flush = false;

  std::vector<FlushedHitRecord> flushed_records;

  HitTracker tracker(config);
  tracker.start([&](const CacheKey &key, AlternateId alternate_id,
                    uint32_t hit_delta, int64_t last_access_ms) {
    flushed_records.push_back({key, alternate_id, hit_delta, last_access_ms});
    return true;
  });

  SECTION("flush_now clears pending hits") {
    CacheKey key("test-key");

    tracker.record_hit(key);
    tracker.record_hit(key);
    tracker.record_hit(key);

    REQUIRE(tracker.pending_hits(key) == 3);
    REQUIRE(tracker.pending_count() == 1);

    tracker.flush_now();

    REQUIRE(tracker.pending_hits(key) == 0);
    REQUIRE(tracker.pending_count() == 0);
    REQUIRE(flushed_records.size() == 1);
    REQUIRE(flushed_records[0].hit_delta == 3);
    REQUIRE(flushed_records[0].alternate_id == AlternateId::Original);
  }

  SECTION("flush_key flushes specific key only") {
    CacheKey key1("test-key-1");
    CacheKey key2("test-key-2");

    tracker.record_hit(key1);
    tracker.record_hit(key1);
    tracker.record_hit(key2);

    REQUIRE(tracker.pending_count() == 2);

    tracker.flush_key(key1);

    REQUIRE(tracker.pending_hits(key1) == 0);
    REQUIRE(tracker.pending_hits(key2) == 1);
    REQUIRE(tracker.pending_count() == 1);
    REQUIRE(flushed_records.size() == 1);
    REQUIRE(flushed_records[0].hit_delta == 2);
  }
}

TEST_CASE("HitTracker threshold-based flush", "[hit_tracker]") {
  HitTrackerConfig config;
  config.enable_background_flush = false;
  config.flush_threshold = 5;

  std::vector<FlushedHitRecord> flushed_records;

  HitTracker tracker(config);
  tracker.start([&](const CacheKey &key, AlternateId alternate_id,
                    uint32_t hit_delta, int64_t last_access_ms) {
    flushed_records.push_back({key, alternate_id, hit_delta, last_access_ms});
    return true;
  });

  CacheKey key("test-key");

  SECTION("auto-flush when threshold reached") {
    // Record hits below threshold
    for (int i = 0; i < 4; ++i) {
      tracker.record_hit(key);
    }
    REQUIRE(flushed_records.empty());  // Not flushed yet

    // Hit threshold
    tracker.record_hit(key);
    REQUIRE(flushed_records.size() == 1);
    REQUIRE(flushed_records[0].hit_delta == 5);
    REQUIRE(tracker.pending_hits(key) == 0);  // Cleared after flush
  }
}

TEST_CASE("HitTracker background flush", "[hit_tracker]") {
  HitTrackerConfig config;
  config.enable_background_flush = true;
  config.flush_interval = std::chrono::milliseconds(50);

  std::vector<FlushedHitRecord> flushed_records;
  std::mutex flushed_mutex;

  HitTracker tracker(config);
  tracker.start([&](const CacheKey &key, AlternateId alternate_id,
                    uint32_t hit_delta, int64_t last_access_ms) {
    std::lock_guard<std::mutex> lock(flushed_mutex);
    flushed_records.push_back({key, alternate_id, hit_delta, last_access_ms});
    return true;
  });

  CacheKey key("test-key");
  tracker.record_hit(key);
  tracker.record_hit(key);

  // Poll for the background flush.  A flush may land between the two
  // record_hit() calls, yielding two records of delta 1 instead of one of
  // delta 2, so sum the deltas; the deadline is generous for loaded CI
  // runners.
  auto flushed_hits = [&] {
    std::lock_guard<std::mutex> lock(flushed_mutex);
    uint64_t sum = 0;
    for (const auto &record : flushed_records) {
      REQUIRE(record.key == key);
      sum += record.hit_delta;
    }
    return sum;
  };
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (flushed_hits() < 2 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  // Checked before stop(): stop() does a final flush, so a pass here proves
  // the background thread flushed.
  REQUIRE(flushed_hits() == 2);

  tracker.stop();
}

TEST_CASE("HitTracker stop performs final flush", "[hit_tracker]") {
  HitTrackerConfig config;
  config.enable_background_flush = false;

  std::vector<FlushedHitRecord> flushed_records;

  HitTracker tracker(config);
  tracker.start([&](const CacheKey &key, AlternateId alternate_id,
                    uint32_t hit_delta, int64_t last_access_ms) {
    flushed_records.push_back({key, alternate_id, hit_delta, last_access_ms});
    return true;
  });

  CacheKey key("test-key");
  tracker.record_hit(key);
  tracker.record_hit(key);

  REQUIRE(flushed_records.empty());

  tracker.stop();

  REQUIRE(flushed_records.size() == 1);
  REQUIRE(flushed_records[0].hit_delta == 2);
}

TEST_CASE("HitTracker timestamp tracking", "[hit_tracker]") {
  HitTrackerConfig config;
  config.enable_background_flush = false;

  std::vector<FlushedHitRecord> flushed_records;

  HitTracker tracker(config);
  tracker.start([&](const CacheKey &key, AlternateId alternate_id,
                    uint32_t hit_delta, int64_t last_access_ms) {
    flushed_records.push_back({key, alternate_id, hit_delta, last_access_ms});
    return true;
  });

  auto before = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();

  CacheKey key("test-key");
  tracker.record_hit(key);

  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  tracker.record_hit(key);

  auto after = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
                   .count();

  tracker.flush_now();

  REQUIRE(flushed_records.size() == 1);
  int64_t last_access = flushed_records[0].last_access_ms;
  REQUIRE(last_access >= before);
  REQUIRE(last_access <= after);
}

TEST_CASE("HitTracker concurrent access", "[hit_tracker]") {
  HitTrackerConfig config;
  config.enable_background_flush = false;

  HitTracker tracker(config);

  constexpr int kNumThreads = 4;
  constexpr int kHitsPerThread = 1000;

  CacheKey key("concurrent-key");

  std::vector<std::thread> threads;
  threads.reserve(kNumThreads);
  for (int i = 0; i < kNumThreads; ++i) {
    threads.emplace_back([&]() {
      for (int j = 0; j < kHitsPerThread; ++j) {
        tracker.record_hit(key);
      }
    });
  }

  for (auto &t : threads) {
    t.join();
  }

  REQUIRE(tracker.pending_hits(key) == kNumThreads * kHitsPerThread);
}

TEST_CASE("HitTracker merges cross-thread records into one flush callback",
          "[hit_tracker]") {
  // Sharded-counters regression guard, implementation-agnostic: however
  // the
  // tracker shards its pending records internally, hits recorded from many
  // threads must flush as exactly ONE callback per (key, alternate) with
  // the full delta, and pending_count() must count distinct entries.
  HitTrackerConfig config;
  config.enable_background_flush = false;

  HitTracker tracker(config);

  std::vector<FlushedHitRecord> flushed;
  std::mutex flushed_mutex;
  tracker.start([&](const CacheKey &key, AlternateId alternate_id,
                    uint32_t hit_delta, int64_t last_access_ms) {
    std::lock_guard<std::mutex> lock(flushed_mutex);
    flushed.push_back({key, alternate_id, hit_delta, last_access_ms});
    return true;
  });

  constexpr int kNumThreads = 8;
  constexpr int kHitsPerThread = 500;
  CacheKey key("merge-key");

  std::vector<std::thread> threads;
  threads.reserve(kNumThreads);
  for (int i = 0; i < kNumThreads; ++i) {
    threads.emplace_back([&]() {
      for (int j = 0; j < kHitsPerThread; ++j) {
        tracker.record_hit(key);
      }
    });
  }
  for (auto &t : threads) {
    t.join();
  }

  REQUIRE(tracker.pending_hits(key) == kNumThreads * kHitsPerThread);
  REQUIRE(tracker.pending_count() == 1);  // distinct entries, not per-stripe

  tracker.flush_now();

  REQUIRE(flushed.size() == 1);
  REQUIRE(flushed[0].key == key);
  REQUIRE(flushed[0].hit_delta == kNumThreads * kHitsPerThread);
  REQUIRE(tracker.pending_hits(key) == 0);
  REQUIRE(tracker.pending_count() == 0);

  tracker.stop();
}

TEST_CASE("HitTracker bounds pending entries", "[hit_tracker]") {
  HitTrackerConfig config;
  config.enable_background_flush = false;
  config.max_pending_entries = 50;

  std::atomic<uint64_t> total_flushed{0};

  HitTracker tracker(config);
  tracker.start(
      [&](const CacheKey &, AlternateId, uint32_t hit_delta, int64_t) {
        total_flushed.fetch_add(hit_delta, std::memory_order_relaxed);
        return true;
      });

  // Record 100 unique keys — should trigger flush when exceeding 50
  for (int i = 0; i < 100; ++i) {
    CacheKey key("bounded-" + std::to_string(i));
    tracker.record_hit(key);

    // Pending count should never greatly exceed max_pending_entries
    // (there's a brief window where it can be max+1 before flush_now runs)
    REQUIRE(tracker.pending_count() <= config.max_pending_entries + 1);
  }

  tracker.stop();

  // All 100 hits should have been accounted for (pending + flushed)
  REQUIRE(total_flushed.load() + tracker.total_pending_hits() == 100);
}

TEST_CASE("HitTracker bounded flush preserves all hits", "[hit_tracker]") {
  HitTrackerConfig config;
  config.enable_background_flush = false;
  config.max_pending_entries = 20;

  uint64_t total_flushed = 0;

  HitTracker tracker(config);
  tracker.start(
      [&](const CacheKey &, AlternateId, uint32_t hit_delta, int64_t) {
        total_flushed += hit_delta;
        return true;
      });

  constexpr int kTotalKeys = 200;
  constexpr int kHitsPerKey = 3;

  for (int i = 0; i < kTotalKeys; ++i) {
    CacheKey key("preserve-" + std::to_string(i));
    for (int j = 0; j < kHitsPerKey; ++j) {
      tracker.record_hit(key);
    }
  }

  // Final flush
  tracker.flush_now();

  // Total flushed should equal total recorded
  uint64_t remaining = tracker.total_pending_hits();
  REQUIRE(total_flushed + remaining ==
          static_cast<uint64_t>(kTotalKeys * kHitsPerKey));
}

TEST_CASE("HitTracker bounded under concurrent load",
          "[hit_tracker][concurrent]") {
  HitTrackerConfig config;
  config.enable_background_flush = false;
  config.max_pending_entries = 500;

  std::atomic<uint64_t> total_flushed{0};

  HitTracker tracker(config);
  tracker.start(
      [&](const CacheKey &, AlternateId, uint32_t hit_delta, int64_t) {
        total_flushed.fetch_add(hit_delta, std::memory_order_relaxed);
        return true;
      });

  constexpr int kNumThreads = 4;
  constexpr int kKeysPerThread = 1000;

  std::vector<std::thread> threads;
  threads.reserve(kNumThreads);
  for (int i = 0; i < kNumThreads; ++i) {
    threads.emplace_back([&, i]() {
      for (int j = 0; j < kKeysPerThread; ++j) {
        CacheKey key("concurrent-bounded-" + std::to_string(i) + "-" +
                     std::to_string(j));
        tracker.record_hit(key);
      }
    });
  }

  for (auto &t : threads) {
    t.join();
  }

  // Final flush
  tracker.flush_now();

  // All hits accounted for
  uint64_t remaining = tracker.total_pending_hits();
  REQUIRE(total_flushed.load() + remaining ==
          static_cast<uint64_t>(kNumThreads) * kKeysPerThread);
}

TEST_CASE("HitTracker stop is idempotent", "[hit_tracker]") {
  // Regression guard for the thread-lifecycle leak fix: stop() must be safe to
  // call repeatedly, and ~HitTracker after an explicit stop() must not crash,
  // hang, or double-join the flush thread.
  HitTrackerConfig config;
  config.enable_background_flush = true;
  config.flush_interval = std::chrono::milliseconds(50);

  std::vector<FlushedHitRecord> flushed_records;
  std::mutex flushed_mutex;

  SECTION("double stop after background flush is running") {
    HitTracker tracker(config);
    tracker.start([&](const CacheKey &key, AlternateId alternate_id,
                      uint32_t hit_delta, int64_t last_access_ms) {
      std::lock_guard<std::mutex> lock(flushed_mutex);
      flushed_records.push_back({key, alternate_id, hit_delta, last_access_ms});
      return true;
    });

    CacheKey key("idempotent-stop");
    tracker.record_hit(key);
    tracker.record_hit(key);

    // Let the flush thread actually run at least once.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // First stop joins the flush thread and performs a final flush.
    tracker.stop();

    // Second stop must be a no-op: no crash, no hang, returns promptly.
    tracker.stop();

    // A third stop, for good measure.
    tracker.stop();

    // Destructor runs here after an explicit stop -- must not double-join.
  }

  SECTION("destructor after explicit stop") {
    {
      HitTracker tracker(config);
      tracker.start([&](const CacheKey &key, AlternateId alternate_id,
                        uint32_t hit_delta, int64_t last_access_ms) {
        std::lock_guard<std::mutex> lock(flushed_mutex);
        flushed_records.push_back(
            {key, alternate_id, hit_delta, last_access_ms});
        return true;
      });

      CacheKey key("dtor-after-stop");
      tracker.record_hit(key);

      tracker.stop();
      // tracker destroyed here -- ~HitTracker must tolerate an already-stopped,
      // already-joined thread.
    }

    // If we reach here without crash/hang, idempotent teardown works.
    REQUIRE(true);
  }
}

TEST_CASE("HitTracker stop returns within a bounded time", "[hit_tracker]") {
  // Regression guard against an unbounded or never-waking join: the flush
  // thread sleeps the flush interval in small (~100ms) increments while
  // rechecking _running, so stop() must return well within a generous bound
  // even when the configured flush interval is long.
  HitTrackerConfig config;
  config.enable_background_flush = true;
  // Deliberately long interval -- a naive implementation that slept the whole
  // interval before noticing _running would blow past the bound below.
  config.flush_interval = std::chrono::seconds(30);

  std::atomic<uint64_t> total_flushed{0};

  HitTracker tracker(config);
  tracker.start(
      [&](const CacheKey &, AlternateId, uint32_t hit_delta, int64_t) {
        total_flushed.fetch_add(hit_delta, std::memory_order_relaxed);
        return true;
      });

  CacheKey key("bounded-stop");
  tracker.record_hit(key);

  // Ensure the flush thread has spun up and is parked in its sleep loop.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto begin = std::chrono::steady_clock::now();
  tracker.stop();
  auto elapsed = std::chrono::steady_clock::now() - begin;

  auto elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
  INFO("stop() took " << elapsed_ms << " ms");

  // Real bound is ~100ms wake + drain; 2s is generous slack for loaded CI.
  REQUIRE(elapsed < std::chrono::seconds(2));

  // Final flush on stop() must still have drained the pending hit.
  REQUIRE(total_flushed.load() == 1);
}

TEST_CASE("HitTracker callback receives correct key and alternate",
          "[hit_tracker]") {
  HitTrackerConfig config;
  config.enable_background_flush = false;

  std::vector<FlushedHitRecord> flushed_records;

  HitTracker tracker(config);
  tracker.start([&](const CacheKey &key, AlternateId alternate_id,
                    uint32_t hit_delta, int64_t last_access_ms) {
    flushed_records.push_back({key, alternate_id, hit_delta, last_access_ms});
    return true;
  });

  CacheKey key1("test-key-1");
  CacheKey key2("test-key-2");

  tracker.record_hit(key1, AlternateId::Original);
  tracker.record_hit(key1, AlternateId::Brotli);
  tracker.record_hit(key2, AlternateId::Gzip);

  tracker.flush_now();

  REQUIRE(flushed_records.size() == 3);

  // Verify each record has correct key and alternate
  bool found_key1_original = false;
  bool found_key1_brotli = false;
  bool found_key2_gzip = false;

  for (const auto &record : flushed_records) {
    if (record.key == key1 && record.alternate_id == AlternateId::Original) {
      found_key1_original = true;
      REQUIRE(record.hit_delta == 1);
    }
    if (record.key == key1 && record.alternate_id == AlternateId::Brotli) {
      found_key1_brotli = true;
      REQUIRE(record.hit_delta == 1);
    }
    if (record.key == key2 && record.alternate_id == AlternateId::Gzip) {
      found_key2_gzip = true;
      REQUIRE(record.hit_delta == 1);
    }
  }

  REQUIRE(found_key1_original);
  REQUIRE(found_key1_brotli);
  REQUIRE(found_key2_gzip);
}

// Regression for the staged-teardown fix (leak_thread_on_shutdown UAF
// class).
//
// The old HitTracker::stop() joined the flush thread with a 5 s bounded wait
// and, on timeout, abandoned the still-running thread via
// leak_thread_on_shutdown().  When a flush callback stalled (I/O pressure),
// stop() returned while the flush thread was still executing the callback,
// which iterates the Cache's volume list — so the leaked thread went on to
// dereference volumes the caller then destroyed (heap-use-after-free).
//
// The fix joins unconditionally: stop() must not return until the in-flight
// flush callback has finished.  This test forces a flush callback that is in
// flight on the background thread when stop() is called and blocks it past the
// former 5 s deadline; a correct stop() stays blocked in the join until the
// callback is released, whereas the old bounded-join+leak path would have
// returned (and leaked) at ~5 s.  Under ASan/TSan it additionally proves no
// thread survives stop().
TEST_CASE("HitTracker::stop() joins an in-flight flush thread (no leak)",
          "[hit_tracker][teardown][regression]") {
  HitTrackerConfig config;
  config.enable_background_flush = true;
  config.flush_interval = std::chrono::milliseconds(20);
  // Keep the inline flush paths in record_hit dormant so the ONLY callback
  // invocation comes from the background flush thread.
  config.flush_threshold = 1'000'000;
  config.max_pending_entries = 1'000'000;

  std::atomic<bool> callback_started{false};
  std::atomic<bool> release_callback{false};
  std::atomic<int> callback_invocations{0};

  HitTracker tracker(config);
  tracker.start([&](const CacheKey &, AlternateId, uint32_t, int64_t) {
    callback_invocations.fetch_add(1, std::memory_order_relaxed);
    callback_started.store(true, std::memory_order_release);
    // Block the flush thread inside the callback.  Finite safety cap (30 s) so
    // a broken test can never hang the suite; the assertions release it well
    // before then.
    auto safety = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!release_callback.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < safety) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
  });

  // Get one pending entry that the background thread will flush ~20 ms later.
  tracker.record_hit(CacheKey("uaf-regression-key"));

  // Wait until the flush callback is actually in flight on the bg thread.
  // Generous deadline: on a loaded CI runner the first background flush (its
  // loop sleeps in <=100 ms steps) need only START within this budget.
  auto started_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (!callback_started.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < started_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  REQUIRE(callback_started.load(std::memory_order_acquire));

  // Call stop() on another thread so we can observe whether it returns early.
  std::atomic<bool> stop_returned{false};
  auto stop_begin = std::chrono::steady_clock::now();
  std::thread stopper([&] {
    tracker.stop();
    stop_returned.store(true, std::memory_order_release);
  });

  // Hold past the former 5 s bounded-join deadline WITHOUT releasing the
  // callback.  A correct unconditional join is still blocked here; the old
  // bounded-join+leak path would have returned at ~5 s.
  std::this_thread::sleep_for(std::chrono::milliseconds(5500));

  // Capture the discriminating observation, then release + join BEFORE
  // asserting: asserting while `stopper` is still joinable would std::terminate
  // (via ~thread) on failure instead of a clean Catch2 report (the
  // review).
  const bool returned_while_blocked =
      stop_returned.load(std::memory_order_acquire);
  release_callback.store(true, std::memory_order_release);
  stopper.join();
  auto stop_elapsed = std::chrono::steady_clock::now() - stop_begin;

  // A correct unconditional join stays blocked until the callback is released.
  REQUIRE_FALSE(returned_while_blocked);
  REQUIRE(stop_returned.load(std::memory_order_acquire));
  // stop() waited for the in-flight callback rather than abandoning it.
  REQUIRE(stop_elapsed >= std::chrono::milliseconds(5500));
  // Exactly one callback ran, on the background thread, and it completed.
  REQUIRE(callback_invocations.load(std::memory_order_relaxed) == 1);
}
