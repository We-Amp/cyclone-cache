// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Cross-process RAM-cache coherence.
//
// THE EXPOSURE.  The RAM tier is process-local (a heap structure) while the
// directory is shared through the mmap'd file.  When a peer process
// re-records or purges content, this process's RAM copy of the superseded
// bytes is never evicted -- Stripe::remove_epoch, the guard that covers the
// same race locally, is deliberately process-local.  The peer's copy survives
// until its own LRU/CLFUS happens to reclaim it, so the stale bytes are served
// silently, with no error to observe.
//
// THE FIX, under CacheConfig::cross_process_ram_coherence (default OFF).  A
// RAM entry is stamped, at admission, with the shared seqlock version of the
// directory bucket its key hashes to; every RAM hit revalidates that stamp
// against the bucket's CURRENT version and drops the entry on a mismatch.  The
// signal is not new: every completed directory mutation already advances that
// version by +2 through acquire_writer/release_writer, in every build, past
// and future.  That is what makes the toggle a purely LOCAL statement -- see
// the asymmetric case below.
//
// THE VEHICLE.  Two Volume objects opened on ONE file, each with its own
// independent RamCache, in a single process.  That is exactly the
// cross-process-coherence topology (two private RAM tiers over one shared mmap
// directory) and it is fully deterministic -- no fork, no spawned peer, no
// timing.  The shape is borrowed from test_multiprocess_writers.cpp's
// VolumeView; unlike that file this one is NOT fork-based, so it runs on
// Windows too.
//
// WHY VOLUME AND NOT CACHE.  Cache owns exactly one RamCache and hands the
// same shared_ptr to every default-tier volume, so a Cache pair cannot express
// "two processes with private RAM tiers over one directory".  Volume can:
// set_ram_cache() is public and takes the tier per volume.
//
// WHAT EACH CASE PINS
//   T1  peer re-record          the headline fix, plus the OFF control that
//                               asserts the documented status quo
//   T2  reader ON, writer OFF   the toggle is LOCAL: it needs no agreement
//                               from peers.  Must exist.
//   T3  read_sync path          the path that serves (key, Original) from RAM
//                               without consulting the directory at all, and
//                               never repopulates -- where a removed head
//                               "kept being served indefinitely"
//   T4  middle/tail removal     the one directory mutation that does NOT move
//                               a DirEntry, with the writer running NO RAM
//                               cache of its own (the "one writer, many
//                               readers" shape).  Passes silently unless the
//                               publish is both present AND unconditional.
//   T5  single process + ON     inert: RAM hits unchanged, zero rejections
//   T8  counters                nonzero exactly when the feature fires

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/volume.hpp"
#include "cyclone/alternate.hpp"
#include "cyclone/cache.hpp"
#include "cyclone/config.hpp"
#include "cyclone/error.hpp"
#include "cyclone/key.hpp"
#include "ram_cache/ram_cache.hpp"
#include "support/temp_cache.hpp"

using namespace cyclone;

namespace {

// 64 MB: comfortably one auto-tiled stripe, so every key in a case lands on
// the same stripe and no run can wrap (a wrap legitimately drops content,
// which has nothing to do with coherence).
constexpr size_t kVolumeBytes = static_cast<size_t>(64) * 1024 * 1024;
constexpr size_t kRamBytes = static_cast<size_t>(8) * 1024 * 1024;

// Under the 32 KB RAM admission ceiling, so an entry qualifies for the RAM
// tier whatever the volume's mapping mode.
constexpr size_t kContentBytes = 4096;

// --- the two "process" views ----------------------------------------------

struct VolumeView {
  std::shared_ptr<Volume> volume;
  std::shared_ptr<RamCache> ram;  // null => this view has no RAM tier
  std::vector<std::shared_ptr<VolumeReadAnchor>> anchors;

  VolumeView() = default;
  VolumeView(const VolumeView &) = delete;
  VolumeView &operator=(const VolumeView &) = delete;

  // with_ram = false models a peer configured ram_cache_size == 0, which is
  // the house convention for a write-only peer (see tests/support/peer_main).
  bool open(const std::string &path, bool coherence, bool with_ram,
            RamCacheType ram_type = RamCacheType::LRU) {
    VolumeConfig vc;
    vc.path = path;
    vc.size = kVolumeBytes;
    vc.verify_checksum_on_read = true;
    vc.cross_process_ram_coherence = coherence;

    // The production multi-process shape: enabled, 0-of-1, in every process,
    // so every stripe is owned by every view and the mmap directory (and with
    // it the per-bucket version array) is the shared one.
    MultiProcessConfig mp;
    mp.set_enabled(true).set_process_index(0).set_total_processes(1);

    volume = std::make_shared<Volume>(vc, mp);
    if (!volume->open().has_value()) {
      volume.reset();
      return false;
    }
    anchors = volume->make_read_anchors();
    volume->set_read_anchors(anchors.data(), anchors.size());
    if (with_ram) {
      ram = RamCache::create(ram_type, kRamBytes);
      volume->set_ram_cache(ram);
    }
    return true;
  }

  ~VolumeView() {
    if (volume) {
      volume->set_read_anchors(nullptr, 0);
      volume->close();
    }
  }

  [[nodiscard]] uint64_t ram_hits() const {
    return ram ? ram->stats().hits : 0;
  }
  [[nodiscard]] uint64_t rejections() const {
    return volume->stats().ram_coherence_rejections;
  }
  [[nodiscard]] uint64_t put_rejections() const {
    return volume->stats().ram_coherence_put_rejections;
  }
};

// --- self-verifying payloads ----------------------------------------------

// Content that carries its own (id, version) stamp, so a served buffer is
// checked for identity AND self-consistency: a torn or spliced read fails.
std::vector<std::byte> stamped(AlternateId id, uint32_t version,
                               size_t size = kContentBytes) {
  std::vector<std::byte> v(size);
  const auto id8 = static_cast<uint8_t>(id);
  v[0] = static_cast<std::byte>(id8);
  v[1] = static_cast<std::byte>(version & 0xFF);
  v[2] = static_cast<std::byte>((version >> 8) & 0xFF);
  v[3] = static_cast<std::byte>((version >> 16) & 0xFF);
  for (size_t i = 4; i < size; ++i) {
    v[i] = static_cast<std::byte>((id8 * 31 + version * 7 + i) & 0xFF);
  }
  return v;
}

// Versions start at 1, so 0 is an unambiguous "no valid stamp" -- and unlike
// an optional it prints, so a failed assertion names what WAS served.
constexpr uint32_t kNoStamp = 0;

uint32_t stamp_of(std::span<const std::byte> got, AlternateId id) {
  if (got.size() < 4) {
    return kNoStamp;
  }
  const auto id8 = static_cast<uint8_t>(id);
  if (static_cast<uint8_t>(got[0]) != id8) {
    return kNoStamp;
  }
  const uint32_t version = static_cast<uint32_t>(got[1]) |
                           (static_cast<uint32_t>(got[2]) << 8) |
                           (static_cast<uint32_t>(got[3]) << 16);
  for (size_t i = 4; i < got.size(); ++i) {
    if (static_cast<uint8_t>(got[i]) != ((id8 * 31 + version * 7 + i) & 0xFF)) {
      return kNoStamp;
    }
  }
  return version;
}

// Selector that picks exactly one AlternateId, so a read can be aimed at a
// specific chain node.
class IdSelector : public StorageAlternateSelector {
 public:
  explicit IdSelector(AlternateId want) : _want(want) {}
  [[nodiscard]] std::optional<size_t> select(
      std::span<const AlternateInfo> alternates,
      const AlternateSelectionContext & /*ctx*/) const override {
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

// --- operations on a view --------------------------------------------------

bool write_alt(Volume &vol, const CacheKey &key, AlternateId id,
               std::span<const std::byte> content) {
  auto wh = vol.write_alternate_sync(key, id, content.size());
  if (!wh.has_value()) {
    return false;
  }
  if (!wh->write_sync(content).has_value()) {
    return false;
  }
  return wh->close_sync().has_value();
}

void write_alt_ok(Volume &vol, const CacheKey &key, AlternateId id,
                  uint32_t version) {
  REQUIRE(write_alt(vol, key, id, stamped(id, version)));
}

// Read through the ALTERNATE path -- the only read path that PUTS into RAM.
uint32_t read_alt_stamp(Volume &vol, const CacheKey &key, AlternateId id) {
  IdSelector sel(id);
  AlternateSelectionContext ctx;
  auto rh = vol.read_alternate_sync(key, sel, ctx);
  if (!rh.has_value()) {
    return kNoStamp;
  }
  return stamp_of(rh->content(), id);
}

// Read through the PLAIN path -- RAM-gets (key, Original) unconditionally and
// never repopulates.
uint32_t read_plain_stamp(Volume &vol, const CacheKey &key) {
  auto rh = vol.read_sync(key);
  if (!rh.has_value()) {
    return kNoStamp;
  }
  return stamp_of(rh->content(), AlternateId::Original);
}

// Populate this view's RAM tier for (key, id) AND PROVE it happened.
//
// Neither half is optional.  CLFUS runs a scan-resistance admission filter (a
// (key, id) it has not seen is marked, not admitted), so one read never
// populates; and a case that quietly failed to populate RAM would still pass
// every assertion below while testing nothing.  So the population is verified
// against this view's own RAM hit counter rather than assumed.
void warm_ram(VolumeView &view, const CacheKey &key, AlternateId id,
              uint32_t expect_version) {
  REQUIRE(view.ram != nullptr);
  for (int attempt = 0; attempt < 8; ++attempt) {
    const uint64_t before = view.ram_hits();
    REQUIRE(read_alt_stamp(*view.volume, key, id) == expect_version);
    if (view.ram_hits() > before) {
      return;  // that read was served from RAM: the entry is live
    }
  }
  FAIL(
      "RAM cache never served this (key, alternate) -- the staleness oracle "
      "would be vacuous");
}

}  // namespace

// ===========================================================================
// T1 -- a peer's re-record is not served from this process's RAM tier
// ===========================================================================

namespace {

// Returns the version the reader serves for `id` after the writer re-records
// it, having first warmed the reader's RAM tier with the OLD version.
struct RerecordOutcome {
  uint32_t served = kNoStamp;
  uint64_t rejections = 0;
  uint64_t reader_ram_hits = 0;
};

RerecordOutcome run_peer_rerecord(bool reader_coherence, bool writer_coherence,
                                  RamCacheType ram_type) {
  TempCacheDir tmp("ram_coh_rerecord");
  const std::string path = tmp.path();

  VolumeView writer;
  VolumeView reader;
  REQUIRE(writer.open(path, writer_coherence, /*with_ram=*/true, ram_type));
  REQUIRE(reader.open(path, reader_coherence, /*with_ram=*/true, ram_type));

  const CacheKey key("ram-coh-rerecord");
  constexpr auto id = AlternateId::Brotli;

  write_alt_ok(*writer.volume, key, id, 1);

  // The reader pulls v1 into ITS OWN RAM tier and proves it is live there.
  warm_ram(reader, key, id, 1);

  // The peer supersedes it.  This is an ordinary alternate write: it publishes
  // a new head through the directory, which advances the shared bucket
  // version by +2 as a side effect of the existing seqlock -- nothing in the
  // coherence work added.  The writer's own RAM tier is invalidated by its own
  // remove_epoch; the reader's is not reachable from here at all.
  write_alt_ok(*writer.volume, key, id, 2);

  RerecordOutcome out;
  out.served = read_alt_stamp(*reader.volume, key, id);
  out.rejections = reader.rejections();
  out.reader_ram_hits = reader.ram_hits();
  return out;
}

}  // namespace

TEST_CASE("A peer's re-record is not served from RAM with coherence ON",
          "[ram][coherence][multiprocess]") {
  const auto run = [](RamCacheType type) {
    // T1 (ON): the headline fix.
    const auto on = run_peer_rerecord(/*reader_coherence=*/true,
                                      /*writer_coherence=*/true, type);
    CAPTURE(on.served, on.rejections, on.reader_ram_hits);
    REQUIRE(on.served == 2);
    // T8: the counter is nonzero exactly when the feature fires, and the
    // reader really did have a live RAM entry to reject.
    REQUIRE(on.rejections >= 1);
    REQUIRE(on.reader_ram_hits >= 1);

    // T1 (OFF control): asserts the DOCUMENTED STATUS QUO, not an
    // aspiration.  Without the toggle the reader keeps serving the
    // superseded bytes -- that is the historical behaviour every existing
    // consumer has, and the reason the fix is opt-in rather than a silent
    // change.  If this ever stops holding, the toggle is no longer the only
    // thing gating the behaviour and this file's OFF/ON contrast is a lie.
    const auto off = run_peer_rerecord(/*reader_coherence=*/false,
                                       /*writer_coherence=*/false, type);
    CAPTURE(off.served, off.rejections);
    REQUIRE(off.served == 1);
    REQUIRE(off.rejections == 0);
  };
  SECTION("LRU") { run(RamCacheType::LRU); }
  SECTION("CLFUS (production default)") { run(RamCacheType::CLFUS); }
}

// ===========================================================================
// T2 -- ASYMMETRIC toggle: reader ON, writer OFF, reader still coherent
// ===========================================================================

TEST_CASE("Coherence is local: reader ON stays coherent against a writer OFF",
          "[ram][coherence][multiprocess]") {
  // The design's headline claim.  The reader's protection cannot depend on
  // the peer opting in, because the invalidation signal the reader reads is
  // the seqlock version every directory mutation already publishes -- a peer
  // running a binary without the toggle publishes it just the same.  If this
  // ever regressed to needing peer agreement, the toggle would become a
  // cross-process protocol with a "looks protected but isn't" cliff.
  const auto run = [](RamCacheType type) {
    const auto out = run_peer_rerecord(/*reader_coherence=*/true,
                                       /*writer_coherence=*/false, type);
    CAPTURE(out.served, out.rejections, out.reader_ram_hits);
    REQUIRE(out.served == 2);
    REQUIRE(out.rejections >= 1);
    REQUIRE(out.reader_ram_hits >= 1);
  };
  SECTION("LRU") { run(RamCacheType::LRU); }
  SECTION("CLFUS (production default)") { run(RamCacheType::CLFUS); }
}

// ===========================================================================
// T3 -- the read_sync path: a peer-removed Original head
// ===========================================================================

namespace {

struct RemovedHeadOutcome {
  uint32_t served_plain = kNoStamp;
  uint64_t rejections = 0;
};

RemovedHeadOutcome run_peer_removes_original_head(bool reader_coherence,
                                                  RamCacheType ram_type) {
  TempCacheDir tmp("ram_coh_head");
  const std::string path = tmp.path();

  VolumeView writer;
  VolumeView reader;
  REQUIRE(writer.open(path, /*coherence=*/false, /*with_ram=*/true, ram_type));
  REQUIRE(reader.open(path, reader_coherence, /*with_ram=*/true, ram_type));

  const CacheKey key("ram-coh-head");

  // A sibling id keeps the KEY alive after Original is removed, so the
  // directory entry is REPOINTED rather than deleted -- the head-removal
  // shape.  Original is written last, so it is the chain head.
  write_alt_ok(*writer.volume, key, AlternateId::Brotli, 1);
  write_alt_ok(*writer.volume, key, AlternateId::Original, 1);

  // Populate the reader's RAM with (key, Original) through the alternate
  // path -- the only path that puts -- then confirm the PLAIN path serves it
  // from RAM.  read_sync consults no directory at all on a RAM hit, which is
  // exactly why a removed head "kept being served indefinitely" here.
  warm_ram(reader, key, AlternateId::Original, 1);
  REQUIRE(read_plain_stamp(*reader.volume, key) == 1);

  REQUIRE(writer.volume->remove_alternate_sync(key, AlternateId::Original)
              .has_value());

  RemovedHeadOutcome out;
  out.served_plain = read_plain_stamp(*reader.volume, key);
  out.rejections = reader.rejections();
  return out;
}

}  // namespace

TEST_CASE("A peer-removed Original head is not served by read_sync from RAM",
          "[ram][coherence][multiprocess]") {
  const auto run = [](RamCacheType type) {
    const auto on = run_peer_removes_original_head(true, type);
    CAPTURE(on.served_plain, on.rejections);
    // The Original is gone from the chain; the plain path must not resurrect
    // it out of RAM.  Serving the surviving Brotli sibling would fail
    // stamp_of() for Original and read as kNoStamp -- also acceptable, and
    // what the assertion below allows.
    REQUIRE(on.served_plain == kNoStamp);
    REQUIRE(on.rejections >= 1);

    // Status-quo control.
    const auto off = run_peer_removes_original_head(false, type);
    CAPTURE(off.served_plain, off.rejections);
    REQUIRE(off.served_plain == 1);
    REQUIRE(off.rejections == 0);
  };
  SECTION("LRU") { run(RamCacheType::LRU); }
  SECTION("CLFUS (production default)") { run(RamCacheType::CLFUS); }
}

// ===========================================================================
// T4 -- middle/tail alternate removal, writer with NO RAM cache
// ===========================================================================

namespace {

struct MidChainOutcome {
  uint32_t served_plain = kNoStamp;
  uint64_t rejections = 0;
  size_t alternates_before = 0;
  AlternateId head_before = AlternateId::Original;
};

// The one directory mutation that does NOT move a DirEntry: removing a
// middle/tail alternate rewrites the PREDECESSOR DOCUMENT's
// next_alternate_offset in place, under the global directory write lock.  The
// head entry -- and therefore the bucket version -- stays untouched unless the
// removal explicitly publishes one, so a coherence-enabled peer would keep
// serving the alternate that was just unlinked.
//
// The removed id is ORIGINAL, sitting at the chain TAIL, and the oracle is the
// PLAIN read path.  That combination is what makes the case observable at all:
// read_alternate_sync walks the chain and simply stops finding an unlinked id,
// so it can never serve it stale.  read_sync answers (key, Original) straight
// out of RAM without consulting the directory -- the path on which an unlinked
// Original "kept being served indefinitely".
//
// The writer here has NO RAM cache, which is the whole point: that is the
// "one writer, many readers" deployment this feature targets, and it is what
// separates a publish sited OUTSIDE the writer's own RAM-invalidation block
// from one nested inside it.  Nested, this case passes silently.
MidChainOutcome run_peer_removes_midchain(bool reader_coherence,
                                          RamCacheType ram_type) {
  TempCacheDir tmp("ram_coh_midchain");
  const std::string path = tmp.path();

  VolumeView writer;
  VolumeView reader;
  REQUIRE(writer.open(path, /*coherence=*/false, /*with_ram=*/false));
  REQUIRE(reader.open(path, reader_coherence, /*with_ram=*/true, ram_type));

  const CacheKey key("ram-coh-midchain");

  // Alternate writes PREPEND, so after these three the chain is, head first:
  // Gzip -> Brotli -> Original.  Original is strictly at the tail.
  write_alt_ok(*writer.volume, key, AlternateId::Original, 1);
  write_alt_ok(*writer.volume, key, AlternateId::Brotli, 1);
  write_alt_ok(*writer.volume, key, AlternateId::Gzip, 1);

  MidChainOutcome out;
  auto alts = writer.volume->list_alternates_sync(key);
  REQUIRE(alts.has_value());
  out.alternates_before = alts->size();
  REQUIRE_FALSE(alts->empty());
  out.head_before = alts->front().id;

  // Warm the reader's RAM with (key, Original) AFTER every write, so the stamp
  // it carries is the post-write bucket version and only the removal can move
  // it -- then confirm the plain path really is serving from RAM.
  warm_ram(reader, key, AlternateId::Original, 1);
  REQUIRE(read_plain_stamp(*reader.volume, key) == 1);

  REQUIRE(writer.volume->remove_alternate_sync(key, AlternateId::Original)
              .has_value());

  out.served_plain = read_plain_stamp(*reader.volume, key);
  out.rejections = reader.rejections();
  return out;
}

}  // namespace

TEST_CASE("A peer's tail alternate removal reaches a RAM reader",
          "[ram][coherence][multiprocess]") {
  const auto run = [](RamCacheType type) {
    const auto on = run_peer_removes_midchain(true, type);
    CAPTURE(on.served_plain, on.rejections, on.alternates_before);
    // Non-vacuousness: the removal really did take the middle/tail branch
    // (three alternates, and the removed id was not the chain head).
    REQUIRE(on.alternates_before == 3);
    REQUIRE(on.head_before != AlternateId::Original);
    // The unlinked Original must not be served out of the reader's RAM.  The
    // plain path falls through to the surviving chain head, whose bytes fail
    // stamp_of() for Original and so read as kNoStamp.
    REQUIRE(on.served_plain == kNoStamp);
    REQUIRE(on.rejections >= 1);

    // Status-quo control.
    const auto off = run_peer_removes_midchain(false, type);
    CAPTURE(off.served_plain, off.rejections);
    REQUIRE(off.served_plain == 1);
    REQUIRE(off.rejections == 0);
  };
  SECTION("LRU") { run(RamCacheType::LRU); }
  SECTION("CLFUS (production default)") { run(RamCacheType::CLFUS); }
}

// ===========================================================================
// T5 -- single process + toggle ON is INERT
// ===========================================================================

namespace {

struct SingleProcessOutcome {
  uint64_t ram_hits = 0;
  uint64_t rejections = 0;
  uint64_t put_rejections = 0;
  bool active = false;
};

// Deliberately a real Cache in SINGLE-process mode (no mmap directory), which
// is the configuration the guard is about: without the mmap directory there
// is no shared version array, so a naive "stamp 0 means unstamped, treat as
// stale" rule would reject EVERY hit and silently disable the RAM tier.
SingleProcessOutcome run_single_process(bool coherence, RamCacheType ram_type) {
  TempCacheDir tmp("ram_coh_single");
  CacheConfig cfg;
  cfg.ram_cache_size = kRamBytes;
  cfg.ram_cache_type = ram_type;
  cfg.cross_process_ram_coherence = coherence;

  auto created = Cache::create(cfg);
  REQUIRE(created.has_value());
  auto cache = std::move(*created);
  VolumeConfig vc;
  vc.path = tmp.path();
  vc.size = kVolumeBytes;
  REQUIRE(cache->add_volume(vc).has_value());
  REQUIRE(cache->start().has_value());

  const CacheKey key("ram-coh-single");
  constexpr auto id = AlternateId::Brotli;
  const auto content = stamped(id, 1);
  auto wh = cache->write_alternate_sync(key, id, content.size());
  REQUIRE(wh.has_value());
  REQUIRE(wh->write_sync(content).has_value());
  REQUIRE(wh->close_sync().has_value());

  IdSelector sel(id);
  AlternateSelectionContext ctx;
  for (int i = 0; i < 12; ++i) {
    auto rh = cache->read_alternate_sync(key, sel, ctx);
    REQUIRE(rh.has_value());
    REQUIRE(stamp_of(rh->content(), id) == 1);
  }

  SingleProcessOutcome out;
  const auto st = cache->stats();
  out.ram_hits = st.ram_cache_hits;
  out.rejections = st.ram_coherence_rejections;
  out.put_rejections = st.ram_coherence_put_rejections;
  out.active = cache->cross_process_ram_coherence_active();
  cache->stop();
  return out;
}

}  // namespace

TEST_CASE("Single-process with coherence ON is inert",
          "[ram][coherence][regression]") {
  const auto run = [](RamCacheType type) {
    const auto off = run_single_process(false, type);
    const auto on = run_single_process(true, type);
    CAPTURE(off.ram_hits, on.ram_hits, on.rejections, on.put_rejections);

    // The RAM tier must still be serving: identical hit count to the control,
    // and not zero (a zero would make the comparison vacuous).
    REQUIRE(off.ram_hits > 0);
    REQUIRE(on.ram_hits == off.ram_hits);

    // Nothing was rejected and nothing was declined -- the toggle changed
    // nothing at all without a shared directory to validate against.
    REQUIRE(on.rejections == 0);
    REQUIRE(on.put_rejections == 0);
    REQUIRE(off.rejections == 0);
    REQUIRE(off.put_rejections == 0);

    // And the accessor says so, rather than leaving the operator with a false
    // sense of safety.
    REQUIRE_FALSE(on.active);
    REQUIRE_FALSE(off.active);
  };
  SECTION("LRU") { run(RamCacheType::LRU); }
  SECTION("CLFUS (production default)") { run(RamCacheType::CLFUS); }
}

TEST_CASE("cross_process_ram_coherence_active reports the three preconditions",
          "[ram][coherence]") {
  const auto make = [](bool coherence, bool mp, size_t ram_bytes) {
    CacheConfig cfg;
    cfg.ram_cache_size = ram_bytes;
    cfg.cross_process_ram_coherence = coherence;
    if (mp) {
      cfg.set_multi_process(0, 1);
    }
    return cfg;
  };
  const auto active = [](const CacheConfig &cfg) {
    TempCacheDir tmp("ram_coh_active");
    auto created = Cache::create(cfg);
    REQUIRE(created.has_value());
    auto cache = std::move(*created);
    VolumeConfig vc;
    vc.path = tmp.path();
    vc.size = kVolumeBytes;
    REQUIRE(cache->add_volume(vc).has_value());
    REQUIRE(cache->start().has_value());
    const bool a = cache->cross_process_ram_coherence_active();
    cache->stop();
    return a;
  };

  REQUIRE(active(make(true, true, kRamBytes)));
  // Each precondition removed in turn -- all inert, none an error.
  REQUIRE_FALSE(active(make(false, true, kRamBytes)));
  REQUIRE_FALSE(active(make(true, false, kRamBytes)));
  REQUIRE_FALSE(active(make(true, true, 0)));
}
