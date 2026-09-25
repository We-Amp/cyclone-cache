#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# macOS performance_baseline A/B for issue #16, interleaved: main vs this
# change, 4 KiB objects.
#   insert-path-macos-perf-baseline.sh MAIN_BUILD_DIR CHANGE_BUILD_DIR OUT
BASE=$1/performance_baseline
FIX=$2/performance_baseline
OUT=$3
: > "$OUT"
for rep in 1 2 3 4 5; do
  for t in base fix; do
    bin=$BASE; [ $t = fix ] && bin=$FIX
    echo "=== $t r$rep load: $(sysctl -n vm.loadavg)" >> "$OUT"
    "$bin" --cache-size 512 --entries 5000 --content-size 4096 2>&1 \
      | grep -E '^(sequential_write|sequential_read|random_read|mixed)' >> "$OUT"
  done
done
echo DONE >> "$OUT"
