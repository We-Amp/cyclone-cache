// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "cyclone/key.hpp"

#include <cstring>
#include <iomanip>
#include <sstream>

// The OpenSSL backend hashes with the one-context SHA256_Init / _Update /
// _Final functions, not EVP.  They work on a caller-owned context and reach
// no state of the crypto library that it tears down at exit.  EVP does:
// OpenSSL 3 resolves the digest through its default library context on every
// EVP_DigestInit_ex, and frees that context in an exit handler
// (OPENSSL_cleanup) it registers on first use.  A process that exits while
// its own threads are still in cache calls (which Cyclone permits, see
// "Process exit with open caches" in doc/architecture.md) then faulted in
// the digest lookup, or, once the cleanup had finished, got a failed hash
// back for every key.  The functions are deprecated in OpenSSL 3 in favour
// of EVP, for reasons (provider selection) that do not apply to a cache key;
// OPENSSL_SUPPRESS_DEPRECATED keeps the declarations quiet.  A crypto library
// built without them falls back to the bundled implementation: the digest is
// the same either way.
#ifndef CYCLONE_USE_BUNDLED_SHA256
#ifndef OPENSSL_SUPPRESS_DEPRECATED
#define OPENSSL_SUPPRESS_DEPRECATED
#endif
#include <openssl/sha.h>
#ifdef OPENSSL_NO_DEPRECATED_3_0
#define CYCLONE_SHA256_BUNDLED_FALLBACK
#endif
#endif

#if defined(CYCLONE_USE_BUNDLED_SHA256) || \
    defined(CYCLONE_SHA256_BUNDLED_FALLBACK)
#include "sha256.hpp"
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
#if defined(CYCLONE_USE_BUNDLED_SHA256) || \
    defined(CYCLONE_SHA256_BUNDLED_FALLBACK)
  auto digest = crypto::SHA256::hash(data);
  std::memcpy(_hash.data(), digest.data(), kDigestSize);
#else
  static_assert(kDigestSize == SHA256_DIGEST_LENGTH);
  // No allocation and no failure path: the context lives on this stack.
  SHA256_CTX ctx;
  SHA256_Init(&ctx);
  SHA256_Update(&ctx, data.data(), data.size());
  SHA256_Final(reinterpret_cast<unsigned char *>(_hash.data()), &ctx);
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
