// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#pragma once

#include <cstdint>
#include <string>
#include <system_error>

namespace cyclone {

enum class CacheError : std::uint8_t {
  Success = 0,
  NotFound,
  Exists,
  NoSpace,
  IoError,
  Corrupted,
  InvalidKey,
  InvalidArgument,
  NotInitialized,
  AlreadyOpen,
  Closed,
  Busy,  // Transient contention; retry.  From a lookup (read, exists,
         // list/read alternates): a writer held the key's directory bucket
         // for the whole seqlock wait budget, so whether the key is present
         // is UNKNOWN -- deliberately not NotFound.
  Timeout,
  PluginError,
  InternalError,

  // Alternate chain errors
  TooManyAlternates,  // Exceeded kMaxAlternatesPerKey
  AlternateNotFound,  // Requested alternate doesn't exist
  ChainCorrupted,     // Cycle detected or invalid offset in alternate chain

  // Version compatibility errors
  IncompatibleVersion,  // Cache format version incompatible

  // Optimization system errors
  OptimizationQueueFull,  // Work queue at capacity
  OptimizationCancelled,  // Work item was cancelled
  TransformFailed,        // Plugin transform operation failed

  // Multi-process errors
  NotOwned,              // Stripe not owned by this process (write rejected)
  InvalidConfiguration,  // Invalid configuration (e.g., process_index >=
                         // total_processes)

  // Reset gate.  APPENDED AT THE TAIL: these values are compiled in lockstep
  // with consumers, so new enumerators go here to avoid renumbering.
  ResetRefusedLivePeer,  // A live peer still has this cache open, so the
                         // format/geometry reset it needs was refused rather
                         // than performed under the peer.  Distinct from
                         // IncompatibleVersion: the format is FINE, the cure is
                         // to drain the peer.  This error string is the ONLY
                         // signal a refusal has -- the Cache never starts, so
                         // no stats reader can observe it.
  ObjectTooLarge         // Write content exceeds the configured max_object_size
};

class CacheErrorCategory : public std::error_category {
 public:
  [[nodiscard]] const char *name() const noexcept override { return "cyclone"; }

  [[nodiscard]] std::string message(int ev) const override {
    switch (static_cast<CacheError>(ev)) {
      case CacheError::Success:
        return "success";
      case CacheError::NotFound:
        return "cache entry not found";
      case CacheError::Exists:
        return "cache entry already exists";
      case CacheError::NoSpace:
        return "no space available";
      case CacheError::IoError:
        return "I/O error";
      case CacheError::Corrupted:
        return "cache data corrupted";
      case CacheError::InvalidKey:
        return "invalid cache key";
      case CacheError::InvalidArgument:
        return "invalid argument";
      case CacheError::NotInitialized:
        return "cache not initialized";
      case CacheError::AlreadyOpen:
        return "handle already open";
      case CacheError::Closed:
        return "handle closed";
      case CacheError::Busy:
        return "resource busy";
      case CacheError::Timeout:
        return "operation timed out";
      case CacheError::PluginError:
        return "plugin error";
      case CacheError::InternalError:
        return "internal error";
      case CacheError::TooManyAlternates:
        return "too many alternates for key";
      case CacheError::AlternateNotFound:
        return "alternate not found";
      case CacheError::ChainCorrupted:
        return "alternate chain corrupted";
      case CacheError::IncompatibleVersion:
        return "cache format version incompatible";
      case CacheError::OptimizationQueueFull:
        return "optimization work queue at capacity";
      case CacheError::OptimizationCancelled:
        return "optimization work item was cancelled";
      case CacheError::TransformFailed:
        return "plugin transform operation failed";
      case CacheError::NotOwned:
        return "stripe not owned by this process";
      case CacheError::InvalidConfiguration:
        return "invalid configuration";
      case CacheError::ResetRefusedLivePeer:
        return "cache reset refused: another process still has this cache open";
      case CacheError::ObjectTooLarge:
        return "object exceeds configured max_object_size";
      default:
        return "unknown error";
    }
  }
};

inline const CacheErrorCategory &cache_error_category() {
  static CacheErrorCategory instance;
  return instance;
}

inline std::error_code make_error_code(CacheError e) {
  return {static_cast<int>(e), cache_error_category()};
}

}  // namespace cyclone

namespace std {
template <>
struct is_error_code_enum<cyclone::CacheError> : true_type {};
}  // namespace std
