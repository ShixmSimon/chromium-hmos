#include <cassert>
#include <limits>
#include "ui/ozone/platform/ohos/ohos_display_brightness.h"
int main() {
  auto value = ui::MakeOhosDisplayBrightness(7, 120, 3, 5);
  assert(value && value->display_id == 7);
  assert(value->hdr.white_millinits == 120000 && value->hdr.peak_millinits == 360000);
  assert(ui::ValidOhosOutputColor(value->hdr));
  // Current headroom, not the hardware maximum, controls this frame's peak.
  auto dimmed = ui::MakeOhosDisplayBrightness(7, 12.5, 1, 5);
  assert(dimmed && dimmed->hdr.white_millinits == 12500);
  assert(dimmed->hdr.peak_millinits == 12500 && ui::ValidOhosOutputColor(dimmed->hdr));
  auto capped = ui::MakeOhosDisplayBrightness(7, 200, 10, 4);
  assert(capped && capped->hdr.peak_millinits == 800000);
  auto pq = ui::MakeOhosDisplayBrightness(7, 1000, 20, 20);
  assert(pq && pq->hdr.peak_millinits == 10000000);
  for (double invalid : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                         std::numeric_limits<double>::quiet_NaN()}) {
    assert(!ui::MakeOhosDisplayBrightness(0, invalid, 2, 4));
    assert(!ui::MakeOhosDisplayBrightness(0, 100, invalid, 4));
    assert(!ui::MakeOhosDisplayBrightness(0, 100, 2, invalid));
  }
}
