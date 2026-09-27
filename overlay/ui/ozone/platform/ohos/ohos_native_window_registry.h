#ifndef UI_OZONE_PLATFORM_OHOS_OHOS_NATIVE_WINDOW_REGISTRY_H_
#define UI_OZONE_PLATFORM_OHOS_OHOS_NATIVE_WINDOW_REGISTRY_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "base/functional/callback.h"
#include "base/time/time.h"
#include "ui/base/dragdrop/os_exchange_data.h"
#include "ui/gfx/geometry/point.h"
#include "ui/gfx/geometry/point_f.h"
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

// What the system's drag is doing over one window. The shell sees HarmonyOS's
// drag events on the XComponent and sends them here; the platform window
// hands them to Chromium's drop handler, which is the same path Wayland and
// X11 take.
enum class OhosDragStage {
  kEnter,
  kMove,
  kLeave,
  kDrop,
  // Not the system dragging over us but our own drag, the one the page
  // started, coming to an end. `operations` carries what the thing it landed
  // on took, or none when it landed on nothing.
  kSourceFinished,
};

struct OhosDragEvent {
  OhosDragEvent();
  OhosDragEvent(OhosDragEvent&&);
  OhosDragEvent& operator=(OhosDragEvent&&);
  ~OhosDragEvent();

  OhosDragStage stage = OhosDragStage::kMove;
  // Where the finger or pointer is, in this window's coordinates, in DIP.
  gfx::PointF location;
  // What the source will allow, as ui::DragDropTypes bits.
  int operations = 0;
  // The data. Present on enter and on drop, null on the rest: HarmonyOS only
  // says what the types are until the finger lifts, so what arrives on enter
  // is a summary and what arrives on drop is the real thing.
  std::unique_ptr<OSExchangeData> data;
};

// Returns what the window will do with the drag, as a ui::DragDropTypes bit,
// so the shell can show the right badge before the finger lifts.
using OhosDragCallback =
    base::RepeatingCallback<int(OhosDragEvent event)>;

// A drag the page started, on its way out to the system.
//
// Chromium cannot start a HarmonyOS drag itself: that wants an ArkUI node on
// the ArkUI thread, and the browser runs on a thread of its own. So the app
// side starts it and reports how it ended, and the window's StartDrag blocks
// on a nested loop in between -- which is what StartDrag does on every other
// platform too.
struct OhosDragOutRequest {
  gfx::AcceleratedWidget widget = gfx::kNullAcceleratedWidget;
  std::string text;
  std::string url;
  std::string html;
  // What the page will allow, as ui::DragDropTypes bits.
  int operations = 0;
};

// Returns false when nobody is listening, and then the drag does not start.
using OhosDragOutCallback = base::RepeatingCallback<bool(OhosDragOutRequest)>;
void SetOhosDragOutCallback(OhosDragOutCallback callback);
bool StartOhosDragOut(OhosDragOutRequest request);

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
void SetOhosDragCallback(gfx::AcceleratedWidget widget,
                         OhosDragCallback callback);
// Nothing happens, and DRAG_NONE comes back, when no window has that widget
// -- a drag arriving while a window is closing, say.
int DispatchOhosDragEvent(gfx::AcceleratedWidget widget, OhosDragEvent event);
bool RequestCloseOhosLogicalWindow(gfx::AcceleratedWidget widget);

}  // namespace ui

#endif  // UI_OZONE_PLATFORM_OHOS_OHOS_NATIVE_WINDOW_REGISTRY_H_
