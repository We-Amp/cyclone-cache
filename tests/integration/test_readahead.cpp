// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Large-document readahead (CacheConfig/VolumeConfig::readahead_min_bytes).
//
// The volume mapping is opened MADV_RANDOM, which is right for 4 KB HTTP
// objects but turns a cold read of a multi-megabyte document into one serial
// page fault per 4 KB page.  The disk read path now issues a single
// readahead hint (MADV_WILLNEED / PrefetchVirtualMemory) over exactly the
// document's byte range, at the one point where the range is known and
// nothing has touched the content yet -- after the full-key re-verification,
// before the CRC pass.
//
// The hint itself is unobservable from the public API by construction: it is
// a kernel advice with no functional effect.  What these cases pin is that
// switching it on, off, or out of reach never changes what a read returns --
// the advised range is exactly the document, so a stray length or a
// mis-aligned address would surface as a CRC failure, a short read or
// corrupted bytes rather than as silence.
//
// What each case is for:
//
//   * CONFIG -- the default is 256 KiB on both CacheConfig and VolumeConfig,
//     the fluent setter works, and 0 is accepted as "off".
//   * ROUND TRIP -- documents below AND above the threshold read back
//     byte-identical with verify_checksum_on_read on (so the CRC pass really
//     did walk the advised range).
//   * THRESHOLD SWEEP -- off (0), the default, and a threshold far above any
//     document all return identical bytes for the same content.
//   * REOPEN -- the first-touch path: a cache closed and reopened over the
//     same volume file re-reads a large document through the advise +
//     CRC-verify path with no warm mapping.
//   * ALTERNATE -- the second hook, on the selected-alternate read path.
//   * BOUNDARY -- a document whose length (header included) is exactly the
//     threshold issues one hint, and one a byte under it issues none; both
//     read back intact.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <vector>

#include "core/document.hpp"
#include "cyclone/alternate.hpp"
#include "cyclone/cache.hpp"
#include "cyclone/config.hpp"
#include "cyclone/key.hpp"
#include "support/temp_cache.hpp"

using namespace cyclone;

namespace {

constexpr size_t kKB = 1024;
constexpr size_t kMB = 1024 * kKB;
constexpr size_t kVolumeBytes = 256 * kMB;

// RAM cache OFF: every read must reach the disk path, which is the only
// place the readahead hint lives.  Checksums ON: the CRC pass is the first
// content touch, i.e. the consumer of the advised range.
CacheConfig readahead_config(std::optional<size_t> min_bytes) {
  CacheConfig c;
  c.ram_cache_size = 0;
  c.verify_checksum_on_read = true;
  if (min_bytes) {
    c.readahead_min_bytes = *min_bytes;
  }
  return c;
}

std::unique_ptr<Cache> make_cache(const TempCacheDir& dir,
                                  const CacheConfig& cfg) {
  auto created = Cache::create(cfg);
  REQUIRE(created.has_value());
  auto cache = std::move(*created);
  VolumeConfig vc;
  vc.path = dir.path();
  vc.size = kVolumeBytes;
  REQUIRE(cache->add_volume(vc).has_value());
  REQUIRE(cache->start().has_value());
  return cache;
}

// Patterned content, seeded so different documents differ: byte i is
// (i * 31 + seed) & 0xFF.
std::vector<std::byte> patterned(size_t size, uint8_t seed) {
  std::vector<std::byte> v(size);
  for (size_t i = 0; i < size; ++i) {
    v[i] = static_cast<std::byte>((i * 31 + seed) & 0xFF);
  }
  return v;
}

bool content_matches(std::span<const std::byte> got, size_t size,
                     uint8_t seed) {
  if (got.size() != size) {
    return false;
  }
  for (size_t i = 0; i < size; ++i) {
    if (got[i] != static_cast<std::byte>((i * 31 + seed) & 0xFF)) {
      return false;
    }
  }
  return true;
}

void write_doc(Cache& cache, const CacheKey& key, size_t size, uint8_t seed) {
  auto content = patterned(size, seed);
  auto wh = cache.write_sync(key, content.size());
  REQUIRE(wh.has_value());
  REQUIRE(wh->write_sync(std::span<const std::byte>(content)).has_value());
  REQUIRE(wh->close_sync().has_value());
}

// Read back and assert byte identity.  Returns false on any miss.
bool read_and_verify(Cache& cache, const CacheKey& key, size_t size,
                     uint8_t seed) {
  auto rh = cache.read_sync(key);
  if (!rh.has_value()) {
    return false;
  }
  return content_matches(rh->content(), size, seed);
}

class IdSelector : public StorageAlternateSelector {
 public:
  explicit IdSelector(AlternateId want) : _want(want) {}
  [[nodiscard]] std::optional<size_t> select(
      std::span<const AlternateInfo> alternates,
      const AlternateSelectionContext& /*ctx*/) const override {
    for (size_t i = 0; i < alternates.size(); ++i) {
      if (alternates[i].id == _want) {
        return i;
      }
    }
    return std::nullopt;
  }

 private:
  AlternateId _want;
};

}  // namespace

TEST_CASE("readahead_min_bytes config plumbing", "[readahead][config]") {
  SECTION("defaults are 256 KiB on both configs") {
    CacheConfig cc;
    REQUIRE(cc.readahead_min_bytes == 256 * kKB);
    VolumeConfig vc;
    REQUIRE(vc.readahead_min_bytes == 256 * kKB);
  }

  SECTION("fluent setter assigns and chains") {
    CacheConfig cc;
    CacheConfig& same = cc.set_readahead_min_bytes(4 * kMB);
    REQUIRE(&same == &cc);
    REQUIRE(cc.readahead_min_bytes == 4 * kMB);
  }

  SECTION("0 is accepted as off and survives cache construction") {
    CacheConfig cc = readahead_config(0);
    REQUIRE(cc.readahead_min_bytes == 0);
    TempCacheDir tmp("readahead_cfg_off");
    auto cache = make_cache(tmp, cc);
    REQUIRE(cache != nullptr);
  }
}

TEST_CASE("Reads below and above the readahead threshold are intact",
          "[readahead][integration]") {
  TempCacheDir tmp("readahead_roundtrip");
  auto cache = make_cache(tmp, readahead_config(std::nullopt));

  // 4 KB: well below the 256 KiB default, the hint never fires.
  const CacheKey small("readahead-small");
  write_doc(*cache, small, 4 * kKB, 11);
  REQUIRE(read_and_verify(*cache, small, 4 * kKB, 11));

  // 2 MiB: above the default, the hint fires and the CRC pass walks the
  // advised range.
  const CacheKey large("readahead-large");
  write_doc(*cache, large, 2 * kMB, 23);
  REQUIRE(read_and_verify(*cache, large, 2 * kMB, 23));

  // Re-read: the second read takes the checksum-validation cache path, so
  // the advice runs ahead of a pass that does NOT re-CRC.
  REQUIRE(read_and_verify(*cache, large, 2 * kMB, 23));
}

TEST_CASE("Threshold setting never changes what a read returns",
          "[readahead][integration]") {
  constexpr size_t kSize = 1 * kMB;

  // off / default / unreachably high -- identical bytes out of all three.
  const size_t thresholds[] = {0, 256 * kKB, 64 * kMB};
  for (size_t threshold : thresholds) {
    TempCacheDir tmp("readahead_sweep");
    auto cache = make_cache(tmp, readahead_config(threshold));
    const CacheKey key("readahead-sweep");
    write_doc(*cache, key, kSize, 77);
    INFO("readahead_min_bytes = " << threshold);
    REQUIRE(read_and_verify(*cache, key, kSize, 77));
  }
}

TEST_CASE("Large document survives a reopen through the advise path",
          "[readahead][integration]") {
  TempCacheDir tmp("readahead_reopen");
  const CacheKey key("readahead-reopen");
  constexpr size_t kSize = 2 * kMB;

  // The shared mmap directory is what makes the index outlive the process;
  // with the in-memory directory a reopen is a guaranteed miss and says
  // nothing about the read path.
  auto persistent = [](std::optional<size_t> min_bytes) {
    CacheConfig c = readahead_config(min_bytes);
    c.set_multi_process(0, 1);
    return c;
  };

  {
    auto cache = make_cache(tmp, persistent(std::nullopt));
    write_doc(*cache, key, kSize, 42);
    REQUIRE(read_and_verify(*cache, key, kSize, 42));
    cache->stop();
  }

  // Fresh Cache over the same volume file: no warm mapping, no validated
  // checksum cache -- the first read is a first-touch read straight through
  // the advise + CRC-verify path.
  {
    auto cache = make_cache(tmp, persistent(std::nullopt));
    REQUIRE(read_and_verify(*cache, key, kSize, 42));
    cache->stop();
  }

  // Same again with the hint disabled: identical result.
  {
    auto cache = make_cache(tmp, persistent(0));
    REQUIRE(read_and_verify(*cache, key, kSize, 42));
    cache->stop();
  }
}

TEST_CASE("Selected-alternate reads honour the readahead threshold",
          "[readahead][alternate]") {
  TempCacheDir tmp("readahead_alternate");
  auto cache = make_cache(tmp, readahead_config(std::nullopt));
  const CacheKey key("readahead-alternate");

  // Original below the threshold, Brotli alternate above it: the alternate
  // hook is the one that must fire, and only for the alternate.
  write_doc(*cache, key, 8 * kKB, 5);

  constexpr size_t kAltSize = 1 * kMB;
  auto alt = patterned(kAltSize, 91);
  auto wh = cache->write_alternate_sync(key, AlternateId::Brotli, alt.size());
  REQUIRE(wh.has_value());
  REQUIRE(wh->write_sync(std::span<const std::byte>(alt)).has_value());
  REQUIRE(wh->close_sync().has_value());

  IdSelector want_brotli(AlternateId::Brotli);
  AlternateSelectionContext ctx;
  auto rh = cache->read_alternate_sync(key, want_brotli, ctx);
  REQUIRE(rh.has_value());
  REQUIRE(content_matches(rh->content(), kAltSize, 91));

  IdSelector want_original(AlternateId::Original);
  auto orig = cache->read_alternate_sync(key, want_original, ctx);
  REQUIRE(orig.has_value());
  REQUIRE(content_matches(orig->content(), 8 * kKB, 5));
}

TEST_CASE("Documents at the readahead threshold boundary read back intact",
          "[readahead][edge]") {
  constexpr size_t kThreshold = 256 * kKB;
  TempCacheDir tmp("readahead_boundary");
  auto cache = make_cache(tmp, readahead_config(kThreshold));

  // The threshold compares against the DOCUMENT length, Document::len, which
  // is the fixed header plus the (here empty) HTTP header plus the content.
  // Size the CONTENT so the document itself lands exactly on each side of
  // the edge, and pin which side fires through readahead_hints_issued: a
  // content size of kThreshold - 1 would be a document well OVER the
  // threshold and would not test the edge at all.
  constexpr size_t kAtContent = kThreshold - Document::kHeaderSize;
  constexpr size_t kUnderContent = kAtContent - 1;

  // Document length kThreshold - 1: no hint.
  const CacheKey under("readahead-under");
  write_doc(*cache, under, kUnderContent, 4);
  const uint64_t before_under = cache->stats().readahead_hints_issued;
  REQUIRE(read_and_verify(*cache, under, kUnderContent, 4));
  CHECK(cache->stats().readahead_hints_issued == before_under);

  // Document length exactly kThreshold: exactly one hint.
  const CacheKey at("readahead-at");
  write_doc(*cache, at, kAtContent, 3);
  const uint64_t before_at = cache->stats().readahead_hints_issued;
  REQUIRE(read_and_verify(*cache, at, kAtContent, 3));
  CHECK(cache->stats().readahead_hints_issued == before_at + 1);

  // One page over, to cover a range whose page-aligned end is past the
  // document end.
  const CacheKey over("readahead-over");
  write_doc(*cache, over, kThreshold + 4096 + 7, 6);
  REQUIRE(read_and_verify(*cache, over, kThreshold + 4096 + 7, 6));
}

TEST_CASE("The readahead hint is re-issued after the re-advise interval",
          "[readahead][integration]") {
  // The filter is time-DECAYED, not once-ever: a KV tier is normally larger
  // than RAM, so "advised once, evicted from the page cache, read cold
  // again" is the common case.  A permanent filter would drop the readahead
  // exactly where it is worth most, while no filter at all would make every
  // warm read pay for a full-range madvise() walk.
  //
  // CacheStats::readahead_hints_issued counts the hints that actually
  // reached the kernel, which is the only observable the mechanism has.
  // The interval is Volume::kReadaheadReadviseSeconds (2 s); this case
  // reaches it only through behaviour, and gates both assertions on the
  // measured wall clock so a scheduling stall can never turn them flaky.
  constexpr auto kInterval = std::chrono::seconds(2);
  constexpr size_t kSize = 1 * kMB;

  TempCacheDir tmp("readahead_decay");
  auto cache = make_cache(tmp, readahead_config(std::nullopt));
  const CacheKey key("readahead-decay");
  write_doc(*cache, key, kSize, 55);

  // First read of the placement: exactly one hint.
  const uint64_t base = cache->stats().readahead_hints_issued;
  REQUIRE(read_and_verify(*cache, key, kSize, 55));
  const uint64_t after_first = cache->stats().readahead_hints_issued;
  REQUIRE(after_first == base + 1);

  // Hot re-reads inside the interval issue nothing.  Gated on elapsed time:
  // under one second the tick difference is at most 1, strictly inside the
  // 2 s interval, so the assertion only runs when its premise held.
  const auto burst_start = std::chrono::steady_clock::now();
  for (int i = 0; i < 20; ++i) {
    REQUIRE(read_and_verify(*cache, key, kSize, 55));
  }
  const auto burst_elapsed = std::chrono::steady_clock::now() - burst_start;
  const uint64_t after_burst = cache->stats().readahead_hints_issued;
  if (burst_elapsed < std::chrono::seconds(1)) {
    INFO("20 warm reads must not issue a hint inside the interval");
    REQUIRE(after_burst == after_first);
  }

  // Past the interval the next read re-advises.  Sleeping a whole interval
  // plus a margin makes the tick difference >= 2 wherever the first read
  // happened to fall inside its second.
  std::this_thread::sleep_for(kInterval + std::chrono::milliseconds(250));
  REQUIRE(read_and_verify(*cache, key, kSize, 55));
  REQUIRE(cache->stats().readahead_hints_issued == after_burst + 1);

  // The decay re-arms: the read right after it is filtered again.
  const uint64_t after_decay = cache->stats().readahead_hints_issued;
  const auto second_start = std::chrono::steady_clock::now();
  REQUIRE(read_and_verify(*cache, key, kSize, 55));
  if (std::chrono::steady_clock::now() - second_start <
      std::chrono::seconds(1)) {
    REQUIRE(cache->stats().readahead_hints_issued == after_decay);
  }
}

TEST_CASE("Documents below the threshold never issue a readahead hint",
          "[readahead][integration]") {
  // The counter pins the negative too: the small-object path stays exactly
  // as it was, whatever the threshold machinery does above it.
  TempCacheDir tmp("readahead_counter_off");
  auto cache = make_cache(tmp, readahead_config(std::nullopt));

  const CacheKey small("readahead-count-small");
  write_doc(*cache, small, 4 * kKB, 7);
  for (int i = 0; i < 5; ++i) {
    REQUIRE(read_and_verify(*cache, small, 4 * kKB, 7));
  }
  REQUIRE(cache->stats().readahead_hints_issued == 0);

  // readahead_min_bytes = 0 disables the hint at every size.
  TempCacheDir off_tmp("readahead_counter_zero");
  auto off = make_cache(off_tmp, readahead_config(0));
  const CacheKey large("readahead-count-large");
  write_doc(*off, large, 1 * kMB, 8);
  REQUIRE(read_and_verify(*off, large, 1 * kMB, 8));
  REQUIRE(off->stats().readahead_hints_issued == 0);
}
