#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Issue #35, day 2: the order the points ran in.  Each phase was its own
# script, run in turn; they are concatenated here verbatim.  Phase A's
# paced points used an earlier lockstep pacing and were stopped and
# discarded after one run; its closed-loop points are kept.
# Not meant to be run as one file.
exit 0

# ===== phaseA.sh
# Issue #35, hit-p99 investigation: rebased trees, T=4 and T=1 zipf, closed
# loop (5 interleaved runs each at T=4, 3 at T=1) and paced (3 each).
set -u
export BEFORE=$HOME/i35/before2 AFTER=$HOME/i35/after2
export PEER_BUILD=$HOME/cyclone-kv-bench/build OUTROOT=$HOME/i35/res2
export DATA=$HOME/kvdata/i35
D=$AFTER/doc/kv-cache-benchmark/insert-tail/driver.sh
( cd "$AFTER/build" && ./cyclone-tests "[tail_fill]" > "$OUTROOT-tailfill-tests.log" 2>&1 )
for r in 1 2 3 4 5; do
  bash "$D" extra "$r" before:zipf:4 after:zipf:4
done
for r in 1 2 3; do
  bash "$D" extra "$r" before:zipf:4:300 after:zipf:4:300
done
for r in 1 2 3; do
  bash "$D" extra "$r" before:zipf:1 after:zipf:1 before:zipf:1:850 after:zipf:1:850
done
touch "$OUTROOT/PHASEA_DONE"

# ===== phaseA2.sh
# Issue #35, hit-p99 investigation, part 2: paced (Poisson) T=4 and T=1,
# closed-loop T=1, same-day LMDB at T=4 zipf.
set -u
export BEFORE=$HOME/i35/before2 AFTER=$HOME/i35/after2
export PEER_BUILD=$HOME/cyclone-kv-bench/build OUTROOT=$HOME/i35/res2
export DATA=$HOME/kvdata/i35
D=$AFTER/doc/kv-cache-benchmark/insert-tail/driver.sh
for r in 1 2 3; do
  bash "$D" extra "$r" before:zipf:4:250 after:zipf:4:250
done
for r in 1 2 3; do
  bash "$D" extra "$r" before:zipf:1 after:zipf:1 before:zipf:1:800 after:zipf:1:800
done
bash "$D" extra 1 lmdb:zipf:4
bash "$D" extra 2 lmdb:zipf:4
touch "$OUTROOT/PHASEA2_DONE"

# ===== phaseA3.sh
# Issue #35, part 3: is it the fill or the pwritev?  Paced T=4 zipf at 250
# ops/s per thread: main, the change, and the change with the fill off.
set -u
until [ -e "$HOME/i35/res2/PHASEA2_DONE" ]; do sleep 30; done
export BEFORE=$HOME/i35/before2 AFTER=$HOME/i35/after2 TREES=$HOME/i35/trees
export PEER_BUILD=$HOME/cyclone-kv-bench/build OUTROOT=$HOME/i35/res2
export DATA=$HOME/kvdata/i35
D=$AFTER/doc/kv-cache-benchmark/insert-tail/driver.sh
for r in 4 5 6; do
  bash "$D" extra "$r" nofill:zipf:4:250 before:zipf:4:250 after:zipf:4:250
done
touch "$OUTROOT/PHASEA3_DONE"

# ===== phaseA4.sh
# Issue #35, part 4: does a small synchronous read per insert (the device
# footprint of the old read-before-write) bring the hit tail back?
set -u
until [ -e "$HOME/i35/res2/PHASEA3_DONE" ]; do sleep 30; done
export BEFORE=$HOME/i35/before2 AFTER=$HOME/i35/after2 TREES=$HOME/i35/trees
export PEER_BUILD=$HOME/cyclone-kv-bench/build OUTROOT=$HOME/i35/res2
export DATA=$HOME/kvdata/i35
D=$AFTER/doc/kv-cache-benchmark/insert-tail/driver.sh
for r in 7 8 9; do
  bash "$D" extra "$r" probe:zipf:4:250 before:zipf:4:250 after:zipf:4:250
done
touch "$OUTROOT/PHASEA4_DONE"

# ===== phaseB.sh
# Issue #35, phase B: closed-loop probe variant (the trade-off option),
# zipf+scan at T=4 with same-day LMDB, then the kv_bench PUT sweeps with
# same-day LMDB and file-per-block.
set -u
export BEFORE=$HOME/i35/before2 AFTER=$HOME/i35/after2 TREES=$HOME/i35/trees
export PEER_BUILD=$HOME/cyclone-kv-bench/build OUTROOT=$HOME/i35/res2
export DATA=$HOME/kvdata/i35
D=$AFTER/doc/kv-cache-benchmark/insert-tail/driver.sh
for r in 6 7 8; do
  bash "$D" extra "$r" probe:zipf:4 before:zipf:4 after:zipf:4
done
for r in 1 2 3; do
  specs="before:zipf+scan:4 after:zipf+scan:4"
  [ "$r" -le 2 ] && specs="$specs lmdb:zipf+scan:4"
  bash "$D" extra "$r" $specs
done
touch "$OUTROOT/CHURNB_DONE"
bash "$D" put 3
touch "$OUTROOT/PHASEB_DONE"

# ===== phaseC.sh
# Issue #35, phase C: the probe variant on zipf+scan at T=4 and at T=1 zipf,
# interleaved with main and the change.
set -u
until [ -e "$HOME/i35/res2/PHASEB_DONE" ]; do sleep 30; done
export BEFORE=$HOME/i35/before2 AFTER=$HOME/i35/after2 TREES=$HOME/i35/trees
export PEER_BUILD=$HOME/cyclone-kv-bench/build OUTROOT=$HOME/i35/res2
export DATA=$HOME/kvdata/i35
D=$AFTER/doc/kv-cache-benchmark/insert-tail/driver.sh
for r in 4 5 6; do
  bash "$D" extra "$r" probe:zipf+scan:4 before:zipf+scan:4 after:zipf+scan:4
done
for r in 4 5; do
  bash "$D" extra "$r" probe:zipf:1 before:zipf:1 after:zipf:1
done
touch "$OUTROOT/PHASEC_DONE"

# ===== phaseD.sh
# Issue #35, phase D: is it the wait or the read?  The change plus an
# asynchronous 4 KiB readahead past each fill, on zipf+scan at T=4.  Hands
# the lock over for five minutes after phase C first.
set -u
until [ -e "$HOME/i35/res2/PHASEC_DONE" ]; do sleep 30; done
rm -f "$HOME/bench.lock/owner"; rmdir "$HOME/bench.lock"
echo "$(date +%T) lock released after phase C" >> "$HOME/i35/res2/out/progress.txt"
sleep 300
until mkdir "$HOME/bench.lock" 2>/dev/null; do sleep 60; done
echo "issue #35 (tail fill) $(date -Is)" > "$HOME/bench.lock/owner"
echo "$(date +%T) lock re-acquired" >> "$HOME/i35/res2/out/progress.txt"
export BEFORE=$HOME/i35/before2 AFTER=$HOME/i35/after2 TREES=$HOME/i35/trees
export PEER_BUILD=$HOME/cyclone-kv-bench/build OUTROOT=$HOME/i35/res2
export DATA=$HOME/kvdata/i35
D=$AFTER/doc/kv-cache-benchmark/insert-tail/driver.sh
for r in 7 8 9; do
  bash "$D" extra "$r" aprobe:zipf+scan:4 before:zipf+scan:4 after:zipf+scan:4
done
touch "$OUTROOT/PHASED_DONE"
