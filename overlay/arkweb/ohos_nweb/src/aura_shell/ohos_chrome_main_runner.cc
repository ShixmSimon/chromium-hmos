// Copyright (c) 2026
// Licensed under the Apache License, Version 2.0.

#include "ohos_nweb/src/aura_shell/ohos_chrome_main_runner.h"

#include <dlfcn.h>
#include <errno.h>
#include <hilog/log.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cmath>
#include <limits>
#include <utility>

#include "base/base_paths.h"
#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/memory_mapped_file.h"
#include "base/functional/callback.h"
#include "base/i18n/icu_util.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/process/fork_server_ohos.h"
#include "base/process/launch_ohos.h"
#include "chrome/browser/ui/ohos/aura_shell_runtime_bridge.h"
#include "ohos_nweb/src/nweb_hilog.h"
#include "ui/ozone/platform/ohos/ohos_gpu_child_channel.h"
#include "ui/ozone/platform/ohos/ohos_native_window_registry.h"
#include "ui/ozone/platform/ohos/ohos_output_color_config.h"
#include "ui/ozone/platform/ohos/ohos_phone_gpu_shared_buffer.h"

extern "C" int ChromeMain(int argc, const char** argv);

namespace ohos_nweb {
namespace {

// The ArkWeb DFX logging macros compile out when enable_arkweb=false, which
// would hide startup failures of a runtime that has no window to show them.
#define AURA_LOG_I(...) \
  OH_LOG_Print(LOG_APP, LOG_INFO, 0xc233, "AuraShell", __VA_ARGS__)
#define AURA_LOG_E(...) \
  OH_LOG_Print(LOG_APP, LOG_ERROR, 0xc233, "AuraShell", __VA_ARGS__)


// Whether this process may make anonymous memory executable, which V8's JIT
// needs. HarmonyOS withdraws that from apps in some security modes, and V8
// then dies while its first isolate reserves the JIT code range ("V8 process
// OOM (Failed to reserve virtual memory for CodeRange)") -- a crash on every
// launch. Asked once per process, before Chromium starts.
bool ProcessCanJit() {
  const size_t size = static_cast<size_t>(getpagesize());
  void* page = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (page == MAP_FAILED) {
    return false;
  }
  const bool executable = mprotect(page, size, PROT_READ | PROT_EXEC) == 0;
  munmap(page, size);
  return executable;
}

// Points ICU, and so every page's Date and Intl, at the system time zone.
// HarmonyOS has no /etc/localtime and sets no TZ, so ICU fell back to the C
// library's abbreviation "CST" and read it as America/Chicago: pages ran 13
// hours off China time, and Cloudflare's checks failed on the mismatch with
// the IP's location. Must run before Chromium initializes ICU; a TZ the
// environment already sets wins. Loaded dynamically: the call is API 12+.
void ApplySystemTimeZone() {
  if (getenv("TZ")) {
    return;
  }
  using GetTimeZoneFn = int (*)(char*, uint32_t);
  void* library = dlopen("libtime_service_ndk.so", RTLD_NOW | RTLD_LOCAL);
  auto* get_time_zone = library ? reinterpret_cast<GetTimeZoneFn>(dlsym(
                                      library, "OH_TimeService_GetTimeZone"))
                                : nullptr;
  constexpr uint32_t kTimeZoneBufferSize = 64;
  char time_zone[kTimeZoneBufferSize] = {};
  if (!get_time_zone ||
      get_time_zone(time_zone, kTimeZoneBufferSize) != 0 || !time_zone[0]) {
    AURA_LOG_E("AuraShell could not read the system time zone");
    return;
  }
  setenv("TZ", time_zone, /*overwrite=*/0);
  AURA_LOG_I("AuraShell time zone %{public}s", time_zone);
}

void AppendSwitchWithValue(std::vector<std::string>* arguments,
                           const std::string& name,
                           const std::string& value) {
  if (!value.empty()) {
    arguments->push_back(name + "=" + value);
  }
}

// Whether this device lets the app create native child processes. HarmonyOS
// opens that only on PC/2-in-1 and tablet; a phone refuses with
// NCP_ERR_NOT_SUPPORTED (801). Without child processes Chromium has to run
// --single-process, and single-process Chromium cannot hold more than one
// BrowserContext -- which is why incognito is unavailable on phones.
//
// OH_Ability_IsNativeChildProcessSupported() is the platform's own answer, but
// it only exists from API 26. Resolve it at run time and fall back to the
// device class where it is missing, so an older device still boots.
bool SupportsNativeChildProcess(const std::string& device_class) {
  using IsSupportedFn = bool (*)();
  static IsSupportedFn is_supported = []() -> IsSupportedFn {
    void* lib = dlopen("libchild_process.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
      return nullptr;
    }
    return reinterpret_cast<IsSupportedFn>(
        dlsym(lib, "OH_Ability_IsNativeChildProcessSupported"));
  }();

  if (is_supported) {
    const bool supported = is_supported();
    AURA_LOG_I("AuraShell native child process supported=%{public}d (platform)",
               supported ? 1 : 0);
    return supported;
  }
  const bool supported = device_class == "tablet" || device_class == "2in1";
  AURA_LOG_I(
      "AuraShell native child process supported=%{public}d (device class, "
      "platform query unavailable)",
      supported ? 1 : 0);
  return supported;
}

// Chromium keeps only the last value of a repeated switch, so one of these
// lists added by the shell would replace the list built here and silently
// turn off what the engine needs (UseOzonePlatform, the phone's own pickers).
// They are merged instead.
bool IsListSwitch(const std::string& key) {
  return key == "enable-features" || key == "disable-features" ||
         key == "enable-blink-features" || key == "disable-blink-features";
}

// Adds `value` to the --`key`= argument already in `arguments`, or appends
// one if there is none.
void AppendToListSwitch(std::vector<std::string>* arguments,
                        const std::string& key,
                        const std::string& value) {
  const std::string prefix = "--" + key + "=";
  for (std::string& argument : *arguments) {
    if (argument.rfind(prefix, 0) == 0) {
      argument += "," + value;
      return;
    }
  }
  arguments->push_back(prefix + value);
}

bool IsValidSwitchKey(const std::string& key) {
  if (key.empty() || key.front() == '-') {
    return false;
  }
  for (const char c : key) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
          c == '-' || c == '_')) {
      return false;
    }
  }
  return true;
}

bool ForwardChromiumLogToHilog(int severity,
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
  OH_LOG_Print(LOG_APP, level, 0xc233, "Chromium", "%{public}s",
               message.c_str());
  return false;
}

bool InitializeIcuFromRawFile(const AuraStartupConfig& config) {
#if ICU_UTIL_DATA_IMPL == ICU_UTIL_DATA_FILE
  if (config.icu_data_fd < 0 || config.icu_data_offset < 0 ||
      config.icu_data_length <= 0 ||
      static_cast<uint64_t>(config.icu_data_length) >
          std::numeric_limits<size_t>::max()) {
    AURA_LOG_E("AuraShell ICU rawfile descriptor is invalid");
    return false;
  }

  const int duplicated_fd = dup(config.icu_data_fd);
  if (duplicated_fd < 0) {
    AURA_LOG_E("AuraShell failed to duplicate ICU rawfile descriptor");
    return false;
  }

  const base::MemoryMappedFile::Region region = {
      config.icu_data_offset, static_cast<size_t>(config.icu_data_length)};
  if (!base::i18n::InitializeICUWithFileDescriptor(duplicated_fd, region)) {
    AURA_LOG_E("AuraShell failed to initialize Chromium ICU data");
    return false;
  }
  AURA_LOG_I("AuraShell initialized Chromium ICU data from HAP rawfile");
#endif
  return true;
}

bool ConfigureRuntimePaths(const AuraStartupConfig& config) {
  if (config.resources_dir.empty()) {
    OH_LOG_PrintMsg(LOG_APP, LOG_ERROR, 0xc233, "Chromium",
                    "AuraShell Chromium resources directory is empty");
    return false;
  }

  const base::FilePath resources_dir(config.resources_dir);
  if (!resources_dir.IsAbsolute() ||
      !base::PathService::Override(base::DIR_ASSETS, resources_dir)) {
    OH_LOG_PrintMsg(LOG_APP, LOG_ERROR, 0xc233, "Chromium",
                    "AuraShell failed to configure Chromium resources path");
    return false;
  }

  Dl_info module_info = {};
  if (!dladdr(reinterpret_cast<const void*>(&ConfigureRuntimePaths),
              &module_info) ||
      !module_info.dli_fname) {
    OH_LOG_PrintMsg(LOG_APP, LOG_ERROR, 0xc233, "Chromium",
                    "AuraShell failed to locate libweb_engine.so");
    return false;
  }

  const base::FilePath module_dir =
      base::FilePath(module_info.dli_fname).DirName();
  if (!module_dir.IsAbsolute() ||
      !base::PathService::Override(base::DIR_MODULE, module_dir)) {
    OH_LOG_PrintMsg(LOG_APP, LOG_ERROR, 0xc233, "Chromium",
                    "AuraShell failed to configure Chromium module path");
    return false;
  }
  return true;
}

}  // namespace

OhosChromeMainRunner::OhosChromeMainRunner() = default;

OhosChromeMainRunner::~OhosChromeMainRunner() = default;

bool OhosChromeMainRunner::EnsureStarted(const AuraStartupConfig& config) {
  chrome::ohos::UpdateAuraShellUiFamily(config.ui_family);
  chrome::ohos::UpdateAuraShellBrowserChrome(config.browser_chrome);
  chrome::ohos::SetAuraShellUiMenuLabel(config.shell_ui_menu_label);
  chrome::ohos::SetAuraShellProductName(config.product_name);
  chrome::ohos::UpdateAuraShellColorScheme(config.color_scheme);
  chrome::ohos::UpdateAuraShellPrintOutputDirectory(config.print_output_dir);

  std::lock_guard<std::mutex> lock(mutex_);
  if (started_) {
    if (startup_config_.vulkan_output_mode != config.vulkan_output_mode ||
        startup_config_.vulkan_hdr_white_nits != config.vulkan_hdr_white_nits ||
        startup_config_.vulkan_hdr_peak_nits != config.vulkan_hdr_peak_nits ||
        startup_config_.experimental_phone_fork !=
            config.experimental_phone_fork ||
        (config.experimental_phone_fork &&
         startup_config_.gpu_process != config.gpu_process)) {
      AURA_LOG_E("AuraShell process mode switch requires a cold app restart");
      return false;
    }
    return true;
  }

  // Only reset the color failure latch on a genuinely new browser session.
  if (!config.user_data_dir.empty()) {
    unlink((config.user_data_dir + "/GpuOutputColorFailed").c_str());
    unlink((config.user_data_dir + "/GpuGraphiteFailed").c_str());
  }
  startup_config_ = config;
  ApplySystemTimeZone();
  // ANGLE reaches the system EGL and GLES through the HarmonyOS wrapper's
  // exports here too, as in a GPU process of its own: an EGLImage of a
  // native buffer can only be made by the wrapper, against a display the
  // wrapper gave out, and the display ANGLE got through eglGetProcAddress
  // was the driver's. See ohos-angle.patch.
  setenv("OHOS_ANGLE_WRAPPER_EXPORTS", "1", /*overwrite=*/0);
  arguments_ = BuildArgumentsLocked(config);
  const base::CommandLine startup_command_line(arguments_);
  if (startup_command_line.HasSwitch(base::internal::kOhosIsolateRenderers) &&
      startup_command_line.HasSwitch("single-process")) {
    AURA_LOG_E("AuraShell isolated renderers require native child processes; "
               "startup refused");
    return false;
  }
  if (config.experimental_phone_fork !=
      startup_command_line.HasSwitch(base::internal::kOhosPhoneFork)) {
    AURA_LOG_E("AuraShell phone fork must be selected by experimentalPhoneFork");
    return false;
  }
  if (config.experimental_phone_fork) {
    if ((config.device_class != "phone" && config.device_class != "foldable") ||
        config.headless || startup_command_line.HasSwitch("single-process") ||
        startup_command_line.HasSwitch("type") ||
        startup_command_line.HasSwitch(base::internal::kOhosIsolateRenderers) ||
        base::CommandLine::InitializedForCurrentProcess()) {
      AURA_LOG_E("AuraShell phone fork configuration conflicts or runtime "
                 "already initialized; startup refused");
      return false;
    }
    const base::FilePath trace_directory(config.user_data_dir);
    const bool fd_trace =
        !trace_directory.empty() && base::CreateDirectory(trace_directory) &&
        base::internal::ConfigureOhosForkFdTrace(
            trace_directory.AppendASCII("PhoneForkFdTrace.log").value());
    AURA_LOG_I(
        "AuraShell phone FD trace enabled=%{public}d file=PhoneForkFdTrace.log "
        "in user-data-dir",
        fd_trace);
    if (config.gpu_process == "separate") {
      const bool prepared = ui::phone_gpu::PrepareSharedBufferAllocator();
      AURA_LOG_I("AuraShell phone GPU shared-buffer allocator prepared=%{public}d", prepared);
    }
    Dl_info module = {};
    if (!dladdr(reinterpret_cast<const void*>(&ConfigureRuntimePaths), &module) ||
        !module.dli_fname ||
        !base::internal::StartOhosForkServer(
            base::FilePath(module.dli_fname)
                .DirName()
                .AppendASCII("libnweb_render.so")
                .value())) {
      AURA_LOG_E(
          "AuraShell failed to start early phone fork server errno=%{public}d",
          errno);
      return false;
    }
    AURA_LOG_I("AuraShell experimental phone fork server ready before ChromeMain; "
               "same UID, no renderer sandbox");
  }
  started_ = true;
  if (!config.headless) {
    mcp_server_.Start();
  }
  chrome_thread_ =
      std::thread(&OhosChromeMainRunner::RunChromeMainOnThread, this);
  chrome_thread_.detach();
  AURA_LOG_I(
      "AuraShell Chromium runtime requested uiFamily=%{public}s "
      "startUrl=%{public}s",
      config.ui_family.c_str(), UrlForLog(config.start_url).c_str());
  return true;
}

bool OhosChromeMainRunner::Navigate(const std::string& component_id,
                                    const std::string& url) {
  const gfx::AcceleratedWidget widget =
      ui::GetOhosAcceleratedWidgetForNativeSurface(component_id);
  if (widget == gfx::kNullAcceleratedWidget && component_id != "aura_shell") {
    return false;
  }
  return chrome::ohos::NavigateAuraShellBrowser(widget, url);
}

bool OhosChromeMainRunner::ExecuteBrowserCommand(
    const std::string& component_id,
    const std::string& command_json) {
  const gfx::AcceleratedWidget widget =
      ui::GetOhosAcceleratedWidgetForNativeSurface(component_id);
  if (widget == gfx::kNullAcceleratedWidget && component_id != "aura_shell") {
    return false;
  }
  return chrome::ohos::ExecuteAuraShellBrowserCommand(widget, command_json);
}

void OhosChromeMainRunner::SetBrowserStateCallback(
    base::RepeatingCallback<void(gfx::AcceleratedWidget, const std::string&)>
        callback) {
  chrome::ohos::SetAuraShellBrowserStateCallback(std::move(callback));
}

void OhosChromeMainRunner::SetVisible(const std::string& component_id,
                                      bool visible) {
  const gfx::AcceleratedWidget widget =
      ui::GetOhosAcceleratedWidgetForNativeSurface(component_id);
  if (widget == gfx::kNullAcceleratedWidget && component_id != "aura_shell") {
    return;
  }
  chrome::ohos::SetAuraShellBrowserVisible(widget, visible);
}

void OhosChromeMainRunner::SetFocused(const std::string& component_id,
                                      bool focused) {
  const gfx::AcceleratedWidget widget =
      ui::GetOhosAcceleratedWidgetForNativeSurface(component_id);
  if (widget == gfx::kNullAcceleratedWidget && component_id != "aura_shell") {
    return;
  }
  chrome::ohos::SetAuraShellBrowserFocused(widget, focused);
}

void OhosChromeMainRunner::OnThemeFontChanged(const std::string& font_id) {
  chrome::ohos::NotifyAuraShellThemeFontChanged(font_id);
}

void OhosChromeMainRunner::Shutdown() {
  AURA_LOG_I("AuraShell Chromium runtime shutdown requested");
  mcp_server_.Stop();
  chrome::ohos::ShutdownAuraShellBrowser();
}

bool OhosChromeMainRunner::started() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return started_;
}

std::vector<std::string> OhosChromeMainRunner::arguments_for_testing() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return arguments_;
}

std::vector<std::string> OhosChromeMainRunner::BuildArgumentsForTesting(
    const AuraStartupConfig& config) const {
  return BuildArgumentsLocked(config);
}

std::vector<std::string> OhosChromeMainRunner::BuildArgumentsLocked(
    const AuraStartupConfig& config) const {
  std::vector<std::string> arguments = {
      "chrome",
      "--no-first-run",
      "--disable-fre",
      "--hide-crash-restore-bubble",
      "--ozone-platform=ohos",
      "--no-zygote",
      "--no-sandbox",
      "--disable-component-update",
      "--disable-domain-reliability",
  };

  if (config.headless) {
    // The host app owns the surfaces and GL context; any local app could
    // reach debugging ports.
    arguments.push_back("--no-startup-window");
    arguments.push_back("--disable-gpu");
    // This mode exists to host browser services, including a sync
    // backend the embedder brings itself.
    arguments.push_back("--ohos-enable-sync");
  } else {
    arguments.push_back("--remote-debugging-address=127.0.0.1");
    arguments.push_back("--remote-debugging-port=9222");
    const bool can_jit = ProcessCanJit();
    AURA_LOG_I("AuraShell JIT %{public}s (config jitless=%{public}d)",
               can_jit ? "available" : "unavailable", config.jitless);
    if (config.jitless || !can_jit) {
      arguments.push_back("--js-flags=--jitless --wasm-jitless");
    }

    // Unset on purpose almost always: the blocklist already takes
    // rasterization off a GPU whose fences do not work, which is what the
    // 2in1 emulator turned out to have. This is for running the comparison
    // the other way without a rebuild.
    // No switch turns compositing back on -- it is on unless something
    // took it away -- so only the off case has anything to add.
    if (config.gpu_compositing.has_value() && !*config.gpu_compositing) {
      arguments.push_back("--disable-gpu-compositing");
      AURA_LOG_I("AuraShell GPU compositing off by config");
    }

    // Chromium's own counter, over the page and over its own UI, rather
    // than a number invented here: it is what the compositor actually
    // scheduled, and it is already drawn in a form people know how to read.
    if (config.show_frame_rate) {
      arguments.push_back("--show-fps-counter");
      arguments.push_back("--ui-show-fps-counter");
      AURA_LOG_I("AuraShell frame rate counter on");
    }

    if (config.gpu_rasterization.has_value()) {
      arguments.push_back(*config.gpu_rasterization
                              ? "--enable-gpu-rasterization"
                              : "--disable-gpu-rasterization");
      AURA_LOG_I("AuraShell GPU rasterization forced %{public}s by config",
                 *config.gpu_rasterization ? "on" : "off");
    }

    arguments.push_back("--use-gl=angle");
    // ANGLE on HarmonyOS's own EGL and GLES unless the shell asks for
    // Vulkan, which presents through VK_OHOS_surface
    // (ohos-angle.patch) and is here to be measured against
    // GLES before it could be anyone's default.
    if (config.angle_backend == "vulkan" &&
        !(config.experimental_phone_fork && config.gpu_process == "separate")) {
      arguments.push_back("--use-angle=vulkan");
      AURA_LOG_I("AuraShell ANGLE backend vulkan by config");
    } else {
      arguments.push_back("--use-angle=gles-egl");
    }
  }

  AppendSwitchWithValue(&arguments, "--ohos-ui-profile", config.ui_profile);
  AppendSwitchWithValue(&arguments, "--ohos-ui-family", config.ui_family);
  AppendSwitchWithValue(&arguments, "--ohos-device-class", config.device_class);
  AppendSwitchWithValue(&arguments, "--ohos-color-scheme", config.color_scheme);
  AppendSwitchWithValue(&arguments, "--user-data-dir", config.user_data_dir);
  AppendSwitchWithValue(&arguments, "--lang", config.application_locale);

  // A repeated --enable-features switch would replace this list.
  //
  // OhosExternalPopupMenus (chrome_client_impl.cc): on a phone a <select>
  // and date and time inputs open the shell's own pickers, as Android opens
  // its own.
  //
  // TouchTextEditingRedesign (ui_base_features.cc, on by default only on
  // ChromeOS): in a text field, long press then drag moves the caret or the
  // selection with a magnifier above the finger, and a horizontal swipe moves
  // the caret -- what a phone user expects from any text field. Without it
  // Aura offers only the handles.
  arguments.push_back(
      config.ui_family == "mobile_phone"
          ? "--enable-features=UseOzonePlatform,OverlayScrollbar,"
            "OhosExternalPopupMenus,TouchTextEditingRedesign"
          : "--enable-features=UseOzonePlatform");

  if (config.ui_family == "mobile_phone") {
    arguments.push_back("--use-mobile-user-agent");
  }

  // The Shape Detection API is backed by ScanKit and CoreVisionKit here
  // (//components/ohos_shape_detection). Upstream ships BarcodeDetector only
  // where a platform backend exists and keeps the other two experimental;
  // turning them on at startup avoids regenerating Blink's feature table.
  //
  // navigator.share, which upstream turns on only where it binds a share
  // service; ours sends to the shell (BindAuraShellShareService).
  //
  // MediaCapture, stable only on Android: without it Blink drops
  // <input capture> before FileChooserParams, and the shell cannot open the
  // camera for it (filePickerRequested's capture field).
  //
  // ContactsManager (navigator.contacts) and its address property, stable
  // only on Android; the shell shows the system contact picker
  // (contactsPickerRequested).
  //
  // On a phone also Web NFC (NDEFReader), stable only on Android: the shell
  // reads and writes tags (BindAuraShellNfc).
  //
  // On a phone also window.orientation and orientationchange, which Android
  // ships and mobile sites and players use to tell a phone and follow its
  // rotation. Not on a tablet or PC: their desktop User-Agent is there to get
  // desktop pages, and sites take window.orientation for a phone.
  arguments.push_back(
      config.ui_family == "mobile_phone"
          ? "--enable-blink-features=BarcodeDetector,FaceDetector,"
            "TextDetector,WebShare,MediaCapture,ContactsManager,"
            "ContactsManagerExtraProperties,WebNFC,OrientationEvent"
          : "--enable-blink-features=BarcodeDetector,FaceDetector,"
            "TextDetector,WebShare,MediaCapture,ContactsManager,"
            "ContactsManagerExtraProperties");

  // Reader mode needs the renderer to say whether a page is an article, and
  // the agent that answers is only created when this switch is present --
  // ShouldStartDistillabilityService() reads nothing else. Without it the
  // browser side waits forever and reader mode is offered on no page at all.
  arguments.push_back("--enable-distillability-service");
  // And this one, or tapping reader mode kills the browser process.
  // RegisterViewerSource() returns early without it, so the distiller's
  // isolated-world id is never set and keeps its "unset" value of -1;
  // ExecuteJavaScriptInIsolatedWorld then CHECKs that the id is above zero
  // and traps. It also registers the chrome-distiller:// data source, so
  // without it there would be nothing to show even if it survived.
  arguments.push_back("--enable-dom-distiller");

  const bool supports_native_child_process =
      SupportsNativeChildProcess(config.device_class);
  if (supports_native_child_process || config.experimental_phone_fork) {
    arguments.push_back("--renderer-process-limit=16");
  } else {
    arguments.push_back("--single-process");
  }

  if (config.experimental_phone_fork) {
    arguments.push_back(std::string("--") + base::internal::kOhosPhoneFork);
  }

  // The GPU stays in this process unless the shell asks otherwise and the
  // device can start a native child to hold it. A phone cannot; asking there
  // is logged and ignored rather than turned into a failed start.
  bool separate_gpu_process = config.gpu_process == "separate";
  // The last run could not start its GPU child and said so. Once in the
  // browser process, then the next launch tries again: see
  // ohos_gpu_child_channel.cc.
  if (separate_gpu_process && !config.user_data_dir.empty()) {
    const std::string marker =
        config.user_data_dir + "/" + ui::kOhosGpuChildFailedMarker;
    if (access(marker.c_str(), F_OK) == 0) {
      unlink(marker.c_str());
      AURA_LOG_E("AuraShell GPU child failed last run; in process this once");
      separate_gpu_process = false;
    }
  }
  const bool gpu_in_own_process =
      separate_gpu_process &&
      (supports_native_child_process || config.experimental_phone_fork) &&
      !config.headless;
  if (gpu_in_own_process) {
    AURA_LOG_I("AuraShell GPU in a process of its own by config");
  } else {
    if (separate_gpu_process) {
      AURA_LOG_E("AuraShell ignored gpuProcess separate: device class "
                 "%{public}s cannot start native children",
                 config.device_class.c_str());
    }
    arguments.push_back("--in-process-gpu");
  }

  // Skia on Vulkan, for comparing with GL. Upstream only runs it with the GPU
  // in a process of its own, so that is the only place it is asked for. If
  // Vulkan cannot be brought up there, Chromium falls back to GL by itself;
  // the GPU process says which step failed (ohos_gpu_child_channel.cc).
  const bool graphite = config.skia_backend == "graphite";
  if (config.skia_backend == "vulkan" || graphite) {
    if (gpu_in_own_process) {
      arguments.push_back("--use-vulkan=native");
      AppendToListSwitch(&arguments, "enable-features", "Vulkan");
      if (graphite) {
        arguments.push_back("--enable-skia-graphite");
        arguments.push_back("--skia-graphite-dawn-backend=vulkan");
        AURA_LOG_I("AuraShell experimental Graphite Vulkan; Ganesh fallback");
      } else {
        arguments.push_back("--disable-skia-graphite");
      }
      AURA_LOG_I("AuraShell Skia on Vulkan by config");
    } else {
      AURA_LOG_E(
          "AuraShell ignored skiaBackend vulkan: the GPU is not in a "
          "separate GPU process");
    }
  }

  // Native EGL WSI needs the same user output mode and trusted luminance
  // snapshot as Skia Vulkan. This does not change the phone fork's GL route.
  if (((config.skia_backend == "vulkan" || graphite) && gpu_in_own_process) ||
      (!config.headless && config.angle_backend == "vulkan" &&
       !(config.experimental_phone_fork && config.gpu_process == "separate"))) {
    if (config.vulkan_output_mode == "sdr10" ||
        config.vulkan_output_mode == "hdr10" ||
        config.vulkan_output_mode == "auto") {
      arguments.push_back("--ohos-vulkan-output=" +
                          config.vulkan_output_mode);
      auto brightness = ui::GetOhosDisplayBrightness();
      auto color = brightness ? ui::OhosContentOutputColor(*brightness)
                              : ui::OhosOutputColor{};
      const bool fixed = config.vulkan_hdr_white_nits != 0 ||
                         config.vulkan_hdr_peak_nits != 0;
      if (fixed) {
        auto override = ui::MakeOhosDisplayBrightness(
            0, config.vulkan_hdr_white_nits,
            config.vulkan_hdr_peak_nits / config.vulkan_hdr_white_nits,
            config.vulkan_hdr_peak_nits / config.vulkan_hdr_white_nits);
        color = override ? override->hdr : ui::OhosOutputColor{};
        arguments.push_back("--ohos-hdr-fixed-luminance");
      }
      if (color.mode == ui::OhosOutputMode::kHdr10) {
        arguments.push_back("--ohos-hdr-white-millinits=" +
                            std::to_string(color.white_millinits));
        arguments.push_back("--ohos-hdr-peak-millinits=" +
                            std::to_string(color.peak_millinits));
      }
      // This snapshot predates ForwardChromiumLogToHilog. Use the native
      // startup logger so a successful initial query is not invisible.
      AURA_LOG_I(
          "OHOS HDR startup: requested=%{public}s "
          "system_brightness=%{public}d "
          "fixed=%{public}d white_millinits=%{public}u "
          "peak_millinits=%{public}u",
          config.vulkan_output_mode.c_str(), brightness.has_value(), fixed,
          color.white_millinits, color.peak_millinits);
    }
  }

  for (const AuraAdditionalSwitch& additional_switch :
       config.additional_switches) {
    if (!IsValidSwitchKey(additional_switch.key)) {
      AURA_LOG_E("AuraShell ignored an invalid additional switch key");
      continue;
    }
    if (IsListSwitch(additional_switch.key)) {
      // Empty, it would clear the list rather than add to it.
      if (additional_switch.value.empty()) {
        AURA_LOG_E("AuraShell ignored an empty --%{public}s",
                   additional_switch.key.c_str());
      } else {
        AppendToListSwitch(&arguments, additional_switch.key,
                           additional_switch.value);
      }
      continue;
    }
    arguments.push_back(
        additional_switch.value.empty()
            ? "--" + additional_switch.key
            : "--" + additional_switch.key + "=" + additional_switch.value);
  }

  if (!config.headless) {
    arguments.push_back(config.start_url.empty() ? kChromiumHomeUrl
                                                 : config.start_url);
  }
  return arguments;
}

void OhosChromeMainRunner::RunChromeMainOnThread() {
  std::vector<std::string> arguments;
  AuraStartupConfig startup_config;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    arguments = arguments_;
    startup_config = startup_config_;
  }

  logging::SetLogMessageHandler(&ForwardChromiumLogToHilog);
  if (!InitializeIcuFromRawFile(startup_config) ||
      !ConfigureRuntimePaths(startup_config)) {
    if (startup_config.experimental_phone_fork)
      base::internal::StopOhosForkServer();
    std::lock_guard<std::mutex> lock(mutex_);
    started_ = false;
    return;
  }

  std::vector<const char*> argv;
  argv.reserve(arguments.size());
  for (const std::string& argument : arguments) {
    argv.push_back(argument.c_str());
  }

  AURA_LOG_I("AuraShell starting ChromeMain argc=%{public}zu", argv.size());
  const int exit_code = ChromeMain(static_cast<int>(argv.size()), argv.data());
  if (startup_config.experimental_phone_fork)
    base::internal::StopOhosForkServer();
  mcp_server_.Stop();
  chrome::ohos::NotifyAuraShellBrowserStopped();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    started_ = false;
  }
  AURA_LOG_I("AuraShell ChromeMain exited code=%{public}d", exit_code);
  (void)exit_code;
}

OhosChromeMainRunner& GetOhosChromeMainRunner() {
  static base::NoDestructor<OhosChromeMainRunner> runner;
  return *runner;
}

std::string UrlForLog(std::string_view url) {
  const size_t colon = url.find(':');
  if (colon == std::string_view::npos || colon == 0) {
    return url.empty() ? std::string() : std::string("(no scheme)");
  }
  const std::string_view scheme = url.substr(0, colon);
  for (const char c : scheme) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '+' || c == '-' ||
                    c == '.';
    if (!ok) {
      return "(no scheme)";
    }
  }
  std::string_view rest = url.substr(colon + 1);
  if (!rest.starts_with("//")) {
    return std::string(scheme) + ":";
  }
  rest.remove_prefix(2);
  rest = rest.substr(0, rest.find_first_of("/?#\\"));
  // user:password@host keeps only the host.
  const size_t at = rest.rfind('@');
  if (at != std::string_view::npos) {
    rest.remove_prefix(at + 1);
  }
  return std::string(scheme) + "://" + std::string(rest);
}

}  // namespace ohos_nweb
