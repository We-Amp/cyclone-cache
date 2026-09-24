#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Chunk-size sweep, part 2: smaller chunks, and the large block sizes.
set -u
OUT=$HOME/ra512/out
rm -f "$OUT/sweep-chunk2.DONE"
one() {  # bs chunk
  local n=$(( (4294967296 / $1) < 4096 ? (4294967296 / $1) : 4096 ))
  RA_CHUNK=$2 INSTR_N=$(( n + 2 )) "$HOME/ra512/run1.sh" "chunk2-$1-$2" \
    "$HOME/cyclone-ra512-exp/build/kv_bench" --block-size "$1" \
    --seconds 1 --threads 1 --skip-multiprocess
}
for ch in 32768 16384 65536; do one 524288 $ch; done
for ch in 524288 65536 32768; do one 2097152 $ch; done
for ch in 524288 65536 32768; do one 8388608 $ch; done
for ch in 524288 65536 32768; do one 33554432 $ch; done
touch "$OUT/sweep-chunk2.DONE"
