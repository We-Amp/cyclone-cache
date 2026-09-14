// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#pragma once

#include <cstddef>
#include <expected>
#include <span>
#include <vector>

#include "cyclone/error.hpp"

namespace cyclone {

class WriteBuffer {
 public:
  static constexpr size_t kDefaultMaxSize =
      size_t{256} * 1024 * 1024;  // 256MB max

  explicit WriteBuffer(size_t capacity = size_t{64} * 1024,
                       size_t max_size = kDefaultMaxSize);

  std::expected<void, CacheError> append(std::span<const std::byte> data);
  void clear();

  [[nodiscard]] std::span<const std::byte> data() const {
    return {_buffer.data(), _size};
  }
  [[nodiscard]] size_t size() const { return _size; }
  [[nodiscard]] size_t capacity() const { return _buffer.size(); }
  [[nodiscard]] bool empty() const { return _size == 0; }
  [[nodiscard]] size_t max_size() const { return _max_size; }

  void reserve(size_t capacity);

 private:
  std::vector<std::byte> _buffer;
  size_t _size = 0;
  size_t _max_size = kDefaultMaxSize;
};

}  // namespace cyclone
