#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <random>
#include "ui/ozone/platform/ohos/ohos_overlay_geometry.h"

using namespace ui::ohos_overlay;
void Near(float actual, float expected) {
  assert(std::abs(actual - expected) < 0.001f);
}
int main() {
  const OhosOverlayRect primary{0, 0, 1316, 2832};
  const OhosOverlayRect source{0, 0, 640, 360};
  const OhosOverlayRect fractional{0.25f, 492.875f, 800.5f, 450.25f};
  auto unchanged = ClipVideoGeometry(primary, fractional, source, std::nullopt);
  assert(unchanged);
  Near(unchanged->destination.x, fractional.x);
  Near(unchanged->destination.y, fractional.y);
  Near(unchanged->destination.width, fractional.width);
  Near(unchanged->source.width, 640);
  Near(unchanged->source.height, 360);

  // Bilibili's measured width extends beyond the 1316-pixel root surface.
  auto bili = ClipVideoGeometry(primary, {0, 492.875f, 1320, 742.5f}, source,
                                std::nullopt);
  assert(bili);
  Near(bili->destination.width, 1316);
  Near(bili->source.width, 640.f * 1316 / 1320);
  Near(bili->source.height, 360);

  // Clip all four edges, including an existing non-full source crop.
  auto clipped =
      ClipVideoGeometry({0, 0, 100, 100}, {-20, -10, 200, 200},
                        {10, 20, 400, 600}, OhosOverlayRect{5, 5, 90, 90});
  assert(clipped);
  Near(clipped->destination.x, 5);
  Near(clipped->destination.y, 5);
  Near(clipped->destination.width, 90);
  Near(clipped->destination.height, 90);
  Near(clipped->source.x, 60);
  Near(clipped->source.y, 65);
  Near(clipped->source.width, 180);
  Near(clipped->source.height, 270);

  for (const auto invalid :
       {OhosOverlayRect{1316, 0, 10, 10}, OhosOverlayRect{0, 0, 0, 10},
        OhosOverlayRect{0, 0, -10, 10}, OhosOverlayRect{0, 0, INFINITY, 10},
        OhosOverlayRect{NAN, 0, 10, 10}})
    assert(!ClipVideoGeometry(primary, invalid, source, std::nullopt));
  assert(!ClipVideoGeometry(primary, fractional, source,
                            OhosOverlayRect{0, 0, 0, 0}));
  assert(!ClipVideoGeometry(primary, fractional, {0, 0, NAN, 1}, std::nullopt));

  // Destination clipping must preserve the affine mapping for arbitrary
  // fractional geometry, with output contained by both input rectangles.
  std::mt19937 rng(37);
  std::uniform_real_distribution<float> position(-600, 1800), extent(1, 2000);
  for (int n = 0; n < 10000; ++n) {
    OhosOverlayRect d{position(rng), position(rng), extent(rng), extent(rng)};
    auto result = ClipVideoGeometry(primary, d, source, std::nullopt);
    if (!result)
      continue;
    const auto& out = result->destination;
    const auto& src = result->source;
    assert(out.x >= 0 && out.y >= 0 && out.x + out.width <= 1316 &&
           out.y + out.height <= 2832);
    assert(src.x >= 0 && src.y >= 0 && src.x + src.width <= 640 &&
           src.y + src.height <= 360);
    Near(src.x, (out.x - d.x) * 640 / d.width);
    Near(src.y, (out.y - d.y) * 360 / d.height);
    Near(src.width, out.width * 640 / d.width);
    Near(src.height, out.height * 360 / d.height);
  }
  puts(
      "PASS fractional geometry, Bilibili viewport clipping, source mapping "
      "and invalid input");
}
