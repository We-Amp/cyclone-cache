// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "http_metadata.hpp"

#include <algorithm>
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

constexpr int64_t kMaxLifetimeSeconds =
    31536000;  // 1 year, RFC 9111 5.2.2.1 recommendation

bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i) {
    char x = a[i];
    char y = b[i];
    if (x >= 'A' && x <= 'Z') {
      x = static_cast<char>(x - 'A' + 'a');
    }
    if (y >= 'A' && y <= 'Z') {
      y = static_cast<char>(y - 'A' + 'a');
    }
    if (x != y) {
      return false;
    }
  }
  return true;
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
    s.remove_prefix(1);
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
    s.remove_suffix(1);
  }
  return s;
}

// Parses an IMF-fixdate ("Sun, 06 Nov 1994 08:49:37 GMT") to seconds since
// the Unix epoch. Returns nullopt for anything else.
std::optional<int64_t> parse_http_date(std::string_view v) {
  v = trim(v);
  if (v.size() != 29 || v[3] != ',' || v[4] != ' ' || v[7] != ' ' ||
      v[11] != ' ' || v[16] != ' ' || v[19] != ':' || v[22] != ':' ||
      v[25] != ' ' || v.substr(26) != "GMT") {
    return std::nullopt;
  }
  auto num = [&v](size_t pos, size_t len) -> int {
    int r = 0;
    for (size_t i = pos; i < pos + len; ++i) {
      if (v[i] < '0' || v[i] > '9') {
        return -1;
      }
      r = r * 10 + (v[i] - '0');
    }
    return r;
  };
  static constexpr std::string_view kMonths[] = {"Jan", "Feb", "Mar", "Apr",
                                                 "May", "Jun", "Jul", "Aug",
                                                 "Sep", "Oct", "Nov", "Dec"};
  int month = 0;
  for (int m = 0; m < 12; ++m) {
    if (iequals(v.substr(8, 3), kMonths[m])) {
      month = m + 1;
      break;
    }
  }
  int day = num(5, 2);
  int year = num(12, 4);
  int hour = num(17, 2);
  int minute = num(20, 2);
  int second = num(23, 2);
  if (month == 0 || day < 1 || day > 31 || year < 1970 || hour < 0 ||
      hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 60) {
    return std::nullopt;
  }
  // Days since 1970-01-01 for a proleptic Gregorian date.
  int y = month <= 2 ? year - 1 : year;
  int64_t era = y / 400;
  int64_t yoe = y - era * 400;
  int64_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  int64_t days = era * 146097 + doe - 719468;
  return days * 86400 + hour * 3600 + minute * 60 + second;
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
  int64_t result = static_cast<int64_t>(now_time_t - _response_time);
  // Time the response had already spent before we received it (RFC 9111
  // 4.2.3 apparent age): how far our receive time is past the origin's Date.
  if (auto date = get_response_header("date")) {
    if (auto parsed = parse_http_date(*date)) {
      int64_t apparent = static_cast<int64_t>(_response_time) - *parsed;
      if (apparent > 0) {
        result += apparent;
      }
    }
  }
  return std::chrono::seconds(result);
}

std::optional<std::chrono::seconds> HttpCacheAlt::max_age() const {
  auto cc = get_response_header("cache-control");
  if (!cc) {
    return std::nullopt;
  }

  // Walk the comma separated directives; names are case-insensitive and
  // matched as whole tokens.
  std::string_view rest = *cc;
  while (!rest.empty()) {
    size_t comma = rest.find(',');
    std::string_view directive = trim(rest.substr(0, comma));
    rest = comma == std::string_view::npos ? std::string_view()
                                           : rest.substr(comma + 1);

    size_t eq = directive.find('=');
    if (eq == std::string_view::npos ||
        !iequals(trim(directive.substr(0, eq)), "max-age")) {
      continue;
    }

    std::string_view value = trim(directive.substr(eq + 1));
    if (!value.empty() && value.front() == '"') {
      value.remove_prefix(1);
    }
    int64_t seconds = 0;
    for (char c : value) {
      if (c < '0' || c > '9') {
        break;
      }
      seconds = seconds * 10 + (c - '0');
      if (seconds > kMaxLifetimeSeconds) {
        seconds = kMaxLifetimeSeconds;
        break;
      }
    }
    return std::chrono::seconds(seconds);
  }
  return std::nullopt;
}

std::optional<std::chrono::seconds> HttpCacheAlt::freshness_lifetime() const {
  if (auto ma = max_age()) {
    return ma;
  }

  // Reference time for Expires and heuristics: the origin's Date when it is
  // usable, else when we received the response.
  int64_t base = static_cast<int64_t>(_response_time);
  if (auto date = get_response_header("date")) {
    if (auto parsed = parse_http_date(*date)) {
      base = *parsed;
    }
  }

  if (auto expires = get_response_header("expires")) {
    // An unparseable Expires (such as "0") means already expired.
    auto parsed = parse_http_date(*expires);
    int64_t life = parsed ? *parsed - base : 0;
    life = std::clamp<int64_t>(life, 0, kMaxLifetimeSeconds);
    return std::chrono::seconds(life);
  }

  // Heuristic freshness (RFC 9111 4.2.2): a tenth of the time since the
  // resource was last modified, and only when Last-Modified is known.
  if (auto lm = get_response_header("last-modified")) {
    if (auto parsed = parse_http_date(*lm); parsed && *parsed < base) {
      int64_t life =
          std::min<int64_t>((base - *parsed) / 10, kMaxLifetimeSeconds);
      return std::chrono::seconds(life);
    }
  }

  return std::nullopt;
}

bool HttpCacheAlt::is_fresh() const {
  // A response time in the future cannot be trusted (clock change or damaged
  // metadata); such an entry is stale rather than fresh for its whole span.
  if (_response_time >
      std::chrono::system_clock::to_time_t(std::chrono::system_clock::now())) {
    return false;
  }
  // Without a usable freshness lifetime the entry is stale.
  auto life = freshness_lifetime();
  if (!life) {
    return false;
  }
  return age() < *life;
}

}  // namespace cyclone
