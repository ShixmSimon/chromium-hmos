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
#include "base/command_line.h"
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

// These APIs appeared in API 20, with UID isolation added in API 21. Resolve
// the entire set before creating anything: older devices must reject the
// experiment, not silently run a renderer with the application's authority.
const OhosIsolatedChildApi& GetIsolatedChildApi() {
  static const OhosIsolatedChildApi api = [] {
    OhosIsolatedChildApi result;
    // Retained for the lifetime of the function pointers.
    void* library = dlopen("libchild_process.so", RTLD_NOW | RTLD_LOCAL);
    if (!library) {
      return result;
    }
    result.create = reinterpret_cast<OhosIsolatedChildApi::Create>(
        dlsym(library, "OH_Ability_CreateChildProcessConfigs"));
    result.destroy = reinterpret_cast<OhosIsolatedChildApi::Destroy>(
        dlsym(library, "OH_Ability_DestroyChildProcessConfigs"));
    result.set_mode = reinterpret_cast<OhosIsolatedChildApi::SetMode>(
        dlsym(library, "OH_Ability_ChildProcessConfigs_SetIsolationMode"));
    result.set_uid = reinterpret_cast<OhosIsolatedChildApi::SetUid>(
        dlsym(library, "OH_Ability_ChildProcessConfigs_SetIsolationUid"));
    result.start = reinterpret_cast<OhosIsolatedChildApi::Start>(
        dlsym(library, "OH_Ability_StartNativeChildProcessWithConfigs"));
    return result;
  }();
  return api;
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

Ability_NativeChildProcess_ErrCode StartOhosIsolatedRenderer(
    const OhosIsolatedChildApi& api, NativeChildProcess_Args args, int32_t* pid) {
  if (!pid) {
    return NCP_ERR_INVALID_PARAM;
  }
  *pid = -1;
  if (!api.create || !api.destroy || !api.set_mode || !api.set_uid ||
      !api.start) {
    LOG(ERROR) << "OHOS isolated renderer APIs unavailable; launch refused";
    return NCP_ERR_NOT_SUPPORTED;
  }
  Ability_ChildProcessConfigs* configs = api.create();
  if (!configs) {
    return NCP_ERR_INTERNAL;
  }
  auto result = api.set_mode(configs, NCP_ISOLATION_MODE_ISOLATED);
  if (result == NCP_NO_ERROR) {
    result = api.set_uid(configs, true);
  }
  if (result == NCP_NO_ERROR) {
    result = api.start(kNativeChildEntry, args, configs, pid);
  }
  api.destroy(configs);
  return result;
}

Process LaunchProcessOhos(const std::vector<std::string>& argv,
                          const LaunchOptions& options) {
  if (argv.empty()) {
    LOG(ERROR) << "OHOS native child process has an empty command line";
    return Process();
  }
  if (options.pre_exec_delegate) {
    // A delegate may establish a security boundary. Running the child without
    // it would silently remove that boundary; the appspawn API cannot run it.
    LOG(ERROR) << "OHOS native child cannot run pre_exec_delegate; launch refused";
    return Process();
  }

  EnsureNativeChildExitCallbackRegistered();

  std::string encoded_params;
  if (!EncodeOhosNativeChildParams(argv, options, &encoded_params)) {
    LOG(ERROR) << "Failed to serialize OHOS native child process arguments";
    return Process();
  }

  const std::string process_type = CommandLine(argv).GetSwitchValueASCII("type");
  if (process_type == "gpu-process") {
    if (!g_gpu_child_launcher) {
      LOG(ERROR) << "OHOS GPU process requested with no launcher for it";
      return Process();
    }
    std::vector<std::pair<int, int>> gpu_fds(options.fds_to_remap.begin(),
                                             options.fds_to_remap.end());
    const ProcessId gpu_pid = g_gpu_child_launcher(encoded_params, gpu_fds);
    return gpu_pid == kNullProcessId ? Process() : Process(gpu_pid);
  }

  // The NDK accepts at most 16 descriptors. Reject before constructing the
  // list rather than relying on partial or platform-dependent processing.
  if (options.fds_to_remap.size() > 16) {
    LOG(ERROR) << "OHOS native child descriptor limit exceeded";
    return Process();
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
  const bool isolate_renderer =
      CommandLine::ForCurrentProcess()->HasSwitch(kOhosIsolateRenderers) &&
      process_type == "renderer";
  const Ability_NativeChildProcess_ErrCode result =
      isolate_renderer
          ? StartOhosIsolatedRenderer(GetIsolatedChildApi(), child_args, &pid)
          : OH_Ability_StartNativeChildProcess(kNativeChildEntry, child_args,
                                              child_options, &pid);
  if (result != NCP_NO_ERROR || pid <= 0) {
    LOG(ERROR) << "OHOS native child process failed result=" << result;
    return Process();
  }
  if (isolate_renderer) {
    // This records the requested launch policy, not a verified security test.
    LOG(WARNING) << "OHOS renderer started with isolated sandbox and UID requested"
                 << " pid=" << pid;
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
