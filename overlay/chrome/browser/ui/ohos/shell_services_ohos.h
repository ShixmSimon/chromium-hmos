// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_OHOS_SHELL_SERVICES_OHOS_H_
#define CHROME_BROWSER_UI_OHOS_SHELL_SERVICES_OHOS_H_

#include <stdint.h>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "base/functional/callback.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_observer.h"
#include "ui/gfx/native_ui_types.h"

class BrowserWindowInterface;
class GURL;

namespace chrome::ohos {

// Profile data the ArkUI shell draws its own pages for -- bookmarks, history,
// downloads, settings, favicons. The data stays in Chromium's Profile; the
// shell asks with browser commands and gets answers as runtime events, the
// same channel as everything else (docs/SHELL_CONTRACT.md).
//
// Each area lives in its own file (shell_bookmarks_ohos.cc and so on) and
// exposes one handler; this file routes to them and holds what they share.

// Where a shell command came from. `profile` is the profile of the browser
// window that sent it, so an incognito window works on its own profile.
struct ShellCommandContext {
  gfx::AcceleratedWidget widget = gfx::kNullAcceleratedWidget;
  BrowserWindowInterface* browser = nullptr;
  Profile* profile = nullptr;
};

// Answers the window that sent the command. Adds "incognito".
void ReplyToShell(const ShellCommandContext& context, base::DictValue event);

// Sends to every shell window showing `profile` -- for "something changed"
// events that no command asked for. Adds "incognito".
void BroadcastToShell(Profile* profile, base::DictValue event);

// Whether `name` is a command one of the shell services handles. The bridge's
// command allowlist accepts these.
bool IsShellServiceCommand(std::string_view name);

// Runs a shell service command. False when `name` is not one of them.
bool HandleShellServiceCommand(const ShellCommandContext& context,
                               std::string_view name,
                               const base::DictValue& command);

// Starts the observers that push change events (bookmarksChanged,
// historyChanged, downloadUpdated) and applies startup settings such as the
// download directory. Idempotent; the bridge calls it whenever it sees a
// browser, so observers exist before the shell first asks for anything.
void EnsureShellServices(Profile* profile);

// For BrowserStateSnapshot.bookmarked.
bool IsUrlBookmarked(Profile* profile, const GURL& url);

// --- Conventions every area follows. --------------------------------------

// Times cross to the shell as milliseconds since the Unix epoch (UTC); a null
// time is -1. Byte counts are numbers; unknown is -1.
double ToShellTime(base::Time time);
// Absent or negative means "no time" (a null base::Time).
base::Time FromShellTime(std::optional<double> ms);

// Chromium's int64 ids travel as strings: JavaScript numbers lose precision
// past 2^53.
std::string ToShellId(int64_t id);
std::optional<int64_t> FromShellId(const std::string* id);

// The "requestId" a query carries, echoed in its answer. 0 when absent.
int ReadRequestId(const base::DictValue& command);

// Per-profile state -- an observer, a task tracker -- that must not outlive
// its profile. Get() creates on first use; the entry is destroyed when the
// profile is, before its keyed services go away.
template <typename T>
class PerProfile : public ProfileObserver {
 public:
  using Factory = base::RepeatingCallback<std::unique_ptr<T>(Profile*)>;

  explicit PerProfile(Factory factory) : factory_(std::move(factory)) {}
  PerProfile(const PerProfile&) = delete;
  PerProfile& operator=(const PerProfile&) = delete;
  ~PerProfile() override {
    for (auto& [profile, entry] : entries_) {
      profile->RemoveObserver(this);
    }
  }

  T* Get(Profile* profile) {
    if (!profile) {
      return nullptr;
    }
    auto it = entries_.find(profile);
    if (it == entries_.end()) {
      it = entries_.emplace(profile, factory_.Run(profile)).first;
      profile->AddObserver(this);
    }
    return it->second.get();
  }

  T* Find(Profile* profile) {
    auto it = entries_.find(profile);
    return it == entries_.end() ? nullptr : it->second.get();
  }

  // ProfileObserver:
  void OnProfileWillBeDestroyed(Profile* profile) override {
    profile->RemoveObserver(this);
    entries_.erase(profile);
  }

 private:
  Factory factory_;
  std::map<Profile*, std::unique_ptr<T>> entries_;
};

// --- Per-area entry points, implemented in the area's own file. -----------

// Each returns true when it recognised and handled `name`.
bool HandleBookmarksCommand(const ShellCommandContext& context,
                            std::string_view name,
                            const base::DictValue& command);
bool HandleFaviconsCommand(const ShellCommandContext& context,
                           std::string_view name,
                           const base::DictValue& command);
bool HandleHistoryCommand(const ShellCommandContext& context,
                          std::string_view name,
                          const base::DictValue& command);
bool HandleDownloadsCommand(const ShellCommandContext& context,
                            std::string_view name,
                            const base::DictValue& command);
bool HandleSettingsCommand(const ShellCommandContext& context,
                           std::string_view name,
                           const base::DictValue& command);

// Each starts its area's observers for `profile`; idempotent.
void EnsureBookmarksObserver(Profile* profile);
void EnsureHistoryObserver(Profile* profile);
void EnsureDownloadsObserver(Profile* profile);

}  // namespace chrome::ohos

#endif  // CHROME_BROWSER_UI_OHOS_SHELL_SERVICES_OHOS_H_
