#!/usr/bin/env python3

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Small cold reads (issue #29): performance_baseline and
concurrent_read_bench, base vs after.

  summarize_tools.py DIR

Reads DIR/small-reads-[macos-]<kind>-<variant>-<rep>.txt, where kind is
pb or perfbase (performance_baseline), crb (concurrent_read_bench, 512 B
objects) or crb64k (64 KiB objects), and prints for each metric the median
per variant, the per-run values, and after / base.  The kv_bench JSONL files
go through summarize.py.
"""
import glob
import os
import re
import statistics
import sys
from collections import defaultdict

NAME = re.compile(r"^small-reads-(?:macos-)?(pb|perfbase|crb|crb64k)-"
                  r"(base|after)-(\d+)\.txt$")

d = sys.argv[1]
vals = defaultdict(lambda: defaultdict(list))
for p in sorted(glob.glob(os.path.join(d, "*.txt"))):
    m = NAME.match(os.path.basename(p))
    if not m:
        continue
    kind, var = m.group(1), m.group(2)
    kind = "pb" if kind == "perfbase" else kind
    for line in open(p):
        f = line.split()
        if kind == "pb" and len(f) == 6 and f[1].isdigit():
            vals[(kind, f[0])][var].append(float(f[1]))
        elif kind != "pb" and len(f) >= 3 and f[0].isdigit() \
                and f[1].isdigit():
            vals[(kind, f"T={f[0]}")][var].append(float(f[1]))

for key in vals:
    b = vals[key]["base"]
    a = vals[key]["after"]
    if not a or not b:
        continue
    mb, ma = statistics.median(b), statistics.median(a)
    print(f"{key[0]:<7} {key[1]:<28} base {mb:>12,.0f} {[int(x) for x in b]}"
          f"  after {ma:>12,.0f} {[int(x) for x in a]}  after/base "
          f"{ma / mb:.3f}")
