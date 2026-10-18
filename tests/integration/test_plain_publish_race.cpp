// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Two processes that overwrite one key at the same moment must leave exactly
// one directory entry for it.
//
// A plain write elects the key's existing entry (the one to update in place)
// under the stripe mutex, which is per process, after it has released the
// cross-process write lock.  Another process can publish the same key
// between that election and the insert.  An insert made against the election
// as it WAS then lands beside the other process's entry: two live entries for
// one key.  Reads stop at the first one, so the other write stays shadowed
// until a third write of the key clears one of them.
//
// Two Volume views on one file stand for the two processes (private stripe
// mutex and cursor each, shared directory and write lock).  The order is
// forced through the writer seam that fires between the election and the
// publish: the first writer is parked there, the second writer runs to
// completion, then the first writer publishes.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/directory.hpp"
#include "core/mmap_directory.hpp"
#include "core/volume.hpp"
#include "cyclone/config.hpp"
#include "cyclone/error.hpp"
#include "cyclone/key.hpp"
#include "support/temp_cache.hpp"

using namespace cyclone;

namespace {

// One stripe, so every write below meets the same directory and write lock.
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
CacheError write_plain(Volume &volume, const CacheKey &key,
                       std::string_view text) {
  const auto bytes = std::as_bytes(std::span(text.data(), text.size()));
  auto handle = volume.write_sync(key, bytes.size());
  if (!handle.has_value()) {
    return handle.error();
  }
  if (auto written = handle->write_sync(bytes); !written.has_value()) {
    return written.error();
  }
  auto closed = handle->close_sync();
  return closed.has_value() ? CacheError::Success : closed.error();
}

std::optional<std::string> read_plain(Volume &volume, const CacheKey &key) {
  auto handle = volume.read_sync(key);
  if (!handle.has_value()) {
    return std::nullopt;
  }
  const auto content = handle->content();
  return std::string(reinterpret_cast<const char *>(content.data()),
                     content.size());
}

// Directory entries carrying `key`'s tag in the current phase.
size_t entries_for(Volume &volume, const CacheKey &key) {
  MmapDirectory *dir = volume.mmap_directory_for_test(key);
  REQUIRE(dir != nullptr);
  size_t count = 0;
  REQUIRE(dir->probe_each(key, [&](const DirEntry & /*entry*/) {
    ++count;
    return true;
  }));
  return count;
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
  ~SeamGuard() { Volume::s_writer_seam_for_test = {}; }
};

struct RaceOutcome {
  bool first_parked = false;
  CacheError first_result = CacheError::InternalError;
  CacheError second_result = CacheError::InternalError;
};

// `first` elects and is parked before its publish, `second` writes the same
// key to completion, then `first` publishes.
RaceOutcome race(Volume &first, std::string_view first_text, Volume &second,
                 std::string_view second_text, const CacheKey &key) {
  RaceOutcome outcome;
  std::atomic<bool> first_parked{false};
  std::atomic<bool> release_first{false};
  std::atomic<bool> parked_once{false};
  std::atomic<std::thread::id> first_id{};
  SeamGuard guard;
  Volume::s_writer_seam_for_test = [&](Volume::WriterSeam at) {
    if (at != Volume::WriterSeam::kBeforePlainPublish ||
        std::this_thread::get_id() != first_id.load() ||
        parked_once.exchange(true)) {
      return;
    }
    first_parked.store(true);
    while (!release_first.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  };

  std::atomic<CacheError> first_result{CacheError::InternalError};
  std::thread first_thread([&] {
    first_id.store(std::this_thread::get_id());
    first_result.store(write_plain(first, key, first_text));
  });
  outcome.first_parked = wait_for([&] { return first_parked.load(); });
  if (outcome.first_parked) {
    outcome.second_result = write_plain(second, key, second_text);
  }
  release_first.store(true);
  first_thread.join();
  outcome.first_result = first_result.load();
  return outcome;
}

}  // namespace

TEST_CASE("Racing plain writes of a new key leave one entry",
          "[publishrace][multiprocess]") {
  const TempCacheDir dir("plainrace");
  ProcessView a;
  ProcessView b;
  REQUIRE(a.open(dir.path()));
  REQUIRE(b.open(dir.path()));
  REQUIRE(a.volume->stats().stripe_count == 1);

  const CacheKey key("http://example.test/plain/new.css");
  const RaceOutcome out =
      race(*a.volume, "written by a", *b.volume, "written by b", key);
  REQUIRE(out.first_parked);
  CHECK(out.first_result == CacheError::Success);
  CHECK(out.second_result == CacheError::Success);

  CHECK(entries_for(*a.volume, key) == 1);
  // The write that published last is the one both views read.
  CHECK(read_plain(*a.volume, key) ==
        std::optional<std::string>("written by a"));
  CHECK(read_plain(*b.volume, key) ==
        std::optional<std::string>("written by a"));
}

TEST_CASE("Racing plain overwrites of an existing key leave one entry",
          "[publishrace][multiprocess]") {
  const TempCacheDir dir("plainrace-existing");
  ProcessView a;
  ProcessView b;
  REQUIRE(a.open(dir.path()));
  REQUIRE(b.open(dir.path()));
  REQUIRE(a.volume->stats().stripe_count == 1);

  const CacheKey key("http://example.test/plain/existing.css");
  REQUIRE(write_plain(*a.volume, key, "original") == CacheError::Success);
  REQUIRE(entries_for(*a.volume, key) == 1);

  const RaceOutcome out =
      race(*a.volume, "rewritten by a", *b.volume, "rewritten by b", key);
  REQUIRE(out.first_parked);
  CHECK(out.first_result == CacheError::Success);
  CHECK(out.second_result == CacheError::Success);

  CHECK(entries_for(*a.volume, key) == 1);
  CHECK(read_plain(*a.volume, key) ==
        std::optional<std::string>("rewritten by a"));
  CHECK(read_plain(*b.volume, key) ==
        std::optional<std::string>("rewritten by a"));

  // A third write replaces the one entry; nothing older resurfaces.
  REQUIRE(write_plain(*b.volume, key, "third") == CacheError::Success);
  CHECK(entries_for(*a.volume, key) == 1);
  CHECK(read_plain(*a.volume, key) == std::optional<std::string>("third"));
}
