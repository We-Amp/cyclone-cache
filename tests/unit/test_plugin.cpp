// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>

#include "cyclone/plugin/alternate.hpp"
#include "cyclone/plugin/metadata.hpp"
#include "cyclone/plugin/plugin.hpp"

using namespace cyclone;

TEST_CASE("Metadata basic operations", "[plugin]") {
  Metadata meta;
  REQUIRE(meta.empty());
  REQUIRE(meta.empty());

  std::vector<std::byte> data = {std::byte{1}, std::byte{2}, std::byte{3}};
  meta = Metadata(std::span<const std::byte>(data));

  REQUIRE_FALSE(meta.empty());
  REQUIRE(meta.size() == 3);
  REQUIRE(meta.data()[0] == std::byte{1});
  REQUIRE(meta.data()[1] == std::byte{2});
  REQUIRE(meta.data()[2] == std::byte{3});
}

TEST_CASE("Metadata assign", "[plugin]") {
  Metadata meta;
  std::vector<std::byte> data = {std::byte{0xAB}, std::byte{0xCD}};
  meta.assign(std::span<const std::byte>(data));

  REQUIRE(meta.size() == 2);
  REQUIRE(meta.data()[0] == std::byte{0xAB});
  REQUIRE(meta.data()[1] == std::byte{0xCD});
}

TEST_CASE("Metadata resize and mutable_data", "[plugin]") {
  Metadata meta;
  meta.resize(4);
  REQUIRE(meta.size() == 4);

  auto mutable_span = meta.mutable_data();
  mutable_span[0] = std::byte{0x10};
  mutable_span[1] = std::byte{0x20};
  mutable_span[2] = std::byte{0x30};
  mutable_span[3] = std::byte{0x40};

  REQUIRE(meta.data()[0] == std::byte{0x10});
  REQUIRE(meta.data()[3] == std::byte{0x40});
}

TEST_CASE("Metadata clear", "[plugin]") {
  std::vector<std::byte> data = {std::byte{1}, std::byte{2}};
  Metadata meta{std::span<const std::byte>(data)};

  REQUIRE_FALSE(meta.empty());
  meta.clear();
  REQUIRE(meta.empty());
  REQUIRE(meta.empty());
}

TEST_CASE("MetadataCollection basic operations", "[plugin]") {
  MetadataCollection collection;
  REQUIRE(collection.empty());
  REQUIRE(collection.count() == 0);

  std::vector<std::byte> data1 = {std::byte{1}, std::byte{2}};
  collection.set(1, Metadata(std::span<const std::byte>(data1)));

  REQUIRE_FALSE(collection.empty());
  REQUIRE(collection.count() == 1);
  REQUIRE(collection.has(1));
  REQUIRE_FALSE(collection.has(2));

  auto retrieved = collection.get(1);
  REQUIRE(retrieved.has_value());
  REQUIRE(retrieved->size() == 2);
}

TEST_CASE("MetadataCollection multiple plugins", "[plugin]") {
  MetadataCollection collection;

  std::vector<std::byte> data1 = {std::byte{0xAA}};
  std::vector<std::byte> data2 = {std::byte{0xBB}, std::byte{0xCC}};
  std::vector<std::byte> data3 = {std::byte{0xDD}, std::byte{0xEE},
                                  std::byte{0xFF}};

  collection.set(1, Metadata(std::span<const std::byte>(data1)));
  collection.set(2, Metadata(std::span<const std::byte>(data2)));
  collection.set(3, Metadata(std::span<const std::byte>(data3)));

  REQUIRE(collection.count() == 3);

  auto m1 = collection.get(1);
  auto m2 = collection.get(2);
  auto m3 = collection.get(3);

  REQUIRE(m1->size() == 1);
  REQUIRE(m2->size() == 2);
  REQUIRE(m3->size() == 3);
}

TEST_CASE("MetadataCollection remove", "[plugin]") {
  MetadataCollection collection;

  std::vector<std::byte> data = {std::byte{1}};
  collection.set(1, Metadata(std::span<const std::byte>(data)));
  collection.set(2, Metadata(std::span<const std::byte>(data)));

  REQUIRE(collection.count() == 2);

  collection.remove(1);
  REQUIRE(collection.count() == 1);
  REQUIRE_FALSE(collection.has(1));
  REQUIRE(collection.has(2));
}

TEST_CASE("MetadataCollection serialization round-trip", "[plugin]") {
  MetadataCollection original;

  std::vector<std::byte> data1 = {std::byte{0x11}, std::byte{0x22}};
  std::vector<std::byte> data2 = {std::byte{0x33}, std::byte{0x44},
                                  std::byte{0x55}};

  original.set(100, Metadata(std::span<const std::byte>(data1)));
  original.set(200, Metadata(std::span<const std::byte>(data2)));

  auto serialized = original.serialize();
  REQUIRE(!serialized.empty());

  auto restored =
      MetadataCollection::deserialize(std::span<const std::byte>(serialized));

  REQUIRE(restored.count() == 2);
  REQUIRE(restored.has(100));
  REQUIRE(restored.has(200));

  auto r1 = restored.get(100);
  auto r2 = restored.get(200);

  REQUIRE(r1->size() == 2);
  REQUIRE(r1->data()[0] == std::byte{0x11});
  REQUIRE(r1->data()[1] == std::byte{0x22});

  REQUIRE(r2->size() == 3);
  REQUIRE(r2->data()[0] == std::byte{0x33});
  REQUIRE(r2->data()[2] == std::byte{0x55});
}

TEST_CASE("MetadataCollection deserialize empty", "[plugin]") {
  std::vector<std::byte> empty_data;
  auto result =
      MetadataCollection::deserialize(std::span<const std::byte>(empty_data));
  REQUIRE(result.empty());
}

TEST_CASE("MetadataCollection clear", "[plugin]") {
  MetadataCollection collection;

  std::vector<std::byte> data = {std::byte{1}};
  collection.set(1, Metadata(std::span<const std::byte>(data)));
  collection.set(2, Metadata(std::span<const std::byte>(data)));

  REQUIRE(collection.count() == 2);
  collection.clear();
  REQUIRE(collection.empty());
  REQUIRE(collection.count() == 0);
}

TEST_CASE("PluginManager register and get", "[plugin]") {
  PluginManager manager;

  class TestPlugin : public CachePlugin {
   public:
    [[nodiscard]] PluginInfo info() const override {
      return {"test-plugin", "1.0", 42};
    }

    std::optional<size_t> select_variant(const VariantCollection &variants,
                                         const LookupContext &ctx) override {
      (void)ctx;
      if (variants.empty()) {
        return std::nullopt;
      }
      return 0;
    }
  };

  auto plugin = std::make_shared<TestPlugin>();
  manager.register_plugin(plugin);

  auto retrieved = manager.get_plugin(42);
  REQUIRE(retrieved != nullptr);
  REQUIRE(retrieved->info().name == "test-plugin");
}

TEST_CASE("PluginManager unregister", "[plugin]") {
  PluginManager manager;

  class TestPlugin : public CachePlugin {
   public:
    [[nodiscard]] PluginInfo info() const override {
      return {"unregister-test", "1.0", 99};
    }

    std::optional<size_t> select_variant(const VariantCollection &,
                                         const LookupContext &) override {
      return std::nullopt;
    }
  };

  auto plugin = std::make_shared<TestPlugin>();
  manager.register_plugin(plugin);

  REQUIRE(manager.get_plugin(99) != nullptr);

  manager.unregister_plugin(99);
  REQUIRE(manager.get_plugin(99) == nullptr);
}

TEST_CASE("PluginManager alternate selector", "[plugin]") {
  PluginManager manager;

  class TestAlternatePlugin : public CachePlugin {
   public:
    [[nodiscard]] PluginInfo info() const override {
      return {"alternate-test", "1.0", 50};
    }

    std::optional<size_t> select_variant(const VariantCollection &variants,
                                         const LookupContext &) override {
      if (variants.empty()) {
        return std::nullopt;
      }
      return variants.size() - 1;
    }
  };

  auto plugin = std::make_shared<TestAlternatePlugin>();
  manager.set_alternate_selector(plugin);

  auto selector = manager.get_alternate_selector();
  REQUIRE(selector != nullptr);
  REQUIRE(selector->info().plugin_id == 50);
}

TEST_CASE("SimpleAlternateSelector returns first variant", "[plugin]") {
  SimpleAlternateSelector selector;

  VariantCollection empty_variants;
  LookupContext ctx;

  auto result1 = selector.select_variant(empty_variants, ctx);
  REQUIRE_FALSE(result1.has_value());

  CacheVariant v1;
  v1.key = CacheKey("key1");
  CacheVariant v2;
  v2.key = CacheKey("key2");

  VariantCollection variants;
  variants.add(std::move(v1));
  variants.add(std::move(v2));

  auto result2 = selector.select_variant(variants, ctx);
  REQUIRE(result2.has_value());
  REQUIRE(*result2 == 0);
}

TEST_CASE("VariantCollection operations", "[plugin]") {
  VariantCollection collection;
  REQUIRE(collection.empty());
  REQUIRE(collection.empty());

  CacheVariant v1;
  v1.key = CacheKey("key1");
  v1.content_length = 100;

  CacheVariant v2;
  v2.key = CacheKey("key2");
  v2.content_length = 200;

  collection.add(std::move(v1));
  collection.add(std::move(v2));

  REQUIRE_FALSE(collection.empty());
  REQUIRE(collection.size() == 2);
  REQUIRE(collection[0].content_length == 100);
  REQUIRE(collection[1].content_length == 200);

  collection.clear();
  REQUIRE(collection.empty());
}

TEST_CASE("VariantCollection iteration", "[plugin]") {
  VariantCollection collection;

  for (int i = 0; i < 5; ++i) {
    CacheVariant v;
    v.key = CacheKey("key" + std::to_string(i));
    v.content_length = static_cast<uint64_t>(i) * 100;
    collection.add(std::move(v));
  }

  size_t count = 0;
  for (const auto &variant : collection) {
    REQUIRE(variant.content_length == count * 100);
    ++count;
  }
  REQUIRE(count == 5);
}

TEST_CASE("CacheVariant fields", "[plugin]") {
  CacheVariant variant;
  variant.key = CacheKey("test-key");
  variant.content_length = 1024;
  variant.created = std::chrono::system_clock::now();
  variant.last_accessed = std::chrono::system_clock::now();
  variant.hit_count = 5;

  REQUIRE(variant.content_length == 1024);
  REQUIRE(variant.hit_count == 5);
}

TEST_CASE("FreshnessContext default values", "[plugin]") {
  FreshnessContext ctx;
  REQUIRE_FALSE(ctx.force_revalidate);
  REQUIRE(ctx.user_data == nullptr);
}

TEST_CASE("KeyContext fields", "[plugin]") {
  KeyContext ctx;
  ctx.url = "http://example.com/path";
  ctx.hostname = "example.com";
  ctx.user_data = nullptr;

  REQUIRE(ctx.url == "http://example.com/path");
  REQUIRE(ctx.hostname == "example.com");
}

TEST_CASE("MetadataCollection deserialize with excessive count",
          "[plugin][security]") {
  // Craft malicious data with huge count value
  std::vector<std::byte> malicious_data(sizeof(uint32_t));
  uint32_t huge_count = 0xFFFFFFFF;
  std::memcpy(malicious_data.data(), &huge_count, sizeof(huge_count));

  // Should return empty collection, not loop forever or crash
  auto result = MetadataCollection::deserialize(
      std::span<const std::byte>(malicious_data));
  REQUIRE(result.empty());
}

TEST_CASE("MetadataCollection deserialize with excessive length",
          "[plugin][security]") {
  // Craft data with valid count but huge length for first section
  std::vector<std::byte> malicious_data(sizeof(uint32_t) * 3);
  uint32_t count = 1;
  uint32_t plugin_id = 1;
  uint32_t huge_len = 0xFFFFFFFF;
  std::memcpy(malicious_data.data(), &count, sizeof(count));
  std::memcpy(malicious_data.data() + sizeof(count), &plugin_id,
              sizeof(plugin_id));
  std::memcpy(malicious_data.data() + sizeof(count) + sizeof(plugin_id),
              &huge_len, sizeof(huge_len));

  // Should return empty collection, not crash or allocate huge memory
  auto result = MetadataCollection::deserialize(
      std::span<const std::byte>(malicious_data));
  REQUIRE(result.empty());
}

TEST_CASE("MetadataCollection deserialize with truncated data",
          "[plugin][security]") {
  // Valid header but data truncated mid-section
  std::vector<std::byte> truncated_data(sizeof(uint32_t) * 3 + 2);
  uint32_t count = 1;
  uint32_t plugin_id = 1;
  uint32_t len = 100;  // Claims 100 bytes but only 2 available
  std::memcpy(truncated_data.data(), &count, sizeof(count));
  std::memcpy(truncated_data.data() + sizeof(count), &plugin_id,
              sizeof(plugin_id));
  std::memcpy(truncated_data.data() + sizeof(count) + sizeof(plugin_id), &len,
              sizeof(len));

  auto result = MetadataCollection::deserialize(
      std::span<const std::byte>(truncated_data));
  // Should not crash, may return empty or partial results
  REQUIRE(result.count() == 0);
}

TEST_CASE("MetadataCollection deserialize respects max section count",
          "[plugin][security]") {
  // Check that the limit constant is accessible
  REQUIRE(MetadataCollection::kMaxSectionCount == 1024);
  REQUIRE(MetadataCollection::kMaxSectionSize == 16 * 1024 * 1024);
}

// --- Phase 3B: CachePlugin default implementations and OptimizationPlugin
// tests ---

#include "cyclone/plugin/optimization.hpp"

TEST_CASE("CachePlugin default check_freshness returns Fresh", "[plugin]") {
  // Minimal concrete subclass that only implements the pure virtual methods
  class MinimalPlugin : public CachePlugin {
   public:
    [[nodiscard]] PluginInfo info() const override {
      return {"minimal", "1.0", 999};
    }

    std::optional<size_t> select_variant(const VariantCollection &,
                                         const LookupContext &) override {
      return std::nullopt;
    }
  };

  MinimalPlugin plugin;

  CacheVariant variant;
  variant.key = CacheKey("freshness-test");

  FreshnessContext ctx;

  auto result = plugin.check_freshness(variant, ctx);
  REQUIRE(result == FreshnessResult::Fresh);
}

TEST_CASE("CachePlugin default eviction_priority returns 1.0", "[plugin]") {
  class MinimalPlugin : public CachePlugin {
   public:
    [[nodiscard]] PluginInfo info() const override {
      return {"minimal", "1.0", 998};
    }

    std::optional<size_t> select_variant(const VariantCollection &,
                                         const LookupContext &) override {
      return std::nullopt;
    }
  };

  MinimalPlugin plugin;

  CacheVariant variant;
  variant.key = CacheKey("eviction-priority-test");

  double priority = plugin.eviction_priority(variant);
  REQUIRE(priority == 1.0);
}

TEST_CASE("OptimizationContext accessors and cancellation",
          "[plugin][optimization]") {
  CacheKey key("optimization-context-test");

  std::vector<std::byte> header_data = {std::byte{0x01}, std::byte{0x02}};
  std::vector<std::byte> content_data = {std::byte{0x03}, std::byte{0x04},
                                         std::byte{0x05}};
  uint32_t hit_count = 42;

  std::atomic<bool> cancelled{false};

  OptimizationContext ctx(key, std::span<const std::byte>(header_data),
                          std::span<const std::byte>(content_data), hit_count,
                          nullptr,  // cache pointer
                          cancelled);

  // Verify accessors
  REQUIRE(std::equal(ctx.key().digest().begin(), ctx.key().digest().end(),
                     key.digest().begin()));
  REQUIRE(ctx.source_header().size() == 2);
  REQUIRE(ctx.source_header()[0] == std::byte{0x01});
  REQUIRE(ctx.source_content().size() == 3);
  REQUIRE(ctx.source_content()[0] == std::byte{0x03});
  REQUIRE(ctx.hit_count() == 42);
  REQUIRE(ctx.cache() == nullptr);

  // Verify cancellation support
  REQUIRE_FALSE(ctx.is_cancelled());

  cancelled.store(true, std::memory_order_release);
  REQUIRE(ctx.is_cancelled());
}

TEST_CASE("OptimizationPlan add and empty", "[plugin][optimization]") {
  OptimizationPlan plan;
  REQUIRE(plan.empty());
  REQUIRE(plan.targets.empty());

  plan.add(AlternateId::Gzip, 10, true, 4096);
  REQUIRE_FALSE(plan.empty());
  REQUIRE(plan.targets.size() == 1);
  REQUIRE(plan.targets[0].target_alternate == AlternateId::Gzip);
  REQUIRE(plan.targets[0].priority == 10);
  REQUIRE(plan.targets[0].deferrable == true);
  REQUIRE(plan.targets[0].estimated_memory == 4096);

  // Add another target
  plan.add(AlternateId::Brotli, 20, false, 8192);
  REQUIRE(plan.targets.size() == 2);
  REQUIRE(plan.targets[1].target_alternate == AlternateId::Brotli);
  REQUIRE(plan.targets[1].priority == 20);
  REQUIRE(plan.targets[1].deferrable == false);
  REQUIRE(plan.targets[1].estimated_memory == 8192);
}

TEST_CASE("NullOptimizationPlugin behavior", "[plugin][optimization]") {
  NullOptimizationPlugin plugin;

  // Verify info
  auto plugin_info = plugin.info();
  REQUIRE(plugin_info.name == "null-optimization");
  REQUIRE(plugin_info.version == "1.0.0");
  REQUIRE(plugin_info.plugin_id == 0);

  // plan_optimization should return empty plan
  CacheKey key("null-opt-test");
  std::vector<std::byte> header_data = {std::byte{0x01}};
  auto plan =
      plugin.plan_optimization(key, std::span<const std::byte>(header_data),
                               1024, AlternateId::Original, 5);
  REQUIRE(plan.empty());

  // transform should return error (should never be called in practice)
  std::atomic<bool> cancelled{false};
  OptimizationContext ctx(key, std::span<const std::byte>(header_data),
                          std::span<const std::byte>(header_data), 0, nullptr,
                          cancelled);
  auto result = plugin.transform(AlternateId::Gzip, ctx);
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error() == CacheError::PluginError);
}

// ============================================================================
// Quick-win: AlternateSelector default implementations
// ============================================================================

TEST_CASE("AlternateSelector default generate_key with hostname",
          "[plugin][alternate]") {
  class TestSelector : public AlternateSelector {
   public:
    std::optional<size_t> select_variant(const VariantCollection &variants,
                                         const LookupContext &) override {
      return variants.empty() ? std::nullopt : std::optional<size_t>(0);
    }
  };

  TestSelector selector;

  // With hostname
  KeyContext ctx_with_host;
  ctx_with_host.url = "/path/resource";
  ctx_with_host.hostname = "example.com";
  auto key1 = selector.generate_key(ctx_with_host);
  REQUIRE_FALSE(key1.is_zero());

  // Without hostname
  KeyContext ctx_no_host;
  ctx_no_host.url = "http://example.com/path";
  ctx_no_host.hostname = "";
  auto key2 = selector.generate_key(ctx_no_host);
  REQUIRE_FALSE(key2.is_zero());

  // Keys should differ (different input data)
  REQUIRE(key1 != key2);
}

TEST_CASE("AlternateSelector default check_freshness returns Fresh",
          "[plugin][alternate]") {
  SimpleAlternateSelector selector;
  CacheVariant variant;
  FreshnessContext ctx;
  REQUIRE(selector.check_freshness(variant, ctx) == FreshnessResult::Fresh);
}

TEST_CASE("AlternateSelector default eviction_priority returns 1.0",
          "[plugin][alternate]") {
  SimpleAlternateSelector selector;
  CacheVariant variant;
  REQUIRE(selector.eviction_priority(variant) == 1.0);
}

TEST_CASE("VariantCollection mutable access", "[plugin][alternate]") {
  VariantCollection vc;
  CacheVariant v;
  v.hit_count = 5;
  vc.add(std::move(v));

  // Mutable access via operator[]
  vc[0].hit_count = 42;
  REQUIRE(vc[0].hit_count == 42);

  // Mutable iterator access
  for (auto &variant : vc) {
    variant.hit_count = 99;
  }
  REQUIRE(vc[0].hit_count == 99);
}

TEST_CASE("CachePlugin default generate_key", "[plugin]") {
  class MinimalPlugin : public CachePlugin {
   public:
    [[nodiscard]] PluginInfo info() const override {
      return {"minimal", "1.0", 1};
    }
    std::optional<size_t> select_variant(const VariantCollection &variants,
                                         const LookupContext &) override {
      return variants.empty() ? std::nullopt : std::optional<size_t>(0);
    }
  };

  MinimalPlugin plugin;

  // With hostname
  KeyContext ctx_with_host;
  ctx_with_host.url = "/page";
  ctx_with_host.hostname = "host.test";
  auto key1 = plugin.generate_key(ctx_with_host);
  REQUIRE_FALSE(key1.is_zero());

  // Without hostname
  KeyContext ctx_no_host;
  ctx_no_host.url = "http://host.test/page";
  ctx_no_host.hostname = "";
  auto key2 = plugin.generate_key(ctx_no_host);
  REQUIRE_FALSE(key2.is_zero());
}
