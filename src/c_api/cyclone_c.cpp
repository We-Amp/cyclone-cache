// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "cyclone/cyclone_c.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cyclone/cache.hpp"

using namespace cyclone;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static CycloneError to_c_error(CacheError e) {
  switch (e) {
    case CacheError::Success:
      return CYCLONE_OK;
    case CacheError::NotFound:
      return CYCLONE_NOT_FOUND;
    case CacheError::Exists:
      return CYCLONE_EXISTS;
    case CacheError::NoSpace:
      return CYCLONE_NO_SPACE;
    case CacheError::IoError:
      return CYCLONE_IO_ERROR;
    case CacheError::Corrupted:
      return CYCLONE_CORRUPTED;
    case CacheError::InvalidKey:
      return CYCLONE_INVALID_KEY;
    case CacheError::InvalidArgument:
      return CYCLONE_INVALID_ARGUMENT;
    case CacheError::NotInitialized:
      return CYCLONE_NOT_INITIALIZED;
    case CacheError::ResetRefusedLivePeer:
      return CYCLONE_RESET_REFUSED_LIVE_PEER;
    case CacheError::ObjectTooLarge:
      return CYCLONE_OBJECT_TOO_LARGE;
    default:
      return CYCLONE_INTERNAL_ERROR;
  }
}

static Tier to_cpp_tier(CycloneTier tier) {
  return tier == CYCLONE_TIER_SMALL ? Tier::kSmall : Tier::kDefault;
}

// ---------------------------------------------------------------------------
// Opaque handle definitions
// ---------------------------------------------------------------------------

struct InFlightEntry {
  std::vector<std::pair<CycloneReadCallback, void *>> waiters;
  bool fetch_in_progress = false;
};

// Context passed to the miss handler's done_cb, heap-allocated per miss.
// Must outlive all potential done_cb calls (including erroneous double calls).
struct MissDoneContext {
  struct CycloneCacheHandle *cache_handle;
  std::string key;
  std::atomic<bool> completed{false};  // Prevents double-completion
};

struct CycloneCacheHandle {
  std::unique_ptr<Cache> cache;

  // Miss callback state
  std::mutex in_flight_mu;
  std::condition_variable in_flight_drained;
  std::unordered_map<std::string, InFlightEntry> in_flight;
  std::vector<std::unique_ptr<MissDoneContext>>
      completed_contexts;  // deferred cleanup
  CycloneMissHandler miss_handler = nullptr;
  void *miss_handler_user_data = nullptr;
  std::atomic<bool> shutting_down{false};
  std::atomic<int> pending_writes{0};  // tracks post-drain cache writes
  uint32_t max_in_flight = 0;          // 0 = unlimited
};

struct CycloneReadHandle {
  ReadHandle handle;
  std::vector<char> data_buf;
};

// ---------------------------------------------------------------------------
// Core synchronous API
// ---------------------------------------------------------------------------

extern "C" {

CycloneError cyclone_cache_create(const CycloneCacheConfig *config,
                                  CycloneCacheHandle **out) {
  if ((config == nullptr) || (out == nullptr) ||
      (config->cache_path == nullptr)) {
    return CYCLONE_INVALID_ARGUMENT;
  }

  CacheConfig cc;
  cc.ram_cache_size = config->ram_cache_size_bytes;
  cc.enable_checksum = config->enable_checksum != 0;
  cc.num_segments = config->num_segments > 0 ? config->num_segments : 4;
  cc.small_tier_percent = config->small_tier_percent;
  // Negative-logic at the C boundary so a zero-initialised config keeps the
  // unlink enabled (see disable_alternate_unlink in cyclone_c.h).
  cc.unlink_superseded_alternates = config->disable_alternate_unlink == 0;
  // Sentinel mapping: C 0 = library default (leave the C++ default
  // untouched), C UINT64_MAX = disabled (C++ 0), any other N = bound N.
  if (config->max_object_size == UINT64_MAX) {
    cc.max_object_size = 0;
  } else if (config->max_object_size != 0) {
    cc.max_object_size = config->max_object_size;
  }
  // Positive logic: a zero-initialised config lands on OFF, which is
  // the C++ default and the historical behaviour.
  cc.cross_process_ram_coherence =
      config->enable_cross_process_ram_coherence != 0;
  if (config->enable_mmap_directory != 0) {
    cc.multi_process_config.enabled = true;
  }

  auto cache_result = Cache::create(cc);
  if (!cache_result) {
    fprintf(stderr, "cyclone: Cache::create failed: %d\n",
            static_cast<int>(cache_result.error()));
    return to_c_error(cache_result.error());
  }

  auto vol_result =
      (*cache_result)->add_volume(config->cache_path, config->cache_size_bytes);
  if (!vol_result) {
    fprintf(stderr, "cyclone: add_volume failed: %d\n",
            static_cast<int>(vol_result.error()));
    return to_c_error(vol_result.error());
  }

  auto start_result = (*cache_result)->start();
  if (!start_result) {
    fprintf(stderr, "cyclone: start() failed: %s (%d)\n",
            cyclone::make_error_code(start_result.error()).message().c_str(),
            static_cast<int>(start_result.error()));
    return to_c_error(start_result.error());
  }

  auto *h = new CycloneCacheHandle();
  h->cache = std::move(*cache_result);
  h->max_in_flight = config->max_in_flight_requests > 0
                         ? config->max_in_flight_requests
                         : 10000;  // Default limit
  *out = h;
  return CYCLONE_OK;
}

void cyclone_cache_destroy(CycloneCacheHandle *cache) {
  if (cache == nullptr) return;
  // Drain in-flight requests with a 30-second timeout
  cyclone_cache_drain_pending(cache, 30000);
  delete cache;
}

// Shared implementations for the tier-less functions and their _tier
// variants (the tier-less functions are exactly the CYCLONE_TIER_DEFAULT
// case).
static CycloneError cache_read_impl(CycloneCacheHandle *cache, const char *key,
                                    size_t key_len, Tier tier,
                                    CycloneReadHandle **out) {
  if ((cache == nullptr) || (key == nullptr) || (out == nullptr) ||
      key_len == 0)
    return CYCLONE_INVALID_ARGUMENT;

  CacheKey ck(std::string_view(key, key_len));
  auto result = cache->cache->read_sync(ck, tier);
  if (!result) {
    return to_c_error(result.error());
  }

  auto *rh = new CycloneReadHandle();
  rh->handle = std::move(*result);

  auto content = rh->handle.content();
  rh->data_buf.assign(
      reinterpret_cast<const char *>(content.data()),
      reinterpret_cast<const char *>(content.data()) + content.size());
  *out = rh;
  return CYCLONE_OK;
}

static CycloneError cache_write_impl(CycloneCacheHandle *cache, const char *key,
                                     size_t key_len, const char *data,
                                     size_t data_len, Tier tier) {
  if ((cache == nullptr) || (key == nullptr) || key_len == 0)
    return CYCLONE_INVALID_ARGUMENT;
  if (data_len > 0 && (data == nullptr)) return CYCLONE_INVALID_ARGUMENT;

  CacheKey ck(std::string_view(key, key_len));
  auto wh_result = cache->cache->write_sync(ck, data_len, tier);
  if (!wh_result) {
    return to_c_error(wh_result.error());
  }

  auto &wh = *wh_result;
  if ((data != nullptr) && data_len > 0) {
    auto write_result = wh.write_sync(std::span<const std::byte>(
        reinterpret_cast<const std::byte *>(data), data_len));
    if (!write_result) {
      wh.abort();
      return to_c_error(write_result.error());
    }
  }

  auto close_result = wh.close_sync();
  if (!close_result) {
    return to_c_error(close_result.error());
  }
  return CYCLONE_OK;
}

static CycloneError cache_delete_impl(CycloneCacheHandle *cache,
                                      const char *key, size_t key_len,
                                      Tier tier) {
  if ((cache == nullptr) || (key == nullptr) || key_len == 0)
    return CYCLONE_INVALID_ARGUMENT;
  CacheKey ck(std::string_view(key, key_len));
  auto result = cache->cache->remove_sync(ck, tier);
  if (!result) return to_c_error(result.error());
  return CYCLONE_OK;
}

static CycloneError cache_exists_impl(CycloneCacheHandle *cache,
                                      const char *key, size_t key_len,
                                      Tier tier) {
  if ((cache == nullptr) || (key == nullptr) || key_len == 0)
    return CYCLONE_INVALID_ARGUMENT;
  CacheKey ck(std::string_view(key, key_len));
  auto result = cache->cache->exists_sync(ck, tier);
  if (!result) return to_c_error(result.error());
  return result.value() ? CYCLONE_OK : CYCLONE_NOT_FOUND;
}

CycloneError cyclone_cache_read(CycloneCacheHandle *cache, const char *key,
                                size_t key_len, CycloneReadHandle **out) {
  return cache_read_impl(cache, key, key_len, Tier::kDefault, out);
}

CycloneError cyclone_cache_read_data(CycloneReadHandle *handle,
                                     const char **data, size_t *data_len) {
  if ((handle == nullptr) || (data == nullptr) || (data_len == nullptr))
    return CYCLONE_INVALID_ARGUMENT;
  *data = handle->data_buf.data();
  *data_len = handle->data_buf.size();
  return CYCLONE_OK;
}

void cyclone_cache_read_close(CycloneReadHandle *handle) { delete handle; }

CycloneError cyclone_cache_write(CycloneCacheHandle *cache, const char *key,
                                 size_t key_len, const char *data,
                                 size_t data_len) {
  return cache_write_impl(cache, key, key_len, data, data_len, Tier::kDefault);
}

CycloneError cyclone_cache_delete(CycloneCacheHandle *cache, const char *key,
                                  size_t key_len) {
  return cache_delete_impl(cache, key, key_len, Tier::kDefault);
}

CycloneError cyclone_cache_exists(CycloneCacheHandle *cache, const char *key,
                                  size_t key_len) {
  return cache_exists_impl(cache, key, key_len, Tier::kDefault);
}

CycloneError cyclone_cache_read_tier(CycloneCacheHandle *cache, const char *key,
                                     size_t key_len, CycloneTier tier,
                                     CycloneReadHandle **out) {
  return cache_read_impl(cache, key, key_len, to_cpp_tier(tier), out);
}

CycloneError cyclone_cache_write_tier(CycloneCacheHandle *cache,
                                      const char *key, size_t key_len,
                                      const char *data, size_t data_len,
                                      CycloneTier tier) {
  return cache_write_impl(cache, key, key_len, data, data_len,
                          to_cpp_tier(tier));
}

CycloneError cyclone_cache_delete_tier(CycloneCacheHandle *cache,
                                       const char *key, size_t key_len,
                                       CycloneTier tier) {
  return cache_delete_impl(cache, key, key_len, to_cpp_tier(tier));
}

CycloneError cyclone_cache_exists_tier(CycloneCacheHandle *cache,
                                       const char *key, size_t key_len,
                                       CycloneTier tier) {
  return cache_exists_impl(cache, key, key_len, to_cpp_tier(tier));
}

int cyclone_cache_small_tier_active(CycloneCacheHandle *cache) {
  if (cache == nullptr) return 0;
  return cache->cache->small_tier_active() ? 1 : 0;
}

int cyclone_cache_cross_process_ram_coherence_active(
    CycloneCacheHandle *cache) {
  if (cache == nullptr) return 0;
  return cache->cache->cross_process_ram_coherence_active() ? 1 : 0;
}

CycloneError cyclone_cache_stats(CycloneCacheHandle *cache,
                                 CycloneCacheStats *out) {
  if ((cache == nullptr) || (out == nullptr)) return CYCLONE_INVALID_ARGUMENT;
  auto s = cache->cache->stats();
  out->ram_cache_hits = s.ram_cache_hits;
  out->ram_cache_misses = s.ram_cache_misses;
  out->disk_cache_hits = s.disk_cache_hits;
  out->disk_cache_misses = s.disk_cache_misses;
  out->bytes_read = s.bytes_read;
  out->bytes_written = s.bytes_written;
  out->evictions = s.evictions;
  out->current_size = s.current_bytes;
  out->current_entries = s.current_entries;
  out->write_buffer_wraps = s.write_buffer_wraps;
  out->last_wrap_interval_ns = s.last_wrap_interval_ns;
  out->min_wrap_interval_ns = s.min_wrap_interval_ns;
  out->last_wrap_age_ns = s.last_wrap_age_ns;
  out->wraps_deferred_by_lease = s.wraps_deferred_by_lease;
  out->writes_dropped_by_lease = s.writes_dropped_by_lease;
  out->wraps_forced_past_lease = s.wraps_forced_past_lease;
  out->tag_collision_evictions = s.tag_collision_evictions;
  out->borrows_outstanding = s.borrows_outstanding;
  out->volumes_with_degraded_reset_gate = s.volumes_with_degraded_reset_gate;
  out->bucket_full_evictions = s.bucket_full_evictions;
  out->resets_under_degraded_gate = s.resets_under_degraded_gate;
  out->resets_gate_verified = s.resets_gate_verified;
  out->alternate_shadows_unlinked = s.alternate_shadows_unlinked;
  out->alternate_splice_deferred = s.alternate_splice_deferred;
  out->alternate_chain_resets = s.alternate_chain_resets;
  out->alternate_max_chain_depth = s.alternate_max_chain_depth;
  out->alternate_wrap_refusals = s.alternate_wrap_refusals;
  out->ram_coherence_rejections = s.ram_coherence_rejections;
  out->ram_coherence_put_rejections = s.ram_coherence_put_rejections;
  return CYCLONE_OK;
}

// ---------------------------------------------------------------------------
// Miss callback hook
// ---------------------------------------------------------------------------

// Internal done callback invoked by the miss handler when the fetch completes.
static void internal_miss_done(void *user_data, const char *data,
                               size_t data_len, CycloneError err) {
  auto *ctx = static_cast<MissDoneContext *>(user_data);

  // Prevent double-completion: if handler calls done_cb twice, ignore second
  // call.
  bool expected = false;
  if (!ctx->completed.compare_exchange_strong(expected, true)) {
    return;  // Already completed, ignore this call
  }

  auto *ch = ctx->cache_handle;

  // Collect waiters under lock, then notify outside lock.
  // We notify BEFORE writing to cache so disk I/O doesn't block callbacks.
  // Move ctx to the completed list so it stays alive for any erroneous
  // second done_cb call (the atomic guard above will safely reject it).
  std::vector<std::pair<CycloneReadCallback, void *>> waiters;
  std::string key_copy = ctx->key;
  bool will_write = (err == CYCLONE_OK && (data != nullptr) && data_len > 0);
  {
    std::lock_guard<std::mutex> lock(ch->in_flight_mu);
    auto it = ch->in_flight.find(ctx->key);
    if (it != ch->in_flight.end()) {
      waiters = std::move(it->second.waiters);
      ch->in_flight.erase(it);
    }
    // Bound completed_contexts: when it exceeds 128 entries, keep only the
    // most recent 64.  Older contexts are safe to free — any racing
    // double-done-cb on a previous miss would have already resolved.
    if (ch->completed_contexts.size() > 128) {
      ch->completed_contexts.erase(
          ch->completed_contexts.begin(),
          ch->completed_contexts.begin() +
              static_cast<ptrdiff_t>(ch->completed_contexts.size() - 64));
    }
    // Defer ctx deletion: move to completed list so a double done_cb
    // call can still safely read the atomic completed flag.
    ch->completed_contexts.emplace_back(ctx);
    // Track the upcoming cache write so drain knows we're not done yet.
    if (will_write) {
      ch->pending_writes.fetch_add(1, std::memory_order_release);
    }
    if (ch->shutting_down && ch->in_flight.empty() &&
        ch->pending_writes.load(std::memory_order_acquire) == 0) {
      ch->in_flight_drained.notify_all();
    }
  }

  // Notify all coalesced waiters FIRST (before cache write).
  for (auto &[cb, ud] : waiters) {
    cb(ud, data, data_len, err);
  }

  // THEN store in cache (non-blocking for waiters).
  // Failure to write is non-fatal: waiters already have the data.
  if (will_write) {
    cyclone_cache_write(ch, key_copy.data(), key_copy.size(), data, data_len);
    // Decrement pending_writes and wake drain if this was the last one.
    if (ch->pending_writes.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      std::lock_guard<std::mutex> lock(ch->in_flight_mu);
      if (ch->shutting_down && ch->in_flight.empty()) {
        ch->in_flight_drained.notify_all();
      }
    }
  }
}

CycloneError cyclone_cache_set_miss_handler(CycloneCacheHandle *cache,
                                            CycloneMissHandler handler,
                                            void *handler_user_data) {
  if (cache == nullptr) return CYCLONE_INVALID_ARGUMENT;
  std::lock_guard<std::mutex> lock(cache->in_flight_mu);
  cache->miss_handler = handler;
  cache->miss_handler_user_data = handler_user_data;
  return CYCLONE_OK;
}

CycloneError cyclone_cache_read_async(CycloneCacheHandle *cache,
                                      const char *key, size_t key_len,
                                      CycloneReadCallback read_cb,
                                      void *read_user_data) {
  if ((cache == nullptr) || (key == nullptr) || (read_cb == nullptr) ||
      key_len == 0)
    return CYCLONE_INVALID_ARGUMENT;

  // Reject if shutting down.
  if (cache->shutting_down.load(std::memory_order_acquire)) {
    read_cb(read_user_data, nullptr, 0, CYCLONE_NOT_INITIALIZED);
    return CYCLONE_OK;
  }

  // Try synchronous read first.
  CacheKey ck(std::string_view(key, key_len));
  auto result = cache->cache->read_sync(ck);
  if (result) {
    // Copy data to ensure it remains valid for the duration of the callback.
    // The ReadHandle (result) would be destroyed at function return, so we
    // cannot pass a pointer to its internal buffer.
    auto content = result->content();
    std::vector<char> data_copy(
        reinterpret_cast<const char *>(content.data()),
        reinterpret_cast<const char *>(content.data()) + content.size());
    read_cb(read_user_data, data_copy.data(), data_copy.size(), CYCLONE_OK);
    return CYCLONE_OK;
  }

  // Not a hit. If no miss handler, report not-found.
  CycloneMissHandler handler;
  void *handler_ud;
  {
    std::lock_guard<std::mutex> lock(cache->in_flight_mu);
    handler = cache->miss_handler;
    handler_ud = cache->miss_handler_user_data;
  }

  if (handler == nullptr) {
    read_cb(read_user_data, nullptr, 0, CYCLONE_NOT_FOUND);
    return CYCLONE_OK;
  }

  // Coalescing logic.
  std::string key_str(key, key_len);
  bool should_invoke_handler = false;
  {
    std::lock_guard<std::mutex> lock(cache->in_flight_mu);

    // Re-check shutdown under lock.
    if (cache->shutting_down.load(std::memory_order_relaxed)) {
      read_cb(read_user_data, nullptr, 0, CYCLONE_NOT_INITIALIZED);
      return CYCLONE_OK;
    }

    // Check in-flight limit (only for new keys, coalescing is always allowed).
    auto it = cache->in_flight.find(key_str);
    if (it == cache->in_flight.end() && cache->max_in_flight > 0 &&
        cache->in_flight.size() >= cache->max_in_flight) {
      read_cb(read_user_data, nullptr, 0, CYCLONE_NO_SPACE);
      return CYCLONE_OK;
    }

    auto &entry = cache->in_flight[key_str];
    entry.waiters.emplace_back(read_cb, read_user_data);

    if (!entry.fetch_in_progress) {
      entry.fetch_in_progress = true;
      should_invoke_handler = true;
    }
  }

  if (should_invoke_handler) {
    auto ctx = std::make_unique<MissDoneContext>();
    ctx->cache_handle = cache;
    ctx->key = key_str;

    // Release ownership: the handler takes the raw pointer and must
    // eventually pass it to internal_miss_done (which transfers it
    // into completed_contexts).  Using unique_ptr above ensures no
    // leak if key assignment throws std::bad_alloc.
    handler(key, key_len, handler_ud, internal_miss_done, ctx.release());
  }

  return CYCLONE_OK;
}

CycloneError cyclone_cache_drain_pending(CycloneCacheHandle *cache,
                                         uint32_t timeout_ms) {
  if (cache == nullptr) return CYCLONE_INVALID_ARGUMENT;

  cache->shutting_down.store(true, std::memory_order_release);

  std::unique_lock<std::mutex> lock(cache->in_flight_mu);
  auto is_idle = [&] {
    return cache->in_flight.empty() &&
           cache->pending_writes.load(std::memory_order_acquire) == 0;
  };

  if (is_idle()) {
    return CYCLONE_OK;
  }

  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

  while (!is_idle()) {
    if (cache->in_flight_drained.wait_until(lock, deadline) ==
        std::cv_status::timeout) {
      // Timed out: notify remaining waiters with NOT_INITIALIZED and clear.
      auto remaining = std::move(cache->in_flight);
      cache->in_flight.clear();
      lock.unlock();
      for (auto &[k, entry] : remaining) {
        for (auto &[cb, ud] : entry.waiters) {
          cb(ud, nullptr, 0, CYCLONE_NOT_INITIALIZED);
        }
      }
      return CYCLONE_OK;
    }
  }

  return CYCLONE_OK;
}

}  // extern "C"
