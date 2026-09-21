// Copyright (c) 2026
// Licensed under the Apache License, Version 2.0.

#include "ohos_nweb/src/aura_shell/ohos_aura_input_router.h"

#include <cmath>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

#include "arkui/native_key_event.h"
#include "base/json/json_reader.h"
#include "base/numerics/safe_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "base/values.h"
#include "ohos_nweb/src/nweb_hilog.h"
#include "ui/events/event.h"
#include "ui/events/event_constants.h"
#include "ui/events/keycodes/dom/dom_code.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/events/pointer_details.h"
#include "ui/gfx/geometry/point_conversions.h"
#include "ui/gfx/geometry/point_f.h"
#include "ui/gfx/geometry/vector2d.h"
#include "ui/ozone/platform/ohos/ohos_event_source.h"
#include "ui/ozone/platform/ohos/ohos_native_window_registry.h"

namespace ohos_nweb {
namespace {

constexpr base::TimeDelta kSameSourceDuplicateEventWindow =
    base::Milliseconds(16);
constexpr base::TimeDelta kCrossSourceDuplicateEventWindow =
    base::Milliseconds(250);
constexpr double kCrossSourcePointerSlop = 4.0;

bool MouseButtonsMatch(int action, int first, int second) {
  return first == second || (action == 1 && (first < 0 || second < 0));
}

std::optional<double> FindNumber(const base::DictValue& dict,
                                 std::string_view key) {
  const base::Value* value = dict.Find(key);
  if (!value) {
    return std::nullopt;
  }
  if (value->is_int()) {
    return static_cast<double>(value->GetInt());
  }
  if (value->is_double()) {
    return value->GetDouble();
  }
  return std::nullopt;
}

std::optional<ui::EventType> ToTouchEventType(int action) {
  switch (action) {
    case 0:
      return ui::EventType::kTouchPressed;
    case 1:
      return ui::EventType::kTouchReleased;
    case 2:
      return ui::EventType::kTouchMoved;
    case 3:
      return ui::EventType::kTouchCancelled;
    default:
      return std::nullopt;
  }
}

base::TimeTicks ToTouchTimestamp(int64_t timestamp_ns) {
  return timestamp_ns > 0 ? base::TimeTicks() + base::Nanoseconds(timestamp_ns)
                          : base::TimeTicks::Now();
}

int ToMouseButtonFlag(int button) {
  switch (button) {
    case 0:
      return ui::EF_LEFT_MOUSE_BUTTON;
    case 1:
      return ui::EF_RIGHT_MOUSE_BUTTON;
    case 2:
      return ui::EF_MIDDLE_MOUSE_BUTTON;
    case 3:
      return ui::EF_BACK_MOUSE_BUTTON;
    case 4:
      return ui::EF_FORWARD_MOUSE_BUTTON;
    default:
      return 0;
  }
}

int ToWheelOffset(double value) {
  const int rounded = base::ClampRound<int>(value);
  if (rounded != 0 || value == 0) {
    return rounded;
  }
  return value > 0 ? 1 : -1;
}

std::optional<ui::OhosNativeSurface> ResolvePointerSurface(
    const std::string& component_id) {
  if (!component_id.empty()) {
    const gfx::AcceleratedWidget widget =
        ui::GetOhosAcceleratedWidgetForNativeSurface(component_id);
    if (widget != gfx::kNullAcceleratedWidget) {
      if (std::optional<ui::OhosNativeSurface> surface =
              ui::GetOhosNativeSurface(widget)) {
        return surface;
      }
    }
  }
  return ui::GetPrimaryOhosNativeSurface();
}

float ResolvePointerDensity(
    const std::optional<ui::OhosNativeSurface>& surface) {
  if (surface && surface->density > 0.0f) {
    return surface->density;
  }
  if (std::optional<ui::OhosDisplayMetrics> metrics =
          ui::GetOhosDisplayMetrics();
      metrics && metrics->density > 0.0f) {
    return metrics->density;
  }
  return 1.0f;
}

gfx::PointF ToRootLocation(
    const base::DictValue& event,
    const gfx::PointF& location,
    float density,
    bool physical_pixels,
    const std::optional<ui::OhosNativeSurface>& surface) {
  std::optional<double> root_x = FindNumber(event, "rootX");
  std::optional<double> root_y = FindNumber(event, "rootY");
  if (root_x && root_y) {
    const float scale = !physical_pixels && density > 0.0f ? density : 1.0f;
    gfx::PointF root_location(static_cast<float>(*root_x * scale),
                              static_cast<float>(*root_y * scale));
    if (event.FindBool("rootWindowRelative").value_or(false)) {
      if (surface) {
        root_location.Offset(surface->bounds.x(), surface->bounds.y());
      }
    }
    return root_location;
  }
  if (surface) {
    return gfx::PointF(location.x() + surface->bounds.x(),
                       location.y() + surface->bounds.y());
  }
  return location;
}

ui::KeyboardCode ToKeyboardCode(int key_code) {
  if (key_code >= ARKUI_KEYCODE_0 && key_code <= ARKUI_KEYCODE_9) {
    return static_cast<ui::KeyboardCode>(ui::VKEY_0 + key_code -
                                         ARKUI_KEYCODE_0);
  }
  if (key_code >= ARKUI_KEYCODE_A && key_code <= ARKUI_KEYCODE_Z) {
    return static_cast<ui::KeyboardCode>(ui::VKEY_A + key_code -
                                         ARKUI_KEYCODE_A);
  }

  switch (key_code) {
    case ARKUI_KEYCODE_DPAD_UP:
      return ui::VKEY_UP;
    case ARKUI_KEYCODE_DPAD_DOWN:
      return ui::VKEY_DOWN;
    case ARKUI_KEYCODE_DPAD_LEFT:
      return ui::VKEY_LEFT;
    case ARKUI_KEYCODE_DPAD_RIGHT:
      return ui::VKEY_RIGHT;
    case ARKUI_KEYCODE_DPAD_CENTER:
    case ARKUI_KEYCODE_ENTER:
      return ui::VKEY_RETURN;
    case ARKUI_KEYCODE_TAB:
      return ui::VKEY_TAB;
    case ARKUI_KEYCODE_SPACE:
      return ui::VKEY_SPACE;
    case ARKUI_KEYCODE_DEL:
      return ui::VKEY_BACK;
    case ARKUI_KEYCODE_FORWARD_DEL:
      return ui::VKEY_DELETE;
    case ARKUI_KEYCODE_ESCAPE:
      return ui::VKEY_ESCAPE;
    case ARKUI_KEYCODE_PAGE_UP:
      return ui::VKEY_PRIOR;
    case ARKUI_KEYCODE_PAGE_DOWN:
      return ui::VKEY_NEXT;
    case ARKUI_KEYCODE_MOVE_HOME:
      return ui::VKEY_HOME;
    case ARKUI_KEYCODE_MOVE_END:
      return ui::VKEY_END;
    case ARKUI_KEYCODE_INSERT:
      return ui::VKEY_INSERT;
    case ARKUI_KEYCODE_SHIFT_LEFT:
      return ui::VKEY_LSHIFT;
    case ARKUI_KEYCODE_SHIFT_RIGHT:
      return ui::VKEY_RSHIFT;
    case ARKUI_KEYCODE_CTRL_LEFT:
      return ui::VKEY_LCONTROL;
    case ARKUI_KEYCODE_CTRL_RIGHT:
      return ui::VKEY_RCONTROL;
    case ARKUI_KEYCODE_ALT_LEFT:
      return ui::VKEY_LMENU;
    case ARKUI_KEYCODE_ALT_RIGHT:
      return ui::VKEY_RMENU;
    case ARKUI_KEYCODE_META_LEFT:
      return ui::VKEY_LWIN;
    case ARKUI_KEYCODE_META_RIGHT:
      return ui::VKEY_RWIN;
    case ARKUI_KEYCODE_CAPS_LOCK:
      return ui::VKEY_CAPITAL;
    case ARKUI_KEYCODE_SCROLL_LOCK:
      return ui::VKEY_SCROLL;
    case ARKUI_KEYCODE_VOLUME_UP:
      return ui::VKEY_VOLUME_UP;
    case ARKUI_KEYCODE_VOLUME_DOWN:
      return ui::VKEY_VOLUME_DOWN;
    case ARKUI_KEYCODE_VOLUME_MUTE:
    case ARKUI_KEYCODE_MUTE:
      return ui::VKEY_VOLUME_MUTE;
    case ARKUI_KEYCODE_COMMA:
      return ui::VKEY_OEM_COMMA;
    case ARKUI_KEYCODE_PERIOD:
      return ui::VKEY_OEM_PERIOD;
    case ARKUI_KEYCODE_MINUS:
      return ui::VKEY_OEM_MINUS;
    case ARKUI_KEYCODE_EQUALS:
    case ARKUI_KEYCODE_PLUS:
      return ui::VKEY_OEM_PLUS;
    case ARKUI_KEYCODE_SEMICOLON:
      return ui::VKEY_OEM_1;
    case ARKUI_KEYCODE_SLASH:
      return ui::VKEY_OEM_2;
    case ARKUI_KEYCODE_GRAVE:
      return ui::VKEY_OEM_3;
    case ARKUI_KEYCODE_LEFT_BRACKET:
      return ui::VKEY_OEM_4;
    case ARKUI_KEYCODE_BACKSLASH:
      return ui::VKEY_OEM_5;
    case ARKUI_KEYCODE_RIGHT_BRACKET:
      return ui::VKEY_OEM_6;
    case ARKUI_KEYCODE_APOSTROPHE:
      return ui::VKEY_OEM_7;
    default:
      return ui::VKEY_UNKNOWN;
  }
}

int ModifierFlagForKey(ui::KeyboardCode key_code) {
  switch (key_code) {
    case ui::VKEY_LSHIFT:
    case ui::VKEY_RSHIFT:
      return ui::EF_SHIFT_DOWN;
    case ui::VKEY_LCONTROL:
    case ui::VKEY_RCONTROL:
      return ui::EF_CONTROL_DOWN;
    case ui::VKEY_LMENU:
    case ui::VKEY_RMENU:
      return ui::EF_ALT_DOWN;
    case ui::VKEY_LWIN:
    case ui::VKEY_RWIN:
      return ui::EF_COMMAND_DOWN;
    default:
      return 0;
  }
}

}  // namespace

OhosAuraInputRouter::OhosAuraInputRouter(std::string component_id)
    : component_id_(std::move(component_id)) {}

OhosAuraInputRouter::~OhosAuraInputRouter() = default;

void OhosAuraInputRouter::SetFocused(bool focused) {
  focused_ = focused;
  if (!focused) {
    ResetPointerState();
  }
}

void OhosAuraInputRouter::ResetPointerState() {
  event_flags_ = 0;
  mouse_button_flags_ = 0;
  last_pointer_type_.clear();
  last_pointer_dispatch_source_.clear();
  last_pointer_action_ = -1;
  last_pointer_id_ = -1;
  last_pointer_button_ = -1;
  last_pointer_time_ = base::TimeTicks();
  last_pointer_root_x_ = 0.0;
  last_pointer_root_y_ = 0.0;
  last_mouse_event_flags_ = 0;
  last_mouse_changed_button_flags_ = 0;
  recent_pointer_events_.clear();
  recent_key_events_.clear();
  const gfx::AcceleratedWidget widget = GetBoundWidget();
  if (component_id_.empty() || widget != gfx::kNullAcceleratedWidget) {
    ui::OhosEventSource::ResetPointerCaptures(widget);
  }
}

gfx::AcceleratedWidget OhosAuraInputRouter::GetBoundWidget() const {
  if (component_id_.empty()) {
    return gfx::kNullAcceleratedWidget;
  }
  return ui::GetOhosAcceleratedWidgetForNativeSurface(component_id_);
}

bool OhosAuraInputRouter::IsDuplicatePointerEvent(
    const std::string& pointer_type,
    const std::string& dispatch_source,
    int action,
    int pointer_id,
    int button,
    double x,
    double y) {
  const base::TimeTicks now = base::TimeTicks::Now();
  const bool same_event =
      pointer_type == last_pointer_type_ && action == last_pointer_action_ &&
      pointer_id == last_pointer_id_ &&
      (pointer_type == "mouse"
           ? MouseButtonsMatch(action, button, last_pointer_button_)
           : button == last_pointer_button_);
  const base::TimeDelta elapsed = last_pointer_time_.is_null()
                                      ? base::TimeDelta::Max()
                                      : now - last_pointer_time_;
  const bool discrete_mouse_event =
      pointer_type == "mouse" && (action == 0 || action == 1 || action == 13);
  const bool same_source_duplicate =
      discrete_mouse_event && same_event &&
      dispatch_source == last_pointer_dispatch_source_ &&
      elapsed <= kSameSourceDuplicateEventWindow &&
      std::abs(x - last_pointer_x_) <= 1.0 &&
      std::abs(y - last_pointer_y_) <= 1.0;
  while (!recent_pointer_events_.empty() &&
         now - recent_pointer_events_.front().time >
             kCrossSourceDuplicateEventWindow) {
    recent_pointer_events_.pop_front();
  }

  bool cross_source_duplicate = false;
  if (!same_source_duplicate && discrete_mouse_event &&
      dispatch_source != "unknown") {
    for (auto record = recent_pointer_events_.rbegin();
         record != recent_pointer_events_.rend(); ++record) {
      if (record->matched || record->dispatch_source == dispatch_source ||
          record->dispatch_source == "unknown" ||
          record->pointer_type != pointer_type || record->action != action ||
          record->pointer_id != pointer_id ||
          !MouseButtonsMatch(action, record->button, button) ||
          std::abs(record->x - x) > kCrossSourcePointerSlop ||
          std::abs(record->y - y) > kCrossSourcePointerSlop) {
        continue;
      }
      record->matched = true;
      cross_source_duplicate = true;
      break;
    }
    recent_pointer_events_.push_back({.pointer_type = pointer_type,
                                      .dispatch_source = dispatch_source,
                                      .action = action,
                                      .pointer_id = pointer_id,
                                      .button = button,
                                      .x = x,
                                      .y = y,
                                      .time = now,
                                      .matched = cross_source_duplicate});
  }

  const bool duplicate = same_source_duplicate || cross_source_duplicate;
  last_pointer_type_ = pointer_type;
  last_pointer_dispatch_source_ = dispatch_source;
  last_pointer_action_ = action;
  last_pointer_id_ = pointer_id;
  last_pointer_button_ = button;
  last_pointer_x_ = x;
  last_pointer_y_ = y;
  last_pointer_time_ = now;
  if (duplicate) {
    ++suppressed_pointer_events_;
    if (discrete_mouse_event) {
      WVLOG_W(
          "Aura mouse duplicate source=%{public}s action=%{public}d "
          "button=%{public}d x=%{public}.1f y=%{public}.1f "
          "sameSource=%{public}d crossSource=%{public}d",
          dispatch_source.c_str(), action, button, x, y, same_source_duplicate,
          cross_source_duplicate);
    }
  }
  return duplicate;
}

bool OhosAuraInputRouter::IsDuplicateKeyEvent(
    const std::string& dispatch_source,
    int action,
    int key_code) {
  const base::TimeTicks now = base::TimeTicks::Now();
  while (!recent_key_events_.empty() && now - recent_key_events_.front().time >
                                            kCrossSourceDuplicateEventWindow) {
    recent_key_events_.pop_front();
  }

  bool duplicate = false;
  if (dispatch_source != "unknown") {
    for (auto record = recent_key_events_.rbegin();
         record != recent_key_events_.rend(); ++record) {
      if (record->matched || record->dispatch_source == dispatch_source ||
          record->dispatch_source == "unknown" || record->action != action ||
          record->key_code != key_code) {
        continue;
      }
      record->matched = true;
      duplicate = true;
      break;
    }
    recent_key_events_.push_back({.dispatch_source = dispatch_source,
                                  .action = action,
                                  .key_code = key_code,
                                  .time = now,
                                  .matched = duplicate});
  }
  if (duplicate) {
    ++suppressed_key_events_;
  }
  return duplicate;
}

void OhosAuraInputRouter::DispatchNativeTouchEvent(
    const OhosNativeTouchEvent& event) {
  const std::optional<ui::EventType> event_type =
      ToTouchEventType(event.action);
  if (!event_type ||
      IsDuplicatePointerEvent("touch", "native", event.action, event.pointer_id,
                              0, event.x, event.y)) {
    return;
  }

  if (event.action == 0) {
    if (!focused_) {
      WVLOG_I("Aura input router regained focus from native touch press");
    }
    focused_ = true;
  }

  const gfx::PointF location(event.x, event.y);
  const gfx::PointF root_location(event.root_x, event.root_y);
  last_pointer_root_x_ = root_location.x();
  last_pointer_root_y_ = root_location.y();
  last_touch_timestamp_ = ToTouchTimestamp(event.timestamp_ns);
  auto touch_event = std::make_unique<ui::TouchEvent>(
      *event_type, location, root_location, last_touch_timestamp_,
      ui::PointerDetails(ui::EventPointerType::kTouch, event.pointer_id));
  if (!ui::OhosEventSource::PostEvent(std::move(touch_event),
                                      event.target_widget)) {
    WVLOG_W(
        "Aura input router could not post native touch event action=%{public}d "
        "id=%{public}d",
        event.action, event.pointer_id);
  }
}

void OhosAuraInputRouter::DispatchPointerEvent(const std::string& event_json) {
  std::optional<base::DictValue> event =
      base::JSONReader::ReadDict(event_json, base::JSON_PARSE_RFC);
  if (!event) {
    return;
  }

  std::optional<int> action = event->FindInt("action");
  std::optional<double> x = FindNumber(*event, "x");
  std::optional<double> y = FindNumber(*event, "y");
  const std::string* pointer_type = event->FindString("pointerType");
  if (!action || !x || !y || !pointer_type) {
    return;
  }

  const int pointer_id = event->FindInt("pointerId").value_or(0);
  const int button = event->FindInt("button").value_or(-1);
  const std::string* dispatch_source_value =
      event->FindString("dispatchSource");
  const std::string dispatch_source =
      dispatch_source_value ? *dispatch_source_value : "unknown";
  const bool physical_pixels =
      event->FindBool("physicalPixels").value_or(false);
  const int target_widget_value = event->FindInt("targetWidget").value_or(0);
  const gfx::AcceleratedWidget target_widget =
      target_widget_value > 0
          ? static_cast<gfx::AcceleratedWidget>(target_widget_value)
          : gfx::kNullAcceleratedWidget;
  const std::optional<ui::OhosNativeSurface> pointer_surface =
      ResolvePointerSurface(component_id_);
  const float density = ResolvePointerDensity(pointer_surface);
  const double coordinate_scale =
      !physical_pixels && density > 0.0f ? density : 1.0f;
  const double physical_x = *x * coordinate_scale;
  const double physical_y = *y * coordinate_scale;
  if (IsDuplicatePointerEvent(*pointer_type, dispatch_source, *action,
                              pointer_id, button, physical_x, physical_y)) {
    return;
  }

  if (*action == 0 && (*pointer_type == "mouse" || *pointer_type == "touch")) {
    if (!focused_) {
      WVLOG_I("Aura input router regained focus from pointer press");
    }
    focused_ = true;
  }

  const gfx::PointF location(static_cast<float>(physical_x),
                             static_cast<float>(physical_y));
  const gfx::PointF root_location = ToRootLocation(
      *event, location, density, physical_pixels, pointer_surface);
  last_pointer_root_x_ = root_location.x();
  last_pointer_root_y_ = root_location.y();
  if (*pointer_type == "mouse" && (*action == 0 || *action == 1)) {
    WVLOG_W(
        "Aura mouse route source=%{public}s action=%{public}d "
        "button=%{public}d x=%{public}.1f y=%{public}.1f "
        "rootX=%{public}.1f rootY=%{public}.1f flags=%{public}d",
        dispatch_source.c_str(), *action, button, physical_x, physical_y,
        root_location.x(), root_location.y(), mouse_button_flags_);
  }
  if (*pointer_type == "touch") {
    std::optional<ui::EventType> event_type = ToTouchEventType(*action);
    if (!event_type) {
      return;
    }
    auto touch_event = std::make_unique<ui::TouchEvent>(
        *event_type, location, root_location, base::TimeTicks::Now(),
        ui::PointerDetails(ui::EventPointerType::kTouch, pointer_id));
    if (!ui::OhosEventSource::PostEvent(std::move(touch_event),
                                        target_widget)) {
      WVLOG_W(
          "Aura input router could not post touch event action=%{public}d "
          "id=%{public}d",
          *action, pointer_id);
    }
    return;
  }

  if (*pointer_type == "wheel") {
    std::optional<double> delta_x = FindNumber(*event, "deltaX");
    std::optional<double> delta_y = FindNumber(*event, "deltaY");
    if (!delta_x || !delta_y || (*delta_x == 0 && *delta_y == 0)) {
      return;
    }
    ui::UpdateOhosCursorScreenPoint(gfx::ToRoundedPoint(root_location));
    ui::OhosEventSource::PostEvent(
        std::make_unique<ui::MouseWheelEvent>(
            gfx::Vector2d(ToWheelOffset(*delta_x), ToWheelOffset(*delta_y)),
            location, root_location, base::TimeTicks::Now(),
            event_flags_ | mouse_button_flags_, 0),
        target_widget);
    return;
  }

  if (*pointer_type != "mouse") {
    return;
  }

  ui::UpdateOhosCursorScreenPoint(gfx::ToRoundedPoint(root_location));

  const int button_flag = ToMouseButtonFlag(button);
  ui::EventType event_type = ui::EventType::kMouseMoved;
  int changed_button_flags = 0;
  switch (*action) {
    case 0:
      if (button_flag && (mouse_button_flags_ & button_flag)) {
        WVLOG_W("Aura input router recovered a stale pressed mouse button");
        mouse_button_flags_ &= ~button_flag;
        ui::OhosEventSource::ResetPointerCaptures(
            target_widget != gfx::kNullAcceleratedWidget ? target_widget
                                                         : GetBoundWidget());
      }
      mouse_button_flags_ |= button_flag;
      changed_button_flags = button_flag;
      event_type = ui::EventType::kMousePressed;
      break;
    case 1:
      // HarmonyOS can report NONE_BUTTON on release. Preserve the button
      // transition from the current pressed state so Views receives a real
      // left-button release and menu commands can activate.
      changed_button_flags = button_flag ? button_flag : mouse_button_flags_;
      mouse_button_flags_ &= ~changed_button_flags;
      event_type = ui::EventType::kMouseReleased;
      break;
    case 2:
    case 3:
      event_type = mouse_button_flags_ ? ui::EventType::kMouseDragged
                                       : ui::EventType::kMouseMoved;
      break;
    case 13:
      ResetPointerState();
      return;
    default:
      return;
  }

  // Chromium's native event factories retain the changed button in `flags`
  // for both press and release. MenuDelegate::IsTriggerableEvent() relies on
  // that invariant when deciding whether a mouse release executes a command.
  const int mouse_event_flags =
      event_flags_ | mouse_button_flags_ | changed_button_flags;
  last_mouse_event_flags_ = mouse_event_flags;
  last_mouse_changed_button_flags_ = changed_button_flags;
  if (!ui::OhosEventSource::PostEvent(
          std::make_unique<ui::MouseEvent>(
              event_type, location, root_location, base::TimeTicks::Now(),
              mouse_event_flags, changed_button_flags),
          target_widget)) {
    WVLOG_W("Aura input router could not post mouse event action=%{public}d",
            *action);
  }
}

bool OhosAuraInputRouter::DispatchKeyEvent(const std::string& event_json) {
  std::optional<base::DictValue> event =
      base::JSONReader::ReadDict(event_json, base::JSON_PARSE_RFC);
  if (!event) {
    WVLOG_W("Aura input router rejected malformed key JSON");
    return false;
  }
  std::optional<int> action = event->FindInt("action");
  std::optional<int> raw_key_code = event->FindInt("keyCode");
  if (!action || !raw_key_code) {
    WVLOG_W("Aura input router rejected incomplete key event");
    return false;
  }
  const std::string* dispatch_source_value =
      event->FindString("dispatchSource");
  const std::string dispatch_source =
      dispatch_source_value ? *dispatch_source_value : "unknown";
  if (IsDuplicateKeyEvent(dispatch_source, *action, *raw_key_code)) {
    return true;
  }
  if (!focused_) {
    WVLOG_I("Aura input router regained focus from key event");
    focused_ = true;
  }

  const bool pressed = *action == ARKUI_KEY_EVENT_DOWN;
  const ui::KeyboardCode key_code = ToKeyboardCode(*raw_key_code);
  const int modifier_flag = ModifierFlagForKey(key_code);
  if (pressed && key_code == ui::VKEY_CAPITAL) {
    event_flags_ ^= ui::EF_CAPS_LOCK_ON;
  }
  if (pressed) {
    event_flags_ |= modifier_flag;
  } else {
    event_flags_ &= ~modifier_flag;
  }

  const ui::EventType event_type =
      pressed ? ui::EventType::kKeyPressed : ui::EventType::kKeyReleased;
  bool posted = ui::OhosEventSource::PostEvent(std::make_unique<ui::KeyEvent>(
      event_type, key_code, event_flags_, base::TimeTicks::Now()));
  if (!posted) {
    WVLOG_W("Aura input router could not post key event code=%{public}d",
            *raw_key_code);
  }

  const std::string* key_text = event->FindString("keyText");
  const std::u16string text =
      key_text ? base::UTF8ToUTF16(*key_text) : std::u16string();
  const bool has_composed_text = key_code == ui::VKEY_UNKNOWN && key_text &&
                                 !key_text->empty() &&
                                 key_text->rfind("KEYCODE_", 0) != 0;
  if (posted && pressed && has_composed_text && !text.empty() &&
      !(event_flags_ &
        (ui::EF_CONTROL_DOWN | ui::EF_ALT_DOWN | ui::EF_COMMAND_DOWN))) {
    for (char16_t character : text) {
      posted =
          ui::OhosEventSource::PostEvent(
              std::make_unique<ui::KeyEvent>(ui::KeyEvent::FromCharacter(
                  character, key_code, ui::DomCode::NONE, event_flags_))) &&
          posted;
    }
  }
  return posted;
}

}  // namespace ohos_nweb
