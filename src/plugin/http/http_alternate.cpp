// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// HTTP Alternate Selection Plugin
// Implements RFC 7234 content negotiation for cache variants.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

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
  float best_quality = -1.0f;

  for (size_t i = 0; i < variants.size(); ++i) {
    const auto &variant = variants[i];

    auto meta_opt = variant.metadata.data();
    if (meta_opt.empty()) {
      continue;
    }

    HttpCacheAlt alt = HttpCacheAlt::deserialize(meta_opt);

    if (!check_vary_match(alt, ctx.request_headers)) {
      continue;
    }

    float quality = 1.0f;

    auto accept = parse_header_value(ctx.request_headers, "Accept");
    if (!accept.empty()) {
      auto content_type = alt.get_response_header("Content-Type");
      if (content_type) {
        quality *= calculate_accept_quality(accept, *content_type);
      }
    }

    auto accept_encoding =
        parse_header_value(ctx.request_headers, "Accept-Encoding");
    if (!accept_encoding.empty()) {
      auto content_encoding = alt.get_response_header("Content-Encoding");
      quality *= calculate_accept_encoding_quality(
          accept_encoding, content_encoding.value_or("identity"));
    }

    auto accept_language =
        parse_header_value(ctx.request_headers, "Accept-Language");
    if (!accept_language.empty()) {
      auto content_language = alt.get_response_header("Content-Language");
      if (content_language) {
        quality *= calculate_accept_language_quality(accept_language,
                                                     *content_language);
      }
    }

    if (quality > best_quality) {
      best_quality = quality;
      best_index = i;
    }
  }

  if (best_quality < 0.0f) {
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

  HttpCacheAlt alt = HttpCacheAlt::deserialize(meta_opt);

  if (alt.is_fresh()) {
    return FreshnessResult::Fresh;
  }

  auto cc = alt.get_response_header("Cache-Control");
  if (cc) {
    if (cc->find("must-revalidate") != std::string_view::npos ||
        cc->find("no-cache") != std::string_view::npos) {
      return FreshnessResult::MustRevalidate;
    }
  }

  return FreshnessResult::Stale;
}

double HttpAlternatePlugin::eviction_priority(const CacheVariant &variant) {
  double priority = 1.0;

  auto age_seconds =
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now() - variant.last_accessed)
          .count();
  priority += age_seconds / 3600.0;

  if (variant.hit_count > 0) {
    priority /= (1.0 + std::log(variant.hit_count + 1));
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
      if (cv != request_value) {
        return false;
      }
    }

    pos = (end < vary_value.size()) ? end + 1 : end;
  }

  return true;
}

float HttpAlternatePlugin::calculate_accept_quality(
    std::string_view accept, std::string_view content_type) {
  if (accept.empty()) {
    return 1.0f;
  }

  size_t slash = content_type.find('/');
  if (slash == std::string_view::npos) {
    return 0.0f;
  }

  std::string_view type = content_type.substr(0, slash);
  std::string_view subtype = content_type.substr(slash + 1);

  size_t semi = subtype.find(';');
  if (semi != std::string_view::npos) {
    subtype = subtype.substr(0, semi);
  }

  float best_quality = 0.0f;

  size_t pos = 0;
  while (pos < accept.size()) {
    size_t end = accept.find(',', pos);
    if (end == std::string_view::npos) {
      end = accept.size();
    }

    std::string_view media_range = accept.substr(pos, end - pos);

    while (!media_range.empty() && media_range.front() == ' ') {
      media_range.remove_prefix(1);
    }

    float q = 1.0f;
    size_t qpos = media_range.find(";q=");
    if (qpos != std::string_view::npos) {
      std::string_view qval = media_range.substr(qpos + 3);
      q = parse_q_value(qval);
      media_range = media_range.substr(0, qpos);
    }

    while (!media_range.empty() && media_range.back() == ' ') {
      media_range.remove_suffix(1);
    }

    bool matches = false;
    if (media_range == "*/*") {
      matches = true;
    } else if (media_range.size() > 2 &&
               media_range.substr(media_range.size() - 2) == "/*") {
      matches = (media_range.substr(0, media_range.size() - 2) == type);
    } else {
      matches = (media_range == content_type ||
                 media_range == std::string(type) + "/" + std::string(subtype));
    }

    if (matches && q > best_quality) {
      best_quality = q;
    }

    pos = (end < accept.size()) ? end + 1 : end;
  }

  return best_quality;
}

float HttpAlternatePlugin::calculate_accept_encoding_quality(
    std::string_view accept_encoding, std::string_view content_encoding) {
  if (accept_encoding.empty()) {
    return 1.0f;
  }

  size_t pos = 0;
  while (pos < accept_encoding.size()) {
    size_t end = accept_encoding.find(',', pos);
    if (end == std::string_view::npos) {
      end = accept_encoding.size();
    }

    std::string_view encoding = accept_encoding.substr(pos, end - pos);

    while (!encoding.empty() && encoding.front() == ' ') {
      encoding.remove_prefix(1);
    }

    float q = 1.0f;
    size_t qpos = encoding.find(";q=");
    if (qpos != std::string_view::npos) {
      std::string_view qval = encoding.substr(qpos + 3);
      q = parse_q_value(qval);
      encoding = encoding.substr(0, qpos);
    }

    while (!encoding.empty() && encoding.back() == ' ') {
      encoding.remove_suffix(1);
    }

    if (encoding == content_encoding || encoding == "*") {
      return q;
    }

    pos = (end < accept_encoding.size()) ? end + 1 : end;
  }

  return content_encoding == "identity" ? 1.0f : 0.0f;
}

float HttpAlternatePlugin::calculate_accept_language_quality(
    std::string_view accept_language, std::string_view content_language) {
  if (accept_language.empty()) {
    return 1.0f;
  }

  float best_quality = 0.0f;

  size_t pos = 0;
  while (pos < accept_language.size()) {
    size_t end = accept_language.find(',', pos);
    if (end == std::string_view::npos) {
      end = accept_language.size();
    }

    std::string_view lang = accept_language.substr(pos, end - pos);

    while (!lang.empty() && lang.front() == ' ') {
      lang.remove_prefix(1);
    }

    float q = 1.0f;
    size_t qpos = lang.find(";q=");
    if (qpos != std::string_view::npos) {
      std::string_view qval = lang.substr(qpos + 3);
      q = parse_q_value(qval);
      lang = lang.substr(0, qpos);
    }

    while (!lang.empty() && lang.back() == ' ') {
      lang.remove_suffix(1);
    }

    bool matches = lang == "*" || lang == content_language ||
                   (content_language.size() > lang.size() &&
                    content_language.substr(0, lang.size()) == lang &&
                    content_language[lang.size()] == '-');

    if (matches && q > best_quality) {
      best_quality = q;
    }

    pos = (end < accept_language.size()) ? end + 1 : end;
  }

  return best_quality;
}

std::shared_ptr<CachePlugin> create_http_alternate_plugin() {
  return std::make_shared<HttpAlternatePlugin>();
}

}  // namespace cyclone
