#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Issue #35: 4 Hz sampler of the block layer's writeback throttling (blk-wbt)
# for the benchmark device ($2, default nvme0n1), run beside a benchmark
# point until the file $1 exists.  Read-only (debugfs, through sudo -n).
# One line per sample: epoch ms, wb_normal, wb_background (the current
# writeback depth limits: they shrink when wbt scales writeback down because
# reads miss their latency target), the summed in-flight writeback count,
# and min_lat_nsec (the target).  Nothing is printed when debugfs is not
# readable.
stop=$1 dev=${2:-nvme0n1}
d=/sys/kernel/debug/block/$dev/rqos/wbt
sudo -n test -r "$d/wb_normal" 2>/dev/null || exit 0
while [ ! -e "$stop" ]; do
  echo "$(date +%s%3N) $(sudo -n sh -c "cat $d/wb_normal $d/wb_background;
    awk '{s += \$3} END {print s}' $d/inflight; cat $d/min_lat_nsec" |
    tr '\n' ' ')"
  sleep 0.25
done
