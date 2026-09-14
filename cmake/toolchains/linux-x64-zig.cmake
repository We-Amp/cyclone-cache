# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Toolchain file for cross-compiling to Linux x86_64 using Zig
#
# Zig is an excellent cross-compiler that bundles libc for many platforms.
# This allows cross-compiling from any host (macOS, Windows, Linux) to Linux x86_64.
#
# Prerequisites:
#   - Install Zig: https://ziglang.org/download/
#     macOS: brew install zig
#     Linux: Download from ziglang.org or use package manager
#     Windows: Download from ziglang.org or use scoop/chocolatey
#
#   - OpenSSL for target platform (static build recommended):
#     See cmake/openssl/README.md for instructions on getting cross-compiled OpenSSL
#
# Usage:
#   cmake -B build-linux-x64 \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/linux-x64-zig.cmake \
#     -DOPENSSL_ROOT_DIR=/path/to/openssl-linux-x64
#
# Note: Uses musl libc for fully static binaries that run on any Linux distro.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# Get the directory containing this toolchain file
get_filename_component(_toolchain_dir "${CMAKE_CURRENT_LIST_DIR}" DIRECTORY)
set(_zig_wrapper_dir "${_toolchain_dir}/zig")

# Use zig wrapper scripts as compilers
set(CMAKE_C_COMPILER "${_zig_wrapper_dir}/zig-cc-x86_64-linux-musl")
set(CMAKE_CXX_COMPILER "${_zig_wrapper_dir}/zig-cxx-x86_64-linux-musl")
set(CMAKE_AR "${_zig_wrapper_dir}/zig-ar")
set(CMAKE_RANLIB "${_zig_wrapper_dir}/zig-ranlib")

# Search paths
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Static linking for portable binaries
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")

# Help CMake detect that this is cross-compilation
set(CMAKE_CROSSCOMPILING TRUE)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
