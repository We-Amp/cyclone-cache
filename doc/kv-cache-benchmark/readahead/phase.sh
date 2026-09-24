#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# phase.sh samples start_ms dur_ms: fault + nvme0n1 read stats over a window.
awk -v s=$2 -v e=$(($2+$3)) "\$1>=s && \$1<=e" "$1" | awk "NR==1{split(\$0,a)} {split(\$0,b)} END{dt=(b[1]-a[1])/1000; rc=b[3]-a[3]; sec=b[5]-a[5]; printf \"dt_s %.3f majflt %d reads %d merged %d MiB %.0f MB/s %.0f avg_req_KiB %.1f avg_inflight %.2f us_per_req %.0f\n\", dt, b[2]-a[2], rc, b[4]-a[4], sec*512/1048576, sec*512/1e6/dt, sec*512/1024/rc, (b[6]-a[6])/1000/dt, (b[6]-a[6])*1000/rc}"
