// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#ifndef CHROME_BROWSER_OHOS_URL_PROTECTION_RULE_SOURCES_H_
#define CHROME_BROWSER_OHOS_URL_PROTECTION_RULE_SOURCES_H_
#include <memory>
#include "base/callback_list.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/ohos/url_protection/url_protection.h"
class Profile;
class PrefRegistrySimple;
namespace chrome::ohos::url_protection {
// User subscriptions are separate from the package-trusted signed updater.
// One store per original Profile. OTR callers have read-only access.
class RuleSources {
 public:
  static RuleSources* Get(Profile* profile);
  static void RegisterPrefs(PrefRegistrySimple* registry);
  explicit RuleSources(Profile* profile);
  ~RuleSources();
  void Acquire();
  void Release();
  bool ready() const;
  bool usable() const;
  Result Match(const GURL& url) const;
  base::DictValue State() const;
  std::string Mutate(std::string_view action, const base::DictValue& command);
  // True means installed matching rules/config changed, false is status only.
  base::CallbackListSubscription Observe(
      base::RepeatingCallback<void(bool)> cb);
  base::WeakPtr<RuleSources> GetWeakPtr();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
  base::WeakPtrFactory<RuleSources> weak_factory_{this};
};
bool HandleRuleSourcesCommand(const ShellCommandContext& context,
                              std::string_view name,
                              const base::DictValue& command);
}  // namespace chrome::ohos::url_protection
#endif
