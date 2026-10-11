// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#ifndef CHROME_BROWSER_OHOS_URL_PROTECTION_RULE_UPDATER_H_
#define CHROME_BROWSER_OHOS_URL_PROTECTION_RULE_UPDATER_H_
#include <memory>
#include "base/callback_list.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/ohos/url_protection/local_rules.h"
class Profile;
class PrefRegistrySimple;
namespace chrome::ohos::url_protection {
// One public-rule store per original Profile; OTR clients share only these
// public rules and update scheduling. No navigation URL reaches this class.
class RuleUpdater {
 public:
  static RuleUpdater* Get(Profile* profile);
  static void RegisterPrefs(PrefRegistrySimple* registry);
  explicit RuleUpdater(Profile* profile);
  ~RuleUpdater();
  void Acquire();
  void Release();
  std::string RequestUpdate();
  std::shared_ptr<const LocalRules> rules() const;
  base::DictValue State() const;
  base::CallbackListSubscription Observe(base::RepeatingClosure callback);
  base::WeakPtr<RuleUpdater> GetWeakPtr();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
  base::WeakPtrFactory<RuleUpdater> weak_factory_{this};
};
}  // namespace chrome::ohos::url_protection
#endif
