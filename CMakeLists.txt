# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

cmake_minimum_required(VERSION 3.20)
# C is enabled only for the C-ABI header compile guard in tests/c_api; the
# library itself is C++ only.
project(cyclone-cache VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(CYCLONE_BUILD_TESTS "Build tests" ON)
option(CYCLONE_BUILD_HTTP_PLUGIN "Build HTTP alternate plugin" ON)
option(CYCLONE_BUILD_EXAMPLES "Build examples" ON)
option(CYCLONE_BUILD_BENCHMARKS "Build benchmarks" ON)
option(CYCLONE_BUILD_CUDA_BENCHMARKS "Build the CUDA KV transfer benchmark" OFF)
if(CYCLONE_BUILD_CUDA_BENCHMARKS)
  if(WIN32)
    # kv_gpu_cuda_host.cpp uses POSIX file and mmap APIs (unistd, sys/mman).
    message(FATAL_ERROR
      "CYCLONE_BUILD_CUDA_BENCHMARKS is not supported on Windows: the "
      "kv_gpu_cuda host translation unit is POSIX-only")
  endif()
  if(NOT CYCLONE_BUILD_BENCHMARKS)
    message(WARNING
      "CYCLONE_BUILD_CUDA_BENCHMARKS=ON has no effect while "
      "CYCLONE_BUILD_BENCHMARKS=OFF; kv_gpu_cuda will not be built")
  endif()
endif()
option(CYCLONE_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(CYCLONE_USE_BUNDLED_SHA256 "Use bundled SHA-256 instead of OpenSSL (for cross-compilation)" OFF)

if(CYCLONE_ENABLE_ASAN AND NOT MSVC)
  add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address)
endif()

# libFuzzer harnesses (fuzz/).  Clang-only: needs -fsanitize=fuzzer.  When on,
# instrument the ENTIRE library (and everything else built in this tree) with
# coverage + ASan so the fuzzer sees edge coverage through cyclone-cache.  The
# per-target fuzzer runtime is linked in fuzz/CMakeLists.txt.  Build fuzzers on
# their own (tests/examples/benchmarks off) to keep the instrumented tree lean.
option(CYCLONE_BUILD_FUZZERS "Build libFuzzer targets (Clang only)" OFF)
if(CYCLONE_BUILD_FUZZERS)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR
      "CYCLONE_BUILD_FUZZERS requires Clang (needs -fsanitize=fuzzer)")
  endif()
  add_compile_options(-fsanitize=fuzzer-no-link,address,undefined
    -fno-sanitize=vptr -fno-sanitize-recover=undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined -fno-sanitize=vptr -fno-sanitize-recover=undefined)
endif()

# vcpkg integration
if(DEFINED CMAKE_TOOLCHAIN_FILE AND CMAKE_TOOLCHAIN_FILE MATCHES "vcpkg")
  message(STATUS "Using vcpkg toolchain")
endif()

# Cross-compilation OpenSSL hints
if(CMAKE_CROSSCOMPILING AND NOT OPENSSL_ROOT_DIR AND NOT CYCLONE_USE_BUNDLED_SHA256)
  message(STATUS "Cross-compiling: set OPENSSL_ROOT_DIR if OpenSSL not found, or use -DCYCLONE_USE_BUNDLED_SHA256=ON")
endif()

# OpenSSL is optional when using bundled SHA-256
if(NOT CYCLONE_USE_BUNDLED_SHA256)
  find_package(OpenSSL REQUIRED)
endif()
find_package(Threads REQUIRED)

set(CYCLONE_LIBRARY_SOURCES
  src/core/cache.cpp
  src/core/key.cpp
  src/core/volume.cpp
  src/core/directory.cpp
  src/core/mmap_directory.cpp
  src/core/document.cpp
  src/core/crc32c.cpp
  src/core/write_buffer.cpp
  src/core/hit_tracker.cpp
  src/ram_cache/clfus.cpp
  src/ram_cache/lru.cpp
  src/io/mapped_file.cpp
  src/io/executor.cpp
  src/plugin/plugin_manager.cpp
  src/optimization/work_queue.cpp
  src/optimization/adaptive_pool.cpp
  src/optimization/load_monitor.cpp
  src/optimization/optimization_engine.cpp
  src/c_api/cyclone_c.cpp
)
if(CYCLONE_BUILD_HTTP_PLUGIN)
  list(APPEND CYCLONE_LIBRARY_SOURCES
    src/plugin/http/http_metadata.cpp
    src/plugin/http/http_alternate.cpp
  )
endif()

# One definition of how a Cyclone library target is built, shared by the
# release library and the test-seam variant below.
function(cyclone_configure_library target)
  target_include_directories(${target}
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:include>
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_link_libraries(${target}
    PUBLIC
      Threads::Threads
  )

  if(CYCLONE_USE_BUNDLED_SHA256)
    target_compile_definitions(${target} PRIVATE CYCLONE_USE_BUNDLED_SHA256)
  else()
    target_link_libraries(${target} PRIVATE OpenSSL::Crypto)
  endif()

  target_compile_features(${target} PUBLIC cxx_std_23)

  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(${target} PRIVATE CYCLONE_PLATFORM_LINUX)
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_compile_definitions(${target} PRIVATE CYCLONE_PLATFORM_MACOS)
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_compile_definitions(${target} PRIVATE CYCLONE_PLATFORM_WINDOWS)
  endif()

  if(CYCLONE_BUILD_HTTP_PLUGIN)
    target_compile_definitions(${target} PUBLIC CYCLONE_HTTP_PLUGIN)
  endif()
endfunction()

add_library(cyclone-cache ${CYCLONE_LIBRARY_SOURCES})
cyclone_configure_library(cyclone-cache)
if(CYCLONE_USE_BUNDLED_SHA256)
  message(STATUS "Using bundled SHA-256 implementation (no OpenSSL dependency)")
endif()

if(CYCLONE_BUILD_TESTS)
  enable_testing()
  find_package(Catch2 3 QUIET)
  if(NOT Catch2_FOUND)
    include(FetchContent)
    FetchContent_Declare(
      Catch2
      GIT_REPOSITORY https://github.com/catchorg/Catch2.git
      GIT_TAG v3.4.0
    )
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
  endif()
  include(Catch)

  add_executable(cyclone-tests
    tests/unit/test_key.cpp
    tests/unit/test_directory.cpp
    tests/unit/test_document.cpp
    tests/unit/test_crc32c.cpp
    tests/unit/test_alternate.cpp
    tests/unit/test_hit_tracker.cpp
    tests/unit/test_clfus.cpp
    tests/unit/test_lru.cpp
    tests/unit/test_mapped_file.cpp
    tests/unit/test_plugin.cpp
    tests/unit/test_http_plugin.cpp
    tests/unit/test_edge_cases.cpp
    tests/unit/test_security.cpp
    tests/unit/test_version_compat.cpp
    tests/unit/test_fuzzing.cpp
    tests/unit/test_work_queue.cpp
    tests/unit/test_adaptive_pool.cpp
    tests/unit/test_load_monitor.cpp
    tests/integration/test_cache_roundtrip.cpp
    tests/integration/test_concurrent.cpp
    tests/integration/test_eviction.cpp
    tests/integration/test_lifecycle.cpp
    tests/integration/test_optimization.cpp
    tests/unit/test_c_api.cpp
    tests/unit/multi_process_test.cpp
    tests/unit/test_mmap_directory.cpp
    tests/integration/test_handle.cpp
    tests/unit/test_error.cpp
    tests/unit/test_async_api.cpp
    tests/unit/test_config.cpp
    tests/integration/test_stabilization.cpp
    tests/integration/test_production_readiness.cpp
    tests/integration/test_purge.cpp
    tests/integration/test_lockfree_read_races.cpp
    tests/integration/test_header_rmw_races.cpp
    tests/integration/test_power_loss.cpp
    tests/integration/test_small_tier.cpp
    tests/integration/test_wrap_telemetry.cpp
    tests/integration/test_lease_pinning.cpp
    tests/integration/test_tag_collision.cpp
    tests/integration/test_wrap_phase_aba.cpp
    tests/integration/test_alternate_chain_bound.cpp
    tests/integration/test_ram_alternate_invalidation.cpp
    tests/integration/test_ram_coherence_cross_process.cpp
    tests/integration/test_multiprocess_writers.cpp
    tests/integration/test_stress_nightly.cpp
    tests/integration/test_fingerprint_filenames.cpp
    tests/unit/test_inode_revalidation.cpp
    tests/integration/test_gc_superseded.cpp
    tests/integration/test_max_object_size.cpp
    tests/integration/test_write_handle_volume_lifetime.cpp
    tests/integration/test_reset_gate_spawn.cpp
    tests/integration/test_readahead.cpp
    tests/integration/test_wrap_retention.cpp
    tests/integration/test_publish_after_snapshot.cpp
    tests/integration/test_seqlock_read_wait.cpp
  )
  # Compile guard: cyclone_c.h is the C ABI header, so it must stay valid C11
  # (it once used C++ alias declarations and did not compile as C).  Built as
  # an OBJECT library so the C standard is scoped to this one TU, and linked
  # into cyclone-tests so a broken header fails the ordinary test build.
  enable_language(C)  # only the guard below is C; keep it out of LANGUAGES
  add_library(cyclone-c-header-check OBJECT tests/c_api/c_header_compiles.c)
  set_target_properties(cyclone-c-header-check PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  target_include_directories(cyclone-c-header-check
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

  # The test binary links a TEST-SEAM build of the library: the same
  # sources compiled with CYCLONE_TEST_SEAMS, which compiles in the reader
  # seams (pause points on the lock-free read path) and lets the
  # CYCLONE_TEST_WRAP_RETENTION environment variable choose the default
  # eviction mode, so the whole suite runs in both modes.  The release
  # library (cyclone-cache) never defines it: its read path carries no seam.
  add_library(cyclone-cache-testseams STATIC ${CYCLONE_LIBRARY_SOURCES})
  cyclone_configure_library(cyclone-cache-testseams)
  target_compile_definitions(cyclone-cache-testseams PUBLIC CYCLONE_TEST_SEAMS)

  target_link_libraries(cyclone-tests PRIVATE cyclone-cache-testseams
                                              Catch2::Catch2WithMain
                                              cyclone-c-header-check)
  target_include_directories(cyclone-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
                                                   ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  catch_discover_tests(cyclone-tests)

  # Helper peer for the spawn-based reset-gate tests.  A separate image (never
  # a re-exec of cyclone-tests): Catch2WithMain owns that binary's main(),
  # catch_discover_tests runs --list-tests at BUILD time, and the Windows CI
  # lane keys AppVerifier IFEO settings on the image name.  Links the library
  # only -- no Catch2.
  add_executable(cyclone-test-peer tests/support/peer_main.cpp)
  # Same seam build as the tests, so a peer honours the same mode override.
  target_link_libraries(cyclone-test-peer PRIVATE cyclone-cache-testseams)
  # The seam mode drives Volume's writer seams (src/core/volume.hpp).
  target_include_directories(cyclone-test-peer PRIVATE
                             ${CMAKE_CURRENT_SOURCE_DIR}/src)
  add_dependencies(cyclone-tests cyclone-test-peer)
  target_compile_definitions(cyclone-tests PRIVATE
    CYCLONE_TEST_PEER_EXE="$<TARGET_FILE:cyclone-test-peer>")
endif()

if(CYCLONE_BUILD_EXAMPLES)
  add_executable(basic_usage examples/basic_usage.cpp)
  target_link_libraries(basic_usage PRIVATE cyclone-cache)
endif()

if(CYCLONE_BUILD_BENCHMARKS)
  add_executable(cache_benchmark benchmarks/cache_benchmark.cpp)
  target_link_libraries(cache_benchmark PRIVATE cyclone-cache)

  add_executable(performance_baseline benchmarks/performance_baseline.cpp)
  target_link_libraries(performance_baseline PRIVATE cyclone-cache)

  add_executable(concurrent_read_bench benchmarks/concurrent_read_bench.cpp)
  target_link_libraries(concurrent_read_bench PRIVATE cyclone-cache)

  add_executable(kv_bench benchmarks/kv_bench.cpp)
  target_link_libraries(kv_bench PRIVATE cyclone-cache)

  # Bounded-capacity churn (doc/kv-cache-benchmark/kv-churn-spec.md).  Reads
  # the mmap-directory size from src/core to size the data area exactly.
  add_executable(kv_churn benchmarks/kv_churn.cpp)
  target_link_libraries(kv_churn PRIVATE cyclone-cache)
  target_include_directories(kv_churn PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  add_executable(kv_churn_policy benchmarks/kv_churn_policy.cpp)
  target_link_libraries(kv_churn_policy PRIVATE cyclone-cache)

  add_executable(crc32c_bench benchmarks/crc32c_bench.cpp)
  target_link_libraries(crc32c_bench PRIVATE cyclone-cache)
  target_include_directories(crc32c_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

  # Apple-only: the Metal GPU-transfer experiment (ObjC++).  EVERYTHING it
  # needs -- including enable_language(OBJCXX), which would otherwise probe for
  # an Objective-C++ compiler on every platform -- lives inside this guard, so
  # Linux and Windows configures are bit-for-bit unaffected.
  if(APPLE)
    enable_language(OBJCXX)
    find_library(METAL_FRAMEWORK Metal)
    find_library(FOUNDATION_FRAMEWORK Foundation)
    if(METAL_FRAMEWORK AND FOUNDATION_FRAMEWORK)
      add_executable(kv_gpu_metal benchmarks/kv_gpu_metal.mm)
      set_target_properties(kv_gpu_metal PROPERTIES
        OBJCXX_STANDARD 23
        OBJCXX_STANDARD_REQUIRED ON)
      target_compile_options(kv_gpu_metal PRIVATE -fobjc-arc)
      target_link_libraries(kv_gpu_metal PRIVATE
        cyclone-cache
        ${METAL_FRAMEWORK}
        ${FOUNDATION_FRAMEWORK})

      # LMDB is the optional second zero-copy store to compare against; the
      # benchmark prints "lmdb: not built" and skips those rows without it.
      find_path(LMDB_INCLUDE_DIR lmdb.h
        HINTS /opt/homebrew/opt/lmdb/include /usr/local/opt/lmdb/include)
      find_library(LMDB_LIBRARY NAMES lmdb
        HINTS /opt/homebrew/opt/lmdb/lib /usr/local/opt/lmdb/lib)
      if(LMDB_INCLUDE_DIR AND LMDB_LIBRARY)
        message(STATUS "kv_gpu_metal: LMDB ${LMDB_LIBRARY}")
        target_compile_definitions(kv_gpu_metal PRIVATE
          CYCLONE_KV_GPU_HAVE_LMDB)
        target_include_directories(kv_gpu_metal SYSTEM PRIVATE
          ${LMDB_INCLUDE_DIR})
        target_link_libraries(kv_gpu_metal PRIVATE ${LMDB_LIBRARY})
      else()
        message(STATUS
          "kv_gpu_metal: LMDB not found - the lmdb comparison rows are absent")
      endif()
    else()
      message(STATUS
        "kv_gpu_metal: Metal/Foundation not found - target not built")
    endif()
  endif()

  # Opt-in: the CUDA GPU-transfer experiment, the discrete-GPU counterpart of
  # kv_gpu_metal.  OFF by default and EVERYTHING it needs -- including
  # enable_language(CUDA) and find_package(CUDAToolkit), both of which would
  # otherwise probe for nvcc on machines that have none -- lives inside this
  # guard, so an ordinary configure is bit-for-bit unaffected.
  #
  # The target is deliberately two translation units.  Cyclone's public
  # headers are C++23 (std::expected) and nvcc 12.4 stops at C++20, so the
  # store logic is compiled as C++23 by the host compiler in
  # kv_gpu_cuda_host.cpp and only the CUDA runtime calls go through nvcc in
  # kv_gpu_cuda.cu, behind the plain-C seam in kv_gpu_cuda.h.  Pass
  # -DCMAKE_CUDA_HOST_COMPILER=g++-13 (CUDA 12.4 accepts GCC <= 13) and
  # -DCMAKE_CUDA_ARCHITECTURES=<sm> for the device in the machine.
  if(CYCLONE_BUILD_CUDA_BENCHMARKS)
    # The default has to be in place before enable_language(CUDA): that call
    # seeds CMAKE_CUDA_ARCHITECTURES from the compiler's own default, after
    # which a NOT DEFINED check never fires.
    if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
      # Pascal: the GTX 1050 the experiment was first run on.
      set(CMAKE_CUDA_ARCHITECTURES 61)
    endif()
    enable_language(CUDA)
    find_package(CUDAToolkit REQUIRED)
    if(CMAKE_CUDA_HOST_COMPILER)
      message(STATUS
        "kv_gpu_cuda: nvcc host compiler ${CMAKE_CUDA_HOST_COMPILER}")
    endif()
    message(STATUS
      "kv_gpu_cuda: CUDA ${CUDAToolkit_VERSION}, architectures "
      "${CMAKE_CUDA_ARCHITECTURES}")

    # CMake can only request a CUDA dialect it has a flag for, and CUDA20
    # arrived in CMake 3.25 -- Ubuntu 22.04 ships 3.22, where asking for it
    # fails the generate step outright ("does not know the compile flags to
    # use to enable it").  The .cu is deliberately plain (runtime calls,
    # memset/strncpy), so it compiles as C++17 just as well; ask for 20 only
    # where CMake actually knows the flag.
    if(DEFINED CMAKE_CUDA20_STANDARD_COMPILE_OPTION)
      set(CYCLONE_CUDA_STD 20)
    else()
      set(CYCLONE_CUDA_STD 17)
      message(STATUS
        "kv_gpu_cuda: CMake ${CMAKE_VERSION} has no CUDA20 dialect - "
        "building the .cu as C++17")
    endif()

    add_executable(kv_gpu_cuda
      benchmarks/kv_gpu_cuda.cu
      benchmarks/kv_gpu_cuda_host.cpp)
    set_target_properties(kv_gpu_cuda PROPERTIES
      CUDA_STANDARD ${CYCLONE_CUDA_STD}
      CUDA_STANDARD_REQUIRED ON
      CXX_STANDARD 23
      CXX_STANDARD_REQUIRED ON)
    target_link_libraries(kv_gpu_cuda PRIVATE cyclone-cache CUDA::cudart)

    # LMDB is the optional second zero-copy store to compare against; the
    # benchmark prints "lmdb: not built" and skips those rows without it.
    find_path(LMDB_INCLUDE_DIR lmdb.h
      HINTS /opt/homebrew/opt/lmdb/include /usr/local/opt/lmdb/include)
    find_library(LMDB_LIBRARY NAMES lmdb
      HINTS /opt/homebrew/opt/lmdb/lib /usr/local/opt/lmdb/lib)
    if(LMDB_INCLUDE_DIR AND LMDB_LIBRARY)
      message(STATUS "kv_gpu_cuda: LMDB ${LMDB_LIBRARY}")
      # The definition is only meaningful to the host TU; the .cu never sees
      # a store.
      set_source_files_properties(benchmarks/kv_gpu_cuda_host.cpp PROPERTIES
        COMPILE_DEFINITIONS CYCLONE_KV_GPU_HAVE_LMDB)
      target_include_directories(kv_gpu_cuda SYSTEM PRIVATE
        ${LMDB_INCLUDE_DIR})
      target_link_libraries(kv_gpu_cuda PRIVATE ${LMDB_LIBRARY})
    else()
      message(STATUS
        "kv_gpu_cuda: LMDB not found - the lmdb comparison rows are absent")
    endif()
  endif()
endif()

if(CYCLONE_BUILD_FUZZERS)
  add_subdirectory(fuzz)
endif()

install(TARGETS cyclone-cache
  EXPORT cyclone-cache-targets
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
  RUNTIME DESTINATION bin
  INCLUDES DESTINATION include
)

install(DIRECTORY include/cyclone
  DESTINATION include
)

install(EXPORT cyclone-cache-targets
  FILE cyclone-cache-config.cmake
  NAMESPACE cyclone::
  DESTINATION lib/cmake/cyclone-cache
)
