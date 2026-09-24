#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Two more full round-5 sweeps, main and the fix interleaved, so that with
# the one in bench-driver.sh each tree has three (the first full-after run
# had a slow-device window at 8 MiB).  From here on the sampler also records
# background-job activity in its last column.
set -u
R=$HOME/ra512; OUT=$R/out/final2; rm -f "$OUT/full.DONE"
BASE=$HOME/cyclone-ra512-base/build; AFTER=$HOME/cyclone-ra512-after/build
for rep in b c; do
  echo "$(date +%T) full-base-$rep load: $(cat /proc/loadavg)" >> "$OUT/progress.txt"
  COMMIT=b94540d "$R/run1.sh" "final2/full-base-$rep" "$BASE/kv_bench" --seconds 10
  echo "$(date +%T) full-after-$rep load: $(cat /proc/loadavg)" >> "$OUT/progress.txt"
  COMMIT=dd487fb "$R/run1.sh" "final2/full-after-$rep" "$AFTER/kv_bench" --seconds 10
done
touch "$OUT/full.DONE"
