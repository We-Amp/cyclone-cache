// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#pragma once

/// @file optimization.hpp
/// @brief Background optimization plugin interface for Cyclone Cache.
///
/// This header defines the OptimizationPlugin interface which allows plugins to
/// automatically generate optimized alternates (e.g., compressed versions,
/// transcoded images) in the background after content is written to the cache.
///
/// ## Thread Safety
///
/// The optimization system is fully concurrent:
/// - `plan_optimization()` is called from the write path, potentially from
/// multiple threads
/// - `transform()` is called from worker threads in the adaptive thread pool
/// - Multiple `transform()` calls may run concurrently for different keys
/// - Plugins must ensure their implementations are thread-safe
///
/// ## Exception Safety
///
/// - `plan_optimization()` should not throw; exceptions will be caught and
/// logged
/// - `transform()` should not throw; use std::expected for error reporting
/// - If exceptions escape, the work item is marked as failed
///
/// ## Cancellation
///
/// Long-running transformations should periodically check `ctx.is_cancelled()`
/// and return early with `CacheError::OptimizationCancelled` if true. This
/// allows:
/// - Graceful shutdown of the cache
/// - Load shedding during high-load periods
/// - Explicit cancellation of work items
///
/// ## Example
///
/// @code
/// class MyCompressionPlugin : public OptimizationPlugin {
/// public:
///   PluginInfo info() const override {
///     return {"my-compressor", "1.0.0", 42};
///   }
///
///   OptimizationPlan plan_optimization(const CacheKey& key, ...) override {
///     OptimizationPlan plan;
///     if (should_compress(header)) {
///       plan.add(AlternateId::Gzip, 10, true, content_length);
///     }
///     return plan;
///   }
///
///   std::expected<TransformResult, CacheError> transform(...) override {
///     while (processing) {
///       if (ctx.is_cancelled()) {
///         return std::unexpected(CacheError::OptimizationCancelled);
///       }
///       // ... do work ...
///     }
///     return TransformResult{...};
///   }
/// };
/// @endcode

#include <atomic>
#include <cstdint>
#include <expected>
#include <functional>
#include <span>
#include <vector>

#include "cyclone/alternate.hpp"
#include "cyclone/error.hpp"
#include "cyclone/key.hpp"
#include "cyclone/plugin/plugin.hpp"

namespace cyclone {

class Cache;

// Result of a transformation operation
struct TransformResult {
  std::vector<std::byte> header;   // New header for the alternate
  std::vector<std::byte> content;  // Transformed content
  AlternateId alternate_id;        // ID of the created alternate
};

// Context provided to plugins during transformation
class OptimizationContext {
 public:
  OptimizationContext(const CacheKey &key,
                      std::span<const std::byte> source_header,
                      std::span<const std::byte> source_content,
                      uint32_t hit_count, Cache *cache,
                      std::atomic<bool> &cancelled);

  // Source data
  [[nodiscard]] const CacheKey &key() const { return _key; }
  [[nodiscard]] std::span<const std::byte> source_header() const {
    return _source_header;
  }
  [[nodiscard]] std::span<const std::byte> source_content() const {
    return _source_content;
  }
  [[nodiscard]] uint32_t hit_count() const { return _hit_count; }

  // Access to cache for reading related data if needed
  [[nodiscard]] Cache *cache() const { return _cache; }

  // Cancellation support - plugins should check periodically
  [[nodiscard]] bool is_cancelled() const {
    return _cancelled.load(std::memory_order_acquire);
  }

 private:
  const CacheKey &_key;
  std::span<const std::byte> _source_header;
  std::span<const std::byte> _source_content;
  uint32_t _hit_count;
  Cache *_cache;
  std::atomic<bool> &_cancelled;
};

// Plan returned by plugins describing what optimizations to perform
struct OptimizationTarget {
  AlternateId target_alternate;
  uint32_t priority = 0;  // Higher = process first
  bool deferrable = true;
  size_t estimated_memory = 0;
};

struct OptimizationPlan {
  std::vector<OptimizationTarget> targets;

  // Helper to add a target
  OptimizationPlan &add(AlternateId alt, uint32_t priority = 0,
                        bool deferrable = true, size_t mem = 0) {
    targets.push_back({alt, priority, deferrable, mem});
    return *this;
  }

  [[nodiscard]] bool empty() const { return targets.empty(); }
};

/// @class OptimizationPlugin
/// @brief Abstract interface for background optimization plugins.
///
/// Plugins implement this interface to provide custom optimization logic.
/// The optimization engine calls `plan_optimization()` after writes complete,
/// then schedules `transform()` calls on worker threads.
///
/// @note All methods must be thread-safe. Multiple threads may call methods
/// concurrently.
/// @note Implementations should not throw exceptions; use std::expected for
/// errors.
class OptimizationPlugin {
 public:
  virtual ~OptimizationPlugin() = default;

  // Plugin identification
  [[nodiscard]] virtual PluginInfo info() const = 0;

  // Determine what optimizations to queue after a write completes.
  // Called for every write - should be fast and non-blocking.
  //
  // Parameters:
  //   key: The cache key that was written
  //   header: The header data (may contain content-type, etc.)
  //   content_length: Size of the content
  //   written_alternate: Which alternate was written (usually Original)
  //   hit_count: Current hit count for this key
  //
  // Return empty plan to skip optimization for this write.
  virtual OptimizationPlan plan_optimization(const CacheKey &key,
                                             std::span<const std::byte> header,
                                             uint64_t content_length,
                                             AlternateId written_alternate,
                                             uint32_t hit_count) = 0;

  // Perform the actual transformation. Called from worker thread.
  //
  // IMPORTANT: This method may be long-running. Implementations MUST:
  // - Check ctx.is_cancelled() periodically and return early if true
  // - Not hold locks for extended periods
  // - Be thread-safe (may be called concurrently)
  //
  // Parameters:
  //   target_alternate: Which alternate to create
  //   ctx: Context with source data and cancellation support
  //
  // Returns:
  //   TransformResult on success
  //   CacheError on failure (OptimizationCancelled if cancelled)
  virtual std::expected<TransformResult, CacheError> transform(
      AlternateId target_alternate, const OptimizationContext &ctx) = 0;

  // Called when a queued work item is cancelled.
  // Optional - override to clean up resources or log.
  virtual void on_cancelled(const CacheKey &key, AlternateId target) {
    (void)key;
    (void)target;
  }

  // Estimate memory needed for transformation.
  // Used for resource tracking. Default assumes 2x content size.
  virtual size_t estimate_memory(uint64_t content_length,
                                 AlternateId /*target*/) {
    return content_length * 2;
  }
};

// Simple plugin that doesn't optimize anything (useful as base class or for
// testing)
class NullOptimizationPlugin : public OptimizationPlugin {
 public:
  [[nodiscard]] PluginInfo info() const override {
    return {"null-optimization", "1.0.0", 0};
  }

  OptimizationPlan plan_optimization(const CacheKey & /*key*/,
                                     std::span<const std::byte> /*header*/,
                                     uint64_t /*content_length*/,
                                     AlternateId /*written_alternate*/,
                                     uint32_t /*hit_count*/) override {
    return {};  // Never optimize
  }

  std::expected<TransformResult, CacheError> transform(
      AlternateId /*target_alternate*/,
      const OptimizationContext & /*ctx*/) override {
    return std::unexpected(CacheError::PluginError);  // Should never be called
  }
};

}  // namespace cyclone
