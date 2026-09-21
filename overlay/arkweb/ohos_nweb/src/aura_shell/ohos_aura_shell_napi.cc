// Copyright (c) 2026
// Licensed under the Apache License, Version 2.0.

#include "ohos_nweb/src/aura_shell/ohos_aura_shell_napi.h"

#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "ace/xcomponent/native_interface_xcomponent.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/values.h"
#include "chrome/browser/ui/ohos/aura_shell_runtime_bridge.h"
#include "ohos_nweb/src/aura_shell/ohos_aura_shell_host.h"
#include "ohos_nweb/src/aura_shell/ohos_chrome_main_runner.h"
#include "ohos_nweb/src/nweb_hilog.h"
#include "ui/ozone/platform/ohos/ohos_native_window_registry.h"

namespace ohos_nweb {
namespace {

using HostPtr = std::shared_ptr<OhosAuraShellHost>;
using HostMap = std::map<std::string, HostPtr>;
using WindowActionFunctionMap = std::map<std::string, napi_threadsafe_function>;
using BrowserEventFunctionMap = std::map<std::string, napi_threadsafe_function>;
using AuxiliaryWindowEventFunctionMap =
    std::map<std::string, napi_threadsafe_function>;

constexpr char kAuxiliarySurfacePrefix[] = "aura_aux_";
constexpr char kPwaSurfacePrefix[] = "aura_pwa_";

std::mutex& HostsMutex() {
  static base::NoDestructor<std::mutex> mutex;
  return *mutex;
}

HostMap& Hosts() {
  static base::NoDestructor<HostMap> hosts;
  return *hosts;
}

WindowActionFunctionMap& WindowActionFunctions() {
  static base::NoDestructor<WindowActionFunctionMap> functions;
  return *functions;
}

std::mutex& BrowserEventFunctionsMutex() {
  static base::NoDestructor<std::mutex> mutex;
  return *mutex;
}

BrowserEventFunctionMap& BrowserEventFunctions() {
  static base::NoDestructor<BrowserEventFunctionMap> functions;
  return *functions;
}

std::mutex& AuxiliaryWindowEventFunctionsMutex() {
  static base::NoDestructor<std::mutex> mutex;
  return *mutex;
}

AuxiliaryWindowEventFunctionMap& AuxiliaryWindowEventFunctions() {
  static base::NoDestructor<AuxiliaryWindowEventFunctionMap> functions;
  return *functions;
}

bool IsAuxiliaryComponentId(const std::string& component_id) {
  return component_id.starts_with(kAuxiliarySurfacePrefix);
}

bool IsPwaComponentId(const std::string& component_id) {
  return component_id.starts_with(kPwaSurfacePrefix);
}

void AddComponentEventTarget(const std::string& component_id,
                             base::DictValue* event) {
  if (!event || (!IsAuxiliaryComponentId(component_id) &&
                 !IsPwaComponentId(component_id))) {
    return;
  }
  const gfx::AcceleratedWidget widget =
      ui::GetOhosAcceleratedWidgetForNativeSurface(component_id);
  if (widget != gfx::kNullAcceleratedWidget &&
      widget <= static_cast<gfx::AcceleratedWidget>(
                    std::numeric_limits<int>::max())) {
    event->Set("targetWidget", static_cast<int>(widget));
  }
}

gfx::AcceleratedWidget GetComponentEventTarget(
    const std::string& component_id) {
  if (!IsAuxiliaryComponentId(component_id) &&
      !IsPwaComponentId(component_id)) {
    return gfx::kNullAcceleratedWidget;
  }
  return ui::GetOhosAcceleratedWidgetForNativeSurface(component_id);
}

const char* WindowActionName(ui::OhosWindowAction action) {
  switch (action) {
    case ui::OhosWindowAction::kClose:
      return "close";
    case ui::OhosWindowAction::kEnterFullscreen:
      return "enterFullscreen";
    case ui::OhosWindowAction::kExitFullscreen:
      return "exitFullscreen";
    case ui::OhosWindowAction::kMaximize:
      return "maximize";
    case ui::OhosWindowAction::kMinimize:
      return "minimize";
    case ui::OhosWindowAction::kRestore:
      return "restore";
    case ui::OhosWindowAction::kStartMoving:
      return "startMoving";
  }
}

void CallJsWindowAction(napi_env env,
                        napi_value callback,
                        void* context,
                        void* data) {
  (void)context;
  std::unique_ptr<std::string> action(static_cast<std::string*>(data));
  if (!env || !callback || !action) {
    return;
  }

  napi_value receiver = nullptr;
  napi_value argument = nullptr;
  napi_get_undefined(env, &receiver);
  napi_create_string_utf8(env, action->c_str(), action->size(), &argument);
  napi_call_function(env, receiver, callback, 1, &argument, nullptr);
}

void DispatchWindowAction(napi_threadsafe_function function,
                          ui::OhosWindowAction action) {
  auto action_name = std::make_unique<std::string>(WindowActionName(action));
  if (napi_call_threadsafe_function(function, action_name.get(),
                                    napi_tsfn_nonblocking) == napi_ok) {
    action_name.release();
  }
}

void RegisterWindowActionFunction(napi_env env,
                                  const std::string& component_id,
                                  napi_value callback) {
  if (WindowActionFunctions().contains(component_id)) {
    return;
  }

  napi_valuetype callback_type = napi_undefined;
  if (napi_typeof(env, callback, &callback_type) != napi_ok ||
      callback_type != napi_function) {
    WVLOG_E("AuraShell window action handler is not a function");
    return;
  }

  napi_value resource_name = nullptr;
  napi_create_string_utf8(env, "ChromiumAuraWindowAction", NAPI_AUTO_LENGTH,
                          &resource_name);
  napi_threadsafe_function function = nullptr;
  if (napi_create_threadsafe_function(
          env, callback, nullptr, resource_name, 0, 1, nullptr, nullptr,
          nullptr, CallJsWindowAction, &function) != napi_ok) {
    WVLOG_E("AuraShell failed to create window action bridge");
    return;
  }

  napi_unref_threadsafe_function(env, function);
  WindowActionFunctions()[component_id] = function;
  ui::SetOhosWindowActionCallback(
      component_id,
      base::BindRepeating(&DispatchWindowAction, base::Unretained(function)));
}

void ReleaseWindowActionFunction(const std::string& component_id) {
  ui::SetOhosWindowActionCallback(component_id, {});
  auto it = WindowActionFunctions().find(component_id);
  if (it == WindowActionFunctions().end()) {
    return;
  }
  napi_release_threadsafe_function(it->second, napi_tsfn_abort);
  WindowActionFunctions().erase(it);
}

void CallJsBrowserEvent(napi_env env,
                        napi_value callback,
                        void* context,
                        void* data) {
  (void)context;
  std::unique_ptr<std::string> state_json(static_cast<std::string*>(data));
  if (!env || !callback || !state_json) {
    return;
  }

  napi_value receiver = nullptr;
  napi_value argument = nullptr;
  napi_get_undefined(env, &receiver);
  napi_create_string_utf8(env, state_json->c_str(), state_json->size(),
                          &argument);
  napi_call_function(env, receiver, callback, 1, &argument, nullptr);
}

void DispatchBrowserEvent(gfx::AcceleratedWidget widget,
                          const std::string& state_json) {
  std::lock_guard<std::mutex> lock(BrowserEventFunctionsMutex());
  napi_threadsafe_function function = nullptr;
  std::string target_component;
  if (widget != gfx::kNullAcceleratedWidget) {
    const std::optional<std::string> component_id =
        ui::GetOhosNativeSurfaceComponentIdForWidget(widget);
    if (component_id) {
      auto target = BrowserEventFunctions().find(*component_id);
      if (target != BrowserEventFunctions().end()) {
        target_component = target->first;
        function = target->second;
      }
    }
  } else {
    auto main = BrowserEventFunctions().find("aura_shell");
    if (main != BrowserEventFunctions().end()) {
      target_component = main->first;
      function = main->second;
    } else {
      for (const auto& [component_id, candidate] : BrowserEventFunctions()) {
        if (!IsAuxiliaryComponentId(component_id) &&
            !IsPwaComponentId(component_id)) {
          target_component = component_id;
          function = candidate;
          break;
        }
      }
    }
  }
  if (!function) {
    return;
  }
  auto state_copy = std::make_unique<std::string>(state_json);
  if (napi_call_threadsafe_function(function, state_copy.get(),
                                    napi_tsfn_nonblocking) == napi_ok) {
    state_copy.release();
  } else {
    WVLOG_W("AuraShell dropped browser state for component=%{public}s",
            target_component.c_str());
  }
}

void RegisterBrowserEventFunction(napi_env env,
                                  const std::string& component_id,
                                  napi_value callback) {
  std::lock_guard<std::mutex> lock(BrowserEventFunctionsMutex());
  if (BrowserEventFunctions().contains(component_id)) {
    return;
  }

  napi_valuetype callback_type = napi_undefined;
  if (napi_typeof(env, callback, &callback_type) != napi_ok ||
      callback_type != napi_function) {
    WVLOG_E("AuraShell browser event handler is not a function");
    return;
  }

  napi_value resource_name = nullptr;
  napi_create_string_utf8(env, "ChromiumAuraBrowserState", NAPI_AUTO_LENGTH,
                          &resource_name);
  napi_threadsafe_function function = nullptr;
  if (napi_create_threadsafe_function(
          env, callback, nullptr, resource_name, 0, 1, nullptr, nullptr,
          nullptr, CallJsBrowserEvent, &function) != napi_ok) {
    WVLOG_E("AuraShell failed to create browser state bridge");
    return;
  }
  napi_unref_threadsafe_function(env, function);
  BrowserEventFunctions()[component_id] = function;
  GetOhosChromeMainRunner().SetBrowserStateCallback(
      base::BindRepeating(&DispatchBrowserEvent));
}

void ReleaseBrowserEventFunction(const std::string& component_id) {
  bool callbacks_remain = false;
  {
    std::lock_guard<std::mutex> lock(BrowserEventFunctionsMutex());
    auto it = BrowserEventFunctions().find(component_id);
    if (it != BrowserEventFunctions().end()) {
      napi_release_threadsafe_function(it->second, napi_tsfn_abort);
      BrowserEventFunctions().erase(it);
    }
    callbacks_remain = !BrowserEventFunctions().empty();
  }
  if (!callbacks_remain) {
    GetOhosChromeMainRunner().SetBrowserStateCallback({});
  }
}

void DispatchAuxiliaryWindowEvent(const ui::OhosLogicalWindowState& state) {
  base::DictValue event;
  event.Set("version", 1);
  event.Set("widget", static_cast<double>(state.widget));
  event.Set("componentId", std::string(kAuxiliarySurfacePrefix) +
                               base::NumberToString(state.widget));
  event.Set("x", state.bounds.x());
  event.Set("y", state.bounds.y());
  event.Set("width", state.bounds.width());
  event.Set("height", state.bounds.height());
  event.Set("visible", state.visible);
  event.Set("destroyed", state.destroyed);
  event.Set("stackingOrder", static_cast<double>(state.stacking_order));
  event.Set("windowRole", "auxiliary");
  if (std::optional<chrome::ohos::AuraShellWindowMetadata> metadata =
          chrome::ohos::GetAuraShellWindowMetadata(state.widget);
      metadata && metadata->is_pwa) {
    event.Set("windowRole", "pwa");
    event.Set("pwaAppId", metadata->app_id);
    event.Set("title", metadata->title);
    event.Set("url", metadata->url);
    event.Set("pwaStartUrl", metadata->start_url);
  }

  std::string state_json;
  if (!base::JSONWriter::Write(event, &state_json)) {
    return;
  }

  std::lock_guard<std::mutex> lock(AuxiliaryWindowEventFunctionsMutex());
  for (const auto& [component_id, function] : AuxiliaryWindowEventFunctions()) {
    auto state_copy = std::make_unique<std::string>(state_json);
    if (napi_call_threadsafe_function(function, state_copy.get(),
                                      napi_tsfn_nonblocking) == napi_ok) {
      state_copy.release();
    } else {
      WVLOG_W(
          "AuraShell dropped auxiliary window state for component=%{public}s",
          component_id.c_str());
    }
  }
}

void RegisterAuxiliaryWindowEventFunction(napi_env env,
                                          const std::string& component_id,
                                          napi_value callback) {
  napi_valuetype callback_type = napi_undefined;
  if (napi_typeof(env, callback, &callback_type) != napi_ok ||
      callback_type != napi_function) {
    WVLOG_E("AuraShell auxiliary window handler is not a function");
    return;
  }

  napi_value resource_name = nullptr;
  napi_create_string_utf8(env, "ChromiumAuraAuxiliaryWindow", NAPI_AUTO_LENGTH,
                          &resource_name);
  napi_threadsafe_function function = nullptr;
  if (napi_create_threadsafe_function(
          env, callback, nullptr, resource_name, 0, 1, nullptr, nullptr,
          nullptr, CallJsBrowserEvent, &function) != napi_ok) {
    WVLOG_E("AuraShell failed to create auxiliary window bridge");
    return;
  }
  napi_unref_threadsafe_function(env, function);
  {
    std::lock_guard<std::mutex> lock(AuxiliaryWindowEventFunctionsMutex());
    if (AuxiliaryWindowEventFunctions().contains(component_id)) {
      napi_release_threadsafe_function(function, napi_tsfn_abort);
      return;
    }
    AuxiliaryWindowEventFunctions()[component_id] = function;
  }
  ui::SetOhosLogicalWindowStateCallback(
      base::BindRepeating(&DispatchAuxiliaryWindowEvent));
}

void ReleaseAuxiliaryWindowEventFunction(const std::string& component_id) {
  bool callbacks_remain = false;
  {
    std::lock_guard<std::mutex> lock(AuxiliaryWindowEventFunctionsMutex());
    auto it = AuxiliaryWindowEventFunctions().find(component_id);
    if (it != AuxiliaryWindowEventFunctions().end()) {
      napi_release_threadsafe_function(it->second, napi_tsfn_abort);
      AuxiliaryWindowEventFunctions().erase(it);
    }
    callbacks_remain = !AuxiliaryWindowEventFunctions().empty();
  }
  if (!callbacks_remain) {
    ui::SetOhosLogicalWindowStateCallback({});
  }
}

HostPtr EnsureHost(const std::string& component_id);
HostPtr FindHost(const std::string& component_id);
HostPtr TakeHost(const std::string& component_id);

std::optional<std::string> ReadNativeComponentId(
    OH_NativeXComponent* component) {
  char id[OH_XCOMPONENT_ID_LEN_MAX + 1] = {};
  uint64_t size = sizeof(id);
  if (OH_NativeXComponent_GetXComponentId(component, id, &size) !=
      OH_NATIVEXCOMPONENT_RESULT_SUCCESS) {
    return std::nullopt;
  }
  return std::string(id);
}

void ReadNativeSurfaceGeometry(OH_NativeXComponent* component,
                               void* window,
                               double* x,
                               double* y,
                               uint64_t* width,
                               uint64_t* height) {
  if (OH_NativeXComponent_GetXComponentOffset(component, window, x, y) !=
      OH_NATIVEXCOMPONENT_RESULT_SUCCESS) {
    *x = 0.0;
    *y = 0.0;
  }
  if (OH_NativeXComponent_GetXComponentSize(component, window, width, height) !=
      OH_NATIVEXCOMPONENT_RESULT_SUCCESS) {
    *width = 0;
    *height = 0;
  }
}

void NativeSurfaceCreated(OH_NativeXComponent* component, void* window) {
  std::optional<std::string> component_id = ReadNativeComponentId(component);
  if (!component_id || !window) {
    return;
  }
  double x = 0.0;
  double y = 0.0;
  uint64_t width = 0;
  uint64_t height = 0;
  ReadNativeSurfaceGeometry(component, window, &x, &y, &width, &height);

  EnsureHost(*component_id)
      ->OnNativeSurfaceCreated(window, x, y, static_cast<double>(width),
                               static_cast<double>(height));
}

void NativeSurfaceChanged(OH_NativeXComponent* component, void* window) {
  std::optional<std::string> component_id = ReadNativeComponentId(component);
  if (!component_id || !window) {
    return;
  }
  double x = 0.0;
  double y = 0.0;
  uint64_t width = 0;
  uint64_t height = 0;
  ReadNativeSurfaceGeometry(component, window, &x, &y, &width, &height);

  EnsureHost(*component_id)
      ->OnNativeSurfaceChanged(window, x, y, static_cast<double>(width),
                               static_cast<double>(height));
}

void NativeSurfaceDestroyed(OH_NativeXComponent* component, void* window) {
  std::optional<std::string> component_id = ReadNativeComponentId(component);
  if (!component_id) {
    return;
  }
  HostPtr host = IsAuxiliaryComponentId(*component_id)
                     ? TakeHost(*component_id)
                     : FindHost(*component_id);
  if (host) {
    host->OnNativeSurfaceDestroyed(window);
  }
}

void NativeDispatchTouchEvent(OH_NativeXComponent* component, void* window) {
  std::optional<std::string> component_id = ReadNativeComponentId(component);
  if (!component_id || !window) {
    return;
  }

  OH_NativeXComponent_TouchEvent touch_event = {};
  if (OH_NativeXComponent_GetTouchEvent(component, window, &touch_event) !=
      OH_NATIVEXCOMPONENT_RESULT_SUCCESS) {
    return;
  }

  OH_NativeXComponent_EventSourceType source_type =
      OH_NATIVEXCOMPONENT_SOURCE_TYPE_UNKNOWN;
  OH_NativeXComponent_GetTouchEventSourceType(component, touch_event.id,
                                              &source_type);

  uint32_t point_index = 0;
  for (uint32_t index = 0; index < touch_event.numPoints &&
                           index < OH_NATIVE_XCOMPONENT_MAX_TOUCH_POINTS_NUMBER;
       ++index) {
    if (touch_event.touchPoints[index].id == touch_event.id) {
      point_index = index;
      break;
    }
  }
  OH_NativeXComponent_TouchPointToolType tool_type =
      OH_NATIVEXCOMPONENT_TOOL_TYPE_UNKNOWN;
  OH_NativeXComponent_GetTouchPointToolType(component, point_index, &tool_type);

  const bool is_mouse_like =
      source_type == OH_NATIVEXCOMPONENT_SOURCE_TYPE_MOUSE ||
      source_type == OH_NATIVEXCOMPONENT_SOURCE_TYPE_TOUCHPAD ||
      tool_type == OH_NATIVEXCOMPONENT_TOOL_TYPE_MOUSE;
  if (touch_event.type != OH_NATIVEXCOMPONENT_MOVE) {
    WVLOG_W(
        "AuraShell native touch action=%{public}d id=%{public}d "
        "source=%{public}d tool=%{public}d suppressed=%{public}d "
        "x=%{public}.1f y=%{public}.1f screenX=%{public}.1f "
        "screenY=%{public}.1f",
        static_cast<int>(touch_event.type), touch_event.id,
        static_cast<int>(source_type), static_cast<int>(tool_type),
        is_mouse_like, touch_event.x, touch_event.y, touch_event.screenX,
        touch_event.screenY);
  }
  if (is_mouse_like) {
    return;
  }

  HostPtr host = FindHost(*component_id);
  if (!host) {
    return;
  }
  if (touch_event.type == OH_NATIVEXCOMPONENT_DOWN) {
    host->OnFocusChanged(true);
  }
  host->DispatchNativeTouchEvent(
      {.action = static_cast<int>(touch_event.type),
       .pointer_id = touch_event.id,
       .x = touch_event.x,
       .y = touch_event.y,
       .root_x = touch_event.screenX,
       .root_y = touch_event.screenY,
       .timestamp_ns = touch_event.timeStamp,
       .target_widget = GetComponentEventTarget(*component_id)});
}

int NormalizeNativeMouseAction(OH_NativeXComponent_MouseEventAction action) {
  switch (action) {
    case OH_NATIVEXCOMPONENT_MOUSE_PRESS:
      return 0;
    case OH_NATIVEXCOMPONENT_MOUSE_RELEASE:
      return 1;
    case OH_NATIVEXCOMPONENT_MOUSE_MOVE:
      return 2;
    case OH_NATIVEXCOMPONENT_MOUSE_CANCEL:
      return 13;
    case OH_NATIVEXCOMPONENT_MOUSE_NONE:
      return -1;
  }
  return -1;
}

int NormalizeNativeMouseButton(OH_NativeXComponent_MouseEventButton button) {
  switch (button) {
    case OH_NATIVEXCOMPONENT_LEFT_BUTTON:
      return 0;
    case OH_NATIVEXCOMPONENT_RIGHT_BUTTON:
      return 1;
    case OH_NATIVEXCOMPONENT_MIDDLE_BUTTON:
      return 2;
    case OH_NATIVEXCOMPONENT_BACK_BUTTON:
      return 3;
    case OH_NATIVEXCOMPONENT_FORWARD_BUTTON:
      return 4;
    case OH_NATIVEXCOMPONENT_NONE_BUTTON:
      return -1;
  }
  return -1;
}

void NativeDispatchMouseEvent(OH_NativeXComponent* component, void* window) {
  std::optional<std::string> component_id = ReadNativeComponentId(component);
  if (!component_id || !window) {
    return;
  }

  OH_NativeXComponent_MouseEvent mouse_event = {};
  if (OH_NativeXComponent_GetMouseEvent(component, window, &mouse_event) !=
      OH_NATIVEXCOMPONENT_RESULT_SUCCESS) {
    return;
  }
  const int action = NormalizeNativeMouseAction(mouse_event.action);
  if (action < 0) {
    return;
  }
  if (action == 0 || action == 1) {
    WVLOG_W(
        "AuraShell native mouse action=%{public}d button=%{public}d "
        "x=%{public}.1f y=%{public}.1f screenX=%{public}.1f "
        "screenY=%{public}.1f",
        action, NormalizeNativeMouseButton(mouse_event.button), mouse_event.x,
        mouse_event.y, mouse_event.screenX, mouse_event.screenY);
  }

  base::DictValue event;
  event.Set("action", action);
  event.Set("pointerType", "mouse");
  event.Set("dispatchSource", "native");
  event.Set("pointerId", 0);
  event.Set("button", NormalizeNativeMouseButton(mouse_event.button));
  event.Set("x", static_cast<double>(mouse_event.x));
  event.Set("y", static_cast<double>(mouse_event.y));
  event.Set("rootX", static_cast<double>(mouse_event.screenX));
  event.Set("rootY", static_cast<double>(mouse_event.screenY));
  // Unlike native touch events on HarmonyOS PC, native mouse screen
  // coordinates are already global-display coordinates. Adding the
  // application window origin again routes clicks outside every Aura widget.
  event.Set("rootWindowRelative", false);
  event.Set("physicalPixels", true);
  event.Set("timestamp", static_cast<double>(mouse_event.timestamp));
  AddComponentEventTarget(*component_id, &event);

  std::string event_json;
  if (!base::JSONWriter::Write(event, &event_json)) {
    return;
  }
  HostPtr host = FindHost(*component_id);
  if (!host) {
    return;
  }
  if (action == 0) {
    host->OnFocusChanged(true);
  }
  host->DispatchPointerEvent(event_json);
}

void NativeDispatchHoverEvent(OH_NativeXComponent* component, bool is_hover) {
  (void)component;
  (void)is_hover;
}

void NativeFocusEvent(OH_NativeXComponent* component, void* window) {
  (void)window;
  std::optional<std::string> component_id = ReadNativeComponentId(component);
  if (!component_id) {
    return;
  }
  if (HostPtr host = FindHost(*component_id)) {
    host->OnFocusChanged(true);
  }
}

void NativeBlurEvent(OH_NativeXComponent* component, void* window) {
  (void)window;
  std::optional<std::string> component_id = ReadNativeComponentId(component);
  if (!component_id) {
    return;
  }
  if (HostPtr host = FindHost(*component_id)) {
    host->OnFocusChanged(false);
  }
}

bool NativeDispatchKeyEvent(OH_NativeXComponent* component, void* window) {
  (void)window;
  std::optional<std::string> component_id = ReadNativeComponentId(component);
  if (!component_id) {
    return false;
  }

  OH_NativeXComponent_KeyEvent* key_event = nullptr;
  if (OH_NativeXComponent_GetKeyEvent(component, &key_event) !=
          OH_NATIVEXCOMPONENT_RESULT_SUCCESS ||
      !key_event) {
    return false;
  }

  OH_NativeXComponent_KeyAction action = OH_NATIVEXCOMPONENT_KEY_ACTION_UNKNOWN;
  OH_NativeXComponent_KeyCode key_code = KEY_UNKNOWN;
  OH_NativeXComponent_EventSourceType source_type =
      OH_NATIVEXCOMPONENT_SOURCE_TYPE_UNKNOWN;
  int64_t timestamp = 0;
  if (OH_NativeXComponent_GetKeyEventAction(key_event, &action) !=
          OH_NATIVEXCOMPONENT_RESULT_SUCCESS ||
      OH_NativeXComponent_GetKeyEventCode(key_event, &key_code) !=
          OH_NATIVEXCOMPONENT_RESULT_SUCCESS) {
    return false;
  }
  OH_NativeXComponent_GetKeyEventSourceType(key_event, &source_type);
  OH_NativeXComponent_GetKeyEventTimestamp(key_event, &timestamp);

  base::DictValue event;
  event.Set("action", static_cast<int>(action));
  event.Set("keyCode", static_cast<int>(key_code));
  event.Set("keyText", "");
  event.Set("dispatchSource", "native");
  event.Set("sourceTool", static_cast<int>(source_type));
  event.Set("timestamp", static_cast<double>(timestamp));

  std::string event_json;
  if (!base::JSONWriter::Write(event, &event_json)) {
    return false;
  }

  HostPtr host = FindHost(*component_id);
  return host && host->DispatchKeyEvent(event_json);
}

OH_NativeXComponent_Callback g_native_xcomponent_callbacks = {
    .OnSurfaceCreated = NativeSurfaceCreated,
    .OnSurfaceChanged = NativeSurfaceChanged,
    .OnSurfaceDestroyed = NativeSurfaceDestroyed,
    .DispatchTouchEvent = NativeDispatchTouchEvent,
};

OH_NativeXComponent_MouseEvent_Callback g_native_mouse_callbacks = {
    .DispatchMouseEvent = NativeDispatchMouseEvent,
    .DispatchHoverEvent = NativeDispatchHoverEvent,
};

void RegisterNativeXComponentCallbacks(napi_env env, napi_value exports) {
  napi_value native_component_value = nullptr;
  if (napi_get_named_property(env, exports, OH_NATIVE_XCOMPONENT_OBJ,
                              &native_component_value) != napi_ok) {
    WVLOG_W("AuraShell native XComponent export is unavailable");
    return;
  }

  OH_NativeXComponent* native_component = nullptr;
  if (napi_unwrap(env, native_component_value,
                  reinterpret_cast<void**>(&native_component)) != napi_ok ||
      !native_component) {
    WVLOG_E("AuraShell failed to unwrap native XComponent");
    return;
  }

  const int32_t result = OH_NativeXComponent_RegisterCallback(
      native_component, &g_native_xcomponent_callbacks);
  if (result != OH_NATIVEXCOMPONENT_RESULT_SUCCESS) {
    WVLOG_E(
        "AuraShell failed to register XComponent callbacks result=%{public}d",
        result);
  }

  const int32_t mouse_result = OH_NativeXComponent_RegisterMouseEventCallback(
      native_component, &g_native_mouse_callbacks);
  if (mouse_result != OH_NATIVEXCOMPONENT_RESULT_SUCCESS) {
    WVLOG_E(
        "AuraShell failed to register XComponent mouse callbacks "
        "result=%{public}d",
        mouse_result);
  }

  const int32_t focus_result = OH_NativeXComponent_RegisterFocusEventCallback(
      native_component, NativeFocusEvent);
  if (focus_result != OH_NATIVEXCOMPONENT_RESULT_SUCCESS) {
    WVLOG_E(
        "AuraShell failed to register XComponent focus callback "
        "result=%{public}d",
        focus_result);
  }

  const int32_t blur_result = OH_NativeXComponent_RegisterBlurEventCallback(
      native_component, NativeBlurEvent);
  if (blur_result != OH_NATIVEXCOMPONENT_RESULT_SUCCESS) {
    WVLOG_E(
        "AuraShell failed to register XComponent blur callback "
        "result=%{public}d",
        blur_result);
  }

  const int32_t key_result =
      OH_NativeXComponent_RegisterKeyEventCallbackWithResult(
          native_component, NativeDispatchKeyEvent);
  if (key_result != OH_NATIVEXCOMPONENT_RESULT_SUCCESS) {
    WVLOG_E(
        "AuraShell failed to register XComponent key callback "
        "result=%{public}d",
        key_result);
  }

  const int32_t keyboard_result =
      OH_NativeXComponent_SetNeedSoftKeyboard(native_component, true);
  if (keyboard_result != OH_NATIVEXCOMPONENT_RESULT_SUCCESS) {
    WVLOG_W(
        "AuraShell failed to enable the XComponent soft keyboard "
        "result=%{public}d",
        keyboard_result);
  }
}

std::string ReadString(napi_env env, napi_value value) {
  size_t size = 0;
  napi_get_value_string_utf8(env, value, nullptr, 0, &size);
  std::vector<char> buffer(size + 1, '\0');
  napi_get_value_string_utf8(env, value, buffer.data(), buffer.size(), &size);
  return std::string(buffer.data(), size);
}

double ReadNumber(napi_env env, napi_value value) {
  double number = 0.0;
  napi_get_value_double(env, value, &number);
  return number;
}

bool ReadBool(napi_env env, napi_value value) {
  bool boolean_value = false;
  napi_get_value_bool(env, value, &boolean_value);
  return boolean_value;
}

napi_value MakeUndefined(napi_env env) {
  napi_value result = nullptr;
  napi_get_undefined(env, &result);
  return result;
}

napi_value MakeBoolean(napi_env env, bool value) {
  napi_value result = nullptr;
  napi_get_boolean(env, value, &result);
  return result;
}

void ReadStringField(const base::DictValue& dict,
                     const std::string& key,
                     std::string* out) {
  const std::string* value = dict.FindString(key);
  if (value && !value->empty()) {
    *out = *value;
  }
}

void ReadPositiveNumberField(const base::DictValue& dict,
                             const std::string& key,
                             double* out) {
  const base::Value* value = dict.Find(key);
  if (!value) {
    return;
  }
  const double number = value->is_int() ? static_cast<double>(value->GetInt())
                                        : value->GetIfDouble().value_or(0.0);
  if (number > 0.0) {
    *out = number;
  }
}

void ReadNonNegativeIntegerField(const base::DictValue& dict,
                                 const std::string& key,
                                 int64_t* out) {
  const base::Value* value = dict.Find(key);
  if (!value) {
    return;
  }
  const double number = value->is_int() ? static_cast<double>(value->GetInt())
                                        : value->GetIfDouble().value_or(-1.0);
  if (number >= 0.0) {
    *out = static_cast<int64_t>(number);
  }
}

AuraStartupConfig ParseStartupConfig(const std::string& config_json) {
  AuraStartupConfig config;
  std::optional<base::DictValue> parsed =
      base::JSONReader::ReadDict(config_json, base::JSON_PARSE_RFC);
  if (!parsed) {
    WVLOG_W("AuraShell startup config is not valid JSON, using defaults");
    return config;
  }

  const base::DictValue& dict = *parsed;
  ReadStringField(dict, "startUrl", &config.start_url);
  ReadStringField(dict, "uiProfile", &config.ui_profile);
  ReadStringField(dict, "uiFamily", &config.ui_family);
  ReadStringField(dict, "windowMode", &config.window_mode);
  ReadStringField(dict, "deviceClass", &config.device_class);
  ReadStringField(dict, "marketName", &config.market_name);
  ReadStringField(dict, "productModel", &config.product_model);
  ReadStringField(dict, "userDataDir", &config.user_data_dir);
  ReadStringField(dict, "printOutputDir", &config.print_output_dir);
  ReadStringField(dict, "resourcesDir", &config.resources_dir);
  ReadStringField(dict, "applicationLocale", &config.application_locale);
  ReadStringField(dict, "colorScheme", &config.color_scheme);
  ReadPositiveNumberField(dict, "displayWidth", &config.display_width);
  ReadPositiveNumberField(dict, "displayHeight", &config.display_height);
  ReadPositiveNumberField(dict, "displayDensity", &config.display_density);
  std::optional<bool> is_foldable = dict.FindBool("isFoldable");
  if (is_foldable.has_value()) {
    config.is_foldable = *is_foldable;
  }
  int64_t fold_status = -1;
  ReadNonNegativeIntegerField(dict, "foldStatus", &fold_status);
  if (fold_status <= std::numeric_limits<int>::max()) {
    config.fold_status = static_cast<int>(fold_status);
  }
  int64_t icu_data_fd = -1;
  ReadNonNegativeIntegerField(dict, "icuDataFd", &icu_data_fd);
  if (icu_data_fd <= std::numeric_limits<int>::max()) {
    config.icu_data_fd = static_cast<int>(icu_data_fd);
  }
  ReadNonNegativeIntegerField(dict, "icuDataOffset", &config.icu_data_offset);
  ReadNonNegativeIntegerField(dict, "icuDataLength", &config.icu_data_length);

  if (const base::ListValue* switches = dict.FindList("additionalSwitches")) {
    for (const base::Value& entry : *switches) {
      const base::DictValue* item = entry.GetIfDict();
      const std::string* key = item ? item->FindString("key") : nullptr;
      if (!key) {
        continue;
      }
      const std::string* value = item->FindString("value");
      config.additional_switches.push_back(
          {*key, value ? *value : std::string()});
    }
  }

  std::optional<bool> jitless = dict.FindBool("jitless");
  if (jitless.has_value()) {
    config.jitless = *jitless;
  }

  std::optional<bool> fullscreen_requested =
      dict.FindBool("fullscreenRequested");
  if (fullscreen_requested.has_value()) {
    config.fullscreen_requested = *fullscreen_requested;
  }

  if (config.start_url.empty()) {
    config.start_url = kChromiumHomeUrl;
  }
  if (config.ui_family.empty()) {
    config.ui_family = kDefaultUiFamily;
  }
  if (config.color_scheme != "dark") {
    config.color_scheme = "light";
  }
  return config;
}

HostPtr EnsureHost(const std::string& component_id) {
  std::lock_guard<std::mutex> lock(HostsMutex());
  HostMap& hosts = Hosts();
  auto it = hosts.find(component_id);
  if (it == hosts.end()) {
    it = hosts
             .emplace(component_id,
                      std::make_shared<OhosAuraShellHost>(component_id))
             .first;
  }
  return it->second;
}

HostPtr FindHost(const std::string& component_id) {
  std::lock_guard<std::mutex> lock(HostsMutex());
  HostMap& hosts = Hosts();
  auto it = hosts.find(component_id);
  return it == hosts.end() ? nullptr : it->second;
}

HostPtr TakeHost(const std::string& component_id) {
  std::lock_guard<std::mutex> lock(HostsMutex());
  HostMap& hosts = Hosts();
  auto node = hosts.extract(component_id);
  return node.empty() ? nullptr : std::move(node.mapped());
}

napi_value Initialize(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value args[3] = {nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  if (argc < 2) {
    return MakeUndefined(env);
  }

  const std::string component_id = ReadString(env, args[0]);
  const AuraStartupConfig config = ParseStartupConfig(ReadString(env, args[1]));
  if (argc >= 3) {
    RegisterWindowActionFunction(env, component_id, args[2]);
  }

  EnsureHost(component_id)->Initialize(config);
  return MakeUndefined(env);
}

napi_value OnSurfaceCreated(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  if (argc < 1) {
    return MakeUndefined(env);
  }

  const std::string component_id = ReadString(env, args[0]);
  EnsureHost(component_id)->OnSurfaceCreated();
  return MakeUndefined(env);
}

napi_value OnSurfaceChanged(napi_env env, napi_callback_info info) {
  size_t argc = 6;
  napi_value args[6] = {nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  if (argc < 6) {
    return MakeUndefined(env);
  }

  const std::string component_id = ReadString(env, args[0]);
  EnsureHost(component_id)
      ->OnSurfaceChanged(ReadNumber(env, args[1]), ReadNumber(env, args[2]),
                         ReadNumber(env, args[3]), ReadNumber(env, args[4]),
                         ReadNumber(env, args[5]));
  return MakeUndefined(env);
}

napi_value OnSurfaceDestroyed(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  if (argc < 1) {
    return MakeUndefined(env);
  }

  const std::string component_id = ReadString(env, args[0]);
  if (HostPtr host = FindHost(component_id)) {
    host->OnSurfaceDestroyed();
  }
  return MakeUndefined(env);
}

napi_value OnVisibilityChanged(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value args[2] = {nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  if (argc < 2) {
    return MakeUndefined(env);
  }

  const std::string component_id = ReadString(env, args[0]);
  EnsureHost(component_id)->OnVisibilityChanged(ReadBool(env, args[1]));
  return MakeUndefined(env);
}

napi_value OnFocusChanged(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value args[2] = {nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  if (argc < 2) {
    return MakeUndefined(env);
  }

  const std::string component_id = ReadString(env, args[0]);
  EnsureHost(component_id)->OnFocusChanged(ReadBool(env, args[1]));
  return MakeUndefined(env);
}

napi_value OnThemeFontChanged(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value args[2] = {nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  if (argc < 2) {
    return MakeUndefined(env);
  }

  const std::string component_id = ReadString(env, args[0]);
  const std::string font_id = ReadString(env, args[1]);
  EnsureHost(component_id)->OnThemeFontChanged(font_id);
  return MakeUndefined(env);
}

napi_value OnWindowIdChanged(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value args[2] = {nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  if (argc < 2) {
    return MakeUndefined(env);
  }

  const std::string component_id = ReadString(env, args[0]);
  const double value = ReadNumber(env, args[1]);
  if (value > 0.0 &&
      value <= static_cast<double>(std::numeric_limits<int32_t>::max())) {
    ui::SetOhosApplicationWindowIdForNativeSurface(component_id,
                                                   static_cast<int32_t>(value));
  }
  return MakeUndefined(env);
}

napi_value DispatchPointerEvent(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value args[2] = {nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  if (argc < 2) {
    return MakeUndefined(env);
  }

  const std::string component_id = ReadString(env, args[0]);
  const std::string event_json = ReadString(env, args[1]);
  EnsureHost(component_id)->DispatchPointerEvent(event_json);
  return MakeUndefined(env);
}

napi_value DispatchKeyEvent(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value args[2] = {nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  if (argc < 2) {
    return MakeBoolean(env, false);
  }

  const std::string component_id = ReadString(env, args[0]);
  const std::string event_json = ReadString(env, args[1]);
  return MakeBoolean(env,
                     EnsureHost(component_id)->DispatchKeyEvent(event_json));
}

napi_value Navigate(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value args[2] = {nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  if (argc < 2) {
    return MakeUndefined(env);
  }

  const std::string component_id = ReadString(env, args[0]);
  const std::string url = ReadString(env, args[1]);
  EnsureHost(component_id)->Navigate(url);
  return MakeUndefined(env);
}

napi_value SetBrowserEventCallback(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value args[2] = {nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  if (argc < 2) {
    return MakeUndefined(env);
  }
  RegisterBrowserEventFunction(env, ReadString(env, args[0]), args[1]);
  return MakeUndefined(env);
}

napi_value ExecuteBrowserCommand(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value args[2] = {nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  if (argc < 2) {
    return MakeBoolean(env, false);
  }

  const std::string component_id = ReadString(env, args[0]);
  const std::string command_json = ReadString(env, args[1]);
  return MakeBoolean(
      env, EnsureHost(component_id)->ExecuteBrowserCommand(command_json));
}

napi_value SetAuxiliaryWindowEventCallback(napi_env env,
                                           napi_callback_info info) {
  size_t argc = 2;
  napi_value args[2] = {nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  if (argc < 2) {
    return MakeUndefined(env);
  }
  RegisterAuxiliaryWindowEventFunction(env, ReadString(env, args[0]), args[1]);
  return MakeUndefined(env);
}

napi_value DismissAuxiliaryWindow(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value args[2] = {nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  if (argc < 2) {
    return MakeBoolean(env, false);
  }

  const double widget_value = ReadNumber(env, args[1]);
  if (widget_value <= 0.0 ||
      widget_value > static_cast<double>(
                         std::numeric_limits<gfx::AcceleratedWidget>::max())) {
    return MakeBoolean(env, false);
  }
  return MakeBoolean(env,
                     ui::RequestCloseOhosLogicalWindow(
                         static_cast<gfx::AcceleratedWidget>(widget_value)));
}

napi_value Shutdown(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  if (argc < 1) {
    return MakeUndefined(env);
  }

  const std::string component_id = ReadString(env, args[0]);
  HostPtr host = TakeHost(component_id);
  if (host) {
    host->Shutdown();
  }
  // XComponent hosts are transient during fold, rotation, and Ability window
  // recreation. Keep Chromium alive for the application process lifetime.
  ReleaseWindowActionFunction(component_id);
  ReleaseBrowserEventFunction(component_id);
  ReleaseAuxiliaryWindowEventFunction(component_id);
  return MakeUndefined(env);
}

}  // namespace

napi_value InitAuraShellNapi(napi_env env, napi_value exports) {
  RegisterNativeXComponentCallbacks(env, exports);
  napi_property_descriptor desc[] = {
      {"Initialize", nullptr, Initialize, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"OnSurfaceCreated", nullptr, OnSurfaceCreated, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"OnSurfaceChanged", nullptr, OnSurfaceChanged, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"OnSurfaceDestroyed", nullptr, OnSurfaceDestroyed, nullptr, nullptr,
       nullptr, napi_default, nullptr},
      {"OnVisibilityChanged", nullptr, OnVisibilityChanged, nullptr, nullptr,
       nullptr, napi_default, nullptr},
      {"OnFocusChanged", nullptr, OnFocusChanged, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"OnWindowIdChanged", nullptr, OnWindowIdChanged, nullptr, nullptr,
       nullptr, napi_default, nullptr},
      {"OnThemeFontChanged", nullptr, OnThemeFontChanged, nullptr, nullptr,
       nullptr, napi_default, nullptr},
      {"DispatchPointerEvent", nullptr, DispatchPointerEvent, nullptr, nullptr,
       nullptr, napi_default, nullptr},
      {"DispatchKeyEvent", nullptr, DispatchKeyEvent, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"Navigate", nullptr, Navigate, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"SetBrowserEventCallback", nullptr, SetBrowserEventCallback, nullptr,
       nullptr, nullptr, napi_default, nullptr},
      {"ExecuteBrowserCommand", nullptr, ExecuteBrowserCommand, nullptr,
       nullptr, nullptr, napi_default, nullptr},
      {"SetAuxiliaryWindowEventCallback", nullptr,
       SetAuxiliaryWindowEventCallback, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"DismissAuxiliaryWindow", nullptr, DismissAuxiliaryWindow, nullptr,
       nullptr, nullptr, napi_default, nullptr},
      {"Shutdown", nullptr, Shutdown, nullptr, nullptr, nullptr, napi_default,
       nullptr},
  };
  napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
  return exports;
}

}  // namespace ohos_nweb

// Starts Chromium without a window for embedders that only need browser
// services. Startup failures after the runtime thread starts go to hilog.
extern "C" __attribute__((visibility("default"))) bool
ChromiumHarmonyOSStartHeadless(const char* config_json) {
  if (!config_json) {
    return false;
  }
  ohos_nweb::AuraStartupConfig config =
      ohos_nweb::ParseStartupConfig(config_json);
  config.headless = true;
  return ohos_nweb::GetOhosChromeMainRunner().EnsureStarted(config);
}

EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports) {
  return ohos_nweb::InitAuraShellNapi(env, exports);
}
EXTERN_C_END

static napi_module webEngineModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "web_engine",
    .nm_priv = ((void*)0),
    .reserved = {0},
};

extern "C" __attribute__((constructor, visibility("default"))) void
RegisterWebEngineModule(void) {
  napi_module_register(&webEngineModule);
}
