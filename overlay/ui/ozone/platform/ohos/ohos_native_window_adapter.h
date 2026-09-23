#ifndef UI_OZONE_PLATFORM_OHOS_OHOS_NATIVE_WINDOW_ADAPTER_H_
#define UI_OZONE_PLATFORM_OHOS_OHOS_NATIVE_WINDOW_ADAPTER_H_

#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/native_ui_types.h"

namespace ui {

class OhosNativeWindowAdapter {
 public:
  OhosNativeWindowAdapter(const gfx::Rect& bounds,
                          bool expects_native_surface,
                          bool anchored);
  ~OhosNativeWindowAdapter();

  gfx::AcceleratedWidget GetAcceleratedWidget() const;
  void SetBounds(const gfx::Rect& bounds);
  const gfx::Rect& GetBounds() const;
  void SetVisible(bool visible);
  bool IsVisible() const;
  void SetFocused(bool focused);
  bool IsFocused() const;

 private:
  gfx::AcceleratedWidget accelerated_widget_ = gfx::kNullAcceleratedWidget;
  gfx::Rect bounds_;
  bool visible_ = false;
  bool focused_ = false;
};

}  // namespace ui

#endif  // UI_OZONE_PLATFORM_OHOS_OHOS_NATIVE_WINDOW_ADAPTER_H_
