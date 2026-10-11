// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_OHOS_URL_PROTECTION_URL_PROTECTION_H_
#define CHROME_BROWSER_OHOS_URL_PROTECTION_URL_PROTECTION_H_

#include <memory>
#include <string>
#include <string_view>

#include "base/callback_list.h"
#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/values.h"
#include "url/gurl.h"

class PrefRegistrySimple;
class Profile;
namespace chrome::ohos {
struct ShellCommandContext;
}
namespace chrome::ohos::url_protection {

enum class Verdict { kUnknown, kNoMatch, kPhishing, kMalware, kOther };
struct Result {
  Verdict verdict = Verdict::kUnknown;
  std::string source;
  std::string reason;
  GURL url;
  bool dangerous() const {
    return verdict == Verdict::kPhishing || verdict == Verdict::kMalware;
  }
};

// Both local rules and navigations use GURL, preserving path/query/fragment.
bool IsPublicHttpUrl(const GURL& url);
GURL CloudCheckUrl(const GURL& url);
void RegisterPrefs(PrefRegistrySimple* registry);
bool HandleCommand(const ShellCommandContext& context,
                   std::string_view name,
                   const base::DictValue& command);

class Service {
 public:
  using Callback = base::OnceCallback<void(Result)>;
  static Service* Get(Profile* profile);
  explicit Service(Profile* profile);
  ~Service();
  Service(const Service&) = delete;
  Service& operator=(const Service&) = delete;

  bool enabled() const;
  bool uses_local() const;
  bool uses_huawei() const;
  int revision() const;
  base::DictValue State() const;
  std::string Configure(const base::DictValue& command);
  void SetLegacyEnabled(bool enabled);
  base::CallbackListSubscription Observe(base::RepeatingClosure callback);
  Result KnownRisk(const GURL& url) const;
  // Always completes asynchronously, bounded by timeout. No URL is logged.
  std::string Check(const GURL& url,
                    base::TimeDelta timeout,
                    Callback callback);
  void Cancel(const std::string& request_id);
  base::WeakPtr<Service> GetWeakPtr();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
  base::WeakPtrFactory<Service> weak_factory_{this};
};

}  // namespace chrome::ohos::url_protection
#endif
