// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// libFuzzer target: the asynchronous C API (read_async, miss handler, done
// callback, drain, destroy).
//
// Each input drives a deterministic, single-threaded interleaving against a
// fresh small cache.  The miss handler never fetches anything itself: it parks
// the done callback, and the input decides when (and with which result) each
// parked fetch completes, relative to further reads, writes, drains and the
// final destroy.  Invariants:
//   - no crash, leak or undefined behaviour for any interleaving;
//   - every read_async that returned CYCLONE_OK invokes its callback exactly
//     once, and one that returned an error never does;
//   - a callback that reports success exposes addressable bytes.
//
// Every parked fetch is completed exactly once (as the API requires) before the
// cache is destroyed; a completion after a timed-out drain is a case the input
// can reach.

#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <string>
#include <vector>

#include "cyclone/cyclone_c.h"

namespace {

class Reader {
 public:
  Reader(const uint8_t *data, size_t size) : _data(data), _size(size) {}
  bool empty() const { return _pos >= _size; }
  uint8_t u8() { return _pos < _size ? _data[_pos++] : 0; }
  std::string bytes(size_t max) {
    size_t want = u8() % (max + 1);
    size_t n = want < _size - _pos ? want : _size - _pos;
    std::string out(reinterpret_cast<const char *>(_data + _pos), n);
    _pos += n;
    return out;
  }

 private:
  const uint8_t *_data;
  size_t _size;
  size_t _pos = 0;
};

struct ReadRecord {
  int calls = 0;
  bool accepted = false;
};

struct Fetch {
  CycloneMissDoneCallback done = nullptr;
  void *done_user = nullptr;
  bool completed = false;
};

struct State {
  std::deque<ReadRecord> reads;
  std::deque<Fetch> fetches;
};

void on_read(void *user, const char *data, size_t len, CycloneError err) {
  auto *rec = static_cast<ReadRecord *>(user);
  ++rec->calls;
  if (err == CYCLONE_OK) {
    if (data == nullptr && len != 0) {
      __builtin_trap();
    }
    volatile char sink = 0;
    for (size_t i = 0; i < len; ++i) {
      sink = data[i];
    }
    (void)sink;
  }
}

void miss_handler(const char *, size_t, void *user,
                  CycloneMissDoneCallback done, void *done_user) {
  auto *state = static_cast<State *>(user);
  Fetch f;
  f.done = done;
  f.done_user = done_user;
  state->fetches.push_back(f);
}

void complete_fetch(Fetch &f, Reader &r) {
  if (f.completed) {
    return;
  }
  f.completed = true;
  uint8_t mode = r.u8();
  if (mode % 4 == 0) {
    f.done(f.done_user, nullptr, 0, CYCLONE_IO_ERROR);
  } else {
    std::string val = r.bytes(512);
    f.done(f.done_user, val.data(), val.size(), CYCLONE_OK);
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  static const std::string path = [] {
    std::string dir = (std::filesystem::temp_directory_path() /
                       ("cyclone_fuzz_async_" + std::to_string(::getpid())))
                          .string();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return (std::filesystem::path(dir) / "cache.dat").string();
  }();
  std::remove(path.c_str());

  CycloneCacheConfig cfg{};
  cfg.cache_path = path.c_str();
  cfg.cache_size_bytes = 4u * 1024 * 1024;
  cfg.ram_cache_size_bytes = 1u * 1024 * 1024;
  cfg.enable_checksum = 1;
  cfg.num_segments = 2;

  CycloneCacheHandle *cache = nullptr;
  if (cyclone_cache_create(&cfg, &cache) != CYCLONE_OK) {
    return 0;
  }

  State state;
  Reader r(data, size);
  cyclone_cache_set_miss_handler(cache, miss_handler, &state);

  for (int step = 0; step < 24 && !r.empty(); ++step) {
    uint8_t op = r.u8() % 6;
    switch (op) {
      case 0: {  // read_async on one of a few keys so reads coalesce
        std::string key = "k" + std::to_string(r.u8() % 4);
        state.reads.emplace_back();
        ReadRecord &rec = state.reads.back();
        rec.accepted = cyclone_cache_read_async(cache, key.data(), key.size(),
                                                on_read, &rec) == CYCLONE_OK;
        break;
      }
      case 1: {  // complete one parked fetch
        if (!state.fetches.empty()) {
          complete_fetch(state.fetches[r.u8() % state.fetches.size()], r);
        }
        break;
      }
      case 2: {  // synchronous write into the same keyspace
        std::string key = "k" + std::to_string(r.u8() % 4);
        std::string val = r.bytes(512);
        (void)cyclone_cache_write(cache, key.data(), key.size(), val.data(),
                                  val.size());
        break;
      }
      case 3:  // drain with a short deadline; pending reads are answered
        (void)cyclone_cache_drain_pending(cache, r.u8() % 3);
        break;
      case 4:  // toggle the miss handler
        if (r.u8() & 1) {
          cyclone_cache_set_miss_handler(cache, nullptr, nullptr);
        } else {
          cyclone_cache_set_miss_handler(cache, miss_handler, &state);
        }
        break;
      case 5:  // invalid arguments must be refused without a callback
        state.reads.emplace_back();
        state.reads.back().accepted =
            cyclone_cache_read_async(cache, nullptr, 0, on_read,
                                     &state.reads.back()) == CYCLONE_OK;
        break;
      default:
        break;
    }
  }

  // Complete every outstanding fetch exactly once, then tear down.
  for (Fetch &f : state.fetches) {
    complete_fetch(f, r);
  }
  cyclone_cache_destroy(cache);

  for (const ReadRecord &rec : state.reads) {
    if (rec.accepted ? rec.calls != 1 : rec.calls != 0) {
      __builtin_trap();
    }
  }

  std::remove(path.c_str());
  return 0;
}
