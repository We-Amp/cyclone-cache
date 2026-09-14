// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cyclone/key.hpp"
#include "cyclone/plugin/metadata.hpp"

namespace cyclone {

struct CacheVariant {
  CacheKey key;
  Metadata metadata;
  uint64_t content_length = 0;
  std::chrono::system_clock::time_point created;
  std::chrono::system_clock::time_point last_accessed;
  uint32_t hit_count = 0;
};

class VariantCollection {
 public:
  [[nodiscard]] size_t size() const { return _variants.size(); }
  [[nodiscard]] bool empty() const { return _variants.empty(); }

  const CacheVariant &operator[](size_t index) const {
    return _variants[index];
  }
  CacheVariant &operator[](size_t index) { return _variants[index]; }

  void add(CacheVariant variant) { _variants.push_back(std::move(variant)); }
  void clear() { _variants.clear(); }

  auto begin() { return _variants.begin(); }
  auto end() { return _variants.end(); }
  [[nodiscard]] auto begin() const { return _variants.begin(); }
  [[nodiscard]] auto end() const { return _variants.end(); }

 private:
  std::vector<CacheVariant> _variants;
};

struct KeyContext {
  std::string_view url;
  std::string_view hostname;
  std::span<const std::byte> request_headers;
  void *user_data = nullptr;
};

struct LookupContext {
  std::span<const std::byte> request_headers;
  void *user_data = nullptr;
};

struct FreshnessContext {
  std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
  bool force_revalidate = false;
  void *user_data = nullptr;
};

enum class FreshnessResult : std::uint8_t {
  Fresh,
  Stale,
  MustRevalidate,
  Error
};

class AlternateSelector {
 public:
  virtual ~AlternateSelector() = default;

  virtual CacheKey generate_key(const KeyContext &ctx) {
    if (!ctx.hostname.empty()) {
      return CacheKey::from_url(ctx.url, ctx.hostname);
    }
    return CacheKey::from_url(ctx.url);
  }

  virtual std::optional<size_t> select_variant(
      const VariantCollection &variants, const LookupContext &ctx) = 0;

  virtual FreshnessResult check_freshness(const CacheVariant &variant,
                                          const FreshnessContext &ctx) {
    (void)variant;
    (void)ctx;
    return FreshnessResult::Fresh;
  }

  virtual double eviction_priority(const CacheVariant &variant) {
    (void)variant;
    return 1.0;
  }
};

class SimpleAlternateSelector : public AlternateSelector {
 public:
  std::optional<size_t> select_variant(const VariantCollection &variants,
                                       const LookupContext &ctx) override {
    (void)ctx;
    if (variants.empty()) {
      return std::nullopt;
    }
    return 0;
  }
};

}  // namespace cyclone
