#include "ui/ozone/platform/ohos/ohos_screen.h"

#include <window_manager/oh_display_info.h>
#include <window_manager/oh_display_manager.h>

#include <string>

#include "components/viz/common/resources/shared_image_format.h"
#include "ui/gfx/display_color_spaces.h"

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/synchronization/lock.h"
#include "base/task/single_thread_task_runner.h"
#include "ui/ozone/platform/ohos/ohos_native_window_registry.h"

namespace ui {

namespace {

constexpr int64_t kOhosPrimaryDisplayId = 1;
constexpr float kOhosPrimaryDisplayScale = 1.0f;
constexpr gfx::Size kOhosPrimaryDisplaySize(1920, 1080);

// The display manager reports how far the screen is turned clockwise from its
// natural orientation. Passed through as the panel rotation, which is what
// screen.orientation.angle and the orientation type are computed from; the
// display's own bounds already follow the rotation.
// What the panel can show, as Chromium's compositor asks the question.
//
// On a panel with HDR, HDR content is output as Rec. 2020 PQ and everything
// else stays sRGB, and an opaque window is drawn in RGBA_1010102 whatever it
// holds: the GL surface factory gives such a window a 10-bit EGL config
// (LogTenBitConfigs lists what there is), and Skia takes the format given
// here as what that framebuffer is. 8-bit PQ is a quarter of the precision
// HDR10 is defined with, and SDR content drawn into PQ -- the whole page
// while HDR video plays -- has only ~150 of 256 codes to spend. A window
// with alpha stays RGBA_8888: two bits of alpha is not enough to round a
// popup's corners, and the factory keeps those windows 8-bit.
gfx::DisplayColorSpaces ReadDisplayColorSpaces() {
  const bool hdr = OhosDisplaySupportsHdr();
  gfx::DisplayColorSpaces color_spaces(gfx::ColorSpace::CreateSRGB());
  if (!hdr) {
    return color_spaces;
  }

  const gfx::ColorSpace srgb = gfx::ColorSpace::CreateSRGB();
  const gfx::ColorSpace hdr_space = gfx::ColorSpace::CreateHDR10();
  const viz::SharedImageFormat ten_bit = viz::SinglePlaneFormat::kRGBA_1010102;
  const viz::SharedImageFormat eight_bit = viz::SinglePlaneFormat::kRGBA_8888;
  for (gfx::ContentColorUsage usage :
       {gfx::ContentColorUsage::kSRGB, gfx::ContentColorUsage::kWideColorGamut,
        gfx::ContentColorUsage::kHDR}) {
    const gfx::ColorSpace& space =
        usage == gfx::ContentColorUsage::kHDR ? hdr_space : srgb;
    color_spaces.SetOutputColorSpaceAndFormat(usage, /*needs_alpha=*/false,
                                              space, ten_bit);
    color_spaces.SetOutputColorSpaceAndFormat(usage, /*needs_alpha=*/true,
                                              space, eight_bit);
  }
  return color_spaces;
}

display::Display::Rotation ReadDisplayRotation() {
  NativeDisplayManager_Rotation rotation = DISPLAY_MANAGER_ROTATION_0;
  if (OH_NativeDisplayManager_GetDefaultDisplayRotation(&rotation) !=
      DISPLAY_MANAGER_OK) {
    return display::Display::ROTATE_0;
  }
  switch (rotation) {
    case DISPLAY_MANAGER_ROTATION_90:
      return display::Display::ROTATE_90;
    case DISPLAY_MANAGER_ROTATION_180:
      return display::Display::ROTATE_180;
    case DISPLAY_MANAGER_ROTATION_270:
      return display::Display::ROTATE_270;
    default:
      return display::Display::ROTATE_0;
  }
}

// The display change listener takes a bare function with no user data, and
// runs on a display manager thread.
struct DisplayChangeRelay {
  base::Lock lock;
  scoped_refptr<base::SingleThreadTaskRunner> task_runner;
  base::WeakPtr<OhosScreen> screen;
};

DisplayChangeRelay& GetDisplayChangeRelay() {
  static base::NoDestructor<DisplayChangeRelay> relay;
  return *relay;
}

}  // namespace

bool OhosDisplaySupportsHdr() {
  NativeDisplayManager_DisplayInfo* info = nullptr;
  if (OH_NativeDisplayManager_CreateDisplayById(0, &info) !=
          DISPLAY_MANAGER_OK ||
      !info) {
    return false;
  }

  bool hdr = false;
  std::string formats;
  if (info->hdrFormat && info->hdrFormat->hdrFormats) {
    for (uint32_t i = 0; i < info->hdrFormat->hdrFormatLength; ++i) {
      formats += " " + base::NumberToString(info->hdrFormat->hdrFormats[i]);
      // Any format beyond "none" means the panel can show more than SDR.
      if (info->hdrFormat->hdrFormats[i] != 0) {
        hdr = true;
      }
    }
  }
  OH_NativeDisplayManager_DestroyDisplay(info);

  // Asked from the UI thread and the GPU thread alike.
  static base::NoDestructor<base::Lock> lock;
  static base::NoDestructor<std::string> last_said;
  const std::string said =
      "panel HDR formats [" + formats + " ], " +
      (hdr ? "HDR content is output as Rec. 2020 PQ, opaque windows in "
             "RGBA_1010102"
           : "HDR content is tone-mapped to sRGB");
  base::AutoLock hold(*lock);
  if (*last_said != said) {
    *last_said = said;
    LOG(WARNING) << "OHOS display: " << said;
  }
  return hdr;
}

OhosScreen::OhosScreen() {
  float scale = kOhosPrimaryDisplayScale;
  gfx::Rect bounds(kOhosPrimaryDisplaySize);
  if (std::optional<OhosDisplayMetrics> metrics = GetOhosDisplayMetrics()) {
    scale = metrics->density;
    bounds = gfx::Rect(metrics->pixel_size);
  }

  pixel_size_ = bounds.size();
  density_ = scale;
  display::Display display(kOhosPrimaryDisplayId);
  display.SetScaleAndBounds(scale, bounds);
  const display::Display::Rotation rotation = ReadDisplayRotation();
  display.set_rotation(rotation);
  display.set_panel_rotation(rotation);
  display.SetColorSpaces(ReadDisplayColorSpaces());
  uint32_t refresh_rate = 0;
  if (OH_NativeDisplayManager_GetDefaultDisplayRefreshRate(
          &refresh_rate) == DISPLAY_MANAGER_OK &&
      refresh_rate > 0) {
    display.set_display_frequency(static_cast<float>(refresh_rate));
  }
  display_list_.AddDisplay(display, display::DisplayList::Type::PRIMARY);

  // A 180 degree turn changes neither size nor density, so the metrics
  // callback below never sees it.
  {
    DisplayChangeRelay& relay = GetDisplayChangeRelay();
    base::AutoLock lock(relay.lock);
    relay.task_runner = base::SingleThreadTaskRunner::GetCurrentDefault();
    relay.screen = weak_factory_.GetWeakPtr();
  }
  display_listener_registered_ =
      OH_NativeDisplayManager_RegisterDisplayChangeListener(
          [](uint64_t display_id) {
            DisplayChangeRelay& relay = GetDisplayChangeRelay();
            base::AutoLock lock(relay.lock);
            if (relay.task_runner) {
              relay.task_runner->PostTask(
                  FROM_HERE,
                  base::BindOnce(&OhosScreen::OnDisplayChanged, relay.screen));
            }
          },
          &display_listener_index_) == DISPLAY_MANAGER_OK;

  SetOhosDisplayMetricsChangedCallback(base::BindRepeating(
      [](scoped_refptr<base::SingleThreadTaskRunner> task_runner,
         base::WeakPtr<OhosScreen> screen, gfx::Size pixel_size,
         float density) {
        task_runner->PostTask(
            FROM_HERE, base::BindOnce(&OhosScreen::OnDisplayMetricsChanged,
                                      screen, pixel_size, density));
      },
      base::SingleThreadTaskRunner::GetCurrentDefault(),
      weak_factory_.GetWeakPtr()));
}

OhosScreen::~OhosScreen() {
  SetOhosDisplayMetricsChangedCallback({});
  if (display_listener_registered_) {
    OH_NativeDisplayManager_UnregisterDisplayChangeListener(
        display_listener_index_);
  }
  DisplayChangeRelay& relay = GetDisplayChangeRelay();
  base::AutoLock lock(relay.lock);
  relay.task_runner = nullptr;
}

const std::vector<display::Display>& OhosScreen::GetAllDisplays() const {
  return display_list_.displays();
}

display::Display OhosScreen::GetPrimaryDisplay() const {
  auto iter = display_list_.GetPrimaryDisplayIterator();
  DCHECK(iter != display_list_.displays().end());
  return *iter;
}

display::Display OhosScreen::GetDisplayForAcceleratedWidget(
    gfx::AcceleratedWidget widget) const {
  (void)widget;
  return GetPrimaryDisplayWithSurfaceWorkArea();
}

gfx::Point OhosScreen::GetCursorScreenPoint() const {
  return GetOhosCursorScreenPoint();
}

gfx::AcceleratedWidget OhosScreen::GetAcceleratedWidgetAtScreenPoint(
    const gfx::Point& point) const {
  return GetOhosAcceleratedWidgetAtScreenPoint(point);
}

display::Display OhosScreen::GetDisplayNearestPoint(
    const gfx::Point& point) const {
  (void)point;
  return GetPrimaryDisplayWithSurfaceWorkArea();
}

display::Display OhosScreen::GetDisplayMatching(
    const gfx::Rect& match_rect) const {
  (void)match_rect;
  return GetPrimaryDisplayWithSurfaceWorkArea();
}

void OhosScreen::AddObserver(display::DisplayObserver* observer) {
  display_list_.AddObserver(observer);
}

void OhosScreen::RemoveObserver(display::DisplayObserver* observer) {
  display_list_.RemoveObserver(observer);
}

display::Display OhosScreen::GetPrimaryDisplayWithSurfaceWorkArea() const {
  display::Display display = GetPrimaryDisplay();
  const std::optional<OhosNativeSurface> surface =
      GetPrimaryOhosNativeSurface();
  if (!surface || surface->bounds.IsEmpty()) {
    return display;
  }

  // Aura child widgets share the XComponent compositor and cannot draw beyond
  // its surface. Use that surface as the popup work area so menus are shifted
  // or scrolled before their lower rows would be clipped by the ArkUI window.
  const float scale = display.device_scale_factor();
  if (scale <= 0.0f) {
    return display;
  }
  gfx::Rect work_area = gfx::ScaleToEnclosedRect(surface->bounds, 1.0f / scale);
  work_area.Intersect(display.bounds());
  if (!work_area.IsEmpty()) {
    display.set_work_area(work_area);
  }
  return display;
}

void OhosScreen::OnDisplayMetricsChanged(gfx::Size pixel_size, float density) {
  if (pixel_size.IsEmpty() || density <= 0.0f) {
    return;
  }
  pixel_size_ = pixel_size;
  density_ = density;
  UpdatePrimaryDisplay();
}

void OhosScreen::OnDisplayChanged() {
  // A rotation reaches this listener before the window metrics callback
  // reports the new size. Publishing the new rotation with the old size told
  // pages "portrait-primary, 270 degrees" after a landscape lock, and the
  // lock never resolved; read the size the display has now.
  int32_t width = 0;
  int32_t height = 0;
  if (OH_NativeDisplayManager_GetDefaultDisplayWidth(&width) ==
          DISPLAY_MANAGER_OK &&
      OH_NativeDisplayManager_GetDefaultDisplayHeight(&height) ==
          DISPLAY_MANAGER_OK &&
      width > 0 && height > 0) {
    pixel_size_ = gfx::Size(width, height);
  }
  UpdatePrimaryDisplay();
}

void OhosScreen::UpdatePrimaryDisplay() {
  display::Display display = GetPrimaryDisplay();
  display.SetScaleAndBounds(density_, gfx::Rect(pixel_size_));
  const display::Display::Rotation rotation = ReadDisplayRotation();
  display.set_rotation(rotation);
  display.set_panel_rotation(rotation);
  display_list_.UpdateDisplay(display, display::DisplayList::Type::PRIMARY);
}

}  // namespace ui
