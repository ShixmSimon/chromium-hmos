// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "chrome/browser/ohos/url_protection/local_rules.h"

#include <algorithm>
#include <cmath>
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "chrome/browser/ohos/url_protection/url_protection.h"
#include "crypto/hash.h"

namespace chrome::ohos::url_protection {
namespace {
constexpr size_t kMaxBytes = 16 * 1024 * 1024;
constexpr size_t kMaxRules = 200000;
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
