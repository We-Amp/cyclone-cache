#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Issue #35: 1 Hz device sampler, run beside a benchmark point until the file
# $1 exists.  One line per second: epoch ms, then the /proc/diskstats fields
# of the benchmark device ($2, default nvme0n1) after its name:
#   reads reads_merged sectors_read read_ms writes writes_merged
#   sectors_written write_ms in_flight io_ms weighted_io_ms [discard/flush...]
# (cumulative; diskstats_summary.py takes deltas over the measured window).
stop=$1 dev=${2:-nvme0n1}
while [ ! -e "$stop" ]; do
  echo "$(date +%s%3N) $(awk -v d="$dev" '$3 == d {for (i = 4; i <= NF; i++)
    printf "%s ", $i}' /proc/diskstats)"
  sleep 1
done
