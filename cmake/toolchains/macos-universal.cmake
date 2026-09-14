# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Toolchain file for building macOS Universal binaries (x86_64 + arm64)
#
# Prerequisites:
#   - Xcode with support for both architectures
#   - brew install openssl@3 (Homebrew OpenSSL)
#
# Usage:
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/macos-universal.cmake
#
# Verify universal binary:
#   lipo -archs build/libcyclone-cache.a  # Should show: x86_64 arm64

set(CMAKE_SYSTEM_NAME Darwin)

# Build for both x86_64 and arm64
set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "macOS architectures")

# Minimum deployment target (macOS 11 Big Sur for ARM64 support)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")

# Homebrew OpenSSL paths (universal builds need special handling)
# Homebrew installs architecture-specific versions, so for true universal
# builds you may need to build OpenSSL yourself or use separate builds
if(NOT OPENSSL_ROOT_DIR)
  # Try common Homebrew locations
  if(EXISTS "/opt/homebrew/opt/openssl@3")
    set(OPENSSL_ROOT_DIR "/opt/homebrew/opt/openssl@3")
  elseif(EXISTS "/usr/local/opt/openssl@3")
    set(OPENSSL_ROOT_DIR "/usr/local/opt/openssl@3")
  endif()
endif()
