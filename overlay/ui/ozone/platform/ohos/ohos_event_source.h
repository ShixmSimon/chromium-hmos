#ifndef UI_OZONE_PLATFORM_OHOS_OHOS_EVENT_SOURCE_H_
#define UI_OZONE_PLATFORM_OHOS_OHOS_EVENT_SOURCE_H_

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>

#include "base/memory/weak_ptr.h"
#include "ui/events/platform/platform_event_source.h"
#include "ui/gfx/native_ui_types.h"

namespace ui {

class Event;

class OhosEventSource : public PlatformEventSource {
 public:
  struct TouchMoveQueueStats {
    uint64_t received = 0;
    uint64_t dispatched = 0;
    uint64_t coalesced = 0;
    size_t pending = 0;
  };

  OhosEventSource();
  ~OhosEventSource() override;

  static bool PostEvent(
      std::unique_ptr<Event> event,
      gfx::AcceleratedWidget target_hint = gfx::kNullAcceleratedWidget);
  static bool ResetPointerCaptures(
      gfx::AcceleratedWidget widget = gfx::kNullAcceleratedWidget);
  static bool WasTouchInteractionRecent();
  static gfx::AcceleratedWidget GetCurrentDispatchTarget();
  static TouchMoveQueueStats GetTouchMoveQueueStatsForTesting();

  gfx::AcceleratedWidget ResolveDispatchTargetForTesting(
      Event* event,
      gfx::AcceleratedWidget target_hint = gfx::kNullAcceleratedWidget);

 private:
  gfx::AcceleratedWidget ResolveDispatchTarget(
      Event* event,
      gfx::AcceleratedWidget target_hint);
  void ReleasePointerCaptureAfterDispatch(const Event& event);
  void ResetPointerCapturesOnEventThread(gfx::AcceleratedWidget widget);
  void DispatchOwnedEvent(std::unique_ptr<Event> event,
                          gfx::AcceleratedWidget target_hint);

  gfx::AcceleratedWidget dispatch_target_ = gfx::kNullAcceleratedWidget;
  gfx::AcceleratedWidget mouse_capture_ = gfx::kNullAcceleratedWidget;
  std::map<int, gfx::AcceleratedWidget> touch_captures_;
  base::WeakPtrFactory<OhosEventSource> weak_factory_{this};
};

}  // namespace ui

#endif  // UI_OZONE_PLATFORM_OHOS_OHOS_EVENT_SOURCE_H_
