// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// HTTP Alternate Selection Plugin
// Implements RFC 7234 content negotiation for cache variants.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>
#include <string>
#include <vector>

#include "cyclone/plugin/plugin.hpp"
#include "http_metadata.hpp"

namespace cyclone {

// Parse a q-value from an HTTP header (RFC 7231 §5.3.1).
// Returns 1.0 on any parse failure (per RFC, missing q defaults to 1).
// Clamps to [0.0, 1.0] range.
static float parse_q_value(std::string_view qval) noexcept {
  // Trim leading whitespace
  while (!qval.empty() && qval.front() == ' ') {
    qval.remove_prefix(1);
  }
  if (qval.empty()) {
    return 1.0f;
  }

  // Simple digit parser: handles "0", "1", "0.5", "0.123" etc.
  // Avoids std::stof which throws on malformed input.
  float result = 0.0f;
  size_t pos = 0;

  // Handle optional leading minus (clamp to 0 later)
  bool negative = false;
  if (pos < qval.size() && qval[pos] == '-') {
    negative = true;
    ++pos;
  }

  // Integer part
  bool has_digits = false;
  while (pos < qval.size() && qval[pos] >= '0' && qval[pos] <= '9') {
    result = result * 10.0f + static_cast<float>(qval[pos] - '0');
    has_digits = true;
    ++pos;
    if (result > 1.0f) {
      return 1.0f;  // Clamp: q-values are 0-1
    }
  }

  // Fractional part
  if (pos < qval.size() && qval[pos] == '.') {
    ++pos;
    float divisor = 10.0f;
    while (pos < qval.size() && qval[pos] >= '0' && qval[pos] <= '9') {
      result += static_cast<float>(qval[pos] - '0') / divisor;
      divisor *= 10.0f;
      has_digits = true;
      ++pos;
    }
  }

  if (!has_digits) {
    return 1.0f;  // No valid digits found
  }

  if (negative) {
    return 0.0f;
  }
  return result > 1.0f ? 1.0f : result;
}

class HttpAlternatePlugin : public CachePlugin {
 public:
  static constexpr uint32_t kPluginId = HttpCacheAlt::kPluginId;

  [[nodiscard]] PluginInfo info() const override {
    return {"http-alternate", "1.0.0", kPluginId};
  }

  CacheKey generate_key(const KeyContext &ctx) override {
    return CacheKey::from_url(ctx.url, ctx.hostname);
  }

  std::optional<size_t> select_variant(const VariantCollection &variants,
                                       const LookupContext &ctx) override;

  FreshnessResult check_freshness(const CacheVariant &variant,
                                  const FreshnessContext &ctx) override;

  double eviction_priority(const CacheVariant &variant) override;

 private:
  static std::string_view parse_header_value(std::span<const std::byte> headers,
                                             std::string_view name);

  bool check_vary_match(const HttpCacheAlt &cached,
                        std::span<const std::byte> request_headers);

  float calculate_accept_quality(std::string_view accept,
                                 std::string_view content_type);
  float calculate_accept_encoding_quality(std::string_view accept_encoding,
                                          std::string_view content_encoding);
  float calculate_accept_language_quality(std::string_view accept_language,
                                          std::string_view content_language);
};

std::optional<size_t> HttpAlternatePlugin::select_variant(
    const VariantCollection &variants, const LookupContext &ctx) {
  if (variants.empty()) {
    return std::nullopt;
  }

  size_t best_index = 0;
  float best_quality = 0.0f;

  // The negotiation headers do not depend on the variant: scan the request
  // header block once per lookup instead of once per variant.
  const auto accept = parse_header_value(ctx.request_headers, "Accept");
  const auto accept_encoding =
      parse_header_value(ctx.request_headers, "Accept-Encoding");
  const auto accept_language =
      parse_header_value(ctx.request_headers, "Accept-Language");

  for (size_t i = 0; i < variants.size(); ++i) {
    const auto &variant = variants[i];

    auto meta_opt = variant.metadata.data();
    if (meta_opt.empty()) {
      continue;
    }

    auto parsed = HttpCacheAlt::deserialize(meta_opt);
    if (!parsed) {
      continue;
    }
    const HttpCacheAlt &alt = *parsed;

    if (!check_vary_match(alt, ctx.request_headers)) {
      continue;
    }

    float quality = 1.0f;

    if (!accept.empty()) {
      auto content_type = alt.get_response_header("Content-Type");
      if (content_type) {
        quality *= calculate_accept_quality(accept, *content_type);
      }
    }

    if (!accept_encoding.empty()) {
      auto content_encoding = alt.get_response_header("Content-Encoding");
      quality *= calculate_accept_encoding_quality(
          accept_encoding, content_encoding.value_or("identity"));
    }

    if (!accept_language.empty()) {
      auto content_language = alt.get_response_header("Content-Language");
      if (content_language) {
        quality *= calculate_accept_language_quality(accept_language,
                                                     *content_language);
      }
    }

    // A quality of zero means "not acceptable" (RFC 9110 12.4.2).
    if (quality > best_quality) {
      best_quality = quality;
      best_index = i;
    }
  }

  if (best_quality <= 0.0f) {
    return std::nullopt;
  }

  return best_index;
}

FreshnessResult HttpAlternatePlugin::check_freshness(
    const CacheVariant &variant, const FreshnessContext &ctx) {
  if (ctx.force_revalidate) {
    return FreshnessResult::MustRevalidate;
  }

  auto meta_opt = variant.metadata.data();
  if (meta_opt.empty()) {
    return FreshnessResult::Error;
  }

  auto parsed = HttpCacheAlt::deserialize(meta_opt);
  if (!parsed) {
    return FreshnessResult::Error;
  }
  const HttpCacheAlt &alt = *parsed;

  bool no_cache = false;
  bool must_revalidate = false;
  if (auto cc = alt.get_response_header("Cache-Control")) {
    std::string lowered(*cc);
    for (char &c : lowered) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    no_cache = lowered.find("no-cache") != std::string::npos;
    // A shared cache also may not serve a stale entry without validation when
    // the response carries proxy-revalidate or s-maxage (RFC 9111 5.2.2.8,
    // 5.2.2.10).
    must_revalidate = lowered.find("must-revalidate") != std::string::npos ||
                      lowered.find("proxy-revalidate") != std::string::npos ||
                      lowered.find("s-maxage") != std::string::npos;
  }

  // no-cache requires validation before every reuse, even while the entry is
  // still fresh (RFC 9111 5.2.2.4).
  if (no_cache) {
    return FreshnessResult::MustRevalidate;
  }

  if (alt.is_fresh()) {
    return FreshnessResult::Fresh;
  }

  if (must_revalidate) {
    return FreshnessResult::MustRevalidate;
  }

  return FreshnessResult::Stale;
}

double HttpAlternatePlugin::eviction_priority(const CacheVariant &variant) {
  double priority = 1.0;

  // A last-access time ahead of the clock counts as just accessed.
  auto age_seconds =
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now() - variant.last_accessed)
          .count();
  priority += std::max<int64_t>(age_seconds, 0) / 3600.0;

  if (variant.hit_count > 0) {
    priority /= (1.0 + std::log(static_cast<double>(variant.hit_count) + 1.0));
  }

  return priority;
}

std::string_view HttpAlternatePlugin::parse_header_value(
    std::span<const std::byte> headers, std::string_view name) {
  if (headers.empty()) {
    return {};
  }

  std::string_view data(reinterpret_cast<const char *>(headers.data()),
                        headers.size());

  std::string search_name(name);
  search_name += ':';

  size_t pos = 0;
  while (pos < data.size()) {
    size_t line_end = data.find('\n', pos);
    if (line_end == std::string_view::npos) {
      line_end = data.size();
    }

    std::string_view line = data.substr(pos, line_end - pos);

    if (line.size() >= search_name.size()) {
      bool match = true;
      for (size_t i = 0; i < search_name.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(line[i])) !=
            std::tolower(static_cast<unsigned char>(search_name[i]))) {
          match = false;
          break;
        }
      }

      if (match) {
        std::string_view value = line.substr(search_name.size());
        while (!value.empty() &&
               (value.front() == ' ' || value.front() == '\t')) {
          value.remove_prefix(1);
        }
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t' ||
                                  value.back() == '\r')) {
          value.remove_suffix(1);
        }
        return value;
      }
    }

    pos = line_end + 1;
  }

  return {};
}

namespace {

std::string_view trim_vary_ows(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
    s.remove_prefix(1);
  }
  while (!s.empty() &&
         (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
    s.remove_suffix(1);
  }
  return s;
}

// Content negotiation headers whose values are comma separated lists that
// are case-insensitive by definition.
bool is_token_list_header(std::string_view name) {
  static constexpr std::string_view kNames[] = {
      "accept", "accept-encoding", "accept-language", "accept-charset"};
  for (std::string_view candidate : kNames) {
    if (name.size() != candidate.size()) {
      continue;
    }
    bool same = true;
    for (size_t i = 0; i < name.size(); ++i) {
      if (std::tolower(static_cast<unsigned char>(name[i])) != candidate[i]) {
        same = false;
        break;
      }
    }
    if (same) {
      return true;
    }
  }
  return false;
}

// Lowercased list elements with whitespace around ';' and '=' removed, as a
// sorted set.
std::vector<std::string> canonical_token_set(std::string_view value) {
  std::vector<std::string> tokens;
  size_t pos = 0;
  while (pos <= value.size()) {
    size_t end = value.find(',', pos);
    if (end == std::string_view::npos) {
      end = value.size();
    }
    std::string_view element = trim_vary_ows(value.substr(pos, end - pos));
    if (!element.empty()) {
      std::string canonical;
      for (char c : element) {
        if (c == ' ' || c == '\t') {
          continue;
        }
        canonical.push_back(
            static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
      }
      tokens.push_back(std::move(canonical));
    }
    pos = end + 1;
  }
  std::sort(tokens.begin(), tokens.end());
  tokens.erase(std::unique(tokens.begin(), tokens.end()), tokens.end());
  return tokens;
}

bool vary_values_match(std::string_view name, std::string_view cached,
                       std::string_view request) {
  if (is_token_list_header(name)) {
    return canonical_token_set(cached) == canonical_token_set(request);
  }
  return trim_vary_ows(cached) == trim_vary_ows(request);
}

}  // namespace

bool HttpAlternatePlugin::check_vary_match(
    const HttpCacheAlt &cached, std::span<const std::byte> request_headers) {
  auto vary = cached.get_response_header("Vary");
  if (!vary || vary->empty()) {
    return true;
  }

  if (*vary == "*") {
    return false;
  }

  std::string_view vary_value = *vary;
  size_t pos = 0;

  while (pos < vary_value.size()) {
    size_t end = vary_value.find(',', pos);
    if (end == std::string_view::npos) {
      end = vary_value.size();
    }

    std::string_view header_name = vary_value.substr(pos, end - pos);

    while (!header_name.empty() && header_name.front() == ' ') {
      header_name.remove_prefix(1);
    }
    while (!header_name.empty() && header_name.back() == ' ') {
      header_name.remove_suffix(1);
    }

    if (!header_name.empty()) {
      auto cached_value = cached.get_request_header(header_name);
      auto request_value = parse_header_value(request_headers, header_name);

      std::string_view cv = cached_value.value_or("");
      if (!vary_values_match(header_name, cv, request_value)) {
        return false;
      }
    }

    pos = (end < vary_value.size()) ? end + 1 : end;
  }

  return true;
}

namespace {

std::string_view trim_ows(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
    s.remove_prefix(1);
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
    s.remove_suffix(1);
  }
  return s;
}

bool iequal(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

// Splits one list element ("token;param=value;q=0.5") into its token and
// quality. Whitespace around ';' and '=' is allowed and the parameter name
// "q" is case-insensitive. A missing or unparsable q means 1.
struct RangeItem {
  std::string_view token;
  float q = 1.0f;
};

RangeItem parse_range_item(std::string_view item) {
  RangeItem result;
  size_t semi = item.find(';');
  result.token = trim_ows(item.substr(0, semi));
  while (semi != std::string_view::npos) {
    size_t next = item.find(';', semi + 1);
    std::string_view param = item.substr(
        semi + 1, next == std::string_view::npos ? next : next - semi - 1);
    size_t eq = param.find('=');
    if (eq != std::string_view::npos &&
        iequal(trim_ows(param.substr(0, eq)), "q")) {
      result.q = parse_q_value(trim_ows(param.substr(eq + 1)));
    }
    semi = next;
  }
  return result;
}

// Calls fn(RangeItem) for each comma separated element of a list header.
template <typename Fn>
void for_each_range(std::string_view list, Fn &&fn) {
  size_t pos = 0;
  while (pos < list.size()) {
    size_t end = list.find(',', pos);
    if (end == std::string_view::npos) {
      end = list.size();
    }
    RangeItem item = parse_range_item(list.substr(pos, end - pos));
    if (!item.token.empty()) {
      fn(item);
    }
    pos = end + 1;
  }
}

}  // namespace

float HttpAlternatePlugin::calculate_accept_quality(
    std::string_view accept, std::string_view content_type) {
  if (accept.empty()) {
    return 1.0f;
  }

  size_t slash = content_type.find('/');
  if (slash == std::string_view::npos) {
    return 0.0f;
  }

  std::string_view type = trim_ows(content_type.substr(0, slash));
  std::string_view subtype = content_type.substr(slash + 1);

  size_t semi = subtype.find(';');
  if (semi != std::string_view::npos) {
    subtype = subtype.substr(0, semi);
  }
  subtype = trim_ows(subtype);

  // The most specific matching range decides (RFC 9110 12.5.1): an exact
  // type/subtype beats type/*, which beats */*. Among equally specific
  // ranges the highest quality is used.
  int best_specificity = 0;
  float quality = 0.0f;

  for_each_range(accept, [&](const RangeItem &item) {
    std::string_view range = item.token;
    int specificity = 0;
    if (range == "*/*") {
      specificity = 1;
    } else if (range.size() > 2 && range.substr(range.size() - 2) == "/*") {
      if (iequal(range.substr(0, range.size() - 2), type)) {
        specificity = 2;
      }
    } else {
      size_t rslash = range.find('/');
      if (rslash != std::string_view::npos &&
          iequal(range.substr(0, rslash), type) &&
          iequal(range.substr(rslash + 1), subtype)) {
        specificity = 3;
      }
    }
    if (specificity == 0) {
      return;
    }
    if (specificity > best_specificity ||
        (specificity == best_specificity && item.q > quality)) {
      best_specificity = specificity;
      quality = item.q;
    }
  });

  return quality;
}

float HttpAlternatePlugin::calculate_accept_encoding_quality(
    std::string_view accept_encoding, std::string_view content_encoding) {
  if (accept_encoding.empty()) {
    return 1.0f;
  }

  // An exact coding beats "*" regardless of order.
  int best_specificity = 0;
  float quality = 0.0f;

  for_each_range(accept_encoding, [&](const RangeItem &item) {
    int specificity = 0;
    if (iequal(item.token, content_encoding)) {
      specificity = 2;
    } else if (item.token == "*") {
      specificity = 1;
    }
    if (specificity == 0) {
      return;
    }
    if (specificity > best_specificity ||
        (specificity == best_specificity && item.q > quality)) {
      best_specificity = specificity;
      quality = item.q;
    }
  });

  if (best_specificity > 0) {
    return quality;
  }
  return iequal(content_encoding, "identity") ? 1.0f : 0.0f;
}

float HttpAlternatePlugin::calculate_accept_language_quality(
    std::string_view accept_language, std::string_view content_language) {
  if (accept_language.empty()) {
    return 1.0f;
  }

  // The longest matching range decides; "*" is the least specific.
  size_t best_length = 0;
  bool matched = false;
  float quality = 0.0f;

  for_each_range(accept_language, [&](const RangeItem &item) {
    std::string_view lang = item.token;
    size_t length = 0;
    bool matches = false;
    if (lang == "*") {
      matches = true;
    } else if (iequal(lang, content_language)) {
      matches = true;
      length = lang.size();
    } else if (content_language.size() > lang.size() &&
               iequal(content_language.substr(0, lang.size()), lang) &&
               content_language[lang.size()] == '-') {
      matches = true;
      length = lang.size();
    }
    if (!matches) {
      return;
    }
    if (!matched || length > best_length ||
        (length == best_length && item.q > quality)) {
      matched = true;
      best_length = length;
      quality = item.q;
    }
  });

  return matched ? quality : 0.0f;
}

std::shared_ptr<CachePlugin> create_http_alternate_plugin() {
  return std::make_shared<HttpAlternatePlugin>();
}

}  // namespace cyclone
