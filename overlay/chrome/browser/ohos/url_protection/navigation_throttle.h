// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef CHROME_BROWSER_OHOS_URL_PROTECTION_NAVIGATION_THROTTLE_H_
#define CHROME_BROWSER_OHOS_URL_PROTECTION_NAVIGATION_THROTTLE_H_
#include <vector>
#include "base/callback_list.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "chrome/browser/ohos/url_protection/url_protection.h"
#include "content/public/browser/navigation_throttle.h"

namespace chrome::ohos::url_protection {
bool HandleSiteCommand(const ShellCommandContext& context,
                       std::string_view name,
                       const base::DictValue& command);
class NavigationThrottle : public content::NavigationThrottle {
 public:
  static void MaybeCreateAndAdd(content::NavigationThrottleRegistry& registry);
  explicit NavigationThrottle(content::NavigationThrottleRegistry& registry);
  ~NavigationThrottle() override;
  ThrottleCheckResult WillStartRequest() override;
  ThrottleCheckResult WillRedirectRequest() override;
  ThrottleCheckResult WillProcessResponse() override;
  const char* GetNameForLogging() override;

 private:
  void CheckCurrentUrl();
  void OnResult(int revision, Result result);
  void OnPolicyChanged();
  void ReportState();
  ThrottleCheckResult Block();
  base::WeakPtr<Service> service_;
  base::CallbackListSubscription subscription_;
  base::TimeTicks deadline_;
  std::vector<GURL> urls_;
  std::vector<std::string> requests_;
  Result risk_;
  Result latest_result_;
  int pending_ = 0;
  bool deferred_ = false;
  base::WeakPtrFactory<NavigationThrottle> weak_factory_{this};
};
}  // namespace chrome::ohos::url_protection
#endif
