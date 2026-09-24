#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Chunk-size sweep of the Linux MADV_WILLNEED hint (throwaway RA_CHUNK env).
set -u
OUT=$HOME/ra512/out
rm -f "$OUT/sweep-chunk.DONE"
for bs in 524288 2097152; do
  for ch in 524288 131072 262144 65536; do
    RA_CHUNK=$ch INSTR_N=$(( bs == 524288 ? 4098 : 2050 )) \
      "$HOME/ra512/run1.sh" "chunk-$bs-$ch" \
      "$HOME/cyclone-ra512-exp/build/kv_bench" --block-size "$bs" \
      --seconds 1 --threads 1 --skip-multiprocess
  done
done
touch "$OUT/sweep-chunk.DONE"
