// Copyright (c) 2026
// Licensed under the Apache License, Version 2.0.

#ifndef OHOS_NWEB_SRC_AURA_SHELL_OHOS_CHROME_MAIN_RUNNER_H_
#define OHOS_NWEB_SRC_AURA_SHELL_OHOS_CHROME_MAIN_RUNNER_H_

#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "base/functional/callback_forward.h"
#include "ohos_nweb/src/aura_shell/ohos_aura_shell_types.h"
#include "ohos_nweb/src/aura_shell/ohos_mcp_server.h"
#include "ui/gfx/native_ui_types.h"

namespace ohos_nweb {

class OhosChromeMainRunner {
 public:
  OhosChromeMainRunner();
  OhosChromeMainRunner(const OhosChromeMainRunner&) = delete;
  OhosChromeMainRunner& operator=(const OhosChromeMainRunner&) = delete;
  ~OhosChromeMainRunner();

  bool EnsureStarted(const AuraStartupConfig& config);
  bool Navigate(const std::string& component_id, const std::string& url);
  bool ExecuteBrowserCommand(const std::string& component_id,
                             const std::string& command_json);
  void SetBrowserStateCallback(
      base::RepeatingCallback<void(gfx::AcceleratedWidget, const std::string&)>
          callback);
  void SetVisible(const std::string& component_id, bool visible);
  void SetFocused(const std::string& component_id, bool focused);
  void OnThemeFontChanged(const std::string& font_id);
  void Shutdown();

  bool started() const;
  std::vector<std::string> arguments_for_testing() const;
  std::vector<std::string> BuildArgumentsForTesting(
      const AuraStartupConfig& config) const;

 private:
  std::vector<std::string> BuildArgumentsLocked(
      const AuraStartupConfig& config) const;
  void RunChromeMainOnThread();

  mutable std::mutex mutex_;
  bool started_ = false;
  AuraStartupConfig startup_config_;
  std::vector<std::string> arguments_;
  std::thread chrome_thread_;
  OhosMcpServer mcp_server_;
};

OhosChromeMainRunner& GetOhosChromeMainRunner();

// A URL as a log line may show it: "scheme://host[:port]" or "scheme:".
// Never the user info, path, query or fragment -- a link another app opened
// Lumie with can carry a session token there, and hilog is readable outside
// the app. Chromium's own log lines use chrome::ohos::UrlForLog().
std::string UrlForLog(std::string_view url);

}  // namespace ohos_nweb

#endif  // OHOS_NWEB_SRC_AURA_SHELL_OHOS_CHROME_MAIN_RUNNER_H_
