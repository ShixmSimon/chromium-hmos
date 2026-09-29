// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/process/launch_ohos.h"

#include <dlfcn.h>
#include <sys/wait.h>

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "AbilityKit/native_child_process.h"
#include "base/base_paths.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/posix/eintr_wrapper.h"
#include "base/strings/string_number_conversions.h"
#include "base/synchronization/lock.h"
#include "base/threading/scoped_blocking_call.h"
#include "base/values.h"

namespace base::internal {
namespace {

constexpr char kNativeChildEntry[] =
    "libnweb_render.so:ChromiumNativeChildMain";

OhosGpuChildLauncher g_gpu_child_launcher = nullptr;

bool IsGpuProcessCommandLine(const std::vector<std::string>& argv) {
  for (const std::string& argument : argv) {
    if (argument == "--type=gpu-process") {
      return true;
    }
  }
  return false;
}

struct NativeChildExitRegistry {
  Lock lock;
  std::map<ProcessHandle, int> exit_signals GUARDED_BY(lock);
};

NativeChildExitRegistry& GetNativeChildExitRegistry() {
  static NoDestructor<NativeChildExitRegistry> registry;
  return *registry;
}

void OnNativeChildProcessExit(int32_t pid, int32_t signal) {
  NativeChildExitRegistry& registry = GetNativeChildExitRegistry();
  {
    AutoLock lock(registry.lock);
    registry.exit_signals[pid] = signal;
  }
  LOG(WARNING) << "OHOS native child exit pid=" << pid << " signal=" << signal;
}

bool EnsureNativeChildExitCallbackRegistered() {
  static const bool registered = [] {
    using NativeChildExitCallback = void (*)(int32_t, int32_t);
    using RegisterExitCallback =
        Ability_NativeChildProcess_ErrCode (*)(NativeChildExitCallback);
    void* child_process_library =
        dlopen("libchild_process.so", RTLD_NOW | RTLD_LOCAL);
    if (!child_process_library) {
      LOG(ERROR) << "Failed to load OHOS native child process library: "
                 << dlerror();
      return false;
    }
    auto* register_callback = reinterpret_cast<RegisterExitCallback>(
        dlsym(child_process_library,
              "OH_Ability_RegisterNativeChildProcessExitCallback"));
    if (!register_callback) {
      LOG(ERROR) << "OHOS native child exit callback API is unavailable";
      return false;
    }
    const Ability_NativeChildProcess_ErrCode result =
        register_callback(&OnNativeChildProcessExit);
    if (result != NCP_NO_ERROR) {
      LOG(ERROR) << "Failed to register OHOS native child exit callback: "
                 << result;
      return false;
    }
    LOG(WARNING) << "Registered OHOS native child exit callback";
    return true;
  }();
  return registered;
}

bool EncodeOhosNativeChildParams(const std::vector<std::string>& argv,
                                 const LaunchOptions& options,
                                 std::string* encoded) {
  ListValue encoded_argv;
  for (const std::string& argument : argv) {
    encoded_argv.Append(argument);
  }

  DictValue encoded_environment;
  for (const auto& [key, value] : options.environment) {
    encoded_environment.Set(key, value);
  }

  DictValue root;
  root.Set("argv", std::move(encoded_argv));
  root.Set("environment", std::move(encoded_environment));
  root.Set("clearEnvironment", options.clear_environment);
  root.Set("currentDirectory", options.current_directory.value());

  FilePath resources_directory;
  if (!PathService::Get(DIR_ASSETS, &resources_directory) ||
      !resources_directory.IsAbsolute()) {
    LOG(ERROR) << "OHOS native child process has no resources directory";
    return false;
  }
  root.Set("resourcesDirectory", resources_directory.value());
  return JSONWriter::Write(root, encoded);
}

}  // namespace

Process LaunchProcessOhos(const std::vector<std::string>& argv,
                          const LaunchOptions& options) {
  if (argv.empty()) {
    LOG(ERROR) << "OHOS native child process has an empty command line";
    return Process();
  }
  if (options.pre_exec_delegate) {
    LOG(WARNING) << "OHOS native child process ignores pre_exec_delegate";
  }

  EnsureNativeChildExitCallbackRegistered();

  std::string encoded_params;
  if (!EncodeOhosNativeChildParams(argv, options, &encoded_params)) {
    LOG(ERROR) << "Failed to serialize OHOS native child process arguments";
    return Process();
  }

  if (IsGpuProcessCommandLine(argv)) {
    if (!g_gpu_child_launcher) {
      LOG(ERROR) << "OHOS GPU process requested with no launcher for it";
      return Process();
    }
    std::vector<std::pair<int, int>> gpu_fds(options.fds_to_remap.begin(),
                                             options.fds_to_remap.end());
    const ProcessId gpu_pid = g_gpu_child_launcher(encoded_params, gpu_fds);
    return gpu_pid == kNullProcessId ? Process() : Process(gpu_pid);
  }

  std::vector<std::string> fd_names;
  fd_names.reserve(options.fds_to_remap.size());
  for (const auto& [source_fd, destination_fd] : options.fds_to_remap) {
    (void)source_fd;
    fd_names.push_back(NumberToString(destination_fd));
  }

  std::vector<NativeChildProcess_Fd> fd_nodes(options.fds_to_remap.size());
  for (size_t index = 0; index < options.fds_to_remap.size(); ++index) {
    fd_nodes[index].fdName = fd_names[index].data();
    fd_nodes[index].fd = options.fds_to_remap[index].first;
    fd_nodes[index].next =
        index + 1 < fd_nodes.size() ? &fd_nodes[index + 1] : nullptr;
  }

  NativeChildProcess_Args child_args = {
      .entryParams = encoded_params.data(),
      .fdList = {.head = fd_nodes.empty() ? nullptr : &fd_nodes.front()},
  };
  NativeChildProcess_Options child_options = {
      .isolationMode = NCP_ISOLATION_MODE_NORMAL,
      .reserved = 0,
  };
  int32_t pid = -1;
  const Ability_NativeChildProcess_ErrCode result =
      OH_Ability_StartNativeChildProcess(kNativeChildEntry, child_args,
                                         child_options, &pid);
  if (result != NCP_NO_ERROR || pid <= 0) {
    LOG(ERROR) << "OHOS native child process failed result=" << result;
    return Process();
  }

  if (options.wait) {
    ScopedBlockingCall scoped_blocking_call(FROM_HERE, BlockingType::MAY_BLOCK);
    const pid_t waited_pid = HANDLE_EINTR(waitpid(pid, nullptr, 0));
    DPCHECK(waited_pid == pid);
  }
  return Process(pid);
}

void SetOhosGpuChildLauncher(OhosGpuChildLauncher launcher) {
  g_gpu_child_launcher = launcher;
}

std::optional<int> GetOhosNativeChildExitSignal(ProcessHandle handle) {
  NativeChildExitRegistry& registry = GetNativeChildExitRegistry();
  AutoLock lock(registry.lock);
  auto found = registry.exit_signals.find(handle);
  if (found == registry.exit_signals.end()) {
    return std::nullopt;
  }
  return found->second;
}

bool DecodeOhosNativeChildParams(std::string_view encoded,
                                 OhosNativeChildParams* params) {
  if (!params) {
    return false;
  }

  std::optional<DictValue> root = JSONReader::ReadDict(encoded, JSON_PARSE_RFC);
  if (!root) {
    return false;
  }

  const ListValue* encoded_argv = root->FindList("argv");
  if (!encoded_argv || encoded_argv->empty()) {
    return false;
  }

  OhosNativeChildParams decoded;
  decoded.argv.reserve(encoded_argv->size());
  for (const Value& argument : *encoded_argv) {
    const std::string* value = argument.GetIfString();
    if (!value) {
      return false;
    }
    decoded.argv.push_back(*value);
  }

  if (const DictValue* environment = root->FindDict("environment")) {
    for (const auto [key, value] : *environment) {
      const std::string* environment_value = value.GetIfString();
      if (!environment_value) {
        return false;
      }
      decoded.environment.emplace(key, *environment_value);
    }
  }

  decoded.clear_environment =
      root->FindBool("clearEnvironment").value_or(false);
  if (const std::string* current_directory =
          root->FindString("currentDirectory")) {
    decoded.current_directory = FilePath(*current_directory);
  }
  if (const std::string* resources_directory =
          root->FindString("resourcesDirectory")) {
    decoded.resources_directory = FilePath(*resources_directory);
  }

  *params = std::move(decoded);
  return true;
}

}  // namespace base::internal
