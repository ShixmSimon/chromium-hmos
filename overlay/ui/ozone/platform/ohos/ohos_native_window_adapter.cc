#include "ui/ozone/platform/ohos/ohos_native_window_adapter.h"

#include <atomic>

#include "ui/ozone/platform/ohos/ohos_native_window_registry.h"

namespace ui {

namespace {

std::atomic_uint32_t g_next_widget{1u};

}  // namespace

OhosNativeWindowAdapter::OhosNativeWindowAdapter(
    const gfx::Rect& bounds,
    bool expects_native_surface,
    bool anchored)
    : accelerated_widget_(g_next_widget.fetch_add(1u)), bounds_(bounds) {
  if (std::optional<OhosNativeSurface> surface =
          BindOhosNativeSurface(accelerated_widget_)) {
    bounds_ = surface->bounds;
  } else if (expects_native_surface) {
    ExpectOhosNativeSurface(accelerated_widget_);
  }
  RegisterOhosLogicalWindow(accelerated_widget_, bounds_, anchored);
}

OhosNativeWindowAdapter::~OhosNativeWindowAdapter() {
  UnregisterOhosLogicalWindow(accelerated_widget_);
  UnbindOhosNativeSurface(accelerated_widget_);
}

gfx::AcceleratedWidget OhosNativeWindowAdapter::GetAcceleratedWidget() const {
  return accelerated_widget_;
}

void OhosNativeWindowAdapter::SetBounds(const gfx::Rect& bounds) {
  bounds_ = bounds;
  UpdateOhosLogicalWindowBounds(accelerated_widget_, bounds_);
}

const gfx::Rect& OhosNativeWindowAdapter::GetBounds() const {
  return bounds_;
}

void OhosNativeWindowAdapter::SetVisible(bool visible) {
  visible_ = visible;
  SetOhosLogicalWindowVisible(accelerated_widget_, visible_);
}

bool OhosNativeWindowAdapter::IsVisible() const {
  return visible_;
}

void OhosNativeWindowAdapter::SetFocused(bool focused) {
  focused_ = focused;
  if (focused_) {
    ActivateOhosLogicalWindow(accelerated_widget_);
  } else {
    DeactivateOhosLogicalWindow(accelerated_widget_);
  }
}

bool OhosNativeWindowAdapter::IsFocused() const {
  return focused_;
}

}  // namespace ui
