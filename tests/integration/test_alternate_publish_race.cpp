// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Two processes that write alternates of one key at the same moment must
// not lose one of them.
//
// An alternate write resolves the key's chain first (under the stripe mutex,
// which is per process) and publishes its new head later, after the data is
// written.  Another process can publish for the same key in between.  A
// publish made against the chain as it WAS then adds a second directory
// entry for the key, and the next write of the key keeps one of the two
// chains and clears the other -- dropping every alternate only that chain
// held, with no error anywhere.
//
// PageSpeed hit this as a stylesheet that stays unoptimized: a web-server
// process re-records the original while the optimizer writes the optimized
// copy, and the optimized copy is gone a moment later although its gzip and
// brotli siblings, written after it, are there.
//
// Two Volume views on one file stand for the two processes (private stripe
// mutex and cursor each, shared directory and write lock).  The order is
// forced through the write hook, which fires after a write resolved its
// chain and took the write lock, before it writes:
//
//   * the first writer is parked in the hook;
//   * the second writer resolves the same chain -- seen from outside as its
//     chain-depth statistic moving -- and waits for the write lock;
//   * the first writer is released and publishes;
//   * the second writer's hook call waits until the first write returned.
//
// The removal of a head alternate republishes its successor and has the same
// window; its own hook fires between its resolve and that publish.

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
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

using namespace cyclone;

namespace {

// One stripe, so every write below meets the same write lock.
constexpr size_t kVolumeBytes = size_t{16} * 1024 * 1024;

// The ids PageSpeed uses: the stored original, the optimized copy, and the
// optimized copy's gzip and brotli siblings.
constexpr auto kOriginal = static_cast<AlternateId>(0x0C);
constexpr auto kOptimized = static_cast<AlternateId>(0x08);
constexpr auto kOptimizedGzip = static_cast<AlternateId>(0x48);
constexpr auto kOptimizedBrotli = static_cast<AlternateId>(0x88);

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
CacheError write_alternate(Volume &volume, const CacheKey &key, AlternateId id,
                           std::string_view text) {
  const auto bytes = std::as_bytes(std::span(text.data(), text.size()));
  auto handle = volume.write_alternate_sync(key, id, bytes.size());
  if (!handle.has_value()) {
    return handle.error();
  }
  if (auto written = handle->write_sync(bytes); !written.has_value()) {
    return written.error();
  }
  auto closed = handle->close_sync();
  return closed.has_value() ? CacheError::Success : closed.error();
}

std::vector<int> list_ids(Volume &volume, const CacheKey &key) {
  std::vector<int> ids;
  auto listed = volume.list_alternates_sync(key);
  if (listed.has_value()) {
    for (const auto &alternate : *listed) {
      ids.push_back(static_cast<int>(static_cast<uint8_t>(alternate.id)));
    }
  }
  return ids;
}

std::string printed(const std::vector<int> &ids) {
  std::string out;
  for (const int id : ids) {
    if (!out.empty()) {
      out += ", ";
    }
    out += std::to_string(id);
  }
  return out.empty() ? std::string("(none)") : out;
}

bool contains(const std::vector<int> &ids, int id) {
  return std::find(ids.begin(), ids.end(), id) != ids.end();
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

std::optional<std::string> read_by_id(Volume &volume, const CacheKey &key,
                                      AlternateId id) {
  const ExactIdSelector selector(id);
  auto handle = volume.read_alternate_sync(key, selector, {});
  if (!handle.has_value()) {
    return std::nullopt;
  }
  const auto content = handle->content();
  std::string out(reinterpret_cast<const char *>(content.data()),
                  content.size());
  handle->close();
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

// Clears the test hooks on every exit path of a test case.
struct HookGuard {
  ~HookGuard() {
    Volume::s_write_tear_gate_for_test = {};
    Volume::s_remove_republish_gate_for_test = {};
  }
};

// For the cases with many writers at once: a writer that waits for a lock
// behind holders that keep changing gives up as Busy after a quarter of a
// second by default.  That is a different Busy from the one under test, so
// the wait is made long enough never to end here.
struct PatientLocks {
  PatientLocks() {
    MmapDirectory::s_lock_wait_cap_us_for_test.store(60ULL * 1000 * 1000);
  }
  ~PatientLocks() { MmapDirectory::s_lock_wait_cap_us_for_test.store(0); }
};

// Runs `body(i)` on `count` threads released together.
void run_together(int count, const std::function<void(int)> &body) {
  std::atomic<bool> go{false};
  std::vector<std::thread> threads;
  threads.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    threads.emplace_back([&, i] {
      while (!go.load()) {
        std::this_thread::yield();
      }
      body(i);
    });
  }
  go.store(true);
  for (std::thread &thread : threads) {
    thread.join();
  }
}

// The outcome of one forced race: `first` is parked after resolving the
// chain, `second` resolves the same chain, then `first` publishes before
// `second` does.
struct RaceOutcome {
  bool first_parked = false;
  bool second_resolved = false;
  CacheError first_result = CacheError::InternalError;
  CacheError second_result = CacheError::InternalError;
};

RaceOutcome race(Volume &first, AlternateId first_id,
                 std::string_view first_text, Volume &second,
                 AlternateId second_id, std::string_view second_text,
                 const CacheKey &key) {
  RaceOutcome outcome;
  const uint64_t depth_before = second.stats().alternate_max_chain_depth;
  std::atomic<int> hook_calls{0};
  std::atomic<bool> first_parked{false};
  std::atomic<bool> release_first{false};
  std::atomic<bool> first_returned{false};
  HookGuard guard;
  Volume::s_write_tear_gate_for_test = [&](uint64_t, uint64_t) {
    const int call = hook_calls.fetch_add(1);
    if (call == 0) {
      first_parked.store(true);
      while (!release_first.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    } else if (call == 1) {
      while (!first_returned.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
    // Later calls (a write that resolved again) pass straight through.
  };

  std::atomic<CacheError> first_result{CacheError::InternalError};
  std::atomic<CacheError> second_result{CacheError::InternalError};
  std::thread first_thread([&] {
    first_result.store(write_alternate(first, key, first_id, first_text));
    first_returned.store(true);
  });
  outcome.first_parked = wait_for([&] { return first_parked.load(); });
  std::thread second_thread;
  if (outcome.first_parked) {
    second_thread = std::thread([&] {
      second_result.store(write_alternate(second, key, second_id, second_text));
    });
    // The chain-depth high-water mark is raised right after the chain walk
    // and before the write lock is asked for.
    outcome.second_resolved = wait_for([&] {
      return second.stats().alternate_max_chain_depth > depth_before;
    });
  }
  release_first.store(true);
  first_thread.join();
  first_returned.store(true);
  if (second_thread.joinable()) {
    second_thread.join();
  }
  outcome.first_result = first_result.load();
  outcome.second_result = second_result.load();
  return outcome;
}

}  // namespace

TEST_CASE(
    "A write that resolved an older chain does not drop another "
    "writer's alternate",
    "[alternate][multiprocess][publishrace]") {
  const TempCacheDir dir("publishrace");
  ProcessView server;
  ProcessView optimizer;
  REQUIRE(server.open(dir.path()));
  REQUIRE(optimizer.open(dir.path()));
  REQUIRE(server.volume->stats().stripe_count == 1);

  const CacheKey key("http://example.test/styles/site.css");
  REQUIRE(write_alternate(*server.volume, key, kOriginal,
                          std::string(4307, 'o')) == CacheError::Success);
  REQUIRE(list_ids(*optimizer.volume, key) == std::vector<int>{0x0C});

  // The web server re-records the original and publishes FIRST; the
  // optimizer, which resolved the same chain, publishes second.
  const RaceOutcome outcome =
      race(*server.volume, kOriginal, std::string(4307, 'p'), *optimizer.volume,
           kOptimized, std::string(3899, 'm'), key);
  REQUIRE(outcome.first_parked);
  REQUIRE(outcome.second_resolved);
  REQUIRE(outcome.first_result == CacheError::Success);
  REQUIRE(outcome.second_result == CacheError::Success);

  // The optimizer writes the compressed siblings, as it always does.
  REQUIRE(write_alternate(*optimizer.volume, key, kOptimizedGzip,
                          std::string(1200, 'g')) == CacheError::Success);
  REQUIRE(write_alternate(*optimizer.volume, key, kOptimizedBrotli,
                          std::string(1000, 'b')) == CacheError::Success);

  const std::vector<int> through_optimizer = list_ids(*optimizer.volume, key);
  const std::vector<int> through_server = list_ids(*server.volume, key);
  INFO("through the optimizer's view: " << printed(through_optimizer));
  INFO("through the web server's view: " << printed(through_server));
  CHECK(contains(through_optimizer, 0x08));
  CHECK(contains(through_server, 0x08));
  CHECK(contains(through_optimizer, 0x0C));
  CHECK(contains(through_optimizer, 0x48));
  CHECK(contains(through_optimizer, 0x88));
  CHECK(std::count(through_optimizer.begin(), through_optimizer.end(), 0x0C) ==
        1);
  // The write that lost the race noticed, and resolved again.
  CHECK(optimizer.volume->stats().alternate_publish_retries >= 1);
  CHECK(server.volume->stats().alternate_publish_retries == 0);
}

TEST_CASE(
    "A re-recorded alternate that lost the publish race still replaces "
    "the older copy",
    "[alternate][multiprocess][publishrace]") {
  // The other order: the optimizer publishes first, and the web server's
  // re-record, which resolved the chain before that, publishes second.  The
  // key must end with the NEW original and the optimized copy -- not with
  // the old original because the newer one landed on a second head that the
  // next write cleared.
  const TempCacheDir dir("publishrace");
  ProcessView server;
  ProcessView optimizer;
  REQUIRE(server.open(dir.path()));
  REQUIRE(optimizer.open(dir.path()));

  const CacheKey key("http://example.test/styles/site.css");
  const std::string original_first(4307, 'o');
  const std::string original_second(4307, 'p');
  REQUIRE(write_alternate(*server.volume, key, kOriginal, original_first) ==
          CacheError::Success);

  const RaceOutcome outcome =
      race(*optimizer.volume, kOptimized, std::string(3899, 'm'),
           *server.volume, kOriginal, original_second, key);
  REQUIRE(outcome.first_parked);
  REQUIRE(outcome.second_resolved);
  REQUIRE(outcome.first_result == CacheError::Success);
  REQUIRE(outcome.second_result == CacheError::Success);

  REQUIRE(write_alternate(*optimizer.volume, key, kOptimizedGzip,
                          std::string(1200, 'g')) == CacheError::Success);

  const std::vector<int> ids = list_ids(*optimizer.volume, key);
  INFO("the key lists: " << printed(ids));
  CHECK(contains(ids, 0x08));
  CHECK(contains(ids, 0x48));
  CHECK(std::count(ids.begin(), ids.end(), 0x0C) == 1);
  const auto original = read_by_id(*optimizer.volume, key, kOriginal);
  REQUIRE(original.has_value());
  const bool newest_original_kept = *original == original_second;
  CHECK(newest_original_kept);
  CHECK(server.volume->stats().alternate_publish_retries >= 1);
}

TEST_CASE(
    "A write that loses the publish race many times in a row still "
    "lands",
    "[alternate][multiprocess][publishrace]") {
  // There is no number of lost rounds after which a write gives up.  Twelve
  // attempts of one write in a row find the key's head changed under them:
  // while the writer sits in the hook (chain resolved, slot reserved, write
  // lock held) the other process removes the current head, which needs only
  // the directory's own locks.  The thirteenth attempt is left alone and
  // publishes.  The key then holds the written alternate and exactly what
  // the removals left, with no second head.
  const TempCacheDir dir("publishrace");
  ProcessView server;
  ProcessView optimizer;
  REQUIRE(server.open(dir.path()));
  REQUIRE(optimizer.open(dir.path()));

  const CacheKey key("http://example.test/styles/site.css");
  const int lost_rounds = 12;
  // One alternate per lost round to remove, plus one that stays.
  for (int i = 0; i <= lost_rounds; ++i) {
    REQUIRE(write_alternate(*server.volume, key,
                            static_cast<AlternateId>(0x10 + i),
                            std::string(600, static_cast<char>('a' + i))) ==
            CacheError::Success);
  }

  std::atomic<int> hook_calls{0};
  std::atomic<int> failed_removes{0};
  HookGuard guard;
  Volume::s_write_tear_gate_for_test = [&](uint64_t, uint64_t) {
    const int call = hook_calls.fetch_add(1);
    if (call >= lost_rounds) {
      return;  // let this attempt through
    }
    // The head is the alternate written last that is still there.
    const auto head = static_cast<AlternateId>(0x10 + lost_rounds - call);
    if (!server.volume->remove_alternate_sync(key, head).has_value()) {
      failed_removes.fetch_add(1);
    }
  };
  const CacheError result = write_alternate(*optimizer.volume, key, kOptimized,
                                            std::string(3899, 'm'));
  Volume::s_write_tear_gate_for_test = {};

  CHECK(result == CacheError::Success);
  CHECK(hook_calls.load() == lost_rounds + 1);
  CHECK(failed_removes.load() == 0);
  // Every lost round here was noticed at the publish, after the document
  // had been written: the change landed while the writer held the lock.
  CHECK(optimizer.volume->stats().alternate_publish_retries ==
        static_cast<uint64_t>(lost_rounds));
  CHECK(optimizer.volume->stats().alternate_publish_rewrites ==
        static_cast<uint64_t>(lost_rounds));
  const std::vector<int> ids = list_ids(*server.volume, key);
  INFO("the key lists: " << printed(ids));
  CHECK(ids == (std::vector<int>{0x08, 0x10}));
  CHECK(list_ids(*optimizer.volume, key) == (std::vector<int>{0x08, 0x10}));
}

TEST_CASE(
    "A write that finds the key changed before it writes does not "
    "write its document twice",
    "[alternate][multiprocess][publishrace]") {
  // The cheap path, which is the common one: the writer resolved the chain,
  // then waited for the write lock, and the key changed while it waited.
  // It notices as soon as it holds the lock, before writing anything, gives
  // the reserved slot back and resolves again.
  //
  // A third process holds the write lock (parked in the hook while writing
  // another key).  The optimizer resolves the chain and waits.  The web
  // server removes the key's head -- no write lock needed.  The lock holder
  // is released.
  const TempCacheDir dir("publishrace");
  ProcessView server;
  ProcessView optimizer;
  ProcessView other;
  REQUIRE(server.open(dir.path()));
  REQUIRE(optimizer.open(dir.path()));
  REQUIRE(other.open(dir.path()));

  const CacheKey key("http://example.test/styles/site.css");
  const CacheKey other_key("http://example.test/other.css");
  REQUIRE(write_alternate(*server.volume, key, static_cast<AlternateId>(0x10),
                          std::string(600, 'a')) == CacheError::Success);
  REQUIRE(write_alternate(*server.volume, key, static_cast<AlternateId>(0x11),
                          std::string(600, 'b')) == CacheError::Success);
  const uint64_t depth_before =
      optimizer.volume->stats().alternate_max_chain_depth;

  std::atomic<int> hook_calls{0};
  std::atomic<bool> holder_parked{false};
  std::atomic<bool> release_holder{false};
  HookGuard guard;
  Volume::s_write_tear_gate_for_test = [&](uint64_t, uint64_t) {
    if (hook_calls.fetch_add(1) == 0) {
      holder_parked.store(true);
      while (!release_holder.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
  };

  std::atomic<CacheError> holder_result{CacheError::InternalError};
  std::atomic<CacheError> optimizer_result{CacheError::InternalError};
  std::thread holder_thread([&] {
    holder_result.store(write_alternate(*other.volume, other_key, kOriginal,
                                        std::string(600, 'z')));
  });
  const bool parked = wait_for([&] { return holder_parked.load(); });
  std::thread optimizer_thread;
  bool resolved = false;
  bool removed = false;
  if (parked) {
    optimizer_thread = std::thread([&] {
      optimizer_result.store(write_alternate(*optimizer.volume, key, kOptimized,
                                             std::string(3899, 'm')));
    });
    resolved = wait_for([&] {
      return optimizer.volume->stats().alternate_max_chain_depth > depth_before;
    });
    // The key changes while the optimizer waits for the write lock.
    removed = server.volume
                  ->remove_alternate_sync(key, static_cast<AlternateId>(0x11))
                  .has_value();
  }
  release_holder.store(true);
  holder_thread.join();
  if (optimizer_thread.joinable()) {
    optimizer_thread.join();
  }
  Volume::s_write_tear_gate_for_test = {};

  REQUIRE(parked);
  REQUIRE(resolved);
  REQUIRE(removed);
  REQUIRE(holder_result.load() == CacheError::Success);
  CHECK(optimizer_result.load() == CacheError::Success);
  CHECK(optimizer.volume->stats().alternate_publish_retries == 1);
  CHECK(optimizer.volume->stats().alternate_publish_rewrites == 0);
  // The holder's write and the optimizer's ONE written attempt.
  CHECK(hook_calls.load() == 2);
  const std::vector<int> ids = list_ids(*server.volume, key);
  INFO("the key lists: " << printed(ids));
  CHECK(ids == (std::vector<int>{0x08, 0x10}));
}

TEST_CASE(
    "Removing a head alternate while another process writes the key "
    "removes it and leaves one head",
    "[alternate][multiprocess][publishrace][remove]") {
  // Removing the alternate at the head of a chain republishes its successor
  // in the head's place.  The removal resolved the head first; if another
  // process prepends an alternate in between, the entry the removal meant to
  // repoint now names that new head.  A republish made against the old state
  // lands beside it: the key has two directory entries, and the alternate
  // the removal reported as removed is still reachable through the other
  // one.
  //
  // The web server starts removing the head and is parked after resolving
  // it; the optimizer writes its copy, which replaces the head entry in
  // place; the web server is released.
  const TempCacheDir dir("publishrace");
  ProcessView server;
  ProcessView optimizer;
  REQUIRE(server.open(dir.path()));
  REQUIRE(optimizer.open(dir.path()));

  const CacheKey key("http://example.test/styles/site.css");
  const auto older = static_cast<AlternateId>(0x10);
  const auto head = static_cast<AlternateId>(0x11);
  REQUIRE(write_alternate(*server.volume, key, older, std::string(600, 'a')) ==
          CacheError::Success);
  REQUIRE(write_alternate(*server.volume, key, head, std::string(600, 'b')) ==
          CacheError::Success);
  REQUIRE(list_ids(*optimizer.volume, key) == (std::vector<int>{0x11, 0x10}));
  REQUIRE(server.volume->stats().entry_count == 1);

  std::atomic<int> hook_calls{0};
  std::atomic<bool> remover_parked{false};
  std::atomic<bool> release_remover{false};
  HookGuard guard;
  Volume::s_remove_republish_gate_for_test = [&] {
    if (hook_calls.fetch_add(1) == 0) {
      remover_parked.store(true);
      while (!release_remover.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
  };

  std::atomic<CacheError> remove_result{CacheError::InternalError};
  std::thread remover([&] {
    auto removed = server.volume->remove_alternate_sync(key, head);
    remove_result.store(removed.has_value() ? CacheError::Success
                                            : removed.error());
  });
  const bool parked = wait_for([&] { return remover_parked.load(); });
  CacheError write_result = CacheError::InternalError;
  if (parked) {
    // The key changes between the removal's resolve and its publish.
    write_result = write_alternate(*optimizer.volume, key, kOptimized,
                                   std::string(3899, 'm'));
  }
  release_remover.store(true);
  remover.join();
  Volume::s_remove_republish_gate_for_test = {};

  REQUIRE(parked);
  REQUIRE(write_result == CacheError::Success);
  CHECK(remove_result.load() == CacheError::Success);

  const std::vector<int> through_server = list_ids(*server.volume, key);
  const std::vector<int> through_optimizer = list_ids(*optimizer.volume, key);
  INFO("through the web server's view: " << printed(through_server));
  INFO("through the optimizer's view: " << printed(through_optimizer));
  // The removed alternate is gone, the one written meanwhile and the older
  // one are there ...
  CHECK(through_server == (std::vector<int>{0x08, 0x10}));
  CHECK(through_optimizer == (std::vector<int>{0x08, 0x10}));
  // ... and the key still has ONE directory entry (it is the only key in
  // the volume), not a second head beside the real one.
  CHECK(server.volume->stats().entry_count == 1);
  // The removal noticed that the head moved under it and resolved again:
  // the alternate was then no longer the head, so the second round unlinked
  // it from inside the chain and republished nothing.
  CHECK(server.volume->stats().alternate_publish_retries == 1);
  CHECK(hook_calls.load() == 1);

  // A later write of the key finds everything that should be there.
  REQUIRE(write_alternate(*optimizer.volume, key, kOptimizedGzip,
                          std::string(1200, 'g')) == CacheError::Success);
  CHECK(list_ids(*server.volume, key) == (std::vector<int>{0x48, 0x08, 0x10}));
  CHECK(server.volume->stats().entry_count == 1);
}

TEST_CASE("Sixteen processes writing alternates of one key at once lose none",
          "[alternate][multiprocess][publishrace]") {
  // No hook, no forced order: sixteen views of one file, each writing its
  // own alternate of the same key at the same moment, round after round on
  // fresh keys.  Every write must succeed and every alternate must be
  // listed afterwards, through a view that wrote none of them.
  const int writers = 16;
  const int rounds = 25;
  const PatientLocks patient;
  const TempCacheDir dir("publishrace");
  std::vector<std::unique_ptr<ProcessView>> views;
  for (int i = 0; i < writers; ++i) {
    views.push_back(std::make_unique<ProcessView>());
    REQUIRE(views.back()->open(dir.path()));
  }
  ProcessView observer;
  REQUIRE(observer.open(dir.path()));

  // A write may still report Busy for a reason that is not under test here
  // (a directory bucket another writer held for the whole read budget).
  // Such a write stored nothing and said so; it is counted apart from the
  // failure this case exists for: a write that reported success and whose
  // alternate is not there.
  int busy_writes = 0;
  int failed_writes = 0;
  int missing = 0;
  std::string first_bad;
  for (int round = 0; round < rounds; ++round) {
    const CacheKey key("http://example.test/many/" + std::to_string(round));
    std::vector<CacheError> results(static_cast<size_t>(writers),
                                    CacheError::InternalError);
    run_together(writers, [&](int i) {
      results[static_cast<size_t>(i)] =
          write_alternate(*views[static_cast<size_t>(i)]->volume, key,
                          static_cast<AlternateId>(1 + i),
                          std::string(2000, static_cast<char>('a' + i)));
    });
    const std::vector<int> ids = list_ids(*observer.volume, key);
    for (int i = 0; i < writers; ++i) {
      const CacheError result = results[static_cast<size_t>(i)];
      if (result == CacheError::Busy) {
        ++busy_writes;
        continue;
      }
      if (result != CacheError::Success) {
        ++failed_writes;
        continue;
      }
      if (!contains(ids, 1 + i)) {
        ++missing;
        if (first_bad.empty()) {
          first_bad =
              "round " + std::to_string(round) + " lists " + printed(ids);
        }
      }
    }
  }
  INFO("first key with a missing alternate: " << first_bad);
  INFO("writes that reported Busy: " << busy_writes);
  CHECK(missing == 0);
  CHECK(failed_writes == 0);
  CHECK(busy_writes == 0);
}

TEST_CASE(
    "Sixteen processes re-recording an original while the optimizer "
    "writes its copies lose none of the copies",
    "[alternate][multiprocess][publishrace]") {
  // The product's shape at a wider burst than the rig's: sixteen web-server
  // processes each remove the stored original and write it again (a
  // re-record is exactly that pair), while the optimizer writes the
  // optimized copy and its gzip and brotli siblings of the same URL.  The
  // head removal republishes the next node, so this also runs the removal's
  // conditional publish against concurrent writers.
  const int recorders = 16;
  const int rounds = 25;
  const PatientLocks patient;
  const TempCacheDir dir("publishrace");
  std::vector<std::unique_ptr<ProcessView>> servers;
  for (int i = 0; i < recorders; ++i) {
    servers.push_back(std::make_unique<ProcessView>());
    REQUIRE(servers.back()->open(dir.path()));
  }
  ProcessView optimizer;
  ProcessView observer;
  REQUIRE(optimizer.open(dir.path()));
  REQUIRE(observer.open(dir.path()));

  // Busy is counted apart from a lost copy, as in the case above.  A round
  // in which one of the optimizer's own writes reported Busy cannot be
  // judged for a lost copy and is counted as busy only.
  int busy_writes = 0;
  int failed_writes = 0;
  int lost_copies = 0;
  int keys_without_original = 0;
  std::string first_bad;
  for (int round = 0; round < rounds; ++round) {
    const CacheKey key("http://example.test/burst/" + std::to_string(round));
    // The first request recorded the original and notified the optimizer.
    REQUIRE(write_alternate(*servers[0]->volume, key, kOriginal,
                            std::string(4307, 'o')) == CacheError::Success);
    std::atomic<int> failures{0};
    std::atomic<int> busy{0};
    std::atomic<int> optimizer_busy{0};
    std::atomic<int> originals_written{0};
    const auto count = [&](CacheError result, bool by_optimizer) {
      if (result == CacheError::Busy) {
        busy.fetch_add(1);
        if (by_optimizer) {
          optimizer_busy.fetch_add(1);
        }
      } else if (result != CacheError::Success) {
        failures.fetch_add(1);
      } else if (!by_optimizer) {
        originals_written.fetch_add(1);
      }
    };
    run_together(recorders + 1, [&](int i) {
      if (i == recorders) {
        for (const AlternateId id :
             {kOptimized, kOptimizedGzip, kOptimizedBrotli}) {
          count(write_alternate(*optimizer.volume, key, id,
                                std::string(1500, 'm')),
                /*by_optimizer=*/true);
        }
        return;
      }
      Volume &volume = *servers[static_cast<size_t>(i)]->volume;
      // A re-record: unlink the previous original (it may already be gone,
      // or the removal may report busy -- both are fine, as in the product),
      // then write the new one.
      (void)volume.remove_alternate_sync(key, kOriginal);
      count(write_alternate(volume, key, kOriginal, std::string(4307, 'p')),
            /*by_optimizer=*/false);
    });
    failed_writes += failures.load();
    busy_writes += busy.load();
    const std::vector<int> ids = list_ids(*observer.volume, key);
    const bool copies_there =
        contains(ids, 0x08) && contains(ids, 0x48) && contains(ids, 0x88);
    if (!copies_there && optimizer_busy.load() == 0) {
      ++lost_copies;
      if (first_bad.empty()) {
        first_bad = "round " + std::to_string(round) + " lists " + printed(ids);
      }
    }
    if (!contains(ids, 0x0C) && originals_written.load() != 0) {
      ++keys_without_original;
    }
  }
  INFO("first key that lost a copy: " << first_bad);
  INFO("writes that reported Busy: " << busy_writes);
  CHECK(lost_copies == 0);
  CHECK(keys_without_original == 0);
  CHECK(failed_writes == 0);
  CHECK(busy_writes == 0);
}
