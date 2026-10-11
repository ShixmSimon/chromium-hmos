// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef CHROME_BROWSER_OHOS_URL_PROTECTION_LOCAL_RULES_H_
#define CHROME_BROWSER_OHOS_URL_PROTECTION_LOCAL_RULES_H_

#include <array>
#include <string>
#include <string_view>
#include <vector>
#include "base/files/file_path.h"
#include "base/time/time.h"
#include "url/gurl.h"

namespace chrome::ohos::url_protection {
// A package-shipped snapshot, never a file supplied by a web page or shell
// command. Downloaded feeds require the separate signed updater (not v1).
struct LocalRules {
  std::string status = "missing";
  std::string version;
  base::Time generated;
  base::Time expires;
  std::vector<std::array<uint8_t, 32>> hashes;
  bool IsUsable(base::Time now) const;
  bool Matches(const GURL& url, base::Time now) const;
};
LocalRules ParseLocalRules(std::string_view json, base::Time now);
LocalRules LoadLocalRules(const base::FilePath& path);
}  // namespace chrome::ohos::url_protection
#endif
