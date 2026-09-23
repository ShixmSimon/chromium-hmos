#ifndef UI_OZONE_PLATFORM_OHOS_OHOS_NATIVE_WINDOW_REGISTRY_H_
#define UI_OZONE_PLATFORM_OHOS_OHOS_NATIVE_WINDOW_REGISTRY_H_

#include <cstdint>
#include <optional>
#include <string>

#include "base/functional/callback.h"
#include "base/time/time.h"
#include "ui/gfx/geometry/point.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/native_ui_types.h"

namespace ui {

struct OhosNativeSurface {
  void* window = nullptr;
  gfx::Rect bounds;
  float density = 1.0f;
};

struct OhosDisplayMetrics {
  gfx::Size pixel_size;
  float density = 1.0f;
};

struct OhosLogicalWindowState {
  gfx::AcceleratedWidget widget = gfx::kNullAcceleratedWidget;
  gfx::Rect bounds;
  bool visible = false;
  bool auxiliary = false;
  // A popup, menu or tooltip that Chromium places next to page content (an
  // autofill list below a field, a <select> dropdown). The shell draws it at
  // `bounds`; other auxiliary windows it lays out itself.
  bool anchored = false;
  bool destroyed = false;
  uint64_t stacking_order = 0;
};

enum class OhosWindowAction {
  kClose,
  kEnterFullscreen,
  kExitFullscreen,
  kMaximize,
  kMinimize,
  kRestore,
  kStartMoving,
};

using OhosNativeSurfaceBoundsCallback =
    base::RepeatingCallback<void(gfx::Rect bounds, float density)>;
using OhosDisplayMetricsChangedCallback =
    base::RepeatingCallback<void(gfx::Size pixel_size, float density)>;
using OhosWindowActionCallback =
    base::RepeatingCallback<void(OhosWindowAction action)>;
using OhosLogicalWindowStateCallback =
    base::RepeatingCallback<void(const OhosLogicalWindowState& state)>;
using OhosLogicalWindowCloseCallback = base::RepeatingClosure;

void RegisterOhosNativeSurface(const std::string& component_id,
                               void* window,
                               const gfx::Rect& bounds,
                               float density);
void UpdateOhosNativeSurface(const std::string& component_id,
                             void* window,
                             const gfx::Rect& bounds,
                             float density);
void UnregisterOhosNativeSurface(const std::string& component_id, void* window);

std::optional<OhosNativeSurface> BindOhosNativeSurface(
    gfx::AcceleratedWidget widget);
void UnbindOhosNativeSurface(gfx::AcceleratedWidget widget);
std::optional<OhosNativeSurface> GetOhosNativeSurface(
    gfx::AcceleratedWidget widget);
void ExpectOhosNativeSurface(gfx::AcceleratedWidget widget);
bool IsOhosNativeSurfaceExpected(gfx::AcceleratedWidget widget);
// Whether `widget` is a popup, menu or tooltip (see OhosLogicalWindowState).
bool IsOhosAnchoredWindow(gfx::AcceleratedWidget widget);
std::optional<OhosNativeSurface> WaitForOhosNativeSurface(
    gfx::AcceleratedWidget widget,
    base::TimeDelta timeout);
gfx::AcceleratedWidget GetOhosAcceleratedWidgetForNativeSurface(
    const std::string& component_id);
std::optional<std::string> GetOhosNativeSurfaceComponentIdForWidget(
    gfx::AcceleratedWidget widget);
std::optional<OhosNativeSurface> GetPrimaryOhosNativeSurface();

// Aura menus, bubbles, and popup widgets share the application's XComponent
// compositor. They still need independent bounds and stacking state for input
// routing even though they do not own an OHNativeWindow.
void RegisterOhosLogicalWindow(gfx::AcceleratedWidget widget,
                               const gfx::Rect& bounds,
                               bool anchored = false);
void UnregisterOhosLogicalWindow(gfx::AcceleratedWidget widget);
void UpdateOhosLogicalWindowBounds(gfx::AcceleratedWidget widget,
                                   const gfx::Rect& bounds);
void SetOhosLogicalWindowVisible(gfx::AcceleratedWidget widget, bool visible);
void ActivateOhosLogicalWindow(gfx::AcceleratedWidget widget);
void DeactivateOhosLogicalWindow(gfx::AcceleratedWidget widget);
std::optional<gfx::Rect> GetOhosLogicalWindowBounds(
    gfx::AcceleratedWidget widget);
gfx::AcceleratedWidget GetOhosFocusedLogicalWindow();
bool IsOhosPrimaryLogicalWindow(gfx::AcceleratedWidget widget);
gfx::AcceleratedWidget GetOhosAcceleratedWidgetAtScreenPoint(
    const gfx::Point& point);

void SetOhosNativeSurfaceBoundsCallback(
    gfx::AcceleratedWidget widget,
    OhosNativeSurfaceBoundsCallback callback);
void UpdateOhosDisplayMetrics(const gfx::Size& pixel_size, float density);
std::optional<OhosDisplayMetrics> GetOhosDisplayMetrics();
void SetOhosDisplayMetricsChangedCallback(
    OhosDisplayMetricsChangedCallback callback);
void UpdateOhosCursorScreenPoint(const gfx::Point& point);
gfx::Point GetOhosCursorScreenPoint();
void SetOhosApplicationWindowId(int32_t window_id);
int32_t GetOhosApplicationWindowId();
void SetOhosApplicationWindowIdForNativeSurface(const std::string& component_id,
                                                int32_t window_id);
int32_t GetOhosApplicationWindowIdForWidget(gfx::AcceleratedWidget widget);
void SetOhosWindowActionCallback(const std::string& component_id,
                                 OhosWindowActionCallback callback);
bool RequestOhosWindowAction(gfx::AcceleratedWidget widget,
                             OhosWindowAction action);
void SetOhosLogicalWindowStateCallback(OhosLogicalWindowStateCallback callback);
void SetOhosLogicalWindowCloseCallback(gfx::AcceleratedWidget widget,
                                       OhosLogicalWindowCloseCallback callback);
bool RequestCloseOhosLogicalWindow(gfx::AcceleratedWidget widget);

}  // namespace ui

#endif  // UI_OZONE_PLATFORM_OHOS_OHOS_NATIVE_WINDOW_REGISTRY_H_
