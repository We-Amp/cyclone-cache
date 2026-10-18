// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cyclone {

class HttpCacheAlt {
 public:
  static constexpr uint32_t kPluginId = 1;

  HttpCacheAlt();

  void set_request_method(std::string_view method) { _request_method = method; }
  void set_request_url(std::string_view url) { _request_url = url; }
  void set_request_header(std::string_view name, std::string_view value);

  void set_status_code(uint16_t code) { _status_code = code; }
  void set_response_header(std::string_view name, std::string_view value);

  // Times are clamped to [0, year 9999] so age and freshness arithmetic cannot
  // overflow, whatever a stored blob or a caller supplies.
  void set_request_time(std::time_t t) {
    _request_time = clamp_time(static_cast<int64_t>(t));
  }
  void set_response_time(std::time_t t) {
    _response_time = clamp_time(static_cast<int64_t>(t));
  }

  [[nodiscard]] std::string_view request_method() const {
    return _request_method;
  }
  [[nodiscard]] std::string_view request_url() const { return _request_url; }
  [[nodiscard]] std::optional<std::string_view> get_request_header(
      std::string_view name) const;

  [[nodiscard]] uint16_t status_code() const { return _status_code; }
  [[nodiscard]] std::optional<std::string_view> get_response_header(
      std::string_view name) const;

  [[nodiscard]] std::time_t request_time() const { return _request_time; }
  [[nodiscard]] std::time_t response_time() const { return _response_time; }

  [[nodiscard]] std::chrono::seconds age() const;
  // Same, against a caller supplied clock reading (seconds since the epoch).
  [[nodiscard]] std::chrono::seconds age(std::time_t now) const;
  [[nodiscard]] std::optional<std::chrono::seconds> max_age() const;
  // Freshness lifetime for a shared cache: s-maxage, else max-age, else
  // Expires, else a heuristic (only with Last-Modified and a status that is
  // cacheable by default); nullopt when none applies. Whether a response may
  // be stored at all (private, no-store) is the embedder's decision; this
  // class only answers how long a stored entry stays fresh.
  [[nodiscard]] std::optional<std::chrono::seconds> freshness_lifetime() const;
  [[nodiscard]] bool is_fresh() const;
  [[nodiscard]] bool is_fresh(std::time_t now) const;

  [[nodiscard]] std::vector<std::byte> serialize() const;
  // Returns nullopt unless the whole input is a well-formed serialized blob.
  [[nodiscard]] static std::optional<HttpCacheAlt> deserialize(
      std::span<const std::byte> data);

 private:
  static_assert(
      sizeof(std::time_t) >= 8,
      "metadata times are 64 bit; a narrower time_t cannot hold them");
  static constexpr int64_t kMaxTime = 253402300799;  // 9999-12-31T23:59:59Z
  // Clamps in 64 bit before the value becomes a time_t.
  static constexpr std::time_t clamp_time(int64_t t) {
    return static_cast<std::time_t>(t < 0 ? 0 : (t > kMaxTime ? kMaxTime : t));
  }

  std::string _request_method;
  std::string _request_url;
  std::map<std::string, std::string> _request_headers;

  uint16_t _status_code = 0;
  std::map<std::string, std::string> _response_headers;

  std::time_t _request_time = 0;
  std::time_t _response_time = 0;
};

}  // namespace cyclone
