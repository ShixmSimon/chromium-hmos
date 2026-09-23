#ifndef UI_OZONE_PLATFORM_OHOS_OHOS_PLATFORM_WINDOW_H_
#define UI_OZONE_PLATFORM_OHOS_OHOS_PLATFORM_WINDOW_H_

#include "base/memory/weak_ptr.h"
#include "ui/events/platform/platform_event_dispatcher.h"
#include "ui/ozone/platform/ohos/ohos_native_window_adapter.h"
#include "ui/platform_window/stub/stub_window.h"
#include "ui/platform_window/wm/wm_move_loop_handler.h"

namespace ui {

class OhosPlatformWindow : public StubWindow,
                           public PlatformEventDispatcher,
                           public WmMoveLoopHandler {
 public:
  OhosPlatformWindow(PlatformWindowDelegate* delegate,
                     const gfx::Rect& bounds,
                     bool expects_native_surface,
                     bool anchored);
  ~OhosPlatformWindow() override;

  void Show(bool inactive) override;
  void Hide() override;
  void Close() override;
  bool IsVisible() const override;
  void SetBoundsInPixels(const gfx::Rect& bounds) override;
  gfx::Rect GetBoundsInPixels() const override;
  void SetBoundsInDIP(const gfx::Rect& bounds) override;
  gfx::Rect GetBoundsInDIP() const override;
  void Activate() override;
  void Deactivate() override;
  void SetFullscreen(bool fullscreen, int64_t target_display_id) override;
  void Maximize() override;
  void Minimize() override;
  void Restore() override;
  PlatformWindowState GetPlatformWindowState() const override;

  bool CanDispatchEvent(const PlatformEvent& event) override;
  uint32_t DispatchEvent(const PlatformEvent& event) override;

  bool RunMoveLoop(const gfx::Vector2d& drag_offset) override;
  void EndMoveLoop() override;

 private:
  void OnLogicalWindowCloseRequest();
  void OnNativeSurfaceBoundsChanged(gfx::Rect bounds, float density);
  void SetWindowState(PlatformWindowState state);

  OhosNativeWindowAdapter adapter_;
  // A popup, menu or tooltip: Chromium anchors it to page content, and the
  // shell hosts it in an XComponent of its own.
  const bool anchored_;
  PlatformWindowState window_state_ = PlatformWindowState::kNormal;
  base::WeakPtrFactory<OhosPlatformWindow> weak_factory_{this};
};

}  // namespace ui

#endif  // UI_OZONE_PLATFORM_OHOS_OHOS_PLATFORM_WINDOW_H_
