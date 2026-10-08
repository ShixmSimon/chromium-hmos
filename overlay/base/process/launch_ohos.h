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

#include "AbilityKit/native_child_process.h"
#include "base/base_export.h"
#include "base/environment.h"
#include "base/files/file_path.h"
#include "base/process/launch.h"
#include "base/process/process.h"

struct Ability_ChildProcessConfigs;

namespace base::internal {

// Bring-up only: require an OHOS isolated sandbox and a separate UID for
// renderers. This does not enable Chromium's Linux/seccomp sandbox. There is
// deliberately no fallback to a shared sandbox when this is requested.
inline constexpr char kOhosIsolateRenderers[] = "ohos-isolate-renderers";
// Child-only marker, set by the launcher after selecting the isolated mode.
inline constexpr char kOhosIsolatedRenderer[] = "ohos-isolated-renderer";
inline constexpr char kOhosIcuData[] = "ohos_icu";
inline constexpr char kOhosLocalePak[] = "ohos_locale";
inline constexpr char kOhosChrome100Pak[] = "ohos_pak100";
inline constexpr char kOhosChrome200Pak[] = "ohos_pak200";
inline constexpr char kOhosResourcesPak[] = "ohos_resources";
inline constexpr char kOhosV8Snapshot[] = "ohos_v8";
inline constexpr const char* kOhosRendererResourceNames[] = {
    kOhosIcuData,      kOhosLocalePak,    kOhosChrome100Pak,
    kOhosChrome200Pak, kOhosResourcesPak, kOhosV8Snapshot};
BASE_EXPORT bool IsOhosIsolatedRenderer();

// Runtime-resolved NDK entry points. Keeping the launch transaction separate
// lets tests exercise unavailable APIs and failures without starting a child.
struct BASE_EXPORT OhosIsolatedChildApi {
  using Create = Ability_ChildProcessConfigs* (*)();
  using Destroy =
      Ability_NativeChildProcess_ErrCode (*)(Ability_ChildProcessConfigs*);
  using SetMode =
      Ability_NativeChildProcess_ErrCode (*)(Ability_ChildProcessConfigs*,
                                             NativeChildProcess_IsolationMode);
  using SetUid =
      Ability_NativeChildProcess_ErrCode (*)(Ability_ChildProcessConfigs*,
                                             bool);
  using Start =
      Ability_NativeChildProcess_ErrCode (*)(const char*,
                                             NativeChildProcess_Args,
                                             Ability_ChildProcessConfigs*,
                                             int32_t*);

  Create create = nullptr;
  Destroy destroy = nullptr;
  SetMode set_mode = nullptr;
  SetUid set_uid = nullptr;
  Start start = nullptr;
};

BASE_EXPORT Ability_NativeChildProcess_ErrCode
StartOhosIsolatedRenderer(const OhosIsolatedChildApi& api,
                          NativeChildProcess_Args args,
                          int32_t* pid);

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
