// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include <catch2/catch_test_macros.hpp>

#include "cyclone/key.hpp"

using namespace cyclone;

TEST_CASE("CacheKey from string", "[key]") {
  CacheKey key("test-url");

  REQUIRE_FALSE(key.is_zero());
  REQUIRE(key.to_hex().size() == 64);
}

TEST_CASE("CacheKey from URL", "[key]") {
  CacheKey key1 = CacheKey::from_url("http://example.com/path");
  CacheKey key2 = CacheKey::from_url("http://example.com/path");
  CacheKey key3 = CacheKey::from_url("http://example.com/other");

  REQUIRE(key1 == key2);
  REQUIRE(key1 != key3);
}

TEST_CASE("CacheKey from URL with hostname", "[key]") {
  CacheKey key1 = CacheKey::from_url("/path", "example.com");
  CacheKey key2 = CacheKey::from_url("/path", "example.com");
  CacheKey key3 = CacheKey::from_url("/path", "other.com");

  REQUIRE(key1 == key2);
  REQUIRE(key1 != key3);
}

TEST_CASE("CacheKey hex round-trip", "[key]") {
  CacheKey original("test-data-for-hex-conversion");
  std::string hex = original.to_hex();

  CacheKey restored = CacheKey::from_hex(hex);

  REQUIRE(original == restored);
}

TEST_CASE("CacheKey hash functions", "[key]") {
  CacheKey key("test-key");

  uint32_t segment = key.segment_hash();
  uint32_t bucket = key.bucket_hash();
  uint16_t tag = key.tag();

  REQUIRE(segment != 0);
  REQUIRE(bucket != 0);
  REQUIRE(tag <= 0x0FFF);
}

TEST_CASE("CacheKey zero detection", "[key]") {
  CacheKey empty;
  CacheKey filled("data");

  REQUIRE(empty.is_zero());
  REQUIRE_FALSE(filled.is_zero());
}

TEST_CASE("CacheKey std::hash", "[key]") {
  CacheKey key1("test");
  CacheKey key2("test");
  CacheKey key3("other");

  std::hash<CacheKey> hasher;

  REQUIRE(hasher(key1) == hasher(key2));
  REQUIRE(hasher(key1) != hasher(key3));
}
