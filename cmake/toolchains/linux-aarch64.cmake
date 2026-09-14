# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Toolchain file for cross-compiling to Linux ARM64 (aarch64) from x86_64
#
# Prerequisites (Ubuntu/Debian):
#   sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu
#   sudo apt install libssl-dev:arm64  # For OpenSSL (requires multiarch)
#
# Usage:
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/linux-aarch64.cmake

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# Cross-compiler settings
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

# Search paths for cross-compiled libraries
set(CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu)

# Search for programs in the host environment
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)

# Search for libraries and headers in the target environment
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# OpenSSL hints for cross-compilation
# Users may need to set OPENSSL_ROOT_DIR if not found automatically
if(NOT OPENSSL_ROOT_DIR)
  set(OPENSSL_ROOT_DIR /usr/aarch64-linux-gnu)
endif()
