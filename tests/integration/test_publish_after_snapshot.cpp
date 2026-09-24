// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// A lock-free reader must not miss a key whose directory entry was
// republished between the reader's stripe snapshot and its directory probe.
//
// Every lock-free reader takes ONE StripeSnapshot (epoch, then write cursor)
// BEFORE it probes the directory, and admits each entry against it: the
// phase-ABA positional guard rejects a current-phase entry at or ahead of
// snap.cursor_rel.  A writer that commits in that window allocates its
// document at/after the cursor the reader sampled, advances the cursor and
// then updates the key's directory entry IN PLACE (the verified head is
// replaced, never inserted beside).  The reader's probe then sees only the
// new entry, rejects it as "ahead of the cursor", finds nothing else and
// reports a clean NotFound -- for a key that was continuously present.
//
// PageSpeed hit this as an intermittent "original became unreadable" in its
// cross-process burst test: ReadBestAlternate of the original missed while
// the worker appended other alternates of the same key.
//
// The test parks the reader at the kSnapshotDone seam (after the snapshot,
// before the probe) and commits one write of the SAME key in that window.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "core/volume.hpp"
#include "cyclone/alternate.hpp"
#include "cyclone/config.hpp"
#include "cyclone/key.hpp"
#include "support/temp_cache.hpp"

using namespace cyclone;

namespace {

constexpr size_t kVolumeBytes = size_t{16} * 1024 * 1024;
constexpr size_t kDocBytes = 2048;

std::vector<std::byte> make_content(std::byte fill) {
  return std::vector<std::byte>(kDocBytes, fill);
}

bool write_plain(Volume &volume, const CacheKey &key,
                 std::span<const std::byte> content) {
  auto wh = volume.write_sync(key, content.size());
  return wh.has_value() && wh->write_sync(content).has_value() &&
         wh->close_sync().has_value();
}

bool write_alt(Volume &volume, const CacheKey &key, AlternateId id,
               std::span<const std::byte> content) {
  auto wh = volume.write_alternate_sync(key, id, content.size());
  return wh.has_value() && wh->write_sync(content).has_value() &&
         wh->close_sync().has_value();
}

class ExactIdSelector : public StorageAlternateSelector {
 public:
  explicit ExactIdSelector(AlternateId target) : _target(target) {}
  [[nodiscard]] std::optional<size_t> select(
      std::span<const AlternateInfo> alternates,
      const AlternateSelectionContext & /*ctx*/) const override {
    for (size_t i = 0; i < alternates.size(); ++i) {
      if (alternates[i].id == _target) {
        return i;
      }
    }
    return std::nullopt;
  }

 private:
  AlternateId _target;
};

std::shared_ptr<Volume> open_volume(const std::string &path, bool mmap_dir) {
  VolumeConfig vc;
  vc.path = path;
  vc.size = kVolumeBytes;
  vc.verify_checksum_on_read = true;
  std::shared_ptr<Volume> volume;
  if (mmap_dir) {
    // The production multi-process setting: enabled, 0-of-1, mmap'd
    // directory, shared write cursor.
    MultiProcessConfig mp;
    mp.set_enabled(true).set_process_index(0).set_total_processes(1);
    volume = std::make_shared<Volume>(vc, mp);
  } else {
    volume = std::make_shared<Volume>(vc);
  }
  REQUIRE(volume->open().has_value());
  return volume;
}

struct Rendezvous {
  std::mutex m;
  std::condition_variable cv;
  bool arrived = false;
  bool released = false;
  void arrive_and_wait() {
    std::unique_lock<std::mutex> lk(m);
    arrived = true;
    cv.notify_all();
    cv.wait(lk, [&] { return released; });
  }
  void wait_arrived() {
    std::unique_lock<std::mutex> lk(m);
    cv.wait(lk, [&] { return arrived; });
  }
  void release() {
    std::unique_lock<std::mutex> lk(m);
    released = true;
    cv.notify_all();
  }
};

// Parks the bound thread ONCE at `seam`; every other thread (the writer's
// own snapshots included) passes straight through.
struct ReaderPause {
  Rendezvous rv;
  std::atomic<bool> fired{false};
  std::atomic<std::thread::id> only{};
  explicit ReaderPause(Volume::ReaderSeam seam) {
    Volume::s_reader_seam_for_test = [this, seam](Volume::ReaderSeam at) {
      if (at == seam && std::this_thread::get_id() == only.load() &&
          !fired.exchange(true)) {
        rv.arrive_and_wait();
      }
    };
  }
  void bind_this_thread() { only.store(std::this_thread::get_id()); }
  ~ReaderPause() { Volume::s_reader_seam_for_test = nullptr; }
  ReaderPause(const ReaderPause &) = delete;
  ReaderPause &operator=(const ReaderPause &) = delete;
};

enum class ReaderKind { kReadAlternate, kListAlternates, kReadSync, kExists };

const char *name_of(ReaderKind kind) {
  switch (kind) {
    case ReaderKind::kReadAlternate:
      return "read_alternate_sync";
    case ReaderKind::kListAlternates:
      return "list_alternates_sync";
    case ReaderKind::kReadSync:
      return "read_sync";
    case ReaderKind::kExists:
      return "exists_sync";
  }
  return "?";
}

bool is_alternate_reader(ReaderKind kind) {
  return kind == ReaderKind::kReadAlternate ||
         kind == ReaderKind::kListAlternates;
}

// Runs one read of `key` and reports whether it saw the key's Original.
bool reader_sees_original(Volume &volume, ReaderKind kind,
                          const CacheKey &key) {
  switch (kind) {
    case ReaderKind::kReadAlternate: {
      ExactIdSelector selector(AlternateId::Original);
      auto rh = volume.read_alternate_sync(key, selector, {});
      return rh.has_value() && rh->is_valid() &&
             rh->content().size() == kDocBytes;
    }
    case ReaderKind::kListAlternates: {
      auto alts = volume.list_alternates_sync(key);
      if (!alts.has_value()) {
        return false;
      }
      for (const auto &alt : *alts) {
        if (alt.id == AlternateId::Original) {
          return true;
        }
      }
      return false;
    }
    case ReaderKind::kReadSync: {
      auto rh = volume.read_sync(key);
      return rh.has_value() && rh->is_valid() &&
             rh->content().size() == kDocBytes;
    }
    case ReaderKind::kExists: {
      auto found = volume.exists_sync(key);
      return found.has_value() && *found;
    }
  }
  return false;
}

}  // namespace

TEST_CASE(
    "Publish after snapshot: a same-key write committed between a reader's "
    "snapshot and its probe never turns a present key into a miss",
    "[alternate][concurrent][lockfree][regression]") {
  for (const bool mmap_dir : {false, true}) {
    for (const ReaderKind kind :
         {ReaderKind::kReadAlternate, ReaderKind::kListAlternates,
          ReaderKind::kReadSync, ReaderKind::kExists}) {
      CAPTURE(mmap_dir, name_of(kind));
      TempCacheDir tmp("pubsnap");
      auto volume = open_volume(tmp.path(), mmap_dir);
      const CacheKey key("publish-after-snapshot");
      const auto original = make_content(std::byte{0x0A});
      const auto variant = make_content(std::byte{0x0B});

      if (is_alternate_reader(kind)) {
        REQUIRE(write_alt(*volume, key, AlternateId::Original, original));
      } else {
        REQUIRE(write_plain(*volume, key, original));
      }
      REQUIRE(reader_sees_original(*volume, kind, key));

      bool saw = false;
      {
        ReaderPause pause(Volume::ReaderSeam::kSnapshotDone);
        std::thread reader([&] {
          pause.bind_this_thread();
          saw = reader_sees_original(*volume, kind, key);
        });
        pause.rv.wait_arrived();
        // The reader holds a snapshot whose cursor predates this write.
        // The write lands at/after that cursor and replaces the key's
        // directory entry in place.
        if (is_alternate_reader(kind)) {
          REQUIRE(write_alt(*volume, key, AlternateId::Brotli, variant));
        } else {
          REQUIRE(write_plain(*volume, key, original));
        }
        pause.rv.release();
        reader.join();
      }
      CHECK(saw);
      // And the key is (still) present for a reader that starts now.
      CHECK(reader_sees_original(*volume, kind, key));
      volume->close();
    }
  }
}
