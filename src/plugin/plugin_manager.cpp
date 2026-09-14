// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include <mutex>
#include <unordered_map>

#include "cyclone/plugin/plugin.hpp"

namespace cyclone {

struct PluginManager::Impl {
  std::unordered_map<uint32_t, std::shared_ptr<CachePlugin>> plugins;
  std::shared_ptr<CachePlugin> alternate_selector;
  mutable std::mutex mutex;
};

PluginManager::PluginManager() : _impl(std::make_unique<Impl>()) {}

PluginManager::~PluginManager() = default;

void PluginManager::register_plugin(std::shared_ptr<CachePlugin> plugin) {
  if (!plugin) {
    return;
  }

  std::lock_guard<std::mutex> lock(_impl->mutex);
  auto info = plugin->info();
  _impl->plugins[info.plugin_id] = std::move(plugin);
}

void PluginManager::unregister_plugin(uint32_t plugin_id) {
  std::lock_guard<std::mutex> lock(_impl->mutex);
  _impl->plugins.erase(plugin_id);
}

std::shared_ptr<CachePlugin> PluginManager::get_plugin(
    uint32_t plugin_id) const {
  std::lock_guard<std::mutex> lock(_impl->mutex);
  auto it = _impl->plugins.find(plugin_id);
  if (it != _impl->plugins.end()) {
    return it->second;
  }
  return nullptr;
}

std::shared_ptr<CachePlugin> PluginManager::get_alternate_selector() const {
  std::lock_guard<std::mutex> lock(_impl->mutex);
  return _impl->alternate_selector;
}

void PluginManager::set_alternate_selector(
    std::shared_ptr<CachePlugin> plugin) {
  std::lock_guard<std::mutex> lock(_impl->mutex);
  _impl->alternate_selector = std::move(plugin);
}

}  // namespace cyclone
