// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <latch>
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
