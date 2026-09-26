#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Small cold reads (issue #29): block until no GitHub Actions runner job and
# no container job is running and the 1-minute load is below 1.0.  The
# round-6 idle.sh only watched Runner.Worker; on this day a containerised
# build (docker-init, ~12 cores busy) started mid-run twice without one.
# Prints the load it started at.
while true; do
  l=$(cut -d" " -f1 /proc/loadavg)
  if pgrep -x Runner.Worker >/dev/null || pgrep -x docker-init >/dev/null; then
    sleep 20; continue
  fi
  if awk -v l="$l" 'BEGIN{exit !(l<1.0)}'; then break; fi
  sleep 15
done
echo "idle: $(cat /proc/loadavg)"
