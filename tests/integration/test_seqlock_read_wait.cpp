// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// A lock-free directory reader must never report "not found" merely because
// it ran out of seqlock retries (issue #21).
//
// A writer publishes "bucket busy" (odd version), mutates, then publishes
// "done" (even).  Descheduled INSIDE that window, it holds the bucket for a
// scheduling quantum.  Readers used to give up after a fixed 100 retries and
// report a miss for a key that was present throughout.  Now they wait
// (SeqlockReadWait: the old fast retries, then a time-bounded, yielding
// wait) and, only if the budget runs out, report Busy -- never NotFound.
//
// Each test parks a writer in its odd window on the key's bucket through a
// test seam (holding the stripe mutex, as every production mutator does),
// and observes, through SeqlockReadWait's timed-phase counter, that the
// reader got past the point where the old fixed retry count gave up.

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "core/directory.hpp"
#include "core/mmap_directory.hpp"
#include "core/volume.hpp"
#include "cyclone/alternate.hpp"
#include "cyclone/config.hpp"
#include "cyclone/cyclone_c.h"
#include "cyclone/error.hpp"
#include "cyclone/key.hpp"
#include "support/temp_cache.hpp"

using namespace cyclone;

namespace {

constexpr size_t kVolumeBytes = size_t{16} * 1024 * 1024;
constexpr size_t kDocBytes = 2048;

// Long enough that the "waits it out" tests never depend on timing: the
// writer is released by the test, not by the budget.
constexpr uint64_t kUnboundedBudgetUs = uint64_t{60} * 1000 * 1000;
// Short enough to keep the "budget spent" tests fast.
constexpr uint64_t kShortBudgetUs = 2000;

// Sets the SeqlockReadWait budget override for one scope.
struct BudgetOverride {
  explicit BudgetOverride(uint64_t us) {
    SeqlockReadWait::s_budget_us_for_test.store(us);
  }
  ~BudgetOverride() { SeqlockReadWait::s_budget_us_for_test.store(0); }
  BudgetOverride(const BudgetOverride &) = delete;
  BudgetOverride &operator=(const BudgetOverride &) = delete;
};

// Blocks until some probe has entered SeqlockReadWait's timed phase since
// `baseline`, i.e. retried past the old fixed budget.  Bounded so a broken
// build fails instead of hanging.
bool wait_for_timed_phase(uint64_t baseline) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (SeqlockReadWait::s_timed_waits_for_test.load() == baseline) {
    if (std::chrono::steady_clock::now() > deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

// Directory and MmapDirectory under one interface for the unit cases.
struct InMemoryDir {
  Directory dir{64};
  void begin(const CacheKey &key) { dir.begin_bucket_write_for_test(key); }
  void end(const CacheKey &key) { dir.end_bucket_write_for_test(key); }
};

struct MappedDir {
  static constexpr size_t kBuckets = 64;
  std::vector<std::byte> region =
      std::vector<std::byte>(MmapDirectory::required_size(kBuckets));
  MmapDirectory dir =
      *MmapDirectory::init(std::span<std::byte>(region), kBuckets);
  void begin(const CacheKey &key) { dir.begin_bucket_write_for_test(key); }
  void end(const CacheKey &key) { dir.end_bucket_write_for_test(key); }
};

template <typename Dir>
void directory_waits_out_parked_writer() {
  BudgetOverride budget(kUnboundedBudgetUs);
  Dir d;
  const CacheKey key("parked-writer-dir");
  REQUIRE(d.dir.insert(key, 4096, 512));

  const uint64_t baseline = SeqlockReadWait::s_timed_waits_for_test.load();
  d.begin(key);  // the writer is now "descheduled" mid-update

  std::atomic<bool> done{false};
  bool complete = false;
  size_t seen = 0;
  std::thread reader([&] {
    complete = d.dir.probe_each(key, [&](const DirEntry &) {
      ++seen;
      return false;
    });
    done.store(true);
  });

  // The reader retried past the old budget -- where it used to report a
  // miss -- and is still waiting.
  const bool reached = wait_for_timed_phase(baseline);
  const bool still_waiting = !done.load();
  d.end(key);  // the writer runs again and publishes
  reader.join();

  REQUIRE(reached);
  REQUIRE(still_waiting);
  REQUIRE(complete);
  REQUIRE(seen == 1);
}

template <typename Dir>
void directory_reports_unknown_after_budget() {
  BudgetOverride budget(kShortBudgetUs);
  Dir d;
  const CacheKey key("stuck-writer-dir");
  REQUIRE(d.dir.insert(key, 4096, 512));

  d.begin(key);
  size_t seen = 0;
  const auto start = std::chrono::steady_clock::now();
  const bool complete = d.dir.probe_each(key, [&](const DirEntry &) {
    ++seen;
    return false;
  });
  const auto waited = std::chrono::steady_clock::now() - start;
  d.end(key);

  // Unknown, not "absent": the probe says it never saw the bucket
  // consistent, and it waited at least the budget before saying so.
  REQUIRE_FALSE(complete);
  REQUIRE(seen == 0);
  REQUIRE(waited >= std::chrono::microseconds(kShortBudgetUs));

  // Released: the same probe answers again.
  REQUIRE(d.dir.probe_each(key, [&](const DirEntry &) {
    ++seen;
    return false;
  }));
  REQUIRE(seen == 1);
}

std::vector<std::byte> make_content(std::byte fill) {
  return std::vector<std::byte>(kDocBytes, fill);
}

std::shared_ptr<Volume> open_volume(const std::string &path, bool mmap_dir) {
  VolumeConfig vc;
  vc.path = path;
  vc.size = kVolumeBytes;
  vc.verify_checksum_on_read = true;
  MultiProcessConfig mp;
  if (mmap_dir) {
    // The production multi-process setting: mmap'd directory, 0-of-1.
    mp.set_enabled(true).set_process_index(0).set_total_processes(1);
  }
  auto volume = std::make_shared<Volume>(vc, mp);
  REQUIRE(volume->open().has_value());
  return volume;
}

class OriginalSelector : public StorageAlternateSelector {
 public:
  [[nodiscard]] std::optional<size_t> select(
      std::span<const AlternateInfo> alternates,
      const AlternateSelectionContext & /*ctx*/) const override {
    for (size_t i = 0; i < alternates.size(); ++i) {
      if (alternates[i].id == AlternateId::Original) {
        return i;
      }
    }
    return std::nullopt;
  }
};

enum class ReaderKind { kReadSync, kExists, kReadAlternate, kListAlternates };

const char *name_of(ReaderKind kind) {
  switch (kind) {
    case ReaderKind::kReadSync:
      return "read_sync";
    case ReaderKind::kExists:
      return "exists_sync";
    case ReaderKind::kReadAlternate:
      return "read_alternate_sync";
    case ReaderKind::kListAlternates:
      return "list_alternates_sync";
  }
  return "?";
}

// One read of `key`: Success when it saw the key's Original, else the error
// it reported (NotFound for a clean miss or an empty answer).
CacheError read_once(Volume &volume, ReaderKind kind, const CacheKey &key) {
  switch (kind) {
    case ReaderKind::kReadSync: {
      auto rh = volume.read_sync(key);
      if (!rh) {
        return rh.error();
      }
      return rh->content().size() == kDocBytes ? CacheError::Success
                                               : CacheError::NotFound;
    }
    case ReaderKind::kExists: {
      auto found = volume.exists_sync(key);
      if (!found) {
        return found.error();
      }
      return *found ? CacheError::Success : CacheError::NotFound;
    }
    case ReaderKind::kReadAlternate: {
      OriginalSelector selector;
      auto rh = volume.read_alternate_sync(key, selector, {});
      if (!rh) {
        return rh.error();
      }
      return rh->content().size() == kDocBytes ? CacheError::Success
                                               : CacheError::NotFound;
    }
    case ReaderKind::kListAlternates: {
      auto alts = volume.list_alternates_sync(key);
      if (!alts) {
        return alts.error();
      }
      for (const auto &alt : *alts) {
        if (alt.id == AlternateId::Original) {
          return CacheError::Success;
        }
      }
      return CacheError::NotFound;
    }
  }
  return CacheError::InternalError;
}

bool write_for(Volume &volume, ReaderKind kind, const CacheKey &key,
               std::span<const std::byte> content) {
  const bool alternate =
      kind == ReaderKind::kReadAlternate || kind == ReaderKind::kListAlternates;
  auto wh = alternate ? volume.write_alternate_sync(key, AlternateId::Original,
                                                    content.size())
                      : volume.write_sync(key, content.size());
  return wh.has_value() && wh->write_sync(content).has_value() &&
         wh->close_sync().has_value();
}

constexpr std::array<ReaderKind, 4> kReaders = {
    ReaderKind::kReadSync, ReaderKind::kExists, ReaderKind::kReadAlternate,
    ReaderKind::kListAlternates};

}  // namespace

TEST_CASE(
    "Seqlock read wait: a directory probe waits out a writer parked past the "
    "old retry count and then answers",
    "[directory][mmap_directory][concurrent][lockfree][regression]") {
  SECTION("Directory") { directory_waits_out_parked_writer<InMemoryDir>(); }
  SECTION("MmapDirectory") { directory_waits_out_parked_writer<MappedDir>(); }
}

TEST_CASE(
    "Seqlock read wait: a directory probe whose bucket stays busy past the "
    "budget reports unknown, not absent",
    "[directory][mmap_directory][concurrent][lockfree][regression]") {
  SECTION("Directory") {
    directory_reports_unknown_after_budget<InMemoryDir>();
  }
  SECTION("MmapDirectory") {
    directory_reports_unknown_after_budget<MappedDir>();
  }
}

TEST_CASE(
    "Seqlock read wait: every lock-free reader waits out a parked writer "
    "instead of reporting a false miss",
    "[concurrent][lockfree][regression]") {
  BudgetOverride budget(kUnboundedBudgetUs);
  for (const bool mmap_dir : {false, true}) {
    for (const ReaderKind kind : kReaders) {
      CAPTURE(mmap_dir, name_of(kind));
      TempCacheDir tmp("seqwait");
      auto volume = open_volume(tmp.path(), mmap_dir);
      const CacheKey key("parked-writer-volume");
      REQUIRE(write_for(*volume, kind, key, make_content(std::byte{0x5A})));
      REQUIRE(read_once(*volume, kind, key) == CacheError::Success);

      const uint64_t baseline = SeqlockReadWait::s_timed_waits_for_test.load();
      volume->begin_bucket_write_for_test(key);

      std::atomic<bool> done{false};
      CacheError outcome = CacheError::InternalError;
      std::thread reader([&] {
        outcome = read_once(*volume, kind, key);
        done.store(true);
      });

      const bool reached = wait_for_timed_phase(baseline);
      const bool still_waiting = !done.load();
      volume->end_bucket_write_for_test(key);
      reader.join();

      REQUIRE(reached);
      REQUIRE(still_waiting);
      REQUIRE(outcome == CacheError::Success);
      REQUIRE(volume->stats().directory_read_timeouts == 0);
    }
  }
}

TEST_CASE(
    "Seqlock read wait: a reader whose bucket stays busy past the budget "
    "reports Busy, never NotFound",
    "[concurrent][lockfree][regression]") {
  BudgetOverride budget(kShortBudgetUs);
  for (const bool mmap_dir : {false, true}) {
    TempCacheDir tmp("seqbusy");
    auto volume = open_volume(tmp.path(), mmap_dir);
    uint64_t timeouts = 0;
    for (const ReaderKind kind : kReaders) {
      CAPTURE(mmap_dir, name_of(kind));
      const CacheKey key(std::string("stuck-writer-volume-") + name_of(kind));
      REQUIRE(write_for(*volume, kind, key, make_content(std::byte{0x3C})));

      volume->begin_bucket_write_for_test(key);
      // Read from another thread, as a real reader would be: the parking
      // thread holds the stripe mutex.
      CacheError outcome = CacheError::InternalError;
      std::thread reader([&] { outcome = read_once(*volume, kind, key); });
      reader.join();
      volume->end_bucket_write_for_test(key);

      REQUIRE(outcome == CacheError::Busy);
      REQUIRE(volume->stats().directory_read_timeouts == ++timeouts);
      // The key was there all along.
      REQUIRE(read_once(*volume, kind, key) == CacheError::Success);
    }
  }
}

TEST_CASE("Seqlock read wait: Busy has its own C API code, appended",
          "[c_api][regression]") {
  // Appended at the tail: every existing code keeps its value.
  STATIC_REQUIRE(CYCLONE_OBJECT_TOO_LARGE == 11);
  STATIC_REQUIRE(CYCLONE_BUSY == CYCLONE_OBJECT_TOO_LARGE + 1);
  STATIC_REQUIRE(CYCLONE_BUSY != CYCLONE_NOT_FOUND);
}
