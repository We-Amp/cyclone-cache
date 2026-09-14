// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#pragma once

// Self-contained SHA-256 (FIPS 180-4) used when building without OpenSSL.
// Independent implementation; no third-party source.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace cyclone::crypto {

class SHA256 {
 public:
  static constexpr size_t DIGEST_SIZE = 32;
  static constexpr size_t BLOCK_SIZE = 64;

  SHA256() noexcept { reset(); }

  void reset() noexcept {
    state_[0] = 0x6a09e667;
    state_[1] = 0xbb67ae85;
    state_[2] = 0x3c6ef372;
    state_[3] = 0xa54ff53a;
    state_[4] = 0x510e527f;
    state_[5] = 0x9b05688c;
    state_[6] = 0x1f83d9ab;
    state_[7] = 0x5be0cd19;
    count_ = 0;
    buffer_len_ = 0;
  }

  void update(std::span<const std::byte> data) noexcept {
    const auto *ptr = reinterpret_cast<const uint8_t *>(data.data());
    size_t len = data.size();

    count_ += len;

    if (buffer_len_ > 0) {
      size_t to_copy = std::min(len, BLOCK_SIZE - buffer_len_);
      std::copy_n(ptr, to_copy, buffer_.data() + buffer_len_);
      buffer_len_ += to_copy;
      ptr += to_copy;
      len -= to_copy;

      if (buffer_len_ == BLOCK_SIZE) {
        transform(buffer_.data());
        buffer_len_ = 0;
      }
    }

    while (len >= BLOCK_SIZE) {
      transform(ptr);
      ptr += BLOCK_SIZE;
      len -= BLOCK_SIZE;
    }

    if (len > 0) {
      std::copy_n(ptr, len, buffer_.data());
      buffer_len_ = len;
    }
  }

  std::array<std::byte, DIGEST_SIZE> finalize() noexcept {
    uint64_t bits = count_ * 8;

    // Pad message
    buffer_[buffer_len_++] = 0x80;
    if (buffer_len_ > 56) {
      while (buffer_len_ < BLOCK_SIZE) {
        buffer_[buffer_len_++] = 0;
      }
      transform(buffer_.data());
      buffer_len_ = 0;
    }
    while (buffer_len_ < 56) {
      buffer_[buffer_len_++] = 0;
    }

    // Append length in bits
    for (int i = 7; i >= 0; --i) {
      buffer_[buffer_len_++] = static_cast<uint8_t>(bits >> (i * 8));
    }
    transform(buffer_.data());

    // Output hash
    std::array<std::byte, DIGEST_SIZE> result;
    for (size_t i = 0; i < 8; ++i) {
      result[i * 4 + 0] = static_cast<std::byte>(state_[i] >> 24);
      result[i * 4 + 1] = static_cast<std::byte>(state_[i] >> 16);
      result[i * 4 + 2] = static_cast<std::byte>(state_[i] >> 8);
      result[i * 4 + 3] = static_cast<std::byte>(state_[i]);
    }
    return result;
  }

  static std::array<std::byte, DIGEST_SIZE> hash(
      std::span<const std::byte> data) noexcept {
    SHA256 ctx;
    ctx.update(data);
    return ctx.finalize();
  }

 private:
  static constexpr uint32_t K[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
      0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
      0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
      0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
      0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
      0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
      0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
      0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
      0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
  };

  static constexpr uint32_t rotr(uint32_t x, unsigned n) noexcept {
    return (x >> n) | (x << (32 - n));
  }

  static constexpr uint32_t ch(uint32_t x, uint32_t y, uint32_t z) noexcept {
    return (x & y) ^ (~x & z);
  }

  static constexpr uint32_t maj(uint32_t x, uint32_t y, uint32_t z) noexcept {
    return (x & y) ^ (x & z) ^ (y & z);
  }

  static constexpr uint32_t sigma0(uint32_t x) noexcept {
    return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22);
  }

  static constexpr uint32_t sigma1(uint32_t x) noexcept {
    return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25);
  }

  static constexpr uint32_t gamma0(uint32_t x) noexcept {
    return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3);
  }

  static constexpr uint32_t gamma1(uint32_t x) noexcept {
    return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10);
  }

  void transform(const uint8_t *block) noexcept {
    uint32_t W[64];

    // Load block into W[0..15]
    for (size_t i = 0; i < 16; ++i) {
      W[i] = (static_cast<uint32_t>(block[i * 4 + 0]) << 24) |
             (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
             (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
             (static_cast<uint32_t>(block[i * 4 + 3]));
    }

    // Extend W[16..63]
    for (size_t i = 16; i < 64; ++i) {
      W[i] = gamma1(W[i - 2]) + W[i - 7] + gamma0(W[i - 15]) + W[i - 16];
    }

    uint32_t a = state_[0];
    uint32_t b = state_[1];
    uint32_t c = state_[2];
    uint32_t d = state_[3];
    uint32_t e = state_[4];
    uint32_t f = state_[5];
    uint32_t g = state_[6];
    uint32_t h = state_[7];

    for (size_t i = 0; i < 64; ++i) {
      uint32_t t1 = h + sigma1(e) + ch(e, f, g) + K[i] + W[i];
      uint32_t t2 = sigma0(a) + maj(a, b, c);
      h = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
  }

  std::array<uint32_t, 8> state_;
  std::array<uint8_t, BLOCK_SIZE> buffer_;
  size_t buffer_len_;
  uint64_t count_;
};

}  // namespace cyclone::crypto
