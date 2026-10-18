// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// A bucket one writer is stuck on must not stall every OTHER mutation of the
// directory: the phase lock is stripe-wide, so an insert that holds it across
// its (sleeping, per-holder-budgeted) wait on a wedged bucket serializes all
// writes of the stripe behind that one bucket -- and queues the wrap path's
// phase publication behind it too.
//
// The insert takes the per-bucket writer lock FIRST (the sleepy wait holds
// nothing else) and the phase lock SECOND, for the short read-phase + mutate
// bracket only.  The phase bracket itself is unchanged: the phase is still
// read and every entry store still happens under the phase lock, so no phase
// toggle can interleave between them (the ABA protection the bracket exists
// for).

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "core/mmap_directory.hpp"
#include "cyclone/key.hpp"

using namespace cyclone;

namespace {

constexpr size_t kBuckets = 64;

// One writer stuck inside its odd window on `key`'s bucket (like a writer
// descheduled mid-update), plus the insert thread that is waiting on it.
// Both are cleaned up on destruction, so an assertion failure never leaves a
// joinable thread behind.
class StuckBucketInsert {
 public:
  StuckBucketInsert(MmapDirectory &dir, const CacheKey &key)
      : _dir(dir), _key(key) {
    _token = _dir.begin_bucket_write_for_test(key);
    _waiter = std::thread([this] {
      _waiting.store(true);
      // This insert blocks on the stuck bucket for the stretched budget.
      _inserted = _dir.insert(_key, 4096, 512);
    });
    while (!_waiting.load()) {
      std::this_thread::yield();
    }
  }

  bool release() {
    if (_waiter.joinable()) {
      _dir.end_bucket_write_for_test(_key, _token);
      _waiter.join();
    }
    return _inserted;
  }

  ~StuckBucketInsert() { (void)release(); }

  StuckBucketInsert(const StuckBucketInsert &) = delete;
  StuckBucketInsert &operator=(const StuckBucketInsert &) = delete;

 private:
  MmapDirectory &_dir;
  CacheKey _key;
  uint32_t _token = 0;
  std::atomic<bool> _waiting{false};
  bool _inserted = false;
  std::thread _waiter;
};

// Overrides the per-holder wait budgets for one scope (test seams).
struct BudgetOverride {
  BudgetOverride(uint64_t bucket_us, uint64_t phase_us) {
    MmapDirectory::s_bucket_writer_budget_us_for_test.store(bucket_us);
    MmapDirectory::s_phase_lock_budget_us_for_test.store(phase_us);
  }
  ~BudgetOverride() {
    MmapDirectory::s_bucket_writer_budget_us_for_test.store(0);
    MmapDirectory::s_phase_lock_budget_us_for_test.store(0);
  }
  BudgetOverride(const BudgetOverride &) = delete;
  BudgetOverride &operator=(const BudgetOverride &) = delete;
};

struct TestDir {
  std::vector<std::byte> region =
      std::vector<std::byte>(MmapDirectory::required_size(kBuckets));
  MmapDirectory dir =
      *MmapDirectory::init(std::span<std::byte>(region), kBuckets);
};

size_t bucket_of(const CacheKey &key) { return key.bucket_hash() % kBuckets; }

}  // namespace

TEST_CASE(
    "Phase lock scope: an insert on another bucket is not stalled by a "
    "writer waiting on a wedged bucket",
    "[mmap_directory][concurrent][regression]") {
  // The stuck holder is waited out by TIME: the bucket budget is stretched
  // past the test so the waiting insert stays parked for the whole
  // measurement, and the phase budget keeps its default (a waiter on the
  // phase lock force-recovers after ~1 s, which is the stall being pinned).
  BudgetOverride budgets(/*bucket_us=*/uint64_t{10} * 1000 * 1000,
                         /*phase_us=*/0);
  TestDir d;
  const CacheKey stuck_key("phase-scope-stuck");
  // A key on a DIFFERENT bucket: its own writer bracket is free.
  CacheKey other_key("phase-scope-other-0");
  for (int i = 1; bucket_of(other_key) == bucket_of(stuck_key); ++i) {
    other_key = CacheKey("phase-scope-other-" + std::to_string(i));
  }
  REQUIRE(bucket_of(other_key) != bucket_of(stuck_key));

  StuckBucketInsert stuck(d.dir, stuck_key);
  // Let the waiting insert reach its bucket wait.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  const auto t0 = std::chrono::steady_clock::now();
  const bool inserted = d.dir.insert(other_key, 8192, 512);
  const auto elapsed = std::chrono::steady_clock::now() - t0;

  REQUIRE(inserted);
  // Generous bound: with the phase lock no longer held across the bucket
  // wait this is microseconds; holding it made the peer wait out the
  // whole ~1 s phase-lock budget.
  REQUIRE(elapsed < std::chrono::milliseconds(300));
  REQUIRE(d.dir.probe(other_key).has_value());

  REQUIRE(stuck.release());
}

TEST_CASE(
    "Phase lock scope: a phase publication is not queued behind a writer "
    "waiting on a wedged bucket",
    "[mmap_directory][concurrent][regression]") {
  BudgetOverride budgets(/*bucket_us=*/uint64_t{10} * 1000 * 1000,
                         /*phase_us=*/0);
  TestDir d;
  const CacheKey stuck_key("phase-scope-toggle");
  const bool phase_before = d.dir.current_phase();

  StuckBucketInsert stuck(d.dir, stuck_key);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // The wrap path publishes the derived phase under the phase lock; it must
  // not wait out the phase budget behind an insert parked on a bucket.
  const auto t0 = std::chrono::steady_clock::now();
  d.dir.set_current_phase(!phase_before);
  const auto elapsed = std::chrono::steady_clock::now() - t0;

  REQUIRE(elapsed < std::chrono::milliseconds(300));
  REQUIRE(d.dir.current_phase() == !phase_before);

  (void)stuck.release();
}

TEST_CASE(
    "Phase lock scope: a stuck phase lock does not leave the inserting "
    "bucket odd for its readers",
    "[mmap_directory][concurrent][regression]") {
  // The phase lock is held by a live-but-stuck peer for the whole test (its
  // budget is stretched so nobody recovers it).  An insert on `key` parks
  // behind it.  While it waits it must hold nothing: a reader of the same
  // bucket must still see the entry stored earlier, promptly.
  BudgetOverride budgets(/*bucket_us=*/0,
                         /*phase_us=*/uint64_t{10} * 1000 * 1000);
  TestDir d;
  const CacheKey key("phase-scope-reader");
  REQUIRE(d.dir.insert(key, 4096, 512));

  auto phase_holder = d.dir.acquire_phase_lock_for_test();
  std::atomic<bool> started{false};
  bool inserted = true;
  std::thread waiter([&] {
    started.store(true);
    bool busy = false;
    inserted = d.dir.insert(key, 8192, 512, MmapDirectory::kMatchAnyTag,
                            nullptr, nullptr, nullptr, {}, &busy);
  });
  while (!started.load()) {
    std::this_thread::yield();
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  const auto t0 = std::chrono::steady_clock::now();
  const auto found = d.dir.probe(key);
  const auto elapsed = std::chrono::steady_clock::now() - t0;
  REQUIRE(found.has_value());
  REQUIRE(elapsed < std::chrono::milliseconds(20));

  d.dir.release_phase_lock_for_test(phase_holder);
  waiter.join();
  (void)inserted;
}
