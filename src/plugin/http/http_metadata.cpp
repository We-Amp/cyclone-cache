// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "http_metadata.hpp"

#include <algorithm>
#include <cstring>
#include <ctime>
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

// Seconds since the Unix epoch for a UTC calendar time, or nullopt when a
// field is out of range.
std::optional<int64_t> to_epoch(int year, int month, int day, int hour,
                                int minute, int second) {
  if (month < 1 || month > 12 || day < 1 || day > 31 || year < 1970 ||
      hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 ||
      second > 60) {
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

int parse_month(std::string_view v) {
  static constexpr std::string_view kMonths[] = {"Jan", "Feb", "Mar", "Apr",
                                                 "May", "Jun", "Jul", "Aug",
                                                 "Sep", "Oct", "Nov", "Dec"};
  for (int m = 0; m < 12; ++m) {
    if (iequals(v, kMonths[m])) {
      return m + 1;
    }
  }
  return 0;
}

// Parses a run of exactly `len` digits, or `-1`.
int parse_digits(std::string_view v, size_t pos, size_t len) {
  if (pos + len > v.size()) {
    return -1;
  }
  int r = 0;
  for (size_t i = pos; i < pos + len; ++i) {
    if (v[i] < '0' || v[i] > '9') {
      return -1;
    }
    r = r * 10 + (v[i] - '0');
  }
  return r;
}

// "hh:mm:ss" at `pos`; returns false when malformed.
bool parse_clock(std::string_view v, size_t pos, int &h, int &m, int &s) {
  if (pos + 8 > v.size() || v[pos + 2] != ':' || v[pos + 5] != ':') {
    return false;
  }
  h = parse_digits(v, pos, 2);
  m = parse_digits(v, pos + 3, 2);
  s = parse_digits(v, pos + 6, 2);
  return h >= 0 && m >= 0 && s >= 0;
}

// Parses an HTTP-date in any of the three formats of RFC 9110 5.6.7 to
// seconds since the Unix epoch: IMF-fixdate ("Sun, 06 Nov 1994 08:49:37
// GMT"), the obsolete RFC 850 form ("Sunday, 06-Nov-94 08:49:37 GMT") and
// asctime ("Sun Nov  6 08:49:37 1994"). Returns nullopt for anything else.
std::optional<int64_t> parse_http_date(std::string_view v) {
  v = trim(v);
  int day = -1;
  int month = 0;
  int year = -1;
  int h = -1;
  int mi = -1;
  int sec = -1;

  size_t comma = v.find(',');
  if (comma != std::string_view::npos) {
    std::string_view rest = trim(v.substr(comma + 1));
    if (rest.size() == 24) {
      // IMF-fixdate: "06 Nov 1994 08:49:37 GMT"
      if (rest[2] != ' ' || rest[6] != ' ' || rest[11] != ' ' ||
          rest[20] != ' ' || rest.substr(21) != "GMT") {
        return std::nullopt;
      }
      day = parse_digits(rest, 0, 2);
      month = parse_month(rest.substr(3, 3));
      year = parse_digits(rest, 7, 4);
      if (!parse_clock(rest, 12, h, mi, sec)) {
        return std::nullopt;
      }
    } else if (rest.size() == 22) {
      // RFC 850: "06-Nov-94 08:49:37 GMT"
      if (rest[2] != '-' || rest[6] != '-' || rest[9] != ' ' ||
          rest[18] != ' ' || rest.substr(19) != "GMT") {
        return std::nullopt;
      }
      day = parse_digits(rest, 0, 2);
      month = parse_month(rest.substr(3, 3));
      int yy = parse_digits(rest, 7, 2);
      if (yy < 0 || !parse_clock(rest, 10, h, mi, sec)) {
        return std::nullopt;
      }
      // A two digit year more than 50 years ahead of now is in the past.
      int current_year = static_cast<int>(
          1970 + static_cast<int64_t>(std::time(nullptr)) / 31556952);
      year = (current_year / 100) * 100 + yy;
      if (year > current_year + 50) {
        year -= 100;
      }
    } else {
      return std::nullopt;
    }
  } else {
    // asctime: "Sun Nov  6 08:49:37 1994"
    if (v.size() != 24 || v[3] != ' ' || v[7] != ' ' || v[10] != ' ' ||
        v[19] != ' ') {
      return std::nullopt;
    }
    month = parse_month(v.substr(4, 3));
    day = v[8] == ' ' ? parse_digits(v, 9, 1) : parse_digits(v, 8, 2);
    if (!parse_clock(v, 11, h, mi, sec)) {
      return std::nullopt;
    }
    year = parse_digits(v, 20, 4);
  }

  if (month == 0 || day < 0 || year < 0) {
    return std::nullopt;
  }
  return to_epoch(year, month, day, h, mi, sec);
}

// Statuses that may be cached without explicit freshness information
// (RFC 9110 15.1); heuristic freshness applies only to these.
bool cacheable_by_default(uint16_t status) {
  switch (status) {
    case 200:
    case 203:
    case 204:
    case 206:
    case 300:
    case 301:
    case 308:
    case 404:
    case 405:
    case 410:
    case 414:
    case 501:
      return true;
    default:
      return false;
  }
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
  int64_t resident = static_cast<int64_t>(now_time_t - _response_time);

  // RFC 9111 4.2.3: corrected_initial_age is the larger of the apparent age
  // (receive time past the origin's Date) and the Age header corrected by
  // the response delay.
  int64_t apparent = 0;
  if (auto date = get_response_header("date")) {
    if (auto parsed = parse_http_date(*date)) {
      apparent =
          std::max<int64_t>(0, static_cast<int64_t>(_response_time) - *parsed);
    }
  }
  int64_t corrected = 0;
  if (auto age_hdr = get_response_header("age")) {
    std::string_view v = trim(*age_hdr);
    bool valid = !v.empty();
    int64_t value = 0;
    for (char c : v) {
      if (c < '0' || c > '9') {
        valid = false;
        break;
      }
      value = value * 10 + (c - '0');
      if (value > kMaxLifetimeSeconds * 100) {
        value = kMaxLifetimeSeconds * 100;
      }
    }
    if (valid) {
      int64_t delay = 0;
      if (_request_time > 0 && _request_time <= _response_time) {
        delay = static_cast<int64_t>(_response_time - _request_time);
      }
      corrected = value + delay;
    }
  }
  return std::chrono::seconds(resident + std::max(apparent, corrected));
}

namespace {

// Reads a delta-seconds Cache-Control directive such as max-age or s-maxage.
// Names are case-insensitive and matched as whole tokens.
std::optional<std::chrono::seconds> cache_control_seconds(
    std::string_view cache_control, std::string_view directive_name) {
  std::string_view rest = cache_control;
  while (!rest.empty()) {
    size_t comma = rest.find(',');
    std::string_view directive = trim(rest.substr(0, comma));
    rest = comma == std::string_view::npos ? std::string_view()
                                           : rest.substr(comma + 1);

    size_t eq = directive.find('=');
    if (eq == std::string_view::npos ||
        !iequals(trim(directive.substr(0, eq)), directive_name)) {
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

}  // namespace

std::optional<std::chrono::seconds> HttpCacheAlt::max_age() const {
  auto cc = get_response_header("cache-control");
  if (!cc) {
    return std::nullopt;
  }
  return cache_control_seconds(*cc, "max-age");
}

std::optional<std::chrono::seconds> HttpCacheAlt::freshness_lifetime() const {
  // This is a shared cache: s-maxage (RFC 9111 5.2.2.10) overrides max-age
  // and Expires.
  if (auto cc = get_response_header("cache-control")) {
    if (auto sma = cache_control_seconds(*cc, "s-maxage")) {
      return sma;
    }
  }
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
  if (auto lm = get_response_header("last-modified");
      lm && cacheable_by_default(_status_code)) {
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
