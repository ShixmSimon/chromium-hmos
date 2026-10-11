// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "chrome/browser/ohos/url_protection/local_rules.h"

#include <algorithm>
#include <cmath>
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/strings/string_util.h"
#include "chrome/browser/ohos/url_protection/url_protection.h"
#include "crypto/hash.h"
#include "net/base/ip_address.h"

namespace chrome::ohos::url_protection {
namespace {
constexpr size_t kMaxBytes = 16 * 1024 * 1024;
constexpr size_t kMaxRules = 200000;
}  // namespace
bool IsPublicHttpUrl(const GURL& url) {
  if (!url.is_valid() || !url.SchemeIsHTTPOrHTTPS() || url.has_username() ||
      url.has_password() || url.spec().size() > 4096)
    return false;
  std::string host = url.HostNoBrackets();
  while (host.ends_with('.'))
    host.pop_back();
  net::IPAddress address;
  if (address.AssignFromIPLiteral(host)) {
    if (address.IsIPv4MappedIPv6())
      address = net::ConvertIPv4MappedIPv6ToIPv4(address);
    return address.IsPubliclyRoutable();
  }
  return host.find('.') != std::string::npos && host != "localhost" &&
         !host.ends_with(".localhost") && !host.ends_with(".local") &&
         !host.ends_with(".internal") && !host.ends_with(".home.arpa");
}

GURL CloudCheckUrl(const GURL& url) {
  if (!IsPublicHttpUrl(url))
    return {};
  // Conservative v1: paths with encoded or token-like segments stay local.
  // No promise to recognize every sensitive path or split-DNS hostname.
  const std::string path = base::ToLowerASCII(url.path());
  if (path.find('%') != std::string::npos ||
      path.find('@') != std::string::npos ||
      path.find("token") != std::string::npos ||
      path.find("oauth") != std::string::npos || path.size() > 256)
    return {};
  size_t segment = 0;
  for (char c : path) {
    segment = c == '/' ? 0 : segment + 1;
    if (segment > 48)
      return {};
  }
  GURL::Replacements replacements;
  replacements.ClearQuery();
  replacements.ClearRef();
  return url.ReplaceComponents(replacements);
}

bool LocalRules::IsUsable(base::Time now) const {
  return status == "ready" && now >= generated && now < expires;
}
bool LocalRules::Matches(const GURL& url, base::Time now) const {
  return IsUsable(now) && IsPublicHttpUrl(url) &&
         std::ranges::binary_search(hashes, crypto::hash::Sha256(url.spec()));
}
LocalRules ParseLocalRules(std::string_view json, base::Time now) {
  LocalRules rules;
  rules.status = "invalid";
  if (json.size() > kMaxBytes)
    return rules;
  auto root = base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  if (!root || root->FindInt("format") != 1)
    return rules;
  const auto* version = root->FindString("version");
  const auto* source = root->FindString("source");
  const auto* urls = root->FindList("urls");
  auto generated = root->FindDouble("generatedAt");
  auto expires = root->FindDouble("expiresAt");
  if (!version || version->empty() || version->size() > 80 || !source ||
      *source != "Phishing.Database" || !urls || urls->empty() ||
      urls->size() > kMaxRules || !generated || !expires ||
      !std::isfinite(*generated) || !std::isfinite(*expires))
    return rules;
  rules.generated = base::Time::FromMillisecondsSinceUnixEpoch(*generated);
  rules.expires = base::Time::FromMillisecondsSinceUnixEpoch(*expires);
  if (rules.generated > now || rules.expires <= rules.generated ||
      rules.expires - rules.generated > base::Days(7))
    return rules;
  rules.version = *version;
  if (rules.expires <= now) {
    rules.status = "expired";
    return rules;
  }
  for (const auto& item : *urls) {
    if (!item.is_string() || item.GetString().size() > 4096)
      return LocalRules{.status = "invalid"};
    GURL url(item.GetString());
    if (!IsPublicHttpUrl(url))
      return LocalRules{.status = "invalid"};
    rules.hashes.push_back(crypto::hash::Sha256(url.spec()));
  }
  std::ranges::sort(rules.hashes);
  auto duplicates = std::ranges::unique(rules.hashes);
  rules.hashes.erase(duplicates.begin(), duplicates.end());
  rules.status = "ready";
  return rules;
}
LocalRules LoadLocalRules(const base::FilePath& path) {
  std::string json;
  if (!base::PathExists(path))
    return {};
  if (!base::ReadFileToStringWithMaxSize(path, &json, kMaxBytes))
    return LocalRules{.status = "invalid"};
  return ParseLocalRules(json, base::Time::Now());
}
}  // namespace chrome::ohos::url_protection
