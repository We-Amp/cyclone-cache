#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# wait until load1 < 1.0 and no GitHub runner job is running
while true; do
  l=$(cut -d" " -f1 /proc/loadavg)
  if pgrep -x Runner.Worker >/dev/null; then sleep 20; continue; fi
  if awk -v l="$l" "BEGIN{exit !(l<1.0)}"; then break; fi
  sleep 15
done
echo "idle: $(cat /proc/loadavg)"
