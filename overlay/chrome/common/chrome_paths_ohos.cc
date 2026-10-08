// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/base_paths.h"
#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/path_service.h"
#include "base/process/launch_ohos.h"
#include "chrome/common/chrome_paths.h"
#include "chrome/common/chrome_paths_internal.h"
#include "chrome/common/chrome_switches.h"

namespace chrome {
namespace {

bool GetHomeSubdirectory(const char* name, base::FilePath* result) {
  base::FilePath home;
  if (!base::PathService::Get(base::DIR_HOME, &home)) {
    return false;
  }
  *result = home.AppendASCII(name);
  return true;
}

}  // namespace

bool GetDefaultUserDataDirectory(base::FilePath* result) {
  const base::CommandLine* command_line =
      base::CommandLine::ForCurrentProcess();
  if (command_line && command_line->HasSwitch(switches::kUserDataDir)) {
    base::FilePath user_data_dir =
        command_line->GetSwitchValuePath(switches::kUserDataDir);
    if (!user_data_dir.empty()) {
      *result = user_data_dir;
      return true;
    }
  }

  base::FilePath home;
  if (!base::PathService::Get(base::DIR_HOME, &home)) {
    return false;
  }
  *result = home.AppendASCII("chromium");
  return true;
}

void GetUserCacheDirectory(const base::FilePath& profile_dir,
                           base::FilePath* result) {
  *result = profile_dir.AppendASCII("Cache");
}

bool GetUserDocumentsDirectory(base::FilePath* result) {
  return GetHomeSubdirectory("Documents", result);
}

bool GetUserDownloadsDirectorySafe(base::FilePath* result) {
  return GetHomeSubdirectory("Downloads", result);
}

bool GetUserDownloadsDirectory(base::FilePath* result) {
  return GetHomeSubdirectory("Downloads", result);
}

bool GetUserMusicDirectory(base::FilePath* result) {
  return GetHomeSubdirectory("Music", result);
}

bool GetUserPicturesDirectory(base::FilePath* result) {
  return GetHomeSubdirectory("Pictures", result);
}

bool GetUserVideosDirectory(base::FilePath* result) {
  return GetHomeSubdirectory("Videos", result);
}

bool ProcessNeedsProfileDir(const std::string& process_type) {
  return !base::internal::IsOhosIsolatedRenderer();
}

}  // namespace chrome
