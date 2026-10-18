// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// libFuzzer target: the HTTP cache metadata deserializer.
//
// HttpCacheAlt::deserialize parses a length-prefixed blob read back from the
// cache.  Invariants: no crash or undefined behaviour for any input; a blob
// that decodes must survive the accessors and freshness calculations; and the
// encoding is canonical, so serialize(deserialize(x)) decodes again and
// re-serializes to the same bytes.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "plugin/http/http_metadata.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  using cyclone::HttpCacheAlt;

  auto bytes = std::as_bytes(std::span(data, size));
  std::optional<HttpCacheAlt> alt = HttpCacheAlt::deserialize(bytes);
  if (!alt) {
    return 0;
  }

  (void)alt->request_method();
  (void)alt->request_url();
  (void)alt->status_code();
  (void)alt->get_request_header("accept-encoding");
  (void)alt->get_response_header("cache-control");
  (void)alt->get_response_header("vary");
  (void)alt->age();
  (void)alt->max_age();
  (void)alt->freshness_lifetime();
  (void)alt->is_fresh();

  std::vector<std::byte> once = alt->serialize();
  std::optional<HttpCacheAlt> again = HttpCacheAlt::deserialize(once);
  if (!again) {
    __builtin_trap();  // our own output must decode
  }
  if (again->serialize() != once) {
    __builtin_trap();  // the encoding must be canonical
  }
  return 0;
}
