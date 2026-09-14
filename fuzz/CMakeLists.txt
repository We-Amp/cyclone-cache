# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# libFuzzer harnesses for the read gauntlet (see fuzz/README.md).
#
# Only configured when CYCLONE_BUILD_FUZZERS=ON, which the top-level
# CMakeLists guards to Clang and instruments the whole cyclone-cache library
# with -fsanitize=fuzzer-no-link,address,undefined (vptr carved out,
# mirroring the asan CI job) for coverage-guided fuzzing.  Each
# target links the fuzzer runtime (which supplies main()); make_corpus is a
# plain tool with its own main().

set(CYCLONE_FUZZ_TARGETS
  fuzz_document_parse
  fuzz_volume_open
  fuzz_c_api
)

foreach(target ${CYCLONE_FUZZ_TARGETS})
  add_executable(${target} ${target}.cpp)
  target_link_libraries(${target} PRIVATE cyclone-cache)
  target_include_directories(${target} PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(${target} PRIVATE
    -fsanitize=fuzzer,address,undefined -fno-sanitize=vptr -fno-sanitize-recover=undefined)
  target_link_options(${target} PRIVATE
    -fsanitize=fuzzer,address,undefined -fno-sanitize=vptr -fno-sanitize-recover=undefined)
endforeach()

# Seed-corpus generator: a normal executable (its own main()), ASan-only.
add_executable(make_corpus make_corpus.cpp)
target_link_libraries(make_corpus PRIVATE cyclone-cache)
target_include_directories(make_corpus PRIVATE ${CMAKE_SOURCE_DIR}/src)
