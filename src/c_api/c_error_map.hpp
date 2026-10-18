// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Mapping from the library's CacheError to the C API's CycloneError.
// Private to the C bridge; a header of its own so the tests can check the
// whole table.

#ifndef CYCLONE_SRC_C_API_C_ERROR_MAP_HPP
#define CYCLONE_SRC_C_API_C_ERROR_MAP_HPP

#include "cyclone/cyclone_c.h"
#include "cyclone/error.hpp"

namespace cyclone::c_api_detail {

// Every enumerator is listed (no default), so a new CacheError draws a
// -Wswitch warning (not an error: the build does not use -Werror) until it
// is mapped here.  Conditions the C API cannot reach map to
// CYCLONE_INTERNAL_ERROR, as does a value outside the enum.
inline CycloneError to_c_error(CacheError e) {
  switch (e) {
    case CacheError::Success:
      return CYCLONE_OK;
    case CacheError::NotFound:
    case CacheError::AlternateNotFound:
      return CYCLONE_NOT_FOUND;
    case CacheError::Exists:
      return CYCLONE_EXISTS;
    case CacheError::NoSpace:
      return CYCLONE_NO_SPACE;
    case CacheError::IoError:
      return CYCLONE_IO_ERROR;
    case CacheError::Corrupted:
    case CacheError::ChainCorrupted:
      return CYCLONE_CORRUPTED;
    case CacheError::InvalidKey:
      return CYCLONE_INVALID_KEY;
    case CacheError::InvalidArgument:
    case CacheError::InvalidConfiguration:
      return CYCLONE_INVALID_ARGUMENT;
    case CacheError::NotInitialized:
    case CacheError::Closed:  // the cache is stopping
      return CYCLONE_NOT_INITIALIZED;
    case CacheError::ResetRefusedLivePeer:
      return CYCLONE_RESET_REFUSED_LIVE_PEER;
    case CacheError::ObjectTooLarge:
      return CYCLONE_OBJECT_TOO_LARGE;
    case CacheError::Busy:
      return CYCLONE_BUSY;
    case CacheError::Timeout:
      return CYCLONE_TIMEOUT;
    case CacheError::NotOwned:
      return CYCLONE_NOT_OWNED;
    case CacheError::IncompatibleVersion:
      return CYCLONE_INCOMPATIBLE_VERSION;
    case CacheError::AlreadyOpen:
    case CacheError::PluginError:
    case CacheError::InternalError:
    case CacheError::TooManyAlternates:
    case CacheError::OptimizationQueueFull:
    case CacheError::OptimizationCancelled:
    case CacheError::TransformFailed:
      return CYCLONE_INTERNAL_ERROR;
  }
  return CYCLONE_INTERNAL_ERROR;
}

}  // namespace cyclone::c_api_detail

#endif  // CYCLONE_SRC_C_API_C_ERROR_MAP_HPP
