// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_OHOS_SHELL_SETTINGS_OHOS_INTERNAL_H_
#define CHROME_BROWSER_UI_OHOS_SHELL_SETTINGS_OHOS_INTERNAL_H_

#include <optional>

#include "base/values.h"
#include "chrome/browser/ui/ohos/shell_services_ohos.h"
#include "components/browsing_data/core/browsing_data_utils.h"

class Profile;

// Shared by the settings area's files only: shell_settings_ohos.cc routes the
// commands, and each of the other files implements one group of them. The
// shell's JSON shapes are documented next to each command.

namespace chrome::ohos::settings_internal {

// Settings belong to the regular profile, even when an incognito window asks:
// Chromium's own settings pages work the same way, and several writers (the
// default content settings among them) must not be called on an incognito
// profile.
Profile* SettingsProfile(const ShellCommandContext& context);

// Sends "prefsChanged" {keys} to the windows of the settings profile and,
// when it asked from incognito, to the incognito windows too.
void BroadcastPrefsChanged(const ShellCommandContext& context,
                           base::ListValue keys);

// A command handler. It validates its own input; a bad command is logged and
// dropped, and a query still gets an (empty) answer so the shell never waits.
using SettingsCommandHandler = void (*)(const ShellCommandContext& context,
                                        const base::DictValue& command);

// The command's "timeRange" ('lastHour' | 'lastDay' | 'lastWeek' |
// 'last4Weeks' | 'all'). Null, logged, when absent or unknown.
// shell_browsing_data_ohos.cc
std::optional<browsing_data::TimePeriod> ReadTimePeriod(
    const base::DictValue& command);

// shell_browsing_data_ohos.cc
void ClearBrowsingData(const ShellCommandContext& context,
                       const base::DictValue& command);

// shell_browsing_data_counts_ohos.cc
void GetBrowsingDataCounts(const ShellCommandContext& context,
                           const base::DictValue& command);

// shell_search_engines_ohos.cc
void GetSearchEngines(const ShellCommandContext& context,
                      const base::DictValue& command);
void SetDefaultSearchEngine(const ShellCommandContext& context,
                            const base::DictValue& command);

// shell_prefs_ohos.cc
void GetPrefs(const ShellCommandContext& context,
              const base::DictValue& command);
void SetPref(const ShellCommandContext& context,
             const base::DictValue& command);

// shell_site_settings_ohos.cc
void GetSiteSettings(const ShellCommandContext& context,
                     const base::DictValue& command);
void GetSiteSettingsForOrigin(const ShellCommandContext& context,
                              const base::DictValue& command);
void SetSiteSetting(const ShellCommandContext& context,
                    const base::DictValue& command);
void SetDefaultSiteSetting(const ShellCommandContext& context,
                           const base::DictValue& command);
void ResetSiteSettings(const ShellCommandContext& context,
                       const base::DictValue& command);

}  // namespace chrome::ohos::settings_internal

#endif  // CHROME_BROWSER_UI_OHOS_SHELL_SETTINGS_OHOS_INTERNAL_H_
