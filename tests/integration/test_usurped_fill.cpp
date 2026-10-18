// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// A writer whose write lock was taken over while it was stalled inside its
// fill must not have its document served in place of the one the new holder
// published.
//
// The cross-process write lock is held from the reservation until the fill
// is durable, and the shared cursor advances only then.  A holder that stalls
// inside its fill longer than the escalation deadline is taken over; the new
// holder reserves at the same, un-advanced cursor, fills and publishes.  The
// stalled holder's commit is refused, but a pwrite already in flight cannot
// be recalled: when it lands it replaces the new holder's bytes with a
// complete, self-consistent document of its own.  If both wrote the same key,
// that document passes every check a read makes on the document alone (magic,
// lengths, key, checksum), and the directory entry the new holder published
// would serve it: the older write, served as current, by a write that
// reported failure.
//
// The directory entry therefore carries the fill serial of the write that
// published it, and the document carries the serial of the fill that wrote
// it; a read serves the document only when the two agree.
//
// Two Volume views on one file stand for the two processes.  The stalled
// holder is parked in the tear gate (lock held, reservation made, nothing
// written yet); the other view's write takes the lock over after a shortened
// escalation, fills and publishes; then the parked holder's fill lands.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/mmap_directory.hpp"
#include "core/volume.hpp"
#include "cyclone/alternate.hpp"
#include "cyclone/config.hpp"
#include "cyclone/error.hpp"
#include "cyclone/key.hpp"
#include "support/temp_cache.hpp"

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>

using namespace cyclone;

namespace {

// One stripe, so both writers meet the same write lock and cursor.
constexpr size_t kVolumeBytes = size_t{16} * 1024 * 1024;

struct ProcessView {
  std::shared_ptr<Volume> volume;
  std::vector<std::shared_ptr<VolumeReadAnchor>> anchors;

  ProcessView() = default;
  ProcessView(const ProcessView &) = delete;
  ProcessView &operator=(const ProcessView &) = delete;

  bool open(const std::string &path) {
    VolumeConfig config;
    config.path = path;
    config.size = kVolumeBytes;
    config.verify_checksum_on_read = true;
    MultiProcessConfig multi_process;
    multi_process.set_enabled(true).set_process_index(0).set_total_processes(1);
    volume = std::make_shared<Volume>(config, multi_process);
    if (!volume->open().has_value()) {
      volume.reset();
      return false;
    }
    anchors = volume->make_read_anchors();
    volume->set_read_anchors(anchors.data(), anchors.size());
    return true;
  }

  ~ProcessView() {
    if (volume) {
      volume->set_read_anchors(nullptr, 0);
      volume->close();
    }
  }
};

// Never asserts: used from writer threads, where Catch2 must not be touched.
CacheError write_doc(Volume &volume, const CacheKey &key,
                     std::optional<AlternateId> id, std::string_view text) {
  const auto bytes = std::as_bytes(std::span(text.data(), text.size()));
  auto handle = id ? volume.write_alternate_sync(key, *id, bytes.size())
                   : volume.write_sync(key, bytes.size());
  if (!handle.has_value()) {
    return handle.error();
  }
  if (auto written = handle->write_sync(bytes); !written.has_value()) {
    return written.error();
  }
  auto closed = handle->close_sync();
  return closed.has_value() ? CacheError::Success : closed.error();
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

struct Served {
  std::optional<std::string> text;
  uint64_t content_offset = ReadHandle::kNoFileOffset;
};

Served read_doc(Volume &volume, const CacheKey &key,
                std::optional<AlternateId> id) {
  Served out;
  std::expected<ReadHandle, CacheError> handle =
      id ? volume.read_alternate_sync(key, ExactIdSelector(*id), {})
         : volume.read_sync(key);
  if (!handle.has_value()) {
    return out;
  }
  const auto content = handle->content();
  out.text = std::string(reinterpret_cast<const char *>(content.data()),
                         content.size());
  out.content_offset = handle->content_file_offset();
  return out;
}

std::string pread_text(const std::string &path, uint64_t offset, size_t n) {
  std::string out(n, '\0');
  const int fd = ::open(path.c_str(), O_RDONLY);
  REQUIRE(fd >= 0);
  const ssize_t got = ::pread(fd, out.data(), n, static_cast<off_t>(offset));
  ::close(fd);
  REQUIRE(got == static_cast<ssize_t>(n));
  return out;
}

bool wait_for(const std::function<bool()> &done) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(60);
  while (!done()) {
    if (std::chrono::steady_clock::now() > deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

struct SeamGuard {
  ~SeamGuard() {
    Volume::s_write_tear_gate_for_test = {};
    MmapDirectory::s_write_lock_max_live_waits_for_test.store(0);
  }
};

}  // namespace

TEST_CASE(
    "A taken-over writer's late fill is never served in place of the "
    "document the new holder published",
    "[multiprocess][writelock][usurp]") {
  for (const bool alternate : {false, true}) {
    CAPTURE(alternate);
    const std::optional<AlternateId> id =
        alternate ? std::optional<AlternateId>(AlternateId::Brotli)
                  : std::nullopt;
    const TempCacheDir dir("usurped-fill");
    ProcessView stalled;
    ProcessView usurper;
    REQUIRE(stalled.open(dir.path()));
    REQUIRE(usurper.open(dir.path()));
    REQUIRE(stalled.volume->stats().stripe_count == 1);
    const std::string file = dir.path();  // a bare Volume opens this file

    const CacheKey key("http://example.test/usurped/fill.css");
    // Same length, so the late fill covers the published document exactly.
    const std::string older = "older version written by the stalled holder";
    const std::string newer = "newer version written by the next lock holder";
    const std::string older_same =
        older + std::string(newer.size() - older.size(), '.');
    REQUIRE(older_same.size() == newer.size());

    SeamGuard guard;
    std::atomic<std::thread::id> stalled_id{};
    std::atomic<bool> parked{false};
    std::atomic<bool> release{false};
    Volume::s_write_tear_gate_for_test = [&](uint64_t, uint64_t) {
      if (std::this_thread::get_id() != stalled_id.load() ||
          parked.exchange(true)) {
        return;
      }
      while (!release.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    };

    std::atomic<CacheError> stalled_result{CacheError::InternalError};
    std::thread stalled_thread([&] {
      stalled_id.store(std::this_thread::get_id());
      stalled_result.store(write_doc(*stalled.volume, key, id, older_same));
    });
    const bool was_parked = wait_for([&] { return parked.load(); });

    CacheError usurper_result = CacheError::InternalError;
    Served published;
    if (was_parked) {
      // Take the lock over after a few waits instead of the production
      // escalation deadline.
      MmapDirectory::s_write_lock_max_live_waits_for_test.store(4);
      usurper_result = write_doc(*usurper.volume, key, id, newer);
      MmapDirectory::s_write_lock_max_live_waits_for_test.store(0);
      published = read_doc(*usurper.volume, key, id);
    }
    release.store(true);
    stalled_thread.join();

    REQUIRE(was_parked);
    REQUIRE(usurper_result == CacheError::Success);
    REQUIRE(published.text == std::optional<std::string>(newer));
    REQUIRE(published.content_offset != ReadHandle::kNoFileOffset);
    // The stalled holder's commit was refused...
    CHECK(stalled_result.load() == CacheError::Busy);
    // ...but its fill landed over the published document.
    REQUIRE(pread_text(file, published.content_offset, newer.size()) ==
            older_same);

    // Neither view serves the older document.
    for (Volume *view : {stalled.volume.get(), usurper.volume.get()}) {
      const Served now = read_doc(*view, key, id);
      CHECK(now.text != std::optional<std::string>(older_same));
    }

    // The next write of the key lands and is served.
    REQUIRE(write_doc(*usurper.volume, key, id, "third") ==
            CacheError::Success);
    CHECK(read_doc(*stalled.volume, key, id).text ==
          std::optional<std::string>("third"));
  }
}
#endif  // !_WIN32
