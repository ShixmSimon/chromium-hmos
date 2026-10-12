// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#ifndef CHROME_BROWSER_OHOS_URL_PROTECTION_SUBSCRIPTION_RULES_H_
#define CHROME_BROWSER_OHOS_URL_PROTECTION_SUBSCRIPTION_RULES_H_

#include <array>
#include <string>
#include <string_view>
#include <vector>
#include "base/time/time.h"
#include "url/gurl.h"

namespace chrome::ohos::url_protection {
constexpr size_t kSubscriptionMaxBytes = 16 * 1024 * 1024;
constexpr size_t kSubscriptionMaxRecords = 500000;
constexpr size_t kSubscriptionTotalRecords = 1000000;
constexpr size_t kSubscriptionMaxSources = 8;
// Immutable sorted hashes: at most 32 MB of installed subscription indexes.
// Parsing one replacement can temporarily use another 16 MB of index memory.
struct SubscriptionRules {
  std::string error;
  std::string rule_type;
  std::string digest;
  size_t input_count = 0;
  size_t rejected_broad_count = 0;
  base::Time fetched;
  std::vector<std::array<uint8_t, 32>> hashes;
  bool usable(base::Time now) const;
  bool Matches(const GURL& url, base::Time now) const;
};
bool ValidSubscriptionEndpoint(const GURL& url);
bool ValidSubscriptionFormat(std::string_view format,
                             std::string_view rule_type);
SubscriptionRules ParseSubscriptionRules(std::string_view text,
                                         std::string_view format,
                                         std::string_view rule_type,
                                         base::Time fetched);
// Cache binds raw bytes to the exact subscription configuration. These are
// integrity checks for a user-trusted feed, not an official digital signature.
std::string EncodeSubscriptionCache(const GURL& endpoint,
                                    std::string_view format,
                                    const SubscriptionRules& rules,
                                    std::string_view raw);
SubscriptionRules DecodeSubscriptionCache(std::string_view cache,
                                          const GURL& endpoint,
                                          std::string_view format,
                                          std::string_view rule_type,
                                          base::Time now);
}  // namespace chrome::ohos::url_protection
#endif
