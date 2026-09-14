// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "../../src/core/directory.hpp"
#include "../../src/core/mmap_directory.hpp"

using namespace cyclone;

TEST_CASE("DirEntry size is 10 bytes", "[directory]") {
  REQUIRE(sizeof(DirEntry) == 10);
  REQUIRE(DirEntry::kSize == 10);
}

TEST_CASE("DirEntry offset operations", "[directory]") {
  DirEntry entry;
  entry.clear();

  entry.set_offset(0);
  REQUIRE(entry.offset() == 0);

  entry.set_offset(0x12345678);
  REQUIRE(entry.offset() == 0x12345678);

  entry.set_offset(0xFFFFFFFFFF);
  REQUIRE(entry.offset() == 0xFFFFFFFFFF);
}

TEST_CASE("DirEntry size encoding", "[directory]") {
  DirEntry entry;
  entry.clear();

  entry.set_big(0);
  entry.set_size(0);
  REQUIRE(entry.big() == 0);
  REQUIRE(entry.size() == 0);

  entry.set_big(3);
  entry.set_size(63);
  REQUIRE(entry.big() == 3);
  REQUIRE(entry.size() == 63);
}

TEST_CASE("DirEntry approx size", "[directory]") {
  DirEntry entry;
  entry.clear();

  entry.set_approx_size(512);
  REQUIRE(entry.approx_size() >= 512);

  entry.set_approx_size(4096);
  REQUIRE(entry.approx_size() >= 4096);

  // Note: The size encoding uses 6 bits for size (0-63) and 2 bits for big
  // (0-3) Max representable size is approximately (64 * 512) * 8 = 256KB
  entry.set_approx_size(static_cast<uint64_t>(64 * 1024));
  REQUIRE(entry.approx_size() >= static_cast<uint64_t>(64 * 1024));
}

TEST_CASE("DirEntry approx size rounds up", "[directory][edge]") {
  DirEntry entry;
  entry.clear();

  // Test values just over block boundaries that would truncate without ceiling
  // division Block size is 512 bytes at big=0, 1024 at big=1, etc.

  SECTION("Just over 512-byte boundary") {
    entry.set_approx_size(513);
    REQUIRE(entry.approx_size() >= 513);

    entry.set_approx_size(1023);
    REQUIRE(entry.approx_size() >= 1023);
  }

  SECTION("Just over 1024-byte boundary") {
    entry.set_approx_size(1025);
    REQUIRE(entry.approx_size() >= 1025);

    entry.set_approx_size(2047);
    REQUIRE(entry.approx_size() >= 2047);
  }

  SECTION("Odd sizes that don't align to block boundaries") {
    entry.set_approx_size(777);
    REQUIRE(entry.approx_size() >= 777);

    entry.set_approx_size(1500);
    REQUIRE(entry.approx_size() >= 1500);

    entry.set_approx_size(3333);
    REQUIRE(entry.approx_size() >= 3333);
  }

  SECTION("Large odd sizes") {
    entry.set_approx_size(50000);
    REQUIRE(entry.approx_size() >= 50000);

    entry.set_approx_size(100001);
    REQUIRE(entry.approx_size() >= 100001);
  }
}

TEST_CASE("DirEntry approx size guarantees sufficient space",
          "[directory][edge]") {
  DirEntry entry;
  entry.clear();

  // Test a range of sizes to ensure approx_size() >= input for all
  std::vector<uint64_t> test_sizes = {
      1,    10,   100,  256,  511,  512,   513,   1000,  1023,  1024, 1025,
      2000, 4095, 4096, 4097, 8000, 10000, 16384, 32768, 50000, 65536};

  for (uint64_t size : test_sizes) {
    entry.clear();
    entry.set_approx_size(size);
    CAPTURE(size);
    REQUIRE(entry.approx_size() >= size);
  }
}

TEST_CASE("DirEntry approx size minimum is one block", "[directory][edge]") {
  DirEntry entry;
  entry.clear();

  // Even for size 0 or 1, we should get at least 512 bytes (one block)
  entry.set_approx_size(0);
  REQUIRE(entry.approx_size() >= 512);

  entry.set_approx_size(1);
  REQUIRE(entry.approx_size() >= 512);
}

TEST_CASE("DirEntry tag operations", "[directory]") {
  DirEntry entry;
  entry.clear();

  entry.set_tag(0);
  REQUIRE(entry.tag() == 0);

  entry.set_tag(0xFFF);
  REQUIRE(entry.tag() == 0xFFF);

  entry.set_tag(0xFFFF);
  REQUIRE(entry.tag() == 0xFFF);
}

TEST_CASE("DirEntry flags", "[directory]") {
  DirEntry entry;
  entry.clear();

  REQUIRE_FALSE(entry.phase());
  REQUIRE_FALSE(entry.head());
  REQUIRE_FALSE(entry.pinned());

  entry.set_phase(true);
  REQUIRE(entry.phase());

  entry.set_head(true);
  REQUIRE(entry.head());

  entry.set_pinned(true);
  REQUIRE(entry.pinned());
}

TEST_CASE("DirEntry empty detection", "[directory]") {
  DirEntry entry;
  entry.clear();

  REQUIRE(entry.is_empty());

  entry.set_offset(1);
  REQUIRE_FALSE(entry.is_empty());

  entry.clear();
  REQUIRE(entry.is_empty());
}

TEST_CASE("Directory basic operations", "[directory]") {
  Directory dir(1024);

  REQUIRE(dir.count() == 0);
  REQUIRE(dir.bucket_count() == 1024);

  CacheKey key("test-key");

  REQUIRE_FALSE(dir.probe(key).has_value());

  REQUIRE(dir.insert(key, 12345, 1024));
  REQUIRE(dir.count() == 1);

  auto entry = dir.probe(key);
  REQUIRE(entry.has_value());
  REQUIRE(entry->offset() == 12345);

  REQUIRE(dir.remove(key));
  REQUIRE(dir.count() == 0);
  REQUIRE_FALSE(dir.probe(key).has_value());
}

// Tests for remove_at: offset-precise removal.
// The Volume-level fix (remove_sync) uses probe_each to find candidates, reads
// the document to verify the full SHA-256 key, then calls remove_at with the
// precise offset.  These tests verify remove_at's correctness at the directory
// level — the Volume-level collision test is in test_purge.cpp.
TEST_CASE("Directory remove_at matches on both tag and offset",
          "[directory][remove][regression]") {
  Directory dir(1024);

  CacheKey key("remove-at-key");

  REQUIRE(dir.insert(key, 5000, 512));
  REQUIRE(dir.count() == 1);

  SECTION("correct offset removes the entry") {
    REQUIRE(dir.remove_at(key, 5000));
    REQUIRE(dir.count() == 0);
    REQUIRE_FALSE(dir.probe(key).has_value());
  }

  SECTION("wrong offset does not remove the entry") {
    REQUIRE_FALSE(dir.remove_at(key, 9999));
    REQUIRE(dir.count() == 1);
    auto entry = dir.probe(key);
    REQUIRE(entry.has_value());
    REQUIRE(entry->offset() == 5000);
  }

  SECTION("wrong key does not remove the entry") {
    CacheKey other("some-other-key");
    REQUIRE_FALSE(dir.remove_at(other, 5000));
    REQUIRE(dir.count() == 1);
  }
}

// Test remove_at with different keys in different buckets (entries are
// independent)
TEST_CASE("Directory remove_at does not affect other entries",
          "[directory][remove][regression]") {
  Directory dir(1024);

  CacheKey key_a("key-alpha");
  CacheKey key_b("key-beta");
  CacheKey key_c("key-gamma");

  REQUIRE(dir.insert(key_a, 100, 512));
  REQUIRE(dir.insert(key_b, 200, 512));
  REQUIRE(dir.insert(key_c, 300, 512));
  REQUIRE(dir.count() == 3);

  // Remove only key_b
  REQUIRE(dir.remove_at(key_b, 200));
  REQUIRE(dir.count() == 2);

  // key_a and key_c must still be intact
  auto ea = dir.probe(key_a);
  REQUIRE(ea.has_value());
  REQUIRE(ea->offset() == 100);

  auto ec = dir.probe(key_c);
  REQUIRE(ec.has_value());
  REQUIRE(ec->offset() == 300);

  // key_b must be gone
  REQUIRE_FALSE(dir.probe(key_b).has_value());
}

// MmapDirectory remove_at tests
TEST_CASE("MmapDirectory remove_at matches on both tag and offset",
          "[directory][remove][regression]") {
  constexpr size_t kBuckets = 256;
  size_t region_size = MmapDirectory::required_size(kBuckets);
  std::vector<std::byte> region(region_size, std::byte{0});

  auto dir_opt = MmapDirectory::init(std::span<std::byte>(region), kBuckets);
  REQUIRE(dir_opt.has_value());
  auto &dir = *dir_opt;

  CacheKey key("mmap-remove-at-key");

  REQUIRE(dir.insert(key, 5000, 512));
  REQUIRE(dir.count() == 1);

  SECTION("correct offset removes the entry") {
    REQUIRE(dir.remove_at(key, 5000));
    REQUIRE(dir.count() == 0);
  }

  SECTION("wrong offset does not remove the entry") {
    REQUIRE_FALSE(dir.remove_at(key, 9999));
    REQUIRE(dir.count() == 1);
    auto entry = dir.probe(key);
    REQUIRE(entry.has_value());
    REQUIRE(entry->offset() == 5000);
  }

  SECTION("wrong key does not remove the entry") {
    CacheKey other("some-other-mmap-key");
    REQUIRE_FALSE(dir.remove_at(other, 5000));
    REQUIRE(dir.count() == 1);
  }
}

TEST_CASE("MmapDirectory remove_at does not affect other entries",
          "[directory][remove][regression]") {
  constexpr size_t kBuckets = 256;
  size_t region_size = MmapDirectory::required_size(kBuckets);
  std::vector<std::byte> region(region_size, std::byte{0});

  auto dir_opt = MmapDirectory::init(std::span<std::byte>(region), kBuckets);
  REQUIRE(dir_opt.has_value());
  auto &dir = *dir_opt;

  CacheKey key_a("mmap-key-alpha");
  CacheKey key_b("mmap-key-beta");
  CacheKey key_c("mmap-key-gamma");

  REQUIRE(dir.insert(key_a, 100, 512));
  REQUIRE(dir.insert(key_b, 200, 512));
  REQUIRE(dir.insert(key_c, 300, 512));
  REQUIRE(dir.count() == 3);

  // Remove only key_b
  REQUIRE(dir.remove_at(key_b, 200));
  REQUIRE(dir.count() == 2);

  // key_a and key_c must still be intact
  auto ea = dir.probe(key_a);
  REQUIRE(ea.has_value());
  REQUIRE(ea->offset() == 100);

  auto ec = dir.probe(key_c);
  REQUIRE(ec.has_value());
  REQUIRE(ec->offset() == 300);

  // key_b must be gone
  REQUIRE_FALSE(dir.probe(key_b).has_value());
}

TEST_CASE("Directory serialization", "[directory]") {
  Directory dir(128);

  CacheKey key1("key1");
  CacheKey key2("key2");

  dir.insert(key1, 100, 512);
  dir.insert(key2, 200, 1024);

  auto data = dir.serialize();
  REQUIRE_FALSE(data.empty());

  Directory dir2(128);
  dir2.deserialize(data);

  REQUIRE(dir2.count() == 2);
  REQUIRE(dir2.probe(key1).has_value());
  REQUIRE(dir2.probe(key2).has_value());
}

// =============================================================================
// Tag-collision regression tests (collision-destructive insert fix).
// A DirEntry holds no key material — only a 12-bit tag — so two distinct
// keys can collide on (bucket, tag).  insert() must never update a colliding
// foreign entry in place unless the caller verified the stored key
// (verified_offset); a full bucket evicts the collider and reports it.
// The Volume-level end-to-end test is in test_tag_collision.cpp.
// =============================================================================

namespace {

// Find two distinct keys colliding on (bucket_hash % num_buckets, tag).
std::pair<CacheKey, CacheKey> find_colliding_pair(size_t num_buckets) {
  std::map<uint64_t, CacheKey> seen;
  for (uint32_t i = 0;; ++i) {
    CacheKey key("dir-collide-" + std::to_string(i));
    uint64_t slot = ((key.bucket_hash() % num_buckets) << 12) | key.tag();
    auto [it, inserted] = seen.emplace(slot, key);
    if (!inserted) {
      return {it->second, key};
    }
  }
}

// Find `count` keys in the anchor's bucket with tags distinct from the
// anchor's tag and from each other.
std::vector<CacheKey> find_bucket_fillers(const CacheKey &anchor,
                                          size_t num_buckets, size_t count) {
  std::vector<CacheKey> fillers;
  std::vector<uint16_t> used_tags{anchor.tag()};
  for (uint32_t i = 0; fillers.size() < count; ++i) {
    CacheKey key("dir-filler-" + std::to_string(i));
    if (key.bucket_hash() % num_buckets != anchor.bucket_hash() % num_buckets) {
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

}  // namespace

TEST_CASE("Directory insert preserves a colliding foreign entry",
          "[directory][collision][regression]") {
  constexpr size_t kBuckets = 8;
  Directory dir(kBuckets);

  auto [key_a, key_b] = find_colliding_pair(kBuckets);
  REQUIRE(key_a.tag() == key_b.tag());
  REQUIRE_FALSE(key_a == key_b);

  REQUIRE(dir.insert(key_a, 1000, 512, Directory::kNoVerifiedEntry));
  REQUIRE(dir.count() == 1);

  // B collides on (bucket, tag) but is a different key: with no verified
  // entry it must NOT update A's slot in place — both entries coexist.
  bool evicted = false;
  REQUIRE(dir.insert(key_b, 2000, 512, Directory::kNoVerifiedEntry, &evicted));
  REQUIRE_FALSE(evicted);
  REQUIRE(dir.count() == 2);

  auto entries = dir.probe_all(key_a);
  REQUIRE(entries.size() == 2);
  bool has_a = false;
  bool has_b = false;
  for (const auto &entry : entries) {
    has_a = has_a || entry.offset() == 1000;
    has_b = has_b || entry.offset() == 2000;
  }
  REQUIRE(has_a);
  REQUIRE(has_b);

  // In-place update of A via its verified offset leaves B alone.
  REQUIRE(dir.insert(key_a, 3000, 512, /*verified_offset=*/1000));
  REQUIRE(dir.count() == 2);
  entries = dir.probe_all(key_a);
  REQUIRE(entries.size() == 2);
  has_a = false;
  has_b = false;
  for (const auto &entry : entries) {
    has_a = has_a || entry.offset() == 3000;
    has_b = has_b || entry.offset() == 2000;
  }
  REQUIRE(has_a);
  REQUIRE(has_b);
}

TEST_CASE("Directory insert evicts the collider only when the bucket is full",
          "[directory][collision][regression]") {
  constexpr size_t kBuckets = 8;
  Directory dir(kBuckets);

  auto [key_a, key_b] = find_colliding_pair(kBuckets);
  auto fillers =
      find_bucket_fillers(key_a, kBuckets, Directory::kEntriesPerBucket - 1);

  REQUIRE(dir.insert(key_a, 1000, 512, Directory::kNoVerifiedEntry));
  uint64_t offset = 2000;
  for (const auto &filler : fillers) {
    REQUIRE(dir.insert(filler, offset, 512, Directory::kNoVerifiedEntry));
    offset += 1000;
  }
  REQUIRE(dir.count() == Directory::kEntriesPerBucket);

  // Bucket full of current-phase entries: the colliding insert replaces the
  // collider (reported via collision_evicted), never any other entry.
  bool evicted = false;
  bool bucket_full_evicted = false;
  REQUIRE(dir.insert(key_b, 9000, 512, Directory::kNoVerifiedEntry, &evicted,
                     &bucket_full_evicted));
  REQUIRE(evicted);
  REQUIRE(dir.count() == Directory::kEntriesPerBucket);

  auto entries = dir.probe_all(key_b);
  REQUIRE(entries.size() == 1);
  REQUIRE(entries[0].offset() == 9000);

  for (const auto &filler : fillers) {
    REQUIRE(dir.probe(filler).has_value());
  }

  // The collider path must not be conflated with the bucket-full path.
  REQUIRE_FALSE(bucket_full_evicted);
}

TEST_CASE("Directory insert evicts the entry nearest the wrap cursor",
          "[directory][eviction][regression]") {
  // a bucket full of distinct current-phase entries must not fail
  // the insert — the entry nearest the wrap cursor makes way.
  constexpr size_t kBuckets = 8;
  Directory dir(kBuckets);

  CacheKey anchor("dir-bucket-full-anchor");
  // kEntriesPerBucket + 1 keys sharing anchor's bucket, all tags distinct, so
  // no insert can update in place or find a collider to displace.
  auto keys =
      find_bucket_fillers(anchor, kBuckets, Directory::kEntriesPerBucket + 1);

  uint64_t offset = 1000;
  for (size_t i = 0; i < Directory::kEntriesPerBucket; ++i) {
    REQUIRE(dir.insert(keys[i], offset, 512, Directory::kNoVerifiedEntry));
    offset += 1000;
  }
  REQUIRE(dir.count() == Directory::kEntriesPerBucket);

  bool collision_evicted = false;
  bool bucket_full_evicted = false;
  REQUIRE(dir.insert(keys[Directory::kEntriesPerBucket], 9000, 512,
                     Directory::kNoVerifiedEntry, &collision_evicted,
                     &bucket_full_evicted));

  // Counted on the bucket-full path only — never as a tag collision.
  REQUIRE(bucket_full_evicted);
  REQUIRE_FALSE(collision_evicted);
  // Replaced a slot, so the count is unchanged.
  REQUIRE(dir.count() == Directory::kEntriesPerBucket);

  // The new entry landed.
  auto entries = dir.probe_all(keys[Directory::kEntriesPerBucket]);
  REQUIRE(entries.size() == 1);
  REQUIRE(entries[0].offset() == 9000);

  // The victim is the lowest offset (1000) — every other entry survives.
  REQUIRE_FALSE(dir.probe(keys[0]).has_value());
  for (size_t i = 1; i < Directory::kEntriesPerBucket; ++i) {
    auto survivor = dir.probe(keys[i]);
    REQUIRE(survivor.has_value());
    REQUIRE(survivor->offset() == (i + 1) * 1000);
  }
}
