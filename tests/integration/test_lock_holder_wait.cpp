// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// A writer waiting on a cross-process lock must not take it over from a
// holder that is alive but descheduled (issue #27).
//
// The mmap directory's phase lock used to presume its holder dead after
// 100k spin hints (~33 us on Apple M), and the per-bucket writer seqlock
// after 100k (~0.9 ms) -- both far below a scheduler quantum.  Both now
// wait by TIME, per holder (LockHolderWait): a short spin, then sleeping
// backoff, and a holder is presumed stuck only after it alone held the lock
// for the whole budget.  A recovered holder's late release is a token CAS
// that can no longer free anyone else's critical section.
//
// The Volume cases use two Volumes on one file, both process 0 of 1: the
// graceful-reload / overlapped-recycle overlap in which two processes own
// the same stripe.  Holders are parked through test seams
// (cyclone-cache-testseams); budgets are shortened through seam overrides
// only where a test must run past them.

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "core/directory.hpp"
#include "core/mmap_directory.hpp"
#include "core/volume.hpp"
#include "cyclone/config.hpp"
#include "cyclone/key.hpp"
#include "support/temp_cache.hpp"

using namespace cyclone;
using namespace std::chrono_literals;

namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t kVolumeBytes = size_t{16} * 1024 * 1024;
constexpr size_t kDocBytes = 2048;

// Scoped override of one of MmapDirectory's lock budgets (microseconds).
class BudgetOverride {
 public:
  BudgetOverride(std::atomic<uint64_t> &slot, std::chrono::microseconds us)
      : _slot(slot) {
    _slot.store(static_cast<uint64_t>(us.count()));
  }
  ~BudgetOverride() { _slot.store(0); }
  BudgetOverride(const BudgetOverride &) = delete;
  BudgetOverride &operator=(const BudgetOverride &) = delete;

 private:
  std::atomic<uint64_t> &_slot;
};

std::shared_ptr<Volume> open_volume(const std::string &path) {
  VolumeConfig vc;
  vc.path = path;
  vc.size = kVolumeBytes;
  vc.verify_checksum_on_read = true;
  MultiProcessConfig mp;
  mp.set_enabled(true).set_process_index(0).set_total_processes(1);
  auto volume = std::make_shared<Volume>(vc, mp);
  REQUIRE(volume->open().has_value());
  return volume;
}

std::vector<std::byte> make_content(std::byte fill) {
  return std::vector<std::byte>(kDocBytes, fill);
}

bool write(Volume &volume, const CacheKey &key,
           std::span<const std::byte> content) {
  auto wh = volume.write_sync(key, content.size());
  return wh.has_value() && wh->write_sync(content).has_value() &&
         wh->close_sync().has_value();
}

std::vector<std::byte> read_content(Volume &volume, const CacheKey &key) {
  auto rh = volume.read_sync(key);
  if (!rh) {
    return {};
  }
  auto c = rh->content();
  return {c.begin(), c.end()};
}

// A live peer Volume parked inside the phase lock of `key`'s stripe, from
// its own thread, until release() -- or, with `hold`, for that long.
class ParkedPhasePeer {
 public:
  ParkedPhasePeer(Volume &peer, const CacheKey &key,
                  std::chrono::milliseconds hold = 0ms)
      : _peer(peer), _key(key) {
    _thread = std::thread([this, hold] {
      _token = _peer.begin_phase_lock_for_test(_key);
      _parked.store(true);
      if (hold != 0ms) {
        // Descheduled, as far as the waiter can tell: the lock stays held
        // and this thread does not run.
        std::this_thread::sleep_for(hold);
      } else {
        while (!_release.load()) {
          std::this_thread::sleep_for(1ms);
        }
      }
      // For a recovered holder this is the LATE release.
      _peer.end_phase_lock_for_test(_key, _token);
    });
    while (!_parked.load()) {
      std::this_thread::yield();
    }
  }
  void release() {
    if (_thread.joinable()) {
      _release.store(true);
      _thread.join();
    }
  }
  [[nodiscard]] MmapDirectory::PhaseLockToken token() const { return _token; }
  ~ParkedPhasePeer() { release(); }
  ParkedPhasePeer(const ParkedPhasePeer &) = delete;
  ParkedPhasePeer &operator=(const ParkedPhasePeer &) = delete;

 private:
  Volume &_peer;
  CacheKey _key;
  MmapDirectory::PhaseLockToken _token;
  std::atomic<bool> _parked{false};
  std::atomic<bool> _release{false};
  std::thread _thread;
};

struct MappedDir {
  static constexpr size_t kBuckets = 64;
  std::vector<std::byte> region =
      std::vector<std::byte>(MmapDirectory::required_size(kBuckets));
  MmapDirectory dir =
      *MmapDirectory::init(std::span<std::byte>(region), kBuckets);
};

}  // namespace

TEST_CASE(
    "Lock holder wait: a phase-lock holder descheduled for longer than a "
    "scheduler quantum is waited out, not usurped",
    "[mmap_directory][multiprocess][concurrent][regression]") {
  // Production budget.  The hold is far past the old ~33 us spin bound and
  // past a 10 ms scheduler quantum, and well inside kPhaseLockBudget.
  constexpr auto kHold = 60ms;
  static_assert(kHold * 4 < MmapDirectory::kPhaseLockBudget);
  TempCacheDir tmp("phaselive");
  auto local = open_volume(tmp.path());
  auto peer = open_volume(tmp.path());
  const CacheKey key("phase-lock-live-holder");
  const auto content = make_content(std::byte{0x21});
  MmapDirectory *dir = local->mmap_directory_for_test(key);
  REQUIRE(dir != nullptr);
  const uint64_t recoveries =
      MmapDirectory::s_phase_lock_recoveries_for_test.load();
  const uint16_t gen = dir->phase_lock_gen_for_test();

  const auto t0 = Clock::now();
  ParkedPhasePeer parked(*peer, key, kHold);
  REQUIRE(dir->phase_lock_value_for_test() == parked.token().value);
  REQUIRE(write(*local, key, content));  // Its insert waits on the peer
  const auto waited = Clock::now() - t0;
  parked.release();

  CAPTURE(std::chrono::duration_cast<std::chrono::microseconds>(waited));
  REQUIRE(waited >= kHold - 5ms);  // It did wait for the holder
  REQUIRE(MmapDirectory::s_phase_lock_recoveries_for_test.load() == recoveries);
  // Two acquisitions (the peer's, the insert's), no recovery.
  REQUIRE(dir->phase_lock_gen_for_test() == static_cast<uint16_t>(gen + 2));
  REQUIRE(dir->phase_lock_value_for_test() == 0);
  REQUIRE(read_content(*local, key) == content);
}

TEST_CASE(
    "Lock holder wait: a phase-lock holder kept past the budget is "
    "recovered, and its late release does not free the next holder",
    "[mmap_directory][multiprocess][concurrent][regression]") {
  constexpr auto kBudget = 50ms;
  BudgetOverride budget(MmapDirectory::s_phase_lock_budget_us_for_test,
                        kBudget);
  TempCacheDir tmp("phasestuck");
  auto local = open_volume(tmp.path());
  auto peer = open_volume(tmp.path());
  const CacheKey key("phase-lock-stuck-holder");
  const auto first = make_content(std::byte{0x31});
  const auto second = make_content(std::byte{0x32});
  MmapDirectory *dir = local->mmap_directory_for_test(key);
  REQUIRE(dir != nullptr);
  REQUIRE(write(*local, key, first));
  const uint64_t recoveries =
      MmapDirectory::s_phase_lock_recoveries_for_test.load();
  const uint16_t gen = dir->phase_lock_gen_for_test();

  ParkedPhasePeer parked(*peer, key);  // Held until released
  const MmapDirectory::PhaseLockToken stale = parked.token();
  const auto t0 = Clock::now();
  REQUIRE(write(*local, key, second));  // Recovers the lock, then lands
  const auto waited = Clock::now() - t0;
  CAPTURE(std::chrono::duration_cast<std::chrono::microseconds>(waited));
  REQUIRE(waited >= kBudget);
  REQUIRE(MmapDirectory::s_phase_lock_recoveries_for_test.load() ==
          recoveries + 1);
  // The peer's acquisition, the recovery, the insert's acquisition.
  REQUIRE(dir->phase_lock_gen_for_test() == static_cast<uint16_t>(gen + 3));
  REQUIRE(read_content(*local, key) == second);

  // A new holder takes the lock under a later generation's token ...
  const MmapDirectory::PhaseLockToken fresh =
      local->begin_phase_lock_for_test(key);
  REQUIRE(fresh.value != stale.value);
  REQUIRE(dir->phase_lock_value_for_test() == fresh.value);
  // ... and the usurped holder's late release must not free it.  (A blind
  // store of 0 would, admitting a second holder into its critical section.)
  parked.release();
  REQUIRE(dir->phase_lock_value_for_test() == fresh.value);
  local->end_phase_lock_for_test(key, fresh);
  REQUIRE(dir->phase_lock_value_for_test() == 0);

  // The lock is healthy: the next write needs no recovery.
  REQUIRE(write(*local, key, first));
  REQUIRE(MmapDirectory::s_phase_lock_recoveries_for_test.load() ==
          recoveries + 1);
  REQUIRE(read_content(*local, key) == first);
}

TEST_CASE(
    "Lock holder wait: a bucket waiter that sits through several short "
    "holders never presumes the latest one stuck",
    "[mmap_directory][directory][concurrent][regression]") {
  // Each holder keeps the bucket for 25 ms, a tenth of the (production)
  // budget, so even a badly oversleeping CI runner stays far inside it;
  // twelve of them in a row keep it odd for 300 ms.  A budget measured from
  // when the WAITER started would run out on the eleventh holder and force
  // it.
  constexpr auto kHold = 25ms;
  constexpr int kHolders = 12;
  static_assert(kHold * kHolders > MmapDirectory::kBucketWriterBudget);
  static_assert(kHold * 10 <= MmapDirectory::kBucketWriterBudget);
  MappedDir d;
  const CacheKey key("bucket-relay");
  const uint64_t recoveries =
      MmapDirectory::s_bucket_recoveries_for_test.load();
  const uint32_t v0 = d.dir.bucket_version(key);

  std::atomic<bool> first_parked{false};
  std::atomic<int> clean_releases{0};
  std::thread holders([&] {
    uint32_t token = d.dir.begin_bucket_write_for_test(key);
    first_parked.store(true);
    for (int i = 0; i < kHolders; ++i) {
      std::this_thread::sleep_for(kHold);
      // Token-checked: completes only if nobody forced the bucket.  (The
      // waiter may take it right after; the final accounting catches a
      // release that went missing.)
      d.dir.end_bucket_write_for_test(key, token);
      clean_releases.fetch_add(1);
      if (i + 1 < kHolders) {
        token = d.dir.begin_bucket_write_for_test(key);  // Next holder
      }
    }
  });
  while (!first_parked.load()) {
    std::this_thread::yield();
  }
  const uint32_t waiter_token = d.dir.begin_bucket_write_for_test(key);
  d.dir.end_bucket_write_for_test(key, waiter_token);
  holders.join();

  REQUIRE(clean_releases.load() == kHolders);
  REQUIRE(MmapDirectory::s_bucket_recoveries_for_test.load() == recoveries);
  // Every bracket, the waiter's included, advanced the version by exactly 2:
  // no forced release, no late release that went missing.
  REQUIRE(d.dir.bucket_version(key) == v0 + 2 * (kHolders + 1));
}

TEST_CASE(
    "Lock holder wait: a bucket holder kept past the budget is recovered, "
    "and its late release is a no-op",
    "[mmap_directory][directory][concurrent][regression]") {
  constexpr auto kBudget = 40ms;
  BudgetOverride budget(MmapDirectory::s_bucket_writer_budget_us_for_test,
                        kBudget);
  MappedDir d;
  const CacheKey key("bucket-stuck");
  const uint64_t recoveries =
      MmapDirectory::s_bucket_recoveries_for_test.load();

  const uint32_t stale = d.dir.begin_bucket_write_for_test(key);
  uint32_t waiter_token = 0;
  Clock::duration waited{};
  std::thread waiter([&] {
    const auto t0 = Clock::now();
    waiter_token = d.dir.begin_bucket_write_for_test(key);
    waited = Clock::now() - t0;
  });
  waiter.join();
  REQUIRE(waited >= kBudget);
  REQUIRE(MmapDirectory::s_bucket_recoveries_for_test.load() == recoveries + 1);
  REQUIRE(waiter_token == stale + 2);  // Forced from stale+1 to stale+2
  REQUIRE((d.dir.bucket_version(key) & 1) == 1);  // The waiter holds it

  // The recovered holder's late release must not end the waiter's bracket.
  d.dir.end_bucket_write_for_test(key, stale);
  REQUIRE(d.dir.bucket_version(key) == waiter_token + 1);
  d.dir.end_bucket_write_for_test(key, waiter_token);
  REQUIRE(d.dir.bucket_version(key) == waiter_token + 2);
}

TEST_CASE(
    "Lock holder wait: the write lock waits out a live holder past the old "
    "spin bound, and a taken-over holder's late release frees nobody",
    "[mmap_directory][multiprocess][concurrent][regression][writelock]") {
  MappedDir d;
  MmapDirectory &dir = d.dir;

  SECTION("a live holder is waited on, not recovered") {
    auto holder = dir.acquire_write_lock();
    REQUIRE(holder.acquired);
    MmapDirectory::WriteLockToken waiter;
    std::thread t([&] { waiter = dir.acquire_write_lock(); });
    std::this_thread::sleep_for(50ms);  // Holder "descheduled" inside
    REQUIRE(dir.revalidate_write_lock(holder));
    dir.release_write_lock(holder);
    t.join();
    REQUIRE(waiter.acquired);
    REQUIRE_FALSE(waiter.forced_release);
    REQUIRE_FALSE(waiter.escalated_takeover);
    REQUIRE(waiter.live_waits > 0);  // It slept, probing liveness
    REQUIRE(waiter.generation == static_cast<uint16_t>(holder.generation + 1));
    dir.release_write_lock(waiter);
    REQUIRE(dir.write_lock_value_for_test() == 0);
  }

  SECTION("an escalated takeover's late release frees nobody") {
    BudgetOverride budget(MmapDirectory::s_write_lock_escalation_us_for_test,
                          30ms);
    auto usurped = dir.acquire_write_lock();
    REQUIRE(usurped.acquired);
    MmapDirectory::WriteLockToken usurper;
    std::thread t([&] { usurper = dir.acquire_write_lock(); });
    t.join();  // This process is alive: only the escalation budget frees it
    REQUIRE(usurper.acquired);
    REQUIRE(usurper.escalated_takeover);
    REQUIRE(usurper.value != usurped.value);
    REQUIRE_FALSE(dir.revalidate_write_lock(usurped));
    dir.release_write_lock(usurper);

    auto next = dir.acquire_write_lock();  // A later, uncontended holder
    REQUIRE(next.acquired);
    REQUIRE(dir.write_lock_value_for_test() == next.value);
    dir.release_write_lock(usurped);  // Late: must not free `next`
    REQUIRE(dir.write_lock_value_for_test() == next.value);
    REQUIRE(dir.revalidate_write_lock(next));
    dir.release_write_lock(next);
    REQUIRE(dir.write_lock_value_for_test() == 0);
  }
}

// Holds a lock through a test seam, releasing and re-acquiring it at once
// every kRelayHold until stop(): a live peer that a sleeping waiter
// essentially never sees free.  `take` returns a holder token, `check`
// reports whether that holder still owns the lock, `give` releases it.
constexpr auto kRelayHold = 10ms;
// Slack over the cap for a loaded CI runner.
constexpr auto kCapMargin = 750ms;
// The waiter may still catch one of the relay's release-to-re-acquire gaps
// (its brief spin after each sleep) and take the lock fairly; each attempt
// must then have ended without a takeover, inside the bound.  Up to this
// many attempts are made to observe the give-up itself (a slow runner may
// catch several gaps in a row).
constexpr int kRelayAttempts = 20;
// Under ThreadSanitizer every release-to-re-acquire gap is wide enough that
// the waiter nearly always catches one before the cap; there only the bound
// and the absence of a takeover are checked.
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define CYCLONE_TEST_UNDER_TSAN 1
#endif
#endif
#if defined(__SANITIZE_THREAD__)
#define CYCLONE_TEST_UNDER_TSAN 1
#endif
#ifdef CYCLONE_TEST_UNDER_TSAN
constexpr bool kExpectGiveUp = false;
#else
constexpr bool kExpectGiveUp = true;
#endif

template <typename Token>
class Relay {
 public:
  template <typename Take, typename Check, typename Give>
  Relay(Take take, Check check, Give give) {
    _thread = std::thread([this, take, check, give] {
      Token token = take();
      _parked.store(true);
      // Bounded, so a waiter that never gives up fails its time check
      // instead of hanging the test.
      const auto end = Clock::now() + 10s;
      while (!_stop.load() && Clock::now() < end) {
        std::this_thread::sleep_for(kRelayHold);
        if (!check(token)) {
          _lost.fetch_add(1);  // Taken from a live holder
        }
        give(token);
        token = take();
        _holds.fetch_add(1);
      }
      give(token);
    });
    while (!_parked.load()) {
      std::this_thread::yield();
    }
  }
  void stop() {
    if (_thread.joinable()) {
      _stop.store(true);
      _thread.join();
    }
  }
  ~Relay() { stop(); }
  Relay(const Relay &) = delete;
  Relay &operator=(const Relay &) = delete;
  [[nodiscard]] int lost() const { return _lost.load(); }
  [[nodiscard]] int holds() const { return _holds.load(); }

 private:
  std::atomic<bool> _parked{false};
  std::atomic<bool> _stop{false};
  std::atomic<int> _lost{0};
  std::atomic<int> _holds{0};
  std::thread _thread;
};

using Ms = std::chrono::milliseconds;

TEST_CASE(
    "Lock holder wait: a write behind a live peer relaying the phase lock "
    "reports Busy within the cap, and never usurps it",
    "[mmap_directory][multiprocess][concurrent][regression]") {
  // Every relay holder re-acquires the instant it releases.  Two things are
  // pinned:
  //   - no takeover: each hold (10 ms) is far inside the 100 ms per-holder
  //     budget, and only the acquisition generation tells the waiter the
  //     holder changed (before it, every holder of a generation looked the
  //     same and the waiter usurped the relay at the budget);
  //   - bounded stall: the lock is unfair, so the waiter gives up after
  //     kLockWaitCap of changing holders and the write reports Busy (it
  //     used to wait for as long as the relay ran).
  BudgetOverride budget(MmapDirectory::s_phase_lock_budget_us_for_test, 100ms);
  TempCacheDir tmp("phaserelay");
  auto local = open_volume(tmp.path());
  auto peer = open_volume(tmp.path());
  const CacheKey key("phase-lock-relay");
  const auto content = make_content(std::byte{0x41});
  MmapDirectory *dir = local->mmap_directory_for_test(key);
  MmapDirectory *peer_dir = peer->mmap_directory_for_test(key);
  REQUIRE(dir != nullptr);
  REQUIRE(peer_dir != nullptr);
  const uint64_t recoveries =
      MmapDirectory::s_phase_lock_recoveries_for_test.load();
  const uint64_t give_ups = MmapDirectory::s_lock_give_ups_for_test.load();

  Relay<MmapDirectory::PhaseLockToken> relay(
      [&] { return peer_dir->acquire_phase_lock_for_test(); },
      [&](const MmapDirectory::PhaseLockToken &t) {
        return peer_dir->phase_lock_gen_for_test() == t.generation &&
               peer_dir->phase_lock_value_for_test() == t.value;
      },
      [&](const MmapDirectory::PhaseLockToken &t) {
        peer_dir->release_phase_lock_for_test(t);
      });

  bool gave_up = false;
  for (int attempt = 0; attempt < kRelayAttempts && !gave_up; ++attempt) {
    const CacheKey attempt_key("phase-lock-relay-" + std::to_string(attempt));
    auto wh = local->write_sync(attempt_key, content.size());
    REQUIRE(wh.has_value());
    REQUIRE(wh->write_sync(content).has_value());
    const auto t0 = Clock::now();
    auto closed = wh->close_sync();  // The insert waits on the relay
    const auto waited = Clock::now() - t0;
    CAPTURE(attempt, std::chrono::duration_cast<Ms>(waited).count(),
            relay.holds());
    REQUIRE(waited < MmapDirectory::kLockWaitCap + kCapMargin);
    if (closed.has_value()) {
      REQUIRE(read_content(*local, attempt_key) == content);
      continue;  // Caught a gap: acquired fairly
    }
    REQUIRE(closed.error() == CacheError::Busy);
    REQUIRE(waited >= MmapDirectory::kLockWaitCap);
    // The abandoned write published nothing.
    REQUIRE_FALSE(local->read_sync(attempt_key).has_value());
    gave_up = true;
  }
  REQUIRE((gave_up || !kExpectGiveUp));
  REQUIRE(MmapDirectory::s_lock_give_ups_for_test.load() ==
          give_ups + (gave_up ? 1 : 0));
  REQUIRE(MmapDirectory::s_phase_lock_recoveries_for_test.load() == recoveries);
  relay.stop();
  REQUIRE(relay.lost() == 0);  // The peer was never disturbed
  REQUIRE(dir->phase_lock_value_for_test() == 0);

  // Once the relay stops, the next write lands.
  REQUIRE(write(*local, key, content));
  REQUIRE(read_content(*local, key) == content);
}

TEST_CASE(
    "Lock holder wait: a write-lock waiter behind a live relaying peer gives "
    "up within the cap, and never takes it over",
    "[mmap_directory][multiprocess][concurrent][regression][writelock]") {
  // As the phase-lock relay, with the same PID on both sides (two threads
  // of one peer look alike by PID and token): the escalation budget must
  // restart with every acquisition, and the cap must end the wait.
  BudgetOverride budget(MmapDirectory::s_write_lock_escalation_us_for_test,
                        100ms);
  MappedDir d;
  MmapDirectory &dir = d.dir;
  Relay<MmapDirectory::WriteLockToken> relay(
      [&] { return dir.acquire_write_lock(/*capped=*/false); },
      [&](const MmapDirectory::WriteLockToken &t) {
        return dir.revalidate_write_lock(t);
      },
      [&](const MmapDirectory::WriteLockToken &t) {
        dir.release_write_lock(t);
      });

  bool gave_up = false;
  for (int attempt = 0; attempt < kRelayAttempts && !gave_up; ++attempt) {
    const auto t0 = Clock::now();
    const auto waiter = dir.acquire_write_lock();  // Capped
    const auto waited = Clock::now() - t0;
    CAPTURE(attempt, std::chrono::duration_cast<Ms>(waited).count(),
            relay.holds());
    REQUIRE_FALSE(waiter.escalated_takeover);
    REQUIRE_FALSE(waiter.forced_release);
    REQUIRE(waited < MmapDirectory::kLockWaitCap + kCapMargin);
    if (waiter.acquired) {
      dir.release_write_lock(waiter);  // Caught a gap: acquired fairly
      continue;
    }
    REQUIRE(waiter.gave_up);
    REQUIRE(waited >= MmapDirectory::kLockWaitCap);
    gave_up = true;
  }
  REQUIRE((gave_up || !kExpectGiveUp));
  relay.stop();
  REQUIRE(relay.lost() == 0);
  REQUIRE(dir.write_lock_value_for_test() == 0);

  const auto next = dir.acquire_write_lock();
  REQUIRE(next.acquired);
  dir.release_write_lock(next);
}

TEST_CASE(
    "Lock holder wait: a remove behind a live peer relaying the bucket gives "
    "up within the cap, and never forces it",
    "[mmap_directory][directory][concurrent][regression]") {
  BudgetOverride budget(MmapDirectory::s_bucket_writer_budget_us_for_test,
                        100ms);
  MappedDir d;
  const CacheKey key("bucket-relay-capped");
  REQUIRE(d.dir.insert(key, 4096, 512));
  const uint64_t recoveries =
      MmapDirectory::s_bucket_recoveries_for_test.load();

  Relay<uint32_t> relay(
      [&] { return d.dir.begin_bucket_write_for_test(key); },
      [&](uint32_t t) { return d.dir.bucket_version(key) == t + 1; },
      [&](uint32_t t) { d.dir.end_bucket_write_for_test(key, t); });

  bool gave_up = false;
  for (int attempt = 0; attempt < kRelayAttempts && !gave_up; ++attempt) {
    // A decoy tag at another offset: removing it changes nothing, so an
    // attempt that catches a gap leaves the real entry in place.
    bool busy = false;
    const auto t0 = Clock::now();
    const bool removed = d.dir.remove_at(key, 8192, &busy);
    const auto waited = Clock::now() - t0;
    CAPTURE(attempt, std::chrono::duration_cast<Ms>(waited).count(),
            relay.holds());
    REQUIRE_FALSE(removed);
    REQUIRE(waited < MmapDirectory::kLockWaitCap + kCapMargin);
    if (!busy) {
      continue;  // Caught a gap: acquired fairly
    }
    REQUIRE(waited >= MmapDirectory::kLockWaitCap);
    gave_up = true;
  }
  REQUIRE((gave_up || !kExpectGiveUp));
  REQUIRE(MmapDirectory::s_bucket_recoveries_for_test.load() == recoveries);
  relay.stop();
  REQUIRE(relay.lost() == 0);
  REQUIRE((d.dir.bucket_version(key) & 1) == 0);

  bool busy = false;
  REQUIRE(d.dir.remove_at(key, 4096, &busy));
  REQUIRE_FALSE(busy);
}

TEST_CASE("Lock holder wait: a capped wait gives up only on changing holders",
          "[directory][concurrent]") {
  using Step = LockHolderWait::Step;
  SECTION("holders that keep changing reach the cap") {
    LockHolderWait wait(1s, 30ms);
    const auto t0 = Clock::now();
    uint64_t holder = 1;
    auto switched = t0;
    Step step = Step::kSpun;
    while ((step = wait.wait(holder)) != Step::kGiveUp) {
      REQUIRE(step != Step::kExpired);
      REQUIRE(Clock::now() - t0 < 1s);
      if (Clock::now() - switched >= 5ms) {
        ++holder;
        switched = Clock::now();
      }
    }
    REQUIRE(Clock::now() - t0 >= 30ms);
  }
  SECTION("one holder that stays is governed by the budget alone") {
    LockHolderWait wait(60ms, 10ms);
    const auto t0 = Clock::now();
    Step step = Step::kSpun;
    while ((step = wait.wait(7)) != Step::kExpired) {
      REQUIRE(step != Step::kGiveUp);
    }
    REQUIRE(Clock::now() - t0 >= 60ms);
  }
}

TEST_CASE(
    "Lock holder wait: the budget restarts when the holder changes, and "
    "only a holder that kept the lock for all of it expires",
    "[directory][concurrent]") {
  using Step = LockHolderWait::Step;
  LockHolderWait wait(20ms);
  const auto t0 = Clock::now();
  // Holder 1 for ~15 ms, then holder 2: the budget restarts at the switch.
  while (Clock::now() - t0 < 15ms) {
    REQUIRE(wait.wait(1) != Step::kExpired);
  }
  const auto t1 = Clock::now();
  Step step = Step::kSpun;
  while ((step = wait.wait(2)) != Step::kExpired) {
  }
  const auto expired_after = Clock::now() - t1;
  CAPTURE(std::chrono::duration_cast<std::chrono::microseconds>(expired_after));
  REQUIRE(expired_after >= 20ms);
  REQUIRE(wait.sleeps() > 0);
}

TEST_CASE(
    "Lock holder wait: a short backoff sleep is short (Windows high-"
    "resolution timer)",
    "[directory][concurrent]") {
  // At Windows' default 15.6 ms timer resolution a plain 10 us sleep takes
  // ~15.6 ms, past the 5 ms seqlock read budget.  Median of several, loose
  // bound for loaded CI runners.
  std::vector<Clock::duration> took;
  for (int i = 0; i < 21; ++i) {
    const auto t0 = Clock::now();
    wait_detail::sleep_for(10us);
    took.push_back(Clock::now() - t0);
  }
  std::nth_element(took.begin(), took.begin() + 10, took.end());
  CAPTURE(std::chrono::duration_cast<std::chrono::microseconds>(took[10]));
  REQUIRE(took[10] < 5ms);
}

TEST_CASE(
    "Lock holder wait: a remove whose bucket wait gives up leaves the key "
    "exactly as it was, never an older version readable",
    "[mmap_directory][concurrent][regression]") {
  // Two entries for one key: the current version and an older one (the
  // shape a retained previous-pass entry or a stale survivor gives).  The
  // remove must take both in one bucket bracket: if the capped wait gives
  // up, nothing is removed and reads still return what they did; if it
  // succeeds, nothing is left.  Removing them one bracket at a time, a
  // give-up after the current entry went left the older one serving.
  TempCacheDir tmp("removecap");
  auto volume = open_volume(tmp.path());
  const CacheKey key("remove-all-or-nothing");
  const auto older = make_content(std::byte{0x51});
  const auto current = make_content(std::byte{0x52});
  MmapDirectory *dir = volume->mmap_directory_for_test(key);
  REQUIRE(dir != nullptr);

  auto entry_offsets = [&] {
    std::vector<uint64_t> offsets;
    (void)dir->probe_each_all_phases(key, [&](const DirEntry &e) {
      offsets.push_back(e.offset());
      return true;
    });
    std::sort(offsets.begin(), offsets.end());
    return offsets;
  };

  REQUIRE(write(*volume, key, older));
  const auto first = entry_offsets();
  REQUIRE(first.size() == 1);
  REQUIRE(write(*volume, key, current));  // Replaces the entry in place
  REQUIRE(entry_offsets().size() == 1);
  // Re-add an entry for the older document beside it.
  REQUIRE(
      dir->insert(key, first[0], older.size(), Directory::kNoVerifiedEntry));
  const auto both = entry_offsets();
  REQUIRE(both.size() == 2);
  const auto served = read_content(*volume, key);
  REQUIRE_FALSE(served.empty());

  struct GiveUpAfter {
    explicit GiveUpAfter(int n) {
      MmapDirectory::s_bucket_give_up_after_for_test.store(n);
    }
    ~GiveUpAfter() { MmapDirectory::s_bucket_give_up_after_for_test.store(-1); }
    GiveUpAfter(const GiveUpAfter &) = delete;
    GiveUpAfter &operator=(const GiveUpAfter &) = delete;
  };

  SECTION("the first bucket wait gives up: nothing removed") {
    GiveUpAfter give_up(0);
    auto removed = volume->remove_sync(key);
    REQUIRE_FALSE(removed.has_value());
    REQUIRE(removed.error() == CacheError::Busy);
    REQUIRE(entry_offsets() == both);
    REQUIRE(read_content(*volume, key) == served);
  }
  SECTION("a later bucket wait would give up: all or nothing") {
    GiveUpAfter give_up(1);
    auto removed = volume->remove_sync(key);
    if (removed.has_value()) {
      REQUIRE(entry_offsets().empty());
      REQUIRE_FALSE(volume->read_sync(key).has_value());
    } else {
      REQUIRE(removed.error() == CacheError::Busy);
      REQUIRE(entry_offsets() == both);
      REQUIRE(read_content(*volume, key) == served);
    }
  }
}
