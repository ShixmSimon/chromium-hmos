// Copyright (c) 2026
// Licensed under the Apache License, Version 2.0.

#include "ohos_nweb/src/aura_shell/ohos_chrome_main_runner.h"

#include <dlfcn.h>
#include <hilog/log.h>
#include <unistd.h>

#include <limits>
#include <utility>

#include "base/base_paths.h"
#include "base/files/file_path.h"
#include "base/files/memory_mapped_file.h"
#include "base/functional/callback.h"
#include "base/i18n/icu_util.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "chrome/browser/ui/ohos/aura_shell_runtime_bridge.h"
#include "ohos_nweb/src/nweb_hilog.h"
#include "ui/ozone/platform/ohos/ohos_native_window_registry.h"

extern "C" int ChromeMain(int argc, const char** argv);

namespace ohos_nweb {
namespace {

// The ArkWeb DFX logging macros compile out when enable_arkweb=false, which
// would hide startup failures of a runtime that has no window to show them.
#define AURA_LOG_I(...) \
  OH_LOG_Print(LOG_APP, LOG_INFO, 0xc233, "AuraShell", __VA_ARGS__)
#define AURA_LOG_E(...) \
  OH_LOG_Print(LOG_APP, LOG_ERROR, 0xc233, "AuraShell", __VA_ARGS__)


void AppendSwitchWithValue(std::vector<std::string>* arguments,
                           const std::string& name,
                           const std::string& value) {
  if (!value.empty()) {
    arguments->push_back(name + "=" + value);
  }
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
  chrome::ohos::UpdateAuraShellColorScheme(config.color_scheme);
  chrome::ohos::UpdateAuraShellPrintOutputDirectory(config.print_output_dir);

  std::lock_guard<std::mutex> lock(mutex_);
  if (started_) {
    return true;
  }

  startup_config_ = config;
  arguments_ = BuildArgumentsLocked(config);
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
      config.ui_family.c_str(), config.start_url.c_str());
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
      "--in-process-gpu",
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
    if (config.jitless) {
    arguments.push_back("--js-flags=--jitless --wasm-jitless");
  }

  arguments.push_back("--use-gl=angle");
    // The device Vulkan driver lacks VK_KHR_display required by ANGLE's
    // Linux Vulkan display; use ANGLE on the native HarmonyOS EGL/GLES.
    arguments.push_back("--use-angle=gles-egl");
  }

  AppendSwitchWithValue(&arguments, "--ohos-ui-profile", config.ui_profile);
  AppendSwitchWithValue(&arguments, "--ohos-ui-family", config.ui_family);
  AppendSwitchWithValue(&arguments, "--ohos-device-class", config.device_class);
  AppendSwitchWithValue(&arguments, "--ohos-color-scheme", config.color_scheme);
  AppendSwitchWithValue(&arguments, "--user-data-dir", config.user_data_dir);
  AppendSwitchWithValue(&arguments, "--lang", config.application_locale);

  // A repeated --enable-features switch would replace this list.
  //
  // ArkUI resamples touch moves to one per display frame, and now and then a
  // frame gets none, or gets it after the compositor's scroll deadline. The
  // default dispatch mode then produces no frame at all, so the page holds
  // for a refresh and jumps the next one: on PLA-AL10, 86 of 1360 frames
  // during a drag. UseScrollPredictorForDeadline keeps the wait-for-late-
  // scroll deadline and, when it passes empty, scrolls by the predicted
  // finger position instead.
  constexpr char kScrollPrediction[] =
      "WaitForLateScrollEvents:mode/UseScrollPredictorForDeadline";
  arguments.push_back(
      std::string("--enable-features=UseOzonePlatform,") +
      (config.ui_family == "mobile_phone" ? "OverlayScrollbar," : "") +
      kScrollPrediction);

  if (config.ui_family == "mobile_phone") {
    arguments.push_back("--use-mobile-user-agent");
  }

  const bool supports_native_child_process =
      config.device_class == "tablet" || config.device_class == "2in1";
  if (supports_native_child_process) {
    arguments.push_back("--renderer-process-limit=16");
  } else {
    arguments.push_back("--single-process");
  }

  for (const AuraAdditionalSwitch& additional_switch :
       config.additional_switches) {
    if (!IsValidSwitchKey(additional_switch.key)) {
      AURA_LOG_E("AuraShell ignored an invalid additional switch key");
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

}  // namespace ohos_nweb
