// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/views/frame/browser_desktop_window_tree_host.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/frame/browser_widget.h"
#include "base/memory/raw_ptr.h"
#include "ui/aura/window.h"
#include "ui/base/hit_test.h"
#include "ui/events/event.h"
#include "ui/events/event_handler.h"
#include "ui/gfx/geometry/point_conversions.h"
#include "ui/ozone/platform/ohos/ohos_native_window_registry.h"
#include "ui/platform_window/wm/wm_move_resize_handler.h"
#include "ui/views/widget/desktop_aura/desktop_native_widget_aura.h"
#include "ui/views/widget/desktop_aura/desktop_window_tree_host_platform.h"
#include "ui/views/window/non_client_view.h"

namespace {

class BrowserDesktopWindowTreeHostOhos
    : public views::DesktopWindowTreeHostPlatform,
      public BrowserDesktopWindowTreeHost,
      public ui::EventHandler {
 public:
  BrowserDesktopWindowTreeHostOhos(
      views::internal::NativeWidgetDelegate* native_widget_delegate,
      views::DesktopNativeWidgetAura* desktop_native_widget_aura,
      BrowserView* browser_view,
      BrowserWidget* browser_widget)
      : DesktopWindowTreeHostPlatform(native_widget_delegate,
                                      desktop_native_widget_aura),
        browser_widget_(browser_widget),
        web_popup_(browser_view->browser()->GetType() ==
                   BrowserWindowInterface::Type::TYPE_POPUP) {}

  ~BrowserDesktopWindowTreeHostOhos() override {
    if (handler_installed_ && window()) {
      window()->RemovePreTargetHandler(this);
    }
  }

  void Init(const views::Widget::InitParams& params) override {
    DesktopWindowTreeHostPlatform::Init(params);
    if (web_popup_) {
      window()->AddPreTargetHandler(this);
      handler_installed_ = true;
    }
  }

  void OnMouseEvent(ui::MouseEvent* event) override {
    if (event->type() == ui::EventType::kMousePressed &&
        event->IsOnlyLeftMouseButton()) {
      HandleCaptionPress(event);
    }
  }

  void OnTouchEvent(ui::TouchEvent* event) override {
    if (event->type() == ui::EventType::kTouchPressed) {
      HandleCaptionPress(event);
    }
  }

  BrowserDesktopWindowTreeHostOhos(const BrowserDesktopWindowTreeHostOhos&) =
      delete;
  BrowserDesktopWindowTreeHostOhos& operator=(
      const BrowserDesktopWindowTreeHostOhos&) = delete;

  views::DesktopWindowTreeHost* AsDesktopWindowTreeHost() override {
    return this;
  }

  bool UsesNativeSystemMenu() const override { return false; }

 private:
  void HandleCaptionPress(ui::LocatedEvent* event) {
    if (!browser_widget_->non_client_view() || !platform_window()) {
      return;
    }
    const int component = browser_widget_->non_client_view()->NonClientHitTest(
        gfx::ToRoundedPoint(event->root_location_f()));
    if (!ui::CanPerformDragOrResize(component)) {
      return;  // Close buttons and page content keep their normal Views input.
    }
    auto* handler = ui::GetWmMoveResizeHandler(*platform_window());
    if (!handler) {
      return;
    }
    event->SetHandled();
    event->StopPropagation();
    handler->DispatchHostWindowDragMovement(
        component, ui::GetOhosCursorScreenPoint());
  }

  const raw_ptr<BrowserWidget> browser_widget_;
  const bool web_popup_;
  bool handler_installed_ = false;
};

}  // namespace

BrowserDesktopWindowTreeHost*
BrowserDesktopWindowTreeHost::CreateBrowserDesktopWindowTreeHost(
    views::internal::NativeWidgetDelegate* native_widget_delegate,
    views::DesktopNativeWidgetAura* desktop_native_widget_aura,
    BrowserView* browser_view,
    BrowserWidget* browser_widget) {
  return new BrowserDesktopWindowTreeHostOhos(native_widget_delegate,
                                              desktop_native_widget_aura,
                                              browser_view, browser_widget);
}
