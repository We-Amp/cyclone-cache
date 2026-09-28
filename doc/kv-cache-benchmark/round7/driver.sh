#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Round 7 driver, Linux benchmark machine: the round-6 matrix at main, plus
# the write-path options fill_large_document_tail (#40) and write_behind
# (#45).  One benchmark at a time; every point waits for small-reads/idle.sh
# (no runner job, no container job, 1-minute load < 1.0) and runs with
# round6/sampler.sh, insert-tail/diskstats.sh, insert-tail/wbt.sh and a
# runner/container job watcher beside it (the admission-control driver's
# point()).  A point refuses to start with less than 30 GB free.
#
# Usage: driver.sh churn REPS
#        driver.sh sweep REPS
#        driver.sh extra REP NAME:PATTERN:T ...     (churn points)
#        driver.sh extra-sweep REP TAG ...          (sweep points)
#
#   churn : kv_churn / kvchurn, 2 MiB, C = 16 GiB, 4 GiB cgroup, 120 s, for
#           REPS repetitions of: T=4 then T=1, zipf then zipf+scan, the
#           configurations below interleaved.
#   sweep : per repetition, the round-6 sweep (kv_bench --seconds 10, the
#           2 MiB verification-off run, kvpeer LMDB / filedir / RocksDB, the
#           7-size cold sweep for Cyclone and LMDB) and four short kv_bench
#           runs (--seconds 1 --threads 1 --skip-multiprocess, four sizes)
#           for the PUT rows: default, fill, write-behind, both.
#
# Churn configurations (NAME):
#   ret     library defaults (wrap retention on, fill off, write-behind off)
#   fill    --fill-tail on
#   wb      --write-behind on
#   fillwb  --fill-tail on --write-behind on
#   flush   --wrap-retention off
#   lmdb, filedir   the peer harness's kvchurn
#
# Environment: SRC (Cyclone tree, built in $SRC/build), PEER_BUILD (peer
# harness build dir: kvpeer, kvchurn), OUTROOT (output root), DATA (store
# root).
set -u
mode=$1 reps=${2:-3}
SRC=${SRC:?set SRC to the Cyclone tree}
B=$SRC/build
PEER_BUILD=${PEER_BUILD:?set PEER_BUILD to the peer harness build dir}
OUTROOT=${OUTROOT:?set OUTROOT}
DATA=${DATA:?set DATA}
KB=$SRC/doc/kv-cache-benchmark
IDLE=$KB/small-reads/idle.sh
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
    echo "cyclone: $(cat "$SRC/COMMIT" 2>/dev/null)"
    echo "--- kv_bench"; "$B/kv_bench" --print-vectors 2>&1
    echo "--- kv_churn"; "$B/kv_churn" --print-vectors 2>&1
    echo "--- kvchurn"; "$PEER_BUILD/kvchurn" --print-vectors 2>&1
  } > "$OUT/machine.txt" 2>&1
fi

# point TAG CMD...  : wait for idle, sample, run, wipe the store.
point() {
  local tag=$1; shift
  local free
  free=$(df -B1G --output=avail "$DATA" | tail -1 | tr -d ' ')
  if [ "$free" -lt 30 ]; then
    echo "$(date +%T) $tag SKIPPED: only ${free} GB free" >> "$OUT/progress.txt"
    return 1
  fi
  "$IDLE" > /dev/null
  echo "$(date +%T) $tag load: $(cat /proc/loadavg)" >> "$OUT/progress.txt"
  rm -f "$OUT/samples/$tag.stop"
  "$SRC/doc/kv-cache-benchmark/round6/sampler.sh" "$OUT/samples/$tag.stop" \
    > "$OUT/samples/$tag.txt" &
  local sp=$!
  "$KB/insert-tail/diskstats.sh" "$OUT/samples/$tag.stop" \
    > "$OUT/samples/$tag.disk" &
  local dp=$!
  "$KB/insert-tail/wbt.sh" "$OUT/samples/$tag.stop" > "$OUT/samples/$tag.wbt" &
  local wp=$!
  # Every 5 s: note any runner or container job running during the point.
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

run_bench() {  # TAG CMD... ; JSONL and log per tag
  local tag=$1; shift
  "$@" --output "$OUT/$tag.jsonl" --path "$DATA" --drop-caches-cmd "$DROP" \
    > "$OUT/$tag.log" 2>&1
}
bench() { point "$1" run_bench "$@"; }

COLD=(--block-size 65536 --block-size 262144 --block-size 524288
      --block-size 1048576 --block-size 2097152 --block-size 8388608
      --block-size 33554432 --seconds 1 --threads 1 --skip-multiprocess)
SHORT=(--seconds 1 --threads 1 --skip-multiprocess)

sweep_one() {  # TAG (without the -REP suffix) REP
  local t=$1 r=$2
  case $t in
    cyclone-full) bench "$t-$r" "$B/kv_bench" --seconds 10 ;;
    cyclone-noverify) bench "$t-$r" "$B/kv_bench" --block-size 2097152 \
                        --no-mmap-dir --no-verify --seconds 10 ;;
    cyclone-put) bench "$t-$r" "$B/kv_bench" "${SHORT[@]}" ;;
    cyfill-put) bench "$t-$r" "$B/kv_bench" "${SHORT[@]}" --fill-tail on ;;
    cywb-put) bench "$t-$r" "$B/kv_bench" "${SHORT[@]}" --write-behind on ;;
    cyfillwb-put) bench "$t-$r" "$B/kv_bench" "${SHORT[@]}" --fill-tail on \
                    --write-behind on ;;
    lmdb-full|filedir-full|rocksdb-full)
      bench "$t-$r" "$PEER_BUILD/kvpeer" --store "${t%-full}" --seconds 10 ;;
    cyclone-cold) bench "$t-$r" "$B/kv_bench" "${COLD[@]}" ;;
    lmdb-cold) bench "$t-$r" "$PEER_BUILD/kvpeer" --store lmdb "${COLD[@]}" ;;
    *) echo "unknown sweep tag $t" >&2 ;;
  esac
}

churn() {  # TAG STORE PATTERN THREADS [ARGS...]
  local tag=$1 store=$2 pattern=$3 t=$4; shift 4
  point "$tag" env CYCLONE_BUILD="$B" PEER_BUILD="$PEER_BUILD" \
    DATA="$DATA/churn" bash "$KB/churn/run-churn.sh" \
    "$store" "$pattern" "$t" 17179869184 120 "$OUT/churn-$tag.jsonl" \
    "$OUT/churn-$tag.txt" --block-size 2097152 --max-warmup-seconds 1800 "$@"
}

one() {  # NAME PATTERN T REP
  local name=$1 p=$2 t=$3 r=$4
  local tag="$name-$p-t$t-$r"
  case $name in
    ret) churn "$tag" cyclone "$p" "$t" ;;
    fill) churn "$tag" cyclone "$p" "$t" --fill-tail on ;;
    wb) churn "$tag" cyclone "$p" "$t" --write-behind on ;;
    fillwb) churn "$tag" cyclone "$p" "$t" --fill-tail on --write-behind on ;;
    flush) churn "$tag" cyclone "$p" "$t" --wrap-retention off ;;
    lmdb|filedir) churn "$tag" "$name" "$p" "$t" ;;
    *) echo "unknown churn configuration $name" >&2 ;;
  esac
}

case $mode in
  churn)
    for r in $(seq 1 "$reps"); do
      for t in 4 1; do
        for p in zipf zipf+scan; do
          for name in ret fill wb fillwb flush lmdb filedir; do
            one "$name" "$p" "$t" "$r"
          done
        done
      done
    done ;;
  sweep)
    for r in $(seq 1 "$reps"); do
      for t in cyclone-full cyclone-noverify cyclone-put cyfill-put cywb-put \
               cyfillwb-put lmdb-full filedir-full rocksdb-full cyclone-cold \
               lmdb-cold; do
        sweep_one "$t" "$r"
      done
    done ;;
  extra)
    r=$2; shift 2
    for spec in "$@"; do
      IFS=: read -r name p t <<< "$spec"
      one "$name" "$p" "$t" "$r"
    done ;;
  extra-sweep)
    r=$2; shift 2
    for t in "$@"; do sweep_one "$t" "$r"; done ;;
esac
touch "$OUT/DONE-$mode"
