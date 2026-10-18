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

  if (size < 2) {
    return 0;
  }
  size_t split = (static_cast<size_t>(data[0]) << 8 | data[1]) % (size - 1);
  const uint8_t *body = data + 2;
  size_t body_size = size - 2;
  split %= body_size + 1;

  auto metadata = std::as_bytes(std::span(body, split));
  auto request_headers =
      std::as_bytes(std::span(body + split, body_size - split));

  VariantCollection variants;
  for (int i = 0; i < 2; ++i) {
    CacheVariant variant;
    variant.key = CacheKey("fuzz-variant-" + std::to_string(i));
    variant.metadata = Metadata(metadata);
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

  FreshnessContext fresh;
  (void)plugin->check_freshness(variants[0], fresh);
  (void)plugin->eviction_priority(variants[0]);
  return 0;
}
