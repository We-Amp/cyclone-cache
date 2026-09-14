// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace cyclone {

class Metadata {
 public:
  Metadata() = default;

  explicit Metadata(std::vector<std::byte> data) : _data(std::move(data)) {}

  explicit Metadata(std::span<const std::byte> data)
      : _data(data.begin(), data.end()) {}

  [[nodiscard]] std::span<const std::byte> data() const { return _data; }

  std::span<std::byte> mutable_data() { return _data; }

  [[nodiscard]] size_t size() const { return _data.size(); }

  [[nodiscard]] bool empty() const { return _data.empty(); }

  void resize(size_t size) { _data.resize(size); }

  void clear() { _data.clear(); }

  void assign(std::span<const std::byte> data) {
    _data.assign(data.begin(), data.end());
  }

 private:
  std::vector<std::byte> _data;
};

class MetadataCollection {
 public:
  static constexpr uint32_t kCorePluginId = 0;

  void set(uint32_t plugin_id, Metadata data) {
    _sections[plugin_id] = std::move(data);
  }

  [[nodiscard]] std::optional<Metadata> get(uint32_t plugin_id) const {
    auto it = _sections.find(plugin_id);
    if (it != _sections.end()) {
      return it->second;
    }
    return std::nullopt;
  }

  [[nodiscard]] bool has(uint32_t plugin_id) const {
    return _sections.find(plugin_id) != _sections.end();
  }

  void remove(uint32_t plugin_id) { _sections.erase(plugin_id); }

  void clear() { _sections.clear(); }

  [[nodiscard]] bool empty() const { return _sections.empty(); }

  [[nodiscard]] size_t count() const { return _sections.size(); }

  [[nodiscard]] std::vector<std::byte> serialize() const {
    std::vector<std::byte> result;

    auto count = static_cast<uint32_t>(_sections.size());
    result.resize(sizeof(count));
    std::memcpy(result.data(), &count, sizeof(count));

    for (const auto &[plugin_id, metadata] : _sections) {
      size_t offset = result.size();
      result.resize(offset + sizeof(plugin_id) + sizeof(uint32_t) +
                    metadata.size());

      std::memcpy(result.data() + offset, &plugin_id, sizeof(plugin_id));
      offset += sizeof(plugin_id);

      auto len = static_cast<uint32_t>(metadata.size());
      std::memcpy(result.data() + offset, &len, sizeof(len));
      offset += sizeof(len);

      std::memcpy(result.data() + offset, metadata.data().data(),
                  metadata.size());
    }

    return result;
  }

  static constexpr uint32_t kMaxSectionCount = 1024;
  static constexpr uint32_t kMaxSectionSize =
      16 * 1024 * 1024;  // 16MB per section

  static MetadataCollection deserialize(std::span<const std::byte> data) {
    MetadataCollection result;

    if (data.size() < sizeof(uint32_t)) {
      return result;
    }

    uint32_t count;
    std::memcpy(&count, data.data(), sizeof(count));
    size_t offset = sizeof(count);

    // Limit count to prevent excessive iterations from malicious data
    if (count > kMaxSectionCount) {
      return result;
    }

    for (uint32_t i = 0; i < count; ++i) {
      // Check we have enough data for plugin_id and len fields
      if (offset > data.size() || data.size() - offset < sizeof(uint32_t) * 2) {
        break;
      }

      uint32_t plugin_id;
      std::memcpy(&plugin_id, data.data() + offset, sizeof(plugin_id));
      offset += sizeof(plugin_id);

      uint32_t len;
      std::memcpy(&len, data.data() + offset, sizeof(len));
      offset += sizeof(len);

      // Validate len is within reasonable bounds and doesn't overflow
      if (len > kMaxSectionSize) {
        break;
      }

      if (offset > data.size() || len > data.size() - offset) {
        break;
      }

      result.set(plugin_id, Metadata(data.subspan(offset, len)));
      offset += len;
    }

    return result;
  }

 private:
  std::map<uint32_t, Metadata> _sections;
};

}  // namespace cyclone
