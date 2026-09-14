// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Regression tests for the lock-free read path (readers take no stripe
// lock).  Two hazards the removed reader lock used to exclude:
//
// 1. CROSS-KEY CHAIN ESCAPE: remove_alternate_sync repoints a chain's
//    next_alternate_offset in place; a concurrent lock-free chain walk can
//    (via a stale pointer to a since-wrapped node — a torn read itself is
//    now prevented by the atomic store/load pair) land on an arbitrary
//    in-bounds offset.  If a valid document of a DIFFERENT key
//    lives there, the walk used to accept it — chain nodes were never
//    first_key-verified (only the head), and neither was the SELECTED
//    alternate — so key A's read could list and even serve key B's
//    content.  The fix verifies first_key on every chain hop and on the
//    selected alternate.  Tested deterministically by forging the pointer
//    (worst-case torn value) directly in the volume file.
//
// 2. RAM RESURRECTION: the read path's RAM-cache repopulation (put) can
//    complete AFTER a concurrent remove already ran its RAM invalidation
//    — resurrecting purged content that is then served from RAM
//    indefinitely.  The fix re-checks the stripe's remove_epoch (and wrap
//    epoch) after the put and undoes it on a race.  Tested with a
//    rounds-based stress: after all readers quiesce, a removed key MUST
//    miss.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "core/document.hpp"
#include "core/volume.hpp"
#include "cyclone/alternate.hpp"
#include "cyclone/cache.hpp"
#include "cyclone/config.hpp"
#include "cyclone/key.hpp"
#include "support/temp_cache.hpp"

using namespace cyclone;

namespace {

std::vector<std::byte> make_content(std::byte fill, size_t size) {
  return std::vector<std::byte>(size, fill);
}

void write_doc(Volume &volume, const CacheKey &key,
               std::span<const std::byte> content) {
  auto wh = volume.write_sync(key, content.size());
  REQUIRE(wh.has_value());
  (void)wh->write_sync(content);
  (void)wh->close_sync();
}

void write_alt(Volume &volume, const CacheKey &key, AlternateId id,
               std::span<const std::byte> content) {
  auto wh = volume.write_alternate_sync(key, id, content.size());
  REQUIRE(wh.has_value());
  (void)wh->write_sync(content);
  (void)wh->close_sync();
}

// Find the absolute file offsets of all documents whose first_key matches
// `key`, by scanning for the document magic + digest pattern.  Documents
// are written sequentially, so the lowest offset is the head.
std::vector<uint64_t> find_doc_offsets(const std::string &path,
                                       const CacheKey &key) {
  std::ifstream f(path, std::ios::binary);
  REQUIRE(f.good());
  std::vector<char> data((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
  const uint32_t magic = Document::kMagic;
  auto digest = key.digest();
  std::vector<uint64_t> offsets;
  if (data.size() < Document::kHeaderSize) {
    return offsets;
  }
  for (size_t d = 0; d + Document::kHeaderSize <= data.size(); ++d) {
    if (std::memcmp(data.data() + d, &magic, sizeof(magic)) != 0) {
      continue;
    }
    if (std::memcmp(data.data() + d + 16, digest.data(), digest.size()) != 0) {
      continue;
    }
    offsets.push_back(static_cast<uint64_t>(d));
  }
  return offsets;
}

uint64_t read_u64_at(const std::string &path, uint64_t offset) {
  std::ifstream f(path, std::ios::binary);
  REQUIRE(f.good());
  f.seekg(static_cast<std::streamoff>(offset));
  uint64_t value = 0;
  f.read(reinterpret_cast<char *>(&value), sizeof(value));
  REQUIRE(f.good());
  return value;
}

void write_u64_at(const std::string &path, uint64_t offset, uint64_t value) {
  std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
  REQUIRE(f.good());
  f.seekp(static_cast<std::streamoff>(offset));
  f.write(reinterpret_cast<const char *>(&value), sizeof(value));
  REQUIRE(f.good());
  f.flush();
}

// Selector that always picks the LAST listed alternate — steers the read
// at the (possibly foreign) tail of the chain.
class LastAlternateSelector : public StorageAlternateSelector {
 public:
  [[nodiscard]] std::optional<size_t> select(
      std::span<const AlternateInfo> alternates,
      const AlternateSelectionContext & /*ctx*/) const override {
    if (alternates.empty()) {
      return std::nullopt;
    }
    return alternates.size() - 1;
  }
};

}  // namespace

TEST_CASE("Alternate chain repointed at a foreign key's document is rejected",
          "[lockfree][regression]") {
  TempCacheDir tmp;
  std::string path = tmp.path();
  VolumeConfig config;
  config.path = path;
  config.size = static_cast<size_t>(8 * 1024 * 1024);
  auto volume = std::make_shared<Volume>(config);
  REQUIRE(volume->open().has_value());

  CacheKey key_a("victim-key");
  CacheKey key_b("foreign-key");

  auto content_a = make_content(std::byte{0xAA}, 512);
  auto content_a_alt = make_content(std::byte{0xAB}, 512);
  auto content_b = make_content(std::byte{0xBB}, 512);

  write_doc(*volume, key_a, content_a);
  write_alt(*volume, key_a, AlternateId::Brotli, content_a_alt);
  write_doc(*volume, key_b, content_b);

  // Locate A's head + alternate and B's document in the file.
  auto offsets_a = find_doc_offsets(path, key_a);
  auto offsets_b = find_doc_offsets(path, key_b);
  REQUIRE(offsets_a.size() == 2);  // head + alternate
  REQUIRE(offsets_b.size() == 1);
  uint64_t doc_b = offsets_b[0];

  // The chain HEAD is whichever of A's two documents carries a nonzero
  // next_alternate_offset (commit_alternate_write may prepend the new
  // alternate rather than append).  Recover the stripe base from that
  // REAL (stripe-relative) pointer, then forge it at B's document — the
  // worst-case "torn 8-byte read" a concurrent in-place chain repoint can
  // produce.
  uint64_t next0 =
      read_u64_at(path, offsets_a[0] + Document::kNextAlternateOffsetPos);
  uint64_t next1 =
      read_u64_at(path, offsets_a[1] + Document::kNextAlternateOffsetPos);
  REQUIRE(((next0 == 0) != (next1 == 0)));  // exactly one chain hop
  uint64_t chain_head = (next0 != 0) ? offsets_a[0] : offsets_a[1];
  uint64_t chain_tail = (next0 != 0) ? offsets_a[1] : offsets_a[0];
  uint64_t real_next = (next0 != 0) ? next0 : next1;
  REQUIRE(chain_tail > real_next);
  uint64_t stripe_base = chain_tail - real_next;
  uint64_t next_field = chain_head + Document::kNextAlternateOffsetPos;
  uint64_t forged_next = doc_b - stripe_base;
  write_u64_at(path, next_field, forged_next);

  SECTION("list_alternates_sync does not list the foreign document") {
    auto alts = volume->list_alternates_sync(key_a);
    REQUIRE(alts.has_value());
    // The walk must stop at the foreign document: only A's chain head
    // survives (its real second entry was orphaned by the forged pointer).
    REQUIRE(alts->size() == 1);
  }

  SECTION("read_alternate_sync never serves the foreign document") {
    LastAlternateSelector selector;
    AlternateSelectionContext ctx;
    auto rh = volume->read_alternate_sync(key_a, selector, ctx);
    REQUIRE(rh.has_value());
    auto content = rh->content();
    // Key A's read must never return key B's bytes.
    bool is_foreign =
        content.size() == content_b.size() &&
        std::memcmp(content.data(), content_b.data(), content_b.size()) == 0;
    REQUIRE_FALSE(is_foreign);
  }

  volume->close();
}

TEST_CASE("Removed key does not resurrect from a raced RAM repopulation",
          "[lockfree][regression][stress]") {
  TempCacheDir tmp;
  std::string path = tmp.path();
  CacheConfig cache_config;
  cache_config.ram_cache_size = static_cast<size_t>(8 * 1024 * 1024);
  auto cache_result = Cache::create(cache_config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = path;
  vol_config.size = static_cast<size_t>(16 * 1024 * 1024);
  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  // 32KB content: at the RAM tier's sendfile threshold, maximizing the
  // put's copy window that the fix must guard.
  auto content = make_content(std::byte{0xCD}, size_t{32} * 1024);
  CacheKey key("resurrect-me");

  // Readers hammer the key for the whole test.  Every write→remove cycle
  // re-opens the repopulation window: the fresh entry is not in RAM, so
  // the first read after each write takes the disk path and puts.
  std::atomic<bool> stop{false};
  constexpr int kReaders = 8;
  std::vector<std::thread> readers;
  readers.reserve(kReaders);
  for (int t = 0; t < kReaders; ++t) {
    readers.emplace_back([&]() {
      DefaultStorageSelector selector;
      AlternateSelectionContext ctx;
      while (!stop.load(std::memory_order_relaxed)) {
        (void)cache->read_alternate_sync(key, selector, ctx);
      }
    });
  }

  // No REQUIRE inside the loop: a throwing assertion would unwind past
  // the joinable reader threads and terminate.  Record, join, then assert.
  constexpr int kCycles = 1000;
  bool resurrected = false;
  int skipped = 0;
  int failed_cycle = -1;
  for (int cycle = 0; cycle < kCycles && !resurrected; ++cycle) {
    {
      auto wh = cache->write_sync(key, content.size());
      if (!wh.has_value()) {
        ++skipped;
        continue;
      }
      (void)wh->write_sync(std::span<const std::byte>(content));
      if (!wh->close_sync().has_value()) {
        // Commit legitimately dropped (e.g. the lease gate
        // deferred a wrap under the readers' live leases) — nothing was
        // published, so there is nothing to remove or resurrect.
        ++skipped;
        continue;
      }
    }
    // Vary the write→remove gap so the remove lands at different phases
    // of the readers' probe→put window.
    std::this_thread::sleep_for(
        std::chrono::microseconds(50 + (cycle % 40) * 25));
    if (!cache->remove_sync(key).has_value()) {
      // The entry can already be gone (phase-toggled away by a wrap
      // between the commit and the remove).  RAM invalidation still ran;
      // fall through to the verification below either way.
      ++skipped;
    }

    // Drain in-flight reads that started before the remove, then verify:
    // the key must be gone.  A hit here means a raced RAM repopulation
    // resurrected removed content.
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    DefaultStorageSelector selector;
    AlternateSelectionContext ctx;
    auto rh = cache->read_alternate_sync(key, selector, ctx);
    if (rh.has_value()) {
      resurrected = true;
      failed_cycle = cycle;
    }
  }

  stop.store(true, std::memory_order_relaxed);
  for (auto &t : readers) {
    t.join();
  }

  INFO("resurrection at cycle " << failed_cycle << ", skipped " << skipped
                                << " dropped/raced cycles");
  // Keep the test honest: most cycles must actually exercise the
  // write→remove→verify sequence.
  REQUIRE(skipped < kCycles / 2);
  REQUIRE_FALSE(resurrected);

  cache->stop();
}
