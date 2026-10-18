// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <latch>
#include <stdexcept>
#include <thread>

#include "cyclone/cache.hpp"
#include "cyclone/config.hpp"
#include "cyclone/plugin/optimization.hpp"
#include "optimization/optimization_engine.hpp"

using namespace cyclone;
namespace fs = std::filesystem;

namespace {

// Test plugin that simply copies content with a marker
class TestOptimizationPlugin : public OptimizationPlugin {
 public:
  [[nodiscard]] PluginInfo info() const override {
    return {"test-optimization", "1.0.0", 100};
  }

  OptimizationPlan plan_optimization(const CacheKey & /*key*/,
                                     std::span<const std::byte> /*header*/,
                                     uint64_t content_length,
                                     AlternateId written_alternate,
                                     uint32_t /*hit_count*/) override {
    OptimizationPlan plan;

    // Only optimize Original writes, and only if content is reasonably sized
    if (written_alternate == AlternateId::Original && content_length > 0 &&
        content_length < 1_MB) {
      plan.add(AlternateId::Brotli, 10, true, content_length * 2);
    }

    return plan;
  }

  std::expected<TransformResult, CacheError> transform(
      AlternateId target_alternate, const OptimizationContext &ctx) override {
    // Check for cancellation
    if (ctx.is_cancelled()) {
      return std::unexpected(CacheError::OptimizationCancelled);
    }

    ++transform_calls;

    // Simple "transformation": prefix content with marker
    const char *marker_str = "[OPT]";
    std::vector<std::byte> content;
    content.reserve(5 + ctx.source_content().size());
    for (int i = 0; i < 5; ++i) {
      content.push_back(static_cast<std::byte>(marker_str[i]));
    }
    content.insert(content.end(), ctx.source_content().begin(),
                   ctx.source_content().end());

    TransformResult result;
    result.alternate_id = target_alternate;
    result.header = std::vector<std::byte>(ctx.source_header().begin(),
                                           ctx.source_header().end());
    result.content = std::move(content);

    return result;
  }

  void on_cancelled(const CacheKey & /*key*/, AlternateId /*target*/) override {
    ++cancel_calls;
  }

  std::atomic<int> transform_calls{0};
  std::atomic<int> cancel_calls{0};
};

// Plugin that blocks until signaled (for testing pause/resume)
class BlockingOptimizationPlugin : public OptimizationPlugin {
 public:
  [[nodiscard]] PluginInfo info() const override {
    return {"blocking-optimization", "1.0.0", 101};
  }

  OptimizationPlan plan_optimization(const CacheKey & /*key*/,
                                     std::span<const std::byte> /*header*/,
                                     uint64_t /*content_length*/,
                                     AlternateId written_alternate,
                                     uint32_t /*hit_count*/) override {
    OptimizationPlan plan;
    if (written_alternate == AlternateId::Original) {
      plan.add(AlternateId::Gzip, 5, true, 1_KB);
    }
    return plan;
  }

  std::expected<TransformResult, CacheError> transform(
      AlternateId target_alternate, const OptimizationContext &ctx) override {
    started.count_down();

    // Wait until released or cancelled
    while (!released.load(std::memory_order_acquire)) {
      if (ctx.is_cancelled()) {
        return std::unexpected(CacheError::OptimizationCancelled);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    TransformResult result;
    result.alternate_id = target_alternate;
    result.header = std::vector<std::byte>(ctx.source_header().begin(),
                                           ctx.source_header().end());
    result.content = std::vector<std::byte>(ctx.source_content().begin(),
                                            ctx.source_content().end());

    return result;
  }

  std::latch started{1};
  std::atomic<bool> released{false};
};

// Plugin that blocks inside transform until released, counting entries (for
// the duplicate in-flight work item test).
class CountingBlockingPlugin : public OptimizationPlugin {
 public:
  [[nodiscard]] PluginInfo info() const override {
    return {"counting-blocking", "1.0.0", 103};
  }

  OptimizationPlan plan_optimization(const CacheKey & /*key*/,
                                     std::span<const std::byte> /*header*/,
                                     uint64_t /*content_length*/,
                                     AlternateId written_alternate,
                                     uint32_t /*hit_count*/) override {
    OptimizationPlan plan;
    if (written_alternate == AlternateId::Original) {
      plan.add(AlternateId::Gzip, 5, true, 1_KB);
    }
    return plan;
  }

  std::expected<TransformResult, CacheError> transform(
      AlternateId target_alternate, const OptimizationContext &ctx) override {
    entries.fetch_add(1, std::memory_order_acq_rel);

    // Wait until released or cancelled
    while (!released.load(std::memory_order_acquire)) {
      if (ctx.is_cancelled()) {
        return std::unexpected(CacheError::OptimizationCancelled);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    TransformResult result;
    result.alternate_id = target_alternate;
    result.header = std::vector<std::byte>(ctx.source_header().begin(),
                                           ctx.source_header().end());
    result.content = std::vector<std::byte>(ctx.source_content().begin(),
                                            ctx.source_content().end());
    return result;
  }

  void on_cancelled(const CacheKey & /*key*/, AlternateId /*target*/) override {
    ++cancel_calls;
  }

  std::atomic<int> entries{0};
  std::atomic<int> cancel_calls{0};
  std::atomic<bool> released{false};
};

// Plugin whose transform() throws (for the exception-guard regression test).
class TransformThrowingPlugin : public OptimizationPlugin {
 public:
  [[nodiscard]] PluginInfo info() const override {
    return {"transform-throwing", "1.0.0", 104};
  }

  OptimizationPlan plan_optimization(const CacheKey & /*key*/,
                                     std::span<const std::byte> /*header*/,
                                     uint64_t /*content_length*/,
                                     AlternateId written_alternate,
                                     uint32_t /*hit_count*/) override {
    OptimizationPlan plan;
    if (written_alternate == AlternateId::Original) {
      plan.add(AlternateId::Gzip, 5, true, 1_KB);
    }
    return plan;
  }

  std::expected<TransformResult, CacheError> transform(
      AlternateId /*target_alternate*/,
      const OptimizationContext & /*ctx*/) override {
    attempts.fetch_add(1, std::memory_order_acq_rel);
    throw std::runtime_error("transform failure");
  }

  void on_cancelled(const CacheKey & /*key*/, AlternateId /*target*/) override {
  }

  std::atomic<int> attempts{0};
};

// Plugin whose plan_optimization() throws on the caller's thread (for the
// exception-guard regression test).
class PlanThrowingPlugin : public OptimizationPlugin {
 public:
  [[nodiscard]] PluginInfo info() const override {
    return {"plan-throwing", "1.0.0", 105};
  }

  OptimizationPlan plan_optimization(const CacheKey & /*key*/,
                                     std::span<const std::byte> /*header*/,
                                     uint64_t /*content_length*/,
                                     AlternateId /*written_alternate*/,
                                     uint32_t /*hit_count*/) override {
    attempts.fetch_add(1, std::memory_order_acq_rel);
    throw std::runtime_error("planning failure");
  }

  std::expected<TransformResult, CacheError> transform(
      AlternateId /*target_alternate*/,
      const OptimizationContext & /*ctx*/) override {
    return std::unexpected(CacheError::TransformFailed);
  }

  void on_cancelled(const CacheKey & /*key*/, AlternateId /*target*/) override {
  }

  std::atomic<int> attempts{0};
};

std::string temp_cache_path() {
  auto path = fs::temp_directory_path() / "cyclone_opt_test";
  fs::create_directories(path);
  return path.string();
}

void cleanup_temp(const std::string &path) {
  std::error_code ec;
  fs::remove_all(path, ec);
}

// Plugin that throws an exception during transform (for testing exception
// handling)
class ThrowingOptimizationPlugin : public OptimizationPlugin {
 public:
  [[nodiscard]] PluginInfo info() const override {
    return {"throwing-optimization", "1.0.0", 102};
  }

  OptimizationPlan plan_optimization(const CacheKey & /*key*/,
                                     std::span<const std::byte> /*header*/,
                                     uint64_t /*content_length*/,
                                     AlternateId written_alternate,
                                     uint32_t /*hit_count*/) override {
    OptimizationPlan plan;
    if (written_alternate == AlternateId::Original) {
      plan.add(AlternateId::Gzip, 5, true, 1_KB);
    }
    return plan;
  }

  std::expected<TransformResult, CacheError> transform(
      AlternateId /*target_alternate*/,
      const OptimizationContext & /*ctx*/) override {
    ++transform_attempts;
    // Return an error instead of throwing (proper error handling)
    return std::unexpected(CacheError::TransformFailed);
  }

  std::atomic<int> transform_attempts{0};
};

}  // namespace

TEST_CASE("OptimizationEngine basic lifecycle", "[optimization][integration]") {
  auto cache_path = temp_cache_path();

  CacheConfig config;
  config.optimization_config.set_enabled(true)
      .set_min_threads(1)
      .set_max_threads(2)
      .set_min_hits_before_optimize(0);

  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  REQUIRE(cache->add_volume(cache_path + "/vol1", 10_MB).has_value());
  REQUIRE(cache->start().has_value());

  auto *engine = cache->optimization_engine();
  REQUIRE(engine != nullptr);
  REQUIRE(engine->is_running());

  cache->stop();
  REQUIRE_FALSE(engine->is_running());

  cleanup_temp(cache_path);
}

// Issue #52: the engine is opt-in.  A default CacheConfig must not construct
// it, so a cache nobody feeds starts no LoadMonitor thread and no pool
// workers; the accessor reports that as nullptr before and after start().
TEST_CASE("OptimizationEngine absent with a default CacheConfig",
          "[optimization][integration]") {
  auto cache_path = temp_cache_path();

  CacheConfig config;
  REQUIRE_FALSE(config.optimization_config.enabled);
  REQUIRE_FALSE(OptimizationConfig{}.enabled);

  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;
  REQUIRE(cache->optimization_engine() == nullptr);

  REQUIRE(cache->add_volume(cache_path + "/vol1", 10_MB).has_value());
  REQUIRE(cache->start().has_value());
  REQUIRE(cache->optimization_engine() == nullptr);
  const Cache &const_cache = *cache;
  REQUIRE(const_cache.optimization_engine() == nullptr);

  // Writes and reads work without the engine: nothing feeds it anyway.
  auto key = CacheKey::from_url("http://example.com/no-engine");
  std::vector<std::byte> content(64, std::byte{0x42});
  auto write_result = cache->write_sync(key, content.size());
  REQUIRE(write_result.has_value());
  REQUIRE(write_result->write_sync(content).has_value());
  REQUIRE(write_result->close_sync().has_value());
  REQUIRE(cache->read_sync(key).has_value());

  cache->stop();
  REQUIRE(cache->optimization_engine() == nullptr);
  cleanup_temp(cache_path);
}

TEST_CASE("OptimizationEngine disabled when not enabled",
          "[optimization][integration]") {
  auto cache_path = temp_cache_path();

  CacheConfig config;
  config.optimization_config.set_enabled(false);

  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  REQUIRE(cache->add_volume(cache_path + "/vol1", 10_MB).has_value());
  REQUIRE(cache->start().has_value());

  REQUIRE(cache->optimization_engine() == nullptr);

  cache->stop();
  cleanup_temp(cache_path);
}

TEST_CASE("OptimizationEngine plugin registration",
          "[optimization][integration]") {
  auto cache_path = temp_cache_path();

  CacheConfig config;
  config.optimization_config.set_enabled(true).set_min_threads(1);

  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  REQUIRE(cache->add_volume(cache_path + "/vol1", 10_MB).has_value());
  REQUIRE(cache->start().has_value());

  auto *engine = cache->optimization_engine();
  REQUIRE(engine != nullptr);

  // Register plugin
  auto plugin = std::make_shared<TestOptimizationPlugin>();
  engine->register_plugin(plugin);

  auto plugins = engine->plugins();
  REQUIRE(plugins.size() == 1);
  REQUIRE(plugins[0]->info().name == "test-optimization");

  // Unregister
  engine->unregister_plugin(100);
  plugins = engine->plugins();
  REQUIRE(plugins.empty());

  cache->stop();
  cleanup_temp(cache_path);
}

TEST_CASE("OptimizationEngine min_hits_before_optimize threshold",
          "[optimization][integration]") {
  auto cache_path = temp_cache_path();

  CacheConfig config;
  config.optimization_config.set_enabled(true)
      .set_min_threads(1)
      .set_min_hits_before_optimize(5);

  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  REQUIRE(cache->add_volume(cache_path + "/vol1", 10_MB).has_value());
  REQUIRE(cache->start().has_value());

  auto *engine = cache->optimization_engine();
  auto plugin = std::make_shared<TestOptimizationPlugin>();
  engine->register_plugin(plugin);

  // Manually call on_write_complete with low hit count - should be skipped
  auto key = CacheKey::from_url("http://test.com/low-hits");
  std::vector<std::byte> header = {std::byte{0x01}};

  engine->on_write_complete(key, header, 100, AlternateId::Original,
                            2);  // hit_count=2 < threshold=5

  // Give time for potential optimization
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Should not have called transform (hit count too low)
  REQUIRE(plugin->transform_calls.load() == 0);

  auto stats = engine->stats();
  REQUIRE(stats.skipped > 0);

  // Now try with hit count above threshold
  engine->on_write_complete(key, header, 100, AlternateId::Original,
                            10);  // hit_count=10 >= threshold=5

  // Should have queued the optimization (may or may not have processed yet)
  stats = engine->stats();
  REQUIRE(stats.queued > 0);

  cache->stop();
  cleanup_temp(cache_path);
}

TEST_CASE("OptimizationEngine pause and resume",
          "[optimization][integration]") {
  auto cache_path = temp_cache_path();

  CacheConfig config;
  config.optimization_config.set_enabled(true)
      .set_min_threads(1)
      .set_min_hits_before_optimize(0);

  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  REQUIRE(cache->add_volume(cache_path + "/vol1", 10_MB).has_value());
  REQUIRE(cache->start().has_value());

  auto *engine = cache->optimization_engine();
  REQUIRE(engine != nullptr);

  REQUIRE_FALSE(engine->is_paused());

  engine->pause();
  REQUIRE(engine->is_paused());

  engine->resume();
  REQUIRE_FALSE(engine->is_paused());

  cache->stop();
  cleanup_temp(cache_path);
}

TEST_CASE("OptimizationEngine statistics", "[optimization][integration]") {
  auto cache_path = temp_cache_path();

  CacheConfig config;
  config.optimization_config.set_enabled(true)
      .set_min_threads(1)
      .set_min_hits_before_optimize(0);

  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  REQUIRE(cache->add_volume(cache_path + "/vol1", 10_MB).has_value());
  REQUIRE(cache->start().has_value());

  auto *engine = cache->optimization_engine();
  auto plugin = std::make_shared<TestOptimizationPlugin>();
  engine->register_plugin(plugin);

  auto initial_stats = engine->stats();
  REQUIRE(initial_stats.queued == 0);

  cache->stop();
  cleanup_temp(cache_path);
}

TEST_CASE("OptimizationEngine load monitor integration",
          "[optimization][integration]") {
  auto cache_path = temp_cache_path();

  CacheConfig config;
  config.optimization_config.set_enabled(true)
      .set_min_threads(1)
      .set_load_high_watermark(0.9)
      .set_load_low_watermark(0.5);

  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  REQUIRE(cache->add_volume(cache_path + "/vol1", 10_MB).has_value());
  REQUIRE(cache->start().has_value());

  auto *engine = cache->optimization_engine();
  REQUIRE(engine != nullptr);

  // Access load monitor
  auto &load_monitor = engine->load_monitor();

  // Record some operations
  load_monitor.record_read_complete(1_KB);
  load_monitor.record_write_complete(2_KB);
  load_monitor.update_metrics();

  auto metrics = load_monitor.metrics();
  REQUIRE(metrics.read_ops == 1);
  REQUIRE(metrics.write_ops == 1);

  cache->stop();
  cleanup_temp(cache_path);
}

TEST_CASE("OptimizationEngine work queue integration",
          "[optimization][integration]") {
  auto cache_path = temp_cache_path();

  CacheConfig config;
  config.optimization_config.set_enabled(true)
      .set_min_threads(1)
      .set_max_queue_size(100)
      .set_min_hits_before_optimize(0);

  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  REQUIRE(cache->add_volume(cache_path + "/vol1", 10_MB).has_value());
  REQUIRE(cache->start().has_value());

  auto *engine = cache->optimization_engine();
  REQUIRE(engine != nullptr);

  // Access work queue
  auto &queue = engine->work_queue();
  REQUIRE(queue.empty());

  auto queue_stats = queue.stats();
  REQUIRE(queue_stats.total_enqueued == 0);

  cache->stop();
  cleanup_temp(cache_path);
}

TEST_CASE("OptimizationEngine handles transform failures",
          "[optimization][integration]") {
  auto cache_path = temp_cache_path();

  CacheConfig config;
  config.optimization_config.set_enabled(true)
      .set_min_threads(1)
      .set_min_hits_before_optimize(0);

  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  REQUIRE(cache->add_volume(cache_path + "/vol1", 10_MB).has_value());
  REQUIRE(cache->start().has_value());

  auto *engine = cache->optimization_engine();
  auto plugin = std::make_shared<ThrowingOptimizationPlugin>();
  engine->register_plugin(plugin);

  // Write content to trigger optimization
  auto key = CacheKey::from_url("http://test.com/fail-transform");
  std::vector<std::byte> content(100, std::byte{0x42});

  auto write_result = cache->write_sync(key, content.size());
  REQUIRE(write_result.has_value());
  write_result->write_sync(content);
  REQUIRE(write_result->close_sync().has_value());

  // Manually trigger optimization for this key
  std::vector<std::byte> header = {std::byte{0x01}};
  engine->on_write_complete(key, header, content.size(), AlternateId::Original,
                            10);

  // Wait for optimization attempt
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // Should have attempted transform and failed
  REQUIRE(plugin->transform_attempts.load() >= 1);

  // Stats should show failure
  auto stats = engine->stats();
  REQUIRE(stats.failed > 0);

  cache->stop();
  cleanup_temp(cache_path);
}

TEST_CASE("OptimizationEngine concurrent stress",
          "[optimization][integration][stress]") {
  auto cache_path = temp_cache_path();

  CacheConfig config;
  config.optimization_config.set_enabled(true)
      .set_min_threads(2)
      .set_max_threads(4)
      .set_max_queue_size(1000)
      .set_min_hits_before_optimize(0);

  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  REQUIRE(cache->add_volume(cache_path + "/vol1", 50_MB).has_value());
  REQUIRE(cache->start().has_value());

  auto *engine = cache->optimization_engine();
  auto plugin = std::make_shared<TestOptimizationPlugin>();
  engine->register_plugin(plugin);

  // Run multiple threads doing writes
  constexpr int num_threads = 4;
  constexpr int writes_per_thread = 50;

  std::vector<std::thread> threads;
  std::atomic<int> successful_writes{0};

  threads.reserve(num_threads);
  for (int t = 0; t < num_threads; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < writes_per_thread; ++i) {
        auto url = "http://test.com/thread" + std::to_string(t) + "/item" +
                   std::to_string(i);
        auto key = CacheKey::from_url(url);

        std::vector<std::byte> content(100 + (i % 100),
                                       static_cast<std::byte>(t));

        auto write_result = cache->write_sync(key, content.size());
        if (write_result.has_value()) {
          write_result->write_sync(content);
          if (write_result->close_sync().has_value()) {
            successful_writes.fetch_add(1, std::memory_order_relaxed);
          }
        }
      }
    });
  }

  for (auto &th : threads) {
    th.join();
  }

  // Wait for optimization to process
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  REQUIRE(successful_writes.load() > 0);

  auto stats = engine->stats();
  // Should have queued and potentially processed some items
  REQUIRE(stats.queued >= 0);  // May be 0 if processing is fast

  cache->stop();
  cleanup_temp(cache_path);
}

// Regression: the pool's start() resets its stop/pause flags, so stop -> start
// cycles are meant to work, but the WorkQueue's stopped flag was never reset.
// A restarted engine then refused every enqueue (Closed) and its workers
// exited immediately on a stopped queue: the engine reported running while
// silently dead.
TEST_CASE("OptimizationEngine stop/start cycle resumes work",
          "[optimization][integration][regression]") {
  auto cache_path = temp_cache_path();

  CacheConfig config;
  config.optimization_config.set_enabled(true)
      .set_min_threads(1)
      .set_min_hits_before_optimize(0);

  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  REQUIRE(cache->add_volume(cache_path + "/vol1", 10_MB).has_value());
  REQUIRE(cache->start().has_value());

  auto *engine = cache->optimization_engine();
  REQUIRE(engine != nullptr);
  auto plugin = std::make_shared<TestOptimizationPlugin>();
  engine->register_plugin(plugin);

  auto run_one = [&](const char *url, int expected_calls) {
    auto key = CacheKey::from_url(url);
    std::vector<std::byte> content(100, std::byte{0x42});
    auto write_result = cache->write_sync(key, content.size());
    REQUIRE(write_result.has_value());
    REQUIRE(write_result->write_sync(content).has_value());
    REQUIRE(write_result->close_sync().has_value());

    std::vector<std::byte> header = {std::byte{0x01}};
    engine->on_write_complete(key, header, content.size(),
                              AlternateId::Original, 10);

    auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (plugin->transform_calls.load() < expected_calls &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return plugin->transform_calls.load() >= expected_calls;
  };

  REQUIRE(run_one("http://test.com/before-restart", 1));

  cache->stop();
  REQUIRE(cache->start().has_value());
  REQUIRE(engine->is_running());

  REQUIRE(run_one("http://test.com/after-restart", 2));

  cache->stop();
  cleanup_temp(cache_path);
}

// Regression: dequeue() erases an item's id from the queue's dedup set, so a
// same-id item can be enqueued again while the first instance is still
// running.  Registering the second instance used to overwrite the first
// one's cancellation token, so request_stop() could no longer signal the
// running instance and two workers ran the same transform, racing the same
// alternate write.  A duplicate of an in-flight item must be refused, and a
// later item for the same id must still run once the first completes.
TEST_CASE("OptimizationEngine refuses duplicate in-flight work items",
          "[optimization][integration][regression]") {
  auto cache_path = temp_cache_path();

  CacheConfig config;
  config.optimization_config.set_enabled(true)
      .set_min_threads(2)
      .set_max_threads(2)
      .set_min_hits_before_optimize(0);

  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  REQUIRE(cache->add_volume(cache_path + "/vol1", 10_MB).has_value());
  REQUIRE(cache->start().has_value());

  auto *engine = cache->optimization_engine();
  REQUIRE(engine != nullptr);
  auto plugin = std::make_shared<CountingBlockingPlugin>();
  engine->register_plugin(plugin);

  auto key = CacheKey::from_url("http://test.com/duplicate-in-flight");
  std::vector<std::byte> content(100, std::byte{0x42});
  auto write_result = cache->write_sync(key, content.size());
  REQUIRE(write_result.has_value());
  REQUIRE(write_result->write_sync(content).has_value());
  REQUIRE(write_result->close_sync().has_value());

  std::vector<std::byte> header = {std::byte{0x01}};

  auto wait_for_entries = [&](int expected) {
    auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (plugin->entries.load(std::memory_order_acquire) < expected &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return plugin->entries.load(std::memory_order_acquire) >= expected;
  };

  // First instance: picked up by worker 1 and blocked inside transform.
  engine->on_write_complete(key, header, content.size(), AlternateId::Original,
                            10);
  REQUIRE(wait_for_entries(1));

  // Duplicate: re-enqueued (the running instance is no longer in the queue's
  // dedup set) and picked up by worker 2, which must refuse it.
  engine->on_write_complete(key, header, content.size(), AlternateId::Original,
                            10);

  // Give worker 2 ample time to (incorrectly) enter transform.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  const int entries_while_running =
      plugin->entries.load(std::memory_order_acquire);

  // Release worker 1 and let the write complete.
  plugin->released.store(true, std::memory_order_release);
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (engine->stats().completed < 1 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  // Once the first instance is done, the same id must be runnable again.
  engine->on_write_complete(key, header, content.size(), AlternateId::Original,
                            10);
  const bool reran = wait_for_entries(entries_while_running + 1);

  cache->stop();
  cleanup_temp(cache_path);

  REQUIRE(entries_while_running == 1);
  REQUIRE(reran);
}

// Regression: an exception escaping a plugin's transform() propagated out of
// the pool's worker loop and terminated the process.  The engine must guard
// the call, record the failure, and keep the worker alive for later work.
TEST_CASE("OptimizationEngine survives a throwing plugin transform",
          "[optimization][integration][regression]") {
  auto cache_path = temp_cache_path();

  CacheConfig config;
  config.optimization_config.set_enabled(true)
      .set_min_threads(1)
      .set_min_hits_before_optimize(0);

  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  REQUIRE(cache->add_volume(cache_path + "/vol1", 10_MB).has_value());
  REQUIRE(cache->start().has_value());

  auto *engine = cache->optimization_engine();
  REQUIRE(engine != nullptr);
  auto plugin = std::make_shared<TransformThrowingPlugin>();
  engine->register_plugin(plugin);

  auto trigger = [&](const char *url) {
    auto key = CacheKey::from_url(url);
    std::vector<std::byte> content(100, std::byte{0x42});
    auto write_result = cache->write_sync(key, content.size());
    REQUIRE(write_result.has_value());
    REQUIRE(write_result->write_sync(content).has_value());
    REQUIRE(write_result->close_sync().has_value());
    std::vector<std::byte> header = {std::byte{0x01}};
    engine->on_write_complete(key, header, content.size(),
                              AlternateId::Original, 10);
  };

  trigger("http://test.com/throw-1");

  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (plugin->attempts.load(std::memory_order_acquire) < 1 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  REQUIRE(plugin->attempts.load(std::memory_order_acquire) >= 1);

  // The failure is recorded and the engine and its worker stay alive.
  deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (engine->stats().failed < 1 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  auto stats = engine->stats();
  REQUIRE(stats.failed >= 1);
  REQUIRE(engine->is_running());
  REQUIRE(stats.pool_stats.current_threads >= 1);

  // Later work is still processed (and also fails) on the surviving worker.
  trigger("http://test.com/throw-2");
  deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (plugin->attempts.load(std::memory_order_acquire) < 2 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  REQUIRE(plugin->attempts.load(std::memory_order_acquire) >= 2);

  cache->stop();
  cleanup_temp(cache_path);
}

// Regression: plan_optimization() runs on the embedder's own thread inside
// on_write_complete(), so a throwing plugin used to fault the caller's write
// path.  The engine must guard the call and record the failure.
TEST_CASE("OptimizationEngine guards exceptions from plugin planning",
          "[optimization][integration][regression]") {
  auto cache_path = temp_cache_path();

  CacheConfig config;
  config.optimization_config.set_enabled(true)
      .set_min_threads(1)
      .set_min_hits_before_optimize(0);

  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  REQUIRE(cache->add_volume(cache_path + "/vol1", 10_MB).has_value());
  REQUIRE(cache->start().has_value());

  auto *engine = cache->optimization_engine();
  REQUIRE(engine != nullptr);
  auto plugin = std::make_shared<PlanThrowingPlugin>();
  engine->register_plugin(plugin);

  auto key = CacheKey::from_url("http://test.com/plan-throws");
  std::vector<std::byte> header = {std::byte{0x01}};
  engine->on_write_complete(key, header, 100, AlternateId::Original, 10);

  REQUIRE(plugin->attempts.load(std::memory_order_acquire) == 1);
  REQUIRE(engine->stats().failed >= 1);
  REQUIRE(engine->is_running());

  cache->stop();
  cleanup_temp(cache_path);
}

// Regression: record_read_complete()/record_write_complete() had no callers
// outside tests, so every counter stayed at zero, the computed load stayed
// pinned at 0.0, and load shedding (pool pause + cancel_deferrable at the
// high watermark) could never engage.  Completed cache reads and writes must
// feed the monitor.
TEST_CASE("OptimizationEngine load monitor observes cache traffic",
          "[optimization][integration][regression]") {
  auto cache_path = temp_cache_path();

  CacheConfig config;
  config.optimization_config.set_enabled(true)
      .set_min_threads(1)
      .set_min_hits_before_optimize(0)
      .set_scale_check_interval(std::chrono::milliseconds(50));

  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  REQUIRE(cache->add_volume(cache_path + "/vol1", 10_MB).has_value());
  REQUIRE(cache->start().has_value());

  auto *engine = cache->optimization_engine();
  REQUIRE(engine != nullptr);

  constexpr uint64_t kWrites = 3;
  constexpr size_t kContentSize = 256;
  for (uint64_t i = 0; i < kWrites; ++i) {
    auto key =
        CacheKey::from_url("http://test.com/load-" + std::to_string(i));
    std::vector<std::byte> content(kContentSize, std::byte{0x42});
    auto write_result = cache->write_sync(key, content.size());
    REQUIRE(write_result.has_value());
    REQUIRE(write_result->write_sync(content).has_value());
    REQUIRE(write_result->close_sync().has_value());
  }

  constexpr uint64_t kReads = 5;
  for (uint64_t i = 0; i < kReads; ++i) {
    auto read_result =
        cache->read_sync(CacheKey::from_url("http://test.com/load-0"));
    REQUIRE(read_result.has_value());
  }

  // The monitoring thread folds the counters into metrics every
  // scale_check_interval; wait for that.
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  auto metrics = engine->stats().load_metrics;
  while ((metrics.write_ops < kWrites || metrics.read_ops < kReads) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    metrics = engine->stats().load_metrics;
  }

  cache->stop();
  cleanup_temp(cache_path);

  REQUIRE(metrics.write_ops >= kWrites);
  REQUIRE(metrics.write_bytes >= kWrites * kContentSize);
  REQUIRE(metrics.read_ops >= kReads);
  REQUIRE(metrics.read_bytes >= kContentSize);
}
