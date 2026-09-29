// Copyright (c) 2026
// Licensed under the Apache License, Version 2.0.

#include <dlfcn.h>
#include <fcntl.h>
#include <hilog/log.h>
#include <stdlib.h>
#include <unistd.h>

#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "AbilityKit/native_child_process.h"
#include "base/base_paths.h"
#include "base/files/file_path.h"
#include "base/files/scoped_file.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "base/posix/global_descriptors.h"
#include "base/process/launch_ohos.h"
#include "base/strings/string_number_conversions.h"
#include "ohos_nweb/src/nweb_hilog.h"
#include "ui/ozone/platform/ohos/ohos_gpu_child_channel.h"

extern "C" int ChromeMain(int argc, const char** argv);

namespace {

bool ForwardChromiumChildLogToHilog(int severity,
                                    const char*,
                                    int,
                                    size_t,
                                    const std::string& message) {
  LogLevel level = LOG_INFO;
  if (severity >= logging::LOGGING_FATAL) {
    level = LOG_FATAL;
  } else if (severity >= logging::LOGGING_ERROR) {
    level = LOG_ERROR;
  } else if (severity >= logging::LOGGING_WARNING) {
    level = LOG_WARN;
  } else if (severity < logging::LOGGING_INFO) {
    level = LOG_DEBUG;
  }
  OH_LOG_Print(LOG_APP, level, 0xc233, "ChromiumChild", "%{public}s",
               message.c_str());
  return false;
}

int RunChromeMain(std::vector<std::string> argv_strings) {
  if (argv_strings.empty()) {
    argv_strings.push_back("web_render");
  }

  std::vector<const char*> argv;
  argv.reserve(argv_strings.size());
  for (const std::string& item : argv_strings) {
    argv.push_back(item.c_str());
  }
  return ChromeMain(static_cast<int>(argv.size()), argv.data());
}

// `fds` pairs each descriptor's number in Chromium's scheme with the
// descriptor this process received.
bool RestoreFileDescriptors(const std::vector<std::pair<int, int>>& fds) {
  constexpr int kMinChromiumDescriptor = 1000;
  using DescriptorMapping =
      std::pair<base::GlobalDescriptors::Key, base::ScopedFD>;
  std::vector<DescriptorMapping> mappings;
  for (const auto& [destination_fd, received_fd] : fds) {
    if (destination_fd < base::GlobalDescriptors::kBaseDescriptor ||
        received_fd < 0) {
      return false;
    }
    const int source_flags = fcntl(received_fd, F_GETFL);
    const int duplicated_fd =
        fcntl(received_fd, F_DUPFD_CLOEXEC, kMinChromiumDescriptor);
    if (source_flags < 0 || duplicated_fd < 0) {
      return false;
    }
    const auto descriptor_key = static_cast<base::GlobalDescriptors::Key>(
        destination_fd - base::GlobalDescriptors::kBaseDescriptor);
    mappings.emplace_back(descriptor_key, base::ScopedFD(duplicated_fd));
  }

  base::GlobalDescriptors* global_descriptors =
      base::GlobalDescriptors::GetInstance();
  for (auto& [key, descriptor] : mappings) {
    global_descriptors->Set(key, descriptor.release());
  }
  return true;
}

bool ReadFdList(NativeChildProcess_Fd* fd,
                std::vector<std::pair<int, int>>* fds) {
  for (NativeChildProcess_Fd* current = fd; current; current = current->next) {
    int destination_fd = -1;
    if (!current->fdName ||
        !base::StringToInt(current->fdName, &destination_fd)) {
      return false;
    }
    fds->emplace_back(destination_fd, current->fd);
  }
  return true;
}

bool ApplyLaunchParams(
    const base::internal::OhosNativeChildParams& launch_params) {
  if (launch_params.clear_environment && clearenv() != 0) {
    return false;
  }
  for (const auto& [key, value] : launch_params.environment) {
    if (setenv(key.c_str(), value.c_str(), 1) != 0) {
      return false;
    }
  }
  return launch_params.current_directory.empty() ||
         chdir(launch_params.current_directory.value().c_str()) == 0;
}

bool ConfigureRuntimePaths(
    const base::internal::OhosNativeChildParams& launch_params) {
  if (launch_params.resources_directory.empty() ||
      !launch_params.resources_directory.IsAbsolute() ||
      !base::PathService::Override(base::DIR_ASSETS,
                                   launch_params.resources_directory)) {
    return false;
  }

  Dl_info module_info = {};
  if (!dladdr(reinterpret_cast<const void*>(&ConfigureRuntimePaths),
              &module_info) ||
      !module_info.dli_fname) {
    return false;
  }

  const base::FilePath module_directory =
      base::FilePath(module_info.dli_fname).DirName();
  return module_directory.IsAbsolute() &&
         base::PathService::Override(base::DIR_MODULE, module_directory);
}

}  // namespace

extern "C" __attribute__((visibility("default"))) void ChromiumNWebRenderMain(
    const char* args) {
  WVLOG_I("AuraShell NWebRenderMain start");

  std::string args_string = args ? args : "";
  std::stringstream args_stream(args_string);
  std::vector<std::string> argv_strings;
  std::string arg;
  while (std::getline(args_stream, arg, '#')) {
    if (!arg.empty()) {
      argv_strings.push_back(arg);
    }
  }

  const int exit_code = RunChromeMain(std::move(argv_strings));
  WVLOG_I("AuraShell NWebRenderMain end code=%{public}d", exit_code);
  (void)exit_code;
}

namespace {

// Both kinds of child end here: a renderer with what
// OH_Ability_StartNativeChildProcess handed it, the GPU process with what its
// parent sent over the IPC channel.
void RunNativeChild(const char* encoded_params,
                    const std::vector<std::pair<int, int>>& fds,
                    bool gpu_child) {
  base::internal::OhosNativeChildParams launch_params;
  if (!encoded_params || !base::internal::DecodeOhosNativeChildParams(
                             encoded_params, &launch_params)) {
    WVLOG_E("AuraShell native child rejected encoded startup parameters");
    return;
  }
  if (!RestoreFileDescriptors(fds) || !ApplyLaunchParams(launch_params) ||
      !ConfigureRuntimePaths(launch_params)) {
    WVLOG_E("AuraShell native child rejected startup parameters");
    return;
  }
  // After the launch environment, which may have cleared everything: ANGLE
  // reaches the system EGL and GLES through the HarmonyOS wrapper's exports
  // in this process only. See ohos-angle-null-gl-strings.patch.
  if (gpu_child) {
    setenv("OHOS_ANGLE_WRAPPER_EXPORTS", "1", 1);
  }

  std::ostringstream command_line;
  for (const std::string& argument : launch_params.argv) {
    command_line << argument << ' ';
  }
  LOG(WARNING) << "AuraShell native child argv=" << command_line.str();
  const int exit_code = RunChromeMain(std::move(launch_params.argv));
  LOG(WARNING) << "AuraShell native child end pid=" << getpid()
               << " code=" << exit_code;
  (void)exit_code;
}

}  // namespace

extern "C" __attribute__((visibility("default"))) void
ChromiumHarmonyOSNativeChildMain(NativeChildProcess_Args args) {
  logging::SetLogMessageHandler(&ForwardChromiumChildLogToHilog);
  LOG(WARNING) << "AuraShell native child start pid=" << getpid();
  std::vector<std::pair<int, int>> fds;
  if (!ReadFdList(args.fdList.head, &fds)) {
    WVLOG_E("AuraShell native child rejected its descriptors");
    return;
  }
  RunNativeChild(args.entryParams, fds, /*gpu_child=*/false);
}

// The GPU process, started with OH_Ability_CreateNativeChildProcess so that
// it has an IPC channel to the browser: see ohos_gpu_child_channel.h. The
// system calls OnConnect first, then MainProc, and the process ends when
// MainProc returns.
extern "C" __attribute__((visibility("default"))) void*
ChromiumHarmonyOSGpuChildOnConnect() {
  logging::SetLogMessageHandler(&ForwardChromiumChildLogToHilog);
  return ui::CreateOhosGpuChildStub();
}

extern "C" __attribute__((visibility("default"))) void
ChromiumHarmonyOSGpuChildMainProc() {
  LOG(WARNING) << "AuraShell GPU child start pid=" << getpid();
  std::string encoded_params;
  std::vector<std::pair<int, int>> fds;
  if (!ui::WaitForOhosGpuChildBootstrap(&encoded_params, &fds)) {
    WVLOG_E("AuraShell GPU child never received its startup parameters");
    return;
  }
  ui::ProbeOhosGpuChildEgl();
  if (encoded_params.find("--use-vulkan") != std::string::npos) {
    ui::ProbeOhosGpuChildVulkan();
  }
  RunNativeChild(encoded_params.c_str(), fds, /*gpu_child=*/true);
}
