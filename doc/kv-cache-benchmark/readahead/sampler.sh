#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Samples major faults and nvme0n1 read counters every 100 ms until $1 exists.
# columns: t_ms pgmajfault reads_completed reads_merged sectors_read ms_reading
#          inflight nvme_temp_mC background_jobs (processes matching
#          BACKGROUND_JOB_PATTERN; empty when unset)
TEMP=$(grep -l '^nvme$' /sys/class/hwmon/hwmon*/name 2>/dev/null | head -n 1)
TEMP=${TEMP%/name}/temp1_input
while [ ! -e "$1" ]; do
  t=$(date +%s%3N)
  mf=$(awk "/^pgmajfault /{print \$2}" /proc/vmstat)
  ds=$(awk "\$3==\"nvme0n1\"{print \$4, \$5, \$6, \$7, \$12}" /proc/diskstats)
  echo "$t $mf $ds $(cat "$TEMP" 2>/dev/null) $([ -n "${BACKGROUND_JOB_PATTERN:-}" ] && pgrep -c -f "$BACKGROUND_JOB_PATTERN")"
  sleep 0.1
done
