// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/ohos/shell_services_ohos.h"

#include <algorithm>
#include <utility>

#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "chrome/browser/ui/ohos/aura_shell_runtime_bridge.h"
#include "chrome/browser/ui/ohos/shell_downloads_ohos.h"
#include "chrome/browser/ui/ohos/shell_password_cleanup_ohos.h"

namespace chrome::ohos {

namespace {

using AreaHandler = bool (*)(const ShellCommandContext&,
                             std::string_view,
                             const base::DictValue&);

// Which area answers which command. The bridge's allowlist is this table, so a
// new command is one line here plus its handler.
struct ShellServiceCommand {
  std::string_view name;
  AreaHandler handler;
};

constexpr ShellServiceCommand kShellServiceCommands[] = {
    // Bookmarks.
    {"getBookmarks", &HandleBookmarksCommand},
    {"searchBookmarks", &HandleBookmarksCommand},
    {"addBookmark", &HandleBookmarksCommand},
    {"removeBookmarkByUrl", &HandleBookmarksCommand},
    {"updateBookmark", &HandleBookmarksCommand},
    {"moveBookmark", &HandleBookmarksCommand},
    {"removeBookmark", &HandleBookmarksCommand},
    {"createBookmarkFolder", &HandleBookmarksCommand},
    {"getBookmarksForUrl", &HandleBookmarksCommand},
    {"getBookmarkPath", &HandleBookmarksCommand},
    {"getRecentBookmarks", &HandleBookmarksCommand},
    {"moveBookmarks", &HandleBookmarksCommand},
    {"removeBookmarks", &HandleBookmarksCommand},
    {"exportBookmarks", &HandleBookmarksCommand},
    {"importBookmarks", &HandleBookmarksCommand},
    // History, and the omnibox data built on it.
    {"queryHistory", &HandleHistoryCommand},
    {"removeHistoryItems", &HandleHistoryCommand},
    {"removeHistoryForUrl", &HandleHistoryCommand},
    {"clearHistory", &HandleHistoryCommand},
    {"autocomplete", &HandleHistoryCommand},
    {"getTopSites", &HandleHistoryCommand},
    {"removeTopSite", &HandleHistoryCommand},
    // Downloads.
    {"listDownloads", &HandleDownloadsCommand},
    {"downloadAction", &HandleDownloadsCommand},
    {"setDownloadDirectory", &HandleDownloadsCommand},
    // Settings.
    {"clearBrowsingData", &HandleSettingsCommand},
    {"getBrowsingDataCounts", &HandleSettingsCommand},
    {"getSearchEngines", &HandleSettingsCommand},
    {"setDefaultSearchEngine", &HandleSettingsCommand},
    {"getPrefs", &HandleSettingsCommand},
    {"setPref", &HandleSettingsCommand},
    {"getSiteSettings", &HandleSettingsCommand},
    {"getSiteSettingsForOrigin", &HandleSettingsCommand},
    {"setSiteSetting", &HandleSettingsCommand},
    {"setDefaultSiteSetting", &HandleSettingsCommand},
    {"resetSiteSettings", &HandleSettingsCommand},
    {"getAboutInfo", &HandleSettingsCommand},
    // Favicons, shared by every list the shell draws.
    {"getFavicons", &HandleFaviconsCommand},
};

const ShellServiceCommand* FindCommand(std::string_view name) {
  const auto* it = std::ranges::find(kShellServiceCommands, name,
                                     &ShellServiceCommand::name);
  return it == std::ranges::end(kShellServiceCommands) ? nullptr : it;
}

void MarkIncognito(Profile* profile, base::DictValue& event) {
  event.Set("incognito", profile && profile->IsOffTheRecord());
}

}  // namespace

void ReplyToShell(const ShellCommandContext& context, base::DictValue event) {
  MarkIncognito(context.profile, event);
  DispatchAuraShellRuntimeEventToWidget(context.widget, std::move(event));
}

void BroadcastToShell(Profile* profile, base::DictValue event) {
  MarkIncognito(profile, event);
  DispatchAuraShellRuntimeEventToProfile(profile, event);
}

bool IsShellServiceCommand(std::string_view name) {
  return FindCommand(name) != nullptr;
}

bool HandleShellServiceCommand(const ShellCommandContext& context,
                               std::string_view name,
                               const base::DictValue& command) {
  const ShellServiceCommand* entry = FindCommand(name);
  if (!entry) {
    return false;
  }
  if (!context.profile) {
    // Returning false here drops the command with no reply, and a shell
    // waiting on a requestId cannot tell that from a slow answer. Say so.
    LOG(ERROR) << "OHOS shell services: dropping '" << name
               << "' -- the command arrived with no profile";
    return false;
  }
  EnsureShellServices(context.profile);
  return entry->handler(context, name, command);
}

// Adding a bookmark command means touching two lists in two files, and
// forgetting the second one costs nothing at build time and everything at
// run time: the bridge drops the command and the shell waits for a reply that
// never comes. Say so once, in the log, the first time services start.
void WarnAboutUnreachableCommands() {
  for (std::string_view name : BookmarksCommandNames()) {
    if (!FindCommand(name)) {
      LOG(ERROR) << "OHOS shell services: bookmark command '" << name
                 << "' is not in kShellServiceCommands and will be dropped";
    }
  }
}

void EnsureShellServices(Profile* profile) {
  if (!profile) {
    return;
  }
  [[maybe_unused]] static const bool checked = [] {
    WarnAboutUnreachableCommands();
    return true;
  }();
  // Before anything else touches the profile: passwords saved while this
  // platform had nowhere safe to keep them are cleared once.
  ClearPasswordsSavedWithoutProtection(profile);
  ApplyShellDownloadDirectory(profile);
  EnsureBookmarksObserver(profile);
  EnsureHistoryObserver(profile);
  EnsureDownloadsObserver(profile);
}

double ToShellTime(base::Time time) {
  return time.is_null() ? -1.0
                        : time.InMillisecondsFSinceUnixEpochIgnoringNull();
}

base::Time FromShellTime(std::optional<double> ms) {
  if (!ms || *ms < 0) {
    return base::Time();
  }
  return base::Time::FromMillisecondsSinceUnixEpoch(*ms);
}

std::string ToShellId(int64_t id) {
  return base::NumberToString(id);
}

std::optional<int64_t> FromShellId(const std::string* id) {
  int64_t value = 0;
  if (!id || !base::StringToInt64(*id, &value)) {
    return std::nullopt;
  }
  return value;
}

int ReadRequestId(const base::DictValue& command) {
  return command.FindInt("requestId").value_or(0);
}

}  // namespace chrome::ohos
