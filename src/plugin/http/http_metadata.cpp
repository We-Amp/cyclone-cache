// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "http_metadata.hpp"

#include <cstring>
#include <sstream>

namespace cyclone {

HttpCacheAlt::HttpCacheAlt() = default;

void HttpCacheAlt::set_request_header(std::string_view name,
                                      std::string_view value) {
  _request_headers[std::string(name)] = std::string(value);
}

void HttpCacheAlt::set_response_header(std::string_view name,
                                       std::string_view value) {
  _response_headers[std::string(name)] = std::string(value);
}

std::optional<std::string_view> HttpCacheAlt::get_request_header(
    std::string_view name) const {
  auto it = _request_headers.find(std::string(name));
  if (it != _request_headers.end()) {
    return it->second;
  }
  return std::nullopt;
}

std::optional<std::string_view> HttpCacheAlt::get_response_header(
    std::string_view name) const {
  auto it = _response_headers.find(std::string(name));
  if (it != _response_headers.end()) {
    return it->second;
  }
  return std::nullopt;
}

std::vector<std::byte> HttpCacheAlt::serialize() const {
  std::vector<std::byte> result;

  auto write_string = [&result](std::string_view s) {
    auto len = static_cast<uint32_t>(s.size());
    size_t offset = result.size();
    result.resize(offset + sizeof(len) + len);
    std::memcpy(result.data() + offset, &len, sizeof(len));
    std::memcpy(result.data() + offset + sizeof(len), s.data(), len);
  };

  auto write_headers = [&](const std::map<std::string, std::string> &headers) {
    auto count = static_cast<uint32_t>(headers.size());
    size_t offset = result.size();
    result.resize(offset + sizeof(count));
    std::memcpy(result.data() + offset, &count, sizeof(count));

    for (const auto &[name, value] : headers) {
      write_string(name);
      write_string(value);
    }
  };

  write_string(_request_method);
  write_string(_request_url);
  write_headers(_request_headers);

  result.resize(result.size() + sizeof(_status_code));
  std::memcpy(result.data() + result.size() - sizeof(_status_code),
              &_status_code, sizeof(_status_code));

  write_headers(_response_headers);

  result.resize(result.size() + sizeof(_request_time) + sizeof(_response_time));
  size_t offset =
      result.size() - sizeof(_request_time) - sizeof(_response_time);
  std::memcpy(result.data() + offset, &_request_time, sizeof(_request_time));
  std::memcpy(result.data() + offset + sizeof(_request_time), &_response_time,
              sizeof(_response_time));

  return result;
}

HttpCacheAlt HttpCacheAlt::deserialize(std::span<const std::byte> data) {
  HttpCacheAlt alt;
  size_t offset = 0;

  auto read_string = [&data, &offset]() -> std::string {
    if (offset + sizeof(uint32_t) > data.size()) {
      return {};
    }
    uint32_t len;
    std::memcpy(&len, data.data() + offset, sizeof(len));
    offset += sizeof(len);

    if (offset + len > data.size()) {
      return {};
    }
    std::string result(len, '\0');
    std::memcpy(result.data(), data.data() + offset, len);
    offset += len;
    return result;
  };

  auto read_headers = [&]() -> std::map<std::string, std::string> {
    std::map<std::string, std::string> headers;
    if (offset + sizeof(uint32_t) > data.size()) {
      return headers;
    }
    uint32_t count;
    std::memcpy(&count, data.data() + offset, sizeof(count));
    offset += sizeof(count);

    // Sanity bound: an HTTP message rarely has >100 headers.
    // Also bail early if we've exhausted the buffer.
    constexpr uint32_t kMaxHeaders = 1000;
    if (count > kMaxHeaders) {
      count = kMaxHeaders;
    }

    for (uint32_t i = 0; i < count; ++i) {
      if (offset >= data.size()) {
        break;  // Buffer exhausted — stop parsing
      }
      std::string name = read_string();
      std::string value = read_string();
      if (!name.empty()) {
        headers[name] = value;
      }
    }
    return headers;
  };

  alt._request_method = read_string();
  alt._request_url = read_string();
  alt._request_headers = read_headers();

  if (offset + sizeof(alt._status_code) <= data.size()) {
    std::memcpy(&alt._status_code, data.data() + offset,
                sizeof(alt._status_code));
    offset += sizeof(alt._status_code);
  }

  alt._response_headers = read_headers();

  if (offset + sizeof(alt._request_time) + sizeof(alt._response_time) <=
      data.size()) {
    std::memcpy(&alt._request_time, data.data() + offset,
                sizeof(alt._request_time));
    offset += sizeof(alt._request_time);
    std::memcpy(&alt._response_time, data.data() + offset,
                sizeof(alt._response_time));
  }

  return alt;
}

std::chrono::seconds HttpCacheAlt::age() const {
  auto now = std::chrono::system_clock::now();
  auto now_time_t = std::chrono::system_clock::to_time_t(now);
  return std::chrono::seconds(now_time_t - _response_time);
}

std::optional<std::chrono::seconds> HttpCacheAlt::max_age() const {
  auto cc = get_response_header("Cache-Control");
  if (!cc) {
    return std::nullopt;
  }

  std::string_view value = *cc;
  auto pos = value.find("max-age=");
  if (pos == std::string_view::npos) {
    return std::nullopt;
  }

  pos += 8;
  uint64_t seconds = 0;
  constexpr uint64_t kMaxAge =
      31536000;  // 1 year — RFC 7234 §5.2.2.8 recommendation
  while (pos < value.size() && value[pos] >= '0' && value[pos] <= '9') {
    seconds = seconds * 10 + static_cast<uint64_t>(value[pos] - '0');
    if (seconds > kMaxAge) {
      seconds = kMaxAge;
      break;
    }
    ++pos;
  }

  return std::chrono::seconds(static_cast<int64_t>(seconds));
}

bool HttpCacheAlt::is_fresh() const {
  auto ma = max_age();
  if (ma) {
    return age() < *ma;
  }
  return true;
}

}  // namespace cyclone
