#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Small cold reads (issue #29), Linux benchmark machine.  One benchmark at a
# time; every point waits for idle.sh (no GitHub runner job, no container
# job, 1-minute load < 1.0) and runs with sampler.sh beside it, both from
# this directory.  (The ablation and MADV_NORMAL runs, and the first cold
# sweep, used ../round6/idle.sh and ../round6/sampler.sh, which do not see
# container jobs.)
#
#   base  = origin/main          ($BASE/build)
#   after = the change           ($AFTER/build)
#   lmdb  = peer harness kvpeer, same day
#
# Usage: driver.sh MODE [REPS]
#   cold    : 8-size cold sweep (64 KiB..32 MiB, first touch + restart),
#             base / after / LMDB interleaved, REPS repetitions
#   warm    : 64 KiB, 256 KiB, 512 KiB, all phases incl. the 4-process
#             phase, 5 s points, base / after interleaved
#   ablate  : after with the sequential window off, and window sizes
#             128 KiB .. 1 MiB ($WINDOWS), at 64 KiB..1 MiB cold
#   normal  : the mapping-wide MADV_NORMAL experiment ($NORMAL/build: base
#             with the open-time MADV_RANDOM removed), cold + warm 64 KiB
#   resident: 64 KiB..1 MiB without dropping caches, base / after /
#             after with the window off, interleaved: first touch and
#             restart then CRC-verify resident documents, the one place the
#             new hints cost a call
#   baseline: performance_baseline (4 KB objects), base / after interleaved
#
# Environment: BASE, AFTER, NORMAL (trees), PEER_BUILD, OUT, DATA, and
# FIRST (cold only: the first repetition number, default 1).
set -u
mode=$1 reps=${2:-3}
BASE=${BASE:-$HOME/cy29-base}
AFTER=${AFTER:-$HOME/cy29-after}
NORMAL=${NORMAL:-$HOME/cy29-normal}
PEER_BUILD=${PEER_BUILD:-$HOME/cyclone-kv-bench/build}
OUT=${OUT:-$HOME/cy29/out}
DATA=${DATA:-$HOME/kvdata/cy29}
HERE=${HERE:-$AFTER/doc/kv-cache-benchmark/small-reads}
DROP='sync; echo 3 | sudo -n tee /proc/sys/vm/drop_caches >/dev/null'
mkdir -p "$OUT/samples" "$DATA"

if [ ! -e "$OUT/machine.txt" ]; then
  {
    date -Is; uname -r
    cat /sys/block/nvme0n1/device/model /sys/block/nvme0n1/device/firmware_rev
    for q in read_ahead_kb max_sectors_kb scheduler; do
      echo "$q: $(cat /sys/block/nvme0n1/queue/$q)"; done
    sysctl vm.dirty_ratio vm.dirty_background_ratio vm.dirty_bytes \
      vm.dirty_background_bytes
    clang++-20 --version | head -1
    for t in "$BASE" "$AFTER" "$NORMAL"; do
      echo "$(basename "$t"): $(git -C "$t" rev-parse --short HEAD 2>/dev/null)"
    done
  } > "$OUT/machine.txt" 2>&1
fi

# point TAG CMD...  : wait for idle, sample, run, wipe the store.
point() {
  local tag=$1; shift
  "$HERE/idle.sh" > /dev/null
  echo "$(date +%T) $tag load: $(cat /proc/loadavg)" >> "$OUT/progress.txt"
  rm -f "$OUT/samples/$tag.stop"
  "$HERE/sampler.sh" "$OUT/samples/$tag.stop" > "$OUT/samples/$tag.txt" &
  local sp=$!
  "$@"
  local rc=$?
  touch "$OUT/samples/$tag.stop"; wait "$sp"; rm -f "$OUT/samples/$tag.stop"
  rm -rf "${DATA:?}"/*
  echo "$(date +%T) $tag rc=$rc load: $(cat /proc/loadavg)" >> "$OUT/progress.txt"
}
run_bench() {  # TAG CMD...
  local tag=$1; shift
  "$@" --output "$OUT/$tag.jsonl" --path "$DATA" --drop-caches-cmd "$DROP" \
    > "$OUT/$tag.log" 2>&1
}
bench() { point "$1" run_bench "$@"; }
run_nodrop() {  # TAG CMD...
  local tag=$1; shift
  "$@" --output "$OUT/$tag.jsonl" --path "$DATA" > "$OUT/$tag.log" 2>&1
}

COLD=(--block-size 65536 --block-size 131072 --block-size 262144
      --block-size 524288 --block-size 1048576 --block-size 2097152
      --block-size 8388608 --block-size 33554432
      --seconds 1 --threads 1 --skip-multiprocess)
WARM=(--block-size 65536 --block-size 262144 --block-size 524288
      --seconds 5 --threads 1,4)
SMALLCOLD=(--block-size 65536 --block-size 131072 --block-size 262144
           --block-size 524288 --block-size 1048576
           --seconds 1 --threads 1 --skip-multiprocess)

case $mode in
  cold)
    # FIRST (default 1) lets a later invocation add repetitions.
    for r in $(seq "${FIRST:-1}" "$reps"); do
      bench "cold-base-$r" "$BASE/build/kv_bench" "${COLD[@]}"
      bench "cold-after-$r" "$AFTER/build/kv_bench" "${COLD[@]}"
      bench "cold-lmdb-$r" "$PEER_BUILD/kvpeer" --store lmdb "${COLD[@]}"
    done ;;
  warm)
    for r in $(seq 1 "$reps"); do
      bench "warm-base-$r" "$BASE/build/kv_bench" "${WARM[@]}"
      bench "warm-after-$r" "$AFTER/build/kv_bench" "${WARM[@]}"
    done ;;
  ablate)
    for r in $(seq 1 "$reps"); do
      for w in ${WINDOWS:-0 131072 262144 524288 1048576}; do
        bench "ablate-w$w-$r" "$AFTER/build/kv_bench" "${SMALLCOLD[@]}" \
          --sequential-readahead-bytes "$w"
      done
    done ;;
  normal)
    for r in $(seq 1 "$reps"); do
      bench "normal-cold-$r" "$NORMAL/build/kv_bench" "${SMALLCOLD[@]}"
      bench "normal-warm-$r" "$NORMAL/build/kv_bench" --block-size 65536 \
        --seconds 5 --threads 1,4
    done ;;
  resident)
    # afterw0: the window off, so every CRC-pending read takes the
    # per-document hint (the path a non-sequential first read takes).
    for r in $(seq 1 "$reps"); do
      for t in base after afterw0; do
        tree=$BASE extra=()
        [ "$t" != base ] && tree=$AFTER
        [ "$t" = afterw0 ] && extra=(--sequential-readahead-bytes 0)
        point "resident-$t-$r" run_nodrop "resident-$t-$r" \
          "$tree/build/kv_bench" "${SMALLCOLD[@]}" "${extra[@]}"
      done
    done ;;
  baseline)
    for r in $(seq 1 "$reps"); do
      for t in base after; do
        tree=$BASE; [ "$t" = after ] && tree=$AFTER
        point "perfbase-$t-$r" sh -c "'$tree/build/performance_baseline' \
          --cache-size 512 --entries 5000 --content-size 4096 \
          > '$OUT/perfbase-$t-$r.txt' 2>&1"
      done
    done ;;
  *) echo "unknown mode $mode" >&2; exit 2 ;;
esac
touch "$OUT/DONE-$mode"
