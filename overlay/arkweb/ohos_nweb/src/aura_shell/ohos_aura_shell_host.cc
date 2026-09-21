// Copyright (c) 2026
// Licensed under the Apache License, Version 2.0.

#include "ohos_nweb/src/aura_shell/ohos_aura_shell_host.h"

#include <utility>

#include "ohos_nweb/src/aura_shell/ohos_chrome_main_runner.h"
#include "ohos_nweb/src/nweb_hilog.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/ozone/platform/ohos/ohos_native_window_registry.h"

namespace ohos_nweb {
namespace {

gfx::Rect ToPixelBounds(double x, double y, double width, double height) {
  return gfx::Rect(static_cast<int>(x), static_cast<int>(y),
                   static_cast<int>(width), static_cast<int>(height));
}

gfx::Rect ToPixelBoundsWithOrigin(double fallback_x,
                                  double fallback_y,
                                  double width,
                                  double height,
                                  bool has_screen_origin,
                                  double screen_x,
                                  double screen_y) {
  return ToPixelBounds(has_screen_origin ? screen_x : fallback_x,
                       has_screen_origin ? screen_y : fallback_y, width,
                       height);
}

}  // namespace

OhosAuraShellHost::OhosAuraShellHost(std::string component_id)
    : component_id_(std::move(component_id)), input_router_(component_id_) {}

OhosAuraShellHost::~OhosAuraShellHost() = default;

void OhosAuraShellHost::Initialize(AuraStartupConfig config) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  startup_config_ = std::move(config);
  if (startup_config_.start_url.empty()) {
    startup_config_.start_url = kChromiumHomeUrl;
  }

  current_url_ = startup_config_.start_url;
  initialized_ = true;
  ui::UpdateOhosDisplayMetrics(
      gfx::Size(static_cast<int>(startup_config_.display_width),
                static_cast<int>(startup_config_.display_height)),
      static_cast<float>(startup_config_.display_density));
  if (native_window_) {
    const AuraSurfaceState& surface = xcomponent_bridge_.state();
    xcomponent_bridge_.OnSurfaceChanged(surface.x, surface.y, surface.width,
                                        surface.height,
                                        startup_config_.display_density);
    ui::UpdateOhosNativeSurface(
        component_id_, native_window_,
        ToPixelBounds(surface.x, surface.y, surface.width, surface.height),
        static_cast<float>(startup_config_.display_density));
  }
  MaybeStartChromium();
  WVLOG_I(
      "AuraShell host initialized component=%{public}s "
      "uiFamily=%{public}s url=%{public}s",
      component_id_.c_str(), startup_config_.ui_family.c_str(),
      current_url_.c_str());
}

void OhosAuraShellHost::OnNativeSurfaceCreated(void* window,
                                               double x,
                                               double y,
                                               double width,
                                               double height) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (!window) {
    return;
  }
  if (native_window_ && native_window_ != window) {
    WVLOG_I("AuraShell replacing native surface component=%{public}s",
            component_id_.c_str());
    input_router_.ResetPointerState();
    ui::UnregisterOhosNativeSurface(component_id_, native_window_);
    xcomponent_bridge_.OnSurfaceDestroyed();
  }
  native_window_ = window;
  const double density = initialized_ ? startup_config_.display_density
                                      : xcomponent_bridge_.state().density;
  xcomponent_bridge_.OnSurfaceCreated();
  const gfx::Rect bounds = ToPixelBoundsWithOrigin(
      x, y, width, height, has_screen_origin_, screen_x_, screen_y_);
  xcomponent_bridge_.OnSurfaceChanged(bounds.x(), bounds.y(), bounds.width(),
                                      bounds.height(), density);
  ui::RegisterOhosNativeSurface(component_id_, window, bounds,
                                static_cast<float>(density));
  MaybeStartChromium();
}

void OhosAuraShellHost::OnNativeSurfaceChanged(void* window,
                                               double x,
                                               double y,
                                               double width,
                                               double height) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (native_window_ != window) {
    WVLOG_W(
        "AuraShell surface change rebound a replacement window "
        "component=%{public}s",
        component_id_.c_str());
    OnNativeSurfaceCreated(window, x, y, width, height);
    return;
  }
  const gfx::Rect bounds = ToPixelBoundsWithOrigin(
      x, y, width, height, has_screen_origin_, screen_x_, screen_y_);
  xcomponent_bridge_.OnSurfaceChanged(bounds.x(), bounds.y(), bounds.width(),
                                      bounds.height(),
                                      xcomponent_bridge_.state().density);
  ui::UpdateOhosNativeSurface(
      component_id_, window, bounds,
      static_cast<float>(xcomponent_bridge_.state().density));
}

void OhosAuraShellHost::OnNativeSurfaceDestroyed(void* window) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (native_window_ != window) {
    return;
  }
  input_router_.ResetPointerState();
  ui::UnregisterOhosNativeSurface(component_id_, window);
  native_window_ = nullptr;
  xcomponent_bridge_.OnSurfaceDestroyed();
}

void OhosAuraShellHost::OnSurfaceCreated() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  xcomponent_bridge_.OnSurfaceCreated();
}

void OhosAuraShellHost::OnSurfaceChanged(double x,
                                         double y,
                                         double width,
                                         double height,
                                         double density) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  has_screen_origin_ = true;
  screen_x_ = x;
  screen_y_ = y;
  xcomponent_bridge_.OnSurfaceChanged(x, y, width, height, density);
  if (native_window_) {
    ui::UpdateOhosNativeSurface(component_id_, native_window_,
                                ToPixelBounds(x, y, width, height),
                                static_cast<float>(density));
  }
}

void OhosAuraShellHost::OnSurfaceDestroyed() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  input_router_.ResetPointerState();
  if (native_window_) {
    ui::UnregisterOhosNativeSurface(component_id_, native_window_);
    native_window_ = nullptr;
  }
  xcomponent_bridge_.OnSurfaceDestroyed();
}

void OhosAuraShellHost::OnVisibilityChanged(bool visible) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  xcomponent_bridge_.OnVisibilityChanged(visible);
  GetOhosChromeMainRunner().SetVisible(component_id_, visible);
}

void OhosAuraShellHost::OnFocusChanged(bool focused) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  xcomponent_bridge_.OnFocusChanged(focused);
  input_router_.SetFocused(focused);
  GetOhosChromeMainRunner().SetFocused(component_id_, focused);
}

void OhosAuraShellHost::OnThemeFontChanged(const std::string& font_id) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  GetOhosChromeMainRunner().OnThemeFontChanged(font_id);
}

void OhosAuraShellHost::DispatchNativeTouchEvent(
    const OhosNativeTouchEvent& event) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  input_router_.DispatchNativeTouchEvent(event);
}

void OhosAuraShellHost::DispatchPointerEvent(const std::string& event_json) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  input_router_.DispatchPointerEvent(event_json);
}

bool OhosAuraShellHost::DispatchKeyEvent(const std::string& event_json) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  return input_router_.DispatchKeyEvent(event_json);
}

void OhosAuraShellHost::Navigate(const std::string& url) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  current_url_ = url.empty() ? kChromiumHomeUrl : url;
  const bool accepted =
      GetOhosChromeMainRunner().Navigate(component_id_, current_url_);
  WVLOG_I("AuraShell navigate component=%{public}s url=%{public}s",
          component_id_.c_str(), current_url_.c_str());
  if (!accepted) {
    WVLOG_E("AuraShell rejected invalid navigation URL component=%{public}s",
            component_id_.c_str());
  }
}

bool OhosAuraShellHost::ExecuteBrowserCommand(const std::string& command_json) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  return GetOhosChromeMainRunner().ExecuteBrowserCommand(component_id_,
                                                         command_json);
}

void OhosAuraShellHost::Shutdown() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  WVLOG_I("AuraShell host shutdown component=%{public}s",
          component_id_.c_str());
  input_router_.ResetPointerState();
  if (native_window_) {
    ui::UnregisterOhosNativeSurface(component_id_, native_window_);
    native_window_ = nullptr;
  }
  xcomponent_bridge_.OnSurfaceDestroyed();
  initialized_ = false;
}

void OhosAuraShellHost::MaybeStartChromium() {
  if (!initialized_ || !native_window_) {
    return;
  }
  GetOhosChromeMainRunner().EnsureStarted(startup_config_);
}

}  // namespace ohos_nweb
