// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BASE_PROCESS_LAUNCH_OHOS_H_
#define BASE_PROCESS_LAUNCH_OHOS_H_

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/base_export.h"
#include "base/environment.h"
#include "base/files/file_path.h"
#include "base/process/launch.h"
#include "base/process/process.h"

namespace base::internal {

struct BASE_EXPORT OhosNativeChildParams {
  std::vector<std::string> argv;
  EnvironmentMap environment;
  bool clear_environment = false;
  FilePath current_directory;
  FilePath resources_directory;
};

BASE_EXPORT Process LaunchProcessOhos(const std::vector<std::string>& argv,
                                      const LaunchOptions& options);

// Returns the signal reported by HarmonyOS for a native child that has exited.
// A value of zero represents a normal exit. Native children are spawned by the
// system, so waitpid() cannot provide this information to the browser process.
BASE_EXPORT std::optional<int> GetOhosNativeChildExitSignal(
    ProcessHandle handle);

BASE_EXPORT bool DecodeOhosNativeChildParams(std::string_view encoded,
                                             OhosNativeChildParams* params);

// Starts --type=gpu-process when the GPU is not in the browser process.
//
// The GPU process has to be started differently from a renderer -- it needs
// an IPC channel to its parent to receive the windows it draws into -- and
// that channel is ozone's business, so ozone registers the launcher here.
// `fds` pairs each descriptor to pass with its number in the child, the way
// LaunchOptions::fds_to_remap does. Returns the child's pid, or
// kNullProcessId.
using OhosGpuChildLauncher =
    ProcessId (*)(const std::string& encoded_params,
                  const std::vector<std::pair<int, int>>& fds);
BASE_EXPORT void SetOhosGpuChildLauncher(OhosGpuChildLauncher launcher);

}  // namespace base::internal

#endif  // BASE_PROCESS_LAUNCH_OHOS_H_
