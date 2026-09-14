// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace cyclone {

class CacheKey {
 public:
  static constexpr size_t kDigestSize = 32;  // SHA-256

  CacheKey() = default;

  explicit CacheKey(std::span<const std::byte> data);
  explicit CacheKey(std::string_view s);

  static CacheKey from_url(std::string_view url,
                           std::string_view hostname = {});
  static CacheKey from_digest(std::span<const std::byte, kDigestSize> digest);

  [[nodiscard]] uint32_t segment_hash() const;
  [[nodiscard]] uint32_t bucket_hash() const;
  [[nodiscard]] uint16_t tag()
      const;  // 12-bit collision tag (stored in 16-bit)

  [[nodiscard]] std::span<const std::byte, kDigestSize> digest()
      const noexcept {
    return std::span<const std::byte, kDigestSize>(_hash);
  }

  bool operator==(const CacheKey &other) const noexcept {
    return _hash == other._hash;
  }

  [[nodiscard]] std::string to_hex() const;
  static CacheKey from_hex(std::string_view hex);

  [[nodiscard]] bool is_zero() const;

 private:
  std::array<std::byte, kDigestSize> _hash{};

  void compute_sha256(std::span<const std::byte> data);
};

}  // namespace cyclone

namespace std {
template <>
struct hash<cyclone::CacheKey> {
  size_t operator()(const cyclone::CacheKey &key) const noexcept {
    // XOR-fold all 32 bytes of the SHA-256 digest for better distribution
    auto digest = key.digest();
    size_t result = 0;
    for (size_t i = 0; i < cyclone::CacheKey::kDigestSize;
         i += sizeof(size_t)) {
      size_t chunk;
      std::memcpy(&chunk, digest.data() + i, sizeof(chunk));
      result ^= chunk;
    }
    return result;
  }
};
}  // namespace std
