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
// Sanitizers.  The storm children start no thread, so the storms and the
// child-contract case run under ThreadSanitizer too (it only refuses a
// forked child that creates threads); iteration counts and deadlines are
// scaled for the slower build.  Three cases do NOT run under
// ThreadSanitizer and are compiled out there, each with its reason at the
// case: the two that run a scenario in a forked subprocess which starts
// threads, and the one whose child exits after a fork made while a test
// thread of the parent was alive (the sanitizer reports that thread as
// leaked in the child and turns the child's exit code into its own).

#ifndef _WIN32

#include <pthread.h>
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
#include "cyclone/plugin/optimization.hpp"
#include "optimization/optimization_engine.hpp"
#include "support/temp_cache.hpp"

#if defined(__SANITIZE_THREAD__)
#define CYCLONE_FORK_STORM_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define CYCLONE_FORK_STORM_TSAN 1
#endif
#endif

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

// Not under ThreadSanitizer: the parent has a worker thread at the fork,
// and the sanitizer's exit hook in the child reports that thread (which the
// child does not have and cannot join) as leaked and replaces the exit code
// this test reads.  Seen on macOS always and on Linux depending on timing.
#if !defined(CYCLONE_FORK_STORM_TSAN)
TEST_CASE("Fork gate: fork() waits for a background pass in flight",
          "[fork][forkgate]") {
  REQUIRE(install_fork_handlers());
  const auto kPassLength = Ms{300};
  std::atomic<bool> keep_running{true};
  std::atomic<int> entered{-1};  // Set by the worker: 1 entered, 0 refused
  std::atomic<bool> pass_over{false};
  std::thread worker([&] {
    ForkGatedPass pass(keep_running);
    entered.store(pass ? 1 : 0, std::memory_order_release);
    std::this_thread::sleep_for(kPassLength);
    pass_over.store(true, std::memory_order_release);
  });
  while (entered.load(std::memory_order_acquire) < 0) {
    std::this_thread::sleep_for(Ms{1});
  }

  const uint32_t epoch_before = fork_epoch();
  const pid_t pid = ::fork();
  if (pid == 0) {
    // The pass had ended before the child's memory was copied, and the
    // child knows it is a new process.
    ::_exit(pass_over.load() && fork_epoch() == epoch_before + 1 ? 0 : 1);
  }
  // fork() returned in the parent only after the pass was over.
  const bool over_when_fork_returned =
      pass_over.load(std::memory_order_acquire);
  worker.join();
  REQUIRE(pid > 0);
  CHECK(entered.load() == 1);
  CHECK(over_when_fork_returned);
  CHECK(fork_epoch() == epoch_before);
  CHECK(exit_code_within_deadline(pid) == 0);

  // The gate is open again: a pass enters at once.
  ForkGatedPass again(keep_running);
  CHECK(static_cast<bool>(again));
}
#endif

// ---------------------------------------------------------------------------
// Forks that meet each other.
//
// A scenario here can only fail by hanging, and a hung fork() cannot be
// recovered inside the process (its thread never comes back and the gate
// stays shut for everything else).  So each scenario runs in a forked
// SUBPROCESS: it watches its own progress and leaves with an exit code when
// nothing has moved for a while, and the test reads that code with a
// deadline.  The subprocess starts threads, which ThreadSanitizer refuses
// in a forked child ("starting new threads after multi-threaded fork"), so
// these cases are compiled out under it.
// ---------------------------------------------------------------------------
#if !defined(CYCLONE_FORK_STORM_TSAN)
namespace {

constexpr int kScenarioOk = 0;
constexpr int kScenarioOkSerialized = 40;  // Passed in the reduced form
constexpr int kScenarioStalled = 50;       // A fork or a pass hung
constexpr int kScenarioWrong = 51;         // Finished, but something is off

// Runs `scenario` in a forked child and returns its exit code, or -1 when
// it did not exit within `budget` (it is killed) or died of a signal.
int run_in_subprocess(const std::function<int()>& scenario, Ms budget) {
  const pid_t pid = ::fork();
  if (pid < 0) {
    return -1;
  }
  if (pid == 0) {
    int rc = 90;
    try {
      rc = scenario();
    } catch (...) {
      rc = 91;
    }
    ::_exit(rc);
  }
  int status = 0;
  const auto deadline = Clock::now() + budget;
  pid_t done = 0;
  while ((done = ::waitpid(pid, &status, WNOHANG)) == 0 &&
         Clock::now() < deadline) {
    std::this_thread::sleep_for(Ms{5});
  }
  if (done != pid) {
    ::kill(pid, SIGKILL);
    ::waitpid(pid, &status, 0);
    return -1;
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// Forks a child that exits at once and reaps it.
bool fork_and_reap() {
  const pid_t pid = ::fork();
  if (pid == 0) {
    ::_exit(0);
  }
  if (pid < 0) {
    return false;
  }
  int status = 0;
  return ::waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
         WEXITSTATUS(status) == 0;
}

// Leaves the process with kScenarioStalled when `progress` has not moved
// for `patience`.  Runs on its own thread until `done`.
void watch_progress(const std::atomic<long>& progress,
                    const std::atomic<bool>& done, Ms patience) {
  long last = -1;
  auto last_change = Clock::now();
  while (!done.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(Ms{50});
    const long now = progress.load(std::memory_order_acquire);
    if (now != last) {
      last = now;
      last_change = Clock::now();
    } else if (Clock::now() - last_change > patience) {
      ::_exit(kScenarioStalled);
    }
  }
}

// Does this C library run the fork handlers of one fork() at a time?
// Measured, not assumed: a prepare handler that waits a moment for a second
// fork's handler to show up next to it.  If the library holds a lock of its
// own around the handlers, the second one cannot.  Registers a handler for
// good, so it is only ever called in a scenario subprocess.
std::atomic<bool> g_probe_on{false};
std::atomic<int> g_probe_inside{0};
std::atomic<bool> g_probe_met{false};

void probe_prepare() {
  if (!g_probe_on.load(std::memory_order_acquire)) {
    return;
  }
  if (g_probe_inside.fetch_add(1, std::memory_order_acq_rel) + 1 >= 2) {
    g_probe_met.store(true, std::memory_order_release);
  }
  const auto until = Clock::now() + Ms{300};
  while (!g_probe_met.load(std::memory_order_acquire) && Clock::now() < until) {
    std::this_thread::sleep_for(Ms{1});
  }
  g_probe_inside.fetch_sub(1, std::memory_order_acq_rel);
}

bool libc_runs_fork_handlers_one_fork_at_a_time() {
  if (::pthread_atfork(&probe_prepare, nullptr, nullptr) != 0) {
    return true;  // Cannot tell: take the careful branch.
  }
  g_probe_on.store(true, std::memory_order_release);
  std::thread first([] { (void)fork_and_reap(); });
  std::thread second([] { (void)fork_and_reap(); });
  first.join();
  second.join();
  g_probe_on.store(false, std::memory_order_release);
  return !g_probe_met.load(std::memory_order_acquire);
}

// Two threads that fork from inside a pass, a thread that forks outside any
// pass, and a thread that runs ordinary passes, all at once.
int scenario_forks_from_inside_passes(int iterations) {
  if (!install_fork_handlers()) {
    return kScenarioWrong;
  }
  const bool serialized = libc_runs_fork_handlers_one_fork_at_a_time();

  std::atomic<bool> keep_running{true};
  std::atomic<long> progress{0};
  std::atomic<long> plain_passes{0};
  std::atomic<long> failed{0};
  std::atomic<bool> done{false};
  std::thread watchdog(
      [&] { watch_progress(progress, done, Ms{5000} * kSanitizerSlowdown); });

  const auto in_pass_forker = [&] {
    for (int i = 0; i < iterations; ++i) {
      ForkGatedPass pass(keep_running);
      if (!pass || !fork_and_reap()) {
        failed.fetch_add(1);
      }
      progress.fetch_add(1);
    }
  };
  const auto outside_forker = [&] {
    for (int i = 0; i < iterations; ++i) {
      if (!fork_and_reap()) {
        failed.fetch_add(1);
      }
      progress.fetch_add(1);
    }
  };
  std::atomic<bool> forkers_done{false};
  const auto plain_passer = [&] {
    while (!forkers_done.load(std::memory_order_acquire)) {
      {
        ForkGatedPass pass(keep_running);
        if (!pass) {
          failed.fetch_add(1);
        }
        std::this_thread::sleep_for(std::chrono::microseconds(30));
      }
      plain_passes.fetch_add(1);
      std::this_thread::sleep_for(std::chrono::microseconds(30));
    }
  };

  // Phase 1: the forks from inside passes.  Where two forks can be inside
  // the C library at once, the outside forker runs right along with them.
  // Where they cannot, it must not: an outside fork that waits for a pass
  // holds the C library's fork lock, and the pass it waits for blocks on
  // that lock the moment it forks.  No handler can undo that (see
  // fork_gate.hpp), so there the outside forker gets a phase of its own.
  {
    std::thread passer(plain_passer);
    std::thread a(in_pass_forker);
    std::thread b(in_pass_forker);
    std::thread c;
    if (!serialized) {
      c = std::thread(outside_forker);
    }
    a.join();
    b.join();
    if (c.joinable()) {
      c.join();
    }
    forkers_done.store(true, std::memory_order_release);
    passer.join();
  }
  const long passes_phase1 = plain_passes.load();

  // Phase 2 (serialized libraries only): the outside forker against
  // ordinary passes.
  if (serialized) {
    forkers_done.store(false, std::memory_order_release);
    std::thread passer(plain_passer);
    std::thread c(outside_forker);
    c.join();
    forkers_done.store(true, std::memory_order_release);
    passer.join();
  }

  done.store(true, std::memory_order_release);
  watchdog.join();
  // Nothing is left pending, every fork worked, and ordinary passes got
  // through while the forks were going on.
  if (failed.load() != 0 || forks_pending_for_test() != 0 ||
      passes_phase1 == 0) {
    return kScenarioWrong;
  }
  ForkGatedPass after(keep_running);
  if (!after) {
    return kScenarioWrong;
  }
  return serialized ? kScenarioOkSerialized : kScenarioOk;
}

// A plugin whose transform() starts a helper process with fork().
class ForkingPlugin : public OptimizationPlugin {
 public:
  [[nodiscard]] PluginInfo info() const override {
    return {"forking-plugin", "1.0.0", 4242};
  }

  OptimizationPlan plan_optimization(const CacheKey& /*key*/,
                                     std::span<const std::byte> /*header*/,
                                     uint64_t content_length,
                                     AlternateId written_alternate,
                                     uint32_t /*hit_count*/) override {
    OptimizationPlan plan;
    if (written_alternate == AlternateId::Original) {
      plan.add(AlternateId::Gzip, 5, true, content_length * 2);
    }
    return plan;
  }

  std::expected<TransformResult, CacheError> transform(
      AlternateId target_alternate, const OptimizationContext& ctx) override {
    if (!fork_and_reap()) {
      failed_forks.fetch_add(1);
    }
    TransformResult result;
    result.alternate_id = target_alternate;
    result.header = std::vector<std::byte>(ctx.source_header().begin(),
                                           ctx.source_header().end());
    result.content = std::vector<std::byte>(ctx.source_content().begin(),
                                            ctx.source_content().end());
    transforms.fetch_add(1);
    if (progress != nullptr) {
      progress->fetch_add(1);
    }
    return result;
  }

  std::atomic<long> transforms{0};
  std::atomic<long> failed_forks{0};
  std::atomic<long>* progress = nullptr;
};

// The optimization engine with two workers and a plugin that forks in
// transform(), while an application thread forks as well and the hit-count
// flush and directory sync run at 1 ms.
int scenario_plugin_forks(int transforms_wanted) {
  TempCacheDir dir("fork_plugin");
  CacheConfig cache_config;
  cache_config.set_ram_cache_size(0);
  cache_config.enable_hit_tracking = true;
  cache_config.hit_flush_interval = Ms{1};
  cache_config.set_multi_process(0, 1);
  cache_config.set_directory_sync_interval(Ms{1});
  cache_config.optimization_config.enabled = true;
  cache_config.optimization_config.min_threads = 2;
  cache_config.optimization_config.max_threads = 2;
  cache_config.optimization_config.min_hits_before_optimize = 0;
  // Keep load shedding out of it: a paused pool would look like a hang.
  cache_config.optimization_config.load_high_watermark = 1e12;

  auto created = Cache::create(cache_config);
  if (!created.has_value()) {
    return kScenarioWrong;
  }
  std::unique_ptr<Cache> cache = std::move(*created);
  if (!cache->add_volume(dir.path(), size_t{64} << 20).has_value() ||
      !cache->start().has_value() || cache->optimization_engine() == nullptr) {
    return kScenarioWrong;
  }
  auto plugin = std::make_shared<ForkingPlugin>();
  std::atomic<long> progress{0};
  plugin->progress = &progress;
  cache->optimization_engine()->register_plugin(plugin);

  std::atomic<bool> done{false};
  std::thread watchdog(
      [&] { watch_progress(progress, done, Ms{5000} * kSanitizerSlowdown); });

  std::atomic<bool> stop_forking{false};
  std::atomic<long> outside_forks{0};
  std::atomic<long> failed{0};
  std::thread outside_forker([&] {
    while (!stop_forking.load(std::memory_order_acquire)) {
      if (!fork_and_reap()) {
        failed.fetch_add(1);
      }
      outside_forks.fetch_add(1);
    }
  });

  // Feed the engine until the plugin has forked often enough.
  const std::string body(2048, 'x');
  const std::vector<std::byte> header(16, std::byte{1});
  for (long n = 0; plugin->transforms.load() < transforms_wanted; ++n) {
    const CacheKey key("fork-plugin-" + std::to_string(n % 512));
    if (write_document(*cache, key, body)) {
      cache->optimization_engine()->on_write_complete(
          key, header, body.size(), AlternateId::Original, 10);
    }
    if (cache->optimization_engine()->queue_depth() > 64) {
      std::this_thread::sleep_for(Ms{1});
    }
  }
  stop_forking.store(true, std::memory_order_release);
  outside_forker.join();

  const CacheStats stats = cache->stats();
  cache->stop();
  done.store(true, std::memory_order_release);
  watchdog.join();

  if (failed.load() != 0 || plugin->failed_forks.load() != 0 ||
      outside_forks.load() == 0 || stats.directory_syncs == 0 ||
      forks_pending_for_test() != 0) {
    return kScenarioWrong;
  }
  return kScenarioOk;
}

}  // namespace

TEST_CASE(
    "Fork gate: forks from inside passes never hold up each other, "
    "other forks or other passes",
    "[fork][forkgate]") {
  // No code of the library forks from inside a pass, and a pass runs no
  // application code, so this only happens through the gate's own interface
  // (here) or through a future mistake.  It must still never hang the
  // process: such a fork bypasses the gate.  1500 rounds per thread; the
  // earlier form of the gate, in which such a fork waited for the other
  // passes, stopped within a few hundred.
  const int code =
      run_in_subprocess([] { return scenario_forks_from_inside_passes(1500); },
                        Ms{120'000} * kSanitizerSlowdown);
  INFO("scenario exit code "
       << code << " (0 ok; 40 ok, the C library runs fork handlers one fork "
       << "at a time, so the outside forker ran in a phase of its own; 50 "
       << "hung; 51 finished wrong; -1 killed at the deadline)");
  CHECK((code == kScenarioOk || code == kScenarioOkSerialized));
#if defined(__linux__) && defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 36)
  // glibc from 2.36 on lets two forks run their handlers side by side, so
  // the full scenario must have run there (CI's Linux is one of those).
  CHECK(code == kScenarioOk);
#endif
#endif
}

TEST_CASE(
    "Fork: a plugin that forks in transform() and an application "
    "thread that forks do not block each other",
    "[fork][forkgate][optimization]") {
  // transform() runs between two passes, not inside one, so a helper
  // process started there is an ordinary fork from outside a pass: it
  // waits for the passes in flight like any other, on every platform,
  // including those whose C library runs fork handlers one fork at a time.
  const int code = run_in_subprocess([] { return scenario_plugin_forks(300); },
                                     Ms{120'000} * kSanitizerSlowdown);
  INFO("scenario exit code " << code
                             << " (0 ok; 50 hung; 51 finished wrong; -1 "
                             << "killed at the deadline)");
  CHECK(code == kScenarioOk);
}
#endif  // !CYCLONE_FORK_STORM_TSAN

TEST_CASE(
    "Fork gate: a pass that waits for a fork gives up when its owner "
    "stops",
    "[fork][forkgate]") {
  // The fork below cannot finish until `blocker` ends its pass, and
  // `waiter` cannot enter its pass until the fork finishes.  An owner that
  // stops `waiter` meanwhile must get it back without waiting for either:
  // a stop() that joins a background thread is never held up by a fork.
  REQUIRE(install_fork_handlers());
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
