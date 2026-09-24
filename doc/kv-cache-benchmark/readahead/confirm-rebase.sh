#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Confirmation after rebasing onto main e4c051e (wrap retention on by
# default since #23): main vs the rebased fix, 512 KiB and 2 MiB, all
# phases, 2 interleaved reps, plus the fix tree's test suite.
set -u
R=$HOME/ra512; OUT=$R/out/rebase; mkdir -p "$OUT"; rm -f "$OUT/DONE"
MAIN=$HOME/cyclone-ra512-main2/build; FIX=$HOME/cyclone-ra512-after2/build
(cd "$FIX" && ./cyclone-tests > "$OUT/tests.txt" 2>&1; echo "rc=$?" >> "$OUT/tests.txt")
A=(--block-size 524288 --block-size 2097152 --seconds 5 --threads 1,4)
for rep in 1 2; do
  COMMIT=e4c051e "$R/run1.sh" "rebase/warm-base-$rep" "$MAIN/kv_bench" "${A[@]}"
  COMMIT=f9cc804 "$R/run1.sh" "rebase/warm-after-$rep" "$FIX/kv_bench" "${A[@]}"
done
touch "$OUT/DONE"
