// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// fork() while the cache's background threads run.
//
// A forking server (an Apache parent, an nginx master) opens and start()s
// the cache itself and then forks its workers, so it is the parent that
// runs the library's background threads: the hit-count flush and the
// periodic directory sync.  fork() copies memory as it is at that instant
// and only the forking thread survives, so a lock one of those threads held
// at that instant stays locked in the child for good.  Two hangs came of it:
//
//   * the flush thread sweeps the 4096 hit-tracker stripe mutexes; a child
//     that inherited one locked blocked on its first read of a key hashing
//     to that stripe;
//   * the sync thread holds a shard of the cache gate (shared) for its whole
//     msync/fsync sweep; a child that inherited it blocked when it destroyed
//     the cache (stop() followed by the destructor's second stop(), which
//     took the gate exclusively).
//
// The tests below run both intervals at 1 ms, so a pass is in flight at
// most forks, and fork children in a loop.  Each child reads one key per
// hit-tracker stripe, writes, stops and destroys the cache and must _exit(0)
// before a deadline.  The parent never waits without a bound: a child that
// misses the deadline is killed and the test fails.
//
// Runtime: the default iteration counts take a few seconds.  Set
// CYCLONE_FORK_STORM_ITERATIONS to soak (for example 20000).
//
// Sanitizers: the children start no thread, so this runs under
// ThreadSanitizer as well (it only refuses a forked child that creates
// threads); the iteration count and deadline are scaled for the slower
// build.

#ifndef _WIN32

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "core/fork_gate.hpp"
#include "core/hit_tracker.hpp"
#include "cyclone/cache.hpp"
#include "cyclone/config.hpp"
#include "cyclone/key.hpp"
#include "support/temp_cache.hpp"

#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
#define CYCLONE_FORK_STORM_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
#define CYCLONE_FORK_STORM_SANITIZED 1
#endif
#endif

using namespace cyclone;

namespace {

using Clock = std::chrono::steady_clock;
using Ms = std::chrono::milliseconds;

#if defined(CYCLONE_FORK_STORM_SANITIZED)
constexpr int kSanitizerSlowdown = 5;
#else
constexpr int kSanitizerSlowdown = 1;
#endif

// How long a child may take.  A healthy child needs a few milliseconds; the
// budget only has to be far above scheduling noise on a loaded machine.
constexpr auto kChildDeadline = Ms{10'000} * kSanitizerSlowdown;
// Distinct keys the children write between them (child N writes key
// N % kChildKeys).
constexpr int kChildKeys = 64;
// The volume is a ring: every write, a rewrite included, takes new space,
// and the oldest documents go when it wraps.  In a soak the parent stores
// the per-stripe documents again every so many forks, long before the
// children's writes have come round to them.
constexpr int kRefreshEveryForks = 20'000;
// Children alive at once.  More than one, so that forks keep landing while
// earlier children are still reading; few, so the parent's own threads get
// processor time on a small runner.
constexpr size_t kChildrenAtOnce = 4;

// The number of forks: `fallback` unless CYCLONE_FORK_STORM_ITERATIONS says
// otherwise (the soak mode).
int storm_iterations(int fallback) {
  if (const char* env = std::getenv("CYCLONE_FORK_STORM_ITERATIONS")) {
    const long parsed = std::strtol(env, nullptr, 10);
    if (parsed > 0) {
      return static_cast<int>(std::min<long>(parsed, 10'000'000));
    }
  }
  return std::max(1, fallback / kSanitizerSlowdown);
}

std::span<const std::byte> as_bytes(const std::string& text) {
  return std::as_bytes(std::span<const char>(text.data(), text.size()));
}

bool write_document(Cache& cache, const CacheKey& key,
                    const std::string& body) {
  // Busy is transient by contract (a peer held the cross-process write lock
  // or the key's bucket): retry a few times before calling it a failure.
  for (int attempt = 0; attempt < 50; ++attempt) {
    auto handle = cache.write_sync(key, body.size());
    if (handle.has_value() && handle->write_sync(as_bytes(body)).has_value() &&
        handle->close_sync().has_value()) {
      return true;
    }
    std::this_thread::sleep_for(Ms{2});
  }
  return false;
}

bool read_document(Cache& cache, const CacheKey& key) {
  for (int attempt = 0; attempt < 50; ++attempt) {
    auto handle = cache.read_sync(key);
    if (handle.has_value()) {
      return !handle->content().empty();
    }
    if (handle.error() != CacheError::Busy) {
      return false;
    }
    std::this_thread::sleep_for(Ms{2});
  }
  return false;
}

// One key for every hit-tracker stripe, so a child that reads them all
// takes every stripe mutex once.
std::vector<CacheKey> one_key_per_stripe() {
  const size_t stripes = HitTracker::stripe_count();
  std::vector<CacheKey> keys;
  keys.reserve(stripes);
  std::vector<bool> covered(stripes, false);
  for (size_t candidate = 0; keys.size() < stripes; ++candidate) {
    CacheKey key("fork-storm-" + std::to_string(candidate));
    const size_t stripe = HitTracker::stripe_of(key);
    if (!covered[stripe]) {
      covered[stripe] = true;
      keys.push_back(key);
    }
  }
  return keys;
}

struct StormConfig {
  Ms hit_flush_interval;
  Ms directory_sync_interval;  // 0 = no directory sync thread
  int iterations;
};

struct StormResult {
  int forked = 0;
  int hung = 0;      // Missed the deadline: killed
  int bad_exit = 0;  // Exited, but not with 0
  int last_bad = 0;  // Exit code (or 1000 + signal) of the last bad child
  int fork_failed = 0;
  int parent_io_failed = 0;  // Parent reads/writes during the storm
};

// What a forked child does with the cache it inherited.  Never returns: no
// Catch2 assertion and no destructor of the parent's test state may run in
// the child (the temp directory guard would delete the parent's files).
[[noreturn]] void child_main(std::unique_ptr<Cache>& cache,
                             const std::vector<CacheKey>& keys, int ordinal) {
  // Belt and braces: never outlive the parent's deadline by much.
  ::alarm(static_cast<unsigned>(
      std::chrono::duration_cast<std::chrono::seconds>(kChildDeadline).count() +
      5));
  int rc = 90;  // An exception escaped
  try {
    rc = [&]() -> int {
      // Every stripe: a stripe mutex inherited locked blocks here.
      for (const auto& key : keys) {
        if (!read_document(*cache, key)) {
          return 10;
        }
      }
      // A bounded set of keys: a soak must not fill the directory with one
      // new key per child and push the per-stripe documents out.
      const CacheKey own("fork-storm-child-" +
                         std::to_string(ordinal % kChildKeys));
      if (!write_document(*cache, own, "written by a forked child")) {
        return 11;
      }
      if (!read_document(*cache, own)) {
        return 12;
      }
      // The sequence a server module runs at worker exit: an explicit
      // stop(), then the destructor (which calls stop() again).  A cache
      // gate inherited locked blocks in the second one.
      cache->stop();
      cache.reset();
      return 0;
    }();
  } catch (...) {
    rc = 91;
  }
  ::_exit(rc);
}

struct LiveChild {
  pid_t pid;
  Clock::time_point deadline;
};

// Reaps what has exited and kills what is past its deadline.  Returns true
// when at least one child left the set.
bool reap_children(std::vector<LiveChild>& live, StormResult& result) {
  bool any = false;
  for (size_t i = 0; i < live.size();) {
    int status = 0;
    const pid_t done = ::waitpid(live[i].pid, &status, WNOHANG);
    bool gone = false;
    if (done == live[i].pid) {
      gone = true;
      if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        ++result.bad_exit;
        result.last_bad =
            WIFEXITED(status) ? WEXITSTATUS(status) : 1000 + WTERMSIG(status);
      }
    } else if (Clock::now() >= live[i].deadline) {
      gone = true;
      ++result.hung;
      ::kill(live[i].pid, SIGKILL);
      ::waitpid(live[i].pid, &status, 0);
    }
    if (gone) {
      live[i] = live.back();
      live.pop_back();
      any = true;
    } else {
      ++i;
    }
  }
  return any;
}

struct StormCache {
  TempCacheDir dir{"fork_storm"};
  std::unique_ptr<Cache> cache;
  std::vector<CacheKey> keys;
};

// Stores (or stores again) one document per hit-tracker stripe; returns how
// many writes succeeded.
size_t store_stripe_documents(StormCache& storm) {
  size_t stored = 0;
  for (const auto& key : storm.keys) {
    stored +=
        write_document(*storm.cache, key, "one document per stripe") ? 1 : 0;
  }
  return stored;
}

// Opens a cache the way a forking server does (persistent directory, one
// process index shared by the whole family) and stores one document per
// hit-tracker stripe.
void open_storm_cache(StormCache& storm, const StormConfig& config) {
  CacheConfig cache_config;
  cache_config.set_ram_cache_size(0);  // Every read reaches the hit tracker
  cache_config.enable_hit_tracking = true;
  cache_config.hit_flush_interval = config.hit_flush_interval;
  cache_config.set_multi_process(0, 1);
  cache_config.set_directory_sync_interval(config.directory_sync_interval);

  auto created = Cache::create(cache_config);
  REQUIRE(created.has_value());
  storm.cache = std::move(*created);
  REQUIRE(
      storm.cache->add_volume(storm.dir.path(), size_t{64} << 20).has_value());
  REQUIRE(storm.cache->start().has_value());

  storm.keys = one_key_per_stripe();
  REQUIRE(storm.keys.size() == HitTracker::stripe_count());
  REQUIRE(store_stripe_documents(storm) == storm.keys.size());
  size_t found = 0;
  for (const auto& key : storm.keys) {
    found += read_document(*storm.cache, key) ? 1 : 0;
  }
  REQUIRE(found == storm.keys.size());
}

// Forks `config.iterations` children, a few at a time, and gives up at the
// first one that hangs (each hung child costs a full deadline).  The parent
// keeps reading and writing meanwhile -- on the forking thread: the library
// keeps its OWN threads out of fork()'s way, while a second application
// thread inside a cache call at the instant of a fork is the application's
// to avoid (see the fork contract in cyclone/cache.hpp).  `at_half_time`
// runs once, on the forking thread, when half the children have been forked.
StormResult run_storm(StormCache& storm, const StormConfig& config,
                      const std::function<void()>& at_half_time = {}) {
  StormResult result;
  std::vector<LiveChild> live;
  live.reserve(kChildrenAtOnce);
  const CacheKey parent_key("fork-storm-parent");

  bool refresh_due = false;
  while ((result.forked < config.iterations && result.hung == 0) ||
         !live.empty()) {
    if (refresh_due && live.empty()) {
      // No child is reading while the documents are replaced.
      refresh_due = false;
      if (store_stripe_documents(storm) != storm.keys.size()) {
        ++result.parent_io_failed;
      }
    }
    while (!refresh_due && live.size() < kChildrenAtOnce &&
           result.forked < config.iterations && result.hung == 0) {
      const pid_t pid = ::fork();
      if (pid < 0) {
        ++result.fork_failed;
        break;
      }
      if (pid == 0) {
        child_main(storm.cache, storm.keys, result.forked);
      }
      live.push_back({pid, Clock::now() + kChildDeadline});
      ++result.forked;
      if (at_half_time && result.forked == (config.iterations + 1) / 2) {
        at_half_time();
      }
      refresh_due = result.forked % kRefreshEveryForks == 0 &&
                    result.forked < config.iterations;

      // The parent is a full user of the cache during the storm.
      if (result.forked % 16 == 0) {
        const auto& key =
            storm.keys[static_cast<size_t>(result.forked) % storm.keys.size()];
        if (!read_document(*storm.cache, key) ||
            !write_document(*storm.cache, parent_key,
                            "parent " + std::to_string(result.forked))) {
          ++result.parent_io_failed;
        }
      }
    }
    if (result.fork_failed != 0 && live.empty()) {
      break;
    }
    if (!reap_children(live, result)) {
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
  }
  return result;
}

void check_storm(const StormResult& result, const StormConfig& config) {
  INFO("forked " << result.forked << " of " << config.iterations
                 << " children; hung " << result.hung << ", bad exit "
                 << result.bad_exit << " (last " << result.last_bad
                 << "), fork failures " << result.fork_failed);
  CHECK(result.hung == 0);
  CHECK(result.bad_exit == 0);
  CHECK(result.fork_failed == 0);
  CHECK(result.parent_io_failed == 0);
  CHECK(result.forked == config.iterations);
}

// Polls stats() until `advanced` says so, for at most `budget`.
template <typename Predicate>
bool eventually(Cache& cache, Ms budget, Predicate advanced) {
  const auto deadline = Clock::now() + budget;
  for (;;) {
    if (advanced(cache.stats())) {
      return true;
    }
    if (Clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(Ms{5});
  }
}

}  // namespace

TEST_CASE(
    "Fork storm: a child forked during a hit-count flush reads every stripe",
    "[fork][forkstorm][multiprocess]") {
  // Only the flush thread is busy: the directory sync thread is off, so a
  // hang here is a hit-tracker stripe mutex inherited locked.
  const StormConfig config{Ms{1}, Ms{0}, storm_iterations(2000)};
  StormCache storm;
  open_storm_cache(storm, config);

  const StormResult result = run_storm(storm, config);
  check_storm(result, config);

  storm.cache->stop();
}

TEST_CASE(
    "Fork storm: a child forked during a directory sync destroys its cache",
    "[fork][forkstorm][multiprocess]") {
  // Only the sync thread is busy: the flush thread sleeps for an hour, so a
  // hang here is a cache gate shard inherited locked.
  const StormConfig config{std::chrono::hours{1}, Ms{1}, storm_iterations(500)};
  StormCache storm;
  open_storm_cache(storm, config);

  const StormResult result = run_storm(storm, config);
  check_storm(result, config);

  storm.cache->stop();
}

TEST_CASE(
    "Fork storm: the parent's background passes keep running during and "
    "after the forks",
    "[fork][forkstorm][multiprocess]") {
  // At least 64 forks: the parent reads on every 16th, and the second half
  // of the storm needs a few of those reads for the flush thread to flush.
  const StormConfig config{Ms{1}, Ms{1}, std::max(64, storm_iterations(1000))};
  StormCache storm;
  open_storm_cache(storm, config);
  Cache& cache = *storm.cache;

  // The parent's own hits reach the disk through its flush thread.
  const auto record_parent_hits = [&] {
    for (size_t i = 0; i < 64; ++i) {
      (void)read_document(cache, storm.keys[i]);
    }
  };

  // Compare the parent's counters between half time and the end, so the
  // difference was made WHILE children were being forked: fork() holds the
  // passes off only for its own duration, and they must still get through
  // between forks.  (run_storm() reads on the forking thread every 16
  // forks, which is what gives the flush thread something to flush.)
  CacheStats half_time;
  const StormResult result =
      run_storm(storm, config, [&] { half_time = cache.stats(); });
  const CacheStats after = cache.stats();

  check_storm(result, config);
  const uint64_t syncs_during =
      after.directory_syncs - half_time.directory_syncs;
  const uint64_t flushes_during =
      after.hit_flush_successes - half_time.hit_flush_successes;
  INFO("second half of the storm: " << syncs_during << " directory syncs, "
                                    << flushes_during << " hits flushed");
  CHECK(syncs_during > 0);
  CHECK(flushes_during > 0);

  // And after it: both threads are still alive and passing.
  record_parent_hits();
  CHECK(eventually(cache, Ms{5000} * kSanitizerSlowdown,
                   [&](const CacheStats& now) {
                     return now.directory_syncs > after.directory_syncs &&
                            now.hit_flush_successes > after.hit_flush_successes;
                   }));

  // The children's writes are visible to the parent, and the parent still
  // reads every stripe and writes.
  size_t found = 0;
  for (const auto& key : storm.keys) {
    found += read_document(cache, key) ? 1 : 0;
  }
  CHECK(found == storm.keys.size());
  CHECK(read_document(cache, CacheKey("fork-storm-child-0")));
  CHECK(write_document(cache, CacheKey("fork-storm-after"), "after the storm"));

  cache.stop();
  CHECK_FALSE(cache.is_running());
}

TEST_CASE("Fork: a forked child's inherited cache is finished after stop()",
          "[fork][lifecycle][multiprocess]") {
  CacheConfig cache_config;
  cache_config.set_ram_cache_size(0);
  cache_config.set_multi_process(0, 1);
  cache_config.optimization_config.enabled = true;
  TempCacheDir dir("fork_contract");
  auto created = Cache::create(cache_config);
  REQUIRE(created.has_value());
  std::unique_ptr<Cache> cache = std::move(*created);
  REQUIRE(cache->add_volume(dir.path(), size_t{32} << 20).has_value());
  REQUIRE(cache->start().has_value());
  REQUIRE(cache->optimization_engine() != nullptr);
  const CacheKey key("fork-contract");
  REQUIRE(write_document(*cache, key, "before the fork"));

  const pid_t pid = ::fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    ::alarm(30);
    // Every step runs whatever the earlier ones found; the exit code names
    // the first that failed.
    int rc = 0;
    const auto expect = [&rc](bool ok, int code) {
      if (!ok && rc == 0) {
        rc = code;
      }
    };
    // The engine's threads are not here: the accessor says so at once.
    expect(cache->optimization_engine() == nullptr, 20);
    // Reads and writes work on the inherited handle.
    expect(read_document(*cache, key), 21);
    expect(write_document(*cache, CacheKey("fork-contract-child"),
                          "from the child"),
           22);
    cache->stop();
    expect(!cache->is_running(), 23);
    // Finished: no restart, no operations, and stop() again (as the
    // destructor does) is a no-op.
    const auto restarted = cache->start();
    expect(!restarted.has_value() && restarted.error() == CacheError::Closed,
           24);
    const auto read_after_stop = cache->read_sync(key);
    expect(!read_after_stop.has_value() &&
               read_after_stop.error() == CacheError::NotInitialized,
           25);
    cache->stop();
    cache.reset();
    ::_exit(rc);
  }

  int status = 0;
  const auto deadline = Clock::now() + kChildDeadline;
  pid_t done = 0;
  while ((done = ::waitpid(pid, &status, WNOHANG)) == 0 &&
         Clock::now() < deadline) {
    std::this_thread::sleep_for(Ms{1});
  }
  if (done != pid) {
    ::kill(pid, SIGKILL);
    ::waitpid(pid, &status, 0);
  }
  REQUIRE(done == pid);
  REQUIRE(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 0);

  // The parent is unaffected: still running, engine still there, and it
  // sees the child's write.
  CHECK(cache->is_running());
  CHECK(cache->optimization_engine() != nullptr);
  CHECK(read_document(*cache, CacheKey("fork-contract-child")));
  cache->stop();
}

namespace {

// Waits for `pid` up to the child deadline; kills it when it is late.
// Returns its exit code, or -1 when it was killed or died of a signal.
int exit_code_within_deadline(pid_t pid) {
  int status = 0;
  const auto deadline = Clock::now() + kChildDeadline;
  pid_t done = 0;
  while ((done = ::waitpid(pid, &status, WNOHANG)) == 0 &&
         Clock::now() < deadline) {
    std::this_thread::sleep_for(Ms{1});
  }
  if (done != pid) {
    ::kill(pid, SIGKILL);
    ::waitpid(pid, &status, 0);
    return -1;
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

}  // namespace

TEST_CASE("Fork gate: fork() waits for a background pass in flight",
          "[fork][forkgate]") {
  install_fork_handlers();
  const auto kPassLength = Ms{300};
  std::atomic<bool> keep_running{true};
  std::atomic<bool> in_pass{false};
  std::atomic<bool> pass_over{false};
  std::thread worker([&] {
    ForkGatedPass pass(keep_running);
    REQUIRE(static_cast<bool>(pass));
    in_pass.store(true, std::memory_order_release);
    std::this_thread::sleep_for(kPassLength);
    pass_over.store(true, std::memory_order_release);
  });
  while (!in_pass.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(Ms{1});
  }

  const uint32_t epoch_before = fork_epoch();
  const pid_t pid = ::fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    // The pass had ended before the child's memory was copied, and the
    // child knows it is a new process.
    ::_exit(pass_over.load() && fork_epoch() == epoch_before + 1 ? 0 : 1);
  }
  // fork() returned in the parent only after the pass was over.
  CHECK(pass_over.load(std::memory_order_acquire));
  CHECK(fork_epoch() == epoch_before);
  worker.join();
  CHECK(exit_code_within_deadline(pid) == 0);

  // The gate is open again: a pass enters at once.
  ForkGatedPass again(keep_running);
  CHECK(static_cast<bool>(again));
}

TEST_CASE("Fork gate: a thread inside a pass can itself fork",
          "[fork][forkgate]") {
  // An optimization plugin that spawns a helper process forks from inside
  // a work item, which is a pass: prepare must not wait for its own caller.
  install_fork_handlers();
  std::atomic<bool> keep_running{true};
  ForkGatedPass outer(keep_running);
  REQUIRE(static_cast<bool>(outer));
  ForkGatedPass nested(keep_running);  // Nesting never waits
  REQUIRE(static_cast<bool>(nested));

  const pid_t pid = ::fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    ::alarm(30);
    // Still inside the inherited passes; a further fork works here too.
    const pid_t grandchild = ::fork();
    if (grandchild == 0) {
      ::_exit(0);
    }
    int status = 0;
    const bool ok = grandchild > 0 &&
                    ::waitpid(grandchild, &status, 0) == grandchild &&
                    WIFEXITED(status) && WEXITSTATUS(status) == 0;
    ::_exit(ok ? 0 : 1);
  }
  CHECK(exit_code_within_deadline(pid) == 0);
}

TEST_CASE(
    "Fork gate: a pass that waits for a fork gives up when its owner "
    "stops",
    "[fork][forkgate]") {
  // The fork below cannot finish until `blocker` ends its pass, and
  // `waiter` cannot enter its pass until the fork finishes.  An owner that
  // stops `waiter` meanwhile must get it back without waiting for either:
  // a stop() that joins a background thread is never held up by a fork.
  install_fork_handlers();
  std::atomic<bool> blocker_running{true};
  std::atomic<bool> release_blocker{false};
  std::atomic<bool> blocker_in_pass{false};
  std::thread blocker([&] {
    ForkGatedPass pass(blocker_running);
    blocker_in_pass.store(true, std::memory_order_release);
    while (!release_blocker.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(Ms{1});
    }
  });
  while (!blocker_in_pass.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(Ms{1});
  }

  std::atomic<pid_t> child{0};
  std::thread forker([&] {
    const pid_t pid = ::fork();  // Waits in prepare for `blocker`
    if (pid == 0) {
      ::_exit(0);
    }
    child.store(pid, std::memory_order_release);
  });
  // Wait until the forker is in its prepare handler (it stays there until
  // the blocker is released), so the waiter below is certain to find the
  // fork pending.
  const auto pending_deadline = Clock::now() + kChildDeadline;
  while (forks_pending_for_test() == 0 && Clock::now() < pending_deadline) {
    std::this_thread::sleep_for(Ms{1});
  }
  const bool fork_was_pending = forks_pending_for_test() == 1;

  std::atomic<bool> waiter_running{true};
  std::atomic<int> waiter_entered{-1};
  std::thread waiter([&] {
    ForkGatedPass pass(waiter_running);
    waiter_entered.store(pass ? 1 : 0, std::memory_order_release);
  });
  std::this_thread::sleep_for(Ms{20});
  // Still waiting: the fork is pending and nobody asked it to stop.
  const bool waited = waiter_entered.load(std::memory_order_acquire) < 0;
  waiter_running.store(false, std::memory_order_release);  // The owner stops
  const auto stop_deadline = Clock::now() + kChildDeadline;
  while (waiter_entered.load(std::memory_order_acquire) < 0 &&
         Clock::now() < stop_deadline) {
    std::this_thread::sleep_for(Ms{1});
  }
  const int entered = waiter_entered.load(std::memory_order_acquire);
  // Unblock everything before asserting, so a failure cannot hang the run.
  release_blocker.store(true, std::memory_order_release);
  blocker.join();
  forker.join();
  waiter.join();

  CHECK(fork_was_pending);
  CHECK(waited);
  CHECK(entered == 0);  // Returned while the fork was still pending
  REQUIRE(child.load() > 0);
  CHECK(exit_code_within_deadline(child.load()) == 0);
}

#endif  // !_WIN32
