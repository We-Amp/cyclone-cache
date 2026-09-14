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

  void set_request_time(std::time_t t) { _request_time = t; }
  void set_response_time(std::time_t t) { _response_time = t; }

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
  [[nodiscard]] std::optional<std::chrono::seconds> max_age() const;
  [[nodiscard]] bool is_fresh() const;

  [[nodiscard]] std::vector<std::byte> serialize() const;
  static HttpCacheAlt deserialize(std::span<const std::byte> data);

 private:
  std::string _request_method;
  std::string _request_url;
  std::map<std::string, std::string> _request_headers;

  uint16_t _status_code = 0;
  std::map<std::string, std::string> _response_headers;

  std::time_t _request_time = 0;
  std::time_t _response_time = 0;
};

}  // namespace cyclone
