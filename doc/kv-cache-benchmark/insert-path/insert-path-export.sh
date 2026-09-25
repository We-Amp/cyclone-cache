#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Collect the issue #16 raw data into one flat directory, home paths scrubbed.
set -eu
R=$HOME/kvresults/insert-path; E=$HOME/ip-export
rm -rf "$E"; mkdir -p "$E"
cp "$R/ib-nocg-2m.txt" "$E/insert-path-profile-bench-main.txt"
cp "$R/instr-nocg-2m.txt" "$E/insert-path-instr-bench.txt"
cat "$R/instr-churn-t1.txt" "$R/instr-churn-log.txt" > "$E/insert-path-instr-churn.txt"
cp "$R/instr-churn.jsonl" "$E/insert-path-instr-churn.jsonl"
cp "$R/perf-base-report.txt" "$E/insert-path-perf-main.txt"
cp "$R/instr.patch" "$E/insert-path-instr.patch"
# Pass 1: main vs this change (before small-object coalescing), churn.
A=$R/ab
cp "$A/insert-bench.txt" "$E/insert-path-ab1-insert-bench.txt"
for t in base fix fix-reserve; do
  cp "$A/insert-$t.jsonl" "$E/insert-path-ab1-insert-$t.jsonl"
  cp "$A/churn-$t.txt" "$E/insert-path-churn-$t.txt"
  cp "$A/churn-$t.jsonl" "$E/insert-path-churn-$t.jsonl"
done
for t in base fix; do
  cp "$A/kv-bench-$t.txt" "$E/insert-path-ab1-kv-bench-$t.txt"
  cat "$A"/kv-bench-$t-r*.jsonl > "$E/insert-path-ab1-kv-bench-$t.jsonl"
  cp "$A/perf-baseline-$t.txt" "$E/insert-path-ab1-perf-baseline-$t.txt"
  cp "$A/read-bench-$t.txt" "$E/insert-path-read-bench-$t.txt"
done
cp "$A/progress.txt" "$E/insert-path-ab1-progress.txt"
cp "$A/sysctl.txt" "$E/insert-path-sysctl.txt"
# Pass 2: the final tree.
B=$R/ab2
cp "$B/insert-bench.txt" "$E/insert-path-ab2-insert-bench.txt"
for t in fix fix-reserve; do
  cp "$B/insert-$t.jsonl" "$E/insert-path-ab2-insert-$t.jsonl"
done
for t in base fix; do
  cp "$B/perf-baseline-$t.txt" "$E/insert-path-ab2-perf-baseline-$t.txt"
done
cp "$B/perf-fix-report.txt" "$E/insert-path-perf-final.txt"
cp "$B/perf-fix-run.txt" "$E/insert-path-perf-final-run.txt"
cp "$B/progress.txt" "$E/insert-path-ab2-progress.txt"
# Pass 3: same-day peers.
C=$R/ab3
for t in fix filedir; do
  cp "$C/kv-bench-$t.txt" "$E/insert-path-ab3-kv-bench-$t.txt"
  cat "$C"/kv-bench-$t-r*.jsonl > "$E/insert-path-ab3-kv-bench-$t.jsonl"
done
for st in filedir lmdb; do
  cp "$C/churn-$st.txt" "$E/insert-path-peers-churn-$st.txt"
  cp "$C/churn-$st.jsonl" "$E/insert-path-peers-churn-$st.jsonl"
done
cp "$C/progress.txt" "$E/insert-path-ab3-progress.txt"
# Pass 4: kv_bench size order and the allocator.
D=$R/ab4
cp "$D/kv-bench-order.txt" "$E/insert-path-order.txt"
cat "$D"/kv-bench-*.jsonl > "$E/insert-path-order.jsonl"
cp "$D/progress.txt" "$E/insert-path-ab4-progress.txt"
# Scrub the home directory from every path.
sed -i "s#$HOME#~#g" "$E"/*
grep -il "$(basename "$HOME")" "$E"/* || true
ls "$E" | wc -l
