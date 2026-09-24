// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The shell's search engine picker.
//
//   getSearchEngines {requestId}
//     -> "searchEngines" {requestId,
//                         items: [{id, name, keyword, url, isDefault}]}
//        id is the TemplateURLID as a string; url has Chromium's
//        {searchTerms} placeholders.
//   setDefaultSearchEngine {id}
//     -> no answer; the next getSearchEngines shows the new default.
//
// The list is the one Chromium's settings offer as default candidates
// (TemplateURLService::ShowInDefaultList), not every keyword it knows.

#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "base/callback_list.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/search_engines/template_url_service_factory.h"
#include "chrome/browser/ui/ohos/shell_services_ohos.h"
#include "chrome/browser/ui/ohos/shell_settings_ohos_internal.h"
#include "components/search_engines/template_url.h"
#include "components/search_engines/template_url_service.h"

namespace chrome::ohos::settings_internal {

namespace {

base::DictValue EngineToShell(const TemplateURL& engine, bool is_default) {
  base::DictValue item;
  item.Set("id", ToShellId(engine.id()));
  item.Set("name", engine.short_name());
  item.Set("keyword", engine.keyword());
  item.Set("url", engine.url());
  item.Set("isDefault", is_default);
  return item;
}

void ReplyEngines(const ShellCommandContext& context,
                  int request_id,
                  TemplateURLService* service) {
  base::ListValue items;
  if (service && service->loaded()) {
    const TemplateURL* default_engine = service->GetDefaultSearchProvider();
    for (const TemplateURL* engine : service->GetTemplateURLs()) {
      if (service->ShowInDefaultList(engine)) {
        items.Append(EngineToShell(*engine, engine == default_engine));
      }
    }
  }
  base::DictValue event;
  event.Set("event", "searchEngines");
  event.Set("requestId", request_id);
  event.Set("items", std::move(items));
  ReplyToShell(context, std::move(event));
}

// getSearchEngines that arrived before the keywords database loaded. Owned
// per asking profile, so the load subscription goes before the service does
// and an incognito window that closes takes its pending answers with it.
class PendingEngineQueries {
 public:
  explicit PendingEngineQueries(Profile* asking_profile)
      : settings_profile_(asking_profile->GetOriginalProfile()) {}
  PendingEngineQueries(const PendingEngineQueries&) = delete;
  PendingEngineQueries& operator=(const PendingEngineQueries&) = delete;
  ~PendingEngineQueries() = default;

  void Add(const ShellCommandContext& context,
           int request_id,
           TemplateURLService* service) {
    pending_.emplace_back(context, request_id);
    if (!subscription_) {
      subscription_ = service->RegisterOnLoadedCallback(base::BindOnce(
          &PendingEngineQueries::OnLoaded, weak_factory_.GetWeakPtr()));
    }
    // Starts loading if nothing has yet; a no-op otherwise.
    service->Load();
  }

 private:
  void OnLoaded() {
    subscription_ = {};
    std::vector<std::pair<ShellCommandContext, int>> pending;
    pending.swap(pending_);
    TemplateURLService* service =
        TemplateURLServiceFactory::GetForProfile(settings_profile_);
    for (const auto& [context, request_id] : pending) {
      ReplyEngines(context, request_id, service);
    }
  }

  const raw_ptr<Profile> settings_profile_;
  std::vector<std::pair<ShellCommandContext, int>> pending_;
  base::CallbackListSubscription subscription_;
  base::WeakPtrFactory<PendingEngineQueries> weak_factory_{this};
};

PerProfile<PendingEngineQueries>& PendingPerProfile() {
  static base::NoDestructor<PerProfile<PendingEngineQueries>> pending(
      base::BindRepeating([](Profile* profile) {
        return std::make_unique<PendingEngineQueries>(profile);
      }));
  return *pending;
}

TemplateURL* FindEngine(TemplateURLService* service, int64_t id) {
  for (TemplateURL* engine : service->GetTemplateURLs()) {
    if (engine->id() == id) {
      return engine;
    }
  }
  return nullptr;
}

}  // namespace

void GetSearchEngines(const ShellCommandContext& context,
                      const base::DictValue& command) {
  const int request_id = ReadRequestId(command);
  Profile* profile = SettingsProfile(context);
  TemplateURLService* service =
      profile ? TemplateURLServiceFactory::GetForProfile(profile) : nullptr;
  if (!service || service->loaded()) {
    ReplyEngines(context, request_id, service);
    return;
  }
  PendingPerProfile().Get(context.profile)->Add(context, request_id, service);
}

void SetDefaultSearchEngine(const ShellCommandContext& context,
                            const base::DictValue& command) {
  Profile* profile = SettingsProfile(context);
  TemplateURLService* service =
      profile ? TemplateURLServiceFactory::GetForProfile(profile) : nullptr;
  const std::optional<int64_t> id = FromShellId(command.FindString("id"));
  if (!service || !service->loaded() || !id) {
    LOG(WARNING) << "OHOS shell setDefaultSearchEngine: bad id or engines "
                    "not loaded";
    return;
  }
  TemplateURL* engine = FindEngine(service, *id);
  // CanMakeDefault is false for every engine while policy or an extension
  // controls the default; setting it then would assert.
  if (!engine || !service->CanMakeDefault(engine)) {
    LOG(WARNING) << "OHOS shell setDefaultSearchEngine: engine " << *id
                 << " missing or cannot be the default";
    return;
  }
  service->SetUserSelectedDefaultSearchProvider(engine);
}

}  // namespace chrome::ohos::settings_internal
