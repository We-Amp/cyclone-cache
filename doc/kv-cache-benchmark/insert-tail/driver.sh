#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Issue #35 (tail fill) before/after driver, Linux benchmark machine.  One
# benchmark at a time; every point waits for round6/idle.sh (no runner job,
# 1-minute load < 1.0) and runs with round6/sampler.sh beside it, exactly as
# the round-6 driver does.  Usage: driver.sh churn|put|diag [REPS]
#
#   churn : kv_churn, 2 MiB, zipf, C = 16 GiB, 4 GiB cgroup, 120 s, wrap
#           retention on (the default), T=1 and T=4, before and after
#           interleaved, REPS repetitions; LMDB and file-per-block (kvchurn)
#           at both thread counts in the first PEER_REPS repetitions.
#   put   : kv_bench full sweep (the round-5/6 command; its PUT rows are the
#           ones compared), before and after interleaved, REPS repetitions;
#           kvpeer LMDB and file-per-block full sweeps in the first
#           PEER_REPS repetitions.
#   diag  : round6/diag.sh (60 s off-CPU trace of the T=1 zipf point) for
#           the before and the after tree, retention on and flush mode.
#
# Environment: BEFORE / AFTER (Cyclone trees, each built in ./build),
# PEER_BUILD (peer harness build dir: kvpeer, kvchurn), OUTROOT (output
# root), DATA (store root), PEER_REPS (default 2).
set -u
mode=$1 reps=${2:-3}
BEFORE=${BEFORE:?set BEFORE to the tree without the change}
AFTER=${AFTER:?set AFTER to the tree with the change}
PEER_BUILD=${PEER_BUILD:?set PEER_BUILD to the peer harness build dir}
OUTROOT=${OUTROOT:?set OUTROOT}
DATA=${DATA:?set DATA}
PEER_REPS=${PEER_REPS:-2}
R6=$AFTER/doc/kv-cache-benchmark/round6
OUT=$OUTROOT/out
DROP='sync; echo 3 | sudo -n tee /proc/sys/vm/drop_caches >/dev/null'
mkdir -p "$OUT/samples" "$DATA"
rm -f "$OUT/DONE-$mode"

if [ ! -e "$OUT/machine.txt" ]; then
  {
    date -Is; uname -r
    cat /sys/block/nvme0n1/device/model /sys/block/nvme0n1/device/firmware_rev
    for q in read_ahead_kb max_sectors_kb scheduler; do
      echo "$q: $(cat /sys/block/nvme0n1/queue/$q)"; done
    sysctl vm.dirty_ratio vm.dirty_background_ratio vm.dirty_bytes \
      vm.dirty_background_bytes vm.dirty_expire_centisecs \
      vm.dirty_writeback_centisecs
    echo "filesystem: $(findmnt -no FSTYPE -T "$DATA")"
    clang++-20 --version | head -1
    echo "before: $(git -C "$BEFORE" rev-parse --short HEAD)"
    echo "after: $(git -C "$AFTER" rev-parse --short HEAD)"
    "$AFTER/build/kv_bench" --print-vectors
    echo "--- kv_churn"; "$AFTER/build/kv_churn" --print-vectors
    echo "--- kvchurn"; "$PEER_BUILD/kvchurn" --print-vectors
  } > "$OUT/machine.txt" 2>&1
fi

# point TAG CMD...  : wait for idle, sample, run, wipe the store.
point() {
  local tag=$1; shift
  "$R6/idle.sh" > /dev/null
  echo "$(date +%T) $tag load: $(cat /proc/loadavg)" >> "$OUT/progress.txt"
  rm -f "$OUT/samples/$tag.stop"
  "$R6/sampler.sh" "$OUT/samples/$tag.stop" > "$OUT/samples/$tag.txt" &
  local sp=$!
  "$@"
  local rc=$?
  touch "$OUT/samples/$tag.stop"; wait "$sp"; rm -f "$OUT/samples/$tag.stop"
  rm -rf "${DATA:?}"/*
  echo "$(date +%T) $tag rc=$rc load: $(cat /proc/loadavg)" >> "$OUT/progress.txt"
}

run_bench() {  # TAG CMD... ; JSONL and log per tag
  local tag=$1; shift
  "$@" --output "$OUT/$tag.jsonl" --path "$DATA" --drop-caches-cmd "$DROP" \
    > "$OUT/$tag.log" 2>&1
}
bench() { point "$1" run_bench "$@"; }

churn() {  # TAG STORE PATTERN THREADS CYCLONE_BUILD [ARGS...]
  local tag=$1 store=$2 pattern=$3 t=$4 cb=$5; shift 5
  point "$tag" env CYCLONE_BUILD="$cb" PEER_BUILD="$PEER_BUILD" \
    DATA="$DATA/churn" bash "$AFTER/doc/kv-cache-benchmark/churn/run-churn.sh" \
    "$store" "$pattern" "$t" 17179869184 120 "$OUT/churn-$tag.jsonl" \
    "$OUT/churn-$tag.txt" --block-size 2097152 --max-warmup-seconds 1800 "$@"
}

case $mode in
  churn)
    for r in $(seq 1 "$reps"); do
      for t in 1 4; do
        churn "before-zipf-t$t-$r" cyclone zipf "$t" "$BEFORE/build" \
          --wrap-retention on
        churn "after-zipf-t$t-$r" cyclone zipf "$t" "$AFTER/build" \
          --wrap-retention on
        if [ "$r" -le "$PEER_REPS" ]; then
          churn "lmdb-zipf-t$t-$r" lmdb zipf "$t" "$AFTER/build"
          churn "filedir-zipf-t$t-$r" filedir zipf "$t" "$AFTER/build"
        fi
      done
    done ;;
  put)
    for r in $(seq 1 "$reps"); do
      bench "before-full-$r" "$BEFORE/build/kv_bench" --seconds 10
      bench "after-full-$r" "$AFTER/build/kv_bench" --seconds 10
      if [ "$r" -le "$PEER_REPS" ]; then
        bench "lmdb-full-$r" "$PEER_BUILD/kvpeer" --store lmdb --seconds 10
        bench "filedir-full-$r" "$PEER_BUILD/kvpeer" --store filedir \
          --seconds 10
      fi
    done ;;
  diag)
    # round6/diag.sh from each tree, against that tree's build; the output
    # lands in $OUTROOT/out/diag.  The after tree's offcpu.py also files a
    # pwritev under "via pwrite".
    for tree in before after; do
      src=$BEFORE
      [ "$tree" = after ] && src=$AFTER
      for mode_arg in on off; do
        name=ret
        [ "$mode_arg" = off ] && name=flush
        SRC=$src R6=$OUTROOT DATA=$DATA PEER_BUILD=$PEER_BUILD \
          bash "$src/doc/kv-cache-benchmark/round6/diag.sh" \
          "$tree-$name" cyclone --wrap-retention "$mode_arg"
      done
    done ;;
esac
touch "$OUT/DONE-$mode"
