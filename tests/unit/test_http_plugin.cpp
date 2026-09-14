// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstring>

#include "cyclone/plugin/alternate.hpp"
#include "cyclone/plugin/plugin.hpp"

#ifdef CYCLONE_HTTP_PLUGIN
#include "plugin/http/http_metadata.hpp"

namespace cyclone {
std::shared_ptr<CachePlugin> create_http_alternate_plugin();
}

using namespace cyclone;

TEST_CASE("HttpCacheAlt serialization round-trip", "[http]") {
  HttpCacheAlt alt;
  alt.set_request_method("GET");
  alt.set_request_url("http://example.com/path");
  alt.set_request_header("Accept", "text/html");
  alt.set_request_header("Accept-Encoding", "gzip, deflate");

  alt.set_status_code(200);
  alt.set_response_header("Content-Type", "text/html; charset=utf-8");
  alt.set_response_header("Cache-Control", "max-age=3600");
  alt.set_response_header("Content-Encoding", "gzip");

  auto now = std::time(nullptr);
  alt.set_request_time(now);
  alt.set_response_time(now);

  auto data = alt.serialize();
  REQUIRE(!data.empty());

  auto restored = HttpCacheAlt::deserialize(std::span<const std::byte>(data));

  REQUIRE(restored.request_method() == "GET");
  REQUIRE(restored.request_url() == "http://example.com/path");
  REQUIRE(restored.get_request_header("Accept").value_or("") == "text/html");
  REQUIRE(restored.get_request_header("Accept-Encoding").value_or("") ==
          "gzip, deflate");

  REQUIRE(restored.status_code() == 200);
  REQUIRE(restored.get_response_header("Content-Type").value_or("") ==
          "text/html; charset=utf-8");
  REQUIRE(restored.get_response_header("Cache-Control").value_or("") ==
          "max-age=3600");

  REQUIRE(restored.request_time() == now);
  REQUIRE(restored.response_time() == now);
}

TEST_CASE("HttpCacheAlt freshness calculation", "[http]") {
  HttpCacheAlt alt;
  auto now = std::time(nullptr);
  alt.set_response_time(now);
  alt.set_response_header("Cache-Control", "max-age=3600");

  REQUIRE(alt.is_fresh());

  auto max_age = alt.max_age();
  REQUIRE(max_age.has_value());
  REQUIRE(max_age->count() == 3600);
}

TEST_CASE("HttpCacheAlt stale entry", "[http]") {
  HttpCacheAlt alt;
  auto past = std::time(nullptr) - 7200;
  alt.set_response_time(past);
  alt.set_response_header("Cache-Control", "max-age=3600");

  REQUIRE_FALSE(alt.is_fresh());
}

TEST_CASE("HttpCacheAlt no Cache-Control defaults to fresh", "[http]") {
  HttpCacheAlt alt;
  alt.set_response_time(std::time(nullptr));

  REQUIRE(alt.is_fresh());
  REQUIRE_FALSE(alt.max_age().has_value());
}

TEST_CASE("HttpAlternatePlugin info", "[http]") {
  auto plugin = create_http_alternate_plugin();
  REQUIRE(plugin != nullptr);

  auto info = plugin->info();
  REQUIRE(info.name == "http-alternate");
  REQUIRE(info.version == "1.0.0");
  REQUIRE(info.plugin_id == HttpCacheAlt::kPluginId);
}

TEST_CASE("HttpAlternatePlugin select_variant with empty collection",
          "[http]") {
  auto plugin = create_http_alternate_plugin();
  VariantCollection variants;
  LookupContext ctx;

  auto result = plugin->select_variant(variants, ctx);
  REQUIRE_FALSE(result.has_value());
}

TEST_CASE("HttpAlternatePlugin select_variant with single variant", "[http]") {
  auto plugin = create_http_alternate_plugin();

  HttpCacheAlt alt;
  alt.set_request_method("GET");
  alt.set_response_header("Content-Type", "text/html");
  auto meta_data = alt.serialize();

  CacheVariant variant;
  variant.key = CacheKey("test-key");
  variant.metadata = Metadata(std::span<const std::byte>(meta_data));

  VariantCollection variants;
  variants.add(std::move(variant));

  LookupContext ctx;
  auto result = plugin->select_variant(variants, ctx);
  REQUIRE(result.has_value());
  REQUIRE(*result == 0);
}

TEST_CASE("HttpAlternatePlugin select_variant with Accept header", "[http]") {
  auto plugin = create_http_alternate_plugin();

  HttpCacheAlt alt1;
  alt1.set_response_header("Content-Type", "text/html");
  auto meta1 = alt1.serialize();

  HttpCacheAlt alt2;
  alt2.set_response_header("Content-Type", "application/json");
  auto meta2 = alt2.serialize();

  CacheVariant v1;
  v1.key = CacheKey("test-key-1");
  v1.metadata = Metadata(std::span<const std::byte>(meta1));

  CacheVariant v2;
  v2.key = CacheKey("test-key-2");
  v2.metadata = Metadata(std::span<const std::byte>(meta2));

  VariantCollection variants;
  variants.add(std::move(v1));
  variants.add(std::move(v2));

  std::string headers = "Accept: application/json\r\n";
  LookupContext ctx;
  ctx.request_headers = std::span<const std::byte>(
      reinterpret_cast<const std::byte *>(headers.data()), headers.size());

  auto result = plugin->select_variant(variants, ctx);
  REQUIRE(result.has_value());
  REQUIRE(*result == 1);
}

TEST_CASE("HttpAlternatePlugin check_freshness", "[http]") {
  auto plugin = create_http_alternate_plugin();

  HttpCacheAlt alt;
  alt.set_response_time(std::time(nullptr));
  alt.set_response_header("Cache-Control", "max-age=3600");
  auto meta = alt.serialize();

  CacheVariant variant;
  variant.key = CacheKey("test-key");
  variant.metadata = Metadata(std::span<const std::byte>(meta));

  FreshnessContext ctx;
  auto result = plugin->check_freshness(variant, ctx);
  REQUIRE(result == FreshnessResult::Fresh);
}

TEST_CASE("HttpAlternatePlugin check_freshness force revalidate", "[http]") {
  auto plugin = create_http_alternate_plugin();

  HttpCacheAlt alt;
  alt.set_response_time(std::time(nullptr));
  alt.set_response_header("Cache-Control", "max-age=3600");
  auto meta = alt.serialize();

  CacheVariant variant;
  variant.key = CacheKey("test-key");
  variant.metadata = Metadata(std::span<const std::byte>(meta));

  FreshnessContext ctx;
  ctx.force_revalidate = true;

  auto result = plugin->check_freshness(variant, ctx);
  REQUIRE(result == FreshnessResult::MustRevalidate);
}

TEST_CASE("HttpAlternatePlugin check_freshness stale", "[http]") {
  auto plugin = create_http_alternate_plugin();

  HttpCacheAlt alt;
  alt.set_response_time(std::time(nullptr) - 7200);
  alt.set_response_header("Cache-Control", "max-age=3600");
  auto meta = alt.serialize();

  CacheVariant variant;
  variant.key = CacheKey("test-key");
  variant.metadata = Metadata(std::span<const std::byte>(meta));

  FreshnessContext ctx;
  auto result = plugin->check_freshness(variant, ctx);
  REQUIRE(result == FreshnessResult::Stale);
}

TEST_CASE("HttpAlternatePlugin check_freshness must-revalidate", "[http]") {
  auto plugin = create_http_alternate_plugin();

  HttpCacheAlt alt;
  alt.set_response_time(std::time(nullptr) - 7200);
  alt.set_response_header("Cache-Control", "max-age=3600, must-revalidate");
  auto meta = alt.serialize();

  CacheVariant variant;
  variant.key = CacheKey("test-key");
  variant.metadata = Metadata(std::span<const std::byte>(meta));

  FreshnessContext ctx;
  auto result = plugin->check_freshness(variant, ctx);
  REQUIRE(result == FreshnessResult::MustRevalidate);
}

TEST_CASE("HttpAlternatePlugin Vary header matching", "[http]") {
  auto plugin = create_http_alternate_plugin();

  HttpCacheAlt alt;
  alt.set_request_header("Accept-Encoding", "gzip");
  alt.set_response_header("Vary", "Accept-Encoding");
  alt.set_response_header("Content-Type", "text/html");
  auto meta = alt.serialize();

  CacheVariant variant;
  variant.key = CacheKey("test-key");
  variant.metadata = Metadata(std::span<const std::byte>(meta));

  VariantCollection variants;
  variants.add(std::move(variant));

  std::string headers = "Accept-Encoding: gzip\r\n";
  LookupContext ctx;
  ctx.request_headers = std::span<const std::byte>(
      reinterpret_cast<const std::byte *>(headers.data()), headers.size());

  auto result = plugin->select_variant(variants, ctx);
  REQUIRE(result.has_value());
  REQUIRE(*result == 0);
}

TEST_CASE("HttpAlternatePlugin Vary header mismatch", "[http]") {
  auto plugin = create_http_alternate_plugin();

  HttpCacheAlt alt;
  alt.set_request_header("Accept-Encoding", "gzip");
  alt.set_response_header("Vary", "Accept-Encoding");
  alt.set_response_header("Content-Type", "text/html");
  auto meta = alt.serialize();

  CacheVariant variant;
  variant.key = CacheKey("test-key");
  variant.metadata = Metadata(std::span<const std::byte>(meta));

  VariantCollection variants;
  variants.add(std::move(variant));

  std::string headers = "Accept-Encoding: br\r\n";
  LookupContext ctx;
  ctx.request_headers = std::span<const std::byte>(
      reinterpret_cast<const std::byte *>(headers.data()), headers.size());

  auto result = plugin->select_variant(variants, ctx);
  REQUIRE_FALSE(result.has_value());
}

TEST_CASE("HttpAlternatePlugin Vary star rejects all", "[http]") {
  auto plugin = create_http_alternate_plugin();

  HttpCacheAlt alt;
  alt.set_response_header("Vary", "*");
  alt.set_response_header("Content-Type", "text/html");
  auto meta = alt.serialize();

  CacheVariant variant;
  variant.key = CacheKey("test-key");
  variant.metadata = Metadata(std::span<const std::byte>(meta));

  VariantCollection variants;
  variants.add(std::move(variant));

  LookupContext ctx;
  auto result = plugin->select_variant(variants, ctx);
  REQUIRE_FALSE(result.has_value());
}

TEST_CASE("HttpAlternatePlugin eviction_priority", "[http]") {
  auto plugin = create_http_alternate_plugin();

  CacheVariant hot_variant;
  hot_variant.key = CacheKey("hot-key");
  hot_variant.last_accessed = std::chrono::system_clock::now();
  hot_variant.hit_count = 100;

  CacheVariant cold_variant;
  cold_variant.key = CacheKey("cold-key");
  cold_variant.last_accessed =
      std::chrono::system_clock::now() - std::chrono::hours(24);
  cold_variant.hit_count = 1;

  double hot_priority = plugin->eviction_priority(hot_variant);
  double cold_priority = plugin->eviction_priority(cold_variant);

  REQUIRE(cold_priority > hot_priority);
}

TEST_CASE("HTTP alternate Accept with multiple ranges and q-values",
          "[http][alternate]") {
  auto plugin = create_http_alternate_plugin();

  // Variant 0: text/html
  HttpCacheAlt alt1;
  alt1.set_response_header("Content-Type", "text/html");
  auto meta1 = alt1.serialize();

  // Variant 1: application/json
  HttpCacheAlt alt2;
  alt2.set_response_header("Content-Type", "application/json");
  auto meta2 = alt2.serialize();

  // Variant 2: text/plain
  HttpCacheAlt alt3;
  alt3.set_response_header("Content-Type", "text/plain");
  auto meta3 = alt3.serialize();

  CacheVariant v1;
  v1.key = CacheKey("accept-q-key-1");
  v1.metadata = Metadata(std::span<const std::byte>(meta1));

  CacheVariant v2;
  v2.key = CacheKey("accept-q-key-2");
  v2.metadata = Metadata(std::span<const std::byte>(meta2));

  CacheVariant v3;
  v3.key = CacheKey("accept-q-key-3");
  v3.metadata = Metadata(std::span<const std::byte>(meta3));

  VariantCollection variants;
  variants.add(std::move(v1));
  variants.add(std::move(v2));
  variants.add(std::move(v3));

  // Request prefers application/json (q=1.0) over text/html (q=0.5) over
  // text/plain (q=0.1)
  std::string headers =
      "Accept: text/html;q=0.5, application/json;q=1.0, text/plain;q=0.1\r\n";
  LookupContext ctx;
  ctx.request_headers = std::span<const std::byte>(
      reinterpret_cast<const std::byte *>(headers.data()), headers.size());

  auto result = plugin->select_variant(variants, ctx);
  REQUIRE(result.has_value());
  // application/json (index 1) has q=1.0, the highest
  REQUIRE(*result == 1);
}

TEST_CASE("HTTP alternate Accept wildcard matching", "[http][alternate]") {
  auto plugin = create_http_alternate_plugin();

  // Variant 0: image/png
  HttpCacheAlt alt1;
  alt1.set_response_header("Content-Type", "image/png");
  auto meta1 = alt1.serialize();

  // Variant 1: image/jpeg
  HttpCacheAlt alt2;
  alt2.set_response_header("Content-Type", "image/jpeg");
  auto meta2 = alt2.serialize();

  // Variant 2: text/html
  HttpCacheAlt alt3;
  alt3.set_response_header("Content-Type", "text/html");
  auto meta3 = alt3.serialize();

  CacheVariant v1;
  v1.key = CacheKey("wildcard-key-1");
  v1.metadata = Metadata(std::span<const std::byte>(meta1));

  CacheVariant v2;
  v2.key = CacheKey("wildcard-key-2");
  v2.metadata = Metadata(std::span<const std::byte>(meta2));

  CacheVariant v3;
  v3.key = CacheKey("wildcard-key-3");
  v3.metadata = Metadata(std::span<const std::byte>(meta3));

  // Test type/* wildcard: "image/*" should match image/png and image/jpeg
  {
    VariantCollection variants;
    variants.add(CacheVariant{CacheKey("wc-1"),
                              Metadata(std::span<const std::byte>(meta1)),
                              0,
                              {},
                              {},
                              0});
    variants.add(CacheVariant{CacheKey("wc-2"),
                              Metadata(std::span<const std::byte>(meta2)),
                              0,
                              {},
                              {},
                              0});
    variants.add(CacheVariant{CacheKey("wc-3"),
                              Metadata(std::span<const std::byte>(meta3)),
                              0,
                              {},
                              {},
                              0});

    std::string headers = "Accept: image/*;q=0.8, text/html;q=0.5\r\n";
    LookupContext ctx;
    ctx.request_headers = std::span<const std::byte>(
        reinterpret_cast<const std::byte *>(headers.data()), headers.size());

    auto result = plugin->select_variant(variants, ctx);
    REQUIRE(result.has_value());
    // image/* matches indices 0 and 1 with q=0.8; text/html matches index 2
    // with q=0.5 First image type (index 0) should win
    REQUIRE(*result == 0);
  }

  // Test */* wildcard: should match everything
  {
    VariantCollection variants;
    variants.add(CacheVariant{CacheKey("wc-a"),
                              Metadata(std::span<const std::byte>(meta3)),
                              0,
                              {},
                              {},
                              0});

    std::string headers = "Accept: */*\r\n";
    LookupContext ctx;
    ctx.request_headers = std::span<const std::byte>(
        reinterpret_cast<const std::byte *>(headers.data()), headers.size());

    auto result = plugin->select_variant(variants, ctx);
    REQUIRE(result.has_value());
    REQUIRE(*result == 0);
  }
}

TEST_CASE("HTTP alternate Accept-Language selection", "[http][alternate]") {
  auto plugin = create_http_alternate_plugin();

  // Variant 0: English
  HttpCacheAlt alt1;
  alt1.set_response_header("Content-Type", "text/html");
  alt1.set_response_header("Content-Language", "en");
  auto meta1 = alt1.serialize();

  // Variant 1: French
  HttpCacheAlt alt2;
  alt2.set_response_header("Content-Type", "text/html");
  alt2.set_response_header("Content-Language", "fr");
  auto meta2 = alt2.serialize();

  // Variant 2: German
  HttpCacheAlt alt3;
  alt3.set_response_header("Content-Type", "text/html");
  alt3.set_response_header("Content-Language", "de");
  auto meta3 = alt3.serialize();

  VariantCollection variants;
  variants.add(CacheVariant{CacheKey("lang-1"),
                            Metadata(std::span<const std::byte>(meta1)),
                            0,
                            {},
                            {},
                            0});
  variants.add(CacheVariant{CacheKey("lang-2"),
                            Metadata(std::span<const std::byte>(meta2)),
                            0,
                            {},
                            {},
                            0});
  variants.add(CacheVariant{CacheKey("lang-3"),
                            Metadata(std::span<const std::byte>(meta3)),
                            0,
                            {},
                            {},
                            0});

  // Request prefers French
  std::string headers = "Accept-Language: fr;q=1.0, en;q=0.5, de;q=0.3\r\n";
  LookupContext ctx;
  ctx.request_headers = std::span<const std::byte>(
      reinterpret_cast<const std::byte *>(headers.data()), headers.size());

  auto result = plugin->select_variant(variants, ctx);
  REQUIRE(result.has_value());
  // French (index 1) has q=1.0
  REQUIRE(*result == 1);
}

TEST_CASE("HTTP alternate Accept-Language prefix matching",
          "[http][alternate]") {
  auto plugin = create_http_alternate_plugin();

  // Variant 0: en-US
  HttpCacheAlt alt1;
  alt1.set_response_header("Content-Type", "text/html");
  alt1.set_response_header("Content-Language", "en-US");
  auto meta1 = alt1.serialize();

  // Variant 1: en-GB
  HttpCacheAlt alt2;
  alt2.set_response_header("Content-Type", "text/html");
  alt2.set_response_header("Content-Language", "en-GB");
  auto meta2 = alt2.serialize();

  // Variant 2: fr
  HttpCacheAlt alt3;
  alt3.set_response_header("Content-Type", "text/html");
  alt3.set_response_header("Content-Language", "fr");
  auto meta3 = alt3.serialize();

  VariantCollection variants;
  variants.add(CacheVariant{CacheKey("prefix-1"),
                            Metadata(std::span<const std::byte>(meta1)),
                            0,
                            {},
                            {},
                            0});
  variants.add(CacheVariant{CacheKey("prefix-2"),
                            Metadata(std::span<const std::byte>(meta2)),
                            0,
                            {},
                            {},
                            0});
  variants.add(CacheVariant{CacheKey("prefix-3"),
                            Metadata(std::span<const std::byte>(meta3)),
                            0,
                            {},
                            {},
                            0});

  // Request "en" should match "en-US" and "en-GB" via prefix matching
  std::string headers = "Accept-Language: en;q=0.9, fr;q=0.5\r\n";
  LookupContext ctx;
  ctx.request_headers = std::span<const std::byte>(
      reinterpret_cast<const std::byte *>(headers.data()), headers.size());

  auto result = plugin->select_variant(variants, ctx);
  REQUIRE(result.has_value());
  // "en" matches both "en-US" (index 0) and "en-GB" (index 1) with q=0.9
  // "fr" matches "fr" (index 2) with q=0.5
  // First match (index 0) should win since both have same quality
  REQUIRE(*result == 0);
}

TEST_CASE("HTTP alternate empty Accept headers", "[http][alternate]") {
  auto plugin = create_http_alternate_plugin();

  // Variant 0: text/html
  HttpCacheAlt alt1;
  alt1.set_response_header("Content-Type", "text/html");
  auto meta1 = alt1.serialize();

  // Variant 1: application/json
  HttpCacheAlt alt2;
  alt2.set_response_header("Content-Type", "application/json");
  auto meta2 = alt2.serialize();

  VariantCollection variants;
  variants.add(CacheVariant{CacheKey("empty-accept-1"),
                            Metadata(std::span<const std::byte>(meta1)),
                            0,
                            {},
                            {},
                            0});
  variants.add(CacheVariant{CacheKey("empty-accept-2"),
                            Metadata(std::span<const std::byte>(meta2)),
                            0,
                            {},
                            {},
                            0});

  // Empty request headers - no Accept, no Accept-Language, no Accept-Encoding
  LookupContext ctx;
  // No request_headers set, defaults to empty span

  auto result = plugin->select_variant(variants, ctx);
  REQUIRE(result.has_value());
  // With no Accept headers, all variants have equal quality (1.0)
  // First variant (index 0) should be selected
  REQUIRE(*result == 0);
}

TEST_CASE("Malformed q-values must not throw or crash", "[http][security]") {
  auto plugin = create_http_alternate_plugin();

  HttpCacheAlt alt;
  alt.set_response_header("Content-Type", "text/html");
  alt.set_response_header("Content-Encoding", "gzip");
  alt.set_response_header("Content-Language", "en");
  auto meta = alt.serialize();

  auto make_variants = [&]() {
    VariantCollection variants;
    variants.add(CacheVariant{CacheKey("qval-key"),
                              Metadata(std::span<const std::byte>(meta)),
                              0,
                              {},
                              {},
                              0});
    return variants;
  };

  // Accept with non-numeric q-value — previously threw std::invalid_argument
  SECTION("Accept: non-numeric q-value") {
    std::string headers = "Accept: text/html;q=NOTANUMBER\r\n";
    LookupContext ctx;
    ctx.request_headers = std::span<const std::byte>(
        reinterpret_cast<const std::byte *>(headers.data()), headers.size());
    auto variants = make_variants();
    REQUIRE_NOTHROW(plugin->select_variant(variants, ctx));
  }

  // Accept with overflowing q-value — previously threw std::out_of_range
  SECTION("Accept: overflowing q-value") {
    std::string headers =
        "Accept: text/html;q=999999999999999999999999999999\r\n";
    LookupContext ctx;
    ctx.request_headers = std::span<const std::byte>(
        reinterpret_cast<const std::byte *>(headers.data()), headers.size());
    auto variants = make_variants();
    REQUIRE_NOTHROW(plugin->select_variant(variants, ctx));
  }

  // Accept-Encoding with non-numeric q-value
  SECTION("Accept-Encoding: non-numeric q-value") {
    std::string headers = "Accept-Encoding: gzip;q=abc\r\n";
    LookupContext ctx;
    ctx.request_headers = std::span<const std::byte>(
        reinterpret_cast<const std::byte *>(headers.data()), headers.size());
    auto variants = make_variants();
    REQUIRE_NOTHROW(plugin->select_variant(variants, ctx));
  }

  // Accept-Language with non-numeric q-value
  SECTION("Accept-Language: non-numeric q-value") {
    std::string headers = "Accept-Language: en;q=???\r\n";
    LookupContext ctx;
    ctx.request_headers = std::span<const std::byte>(
        reinterpret_cast<const std::byte *>(headers.data()), headers.size());
    auto variants = make_variants();
    REQUIRE_NOTHROW(plugin->select_variant(variants, ctx));
  }

  // Empty q-value after ;q=
  SECTION("Accept: empty q-value") {
    std::string headers = "Accept: text/html;q=\r\n";
    LookupContext ctx;
    ctx.request_headers = std::span<const std::byte>(
        reinterpret_cast<const std::byte *>(headers.data()), headers.size());
    auto variants = make_variants();
    REQUIRE_NOTHROW(plugin->select_variant(variants, ctx));
  }

  // Negative q-value — should clamp to valid range
  SECTION("Accept: negative q-value") {
    std::string headers = "Accept: text/html;q=-1.0\r\n";
    LookupContext ctx;
    ctx.request_headers = std::span<const std::byte>(
        reinterpret_cast<const std::byte *>(headers.data()), headers.size());
    auto variants = make_variants();
    REQUIRE_NOTHROW(plugin->select_variant(variants, ctx));
  }

  // q-value with trailing non-digit characters — parser should stop at the
  // first non-digit
  SECTION("Accept: q-value with trailing garbage") {
    std::string headers = "Accept: text/html;q=0.5xyz\r\n";
    LookupContext ctx;
    ctx.request_headers = std::span<const std::byte>(
        reinterpret_cast<const std::byte *>(headers.data()), headers.size());
    auto variants = make_variants();
    REQUIRE_NOTHROW(plugin->select_variant(variants, ctx));
  }

  // Valid q-value still works correctly
  SECTION("Accept: valid q=0.8 still works") {
    std::string headers = "Accept: text/html;q=0.8\r\n";
    LookupContext ctx;
    ctx.request_headers = std::span<const std::byte>(
        reinterpret_cast<const std::byte *>(headers.data()), headers.size());
    auto variants = make_variants();
    auto result = plugin->select_variant(variants, ctx);
    REQUIRE(result.has_value());
    REQUIRE(*result == 0);
  }
}

TEST_CASE("max-age integer overflow must not cause UB", "[http][security]") {
  // signed integer overflow in max-age parsing
  HttpCacheAlt alt;
  alt.set_response_time(std::time(nullptr));
  alt.set_response_header("Cache-Control", "max-age=99999999999999999999");

  // Must not crash or exhibit UB — should clamp to 1 year (31536000s) per RFC
  // 7234 §5.2.2.8
  auto ma = alt.max_age();
  REQUIRE(ma.has_value());
  REQUIRE(ma->count() == 31536000);
}

TEST_CASE("Crafted header count must not cause CPU DoS in deserialize",
          "[http][security]") {
  // A crafted serialized blob with count=0xFFFFFFFF in the header
  // map causes ~4 billion loop iterations (CPU denial of service).
  // After the fix, deserialize must return quickly with an empty/partial
  // result.

  // Build a minimal serialized blob: empty method, empty url, then a header
  // count of 0xFFFFFFFF for request_headers.
  std::vector<std::byte> crafted;

  // write_string("") — method
  uint32_t zero = 0;
  crafted.resize(crafted.size() + sizeof(zero));
  std::memcpy(crafted.data(), &zero, sizeof(zero));

  // write_string("") — url
  crafted.resize(crafted.size() + sizeof(zero));
  std::memcpy(crafted.data() + sizeof(zero), &zero, sizeof(zero));

  // Malicious header count
  uint32_t huge_count = 0xFFFFFFFF;
  crafted.resize(crafted.size() + sizeof(huge_count));
  std::memcpy(crafted.data() + 2 * sizeof(zero), &huge_count,
              sizeof(huge_count));

  auto start = std::chrono::steady_clock::now();
  auto alt = HttpCacheAlt::deserialize(std::span<const std::byte>(crafted));
  auto elapsed = std::chrono::steady_clock::now() - start;

  // Must complete in well under 1 second (was previously ~seconds with 4B
  // iterations)
  REQUIRE(elapsed < std::chrono::milliseconds(100));
}

#endif  // CYCLONE_HTTP_PLUGIN
