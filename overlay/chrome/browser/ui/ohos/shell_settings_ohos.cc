// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The settings area of the shell services: routes the settings commands to
// the file that implements each group, and answers getAboutInfo.
//
//   getAboutInfo {requestId}
//     -> "aboutInfo" {requestId, chromiumVersion, engineCommit, userAgent,
//                     bookmarkApiVersion}

#include <string>
#include <string_view>
#include <utility>

#include "base/values.h"
#include "base/version_info/version_info.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/ohos/shell_services_ohos.h"
#include "chrome/browser/ui/ohos/shell_settings_ohos_internal.h"
#include "components/embedder_support/user_agent_utils.h"

namespace chrome::ohos {

namespace settings_internal {

Profile* SettingsProfile(const ShellCommandContext& context) {
  return context.profile ? context.profile->GetOriginalProfile() : nullptr;
}

void BroadcastPrefsChanged(const ShellCommandContext& context,
                           base::ListValue keys) {
  Profile* profile = SettingsProfile(context);
  if (!profile) {
    return;
  }
  base::DictValue event;
  event.Set("event", "prefsChanged");
  event.Set("keys", std::move(keys));
  BroadcastToShell(profile, event.Clone());
  // An incognito window reads through to the same settings, so it has to
  // hear about the change as well.
  if (context.profile != profile) {
    BroadcastToShell(context.profile, std::move(event));
  }
}

}  // namespace settings_internal

namespace {

using settings_internal::SettingsCommandHandler;

void GetAboutInfo(const ShellCommandContext& context,
                  const base::DictValue& command) {
  base::DictValue event;
  event.Set("event", "aboutInfo");
  event.Set("requestId", ReadRequestId(command));
  event.Set("chromiumVersion", version_info::GetVersionNumber());
  // The Chromium revision this build is cut from; empty in builds made
  // without a checkout that knows it.
  event.Set("engineCommit", version_info::GetLastChange());
  event.Set("userAgent", embedder_support::GetUserAgent());
  // What the bookmarks commands understand. 1 was the original set; 2 adds
  // insertion positions, operation results, child counts, root types, batch
  // moves and removals, URL lookup and path lookup; 3 adds import and export.
  // A shell reads it once at startup instead of probing each new command with
  // a timeout.
  event.Set("bookmarkApiVersion", 3);
  // What the browsing commands understand. 1 was the set the browsing-UI
  // round added -- permissions, load progress, tab ids, thumbnails, recently
  // closed, desktop site, reader mode; 2 adds tab groups, which is groupId
  // and openerId on each tab, newTab's groupId and background, activateTabById
  // and closeTabById, and the openedInGroup event; 3 adds reading and
  // restoring where the reader is in a page, for app continuation.
  event.Set("browsingApiVersion", 3);
  ReplyToShell(context, std::move(event));
}

struct SettingsCommand {
  std::string_view name;
  SettingsCommandHandler handler;
};

// Every settings command the shell may send. Adding one is one line here and
// a handler in the file of its group.
constexpr SettingsCommand kSettingsCommands[] = {
    // Browsing data (shell_browsing_data_ohos.cc).
    {"clearBrowsingData", &settings_internal::ClearBrowsingData},
    {"getBrowsingDataCounts", &settings_internal::GetBrowsingDataCounts},
    // Search engines (shell_search_engines_ohos.cc).
    {"getSearchEngines", &settings_internal::GetSearchEngines},
    {"setDefaultSearchEngine", &settings_internal::SetDefaultSearchEngine},
    // Preferences (shell_prefs_ohos.cc).
    {"getPrefs", &settings_internal::GetPrefs},
    {"setPref", &settings_internal::SetPref},
    // Site settings (shell_site_settings_ohos.cc).
    {"getSiteSettings", &settings_internal::GetSiteSettings},
    {"getSiteSettingsForOrigin", &settings_internal::GetSiteSettingsForOrigin},
    {"setSiteSetting", &settings_internal::SetSiteSetting},
    {"setDefaultSiteSetting", &settings_internal::SetDefaultSiteSetting},
    {"resetSiteSettings", &settings_internal::ResetSiteSettings},
    // About.
    {"getAboutInfo", &GetAboutInfo},
};

}  // namespace

bool HandleSettingsCommand(const ShellCommandContext& context,
                           std::string_view name,
                           const base::DictValue& command) {
  for (const SettingsCommand& entry : kSettingsCommands) {
    if (entry.name == name) {
      entry.handler(context, command);
      return true;
    }
  }
  return false;
}

}  // namespace chrome::ohos
