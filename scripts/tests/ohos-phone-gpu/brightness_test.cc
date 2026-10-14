#include <cassert>
#include <limits>
#include "ui/ozone/platform/ohos/ohos_display_brightness.h"
int main() {
  auto value = ui::MakeOhosDisplayBrightness(7, 120, 3, 5);
  assert(value && value->display_id == 7);
  assert(value->hdr.white_millinits == 120000 &&
         value->hdr.peak_millinits == 360000);
  assert(ui::ValidOhosOutputColor(value->hdr));
  // Current headroom, not the hardware maximum, controls this frame's peak.
  auto dimmed = ui::MakeOhosDisplayBrightness(7, 12.5, 1, 5);
  assert(dimmed && dimmed->hdr.white_millinits == 12500);
  assert(dimmed->hdr.peak_millinits == 12500 &&
         ui::ValidOhosOutputColor(dimmed->hdr));
  auto capped = ui::MakeOhosDisplayBrightness(7, 200, 10, 4);
  assert(capped && capped->hdr.peak_millinits == 800000);
  auto pq = ui::MakeOhosDisplayBrightness(7, 1000, 20, 20);
  assert(pq && pq->hdr.peak_millinits == 10000000);
  // SDR -> HDR -> SDR uses a fresh capability snapshot for each frame; an
  // SDR frame must not destroy the headroom needed by subsequent HDR content.
  const auto display = ui::MakeOhosDisplayBrightness(7, 284.736, 4, 5)->hdr;
  for (bool hdr : {false, true, false}) {
    const auto frame = ui::OhosOutputColorForContent(display, hdr);
    assert(ui::ValidOhosOutputColor(frame));
    assert(frame.mode == ui::OhosOutputMode::kHdr10);
    assert(frame.white_millinits == display.white_millinits);
    assert(frame.peak_millinits == (hdr ? 1138944u : 284736u));
    assert(ui::OhosOutputNativeFormat(frame) == 34);
  }
  // A system headroom change must not change SDR frame metadata when SDR
  // white stays fixed, but it must reach HDR frames without a restart.
  const auto brighter = ui::MakeOhosDisplayBrightness(7, 284.736, 5, 5)->hdr;
  assert(ui::OhosOutputColorForContent(display, false) ==
         ui::OhosOutputColorForContent(brighter, false));
  assert(ui::OhosOutputColorForContent(display, true) !=
         ui::OhosOutputColorForContent(brighter, true));
  for (auto mode : {ui::OhosOutputMode::kSdr8, ui::OhosOutputMode::kSdr10}) {
    const ui::OhosOutputColor sdr{mode};
    assert(ui::OhosOutputColorForContent(sdr, false) == sdr);
    assert(ui::OhosOutputColorForContent(sdr, true) == sdr);
  }
  for (double invalid : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                         std::numeric_limits<double>::quiet_NaN()}) {
    assert(!ui::MakeOhosDisplayBrightness(0, invalid, 2, 4));
    assert(!ui::MakeOhosDisplayBrightness(0, 100, invalid, 4));
    assert(!ui::MakeOhosDisplayBrightness(0, 100, 2, invalid));
  }
}
