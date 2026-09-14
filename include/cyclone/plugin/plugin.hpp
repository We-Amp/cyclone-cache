// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "cyclone/key.hpp"
#include "cyclone/plugin/alternate.hpp"
#include "cyclone/plugin/metadata.hpp"

namespace cyclone {

struct PluginInfo {
  std::string name;
  std::string version;
  uint32_t plugin_id;
};

class CachePlugin {
 public:
  virtual ~CachePlugin() = default;

  [[nodiscard]] virtual PluginInfo info() const = 0;

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

class PluginManager {
 public:
  PluginManager();
  ~PluginManager();

  void register_plugin(std::shared_ptr<CachePlugin> plugin);
  void unregister_plugin(uint32_t plugin_id);

  [[nodiscard]] std::shared_ptr<CachePlugin> get_plugin(
      uint32_t plugin_id) const;
  [[nodiscard]] std::shared_ptr<CachePlugin> get_alternate_selector() const;

  void set_alternate_selector(std::shared_ptr<CachePlugin> plugin);

 private:
  struct Impl;
  std::unique_ptr<Impl> _impl;
};

}  // namespace cyclone
