// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Regression tests for the collision-destructive directory insert: a
// DirEntry holds no key material (only a 12-bit tag), so two distinct keys
// can collide on (stripe, bucket, tag).  The insert path used to treat any
// same-tag current-phase entry as "same key" and update it in place —
// silently destroying the victim's entry, whose reads then key-verify-fail
// into a clean NotFound (hash-deterministic: the same victim loses every
// time).  The fix verifies the stored first_key before electing an in-place
// update; colliding keys now coexist in the bucket, and only a completely
// full bucket evicts the collider (counted in tag_collision_evictions).

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "core/volume.hpp"
#include "cyclone/config.hpp"
#include "cyclone/key.hpp"
#include "support/temp_cache.hpp"

using namespace cyclone;

namespace {

// Buckets per stripe directory, pinned against kDirectoryEntriesPerSegment
// in volume.cpp (TU-local there, so it cannot be static_asserted).  The
// collision search below keys on bucket_hash() % kBuckets; if the
// implementation value shrinks to a divisor of this, the found pairs still
// collide — only an increase would silently weaken these tests.
constexpr uint32_t kBuckets = 16 * 1024;

uint32_t bucket_of(const CacheKey &key) { return key.bucket_hash() % kBuckets; }

// Find two distinct keys that collide on (bucket, 12-bit tag).  Brute-force
// over deterministic key strings; the (bucket, tag) space is ~2^26, so a
// birthday collision lands after ~10k keys.
std::pair<CacheKey, CacheKey> find_colliding_pair() {
  std::map<uint64_t, CacheKey> seen;
  for (uint32_t i = 0;; ++i) {
    CacheKey key("collide-" + std::to_string(i));
    uint64_t slot = (static_cast<uint64_t>(bucket_of(key)) << 12) | key.tag();
    auto [it, inserted] = seen.emplace(slot, key);
    if (!inserted) {
      return {it->second, key};
    }
  }
}

// Find `count` extra keys in the same bucket as `anchor`, with tags distinct
// from the anchor's tag and from each other (bucket fillers).
std::vector<CacheKey> find_bucket_fillers(const CacheKey &anchor,
                                          size_t count) {
  std::vector<CacheKey> fillers;
  std::vector<uint16_t> used_tags{anchor.tag()};
  for (uint32_t i = 0; fillers.size() < count; ++i) {
    CacheKey key("filler-" + std::to_string(i));
    if (bucket_of(key) != bucket_of(anchor)) {
      continue;
    }
    bool tag_taken = false;
    for (uint16_t tag : used_tags) {
      if (key.tag() == tag) {
        tag_taken = true;
        break;
      }
    }
    if (tag_taken) {
      continue;
    }
    used_tags.push_back(key.tag());
    fillers.push_back(key);
  }
  return fillers;
}

// Deterministic per-seed content so reads can be verified byte-for-byte.
std::vector<std::byte> make_content(uint8_t seed, size_t size = 256) {
  std::vector<std::byte> content(size);
  for (size_t i = 0; i < size; ++i) {
    content[i] = static_cast<std::byte>((seed + i) & 0xFF);
  }
  return content;
}

bool write_entry(Volume &volume, const CacheKey &key,
                 std::span<const std::byte> content) {
  auto wh = volume.write_sync(key, content.size());
  if (!wh.has_value()) {
    return false;
  }
  if (!wh->write_sync(content).has_value()) {
    return false;
  }
  return wh->close_sync().has_value();
}

bool read_and_verify(Volume &volume, const CacheKey &key,
                     std::span<const std::byte> expected) {
  auto rh = volume.read_sync(key);
  if (!rh.has_value()) {
    return false;
  }
  auto content = rh->content();
  return content.size() == expected.size() &&
         std::memcmp(content.data(), expected.data(), expected.size()) == 0;
}

bool read_is_not_found(Volume &volume, const CacheKey &key) {
  auto rh = volume.read_sync(key);
  return !rh.has_value() && rh.error() == CacheError::NotFound;
}

// Open an 8MB single-stripe volume (usable size < kMinStripeSize, so every
// key selects the same stripe and the found (bucket, tag) collisions are
// real).  use_mmap toggles the mmap'd (multi-process) directory so both
// Directory and MmapDirectory insert elections are covered.
std::shared_ptr<Volume> open_volume(const std::string &path, bool use_mmap) {
  VolumeConfig config;
  config.path = path;
  config.size = static_cast<size_t>(8 * 1024 * 1024);
  std::shared_ptr<Volume> volume;
  if (use_mmap) {
    MultiProcessConfig mp_config;
    mp_config.enabled = true;
    mp_config.process_index = 0;
    mp_config.total_processes = 1;
    volume = std::make_shared<Volume>(config, mp_config);
  } else {
    volume = std::make_shared<Volume>(config);
  }
  REQUIRE(volume->open().has_value());
  return volume;
}

}  // namespace

TEST_CASE("Tag-colliding keys coexist without destroying the victim",
          "[collision][regression]") {
  const bool use_mmap = GENERATE(false, true);
  const std::string dir_kind = use_mmap ? "mmap" : "in-memory";
  INFO("directory type: " << dir_kind);

  TempCacheDir tmp;
  std::string path = tmp.path();
  auto volume = open_volume(path, use_mmap);

  auto [key_a, key_b] = find_colliding_pair();
  REQUIRE(key_a.tag() == key_b.tag());
  REQUIRE(bucket_of(key_a) == bucket_of(key_b));
  REQUIRE_FALSE(key_a == key_b);

  auto content_a = make_content(0x11);
  auto content_b = make_content(0x22);

  REQUIRE(write_entry(*volume, key_a, content_a));
  REQUIRE(read_and_verify(*volume, key_a, content_a));

  // Insert the colliding key.  Pre-fix this updated A's entry in place —
  // A then read as a clean NotFound while B served from A's slot.
  REQUIRE(write_entry(*volume, key_b, content_b));

  REQUIRE(read_and_verify(*volume, key_a, content_a));
  REQUIRE(read_and_verify(*volume, key_b, content_b));
  REQUIRE(volume->stats().entry_count == 2);
  REQUIRE(volume->stats().tag_collision_evictions == 0);

  // In-place update still works for the RIGHT key: rewriting A must not
  // create a third entry, and must leave B untouched.
  auto content_a2 = make_content(0x33);
  REQUIRE(write_entry(*volume, key_a, content_a2));
  REQUIRE(read_and_verify(*volume, key_a, content_a2));
  REQUIRE(read_and_verify(*volume, key_b, content_b));
  REQUIRE(volume->stats().entry_count == 2);
  REQUIRE(volume->stats().tag_collision_evictions == 0);

  volume->close();
}

TEST_CASE("Remove of a colliding key never deletes the victim",
          "[collision][regression]") {
  const bool use_mmap = GENERATE(false, true);
  const std::string dir_kind = use_mmap ? "mmap" : "in-memory";
  INFO("directory type: " << dir_kind);

  TempCacheDir tmp;
  std::string path = tmp.path();
  auto volume = open_volume(path, use_mmap);

  auto [key_a, key_b] = find_colliding_pair();

  auto content_a = make_content(0x44);
  auto content_b = make_content(0x55);

  REQUIRE(write_entry(*volume, key_a, content_a));
  REQUIRE(write_entry(*volume, key_b, content_b));

  // Removing B must remove exactly B — never the same-tag victim A.
  REQUIRE(volume->remove_sync(key_b).has_value());
  REQUIRE(read_and_verify(*volume, key_a, content_a));
  REQUIRE(read_is_not_found(*volume, key_b));

  // A second remove of B finds nothing (and still leaves A intact).
  auto again = volume->remove_sync(key_b);
  REQUIRE_FALSE(again.has_value());
  REQUIRE(again.error() == CacheError::NotFound);
  REQUIRE(read_and_verify(*volume, key_a, content_a));

  REQUIRE(volume->remove_sync(key_a).has_value());
  REQUIRE(read_is_not_found(*volume, key_a));

  volume->close();
}

TEST_CASE("Full bucket: colliding insert evicts the collider and counts it",
          "[collision][regression]") {
  const bool use_mmap = GENERATE(false, true);
  const std::string dir_kind = use_mmap ? "mmap" : "in-memory";
  INFO("directory type: " << dir_kind);

  TempCacheDir tmp;
  std::string path = tmp.path();
  auto volume = open_volume(path, use_mmap);

  auto [key_a, key_b] = find_colliding_pair();
  // Fill the remaining bucket slots (kEntriesPerBucket == 4) with
  // distinct-tag keys so B's insert finds neither an empty nor a stale slot.
  auto fillers = find_bucket_fillers(key_a, Directory::kEntriesPerBucket - 1);

  auto content_a = make_content(0x66);
  auto content_b = make_content(0x77);

  REQUIRE(write_entry(*volume, key_a, content_a));
  std::vector<std::vector<std::byte>> filler_contents;
  for (size_t i = 0; i < fillers.size(); ++i) {
    filler_contents.push_back(make_content(static_cast<uint8_t>(0x80 + i)));
    REQUIRE(write_entry(*volume, fillers[i], filler_contents[i]));
  }

  // The bucket is now full of current-phase entries.  B collides with A on
  // the tag and cannot coexist — the collider is evicted (the pre-fix
  // outcome for the victim), but detected and counted, never silent.
  REQUIRE(write_entry(*volume, key_b, content_b));
  REQUIRE(volume->stats().tag_collision_evictions == 1);
  REQUIRE(read_and_verify(*volume, key_b, content_b));
  REQUIRE(read_is_not_found(*volume, key_a));

  // Unrelated same-bucket entries are untouched.
  for (size_t i = 0; i < fillers.size(); ++i) {
    REQUIRE(read_and_verify(*volume, fillers[i], filler_contents[i]));
  }

  volume->close();
}
