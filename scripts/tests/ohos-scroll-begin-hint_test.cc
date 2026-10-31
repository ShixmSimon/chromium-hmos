#include "ui/events/gesture_detection/ohos_scroll_begin_hint.h"

#include <cassert>
#include <iostream>

int main() {
  struct Case {
    float x, y, expected_x, expected_y;
  };
  // Include the sparse-move reproduction, both scroll directions, pure-axis
  // input, and genuinely diagonal gestures near the dominance boundary.
  const Case cases[] = {
      {40, 180, 0, 180}, {-40, -180, 0, -180},
      {40, -180, 0, -180}, {-40, 180, 0, 180},
      {180, 40, 180, 0}, {-180, -40, -180, 0},
      {180, -40, 180, 0}, {-180, 40, -180, 0},
      {0, 180, 0, 180}, {180, 0, 180, 0}, {0, 0, 0, 0},
      {40, 40, 40, 40}, {-40, 40, -40, 40},
      {4, 5, 4, 5}, {5, 4, 5, 4},
      {4, 5.01f, 0, 5.01f}, {5.01f, 4, 5.01f, 0},
  };
  for (const auto& c : cases) {
    const auto hint = ui::internal::GetOhosScrollBeginHint(c.x, c.y);
    assert(hint.x == c.expected_x);
    assert(hint.y == c.expected_y);
  }
  std::cout << "PASS: 17 initial scroll-target direction cases\n";
}
