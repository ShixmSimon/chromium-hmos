// Copyright (c) 2026
// Licensed under the Apache License, Version 2.0.

#ifndef OHOS_NWEB_SRC_AURA_SHELL_OHOS_AURA_SHELL_HOST_H_
#define OHOS_NWEB_SRC_AURA_SHELL_OHOS_AURA_SHELL_HOST_H_

#include <mutex>
#include <string>

#include "ohos_nweb/src/aura_shell/ohos_aura_input_router.h"
#include "ohos_nweb/src/aura_shell/ohos_aura_shell_types.h"
#include "ohos_nweb/src/aura_shell/ohos_aura_xcomponent_bridge.h"

namespace ohos_nweb {

class OhosAuraShellHost {
 public:
  explicit OhosAuraShellHost(std::string component_id);
  OhosAuraShellHost(const OhosAuraShellHost&) = delete;
  OhosAuraShellHost& operator=(const OhosAuraShellHost&) = delete;
  ~OhosAuraShellHost();

  void Initialize(AuraStartupConfig config);
  void OnNativeSurfaceCreated(void* window,
                              double x,
                              double y,
                              double width,
                              double height);
  void OnNativeSurfaceChanged(void* window,
                              double x,
                              double y,
                              double width,
                              double height);
  void OnNativeSurfaceDestroyed(void* window);
  void OnSurfaceCreated();
  void OnSurfaceChanged(double x,
                        double y,
                        double width,
                        double height,
                        double density);
  void OnSurfaceDestroyed();
  void OnVisibilityChanged(bool visible);
  void OnFocusChanged(bool focused);
  void OnThemeFontChanged(const std::string& font_id);
  void DispatchNativeTouchEvent(const OhosNativeTouchEvent& event);
  void DispatchPointerEvent(const std::string& event_json);
  bool DispatchKeyEvent(const std::string& event_json);
  void Navigate(const std::string& url);
  bool ExecuteBrowserCommand(const std::string& command_json);
  void Shutdown();

  const std::string& current_url() const { return current_url_; }
  const AuraStartupConfig& startup_config() const { return startup_config_; }
  const AuraSurfaceState& surface_state() const {
    return xcomponent_bridge_.state();
  }

 private:
  void MaybeStartChromium();

  std::string component_id_;
  AuraStartupConfig startup_config_;
  std::string current_url_ = kChromiumHomeUrl;
  void* native_window_ = nullptr;
  bool initialized_ = false;
  bool has_screen_origin_ = false;
  double screen_x_ = 0.0;
  double screen_y_ = 0.0;
  std::recursive_mutex mutex_;
  OhosAuraXComponentBridge xcomponent_bridge_;
  OhosAuraInputRouter input_router_;
};

}  // namespace ohos_nweb

#endif  // OHOS_NWEB_SRC_AURA_SHELL_OHOS_AURA_SHELL_HOST_H_
