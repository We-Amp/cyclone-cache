// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "cyclone/key.hpp"

#include <cstring>
#include <iomanip>
#include <sstream>

#ifdef CYCLONE_USE_BUNDLED_SHA256
#include "sha256.hpp"
#else
#include <openssl/evp.h>
#endif

namespace cyclone {

CacheKey::CacheKey(std::span<const std::byte> data) { compute_sha256(data); }

CacheKey::CacheKey(std::string_view s) {
  compute_sha256(std::span<const std::byte>(
      reinterpret_cast<const std::byte *>(s.data()), s.size()));
}

CacheKey CacheKey::from_url(std::string_view url, std::string_view hostname) {
  std::string combined;
  if (!hostname.empty()) {
    combined.reserve(hostname.size() + 1 + url.size());
    combined.append(hostname);
    combined.push_back('/');
    combined.append(url);
    return CacheKey(combined);
  }
  return CacheKey(url);
}

CacheKey CacheKey::from_digest(std::span<const std::byte, kDigestSize> digest) {
  CacheKey key;
  std::memcpy(key._hash.data(), digest.data(), kDigestSize);
  return key;
}

void CacheKey::compute_sha256(std::span<const std::byte> data) {
#ifdef CYCLONE_USE_BUNDLED_SHA256
  auto digest = crypto::SHA256::hash(data);
  std::memcpy(_hash.data(), digest.data(), kDigestSize);
#else
  unsigned int len = 0;
  EVP_MD_CTX *ctx = EVP_MD_CTX_new();
  if (!ctx || !EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) ||
      !EVP_DigestUpdate(ctx, data.data(), data.size()) ||
      !EVP_DigestFinal_ex(ctx, reinterpret_cast<unsigned char *>(_hash.data()),
                          &len)) {
    _hash.fill(std::byte{0});
  }
  EVP_MD_CTX_free(ctx);  // EVP_MD_CTX_free(nullptr) is a no-op
#endif
}

uint32_t CacheKey::segment_hash() const {
  uint32_t result;
  std::memcpy(&result, _hash.data(), sizeof(result));
  return result;
}

uint32_t CacheKey::bucket_hash() const {
  uint32_t result;
  std::memcpy(&result, _hash.data() + sizeof(uint32_t), sizeof(result));
  return result;
}

uint16_t CacheKey::tag() const {
  uint16_t result;
  std::memcpy(&result, _hash.data() + 2 * sizeof(uint32_t), sizeof(result));
  return result & 0x0FFF;  // 12-bit tag
}

std::string CacheKey::to_hex() const {
  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  for (std::byte b : _hash) {
    oss << std::setw(2) << static_cast<int>(static_cast<unsigned char>(b));
  }
  return oss.str();
}

CacheKey CacheKey::from_hex(std::string_view hex) {
  CacheKey key;
  if (hex.size() != kDigestSize * 2) {
    return key;
  }

  for (size_t i = 0; i < kDigestSize; ++i) {
    char high = hex[i * 2];
    char low = hex[i * 2 + 1];

    auto from_hex_char = [](char c) -> uint8_t {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return 10 + c - 'a';
      if (c >= 'A' && c <= 'F') return 10 + c - 'A';
      return 0;
    };

    key._hash[i] = std::byte((from_hex_char(high) << 4) | from_hex_char(low));
  }

  return key;
}

bool CacheKey::is_zero() const {
  return _hash == std::array<std::byte, kDigestSize>{};
}

}  // namespace cyclone
