// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Download settings the shell decides: who draws download UI, and where
// downloads go. Kept apart from the downloads service (shell_downloads_ohos.cc)
// because upstream files reach these through shell_downloads_ohos.h.

#include "chrome/browser/ui/ohos/shell_downloads_ohos.h"

#include <optional>
#include <string>
#include <string_view>

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/strings/escape.h"
#include "base/task/task_traits.h"
#include "base/task/thread_pool.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/ohos/aura_shell_runtime_bridge.h"
#include "chrome/common/pref_names.h"
#include "components/prefs/pref_service.h"

namespace chrome::ohos {

namespace {

constexpr char kDownloadUiSwitch[] = "ohos-download-ui";
constexpr char kDownloadDirSwitch[] = "ohos-download-dir";
// HarmonyOS hands out the user's Download folder as a document URI; the rest
// of it is the escaped sandbox path.
constexpr std::string_view kHarmonyDocsUriPrefix = "file://docs";

// The directory a shell named -- a plain path or a HarmonyOS
// "file://docs/..." URI -- or nullopt (logged) when it is empty or not a
// usable absolute path.
std::optional<base::FilePath> ParseShellDownloadDirectory(std::string value) {
  if (value.empty()) {
    return std::nullopt;
  }
  if (value.starts_with(kHarmonyDocsUriPrefix)) {
    value = base::UnescapeURLComponent(
        std::string_view(value).substr(kHarmonyDocsUriPrefix.size()),
        base::UnescapeRule::SPACES | base::UnescapeRule::PATH_SEPARATORS |
            base::UnescapeRule::URL_SPECIAL_CHARS_EXCEPT_PATH_SEPARATORS);
  }
  const base::FilePath path = base::FilePath::FromUTF8Unsafe(value);
  if (!path.IsAbsolute() || path.ReferencesParent()) {
    LOG(WARNING) << "OHOS shell downloads: ignoring download directory "
                 << value;
    return std::nullopt;
  }
  return path;
}

void EnsureDirectoryExists(const base::FilePath& path) {
  if (!base::DirectoryExists(path) && !base::CreateDirectory(path)) {
    LOG(WARNING) << "OHOS shell downloads: cannot create " << path;
  }
}

void UseDownloadDirectory(Profile* profile, const base::FilePath& directory) {
  // Incognito reads these through to the original profile's prefs.
  PrefService* prefs = profile->GetOriginalProfile()->GetPrefs();
  prefs->SetFilePath(prefs::kDownloadDefaultDirectory, directory);
  prefs->SetFilePath(prefs::kSaveFileDefaultDirectory, directory);
  prefs->SetBoolean(prefs::kPromptForDownload, false);
  // Downloads would fail with FILE_FAILED into a missing directory.
  base::ThreadPool::PostTask(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
       base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN},
      base::BindOnce(&EnsureDirectoryExists, directory));
}

}  // namespace

bool ShouldShellDrawDownloadUi() {
  const std::string mode =
      base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
          kDownloadUiSwitch);
  if (mode == "shell") {
    return true;
  }
  if (mode == "native") {
    return false;
  }
  return IsAuraShellMobilePhoneUi();
}

// The directory the shell asked for most recently, and the one last applied.
// A request can come before any profile exists -- the shell learns the
// directory as its page appears, while Chromium is still starting -- so it is
// kept here and applied once there is a profile to apply it to.
struct ShellDownloadDirectoryState {
  std::string requested;
  std::string applied;
};

ShellDownloadDirectoryState& DirectoryState() {
  static base::NoDestructor<ShellDownloadDirectoryState> state([] {
    ShellDownloadDirectoryState initial;
    initial.requested =
        base::CommandLine::ForCurrentProcess()->GetSwitchValueUTF8(
            kDownloadDirSwitch);
    return initial;
  }());
  return *state;
}

void ApplyShellDownloadDirectory(Profile* profile) {
  ShellDownloadDirectoryState& state = DirectoryState();
  if (!profile || state.requested == state.applied) {
    return;
  }
  state.applied = state.requested;
  if (const std::optional<base::FilePath> directory =
          ParseShellDownloadDirectory(state.requested)) {
    UseDownloadDirectory(profile, *directory);
  }
}

bool SetShellDownloadDirectory(Profile* profile, const std::string& location) {
  if (!ParseShellDownloadDirectory(location)) {
    return false;
  }
  DirectoryState().requested = location;
  ApplyShellDownloadDirectory(profile);
  return true;
}

}  // namespace chrome::ohos
