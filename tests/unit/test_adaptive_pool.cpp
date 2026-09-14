// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <latch>
#include <mutex>
#include <queue>
#include <thread>

#include "optimization/adaptive_pool.hpp"

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
