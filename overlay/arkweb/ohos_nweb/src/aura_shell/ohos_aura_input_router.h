// Copyright (c) 2026
// Licensed under the Apache License, Version 2.0.

#ifndef OHOS_NWEB_SRC_AURA_SHELL_OHOS_AURA_INPUT_ROUTER_H_
#define OHOS_NWEB_SRC_AURA_SHELL_OHOS_AURA_INPUT_ROUTER_H_

#include <cstdint>
#include <deque>
#include <string>

#include "base/time/time.h"
#include "ui/gfx/native_ui_types.h"

namespace ohos_nweb {

struct OhosNativeTouchEvent {
  int action = -1;
  int pointer_id = -1;
  float x = 0.0f;
  float y = 0.0f;
  float root_x = 0.0f;
  float root_y = 0.0f;
  int64_t timestamp_ns = 0;
  gfx::AcceleratedWidget target_widget = gfx::kNullAcceleratedWidget;
};

class OhosAuraInputRouter {
 public:
  explicit OhosAuraInputRouter(std::string component_id = {});
  OhosAuraInputRouter(const OhosAuraInputRouter&) = delete;
  OhosAuraInputRouter& operator=(const OhosAuraInputRouter&) = delete;
  ~OhosAuraInputRouter();

  void SetFocused(bool focused);
  void ResetPointerState();
  void DispatchNativeTouchEvent(const OhosNativeTouchEvent& event);
  void DispatchPointerEvent(const std::string& event_json);
  bool DispatchKeyEvent(const std::string& event_json);

  bool focused_for_testing() const { return focused_; }
  int event_flags_for_testing() const { return event_flags_; }
  int mouse_button_flags_for_testing() const { return mouse_button_flags_; }
  double last_pointer_x_for_testing() const { return last_pointer_x_; }
  double last_pointer_y_for_testing() const { return last_pointer_y_; }
  double last_pointer_root_x_for_testing() const {
    return last_pointer_root_x_;
  }
  double last_pointer_root_y_for_testing() const {
    return last_pointer_root_y_;
  }
  int suppressed_pointer_events_for_testing() const {
    return suppressed_pointer_events_;
  }
  int suppressed_key_events_for_testing() const {
    return suppressed_key_events_;
  }
  int last_mouse_event_flags_for_testing() const {
    return last_mouse_event_flags_;
  }
  int last_mouse_changed_button_flags_for_testing() const {
    return last_mouse_changed_button_flags_;
  }
  base::TimeTicks last_touch_timestamp_for_testing() const {
    return last_touch_timestamp_;
  }

 private:
  struct PointerDispatchRecord {
    std::string pointer_type;
    std::string dispatch_source;
    int action = -1;
    int pointer_id = -1;
    int button = -1;
    double x = 0.0;
    double y = 0.0;
    base::TimeTicks time;
    bool matched = false;
  };

  struct KeyDispatchRecord {
    std::string dispatch_source;
    int action = -1;
    int key_code = -1;
    base::TimeTicks time;
    bool matched = false;
  };

  bool IsDuplicatePointerEvent(const std::string& pointer_type,
                               const std::string& dispatch_source,
                               int action,
                               int pointer_id,
                               int button,
                               double x,
                               double y);
  bool IsDuplicateKeyEvent(const std::string& dispatch_source,
                           int action,
                           int key_code);
  gfx::AcceleratedWidget GetBoundWidget() const;

  const std::string component_id_;
  bool focused_ = false;
  int event_flags_ = 0;
  int mouse_button_flags_ = 0;
  std::string last_pointer_type_;
  std::string last_pointer_dispatch_source_;
  int last_pointer_action_ = -1;
  int last_pointer_id_ = -1;
  int last_pointer_button_ = -1;
  double last_pointer_x_ = 0.0;
  double last_pointer_y_ = 0.0;
  double last_pointer_root_x_ = 0.0;
  double last_pointer_root_y_ = 0.0;
  base::TimeTicks last_pointer_time_;
  std::deque<PointerDispatchRecord> recent_pointer_events_;
  std::deque<KeyDispatchRecord> recent_key_events_;
  int suppressed_pointer_events_ = 0;
  int suppressed_key_events_ = 0;
  int last_mouse_event_flags_ = 0;
  int last_mouse_changed_button_flags_ = 0;
  base::TimeTicks last_touch_timestamp_;
};

}  // namespace ohos_nweb

#endif  // OHOS_NWEB_SRC_AURA_SHELL_OHOS_AURA_INPUT_ROUTER_H_
