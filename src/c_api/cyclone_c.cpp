// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "cyclone/cyclone_c.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__GLIBCXX__) && __has_include(<cxxabi.h>)
#include <cxxabi.h>
#endif

#include "c_error_map.hpp"
#include "cyclone/cache.hpp"

using namespace cyclone;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

using cyclone::c_api_detail::to_c_error;

static Tier to_cpp_tier(CycloneTier tier) {
  return tier == CYCLONE_TIER_SMALL ? Tier::kSmall : Tier::kDefault;
}

// Exception barrier.  Every exported function is called from C frames, which
// an exception cannot cross: one would end the process.  Each body runs
// through guarded(), which turns any exception (in practice std::bad_alloc)
// into `on_exception`.
//
// Thread cancellation on glibc is a forced unwind (abi::__forced_unwind),
// which must not be swallowed: the runtime aborts the process if it is.  It
// is rethrown, which is also why the barriers are not noexcept.
#if defined(__GLIBCXX__) && __has_include(<cxxabi.h>)
#define CYCLONE_C_API_FORCED_UNWIND 1
#else
#define CYCLONE_C_API_FORCED_UNWIND 0
#endif

template <typename Fn>
static std::invoke_result_t<Fn> guarded(std::invoke_result_t<Fn> on_exception,
                                        Fn &&fn) {
  try {
    return std::forward<Fn>(fn)();
#if CYCLONE_C_API_FORCED_UNWIND
  } catch (abi::__forced_unwind &) {
    throw;
#endif
  } catch (...) {
    return on_exception;
  }
}

template <typename Fn>
static void guarded_void(Fn &&fn) {
  try {
    std::forward<Fn>(fn)();
#if CYCLONE_C_API_FORCED_UNWIND
  } catch (abi::__forced_unwind &) {
    throw;
#endif
  } catch (...) {  // NOLINT(bugprone-empty-catch): nothing to report to
  }
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
// It holds a reference to the cache handle, so a fetch that completes after
// cyclone_cache_destroy() still finds the bridge state it touches; the first
// done_cb call takes that reference over and drops it on return.
struct MissDoneContext {
  std::shared_ptr<struct CycloneCacheHandle> owner;
  std::string key;
  std::atomic<bool> completed{false};  // Prevents double-completion
};

// Shared ownership: `self` is the reference the application's handle stands
// for, released by cyclone_cache_destroy(); every miss context with a fetch
// outstanding holds another.  The memory is freed when the last one goes.
// The Cache itself is closed by destroy, never later.
struct CycloneCacheHandle : std::enable_shared_from_this<CycloneCacheHandle> {
  std::unique_ptr<Cache> cache;
  std::shared_ptr<CycloneCacheHandle> self;

  // Miss callback state
  std::mutex in_flight_mu;
  std::condition_variable in_flight_drained;
  std::unordered_map<std::string, InFlightEntry> in_flight;
  // Recently completed contexts, kept so a stray second done_cb (undefined
  // behaviour per the header) lands on live memory.  A fixed ring: adding
  // never allocates, and the oldest entry is freed once it is full.  Emptied
  // by drain.  Guarded by in_flight_mu.
  static constexpr size_t kMaxCompletedContexts = 1024;
  std::array<std::unique_ptr<MissDoneContext>, kMaxCompletedContexts>
      completed_contexts;
  size_t completed_next = 0;
  size_t completed_count = 0;

  // Frees every remembered context.  Called by drain, once nothing is
  // outstanding; the handle's destruction frees whatever remains.
  void reclaim_completed_locked() {
    for (auto &c : completed_contexts) c.reset();
    completed_next = 0;
    completed_count = 0;
  }
  CycloneMissHandler miss_handler = nullptr;
  void *miss_handler_user_data = nullptr;
  std::atomic<bool> shutting_down{false};
  std::atomic<int> pending_writes{0};  // tracks post-drain cache writes
  uint32_t max_in_flight = 0;          // 0 = unlimited
  // Set by destroy under in_flight_mu: no write-back may start, because the
  // Cache is about to be closed.
  bool closed = false;
};

// Holds a private copy of the entry only.  The disk borrow taken by the read
// is released as soon as the copy is made: keeping it for the life of the
// handle would pin the stripe and defer its wraps while the application
// holds the handle, for no benefit, since the copy is what is served.
struct CycloneReadHandle {
  std::vector<char> data_buf;
};

// ---------------------------------------------------------------------------
// Core synchronous API
// ---------------------------------------------------------------------------

extern "C" {

static CycloneError cache_create_impl(const CycloneCacheConfig *config,
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
  // Negative-logic too: zero keeps the library default eviction mode (see
  // disable_wrap_retention in cyclone_c.h).
  if (config->disable_wrap_retention != 0) {
    cc.wrap_retention = false;
  }
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

  auto owner = std::make_shared<CycloneCacheHandle>();
  owner->cache = std::move(*cache_result);
  owner->max_in_flight = config->max_in_flight_requests > 0
                             ? config->max_in_flight_requests
                             : 10000;  // Default limit
  CycloneCacheHandle *h = owner.get();
  h->self = std::move(owner);
  *out = h;
  return CYCLONE_OK;
}

CycloneError cyclone_cache_create(const CycloneCacheConfig *config,
                                  CycloneCacheHandle **out) {
  return guarded(CYCLONE_INTERNAL_ERROR,
                 [&] { return cache_create_impl(config, out); });
}

void cyclone_cache_destroy(CycloneCacheHandle *cache) {
  if (cache == nullptr) return;
  guarded_void([&] {
    // Hold the application's reference until this returns; afterwards only
    // fetches still outstanding keep the handle's memory alive.
    std::shared_ptr<CycloneCacheHandle> owner = std::move(cache->self);
    // Drain in-flight requests with a 30-second timeout.  On a timeout the
    // waiters have been failed, but a fetch may still complete later; its
    // done callback finds the handle alive and closed, and writes nothing.
    cyclone_cache_drain_pending(cache, 30000);
    {
      // Refuse new write-backs and wait for any already under way: they use
      // the Cache, which is closed next.
      std::unique_lock<std::mutex> lock(cache->in_flight_mu);
      cache->closed = true;
      cache->in_flight_drained.wait(lock, [&] {
        return cache->pending_writes.load(std::memory_order_acquire) == 0;
      });
    }
    cache->cache.reset();
  });
}

// Shared implementations for the tier-less functions and their _tier
// variants (the tier-less functions are exactly the CYCLONE_TIER_DEFAULT
// case).
static CycloneError cache_read_body(CycloneCacheHandle *cache, const char *key,
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
  auto content = result->content();
  rh->data_buf.assign(
      reinterpret_cast<const char *>(content.data()),
      reinterpret_cast<const char *>(content.data()) + content.size());
  // Release the borrow now; the handle serves the copy.
  result->close();
  *out = rh;
  return CYCLONE_OK;
}

static CycloneError cache_write_body(CycloneCacheHandle *cache, const char *key,
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

static CycloneError cache_delete_body(CycloneCacheHandle *cache,
                                      const char *key, size_t key_len,
                                      Tier tier) {
  if ((cache == nullptr) || (key == nullptr) || key_len == 0)
    return CYCLONE_INVALID_ARGUMENT;
  CacheKey ck(std::string_view(key, key_len));
  auto result = cache->cache->remove_sync(ck, tier);
  if (!result) return to_c_error(result.error());
  return CYCLONE_OK;
}

static CycloneError cache_exists_body(CycloneCacheHandle *cache,
                                      const char *key, size_t key_len,
                                      Tier tier) {
  if ((cache == nullptr) || (key == nullptr) || key_len == 0)
    return CYCLONE_INVALID_ARGUMENT;
  CacheKey ck(std::string_view(key, key_len));
  auto result = cache->cache->exists_sync(ck, tier);
  if (!result) return to_c_error(result.error());
  return result.value() ? CYCLONE_OK : CYCLONE_NOT_FOUND;
}

static CycloneError cache_read_impl(CycloneCacheHandle *cache, const char *key,
                                    size_t key_len, Tier tier,
                                    CycloneReadHandle **out) {
  return guarded(CYCLONE_INTERNAL_ERROR, [&] {
    return cache_read_body(cache, key, key_len, tier, out);
  });
}

static CycloneError cache_write_impl(CycloneCacheHandle *cache, const char *key,
                                     size_t key_len, const char *data,
                                     size_t data_len, Tier tier) {
  return guarded(CYCLONE_INTERNAL_ERROR, [&] {
    return cache_write_body(cache, key, key_len, data, data_len, tier);
  });
}

static CycloneError cache_delete_impl(CycloneCacheHandle *cache,
                                      const char *key, size_t key_len,
                                      Tier tier) {
  return guarded(CYCLONE_INTERNAL_ERROR,
                 [&] { return cache_delete_body(cache, key, key_len, tier); });
}

static CycloneError cache_exists_impl(CycloneCacheHandle *cache,
                                      const char *key, size_t key_len,
                                      Tier tier) {
  return guarded(CYCLONE_INTERNAL_ERROR,
                 [&] { return cache_exists_body(cache, key, key_len, tier); });
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

void cyclone_cache_read_close(CycloneReadHandle *handle) {
  guarded_void([&] { delete handle; });
}

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
  return guarded(0, [&] { return cache->cache->small_tier_active() ? 1 : 0; });
}

int cyclone_cache_cross_process_ram_coherence_active(
    CycloneCacheHandle *cache) {
  if (cache == nullptr) return 0;
  return guarded(0, [&] {
    return cache->cache->cross_process_ram_coherence_active() ? 1 : 0;
  });
}

static CycloneError cache_stats_impl(CycloneCacheHandle *cache,
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
  out->frontier_advances = s.frontier_advances;
  out->advances_deferred_by_lease = s.advances_deferred_by_lease;
  out->early_advances_skipped = s.early_advances_skipped;
  out->retained_hits = s.retained_hits;
  out->stamp_rejections = s.stamp_rejections;
  out->alternates_carried_forward = s.alternates_carried_forward;
  out->alternate_carry_bytes = s.alternate_carry_bytes;
  out->alternates_carry_dropped = s.alternates_carry_dropped;
  out->directory_read_timeouts = s.directory_read_timeouts;
  return CYCLONE_OK;
}

CycloneError cyclone_cache_stats(CycloneCacheHandle *cache,
                                 CycloneCacheStats *out) {
  return guarded(CYCLONE_INTERNAL_ERROR,
                 [&] { return cache_stats_impl(cache, out); });
}

// ---------------------------------------------------------------------------
// Miss callback hook
// ---------------------------------------------------------------------------

static void miss_done_impl(MissDoneContext *ctx, const char *data,
                           size_t data_len, CycloneError err) {
  // Prevent double-completion: if handler calls done_cb twice, ignore second
  // call.
  bool expected = false;
  if (!ctx->completed.compare_exchange_strong(expected, true)) {
    return;  // Already completed, ignore this call
  }

  // Take over the context's reference: the handle stays alive until this
  // returns, even if it was destroyed while the fetch was outstanding.
  std::shared_ptr<CycloneCacheHandle> owner = std::move(ctx->owner);
  auto *ch = owner.get();

  // Collect waiters under lock, then notify outside lock.
  // The value is stored BEFORE the waiters are notified.
  // Move ctx to the completed list so it stays alive for any erroneous
  // second done_cb call (the atomic guard above will safely reject it).
  //
  // Nothing up to the waiter notification may throw: a failure there would
  // leave the waiters unanswered and the key in flight for good.  The key is
  // therefore moved, not copied, out of ctx, and completed_contexts is a
  // fixed ring that is filled without allocating.
  std::vector<std::pair<CycloneReadCallback, void *>> waiters;
  std::string key;
  bool will_write = (err == CYCLONE_OK && (data != nullptr) && data_len > 0);
  {
    std::lock_guard<std::mutex> lock(ch->in_flight_mu);
    auto it = ch->in_flight.find(ctx->key);
    if (it != ch->in_flight.end()) {
      waiters = std::move(it->second.waiters);
      ch->in_flight.erase(it);
    }
    key = std::move(ctx->key);
    // Keep ctx alive for a while so an erroneous second done_cb call can
    // still safely read the atomic completed flag.  The ring never allocates
    // (nothing here can throw); a full ring frees its oldest entry.
    if (!ch->completed_contexts[ch->completed_next]) ++ch->completed_count;
    ch->completed_contexts[ch->completed_next].reset(ctx);
    ch->completed_next =
        (ch->completed_next + 1) % ch->completed_contexts.size();
    // After destroy the Cache is closed: answer the waiters, write nothing.
    if (ch->closed) {
      will_write = false;
    }
    // Track the upcoming cache write so drain knows we're not done yet.
    if (will_write) {
      ch->pending_writes.fetch_add(1, std::memory_order_release);
    }
    if (ch->shutting_down && ch->in_flight.empty() &&
        ch->pending_writes.load(std::memory_order_acquire) == 0) {
      ch->in_flight_drained.notify_all();
    }
  }

  // Ends the write-back registered above however this function is left,
  // including by an exception from a waiter's callback (contained by the
  // barrier in internal_miss_done): destroy waits for pending_writes to
  // reach zero.  Destroy waits for pending_writes alone, so wake it
  // whenever shutting down, not only when nothing is in flight.
  struct WriteBackDone {
    CycloneCacheHandle *ch;
    bool active;
    ~WriteBackDone() {
      if (!active) return;
      if (ch->pending_writes.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        std::lock_guard<std::mutex> lock(ch->in_flight_mu);
        if (ch->shutting_down) {
          ch->in_flight_drained.notify_all();
        }
      }
    }
  } write_back_done{ch, will_write};

  // Store in the cache FIRST, so a waiter that reads the key back from its
  // callback finds the value.  Failure to write is non-fatal: the waiters
  // are answered with the fetched data either way.
  if (will_write) {
    cyclone_cache_write(ch, key.data(), key.size(), data, data_len);
  }

  // THEN notify all coalesced waiters.
  for (auto &[cb, ud] : waiters) {
    cb(ud, data, data_len, err);
  }
}

// Internal done callback invoked by the miss handler when the fetch completes.
// It runs on the miss handler's stack, which belongs to the embedder, so no
// exception may leave it.
static void internal_miss_done(void *user_data, const char *data,
                               size_t data_len, CycloneError err) {
  guarded_void([&] {
    miss_done_impl(static_cast<MissDoneContext *>(user_data), data, data_len,
                   err);
  });
}

CycloneError cyclone_cache_set_miss_handler(CycloneCacheHandle *cache,
                                            CycloneMissHandler handler,
                                            void *handler_user_data) {
  if (cache == nullptr) return CYCLONE_INVALID_ARGUMENT;
  return guarded(CYCLONE_INTERNAL_ERROR, [&] {
    std::lock_guard<std::mutex> lock(cache->in_flight_mu);
    cache->miss_handler = handler;
    cache->miss_handler_user_data = handler_user_data;
    return CYCLONE_OK;
  });
}

static CycloneError read_async_impl(CycloneCacheHandle *cache, const char *key,
                                    size_t key_len, CycloneReadCallback read_cb,
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
    // Busy means the key's presence is unknown (a writer held its directory
    // bucket past the wait budget): say so rather than report a miss.  A
    // disk failure or a corrupted entry is not a miss either: report it as
    // the synchronous read does.
    CycloneError miss_err = CYCLONE_NOT_FOUND;
    switch (result.error()) {
      case CacheError::Busy:
      case CacheError::IoError:
      case CacheError::Corrupted:
        miss_err = to_c_error(result.error());
        break;
      default:
        break;
    }
    read_cb(read_user_data, nullptr, 0, miss_err);
    return CYCLONE_OK;
  }
  // With a miss handler, Busy takes the miss path like every other non-hit:
  // the fetch answers the waiters, so a read never fails on a busy bucket.
  // The write-back does not necessarily fix the bucket.  In the process that
  // owns the key's stripe, the write force-releases a bucket whose holder is
  // stuck and then publishes (Volume::writer_probe).  In any other process
  // the write is refused as NotOwned before it looks at the directory, so
  // the bucket stays busy until the owner next writes, removes or updates
  // hit counts in it.

  // Coalescing logic.  Everything that can fail is allocated before the
  // read is registered: once a waiter is registered and the fetch marked as
  // started, a failure would leave the waiter unanswered and the key in
  // flight for good.  The context is unused when the read coalesces.
  std::string key_str(key, key_len);
  auto ctx = std::make_unique<MissDoneContext>();
  ctx->owner = cache->shared_from_this();
  ctx->key = key_str;
  bool should_invoke_handler = false;
  // A rejection decided under the lock is reported after it is released:
  // read_cb may call back into this cache, and in_flight_mu is not
  // recursive.
  CycloneError rejected = CYCLONE_OK;
  {
    std::lock_guard<std::mutex> lock(cache->in_flight_mu);

    // Re-check shutdown under lock.
    if (cache->shutting_down.load(std::memory_order_relaxed)) {
      rejected = CYCLONE_NOT_INITIALIZED;
    } else {
      // Check in-flight limit (only for new keys, coalescing is always
      // allowed).
      auto it = cache->in_flight.find(key_str);
      if (it == cache->in_flight.end() && cache->max_in_flight > 0 &&
          cache->in_flight.size() >= cache->max_in_flight) {
        rejected = CYCLONE_NO_SPACE;
      } else {
        auto [entry_it, inserted] = cache->in_flight.try_emplace(key_str);
        auto &entry = entry_it->second;
        try {
          entry.waiters.emplace_back(read_cb, read_user_data);
        } catch (...) {
          if (inserted) cache->in_flight.erase(entry_it);
          throw;
        }

        if (!entry.fetch_in_progress) {
          entry.fetch_in_progress = true;
          should_invoke_handler = true;
        }
      }
    }
  }
  if (rejected != CYCLONE_OK) {
    read_cb(read_user_data, nullptr, 0, rejected);
    return CYCLONE_OK;
  }

  if (should_invoke_handler) {
    // Release ownership: the handler takes the raw pointer and must
    // eventually pass it to internal_miss_done (which transfers it
    // into completed_contexts).
    MissDoneContext *raw_ctx = ctx.release();
    try {
      handler(key, key_len, handler_ud, internal_miss_done, raw_ctx);
    } catch (...) {
      // A miss handler must not throw (see cyclone_c.h).  If it did, the
      // fetch it was asked for may never complete, so the read must not
      // stay registered: the error returned below promises that read_cb
      // will not be called, and a registered waiter would be answered
      // later, at drain or destroy, perhaps after the caller freed
      // read_user_data.  So unregister this read, and answer the reads that
      // coalesced onto the fetch meanwhile with an error.  The context
      // stays with the handler, which may still pass it to done; a late
      // done finds no waiters and answers no one.  If done already ran
      // (the handler threw after calling it), this read has been answered:
      // report success, as read_cb has been called.
      std::vector<std::pair<CycloneReadCallback, void *>> others;
      bool unregistered = false;
      {
        std::lock_guard<std::mutex> lock(cache->in_flight_mu);
        auto it = cache->in_flight.find(key_str);
        if (it != cache->in_flight.end()) {
          auto &waiters = it->second.waiters;
          for (auto w = waiters.begin(); w != waiters.end(); ++w) {
            if (w->first == read_cb && w->second == read_user_data) {
              waiters.erase(w);
              unregistered = true;
              break;
            }
          }
          if (unregistered) {
            others = std::move(waiters);
            cache->in_flight.erase(it);
            if (cache->in_flight.empty()) {
              cache->in_flight_drained.notify_all();
            }
          }
        }
      }
      for (auto &[cb, ud] : others) {
        cb(ud, nullptr, 0, CYCLONE_INTERNAL_ERROR);
      }
      if (!unregistered) {
        return CYCLONE_OK;
      }
      throw;
    }
  }

  return CYCLONE_OK;
}

// A non-OK return means the read was neither answered nor registered, and
// read_cb will not be called later (a throwing miss handler included: the
// read is unregistered first). If read_cb itself throws (a contract
// violation), it has already been called when the error is returned.
CycloneError cyclone_cache_read_async(CycloneCacheHandle *cache,
                                      const char *key, size_t key_len,
                                      CycloneReadCallback read_cb,
                                      void *read_user_data) {
  return guarded(CYCLONE_INTERNAL_ERROR, [&] {
    return read_async_impl(cache, key, key_len, read_cb, read_user_data);
  });
}

static CycloneError drain_pending_impl(CycloneCacheHandle *cache,
                                       uint32_t timeout_ms) {
  if (cache == nullptr) return CYCLONE_INVALID_ARGUMENT;

  cache->shutting_down.store(true, std::memory_order_release);

  std::unique_lock<std::mutex> lock(cache->in_flight_mu);
  auto is_idle = [&] {
    return cache->in_flight.empty() &&
           cache->pending_writes.load(std::memory_order_acquire) == 0;
  };

  if (is_idle()) {
    cache->reclaim_completed_locked();
    return CYCLONE_OK;
  }

  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

  if (!cache->in_flight_drained.wait_until(lock, deadline, is_idle)) {
    // Timed out: notify remaining waiters with NOT_INITIALIZED and clear.
    // The fetches themselves may still complete later; that is safe (see
    // MissDoneContext), and the caller learns of them from the result.
    auto remaining = std::move(cache->in_flight);
    cache->in_flight.clear();
    lock.unlock();
    for (auto &[k, entry] : remaining) {
      for (auto &[cb, ud] : entry.waiters) {
        cb(ud, nullptr, 0, CYCLONE_NOT_INITIALIZED);
      }
    }
    return CYCLONE_TIMEOUT;
  }

  if (is_idle()) cache->reclaim_completed_locked();
  return CYCLONE_OK;
}

CycloneError cyclone_cache_drain_pending(CycloneCacheHandle *cache,
                                         uint32_t timeout_ms) {
  return guarded(CYCLONE_INTERNAL_ERROR,
                 [&] { return drain_pending_impl(cache, timeout_ms); });
}

}  // extern "C"

// Test hook (not part of the public header): number of completed miss
// contexts currently remembered.
size_t cyclone_test_completed_context_count(CycloneCacheHandle *cache) {
  std::lock_guard<std::mutex> lock(cache->in_flight_mu);
  return cache->completed_count;
}
