#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Small cold reads (issue #29): check out and build the three trees
# driver.sh compares, on the Linux benchmark machine.
#   $HOME/cy29-base    origin/main
#   $HOME/cy29-after   origin/small-cold-reads (the change)
#   $HOME/cy29-normal  origin/main with the open-time MADV_RANDOM removed
#                      (experiment only, never proposed)
# Then runs the after-tree's full test suite.
set -eu
REPO=https://github.com/We-Amp/cyclone-cache.git
tree() {  # DIR REF
  local dir=$HOME/$1 ref=$2
  [ -d "$dir" ] || git clone -q "$REPO" "$dir"
  git -C "$dir" fetch -q origin
  git -C "$dir" checkout -q --detach "$ref"
  git -C "$dir" reset -q --hard "$ref"
}
tree cy29-base origin/main
tree cy29-after origin/small-cold-reads
tree cy29-normal origin/main
sed -i '/(void)madvise(_persistent_base, _persistent_size, MADV_RANDOM);/d' \
  "$HOME/cy29-normal/src/io/mapped_file.cpp"
! grep -q '_persistent_size, MADV_RANDOM' \
  "$HOME/cy29-normal/src/io/mapped_file.cpp"
for t in cy29-base cy29-after cy29-normal; do
  cmake -S "$HOME/$t" -B "$HOME/$t/build" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_COMPILER=clang++-20 -DCYCLONE_USE_BUNDLED_SHA256=ON > /dev/null
  targets="kv_bench performance_baseline"
  [ "$t" = cy29-after ] && targets="$targets cyclone-tests"
  # shellcheck disable=SC2086
  cmake --build "$HOME/$t/build" -j"$(nproc)" --target $targets > /dev/null
done
mkdir -p "$HOME/cy29/out"
(cd "$HOME/cy29-after/build" && ./cyclone-tests > "$HOME/cy29/out/tests.txt" 2>&1;
 echo "rc=$?" >> "$HOME/cy29/out/tests.txt")
tail -4 "$HOME/cy29/out/tests.txt"
