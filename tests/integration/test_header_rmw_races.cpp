// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Concurrency tests for the two in-place header RMW sites in multi-process
// (mmap-directory) mode.  update_hit_count_sync and remove_alternate_sync's
// middle/tail chain repoint mutate a LIVE published document's fixed header in
// place; lock-free readers deserialize that same header concurrently.  The
// fixed header is outside the document checksum, so a torn read or a stray
// store is not CRC-detectable.  Two properties are asserted here:
//
//   * data integrity — a reader NEVER sees a torn or wrong-variant document
//     while its header is being mutated (checked on the main thread; Catch2
//     assertion macros are not thread-safe, so worker threads only flip an
//     atomic flag);
//   * atomicity — the header store (std::atomic_ref) and the reader's header
//     load (Document::deserialize, also std::atomic_ref post-v6) never form a
//     data race.  This file is in the DEFAULT tag set so the TSan CI lane runs
//     it; before v6 no test combined mmap-directory mode with a header store
//     racing a header read, so TSan was structurally blind to this channel.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "core/document.hpp"
#include "core/volume.hpp"
#include "cyclone/alternate.hpp"
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

// Thread-safe (non-asserting) alternate write for use INSIDE worker threads --
// Catch2's REQUIRE is not thread-safe, so worker threads must never call the
// asserting write_alt above.  Returns false on any failure; callers treat a
// failed best-effort re-add as a tolerated miss.
bool try_write_alt(Volume &volume, const CacheKey &key, AlternateId id,
                   std::span<const std::byte> content) {
  auto wh = volume.write_alternate_sync(key, id, content.size());
  if (!wh.has_value()) {
    return false;
  }
  (void)wh->write_sync(content);
  return wh->close_sync().has_value();
}

// Selector that deterministically picks the alternate carrying a given id,
// falling back to the first listed alternate.
class PickIdSelector : public StorageAlternateSelector {
 public:
  explicit PickIdSelector(AlternateId id) : _id(id) {}
  [[nodiscard]] std::optional<size_t> select(
      std::span<const AlternateInfo> alternates,
      const AlternateSelectionContext & /*ctx*/) const override {
    for (size_t i = 0; i < alternates.size(); ++i) {
      if (alternates[i].id == _id) {
        return i;
      }
    }
    return alternates.empty() ? std::nullopt : std::optional<size_t>(0);
  }

 private:
  AlternateId _id;
};

MultiProcessConfig mp_enabled() {
  MultiProcessConfig mp;  // process 0 of 1 — mmap directory, all stripes owned
  mp.enabled = true;      // exactly what mod_pagespeed sets in every process
  return mp;
}

}  // namespace

TEST_CASE("Concurrent hit-count RMW never races or corrupts a reader",
          "[header_rmw][multiprocess]") {
  TempCacheDir tmp;
  std::string path = tmp.path();
  VolumeConfig cfg;
  cfg.path = path;
  cfg.size = static_cast<size_t>(16 * 1024 * 1024);
  Volume volume(cfg, mp_enabled());
  REQUIRE(volume.open().has_value());

  CacheKey key("hit-rmw-key");
  auto original = make_content(std::byte{0xA0}, 512);
  auto brotli = make_content(std::byte{0xB1}, 512);
  write_doc(volume, key, original);
  write_alt(volume, key, AlternateId::Brotli, brotli);

  std::atomic<bool> stop{false};
  std::atomic<bool> corruption{false};
  std::atomic<uint64_t> updates{0};
  std::atomic<uint64_t> reads{0};

  // Writer: hammer the Brotli alternate's hit_count + last_access in place.
  std::thread writer([&]() {
    int64_t ts = 1;
    while (!stop.load(std::memory_order_relaxed)) {
      auto r = volume.update_hit_count_sync(key, AlternateId::Brotli, 1, ts++);
      if (r.has_value()) {
        updates.fetch_add(1, std::memory_order_relaxed);
      }
    }
  });

  // Readers: lock-free walks that deserialize the very header being mutated.
  constexpr int kReaders = 4;
  std::vector<std::thread> readers;
  readers.reserve(kReaders);
  for (int t = 0; t < kReaders; ++t) {
    readers.emplace_back([&]() {
      PickIdSelector selector(AlternateId::Brotli);
      AlternateSelectionContext ctx;
      while (!stop.load(std::memory_order_relaxed)) {
        auto rh = volume.read_alternate_sync(key, selector, ctx);
        if (!rh.has_value()) {
          continue;  // a transient miss is fine; wrong bytes are not
        }
        auto content = rh->content();
        if (content.size() != brotli.size() ||
            std::memcmp(content.data(), brotli.data(), brotli.size()) != 0) {
          corruption.store(true, std::memory_order_relaxed);
        }
        reads.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  stop.store(true, std::memory_order_relaxed);
  writer.join();
  for (auto &r : readers) {
    r.join();
  }

  REQUIRE_FALSE(corruption.load());
  // Both sides made progress, so the race window was genuinely exercised.
  REQUIRE(updates.load() > 0);
  REQUIRE(reads.load() > 0);

  volume.close();
}

TEST_CASE(
    "Concurrent middle-alternate repoint never races or corrupts a reader",
    "[header_rmw][multiprocess]") {
  TempCacheDir tmp;
  std::string path = tmp.path();
  VolumeConfig cfg;
  cfg.path = path;
  cfg.size = static_cast<size_t>(16 * 1024 * 1024);
  Volume volume(cfg, mp_enabled());
  REQUIRE(volume.open().has_value());

  CacheKey key("repoint-rmw-key");
  auto original = make_content(std::byte{0xC0}, 512);
  auto gzip = make_content(std::byte{0xC3}, 512);
  auto brotli = make_content(std::byte{0xC1}, 512);
  write_doc(volume, key, original);
  write_alt(volume, key, AlternateId::Gzip, gzip);
  write_alt(volume, key, AlternateId::Brotli, brotli);

  std::atomic<bool> stop{false};
  std::atomic<bool> corruption{false};
  std::atomic<uint64_t> repoints{0};
  std::atomic<uint64_t> reads{0};

  // Writer: repeatedly remove the Gzip alternate (an in-place
  // next_alternate_offset repoint of its predecessor) and re-add it.  The
  // reader below is steered THROUGH that repointed pointer to the Brotli node.
  std::thread writer([&]() {
    while (!stop.load(std::memory_order_relaxed)) {
      if (volume.remove_alternate_sync(key, AlternateId::Gzip).has_value()) {
        repoints.fetch_add(1, std::memory_order_relaxed);
      }
      (void)try_write_alt(volume, key, AlternateId::Gzip, gzip);
    }
  });

  constexpr int kReaders = 4;
  std::vector<std::thread> readers;
  readers.reserve(kReaders);
  for (int t = 0; t < kReaders; ++t) {
    readers.emplace_back([&]() {
      PickIdSelector selector(AlternateId::Brotli);
      AlternateSelectionContext ctx;
      while (!stop.load(std::memory_order_relaxed)) {
        auto rh = volume.read_alternate_sync(key, selector, ctx);
        if (!rh.has_value()) {
          continue;  // Brotli may be transiently unreachable — tolerated
        }
        auto content = rh->content();
        // When Brotli is served it must be intact; a chain walked across a
        // torn next_alternate_offset could land on a foreign / partial doc.
        if (content.size() != brotli.size() ||
            std::memcmp(content.data(), brotli.data(), brotli.size()) != 0) {
          corruption.store(true, std::memory_order_relaxed);
        }
        reads.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  stop.store(true, std::memory_order_relaxed);
  writer.join();
  for (auto &r : readers) {
    r.join();
  }

  REQUIRE_FALSE(corruption.load());
  REQUIRE(repoints.load() > 0);
  REQUIRE(reads.load() > 0);

  volume.close();
}
