#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# ip-run.sh NAME CMD... : wait idle, run CMD, log to ~/kvresults/insert-path/NAME.txt, then NAME.DONE
name=$1; shift
out=~/kvresults/insert-path
rm -f "$out/$name.DONE"
~/insert-path-idle.sh > "$out/$name.txt"
echo "=== $(date -Is) $*" >> "$out/$name.txt"
"$@" >> "$out/$name.txt" 2>&1
echo "rc=$? load-after: $(cat /proc/loadavg)" >> "$out/$name.txt"
rm -rf ~/kvdata/insert-path/*
touch "$out/$name.DONE"
