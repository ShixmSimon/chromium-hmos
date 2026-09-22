#include "ui/ozone/platform/ohos/ohos_event_source.h"

#include <algorithm>
#include <map>
#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/synchronization/lock.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "ui/events/event.h"
#include "ui/gfx/geometry/point_conversions.h"
#include "ui/ozone/platform/ohos/ohos_native_window_registry.h"

namespace ui {
namespace {

thread_local OhosEventSource* g_current_ohos_event_source = nullptr;

using TouchMoveKey = std::pair<int, gfx::AcceleratedWidget>;

struct PendingTouchMove {
  std::unique_ptr<Event> event;
  gfx::AcceleratedWidget target_hint = gfx::kNullAcceleratedWidget;
  TouchMoveKey key;
  base::TimeTicks queued_at;
};

struct EventSourceBridge {
  base::Lock lock;
  scoped_refptr<base::SingleThreadTaskRunner> task_runner GUARDED_BY(lock);
  base::WeakPtr<OhosEventSource> event_source GUARDED_BY(lock);
  base::TimeTicks last_touch_press GUARDED_BY(lock);
  std::map<TouchMoveKey, std::shared_ptr<PendingTouchMove>> pending_touch_moves
      GUARDED_BY(lock);
  uint64_t touch_moves_received GUARDED_BY(lock) = 0;
  uint64_t touch_moves_dispatched GUARDED_BY(lock) = 0;
  uint64_t touch_moves_coalesced GUARDED_BY(lock) = 0;
  uint64_t next_touch_move_report GUARDED_BY(lock) = 240;
  size_t queued_touch_move_tasks GUARDED_BY(lock) = 0;
  base::TimeDelta max_touch_move_age GUARDED_BY(lock);
};

EventSourceBridge& GetEventSourceBridge() {
  static base::NoDestructor<EventSourceBridge> bridge;
  return *bridge;
}

}  // namespace

OhosEventSource::OhosEventSource() {
  DCHECK(!g_current_ohos_event_source);
  g_current_ohos_event_source = this;
  EventSourceBridge& bridge = GetEventSourceBridge();
  base::AutoLock lock(bridge.lock);
  bridge.task_runner = base::SingleThreadTaskRunner::GetCurrentDefault();
  bridge.event_source = weak_factory_.GetWeakPtr();
  bridge.pending_touch_moves.clear();
  bridge.touch_moves_received = 0;
  bridge.touch_moves_dispatched = 0;
  bridge.touch_moves_coalesced = 0;
  bridge.next_touch_move_report = 240;
  bridge.queued_touch_move_tasks = 0;
  bridge.max_touch_move_age = base::TimeDelta();
}

OhosEventSource::~OhosEventSource() {
  weak_factory_.InvalidateWeakPtrs();
  EventSourceBridge& bridge = GetEventSourceBridge();
  base::AutoLock lock(bridge.lock);
  bridge.pending_touch_moves.clear();
  bridge.queued_touch_move_tasks = 0;
  bridge.event_source = nullptr;
  bridge.task_runner = nullptr;
  g_current_ohos_event_source = nullptr;
}

// static
bool OhosEventSource::PostEvent(std::unique_ptr<Event> event,
                                gfx::AcceleratedWidget target_hint) {
  if (!event) {
    return false;
  }

  scoped_refptr<base::SingleThreadTaskRunner> task_runner;
  base::WeakPtr<OhosEventSource> event_source;
  std::shared_ptr<PendingTouchMove> pending_touch_move;
  {
    EventSourceBridge& bridge = GetEventSourceBridge();
    base::AutoLock lock(bridge.lock);
    if (event->type() == EventType::kTouchPressed) {
      bridge.last_touch_press = base::TimeTicks::Now();
    }
    task_runner = bridge.task_runner;
    event_source = bridge.event_source;
    if (task_runner && event->IsTouchEvent()) {
      const int pointer_id = event->AsTouchEvent()->pointer_details().id;
      if (event->type() == EventType::kTouchMoved) {
        ++bridge.touch_moves_received;
        const TouchMoveKey key(pointer_id, target_hint);
        auto existing = bridge.pending_touch_moves.find(key);
        if (existing != bridge.pending_touch_moves.end()) {
          existing->second->event = std::move(event);
          ++bridge.touch_moves_coalesced;
          return true;
        }
        pending_touch_move = std::make_shared<PendingTouchMove>();
        pending_touch_move->event = std::move(event);
        pending_touch_move->target_hint = target_hint;
        pending_touch_move->key = key;
        pending_touch_move->queued_at = base::TimeTicks::Now();
        bridge.pending_touch_moves.emplace(key, pending_touch_move);
        ++bridge.queued_touch_move_tasks;
      } else {
        // Close the current coalescing window. A MOVE arriving after this
        // discrete event must be queued after it, even if the earlier MOVE's
        // task has not run yet.
        std::erase_if(bridge.pending_touch_moves,
                      [pointer_id](const auto& entry) {
                        return entry.first.first == pointer_id;
                      });
      }
    }
  }
  if (!task_runner) {
    return false;
  }

  if (pending_touch_move) {
    const bool posted = task_runner->PostTask(
        FROM_HERE,
        base::BindOnce(
            [](base::WeakPtr<OhosEventSource> source,
               std::shared_ptr<PendingTouchMove> pending) {
              std::unique_ptr<Event> event;
              uint64_t received = 0;
              uint64_t dispatched = 0;
              uint64_t coalesced = 0;
              size_t pending_count = 0;
              base::TimeDelta event_age;
              base::TimeDelta queue_delay;
              base::TimeDelta max_event_age;
              bool report_stats = false;
              {
                EventSourceBridge& bridge = GetEventSourceBridge();
                base::AutoLock lock(bridge.lock);
                event = std::move(pending->event);
                auto active = bridge.pending_touch_moves.find(pending->key);
                if (active != bridge.pending_touch_moves.end() &&
                    active->second == pending) {
                  bridge.pending_touch_moves.erase(active);
                }
                if (bridge.queued_touch_move_tasks > 0) {
                  --bridge.queued_touch_move_tasks;
                }
                ++bridge.touch_moves_dispatched;
                const base::TimeTicks now = base::TimeTicks::Now();
                queue_delay = now - pending->queued_at;
                if (event && !event->time_stamp().is_null() &&
                    now >= event->time_stamp()) {
                  event_age = now - event->time_stamp();
                  bridge.max_touch_move_age =
                      std::max(bridge.max_touch_move_age, event_age);
                }
                if (bridge.touch_moves_received >=
                    bridge.next_touch_move_report) {
                  report_stats = true;
                  while (bridge.touch_moves_received >=
                         bridge.next_touch_move_report) {
                    bridge.next_touch_move_report += 240;
                  }
                }
                received = bridge.touch_moves_received;
                dispatched = bridge.touch_moves_dispatched;
                coalesced = bridge.touch_moves_coalesced;
                pending_count = bridge.queued_touch_move_tasks;
                max_event_age = bridge.max_touch_move_age;
              }
              if (report_stats) {
                LOG(INFO) << "OHOS touch MOVE queue received=" << received
                          << " dispatched=" << dispatched
                          << " coalesced=" << coalesced
                          << " pending=" << pending_count
                          << " event_age_ms=" << event_age.InMillisecondsF()
                          << " max_event_age_ms="
                          << max_event_age.InMillisecondsF()
                          << " queue_delay_ms="
                          << queue_delay.InMillisecondsF();
              }
              if (source && event) {
                source->DispatchOwnedEvent(std::move(event),
                                           pending->target_hint);
              }
            },
            std::move(event_source), pending_touch_move));
    if (!posted) {
      EventSourceBridge& bridge = GetEventSourceBridge();
      base::AutoLock lock(bridge.lock);
      auto active = bridge.pending_touch_moves.find(pending_touch_move->key);
      if (active != bridge.pending_touch_moves.end() &&
          active->second == pending_touch_move) {
        bridge.pending_touch_moves.erase(active);
      }
      if (bridge.queued_touch_move_tasks > 0) {
        --bridge.queued_touch_move_tasks;
      }
    }
    return posted;
  }

  return task_runner->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](base::WeakPtr<OhosEventSource> source,
             std::unique_ptr<Event> event, gfx::AcceleratedWidget target_hint) {
            if (source) {
              source->DispatchOwnedEvent(std::move(event), target_hint);
            }
          },
          std::move(event_source), std::move(event), target_hint));
}

// static
bool OhosEventSource::WasTouchInteractionRecent() {
  EventSourceBridge& bridge = GetEventSourceBridge();
  base::AutoLock lock(bridge.lock);
  return !bridge.last_touch_press.is_null() &&
         base::TimeTicks::Now() - bridge.last_touch_press < base::Seconds(1);
}

// static
bool OhosEventSource::ResetPointerCaptures(gfx::AcceleratedWidget widget) {
  scoped_refptr<base::SingleThreadTaskRunner> task_runner;
  base::WeakPtr<OhosEventSource> event_source;
  {
    EventSourceBridge& bridge = GetEventSourceBridge();
    base::AutoLock lock(bridge.lock);
    task_runner = bridge.task_runner;
    event_source = bridge.event_source;
  }
  if (!task_runner) {
    return false;
  }
  if (task_runner->RunsTasksInCurrentSequence()) {
    if (event_source) {
      event_source->ResetPointerCapturesOnEventThread(widget);
    }
    return true;
  }
  return task_runner->PostTask(
      FROM_HERE, base::BindOnce(
                     [](base::WeakPtr<OhosEventSource> source,
                        gfx::AcceleratedWidget widget) {
                       if (source) {
                         source->ResetPointerCapturesOnEventThread(widget);
                       }
                     },
                     std::move(event_source), widget));
}

// static
gfx::AcceleratedWidget OhosEventSource::GetCurrentDispatchTarget() {
  return g_current_ohos_event_source
             ? g_current_ohos_event_source->dispatch_target_
             : gfx::kNullAcceleratedWidget;
}

// static
OhosEventSource::TouchMoveQueueStats
OhosEventSource::GetTouchMoveQueueStatsForTesting() {
  EventSourceBridge& bridge = GetEventSourceBridge();
  base::AutoLock lock(bridge.lock);
  return {.received = bridge.touch_moves_received,
          .dispatched = bridge.touch_moves_dispatched,
          .coalesced = bridge.touch_moves_coalesced,
          .pending = bridge.queued_touch_move_tasks};
}

gfx::AcceleratedWidget OhosEventSource::ResolveDispatchTargetForTesting(
    Event* event,
    gfx::AcceleratedWidget target_hint) {
  return ResolveDispatchTarget(event, target_hint);
}

gfx::AcceleratedWidget OhosEventSource::ResolveDispatchTarget(
    Event* event,
    gfx::AcceleratedWidget target_hint) {
  if (!event) {
    return gfx::kNullAcceleratedWidget;
  }
  if (!event->IsLocatedEvent()) {
    return GetOhosFocusedLogicalWindow();
  }

  const gfx::Point screen_point =
      gfx::ToRoundedPoint(event->AsLocatedEvent()->root_location_f());
  const auto target_at_point = [&]() {
    if (target_hint != gfx::kNullAcceleratedWidget) {
      const std::optional<gfx::Rect> hinted_bounds =
          GetOhosLogicalWindowBounds(target_hint);
      if (!hinted_bounds) {
        return GetOhosAcceleratedWidgetAtScreenPoint(screen_point);
      }
      if (hinted_bounds->Contains(screen_point)) {
        const gfx::AcceleratedWidget topmost =
            GetOhosAcceleratedWidgetAtScreenPoint(screen_point);
        if (topmost != gfx::kNullAcceleratedWidget) {
          return topmost;
        }
      }
      // ArkUI sheets can report coordinates in their own surface space while
      // Chromium still has the pre-sheet logical bounds. Keep the explicit
      // surface target as a fallback so the press cannot reach the page below.
      return target_hint;
    }
    return GetOhosAcceleratedWidgetAtScreenPoint(screen_point);
  };

  if (event->IsTouchEvent()) {
    const int pointer_id = event->AsTouchEvent()->pointer_details().id;
    // A DOWN starts a new pointer sequence. HarmonyOS can omit the matching UP
    // when opening an auxiliary window, so do not let that stale capture keep
    // subsequent menu presses routed to the previous window.
    if (event->type() == EventType::kTouchPressed) {
      touch_captures_.erase(pointer_id);
    }
    auto capture = touch_captures_.find(pointer_id);
    if (capture != touch_captures_.end() &&
        GetOhosLogicalWindowBounds(capture->second).has_value()) {
      return capture->second;
    }
    if (capture != touch_captures_.end()) {
      touch_captures_.erase(capture);
    }
    const gfx::AcceleratedWidget target = target_at_point();
    if (event->type() == EventType::kTouchPressed &&
        target != gfx::kNullAcceleratedWidget) {
      touch_captures_[pointer_id] = target;
    }
    return target;
  }

  if (event->IsMouseEvent()) {
    const MouseEvent& mouse_event = *event->AsMouseEvent();
    const bool starts_new_sequence =
        event->type() == EventType::kMousePressed &&
        (mouse_event.button_flags() & ~mouse_event.changed_button_flags()) == 0;
    const bool has_no_active_buttons =
        !mouse_event.IsAnyButton() &&
        event->type() != EventType::kMouseReleased;
    if (starts_new_sequence || has_no_active_buttons) {
      mouse_capture_ = gfx::kNullAcceleratedWidget;
    }
    if (mouse_capture_ != gfx::kNullAcceleratedWidget &&
        GetOhosLogicalWindowBounds(mouse_capture_).has_value()) {
      return mouse_capture_;
    }
    mouse_capture_ = gfx::kNullAcceleratedWidget;
    const gfx::AcceleratedWidget target = target_at_point();
    if (event->type() == EventType::kMousePressed &&
        target != gfx::kNullAcceleratedWidget) {
      mouse_capture_ = target;
    }
    return target;
  }

  return target_at_point();
}

void OhosEventSource::ReleasePointerCaptureAfterDispatch(const Event& event) {
  if (event.IsTouchEvent() && (event.type() == EventType::kTouchReleased ||
                               event.type() == EventType::kTouchCancelled)) {
    touch_captures_.erase(event.AsTouchEvent()->pointer_details().id);
  }
  if (event.type() == EventType::kMouseReleased ||
      event.type() == EventType::kMouseCaptureChanged) {
    mouse_capture_ = gfx::kNullAcceleratedWidget;
  }
}

void OhosEventSource::ResetPointerCapturesOnEventThread(
    gfx::AcceleratedWidget widget) {
  if (widget == gfx::kNullAcceleratedWidget) {
    dispatch_target_ = gfx::kNullAcceleratedWidget;
    mouse_capture_ = gfx::kNullAcceleratedWidget;
    touch_captures_.clear();
    return;
  }
  if (dispatch_target_ == widget) {
    dispatch_target_ = gfx::kNullAcceleratedWidget;
  }
  if (mouse_capture_ == widget) {
    mouse_capture_ = gfx::kNullAcceleratedWidget;
  }
  std::erase_if(touch_captures_, [widget](const auto& capture) {
    return capture.second == widget;
  });
}

void OhosEventSource::DispatchOwnedEvent(std::unique_ptr<Event> event,
                                         gfx::AcceleratedWidget target_hint) {
  const bool trace_pointer = event->type() == EventType::kMousePressed ||
                             event->type() == EventType::kMouseReleased ||
                             event->type() == EventType::kTouchPressed ||
                             event->type() == EventType::kTouchReleased;
  const gfx::PointF incoming_location =
      event->IsLocatedEvent() ? event->AsLocatedEvent()->location_f()
                              : gfx::PointF();
  const gfx::PointF incoming_root_location =
      event->IsLocatedEvent() ? event->AsLocatedEvent()->root_location_f()
                              : gfx::PointF();
  const gfx::AcceleratedWidget previous_target = dispatch_target_;
  dispatch_target_ = ResolveDispatchTarget(event.get(), target_hint);
  if (trace_pointer) {
    LOG(WARNING) << "OHOS input dispatch type="
                 << static_cast<int>(event->type())
                 << " local=" << incoming_location.ToString()
                 << " root=" << incoming_root_location.ToString()
                 << " target=" << dispatch_target_ << " hint=" << target_hint;
  }
  if (event->IsLocatedEvent() &&
      dispatch_target_ != gfx::kNullAcceleratedWidget) {
    if (std::optional<gfx::Rect> target_bounds =
            GetOhosLogicalWindowBounds(dispatch_target_)) {
      const gfx::PointF screen_location =
          event->AsLocatedEvent()->root_location_f();
      const bool uses_explicit_surface_target =
          target_hint != gfx::kNullAcceleratedWidget &&
          dispatch_target_ == target_hint;
      // Native XComponent coordinates are already relative to their bound
      // surface. Logical popups sharing the primary surface have no explicit
      // target and still need screen-to-local conversion.
      const gfx::PointF local_location =
          IsOhosPrimaryLogicalWindow(dispatch_target_) ||
                  uses_explicit_surface_target
              ? event->AsLocatedEvent()->location_f()
              : gfx::PointF(screen_location.x() - target_bounds->x(),
                            screen_location.y() - target_bounds->y());
      event->AsLocatedEvent()->set_location_f(local_location);
      event->AsLocatedEvent()->set_root_location_f(local_location);
    }
  }
  DispatchEvent(event.get());
  ReleasePointerCaptureAfterDispatch(*event);
  dispatch_target_ = previous_target;
}

}  // namespace ui
