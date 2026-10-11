// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#ifndef CHROME_BROWSER_OHOS_URL_PROTECTION_RULE_UPDATE_FORMAT_H_
#define CHROME_BROWSER_OHOS_URL_PROTECTION_RULE_UPDATE_FORMAT_H_

#include <map>
#include <optional>
#include <string>
#include "base/files/file_path.h"
#include "chrome/browser/ohos/url_protection/local_rules.h"

namespace chrome::ohos::url_protection {
inline constexpr size_t kMaxRuleBytes = 16 * 1024 * 1024;
inline constexpr size_t kMaxManifestBytes = 16 * 1024;
struct UpdateConfig {
  GURL manifest_url;
  int minimum_sequence = 0;
  std::map<std::string, std::string> keys;
  std::string status = "not_configured";
  bool valid() const { return status == "ready"; }
};
struct RuleManifest {
  int sequence = 0;
  int size = 0;
  int count = 0;
  GURL rules_url;
  std::string sha256;
  std::string version;
  base::Time generated;
  base::Time expires;
};
UpdateConfig ParseUpdateConfig(std::string_view json);
std::optional<RuleManifest> VerifyRuleManifest(std::string_view json,
                                               const UpdateConfig& config,
                                               int minimum_sequence,
                                               base::Time now);
std::optional<LocalRules> VerifyRulePayload(std::string_view payload,
                                            const RuleManifest& manifest,
                                            base::Time now);
// Cache contains the original signature and exact payload bytes, and is
// reverified on every launch. It is public threat data, not browsing history.
std::string EncodeRuleCache(std::string_view manifest,
                            std::string_view payload);
bool DecodeRuleCache(std::string_view cache,
                     std::string& manifest,
                     std::string& payload);
}  // namespace chrome::ohos::url_protection
#endif
