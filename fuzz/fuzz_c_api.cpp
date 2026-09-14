// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// libFuzzer target: the public C API entry points that consume external bytes.
//
// Attack surface: cyclone_cache_write / read / read_data / exists / delete /
// stats -- the boundary an embedder (nginx, IIS, ...) drives with attacker-
// influenced keys and payloads.  A single live cache is created ONCE in a temp
// directory; each fuzz input is decoded into a SEQUENCE of operations against
// it, so the fuzzer can find write-then-read / delete-then-read ordering bugs
// as well as raw input-handling faults.  The whole read/write stack --
// CacheKey construction, document serialization, directory insert/probe,
// document deserialization on read-back -- runs under ASan/UBSan.
//
// Invariants:
//   - no crash / no UB for any input;
//   - a handle returned by cyclone_cache_read must expose a self-consistent
//     (data, data_len) pair whose bytes are all addressable;
//   - ROUND-TRIP ORACLE: executions are single-threaded and the cache is
//     private to this process, so a shadow map of key -> last successfully
//     written value is sound (eviction and wrap loss can turn a hit into a
//     MISS, never into a WRONG hit).  Any read hit whose bytes differ from
//     the shadow value is a served-garbage fault -> trap.
//
// The persistent cache is torn down via atexit so LSan's end-of-process leak
// check sees no reachable-by-design allocations and stays enabled for real
// leaks.

#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>

#include "cyclone/cyclone_c.h"

namespace {

// Minimal, dependency-free byte-stream reader over the fuzz input.
class Reader {
 public:
  Reader(const uint8_t *data, size_t size) : _data(data), _size(size) {}

  bool empty() const { return _pos >= _size; }

  uint8_t u8() {
    if (_pos >= _size) {
      return 0;
    }
    return _data[_pos++];
  }

  // Consume up to `max` bytes, length prefixed by one byte.
  std::string bytes(size_t max) {
    size_t want = u8() % (max + 1);
    size_t avail = _size - _pos;
    size_t n = want < avail ? want : avail;
    std::string out(reinterpret_cast<const char *>(_data + _pos), n);
    _pos += n;
    return out;
  }

 private:
  const uint8_t *_data;
  size_t _size;
  size_t _pos = 0;
};

CycloneCacheHandle *g_cache = nullptr;

// Shadow oracle: key -> last value whose write (or tier-write; the small tier
// is disabled in this config, so tier ops fall back to the same keyspace)
// returned CYCLONE_OK.  Erased on successful delete.  Keys absent from the
// map are not checked (a hit for one can only come from a prior exec whose
// map entry persisted -- the map is static too, so absence means we never
// confirmed a commit).
std::unordered_map<std::string, std::string> &shadow() {
  static std::unordered_map<std::string, std::string> map;
  return map;
}

void destroy_cache() {
  if (g_cache != nullptr) {
    cyclone_cache_destroy(g_cache);
    g_cache = nullptr;
  }
}

CycloneCacheHandle *shared_cache() {
  static CycloneCacheHandle *cache = [] {
    // pid-unique: parallel fuzz jobs must not share a cache file.
    std::string dir = (std::filesystem::temp_directory_path() /
                       ("cyclone_fuzz_capi_" + std::to_string(::getpid())))
                          .string();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    static std::string path =
        (std::filesystem::path(dir) / "cache.dat").string();
    std::remove(path.c_str());

    CycloneCacheConfig cfg{};
    cfg.cache_path = path.c_str();
    cfg.cache_size_bytes = 16u * 1024 * 1024;
    cfg.ram_cache_size_bytes = 4u * 1024 * 1024;
    cfg.enable_checksum = 1;
    cfg.num_segments = 4;

    CycloneCacheHandle *h = nullptr;
    if (cyclone_cache_create(&cfg, &h) != CYCLONE_OK) {
      return static_cast<CycloneCacheHandle *>(nullptr);
    }
    g_cache = h;
    std::atexit(destroy_cache);
    return h;
  }();
  return cache;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  CycloneCacheHandle *cache = shared_cache();
  if (cache == nullptr) {
    return 0;  // one-time create failed -- nothing to exercise
  }

  Reader r(data, size);

  // Drive a bounded sequence of operations from this single input.
  for (int step = 0; step < 16 && !r.empty(); ++step) {
    uint8_t op = r.u8() % 6;
    std::string key = r.bytes(64);

    switch (op) {
      case 0: {  // write
        std::string val = r.bytes(4096);
        if (cyclone_cache_write(cache, key.data(), key.size(), val.data(),
                                val.size()) == CYCLONE_OK &&
            !key.empty()) {
          shadow()[key] = val;
        }
        break;
      }
      case 1: {  // read + read_data, checked against the shadow oracle
        CycloneReadHandle *rh = nullptr;
        if (cyclone_cache_read(cache, key.data(), key.size(), &rh) ==
                CYCLONE_OK &&
            rh != nullptr) {
          const char *out = nullptr;
          size_t out_len = 0;
          if (cyclone_cache_read_data(rh, &out, &out_len) == CYCLONE_OK &&
              out != nullptr) {
            volatile char sink = 0;
            for (size_t i = 0; i < out_len; ++i) {
              sink = out[i];  // ASan: every returned byte must be addressable
            }
            (void)sink;
            auto it = shadow().find(key);
            if (it != shadow().end() &&
                (out_len != it->second.size() ||
                 std::memcmp(out, it->second.data(), out_len) != 0)) {
              __builtin_trap();  // hit served bytes != last committed write
            }
          }
          cyclone_cache_read_close(rh);
        }
        break;
      }
      case 2:  // exists
        (void)cyclone_cache_exists(cache, key.data(), key.size());
        break;
      case 3:  // delete
        if (cyclone_cache_delete(cache, key.data(), key.size()) == CYCLONE_OK) {
          shadow().erase(key);
        }
        break;
      case 4: {  // tier-routed write (small tier falls back when disabled)
        std::string val = r.bytes(1024);
        if (cyclone_cache_write_tier(cache, key.data(), key.size(), val.data(),
                                     val.size(),
                                     CYCLONE_TIER_SMALL) == CYCLONE_OK &&
            !key.empty()) {
          shadow()[key] = val;  // small tier disabled -> same keyspace
        }
        break;
      }
      case 5: {  // stats
        CycloneCacheStats stats{};
        (void)cyclone_cache_stats(cache, &stats);
        break;
      }
      default:
        break;
    }
  }

  return 0;
}
