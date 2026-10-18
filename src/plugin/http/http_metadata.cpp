// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "http_metadata.hpp"

#include <cstring>
#include <sstream>

namespace cyclone {

namespace {

// HTTP field names are case-insensitive; headers are stored and looked up
// under their lowercase form.
std::string lower_name(std::string_view name) {
  std::string result(name);
  for (char &c : result) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return result;
}

}  // namespace

HttpCacheAlt::HttpCacheAlt() = default;

void HttpCacheAlt::set_request_header(std::string_view name,
                                      std::string_view value) {
  _request_headers[lower_name(name)] = std::string(value);
}

void HttpCacheAlt::set_response_header(std::string_view name,
                                       std::string_view value) {
  _response_headers[lower_name(name)] = std::string(value);
}

std::optional<std::string_view> HttpCacheAlt::get_request_header(
    std::string_view name) const {
  auto it = _request_headers.find(lower_name(name));
  if (it != _request_headers.end()) {
    return it->second;
  }
  return std::nullopt;
}

std::optional<std::string_view> HttpCacheAlt::get_response_header(
    std::string_view name) const {
  auto it = _response_headers.find(lower_name(name));
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

std::optional<HttpCacheAlt> HttpCacheAlt::deserialize(
    std::span<const std::byte> data) {
  HttpCacheAlt alt;
  size_t offset = 0;

  // Every reader returns false on truncated input; the caller abandons the
  // whole parse so no later field is read from a desynchronised offset.
  auto read_u32 = [&data, &offset](uint32_t &out) -> bool {
    if (data.size() - offset < sizeof(out)) {
      return false;
    }
    std::memcpy(&out, data.data() + offset, sizeof(out));
    offset += sizeof(out);
    return true;
  };

  auto read_string = [&](std::string &out) -> bool {
    uint32_t len = 0;
    if (!read_u32(len)) {
      return false;
    }
    if (len > data.size() - offset) {
      return false;
    }
    out.assign(reinterpret_cast<const char *>(data.data() + offset), len);
    offset += len;
    return true;
  };

  auto read_headers = [&](std::map<std::string, std::string> &headers) -> bool {
    uint32_t count = 0;
    if (!read_u32(count)) {
      return false;
    }

    // Sanity bound: an HTTP message rarely has >100 headers.
    constexpr uint32_t kMaxHeaders = 1000;
    if (count > kMaxHeaders) {
      return false;
    }

    for (uint32_t i = 0; i < count; ++i) {
      std::string name;
      std::string value;
      if (!read_string(name) || !read_string(value)) {
        return false;
      }
      if (!name.empty()) {
        headers[lower_name(name)] = std::move(value);
      }
    }
    return true;
  };

  if (!read_string(alt._request_method) || !read_string(alt._request_url) ||
      !read_headers(alt._request_headers)) {
    return std::nullopt;
  }

  if (data.size() - offset < sizeof(alt._status_code)) {
    return std::nullopt;
  }
  std::memcpy(&alt._status_code, data.data() + offset,
              sizeof(alt._status_code));
  offset += sizeof(alt._status_code);

  if (!read_headers(alt._response_headers)) {
    return std::nullopt;
  }

  if (data.size() - offset !=
      sizeof(alt._request_time) + sizeof(alt._response_time)) {
    return std::nullopt;
  }
  std::memcpy(&alt._request_time, data.data() + offset,
              sizeof(alt._request_time));
  offset += sizeof(alt._request_time);
  std::memcpy(&alt._response_time, data.data() + offset,
              sizeof(alt._response_time));

  return alt;
}

std::chrono::seconds HttpCacheAlt::age() const {
  auto now = std::chrono::system_clock::now();
  auto now_time_t = std::chrono::system_clock::to_time_t(now);
  // An age is never negative (a response time in the future reads as zero).
  if (now_time_t < _response_time) {
    return std::chrono::seconds(0);
  }
  return std::chrono::seconds(now_time_t - _response_time);
}

std::optional<std::chrono::seconds> HttpCacheAlt::max_age() const {
  auto cc = get_response_header("cache-control");
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
  // A response time in the future cannot be trusted (clock change or damaged
  // metadata); such an entry is stale rather than fresh for its whole span.
  if (_response_time >
      std::chrono::system_clock::to_time_t(std::chrono::system_clock::now())) {
    return false;
  }
  auto ma = max_age();
  if (ma) {
    return age() < *ma;
  }
  return true;
}

}  // namespace cyclone
