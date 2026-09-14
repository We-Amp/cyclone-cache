// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "write_buffer.hpp"

#include <cstring>
#include <limits>

#include "cyclone/detail/expected_compat.hpp"

namespace cyclone {

WriteBuffer::WriteBuffer(size_t capacity, size_t max_size)
    : _max_size(max_size) {
  _buffer.resize(capacity);
}

std::expected<void, CacheError> WriteBuffer::append(
    std::span<const std::byte> data) {
  // Check for overflow
  if (data.size() > std::numeric_limits<size_t>::max() - _size) {
    return make_unexpected(CacheError::InvalidArgument);
  }

  size_t new_size = _size + data.size();

  // Check against max size limit
  if (new_size > _max_size) {
    return make_unexpected(CacheError::NoSpace);
  }

  if (new_size > _buffer.size()) {
    size_t new_capacity = std::max(_buffer.size() * 2, new_size);
    // Respect max size for capacity too
    if (new_capacity > _max_size) {
      new_capacity = _max_size;
    }
    _buffer.resize(new_capacity);
  }

  if (!data.empty()) {
    std::memcpy(_buffer.data() + _size, data.data(), data.size());
  }
  _size = new_size;

  return {};
}

void WriteBuffer::clear() { _size = 0; }

void WriteBuffer::reserve(size_t capacity) {
  if (capacity > _buffer.size()) {
    _buffer.resize(capacity);
  }
}

}  // namespace cyclone
