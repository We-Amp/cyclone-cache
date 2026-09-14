// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace cyclone {

// Maximum number of alternates per cache key.
// Bounds chain traversal and storage overhead.
inline constexpr uint8_t kMaxAlternatesPerKey = 64;

// Alternate variant identifier.
// Maps user-agent capabilities to discrete variant classes for normalization.
// This prevents alternate explosion by grouping many UAs into few classes.
enum class AlternateId : uint8_t {
  // Original unmodified content (always present)
  Original = 0,

  // Compression variants (1-15)
  // These represent pre-compressed versions of the original content
  Brotli = 1,  // Accept-Encoding: br
  Zstd = 2,    // Accept-Encoding: zstd
  Gzip = 3,    // Accept-Encoding: gzip

  // Image format variants (16-31)
  // These represent transcoded versions of image content
  WebP = 16,    // Accept: image/webp
  AVIF = 17,    // Accept: image/avif
  JpegXL = 18,  // Accept: image/jxl

  // Combined/optimized variants (32-127)
  // Reserved for combinations like WebP+Brotli

  // Plugin-defined variants (128-255)
  // Custom variants defined by cache plugins
  Custom = 128,
};

// Convert AlternateId to human-readable string
constexpr std::string_view alternate_id_name(AlternateId id) {
  switch (id) {
    case AlternateId::Original:
      return "original";
    case AlternateId::Brotli:
      return "brotli";
    case AlternateId::Zstd:
      return "zstd";
    case AlternateId::Gzip:
      return "gzip";
    case AlternateId::WebP:
      return "webp";
    case AlternateId::AVIF:
      return "avif";
    case AlternateId::JpegXL:
      return "jxl";
    default:
      if (static_cast<uint8_t>(id) >=
          static_cast<uint8_t>(AlternateId::Custom)) {
        return "custom";
      }
      return "unknown";
  }
}

// Check if an AlternateId represents a compression variant
constexpr bool is_compression_alternate(AlternateId id) {
  auto val = static_cast<uint8_t>(id);
  return val >= 1 && val <= 15;
}

// Check if an AlternateId represents an image format variant
constexpr bool is_image_alternate(AlternateId id) {
  auto val = static_cast<uint8_t>(id);
  return val >= 16 && val <= 31;
}

// Information about a single alternate variant.
// Returned when enumerating alternates for a cache key.
struct AlternateInfo {
  AlternateId id = AlternateId::Original;
  uint64_t disk_offset = 0;     // Offset within the cache volume
  uint64_t content_length = 0;  // Size of the content
  uint32_t hit_count = 0;       // Number of cache hits
  std::chrono::system_clock::time_point last_access;  // Last access time
  std::vector<std::byte> header;  // User-provided metadata header (owned copy)
};

// Context provided when selecting an alternate at the storage layer.
// Contains request information used to choose the best variant.
struct AlternateSelectionContext {
  // Request metadata (e.g., serialized Accept-* headers)
  std::span<const std::byte> request_metadata;

  // Hint: prefer compressed variants if available
  bool prefer_compressed = true;

  // Hint: acceptable alternate IDs (empty = accept all). The shipped built-in
  // selectors (DefaultStorageSelector, CompressionAwareSelector) treat a
  // non-empty set as a hard restriction: ids outside it are never selected,
  // not even as a fallback. Custom selectors may treat it as a hint.
  std::span<const AlternateId> acceptable_alternates;
};

// Storage-layer alternate selector interface.
// Note: This is separate from the plugin-layer AlternateSelector in
// plugin/alternate.hpp which works with CacheVariant and is used for HTTP
// content negotiation.
class StorageAlternateSelector {
 public:
  virtual ~StorageAlternateSelector() = default;

  // Select the best alternate from the available variants.
  // Returns the index into the alternates span, or nullopt if none are
  // acceptable.
  [[nodiscard]] virtual std::optional<size_t> select(
      std::span<const AlternateInfo> alternates,
      const AlternateSelectionContext &ctx) const = 0;
};

// Default selector: returns the first alternate (usually Original).
// When ctx.acceptable_alternates is non-empty it restricts eligibility:
// the result is the FIRST alternate whose id is in
// the set, or nullopt ("none are acceptable", per the select() contract) when
// the set excludes every available id. An empty set behaves exactly as
// before.
class DefaultStorageSelector : public StorageAlternateSelector {
 public:
  [[nodiscard]] std::optional<size_t> select(
      std::span<const AlternateInfo> alternates,
      const AlternateSelectionContext &ctx) const override {
    if (alternates.empty()) {
      return std::nullopt;
    }

    // An empty acceptable set accepts every id (backward compatible).
    if (ctx.acceptable_alternates.empty()) {
      return 0;
    }

    // Non-empty set is a hard restriction: first acceptable in chain order.
    for (size_t i = 0; i < alternates.size(); ++i) {
      if (std::ranges::find(ctx.acceptable_alternates, alternates[i].id) !=
          ctx.acceptable_alternates.end()) {
        return i;
      }
    }
    return std::nullopt;
  }
};

// Compression-aware selector: prefers compressed variants when acceptable.
// Falls back to original if no compression variants are available.
// When ctx.acceptable_alternates is non-empty it restricts eligibility:
// ids outside the set are never selected — compression preference only ranks
// acceptable candidates, and with no acceptable alternate the result is
// nullopt ("none are acceptable", per the select() contract).
class CompressionAwareSelector : public StorageAlternateSelector {
 public:
  [[nodiscard]] std::optional<size_t> select(
      std::span<const AlternateInfo> alternates,
      const AlternateSelectionContext &ctx) const override {
    if (alternates.empty()) {
      return std::nullopt;
    }

    // An empty acceptable set accepts every id (backward compatible).
    auto is_acceptable = [&ctx](AlternateId id) {
      return ctx.acceptable_alternates.empty() ||
             std::ranges::find(ctx.acceptable_alternates, id) !=
                 ctx.acceptable_alternates.end();
    };

    if (!ctx.prefer_compressed) {
      // Return first acceptable (original) if compression not preferred
      for (size_t i = 0; i < alternates.size(); ++i) {
        if (is_acceptable(alternates[i].id)) {
          return i;
        }
      }
      return std::nullopt;
    }

    // Prefer Brotli > Zstd > Gzip > Original.
    //
    // Each slot keeps the FIRST occurrence of its id. An id may legitimately
    // appear more than once in a chain (re-recording the same alternate
    // prepends a new document without unlinking the superseded one), and
    // enumeration is newest-first, so the first occurrence is the newest
    // surviving version (remove_alternate_sync unlinks the first match, so
    // after a removal "newest surviving" is the precise invariant).
    // Overwriting on every match would pin the selector to the oldest copy.
    std::optional<size_t> brotli_idx, zstd_idx, gzip_idx, original_idx;
    std::optional<size_t> first_acceptable_idx;

    for (size_t i = 0; i < alternates.size(); ++i) {
      if (!is_acceptable(alternates[i].id)) {
        continue;
      }
      if (!first_acceptable_idx) {
        first_acceptable_idx = i;
      }
      switch (alternates[i].id) {
        case AlternateId::Brotli:
          if (!brotli_idx) {
            brotli_idx = i;
          }
          break;
        case AlternateId::Zstd:
          if (!zstd_idx) {
            zstd_idx = i;
          }
          break;
        case AlternateId::Gzip:
          if (!gzip_idx) {
            gzip_idx = i;
          }
          break;
        case AlternateId::Original:
          if (!original_idx) {
            original_idx = i;
          }
          break;
        default:
          break;
      }
    }

    if (brotli_idx) {
      return brotli_idx;
    }
    if (zstd_idx) {
      return zstd_idx;
    }
    if (gzip_idx) {
      return gzip_idx;
    }
    if (original_idx) {
      return original_idx;
    }

    // Fallback to first acceptable alternate; nullopt when the acceptable
    // set excludes every available id (never serve an unacceptable id).
    return first_acceptable_idx;
  }
};

}  // namespace cyclone
