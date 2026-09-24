#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Chunk-rule comparison at 512K/1M/2M/8M/32M, 3 interleaved reps.
set -u
OUT=$HOME/ra512/out; mkdir -p "$OUT/rule"; rm -f "$OUT/rule/DONE"
BIN=$HOME/cyclone-ra512-exp2/build/kv_bench
SZ=(--block-size 524288 --block-size 1048576 --block-size 2097152
    --block-size 8388608 --block-size 33554432
    --seconds 1 --threads 1 --skip-multiprocess)
for rep in 1 2 3; do
  # uniform 64K
  RA_HEAD=999999999999 "$HOME/ra512/run1.sh" "rule/u64-$rep" "$BIN" "${SZ[@]}"
  # hybrid as committed: 64K over the first 1 MiB, then 512K
  "$HOME/ra512/run1.sh" "rule/hyb-$rep" "$BIN" "${SZ[@]}"
  # sandwich: 64K over the first 256K and the last 512K, 512K between
  RA_HEAD=262144 RA_TAIL=524288 "$HOME/ra512/run1.sh" "rule/sw-$rep" "$BIN" "${SZ[@]}"
  # uniform 128K
  RA_HEAD=999999999999 RA_SMALL=131072 "$HOME/ra512/run1.sh" "rule/u128-$rep" "$BIN" "${SZ[@]}"
done
touch "$OUT/rule/DONE"
