// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <latch>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>

#include "optimization/adaptive_pool.hpp"

#if defined(__GLIBCXX__) && defined(__linux__)
#include <pthread.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace cyclone;

namespace {

class MockTaskProvider : public TaskProvider {
 public:
  void add_task(TaskFunction task) {
    std::lock_guard lock(_mutex);
    _tasks.push(std::move(task));
    _cv.notify_one();
  }

  void signal_stop() {
    std::lock_guard lock(_mutex);
    _stopped = true;
    _cv.notify_all();
  }

  void reset() {
    std::lock_guard lock(_mutex);
    _stopped = false;
  }

  TaskFunction get_task() override {
    std::unique_lock lock(_mutex);
    _cv.wait(lock, [this] { return !_tasks.empty() || _stopped; });

    if (_stopped && _tasks.empty()) {
      return nullptr;
    }

    auto task = std::move(_tasks.front());
    _tasks.pop();
    return task;
  }

  size_t queue_depth() const override {
    std::lock_guard lock(_mutex);
    return _tasks.size();
  }

 private:
  mutable std::mutex _mutex;
  std::condition_variable _cv;
  std::queue<TaskFunction> _tasks;
  bool _stopped = false;
};

OptimizationConfig default_config() {
  OptimizationConfig config;
  config.min_threads = 1;
  config.max_threads = 4;
  config.scale_up_threshold = 5;
  config.scale_down_threshold = 1;
  return config;
}

}  // namespace

TEST_CASE("AdaptiveThreadPool basic lifecycle", "[pool][optimization]") {
  OptimizationConfig config = default_config();
  MockTaskProvider provider;

  AdaptiveThreadPool pool(config, provider);

  SECTION("starts and stops cleanly") {
    pool.start();
    REQUIRE(pool.is_running());
    REQUIRE(pool.thread_count() >= config.min_threads);

    provider.signal_stop();
    pool.stop();
    REQUIRE_FALSE(pool.is_running());
    REQUIRE(pool.thread_count() == 0);
  }

  SECTION("double start is safe") {
    pool.start();
    pool.start();  // Should be no-op
    REQUIRE(pool.is_running());

    provider.signal_stop();
    pool.stop();
  }

  SECTION("double stop is safe") {
    pool.start();
    provider.signal_stop();
    pool.stop();
    pool.stop();  // Should be no-op
    REQUIRE_FALSE(pool.is_running());
  }
}

TEST_CASE("AdaptiveThreadPool task execution", "[pool][optimization]") {
  OptimizationConfig config = default_config();
  MockTaskProvider provider;
  AdaptiveThreadPool pool(config, provider);

  pool.start();

  SECTION("executes single task") {
    std::atomic<bool> executed{false};
    std::latch done(1);

    provider.add_task([&] {
      executed = true;
      done.count_down();
    });

    done.wait();
    REQUIRE(executed.load());
  }

  SECTION("executes multiple tasks") {
    constexpr int num_tasks = 100;
    std::atomic<int> counter{0};
    std::latch done(num_tasks);

    for (int i = 0; i < num_tasks; ++i) {
      provider.add_task([&] {
        counter.fetch_add(1, std::memory_order_relaxed);
        done.count_down();
      });
    }

    done.wait();
    REQUIRE(counter.load() == num_tasks);
  }

  provider.signal_stop();
  pool.stop();
}

TEST_CASE("AdaptiveThreadPool pause and resume", "[pool][optimization]") {
  OptimizationConfig config = default_config();
  MockTaskProvider provider;
  AdaptiveThreadPool pool(config, provider);

  pool.start();

  SECTION("pause blocks task execution") {
    std::atomic<bool> task_started{false};
    std::atomic<bool> task_finished{false};
    std::latch started(1);
    std::latch can_finish(1);

    // Add a task that signals when it starts
    provider.add_task([&] {
      task_started = true;
      started.count_down();
      can_finish.wait();  // Wait to be released
      task_finished = true;
    });

    // Wait for task to start
    started.wait();
    REQUIRE(task_started.load());

    // Pause the pool
    pool.pause();
    REQUIRE(pool.is_paused());

    // Release the running task
    can_finish.count_down();

    // Give time for task to finish
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    REQUIRE(task_finished.load());

    // Add another task while paused
    std::atomic<bool> second_task_executed{false};
    provider.add_task([&] { second_task_executed = true; });

    // Give some time - task should NOT execute while paused
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    REQUIRE_FALSE(second_task_executed.load());

    // Resume
    pool.resume();
    REQUIRE_FALSE(pool.is_paused());

    // Now task should execute
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    REQUIRE(second_task_executed.load());
  }

  provider.signal_stop();
  pool.stop();
}

TEST_CASE("AdaptiveThreadPool scaling", "[pool][optimization]") {
  OptimizationConfig config;
  config.min_threads = 1;
  config.max_threads = 4;
  config.scale_up_threshold = 2;
  config.scale_down_threshold = 1;

  MockTaskProvider provider;
  AdaptiveThreadPool pool(config, provider);

  pool.start();

  SECTION("scale_to forces specific thread count") {
    REQUIRE(pool.thread_count() == 1);  // Started with min

    pool.scale_to(3);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    REQUIRE(pool.thread_count() == 3);

    pool.scale_to(1);
    // Scaling down happens naturally as threads exit
  }

  SECTION("check_scaling scales up with high queue depth") {
    // Add many tasks to increase queue depth
    constexpr int num_tasks = 20;
    std::atomic<int> barrier_counter{0};
    std::atomic<int> completed_counter{0};
    std::latch release_latch(1);

    // Add blocking tasks to fill up
    for (int i = 0; i < num_tasks; ++i) {
      provider.add_task([&] {
        barrier_counter.fetch_add(1);
        release_latch.wait();
        completed_counter.fetch_add(1);
      });
    }

    // Wait for some tasks to start and queue to build up
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    size_t initial_threads = pool.thread_count();
    pool.check_scaling();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Should have scaled up due to queue depth
    REQUIRE(pool.thread_count() >= initial_threads);

    // Release all waiting tasks
    release_latch.count_down();

    // Wait for all tasks to complete before scope ends (to avoid
    // use-after-scope)
    while (completed_counter.load(std::memory_order_acquire) <
           barrier_counter.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  provider.signal_stop();
  pool.stop();
}

// Regression: after a scale-down the self-terminated workers stayed in the
// worker list, and both spawn paths compared the list size (the historical
// maximum) against the new target, so no later scale-up below that watermark
// ever spawned again: the target rose but the live thread count stayed low.
// The spawn decision must be based on the live worker count.
TEST_CASE("AdaptiveThreadPool scales back up after scaling down",
          "[pool][optimization][regression]") {
  OptimizationConfig config;
  config.min_threads = 1;
  config.max_threads = 4;

  MockTaskProvider provider;
  AdaptiveThreadPool pool(config, provider);
  pool.start();

  auto wait_for_threads = [&](size_t expected) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (pool.thread_count() != expected &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pool.thread_count() == expected;
  };

  REQUIRE(wait_for_threads(1));

  // Scale up to max and let the workers register.
  pool.scale_to(4);
  REQUIRE(wait_for_threads(4));

  // Scale down to min.  Parked workers only observe the lowered target when
  // they cycle through the loop, so feed tasks until the surplus exits.
  pool.scale_to(1);
  for (int i = 0; i < 32 && pool.thread_count() > 1; ++i) {
    provider.add_task([] {});
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  REQUIRE(wait_for_threads(1));

  // Scale back up below the historical maximum: the pool must spawn workers
  // again rather than comparing against the list of long-dead threads.
  pool.scale_to(3);
  const bool recovered = wait_for_threads(3);

  provider.signal_stop();
  pool.stop();

  REQUIRE(recovered);
  REQUIRE(pool.stats().current_threads == 0);
}

// Regression: a task that threw an exception propagated out of worker_loop
// and terminated the process.  The pool must guard the call, record the
// failure, and keep the worker alive for later tasks.
TEST_CASE("AdaptiveThreadPool survives a throwing task",
          "[pool][optimization][regression]") {
  OptimizationConfig config = default_config();
  config.min_threads = 1;
  config.max_threads = 1;
  MockTaskProvider provider;
  AdaptiveThreadPool pool(config, provider);
  pool.start();

  std::latch next_ran(1);
  provider.add_task([] { throw std::runtime_error("task failure"); });
  provider.add_task([&] { next_ran.count_down(); });

  // The worker survived the throw and ran the next task.
  next_ran.wait();
  REQUIRE(pool.thread_count() == 1);

  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (pool.stats().tasks_failed != 1 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  auto stats = pool.stats();
  REQUIRE(stats.tasks_failed == 1);
  REQUIRE(stats.tasks_completed == 1);

  provider.signal_stop();
  pool.stop();
}

TEST_CASE("AdaptiveThreadPool statistics", "[pool][optimization]") {
  OptimizationConfig config = default_config();
  MockTaskProvider provider;
  AdaptiveThreadPool pool(config, provider);

  pool.start();

  constexpr int num_tasks = 10;
  std::latch done(num_tasks);

  for (int i = 0; i < num_tasks; ++i) {
    provider.add_task([&] { done.count_down(); });
  }

  done.wait();
  std::this_thread::sleep_for(
      std::chrono::milliseconds(50));  // Let stats update

  auto stats = pool.stats();
  REQUIRE(stats.tasks_started == num_tasks);
  REQUIRE(stats.tasks_completed == num_tasks);
  REQUIRE(stats.current_threads >= 1);

  provider.signal_stop();
  pool.stop();
}

TEST_CASE("AdaptiveThreadPool active count tracking", "[pool][optimization]") {
  OptimizationConfig config = default_config();
  config.min_threads = 2;
  MockTaskProvider provider;
  AdaptiveThreadPool pool(config, provider);

  pool.start();

  std::latch task_started(2);
  std::latch release(1);

  // Add two blocking tasks
  for (int i = 0; i < 2; ++i) {
    provider.add_task([&] {
      task_started.count_down();
      release.wait();
    });
  }

  task_started.wait();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Both threads should be active
  REQUIRE(pool.active_count() == 2);

  release.count_down();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Should be back to 0 active
  REQUIRE(pool.active_count() == 0);

  provider.signal_stop();
  pool.stop();
}

TEST_CASE("AdaptiveThreadPool respects min/max bounds",
          "[pool][optimization]") {
  OptimizationConfig config;
  config.min_threads = 2;
  config.max_threads = 3;

  MockTaskProvider provider;
  AdaptiveThreadPool pool(config, provider);

  pool.start();

  SECTION("cannot scale below min") {
    pool.scale_to(0);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    REQUIRE(pool.thread_count() >= config.min_threads);
  }

  SECTION("cannot scale above max") {
    pool.scale_to(100);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    REQUIRE(pool.thread_count() <= config.max_threads);
  }

  provider.signal_stop();
  pool.stop();
}

TEST_CASE("AdaptiveThreadPool stress test",
          "[pool][optimization][concurrent][stress]") {
  OptimizationConfig config;
  config.min_threads = 2;
  config.max_threads = 8;

  MockTaskProvider provider;
  AdaptiveThreadPool pool(config, provider);

  pool.start();

  constexpr int num_tasks = 1000;
  std::atomic<int> completed{0};
  std::latch done(num_tasks);

  // Submit many short tasks
  for (int i = 0; i < num_tasks; ++i) {
    provider.add_task([&] {
      // Simulate some work
      volatile int x = 0;
      for (int j = 0; j < 1000; ++j) {
        x += j;
      }
      (void)x;
      completed.fetch_add(1, std::memory_order_relaxed);
      done.count_down();
    });
  }

  // Trigger scaling checks periodically
  for (int i = 0; i < 10; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    pool.check_scaling();
  }

  done.wait();

  REQUIRE(completed.load() == num_tasks);

  auto stats = pool.stats();
  REQUIRE(stats.tasks_completed == num_tasks);

  provider.signal_stop();
  pool.stop();
}

TEST_CASE("AdaptiveThreadPool stop is safe after threads exit early",
          "[pool][optimization]") {
  // Simulates the atexit() crash scenario: worker threads exit before
  // stop() is called. Previously, stop() could SIGSEGV in pthread_join
  // on already-dead thread descriptors during process shutdown.
  //
  // We reproduce this by signaling the provider to stop (so threads
  // exit naturally via nullptr return), waiting for threads to die,
  // then calling pool.stop() which must not crash on the dead handles.

  OptimizationConfig config = default_config();
  config.min_threads = 2;
  config.max_threads = 4;

  MockTaskProvider provider;
  AdaptiveThreadPool pool(config, provider);

  pool.start();

  // Let threads start
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  REQUIRE(pool.thread_count() >= 2);

  // Signal provider to return nullptr — threads will exit worker_loop
  provider.signal_stop();

  // Wait for threads to actually die
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // Now call stop() — this must handle already-exited threads gracefully
  pool.stop();

  REQUIRE_FALSE(pool.is_running());
  REQUIRE(pool.thread_count() == 0);
}

// Regression for the staged-teardown fix (leak_thread_on_shutdown UAF
// class).
//
// The old AdaptiveThreadPool::stop() waited up to 5 s for _live_count to reach
// zero and, on timeout, abandoned the still-running workers via
// leak_thread_on_shutdown().  A pool worker executes cache-touching
// optimization tasks; a leaked worker abandoned mid-task then dereferences
// cache state that teardown frees.  The fix joins unconditionally: stop() must
// not return until every worker that is mid-task has returned.
//
// This test parks the single worker inside a task that blocks past the former
// 5 s deadline, then calls stop().  A correct unconditional join stays blocked
// until the task is released; the old bounded-wait+leak path would have
// returned (and leaked the worker) at ~5 s.
TEST_CASE("AdaptiveThreadPool::stop() joins an in-flight task (no leak)",
          "[pool][optimization][teardown][regression]") {
  OptimizationConfig config = default_config();
  config.min_threads = 1;
  config.max_threads = 1;
  MockTaskProvider provider;
  AdaptiveThreadPool pool(config, provider);
  pool.start();

  std::atomic<bool> task_started{false};
  std::atomic<bool> release_task{false};
  provider.add_task([&] {
    task_started.store(true, std::memory_order_release);
    // Finite safety cap so a broken test can never hang the suite.
    auto safety = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!release_task.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < safety) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });

  // Wait until the worker is actually executing the blocking task.  Generous
  // deadline so contention on a loaded CI runner cannot fail this spuriously.
  auto started_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (!task_started.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < started_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  REQUIRE(task_started.load(std::memory_order_acquire));

  // Mirror production teardown ordering (OptimizationEngine::stop() stops the
  // WorkQueue before the pool): once the blocking task returns, the worker's
  // next get_task() must return nullptr so it exits.
  provider.signal_stop();

  std::atomic<bool> stop_returned{false};
  auto stop_begin = std::chrono::steady_clock::now();
  std::thread stopper([&] {
    pool.stop();
    stop_returned.store(true, std::memory_order_release);
  });

  // Hold past the former 5 s bounded-wait deadline without releasing the task.
  std::this_thread::sleep_for(std::chrono::milliseconds(5500));

  // Capture the discriminating observation, then release + join BEFORE
  // asserting: asserting while `stopper` is still joinable would std::terminate
  // (via ~thread) on failure instead of a clean Catch2 report (the
  // review).
  const bool returned_while_blocked =
      stop_returned.load(std::memory_order_acquire);
  release_task.store(true, std::memory_order_release);
  stopper.join();
  auto stop_elapsed = std::chrono::steady_clock::now() - stop_begin;

  // A correct unconditional join stays blocked until the task is released.
  REQUIRE_FALSE(returned_while_blocked);
  REQUIRE(stop_returned.load(std::memory_order_acquire));
  REQUIRE(stop_elapsed >= std::chrono::milliseconds(5500));
  REQUIRE_FALSE(pool.is_running());
  REQUIRE(pool.thread_count() == 0);
}

// Regression: workers that exited while join_workers() was joining the
// previous generation registered their exit after the bookkeeping had been
// cleared.  A later reap matched those stale entries against workers of the
// next start() (thread ids are reused by the platform) and joined a live
// worker while holding the workers mutex, which that worker needed in order
// to register its own exit: the pool deadlocked.  Repeated start/scale/stop
// cycles must always complete.
TEST_CASE("AdaptiveThreadPool restart cycles with scaling never deadlock",
          "[pool][optimization][regression]") {
  // Provider that hands out a no-op task every 20 ms, so workers cycle
  // through the loop and observe target changes, until stopped.
  struct TickingProvider : TaskProvider {
    std::mutex mutex;
    std::condition_variable cv;
    bool stopped = false;
    TaskFunction get_task() override {
      std::unique_lock lock(mutex);
      cv.wait_for(lock, std::chrono::milliseconds(20), [&] { return stopped; });
      if (stopped) {
        return nullptr;
      }
      return [] {};
    }
    size_t queue_depth() const override { return 0; }
  };

  OptimizationConfig config;
  config.min_threads = 2;
  config.max_threads = 8;

  TickingProvider provider;
  AdaptiveThreadPool pool(config, provider);

  std::atomic<int> rounds_done{0};
  constexpr int kRounds = 30;
  std::thread driver([&] {
    for (int round = 0; round < kRounds; ++round) {
      {
        std::lock_guard lock(provider.mutex);
        provider.stopped = false;
      }
      pool.start();
      pool.scale_to(4);
      pool.scale_to(2);
      std::this_thread::sleep_for(std::chrono::milliseconds(60));
      pool.scale_to(6);
      {
        std::lock_guard lock(provider.mutex);
        provider.stopped = true;
      }
      provider.cv.notify_all();
      pool.stop();
      rounds_done.fetch_add(1, std::memory_order_release);
    }
  });

  // Bounded wait: a deadlocked pool can never be unwound, so report and exit
  // instead of hanging the suite.
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  while (rounds_done.load(std::memory_order_acquire) < kRounds) {
    if (std::chrono::steady_clock::now() > deadline) {
      std::fprintf(stderr, "pool deadlocked after %d of %d rounds\n",
                   rounds_done.load(), kRounds);
      std::fflush(stderr);
      std::_Exit(1);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  driver.join();
  REQUIRE(rounds_done.load() == kRounds);
  REQUIRE(pool.thread_count() == 0);
}

#if defined(__GLIBCXX__) && defined(__linux__)
// Regression: thread cancellation on glibc unwinds the thread with a forced
// unwind exception, which must propagate.  The worker loop's catch-all
// swallowed it, and the runtime aborts the process when that happens.  Run in
// a child process so the outcome is observable as an exit status.
TEST_CASE("AdaptiveThreadPool lets a thread cancellation unwind a worker",
          "[pool][optimization][regression]") {
  const pid_t child = fork();
  REQUIRE(child >= 0);
  if (child == 0) {
    OptimizationConfig config;
    config.min_threads = 1;
    config.max_threads = 1;
    MockTaskProvider provider;
    AdaptiveThreadPool pool(config, provider);
    pool.start();
    std::atomic<bool> reached{false};
    provider.add_task([&] {
      reached.store(true, std::memory_order_release);
      pthread_exit(nullptr);
    });
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!reached.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    provider.signal_stop();
    pool.stop();
    std::_Exit(reached.load() ? 0 : 2);
  }
  int status = 0;
  REQUIRE(waitpid(child, &status, 0) == child);
  INFO("child status " << status);
  REQUIRE(WIFEXITED(status));
  REQUIRE(WEXITSTATUS(status) == 0);
}
#endif

// A worker counts as live from the moment it is spawned, not from when it is
// first scheduled: otherwise the spawn paths, which gate on the live count,
// could spawn again for a worker that simply had not run yet, and
// thread_count() lagged behind a scale_to() that had already returned.
TEST_CASE("AdaptiveThreadPool counts a worker as live when it is spawned",
          "[pool][optimization][regression]") {
  OptimizationConfig config;
  config.min_threads = 1;
  config.max_threads = 8;
  MockTaskProvider provider;
  AdaptiveThreadPool pool(config, provider);

  for (int round = 0; round < 20; ++round) {
    pool.start();
    REQUIRE(pool.thread_count() == 1);
    pool.scale_to(8);
    REQUIRE(pool.thread_count() == 8);
    provider.signal_stop();
    pool.stop();
    REQUIRE(pool.thread_count() == 0);
    provider.reset();
  }
}
