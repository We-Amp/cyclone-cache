// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include <catch2/catch_test_macros.hpp>

#include "core/document.hpp"
#include "cyclone/alternate.hpp"
#include "cyclone/error.hpp"

using namespace cyclone;

TEST_CASE("AlternateId enum values", "[alternate]") {
  REQUIRE(static_cast<uint8_t>(AlternateId::Original) == 0);
  REQUIRE(static_cast<uint8_t>(AlternateId::Brotli) == 1);
  REQUIRE(static_cast<uint8_t>(AlternateId::Zstd) == 2);
  REQUIRE(static_cast<uint8_t>(AlternateId::Gzip) == 3);
  REQUIRE(static_cast<uint8_t>(AlternateId::WebP) == 16);
  REQUIRE(static_cast<uint8_t>(AlternateId::AVIF) == 17);
  REQUIRE(static_cast<uint8_t>(AlternateId::JpegXL) == 18);
  REQUIRE(static_cast<uint8_t>(AlternateId::Custom) == 128);
}

TEST_CASE("alternate_id_name returns correct strings", "[alternate]") {
  REQUIRE(alternate_id_name(AlternateId::Original) == "original");
  REQUIRE(alternate_id_name(AlternateId::Brotli) == "brotli");
  REQUIRE(alternate_id_name(AlternateId::Zstd) == "zstd");
  REQUIRE(alternate_id_name(AlternateId::Gzip) == "gzip");
  REQUIRE(alternate_id_name(AlternateId::WebP) == "webp");
  REQUIRE(alternate_id_name(AlternateId::AVIF) == "avif");
  REQUIRE(alternate_id_name(AlternateId::JpegXL) == "jxl");
  REQUIRE(alternate_id_name(AlternateId::Custom) == "custom");
  REQUIRE(alternate_id_name(static_cast<AlternateId>(200)) == "custom");
  REQUIRE(alternate_id_name(static_cast<AlternateId>(50)) == "unknown");
}

TEST_CASE("is_compression_alternate identifies compression variants",
          "[alternate]") {
  REQUIRE_FALSE(is_compression_alternate(AlternateId::Original));
  REQUIRE(is_compression_alternate(AlternateId::Brotli));
  REQUIRE(is_compression_alternate(AlternateId::Zstd));
  REQUIRE(is_compression_alternate(AlternateId::Gzip));
  REQUIRE_FALSE(is_compression_alternate(AlternateId::WebP));
  REQUIRE_FALSE(is_compression_alternate(AlternateId::AVIF));
  REQUIRE_FALSE(is_compression_alternate(AlternateId::Custom));
}

TEST_CASE("is_image_alternate identifies image variants", "[alternate]") {
  REQUIRE_FALSE(is_image_alternate(AlternateId::Original));
  REQUIRE_FALSE(is_image_alternate(AlternateId::Brotli));
  REQUIRE(is_image_alternate(AlternateId::WebP));
  REQUIRE(is_image_alternate(AlternateId::AVIF));
  REQUIRE(is_image_alternate(AlternateId::JpegXL));
  REQUIRE_FALSE(is_image_alternate(AlternateId::Custom));
}

TEST_CASE("kMaxAlternatesPerKey constant", "[alternate]") {
  REQUIRE(kMaxAlternatesPerKey == 64);
}

TEST_CASE("AlternateInfo default values", "[alternate]") {
  AlternateInfo info;
  REQUIRE(info.id == AlternateId::Original);
  REQUIRE(info.disk_offset == 0);
  REQUIRE(info.content_length == 0);
  REQUIRE(info.hit_count == 0);
  REQUIRE(info.header.empty());
}

TEST_CASE("AlternateSelectionContext default values", "[alternate]") {
  AlternateSelectionContext ctx;
  REQUIRE(ctx.request_metadata.empty());
  REQUIRE(ctx.prefer_compressed == true);
  REQUIRE(ctx.acceptable_alternates.empty());
}

TEST_CASE("DefaultStorageSelector returns first alternate", "[alternate]") {
  DefaultStorageSelector selector;
  AlternateSelectionContext ctx;

  SECTION("empty alternates returns nullopt") {
    std::vector<AlternateInfo> empty;
    auto result = selector.select(empty, ctx);
    REQUIRE_FALSE(result.has_value());
  }

  SECTION("single alternate returns index 0") {
    std::vector<AlternateInfo> alternates = {AlternateInfo{}};
    auto result = selector.select(alternates, ctx);
    REQUIRE(result.has_value());
    REQUIRE(*result == 0);
  }

  SECTION("multiple alternates returns index 0") {
    std::vector<AlternateInfo> alternates = {
        AlternateInfo{.id = AlternateId::Brotli},
        AlternateInfo{.id = AlternateId::Original},
        AlternateInfo{.id = AlternateId::Gzip},
    };
    auto result = selector.select(alternates, ctx);
    REQUIRE(result.has_value());
    REQUIRE(*result == 0);
  }
}

TEST_CASE("DefaultStorageSelector honors acceptable_alternates",
          "[alternate]") {
  // acceptable_alternates was declared but ignored. Same
  // semantics as the acceptable-alternates rule: a non-empty set is a hard
  // restriction — the result is the first acceptable alternate in chain order,
  // or nullopt ("none are acceptable", per the select() contract, which
  // read_alternate_sync maps to AlternateNotFound). An empty set behaves
  // exactly as before.
  DefaultStorageSelector selector;
  AlternateSelectionContext ctx;

  SECTION("empty acceptable set returns index 0 (backward compatible)") {
    std::vector<AlternateInfo> alternates = {
        AlternateInfo{.id = AlternateId::Brotli},
        AlternateInfo{.id = AlternateId::Gzip},
    };
    auto result = selector.select(alternates, ctx);
    REQUIRE(result.has_value());
    REQUIRE(*result == 0);
  }

  SECTION("restricts the pick to the acceptable set") {
    // The verbatim repro: acceptable = {Gzip}, chain holds Brotli+Gzip.
    std::vector<AlternateInfo> alternates = {
        AlternateInfo{.id = AlternateId::Brotli},
        AlternateInfo{.id = AlternateId::Gzip},
    };
    const AlternateId acceptable[] = {AlternateId::Gzip};
    ctx.acceptable_alternates = acceptable;
    auto result = selector.select(alternates, ctx);
    REQUIRE(result.has_value());
    REQUIRE(*result == 1);
    REQUIRE(alternates[*result].id == AlternateId::Gzip);
  }

  SECTION("index 0 acceptable returns 0") {
    std::vector<AlternateInfo> alternates = {
        AlternateInfo{.id = AlternateId::Brotli},
        AlternateInfo{.id = AlternateId::Gzip},
    };
    const AlternateId acceptable[] = {AlternateId::Brotli, AlternateId::Gzip};
    ctx.acceptable_alternates = acceptable;
    auto result = selector.select(alternates, ctx);
    REQUIRE(result.has_value());
    REQUIRE(*result == 0);
  }

  SECTION("index 0 unacceptable returns the first acceptable index") {
    std::vector<AlternateInfo> alternates = {
        AlternateInfo{.id = AlternateId::Brotli},
        AlternateInfo{.id = AlternateId::Original},
        AlternateInfo{.id = AlternateId::Zstd},
    };
    const AlternateId acceptable[] = {AlternateId::Original, AlternateId::Zstd};
    ctx.acceptable_alternates = acceptable;
    auto result = selector.select(alternates, ctx);
    REQUIRE(result.has_value());
    REQUIRE(*result == 1);
  }

  SECTION("duplicate ids return the first acceptable occurrence") {
    std::vector<AlternateInfo> alternates = {
        AlternateInfo{.id = AlternateId::Brotli},
        AlternateInfo{.id = AlternateId::Gzip},
        AlternateInfo{.id = AlternateId::Gzip},
    };
    const AlternateId acceptable[] = {AlternateId::Gzip};
    ctx.acceptable_alternates = acceptable;
    auto result = selector.select(alternates, ctx);
    REQUIRE(result.has_value());
    REQUIRE(*result == 1);
  }

  SECTION("only Original acceptable returns the Original index") {
    std::vector<AlternateInfo> alternates = {
        AlternateInfo{.id = AlternateId::Brotli},
        AlternateInfo{.id = AlternateId::Original},
    };
    const AlternateId acceptable[] = {AlternateId::Original};
    ctx.acceptable_alternates = acceptable;
    auto result = selector.select(alternates, ctx);
    REQUIRE(result.has_value());
    REQUIRE(*result == 1);
    REQUIRE(alternates[*result].id == AlternateId::Original);
  }

  SECTION("returns nullopt when nothing acceptable exists") {
    // The fallback must not return index 0 when it is unacceptable.
    std::vector<AlternateInfo> alternates = {
        AlternateInfo{.id = AlternateId::Original},
        AlternateInfo{.id = AlternateId::Gzip},
        AlternateInfo{.id = AlternateId::Brotli},
    };
    const AlternateId acceptable[] = {AlternateId::Zstd};
    ctx.acceptable_alternates = acceptable;
    auto result = selector.select(alternates, ctx);
    REQUIRE_FALSE(result.has_value());
  }
}

TEST_CASE("CompressionAwareSelector prefers compressed variants",
          "[alternate]") {
  CompressionAwareSelector selector;
  AlternateSelectionContext ctx;
  ctx.prefer_compressed = true;

  SECTION("empty alternates returns nullopt") {
    std::vector<AlternateInfo> empty;
    auto result = selector.select(empty, ctx);
    REQUIRE_FALSE(result.has_value());
  }

  SECTION("prefers Brotli over others") {
    std::vector<AlternateInfo> alternates = {
        AlternateInfo{.id = AlternateId::Original},
        AlternateInfo{.id = AlternateId::Gzip},
        AlternateInfo{.id = AlternateId::Brotli},
        AlternateInfo{.id = AlternateId::Zstd},
    };
    auto result = selector.select(alternates, ctx);
    REQUIRE(result.has_value());
    REQUIRE(alternates[*result].id == AlternateId::Brotli);
  }

  SECTION("prefers Zstd when no Brotli") {
    std::vector<AlternateInfo> alternates = {
        AlternateInfo{.id = AlternateId::Original},
        AlternateInfo{.id = AlternateId::Gzip},
        AlternateInfo{.id = AlternateId::Zstd},
    };
    auto result = selector.select(alternates, ctx);
    REQUIRE(result.has_value());
    REQUIRE(alternates[*result].id == AlternateId::Zstd);
  }

  SECTION("prefers Gzip when no Brotli/Zstd") {
    std::vector<AlternateInfo> alternates = {
        AlternateInfo{.id = AlternateId::Original},
        AlternateInfo{.id = AlternateId::Gzip},
    };
    auto result = selector.select(alternates, ctx);
    REQUIRE(result.has_value());
    REQUIRE(alternates[*result].id == AlternateId::Gzip);
  }

  SECTION("falls back to Original when no compression variants") {
    std::vector<AlternateInfo> alternates = {
        AlternateInfo{.id = AlternateId::WebP},
        AlternateInfo{.id = AlternateId::Original},
    };
    auto result = selector.select(alternates, ctx);
    REQUIRE(result.has_value());
    REQUIRE(alternates[*result].id == AlternateId::Original);
  }

  SECTION("returns first when prefer_compressed is false") {
    ctx.prefer_compressed = false;
    std::vector<AlternateInfo> alternates = {
        AlternateInfo{.id = AlternateId::Brotli},
        AlternateInfo{.id = AlternateId::Original},
    };
    auto result = selector.select(alternates, ctx);
    REQUIRE(result.has_value());
    REQUIRE(*result == 0);
  }
}

TEST_CASE("CompressionAwareSelector picks the newest of a duplicated id",
          "[alternate]") {
  // Re-recording an alternate prepends a new document without unlinking the
  // superseded one, so the same id can appear several times in one chain.
  // Enumeration is newest-first, so the newest version is the FIRST
  // occurrence; disk_offset stands in here as a per-version stamp.
  CompressionAwareSelector selector;
  AlternateSelectionContext ctx;
  ctx.prefer_compressed = true;

  SECTION("duplicated Brotli selects the newest occurrence") {
    std::vector<AlternateInfo> alternates = {
        AlternateInfo{.id = AlternateId::Brotli, .disk_offset = 2},
        AlternateInfo{.id = AlternateId::Brotli, .disk_offset = 1},
    };
    auto result = selector.select(alternates, ctx);
    REQUIRE(result.has_value());
    REQUIRE(*result == 0);
    REQUIRE(alternates[*result].disk_offset == 2);
  }

  SECTION("Brotli re-recorded many times still selects the newest") {
    std::vector<AlternateInfo> alternates;
    for (int stamp = 5; stamp >= 1; --stamp) {
      alternates.push_back(AlternateInfo{
          .id = AlternateId::Brotli,
          .disk_offset = static_cast<uint64_t>(stamp),
      });
    }
    auto result = selector.select(alternates, ctx);
    REQUIRE(result.has_value());
    REQUIRE(*result == 0);
    REQUIRE(alternates[*result].disk_offset == 5);
  }

  SECTION("duplicated Original selects the newest occurrence") {
    std::vector<AlternateInfo> alternates = {
        AlternateInfo{.id = AlternateId::WebP, .disk_offset = 9},
        AlternateInfo{.id = AlternateId::Original, .disk_offset = 2},
        AlternateInfo{.id = AlternateId::Original, .disk_offset = 1},
    };
    auto result = selector.select(alternates, ctx);
    REQUIRE(result.has_value());
    REQUIRE(alternates[*result].id == AlternateId::Original);
    REQUIRE(alternates[*result].disk_offset == 2);
  }

  SECTION("duplicated Zstd selects the newest occurrence") {
    std::vector<AlternateInfo> alternates = {
        AlternateInfo{.id = AlternateId::Zstd, .disk_offset = 2},
        AlternateInfo{.id = AlternateId::Zstd, .disk_offset = 1},
        AlternateInfo{.id = AlternateId::Original, .disk_offset = 1},
    };
    auto result = selector.select(alternates, ctx);
    REQUIRE(result.has_value());
    REQUIRE(alternates[*result].id == AlternateId::Zstd);
    REQUIRE(alternates[*result].disk_offset == 2);
  }

  SECTION("duplicated Gzip selects the newest occurrence") {
    std::vector<AlternateInfo> alternates = {
        AlternateInfo{.id = AlternateId::Gzip, .disk_offset = 2},
        AlternateInfo{.id = AlternateId::Gzip, .disk_offset = 1},
        AlternateInfo{.id = AlternateId::Original, .disk_offset = 1},
    };
    auto result = selector.select(alternates, ctx);
    REQUIRE(result.has_value());
    REQUIRE(alternates[*result].id == AlternateId::Gzip);
    REQUIRE(alternates[*result].disk_offset == 2);
  }
}

TEST_CASE("CompressionAwareSelector on a mixed chain with duplicates",
          "[alternate]") {
  // Newest-first chain holding a re-recorded Brotli plus re-recorded and
  // distinct entries of other ids. Dedup (newest per id) and the
  // Brotli > Zstd > Gzip > Original preference must both hold.
  std::vector<AlternateInfo> alternates = {
      AlternateInfo{.id = AlternateId::Original, .disk_offset = 20},
      AlternateInfo{.id = AlternateId::Brotli, .disk_offset = 12},
      AlternateInfo{.id = AlternateId::Gzip, .disk_offset = 30},
      AlternateInfo{.id = AlternateId::Brotli, .disk_offset = 11},
      AlternateInfo{.id = AlternateId::WebP, .disk_offset = 40},
      AlternateInfo{.id = AlternateId::Original, .disk_offset = 10},
  };

  CompressionAwareSelector selector;
  AlternateSelectionContext ctx;

  SECTION("prefers the newest Brotli over the older one and over other ids") {
    ctx.prefer_compressed = true;
    auto result = selector.select(alternates, ctx);
    REQUIRE(result.has_value());
    REQUIRE(alternates[*result].id == AlternateId::Brotli);
    REQUIRE(alternates[*result].disk_offset == 12);
  }

  SECTION("without Brotli, falls through to the newest Gzip") {
    ctx.prefer_compressed = true;
    std::vector<AlternateInfo> no_brotli = {
        AlternateInfo{.id = AlternateId::Original, .disk_offset = 20},
        AlternateInfo{.id = AlternateId::Gzip, .disk_offset = 31},
        AlternateInfo{.id = AlternateId::WebP, .disk_offset = 40},
        AlternateInfo{.id = AlternateId::Gzip, .disk_offset = 30},
        AlternateInfo{.id = AlternateId::Original, .disk_offset = 10},
    };
    auto result = selector.select(no_brotli, ctx);
    REQUIRE(result.has_value());
    REQUIRE(no_brotli[*result].id == AlternateId::Gzip);
    REQUIRE(no_brotli[*result].disk_offset == 31);
  }

  SECTION("prefer_compressed=false still returns the head of the chain") {
    ctx.prefer_compressed = false;
    auto result = selector.select(alternates, ctx);
    REQUIRE(result.has_value());
    REQUIRE(*result == 0);
    REQUIRE(alternates[*result].id == AlternateId::Original);
  }
}

TEST_CASE("CompressionAwareSelector honors acceptable_alternates",
          "[alternate]") {
  // acceptable_alternates was declared but ignored. A non-empty
  // set is a hard restriction — ids outside it are never selected, not even
  // as a fallback; nullopt means "none are acceptable" (the select()
  // contract, which read_alternate_sync maps to AlternateNotFound).
  CompressionAwareSelector selector;
  AlternateSelectionContext ctx;

  SECTION("restricts the pick to the acceptable set") {
    // The verbatim repro: acceptable = {Gzip}, chain holds Brotli+Gzip.
    std::vector<AlternateInfo> alternates = {
        AlternateInfo{.id = AlternateId::Original},
        AlternateInfo{.id = AlternateId::Gzip},
        AlternateInfo{.id = AlternateId::Brotli},
    };
    const AlternateId acceptable[] = {AlternateId::Gzip};
    ctx.acceptable_alternates = acceptable;
    ctx.prefer_compressed = true;
    auto result = selector.select(alternates, ctx);
    REQUIRE(result.has_value());
    REQUIRE(alternates[*result].id == AlternateId::Gzip);
  }

  SECTION("acceptable uncompressed beats unacceptable compressed") {
    // prefer_compressed ranks eligible candidates only; it is never a
    // reason to serve an unacceptable id.
    std::vector<AlternateInfo> alternates = {
        AlternateInfo{.id = AlternateId::Brotli},
        AlternateInfo{.id = AlternateId::Original},
    };
    const AlternateId acceptable[] = {AlternateId::Original};
    ctx.acceptable_alternates = acceptable;
    ctx.prefer_compressed = true;
    auto result = selector.select(alternates, ctx);
    REQUIRE(result.has_value());
    REQUIRE(alternates[*result].id == AlternateId::Original);
  }

  SECTION("returns nullopt when nothing acceptable exists") {
    // The fallback must not return index 0 when it is unacceptable.
    std::vector<AlternateInfo> alternates = {
        AlternateInfo{.id = AlternateId::Original},
        AlternateInfo{.id = AlternateId::Gzip},
        AlternateInfo{.id = AlternateId::Brotli},
    };
    const AlternateId acceptable[] = {AlternateId::Zstd};
    ctx.acceptable_alternates = acceptable;
    ctx.prefer_compressed = true;
    auto result = selector.select(alternates, ctx);
    REQUIRE_FALSE(result.has_value());
  }

  SECTION("falls back to the first acceptable id of any kind") {
    // No Brotli/Zstd/Gzip/Original in the acceptable set: the fallback
    // serves the first acceptable alternate, not index 0.
    std::vector<AlternateInfo> alternates = {
        AlternateInfo{.id = AlternateId::Original},
        AlternateInfo{.id = AlternateId::WebP},
        AlternateInfo{.id = AlternateId::AVIF},
    };
    const AlternateId acceptable[] = {AlternateId::AVIF};
    ctx.acceptable_alternates = acceptable;
    ctx.prefer_compressed = true;
    auto result = selector.select(alternates, ctx);
    REQUIRE(result.has_value());
    REQUIRE(*result == 2);
    REQUIRE(alternates[*result].id == AlternateId::AVIF);
  }

  SECTION("prefer_compressed=false returns the first acceptable alternate") {
    std::vector<AlternateInfo> alternates = {
        AlternateInfo{.id = AlternateId::Brotli},
        AlternateInfo{.id = AlternateId::Original},
    };
    const AlternateId acceptable[] = {AlternateId::Original};
    ctx.acceptable_alternates = acceptable;
    ctx.prefer_compressed = false;
    auto result = selector.select(alternates, ctx);
    REQUIRE(result.has_value());
    REQUIRE(*result == 1);
    REQUIRE(alternates[*result].id == AlternateId::Original);
  }

  SECTION("prefer_compressed=false returns nullopt when none acceptable") {
    std::vector<AlternateInfo> alternates = {
        AlternateInfo{.id = AlternateId::Brotli},
        AlternateInfo{.id = AlternateId::Gzip},
    };
    const AlternateId acceptable[] = {AlternateId::Original};
    ctx.acceptable_alternates = acceptable;
    ctx.prefer_compressed = false;
    auto result = selector.select(alternates, ctx);
    REQUIRE_FALSE(result.has_value());
  }

  SECTION("ranks only eligible candidates") {
    // Brotli outranks Zstd normally, but with Brotli excluded Zstd wins.
    std::vector<AlternateInfo> alternates = {
        AlternateInfo{.id = AlternateId::Brotli},
        AlternateInfo{.id = AlternateId::Zstd},
        AlternateInfo{.id = AlternateId::Original},
    };
    const AlternateId acceptable[] = {AlternateId::Zstd, AlternateId::Gzip};
    ctx.acceptable_alternates = acceptable;
    ctx.prefer_compressed = true;
    auto result = selector.select(alternates, ctx);
    REQUIRE(result.has_value());
    REQUIRE(alternates[*result].id == AlternateId::Zstd);
  }
}

TEST_CASE("CacheError alternate error codes", "[alternate][error]") {
  REQUIRE(make_error_code(CacheError::TooManyAlternates).message() ==
          "too many alternates for key");
  REQUIRE(make_error_code(CacheError::AlternateNotFound).message() ==
          "alternate not found");
  REQUIRE(make_error_code(CacheError::ChainCorrupted).message() ==
          "alternate chain corrupted");
}

// Integration tests for list_alternates_sync are in test_cache_roundtrip.cpp

// =============================================================================
// Phase 4B: Chain Integrity Tests
// =============================================================================

#include <atomic>
#include <cstddef>
#include <cstring>
#include <filesystem>

#include "core/directory.hpp"
#include "cyclone/cache.hpp"
#include "cyclone/key.hpp"
#include "support/temp_cache.hpp"

TEST_CASE("Alternate chain depth limit", "[alternate][security]") {
  // Document::kMaxAlternates is 64. Try to write more alternates than
  // that for the same key. The write should either fail with
  // TooManyAlternates or the chain should be bounded.

  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(50 * 1024 * 1024);

  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  CacheKey key("chain-depth-limit-key");

  // Write the original entry first
  {
    std::string content_str = "Original content";
    std::vector<std::byte> content(content_str.size());
    std::memcpy(content.data(), content_str.data(), content_str.size());

    auto wh = cache->write_sync(key, content.size());
    REQUIRE(wh.has_value());
    wh->write_sync(std::span<const std::byte>(content));
    REQUIRE(wh->close_sync().has_value());
  }

  // Now write many alternates using custom alternate IDs (128+).
  // kMaxAlternates is 64, so after 64 total alternates the write should fail.
  int successful_writes = 0;
  bool got_too_many = false;

  for (int i = 1; i < 70; ++i) {
    auto alt_id = static_cast<AlternateId>(i);
    std::string content_str = "Alt content " + std::to_string(i);
    std::vector<std::byte> content(content_str.size());
    std::memcpy(content.data(), content_str.data(), content_str.size());

    auto wh = cache->write_alternate_sync(key, alt_id, content.size());
    if (!wh.has_value()) {
      if (wh.error() == CacheError::TooManyAlternates) {
        got_too_many = true;
      }
      break;
    }

    wh->write_sync(std::span<const std::byte>(content));
    auto close_result = wh->close_sync();
    if (!close_result.has_value()) {
      if (close_result.error() == CacheError::TooManyAlternates) {
        got_too_many = true;
      }
      break;
    }

    ++successful_writes;
  }

  // The 64th unique alternate should be rejected (original + 63 unique = 64 =
  // limit)
  REQUIRE(got_too_many);
  REQUIRE(successful_writes == 63);  // original(0) + 63 unique = 64 = limit

  // Verify the chain is still functional
  auto alts = cache->list_alternates_sync(key);
  REQUIRE(alts.has_value());
  REQUIRE(alts->size() <= Document::kMaxAlternates);

  cache->stop();
}

TEST_CASE("Duplicate alternate IDs do not inflate unique count",
          "[alternate][regression]") {
  // Simulates the nginx re-write storm scenario: multiple concurrent MISS
  // requests write the same default alternate ID many times.  The unique
  // alternate count should be 1 (not N), leaving room for worker variants.
  //
  // The superseded-alternate unlink is switched OFF here on purpose.  With it
  // on, the 10 duplicate writes collapse into ONE chain node and this case
  // would still pass while no longer building the deep duplicate chain it
  // names — testing nothing, silently.  Off, it keeps covering the
  // duplicates-vs-unique-count rule for the chains a consumer can still
  // produce with the mechanism disabled.

  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  config.unlink_superseded_alternates = false;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(50 * 1024 * 1024);

  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  CacheKey key("duplicate-id-test-key");

  // Write the same alternate ID (0xAA = 170, outside the 1-63 range) 10 times,
  // simulating concurrent nginx writes.
  auto dup_id = static_cast<AlternateId>(0xAA);
  for (int i = 0; i < 10; ++i) {
    std::string content_str = "Duplicate content " + std::to_string(i);
    std::vector<std::byte> content(content_str.size());
    std::memcpy(content.data(), content_str.data(), content_str.size());

    auto wh = cache->write_alternate_sync(key, dup_id, content.size());
    REQUIRE(wh.has_value());
    wh->write_sync(std::span<const std::byte>(content));
    REQUIRE(wh->close_sync().has_value());
  }

  // Now write 63 unique alternate IDs (1..63). All should succeed
  // because there's only 1 unique ID so far (0xAA), not 10.
  int successful_unique = 0;
  for (int i = 1; i <= 63; ++i) {
    auto alt_id = static_cast<AlternateId>(i);
    std::string content_str = "Unique alt " + std::to_string(i);
    std::vector<std::byte> content(content_str.size());
    std::memcpy(content.data(), content_str.data(), content_str.size());

    auto wh = cache->write_alternate_sync(key, alt_id, content.size());
    if (!wh.has_value()) break;
    wh->write_sync(std::span<const std::byte>(content));
    auto close_result = wh->close_sync();
    if (!close_result.has_value()) break;
    ++successful_unique;
  }

  // All 63 unique writes should have succeeded (total unique IDs = 64).
  // With unique counting, only unique IDs matter: 1 (0xAA) + 63 (1-63) = 64.
  REQUIRE(successful_unique == 63);

  // Negative test: the 65th unique ID should be rejected
  {
    auto overflow_id = static_cast<AlternateId>(64);
    std::string content_str = "Overflow alt";
    std::vector<std::byte> content(content_str.size());
    std::memcpy(content.data(), content_str.data(), content_str.size());

    auto wh = cache->write_alternate_sync(key, overflow_id, content.size());
    if (wh.has_value()) {
      wh->write_sync(std::span<const std::byte>(content));
      auto close_result = wh->close_sync();
      REQUIRE_FALSE(close_result.has_value());
      REQUIRE(close_result.error() == CacheError::TooManyAlternates);
    } else {
      REQUIRE(wh.error() == CacheError::TooManyAlternates);
    }
  }

  // Verify the chain is functional via list_alternates
  auto alts = cache->list_alternates_sync(key);
  REQUIRE(alts.has_value());

  cache->stop();
}

TEST_CASE("Massive duplicate writes do not block unique alternates",
          "[alternate][regression]") {
  // Write the same alternate ID 60 times (chain depth 60, unique count 1),
  // then write 63 unique IDs (1-63). All 63 must succeed.  Total chain
  // depth 123 < kMaxChainTraversalDepth (128), but well above the old
  // 64-node traversal limit that would have blocked these writes.
  //
  // Unlink OFF: with it on the 60 duplicates collapse to one node and the
  // 123-node chain this case is named for never exists (see the note on
  // "Duplicate alternate IDs do not inflate unique count").

  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  config.unlink_superseded_alternates = false;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(50 * 1024 * 1024);

  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  CacheKey key("massive-dup-test-key");

  // Write same alternate ID 60 times
  auto dup_id = static_cast<AlternateId>(0xBB);
  for (int i = 0; i < 60; ++i) {
    std::string content_str = "Dup " + std::to_string(i);
    std::vector<std::byte> content(content_str.size());
    std::memcpy(content.data(), content_str.data(), content_str.size());

    auto wh = cache->write_alternate_sync(key, dup_id, content.size());
    REQUIRE(wh.has_value());
    wh->write_sync(std::span<const std::byte>(content));
    REQUIRE(wh->close_sync().has_value());
  }

  // Now write 63 unique IDs — all should succeed (unique count = 1 + 63 = 64)
  int successful_unique = 0;
  for (int i = 1; i <= 63; ++i) {
    auto alt_id = static_cast<AlternateId>(i);
    std::string content_str = "Unique alt " + std::to_string(i);
    std::vector<std::byte> content(content_str.size());
    std::memcpy(content.data(), content_str.data(), content_str.size());

    auto wh = cache->write_alternate_sync(key, alt_id, content.size());
    if (!wh.has_value()) break;
    wh->write_sync(std::span<const std::byte>(content));
    auto close_result = wh->close_sync();
    if (!close_result.has_value()) break;
    ++successful_unique;
  }

  REQUIRE(successful_unique == 63);

  cache->stop();
}

TEST_CASE("Chain at exact traversal depth boundary succeeds",
          "[alternate][regression]") {
  // Write 65 duplicates of one ID + 63 unique IDs = 128 chain nodes
  // (exactly kMaxChainTraversalDepth).  All writes must succeed because
  // the unique count is only 64 and the chain terminates cleanly.
  //
  // Unlink OFF: this is the ONLY regression test that reaches the 128-node
  // traversal cap at all.  With the mechanism on, the 65 duplicates collapse
  // to one node, the chain tops out at 64, and this case would pass forever
  // without ever exercising the boundary it exists for.

  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  config.unlink_superseded_alternates = false;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(50 * 1024 * 1024);

  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  CacheKey key("boundary-128-test-key");

  // Write 65 duplicates of ID 0xCC
  auto dup_id = static_cast<AlternateId>(0xCC);
  for (int i = 0; i < 65; ++i) {
    std::string content_str = "Dup " + std::to_string(i);
    std::vector<std::byte> content(content_str.size());
    std::memcpy(content.data(), content_str.data(), content_str.size());

    auto wh = cache->write_alternate_sync(key, dup_id, content.size());
    REQUIRE(wh.has_value());
    wh->write_sync(std::span<const std::byte>(content));
    REQUIRE(wh->close_sync().has_value());
  }

  // Write 63 unique IDs (1-63) — total chain depth 128 =
  // kMaxChainTraversalDepth
  int successful_unique = 0;
  for (int i = 1; i <= 63; ++i) {
    auto alt_id = static_cast<AlternateId>(i);
    std::string content_str = "Unique alt " + std::to_string(i);
    std::vector<std::byte> content(content_str.size());
    std::memcpy(content.data(), content_str.data(), content_str.size());

    auto wh = cache->write_alternate_sync(key, alt_id, content.size());
    if (!wh.has_value()) break;
    wh->write_sync(std::span<const std::byte>(content));
    auto close_result = wh->close_sync();
    if (!close_result.has_value()) break;
    ++successful_unique;
  }

  REQUIRE(successful_unique == 63);

  // Verify chain is readable
  auto alts = cache->list_alternates_sync(key);
  REQUIRE(alts.has_value());

  cache->stop();
}

TEST_CASE("Bucket full directory behavior", "[directory][edge]") {
  // Create a Directory with a minimal bucket count (1 bucket = 4 entries).
  // Insert entries until the bucket is full.
  // Verify that the 5th insert still lands by evicting the entry nearest the
  // wrap cursor, and that the count does not grow.

  Directory dir(1);  // 1 bucket = kEntriesPerBucket (4) slots

  // We need 4 keys that all map to the same bucket (bucket 0, since there's
  // only 1 bucket). Since bucket_hash() % 1 == 0 for all keys, this is
  // guaranteed.
  std::vector<CacheKey> keys;
  keys.reserve(5);
  for (int i = 0; i < 5; ++i) {
    keys.emplace_back("bucket-full-" + std::to_string(i));
  }

  // Insert 4 entries - should all succeed (kEntriesPerBucket == 4)
  for (int i = 0; i < 4; ++i) {
    bool inserted =
        dir.insert(keys[i], static_cast<uint64_t>(i + 1) * 1000, 512);
    REQUIRE(inserted);
  }

  REQUIRE(dir.count() == 4);

  // The 5th insert lands by evicting a victim (no empty or stale slots).
  bool collision_evicted = false;
  bool bucket_full_evicted = false;
  bool fifth_inserted = dir.insert(keys[4], 5000, 512, Directory::kMatchAnyTag,
                                   &collision_evicted, &bucket_full_evicted);
  REQUIRE(fifth_inserted);

  // kMatchAnyTag updates any same-tag entry in place, so reaching the full
  // bucket at all proves no tag matched: the nearest-to-clobber path ran, and
  // it replaced a slot rather than adding one.
  REQUIRE_FALSE(collision_evicted);
  REQUIRE(bucket_full_evicted);
  REQUIRE(dir.count() == 4);

  // The new entry is findable at its offset.
  auto fifth = dir.probe(keys[4]);
  REQUIRE(fifth.has_value());
  REQUIRE(fifth->offset() == 5000);

  // The victim's slot is gone: no entry holds offset 1000 (keys[0], the entry
  // nearest the wrap cursor) any more.
  for (int i = 0; i < 5; ++i) {
    for (const auto &entry : dir.probe_all(keys[i])) {
      REQUIRE(entry.offset() != 1000);
    }
  }

  // Every non-victim entry is still findable via probe_all (entries may
  // collide on tag, so probe_all is the safe check).
  for (int i = 1; i < 4; ++i) {
    REQUIRE(!dir.probe_all(keys[i]).empty());
  }
}

// =============================================================================
// Phase 5C: Alternate chain removal and readability tests
//
// These tests exercise remove_alternate_sync to verify chain integrity
// after removing head or non-head alternates. They use the full Cache API
// because alternate chain operations require a running cache with disk I/O.
// =============================================================================

#include <cstdio>
#include <cstring>
#include <filesystem>

#include "cyclone/cache.hpp"
#include "cyclone/key.hpp"

TEST_CASE("Remove head alternate updates chain", "[alternate]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(10 * 1024 * 1024);

  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  CacheKey key("remove-head-alt-test");

  // Write two alternates: Original, then Brotli (head)
  std::string original_str = "Original content for head removal test";
  std::vector<std::byte> original_content(original_str.size());
  std::memcpy(original_content.data(), original_str.data(),
              original_str.size());

  {
    auto wh = cache->write_sync(key, original_content.size());
    REQUIRE(wh.has_value());
    wh->write_sync(std::span<const std::byte>(original_content));
    REQUIRE(wh->close_sync().has_value());
  }

  std::string brotli_str = "Brotli content for head removal test";
  std::vector<std::byte> brotli_content(brotli_str.size());
  std::memcpy(brotli_content.data(), brotli_str.data(), brotli_str.size());

  {
    auto wh = cache->write_alternate_sync(key, AlternateId::Brotli,
                                          brotli_content.size());
    REQUIRE(wh.has_value());
    wh->write_sync(std::span<const std::byte>(brotli_content));
    REQUIRE(wh->close_sync().has_value());
  }

  // Verify chain: Brotli (head) -> Original
  {
    auto alts = cache->list_alternates_sync(key);
    REQUIRE(alts.has_value());
    REQUIRE(alts->size() == 2);
    REQUIRE((*alts)[0].id == AlternateId::Brotli);
    REQUIRE((*alts)[1].id == AlternateId::Original);
  }

  // Remove the head (Brotli)
  {
    auto result = cache->remove_alternate_sync(key, AlternateId::Brotli);
    REQUIRE(result.has_value());
  }

  // Verify the second alternate (Original) is still readable
  {
    auto alts = cache->list_alternates_sync(key);
    REQUIRE(alts.has_value());
    REQUIRE(alts->size() == 1);
    REQUIRE((*alts)[0].id == AlternateId::Original);
    REQUIRE((*alts)[0].content_length == original_content.size());
  }

  // Also verify we can read the content back correctly
  {
    auto rh = cache->read_sync(key);
    REQUIRE(rh.has_value());
    auto content = rh->content();
    std::string read_str(reinterpret_cast<const char *>(content.data()),
                         content.size());
    REQUIRE(read_str == original_str);
  }

  cache->stop();
}

TEST_CASE("Remove non-head alternate preserves head", "[alternate]") {
  TempCacheDir tmp;
  std::string cache_path = tmp.path();

  CacheConfig config;
  auto cache_result = Cache::create(config);
  REQUIRE(cache_result.has_value());
  auto &cache = *cache_result;

  VolumeConfig vol_config;
  vol_config.path = cache_path;
  vol_config.size = static_cast<size_t>(10 * 1024 * 1024);

  REQUIRE(cache->add_volume(vol_config).has_value());
  REQUIRE(cache->start().has_value());

  CacheKey key("remove-nonhead-alt-test");

  // Write two alternates: Original, then Brotli (head)
  std::string original_str = "Original content for non-head removal test";
  std::vector<std::byte> original_content(original_str.size());
  std::memcpy(original_content.data(), original_str.data(),
              original_str.size());

  {
    auto wh = cache->write_sync(key, original_content.size());
    REQUIRE(wh.has_value());
    wh->write_sync(std::span<const std::byte>(original_content));
    REQUIRE(wh->close_sync().has_value());
  }

  std::string brotli_str = "Brotli content for non-head removal test";
  std::vector<std::byte> brotli_content(brotli_str.size());
  std::memcpy(brotli_content.data(), brotli_str.data(), brotli_str.size());

  {
    auto wh = cache->write_alternate_sync(key, AlternateId::Brotli,
                                          brotli_content.size());
    REQUIRE(wh.has_value());
    wh->write_sync(std::span<const std::byte>(brotli_content));
    REQUIRE(wh->close_sync().has_value());
  }

  // Verify chain: Brotli (head) -> Original
  {
    auto alts = cache->list_alternates_sync(key);
    REQUIRE(alts.has_value());
    REQUIRE(alts->size() == 2);
    REQUIRE((*alts)[0].id == AlternateId::Brotli);
    REQUIRE((*alts)[1].id == AlternateId::Original);
  }

  // Remove the non-head (Original)
  {
    auto result = cache->remove_alternate_sync(key, AlternateId::Original);
    REQUIRE(result.has_value());
  }

  // Verify the head (Brotli) is still readable
  {
    auto alts = cache->list_alternates_sync(key);
    REQUIRE(alts.has_value());
    REQUIRE(alts->size() == 1);
    REQUIRE((*alts)[0].id == AlternateId::Brotli);
    REQUIRE((*alts)[0].content_length == brotli_content.size());
  }

  // Read the remaining Brotli entry via read_sync
  {
    auto rh = cache->read_sync(key);
    REQUIRE(rh.has_value());
    auto content = rh->content();
    std::string read_str(reinterpret_cast<const char *>(content.data()),
                         content.size());
    REQUIRE(read_str == brotli_str);
  }

  cache->stop();
}

// =============================================================================
// Regression: in-place document-header updates must go through the mapping on
// Windows.
//
// WriteFile on a byte range that is also memory-mapped is NOT guaranteed
// coherent with the views.  On one Windows runner class the divergence is
// real whenever the volume file pre-existed via buffered stdio I/O -- exactly
// how every fixture here creates it: the fd write reported success while the
// file content (through the mapping, the same fd, and fresh handles) kept the
// old bytes, so a removed tail alternate stayed reachable and hit-count
// flushes were silently dropped.  remove_alternate_sync's chain repoint and
// update_hit_count_sync now write through the mapped view (like
// commit_write); this test drives both through a stdio-pre-created file and
// fails on such a machine without the fix.  Volume-level on purpose:
// update_hit_count_sync is not reachable deterministically through Cache.
// =============================================================================

#include <span>

#include "core/volume.hpp"

TEST_CASE("In-place header updates land on a stdio-pre-created volume file",
          "[alternate][regression]") {
  TempCacheDir tmp;
  std::string path = tmp.path();
  // This Windows regression specifically requires a volume file that was
  // pre-created via buffered stdio I/O; Volume is opened directly here (no
  // add_volume fingerprinting), so pre-create the raw path explicitly.
  {
    FILE *f = std::fopen(path.c_str(), "wb");
    REQUIRE(f != nullptr);
    std::fseek(f, static_cast<long>(10 * 1024 * 1024 - 1), SEEK_SET);
    std::fputc(0, f);
    std::fclose(f);
  }

  VolumeConfig config;
  config.path = path;
  config.size = static_cast<size_t>(10 * 1024 * 1024);

  Volume volume(config);
  REQUIRE(volume.open().has_value());

  CacheKey key("inplace-update-regression");
  std::vector<std::byte> original(64, std::byte{0x11});
  std::vector<std::byte> brotli(48, std::byte{0x22});

  {
    auto wh = volume.write_sync(key, original.size());
    REQUIRE(wh.has_value());
    REQUIRE(wh->write_sync(std::span<const std::byte>(original)).has_value());
    REQUIRE(wh->close_sync().has_value());
  }
  {
    auto wh =
        volume.write_alternate_sync(key, AlternateId::Brotli, brotli.size());
    REQUIRE(wh.has_value());
    REQUIRE(wh->write_sync(std::span<const std::byte>(brotli)).has_value());
    REQUIRE(wh->close_sync().has_value());
  }

  // Tail repoint: removing the non-head alternate rewrites the head's
  // next_alternate_offset in place.
  REQUIRE(volume.remove_alternate_sync(key, AlternateId::Original).has_value());
  {
    auto alts = volume.list_alternates_sync(key);
    REQUIRE(alts.has_value());
    REQUIRE(alts->size() == 1);
    REQUIRE((*alts)[0].id == AlternateId::Brotli);
  }

  // Hit-count flush: update_hit_count_sync rewrites hit_count + last_access
  // in place; the count must be observable through the mapped read path.
  REQUIRE(volume.update_hit_count_sync(key, AlternateId::Brotli, 5, 1234567)
              .has_value());
  {
    auto alts = volume.list_alternates_sync(key);
    REQUIRE(alts.has_value());
    REQUIRE(alts->size() == 1);
    REQUIRE((*alts)[0].hit_count == 5);
  }

  volume.close();
}
