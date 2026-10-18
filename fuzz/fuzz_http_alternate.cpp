// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// libFuzzer target: HTTP alternate selection over untrusted bytes.
//
// The input is split into a metadata blob and a raw request header block, and
// handed to the plugin's select_variant (Vary matching, Accept / Accept-
// Encoding / Accept-Language negotiation, quality parsing) and
// check_freshness.  Invariants: no crash or undefined behaviour, and a
// returned index is inside the collection.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>

#include "cyclone/plugin/alternate.hpp"
#include "cyclone/plugin/plugin.hpp"
#include "plugin/http/http_metadata.hpp"

namespace cyclone {
std::shared_ptr<CachePlugin> create_http_alternate_plugin();
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  using namespace cyclone;
  static const std::shared_ptr<CachePlugin> plugin =
      create_http_alternate_plugin();

  // Input: two 8-bit split selectors, then metadata A, metadata B and the
  // request headers back to back.
  if (size < 2) {
    return 0;
  }
  const uint8_t *body = data + 2;
  size_t body_size = size - 2;
  size_t first = data[0] % (body_size + 1);
  size_t second = first + data[1] % (body_size - first + 1);

  std::span<const std::byte> metadata[2] = {
      std::as_bytes(std::span(body, first)),
      std::as_bytes(std::span(body + first, second - first))};
  auto request_headers =
      std::as_bytes(std::span(body + second, body_size - second));

  VariantCollection variants;
  for (int i = 0; i < 2; ++i) {
    CacheVariant variant;
    variant.key = CacheKey("fuzz-variant-" + std::to_string(i));
    variant.metadata = Metadata(metadata[i]);
    variant.hit_count = static_cast<uint32_t>(size) * 0x9E3779B9u;
    variants.add(std::move(variant));
  }

  LookupContext ctx;
  ctx.request_headers = request_headers;
  if (auto picked = plugin->select_variant(variants, ctx)) {
    if (*picked >= variants.size()) {
      __builtin_trap();
    }
  }

  // A fixed clock so a crashing input replays identically.
  FreshnessContext fresh;
  fresh.now =
      std::chrono::system_clock::time_point(std::chrono::seconds(1700000000));
  (void)plugin->check_freshness(variants[0], fresh);
  (void)plugin->check_freshness(variants[1], fresh);
  (void)plugin->eviction_priority(variants[0]);
  return 0;
}
