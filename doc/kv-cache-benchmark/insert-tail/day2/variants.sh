#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Issue #35, day 2: how the diagnostic variant trees were made from the
# change (after2.tar is `git archive` of the rebased branch).
#   nofill : kTailFillAboveBytes = UINT64_MAX (pwritev kept, no fill):
#     sed -i 's/kTailFillAboveBytes = uint64_t{64} \* 1024;/kTailFillAboveBytes = UINT64_MAX;/' nofill/src/core/volume.hpp
#   probe  : + a synchronous 4 KiB pread past each fill, after the write
#   aprobe : + an asynchronous 4 KiB posix_fadvise(WILLNEED) there
# Not meant to be run as one file.
exit 0

# ===== mkprobe.sh
# Variant "probe": the change plus one synchronous buffered 4 KiB read of the
# page just past each large document's fill (old data, normally uncached) --
# the device-side footprint of the read ext4 used to do inside the pwrite,
# without the write waiting for it inside write_begin.
set -e
cd ~/i35/trees
rm -rf probe && mkdir probe && tar xf ~/i35-src/after2.tar -C probe --strip-components=1
cp ~/i35/after2/benchmarks/kv_churn.cpp probe/benchmarks/
python3 - <<'EOF'
p='probe/src/core/volume.cpp'
s=open(p).read()
a="""  bool fill_ok = pwrite_parts(_fd, fill_parts, write_offset);
#endif

  // Sync to ensure data is visible to mmap readers (if configured)."""
assert s.count(a)==1
s=s.replace(a,"""  bool fill_ok = pwrite_parts(_fd, fill_parts, write_offset);
  if (fill_ok && slot.fill_end != 0 &&
      slot.fill_end + 4096 <= stripe->offset + stripe->size) {
    alignas(4096) static thread_local std::array<std::byte, 4096> probe{};
    (void)!pread(_fd, probe.data(), probe.size(),
                 static_cast<off_t>(slot.fill_end));
  }
#endif

  // Sync to ensure data is visible to mmap readers (if configured).""")
open(p,'w').write(s)
EOF
echo "fabdcaf+ probe variant (4 KiB pread past each fill)" > probe/COMMIT
nice cmake -S probe -B probe/build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++-20 -DCMAKE_C_COMPILER=clang-20 -DCYCLONE_USE_BUNDLED_SHA256=ON -DCYCLONE_BUILD_EXAMPLES=OFF -DCYCLONE_BUILD_TESTS=OFF > probe-build.log 2>&1
nice cmake --build probe/build -j3 --target kv_churn >> probe-build.log 2>&1
echo built >> probe-build.log

# ===== mkaprobe.sh
# Variant "aprobe": the change plus one ASYNCHRONOUS 4 KiB readahead (posix_fadvise WILLNEED) of the
# page just past each large document's fill (old data, normally uncached) --
# the device-side footprint of the old read-before-write, with no wait.
set -e
cd ~/i35/trees
rm -rf aprobe && mkdir aprobe && tar xf ~/i35-src/after2.tar -C aprobe --strip-components=1
cp ~/i35/after2/benchmarks/kv_churn.cpp aprobe/benchmarks/
python3 - <<'EOF'
p='aprobe/src/core/volume.cpp'
s=open(p).read()
a="""  bool fill_ok = pwrite_parts(_fd, fill_parts, write_offset);
#endif

  // Sync to ensure data is visible to mmap readers (if configured)."""
assert s.count(a)==1
s=s.replace(a,"""  bool fill_ok = pwrite_parts(_fd, fill_parts, write_offset);
  if (fill_ok && slot.fill_end != 0 &&
      slot.fill_end + 4096 <= stripe->offset + stripe->size) {
    (void)posix_fadvise(_fd, static_cast<off_t>(slot.fill_end), 4096,
                        POSIX_FADV_WILLNEED);
  }
#endif

  // Sync to ensure data is visible to mmap readers (if configured).""")
open(p,'w').write(s)
EOF
echo "fabdcaf+ aprobe variant (async 4 KiB readahead past each fill)" > aprobe/COMMIT
nice cmake -S aprobe -B aprobe/build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++-20 -DCMAKE_C_COMPILER=clang-20 -DCYCLONE_USE_BUNDLED_SHA256=ON -DCYCLONE_BUILD_EXAMPLES=OFF -DCYCLONE_BUILD_TESTS=OFF > aprobe-build.log 2>&1
nice cmake --build aprobe/build -j3 --target kv_churn >> aprobe-build.log 2>&1
echo built >> aprobe-build.log
