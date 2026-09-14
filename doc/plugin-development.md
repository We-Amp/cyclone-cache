# Plugin Development Guide

This guide explains how to create custom plugins for Cyclone Cache.

## Overview

Plugins allow you to customize cache behavior in several ways:

1. **Key Generation**: Control how cache keys are computed from requests
2. **Variant Selection**: Choose the best cached variant for a request
3. **Freshness Checking**: Determine if a cached entry is still valid
4. **Eviction Priority**: Influence which entries are evicted first

## Creating a Basic Plugin

### Step 1: Inherit from CachePlugin

```cpp
#include "cyclone/plugin/plugin.hpp"

class MyPlugin : public cyclone::CachePlugin {
public:
    cyclone::PluginInfo info() const override {
        return {
            "my-plugin",    // name
            "1.0.0",        // version
            1000            // unique plugin_id
        };
    }

    std::optional<size_t> select_variant(
        const cyclone::VariantCollection& variants,
        const cyclone::LookupContext& ctx) override
    {
        // Required: implement variant selection
        if (variants.empty()) {
            return std::nullopt;
        }
        return 0;  // Return first variant
    }
};
```

### Step 2: Register the Plugin

```cpp
auto plugin = std::make_shared<MyPlugin>();
cache->plugin_manager().register_plugin(plugin);

// If this plugin handles variant selection:
cache->plugin_manager().set_alternate_selector(plugin);
```

## Plugin Methods

### info()

Required. Returns plugin identification.

```cpp
PluginInfo info() const override {
    return {"plugin-name", "1.0.0", unique_id};
}
```

- `name`: Human-readable name for logging
- `version`: Version string
- `plugin_id`: Unique uint32_t identifier (use values > 100 for custom plugins)

### generate_key()

Optional. Customizes cache key generation.

```cpp
CacheKey generate_key(const KeyContext& ctx) override {
    // Example: Include query parameters in key
    std::string key_string = std::string(ctx.hostname);
    key_string += ctx.url;
    return CacheKey(key_string);
}
```

**KeyContext fields:**
- `url`: Request URL
- `hostname`: Request hostname
- `request_headers`: Raw request headers
- `user_data`: Custom data pointer

### select_variant()

Required. Chooses the best cached variant for a request.

```cpp
std::optional<size_t> select_variant(
    const VariantCollection& variants,
    const LookupContext& ctx) override
{
    // Return index of best variant, or nullopt if none match
    for (size_t i = 0; i < variants.size(); ++i) {
        if (is_match(variants[i], ctx)) {
            return i;
        }
    }
    return std::nullopt;
}
```

**LookupContext fields:**
- `request_headers`: Raw request headers
- `user_data`: Custom data pointer

**CacheVariant fields:**
- `key`: Cache key
- `metadata`: Plugin-specific metadata
- `content_length`: Size of cached content
- `created`: Creation timestamp
- `last_accessed`: Last access timestamp
- `hit_count`: Number of times accessed

### check_freshness()

Optional. Determines if a cached entry is still valid.

```cpp
FreshnessResult check_freshness(
    const CacheVariant& variant,
    const FreshnessContext& ctx) override
{
    if (ctx.force_revalidate) {
        return FreshnessResult::MustRevalidate;
    }

    auto age = ctx.now - variant.created;
    if (age > std::chrono::hours(24)) {
        return FreshnessResult::Stale;
    }

    return FreshnessResult::Fresh;
}
```

**FreshnessResult values:**
- `Fresh`: Entry is valid, serve directly
- `Stale`: Entry is stale but can be served (while revalidating)
- `MustRevalidate`: Must check with origin before serving
- `Error`: Error checking freshness

### eviction_priority()

Optional. Returns a priority value for eviction decisions.

```cpp
double eviction_priority(const CacheVariant& variant) override {
    // Higher values = more likely to evict

    double priority = 1.0;

    // Older entries get higher priority
    auto age = std::chrono::system_clock::now() - variant.last_accessed;
    auto age_hours = std::chrono::duration_cast<std::chrono::hours>(age).count();
    priority += age_hours * 0.1;

    // Frequently accessed entries get lower priority
    if (variant.hit_count > 10) {
        priority *= 0.5;
    }

    return priority;
}
```

## Storing Metadata

Plugins can store custom metadata with cached entries.

### Defining Metadata

```cpp
struct MyMetadata {
    uint32_t priority;
    std::string content_type;
    std::time_t expires;

    std::vector<std::byte> serialize() const {
        std::vector<std::byte> data;
        // Serialize fields...
        return data;
    }

    static MyMetadata deserialize(std::span<const std::byte> data) {
        MyMetadata meta;
        // Deserialize fields...
        return meta;
    }
};
```

### Storing Metadata on Write

```cpp
// When writing to cache
MyMetadata meta;
meta.priority = 5;
meta.content_type = "text/html";
meta.expires = std::time(nullptr) + 3600;

auto serialized = meta.serialize();
write_handle.set_header(std::span<const std::byte>(serialized));
```

### Reading Metadata

```cpp
std::optional<size_t> select_variant(
    const VariantCollection& variants,
    const LookupContext& ctx) override
{
    for (size_t i = 0; i < variants.size(); ++i) {
        auto& variant = variants[i];

        // Deserialize our metadata
        auto meta = MyMetadata::deserialize(variant.metadata.data());

        if (meta.content_type == get_accept_type(ctx)) {
            return i;
        }
    }
    return std::nullopt;
}
```

## Example: Time-Based Expiry Plugin

```cpp
class ExpiryPlugin : public CachePlugin {
public:
    static constexpr uint32_t kPluginId = 1001;

    PluginInfo info() const override {
        return {"expiry-plugin", "1.0.0", kPluginId};
    }

    std::optional<size_t> select_variant(
        const VariantCollection& variants,
        const LookupContext& ctx) override
    {
        (void)ctx;
        if (variants.empty()) {
            return std::nullopt;
        }
        // Select first non-expired variant
        auto now = std::chrono::system_clock::now();
        for (size_t i = 0; i < variants.size(); ++i) {
            if (!is_expired(variants[i], now)) {
                return i;
            }
        }
        return std::nullopt;
    }

    FreshnessResult check_freshness(
        const CacheVariant& variant,
        const FreshnessContext& ctx) override
    {
        if (ctx.force_revalidate) {
            return FreshnessResult::MustRevalidate;
        }

        if (is_expired(variant, ctx.now)) {
            return FreshnessResult::Stale;
        }

        return FreshnessResult::Fresh;
    }

private:
    bool is_expired(const CacheVariant& variant,
                    std::chrono::system_clock::time_point now) const
    {
        auto age = now - variant.created;
        return age > _default_ttl;
    }

    std::chrono::seconds _default_ttl{3600};  // 1 hour
};
```

## Example: Content-Type Matching Plugin

```cpp
class ContentTypePlugin : public CachePlugin {
public:
    static constexpr uint32_t kPluginId = 1002;

    PluginInfo info() const override {
        return {"content-type-plugin", "1.0.0", kPluginId};
    }

    std::optional<size_t> select_variant(
        const VariantCollection& variants,
        const LookupContext& ctx) override
    {
        auto accept = parse_accept_header(ctx.request_headers);

        size_t best_index = 0;
        float best_quality = 0.0f;

        for (size_t i = 0; i < variants.size(); ++i) {
            auto content_type = get_content_type(variants[i]);
            float quality = calculate_quality(accept, content_type);

            if (quality > best_quality) {
                best_quality = quality;
                best_index = i;
            }
        }

        if (best_quality > 0.0f) {
            return best_index;
        }
        return std::nullopt;
    }

private:
    std::string_view parse_accept_header(std::span<const std::byte> headers) {
        // Parse Accept header from request...
        return {};
    }

    std::string_view get_content_type(const CacheVariant& variant) {
        // Extract Content-Type from variant metadata...
        return {};
    }

    float calculate_quality(std::string_view accept,
                           std::string_view content_type) {
        // RFC 7231 quality value calculation...
        return 1.0f;
    }
};
```

## Best Practices

### 1. Use Unique Plugin IDs

Reserve plugin IDs to avoid conflicts:
- 0: Core cache (reserved)
- 1: HTTP plugin (built-in)
- 2-99: Reserved for future built-in plugins
- 100+: Custom plugins

### 2. Handle Empty Collections

Always check for empty variant collections:

```cpp
if (variants.empty()) {
    return std::nullopt;
}
```

### 3. Efficient Metadata Serialization

Use compact binary formats for metadata:

```cpp
// Good: Fixed-size binary format
struct CompactMeta {
    uint32_t flags;
    int64_t timestamp;
};

// Avoid: String-based formats for frequently accessed data
```

### 4. Thread Safety

Plugin methods may be called from multiple threads. Ensure thread-safe access to shared state:

```cpp
class ThreadSafePlugin : public CachePlugin {
    mutable std::shared_mutex _mutex;
    std::map<std::string, int> _stats;

    void record_hit(const std::string& key) {
        std::unique_lock lock(_mutex);
        _stats[key]++;
    }
};
```

### 5. Fail Gracefully

Handle errors without throwing exceptions:

```cpp
std::optional<size_t> select_variant(...) override {
    try {
        // Plugin logic...
    } catch (...) {
        // Log error, return safe default
        return variants.empty() ? std::nullopt : std::optional{0};
    }
}
```

## Testing Plugins

### Unit Testing

```cpp
#include <catch2/catch_test_macros.hpp>

TEST_CASE("MyPlugin selects correct variant") {
    MyPlugin plugin;

    VariantCollection variants;
    // Add test variants...

    LookupContext ctx;
    // Set up context...

    auto result = plugin.select_variant(variants, ctx);

    REQUIRE(result.has_value());
    REQUIRE(*result == expected_index);
}
```

### Integration Testing

```cpp
TEST_CASE("Plugin integration with cache") {
    CacheConfig config;
    auto cache = Cache::create(config).value();

    auto plugin = std::make_shared<MyPlugin>();
    cache->plugin_manager().set_alternate_selector(plugin);

    // Write and read with plugin active...
}
```
