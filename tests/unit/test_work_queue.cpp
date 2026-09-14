// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <latch>
#include <thread>
#include <vector>

#include "optimization/work_queue.hpp"

using namespace cyclone;

namespace {

WorkItem make_item(const std::string &url, AlternateId alt,
                   uint32_t priority = 0, uint32_t hits = 0) {
  WorkItem item;
  item.id.key = CacheKey::from_url(url);
  item.id.target_alternate = alt;
  item.priority = priority;
  item.hit_count = hits;
  item.content_length = 1024;
  item.estimated_memory = 2048;
  item.deferrable = true;
  return item;
}

OptimizationConfig default_config() {
  OptimizationConfig config;
  config.max_queue_size = 100;
  config.max_memory_bytes = 1_MB;
  return config;
}

}  // namespace

TEST_CASE("WorkQueue basic operations", "[queue][optimization]") {
  OptimizationConfig config = default_config();
  WorkQueue queue(config);

  SECTION("enqueue and dequeue single item") {
    auto item = make_item("http://example.com/test", AlternateId::Brotli);

    auto result = queue.enqueue(item);
    REQUIRE(result.has_value());
    REQUIRE(queue.size() == 1);
    REQUIRE_FALSE(queue.empty());

    auto dequeued = queue.try_dequeue();
    REQUIRE(dequeued.has_value());
    REQUIRE(dequeued->id.key == CacheKey::from_url("http://example.com/test"));
    REQUIRE(dequeued->id.target_alternate == AlternateId::Brotli);
    REQUIRE(queue.empty());
  }

  SECTION("empty queue returns nullopt") {
    auto item = queue.try_dequeue();
    REQUIRE_FALSE(item.has_value());
  }

  SECTION("contains check") {
    auto item = make_item("http://example.com/test", AlternateId::Gzip);
    auto id = item.id;

    REQUIRE_FALSE(queue.contains(id));
    queue.enqueue(item);
    REQUIRE(queue.contains(id));
    queue.try_dequeue();
    REQUIRE_FALSE(queue.contains(id));
  }
}

TEST_CASE("WorkQueue priority ordering", "[queue][optimization]") {
  OptimizationConfig config = default_config();
  WorkQueue queue(config);

  SECTION("higher priority dequeued first") {
    queue.enqueue(make_item("http://a.com", AlternateId::Brotli, 1, 0));
    queue.enqueue(make_item("http://b.com", AlternateId::Brotli, 10, 0));
    queue.enqueue(make_item("http://c.com", AlternateId::Brotli, 5, 0));

    auto first = queue.try_dequeue();
    REQUIRE(first.has_value());
    REQUIRE(first->priority == 10);

    auto second = queue.try_dequeue();
    REQUIRE(second.has_value());
    REQUIRE(second->priority == 5);

    auto third = queue.try_dequeue();
    REQUIRE(third.has_value());
    REQUIRE(third->priority == 1);
  }

  SECTION("hit count as tiebreaker") {
    queue.enqueue(make_item("http://a.com", AlternateId::Brotli, 5, 100));
    queue.enqueue(make_item("http://b.com", AlternateId::Brotli, 5, 500));
    queue.enqueue(make_item("http://c.com", AlternateId::Brotli, 5, 200));

    auto first = queue.try_dequeue();
    REQUIRE(first.has_value());
    REQUIRE(first->hit_count == 500);

    auto second = queue.try_dequeue();
    REQUIRE(second.has_value());
    REQUIRE(second->hit_count == 200);

    auto third = queue.try_dequeue();
    REQUIRE(third.has_value());
    REQUIRE(third->hit_count == 100);
  }
}

TEST_CASE("WorkQueue deduplication", "[queue][optimization]") {
  OptimizationConfig config = default_config();
  WorkQueue queue(config);

  auto item1 = make_item("http://example.com/test", AlternateId::Brotli);
  auto item2 =
      make_item("http://example.com/test", AlternateId::Brotli);  // Same ID

  queue.enqueue(item1);
  auto result = queue.enqueue(item2);

  REQUIRE(result.has_value());  // Duplicate silently succeeds
  REQUIRE(queue.size() == 1);   // But only one item in queue

  auto stats = queue.stats();
  REQUIRE(stats.duplicates == 1);
}

TEST_CASE("WorkQueue cancellation", "[queue][optimization]") {
  OptimizationConfig config = default_config();
  WorkQueue queue(config);

  SECTION("cancel by ID") {
    auto item = make_item("http://example.com/test", AlternateId::Zstd);
    auto id = item.id;

    queue.enqueue(item);
    REQUIRE(queue.size() == 1);

    bool cancelled = queue.cancel(id);
    REQUIRE(cancelled);
    REQUIRE(queue.empty());

    // Cancel non-existent returns false
    REQUIRE_FALSE(queue.cancel(id));
  }

  SECTION("cancel by key") {
    queue.enqueue(make_item("http://example.com/a", AlternateId::Brotli));
    queue.enqueue(make_item("http://example.com/a", AlternateId::Gzip));
    queue.enqueue(make_item("http://example.com/b", AlternateId::Brotli));

    REQUIRE(queue.size() == 3);

    size_t cancelled =
        queue.cancel_by_key(CacheKey::from_url("http://example.com/a"));
    REQUIRE(cancelled == 2);
    REQUIRE(queue.size() == 1);
  }

  SECTION("cancel by non-existent key returns zero") {
    queue.enqueue(make_item("http://example.com/a", AlternateId::Brotli));
    REQUIRE(queue.size() == 1);

    size_t cancelled =
        queue.cancel_by_key(CacheKey::from_url("http://non-existent.com/key"));
    REQUIRE(cancelled == 0);
    REQUIRE(queue.size() == 1);  // Original item still there
  }

  SECTION("cancel deferrable") {
    auto deferrable = make_item("http://a.com", AlternateId::Brotli);
    deferrable.deferrable = true;

    auto non_deferrable = make_item("http://b.com", AlternateId::Brotli);
    non_deferrable.deferrable = false;

    queue.enqueue(deferrable);
    queue.enqueue(non_deferrable);
    REQUIRE(queue.size() == 2);

    size_t cancelled = queue.cancel_deferrable();
    REQUIRE(cancelled == 1);
    REQUIRE(queue.size() == 1);

    auto remaining = queue.try_dequeue();
    REQUIRE(remaining.has_value());
    REQUIRE_FALSE(remaining->deferrable);
  }
}

TEST_CASE("WorkQueue capacity limits", "[queue][optimization]") {
  SECTION("queue size limit") {
    OptimizationConfig config;
    config.max_queue_size = 3;
    config.max_memory_bytes = 100_MB;
    WorkQueue queue(config);

    queue.enqueue(make_item("http://a.com", AlternateId::Brotli));
    queue.enqueue(make_item("http://b.com", AlternateId::Brotli));
    queue.enqueue(make_item("http://c.com", AlternateId::Brotli));

    auto result = queue.enqueue(make_item("http://d.com", AlternateId::Brotli));
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == CacheError::OptimizationQueueFull);

    auto stats = queue.stats();
    REQUIRE(stats.rejected_full == 1);
  }

  SECTION("memory limit") {
    OptimizationConfig config;
    config.max_queue_size = 1000;
    config.max_memory_bytes = 5000;
    WorkQueue queue(config);

    auto item1 = make_item("http://a.com", AlternateId::Brotli);
    item1.estimated_memory = 2000;
    queue.enqueue(item1);

    auto item2 = make_item("http://b.com", AlternateId::Brotli);
    item2.estimated_memory = 2000;
    queue.enqueue(item2);

    auto item3 = make_item("http://c.com", AlternateId::Brotli);
    item3.estimated_memory = 2000;
    auto result = queue.enqueue(item3);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == CacheError::OptimizationQueueFull);

    auto stats = queue.stats();
    REQUIRE(stats.rejected_memory == 1);
  }
}

TEST_CASE("WorkQueue stop behavior", "[queue][optimization]") {
  OptimizationConfig config = default_config();
  WorkQueue queue(config);

  queue.enqueue(make_item("http://a.com", AlternateId::Brotli));
  queue.stop();

  REQUIRE(queue.is_stopped());

  // Enqueue after stop fails
  auto result = queue.enqueue(make_item("http://b.com", AlternateId::Brotli));
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error() == CacheError::Closed);

  // try_dequeue still works to drain
  auto item = queue.try_dequeue();
  REQUIRE(item.has_value());

  // Empty after stop returns nullopt
  auto empty = queue.try_dequeue();
  REQUIRE_FALSE(empty.has_value());
}

TEST_CASE("WorkQueue blocking dequeue", "[queue][optimization][concurrent]") {
  OptimizationConfig config = default_config();
  WorkQueue queue(config);

  std::atomic<bool> got_item{false};
  std::latch start_latch(1);

  std::thread consumer([&] {
    start_latch.wait();
    auto item = queue.dequeue();  // Should block
    if (item.has_value()) {
      got_item = true;
    }
  });

  start_latch.count_down();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  REQUIRE_FALSE(got_item.load());  // Still blocked

  queue.enqueue(make_item("http://test.com", AlternateId::Brotli));

  consumer.join();
  REQUIRE(got_item.load());
}

TEST_CASE("WorkQueue stop unblocks dequeue",
          "[queue][optimization][concurrent]") {
  OptimizationConfig config = default_config();
  WorkQueue queue(config);

  std::atomic<bool> returned{false};
  std::latch start_latch(1);

  std::thread consumer([&] {
    start_latch.wait();
    auto item = queue.dequeue();  // Should block until stop
    REQUIRE_FALSE(item.has_value());
    returned = true;
  });

  start_latch.count_down();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  REQUIRE_FALSE(returned.load());

  queue.stop();

  consumer.join();
  REQUIRE(returned.load());
}

TEST_CASE("WorkQueue memory tracking", "[queue][optimization]") {
  OptimizationConfig config = default_config();
  WorkQueue queue(config);

  REQUIRE(queue.memory_usage() == 0);

  auto item1 = make_item("http://a.com", AlternateId::Brotli);
  item1.estimated_memory = 1000;
  queue.enqueue(item1);

  REQUIRE(queue.memory_usage() == 1000);

  auto item2 = make_item("http://b.com", AlternateId::Brotli);
  item2.estimated_memory = 500;
  queue.enqueue(item2);

  REQUIRE(queue.memory_usage() == 1500);

  queue.try_dequeue();
  // Memory is freed in priority order
  REQUIRE(queue.memory_usage() < 1500);

  queue.try_dequeue();
  REQUIRE(queue.memory_usage() == 0);
}

TEST_CASE("WorkQueue statistics", "[queue][optimization]") {
  OptimizationConfig config = default_config();
  WorkQueue queue(config);

  auto stats = queue.stats();
  REQUIRE(stats.total_enqueued == 0);
  REQUIRE(stats.total_dequeued == 0);

  queue.enqueue(make_item("http://a.com", AlternateId::Brotli));
  queue.enqueue(make_item("http://b.com", AlternateId::Gzip));

  stats = queue.stats();
  REQUIRE(stats.total_enqueued == 2);
  REQUIRE(stats.current_size == 2);

  queue.try_dequeue();
  stats = queue.stats();
  REQUIRE(stats.total_dequeued == 1);
  REQUIRE(stats.current_size == 1);

  queue.cancel(
      WorkItemId{CacheKey::from_url("http://b.com"), AlternateId::Gzip});
  stats = queue.stats();
  REQUIRE(stats.total_cancelled == 1);
  REQUIRE(stats.current_size == 0);
}

TEST_CASE("WorkQueue concurrent stress",
          "[queue][optimization][concurrent][stress]") {
  OptimizationConfig config;
  config.max_queue_size = 10000;
  config.max_memory_bytes = 100_MB;
  WorkQueue queue(config);

  constexpr size_t num_producers = 4;
  constexpr size_t items_per_producer = 1000;
  constexpr size_t num_consumers = 2;

  std::atomic<size_t> produced{0};
  std::atomic<size_t> consumed{0};
  std::latch start_latch(num_producers + num_consumers);

  std::vector<std::thread> threads;

  // Producers
  threads.reserve(num_producers);
  for (size_t p = 0; p < num_producers; ++p) {
    threads.emplace_back([&, p] {
      start_latch.arrive_and_wait();
      for (size_t i = 0; i < items_per_producer; ++i) {
        auto url =
            "http://producer" + std::to_string(p) + "/item" + std::to_string(i);
        auto item =
            make_item(url, AlternateId::Brotli, static_cast<uint32_t>(i % 10));
        if (queue.enqueue(item).has_value()) {
          produced.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }

  // Consumers
  for (size_t c = 0; c < num_consumers; ++c) {
    threads.emplace_back([&] {
      start_latch.arrive_and_wait();
      while (true) {
        auto item = queue.try_dequeue();
        if (item) {
          consumed.fetch_add(1, std::memory_order_relaxed);
        } else {
          // Check if producers are done
          if (produced.load(std::memory_order_acquire) ==
              num_producers * items_per_producer) {
            // Drain remaining
            while (auto remaining = queue.try_dequeue()) {
              consumed.fetch_add(1, std::memory_order_relaxed);
            }
            break;
          }
          std::this_thread::yield();
        }
      }
    });
  }

  for (auto &t : threads) {
    t.join();
  }

  // All items should be consumed (minus any duplicates)
  auto stats = queue.stats();
  REQUIRE(stats.total_enqueued == produced.load());
  REQUIRE(stats.total_dequeued == consumed.load());
  REQUIRE(queue.empty());
}
