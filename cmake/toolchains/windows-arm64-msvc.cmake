# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Toolchain file for cross-compiling to Windows ARM64 using MSVC
#
# Prerequisites:
#   - Visual Studio 2022 with ARM64 build tools installed
#   - vcpkg with ARM64 triplet: vcpkg install openssl:arm64-windows-static
#
# Usage:
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/windows-arm64-msvc.cmake -G "Visual Studio 17 2022"
#
# Note: This toolchain works with Visual Studio generator which handles
# the actual cross-compilation via CMAKE_GENERATOR_PLATFORM

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR ARM64)

# Visual Studio handles cross-compilation via platform selection
set(CMAKE_GENERATOR_PLATFORM ARM64)

# vcpkg triplet for ARM64 Windows
set(VCPKG_TARGET_TRIPLET "arm64-windows-static" CACHE STRING "vcpkg target triplet")

# Search paths
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
