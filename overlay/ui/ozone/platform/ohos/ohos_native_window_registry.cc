#include "ui/ozone/platform/ohos/ohos_native_window_registry.h"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/synchronization/condition_variable.h"
#include "base/synchronization/lock.h"
#include "base/time/time.h"

namespace ui {
namespace {

constexpr char kAuxiliarySurfacePrefix[] = "aura_aux_";
constexpr char kPwaSurfacePrefix[] = "aura_pwa_";
// A browser window the shell hosts full screen, like the first one.
constexpr char kBrowserSurfacePrefix[] = "aura_win_";

std::optional<gfx::AcceleratedWidget> ParseTargetedSurfaceWidget(
    const std::string& component_id,
    std::string_view prefix) {
  if (!base::StartsWith(component_id, prefix, base::CompareCase::SENSITIVE)) {
    return std::nullopt;
  }

  uint64_t value = 0;
  if (!base::StringToUint64(component_id.substr(prefix.size()), &value) ||
      value == 0 ||
      value > std::numeric_limits<gfx::AcceleratedWidget>::max()) {
    return std::nullopt;
  }
  return static_cast<gfx::AcceleratedWidget>(value);
}

std::optional<gfx::AcceleratedWidget> ParseAuxiliarySurfaceWidget(
    const std::string& component_id) {
  return ParseTargetedSurfaceWidget(component_id, kAuxiliarySurfacePrefix);
}

std::optional<gfx::AcceleratedWidget> ParsePwaSurfaceWidget(
    const std::string& component_id) {
  return ParseTargetedSurfaceWidget(component_id, kPwaSurfacePrefix);
}

std::optional<gfx::AcceleratedWidget> ParseBrowserSurfaceWidget(
    const std::string& component_id) {
  return ParseTargetedSurfaceWidget(component_id, kBrowserSurfacePrefix);
}

struct SurfaceRecord {
  OhosNativeSurface surface;
  gfx::AcceleratedWidget widget = gfx::kNullAcceleratedWidget;
};

struct LogicalWindowRecord {
  gfx::Rect bounds;
  bool visible = false;
  bool auxiliary = false;
  bool anchored = false;
  uint64_t stacking_order = 0;
};

OhosLogicalWindowState MakeLogicalWindowState(gfx::AcceleratedWidget widget,
                                              const LogicalWindowRecord& record,
                                              bool destroyed = false) {
  return {
      .widget = widget,
      .bounds = record.bounds,
      .visible = record.visible,
      .auxiliary = record.auxiliary,
      .anchored = record.anchored,
      .destroyed = destroyed,
      .stacking_order = record.stacking_order,
  };
}

class NativeWindowRegistry {
 public:
  NativeWindowRegistry() : surface_available_(&lock_) {}

  void Register(const std::string& component_id,
                void* window,
                const gfx::Rect& bounds,
                float density) {
    OhosLogicalWindowStateCallback callback;
    OhosNativeSurfaceBoundsCallback bounds_callback;
    std::optional<OhosLogicalWindowState> promoted_state;
    std::optional<OhosNativeSurface> bound_surface;
    {
      base::AutoLock lock(lock_);
      SurfaceRecord& record = surfaces_[component_id];
      record.surface = {window, bounds, density};
      if (record.widget != gfx::kNullAcceleratedWidget) {
        auto bounds_it = bounds_callbacks_.find(record.widget);
        if (bounds_it != bounds_callbacks_.end()) {
          bounds_callback = bounds_it->second;
          bound_surface = record.surface;
        }
      } else {
        const std::optional<gfx::AcceleratedWidget> pwa_widget =
            ParsePwaSurfaceWidget(component_id);
        const std::optional<gfx::AcceleratedWidget> browser_widget =
            pwa_widget ? std::nullopt
                       : ParseBrowserSurfaceWidget(component_id);
        // A window the shell hosts in a surface of its own is not auxiliary,
        // whichever kind it is: leaving it marked so routes input and stacking
        // as though it were a popup over the first window.
        const std::optional<gfx::AcceleratedWidget> own_surface_widget =
            pwa_widget ? pwa_widget : browser_widget;
        const std::optional<gfx::AcceleratedWidget> requested_widget =
            own_surface_widget ? own_surface_widget
                               : ParseAuxiliarySurfaceWidget(component_id);
        if (requested_widget) {
          auto pending =
              std::ranges::find(pending_widgets_, *requested_widget);
          auto logical = logical_windows_.find(*requested_widget);
          if (pending != pending_widgets_.end() &&
              logical != logical_windows_.end() &&
              logical->second.auxiliary) {
            record.widget = *requested_widget;
            pending_widgets_.erase(pending);
            widget_bindings_[record.widget] = component_id;
            if (own_surface_widget) {
              promoted_state =
                  MakeLogicalWindowState(record.widget, logical->second, true);
              logical->second.auxiliary = false;
              callback = logical_window_state_callback_;
            }
          }
        } else if (!pending_widgets_.empty()) {
          record.widget = pending_widgets_.front();
          pending_widgets_.erase(pending_widgets_.begin());
          widget_bindings_[record.widget] = component_id;
        }
        if (record.widget != gfx::kNullAcceleratedWidget) {
          auto bounds_it = bounds_callbacks_.find(record.widget);
          if (bounds_it != bounds_callbacks_.end()) {
            bounds_callback = bounds_it->second;
            bound_surface = record.surface;
          }
        }
      }
      surface_available_.Broadcast();
    }
    if (bounds_callback && bound_surface) {
      bounds_callback.Run(bound_surface->bounds, bound_surface->density);
    }
    if (callback && promoted_state) {
      callback.Run(*promoted_state);
    }
  }

  void Update(const std::string& component_id,
              void* window,
              const gfx::Rect& bounds,
              float density) {
    OhosNativeSurfaceBoundsCallback callback;
    {
      base::AutoLock lock(lock_);
      auto it = surfaces_.find(component_id);
      if (it == surfaces_.end()) {
        return;
      }
      if (window && it->second.surface.window != window) {
        return;
      }
      it->second.surface.bounds = bounds;
      it->second.surface.density = density;
      auto callback_it = bounds_callbacks_.find(it->second.widget);
      if (callback_it != bounds_callbacks_.end()) {
        callback = callback_it->second;
      }
    }
    if (callback) {
      callback.Run(bounds, density);
    }
  }

  void Unregister(const std::string& component_id, void* window) {
    base::AutoLock lock(lock_);
    auto it = surfaces_.find(component_id);
    if (it == surfaces_.end() ||
        (window && it->second.surface.window != window)) {
      return;
    }
    if (it->second.widget != gfx::kNullAcceleratedWidget) {
      // XComponent surfaces can be destroyed and recreated while their
      // Chromium PlatformWindow remains alive. Preserve that binding so a
      // pending popup widget cannot steal the replacement surface.
      it->second.surface.window = nullptr;
      it->second.surface.bounds = gfx::Rect();
      return;
    }
    surfaces_.erase(it);
  }

  std::optional<OhosNativeSurface> Bind(gfx::AcceleratedWidget widget) {
    base::AutoLock lock(lock_);
    auto binding = widget_bindings_.find(widget);
    if (binding != widget_bindings_.end()) {
      auto surface = surfaces_.find(binding->second);
      if (surface != surfaces_.end()) {
        return surface->second.surface;
      }
    }

    for (auto& [component_id, record] : surfaces_) {
      if (record.widget == gfx::kNullAcceleratedWidget &&
          record.surface.window) {
        record.widget = widget;
        widget_bindings_[widget] = component_id;
        return record.surface;
      }
    }

    if (!std::ranges::contains(pending_widgets_, widget)) {
      pending_widgets_.push_back(widget);
    }
    return std::nullopt;
  }

  void Unbind(gfx::AcceleratedWidget widget) {
    base::AutoLock lock(lock_);
    auto binding = widget_bindings_.find(widget);
    if (binding != widget_bindings_.end()) {
      auto surface = surfaces_.find(binding->second);
      if (surface != surfaces_.end()) {
        surface->second.widget = gfx::kNullAcceleratedWidget;
        if (!surface->second.surface.window) {
          surfaces_.erase(surface);
        }
      }
      widget_bindings_.erase(binding);
    }
    std::erase(pending_widgets_, widget);
    expected_native_surfaces_.erase(widget);
    bounds_callbacks_.erase(widget);
  }

  std::optional<OhosNativeSurface> Get(gfx::AcceleratedWidget widget) {
    base::AutoLock lock(lock_);
    auto binding = widget_bindings_.find(widget);
    if (binding == widget_bindings_.end()) {
      return std::nullopt;
    }
    auto surface = surfaces_.find(binding->second);
    if (surface == surfaces_.end()) {
      return std::nullopt;
    }
    return surface->second.surface;
  }

  void ExpectSurface(gfx::AcceleratedWidget widget) {
    if (widget == gfx::kNullAcceleratedWidget) {
      return;
    }
    base::AutoLock lock(lock_);
    expected_native_surfaces_.insert(widget);
  }

  bool IsSurfaceExpected(gfx::AcceleratedWidget widget) {
    base::AutoLock lock(lock_);
    return expected_native_surfaces_.contains(widget);
  }

  bool IsAnchored(gfx::AcceleratedWidget widget) {
    base::AutoLock lock(lock_);
    auto it = logical_windows_.find(widget);
    return it != logical_windows_.end() && it->second.anchored;
  }

  std::optional<OhosNativeSurface> WaitForSurface(
      gfx::AcceleratedWidget widget,
      base::TimeDelta timeout) {
    base::AutoLock lock(lock_);
    if (!expected_native_surfaces_.contains(widget)) {
      return std::nullopt;
    }

    const base::TimeTicks deadline = base::TimeTicks::Now() + timeout;
    while (true) {
      auto binding = widget_bindings_.find(widget);
      if (binding != widget_bindings_.end()) {
        auto surface = surfaces_.find(binding->second);
        if (surface != surfaces_.end() && surface->second.surface.window) {
          return surface->second.surface;
        }
      }
      const base::TimeDelta remaining = deadline - base::TimeTicks::Now();
      if (remaining <= base::TimeDelta()) {
        return std::nullopt;
      }
      surface_available_.TimedWait(remaining);
    }
  }

  gfx::AcceleratedWidget GetWidgetForComponent(
      const std::string& component_id) {
    base::AutoLock lock(lock_);
    auto surface = surfaces_.find(component_id);
    return surface == surfaces_.end() ? gfx::kNullAcceleratedWidget
                                      : surface->second.widget;
  }

  std::optional<std::string> GetComponentForWidget(
      gfx::AcceleratedWidget widget) {
    base::AutoLock lock(lock_);
    auto binding = widget_bindings_.find(widget);
    if (binding == widget_bindings_.end()) {
      return std::nullopt;
    }
    return binding->second;
  }

  std::optional<OhosNativeSurface> GetPrimary() {
    base::AutoLock lock(lock_);
    auto main_surface = surfaces_.find("aura_shell");
    if (main_surface != surfaces_.end() &&
        main_surface->second.surface.window) {
      return main_surface->second.surface;
    }
    std::optional<OhosNativeSurface> fallback;
    for (const auto& entry : surfaces_) {
      const SurfaceRecord& record = entry.second;
      if (!record.surface.window) {
        continue;
      }
      if (!fallback) {
        fallback = record.surface;
      }
      auto logical = logical_windows_.find(record.widget);
      if (logical == logical_windows_.end() || !logical->second.auxiliary) {
        return record.surface;
      }
    }
    return fallback;
  }

  void RegisterLogicalWindow(gfx::AcceleratedWidget widget,
                             const gfx::Rect& bounds,
                             bool anchored) {
    if (widget == gfx::kNullAcceleratedWidget) {
      return;
    }
    base::AutoLock lock(lock_);
    logical_windows_[widget] = {
        .bounds = bounds,
        .visible = false,
        .auxiliary = !widget_bindings_.contains(widget),
        .anchored = anchored,
        .stacking_order = ++next_stacking_order_,
    };
  }

  void UnregisterLogicalWindow(gfx::AcceleratedWidget widget) {
    OhosLogicalWindowStateCallback callback;
    std::optional<OhosLogicalWindowState> state;
    {
      base::AutoLock lock(lock_);
      auto it = logical_windows_.find(widget);
      if (it != logical_windows_.end() && it->second.auxiliary) {
        state = MakeLogicalWindowState(widget, it->second, true);
        callback = logical_window_state_callback_;
      }
      logical_windows_.erase(widget);
      logical_window_close_callbacks_.erase(widget);
      if (focused_widget_ == widget) {
        focused_widget_ = gfx::kNullAcceleratedWidget;
      }
    }
    if (callback && state) {
      callback.Run(*state);
    }
  }

  void UpdateLogicalWindowBounds(gfx::AcceleratedWidget widget,
                                 const gfx::Rect& bounds) {
    OhosLogicalWindowStateCallback callback;
    std::optional<OhosLogicalWindowState> state;
    {
      base::AutoLock lock(lock_);
      auto it = logical_windows_.find(widget);
      if (it != logical_windows_.end()) {
        it->second.bounds = bounds;
        if (it->second.auxiliary && it->second.visible) {
          state = MakeLogicalWindowState(widget, it->second);
          callback = logical_window_state_callback_;
        }
      }
    }
    if (callback && state) {
      callback.Run(*state);
    }
  }

  void SetLogicalWindowVisible(gfx::AcceleratedWidget widget, bool visible) {
    OhosLogicalWindowStateCallback callback;
    std::optional<OhosLogicalWindowState> state;
    {
      base::AutoLock lock(lock_);
      auto it = logical_windows_.find(widget);
      if (it == logical_windows_.end()) {
        return;
      }
      it->second.visible = visible;
      if (visible) {
        it->second.stacking_order = ++next_stacking_order_;
      } else if (focused_widget_ == widget) {
        focused_widget_ = gfx::kNullAcceleratedWidget;
      }
      if (it->second.auxiliary) {
        state = MakeLogicalWindowState(widget, it->second);
        callback = logical_window_state_callback_;
      }
    }
    if (callback && state) {
      callback.Run(*state);
    }
  }

  void ActivateLogicalWindow(gfx::AcceleratedWidget widget) {
    OhosLogicalWindowStateCallback callback;
    std::optional<OhosLogicalWindowState> state;
    {
      base::AutoLock lock(lock_);
      auto it = logical_windows_.find(widget);
      if (it == logical_windows_.end()) {
        return;
      }
      focused_widget_ = widget;
      it->second.stacking_order = ++next_stacking_order_;
      if (it->second.auxiliary && it->second.visible) {
        state = MakeLogicalWindowState(widget, it->second);
        callback = logical_window_state_callback_;
      }
    }
    if (callback && state) {
      callback.Run(*state);
    }
  }

  void DeactivateLogicalWindow(gfx::AcceleratedWidget widget) {
    base::AutoLock lock(lock_);
    if (focused_widget_ == widget) {
      focused_widget_ = gfx::kNullAcceleratedWidget;
    }
  }

  std::optional<gfx::Rect> GetLogicalWindowBounds(
      gfx::AcceleratedWidget widget) {
    base::AutoLock lock(lock_);
    auto it = logical_windows_.find(widget);
    if (it == logical_windows_.end()) {
      return std::nullopt;
    }
    return it->second.bounds;
  }

  gfx::AcceleratedWidget GetFocusedLogicalWindow() {
    base::AutoLock lock(lock_);
    return focused_widget_;
  }

  bool IsPrimaryLogicalWindow(gfx::AcceleratedWidget widget) {
    base::AutoLock lock(lock_);
    auto binding = widget_bindings_.find(widget);
    if (binding == widget_bindings_.end()) {
      return false;
    }
    auto surface = surfaces_.find(binding->second);
    return surface != surfaces_.end() && surface->second.surface.window &&
           !ParseAuxiliarySurfaceWidget(binding->second).has_value();
  }

  gfx::AcceleratedWidget GetWidgetAtScreenPoint(const gfx::Point& point) {
    base::AutoLock lock(lock_);
    // Ranked by tier first, then by stacking order. An auxiliary window's
    // XComponent lives in an ArkUI overlay drawn above the browser's own, so
    // it is on top wherever the two overlap -- whatever the activation history
    // says. Stacking order alone got that wrong: it is bumped every time a
    // window is activated, the browser window is activated far more often
    // than a status bar that never takes focus, and a press on the screen
    // share bar's Stop button was handed to the browser underneath instead.
    gfx::AcceleratedWidget target = gfx::kNullAcceleratedWidget;
    std::pair<bool, uint64_t> target_rank{false, 0};
    for (const auto& [widget, record] : logical_windows_) {
      if (!record.visible || record.bounds.IsEmpty() ||
          !record.bounds.Contains(point)) {
        continue;
      }
      const std::pair<bool, uint64_t> rank{record.auxiliary,
                                           record.stacking_order};
      if (rank >= target_rank) {
        target = widget;
        target_rank = rank;
      }
    }
    if (target != gfx::kNullAcceleratedWidget) {
      return target;
    }

    // Keep surface-only callers working during early startup before the
    // corresponding PlatformWindow has registered its logical bounds.
    for (const auto& [component_id, record] : surfaces_) {
      if (record.widget != gfx::kNullAcceleratedWidget &&
          record.surface.bounds.Contains(point)) {
        return record.widget;
      }
    }
    return gfx::kNullAcceleratedWidget;
  }

  void SetBoundsCallback(gfx::AcceleratedWidget widget,
                         OhosNativeSurfaceBoundsCallback callback) {
    std::optional<OhosNativeSurface> surface;
    {
      base::AutoLock lock(lock_);
      if (callback) {
        bounds_callbacks_[widget] = callback;
        auto binding = widget_bindings_.find(widget);
        if (binding != widget_bindings_.end()) {
          auto surface_it = surfaces_.find(binding->second);
          if (surface_it != surfaces_.end()) {
            surface = surface_it->second.surface;
          }
        }
      } else {
        bounds_callbacks_.erase(widget);
      }
    }
    if (callback && surface) {
      callback.Run(surface->bounds, surface->density);
    }
  }

  void UpdateDisplayMetrics(const gfx::Size& pixel_size, float density) {
    if (pixel_size.IsEmpty() || density <= 0.0f) {
      return;
    }

    OhosDisplayMetricsChangedCallback callback;
    {
      base::AutoLock lock(lock_);
      display_metrics_ = OhosDisplayMetrics{pixel_size, density};
      callback = display_metrics_changed_callback_;
    }
    if (callback) {
      callback.Run(pixel_size, density);
    }
  }

  std::optional<OhosDisplayMetrics> GetDisplayMetrics() {
    base::AutoLock lock(lock_);
    return display_metrics_;
  }

  void SetDisplayMetricsChangedCallback(
      OhosDisplayMetricsChangedCallback callback) {
    std::optional<OhosDisplayMetrics> metrics;
    {
      base::AutoLock lock(lock_);
      display_metrics_changed_callback_ = callback;
      metrics = display_metrics_;
    }
    if (callback && metrics) {
      callback.Run(metrics->pixel_size, metrics->density);
    }
  }

  void UpdateCursorScreenPoint(const gfx::Point& point) {
    base::AutoLock lock(lock_);
    cursor_screen_point_ = point;
  }

  gfx::Point GetCursorScreenPoint() {
    base::AutoLock lock(lock_);
    return cursor_screen_point_;
  }

  void SetApplicationWindowId(int32_t window_id) {
    if (window_id <= 0) {
      return;
    }
    base::AutoLock lock(lock_);
    application_window_id_ = window_id;
  }

  int32_t GetApplicationWindowId() {
    base::AutoLock lock(lock_);
    return application_window_id_;
  }

  void SetApplicationWindowIdForComponent(const std::string& component_id,
                                          int32_t window_id) {
    if (component_id.empty() || window_id <= 0) {
      return;
    }
    base::AutoLock lock(lock_);
    application_window_ids_[component_id] = window_id;
    if (component_id == "aura_shell" || application_window_id_ <= 0) {
      application_window_id_ = window_id;
    }
  }

  int32_t GetApplicationWindowIdForWidget(gfx::AcceleratedWidget widget) {
    base::AutoLock lock(lock_);
    auto binding = widget_bindings_.find(widget);
    if (binding == widget_bindings_.end()) {
      return application_window_id_;
    }
    auto window_id = application_window_ids_.find(binding->second);
    return window_id == application_window_ids_.end() ? application_window_id_
                                                      : window_id->second;
  }

  void SetWindowActionCallback(const std::string& component_id,
                               OhosWindowActionCallback callback) {
    base::AutoLock lock(lock_);
    if (callback) {
      window_action_callbacks_[component_id] = std::move(callback);
    } else {
      window_action_callbacks_.erase(component_id);
    }
  }

  bool RequestWindowAction(gfx::AcceleratedWidget widget,
                           OhosWindowAction action) {
    OhosWindowActionCallback callback;
    {
      base::AutoLock lock(lock_);
      auto binding = widget_bindings_.find(widget);
      if (binding == widget_bindings_.end()) {
        return false;
      }
      auto callback_it = window_action_callbacks_.find(binding->second);
      if (callback_it == window_action_callbacks_.end()) {
        return false;
      }
      callback = callback_it->second;
    }
    callback.Run(action);
    return true;
  }

  void SetLogicalWindowStateCallback(OhosLogicalWindowStateCallback callback) {
    std::vector<OhosLogicalWindowState> visible_windows;
    {
      base::AutoLock lock(lock_);
      logical_window_state_callback_ = callback;
      if (callback) {
        for (const auto& [widget, record] : logical_windows_) {
          if (record.auxiliary && record.visible) {
            visible_windows.push_back(MakeLogicalWindowState(widget, record));
          }
        }
      }
    }
    if (callback) {
      for (const OhosLogicalWindowState& state : visible_windows) {
        callback.Run(state);
      }
    }
  }

  void SetLogicalWindowCloseCallback(gfx::AcceleratedWidget widget,
                                     OhosLogicalWindowCloseCallback callback) {
    base::AutoLock lock(lock_);
    if (callback) {
      logical_window_close_callbacks_[widget] = std::move(callback);
    } else {
      logical_window_close_callbacks_.erase(widget);
    }
  }

  bool RequestCloseLogicalWindow(gfx::AcceleratedWidget widget) {
    OhosLogicalWindowCloseCallback callback;
    {
      base::AutoLock lock(lock_);
      auto logical = logical_windows_.find(widget);
      auto close_callback = logical_window_close_callbacks_.find(widget);
      if (logical == logical_windows_.end() || !logical->second.auxiliary ||
          close_callback == logical_window_close_callbacks_.end()) {
        return false;
      }
      callback = close_callback->second;
    }
    callback.Run();
    return true;
  }

 private:
  base::Lock lock_;
  base::ConditionVariable surface_available_;
  std::map<std::string, SurfaceRecord> surfaces_ GUARDED_BY(lock_);
  std::map<gfx::AcceleratedWidget, std::string> widget_bindings_
      GUARDED_BY(lock_);
  std::vector<gfx::AcceleratedWidget> pending_widgets_ GUARDED_BY(lock_);
  std::set<gfx::AcceleratedWidget> expected_native_surfaces_
      GUARDED_BY(lock_);
  std::map<gfx::AcceleratedWidget, OhosNativeSurfaceBoundsCallback>
      bounds_callbacks_ GUARDED_BY(lock_);
  std::map<gfx::AcceleratedWidget, LogicalWindowRecord> logical_windows_
      GUARDED_BY(lock_);
  gfx::AcceleratedWidget focused_widget_ GUARDED_BY(lock_) =
      gfx::kNullAcceleratedWidget;
  uint64_t next_stacking_order_ GUARDED_BY(lock_) = 0;
  std::optional<OhosDisplayMetrics> display_metrics_ GUARDED_BY(lock_);
  OhosDisplayMetricsChangedCallback display_metrics_changed_callback_
      GUARDED_BY(lock_);
  gfx::Point cursor_screen_point_ GUARDED_BY(lock_);
  int32_t application_window_id_ GUARDED_BY(lock_) = 0;
  std::map<std::string, int32_t> application_window_ids_ GUARDED_BY(lock_);
  std::map<std::string, OhosWindowActionCallback> window_action_callbacks_
      GUARDED_BY(lock_);
  OhosLogicalWindowStateCallback logical_window_state_callback_
      GUARDED_BY(lock_);
  std::map<gfx::AcceleratedWidget, OhosLogicalWindowCloseCallback>
      logical_window_close_callbacks_ GUARDED_BY(lock_);
};

NativeWindowRegistry& GetRegistry() {
  static base::NoDestructor<NativeWindowRegistry> registry;
  return *registry;
}

}  // namespace

void RegisterOhosNativeSurface(const std::string& component_id,
                               void* window,
                               const gfx::Rect& bounds,
                               float density) {
  GetRegistry().Register(component_id, window, bounds, density);
}

void UpdateOhosNativeSurface(const std::string& component_id,
                             void* window,
                             const gfx::Rect& bounds,
                             float density) {
  GetRegistry().Update(component_id, window, bounds, density);
}

void UnregisterOhosNativeSurface(const std::string& component_id,
                                 void* window) {
  GetRegistry().Unregister(component_id, window);
}

std::optional<OhosNativeSurface> BindOhosNativeSurface(
    gfx::AcceleratedWidget widget) {
  return GetRegistry().Bind(widget);
}

void UnbindOhosNativeSurface(gfx::AcceleratedWidget widget) {
  GetRegistry().Unbind(widget);
}

std::optional<OhosNativeSurface> GetOhosNativeSurface(
    gfx::AcceleratedWidget widget) {
  return GetRegistry().Get(widget);
}

void ExpectOhosNativeSurface(gfx::AcceleratedWidget widget) {
  GetRegistry().ExpectSurface(widget);
}

bool IsOhosNativeSurfaceExpected(gfx::AcceleratedWidget widget) {
  return GetRegistry().IsSurfaceExpected(widget);
}

bool IsOhosAnchoredWindow(gfx::AcceleratedWidget widget) {
  return GetRegistry().IsAnchored(widget);
}

std::optional<OhosNativeSurface> WaitForOhosNativeSurface(
    gfx::AcceleratedWidget widget,
    base::TimeDelta timeout) {
  return GetRegistry().WaitForSurface(widget, timeout);
}

gfx::AcceleratedWidget GetOhosAcceleratedWidgetForNativeSurface(
    const std::string& component_id) {
  return GetRegistry().GetWidgetForComponent(component_id);
}

std::optional<std::string> GetOhosNativeSurfaceComponentIdForWidget(
    gfx::AcceleratedWidget widget) {
  return GetRegistry().GetComponentForWidget(widget);
}

std::optional<OhosNativeSurface> GetPrimaryOhosNativeSurface() {
  return GetRegistry().GetPrimary();
}

void RegisterOhosLogicalWindow(gfx::AcceleratedWidget widget,
                               const gfx::Rect& bounds,
                               bool anchored) {
  GetRegistry().RegisterLogicalWindow(widget, bounds, anchored);
}

void UnregisterOhosLogicalWindow(gfx::AcceleratedWidget widget) {
  GetRegistry().UnregisterLogicalWindow(widget);
}

void UpdateOhosLogicalWindowBounds(gfx::AcceleratedWidget widget,
                                   const gfx::Rect& bounds) {
  GetRegistry().UpdateLogicalWindowBounds(widget, bounds);
}

void SetOhosLogicalWindowVisible(gfx::AcceleratedWidget widget, bool visible) {
  GetRegistry().SetLogicalWindowVisible(widget, visible);
}

void ActivateOhosLogicalWindow(gfx::AcceleratedWidget widget) {
  GetRegistry().ActivateLogicalWindow(widget);
}

void DeactivateOhosLogicalWindow(gfx::AcceleratedWidget widget) {
  GetRegistry().DeactivateLogicalWindow(widget);
}

std::optional<gfx::Rect> GetOhosLogicalWindowBounds(
    gfx::AcceleratedWidget widget) {
  return GetRegistry().GetLogicalWindowBounds(widget);
}

gfx::AcceleratedWidget GetOhosFocusedLogicalWindow() {
  return GetRegistry().GetFocusedLogicalWindow();
}

bool IsOhosPrimaryLogicalWindow(gfx::AcceleratedWidget widget) {
  return GetRegistry().IsPrimaryLogicalWindow(widget);
}

gfx::AcceleratedWidget GetOhosAcceleratedWidgetAtScreenPoint(
    const gfx::Point& point) {
  return GetRegistry().GetWidgetAtScreenPoint(point);
}

void SetOhosNativeSurfaceBoundsCallback(
    gfx::AcceleratedWidget widget,
    OhosNativeSurfaceBoundsCallback callback) {
  GetRegistry().SetBoundsCallback(widget, std::move(callback));
}

void UpdateOhosDisplayMetrics(const gfx::Size& pixel_size, float density) {
  GetRegistry().UpdateDisplayMetrics(pixel_size, density);
}

std::optional<OhosDisplayMetrics> GetOhosDisplayMetrics() {
  return GetRegistry().GetDisplayMetrics();
}

void SetOhosDisplayMetricsChangedCallback(
    OhosDisplayMetricsChangedCallback callback) {
  GetRegistry().SetDisplayMetricsChangedCallback(std::move(callback));
}

void UpdateOhosCursorScreenPoint(const gfx::Point& point) {
  GetRegistry().UpdateCursorScreenPoint(point);
}

gfx::Point GetOhosCursorScreenPoint() {
  return GetRegistry().GetCursorScreenPoint();
}

void SetOhosApplicationWindowId(int32_t window_id) {
  GetRegistry().SetApplicationWindowId(window_id);
}

int32_t GetOhosApplicationWindowId() {
  return GetRegistry().GetApplicationWindowId();
}

void SetOhosApplicationWindowIdForNativeSurface(const std::string& component_id,
                                                int32_t window_id) {
  GetRegistry().SetApplicationWindowIdForComponent(component_id, window_id);
}

int32_t GetOhosApplicationWindowIdForWidget(gfx::AcceleratedWidget widget) {
  return GetRegistry().GetApplicationWindowIdForWidget(widget);
}

void SetOhosWindowActionCallback(const std::string& component_id,
                                 OhosWindowActionCallback callback) {
  GetRegistry().SetWindowActionCallback(component_id, std::move(callback));
}

bool RequestOhosWindowAction(gfx::AcceleratedWidget widget,
                             OhosWindowAction action) {
  return GetRegistry().RequestWindowAction(widget, action);
}

void SetOhosLogicalWindowStateCallback(
    OhosLogicalWindowStateCallback callback) {
  GetRegistry().SetLogicalWindowStateCallback(std::move(callback));
}

void SetOhosLogicalWindowCloseCallback(
    gfx::AcceleratedWidget widget,
    OhosLogicalWindowCloseCallback callback) {
  GetRegistry().SetLogicalWindowCloseCallback(widget, std::move(callback));
}

bool RequestCloseOhosLogicalWindow(gfx::AcceleratedWidget widget) {
  return GetRegistry().RequestCloseLogicalWindow(widget);
}

}  // namespace ui
