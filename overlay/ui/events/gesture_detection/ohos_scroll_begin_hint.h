// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_EVENTS_GESTURE_DETECTION_OHOS_SCROLL_BEGIN_HINT_H_
#define UI_EVENTS_GESTURE_DETECTION_OHOS_SCROLL_BEGIN_HINT_H_

#include <cmath>

namespace ui::internal {

struct OhosScrollBeginHint {
  float x;
  float y;
};

// Coalesced initial moves can exceed SnapScrollController's narrow starting
// channel on both axes, even for a mostly vertical drag. A nonzero horizontal
// hint then latches a horizontal-only descendant before its vertical parent.
// Match SnapScrollController's dominance ratio when choosing the initial
// scroller. The actual scroll updates retain both deltas for two-axis content.
inline OhosScrollBeginHint GetOhosScrollBeginHint(float x, float y) {
  constexpr float kMinDominanceRatio = 1.25f;
  if (std::abs(y) > std::abs(x) * kMinDominanceRatio) {
    x = 0;
  } else if (std::abs(x) > std::abs(y) * kMinDominanceRatio) {
    y = 0;
  }
  return {x, y};
}

}  // namespace ui::internal

#endif  // UI_EVENTS_GESTURE_DETECTION_OHOS_SCROLL_BEGIN_HINT_H_
