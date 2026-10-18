// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

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

  auto restored_opt =
      HttpCacheAlt::deserialize(std::span<const std::byte>(data));
  REQUIRE(restored_opt.has_value());
  const auto &restored = *restored_opt;

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

TEST_CASE("HttpCacheAlt future response time is never fresh", "[http]") {
  HttpCacheAlt alt;
  alt.set_response_time(std::time(nullptr) + 86400);
  alt.set_response_header("Cache-Control", "max-age=60");

  REQUIRE(alt.age().count() >= 0);
  REQUIRE_FALSE(alt.is_fresh());

  auto plugin = create_http_alternate_plugin();
  auto meta = alt.serialize();
  CacheVariant variant{CacheKey("future"),
                       Metadata(std::span<const std::byte>(meta)),
                       0,
                       {},
                       {},
                       0};
  FreshnessContext fctx;
  REQUIRE(plugin->check_freshness(variant, fctx) != FreshnessResult::Fresh);
}

TEST_CASE("HttpCacheAlt stale entry", "[http]") {
  HttpCacheAlt alt;
  auto past = std::time(nullptr) - 7200;
  alt.set_response_time(past);
  alt.set_response_header("Cache-Control", "max-age=3600");

  REQUIRE_FALSE(alt.is_fresh());
}

TEST_CASE("HttpCacheAlt no freshness lifetime is stale", "[http]") {
  HttpCacheAlt alt;
  alt.set_response_time(std::time(nullptr));

  REQUIRE_FALSE(alt.is_fresh());
  REQUIRE_FALSE(alt.max_age().has_value());

  // Cache-Control without max-age gives no lifetime either.
  alt.set_response_header("Cache-Control", "public");
  REQUIRE_FALSE(alt.is_fresh());
}

namespace {
std::string http_date(std::time_t t) {
  std::tm *tm = std::gmtime(&t);
  char buf[64];
  std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", tm);
  return buf;
}
}  // namespace

TEST_CASE("HttpCacheAlt max-age directive parsing", "[http]") {
  auto now = std::time(nullptr);
  HttpCacheAlt alt;
  alt.set_response_time(now);

  SECTION("directive name is case-insensitive") {
    alt.set_response_header("Cache-Control", "public, MAX-AGE=60");
    REQUIRE(alt.max_age().has_value());
    REQUIRE(alt.max_age()->count() == 60);
    REQUIRE(alt.is_fresh());
  }
  SECTION("directive is matched as a whole token") {
    alt.set_response_header("Cache-Control", "x-max-age=60");
    REQUIRE_FALSE(alt.max_age().has_value());
    REQUIRE_FALSE(alt.is_fresh());
  }
  SECTION("s-maxage alone is not max-age") {
    alt.set_response_header("Cache-Control", "s-maxage=60");
    REQUIRE_FALSE(alt.max_age().has_value());
  }
  SECTION("max-age=0 is stale") {
    alt.set_response_header("Cache-Control", "max-age=0");
    REQUIRE_FALSE(alt.is_fresh());
  }
}

TEST_CASE("HttpCacheAlt age includes the Age header", "[http]") {
  auto now = std::time(nullptr);

  SECTION("Age beyond max-age is stale") {
    HttpCacheAlt alt;
    alt.set_response_time(now);
    alt.set_response_header("Cache-Control", "max-age=60");
    alt.set_response_header("Age", "3600");
    REQUIRE(alt.age().count() >= 3600);
    REQUIRE_FALSE(alt.is_fresh());
  }
  SECTION("small Age stays fresh") {
    HttpCacheAlt alt;
    alt.set_response_time(now);
    alt.set_response_header("Cache-Control", "max-age=600");
    alt.set_response_header("Age", "10");
    REQUIRE(alt.is_fresh());
  }
  SECTION("invalid Age is ignored") {
    HttpCacheAlt alt;
    alt.set_response_time(now);
    alt.set_response_header("Cache-Control", "max-age=600");
    alt.set_response_header("Age", "-5");
    REQUIRE(alt.age().count() < 5);
    alt.set_response_header("Age", "abc");
    REQUIRE(alt.age().count() < 5);
    REQUIRE(alt.is_fresh());
  }
  SECTION("response delay is added to Age") {
    HttpCacheAlt alt;
    alt.set_request_time(now - 100);
    alt.set_response_time(now);
    alt.set_response_header("Cache-Control", "max-age=600");
    alt.set_response_header("Age", "550");
    // 550 + 100 seconds of response delay exceeds the 600 second lifetime.
    REQUIRE_FALSE(alt.is_fresh());
  }
  SECTION("larger of apparent age and corrected Age wins") {
    HttpCacheAlt alt;
    alt.set_response_time(now);
    alt.set_response_header("Date", http_date(now - 1000));
    alt.set_response_header("Age", "5");
    alt.set_response_header("Cache-Control", "max-age=600");
    REQUIRE(alt.age().count() >= 1000);
  }
}

TEST_CASE("HttpCacheAlt s-maxage takes precedence", "[http]") {
  auto now = std::time(nullptr);

  SECTION("s-maxage overrides max-age and Expires") {
    HttpCacheAlt alt;
    alt.set_response_time(now - 120);
    alt.set_response_header("Cache-Control", "max-age=3600, s-maxage=60");
    alt.set_response_header("Expires", http_date(now + 3600));
    REQUIRE(alt.freshness_lifetime().has_value());
    REQUIRE(alt.freshness_lifetime()->count() == 60);
    REQUIRE_FALSE(alt.is_fresh());
  }
  SECTION("longer s-maxage keeps the entry fresh") {
    HttpCacheAlt alt;
    alt.set_response_time(now - 120);
    alt.set_response_header("Cache-Control", "S-MAXAGE=600, max-age=60");
    REQUIRE(alt.is_fresh());
  }
  SECTION("s-maxage alone gives a lifetime") {
    HttpCacheAlt alt;
    alt.set_response_time(now);
    alt.set_response_header("Cache-Control", "s-maxage=60");
    REQUIRE(alt.freshness_lifetime()->count() == 60);
    REQUIRE_FALSE(alt.max_age().has_value());
  }
}

TEST_CASE("HttpCacheAlt accepts all HTTP date formats", "[http]") {
  // Response received at Sun, 06 Nov 1994 08:49:37 GMT.
  const std::time_t base = 784111777;
  auto lifetime = [&](const char *expires) {
    HttpCacheAlt alt;
    alt.set_response_time(base);
    alt.set_response_header("Expires", expires);
    return alt.freshness_lifetime();
  };

  SECTION("IMF-fixdate") {
    auto l = lifetime("Sun, 06 Nov 1994 09:49:37 GMT");
    REQUIRE(l.has_value());
    REQUIRE(l->count() == 3600);
  }
  SECTION("obsolete RFC 850 date") {
    auto l = lifetime("Sunday, 06-Nov-94 09:49:37 GMT");
    REQUIRE(l.has_value());
    REQUIRE(l->count() == 3600);
  }
  SECTION("asctime date") {
    auto l = lifetime("Sun Nov  6 09:49:37 1994");
    REQUIRE(l.has_value());
    REQUIRE(l->count() == 3600);
    l = lifetime("Sun Nov 16 09:49:37 1994");
    REQUIRE(l.has_value());
    REQUIRE(l->count() == 3600 + 10 * 86400);
  }
  SECTION("two digit years within fifty years of now map to this century") {
    auto l = lifetime("Sunday, 06-Nov-94 09:49:37 GMT");
    REQUIRE(l->count() == 3600);  // 94 reads as 1994, not 2094
  }
  SECTION("garbage is rejected") {
    REQUIRE(lifetime("Sun, 06 Nov 1994 09:49:37 PST")->count() == 0);
    REQUIRE(lifetime("Sunday, 06-Foo-94 09:49:37 GMT")->count() == 0);
    REQUIRE(lifetime("Sun Nov  6 09:49:37")->count() == 0);
  }
}

TEST_CASE("HttpCacheAlt honours Expires", "[http]") {
  auto now = std::time(nullptr);

  SECTION("future Expires is fresh") {
    HttpCacheAlt alt;
    alt.set_response_time(now);
    alt.set_response_header("Expires", http_date(now + 3600));
    REQUIRE(alt.is_fresh());
  }
  SECTION("past Expires is stale") {
    HttpCacheAlt alt;
    alt.set_response_time(now - 7200);
    alt.set_response_header("Expires", http_date(now - 3600));
    REQUIRE_FALSE(alt.is_fresh());
  }
  SECTION("invalid Expires is stale") {
    HttpCacheAlt alt;
    alt.set_response_time(now);
    alt.set_response_header("Expires", "0");
    REQUIRE_FALSE(alt.is_fresh());
  }
  SECTION("max-age wins over Expires") {
    HttpCacheAlt alt;
    alt.set_response_time(now);
    alt.set_response_header("Expires", http_date(now - 3600));
    alt.set_response_header("Cache-Control", "max-age=600");
    REQUIRE(alt.is_fresh());
  }
  SECTION("Expires is measured from the Date header") {
    HttpCacheAlt alt;
    // Response stored now but generated two hours ago with a one hour life.
    alt.set_response_time(now);
    alt.set_response_header("Date", http_date(now - 7200));
    alt.set_response_header("Expires", http_date(now - 3600));
    REQUIRE_FALSE(alt.is_fresh());
  }
}

TEST_CASE("HttpCacheAlt heuristic freshness needs Last-Modified", "[http]") {
  auto now = std::time(nullptr);
  constexpr std::time_t kDay = 86400;

  SECTION("recent response of an old resource is fresh") {
    HttpCacheAlt alt;
    alt.set_status_code(200);
    alt.set_response_time(now);
    alt.set_response_header("Last-Modified", http_date(now - 100 * kDay));
    REQUIRE(alt.is_fresh());
  }
  SECTION("response older than a tenth of the resource age is stale") {
    HttpCacheAlt alt;
    alt.set_status_code(200);
    alt.set_response_time(now - 20 * kDay);
    alt.set_response_header("Date", http_date(now - 20 * kDay));
    alt.set_response_header("Last-Modified", http_date(now - 120 * kDay));
    // Resource was 100 days old at response time: heuristic life is 10 days.
    REQUIRE_FALSE(alt.is_fresh());
  }
  SECTION("only statuses cacheable by default get a heuristic") {
    for (uint16_t code :
         {200, 203, 204, 206, 300, 301, 308, 404, 405, 410, 414, 501}) {
      HttpCacheAlt alt;
      alt.set_status_code(code);
      alt.set_response_time(now);
      alt.set_response_header("Last-Modified", http_date(now - 100 * kDay));
      INFO("status " << code);
      REQUIRE(alt.freshness_lifetime().has_value());
    }
    for (uint16_t code : {0, 201, 202, 302, 303, 307, 400, 403, 500, 503}) {
      HttpCacheAlt alt;
      alt.set_status_code(code);
      alt.set_response_time(now);
      alt.set_response_header("Last-Modified", http_date(now - 100 * kDay));
      INFO("status " << code);
      REQUIRE_FALSE(alt.freshness_lifetime().has_value());
      REQUIRE_FALSE(alt.is_fresh());
    }
  }
  SECTION("no Last-Modified gives no heuristic") {
    HttpCacheAlt alt;
    alt.set_response_time(now);
    REQUIRE_FALSE(alt.is_fresh());
  }
}

TEST_CASE("HttpCacheAlt header names are case-insensitive", "[http]") {
  HttpCacheAlt alt;
  alt.set_response_time(std::time(nullptr));
  alt.set_response_header("cache-control", "max-age=3600");
  alt.set_response_header("content-type", "text/html");
  alt.set_request_header("ACCEPT-ENCODING", "gzip");

  REQUIRE(alt.get_response_header("Cache-Control").value_or("") ==
          "max-age=3600");
  REQUIRE(alt.get_response_header("CONTENT-TYPE").value_or("") == "text/html");
  REQUIRE(alt.get_request_header("accept-encoding").value_or("") == "gzip");
  REQUIRE(alt.max_age().has_value());

  // A later set under a different case replaces the earlier value.
  alt.set_response_header("Cache-Control", "max-age=60");
  REQUIRE(alt.get_response_header("cache-control").value_or("") ==
          "max-age=60");

  auto data = alt.serialize();
  auto restored_opt =
      HttpCacheAlt::deserialize(std::span<const std::byte>(data));
  REQUIRE(restored_opt.has_value());
  const auto &restored = *restored_opt;
  REQUIRE(restored.get_response_header("CACHE-CONTROL").value_or("") ==
          "max-age=60");
  REQUIRE(restored.get_request_header("Accept-Encoding").value_or("") ==
          "gzip");
}

TEST_CASE("HttpAlternatePlugin Vary matches names regardless of case",
          "[http]") {
  auto plugin = create_http_alternate_plugin();

  HttpCacheAlt alt;
  alt.set_response_header("vary", "accept-encoding");
  alt.set_request_header("accept-encoding", "gzip");
  auto meta = alt.serialize();

  VariantCollection variants;
  variants.add(CacheVariant{CacheKey("vary-case"),
                            Metadata(std::span<const std::byte>(meta)),
                            0,
                            {},
                            {},
                            0});

  std::string same = "Accept-Encoding: gzip\r\n";
  LookupContext ctx;
  ctx.request_headers = std::span<const std::byte>(
      reinterpret_cast<const std::byte *>(same.data()), same.size());
  REQUIRE(plugin->select_variant(variants, ctx).has_value());

  std::string other = "Accept-Encoding: br\r\n";
  ctx.request_headers = std::span<const std::byte>(
      reinterpret_cast<const std::byte *>(other.data()), other.size());
  REQUIRE_FALSE(plugin->select_variant(variants, ctx).has_value());
}

TEST_CASE("HttpCacheAlt deserialize rejects truncated input", "[http]") {
  HttpCacheAlt alt;
  alt.set_request_method("GET");
  alt.set_request_url("http://example.com/path");
  alt.set_request_header("Accept", "text/html");
  alt.set_status_code(200);
  alt.set_response_header("Cache-Control", "max-age=3600");
  alt.set_response_time(std::time(nullptr));
  auto data = alt.serialize();

  REQUIRE(
      HttpCacheAlt::deserialize(std::span<const std::byte>(data)).has_value());

  // Every strict prefix of a valid blob must be rejected as a whole.
  for (size_t n = 0; n < data.size(); ++n) {
    auto prefix =
        HttpCacheAlt::deserialize(std::span<const std::byte>(data.data(), n));
    INFO("prefix length " << n);
    REQUIRE_FALSE(prefix.has_value());
  }

  // Trailing bytes are not part of a well-formed blob either.
  auto longer = data;
  longer.push_back(std::byte{0});
  REQUIRE_FALSE(HttpCacheAlt::deserialize(std::span<const std::byte>(longer))
                    .has_value());
}

TEST_CASE("HttpAlternatePlugin ignores variants with undecodable metadata",
          "[http]") {
  auto plugin = create_http_alternate_plugin();

  HttpCacheAlt alt;
  alt.set_response_header("Content-Type", "text/html");
  alt.set_response_header("Cache-Control", "max-age=3600");
  alt.set_response_time(std::time(nullptr));
  auto good = alt.serialize();
  auto bad = good;
  bad.resize(bad.size() - 3);

  VariantCollection variants;
  variants.add(CacheVariant{CacheKey("bad"),
                            Metadata(std::span<const std::byte>(bad)),
                            0,
                            {},
                            {},
                            0});
  variants.add(CacheVariant{CacheKey("good"),
                            Metadata(std::span<const std::byte>(good)),
                            0,
                            {},
                            {},
                            0});

  LookupContext ctx;
  auto picked = plugin->select_variant(variants, ctx);
  REQUIRE(picked.has_value());
  REQUIRE(*picked == 1);

  FreshnessContext fctx;
  REQUIRE(plugin->check_freshness(variants[0], fctx) == FreshnessResult::Error);
  REQUIRE(plugin->check_freshness(variants[1], fctx) == FreshnessResult::Fresh);
}

TEST_CASE("HttpAlternatePlugin does not select unacceptable variants",
          "[http][alternate]") {
  auto plugin = create_http_alternate_plugin();

  HttpCacheAlt html;
  html.set_response_header("Content-Type", "text/html");
  auto html_meta = html.serialize();
  HttpCacheAlt png;
  png.set_response_header("Content-Type", "image/png");
  auto png_meta = png.serialize();

  auto select = [&](const std::string &headers, bool both) {
    VariantCollection variants;
    variants.add(CacheVariant{CacheKey("html"),
                              Metadata(std::span<const std::byte>(html_meta)),
                              0,
                              {},
                              {},
                              0});
    if (both) {
      variants.add(CacheVariant{CacheKey("png"),
                                Metadata(std::span<const std::byte>(png_meta)),
                                0,
                                {},
                                {},
                                0});
    }
    LookupContext ctx;
    ctx.request_headers = std::span<const std::byte>(
        reinterpret_cast<const std::byte *>(headers.data()), headers.size());
    return plugin->select_variant(variants, ctx);
  };

  SECTION("explicit q=0 on the only variant") {
    REQUIRE_FALSE(select("Accept: text/html;q=0\r\n", false).has_value());
    REQUIRE_FALSE(select("Accept: text/html;q=0.0\r\n", false).has_value());
  }
  SECTION("media type that does not match the only variant") {
    REQUIRE_FALSE(select("Accept: image/png\r\n", false).has_value());
  }
  SECTION("q=0 variant loses to an acceptable one") {
    auto r = select("Accept: text/html;q=0, image/png;q=0.1\r\n", true);
    REQUIRE(r.has_value());
    REQUIRE(*r == 1);
  }
  SECTION("nothing acceptable among several variants") {
    REQUIRE_FALSE(
        select("Accept: text/html;q=0, image/png;q=0\r\n", true).has_value());
  }
  SECTION("acceptable variant is still selected") {
    auto r = select("Accept: text/html;q=0.5\r\n", false);
    REQUIRE(r.has_value());
    REQUIRE(*r == 0);
  }
}

TEST_CASE("HttpAlternatePlugin ranks negotiation ranges by specificity",
          "[http][alternate]") {
  auto plugin = create_http_alternate_plugin();

  // Index of the selected variant among those built from `variant_headers`
  // (each entry is a list of response header pairs), or -1 for no match.
  using Pairs = std::vector<std::pair<std::string, std::string>>;
  auto select = [&](const std::string &request, const std::vector<Pairs> &vs) {
    std::vector<std::vector<std::byte>> metas;
    VariantCollection variants;
    for (const auto &pairs : vs) {
      HttpCacheAlt alt;
      for (const auto &[n, v] : pairs) {
        alt.set_response_header(n, v);
      }
      metas.push_back(alt.serialize());
      variants.add(
          CacheVariant{CacheKey("v"),
                       Metadata(std::span<const std::byte>(metas.back())),
                       0,
                       {},
                       {},
                       0});
    }
    LookupContext ctx;
    ctx.request_headers = std::span<const std::byte>(
        reinterpret_cast<const std::byte *>(request.data()), request.size());
    auto r = plugin->select_variant(variants, ctx);
    return r ? static_cast<int>(*r) : -1;
  };
  const Pairs html{{"Content-Type", "text/html"}};
  const Pairs png{{"Content-Type", "image/png"}};

  SECTION("Accept: specific range beats wildcard regardless of order") {
    REQUIRE(select("Accept: text/html;q=0, */*\r\n", {html}) == -1);
    REQUIRE(select("Accept: */*, text/html;q=0\r\n", {html}) == -1);
    REQUIRE(select("Accept: text/html;q=0, */*\r\n", {html, png}) == 1);
    REQUIRE(select("Accept: text/*;q=0, text/html\r\n", {html}) == 0);
    REQUIRE(select("Accept: text/html;q=0.1, text/*;q=0.9, image/png;q=0.5\r\n",
                   {html, png}) == 1);
  }
  SECTION("Accept: whitespace around parameters and an upper case Q") {
    REQUIRE(select("Accept: text/html ; q=0, */*\r\n", {html}) == -1);
    REQUIRE(select("Accept: text/html;Q=0, */*\r\n", {html}) == -1);
    REQUIRE(select("Accept: text/html; q = 0\r\n", {html}) == -1);
    REQUIRE(select("Accept: text/html;level=1; q=0.5\r\n", {html}) == 0);
  }
  SECTION("Accept-Encoding: exact coding beats the wildcard") {
    const Pairs gzip{{"Content-Type", "text/html"},
                     {"Content-Encoding", "gzip"}};
    const Pairs plain{{"Content-Type", "text/html"}};
    REQUIRE(select("Accept-Encoding: *, gzip;q=0\r\n", {gzip}) == -1);
    REQUIRE(select("Accept-Encoding: gzip;q=0, *\r\n", {gzip}) == -1);
    REQUIRE(select("Accept-Encoding: *, gzip;q=0\r\n", {gzip, plain}) == 1);
    REQUIRE(select("Accept-Encoding: gzip ; q=0.5, *;q=0.1\r\n",
                   {gzip, plain}) == 0);
    REQUIRE(select("Accept-Encoding: GZIP;Q=0\r\n", {gzip}) == -1);
  }
  SECTION("Accept-Language: the most specific range wins") {
    const Pairs en{{"Content-Language", "en-US"}};
    const Pairs fr{{"Content-Language", "fr"}};
    REQUIRE(select("Accept-Language: *;q=0, en\r\n", {en}) == 0);
    REQUIRE(select("Accept-Language: *;q=0, en\r\n", {fr}) == -1);
    REQUIRE(select("Accept-Language: en-US;q=0, en\r\n", {en}) == -1);
    REQUIRE(select("Accept-Language: en, en-US;q=0\r\n", {en}) == -1);
    REQUIRE(select("Accept-Language: en;q=0.3, fr;q=0.9\r\n", {en, fr}) == 1);
    REQUIRE(select("Accept-Language: en ; q=0\r\n", {en}) == -1);
  }
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

TEST_CASE("HttpAlternatePlugin no-cache forces revalidation of fresh entries",
          "[http]") {
  auto plugin = create_http_alternate_plugin();
  FreshnessContext fctx;

  auto check = [&](const char *cache_control) {
    HttpCacheAlt alt;
    alt.set_response_time(std::time(nullptr));
    alt.set_response_header("Cache-Control", cache_control);
    auto meta = alt.serialize();
    CacheVariant variant{CacheKey("nc"),
                         Metadata(std::span<const std::byte>(meta)),
                         0,
                         {},
                         {},
                         0};
    return plugin->check_freshness(variant, fctx);
  };

  REQUIRE(check("max-age=3600") == FreshnessResult::Fresh);
  REQUIRE(check("max-age=3600, no-cache") == FreshnessResult::MustRevalidate);
  REQUIRE(check("No-Cache, max-age=3600") == FreshnessResult::MustRevalidate);
  // must-revalidate only matters once the entry is stale.
  REQUIRE(check("max-age=3600, must-revalidate") == FreshnessResult::Fresh);
  REQUIRE(check("max-age=0, must-revalidate") ==
          FreshnessResult::MustRevalidate);
  // A shared cache treats s-maxage and proxy-revalidate like must-revalidate
  // once the entry is stale.
  REQUIRE(check("max-age=3600, s-maxage=0") ==
          FreshnessResult::MustRevalidate);
  REQUIRE(check("max-age=0, proxy-revalidate") ==
          FreshnessResult::MustRevalidate);
  REQUIRE(check("max-age=0") == FreshnessResult::Stale);
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
  REQUIRE_FALSE(alt.has_value());
  auto elapsed = std::chrono::steady_clock::now() - start;

  // Must complete in well under 1 second (was previously ~seconds with 4B
  // iterations)
  REQUIRE(elapsed < std::chrono::milliseconds(100));
}

#endif  // CYCLONE_HTTP_PLUGIN
