// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// libFuzzer target: volume-file deserialization gauntlet (the read gauntlet
// from disk).
//
// Attack surface: everything Volume::open() and the subsequent read path parse
// out of a cache file with no prior trust -- the 64-byte VolumeHeader, the
// mmap directory header (magic / version / num_buckets, whose product sizes an
// index and is a classic integer-overflow target), the per-bucket DirEntry
// array, and the document headers a probe hop chases via map_document().
//
// Strategy: a valid mmap-mode volume is built ONCE at startup (a real volume
// with a handful of entries + an alternate chain) and kept in memory as a
// template (see fuzz/volume_template.hpp -- the bytes are normalized to be a
// pure function of the build inputs, so a crash artifact reproduces).  Each
// fuzz input is overlaid onto the FRONT of a fresh copy of that template, so
// the fuzzer starts from a structurally-valid file and mutates the
// parse-sensitive front (header + directory) while the file stays full size.
// A grown input reaches deeper (into the DirEntry array and the first
// documents).  We then open the (corrupted) file and drive reads of the known
// seed keys.
//
// Invariant: open() must return a clean status (ok / Corrupted / error) and
// every read must yield a hit / clean miss / Corrupted -- never a crash, never
// UB (ASan/UBSan-enforced), never a served span outside the mapped file.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include "core/volume.hpp"
#include "cyclone/alternate.hpp"
#include "cyclone/config.hpp"
#include "cyclone/key.hpp"
#include "volume_template.hpp"

using namespace cyclone;

namespace {

const std::vector<std::byte> &volume_template() {
  static const std::vector<std::byte> tmpl =
      cyclone_fuzz::build_volume_template();
  return tmpl;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  const std::vector<std::byte> &tmpl = volume_template();
  if (tmpl.empty()) {
    return 0;  // template build failed (e.g. no temp space) -- nothing to do
  }

  // Copy the template and overlay the fuzz bytes onto its front.
  std::vector<std::byte> file = tmpl;
  size_t overlay = std::min(size, file.size());
  if (overlay > 0) {
    std::memcpy(file.data(), data, overlay);
  }

  std::string path = cyclone_fuzz::unique_temp_path("vol_open");
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.good()) {
      return 0;
    }
    out.write(reinterpret_cast<const char *>(file.data()),
              static_cast<std::streamsize>(file.size()));
  }

  VolumeConfig cfg;
  cfg.path = path;
  cfg.size = file.size();
  cfg.verify_checksum_on_read = true;

  MultiProcessConfig mp;
  mp.enabled = true;
  mp.process_index = 0;
  mp.total_processes = 1;

  Volume volume(cfg, mp);
  auto opened = volume.open();
  if (opened.has_value()) {
    DefaultStorageSelector selector;
    AlternateSelectionContext ctx;
    for (int i = 0; i < cyclone_fuzz::kVolumeSeedKeys; ++i) {
      CacheKey key(cyclone_fuzz::volume_seed_key(i));
      if (auto rh = volume.read_sync(key); rh.has_value()) {
        auto content = rh->content();
        // Touch every byte so ASan flags any span that escaped the mapping.
        volatile std::byte sink{};
        for (std::byte b : content) {
          sink = b;
        }
        (void)sink;
      }
      (void)volume.exists_sync(key);
      (void)volume.list_alternates_sync(key);
      (void)volume.read_alternate_sync(key, selector, ctx);
    }
    volume.close();
  }

  std::remove(path.c_str());
  return 0;
}
