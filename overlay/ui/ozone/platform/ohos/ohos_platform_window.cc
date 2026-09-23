#include "ui/ozone/platform/ohos/ohos_platform_window.h"

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/task/single_thread_task_runner.h"
#include "ui/events/event.h"
#include "ui/events/platform/platform_event_source.h"
#include "ui/ozone/platform/ohos/ohos_event_source.h"
#include "ui/ozone/platform/ohos/ohos_native_window_registry.h"
#include "ui/platform_window/platform_window_delegate.h"
#include "ui/platform_window/wm/wm_move_loop_handler.h"

namespace ui {

OhosPlatformWindow::OhosPlatformWindow(PlatformWindowDelegate* delegate,
                                       const gfx::Rect& bounds,
                                       bool expects_native_surface)
    : StubWindow(delegate, false, bounds),
      adapter_(bounds, expects_native_surface) {
  SetWmMoveLoopHandler(this, this);
  delegate->OnAcceleratedWidgetAvailable(adapter_.GetAcceleratedWidget());
  SetOhosNativeSurfaceBoundsCallback(
      adapter_.GetAcceleratedWidget(),
      base::BindRepeating(
          [](scoped_refptr<base::SingleThreadTaskRunner> task_runner,
             base::WeakPtr<OhosPlatformWindow> window, gfx::Rect bounds,
             float density) {
            task_runner->PostTask(
                FROM_HERE,
                base::BindOnce(
                    &OhosPlatformWindow::OnNativeSurfaceBoundsChanged, window,
                    bounds, density));
          },
          base::SingleThreadTaskRunner::GetCurrentDefault(),
          weak_factory_.GetWeakPtr()));
  SetOhosLogicalWindowCloseCallback(
      adapter_.GetAcceleratedWidget(),
      base::BindRepeating(
          [](scoped_refptr<base::SingleThreadTaskRunner> task_runner,
             base::WeakPtr<OhosPlatformWindow> window) {
            task_runner->PostTask(
                FROM_HERE,
                base::BindOnce(&OhosPlatformWindow::Close, std::move(window)));
          },
          base::SingleThreadTaskRunner::GetCurrentDefault(),
          weak_factory_.GetWeakPtr()));
  if (PlatformEventSource* event_source = PlatformEventSource::GetInstance()) {
    event_source->AddPlatformEventDispatcher(this);
  }
}

OhosPlatformWindow::~OhosPlatformWindow() {
  SetWmMoveLoopHandler(this, nullptr);
  SetOhosLogicalWindowCloseCallback(adapter_.GetAcceleratedWidget(), {});
  SetOhosNativeSurfaceBoundsCallback(adapter_.GetAcceleratedWidget(), {});
  if (PlatformEventSource* event_source = PlatformEventSource::GetInstance()) {
    event_source->RemovePlatformEventDispatcher(this);
  }
}

void OhosPlatformWindow::Show(bool inactive) {
  (void)inactive;
  if (adapter_.IsVisible()) {
    return;
  }
  adapter_.SetVisible(true);
  delegate()->OnOcclusionStateChanged(PlatformWindowOcclusionState::kVisible);
  delegate()->OnDamageRect(gfx::Rect(adapter_.GetBounds().size()));
}

void OhosPlatformWindow::Hide() {
  if (!adapter_.IsVisible()) {
    return;
  }
  adapter_.SetVisible(false);
  delegate()->OnOcclusionStateChanged(PlatformWindowOcclusionState::kHidden);
}

void OhosPlatformWindow::Close() {
  RequestOhosWindowAction(adapter_.GetAcceleratedWidget(),
                          OhosWindowAction::kClose);
  delegate()->OnClosed();
}

bool OhosPlatformWindow::IsVisible() const {
  return adapter_.IsVisible();
}

void OhosPlatformWindow::SetBoundsInPixels(const gfx::Rect& bounds) {
  const bool origin_changed = adapter_.GetBounds().origin() != bounds.origin();
  adapter_.SetBounds(bounds);
  delegate()->OnBoundsChanged({origin_changed});
}

gfx::Rect OhosPlatformWindow::GetBoundsInPixels() const {
  return adapter_.GetBounds();
}

void OhosPlatformWindow::SetBoundsInDIP(const gfx::Rect& bounds) {
  SetBoundsInPixels(delegate()->ConvertRectToPixels(bounds));
}

gfx::Rect OhosPlatformWindow::GetBoundsInDIP() const {
  auto* window = const_cast<OhosPlatformWindow*>(this);
  return window->delegate()->ConvertRectToDIP(adapter_.GetBounds());
}

void OhosPlatformWindow::Activate() {
  if (adapter_.IsFocused()) {
    return;
  }
  adapter_.SetFocused(true);
  delegate()->OnActivationChanged(true);
}

void OhosPlatformWindow::Deactivate() {
  if (!adapter_.IsFocused()) {
    return;
  }
  adapter_.SetFocused(false);
  delegate()->OnActivationChanged(false);
}

void OhosPlatformWindow::SetFullscreen(bool fullscreen,
                                       int64_t target_display_id) {
  (void)target_display_id;
  const OhosWindowAction action = fullscreen
                                      ? OhosWindowAction::kEnterFullscreen
                                      : OhosWindowAction::kExitFullscreen;
  if (RequestOhosWindowAction(adapter_.GetAcceleratedWidget(), action)) {
    SetWindowState(fullscreen ? PlatformWindowState::kFullScreen
                              : PlatformWindowState::kNormal);
  }
}

void OhosPlatformWindow::Maximize() {
  if (RequestOhosWindowAction(adapter_.GetAcceleratedWidget(),
                              OhosWindowAction::kMaximize)) {
    SetWindowState(PlatformWindowState::kMaximized);
  }
}

void OhosPlatformWindow::Minimize() {
  if (RequestOhosWindowAction(adapter_.GetAcceleratedWidget(),
                              OhosWindowAction::kMinimize)) {
    SetWindowState(PlatformWindowState::kMinimized);
  }
}

void OhosPlatformWindow::Restore() {
  if (RequestOhosWindowAction(adapter_.GetAcceleratedWidget(),
                              OhosWindowAction::kRestore)) {
    SetWindowState(PlatformWindowState::kNormal);
  }
}

PlatformWindowState OhosPlatformWindow::GetPlatformWindowState() const {
  return window_state_;
}

bool OhosPlatformWindow::CanDispatchEvent(const PlatformEvent& event) {
  if (!event || !adapter_.IsVisible()) {
    return false;
  }
  return OhosEventSource::GetCurrentDispatchTarget() ==
         adapter_.GetAcceleratedWidget();
}

uint32_t OhosPlatformWindow::DispatchEvent(const PlatformEvent& event) {
  if (event->type() == EventType::kMousePressed ||
      event->type() == EventType::kTouchPressed) {
    Activate();
  }
  delegate()->DispatchEvent(event);
  return POST_DISPATCH_STOP_PROPAGATION;
}

bool OhosPlatformWindow::RunMoveLoop(const gfx::Vector2d& drag_offset) {
  (void)drag_offset;
  return RequestOhosWindowAction(adapter_.GetAcceleratedWidget(),
                                 OhosWindowAction::kStartMoving);
}

void OhosPlatformWindow::EndMoveLoop() {}

void OhosPlatformWindow::OnNativeSurfaceBoundsChanged(gfx::Rect bounds,
                                                      float density) {
  if (bounds.IsEmpty()) {
    return;
  }
  const bool origin_changed = adapter_.GetBounds().origin() != bounds.origin();
  adapter_.SetBounds(bounds);
  delegate()->OnBoundsChanged({origin_changed});
  delegate()->OnDamageRect(gfx::Rect(bounds.size()));
  (void)density;
}

void OhosPlatformWindow::SetWindowState(PlatformWindowState state) {
  if (window_state_ == state) {
    return;
  }
  const PlatformWindowState old_state = window_state_;
  window_state_ = state;
  delegate()->OnWindowStateChanged(old_state, state);
}

}  // namespace ui
