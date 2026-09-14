// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#pragma once

#include <expected>
#include <type_traits>
#include <utility>

namespace cyclone {

// Compatibility helper for std::unexpected
// Works around ambiguity in some libc++ versions where both
// std::unexpected() (deprecated function) and std::unexpected<E> (C++23 class)
// exist in the same namespace.

template <typename E>
[[nodiscard]] constexpr auto make_unexpected(E &&e) noexcept(
    std::is_nothrow_constructible_v<std::remove_cvref_t<E>, E>) {
#if defined(_LIBCPP_VERSION) && defined(_LIBCPP_ABI_NAMESPACE)
  // libc++ with versioned namespace (e.g., Zig's bundled libc++)
  return std::_LIBCPP_ABI_NAMESPACE::unexpected<std::remove_cvref_t<E>>(
      std::forward<E>(e));
#else
  // Standard library without namespace versioning
  return std::unexpected<std::remove_cvref_t<E>>(std::forward<E>(e));
#endif
}

}  // namespace cyclone
