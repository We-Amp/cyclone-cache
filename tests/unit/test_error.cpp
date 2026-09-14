// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include <catch2/catch_test_macros.hpp>

#include "cyclone/error.hpp"

using namespace cyclone;

TEST_CASE("CacheError message strings", "[error]") {
  const auto &cat = cache_error_category();

  REQUIRE(cat.message(static_cast<int>(CacheError::Success)) == "success");
  REQUIRE(cat.message(static_cast<int>(CacheError::NotFound)) ==
          "cache entry not found");
  REQUIRE(cat.message(static_cast<int>(CacheError::Exists)) ==
          "cache entry already exists");
  REQUIRE(cat.message(static_cast<int>(CacheError::NoSpace)) ==
          "no space available");
  REQUIRE(cat.message(static_cast<int>(CacheError::IoError)) == "I/O error");
  REQUIRE(cat.message(static_cast<int>(CacheError::Corrupted)) ==
          "cache data corrupted");
  REQUIRE(cat.message(static_cast<int>(CacheError::InvalidKey)) ==
          "invalid cache key");
  REQUIRE(cat.message(static_cast<int>(CacheError::InvalidArgument)) ==
          "invalid argument");
  REQUIRE(cat.message(static_cast<int>(CacheError::NotInitialized)) ==
          "cache not initialized");
  REQUIRE(cat.message(static_cast<int>(CacheError::AlreadyOpen)) ==
          "handle already open");
  REQUIRE(cat.message(static_cast<int>(CacheError::Closed)) == "handle closed");
  REQUIRE(cat.message(static_cast<int>(CacheError::Busy)) == "resource busy");
  REQUIRE(cat.message(static_cast<int>(CacheError::Timeout)) ==
          "operation timed out");
  REQUIRE(cat.message(static_cast<int>(CacheError::PluginError)) ==
          "plugin error");
  REQUIRE(cat.message(static_cast<int>(CacheError::InternalError)) ==
          "internal error");
  REQUIRE(cat.message(static_cast<int>(CacheError::TooManyAlternates)) ==
          "too many alternates for key");
  REQUIRE(cat.message(static_cast<int>(CacheError::AlternateNotFound)) ==
          "alternate not found");
  REQUIRE(cat.message(static_cast<int>(CacheError::ChainCorrupted)) ==
          "alternate chain corrupted");
  REQUIRE(cat.message(static_cast<int>(CacheError::IncompatibleVersion)) ==
          "cache format version incompatible");
  REQUIRE(cat.message(static_cast<int>(CacheError::OptimizationQueueFull)) ==
          "optimization work queue at capacity");
  REQUIRE(cat.message(static_cast<int>(CacheError::OptimizationCancelled)) ==
          "optimization work item was cancelled");
  REQUIRE(cat.message(static_cast<int>(CacheError::TransformFailed)) ==
          "plugin transform operation failed");
  REQUIRE(cat.message(static_cast<int>(CacheError::NotOwned)) ==
          "stripe not owned by this process");
  REQUIRE(cat.message(static_cast<int>(CacheError::InvalidConfiguration)) ==
          "invalid configuration");
  REQUIRE(cat.message(static_cast<int>(CacheError::ResetRefusedLivePeer)) ==
          "cache reset refused: another process still has this cache open");
  REQUIRE(cat.message(static_cast<int>(CacheError::ObjectTooLarge)) ==
          "object exceeds configured max_object_size");

  // Verify unknown/out-of-range values fall through to default
  REQUIRE(cat.message(100) ==
          "unknown error");  // 100 is not a valid CacheError value
}

TEST_CASE("CacheError make_error_code", "[error]") {
  std::error_code ec = make_error_code(CacheError::NotFound);

  REQUIRE(ec.value() == static_cast<int>(CacheError::NotFound));
  REQUIRE(std::string(ec.category().name()) == "cyclone");
  REQUIRE(ec.message() == "cache entry not found");
}

TEST_CASE("CacheErrorCategory singleton", "[error]") {
  const auto &cat1 = cache_error_category();
  const auto &cat2 = cache_error_category();

  REQUIRE(&cat1 == &cat2);
}

TEST_CASE("CacheError is_error_code_enum", "[error]") {
  // Verify implicit conversion from CacheError to std::error_code compiles
  std::error_code ec = CacheError::InvalidArgument;

  REQUIRE(std::string(ec.category().name()) == "cyclone");
  REQUIRE(ec.value() == static_cast<int>(CacheError::InvalidArgument));
  REQUIRE(ec.message() == "invalid argument");
}
