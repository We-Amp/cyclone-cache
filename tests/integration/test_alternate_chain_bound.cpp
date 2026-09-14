// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Bound on alternate-chain depth: re-recording an alternate id must not grow
// the physical chain.
//
// Before the bound existed, every re-record of an id prepended a new document
// and left the superseded one linked, so a key refreshed on a TTL grew a chain
// of identical-id "shadow" nodes until the traversal cap made the key
// permanently unwritable (TooManyAlternates) — a wedge no traffic could clear.
// The write now splices the superseded same-id node(s) out during the walk it
// already performs, so depth is bounded by the number of DISTINCT ids.
//
// What each case is for:
//
//   * DEPTH BOUND + RED PROOF — 250 re-records of one id stay at depth 1, and
//     the same run with the kill switch off shows depth climbing to the
//     traversal cap.  The RED half is what keeps the GREEN half honest: it
//     proves the assertion measures the mechanism and not the test.
//   * MID-CHAIN SPLICE (gate) — the one genuinely new code path: an in-place
//     store into a live published document's header, under the write lock and
//     the wrap-epoch fence.  Nothing else in this file executes it.
//   * CONCURRENT READER (gate) — the live hazard is writer-vs-reader (one
//     process owns a stripe's writes), so readers walk the chain while the
//     writer repoints it.
//   * CYCLIC CHAIN — the walk must terminate on a chain that points into
//     itself, and must NOT perform an in-place repoint there (the proof that a
//     splice cannot create a cycle assumes an acyclic walk).
//   * CHAIN RESET at the cap, and its negative: a multi-id chain must still be
//     rejected rather than silently truncated.
//   * SERVED-VERSION ORACLE — the property a consumer actually cares about:
//     after a re-record, the NEW bytes are what the cache serves.
//
// Raw on-disk header inspection is POSIX-only (the Windows fill goes through a
// mapped view, which a stdio read of the same file need not observe).  Every
// behavioural assertion runs on both platforms.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "core/document.hpp"
#include "cyclone/alternate.hpp"
#include "cyclone/cache.hpp"
#include "cyclone/config.hpp"
#include "cyclone/key.hpp"
#include "support/temp_cache.hpp"

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

using namespace cyclone;

namespace {

constexpr size_t kVolumeBytes = static_cast<size_t>(64) * 1024 * 1024;

// RAM cache OFF everywhere in this file.  These cases are about what is on
// disk and what the disk path serves; a RAM cache in front would answer reads
// from a copy taken before the re-record (the RAM write-around
// pinning) and mask exactly what is being measured.  Disabling it is a
// deliberate scoping decision, NOT a workaround that shrinks the oracle: see
// the served-version case below.
CacheConfig bound_config(bool unlink_superseded = true) {
  CacheConfig c;
  c.ram_cache_size = 0;
  c.unlink_superseded_alternates = unlink_superseded;
  return c;
}

std::unique_ptr<Cache> make_cache(const TempCacheDir& dir,
                                  const CacheConfig& cfg,
                                  size_t size = kVolumeBytes,
                                  size_t stripe_size = 0) {
  auto created = Cache::create(cfg);
  REQUIRE(created.has_value());
  auto cache = std::move(*created);
  VolumeConfig vc;
  vc.path = dir.path();
  vc.size = size;
  vc.stripe_size = stripe_size;
  REQUIRE(cache->add_volume(vc).has_value());
  REQUIRE(cache->start().has_value());
  return cache;
}

// Content that carries its own (id, version) stamp, so a served buffer can be
// checked for BOTH identity and self-consistency: the tail bytes are derived
// from the stamp, so a torn or spliced-together read fails the check.
std::vector<std::byte> stamped(AlternateId id, uint32_t version,
                               size_t size = 4096) {
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

// Inverse of stamped(): returns the version iff the buffer is a well-formed,
// self-consistent stamp for `id`, else nullopt.
std::optional<uint32_t> stamp_of(std::span<const std::byte> got,
                                 AlternateId id) {
  if (got.size() < 4) {
    return std::nullopt;
  }
  const auto id8 = static_cast<uint8_t>(id);
  if (static_cast<uint8_t>(got[0]) != id8) {
    return std::nullopt;
  }
  const uint32_t version = static_cast<uint32_t>(got[1]) |
                           (static_cast<uint32_t>(got[2]) << 8) |
                           (static_cast<uint32_t>(got[3]) << 16);
  for (size_t i = 4; i < got.size(); ++i) {
    if (static_cast<uint8_t>(got[i]) != ((id8 * 31 + version * 7 + i) & 0xFF)) {
      return std::nullopt;
    }
  }
  return version;
}

bool put_alt(Cache& cache, const CacheKey& key, AlternateId id,
             std::span<const std::byte> content) {
  auto wh = cache.write_alternate_sync(key, id, content.size());
  if (!wh.has_value()) {
    return false;
  }
  if (!wh->write_sync(content).has_value()) {
    return false;
  }
  return wh->close_sync().has_value();
}

// Asserting variant: fails the test on any write error.  Never call from a
// worker thread (Catch2 assertions are not thread-safe).
void put_alt_ok(Cache& cache, const CacheKey& key, AlternateId id,
                std::span<const std::byte> content) {
  REQUIRE(put_alt(cache, key, id, content));
}

// Selector that picks exactly one AlternateId, so a read can be aimed at a
// specific chain node.
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

// Read one alternate and return its stamp, or nullopt on any miss/mismatch.
std::optional<uint32_t> read_stamp(Cache& cache, const CacheKey& key,
                                   AlternateId id) {
  IdSelector sel(id);
  AlternateSelectionContext ctx;
  auto rh = cache.read_alternate_sync(key, sel, ctx);
  if (!rh.has_value()) {
    return std::nullopt;
  }
  return stamp_of(rh->content(), id);
}

#ifndef _WIN32
// The volume file the cache actually opened (fingerprinted name).
std::string volume_file(Cache& cache) {
  auto files = cache.volume_files();
  REQUIRE(files.size() >= 1);
  return files[0].file_path;
}

// Read a document's next_alternate_offset straight from the volume file.
// `doc_abs_offset` is AlternateInfo::disk_offset, which is already absolute.
uint64_t read_next_link(const std::string& path, uint64_t doc_abs_offset) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  REQUIRE(fd >= 0);
  uint64_t value = 0;
  const ssize_t n = ::pread(
      fd, &value, sizeof(value),
      static_cast<off_t>(doc_abs_offset + Document::kNextAlternateOffsetPos));
  ::close(fd);
  REQUIRE(n == static_cast<ssize_t>(sizeof(value)));
  return value;
}

// Overwrite a document's next_alternate_offset in the volume file.  Used only
// to FABRICATE a cyclic chain — nothing in the library can produce one on
// demand, and the walk's termination on a cycle is a load-bearing property.
void write_next_link(const std::string& path, uint64_t doc_abs_offset,
                     uint64_t value) {
  const int fd = ::open(path.c_str(), O_WRONLY);
  REQUIRE(fd >= 0);
  const ssize_t n = ::pwrite(
      fd, &value, sizeof(value),
      static_cast<off_t>(doc_abs_offset + Document::kNextAlternateOffsetPos));
  ::fsync(fd);
  ::close(fd);
  REQUIRE(n == static_cast<ssize_t>(sizeof(value)));
}

// Find one enumerated alternate by id.
const AlternateInfo* find_alt(const std::vector<AlternateInfo>& alts,
                              AlternateId id) {
  for (const auto& a : alts) {
    if (a.id == id) {
      return &a;
    }
  }
  return nullptr;
}
#endif  // !_WIN32

}  // namespace

// ---------------------------------------------------------------------------
// Depth bound, and the RED proof that the mechanism is what bounds it
// ---------------------------------------------------------------------------
TEST_CASE("Re-recording one alternate id keeps the chain at depth 1",
          "[alternate][chain][regression]") {
  constexpr uint32_t kWrites = 250;  // 10x the "depth bounded at N=200+" bar
  const CacheKey key("chain-bound-ladder");

  SECTION("with the unlink enabled: depth stays 1 for every refresh") {
    TempCacheDir tmp("chain_ladder");
    auto cache = make_cache(tmp, bound_config(/*unlink_superseded=*/true));

    for (uint32_t n = 1; n <= kWrites; ++n) {
      const auto content = stamped(AlternateId::Original, n);
      // Not one write may fail: the wedge this bound exists to prevent shows
      // up exactly here.
      REQUIRE(put_alt(*cache, key, AlternateId::Original, content));

      if (n % 25 == 0 || n <= 3 || n == kWrites) {
        auto alts = cache->list_alternates_sync(key);
        REQUIRE(alts.has_value());
        REQUIRE(alts->size() == 1);  // physical depth, not a logical view
        REQUIRE((*alts)[0].id == AlternateId::Original);
        REQUIRE(read_stamp(*cache, key, AlternateId::Original) == n);
      }
    }

    auto st = cache->stats();
    // Every write after the first saw a depth-1 chain and unlinked exactly one
    // superseded node.
    REQUIRE(st.alternate_max_chain_depth == 1);
    REQUIRE(st.alternate_shadows_unlinked == kWrites - 1);
    REQUIRE(st.alternate_splice_deferred == 0);
    REQUIRE(st.alternate_chain_resets == 0);
    REQUIRE(st.alternate_wrap_refusals == 0);  // no wrap in this run
    cache->stop();
  }

  SECTION("RED proof — with the unlink disabled the depth is unbounded") {
    // Same traffic against the kill switch.  If the GREEN section above were
    // measuring something other than the mechanism, this section would look
    // like it too.
    TempCacheDir tmp("chain_ladder_red");
    auto cache = make_cache(tmp, bound_config(/*unlink_superseded=*/false));

    for (uint32_t n = 1; n <= kWrites; ++n) {
      REQUIRE(put_alt(*cache, key, AlternateId::Original,
                      stamped(AlternateId::Original, n)));
    }

    auto st = cache->stats();
    REQUIRE(st.alternate_shadows_unlinked == 0);
    // Depth ran away to the traversal cap instead of staying at 1.
    REQUIRE(st.alternate_max_chain_depth >= 100);
    // ... and the only thing that kept the key writable at that point was the
    // chain reset at the cap (the backstop, which is deliberately NOT gated by
    // the kill switch).  Note what this means: the wedge no longer reproduces
    // even with the mechanism off, because the backstop catches it.
    REQUIRE(st.alternate_chain_resets >= 1);
    // The newest version still serves, through a 100+ node chain.
    REQUIRE(read_stamp(*cache, key, AlternateId::Original) == kWrites);
    cache->stop();
  }
}

// ---------------------------------------------------------------------------
// MERGE GATE: the mid-chain splice — the in-place store on a published header
// ---------------------------------------------------------------------------
TEST_CASE("A superseded alternate in the middle of a chain is spliced out",
          "[alternate][chain][gate]") {
  TempCacheDir tmp("chain_midsplice");
  auto cache = make_cache(tmp, bound_config());
  const CacheKey key("chain-bound-midsplice");

  // Build [Brotli, Original-v1]: Original first, then Brotli, so the
  // superseded Original sits BEHIND a surviving node.  That is what forces the
  // in-place repoint — the free build-time splice only covers a superseded
  // HEAD.
  const auto orig_v1 = stamped(AlternateId::Original, 1);
  const auto brotli = stamped(AlternateId::Brotli, 1);
  put_alt_ok(*cache, key, AlternateId::Original, orig_v1);
  put_alt_ok(*cache, key, AlternateId::Brotli, brotli);

  {
    auto alts = cache->list_alternates_sync(key);
    REQUIRE(alts.has_value());
    REQUIRE(alts->size() == 2);
    REQUIRE((*alts)[0].id == AlternateId::Brotli);    // newest first
    REQUIRE((*alts)[1].id == AlternateId::Original);  // the node to supersede
  }

  const auto before = cache->stats();
#ifndef _WIN32
  uint64_t brotli_offset = 0;
  uint64_t orig_v1_link = 0;
  {
    auto alts = cache->list_alternates_sync(key);
    REQUIRE(alts.has_value());
    const AlternateInfo* b = find_alt(*alts, AlternateId::Brotli);
    const AlternateInfo* o = find_alt(*alts, AlternateId::Original);
    REQUIRE(b != nullptr);
    REQUIRE(o != nullptr);
    brotli_offset = b->disk_offset;
    // Brotli currently points AT the Original we are about to supersede.
    orig_v1_link = read_next_link(volume_file(*cache), brotli_offset);
    REQUIRE(orig_v1_link != 0);
  }
#endif

  // Re-record Original.  The new head links to Brotli (unchanged), so the ONLY
  // way the old Original can leave the chain is the in-place repoint of
  // Brotli's next_alternate_offset.
  put_alt_ok(*cache, key, AlternateId::Original,
             stamped(AlternateId::Original, 2));

  const auto after = cache->stats();
  REQUIRE(after.alternate_shadows_unlinked -
              before.alternate_shadows_unlinked ==
          1);
  REQUIRE(after.alternate_splice_deferred == before.alternate_splice_deferred);
  REQUIRE(after.alternate_chain_resets == before.alternate_chain_resets);

  // Depth did not grow, the newest Original serves, and Brotli is untouched.
  auto alts = cache->list_alternates_sync(key);
  REQUIRE(alts.has_value());
  REQUIRE(alts->size() == 2);
  REQUIRE((*alts)[0].id == AlternateId::Original);
  REQUIRE((*alts)[1].id == AlternateId::Brotli);
  REQUIRE(read_stamp(*cache, key, AlternateId::Original) == 2);
  REQUIRE(read_stamp(*cache, key, AlternateId::Brotli) == 1);

#ifndef _WIN32
  // Direct evidence of the in-place store: Brotli is the SAME document (same
  // offset — it was not rewritten) and its link now terminates the chain
  // instead of pointing at the superseded Original.
  const AlternateInfo* b_after = find_alt(*alts, AlternateId::Brotli);
  REQUIRE(b_after != nullptr);
  REQUIRE(b_after->disk_offset == brotli_offset);
  REQUIRE(read_next_link(volume_file(*cache), brotli_offset) == 0);
#endif

  cache->stop();
}

// ---------------------------------------------------------------------------
// MERGE GATE: readers walking a chain the writer is splicing
// ---------------------------------------------------------------------------
TEST_CASE("Readers never observe a torn chain while a writer splices",
          "[alternate][chain][gate][concurrency]") {
  // Writer-vs-writer on one chain is excluded structurally (exactly one
  // process owns a stripe's writes, and stripe->mutex serializes writers
  // within it), so the surface that is actually live is writer-vs-READER: the
  // splice stores 8 bytes into a published document's fixed header while
  // lock-free readers deserialize that same header.
  //
  // Alternating two ids makes every single write take the mid-chain path:
  // with chain [X, Y], writing Y supersedes the tail Y, so the surviving X is
  // repointed in place.  That is the store under test, on every iteration.
  //
  // Sized so the run cannot WRAP the key's stripe (one 256MB stripe, ~64MB
  // written).  A wrap would let a reader legitimately return Corrupted after
  // exhausting its retries — a documented degradation that has nothing to do
  // with the splice, and would make the "no Corrupted" oracle meaningless.
  // The wrap count is asserted at the end so this stays true.
  constexpr size_t k256MB = static_cast<size_t>(256) * 1024 * 1024;
  TempCacheDir tmp("chain_races");
  auto cache = make_cache(tmp, bound_config(), k256MB, k256MB);
  const CacheKey key("chain-bound-races");

  put_alt_ok(*cache, key, AlternateId::Original,
             stamped(AlternateId::Original, 1));
  put_alt_ok(*cache, key, AlternateId::Brotli, stamped(AlternateId::Brotli, 1));

  std::atomic<bool> stop{false};
  std::atomic<uint32_t> writes{0};
  // Catch2 macros are not thread-safe: workers only flip flags.
  std::atomic<bool> saw_error{false};       // a hard read failure
  std::atomic<bool> saw_torn{false};        // content failed its own stamp
  std::atomic<bool> saw_deep_chain{false};  // depth exceeded #distinct ids
  std::atomic<bool> saw_regression{false};  // a served version went backwards
  std::atomic<uint32_t> max_depth_seen{0};  // deepest chain any reader saw

  std::atomic<int> write_error{-1};

  std::thread writer([&] {
    uint32_t v = 2;
    while (!stop.load(std::memory_order_relaxed)) {
      const auto id =
          (v % 2 == 0) ? AlternateId::Original : AlternateId::Brotli;
      auto wh = cache->write_alternate_sync(key, id, 4096);
      if (!wh.has_value()) {
        write_error.store(static_cast<int>(wh.error()));
        return;
      }
      const auto content = stamped(id, v);
      (void)wh->write_sync(std::span<const std::byte>(content));
      auto closed = wh->close_sync();
      if (!closed.has_value()) {
        write_error.store(static_cast<int>(closed.error()));
        return;
      }
      writes.fetch_add(1, std::memory_order_relaxed);
      ++v;
    }
  });

  auto reader_body = [&] {
    uint32_t last_orig = 0;
    uint32_t last_brotli = 0;
    while (!stop.load(std::memory_order_relaxed)) {
      auto alts = cache->list_alternates_sync(key);
      if (alts.has_value()) {
        // Bound: one node per distinct id, PLUS at most one superseded node
        // still linked.  That extra node is the ordering the design mandates
        // being visible — the write publishes its new head first and splices
        // second, so a reader between the two steps sees [new, keeper,
        // superseded].  Both states are well-formed lists; the reverse order
        // would instead open a window with no reachable version of the id.
        // Anything DEEPER would mean shadows are accumulating.
        if (alts->size() > 3) {
          saw_deep_chain.store(true);
        }
        uint32_t seen = static_cast<uint32_t>(alts->size());
        uint32_t prev = max_depth_seen.load(std::memory_order_relaxed);
        while (seen > prev &&
               !max_depth_seen.compare_exchange_weak(prev, seen)) {
        }
      } else if (alts.error() == CacheError::Corrupted ||
                 alts.error() == CacheError::ChainCorrupted) {
        saw_error.store(true);
      }

      for (auto id : {AlternateId::Original, AlternateId::Brotli}) {
        IdSelector sel(id);
        AlternateSelectionContext ctx;
        auto rh = cache->read_alternate_sync(key, sel, ctx);
        if (!rh.has_value()) {
          if (rh.error() == CacheError::Corrupted ||
              rh.error() == CacheError::ChainCorrupted) {
            saw_error.store(true);
          }
          continue;  // a transient miss is legal; wrong bytes are not
        }
        auto stamp = stamp_of(rh->content(), id);
        if (!stamp) {
          saw_torn.store(true);  // wrong id, or bytes not self-consistent
          continue;
        }
        uint32_t& last =
            (id == AlternateId::Original) ? last_orig : last_brotli;
        if (*stamp < last) {
          saw_regression.store(true);
        }
        last = *stamp;
      }
    }
  };

  std::thread r1(reader_body);
  std::thread r2(reader_body);

  // Long enough to cover many thousands of splices; bounded so the suite stays
  // quick.
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < deadline &&
         writes.load(std::memory_order_relaxed) < 15000) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  stop.store(true);
  writer.join();
  r1.join();
  r2.join();

  CAPTURE(write_error.load(), writes.load(), max_depth_seen.load());
  REQUIRE(write_error.load() == -1);  // no write failed under reader pressure
  REQUIRE(writes.load() > 1000);      // the race window was actually exercised
  REQUIRE_FALSE(saw_error.load());
  REQUIRE_FALSE(saw_torn.load());
  REQUIRE_FALSE(saw_deep_chain.load());
  REQUIRE_FALSE(saw_regression.load());

  // The bound held under contention, and the splice really ran.
  auto st = cache->stats();
  REQUIRE(st.write_buffer_wraps == 0);  // keeps the "no Corrupted" oracle real
  REQUIRE(st.alternate_max_chain_depth <= 2);
  REQUIRE(st.alternate_shadows_unlinked > 1000);
  // Quiescent: no writer, so no publish-before-splice window is open — the
  // chain is back to exactly one node per distinct id.
  auto alts = cache->list_alternates_sync(key);
  REQUIRE(alts.has_value());
  REQUIRE(alts->size() == 2);
  cache->stop();
}

#ifndef _WIN32
// ---------------------------------------------------------------------------
// Cyclic chains: terminate the walk, never repoint in place
// ---------------------------------------------------------------------------
TEST_CASE("A cyclic alternate chain terminates the walk and heals",
          "[alternate][chain][cycle]") {
  TempCacheDir tmp("chain_cycle");
  auto cache = make_cache(tmp, bound_config());
  const CacheKey key("chain-bound-cycle");

  // [Original, Brotli], so Original's link gives us the stripe base needed to
  // express an offset the way the on-disk field does (relative to the stripe).
  put_alt_ok(*cache, key, AlternateId::Brotli, stamped(AlternateId::Brotli, 1));
  put_alt_ok(*cache, key, AlternateId::Original,
             stamped(AlternateId::Original, 1));

  auto alts = cache->list_alternates_sync(key);
  REQUIRE(alts.has_value());
  REQUIRE(alts->size() == 2);
  const AlternateInfo* head = find_alt(*alts, AlternateId::Original);
  const AlternateInfo* tail = find_alt(*alts, AlternateId::Brotli);
  REQUIRE(head != nullptr);
  REQUIRE(tail != nullptr);

  const std::string file = volume_file(*cache);
  const uint64_t tail_rel = read_next_link(file, head->disk_offset);
  REQUIRE(tail_rel != 0);
  // disk_offset is absolute; the link field is stripe-relative.  One known
  // (absolute, relative) pair gives the stripe base for any other node.
  const uint64_t stripe_base = tail->disk_offset - tail_rel;
  const uint64_t head_rel = head->disk_offset - stripe_base;

  SECTION("single-id cycle: the chain is reset, with NO in-place store") {
    // Point the head at itself.  Every reachable node is now an Original, so
    // the walk sees one id and the traversal-boundary reset applies.
    write_next_link(file, head->disk_offset, head_rel);

    const auto before = cache->stats();
    // The write must terminate (a cycle-blind walk would spin to the cap) and
    // succeed rather than wedge the key.
    put_alt_ok(*cache, key, AlternateId::Original,
               stamped(AlternateId::Original, 2));
    const auto after = cache->stats();

    REQUIRE(after.alternate_chain_resets - before.alternate_chain_resets == 1);
    // LOAD-BEARING: a cycle means "predecessor" is not a reliable notion, so
    // no in-place repoint may run.  The proof that a splice cannot create a
    // cycle depends entirely on this rule.
    REQUIRE(after.alternate_shadows_unlinked ==
            before.alternate_shadows_unlinked);

    auto healed = cache->list_alternates_sync(key);
    REQUIRE(healed.has_value());
    REQUIRE(healed->size() == 1);
    REQUIRE((*healed)[0].id == AlternateId::Original);
    REQUIRE(read_stamp(*cache, key, AlternateId::Original) == 2);
  }

  SECTION("multi-id cycle: rejected, not silently truncated") {
    // Original -> Brotli -> Original.  Two distinct ids, so the boundary reset
    // must NOT fire: resetting would drop a live Brotli the caller never asked
    // to lose.
    write_next_link(file, tail->disk_offset, head_rel);

    const auto before = cache->stats();
    auto wh = cache->write_alternate_sync(key, AlternateId::Original, 4096);
    bool rejected = !wh.has_value();
    if (!rejected) {
      (void)wh->write_sync(stamped(AlternateId::Original, 2));
      auto closed = wh->close_sync();
      rejected = !closed.has_value() &&
                 closed.error() == CacheError::TooManyAlternates;
    } else {
      rejected = wh.error() == CacheError::TooManyAlternates;
    }
    REQUIRE(rejected);
    const auto after = cache->stats();
    REQUIRE(after.alternate_chain_resets == before.alternate_chain_resets);
    REQUIRE(after.alternate_shadows_unlinked ==
            before.alternate_shadows_unlinked);
  }

  cache->stop();
}
#endif  // !_WIN32

// ---------------------------------------------------------------------------
// The traversal-boundary reset, and its negative
// ---------------------------------------------------------------------------
TEST_CASE("A multi-id chain at the traversal cap is rejected, not reset",
          "[alternate][chain][regression]") {
  // The kill switch is what lets a chain grow to the cap at all now.  Two ids
  // alternating means the boundary reset cannot fire (it requires every
  // visible node to be a superseded copy of the id being written), so the
  // write must still be refused — the reset must never drift into "truncate
  // whatever we cannot traverse".
  TempCacheDir tmp("chain_multiid_cap");
  auto cache = make_cache(tmp, bound_config(/*unlink_superseded=*/false));
  const CacheKey key("chain-bound-multiid");

  bool got_too_many = false;
  for (uint32_t n = 1; n <= 200 && !got_too_many; ++n) {
    const auto id = (n % 2 == 0) ? AlternateId::Original : AlternateId::Brotli;
    if (!put_alt(*cache, key, id, stamped(id, n))) {
      got_too_many = true;
    }
  }
  REQUIRE(got_too_many);

  auto st = cache->stats();
  REQUIRE(st.alternate_chain_resets == 0);  // the negative that matters
  REQUIRE(st.alternate_shadows_unlinked == 0);
  REQUIRE(st.alternate_max_chain_depth >= 100);
  cache->stop();
}

TEST_CASE("A single-id chain at the traversal cap is reset and stays writable",
          "[alternate][chain][regression]") {
  // Same shape with ONE id: every visible node is a superseded copy of what is
  // being written, so the chain resets and the key never wedges.  This is the
  // backstop that keeps the depth bound's failure mode (a deferred splice)
  // from turning back into the original wedge, and it is deliberately not
  // gated by the kill switch.
  TempCacheDir tmp("chain_singleid_cap");
  auto cache = make_cache(tmp, bound_config(/*unlink_superseded=*/false));
  const CacheKey key("chain-bound-singleid");

  for (uint32_t n = 1; n <= 200; ++n) {
    REQUIRE(put_alt(*cache, key, AlternateId::Original,
                    stamped(AlternateId::Original, n)));
  }

  auto st = cache->stats();
  REQUIRE(st.alternate_chain_resets >= 1);
  REQUIRE(read_stamp(*cache, key, AlternateId::Original) == 200);
  cache->stop();
}

// ---------------------------------------------------------------------------
// Served-version oracle, at the Cache API
// ---------------------------------------------------------------------------
TEST_CASE("A re-recorded alternate serves its NEW bytes from disk",
          "[alternate][chain][oracle]") {
  // The property a consumer actually depends on, asserted end to end through
  // the public Cache API rather than at the selector.
  //
  // SCOPE NOTE: the RAM cache is disabled here.  With it enabled, a read that
  // populated RAM before the re-record keeps serving the OLD bytes, because
  // writes are write-around and do not invalidate the RAM entry — that is
  // A separate defect that this change does not fix and must not
  // pretend to.  Disabling RAM keeps this oracle pointed at the disk path it
  // is meant to cover; it does NOT narrow the assertion to selector level.
  TempCacheDir tmp("chain_oracle");
  auto cache = make_cache(tmp, bound_config());
  const CacheKey key("chain-bound-oracle");

  SECTION("single id, repeated refresh") {
    for (uint32_t v = 1; v <= 6; ++v) {
      put_alt_ok(*cache, key, AlternateId::Original,
                 stamped(AlternateId::Original, v));

      DefaultStorageSelector first;
      AlternateSelectionContext ctx;
      auto rh = cache->read_alternate_sync(key, first, ctx);
      REQUIRE(rh.has_value());
      REQUIRE(stamp_of(rh->content(), AlternateId::Original) == v);
    }
  }

  SECTION("multi id: the compression-aware selector serves the newest Brotli") {
    put_alt_ok(*cache, key, AlternateId::Original,
               stamped(AlternateId::Original, 1));
    for (uint32_t v = 1; v <= 6; ++v) {
      put_alt_ok(*cache, key, AlternateId::Brotli,
                 stamped(AlternateId::Brotli, v));

      CompressionAwareSelector compression_aware;
      AlternateSelectionContext ctx;
      ctx.prefer_compressed = true;
      auto rh = cache->read_alternate_sync(key, compression_aware, ctx);
      REQUIRE(rh.has_value());
      REQUIRE(stamp_of(rh->content(), AlternateId::Brotli) == v);

      // The Original variant is unaffected by Brotli's refreshes.
      REQUIRE(read_stamp(*cache, key, AlternateId::Original) == 1);
    }
    auto alts = cache->list_alternates_sync(key);
    REQUIRE(alts.has_value());
    REQUIRE(alts->size() == 2);
  }

  cache->stop();
}

// ---------------------------------------------------------------------------
// Behaviour with the splice disabled: the shape a DEFERRED splice leaves
// ---------------------------------------------------------------------------
TEST_CASE("With the splice disabled every chain stays well-formed",
          "[alternate][chain][killswitch]") {
  // A splice that cannot proceed (contention, or a wrap racing the write)
  // leaves the superseded node linked and the caller's write succeeds anyway.
  // That end state is exactly the shape below — a binary without the
  // mechanism — so this pins "degrades to the old behaviour, never worse".
  // There is no seam to force a Busy from a test, so this stands in for the
  // per-store deferral case rather than reproducing it; the accounting for it
  // is exercised by the wrap-frontier case in test_wrap_phase_aba.cpp.
  TempCacheDir tmp("chain_killswitch");
  auto cache = make_cache(tmp, bound_config(/*unlink_superseded=*/false));
  const CacheKey key("chain-bound-killswitch");

  for (uint32_t v = 1; v <= 20; ++v) {
    put_alt_ok(*cache, key, AlternateId::Original,
               stamped(AlternateId::Original, v));
  }
  put_alt_ok(*cache, key, AlternateId::Brotli, stamped(AlternateId::Brotli, 1));

  auto alts = cache->list_alternates_sync(key);
  REQUIRE(alts.has_value());
  // 20 shadows + the Brotli head: the chain is longer, but intact and walkable.
  REQUIRE(alts->size() == 21);
  REQUIRE((*alts)[0].id == AlternateId::Brotli);
  // Newest-first enumeration, so the newest Original is still what serves.
  REQUIRE(read_stamp(*cache, key, AlternateId::Original) == 20);
  REQUIRE(read_stamp(*cache, key, AlternateId::Brotli) == 1);

  auto st = cache->stats();
  REQUIRE(st.alternate_shadows_unlinked == 0);
  REQUIRE(st.alternate_splice_deferred == 0);
  cache->stop();
}

// ---------------------------------------------------------------------------
// Public-API consequence: remove_alternate_sync now removes the ONLY copy
// ---------------------------------------------------------------------------
TEST_CASE("remove_alternate_sync removes the only surviving copy of an id",
          "[alternate][chain][api]") {
  // A user-visible behaviour change produced as a side effect of the bound.
  // remove_alternate_sync unlinks the FIRST node carrying the id; with
  // duplicates present that used to roll the served version back one.  With
  // the bound there is at most one node per id, so the call now removes the
  // alternate outright.  Pinned so it is a decision, not a surprise.
  TempCacheDir tmp("chain_remove");
  auto cache = make_cache(tmp, bound_config());
  const CacheKey key("chain-bound-remove");

  put_alt_ok(*cache, key, AlternateId::Original,
             stamped(AlternateId::Original, 1));
  put_alt_ok(*cache, key, AlternateId::Original,
             stamped(AlternateId::Original, 2));
  put_alt_ok(*cache, key, AlternateId::Brotli, stamped(AlternateId::Brotli, 1));

  auto alts = cache->list_alternates_sync(key);
  REQUIRE(alts.has_value());
  REQUIRE(alts->size() == 2);

  REQUIRE(cache->remove_alternate_sync(key, AlternateId::Original).has_value());

  // No rollback to version 1 — the id is gone.
  REQUIRE_FALSE(read_stamp(*cache, key, AlternateId::Original).has_value());
  // The other alternate is untouched.
  REQUIRE(read_stamp(*cache, key, AlternateId::Brotli) == 1);
  auto after = cache->list_alternates_sync(key);
  REQUIRE(after.has_value());
  REQUIRE(after->size() == 1);
  REQUIRE((*after)[0].id == AlternateId::Brotli);
  cache->stop();
}
