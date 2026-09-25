// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "cyclone/detail/expected_compat.hpp"
#include "cyclone/error.hpp"
#include "cyclone/task.hpp"

namespace cyclone {

class Volume;
class MappedFile;

// Lease amendment (2026-07-07): result of the intent-checked lease renewal a
// zero-copy embedder MUST call before every aliased send of a borrowed
// mmap region (renew_lease_strict).  Unlike the epoch-only renew_lease()
// (safe only for the copy-then-verify path, where the read is observable),
// the aliased path's read is an unobservable socket send, so it needs the
// Dekker-ordered stamp-then-(intent,epoch) revalidation borrow uses.
enum class LeaseRenewal : std::uint8_t {
  kOk = 0,       // Lease live, no wrap intent, epoch unchanged: keep aliasing.
  kCopyNow = 1,  // A wrap decision is in flight (region still intact): the
                 // embedder must de-alias by copying the bytes out now.
  kTorn = 2,     // Epoch moved — a wrap committed and may have overwritten
                 // the region: abort the serve (the bytes may be torn).
  kLeasesOff = 3,  // No lease protection configured (read_lease_duration=0):
                   // legacy semantics — the embedder copies but does NOT abort.
};

struct ReadHandleImpl {
  virtual ~ReadHandleImpl() = default;
  [[nodiscard]] virtual std::span<const std::byte> header() const = 0;
  [[nodiscard]] virtual std::span<const std::byte> content() const = 0;
  [[nodiscard]] virtual uint64_t content_length() const = 0;
  [[nodiscard]] virtual bool is_ram_cache_hit() const = 0;
  [[nodiscard]] virtual std::optional<std::span<const std::byte>> mapped_view()
      const = 0;

  // Return the file offset of content() within the cache volume.
  // Returns UINT64_MAX when sendfile is not possible (RAM cache hit,
  // no persistent mapping, or file closed).
  static constexpr uint64_t kNoFileOffset = UINT64_MAX;
  [[nodiscard]] virtual uint64_t content_file_offset() const {
    return kNoFileOffset;
  }

  // Re-stamp the read lease pinning this handle's stripe.
  // Returns false when not applicable (RAM-cache hit, leases disabled,
  // or the cache is gone).  Default: no lease to renew.
  virtual bool renew_lease() { return false; }

  // Lease amendment (2026-07-07): intent-checked renewal for the ALIASED
  // zero-copy path.  Stamps the lease FIRST, then Dekker-revalidates
  // (wrap_intent loaded before epoch) exactly as the initial borrow does, so a
  // normal wrap racing at the lease-lapse boundary defers on our fresh stamp
  // instead of overwriting an in-flight aliased send.  See LeaseRenewal.
  // Default: no lease protection.
  virtual LeaseRenewal renew_lease_strict() { return LeaseRenewal::kLeasesOff; }

  // Nanoseconds until a ceiling-forced write-buffer wrap could
  // overwrite this handle's borrowed mmap region (lease-protocol STEP-3), for
  // a zero-copy embedder to copy the bytes out BEFORE the force-wrap.
  // UINT64_MAX when no wrap is currently deferred on the stripe (or not
  // applicable: RAM hit, leases disabled, cache gone).
  [[nodiscard]] virtual uint64_t ns_until_forced_wrap() const {
    return UINT64_MAX;
  }
};

struct WriteHandleImpl {
  virtual ~WriteHandleImpl() = default;
  virtual void set_header(std::span<const std::byte> header) = 0;
  virtual void set_content_length(uint64_t length) = 0;
  virtual std::expected<size_t, CacheError> write(
      std::span<const std::byte> data) = 0;
  virtual std::expected<void, CacheError> close() = 0;
  virtual void abort() = 0;
  [[nodiscard]] virtual size_t bytes_written() const = 0;
  // Appended last (see WriteHandle::reserve).  The default serves
  // implementations that predate it.
  virtual std::expected<std::span<std::byte>, CacheError> reserve(
      size_t /*length*/) {
    return make_unexpected(CacheError::InvalidArgument);
  }
};

struct UpdateHandleImpl {
  virtual ~UpdateHandleImpl() = default;
  [[nodiscard]] virtual std::span<const std::byte> header() const = 0;
  virtual void set_header(std::span<const std::byte> header) = 0;
  virtual std::expected<void, CacheError> close() = 0;
  virtual void abort() = 0;
};

/// Handle for reading cached content.
///
/// ReadHandle provides access to cached data via memory-mapped I/O.
/// The handle holds a reference to the underlying mapped region.
///
/// **IMPORTANT: Lifetime Requirement**
/// ReadHandles must be destroyed (or closed) before calling Cache::stop().
/// The handle references memory owned by the cache; destroying the cache
/// while handles are outstanding results in undefined behavior.
///
/// A disk-hit handle also pins its stripe against write-buffer wraps while
/// it is open: at cache-full, wrap-needing fills to that stripe
/// are dropped on the handle's behalf (for as long as its lease stays
/// fresh, bounded by CacheConfig::lease_wrap_ceiling).  Close or destroy
/// handles as soon as the bytes are consumed — in particular, do NOT hold a
/// handle open across writes to the same cache, or those writes may be
/// dropped on the held handle's behalf (self-starvation).
///
/// Example:
/// ```cpp
/// {
///     auto read_result = cache->read_sync(key);
///     if (read_result) {
///         auto content = read_result->content();
///         // Use content...
///     }
/// } // Handle destroyed here
/// cache->stop(); // Safe - no handles outstanding
/// ```
class ReadHandle {
 public:
  ReadHandle() = default;
  ReadHandle(ReadHandle &&) noexcept = default;
  ReadHandle &operator=(ReadHandle &&) noexcept = default;
  ReadHandle(const ReadHandle &) = delete;
  ReadHandle &operator=(const ReadHandle &) = delete;
  ~ReadHandle() = default;

  Task<std::expected<size_t, CacheError>> read(std::span<std::byte> buffer);
  Task<std::expected<std::vector<std::byte>, CacheError>> read_all();

  // NOTE: bytes [0, Document::kHeaderSize) of the returned view are
  // VOLATILE. In-place metadata pwrites — hit-count / last-access updates
  // (update_hit_count_sync) and chain-repoint (remove_alternate_sync) —
  // mutate the document header WITHOUT moving the stripe wrap epoch, so the
  // lease/epoch protection does not cover them. A consumer replicating the
  // WHOLE document (header + payload) must not treat the header bytes as
  // stable: snapshot them once or re-derive from a stable source. The
  // content bytes (past kHeaderSize) are the epoch-protected region.
  [[nodiscard]] std::optional<std::span<const std::byte>> mapped_view() const {
    if (_impl) return _impl->mapped_view();
    return std::nullopt;
  }

  [[nodiscard]] std::span<const std::byte> header() const {
    if (_impl) return _impl->header();
    return {};
  }

  [[nodiscard]] std::span<const std::byte> content() const {
    if (_impl) return _impl->content();
    return {};
  }

  [[nodiscard]] uint64_t content_length() const {
    if (_impl) return _impl->content_length();
    return 0;
  }

  [[nodiscard]] bool is_ram_cache_hit() const {
    if (_impl) return _impl->is_ram_cache_hit();
    return false;
  }

  // File offset of content within the volume (for sendfile).
  // Returns kNoFileOffset when not eligible.
  static constexpr uint64_t kNoFileOffset = ReadHandleImpl::kNoFileOffset;
  [[nodiscard]] uint64_t content_file_offset() const {
    if (_impl) return _impl->content_file_offset();
    return kNoFileOffset;
  }

  // Re-stamp the read lease pinning this handle's mmap borrow.
  // A disk-hit handle's bytes are protected from write-buffer-wrap
  // overwrite while the handle is OPEN and the stripe lease holds (issue
  // The wrap gate is borrow-scoped — closing/destroying the handle
  // returns write capacity to the stripe immediately, so protection never
  // outlives the handle).  The lease is stamped for
  // CacheConfig::read_lease_duration (T) at read time, and holders that
  // keep the borrow longer must renew at a cadence <= 3T/4 (the
  // write-avoidance guard's protection floor).  Renewing is NOT sufficient
  // on its own: CacheConfig::lease_wrap_ceiling is a per-STRIPE-EPISODE
  // anti-starvation bound, not a per-hold budget — the deferral clock
  // starts when ANY borrow first defers a wrap on the stripe, so a borrow
  // taken late in an episode may have far less than the full ceiling (down
  // to ~zero) before a forced wrap overwrites its region regardless of
  // renewals.  A holder that keeps a borrow across possible forced wraps
  // must poll ns_until_forced_wrap() and copy the bytes before it expires.
  // Returns false when there is no lease to renew (RAM-cache hit,
  // leases disabled, invalid handle).
  bool renew_lease() {
    if (_impl) return _impl->renew_lease();
    return false;
  }

  // Intent-checked renewal for the aliased zero-copy path.  Call
  // before every aliased send of the borrowed region; act on the result
  // per LeaseRenewal (kOk keep aliasing, kCopyNow/kLeasesOff de-alias by
  // copying, kTorn abort).  kLeasesOff when no lease/handle applies.
  LeaseRenewal renew_lease_strict() {
    if (_impl) return _impl->renew_lease_strict();
    return LeaseRenewal::kLeasesOff;
  }

  // Nanoseconds until a ceiling-forced wrap could overwrite this borrow
  // (lease-protocol STEP-3).  UINT64_MAX = no deferred wrap / not applicable.
  // NORMATIVE for aliased (zero-copy) serving: an aliased consumer MUST
  // poll this with a margin STRICTLY GREATER than its maximum single
  // send-burst duration and de-alias (copy the bytes out of the mmap)
  // as soon as the value drops within that margin. renew_lease_strict()
  // alone cannot protect the FINAL burst: a force that lands mid-burst is
  // detected only by a LATER renew, and the final burst has none — so the
  // ns_until_forced_wrap() margin, not the strict renew, is what keeps the
  // last send safe. Polling this preserves the alias for the uncontended
  // fast case while bounding exposure to a forced wrap.
  [[nodiscard]] uint64_t ns_until_forced_wrap() const {
    if (_impl) return _impl->ns_until_forced_wrap();
    return UINT64_MAX;
  }

  [[nodiscard]] bool is_valid() const noexcept { return _impl != nullptr; }

  void close() noexcept { _impl.reset(); }

 private:
  friend class Cache;
  friend class Volume;

  std::shared_ptr<ReadHandleImpl> _impl;

  explicit ReadHandle(std::shared_ptr<ReadHandleImpl> impl)
      : _impl(std::move(impl)) {}
};

class WriteHandle {
 public:
  WriteHandle() = default;
  WriteHandle(WriteHandle &&) noexcept = default;
  WriteHandle &operator=(WriteHandle &&) noexcept = default;
  WriteHandle(const WriteHandle &) = delete;
  WriteHandle &operator=(const WriteHandle &) = delete;

  ~WriteHandle() {
    // RAII: abort any incomplete write to prevent resource leaks and cache
    // corruption
    if (_impl) {
      _impl->abort();
    }
  }

  Task<std::expected<size_t, CacheError>> write(
      std::span<const std::byte> data);

  // Sync version for convenience
  std::expected<size_t, CacheError> write_sync(
      std::span<const std::byte> data) {
    if (_impl) return _impl->write(data);
    return make_unexpected(CacheError::InvalidArgument);
  }

  // Append `length` bytes to the object and return them for the caller to
  // fill in place: the zero-copy form of write_sync() for a producer that
  // generates the content itself (a KV engine staging tensors, a rewriter
  // emitting output), so it is not first built in a caller buffer and then
  // copied into the handle.  Mixes freely with write_sync(); the bytes count
  // as written as soon as reserve() returns.
  //
  // Contract:
  //  - The span is valid until the next write_sync(), reserve(), close or
  //    abort on this handle.  Fill all of it before closing: the bytes are
  //    NOT zeroed, and whatever they hold at close is what is stored.
  //  - The checksum is computed at close, over the final content, so writes
  //    into the span up to close are covered.
  //  - Nothing is visible to readers before close commits, exactly as with
  //    write_sync().  An abort (or destroying the handle unclosed) after a
  //    partial fill discards everything; nothing is published.
  //  - Same limits and errors as write_sync(): ObjectTooLarge past
  //    max_object_size, NoSpace past the 4 GiB document limit, Closed on a
  //    closed or aborted handle.  A failed reserve() appends nothing.
  std::expected<std::span<std::byte>, CacheError> reserve(size_t length) {
    if (_impl) return _impl->reserve(length);
    return make_unexpected(CacheError::InvalidArgument);
  }

  void set_header(std::span<const std::byte> header) {
    if (_impl) _impl->set_header(header);
  }

  void set_content_length(uint64_t length) {
    if (_impl) _impl->set_content_length(length);
  }

  Task<std::expected<void, CacheError>> close();

  // Sync version for convenience
  std::expected<void, CacheError> close_sync() {
    if (_impl) return _impl->close();
    return make_unexpected(CacheError::InvalidArgument);
  }

  void abort() noexcept {
    if (_impl) _impl->abort();
    _impl.reset();
  }

  [[nodiscard]] bool is_valid() const noexcept { return _impl != nullptr; }

  [[nodiscard]] size_t bytes_written() const noexcept {
    if (_impl) return _impl->bytes_written();
    return 0;
  }

 private:
  friend class Cache;
  friend class Volume;

  std::shared_ptr<WriteHandleImpl> _impl;

  explicit WriteHandle(std::shared_ptr<WriteHandleImpl> impl)
      : _impl(std::move(impl)) {}
};

class UpdateHandle {
 public:
  UpdateHandle() = default;
  UpdateHandle(UpdateHandle &&) noexcept = default;
  UpdateHandle &operator=(UpdateHandle &&) noexcept = default;
  UpdateHandle(const UpdateHandle &) = delete;
  UpdateHandle &operator=(const UpdateHandle &) = delete;

  ~UpdateHandle() {
    // RAII: abort any incomplete update to prevent resource leaks
    if (_impl) {
      _impl->abort();
    }
  }

  [[nodiscard]] std::span<const std::byte> header() const {
    if (_impl) return _impl->header();
    return {};
  }

  void set_header(std::span<const std::byte> header) {
    if (_impl) _impl->set_header(header);
  }

  Task<std::expected<void, CacheError>> close();

  void abort() noexcept {
    if (_impl) _impl->abort();
    _impl.reset();
  }

  [[nodiscard]] bool is_valid() const noexcept { return _impl != nullptr; }

 private:
  friend class Cache;
  friend class Volume;

  std::shared_ptr<UpdateHandleImpl> _impl;

  explicit UpdateHandle(std::shared_ptr<UpdateHandleImpl> impl)
      : _impl(std::move(impl)) {}
};

}  // namespace cyclone
