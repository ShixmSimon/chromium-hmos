#include "ui/ozone/platform/ohos/ohos_platform_window.h"

#include "base/logging.h"
#include "base/run_loop.h"
#include "base/strings/utf_string_conversions.h"
#include "ui/base/cursor/cursor.h"
#include "ui/base/dragdrop/drag_drop_types.h"
#include "ui/base/dragdrop/os_exchange_data.h"
#include "ui/platform_window/wm/wm_drop_handler.h"

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
                                       bool expects_native_surface,
                                       bool anchored)
    : StubWindow(delegate, false, bounds),
      adapter_(bounds, expects_native_surface, anchored),
      anchored_(anchored) {
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
                base::BindOnce(
                    &OhosPlatformWindow::OnLogicalWindowCloseRequest,
                    std::move(window)));
          },
          base::SingleThreadTaskRunner::GetCurrentDefault(),
          weak_factory_.GetWeakPtr()));
  // Registered here rather than by the window tree host: the host reads it
  // back in CreateDragDropClient, which runs right after this constructor.
  SetWmDragHandler(this, this);
  SetOhosDragCallback(
      adapter_.GetAcceleratedWidget(),
      base::BindRepeating(
          [](base::WeakPtr<OhosPlatformWindow> window,
             OhosDragEvent event) -> int {
            // Straight through rather than posted: the caller wants the
            // operation back so it can show the right badge, and it is
            // already on the UI thread.
            return window ? window->OnDragEvent(std::move(event)) : 0;
          },
          weak_factory_.GetWeakPtr()));
  if (PlatformEventSource* event_source = PlatformEventSource::GetInstance()) {
    event_source->AddPlatformEventDispatcher(this);
  }
}

OhosPlatformWindow::~OhosPlatformWindow() {
  SetWmMoveLoopHandler(this, nullptr);
  SetWmDragHandler(this, nullptr);
  SetOhosDragCallback(adapter_.GetAcceleratedWidget(), {});
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

void OhosPlatformWindow::OnLogicalWindowCloseRequest() {
  delegate()->OnCloseRequest();
}

int OhosPlatformWindow::OnDragEvent(OhosDragEvent event) {
  // DesktopDragDropClientOzone is registered as this window's drop handler
  // when the window tree host creates its drag-and-drop client, so from here
  // the path is the one every ozone platform takes: find the aura window
  // under the point, build a DropTargetEvent, ask WebContentsViewAura, which
  // asks the page.
  if (event.stage == OhosDragStage::kSourceFinished) {
    // Our own drag ending, not the system dragging over us. Handled before
    // the drop handler is looked up: there is nothing to hand it, and a
    // window without one must still be able to stop waiting.
    source_drag_operation_ = event.operations;
    if (end_source_drag_) {
      std::move(end_source_drag_).Run();
    }
    return event.operations;
  }

  WmDropHandler* handler = GetWmDropHandler(*this);
  if (!handler) {
    return 0;
  }
  switch (event.stage) {
    case OhosDragStage::kEnter:
      handler->OnDragEnter(event.location, event.operations, /*modifiers=*/0);
      if (event.data) {
        handler->OnDragDataAvailable(std::move(event.data));
      }
      return handler->OnDragMotion(event.location, event.operations,
                                   /*modifiers=*/0);
    case OhosDragStage::kMove:
      return handler->OnDragMotion(event.location, event.operations,
                                   /*modifiers=*/0);
    case OhosDragStage::kLeave:
      handler->OnDragLeave();
      return 0;
    case OhosDragStage::kSourceFinished:
      return 0;  // Answered above.
    case OhosDragStage::kDrop: {
      if (event.data) {
        // Replaces the summary sent on enter. Legal to send twice, and it has
        // to be: until now the file paths were not known.
        handler->OnDragDataAvailable(std::move(event.data));
      }
      // What the page will take, asked one last time now that the real data
      // is here. Zero means nothing under the finger wanted it, and the
      // shell is told so it can decide what dropping on a browser means.
      const int accepted =
          handler->OnDragMotion(event.location, event.operations,
                                /*modifiers=*/0);
      handler->OnDragDrop(/*modifiers=*/0);
      return accepted;
    }
  }
  return 0;
}

bool OhosPlatformWindow::IsVisible() const {
  return adapter_.IsVisible();
}

void OhosPlatformWindow::SetBoundsInPixels(const gfx::Rect& requested) {
  gfx::Rect bounds = requested;
  // A window the shell hosts in a surface of its own is placed and sized by
  // the shell; what Chromium asks for cannot change the XComponent. A second
  // browser window opened restored, at the size WindowSizer chose, which was
  // taller than the XComponent below the status bar: drawn from the bottom
  // up, its top -- tab strip, toolbar, half the bookmark bar -- fell off the
  // surface. Follow the surface instead, as OnNativeSurfaceBoundsChanged does.
  if (!anchored_) {
    std::optional<OhosNativeSurface> surface =
        GetOhosNativeSurface(adapter_.GetAcceleratedWidget());
    if (surface && surface->window && !surface->bounds.IsEmpty() &&
        surface->bounds != bounds) {
      LOG(WARNING) << "OHOS window " << adapter_.GetAcceleratedWidget()
                   << ": keeping the shell's bounds "
                   << surface->bounds.ToString() << " over "
                   << bounds.ToString();
      bounds = surface->bounds;
    }
  }
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

bool OhosPlatformWindow::ShouldWindowContentsBeTransparent() const {
  // Views asks for a translucent widget for anything with a rounded corner or
  // a shadow -- an autofill list, a <select> menu, a bubble -- and paints the
  // area outside the rounded rectangle transparent. Whether that is honoured
  // is decided here: DesktopNativeWidgetAura::UpdateWindowTransparency() reads
  // this through DesktopWindowTreeHostPlatform, and the default answer is
  // false, so the compositor filled those pixels with an opaque frame colour
  // instead. Every popup drew as a rounded card inside a black rectangle.
  //
  // Only the anchored ones: the browser window itself covers the screen and
  // has nothing behind it to show through, and an opaque root is cheaper to
  // composite.
  return anchored_;
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

bool OhosPlatformWindow::StartDrag(const OSExchangeData& data,
                                   int operations,
                                   mojom::DragEventSource source,
                                   gfx::NativeCursor cursor,
                                   bool can_grab_pointer,
                                   base::OnceClosure drag_started_callback,
                                   DragFinishedCallback drag_finished_callback,
                                   LocationDelegate* location_delegate) {
  (void)source;
  (void)cursor;
  (void)can_grab_pointer;
  // No location delegate work: HarmonyOS draws the drag preview and moves it
  // itself, so Chromium is not asked to follow the finger.
  (void)location_delegate;
  if (end_source_drag_) {
    // Already dragging. One at a time is all the system offers.
    return false;
  }

  OhosDragOutRequest request;
  request.widget = adapter_.GetAcceleratedWidget();
  request.operations = operations;
  if (std::optional<std::u16string> text = data.GetString()) {
    request.text = base::UTF16ToUTF8(*text);
  }
  const std::vector<ClipboardUrlInfo> urls =
      data.GetURLs(FilenameToURLPolicy::DO_NOT_CONVERT_FILENAMES);
  if (!urls.empty()) {
    // The one the reader put their finger on, which is the first.
    request.url = urls.front().url.spec();
  }
  if (std::optional<OSExchangeData::HtmlInfo> html = data.GetHtml()) {
    request.html = base::UTF16ToUTF8(html->html);
  }
  if (std::optional<OSExchangeData::FileContentsInfo> contents =
          data.GetFileContents()) {
    // An image. The bytes are the ones the page loaded, in the format it
    // loaded them in, and the name is the one a "save image as" would use.
    // Whoever takes this from here writes them to a file: this layer has no
    // business choosing a directory in the profile.
    request.file_name = contents->filename.AsUTF8Unsafe();
    request.file_contents = std::move(contents->file_contents);
  }
  if (request.text.empty() && request.url.empty() && request.html.empty() &&
      request.file_contents.empty()) {
    // Nothing the system can carry.
    return false;
  }

  if (!StartOhosDragOut(std::move(request))) {
    return false;
  }
  std::move(drag_started_callback).Run();

  source_drag_operation_ = 0;
  base::RunLoop loop(base::RunLoop::Type::kNestableTasksAllowed);
  end_source_drag_ = loop.QuitClosure();
  // Blocks until the app side reports the drag finished, the way StartDrag is
  // specified to. The browser's UI thread is its own thread here, so the app
  // keeps drawing and can still deliver the answer.
  base::WeakPtr<OhosPlatformWindow> alive = weak_factory_.GetWeakPtr();
  loop.Run();
  if (!alive) {
    return false;
  }
  end_source_drag_.Reset();

  const int operation = source_drag_operation_;
  std::move(drag_finished_callback).Run(PreferredDragOperation(operation));
  return operation != DragDropTypes::DRAG_NONE;
}

void OhosPlatformWindow::CancelDrag() {
  if (end_source_drag_) {
    source_drag_operation_ = DragDropTypes::DRAG_NONE;
    std::move(end_source_drag_).Run();
  }
}

void OhosPlatformWindow::UpdateDragImage(const gfx::ImageSkia& image,
                                         const gfx::Vector2d& offset) {
  // HarmonyOS owns the drag preview; Chromium's is never shown.
  (void)image;
  (void)offset;
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
  // An anchored popup's surface reports no screen origin. Its XComponent has
  // no onAreaChange, so OhosAuraShellHost never learns where the shell put it
  // and ToPixelBoundsWithOrigin falls back to the surface-local origin, which
  // is always 0,0. Writing that back as the window's position destroyed the
  // anchor Chromium had just computed: the shell was told the popup now lived
  // at the top-left corner, moved it there, and the wrong position became the
  // truth. It stayed wrong until something set the bounds again -- which is
  // why a second tap on the same field put the popup in the right place.
  //
  // The size is real and has to be taken; the origin is a placeholder.
  if (anchored_) {
    bounds.set_origin(adapter_.GetBounds().origin());
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
