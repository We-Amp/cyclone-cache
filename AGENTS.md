# AGENTS.md

Cyclone Cache — a C++23 disk-cache library (CMake) consumed by mod_pagespeed
2.1 and the PageSpeed optimizer via a downstream Bazel `git_repository`.

**`CLAUDE.md` is the canonical context file. Read it first; this file does not
duplicate its detail.** Must-not-miss facts:

- **Style**: Google-based, enforced by `.clang-format` (`BasedOnStyle: Google`,
  `ColumnLimit: 80`). The private contribution gate rejects non-conforming code
  via `clang-format-20 --dry-run --Werror`. LLVM 20 specifically — not 18/19/21.
- **Build/test like the gate** (bundled SHA-256 + clang-20):
  `cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++-20 -DCYCLONE_USE_BUNDLED_SHA256=ON && cmake --build build -j$(nproc) && (cd build && ./cyclone-tests)`
- **Lint** (required gate): `run-clang-tidy-20 -p build "cyclone/(src|include/cyclone|tests|benchmarks)/.*\.(cpp|cc)$"`
  plus the clang-format check above. Exact commands and sanitizer recipes are in
  CLAUDE.md.
- **This repo is the source of truth.** Copies embedded in consumer checkouts
  (under `reference/` or `vendor/` directories there) are overwritten on
  sync — make ALL Cyclone edits here. Confirm your checkout with
  `git rev-parse --show-toplevel`; never edit a `reference/` or `vendor/`
  copy.
