// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Seed-corpus generator for the libFuzzer targets.  Produces REAL, well-formed
// inputs so the fuzzer starts from valid structures and spends its budget on
// meaningful mutations instead of rediscovering the file format.
//
//   * fuzz_document_parse : raw bytes of documents built with DocumentBuilder
//                           (single-fragment + an alternate), plus a directory
//                           bucket of populated DirEntry bit patterns.
//   * fuzz_volume_open    : the parse-sensitive FRONT slice of a real mmap-mode
//                           volume file (VolumeHeader + mmap directory header +
//                           the start of the bucket array), normalized to be
//                           deterministic (see volume_template.hpp).
//   * fuzz_c_api          : hand-encoded op sequences in this target's own
//                           (op, key, value) wire format.
//
// Usage: make_corpus [corpus_root]   (default: fuzz/corpus)
// Idempotent: safe to re-run to refresh the checked-in seeds.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include "core/directory.hpp"
#include "core/document.hpp"
#include "core/volume.hpp"
#include "cyclone/alternate.hpp"
#include "cyclone/config.hpp"
#include "cyclone/key.hpp"
#include "volume_template.hpp"

using namespace cyclone;
namespace fs = std::filesystem;

namespace {

void write_seed(const fs::path &dir, const std::string &name,
                std::span<const std::byte> bytes) {
  fs::create_directories(dir);
  std::ofstream out(dir / name, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char *>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  std::printf("  %-40s %zu bytes\n", (dir / name).string().c_str(),
              bytes.size());
}

std::vector<std::byte> make_content(std::byte fill, size_t n) {
  return std::vector<std::byte>(n, fill);
}

void gen_document_seeds(const fs::path &root) {
  fs::path dir = root / "fuzz_document_parse";

  // A plain single-fragment document with header + content, checksummed.
  {
    CacheKey key("corpus-doc");
    auto header = make_content(std::byte{0x48}, 24);
    auto content = make_content(std::byte{0xDD}, 200);
    auto bytes = DocumentBuilder()
                     .set_key(key)
                     .set_header(std::span<const std::byte>(header))
                     .set_content(std::span<const std::byte>(content))
                     .enable_checksum(true)
                     .build();
    write_seed(dir, "single_frag.bin", bytes);
  }

  // A header-only document (content_len == 0).
  {
    CacheKey key("corpus-hdr-only");
    auto header = make_content(std::byte{0x11}, 40);
    auto bytes = DocumentBuilder()
                     .set_key(key)
                     .set_header(std::span<const std::byte>(header))
                     .build();
    write_seed(dir, "header_only.bin", bytes);
  }

  // An alternate document carrying a next_alternate_offset (chain hop).
  {
    CacheKey key("corpus-alt");
    auto content = make_content(std::byte{0xA5}, 128);
    auto bytes =
        DocumentBuilder()
            .set_key(key)
            .set_content(std::span<const std::byte>(content))
            .set_alternate_id(static_cast<uint8_t>(AlternateId::Brotli))
            .set_next_alternate_offset(4096)
            .set_hit_count(7)
            .build();
    write_seed(dir, "alternate.bin", bytes);
  }

  // A directory bucket: kEntriesPerBucket populated DirEntry patterns.
  {
    std::vector<std::byte> buf(DirEntry::kSize * Directory::kEntriesPerBucket);
    for (size_t i = 0; i < Directory::kEntriesPerBucket; ++i) {
      DirEntry e;
      e.set_offset((i + 1) * 4096);
      e.set_tag(static_cast<uint16_t>(0x123 + i));
      e.set_size(static_cast<uint8_t>(i * 3));
      e.set_big(static_cast<uint8_t>(i % 4));
      e.set_head(i == 0);
      e.set_phase(i % 2 == 0);
      std::memcpy(buf.data() + i * DirEntry::kSize, e._w, DirEntry::kSize);
    }
    write_seed(dir, "dir_bucket.bin", buf);
  }
}

void gen_volume_seeds(const fs::path &root) {
  fs::path dir = root / "fuzz_volume_open";

  // Shared with fuzz_volume_open: a real mmap-mode volume, normalized to be a
  // pure function of the build inputs (wall-clock fields zeroed) -- this is
  // what keeps make_corpus idempotent and the checked-in seed stable.
  std::vector<std::byte> all = cyclone_fuzz::build_volume_template();
  if (all.empty()) {
    std::fprintf(stderr, "make_corpus: volume template build failed\n");
    return;
  }

  // Front slice: VolumeHeader + mmap directory header + start of the versions
  // / bucket arrays -- the num_buckets / magic / version parse surface, which
  // is where the integer-overflow-shaped bugs live.  Kept to a few KB; the
  // fuzzer grows the input to reach the deeper DirEntry array on its own (and
  // the raw DirEntry bit-pattern surface is covered by fuzz_document_parse).
  size_t front = std::min<size_t>(all.size(), 8192);
  write_seed(dir, "front_header.bin",
             std::span<const std::byte>(all.data(), front));
}

void gen_c_api_seeds(const fs::path &root) {
  fs::path dir = root / "fuzz_c_api";

  auto push_str = [](std::vector<std::byte> &v, const std::string &s) {
    v.push_back(static_cast<std::byte>(s.size() & 0xFF));
    for (char c : s) v.push_back(static_cast<std::byte>(c));
  };

  // write "k1"/"hello", then read "k1", then exists "k1".
  {
    std::vector<std::byte> v;
    v.push_back(std::byte{0});  // op 0 = write
    push_str(v, "k1");
    push_str(v, "hello world payload");
    v.push_back(std::byte{1});  // op 1 = read
    push_str(v, "k1");
    v.push_back(std::byte{2});  // op 2 = exists
    push_str(v, "k1");
    write_seed(dir, "write_read_exists.bin", v);
  }

  // write then delete then read (miss path).
  {
    std::vector<std::byte> v;
    v.push_back(std::byte{0});
    push_str(v, "gone");
    push_str(v, "temp");
    v.push_back(std::byte{3});  // delete
    push_str(v, "gone");
    v.push_back(std::byte{1});  // read -> miss
    push_str(v, "gone");
    write_seed(dir, "write_delete_read.bin", v);
  }
}

}  // namespace

int main(int argc, char **argv) {
  fs::path root = (argc > 1) ? fs::path(argv[1]) : fs::path("fuzz/corpus");
  std::printf("Generating seed corpus under %s\n", root.string().c_str());
  gen_document_seeds(root);
  gen_volume_seeds(root);
  gen_c_api_seeds(root);
  std::printf("done\n");
  return 0;
}
