// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// ###########################################################################
// ##                                                                       ##
// ##   TESTS 1, 2 AND 3 IN THIS FILE WERE THE REPRODUCTION of a            ##
// ##   cursor-adoption defect in Volume::allocate_write_slot, and they     ##
// ##   FAILED on pre-fix code -- BY DESIGN.  THIS PR LANDS BOTH THE FIX    ##
// ##   AND THESE TESTS, so as of this PR they PASS.                        ##
// ##                                                                       ##
// ##   THE DEFECT (fixed in this PR): the shared write cursor was          ##
// ##   adopted only when it was HIGHER than this process's private         ##
// ##   cursor, so a wrap -- which drives the shared cursor DOWN -- was     ##
// ##   refused by every stale-high peer, which then wrapped AGAIN in       ##
// ##   the same round and reserved a byte range that OVERLAPPED the        ##
// ##   first wrapper's reservation.                                        ##
// ##                                                                       ##
// ##   A RED result from tests 1-3 was the expected, correct outcome       ##
// ##   on the UNFIXED code they reproduce.  They turn GREEN with the       ##
// ##   cursor-adoption fix (adopt the shared cursor in BOTH                ##
// ##   directions, i.e. whenever it is the authoritative one), which       ##
// ##   lands in this same PR.  They are deliberately NOT tagged            ##
// ##   [!shouldfail] and NOT hidden behind [.]: a silently "passing"       ##
// ##   reproduction is worse than an honest one, so they assert the        ##
// ##   FIXED behaviour directly.                                           ##
// ##                                                                       ##
// ##   TEST 4 ([lease]) exercises lease borrow protection, which           ##
// ##   was already correct: it passed before this PR and still does.       ##
// ##                                                                       ##
// ###########################################################################
//
// Multi-process SHARED-WRITER coverage: what actually happens when N
// processes concurrently write ALL stripes of one shared cyclone.dat.
//
// WHY THIS TOPOLOGY IS THE PRODUCTION ONE.  mod_pagespeed 1.15 sets
// multi_process_config{enabled = true, process_index = 0,
// total_processes = 1} in EVERY server process, so is_stripe_owned() is
// true for every stripe in every process: the ownership partitioning is a
// NO-OP and every process writes every stripe.  Apache/nginx create and
// start ONE Cache in the master and then fork() their workers, so the
// children share a fork-copied Volume: the mmap'd directory header (and
// with it shared_write_pos, the wrap count, the phase bit, the write lock,
// the borrow slot and the lease) is SHARED, but every process-local member
// -- notably Stripe::write_pos and Stripe::mutex -- is PRIVATE per process.
//
// WHAT IS PINNED (the cursor-adoption defect, Volume::allocate_write_slot):
//
//     uint64_t shared_pos = stripe->mmap_directory->get_shared_write_pos();
//     if (shared_pos > stripe->write_pos &&           // <-- ONE-DIRECTIONAL
//         shared_pos <= stripe->offset + stripe->size) {
//       stripe->write_pos = shared_pos;
//     }
//
// A wrap drives shared_write_pos DOWN (to data_area_start + doc_size).  A
// peer process whose private write_pos is stale-HIGH therefore REFUSES to
// adopt the lower shared cursor, computes `available` from its own stale
// cursor, concludes it must wrap, sets write_pos = data_area_start and
// reserves a byte range that OVERLAPS the range the wrapping process just
// reserved.  Both processes hold the cross-process write lock in turn and
// both pass revalidate_write_lock, so this overlap is produced LEGITIMATELY
// under the lock -- exactly the "undetectable channel" the write-lock
// hardening claims to close.  Consequences: overlapping concurrent pwrites,
// N phase toggles per logical wrap round (phase ABA), and shared_wrap_count
// incremented N times per logical wrap.
//
// TESTS
//   1 [cursor]      Deterministic, fork-free proof: two Volume views on one
//                   file (= two processes' private cursors over one shared
//                   header).  Documents committed by the wrapping view are
//                   destroyed by the stale-high peer's illegitimate wrap.
//                   THIS IS THE GATE.  Failed pre-fix.
//   2 [fork]        Apache/nginx shape: N children writing through a
//                   fork-INHERITED Volume, in lockstep ROUNDS.  Wrap
//                   amplification + integrity.  Failed pre-fix.
//   3 [independent] IIS web-garden / nginx-SIGHUP shape: N children each
//                   independently opening the SAME file.  Same oracles.
//                   Failed pre-fix.
//   4 [lease]       cross-process borrow protection.  Passed pre-fix.
//
// CATCH2 + FORK HOUSE RULE: children NEVER touch Catch2.  They do plain
// C-style checks and _exit(<code>); the parent reaps every child, closes
// every pipe, unmaps every region, and only THEN runs its REQUIREs.  Every
// child body additionally runs inside a try/catch: C++ unwinding must never
// escape a forked child, because Catch2's handler would run this file's
// TempFileGuard destructor IN THE CHILD and unlink the shared cyclone.dat
// out from under the parent and its siblings.

#ifndef _WIN32  // fork()-based; not portable to Windows

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "core/document.hpp"
#include "core/volume.hpp"
#include "cyclone/config.hpp"
#include "cyclone/error.hpp"
#include "cyclone/key.hpp"

using namespace cyclone;

namespace {

// --- temp files (pid-qualified: the suite runs in parallel) ----------------

std::string get_temp_path(const std::string &suffix) {
  return (std::filesystem::temp_directory_path() /
          ("cyclone_mpwriters_" + suffix + "_" + std::to_string(::getpid()) +
           ".cache"))
      .string();
}

void cleanup_temp_file(const std::string &path) { std::remove(path.c_str()); }

struct TempFileGuard {
  std::string path;
  ~TempFileGuard() { cleanup_temp_file(path); }
};

// --- the volume under test -------------------------------------------------

// 16MB => usable/kAutoStripeGranularity rounds to a SINGLE stripe, so every
// key lands on the one stripe whose cursor we are reasoning about.  Asserted
// (stripe_count == 1), never assumed.
constexpr size_t kVolumeBytes = static_cast<size_t>(16 * 1024 * 1024);

// Production write path: the whole document, header included, is ONE pwrite
// of exactly (content + Document::kHeaderSize) bytes at the reserved offset
// (Volume::commit_write) -- no alignment rounding, so byte accounting in
// this file is exact.
constexpr size_t kDocHeaderBytes = 132;  // Document::kHeaderSize
static_assert(kDocHeaderBytes == Document::kHeaderSize,
              "byte accounting in this file (expected_wraps, bytes_ok) is "
              "derived from the document header size; a header-layout "
              "change must update kDocHeaderBytes, not silently skew it");

// A "process view" of the shared volume.  Mirrors exactly what Cache::start()
// does for its volumes (open + install read anchors) minus the layers this
// topology does not use: no RAM cache (production runs ram_cache_size = 0),
// no hit tracker, no optimization engine, no periodic directory syncer.
//
// Why Volume and not Cache: the write-lock recovery counters
// (write_lock_escalation_takeovers / _usurp_aborts / _force_releases) are
// exposed on VolumeStats ONLY -- CacheStats does not carry them and Cache
// hands out no Volume* -- and a forked child has no other way to ship them
// to the parent (they are process-local).  Volume is also the unit that
// contains allocate_write_slot, i.e. the defect itself.
struct VolumeView {
  std::shared_ptr<Volume> volume;
  std::vector<std::shared_ptr<VolumeReadAnchor>> anchors;

  VolumeView() = default;
  VolumeView(const VolumeView &) = delete;
  VolumeView &operator=(const VolumeView &) = delete;
  VolumeView(VolumeView &&) = delete;
  VolumeView &operator=(VolumeView &&) = delete;

  bool open(const std::string &path, std::chrono::milliseconds lease,
            std::chrono::milliseconds ceiling) {
    VolumeConfig vc;
    vc.path = path;
    vc.size = kVolumeBytes;
    vc.read_lease_duration = lease;
    vc.lease_wrap_ceiling = ceiling;
    // Multi-process mode forces this on in Cache::add_volume(); a raw Volume
    // defaults to true.  Stated explicitly: the CRC leg of the read gauntlet
    // is what turns an overlapped/torn document into a clean miss.
    vc.verify_checksum_on_read = true;

    // THE PRODUCTION SETTING: enabled, 0-of-1, in every process => every
    // stripe is "owned" by every process.
    MultiProcessConfig mp;
    mp.set_enabled(true).set_process_index(0).set_total_processes(1);

    volume = std::make_shared<Volume>(vc, mp);
    if (!volume->open().has_value()) {
      volume.reset();
      return false;
    }
    anchors = volume->make_read_anchors();
    volume->set_read_anchors(anchors.data(), anchors.size());
    return true;
  }

  // Never runs in a forked child: children _exit() without teardown, exactly
  // as an Apache/nginx worker does.
  ~VolumeView() {
    if (volume) {
      volume->set_read_anchors(nullptr, 0);
      volume->close();
    }
  }
};

// Default lease knobs: production-shaped, but with BOTH the lease duration
// and the anti-starvation ceiling pushed far out so a slow (sanitizer, ~20x;
// loaded CI runner) run cannot lapse a held lease mid-flood and turn TEST 4
// into a timing test.  The lease must strictly dominate the writer flood's
// whole reap budget (180s), hence 300s.
constexpr auto kLeaseDuration = std::chrono::milliseconds(300000);
constexpr auto kLeaseCeiling = std::chrono::milliseconds(600000);

// --- write / read helpers --------------------------------------------------

// CacheError::Success == "the fill committed".  The wrap decision, the lease
// gate and the write-lock revalidation all live in commit_write(), which runs
// inside close_sync() -- so Busy / NoSpace surface there, not at write_sync().
CacheError write_doc(Volume &vol, const CacheKey &key,
                     std::span<const std::byte> content) {
  auto wh = vol.write_sync(key, content.size());
  if (!wh.has_value()) return wh.error();
  auto written = wh->write_sync(content);
  if (!written.has_value()) return written.error();
  auto closed = wh->close_sync();
  if (!closed.has_value()) return closed.error();
  return CacheError::Success;
}

// Bytes still free between this VIEW's PRIVATE write cursor and the end of
// the stripe -- i.e. exactly the `available` that allocate_write_slot
// computes.  VolumeStats::bytes_used sums (stripe->write_pos - stripe->offset)
// over the PROCESS-LOCAL cursors and stripe_bytes sums the stripe sizes, so
// on a single-stripe volume this is the view's own tail.  This is the public
// observable that makes the divergence of the two private cursors visible
// without reaching into Stripe.
uint64_t tail_available(const Volume &vol) {
  const VolumeStats s = vol.stats();
  return s.stripe_bytes - s.bytes_used;
}

// Deterministic filler; distinct per (tag, index) so a document assembled
// from two writers' overlapping pwrites is not silently self-consistent.
std::vector<std::byte> make_content(uint32_t tag, uint32_t index, size_t size) {
  std::vector<std::byte> data(size);
  for (size_t i = 0; i < size; ++i) {
    const size_t v = static_cast<size_t>(tag) * 2654435761U +
                     static_cast<size_t>(index) * 131U + i;
    data[i] = static_cast<std::byte>(v & 0xFFU);
  }
  return data;
}

bool content_equals(std::span<const std::byte> got,
                    std::span<const std::byte> want) {
  return got.size() == want.size() &&
         std::memcmp(got.data(), want.data(), want.size()) == 0;
}

// --- self-identifying / self-verifying payload (TESTS 2 and 3) -------------
//
// Independent of Cyclone's own CRC: the payload carries its writer's identity,
// its key, its length, and an inner hash over its whole body.  A document
// assembled from two writers' overlapping pwrites therefore fails the inner
// hash even if the outer CRC happened to pass, and a wrong-key serve fails
// the key compare.

constexpr uint32_t kPayloadMagic = 0xC7C10DEDU;
constexpr size_t kPayloadKeyMax = 40;

struct PayloadHeader {
  uint32_t magic;
  uint32_t child;
  uint32_t seq;
  uint32_t size;  // Total payload bytes, header included.
  uint64_t hash;  // FNV-1a over the whole payload with this field zeroed.
  char key[kPayloadKeyMax];
};
static_assert(sizeof(PayloadHeader) == 64, "payload header must be 64 bytes");
static_assert(std::is_trivially_copyable_v<PayloadHeader>,
              "payload header is memcpy'd into the byte buffer");

// VARIABLE document size (fix O1).  With a single constant size every
// defect-induced overlap is an EXACT alias: the loser's reservation and the
// winner's are byte-for-byte the same range, one pwrite replaces the other
// wholesale, the loser's document header is gone, and the read gauntlet's
// full-key verify (volume.cpp: key compare BEFORE the CRC leg) degrades it to
// a clean miss.  The integrity oracle can then never fire -- it is
// STRUCTURALLY vacuous.  Varying the size per (child, seq) makes the
// reservations PARTIALLY overlap, which is the only shape in which a
// key-matching header can survive over a clobbered body and reach the CRC leg
// as CacheError::Corrupted.
//
// Range: [8192, 65528], 8-byte aligned.
constexpr size_t kMinDocBytes = 8192;
constexpr size_t kDocSizeSpread = 57344;
constexpr size_t kMeanDocBytes = kMinDocBytes + kDocSizeSpread / 2;

size_t doc_size_for(uint32_t child, uint32_t seq) {
  const uint32_t h = child * 2654435761U + seq * 2246822519U;
  return kMinDocBytes + static_cast<size_t>((h % kDocSizeSpread) & ~7U);
}

uint64_t fnv1a(std::span<const std::byte> data) {
  uint64_t h = 1469598103934665603ULL;
  for (std::byte b : data) {
    h ^= static_cast<uint64_t>(std::to_integer<uint8_t>(b));
    h *= 1099511628211ULL;
  }
  return h;
}

std::string doc_key_string(uint32_t child, uint32_t seq) {
  return "mpw-c" + std::to_string(child) + "-s" + std::to_string(seq);
}

std::vector<std::byte> make_payload(uint32_t child, uint32_t seq, size_t size) {
  std::vector<std::byte> buf(size);

  // Filler derived from (child, seq): a mix of two writers' pwrites shows up
  // as a body that matches neither writer's pattern AND fails the hash.
  for (size_t i = sizeof(PayloadHeader); i < size; ++i) {
    const size_t v =
        static_cast<size_t>(child) * 131U + static_cast<size_t>(seq) * 17U + i;
    buf[i] = static_cast<std::byte>(v & 0xFFU);
  }

  PayloadHeader hdr{};
  hdr.magic = kPayloadMagic;
  hdr.child = child;
  hdr.seq = seq;
  hdr.size = static_cast<uint32_t>(size);
  hdr.hash = 0;
  const std::string key = doc_key_string(child, seq);
  std::memcpy(static_cast<void *>(hdr.key), key.c_str(),
              std::min(key.size(), kPayloadKeyMax - 1));
  std::memcpy(buf.data(), &hdr, sizeof(hdr));

  hdr.hash = fnv1a(std::span<const std::byte>(buf));
  std::memcpy(buf.data(), &hdr, sizeof(hdr));
  return buf;
}

enum class PayloadVerdict : uint8_t {
  kOk,
  kTooShort,
  kBadMagic,
  kWrongKey,
  kBadSize,
  kBadHash,  // A MIX of two writers' bytes, or any other body damage.
};

PayloadVerdict verify_payload(std::span<const std::byte> got, uint32_t child,
                              uint32_t seq) {
  if (got.size() < sizeof(PayloadHeader)) return PayloadVerdict::kTooShort;

  PayloadHeader hdr{};
  std::memcpy(&hdr, got.data(), sizeof(hdr));
  if (hdr.magic != kPayloadMagic) return PayloadVerdict::kBadMagic;
  if (static_cast<size_t>(hdr.size) != got.size() ||
      got.size() != doc_size_for(child, seq)) {
    return PayloadVerdict::kBadSize;
  }

  const std::string want_key = doc_key_string(child, seq);
  const size_t klen = std::min(want_key.size(), kPayloadKeyMax - 1);
  if (hdr.child != child || hdr.seq != seq ||
      std::strncmp(static_cast<const char *>(hdr.key), want_key.c_str(),
                   klen) != 0 ||
      hdr.key[klen] != '\0') {
    return PayloadVerdict::kWrongKey;
  }

  // Recompute the inner hash with the hash field zeroed, exactly as written.
  std::vector<std::byte> copy(got.begin(), got.end());
  const uint64_t claimed = hdr.hash;
  hdr.hash = 0;
  std::memcpy(copy.data(), &hdr, sizeof(hdr));
  if (fnv1a(std::span<const std::byte>(copy)) != claimed) {
    return PayloadVerdict::kBadHash;
  }
  return PayloadVerdict::kOk;
}

// --- cross-process child reporting + lockstep control ----------------------
//
// Volume's write-lock telemetry is PROCESS-LOCAL, so a child's counters can
// only reach the parent through shared memory.  MAP_SHARED|MAP_ANONYMOUS is
// inherited across fork() and survives the child's _exit().

struct ChildReport {
  uint64_t attempted;
  uint64_t ok;
  uint64_t busy;
  uint64_t nospace;
  uint64_t other_error;
  uint64_t bytes_ok;         // REAL bytes committed (doc + header), Success.
  uint64_t wrap_count_seen;  // SHARED wrap counter as the child saw it.
  uint64_t write_lock_force_releases;
  uint64_t write_lock_escalation_takeovers;
  uint64_t write_lock_live_holder_waits;
  uint64_t write_lock_usurp_aborts;
  uint64_t writes_dropped_by_lease;
  uint64_t flags;  // Test-specific.
};

// Lockstep control block, shared with every child (fix O2).  See round_sync().
struct SharedControl {
  std::atomic<uint64_t> round;
  std::atomic<uint32_t> arrived;
  std::atomic<uint32_t> aborted;
};
static_assert(std::atomic<uint64_t>::is_always_lock_free,
              "shared-memory barrier must not use a process-local lock");
static_assert(std::atomic<uint32_t>::is_always_lock_free,
              "shared-memory barrier must not use a process-local lock");

constexpr size_t kControlBytes = 64;  // one cacheline, ahead of the reports
static_assert(sizeof(SharedControl) <= kControlBytes);

struct SharedReports {
  SharedControl *ctl = nullptr;
  ChildReport *data = nullptr;
  size_t count = 0;
  size_t bytes = 0;

  SharedReports() = default;
  SharedReports(const SharedReports &) = delete;
  SharedReports &operator=(const SharedReports &) = delete;
  // RAII (fix m1): a throw between map() and the happy-path teardown must not
  // leak the shared region.
  ~SharedReports() { unmap(); }

  bool map(size_t n) {
    const size_t total = kControlBytes + sizeof(ChildReport) * n;
    void *base = ::mmap(nullptr, total, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) return false;
    std::memset(base, 0, total);
    ctl = ::new (base) SharedControl{};
    data = reinterpret_cast<ChildReport *>(static_cast<std::byte *>(base) +
                                           kControlBytes);
    count = n;
    bytes = total;
    return true;
  }
  void unmap() {
    if (ctl != nullptr) {
      ::munmap(static_cast<void *>(ctl), bytes);
      ctl = nullptr;
      data = nullptr;
      count = 0;
      bytes = 0;
    }
  }
};

void fill_lock_counters(ChildReport &rep, const Volume &vol) {
  const VolumeStats s = vol.stats();
  rep.wrap_count_seen = s.wrap_count;
  rep.write_lock_force_releases = s.write_lock_force_releases;
  rep.write_lock_escalation_takeovers = s.write_lock_escalation_takeovers;
  rep.write_lock_live_holder_waits = s.write_lock_live_holder_waits;
  rep.write_lock_usurp_aborts = s.write_lock_usurp_aborts;
  rep.writes_dropped_by_lease = s.writes_dropped_by_lease;
}

// LOCKSTEP ROUND BARRIER (fix O2).  Every writer child commits exactly ONE
// document per round and then blocks here until all `participants` have
// committed theirs.
//
// WHY.  Without it, wrap amplification is a SCHEDULING ACCIDENT: each child's
// whole workload is only a few ms of CPU, so on a loaded or single-vCPU runner
// the children can serialize, no peer is ever stale-high at a wrap, no
// amplification occurs, and a wrap-count oracle goes GREEN with the defect
// present.  With the barrier, every child enters each round having adopted the
// same (highest) shared cursor -- so at the round in which the stripe fills,
// EVERY child is stale-high by construction and every one of them takes the
// wrap branch.  "All N processes are stale-high at every wrap" becomes a
// STRUCTURAL property of the harness instead of a race we hope to win.
//
// It deliberately does NOT serialize the writes themselves (that would close
// the concurrent-overlapping-pwrite channel, which is the only route to a
// torn, CRC-failing document): inside a round the children race, between
// rounds they realign.
//
// Bounded: a dead or wedged sibling trips the deadline, raises `aborted`, and
// every peer bails out -- the lockstep loop can never hang CI.
bool round_sync(SharedControl &ctl, uint32_t participants,
                std::chrono::seconds budget) {
  if (ctl.aborted.load(std::memory_order_acquire) != 0) return false;

  const uint64_t my_round = ctl.round.load(std::memory_order_acquire);
  const uint32_t seen = ctl.arrived.fetch_add(1, std::memory_order_acq_rel) + 1;
  if (seen == participants) {
    ctl.arrived.store(0, std::memory_order_relaxed);
    ctl.round.fetch_add(1, std::memory_order_release);
    return true;
  }

  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (ctl.round.load(std::memory_order_acquire) == my_round) {
    if (ctl.aborted.load(std::memory_order_acquire) != 0) return false;
    if (std::chrono::steady_clock::now() >= deadline) {
      ctl.aborted.store(1, std::memory_order_release);
      return false;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  return true;
}

// The lockstep writer body shared by TESTS 2 and 3.  Returns the exit code.
int run_lockstep_writer(Volume &vol, SharedControl &ctl, ChildReport &rep,
                        uint32_t child, uint32_t docs, uint32_t participants) {
  for (uint32_t s = 0; s < docs; ++s) {
    const size_t size = doc_size_for(child, s);
    const auto payload = make_payload(child, s, size);
    ++rep.attempted;
    const CacheError err = write_doc(vol, CacheKey(doc_key_string(child, s)),
                                     std::span<const std::byte>(payload));
    switch (err) {
      case CacheError::Success:
        ++rep.ok;
        rep.bytes_ok += size + kDocHeaderBytes;
        break;
      case CacheError::Busy:
        // A usurp abort.  NOT benign here: no child is ever killed or stalled
        // for seconds in this test, so there is nothing for the write-lock
        // recovery path to legitimately recover FROM.  Any Busy means a live
        // holder was usurped -- a real defect -- which is exactly why the
        // parent asserts usurp_aborts == 0 (and NOT merely "busy is rare").
        ++rep.busy;
        break;
      case CacheError::NoSpace:  // lease-deferred wrap -- legitimate
        ++rep.nospace;
        break;
      default:
        ++rep.other_error;
        break;
    }
    if (!round_sync(ctl, participants, std::chrono::seconds(30))) {
      fill_lock_counters(rep, vol);
      return 22;  // a peer died or wedged; bail rather than hang CI
    }
  }
  fill_lock_counters(rep, vol);
  return rep.other_error == 0 ? 0 : 21;
}

// --- post-hoc verification (parent side, after every child is reaped) ------

struct Damage {
  uint32_t child;
  uint32_t seq;
  PayloadVerdict verdict;
};

struct IntegrityScan {
  std::vector<Damage> damaged;
  uint64_t hits = 0;
  uint64_t misses = 0;
  uint64_t corrupted = 0;
};

IntegrityScan scan_documents(Volume &vol, uint32_t children, uint32_t docs) {
  IntegrityScan scan;
  for (uint32_t c = 0; c < children; ++c) {
    for (uint32_t s = 0; s < docs; ++s) {
      auto rh = vol.read_sync(CacheKey(doc_key_string(c, s)));
      if (!rh.has_value()) {
        // A miss is LEGAL: the entry was evicted by a wrap.  Corrupted is not:
        // the read gauntlet verifies the full key BEFORE the CRC, so a
        // Corrupted verdict means a key-MATCHING document's body was
        // overwritten -- i.e. an overlapping reservation, not an eviction.
        if (rh.error() == CacheError::Corrupted) ++scan.corrupted;
        ++scan.misses;
        continue;
      }
      ++scan.hits;
      const PayloadVerdict v = verify_payload(rh->content(), c, s);
      if (v != PayloadVerdict::kOk) scan.damaged.push_back({c, s, v});
      rh->close();
    }
  }
  return scan;
}

struct Totals {
  uint64_t ok = 0;
  uint64_t bytes_ok = 0;
  uint64_t busy = 0;
  uint64_t nospace = 0;
  uint64_t other = 0;
  uint64_t force_releases = 0;
  uint64_t escalations = 0;
  uint64_t usurp_aborts = 0;
  uint64_t live_waits = 0;
};

Totals aggregate(const SharedReports &reports) {
  Totals t;
  for (size_t c = 0; c < reports.count; ++c) {
    const ChildReport &r = reports.data[c];
    t.ok += r.ok;
    t.bytes_ok += r.bytes_ok;
    t.busy += r.busy;
    t.nospace += r.nospace;
    t.other += r.other_error;
    t.force_releases += r.write_lock_force_releases;
    t.escalations += r.write_lock_escalation_takeovers;
    t.usurp_aborts += r.write_lock_usurp_aborts;
    t.live_waits += r.write_lock_live_holder_waits;
  }
  return t;
}

// --- fork plumbing ---------------------------------------------------------

struct ReapResult {
  bool all_exited = true;
  std::vector<int> statuses;
};

// COLLECT-THEN-ASSERT: no Catch2 in here.  Every child is reaped or SIGKILLed
// before the caller runs a single REQUIRE, so a failing assertion can never
// orphan a child into CI.
ReapResult reap_all(const std::vector<pid_t> &pids,
                    std::chrono::seconds budget) {
  ReapResult result;
  result.statuses.assign(pids.size(), -1);
  const auto deadline = std::chrono::steady_clock::now() + budget;

  for (size_t i = 0; i < pids.size(); ++i) {
    bool exited = false;
    bool gone = false;  // ECHILD: already reaped; there is nothing to kill.
    int status = 0;
    while (std::chrono::steady_clock::now() < deadline) {
      const pid_t r = ::waitpid(pids[i], &status, WNOHANG);
      if (r == pids[i]) {
        exited = true;
        break;
      }
      if (r == -1) {
        // A signal delivered to the PARENT (sanitizer runtime, CI wrapper,
        // SIGCHLD) must not be misread as "the child is gone" -- that would
        // SIGKILL a LIVE child and fail all_exited spuriously (fix M4).
        if (errno == EINTR) continue;
        if (errno == ECHILD) gone = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!exited) {
      if (!gone) {
        ::kill(pids[i], SIGKILL);
        ::waitpid(pids[i], &status, 0);
      }
      result.all_exited = false;
    }
    result.statuses[i] = status;
  }
  return result;
}

bool all_exited_zero(const ReapResult &r) {
  if (!r.all_exited) return false;
  return std::all_of(r.statuses.begin(), r.statuses.end(),
                     [](int s) { return WIFEXITED(s) && WEXITSTATUS(s) == 0; });
}

// Bounded wait for one readiness byte (fix C2).  An unbounded blocking read
// in the PARENT lets a wedged child burn the whole CI job budget; poll() with
// a steady_clock deadline turns that into a clean, self-explaining failure.
// Returns false on timeout, on EOF (the writer end is gone: the child died)
// and on error.
bool wait_for_byte(int fd, std::chrono::seconds budget) {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  for (;;) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return false;
    const auto left =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    struct pollfd pfd = {};
    pfd.fd = fd;
    pfd.events = POLLIN;
    const int r = ::poll(&pfd, 1, static_cast<int>(left.count()));
    if (r < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (r == 0) return false;  // timed out
    unsigned char b = 0;
    const ssize_t n = ::read(fd, &b, 1);
    if (n == 1) return true;
    if (n < 0 && errno == EINTR) continue;
    return false;  // EOF (child died) or a hard error
  }
}

// Start barrier: each child blocks on a 1-byte read; the parent releases all
// of them at once, so the children really contend instead of running in fork
// order.  (No repo precedent -- composed from the pipe idiom in
// multi_process_test.cpp.)
struct Barrier {
  int fds[2] = {-1, -1};

  Barrier() = default;
  Barrier(const Barrier &) = delete;
  Barrier &operator=(const Barrier &) = delete;
  // RAII (fix m1): a throw between create() and the happy-path teardown must
  // not leak the pipe fds.
  ~Barrier() { close_all(); }

  bool create() { return ::pipe(fds) == 0; }
  // Child side: close the write end, block until the parent releases.
  bool wait() {
    ::close(fds[1]);
    fds[1] = -1;
    unsigned char go = 0;
    const ssize_t n = ::read(fds[0], &go, 1);
    ::close(fds[0]);
    fds[0] = -1;
    return n == 1;
  }
  // Parent side: close the read end, release `n` children.
  void release(size_t n) {
    if (n == 0) return;  // nobody holds the read end: writing would SIGPIPE
    if (fds[0] >= 0) {
      ::close(fds[0]);
      fds[0] = -1;
    }
    const std::vector<unsigned char> go(n, 1);
    (void)!::write(fds[1], go.data(), go.size());
  }
  void close_all() {
    for (int &fd : fds) {
      if (fd >= 0) {
        ::close(fd);
        fd = -1;
      }
    }
  }
};

}  // namespace

// ===========================================================================
// TEST 1 -- THE HEADLINE.  Deterministic, no fork, no scheduling.  It was
// the reproduction: it FAILED on pre-fix code, BY DESIGN (see the banner at
// the top of this file), and passes with the fix landed in this same PR.
//
// Two Volume views on one file have the ESSENTIAL property of the forked
// topology and nothing else: each view owns a PRIVATE Stripe::write_pos while
// both read and publish the SAME mmap'd shared_write_pos.  View A plays
// "process A", view B plays "process B".
// ===========================================================================
TEST_CASE(
    "A stale-high peer refuses the wrapped shared cursor and clobbers the "
    "wrapping writer's document",
    "[mpwriters][cursor]") {
  const std::string path = get_temp_path("cursor");
  cleanup_temp_file(path);
  TempFileGuard guard{path};

  VolumeView a;
  VolumeView b;
  REQUIRE(a.open(path, kLeaseDuration, kLeaseCeiling));
  REQUIRE(b.open(path, kLeaseDuration, kLeaseCeiling));
  Volume &va = *a.volume;
  Volume &vb = *b.volume;

  // Geometry, asserted rather than assumed: one stripe, so every key below
  // lands on the cursor we are reasoning about.
  const VolumeStats fresh = va.stats();
  REQUIRE(fresh.stripe_count == 1);
  // On a fresh volume write_pos == data_offset, so bytes_used is exactly the
  // directory area and the rest of the stripe is the data area.
  const uint64_t dir_bytes = fresh.bytes_used;
  const uint64_t data_capacity = fresh.stripe_bytes - dir_bytes;
  INFO("stripe_bytes=" << fresh.stripe_bytes << " dir_bytes=" << dir_bytes
                       << " data_capacity=" << data_capacity);
  REQUIRE(data_capacity > static_cast<uint64_t>(4 * 1024 * 1024));
  REQUIRE(tail_available(va) == data_capacity);
  REQUIRE(tail_available(vb) == data_capacity);
  REQUIRE(va.stats().wrap_count == 0);

  // (2) Drive A forward until its cursor sits near the end of the stripe --
  //     but do NOT wrap it.
  constexpr size_t kFillBytes = static_cast<size_t>(64 * 1024);
  constexpr uint64_t kTailTarget = static_cast<uint64_t>(768 * 1024);
  constexpr size_t kWrapBytes = static_cast<size_t>(1024 * 1024);
  uint32_t fill_index = 0;
  while (tail_available(va) > kTailTarget) {
    const auto content = make_content(1, fill_index, kFillBytes);
    REQUIRE(write_doc(va, CacheKey("fill-" + std::to_string(fill_index)),
                      std::span<const std::byte>(content)) ==
            CacheError::Success);
    ++fill_index;
  }
  const uint64_t tail_a_before = tail_available(va);
  CAPTURE(fill_index, tail_a_before);
  REQUIRE(va.stats().wrap_count == 0);  // Filled forward only: no wrap yet.
  REQUIRE(tail_a_before <= kTailTarget);
  REQUIRE(tail_a_before < kWrapBytes);  // A's next 1MB doc MUST wrap.

  // (3) One small write through B.  B adopts the (higher) shared cursor -- the
  //     legal, working direction of the one-directional sync -- so B's PRIVATE
  //     cursor is now stale-HIGH too.  Asserted, not assumed.
  REQUIRE(tail_available(vb) == data_capacity);  // still pristine
  {
    const auto small = make_content(2, 0, static_cast<size_t>(4 * 1024));
    REQUIRE(
        write_doc(vb, CacheKey("b-sync"), std::span<const std::byte>(small)) ==
        CacheError::Success);
  }
  const uint64_t tail_b_after_sync = tail_available(vb);
  CAPTURE(tail_b_after_sync);
  REQUIRE(tail_b_after_sync < tail_a_before);  // B jumped to A's high cursor
  REQUIRE(tail_b_after_sync < kWrapBytes);     // B's next 1MB doc "must" wrap
  REQUIRE(va.stats().wrap_count == 0);

  // (4) A writes a document that does not fit its tail => A WRAPS.  Its bytes
  //     land at data_area_start and it publishes the LOWERED shared cursor.
  const CacheKey key_a("key-A-postwrap");
  const auto content_a = make_content(3, 0, kWrapBytes);
  REQUIRE(write_doc(va, key_a, std::span<const std::byte>(content_a)) ==
          CacheError::Success);
  const uint64_t wraps_after_a = va.stats().wrap_count;
  CAPTURE(wraps_after_a);
  REQUIRE(wraps_after_a == 1);  // Exactly one LOGICAL wrap has happened.

  // (5) Sanity: A's freshest document reads back correctly.  Scoped, so the
  //     borrow is released before step (6) -- a live borrow would defer the
  //     wrap via the lease gate and MASK the defect.
  {
    auto rh = va.read_sync(key_a);
    REQUIRE(rh.has_value());
    REQUIRE(
        content_equals(rh->content(), std::span<const std::byte>(content_a)));
    rh->close();
  }
  REQUIRE(va.stats().borrows_outstanding == 0);

  // (6) B now writes a document that does not fit ITS (stale-high) tail.
  //
  //     CORRECT: B adopts the lowered shared cursor, sees ~13MB of room, does
  //     NOT wrap, and appends AFTER A's document.
  //     BUGGY  : `shared_pos > stripe->write_pos` is false, so B keeps its
  //              stale-high cursor, "runs out of tail", wraps, and reserves
  //              data_area_start -- straight over A's document.
  const CacheKey key_b("key-B-postwrap");
  const auto content_b = make_content(4, 0, kWrapBytes);
  const CacheError b_result =
      write_doc(vb, key_b, std::span<const std::byte>(content_b));
  const uint64_t wraps_after_b = va.stats().wrap_count;
  const uint64_t tail_b_after_write = tail_available(vb);
  CAPTURE(static_cast<int>(b_result), wraps_after_b, tail_b_after_write);

  // (7) THE ASSERTION.  A's post-wrap document must still be exactly itself,
  //     read through EITHER view.  Under the defect B's illegitimate wrap both
  //     overwrote its bytes and toggled the phase out from under its directory
  //     entry, so the read is a clean miss (or a full-key-verify miss on B's
  //     document) -- either way, a committed document was destroyed by a write
  //     that the write lock was supposed to make impossible.
  {
    auto rh_a = va.read_sync(key_a);
    INFO("read of key-A-postwrap through view A after B's write");
    REQUIRE(rh_a.has_value());
    REQUIRE(
        content_equals(rh_a->content(), std::span<const std::byte>(content_a)));
    rh_a->close();
  }
  {
    auto rh_b = vb.read_sync(key_a);
    INFO("read of key-A-postwrap through view B after B's write");
    REQUIRE(rh_b.has_value());
    REQUIRE(
        content_equals(rh_b->content(), std::span<const std::byte>(content_a)));
    rh_b->close();
  }

  // B's own document must of course also be intact, and B must have committed
  // it (a legitimate append, not a dropped fill).
  REQUIRE(b_result == CacheError::Success);
  {
    auto rh = vb.read_sync(key_b);
    REQUIRE(rh.has_value());
    REQUIRE(
        content_equals(rh->content(), std::span<const std::byte>(content_b)));
    rh->close();
  }

  // The shared wrap counter must have advanced EXACTLY ONCE across steps
  // (4)-(6).  VolumeStats::wrap_count reads MmapDirectory::shared_wrap_count
  // for mmap stripes (Volume::stats()), so it is fleet-wide and identical on
  // both views.  A second increment IS B's illegitimate wrap.
  INFO("shared wrap_count: after A's wrap = "
       << wraps_after_a << ", after B's write = " << wraps_after_b
       << " (2 == B wrapped illegitimately on a stale-high cursor)");
  REQUIRE(vb.stats().wrap_count == wraps_after_b);  // truly shared
  REQUIRE(wraps_after_b == 1);
}

// ===========================================================================
// TEST 2 -- Apache / nginx: ONE Volume created and opened in the master, then
// fork()ed.  Children write through the INHERITED object: shared mmap header,
// private cursors and mutexes.  Failed on pre-fix code, BY DESIGN; passes
// with the fix in this PR.
//
// ORACLE (the discriminator): WRAP AMPLIFICATION, made deterministic by the
// lockstep round barrier (see round_sync()).  Correct code performs ONE wrap
// per logical fill of the stripe.  Under the cursor defect every child is
// stale-high at the fill round and every one of them takes the wrap branch, so
// the shared wrap counter runs at >= kChildren x the fill count.  kChildren >=
// 3 is LOAD-BEARING: the bound only discriminates when the defect's lower
// bound (expected * kChildren - 1) clears the correct-code upper bound
// (expected + 2), i.e. when expected * (kChildren - 1) > 3.
//
// ORACLE (the guard): document integrity, with per-(child, seq) VARIABLE
// document sizes so that a defect-induced overlap is a PARTIAL overlap rather
// than an exact whole-document alias.
// ===========================================================================
TEST_CASE("Forked writers sharing an inherited volume keep documents intact",
          "[mpwriters][fork]") {
  const std::string path = get_temp_path("fork");
  cleanup_temp_file(path);
  TempFileGuard guard{path};

  constexpr uint32_t kChildren = 4;
  static_assert(kChildren >= 3,
                "the wrap-amplification bound only discriminates for N >= 3");
  const size_t mean_doc_total = kMeanDocBytes + kDocHeaderBytes;

  VolumeView view;
  REQUIRE(view.open(path, kLeaseDuration, kLeaseCeiling));
  Volume &vol = *view.volume;

  const VolumeStats fresh = vol.stats();
  REQUIRE(fresh.stripe_count == 1);
  const uint64_t data_capacity = fresh.stripe_bytes - fresh.bytes_used;
  REQUIRE(data_capacity > static_cast<uint64_t>(4 * 1024 * 1024));

  // ~3 full passes over the stripe, spread across the children.
  const uint32_t docs_per_child =
      static_cast<uint32_t>((3 * data_capacity) / (kChildren * mean_doc_total));
  REQUIRE(docs_per_child >= 50);

  SharedReports reports;
  REQUIRE(reports.map(kChildren));
  Barrier barrier;
  REQUIRE(barrier.create());

  std::vector<pid_t> pids;
  pids.reserve(kChildren);
  bool fork_failed = false;
  for (uint32_t c = 0; c < kChildren; ++c) {
    const pid_t pid = fork();
    if (pid < 0) {
      fork_failed = true;
      break;
    }
    if (pid == 0) {
      // ---- CHILD: no Catch2, no cache teardown, _exit() at the end. -------
      // The try/catch is LOAD-BEARING (fix C1): make_payload allocates and
      // CacheKey allocates, and an escaping exception would unwind into
      // Catch2's handler INSIDE THE CHILD, running TempFileGuard's destructor
      // and unlinking the shared cyclone.dat under the parent and siblings.
      int rc = 90;  // 90 = an exception escaped the child body
      try {
        rc = [&]() -> int {
          if (!barrier.wait()) {
            reports.ctl->aborted.store(1, std::memory_order_release);
            return 20;
          }
          return run_lockstep_writer(vol, *reports.ctl, reports.data[c], c,
                                     docs_per_child, kChildren);
        }();
      } catch (...) {
        reports.ctl->aborted.store(1, std::memory_order_release);
        rc = 91;  // 91 = the child body threw
      }
      _exit(rc);
    }
    pids.push_back(pid);
  }

  // Unblock the lockstep loop immediately if a fork failed: the surviving
  // children would otherwise sit out the full round-barrier budget.
  if (fork_failed) {
    reports.ctl->aborted.store(1, std::memory_order_release);
  }
  barrier.release(pids.size());
  const ReapResult reaped = reap_all(pids, std::chrono::seconds(180));
  barrier.close_all();

  // Snapshot the shared wrap counter and verify every document BEFORE any
  // REQUIRE runs (collect-then-assert; the children are all reaped by now).
  const uint64_t observed_wraps = vol.stats().wrap_count;
  const IntegrityScan scan = scan_documents(vol, kChildren, docs_per_child);
  const Totals t = aggregate(reports);
  reports.unmap();

  // Derived from the REAL bytes each child committed (fix O1's consequential
  // edit): with variable document sizes, total_ok * <a constant> is wrong.
  const uint64_t expected_wraps = t.bytes_ok / data_capacity;

  INFO("children=" << kChildren << " docs/child=" << docs_per_child
                   << " ok=" << t.ok << " busy=" << t.busy
                   << " nospace=" << t.nospace << " other=" << t.other
                   << " | hits=" << scan.hits << " misses=" << scan.misses
                   << " corrupted=" << scan.corrupted
                   << " damaged=" << scan.damaged.size() << " | data_capacity="
                   << data_capacity << " bytes_committed=" << t.bytes_ok
                   << " EXPECTED_WRAPS=" << expected_wraps
                   << " OBSERVED_WRAPS=" << observed_wraps
                   << " (defect signature: observed >= expected * " << kChildren
                   << " - 1 = " << (expected_wraps * kChildren - 1)
                   << "; correct code: observed ~= expected)"
                   << " | write_lock: force_releases=" << t.force_releases
                   << " escalations=" << t.escalations << " usurp_aborts="
                   << t.usurp_aborts << " live_holder_waits=" << t.live_waits
                   << " (live_holder_waits > 0 is HEALTHY)");

  REQUIRE(reaped.all_exited);
  REQUIRE(all_exited_zero(reaped));
  REQUIRE_FALSE(fork_failed);

  // (a) INTEGRITY guard.  A miss is legal (a wrap evicted the entry); a MIX,
  //     a wrong key, a wrong length or a Corrupted is not.
  if (!scan.damaged.empty()) {
    CAPTURE(scan.damaged.front().child, scan.damaged.front().seq,
            static_cast<int>(scan.damaged.front().verdict));
  }
  REQUIRE(scan.damaged.empty());
  REQUIRE(scan.corrupted == 0);
  REQUIRE(scan.hits > 0);  // Guard against a vacuous integrity pass.

  // (b) WRAP AMPLIFICATION -- THE DISCRIMINATOR.  Correct code: exactly one
  //     wrap per logical fill (plus at most one boundary wrap for the tail
  //     gap each pass wastes).  Defect: >= kChildren wraps per fill.  The
  //     bound below is therefore GREEN on correct code and RED on the defect.
  REQUIRE(expected_wraps >= 2);  // the flood really did fill the stripe
  REQUIRE(observed_wraps <= expected_wraps + 2);

  // (c) No child was killed or stalled, so any write-lock RECOVERY is a real
  //     defect -- there is nothing legitimate to recover from.
  //
  //     KNOWN FLAKE SUSPECT: on a badly oversubscribed sanitizer runner a
  //     LIVE child stalled for several seconds while holding the
  //     cross-process write lock can legitimately trigger a peer's
  //     escalation takeover (and the usurp-abort / force-release path with
  //     it).  These three assert "no recovery was NEEDED", not the defect
  //     under test -- so if this test ever flakes in CI, suspect them FIRST.
  REQUIRE(t.escalations == 0);
  REQUIRE(t.usurp_aborts == 0);
  REQUIRE(t.force_releases == 0);
}

// ===========================================================================
// TEST 3 -- IIS web garden / nginx SIGHUP reload: every child INDEPENDENTLY
// opens the same live file (no inheritance).  Same shared header, same
// private cursors -- and the same defect.  Same oracles as TEST 2.
// Failed on pre-fix code, BY DESIGN; passes with the fix in this PR.
// ===========================================================================
TEST_CASE("Independently opened writers on one file keep documents intact",
          "[mpwriters][independent]") {
  const std::string path = get_temp_path("independent");
  cleanup_temp_file(path);
  TempFileGuard guard{path};

  constexpr uint32_t kChildren = 4;
  static_assert(kChildren >= 3,
                "the wrap-amplification bound only discriminates for N >= 3");
  const size_t mean_doc_total = kMeanDocBytes + kDocHeaderBytes;

  // The parent opens first (it creates and initializes the volume) and keeps
  // its view for the post-hoc integrity read and the shared wrap counter.  It
  // writes nothing.
  VolumeView view;
  REQUIRE(view.open(path, kLeaseDuration, kLeaseCeiling));
  Volume &vol = *view.volume;

  const VolumeStats fresh = vol.stats();
  REQUIRE(fresh.stripe_count == 1);
  const uint64_t data_capacity = fresh.stripe_bytes - fresh.bytes_used;
  REQUIRE(data_capacity > static_cast<uint64_t>(4 * 1024 * 1024));

  const uint32_t docs_per_child =
      static_cast<uint32_t>((3 * data_capacity) / (kChildren * mean_doc_total));
  REQUIRE(docs_per_child >= 50);

  SharedReports reports;
  REQUIRE(reports.map(kChildren));
  Barrier barrier;
  REQUIRE(barrier.create());

  std::vector<pid_t> pids;
  pids.reserve(kChildren);
  bool fork_failed = false;
  for (uint32_t c = 0; c < kChildren; ++c) {
    const pid_t pid = fork();
    if (pid < 0) {
      fork_failed = true;
      break;
    }
    if (pid == 0) {
      // ---- CHILD: its OWN Volume on the SAME path. ------------------------
      // try/catch: see TEST 2 (fix C1).  `new VolumeView()` throws on OOM.
      int rc = 90;
      try {
        rc = [&]() -> int {
          auto *own = new VolumeView();  // Deliberately leaked: we _exit().
          if (!own->open(path, kLeaseDuration, kLeaseCeiling)) {
            reports.ctl->aborted.store(1, std::memory_order_release);
            return 30;
          }
          if (!barrier.wait()) {
            reports.ctl->aborted.store(1, std::memory_order_release);
            return 31;
          }
          return run_lockstep_writer(*own->volume, *reports.ctl,
                                     reports.data[c], c, docs_per_child,
                                     kChildren);
        }();
      } catch (...) {
        reports.ctl->aborted.store(1, std::memory_order_release);
        rc = 91;
      }
      _exit(rc);
    }
    pids.push_back(pid);
  }

  if (fork_failed) {
    reports.ctl->aborted.store(1, std::memory_order_release);
  }
  barrier.release(pids.size());
  const ReapResult reaped = reap_all(pids, std::chrono::seconds(180));
  barrier.close_all();

  const uint64_t observed_wraps = vol.stats().wrap_count;
  const IntegrityScan scan = scan_documents(vol, kChildren, docs_per_child);
  const Totals t = aggregate(reports);
  reports.unmap();

  const uint64_t expected_wraps = t.bytes_ok / data_capacity;

  INFO("children=" << kChildren << " docs/child=" << docs_per_child
                   << " ok=" << t.ok << " busy=" << t.busy
                   << " nospace=" << t.nospace << " other=" << t.other
                   << " | hits=" << scan.hits << " misses=" << scan.misses
                   << " corrupted=" << scan.corrupted
                   << " damaged=" << scan.damaged.size() << " | data_capacity="
                   << data_capacity << " bytes_committed=" << t.bytes_ok
                   << " EXPECTED_WRAPS=" << expected_wraps
                   << " OBSERVED_WRAPS=" << observed_wraps
                   << " (defect signature: observed >= expected * " << kChildren
                   << " - 1 = " << (expected_wraps * kChildren - 1)
                   << "; correct code: observed ~= expected)"
                   << " | write_lock: force_releases=" << t.force_releases
                   << " escalations=" << t.escalations << " usurp_aborts="
                   << t.usurp_aborts << " live_holder_waits=" << t.live_waits);

  REQUIRE(reaped.all_exited);
  REQUIRE(all_exited_zero(reaped));
  REQUIRE_FALSE(fork_failed);

  if (!scan.damaged.empty()) {
    CAPTURE(scan.damaged.front().child, scan.damaged.front().seq,
            static_cast<int>(scan.damaged.front().verdict));
  }
  REQUIRE(scan.damaged.empty());
  REQUIRE(scan.corrupted == 0);
  REQUIRE(scan.hits > 0);

  REQUIRE(expected_wraps >= 2);
  REQUIRE(observed_wraps <= expected_wraps + 2);

  // KNOWN FLAKE SUSPECT (see TEST 2): on a badly oversubscribed sanitizer
  // runner a LIVE child stalled for several seconds while holding the
  // cross-process write lock can legitimately trigger a peer's escalation
  // takeover (and the usurp-abort / force-release path with it).  These three
  // assert "no recovery was NEEDED", not the defect under test -- so if this
  // test ever flakes in CI, suspect them FIRST.
  REQUIRE(t.escalations == 0);
  REQUIRE(t.usurp_aborts == 0);
  REQUIRE(t.force_releases == 0);
}

// ===========================================================================
// TEST 4 -- cross-process borrow protection.  This path was already
// correct: it passed before this PR and still does.
//
// In mmap mode Volume::acquire_borrow routes to
// MmapDirectory::borrow_acquire() and stamp_read_lease to
// MmapDirectory::stamp_lease_expiry -- both live in the SHARED header (the
// process-local local_borrow_shards are used ONLY on non-mmap stripes;
// verified in src/core/volume.cpp).  So a borrow held in ONE process must
// defer the wrap that ANOTHER process wants to perform.
//
// DEADLINE ORDERING IS LOAD-BEARING (fix C3).  The reader child holds the
// borrow under its own self-timeout; the parent only releases it AFTER the
// writer flood has been reaped (180s budget).  If the reader's hold window
// did not strictly dominate that budget, a slow box would let the reader
// self-terminate early, SKIPPING rh->close() -- the shared borrow slot would
// never be returned and FOUR assertions would cascade into false failures.
// Hold window: 300s > reap budget 180s.  kLeaseDuration is likewise 300s so
// a sanitizer-slowed run cannot lapse the lease mid-flood.
// ===========================================================================
TEST_CASE("A borrow held in one process defers another process's wrap",
          "[mpwriters][lease]") {
  const std::string path = get_temp_path("lease");
  cleanup_temp_file(path);
  TempFileGuard guard{path};

  constexpr uint32_t kWriters = 3;
  constexpr size_t kVictimBytes = static_cast<size_t>(64 * 1024);
  constexpr size_t kDocBytes = static_cast<size_t>(64 * 1024);
  constexpr uint64_t kReaderIntact = 1;  // ChildReport::flags bit
  constexpr auto kReaderHold = std::chrono::seconds(300);
  constexpr auto kWriterReap = std::chrono::seconds(180);
  static_assert(kReaderHold > kWriterReap,
                "the reader's hold window must dominate the writers' reap "
                "budget, or rh->close() is skipped and the borrow slot leaks");

  VolumeView view;
  REQUIRE(view.open(path, kLeaseDuration, kLeaseCeiling));
  Volume &vol = *view.volume;

  const VolumeStats fresh = vol.stats();
  REQUIRE(fresh.stripe_count == 1);
  const uint64_t data_capacity = fresh.stripe_bytes - fresh.bytes_used;
  const size_t doc_total = kDocBytes + kDocHeaderBytes;
  // Enough for ~2 full passes across the writers: they WILL want to wrap.
  const uint32_t docs_per_writer =
      static_cast<uint32_t>((2 * data_capacity) / (kWriters * doc_total));
  REQUIRE(docs_per_writer >= 30);

  // The victim is the FIRST document: it sits at data_area_start, the exact
  // region a wrap overwrites first.
  const CacheKey victim_key("mpw4-victim");
  const auto victim = make_content(9, 0, kVictimBytes);
  REQUIRE(write_doc(vol, victim_key, std::span<const std::byte>(victim)) ==
          CacheError::Success);
  REQUIRE(vol.stats().wrap_count == 0);

  SharedReports reports;
  REQUIRE(reports.map(kWriters + 1));  // slot 0 = reader, 1.. = writers

  int ready_fds[2] = {-1, -1};
  int release_fds[2] = {-1, -1};
  REQUIRE(::pipe(ready_fds) == 0);
  REQUIRE(::pipe(release_fds) == 0);

  // The writers' start barrier is created BEFORE any child exists (fix m1):
  // a REQUIRE while the reader child is alive is a collect-then-assert
  // violation -- a failure there would orphan the reader into CI.
  Barrier barrier;
  REQUIRE(barrier.create());

  // ---- fork the READER: it takes a disk borrow and HOLDS it. --------------
  const pid_t reader_pid = fork();
  REQUIRE(reader_pid >= 0);
  if (reader_pid == 0) {
    int rc = 90;
    try {
      rc = [&]() -> int {
        barrier.close_all();  // the reader is not a barrier participant
        ::close(ready_fds[0]);
        ::close(release_fds[1]);
        ChildReport &rep = reports.data[0];

        auto rh = vol.read_sync(victim_key);
        if (!rh.has_value()) return 40;
        if (!rh->mapped_view().has_value()) return 41;  // a real borrow
        if (!content_equals(rh->content(),
                            std::span<const std::byte>(victim))) {
          return 42;
        }

        const unsigned char ready = 1;
        (void)!::write(ready_fds[1], &ready, 1);

        // Hold the borrow, renewing the lease, until the parent releases us.
        bool intact = true;
        const int flags = ::fcntl(release_fds[0], F_GETFL);
        (void)::fcntl(release_fds[0], F_SETFL, flags | O_NONBLOCK);
        const auto deadline = std::chrono::steady_clock::now() + kReaderHold;
        bool released = false;
        while (std::chrono::steady_clock::now() < deadline) {
          (void)rh->renew_lease();
          if (!content_equals(rh->content(),
                              std::span<const std::byte>(victim))) {
            intact = false;  // Borrowed bytes changed under us: lease broken.
          }
          unsigned char go = 0;
          if (::read(release_fds[0], &go, 1) == 1) {
            released = true;
            break;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        // Belt and braces: the deadline is only a runaway bound.  It must
        // never fire in a healthy run (see the static_assert above).
        if (!released) return 43;

        rep.flags = intact ? kReaderIntact : 0;
        fill_lock_counters(rep, vol);
        rh->close();  // MUST run: this returns the shared borrow slot.
        return 0;
      }();
    } catch (...) {
      rc = 91;
    }
    _exit(rc);
  }
  ::close(ready_fds[1]);
  ready_fds[1] = -1;

  // Wait for the reader to actually hold the borrow before anything else runs.
  // BOUNDED (fix C2): an unbounded ::read() here would hand a wedged reader
  // the entire CI job budget.
  const bool reader_ready =
      wait_for_byte(ready_fds[0], std::chrono::seconds(60));
  bool reader_killed = false;
  if (!reader_ready) {
    ::kill(reader_pid, SIGKILL);
    int st = 0;
    ::waitpid(reader_pid, &st, 0);
    reader_killed = true;
  }

  // ---- fork the WRITERS: they flood the stripe and must be DEFERRED. ------
  std::vector<pid_t> writer_pids;
  writer_pids.reserve(kWriters);
  bool fork_failed = false;
  if (reader_ready) {
    for (uint32_t w = 0; w < kWriters; ++w) {
      const pid_t pid = fork();
      if (pid < 0) {
        fork_failed = true;
        break;
      }
      if (pid == 0) {
        int rc = 90;
        try {
          rc = [&]() -> int {
            ::close(release_fds[0]);
            ::close(release_fds[1]);  // fix m3: the writers never use it
            ::close(ready_fds[0]);
            if (!barrier.wait()) return 50;
            ChildReport &rep = reports.data[w + 1];
            for (uint32_t s = 0; s < docs_per_writer; ++s) {
              const auto payload = make_payload(100 + w, s, kDocBytes);
              ++rep.attempted;
              const CacheError err =
                  write_doc(vol, CacheKey(doc_key_string(100 + w, s)),
                            std::span<const std::byte>(payload));
              switch (err) {
                case CacheError::Success:
                  ++rep.ok;
                  rep.bytes_ok += kDocBytes + kDocHeaderBytes;
                  break;
                case CacheError::Busy:
                  ++rep.busy;
                  break;
                case CacheError::NoSpace:  // <- the deferred wrap
                  ++rep.nospace;
                  break;
                default:
                  ++rep.other_error;
                  break;
              }
            }
            fill_lock_counters(rep, vol);
            return rep.other_error == 0 ? 0 : 51;
          }();
        } catch (...) {
          rc = 91;
        }
        _exit(rc);
      }
      writer_pids.push_back(pid);
    }
    barrier.release(writer_pids.size());
  }

  const ReapResult writers_reaped = reap_all(writer_pids, kWriterReap);
  barrier.close_all();

  // The borrow is still held by the reader: the shared wrap counter MUST NOT
  // have advanced.
  const uint64_t wraps_under_borrow = vol.stats().wrap_count;
  const uint64_t borrows_under_hold = vol.stats().borrows_outstanding;

  // Release the reader, reap it (its close() returns the shared borrow slot).
  // The parent still holds release_fds[0], so this write can never SIGPIPE
  // even if the reader is already gone.
  ReapResult reader_reaped;
  if (!reader_killed) {
    const unsigned char go = 1;
    (void)!::write(release_fds[1], &go, 1);
    reader_reaped = reap_all({reader_pid}, std::chrono::seconds(60));
  } else {
    reader_reaped.all_exited = false;
  }

  // ---- writes must RESUME once the borrow is gone. -----------------------
  const uint64_t borrows_after_release = vol.stats().borrows_outstanding;
  uint32_t resumed_ok = 0;
  for (uint32_t s = 0; s < 64; ++s) {
    const auto payload = make_payload(200, s, kDocBytes);
    if (write_doc(vol, CacheKey(doc_key_string(200, s)),
                  std::span<const std::byte>(payload)) == CacheError::Success) {
      ++resumed_ok;
    }
  }
  const uint64_t wraps_after_release = vol.stats().wrap_count;

  uint64_t writer_dropped_by_lease = 0;
  uint64_t writer_nospace = 0;
  uint64_t writer_ok = 0;
  uint64_t writer_other = 0;
  for (uint32_t w = 0; w < kWriters; ++w) {
    const ChildReport &r = reports.data[w + 1];
    writer_dropped_by_lease += r.writes_dropped_by_lease;
    writer_nospace += r.nospace;
    writer_ok += r.ok;
    writer_other += r.other_error;
  }
  const uint64_t reader_flags = reports.data[0].flags;
  reports.unmap();
  ::close(ready_fds[0]);
  ::close(release_fds[0]);
  ::close(release_fds[1]);

  INFO("reader_ready=" << reader_ready << " reader_killed=" << reader_killed
                       << " borrows_under_hold=" << borrows_under_hold
                       << " borrows_after_release=" << borrows_after_release
                       << " wraps_under_borrow=" << wraps_under_borrow
                       << " wraps_after_release=" << wraps_after_release
                       << " | writers: ok=" << writer_ok << " nospace="
                       << writer_nospace << " other=" << writer_other
                       << " writes_dropped_by_lease=" << writer_dropped_by_lease
                       << " | resumed_ok=" << resumed_ok);

  REQUIRE(reader_ready);  // false => the reader wedged and was SIGKILLed
  REQUIRE_FALSE(fork_failed);
  REQUIRE(all_exited_zero(writers_reaped));
  REQUIRE(all_exited_zero(reader_reaped));

  // The borrow was live and visible ACROSS processes (shared header slot).
  REQUIRE(borrows_under_hold >= 1);
  REQUIRE(borrows_after_release == 0);
  // The reader's borrowed bytes were never overwritten.
  REQUIRE(reader_flags == kReaderIntact);
  // The step that would reuse the borrowed bytes was DEFERRED for the whole
  // flood.  Flush mode: that step is the wrap itself, so none happened.
  // Wrap retention: the wrap is ungated and happens exactly once; the
  // frontier advance over the borrowed chunk is what waited.
  REQUIRE(wraps_under_borrow == (VolumeConfig{}.wrap_retention ? 1U : 0U));
  // ...and the writers noticed: the fills were dropped by the lease gate.
  REQUIRE(writer_dropped_by_lease > 0);
  REQUIRE(writer_nospace > 0);
  // Once the borrow is released, writes resume and the stripe wraps.
  REQUIRE(resumed_ok > 0);
  REQUIRE(wraps_after_release > 0);
}

#endif  // _WIN32
