// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Shared builder for the fuzz volume template: a real mmap-mode volume with a
// few entries and an alternate chain, returned as raw file bytes.  Used by
// fuzz_volume_open (as the overlay base for every execution) and make_corpus
// (to slice the checked-in seed).
//
// The bytes are NORMALIZED to be a pure function of the build inputs --
// libFuzzer's repro contract requires that `./fuzz_volume_open <crashfile>`
// re-creates the exact bytes the crashing run saw, and make_corpus must be
// idempotent.  Two wall-clock fields leak into a freshly built volume and are
// zeroed here:
//   - VolumeHeader::creation_time (stamped from the current time on reset;
//     never validated on open),
//   - Document::last_access in every committed document header (stamped from
//     the current time on commit; NOT covered by the payload CRC32, which
//     spans header_data + content only).
// Everything else is deterministic after a clean close (the mmap directory's
// write_lock_owner_pid is released back to 0; no wraps or leases occur while
// building).

#pragma once

#include <unistd.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include "core/document.hpp"
#include "core/volume.hpp"
#include "cyclone/alternate.hpp"
#include "cyclone/config.hpp"
#include "cyclone/key.hpp"

namespace cyclone_fuzz {

// A few MB: large enough to host one stripe (its ~700 KB mmap directory plus
// a data area for the seed documents), small enough to rewrite cheaply.
inline constexpr size_t kVolumeTemplateSize = 3u * 1024 * 1024;
inline constexpr int kVolumeSeedKeys = 4;

inline std::string volume_seed_key(int i) {
  return "vseed-" + std::to_string(i);
}

// pid + counter: unique under parallel fuzz jobs (-jobs=N forks workers).
inline std::string unique_temp_path(const char *tag) {
  static std::atomic<uint64_t> counter{0};
  return (std::filesystem::temp_directory_path() /
          ("cyclone_fuzz_" + std::string(tag) + "_" +
           std::to_string(::getpid()) + "_" +
           std::to_string(counter.fetch_add(1)) + ".dat"))
      .string();
}

// Zero the wall-clock fields listed above, in place.
inline void normalize_volume_bytes(std::vector<std::byte> &bytes) {
  using cyclone::Document;
  using cyclone::VolumeHeader;

  // VolumeHeader::creation_time: u64 after magic(4) + version_major(2) +
  // version_minor(2).
  constexpr size_t kCreationTimeOffset = 8;
  if (bytes.size() >= kCreationTimeOffset + sizeof(uint64_t)) {
    std::memset(bytes.data() + kCreationTimeOffset, 0, sizeof(uint64_t));
  }

  // Document::last_access in every document header.  Scan for the document
  // magic (same technique as the lockfree-races test fixture); the template
  // content bytes (0xC0/0xA1 fills) cannot alias a magic match.
  const uint32_t magic = Document::kMagic;
  if (bytes.size() < Document::kHeaderSize) {
    return;
  }
  for (size_t off = 0; off + Document::kHeaderSize <= bytes.size(); ++off) {
    if (std::memcmp(bytes.data() + off, &magic, sizeof(magic)) != 0) {
      continue;
    }
    std::memset(bytes.data() + off + Document::kLastAccessOffset, 0,
                sizeof(int64_t));
  }
}

// Build the template volume on disk, read it back, normalize, clean up.
// Returns empty on failure (e.g. no temp space).
inline std::vector<std::byte> build_volume_template() {
  using namespace cyclone;

  std::vector<std::byte> bytes;
  std::string path = unique_temp_path("vol_template");

  VolumeConfig cfg;
  cfg.path = path;
  cfg.size = kVolumeTemplateSize;
  cfg.verify_checksum_on_read = true;

  MultiProcessConfig mp;
  mp.enabled = true;  // forces the mmap directory (shared, on-disk) code path
  mp.process_index = 0;
  mp.total_processes = 1;

  Volume volume(cfg, mp);
  if (volume.open().has_value()) {
    for (int i = 0; i < kVolumeSeedKeys; ++i) {
      CacheKey key(volume_seed_key(i));
      std::vector<std::byte> content(64 + i * 16, std::byte{0xC0});
      auto wh = volume.write_sync(key, content.size());
      if (wh.has_value()) {
        (void)wh->write_sync(std::span<const std::byte>(content));
        (void)wh->close_sync();
      }
    }
    // An alternate chain on seed 0 exercises next_alternate_offset hops.
    {
      CacheKey key(volume_seed_key(0));
      std::vector<std::byte> content(96, std::byte{0xA1});
      auto wh =
          volume.write_alternate_sync(key, AlternateId::Brotli, content.size());
      if (wh.has_value()) {
        (void)wh->write_sync(std::span<const std::byte>(content));
        (void)wh->close_sync();
      }
    }
    volume.close();
  }

  std::ifstream f(path, std::ios::binary);
  f.seekg(0, std::ios::end);
  std::streamoff len = f.tellg();
  f.seekg(0, std::ios::beg);
  if (len > 0) {
    bytes.resize(static_cast<size_t>(len));
    f.read(reinterpret_cast<char *>(bytes.data()), len);
  }
  f.close();
  std::remove(path.c_str());

  normalize_volume_bytes(bytes);
  return bytes;
}

}  // namespace cyclone_fuzz
