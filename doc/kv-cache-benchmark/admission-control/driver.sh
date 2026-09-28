#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Issue #43 (writer admission control) churn driver, Linux benchmark machine.
# The insert-tail driver (../insert-tail/driver.sh) with four Cyclone
# configurations instead of two.  One benchmark at a time; every point waits
# for small-reads/idle.sh (no runner job, no container job, 1-minute load
# < 1.0) and runs with round6/sampler.sh, insert-tail/diskstats.sh,
# insert-tail/wbt.sh and a runner/container job watcher beside it.
#
# Usage: driver.sh matrix REPS
#        driver.sh extra REP NAME:PATTERN:T ...
#
#   matrix : kv_churn, 2 MiB, C = 16 GiB, 4 GiB cgroup, 120 s, wrap
#            retention on, for REPS repetitions of: T=4 then T=1, zipf then
#            zipf+scan, the configurations main, fill, fillwb and wb
#            interleaved, and LMDB (kvchurn) at T=4.
#   extra  : single points as repetition REP.
#
# Configurations (NAME):
#   main    main's kv_churn ($MAIN/build), library defaults (fill off).
#   fill    this change's kv_churn ($WA/build), --fill-tail on.
#   fillwb  the same plus --write-behind on.
#   wb      --write-behind on alone (fill off).
#   lmdb    the peer harness's kvchurn ($PEER_BUILD).
#   other   $WA/build with --fill-tail on and the arguments in $ARGS_<NAME>:
#           the pilot variants, each a tree of its own (WA), built from this
#           change with a patch in this directory applied
#           (admission-control-*.patch; see doc/kv-cache-benchmark.md,
#           "Writer admission").
#
# Environment: MAIN, WA (trees, each built in ./build), PEER_BUILD, OUTROOT
# (output root), DATA (store root).
set -u
mode=$1 reps=${2:-3}
MAIN=${MAIN:?set MAIN to main\'s tree}
WA=${WA:?set WA to the tree with the change}
PEER_BUILD=${PEER_BUILD:?set PEER_BUILD to the peer harness build dir}
OUTROOT=${OUTROOT:?set OUTROOT}
DATA=${DATA:?set DATA}
R6=$WA/doc/kv-cache-benchmark/round6
IT=$WA/doc/kv-cache-benchmark/insert-tail
IDLE=$WA/doc/kv-cache-benchmark/small-reads/idle.sh
OUT=$OUTROOT/out
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
    echo "main: $(cat "$MAIN/COMMIT" 2>/dev/null)"
    echo "change: $(cat "$WA/COMMIT" 2>/dev/null)"
    echo "--- kv_churn"; "$WA/build/kv_churn" --print-vectors
    echo "--- kvchurn"; "$PEER_BUILD/kvchurn" --print-vectors
  } > "$OUT/machine.txt" 2>&1
fi

# point TAG CMD...  : wait for idle, sample, run, wipe the store.
point() {
  local tag=$1; shift
  "$IDLE" > /dev/null
  echo "$(date +%T) $tag load: $(cat /proc/loadavg)" >> "$OUT/progress.txt"
  rm -f "$OUT/samples/$tag.stop"
  "$R6/sampler.sh" "$OUT/samples/$tag.stop" > "$OUT/samples/$tag.txt" &
  local sp=$!
  "$IT/diskstats.sh" "$OUT/samples/$tag.stop" > "$OUT/samples/$tag.disk" &
  local dp=$!
  "$IT/wbt.sh" "$OUT/samples/$tag.stop" > "$OUT/samples/$tag.wbt" &
  local wp=$!
  # Every 5 s: note any runner or container job that started mid-point
  # (round6/sampler.sh sees only Runner.Worker).
  ( while [ ! -e "$OUT/samples/$tag.stop" ]; do
      if pgrep -x Runner.Worker >/dev/null || pgrep -x docker-init >/dev/null
      then date +%s; fi
      sleep 5
    done ) > "$OUT/samples/$tag.jobs" &
  local jp=$!
  "$@"
  local rc=$?
  touch "$OUT/samples/$tag.stop"; wait "$sp" "$dp" "$wp" "$jp"
  rm -f "$OUT/samples/$tag.stop"
  rm -rf "${DATA:?}"/*
  echo "$(date +%T) $tag rc=$rc load: $(cat /proc/loadavg)" >> "$OUT/progress.txt"
}

churn() {  # TAG STORE PATTERN THREADS BUILD [ARGS...]
  local tag=$1 store=$2 pattern=$3 t=$4 cb=$5; shift 5
  point "$tag" env CYCLONE_BUILD="$cb" PEER_BUILD="$PEER_BUILD" \
    DATA="$DATA/churn" bash "$WA/doc/kv-cache-benchmark/churn/run-churn.sh" \
    "$store" "$pattern" "$t" 17179869184 120 "$OUT/churn-$tag.jsonl" \
    "$OUT/churn-$tag.txt" --block-size 2097152 --max-warmup-seconds 1800 "$@"
}

one() {  # NAME PATTERN T REP
  local name=$1 p=$2 t=$3 r=$4
  local tag="$name-$p-t$t-$r"
  case $name in
    main) churn "$tag" cyclone "$p" "$t" "$MAIN/build" --wrap-retention on ;;
    fill) churn "$tag" cyclone "$p" "$t" "$WA/build" --wrap-retention on \
            --fill-tail on ;;
    fillwb) churn "$tag" cyclone "$p" "$t" "$WA/build" --wrap-retention on \
              --fill-tail on --write-behind on ;;
    wb) churn "$tag" cyclone "$p" "$t" "$WA/build" --wrap-retention on \
          --write-behind on ;;
    lmdb) churn "$tag" lmdb "$p" "$t" "$WA/build" ;;
    *)
      local v="ARGS_$name"
      # shellcheck disable=SC2086  # the variant's arguments split on purpose
      churn "$tag" cyclone "$p" "$t" "$WA/build" --wrap-retention on \
        --fill-tail on ${!v:-} ;;
  esac
}

case $mode in
  matrix)
    for r in $(seq 1 "$reps"); do
      for t in 4 1; do
        for p in zipf zipf+scan; do
          for name in main fill fillwb wb; do one "$name" "$p" "$t" "$r"; done
          if [ "$t" = 4 ]; then one lmdb "$p" "$t" "$r"; fi
        done
      done
    done ;;
  extra)
    r=$2; shift 2
    for spec in "$@"; do
      IFS=: read -r name p t <<< "$spec"
      one "$name" "$p" "$t" "$r"
    done ;;
esac
touch "$OUT/DONE-$mode"
