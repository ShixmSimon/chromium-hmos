// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#include "chrome/browser/ohos/url_protection/subscription_rules.h"

#include <algorithm>
#include <cmath>
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "chrome/browser/ohos/url_protection/rule_update_format.h"
#include "chrome/browser/ohos/url_protection/url_protection.h"
#include "crypto/hash.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"

namespace chrome::ohos::url_protection {
namespace {
std::string CanonicalDomain(std::string_view text) {
  if (text.empty() || text.size() > 1024 ||
      text.find_first_of("/:@?#%*[]\\ \t\r\n") != std::string_view::npos)
    return {};
  const GURL url("https://" + std::string(text) + "/");
  if (!IsPublicHttpUrl(url) || url.HostIsIPAddress())
    return {};
  std::string host(url.host());
  if (host.ends_with('.'))
    host.pop_back();
  if (host.empty() || host.size() > 253 || host.starts_with('.') ||
      host.ends_with('.') || host.find("..") != std::string::npos)
    return {};
  size_t start = 0;
  while (start < host.size()) {
    const auto end = host.find('.', start);
    const auto label = std::string_view(host).substr(
        start, end == std::string::npos ? host.size() - start : end - start);
    if (label.empty() || label.size() > 63 || label.front() == '-' ||
        label.back() == '-' || !std::ranges::all_of(label, [](char c) {
          return base::IsAsciiAlphaNumeric(c) || c == '-';
        }))
      return {};
    if (end == std::string::npos)
      break;
    start = end + 1;
  }
  using namespace net::registry_controlled_domains;
  const auto registry = GetCanonicalHostRegistry(
      host, INCLUDE_UNKNOWN_REGISTRIES, INCLUDE_PRIVATE_REGISTRIES);
  return registry && !registry->empty() ? host : std::string();
}
}  // namespace
bool ValidSubscriptionEndpoint(const GURL& url) {
  return IsPublicHttpUrl(url) && url.SchemeIs("https") && !url.has_ref();
}
bool ValidSubscriptionFormat(std::string_view format,
                             std::string_view rule_type) {
  return (format == "domains" &&
          (rule_type == "host_exact" || rule_type == "host_suffix")) ||
         (format == "urls" && rule_type == "url_exact");
}
bool SubscriptionRules::usable(base::Time now) const {
  return error.empty() && !hashes.empty() && now >= fetched &&
         now < fetched + base::Days(7);
}
bool SubscriptionRules::Matches(const GURL& url, base::Time now) const {
  if (!usable(now) || !IsPublicHttpUrl(url))
    return false;
  auto contains = [&](std::string_view value) {
    return std::ranges::binary_search(hashes, crypto::hash::Sha256(value));
  };
  if (rule_type == "url_exact")
    return contains(url.spec());
  std::string host(url.host());
  if (host.ends_with('.'))
    host.pop_back();
  std::string_view candidate(host);
  for (;;) {
    if (contains(candidate))
      return true;
    const auto dot = candidate.find('.');
    if (rule_type != "host_suffix" || dot == std::string_view::npos)
      return false;
    candidate.remove_prefix(dot + 1);
  }
}
SubscriptionRules ParseSubscriptionRules(std::string_view text,
                                         std::string_view format,
                                         std::string_view rule_type,
                                         base::Time fetched) {
  SubscriptionRules rules;
  rules.error = "invalid_format";
  if (!ValidSubscriptionFormat(format, rule_type))
    return rules;
  if (text.size() > kSubscriptionMaxBytes) {
    rules.error = "byte_limit";
    return rules;
  }
  if (!base::IsStringUTF8(text) || text.find('\0') != std::string_view::npos) {
    rules.error = "invalid_utf8";
    return rules;
  }
  rules.digest =
      base::ToLowerASCII(base::HexEncode(crypto::hash::Sha256(text)));
  rules.rule_type = rule_type;
  rules.fetched = fetched;
  if (text.starts_with("\xEF\xBB\xBF"))
    text.remove_prefix(3);
  rules.hashes.reserve(std::min(text.size() / 32, kSubscriptionMaxRecords));
  while (!text.empty()) {
    const auto newline = text.find('\n');
    auto line = text.substr(0, newline);
    text.remove_prefix(newline == std::string_view::npos ? text.size()
                                                         : newline + 1);
    line = base::TrimWhitespaceASCII(line, base::TRIM_ALL);
    if (line.empty() || line.starts_with('#'))
      continue;
    if (++rules.input_count > kSubscriptionMaxRecords) {
      rules.error = "record_limit";
      rules.hashes.clear();
      return rules;
    }
    std::string canonical;
    if (format == "domains") {
      canonical = CanonicalDomain(line);
    } else if (line.find_first_of(" \t\r") == std::string_view::npos &&
               (line.starts_with("https://") || line.starts_with("http://"))) {
      const GURL url(line);
      if (IsPublicHttpUrl(url))
        canonical = url.spec();
    }
    if (canonical.empty()) {
      rules.error = "invalid_rule";
      rules.hashes.clear();
      return rules;
    }
    if (rules.hashes.size() == rules.hashes.capacity())
      rules.hashes.reserve(
          std::min(kSubscriptionMaxRecords,
                   std::max(size_t{1024}, rules.hashes.capacity() * 2)));
    rules.hashes.push_back(crypto::hash::Sha256(canonical));
  }
  std::ranges::sort(rules.hashes);
  auto duplicates = std::ranges::unique(rules.hashes);
  rules.hashes.erase(duplicates.begin(), duplicates.end());
  rules.hashes.shrink_to_fit();
  rules.error = rules.hashes.empty() ? "empty_rules" : "";
  return rules;
}
std::string EncodeSubscriptionCache(const GURL& endpoint,
                                    std::string_view format,
                                    const SubscriptionRules& rules,
                                    std::string_view raw) {
  auto header = base::WriteJson(
      base::DictValue()
          .Set("format", 1)
          .Set("url", endpoint.spec())
          .Set("inputFormat", std::string(format))
          .Set("ruleType", rules.rule_type)
          .Set("sha256", rules.digest)
          .Set("fetchedAt", rules.fetched.InMillisecondsFSinceUnixEpoch()));
  return header ? EncodeRuleCache(*header, raw) : std::string();
}
SubscriptionRules DecodeSubscriptionCache(std::string_view cache,
                                          const GURL& endpoint,
                                          std::string_view format,
                                          std::string_view rule_type,
                                          base::Time now) {
  SubscriptionRules invalid;
  invalid.error = "cache_invalid";
  std::string header, raw;
  if (!DecodeRuleCache(cache, header, raw))
    return invalid;
  auto meta = base::JSONReader::ReadDict(header, base::JSON_PARSE_RFC);
  if (!meta || meta->FindInt("format") != 1)
    return invalid;
  auto matches = [&](const char* key, std::string_view expected) {
    const auto* value = meta->FindString(key);
    return value && *value == expected;
  };
  const auto time = meta->FindDouble("fetchedAt");
  if (!matches("url", endpoint.spec()) || !matches("inputFormat", format) ||
      !matches("ruleType", rule_type) || !time || !std::isfinite(*time))
    return invalid;
  const auto fetched = base::Time::FromMillisecondsSinceUnixEpoch(*time);
  if (fetched > now)
    return invalid;
  if (fetched + base::Days(7) <= now) {
    invalid.error = "cache_expired";
    return invalid;
  }
  auto rules = ParseSubscriptionRules(raw, format, rule_type, fetched);
  if (!rules.usable(now) || !matches("sha256", rules.digest))
    return invalid;
  return rules;
}
}  // namespace chrome::ohos::url_protection
