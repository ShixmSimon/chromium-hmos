// Copyright (c) 2026
// Licensed under the Apache License, Version 2.0.

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <hilog/log.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <ucontext.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "AbilityKit/native_child_process.h"
#include "base/base_paths.h"
#include "base/command_line.h"
#include "base/file_descriptor_store.h"
#include "base/files/file_path.h"
#include "base/files/memory_mapped_file.h"
#include "base/files/scoped_file.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "base/posix/global_descriptors.h"
#include "base/process/fork_server_ohos.h"
#include "base/process/launch_ohos.h"
#include "base/process/ohos_jit_memory.h"
#include "base/strings/string_number_conversions.h"
#include "ohos_nweb/src/nweb_hilog.h"
#include "ui/ozone/platform/ohos/ohos_gpu_child_channel.h"
#include "ui/ozone/platform/ohos/ohos_phone_gpu_channel.h"

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

// Opt-in diagnostics for isolated renderers whose system crash report cannot
// be attributed to the parent bundle. The signal handler only copies registers
// and uses async-signal-safe syscalls; symbol lookup and hilog run on a reader.
// Chain to the original handler with its signal context and crash policy intact.
#if defined(__aarch64__)
struct TrapRegisters {
  uintptr_t pc;
  uintptr_t lr;
  uintptr_t fp;
};
int trap_pipe[2] = {-1, -1};
int trap_ack[2] = {-1, -1};
struct sigaction previous_trap_action = {};

void RecordNativeChildTrap(int signal, siginfo_t* info, void* context) {
  const auto* registers = static_cast<ucontext_t*>(context);
  TrapRegisters record{registers->uc_mcontext.pc,
                       registers->uc_mcontext.regs[30],
                       registers->uc_mcontext.regs[29]};
  if (write(trap_pipe[1], &record, sizeof(record)) ==
      static_cast<ssize_t>(sizeof(record))) {
    // Bound the diagnostic delay even if the logger or dynamic loader is stuck.
    struct pollfd ack = {trap_ack[0], POLLIN, 0};
    poll(&ack, 1, 200);
  }
  struct sigaction restore = previous_trap_action;
  if (restore.sa_flags & SA_RESETHAND) {
    restore.sa_handler = SIG_DFL;
    restore.sa_flags = 0;
  }
  sigaction(signal, &restore, nullptr);
  if (previous_trap_action.sa_handler == SIG_DFL) {
    raise(signal);
  } else if (previous_trap_action.sa_flags & SA_SIGINFO) {
    previous_trap_action.sa_sigaction(signal, info, context);
  } else {
    previous_trap_action.sa_handler(signal);
  }
}

void* LogNativeChildTrap(void*) {
  TrapRegisters record;
  ssize_t count;
  do {
    count = read(trap_pipe[0], &record, sizeof(record));
  } while (count < 0 && errno == EINTR);
  if (count == static_cast<ssize_t>(sizeof(record))) {
    for (auto address : {record.pc, record.lr}) {
      Dl_info info = {};
      dladdr(reinterpret_cast<void*>(address), &info);
      const uintptr_t offset = info.dli_fbase
                                   ? address - reinterpret_cast<uintptr_t>(
                                                   info.dli_fbase)
                                   : 0;
      OH_LOG_Print(LOG_APP, LOG_ERROR, 0xc233, "ChromiumChildTrap",
                   "address=%{public}zx offset=%{public}zx module=%{public}s "
                   "symbol=%{public}s fp=%{public}zx",
                   address, offset, info.dli_fname ? info.dli_fname : "unknown",
                   info.dli_sname ? info.dli_sname : "unknown", record.fp);
    }
    const char ack = 1;
    const ssize_t written = write(trap_ack[1], &ack, sizeof(ack));
    (void)written;
  }
  return nullptr;
}

void EnableNativeChildTrapDiagnostics() {
  if (sigaction(SIGTRAP, nullptr, &previous_trap_action) != 0 ||
      previous_trap_action.sa_handler == SIG_IGN) {
    LOG(WARNING) << "OHOS trap diagnostics unavailable or signal ignored";
    return;
  }
  if (pipe2(trap_pipe, O_CLOEXEC) != 0)
    return;
  if (pipe2(trap_ack, O_CLOEXEC) != 0) {
    close(trap_pipe[0]);
    close(trap_pipe[1]);
    return;
  }
  pthread_t reader;
  if (pthread_create(&reader, nullptr, &LogNativeChildTrap, nullptr) != 0) {
    for (int fd : {trap_pipe[0], trap_pipe[1], trap_ack[0], trap_ack[1]})
      close(fd);
    return;
  }
  pthread_detach(reader);
  struct sigaction action = previous_trap_action;
  action.sa_sigaction = &RecordNativeChildTrap;
  action.sa_flags = (action.sa_flags | SA_SIGINFO) & ~SA_RESETHAND;
  if (sigaction(SIGTRAP, &action, nullptr) == 0)
    LOG(WARNING) << "OHOS isolated renderer trap diagnostics enabled";
}
#endif

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
  // GlobalDescriptors maps logical keys to actual descriptors; there is no
  // dup2 to fixed descriptor numbers here. Fork children must keep these
  // copies above inherited runtime FD numbers, just like SCM_RIGHTS inputs.
  using DescriptorMapping =
      std::pair<base::GlobalDescriptors::Key, base::ScopedFD>;
  std::vector<DescriptorMapping> mappings;
  for (const auto& [destination_fd, received_fd] : fds) {
    if (destination_fd < base::GlobalDescriptors::kBaseDescriptor ||
        received_fd < 0) {
      LOG(ERROR) << "OHOS child bootstrap invalid descriptor mapping";
      return false;
    }
    base::ScopedFD duplicated_fd(
        base::internal::DuplicateOhosChildDescriptor(received_fd));
    if (!duplicated_fd.is_valid()) {
      LOG(ERROR) << "OHOS child bootstrap duplicate descriptor failed errno="
                 << errno << " key="
                 << destination_fd - base::GlobalDescriptors::kBaseDescriptor;
      return false;
    }
    const auto descriptor_key = static_cast<base::GlobalDescriptors::Key>(
        destination_fd - base::GlobalDescriptors::kBaseDescriptor);
    mappings.emplace_back(descriptor_key, std::move(duplicated_fd));
  }

  base::GlobalDescriptors* global_descriptors =
      base::GlobalDescriptors::GetInstance();
  for (auto& [key, descriptor] : mappings) {
    global_descriptors->Set(key, descriptor.release());
  }
  return true;
}

bool ReadFdList(NativeChildProcess_Fd* fd,
                std::vector<std::pair<int, int>>* fds,
                std::map<std::string, int>* resources) {
  std::set<int> destinations;
  size_t count = 0;
  for (NativeChildProcess_Fd* current = fd; current; current = current->next) {
    if (++count > 16 || !current->fdName || current->fd < 0) {
      return false;
    }
    int destination_fd = -1;
    if (base::StringToInt(current->fdName, &destination_fd)) {
      if (!destinations.insert(destination_fd).second) {
        return false;
      }
      fds->emplace_back(destination_fd, current->fd);
      continue;
    }
    bool known_resource = false;
    for (const char* name : base::internal::kOhosRendererResourceNames) {
      known_resource |= std::string_view(current->fdName) == name;
    }
    if (!known_resource ||
        !resources->emplace(current->fdName, current->fd).second) {
      return false;
    }
  }
  return true;
}

bool RestoreResourceDescriptors(const std::map<std::string, int>& resources,
                                bool isolated_renderer) {
  const size_t expected =
      isolated_renderer ? std::size(base::internal::kOhosRendererResourceNames)
                        : 0;
  if (resources.size() != expected) {
    LOG(ERROR) << "OHOS child bootstrap resource count mismatch count="
               << resources.size() << " expected=" << expected;
    return false;
  }
  for (const auto& [name, received_fd] : resources) {
    struct stat info = {};
    const int flags = fcntl(received_fd, F_GETFL);
    if (flags < 0 || (flags & O_ACCMODE) != O_RDONLY ||
        fstat(received_fd, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_size <= 0) {
      LOG(ERROR) << "OHOS child bootstrap invalid readonly resource";
      return false;
    }
    base::ScopedFD fd(base::internal::DuplicateOhosChildDescriptor(received_fd));
    if (!fd.is_valid()) {
      LOG(ERROR) << "OHOS child bootstrap resource duplicate failed errno="
                 << errno;
      return false;
    }
    base::FileDescriptorStore::GetInstance().Set(
        name, std::move(fd), base::MemoryMappedFile::Region::kWholeFile);
  }
  return true;
}

bool ApplyLaunchParams(
    const base::internal::OhosNativeChildParams& launch_params) {
  if (launch_params.clear_environment && clearenv() != 0) {
    LOG(ERROR) << "OHOS child bootstrap clear environment failed errno="
               << errno;
    return false;
  }
  for (const auto& [key, value] : launch_params.environment) {
    if (setenv(key.c_str(), value.c_str(), 1) != 0) {
      LOG(ERROR) << "OHOS child bootstrap set environment failed errno="
                 << errno;
      return false;
    }
  }
  if (!launch_params.current_directory.empty() &&
      chdir(launch_params.current_directory.value().c_str()) != 0) {
    LOG(ERROR) << "OHOS child bootstrap current directory failed errno="
               << errno;
    return false;
  }
  return true;
}

bool ConfigureRuntimePaths(
    const base::internal::OhosNativeChildParams& launch_params,
    bool isolated_renderer) {
  // Isolated renderers consume all startup assets via readonly descriptors.
  // In particular, Override() would mkdir/realpath the parent's private path.
  if (!isolated_renderer &&
      (launch_params.resources_directory.empty() ||
       !launch_params.resources_directory.IsAbsolute() ||
       !base::PathService::Override(base::DIR_ASSETS,
                                    launch_params.resources_directory))) {
    LOG(ERROR) << "OHOS child bootstrap resources path failed errno=" << errno;
    return false;
  }

  Dl_info module_info = {};
  if (!dladdr(reinterpret_cast<const void*>(&ConfigureRuntimePaths),
              &module_info) ||
      !module_info.dli_fname) {
    LOG(ERROR) << "OHOS child bootstrap module lookup failed";
    return false;
  }

  const base::FilePath module_directory =
      base::FilePath(module_info.dli_fname).DirName();
  if (!module_directory.IsAbsolute() ||
      !base::PathService::OverrideAndCreateIfNeeded(
          base::DIR_MODULE, module_directory, true, false)) {
    LOG(ERROR) << "OHOS child bootstrap module path failed errno=" << errno;
    return false;
  }
  return true;
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
int RunNativeChild(const char* encoded_params,
                   const std::vector<std::pair<int, int>>& fds,
                   bool gpu_child,
                   const std::map<std::string, int>& resources = {}) {
  base::internal::OhosNativeChildParams launch_params;
  if (!encoded_params || !base::internal::DecodeOhosNativeChildParams(
                             encoded_params, &launch_params)) {
    LOG(ERROR) << "OHOS child bootstrap decode parameters failed";
    return 70;
  }
  const base::CommandLine command_line(launch_params.argv);
  const bool isolated_renderer =
      command_line.HasSwitch(base::internal::kOhosIsolatedRenderer) &&
      command_line.GetSwitchValueASCII("type") == "renderer";
  if (!RestoreResourceDescriptors(resources, isolated_renderer)) {
    return 76;
  }
  std::vector<std::pair<int, int>> chromium_fds;
  bool phone_gpu = false;
  for (const auto& mapping : fds) {
    if (!gpu_child &&
        command_line.GetSwitchValueASCII("type") == "gpu-process" &&
        mapping.first == ui::phone_gpu::kChannelDescriptor) {
      if (gpu_child || phone_gpu ||
          command_line.GetSwitchValueASCII("type") != "gpu-process" ||
          !ui::InitializeOhosPhoneGpuChild(mapping.second)) {
        return 71;
      }
      phone_gpu = true;
    } else {
      chromium_fds.push_back(mapping);
    }
  }
  if (!gpu_child && command_line.GetSwitchValueASCII("type") == "gpu-process" &&
      !phone_gpu) {
    return 71;
  }
  gpu_child |= phone_gpu;
  if (!RestoreFileDescriptors(chromium_fds)) {
    return 71;
  }
  base::internal::ReportOhosForkStage(
      base::internal::OhosForkStage::kDescriptors);
  if (!ApplyLaunchParams(launch_params)) {
    return 72;
  }
  if (!ConfigureRuntimePaths(launch_params, isolated_renderer)) {
    return 73;
  }
  // After the launch environment, which may have cleared everything: ANGLE
  // reaches the system EGL and GLES through the HarmonyOS wrapper's exports
  // in this process only. See ohos-angle.patch.
  if (gpu_child) {
    setenv("OHOS_ANGLE_WRAPPER_EXPORTS", "1", 1);
  }

#if defined(__aarch64__)
  if (isolated_renderer) {
    for (const auto& argument : launch_params.argv) {
      if (argument == "--ohos-trap-diagnostics") {
        EnableNativeChildTrapDiagnostics();
        break;
      }
    }
  }
#endif

  // A renderer can be created long after the main-process startup probe,
  // including after background cleanup. Revalidate in this process before V8
  // initializes; do not reuse a stale parent result or enable compatibility in
  // an isolated child. GPU and utility children keep their existing policy.
  if (command_line.GetSwitchValueASCII("type") == "renderer") {
    const std::string js_flags = command_line.GetSwitchValueASCII("js-flags");
    if (!base::internal::JitlessRequested(js_flags)) {
      const auto probe = base::internal::ProbeOhosJitMemory();
      if (!probe.available) {
        base::CommandLine renderer_command_line(launch_params.argv);
        renderer_command_line.RemoveSwitch("js-flags");
        renderer_command_line.AppendSwitchASCII(
            "js-flags", js_flags + (js_flags.empty() ? "" : " ") +
                            "--jitless --wasm-jitless");
        launch_params.argv = renderer_command_line.argv();
        LOG(ERROR) << "OHOS renderer JIT revalidation failed stage="
                   << (probe.failure_stage ? probe.failure_stage : "unknown")
                   << " map=" << probe.map_error
                   << " protect=" << probe.protect_error
                   << " discard=" << probe.discard_error
                   << " unmap=" << probe.unmap_error
                   << "; using interpreter before ChromeMain";
      }
    }
  }

  // Arguments can contain user paths and URLs. A stage marker is sufficient
  // to distinguish bootstrap failure from a child stuck inside Chromium.
  LOG(WARNING) << "OHOS child bootstrap entering ChromeMain argc="
               << launch_params.argv.size() << " fds=" << fds.size();
  base::internal::ReportOhosForkStage(
      base::internal::OhosForkStage::kChromeMain);
  const int exit_code = RunChromeMain(std::move(launch_params.argv));
  LOG(WARNING) << "AuraShell native child end pid=" << getpid()
               << " code=" << exit_code;
  return exit_code;
}

}  // namespace

extern "C" __attribute__((visibility("default"))) void
ChromiumHarmonyOSNativeChildMain(NativeChildProcess_Args args) {
  base::internal::ReportOhosForkStage(
      base::internal::OhosForkStage::kNativeMain);
  logging::SetLogMessageHandler(&ForwardChromiumChildLogToHilog);
  LOG(WARNING) << "AuraShell native child start pid=" << getpid();
  struct rlimit fd_limit = {};
  if (getrlimit(RLIMIT_NOFILE, &fd_limit) == 0) {
    LOG(WARNING) << "OHOS child bootstrap uid=" << getuid()
                 << " fd_limit=" << fd_limit.rlim_cur;
  }
  std::vector<std::pair<int, int>> fds;
  std::map<std::string, int> resources;
  if (!ReadFdList(args.fdList.head, &fds, &resources)) {
    LOG(ERROR) << "OHOS child bootstrap descriptor list invalid";
    _exit(74);
  }
  const int result =
      RunNativeChild(args.entryParams, fds, /*gpu_child=*/false, resources);
  if (result != 0) {
    LOG(ERROR) << "OHOS child bootstrap exit code=" << result;
    _exit(result);
  }
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
    LOG(ERROR) << "OHOS GPU child bootstrap parameters unavailable";
    _exit(75);
  }
  ui::ProbeOhosGpuChildEgl();
  if (ui::OhosGpuChildWantsVulkan()) {
    ui::ProbeOhosGpuChildVulkan();
  }
  const int result =
      RunNativeChild(encoded_params.c_str(), fds, /*gpu_child=*/true);
  if (result != 0) {
    LOG(ERROR) << "OHOS GPU child bootstrap exit code=" << result;
    _exit(result);
  }
}
