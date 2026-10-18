// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Read-path document integrity.  Each case writes a document through the
// public API, then changes its bytes in the volume file behind the cache's
// back (the way a torn write, a late write or media damage would land them)
// and checks that the read path never serves the changed document as valid.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/document.hpp"
#include "core/volume.hpp"
#include "cyclone/alternate.hpp"
#include "cyclone/cache.hpp"
#include "cyclone/config.hpp"
#include "cyclone/key.hpp"
#include "support/temp_cache.hpp"

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

using namespace cyclone;

#ifndef _WIN32
namespace {

constexpr size_t kVolBytes = size_t{4} * 1024 * 1024;

// CRC-32C written out independently of the library (see crc32c.hpp for the
// convention), so the checksums planted below do not come from the code
// under test.
const std::array<uint32_t, 256> &crc_table() {
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t n = 0; n < 256; ++n) {
      uint32_t c = n;
      for (int k = 0; k < 8; ++k) {
        c = ((c & 1U) != 0U) ? (0x82f63b78U ^ (c >> 1)) : (c >> 1);
      }
      t[n] = c;
    }
    return t;
  }();
  return table;
}

uint32_t crc_update(uint32_t state, std::span<const std::byte> data) {
  const auto &t = crc_table();
  for (std::byte b : data) {
    state = t[(state ^ static_cast<uint8_t>(b)) & 0xFFU] ^ (state >> 8);
  }
  return state;
}

uint32_t oracle_crc32c(std::span<const std::byte> data) {
  return crc_update(0xFFFFFFFFU, data) ^ 0xFFFFFFFFU;
}

// Rewrite the last four bytes of `content` so that the CRC-32C of
// `prefix` ++ `content` is exactly zero (standard CRC forcing: the table's
// top bytes are a permutation, so the four table indices are recovered
// backwards from the target register, then the bytes forwards from the
// prefix state).
void force_zero_crc(std::span<const std::byte> prefix,
                    std::vector<std::byte> &content) {
  REQUIRE(content.size() >= 4);
  const auto &t = crc_table();
  std::array<uint8_t, 256> inv{};
  for (uint32_t i = 0; i < 256; ++i) {
    inv[t[i] >> 24] = static_cast<uint8_t>(i);
  }
  std::array<uint8_t, 4> idx{};
  uint32_t reg = 0xFFFFFFFFU;  // register value whose final xor is 0
  for (int k = 3; k >= 0; --k) {
    idx[static_cast<size_t>(k)] = inv[reg >> 24];
    reg = (reg ^ t[idx[static_cast<size_t>(k)]]) << 8;
  }
  uint32_t state =
      crc_update(crc_update(0xFFFFFFFFU, prefix),
                 std::span<const std::byte>(content).first(content.size() - 4));
  for (size_t k = 0; k < 4; ++k) {
    const auto b = static_cast<uint8_t>(idx[k] ^ (state & 0xFFU));
    content[content.size() - 4 + k] = static_cast<std::byte>(b);
    state = t[idx[k]] ^ (state >> 8);
  }
  REQUIRE((crc_update(crc_update(0xFFFFFFFFU, prefix), content) ^
           0xFFFFFFFFU) == 0);
}

// The canonical fixed header the document checksum starts from, for a
// document written through the plain write path under `key` with no header
// bytes: the wire header with the fields written in place after the store
// (and the checksum itself) set to zero.
std::vector<std::byte> canonical_header(const CacheKey &key,
                                        std::span<const std::byte> content) {
  auto head = DocumentBuilder()
                  .set_key(key)
                  .set_type(Document::Type::SingleFrag)
                  .enable_checksum(false)
                  .build_head(content);
  REQUIRE(head.size() == Document::kHeaderSize);
  const std::pair<size_t, size_t> in_place[] = {
      {Document::kFillSerialOffset, 4},       {Document::kWriteSerialOffset, 4},
      {Document::kChecksumOffset, 4},         {Document::kHitCountOffset, 4},
      {Document::kNextAlternateOffsetPos, 8}, {Document::kLastAccessOffset, 8},
  };
  for (const auto &[off, size] : in_place) {
    std::memset(head.data() + off, 0, size);
  }
  return head;
}

std::vector<std::byte> make_content(size_t seed, size_t size) {
  std::vector<std::byte> content(size);
  for (size_t j = 0; j < size; ++j) {
    content[j] = static_cast<std::byte>((seed * 131 + j * 7 + 1) & 0xFF);
  }
  return content;
}

bool same_bytes(std::span<const std::byte> got,
                std::span<const std::byte> want) {
  return got.size() == want.size() &&
         std::memcmp(got.data(), want.data(), got.size()) == 0;
}

bool write_entry(Cache &cache, const CacheKey &key,
                 std::span<const std::byte> content) {
  auto wh = cache.write_sync(key, content.size());
  if (!wh.has_value() || !wh->write_sync(content).has_value()) {
    return false;
  }
  return wh->close_sync().has_value();
}

bool write_alternate(Cache &cache, const CacheKey &key, AlternateId id,
                     std::span<const std::byte> content) {
  auto wh = cache.write_alternate_sync(key, id, content.size());
  if (!wh.has_value() || !wh->write_sync(content).has_value()) {
    return false;
  }
  return wh->close_sync().has_value();
}

void pwrite_bytes(const std::string &path, uint64_t offset,
                  std::span<const std::byte> bytes) {
  const int fd = ::open(path.c_str(), O_WRONLY);
  REQUIRE(fd >= 0);
  const ssize_t n =
      ::pwrite(fd, bytes.data(), bytes.size(), static_cast<off_t>(offset));
  ::close(fd);
  REQUIRE(n == static_cast<ssize_t>(bytes.size()));
}

template <typename T>
void pwrite_value(const std::string &path, uint64_t offset, T value) {
  pwrite_bytes(path, offset, std::as_bytes(std::span<const T, 1>(&value, 1)));
}

template <typename T>
T pread_value(const std::string &path, uint64_t offset) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  REQUIRE(fd >= 0);
  T value{};
  const ssize_t n =
      ::pread(fd, &value, sizeof(value), static_cast<off_t>(offset));
  ::close(fd);
  REQUIRE(n == static_cast<ssize_t>(sizeof(value)));
  return value;
}

CacheConfig integrity_config() {
  CacheConfig config;
  config.ram_cache_size = 0;  // every read is a disk (mmap) read
  config.enable_checksum = true;
  config.verify_checksum_on_read = true;
  return config;
}

struct OpenCache {
  TempCacheDir dir{"integrity"};
  std::unique_ptr<Cache> cache;
  std::string file;

  explicit OpenCache(const CacheConfig &config) {
    auto created = Cache::create(config);
    REQUIRE(created.has_value());
    cache = std::move(*created);
    VolumeConfig vol;
    vol.path = dir.path();
    vol.size = kVolBytes;
    REQUIRE(cache->add_volume(vol).has_value());
    REQUIRE(cache->start().has_value());
    file = cache->volume_files().at(0).file_path;
  }
  ~OpenCache() {
    if (cache) {
      cache->stop();
    }
  }
  OpenCache(const OpenCache &) = delete;
  OpenCache &operator=(const OpenCache &) = delete;
};

// File offset of the document `key` resolves to (documents written here
// carry no header bytes, so the content starts right after the fixed one).
uint64_t document_offset(Cache &cache, const CacheKey &key,
                         std::span<const std::byte> content) {
  auto rh = cache.read_sync(key);
  REQUIRE(rh.has_value());
  REQUIRE(same_bytes(rh->content(), content));
  const uint64_t content_off = rh->content_file_offset();
  REQUIRE(content_off != ReadHandle::kNoFileOffset);
  return content_off - Document::kHeaderSize;
}

class OnlyId : public StorageAlternateSelector {
 public:
  explicit OnlyId(AlternateId want) : _want(want) {}
  [[nodiscard]] std::optional<size_t> select(
      std::span<const AlternateInfo> alternates,
      const AlternateSelectionContext & /*ctx*/) const override {
    for (size_t i = 0; i < alternates.size(); ++i) {
      if (alternates[i].id == _want) {
        return i;
      }
    }
    return std::nullopt;
  }

 private:
  AlternateId _want;
};

}  // namespace

TEST_CASE(
    "Read integrity: a stored checksum of zero does not switch off "
    "verification",
    "[integrity][checksum]") {
  OpenCache oc(integrity_config());
  Cache &cache = *oc.cache;

  SECTION("read_sync") {
    const CacheKey key("integrity-zero-crc");
    const auto content = make_content(1, 8192);
    REQUIRE(write_entry(cache, key, content));
    const uint64_t doc = document_offset(cache, key, content);

    // Zero the checksum field and damage one payload byte.
    pwrite_value<uint32_t>(oc.file, doc + Document::kChecksumOffset, 0);
    const std::array<std::byte, 1> flipped = {content[100] ^ std::byte{0x5A}};
    pwrite_bytes(oc.file, doc + Document::kHeaderSize + 100, flipped);

    auto rh = cache.read_sync(key);
    CHECK_FALSE(rh.has_value());
    if (rh.has_value()) {
      CHECK(same_bytes(rh->content(), content));
    }
  }

  SECTION("read_alternate_sync") {
    const CacheKey key("integrity-zero-crc-alt");
    const auto content = make_content(2, 8192);
    REQUIRE(write_alternate(cache, key, AlternateId::Brotli, content));
    auto alts = cache.list_alternates_sync(key);
    REQUIRE(alts.has_value());
    REQUIRE(alts->size() == 1);
    const uint64_t doc = (*alts)[0].disk_offset;
    REQUIRE(pread_value<uint32_t>(oc.file, doc) == Document::kMagic);

    pwrite_value<uint32_t>(oc.file, doc + Document::kChecksumOffset, 0);
    const std::array<std::byte, 1> flipped = {content[100] ^ std::byte{0x5A}};
    pwrite_bytes(oc.file, doc + Document::kHeaderSize + 100, flipped);

    OnlyId want(AlternateId::Brotli);
    AlternateSelectionContext ctx;
    auto rh = cache.read_alternate_sync(key, want, ctx);
    CHECK_FALSE(rh.has_value());
    if (rh.has_value()) {
      CHECK(same_bytes(rh->content(), content));
    }
  }
}

TEST_CASE(
    "Read integrity: a document whose CRC-32C is genuinely zero is served",
    "[integrity][checksum]") {
  // Compatibility pin: every writer stores the computed CRC, so a stored
  // zero is either that genuine value or damage.  The genuine case must
  // keep verifying and serving.
  OpenCache oc(integrity_config());
  Cache &cache = *oc.cache;
  const CacheKey key("integrity-genuine-zero-crc");
  auto content = make_content(3, 4096);
  force_zero_crc(canonical_header(key, content), content);
  REQUIRE(write_entry(cache, key, content));
  const uint64_t doc = document_offset(cache, key, content);
  REQUIRE(pread_value<uint32_t>(oc.file, doc + Document::kChecksumOffset) == 0);
  for (int i = 0; i < 3; ++i) {
    auto rh = cache.read_sync(key);
    REQUIRE(rh.has_value());
    REQUIRE(same_bytes(rh->content(), content));
  }
}
// Wire offsets of the length and type fields in the fixed document header
// (see the field table in src/core/document.cpp).
constexpr uint64_t kTotalLenOffset = 8;
constexpr uint64_t kHeaderLenOffset = 80;
constexpr uint64_t kDocTypeOffset = 84;

TEST_CASE(
    "Read integrity: a document whose length fields disagree is a miss, "
    "never served",
    "[integrity][shape]") {
  OpenCache oc(integrity_config());
  Cache &cache = *oc.cache;
  const auto content = make_content(4, 8192);

  // Each mutation leaves the payload, and so its CRC-32C, intact: only the
  // fixed header's view of it changes.
  struct Mutation {
    const char *name;
    uint64_t offset;
    std::vector<std::byte> bytes;
  };
  const auto u64 = [](uint64_t v) {
    std::vector<std::byte> out(sizeof(v));
    std::memcpy(out.data(), &v, sizeof(v));
    return out;
  };
  const auto u32 = [](uint32_t v) {
    std::vector<std::byte> out(sizeof(v));
    std::memcpy(out.data(), &v, sizeof(v));
    return out;
  };
  const std::vector<Mutation> mutations = {
      {"total_len zero", kTotalLenOffset, u64(0)},
      {"total_len larger", kTotalLenOffset, u64(content.size() + 1)},
      {"header_len shifts the content window", kHeaderLenOffset, u32(16)},
      {"not a single fragment",
       kDocTypeOffset,
       {static_cast<std::byte>(Document::Type::FirstFrag)}},
  };

  size_t n = 0;
  for (const auto &m : mutations) {
    CAPTURE(m.name);
    const CacheKey key("integrity-shape-" + std::to_string(n));
    const CacheKey alt_key("integrity-shape-alt-" + std::to_string(n));
    ++n;
    REQUIRE(write_entry(cache, key, content));
    REQUIRE(write_alternate(cache, alt_key, AlternateId::Gzip, content));
    const uint64_t doc = document_offset(cache, key, content);
    auto alts = cache.list_alternates_sync(alt_key);
    REQUIRE(alts.has_value());
    REQUIRE(alts->size() == 1);
    const uint64_t alt_doc = (*alts)[0].disk_offset;

    pwrite_bytes(oc.file, doc + m.offset, m.bytes);
    pwrite_bytes(oc.file, alt_doc + m.offset, m.bytes);

    {
      auto rh = cache.read_sync(key);
      CHECK_FALSE(rh.has_value());
      if (rh.has_value()) {
        CHECK(rh->content_length() == content.size());
        CHECK(same_bytes(rh->content(), content));
      }
    }
    {
      OnlyId want(AlternateId::Gzip);
      AlternateSelectionContext ctx;
      auto rh = cache.read_alternate_sync(alt_key, want, ctx);
      CHECK_FALSE(rh.has_value());
      if (rh.has_value()) {
        CHECK(rh->content_length() == content.size());
        CHECK(same_bytes(rh->content(), content));
      }
    }
  }
}
// Selects the only alternate and, before returning, rewrites that
// document's total_len behind the cache's back: the fixed header changes
// between the chain walk and the read of the selected document.
class ShapeChangingSelector : public StorageAlternateSelector {
 public:
  ShapeChangingSelector(std::string file, uint64_t doc)
      : _file(std::move(file)), _doc(doc) {}
  [[nodiscard]] std::optional<size_t> select(
      std::span<const AlternateInfo> alternates,
      const AlternateSelectionContext & /*ctx*/) const override {
    if (alternates.empty()) {
      return std::nullopt;
    }
    pwrite_value<uint64_t>(_file, _doc + kTotalLenOffset, 0);
    return 0;
  }

 private:
  std::string _file;
  uint64_t _doc;
};

TEST_CASE(
    "Read integrity: a selected alternate whose length fields disagree is a "
    "miss, as in the chain walk",
    "[integrity][shape]") {
  OpenCache oc(integrity_config());
  Cache &cache = *oc.cache;
  const CacheKey key("integrity-shape-selected");
  const auto content = make_content(12, 4096);
  REQUIRE(write_alternate(cache, key, AlternateId::Brotli, content));
  auto alts = cache.list_alternates_sync(key);
  REQUIRE(alts.has_value());
  REQUIRE(alts->size() == 1);

  ShapeChangingSelector selector(oc.file, (*alts)[0].disk_offset);
  AlternateSelectionContext ctx;
  auto rh = cache.read_alternate_sync(key, selector, ctx);
  REQUIRE_FALSE(rh.has_value());
  CHECK(rh.error() == CacheError::NotFound);
}

TEST_CASE(
    "Read integrity: payload damage under an unchanged header is found on "
    "the warm path within a bounded number of reads",
    "[integrity][crc_cache]") {
  OpenCache oc(integrity_config());
  Cache &cache = *oc.cache;
  const CacheKey key("integrity-warm-damage");
  const auto content = make_content(5, 8192);
  REQUIRE(write_entry(cache, key, content));
  // The first read verifies the document and caches the verdict.
  const uint64_t doc = document_offset(cache, key, content);

  // Damage one payload byte; the fixed header, and so the cached verdict's
  // identity, is unchanged.
  const std::array<std::byte, 1> flipped = {content[4000] ^ std::byte{0x3C}};
  pwrite_bytes(oc.file, doc + Document::kHeaderSize + 4000, flipped);

  Volume::seed_checksum_revalidation_for_test(0x9E3779B97F4A7C15ULL);
  const int limit = static_cast<int>(Volume::kChecksumRevalidateInterval) * 16;
  int first_miss = -1;
  int served_after_miss = 0;
  for (int i = 0; i < limit; ++i) {
    auto rh = cache.read_sync(key);
    if (!rh.has_value()) {
      if (first_miss < 0) {
        first_miss = i;
      }
      continue;
    }
    if (first_miss >= 0) {
      ++served_after_miss;
    }
  }
  // Found within a bounded number of warm reads (the re-check is a 1-in-
  // kChecksumRevalidateInterval draw; the seed above fixes the sequence)...
  REQUIRE(first_miss >= 0);
  // ...and, once found, never served again: the failed pass drops the
  // cached verdict instead of leaving it to vouch for the damaged bytes.
  CHECK(served_after_miss == 0);

  // Repairing the byte makes the document verify and serve again.
  const std::array<std::byte, 1> original = {content[4000]};
  pwrite_bytes(oc.file, doc + Document::kHeaderSize + 4000, original);
  auto rh = cache.read_sync(key);
  REQUIRE(rh.has_value());
  REQUIRE(same_bytes(rh->content(), content));
}
TEST_CASE(
    "Read integrity: a thread that also makes cold reads still re-checks a "
    "damaged warm document",
    "[integrity][crc_cache]") {
  OpenCache oc(integrity_config());
  Cache &cache = *oc.cache;
  Volume::seed_checksum_revalidation_for_test(0x243F6A8885A308D3ULL);
  const CacheKey key("integrity-warm-with-cold");
  const auto content = make_content(10, 4096);
  REQUIRE(write_entry(cache, key, content));
  const uint64_t doc = document_offset(cache, key, content);
  const std::array<std::byte, 1> flipped = {content[300] ^ std::byte{0x42}};
  pwrite_bytes(oc.file, doc + Document::kHeaderSize + 300, flipped);

  // Every 100th read is the first read of a fresh document: a CRC pass
  // over another document, which must not postpone the warm re-check.
  const auto cold = make_content(11, 1024);
  const int limit = static_cast<int>(Volume::kChecksumRevalidateInterval) * 16;
  int cold_index = 0;
  bool found = false;
  for (int i = 1; i <= limit && !found; ++i) {
    if (i % 100 == 0) {
      const CacheKey cold_key("integrity-cold-" + std::to_string(cold_index++));
      REQUIRE(write_entry(cache, cold_key, cold));
      REQUIRE(cache.read_sync(cold_key).has_value());
      continue;
    }
    found = !cache.read_sync(key).has_value();
  }
  CHECK(found);
}

TEST_CASE(
    "Read integrity: a reader cycling over a small set re-checks every "
    "document in it",
    "[integrity][crc_cache]") {
  for (const size_t set_size : {size_t{4}, size_t{8}}) {
    CAPTURE(set_size);
    OpenCache oc(integrity_config());
    Cache &cache = *oc.cache;
    Volume::seed_checksum_revalidation_for_test(0x13198A2E03707344ULL +
                                                set_size);
    std::vector<CacheKey> keys;
    std::vector<std::vector<std::byte>> contents;
    std::vector<uint64_t> docs;
    for (size_t d = 0; d < set_size; ++d) {
      keys.emplace_back("integrity-cycle-" + std::to_string(set_size) + "-" +
                        std::to_string(d));
      contents.push_back(make_content(20 + d, 4096));
      REQUIRE(write_entry(cache, keys.back(), contents.back()));
      docs.push_back(document_offset(cache, keys.back(), contents.back()));
    }
    const int budget =
        static_cast<int>(Volume::kChecksumRevalidateInterval * 16 * set_size);
    for (size_t d = 0; d < set_size; ++d) {
      CAPTURE(d);
      const std::array<std::byte, 1> flipped = {contents[d][77] ^
                                                std::byte{0x24}};
      pwrite_bytes(oc.file, docs[d] + Document::kHeaderSize + 77, flipped);
      bool found = false;
      for (int i = 0; i < budget && !found; ++i) {
        const size_t k = static_cast<size_t>(i) % set_size;
        const bool served = cache.read_sync(keys[k]).has_value();
        if (k == d && !served) {
          found = true;
        }
      }
      CHECK(found);
      // Repair it; the next read verifies and caches it again.
      const std::array<std::byte, 1> original = {contents[d][77]};
      pwrite_bytes(oc.file, docs[d] + Document::kHeaderSize + 77, original);
      auto rh = cache.read_sync(keys[d]);
      REQUIRE(rh.has_value());
      REQUIRE(same_bytes(rh->content(), contents[d]));
    }
  }
}

TEST_CASE("Read integrity: no cached CRC verdict survives a close and reopen",
          "[integrity][crc_cache]") {
  // Multi-process mode keeps the directory on disk, so the same Volume
  // objects find the document again at the same offset after stop/start.
  CacheConfig config = integrity_config();
  config.multi_process_config.enabled = true;
  config.multi_process_config.process_index = 0;
  config.multi_process_config.total_processes = 1;
  OpenCache oc(config);
  Cache &cache = *oc.cache;
  const CacheKey key("integrity-reopen");
  const auto content = make_content(6, 8192);
  REQUIRE(write_entry(cache, key, content));
  // Verified once and cached by this Volume.
  const uint64_t doc = document_offset(cache, key, content);

  cache.stop();
  // While closed, the payload changes under an unchanged header.
  const std::array<std::byte, 1> flipped = {content[2000] ^ std::byte{0x11}};
  pwrite_bytes(oc.file, doc + Document::kHeaderSize + 2000, flipped);
  REQUIRE(cache.start().has_value());

  auto rh = cache.read_sync(key);
  CHECK_FALSE(rh.has_value());
  if (rh.has_value()) {
    CHECK(same_bytes(rh->content(), content));
  }
}

// In-memory directory bucket count (kDirectoryEntriesPerSegment in
// src/core/volume.cpp), mirrored so the filler keys below never share the
// read key's bucket.
constexpr uint32_t kDirectoryBuckets = 16 * 1024;

TEST_CASE(
    "Read integrity: a RAM-tier copy is published only after it is "
    "revalidated against a racing wrap",
    "[integrity][ram]") {
  CacheConfig config = integrity_config();
  config.ram_cache_size = size_t{1} * 1024 * 1024;
  // LRU admits on first sight (CLFUS would defer the first put).
  config.ram_cache_type = RamCacheType::LRU;
  // No read leases: the wrap below is never deferred by the reader's borrow.
  config.read_lease_duration = std::chrono::milliseconds(0);
  config.lease_wrap_ceiling = std::chrono::milliseconds(0);
  // Flush mode: a wrap only overwrites, it carries nothing forward.
  config.wrap_retention = false;

  const CacheKey key("integrity-ram-copy");
  const auto content = make_content(7, 8192);
  const uint32_t avoid = key.bucket_hash() % kDirectoryBuckets;
  const auto filler = make_content(8, 8192);
  const auto damaged = make_content(9, content.size());
  size_t filler_index = 0;
  const auto next_filler = [&] {
    for (;;) {
      CacheKey k("integrity-ram-filler-" + std::to_string(filler_index++));
      if (k.bucket_hash() % kDirectoryBuckets != avoid) {
        return k;
      }
    }
  };

  // Geometry probe on a scratch volume: how many fillers behind the first
  // document the first wrap takes.  Every write to the stripe also bumps
  // its RAM invalidation generation, so the wrap under test must be the
  // only write that lands while the read below is in flight.
  size_t fillers_to_wrap = 0;
  {
    OpenCache probe(config);
    REQUIRE(write_alternate(*probe.cache, key, AlternateId::Original, content));
    while (probe.cache->stats().write_buffer_wraps == 0) {
      REQUIRE(write_entry(*probe.cache, next_filler(), filler));
      ++fillers_to_wrap;
      REQUIRE(fillers_to_wrap < 100000);
    }
  }
  REQUIRE(fillers_to_wrap > 1);

  OpenCache oc(config);
  Cache &cache = *oc.cache;
  REQUIRE(cache.stats().stripe_count == 1);
  // The first document of a fresh single-stripe volume sits at the start of
  // the data area, where the first wrap lands the next document.
  REQUIRE(write_alternate(cache, key, AlternateId::Original, content));
  auto alts = cache.list_alternates_sync(key);
  REQUIRE(alts.has_value());
  REQUIRE(alts->size() == 1);
  const uint64_t doc = (*alts)[0].disk_offset;
  filler_index = 0;
  for (size_t i = 0; i + 1 < fillers_to_wrap; ++i) {
    REQUIRE(write_entry(cache, next_filler(), filler));
  }
  REQUIRE(cache.stats().write_buffer_wraps == 0);

  // The wrapping writer stops right after it published the wrap (epoch
  // stored, intent still set) and the region it is about to fill has
  // changed under the reader: the moment a racing wrap tears a copy.
  std::mutex mu;
  std::condition_variable cv;
  bool wrap_parked = false;
  bool release_writer = false;
  bool in_wrap = false;
  Volume::s_writer_seam_for_test = [&](Volume::WriterSeam at) {
    if (at == Volume::WriterSeam::kWrapAfterCursor) {
      in_wrap = true;
      return;
    }
    if (at != Volume::WriterSeam::kAfterEpochStore || !in_wrap) {
      return;
    }
    in_wrap = false;
    const int fd = ::open(oc.file.c_str(), O_WRONLY);
    if (fd >= 0) {
      (void)!::pwrite(fd, damaged.data(), damaged.size(),
                      static_cast<off_t>(doc + Document::kHeaderSize));
      ::close(fd);
    }
    std::unique_lock lock(mu);
    wrap_parked = true;
    cv.notify_all();
    cv.wait(lock, [&] { return release_writer; });
  };

  std::thread writer;
  bool in_hook = false;
  int nested_reads = 0;
  int wrong_serves = 0;
  Volume::s_reader_seam_for_test = [&](Volume::ReaderSeam at) {
    if (in_hook) {
      return;
    }
    in_hook = true;
    if (at == Volume::ReaderSeam::kRamCopy && !writer.joinable()) {
      writer =
          std::thread([&] { (void)write_entry(cache, next_filler(), filler); });
      std::unique_lock lock(mu);
      cv.wait(lock, [&] { return wrap_parked; });
    } else if (at == Volume::ReaderSeam::kRamCopied) {
      // A concurrent reader at this point must not be served the copy
      // (read_sync consults the RAM tier before the directory).
      ++nested_reads;
      auto rh = cache.read_sync(key);
      if (rh.has_value() && !same_bytes(rh->content(), content)) {
        ++wrong_serves;
      }
    }
    in_hook = false;
  };

  {
    OnlyId want(AlternateId::Original);
    AlternateSelectionContext ctx;
    // Whatever this read returns, the RAM tier must not expose or keep the
    // bytes the wrap changed under it.
    (void)cache.read_alternate_sync(key, want, ctx);
  }
  Volume::s_reader_seam_for_test = nullptr;
  {
    std::lock_guard lock(mu);
    release_writer = true;
  }
  cv.notify_all();
  if (writer.joinable()) {
    writer.join();
  }
  Volume::s_writer_seam_for_test = nullptr;

  REQUIRE(wrap_parked);
  CHECK(wrong_serves == 0);
  auto rh = cache.read_sync(key);
  if (rh.has_value()) {
    CHECK(same_bytes(rh->content(), content));
  }
}

// --- Header coverage of the document checksum ------------------------------

struct HeaderField {
  const char *name;
  uint64_t offset;  // wire offset in the fixed header (document.cpp table)
  uint64_t size;
};

// Fields that never change after the write: the checksum covers them.
const std::vector<HeaderField> &covered_fields() {
  static const std::vector<HeaderField> fields = {
      {"magic", 0, 4},          {"len", 4, 4},
      {"total_len", 8, 8},      {"first_key", 16, 32},
      {"fragment_key", 48, 32}, {"header_len", 80, 4},
      {"doc_type", 84, 1},      {"version_major", 85, 1},
      {"version_minor", 86, 1}, {"flags", 87, 1},
      {"pin_until", 96, 4},     {"frag_offset", 104, 4},
      {"alternate_id", 128, 1}, {"reserved", 129, 3},
  };
  return fields;
}

// Flip the lowest bit of the field's last byte: a small change that keeps
// lengths in range, so only the checksum can tell.
void damage_field(const std::string &file, uint64_t doc,
                  const HeaderField &field) {
  const uint64_t at = doc + field.offset + field.size - 1;
  const auto b = pread_value<std::byte>(file, at);
  pwrite_value<std::byte>(file, at, b ^ std::byte{0x01});
}

TEST_CASE(
    "Read integrity: damage to any fixed header field the checksum covers "
    "reads as a miss",
    "[integrity][format]") {
  OpenCache oc(integrity_config());
  Cache &cache = *oc.cache;
  const auto content = make_content(30, 4096);
  size_t n = 0;
  for (const auto &field : covered_fields()) {
    CAPTURE(field.name);
    for (const bool warm : {false, true}) {
      CAPTURE(warm);
      // An Original-id alternate is what read_sync serves; a Brotli one is
      // only reachable through read_alternate_sync.
      const CacheKey key("integrity-cover-" + std::to_string(n));
      const CacheKey alt_key("integrity-cover-alt-" + std::to_string(n));
      ++n;
      REQUIRE(write_alternate(cache, key, AlternateId::Original, content));
      REQUIRE(write_alternate(cache, alt_key, AlternateId::Brotli, content));
      auto alts = cache.list_alternates_sync(key);
      REQUIRE(alts.has_value());
      REQUIRE(alts->size() == 1);
      const uint64_t doc = (*alts)[0].disk_offset;
      auto alt_alts = cache.list_alternates_sync(alt_key);
      REQUIRE(alt_alts.has_value());
      REQUIRE(alt_alts->size() == 1);
      const uint64_t alt_doc = (*alt_alts)[0].disk_offset;
      OnlyId want(AlternateId::Brotli);
      AlternateSelectionContext ctx;
      if (warm) {
        // A verified, cached verdict must not vouch for the changed header.
        REQUIRE(cache.read_sync(key).has_value());
        REQUIRE(cache.read_alternate_sync(alt_key, want, ctx).has_value());
      }

      damage_field(oc.file, doc, field);
      damage_field(oc.file, alt_doc, field);

      auto rh = cache.read_sync(key);
      CHECK_FALSE(rh.has_value());
      auto ah = cache.read_alternate_sync(alt_key, want, ctx);
      CHECK_FALSE(ah.has_value());
    }
  }
}

TEST_CASE(
    "Read integrity: the fields updated in place stay outside the checksum",
    "[integrity][format]") {
  // Flush mode: retention admission compares the pass stamp itself, which
  // would reject a changed stamp before the checksum is consulted.
  CacheConfig config = integrity_config();
  config.wrap_retention = false;
  OpenCache oc(config);
  Cache &cache = *oc.cache;
  const auto content = make_content(31, 4096);
  const CacheKey key("integrity-inplace");
  const CacheKey alt_key("integrity-inplace-alt");
  REQUIRE(write_alternate(cache, key, AlternateId::Original, content));
  REQUIRE(write_alternate(cache, alt_key, AlternateId::Brotli, content));
  auto alts = cache.list_alternates_sync(key);
  REQUIRE(alts.has_value());
  const uint64_t doc = (*alts)[0].disk_offset;
  auto alt_alts = cache.list_alternates_sync(alt_key);
  REQUIRE(alt_alts.has_value());
  const uint64_t alt_doc = (*alt_alts)[0].disk_offset;

  // write_serial (pass stamp), hit_count and last_access.
  for (const uint64_t d : {doc, alt_doc}) {
    pwrite_value<uint32_t>(oc.file, d + Document::kWriteSerialOffset, 77);
    pwrite_value<uint32_t>(oc.file, d + Document::kHitCountOffset, 12345);
    pwrite_value<int64_t>(oc.file, d + Document::kLastAccessOffset,
                          int64_t{1700000000000});
  }

  auto rh = cache.read_sync(key);
  REQUIRE(rh.has_value());
  CHECK(same_bytes(rh->content(), content));
  OnlyId want(AlternateId::Brotli);
  AlternateSelectionContext ctx;
  auto ah = cache.read_alternate_sync(alt_key, want, ctx);
  REQUIRE(ah.has_value());
  CHECK(same_bytes(ah->content(), content));

  // The library's own in-place path keeps the documents valid as well:
  // removing the middle node of a three-node chain repoints the head's
  // next_alternate_offset in place.
  const CacheKey chain_key("integrity-inplace-chain");
  REQUIRE(write_alternate(cache, chain_key, AlternateId::Original, content));
  REQUIRE(write_alternate(cache, chain_key, AlternateId::Gzip, content));
  REQUIRE(write_alternate(cache, chain_key, AlternateId::Brotli, content));
  REQUIRE(
      cache.remove_alternate_sync(chain_key, AlternateId::Gzip).has_value());
  for (const AlternateId id : {AlternateId::Brotli, AlternateId::Original}) {
    OnlyId pick(id);
    AlternateSelectionContext pick_ctx;
    auto got = cache.read_alternate_sync(chain_key, pick, pick_ctx);
    REQUIRE(got.has_value());
    CHECK(same_bytes(got->content(), content));
  }
}

// Rewrite a document written by this build into the previous format: the
// version byte says 8 and the checksum covers header bytes + content only.
void age_document_to_v8(const std::string &file, uint64_t doc,
                        size_t payload_len) {
  pwrite_value<uint8_t>(file, doc + Document::kVersionMajorOffset, 8);
  std::vector<std::byte> payload(payload_len);
  const int fd = ::open(file.c_str(), O_RDONLY);
  REQUIRE(fd >= 0);
  const ssize_t got = ::pread(fd, payload.data(), payload.size(),
                              static_cast<off_t>(doc + Document::kHeaderSize));
  ::close(fd);
  REQUIRE(got == static_cast<ssize_t>(payload.size()));
  pwrite_value<uint32_t>(file, doc + Document::kChecksumOffset,
                         oracle_crc32c(payload));
}

TEST_CASE(
    "Read integrity: a volume written in the previous format opens as a "
    "clean, empty cache",
    "[integrity][format][version]") {
  CacheConfig config = integrity_config();
  config.multi_process_config.enabled = true;  // persistent directory
  config.multi_process_config.process_index = 0;
  config.multi_process_config.total_processes = 1;
  const auto content = make_content(32, 4096);
  constexpr int kKeys = 8;

  for (const int mode : {0, 1, 2}) {
    // 0: volume header and documents in the previous format, auto reset on
    // 1: the same, auto reset off
    // 2: only the documents in the previous format (a header the gate
    //    passes, e.g. a reset that never ran)
    CAPTURE(mode);
    TempCacheDir dir("integrity-v8");
    std::string file;
    {
      auto created = Cache::create(config);
      REQUIRE(created.has_value());
      auto &cache = *created;
      VolumeConfig vol;
      vol.path = dir.path();
      vol.size = kVolBytes;
      REQUIRE(cache->add_volume(vol).has_value());
      REQUIRE(cache->start().has_value());
      file = cache->volume_files().at(0).file_path;
      for (int i = 0; i < kKeys; ++i) {
        const CacheKey key("integrity-v8-" + std::to_string(i));
        REQUIRE(write_alternate(*cache, key, AlternateId::Original, content));
        auto alts = cache->list_alternates_sync(key);
        REQUIRE(alts.has_value());
        REQUIRE(alts->size() == 1);
        age_document_to_v8(file, (*alts)[0].disk_offset, content.size());
      }
      cache->stop();
    }
    if (mode != 2) {
      // VolumeHeader: magic (4 bytes), then the u16 format major.
      pwrite_value<uint16_t>(file, 4, 8);
    }

    auto created = Cache::create(config);
    REQUIRE(created.has_value());
    auto &cache = *created;
    VolumeConfig vol;
    vol.path = dir.path();
    vol.size = kVolBytes;
    vol.auto_reset_on_incompatible = mode != 1;
    REQUIRE(cache->add_volume(vol).has_value());
    auto started = cache->start();
    if (mode == 1) {
      REQUIRE_FALSE(started.has_value());
      CHECK(started.error() == CacheError::IncompatibleVersion);
      continue;
    }
    REQUIRE(started.has_value());
    if (mode == 0) {
      CHECK(cache->stats().current_entries == 0);
    }
    for (int i = 0; i < kKeys; ++i) {
      const CacheKey key("integrity-v8-" + std::to_string(i));
      CHECK_FALSE(cache->read_sync(key).has_value());
    }
    // The volume is usable.
    const CacheKey fresh("integrity-v8-fresh");
    REQUIRE(write_entry(*cache, fresh, content));
    auto rh = cache->read_sync(fresh);
    REQUIRE(rh.has_value());
    CHECK(same_bytes(rh->content(), content));
    cache->stop();
  }
}

// The selected alternate is mapped from its content length plus a fixed
// allowance for the fixed header and the stored header bytes.  A document
// whose header bytes exceed that allowance must still be read whole: the
// mapping follows the document's own length, as read_sync does.
TEST_CASE(
    "Read integrity: an alternate whose header bytes exceed the map "
    "allowance reads whole",
    "[integrity][alternate]") {
  for (const bool verify : {true, false}) {
    CAPTURE(verify);
    CacheConfig config = integrity_config();
    config.verify_checksum_on_read = verify;
    OpenCache oc(config);
    Cache &cache = *oc.cache;

    const CacheKey key("integrity-large-header-alt");
    const auto header = make_content(7, size_t{96} * 1024);
    const auto content = make_content(8, 4096);
    {
      auto wh =
          cache.write_alternate_sync(key, AlternateId::Brotli, content.size());
      REQUIRE(wh.has_value());
      wh->set_header(header);
      REQUIRE(wh->write_sync(content).has_value());
      REQUIRE(wh->close_sync().has_value());
    }

    // read_sync resolves the same document whole.
    {
      auto rh = cache.read_sync(key);
      REQUIRE(rh.has_value());
      CHECK(same_bytes(rh->header(), header));
      CHECK(same_bytes(rh->content(), content));
    }

    OnlyId want(AlternateId::Brotli);
    AlternateSelectionContext ctx;
    auto rh = cache.read_alternate_sync(key, want, ctx);
    REQUIRE(rh.has_value());
    CHECK(rh->content_length() == content.size());
    CHECK(same_bytes(rh->header(), header));
    CHECK(same_bytes(rh->content(), content));
  }
}
#endif  // !_WIN32
