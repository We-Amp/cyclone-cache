# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Toolchain file for cross-compiling to Windows x86_64 using Zig
#
# Uses MinGW ABI (windows-gnu) which doesn't require Windows SDK.
# Produces .exe and .lib files that run on Windows.
#
# Prerequisites:
#   - Install Zig: https://ziglang.org/download/
#
# Usage:
#   cmake -B build-windows-x64 \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/windows-x64-zig.cmake

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# Get the directory containing this toolchain file
get_filename_component(_toolchain_dir "${CMAKE_CURRENT_LIST_DIR}" DIRECTORY)
set(_zig_wrapper_dir "${_toolchain_dir}/zig")

# Use zig wrapper scripts as compilers
set(CMAKE_C_COMPILER "${_zig_wrapper_dir}/zig-cc-x86_64-windows-gnu")
set(CMAKE_CXX_COMPILER "${_zig_wrapper_dir}/zig-cxx-x86_64-windows-gnu")
set(CMAKE_AR "${_zig_wrapper_dir}/zig-ar")
set(CMAKE_RANLIB "${_zig_wrapper_dir}/zig-ranlib")

# Windows executable suffix
set(CMAKE_EXECUTABLE_SUFFIX ".exe")
set(CMAKE_STATIC_LIBRARY_SUFFIX ".lib")

# Search paths
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Help CMake detect that this is cross-compilation
set(CMAKE_CROSSCOMPILING TRUE)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Windows-specific definitions
add_compile_definitions(_WIN32 CYCLONE_PLATFORM_WINDOWS)
