#include "ui/ozone/platform/ohos/ohos_surface_factory.h"

#include <optional>
#include <map>
#include <cstring>
#include <utility>
#include <unistd.h>
#include "base/functional/callback_helpers.h"
#include "base/threading/watchdog.h"
#include <native_buffer/native_buffer.h>
#include "base/files/scoped_file.h"
#include "ui/ozone/platform/ohos/ohos_phone_gpu_shared_buffer.h"
#include <string>
#include <vector>

#if BUILDFLAG(ENABLE_VULKAN)
#include "gpu/vulkan/ohos/vulkan_implementation_ohos.h"
#include "gpu/vulkan/vulkan_image.h"
#include "gpu/vulkan/vulkan_surface.h"
#include "ui/ozone/platform/ohos/ohos_video_vulkan.h"
#endif

#include <native_window/external_window.h>
#include "base/auto_reset.h"
#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/ref_counted.h"
#include "base/strings/string_split.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "ui/gfx/color_space.h"
#include "ui/gfx/presentation_feedback.h"
#include "ui/gfx/vsync_provider.h"
#include "ui/gl/gl_bindings.h"
#include "ui/gl/gl_context.h"
#include "ui/gl/gl_display.h"
#include "ui/gl/gl_implementation.h"
#include "ui/gl/gl_surface_egl.h"
#include "ui/gl/gl_version_info.h"
#include "ui/ozone/common/egl_util.h"
#include "ui/ozone/common/gl_ozone_egl.h"

#include "ui/ozone/platform/ohos/ohos_native_pixmap.h"
#include "ui/ozone/platform/ohos/ohos_native_window_registry.h"
#include "ui/ozone/platform/ohos/ohos_phone_gpu_channel.h"
#include "ui/ozone/platform/ohos/ohos_gpu_child_channel.h"
#include "ui/ozone/platform/ohos/ohos_output_color_config.h"
#include "ui/ozone/platform/ohos/ohos_screen.h"
#include "ui/ozone/platform/ohos/ohos_vsync_provider.h"

namespace ui {
namespace {

constexpr base::TimeDelta kNativeSurfaceWaitTimeout = base::Seconds(3);

bool IsOffscreenNativeFormat(viz::SharedImageFormat format) {
  return format == viz::SinglePlaneFormat::kRGBA_8888 ||
         format == viz::SinglePlaneFormat::kBGRA_8888 ||
         format == viz::SinglePlaneFormat::kRGBA_F16;
}

// OH_NativeBuffer_Format's RGBA_8888 and RGBA_1010102. Taking the enum by
// value keeps native_buffer out of this target's headers for two constants.
constexpr int32_t kPixelFormatRgba8888 = 12;
constexpr int32_t kPixelFormatRgba1010102 = 34;

// Views asks for a translucent window for anything with a rounded corner or a
// shadow -- an autofill list, a <select> menu, a bubble -- and paints the area
// outside the rounded rectangle fully transparent, expecting the window system
// to composite it away. A SURFACE XComponent's buffer defaults to a format
// with no alpha, so those pixels came out black: the popup showed as a rounded
// card inside a black rectangle. Ask for a buffer that can carry the alpha
// Chromium is painting.
void RequestAlphaCapableBuffer(void* window) {
  if (!window) {
    return;
  }
  auto* native_window = reinterpret_cast<OHNativeWindow*>(window);
  const int32_t result = OH_NativeWindow_NativeWindowHandleOpt(
      native_window, SET_FORMAT, kPixelFormatRgba8888);
  if (result != 0) {
    LOG(WARNING) << "OHOS surface: could not ask for an alpha buffer: "
                 << result;
  }
}

// What a 10-bit output surface can be made from, beside what
// ChooseTenBitWindowConfig picks from it: every config ANGLE (and the driver
// under it) lists with 10 or more bits of red, whether it can back a window,
// and the display's colour-space and pixel-format extensions. Once per
// process.
void LogTenBitConfigs(EGLDisplay display) {
  static bool logged = false;
  if (logged || display == EGL_NO_DISPLAY) {
    return;
  }
  logged = true;

  EGLint count = 0;
  if (!eglGetConfigs(display, nullptr, 0, &count) || count <= 0) {
    LOG(WARNING) << "OHOS EGL configs: none listed";
    return;
  }
  std::vector<EGLConfig> configs(count);
  eglGetConfigs(display, configs.data(), count, &count);
  configs.resize(count);

  auto attrib = [display](EGLConfig config, EGLint name) {
    EGLint value = 0;
    eglGetConfigAttrib(display, config, name, &value);
    return value;
  };
  int ten_bit = 0;
  for (EGLConfig config : configs) {
    const EGLint red = attrib(config, EGL_RED_SIZE);
    if (red < 10) {
      continue;
    }
    ++ten_bit;
    const EGLint surface_type = attrib(config, EGL_SURFACE_TYPE);
    const EGLint renderable = attrib(config, EGL_RENDERABLE_TYPE);
    LOG(WARNING) << "OHOS EGL configs: id " << attrib(config, EGL_CONFIG_ID)
                 << " RGBA " << red << "/" << attrib(config, EGL_GREEN_SIZE)
                 << "/" << attrib(config, EGL_BLUE_SIZE) << "/"
                 << attrib(config, EGL_ALPHA_SIZE) << ", window "
                 << ((surface_type & EGL_WINDOW_BIT) ? "yes" : "no")
                 << ", ES3 "
                 << ((renderable & EGL_OPENGL_ES3_BIT) ? "yes" : "no")
                 << ", native visual " << attrib(config, EGL_NATIVE_VISUAL_ID)
                 << ", depth " << attrib(config, EGL_DEPTH_SIZE)
                 << ", stencil " << attrib(config, EGL_STENCIL_SIZE);
  }

  std::string extensions;
  if (const char* all = eglQueryString(display, EGL_EXTENSIONS)) {
    for (const std::string& extension :
         base::SplitString(all, " ", base::TRIM_WHITESPACE,
                           base::SPLIT_WANT_NONEMPTY)) {
      if (extension.find("colorspace") != std::string::npos ||
          extension.find("pixel_format") != std::string::npos) {
        extensions += " " + extension;
      }
    }
  }
  LOG(WARNING) << "OHOS EGL configs: " << ten_bit << " of " << count
               << " have 10+ bits of red; colour-space extensions ["
               << extensions << " ]";
}

// A window config with exactly 10 bits per colour and 2 of alpha, the
// layout of RGBA_1010102, without depth or stencil if there is one: the
// compositor draws its output into the default framebuffer with neither,
// and Chromium's own 8-bit choice takes the smallest depth there is too.
EGLConfig ChooseTenBitWindowConfig(EGLDisplay display) {
  const EGLint attributes[] = {EGL_RED_SIZE,        10,
                               EGL_GREEN_SIZE,      10,
                               EGL_BLUE_SIZE,       10,
                               EGL_ALPHA_SIZE,      2,
                               EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
                               EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                               EGL_NONE};
  EGLint count = 0;
  if (!eglChooseConfig(display, attributes, nullptr, 0, &count) ||
      count <= 0) {
    return nullptr;
  }
  std::vector<EGLConfig> configs(count);
  if (!eglChooseConfig(display, attributes, configs.data(), count, &count)) {
    return nullptr;
  }
  configs.resize(count);

  auto attrib = [display](EGLConfig config, EGLint name) {
    EGLint value = 0;
    eglGetConfigAttrib(display, config, name, &value);
    return value;
  };
  EGLConfig chosen = nullptr;
  for (EGLConfig config : configs) {
    // eglChooseConfig takes these sizes as minimums, and sorts deeper
    // colour first: RGBA16F would come back as well.
    if (attrib(config, EGL_RED_SIZE) != 10 ||
        attrib(config, EGL_GREEN_SIZE) != 10 ||
        attrib(config, EGL_BLUE_SIZE) != 10 ||
        attrib(config, EGL_ALPHA_SIZE) != 2) {
      continue;
    }
    if (attrib(config, EGL_DEPTH_SIZE) == 0 &&
        attrib(config, EGL_STENCIL_SIZE) == 0) {
      return config;
    }
    if (!chosen) {
      chosen = config;
    }
  }
  return chosen;
}

class OhosNativeViewGLSurfaceEGL final : public gl::NativeViewGLSurfaceEGL {
 public:
  OhosNativeViewGLSurfaceEGL(gl::GLDisplayEGL* display,
                             gfx::AcceleratedWidget widget,
                             EGLNativeWindowType window,
                             std::unique_ptr<gfx::VSyncProvider> vsync_provider)
      : gl::NativeViewGLSurfaceEGL(display, window, std::move(vsync_provider)),
        widget_(widget),
        angle_vulkan_(base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
                          "use-angle") == "vulkan"),
        sdr_output_(OhosRequiresSdrOutput()),
        hdr_panel_(!sdr_output_ && OhosDisplaySupportsHdr()),
        // Whether the window has alpha is only known at the first Resize;
        // until then, a popup is the window that draws it.
        ten_bit_((hdr_panel_ ||
                  (angle_vulkan_ &&
                   !base::CommandLine::ForCurrentProcess()->HasSwitch("ohos-sdr-output") &&
                   GetOhosRequestedOutputColor().mode != OhosOutputMode::kSdr8)) &&
                 !IsOhosAnchoredWindow(widget)) {
    HoldWindow(window);
  }

  // The EGL surface goes first, then the window it was made on.
  ~OhosNativeViewGLSurfaceEGL() override {
    if (angle_vulkan_) {
      ReportOhosWsiOutputCapabilities(widget_, false, false);
      SetOhosEglOutputColorSpaces(static_cast<const gl::GLSurface*>(this), std::nullopt);
    }
    Destroy();
    HoldWindow(0);
  }

  // On an HDR panel the screen tells the compositor an opaque window is
  // RGBA_1010102 (ReadDisplayColorSpaces), and Skia takes that as what the
  // default framebuffer is, so an opaque window gets a 10-bit config to
  // match. A window with alpha -- a popup's rounded corners and shadow --
  // needs more than two bits of it and stays 8-bit, as the screen says too.
  // Chromium's own choice asks for 8 bits per channel.
  EGLConfig GetConfig() override {
    if (!config_ && ten_bit_) {
      config_ = ChooseTenBitWindowConfig(display_->GetDisplay());
      if (!config_) {
        ten_bit_ = false;
        ten_bit_unavailable_ = true;
      }
      LogConfig(config_ ? "10-bit" : "8-bit (no 10-bit window config)");
    }
    return gl::NativeViewGLSurfaceEGL::GetConfig();
  }

  // A 10-bit config the driver lists but cannot make a window from must not
  // cost the window: viz takes a failed view surface as GPU compositing
  // being broken and falls back to software, which reaches no screen here.
  EGLint GetEGLColorSpace() const override {
    if (!angle_vulkan_)
      return gl::NativeViewGLSurfaceEGL::GetEGLColorSpace();
    if (output_color_.mode == OhosOutputMode::kHdr10)
      return EGL_GL_COLORSPACE_BT2020_PQ_EXT;
    if (output_color_.mode == OhosOutputMode::kP3Sdr10)
      return EGL_GL_COLORSPACE_DISPLAY_P3_PASSTHROUGH_EXT;
    return EGL_GL_COLORSPACE_LINEAR_KHR;
  }

  bool Initialize(gl::GLSurfaceFormat format) override {
    GetConfig();
    if (angle_vulkan_ && ten_bit_ && !probed_) {
      probed_ = true;
      // No GL context is current during initialization/recreation. Probe real
      // swapchains on this window, never infer support from the panel alone.
      const char* raw_extensions =
          eglQueryString(display_->GetDisplay(), EGL_EXTENSIONS);
      const std::string extensions = raw_extensions ? raw_extensions : "";
      const auto has = [&](const char* extension) {
        return (" " + extensions + " ").find(
                   " " + std::string(extension) + " ") != std::string::npos;
      };
      const auto probe = [&](OhosOutputColor color) {
        output_color_ = color;
        const bool ok = gl::NativeViewGLSurfaceEGL::Initialize(format);
        LOG(WARNING) << "OHOS EGL output probe: window=" << widget_
                     << " mode=" << static_cast<uint32_t>(color.mode)
                     << " allocated=" << ok;
        Destroy();
        return ok;
      };
      p3_capable_ = has("EGL_EXT_gl_colorspace_display_p3_passthrough") &&
                    OhosSingleDisplaySupportsP3() &&
                    probe({OhosOutputMode::kP3Sdr10});
      hdr_color_ = GetOhosRequestedOutputColor();
      if (OhosDynamicOutputLuminance()) {
        if (auto brightness = GetOhosDisplayBrightness())
          hdr_color_ = OhosContentOutputColor(*brightness);
      }
      hdr_capable_ = has("EGL_EXT_gl_colorspace_bt2020_pq") &&
                     GetOhosRequestedOutputColor().mode == OhosOutputMode::kHdr10 &&
                     hdr_color_.mode == OhosOutputMode::kHdr10 &&
                     ValidOhosOutputColor(hdr_color_) &&
                     OhosSingleDisplaySupportsHdr10() && probe(hdr_color_);
      // Probes never present content. The first page must be sRGB even on an
      // HDR-capable panel; each later content transition recreates the EGL
      // surface with the corresponding immutable colorspace attribute.
      output_color_ = {OhosOutputMode::kSdr10};
    }
    if (gl::NativeViewGLSurfaceEGL::Initialize(format)) {
      initialized_ = true;
      PublishEglCapabilities();
      return true;
    }
    const auto surface = GetOhosNativeSurface(widget_);
    if (!ten_bit_ || !surface || !surface->window)
      return false;
    // A live context cannot silently switch to an incompatible 8-bit config.
    // Fail this generation; the persisted gate bounds the subsequent restart.
    if (angle_vulkan_ && initialized_) {
      FailEglOutput("live EGL colorspace recreation failed");
      return false;
    }
    ten_bit_ = false;
    ten_bit_unavailable_ = true;
    p3_capable_ = hdr_capable_ = false;
    output_color_ = {};
    config_ = nullptr;
    LogConfig("8-bit (the 10-bit window could not be created)");
    const bool ok = gl::NativeViewGLSurfaceEGL::Initialize(format);
    if (ok) {
      initialized_ = true;
      PublishEglCapabilities();
    }
    return ok;
  }

  bool InitializeNativeWindow() override {
    std::optional<OhosNativeSurface> surface = GetOhosNativeSurface(widget_);
    if (!surface || !surface->window) {
      return false;
    }
    const EGLNativeWindowType window =
        reinterpret_cast<EGLNativeWindowType>(surface->window);
    if (window != window_) {
      tagged_pq_.reset();
      tagged_color_.reset();
      original_format_.reset();
    }
    window_ = window;
    SetBufferFormat();
    return TagNativeWindowColorSpace();
  }

  bool Resize(const gfx::Size& size,
              float scale_factor,
              const gfx::ColorSpace& color_space,
              bool has_alpha) override {
    const WindowRefreshResult refresh = RefreshNativeWindow();
    if (refresh == WindowRefreshResult::kUnavailable) {
      // The shell took the window away; keep the size until it is back.
      return true;
    }
    bool color_changed = false;
    if (angle_vulkan_) {
      OhosOutputColor next{ten_bit_ ? OhosOutputMode::kSdr10
                                    : OhosOutputMode::kSdr8};
      if (color_space.GetTransferID() == gfx::ColorSpace::TransferID::PQ) {
        if (!hdr_capable_ || has_alpha || !hdr_guard_.Validate()) {
          FailEglOutput("unverified HDR EGL reshape");
          return false;
        }
        next = hdr_color_;
        if (OhosDynamicOutputLuminance()) {
          if (auto brightness = GetOhosDisplayBrightness())
            next = OhosContentOutputColor(*brightness);
        }
      } else if (color_space.GetPrimaryID() ==
                 gfx::ColorSpace::PrimaryID::P3) {
        if (!p3_capable_ || has_alpha) {
          FailEglOutput("unverified P3 EGL reshape");
          return false;
        }
        next = {OhosOutputMode::kP3Sdr10};
      }
      color_changed = next.mode != output_color_.mode;
      output_color_ = next;
    } else {
      pq_output_ = color_space.GetTransferID() ==
                   gfx::ColorSpace::TransferID::PQ;
    }
    const bool ten_bit = (angle_vulkan_ ? ten_bit_ : hdr_panel_) &&
                         !has_alpha && !ten_bit_unavailable_;
    const bool config_changed = ten_bit != ten_bit_;
    if (config_changed) {
      ten_bit_ = ten_bit;
      config_ = nullptr;
    }
    if (refresh == WindowRefreshResult::kChanged || config_changed ||
        color_changed) {
      size_ = size;
      if (!Recreate()) {
        if (angle_vulkan_ && ten_bit_)
          FailEglOutput("EGL recreation or make-current failed");
        return false;
      }
      HoldWindow(window_);
      return true;
    }
    if (!TagNativeWindowColorSpace())
      return false;
    return gl::NativeViewGLSurfaceEGL::Resize(size, scale_factor, color_space,
                                              has_alpha);
  }

  gfx::SwapResult SwapBuffers(PresentationCallback callback,
                              gfx::FrameData data) override {
    if (WindowGone()) {
      return DropFrame(std::move(callback));
    }
    if (!RefreshAndRecreateNativeWindow()) {
      std::move(callback).Run(gfx::PresentationFeedback::Failure());
      return gfx::SwapResult::SWAP_FAILED;
    }
    const auto result = gl::NativeViewGLSurfaceEGL::SwapBuffers(
        std::move(callback), std::move(data));
    if (result == gfx::SwapResult::SWAP_FAILED && angle_vulkan_ && ten_bit_)
      FailEglOutput("ten-bit EGL swap failed");
    return result;
  }

  gfx::SwapResult PostSubBuffer(int x,
                                int y,
                                int width,
                                int height,
                                PresentationCallback callback,
                                gfx::FrameData data) override {
    if (WindowGone()) {
      return DropFrame(std::move(callback));
    }
    if (!RefreshAndRecreateNativeWindow()) {
      std::move(callback).Run(gfx::PresentationFeedback::Failure());
      return gfx::SwapResult::SWAP_FAILED;
    }
    const auto result = gl::NativeViewGLSurfaceEGL::PostSubBuffer(
        x, y, width, height, std::move(callback), std::move(data));
    if (result == gfx::SwapResult::SWAP_FAILED && angle_vulkan_ && ten_bit_)
      FailEglOutput("ten-bit EGL partial swap failed");
    return result;
  }

 private:
  enum class WindowRefreshResult { kUnavailable, kUnchanged, kChanged };

  void FailEglOutput(const char* reason) {
    p3_capable_ = hdr_capable_ = false;
    ReportOhosWsiOutputCapabilities(widget_, false, false);
    SetOhosEglOutputColorSpaces(static_cast<const gl::GLSurface*>(this), std::nullopt);
    NoteOhosOutputColorFailure(reason);
  }

  void PublishEglCapabilities() {
    if (!angle_vulkan_)
      return;
    const auto format = ten_bit_ ? viz::SinglePlaneFormat::kRGBA_1010102
                                  : viz::SinglePlaneFormat::kRGBA_8888;
    gfx::DisplayColorSpaces spaces(gfx::ColorSpace::CreateSRGB());
    spaces.SetOutputFormats(format, format);
    spaces.SetHDRMaxLuminanceRelative(1.f);
    if (hdr_capable_) {
      spaces.SetSDRMaxLuminanceNits(hdr_color_.white_millinits / 1000.f);
      spaces.SetHDRMaxLuminanceRelative(
          static_cast<float>(hdr_color_.peak_millinits) /
          hdr_color_.white_millinits);
      for (bool alpha : {false, true})
        spaces.SetOutputColorSpaceAndFormat(gfx::ContentColorUsage::kHDR,
            alpha, gfx::ColorSpace::CreateHDR10(), format);
    }
    if (p3_capable_) {
      const auto p3 = gfx::ColorSpace::CreateDisplayP3D65();
      for (bool alpha : {false, true})
        spaces.SetOutputColorSpaceAndFormat(
            gfx::ContentColorUsage::kWideColorGamut, alpha, p3, format);
      spaces.SetPrimaries(gfx::DisplayColorSpaces(p3).GetPrimaries());
    }
    SetOhosEglOutputColorSpaces(static_cast<const gl::GLSurface*>(this), spaces);
    ReportOhosWsiOutputCapabilities(widget_, hdr_capable_, p3_capable_);
    LOG(WARNING) << "OHOS EGL output verified: window=" << widget_
                 << " ten_bit=" << ten_bit_ << " p3=" << p3_capable_
                 << " hdr=" << hdr_capable_;
  }

  bool TagNativeWindowColorSpace() {
    auto* native_window = reinterpret_cast<OHNativeWindow*>(window_);
    if (angle_vulkan_) {
      if (tagged_color_ && *tagged_color_ == output_color_)
        return true;
      if (!SetOhosWindowOutputColor(native_window, output_color_))
        return false;
      tagged_color_ = output_color_;
      LOG(WARNING) << "OHOS EGL native color: window=" << widget_
                   << " mode=" << static_cast<uint32_t>(output_color_.mode)
                   << " egl_colorspace=" << GetEGLColorSpace()
                   << " white_millinits=" << output_color_.white_millinits
                   << " peak_millinits=" << output_color_.peak_millinits;
      return true;
    }
    if (!native_window || (tagged_pq_ && pq_output_ == *tagged_pq_))
      return native_window != nullptr;
    tagged_pq_ = pq_output_;
    const int32_t result = OH_NativeWindow_SetColorSpace(
        native_window,
        pq_output_ ? OH_COLORSPACE_BT2020_PQ_FULL : OH_COLORSPACE_SRGB_FULL);
    uint8_t type = static_cast<uint8_t>(
        pq_output_ ? OH_VIDEO_HDR_HDR10 : OH_VIDEO_NONE);
    const int32_t metadata = OH_NativeWindow_SetMetadataValue(
        native_window, OH_HDR_METADATA_TYPE, sizeof(type), &type);
    return result == 0 && metadata == 0;
  }

  // The window's buffers have to hold what the EGL config draws: a 10-bit
  // config into the RGBA_8888 buffers a window starts with would be read
  // back as the wrong bits. Raised to RGBA_1010102 for a 10-bit config and
  // put back to what the window had when it drops to 8 bits.
  void SetBufferFormat() {
    auto* native_window = reinterpret_cast<OHNativeWindow*>(window_);
    if (!native_window) {
      return;
    }
    if (ten_bit_) {
      if (!original_format_) {
        int32_t format = kPixelFormatRgba8888;
        OH_NativeWindow_NativeWindowHandleOpt(native_window, GET_FORMAT,
                                              &format);
        original_format_ = format;
      }
      const int32_t result = OH_NativeWindow_NativeWindowHandleOpt(
          native_window, SET_FORMAT, kPixelFormatRgba1010102);
      if (result != 0) {
        LOG(WARNING) << "OHOS surface: could not ask for a 10-bit buffer: "
                     << result;
      }
      return;
    }
    if (sdr_output_ || IsOhosAnchoredWindow(widget_)) {
      RequestAlphaCapableBuffer(native_window);
    } else if (original_format_) {
      OH_NativeWindow_NativeWindowHandleOpt(native_window, SET_FORMAT,
                                            *original_format_);
    }
    original_format_.reset();
  }

  void LogConfig(const char* what) {
    LOG(WARNING) << "OHOS surface config: window " << widget_ << " is "
                 << what;
  }

  bool pq_output_ = false;
  std::optional<bool> tagged_pq_;
  // The buffer format the window had before it was raised to 10 bits.
  std::optional<int32_t> original_format_;

  WindowRefreshResult RefreshNativeWindow() {
    std::optional<OhosNativeSurface> surface = GetOhosNativeSurface(widget_);
    if (!surface || !surface->window) {
      return WindowRefreshResult::kUnavailable;
    }

    const EGLNativeWindowType next_window =
        reinterpret_cast<EGLNativeWindowType>(surface->window);
    if (next_window == window_) {
      return WindowRefreshResult::kUnchanged;
    }

    window_ = next_window;
    tagged_pq_.reset();
    tagged_color_.reset();
    if (angle_vulkan_) {
      probed_ = false;
      ReportOhosWsiOutputCapabilities(widget_, false, false);
    }
    original_format_.reset();
    LOG(INFO) << "OHOS XComponent native window changed; recreating EGL "
                 "surface";
    return WindowRefreshResult::kChanged;
  }

  bool RefreshAndRecreateNativeWindow() {
    const WindowRefreshResult refresh = RefreshNativeWindow();
    if (refresh == WindowRefreshResult::kUnavailable) {
      return false;
    }
    if (refresh == WindowRefreshResult::kChanged) {
      if (!Recreate()) {
        if (angle_vulkan_ && ten_bit_)
          FailEglOutput("EGL recreation or make-current failed");
        return false;
      }
      HoldWindow(window_);
    }
    return true;
  }

  // Whether the shell has taken this window's XComponent away -- a second
  // browser window the phone shell tore down 10 ms after making it. Its
  // frames go nowhere until a surface is back. Failing the swap instead made
  // viz treat it as a lost context: every context in the process was lost
  // with it, and WebGL was blocked for each page as having caused a GPU
  // reset.
  bool WindowGone() {
    std::optional<OhosNativeSurface> surface = GetOhosNativeSurface(widget_);
    return !surface || !surface->window;
  }

  gfx::SwapResult DropFrame(PresentationCallback callback) {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(std::move(callback),
                                  gfx::PresentationFeedback::Failure()));
    return gfx::SwapResult::SWAP_ACK;
  }

  // Keeps the window the EGL surface is made on alive while the surface is:
  // the shell's XComponent releases it when it goes, and drawing to a freed
  // window is the driver's to get wrong.
  void HoldWindow(EGLNativeWindowType window) {
    if (held_window_ == window) {
      return;
    }
    if (held_window_) {
      OH_NativeWindow_NativeObjectUnreference(
          reinterpret_cast<void*>(held_window_));
    }
    held_window_ = window;
    if (held_window_) {
      OH_NativeWindow_NativeObjectReference(
          reinterpret_cast<void*>(held_window_));
    }
  }

  EGLNativeWindowType held_window_ = 0;

  const gfx::AcceleratedWidget widget_;
  const bool angle_vulkan_;
  bool probed_ = false;
  bool initialized_ = false;
  bool p3_capable_ = false;
  bool hdr_capable_ = false;
  OhosOutputColor output_color_;
  OhosOutputColor hdr_color_;
  std::optional<OhosOutputColor> tagged_color_;
  OhosHdrDisplayGuard hdr_guard_;
  const bool sdr_output_;
  const bool hdr_panel_;
  bool ten_bit_;
  // The driver had no 10-bit window config, or could not make a window from
  // it; asking again on every resize would recreate the surface each time.
  bool ten_bit_unavailable_ = false;
};

// A window's surface when the shell never made one for it: draws into a 1x1
// pbuffer and drops every frame. viz presents to a window's surface each
// frame, and a pbuffer's own SwapBuffers is NOTREACHED -- the GPU thread
// trapped on the first frame of a second browser window drawn offscreen.
class OhosDetachedGLSurface : public gl::PbufferGLSurfaceEGL {
 public:
  explicit OhosDetachedGLSurface(gl::GLDisplayEGL* display)
      : PbufferGLSurfaceEGL(display, gfx::Size(1, 1)) {}

  gfx::SwapResult SwapBuffers(PresentationCallback callback,
                              gfx::FrameData data) override {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(std::move(callback),
                                  gfx::PresentationFeedback::Failure()));
    return gfx::SwapResult::SWAP_ACK;
  }

  // Stays 1x1: nothing it draws is seen, so it need not be window-sized.
  bool Resize(const gfx::Size& size,
              float scale_factor,
              const gfx::ColorSpace& color_space,
              bool has_alpha) override {
    return true;
  }

 private:
  ~OhosDetachedGLSurface() override = default;
};

// The browser owns the NativeWindow queue; this fork child draws directly into
// leased buffers. The pbuffer only keeps the EGL context current. Skia sees one
// stable FBO whose attachment is changed before each BeginPaint.
class PhoneGpuVSyncProvider final : public gfx::VSyncProvider {
 public:
  explicit PhoneGpuVSyncProvider(uint64_t widget) : widget_(widget) {}
  void GetVSyncParameters(UpdateVSyncCallback callback) override {
    base::TimeTicks timebase;
    base::TimeDelta interval;
    if (GetVSyncParametersIfAvailable(&timebase, &interval))
      std::move(callback).Run(timebase, interval);
  }
  bool GetVSyncParametersIfAvailable(base::TimeTicks* timebase,
                                     base::TimeDelta* interval) override {
    return GetOhosPhoneGpuTiming(widget_, timebase, interval);
  }
  bool SupportGetVSyncParametersIfAvailable() const override { return true; }
  bool IsHWClock() const override { return true; }
  void SetPreferredFrameInterval(base::TimeDelta interval) override {
    SetOhosPhoneGpuFrameInterval(widget_, interval);
  }

 private:
  const uint64_t widget_;
};

class OhosPhoneGpuGLSurface final : public gl::PbufferGLSurfaceEGL {
 public:
  OhosPhoneGpuGLSurface(gl::GLDisplayEGL* display,
                        gfx::AcceleratedWidget widget)
      : PbufferGLSurfaceEGL(display, gfx::Size(1, 1)),
        widget_(widget),
        timing_(widget) {}
  EGLConfig GetConfig() override {
    base::AutoReset<bool> selecting(&selecting_config_, true);
    return PbufferGLSurfaceEGL::GetConfig();
  }
  bool IsOffscreen() override { return selecting_config_; }
  gfx::SurfaceOrigin GetOrigin() const override {
    return gfx::SurfaceOrigin::kTopLeft;
  }
  gfx::VSyncProvider* GetVSyncProvider() override { return &timing_; }
  gfx::Size GetSize() override { return output_size_; }
  bool Resize(const gfx::Size& size,
              float scale_factor,
              const gfx::ColorSpace& color_space,
              bool has_alpha) override {
    phone_gpu::Frame frame;
    frame.widget = widget_;
    frame.sequence = 1;
    frame.width = size.width();
    frame.height = size.height();
    if (!phone_gpu::FrameBytes(frame))
      return false;
    if (size != GetSize()) {
      ReleaseLease();
      ClearImports();
      prepared_ = false;
      fallback_size_ = gfx::Size();
    }
    output_size_ = size;
    return true;
  }
  unsigned int GetBackingFramebufferObject() override {
    if (prepared_ || failed_)
      return fbo_;
    auto* context = gl::GLContext::GetCurrent();
    if (!context) {
      failed_ = true;
      return fbo_;
    }
    owner_context_ = eglGetCurrentContext();
    const bool es3 = context->GetVersionInfo()->IsAtLeastGLES(3, 0);
    GLint draw = 0, read = 0, texture = 0;
    glGetIntegerv(es3 ? GL_DRAW_FRAMEBUFFER_BINDING : GL_FRAMEBUFFER_BINDING,
                  &draw);
    if (es3)
      glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    if (!fbo_)
      glGenFramebuffersEXT(1, &fbo_);
    glBindFramebufferEXT(GL_FRAMEBUFFER, fbo_);
    bool attached = false;
    if (!sharing_disabled_)
      attached = AttachShared();
    if (!attached)
      attached = AttachFallback(es3);
    failed_ = !attached;
    prepared_ = attached;
    glBindTexture(GL_TEXTURE_2D, texture);
    glBindFramebufferEXT(es3 ? GL_DRAW_FRAMEBUFFER : GL_FRAMEBUFFER, draw);
    if (es3)
      glBindFramebufferEXT(GL_READ_FRAMEBUFFER, read);
    return fbo_;
  }
  gfx::SwapResult SwapBuffers(PresentationCallback callback,
                              gfx::FrameData data) override {
    auto result = phone_gpu::Result::kFailed;
    if (!failed_ && prepared_) {
      if (lease_) {
        const EGLint attributes[] = {EGL_NONE};
        const EGLDisplay display =
            GetGLDisplay()->GetAs<gl::GLDisplayEGL>()->GetDisplay();
        EGLSyncKHR sync = eglCreateSyncKHR(
            display, EGL_SYNC_NATIVE_FENCE_ANDROID, attributes);
        glFlush();
        int fence = sync == EGL_NO_SYNC_KHR
                        ? -1
                        : eglDupNativeFenceFDANDROID(display, sync);
        if (sync != EGL_NO_SYNC_KHR)
          eglDestroySyncKHR(display, sync);
        if (fence >= 0) {
          result = PresentOhosPhoneGpuBuffer(widget_, lease_, fence);
          lease_ = 0;
        } else {
          // Work may already be running: detach, never abort/reuse this buffer.
          ReleaseLease();
        }
      } else {
        result = Readback();
      }
    }
    prepared_ = false;
    failed_ = result == phone_gpu::Result::kFailed;
    base::TimeTicks timebase;
    base::TimeDelta interval;
    GetOhosPhoneGpuTiming(widget_, &timebase, &interval);
    // Submission acknowledgment, not a measured display timestamp. Do not set
    // HWCompletion/VSync flags or invent a fixed 16 ms display interval.
    const auto feedback =
        result == phone_gpu::Result::kPresented
            ? gfx::PresentationFeedback(base::TimeTicks::Now(), interval, 0)
            : gfx::PresentationFeedback::Failure();
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(std::move(callback), feedback));
    return failed_ ? gfx::SwapResult::SWAP_FAILED : gfx::SwapResult::SWAP_ACK;
  }
  void Destroy() override {
    ReleaseLease();
    ClearImports();
    if (OwnContextCurrent()) {
      if (fallback_texture_)
        glDeleteTextures(1, &fallback_texture_);
      if (fbo_)
        glDeleteFramebuffersEXT(1, &fbo_);
    }
    fallback_texture_ = fbo_ = 0;
    PbufferGLSurfaceEGL::Destroy();
  }

 private:
  class ImportTimeout final : public base::Watchdog::Delegate {
   public:
    void Alarm() override {
      LOG(ERROR)
          << "OHOS phone GPU: shared import timed out; terminating GPU child";
      _exit(75);
    }
  };
  struct Imported {
    OH_NativeBuffer* native = nullptr;
    OHNativeWindowBuffer* window_buffer = nullptr;
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    GLuint texture = 0;
  };
  ~OhosPhoneGpuGLSurface() override { Destroy(); }
  void ReleaseLease() {
    if (lease_)
      DiscardOhosPhoneGpuBuffer(widget_, std::exchange(lease_, 0));
  }
  bool OwnContextCurrent() const {
    return owner_context_ != EGL_NO_CONTEXT &&
           eglGetCurrentContext() == owner_context_ &&
           gl::GLContext::GetCurrent();
  }
  void ClearImports() {
    const bool current = OwnContextCurrent();
    // Resize/teardown only, never per frame. Retire old GPU references first.
    if (current && !imports_.empty())
      glFinish();
    for (auto& [key, entry] : imports_) {
      if (current && entry.texture)
        glDeleteTextures(1, &entry.texture);
      if (entry.image != EGL_NO_IMAGE_KHR)
        eglDestroyImageKHR(
            GetGLDisplay()->GetAs<gl::GLDisplayEGL>()->GetDisplay(),
            entry.image);
      if (entry.window_buffer)
        OH_NativeWindow_DestroyNativeWindowBuffer(entry.window_buffer);
      if (entry.native)
        OH_NativeBuffer_Unreference(entry.native);
    }
    imports_.clear();
  }
  bool AttachShared() {
    const EGLDisplay display =
        GetGLDisplay()->GetAs<gl::GLDisplayEGL>()->GetDisplay();
    if (!checked_extensions_) {
      checked_extensions_ = true;
      const char* extensions = eglQueryString(display, EGL_EXTENSIONS);
      sharing_disabled_ =
          !extensions || !strstr(extensions, "EGL_ANDROID_native_fence_sync") ||
          !strstr(extensions, "EGL_KHR_wait_sync");
      if (sharing_disabled_)
        LOG(WARNING)
            << "OHOS phone GPU: native fence unavailable; readback fallback";
    }
    if (sharing_disabled_)
      return false;
    phone_gpu::Frame frame;
    frame.widget = widget_;
    frame.sequence = 1;
    frame.width = GetSize().width();
    frame.height = GetSize().height();
    phone_gpu::SharedMessage message;
    if (!AcquireOhosPhoneGpuBuffer(frame, &message))
      return false;
    // A mapper/Binder regression must not freeze the GPU sequence forever.
    // Chromium's GPU recovery handles this exit; the browser detaches leases.
    if (!import_watchdog_) {
      import_watchdog_ = std::make_unique<base::Watchdog>(
          base::Seconds(2), "PhoneGpuImport", true, &import_timeout_);
    }
    import_watchdog_->Arm();
    base::ScopedClosureRunner disarm(base::BindOnce(
        &base::Watchdog::Disarm, base::Unretained(import_watchdog_.get())));
    const uint64_t lease = message.packet.lease;
    const uint32_t key = message.packet.prefix[0];
    if (!imports_.contains(key)) {
      // NativeWindow normally rotates 3 buffers. Bound old surface generations.
      if (imports_.size() >= 8)
        ClearImports();
      auto& entry = imports_[key];
      entry.native = phone_gpu::ImportSharedBuffer(message);
      if (entry.native)
        entry.window_buffer =
            OH_NativeWindow_CreateNativeWindowBufferFromNativeBuffer(
                entry.native);
      if (entry.window_buffer) {
        const EGLint attrs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
        entry.image = eglCreateImageKHR(display, EGL_NO_CONTEXT, 0x34E1,
                                        entry.window_buffer, attrs);
      }
      if (entry.image != EGL_NO_IMAGE_KHR) {
        glGenTextures(1, &entry.texture);
        glBindTexture(GL_TEXTURE_2D, entry.texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, entry.image);
      }
    }
    const auto& entry = imports_.at(key);
    bool ok = entry.texture && glGetError() == GL_NO_ERROR;
    if (ok) {
      glFramebufferTexture2DEXT(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                GL_TEXTURE_2D, entry.texture, 0);
      ok = glCheckFramebufferStatusEXT(GL_FRAMEBUFFER) ==
           GL_FRAMEBUFFER_COMPLETE;
    }
    base::ScopedFD acquire(message.TakeFence());
    if (ok && acquire.is_valid()) {
      const EGLint attrs[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, acquire.get(),
                              EGL_NONE};
      EGLSyncKHR sync =
          eglCreateSyncKHR(display, EGL_SYNC_NATIVE_FENCE_ANDROID, attrs);
      if (sync != EGL_NO_SYNC_KHR) {
        (void)acquire.release();  // EGL now owns this FD.
        ok = eglWaitSyncKHR(display, sync, 0) == EGL_TRUE;
        eglDestroySyncKHR(display, sync);
      } else {
        ok = false;
      }
    }
    if (!ok) {
      // No drawing into the leased allocation has occurred yet.
      CancelOhosPhoneGpuBuffer(widget_, lease);
      sharing_disabled_ = true;
      LOG(WARNING) << "OHOS phone GPU: shared import failed; readback fallback";
      return false;
    }
    lease_ = lease;
    if (!logged_shared_) {
      logged_shared_ = true;
      LOG(WARNING) << "OHOS phone GPU: direct shared-buffer FBO active (no "
                      "readback/blit)";
    }
    return true;
  }
  bool AttachFallback(bool es3) {
    if (!fallback_texture_)
      glGenTextures(1, &fallback_texture_);
    glBindTexture(GL_TEXTURE_2D, fallback_texture_);
    if (fallback_size_ != GetSize()) {
      GLint unpack = 0;
      if (es3) {
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
      }
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, GetSize().width(),
                   GetSize().height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      if (es3)
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, unpack);
      fallback_size_ = GetSize();
      LOG(WARNING) << "OHOS phone GPU: readback fallback FBO "
                   << GetSize().ToString();
    }
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                              GL_TEXTURE_2D, fallback_texture_, 0);
    return glCheckFramebufferStatusEXT(GL_FRAMEBUFFER) ==
           GL_FRAMEBUFFER_COMPLETE;
  }
  phone_gpu::Result Readback() {
    phone_gpu::Frame frame;
    frame.widget = widget_;
    frame.sequence = 1;  // Submit assigns the connection's monotonic sequence.
    frame.width = GetSize().width();
    frame.height = GetSize().height();
    frame.reserved = phone_gpu::kTopLeftRows;
    auto result = phone_gpu::Result::kFailed;
    if (!failed_ && buffer_.Resize(phone_gpu::FrameBytes(frame),
                                   OhosPhoneGpuMinimumBufferFd())) {
      const bool es3 =
          gl::GLContext::GetCurrent()->GetVersionInfo()->IsAtLeastGLES(3, 0);
      GLint framebuffer = 0, alignment = 4;
      GLint pack_buffer = 0, row_length = 0, skip_rows = 0, skip_pixels = 0;
      glGetIntegerv(es3 ? GL_READ_FRAMEBUFFER_BINDING : GL_FRAMEBUFFER_BINDING,
                    &framebuffer);
      glGetIntegerv(GL_PACK_ALIGNMENT, &alignment);
      if (es3) {
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buffer);
        glGetIntegerv(GL_PACK_ROW_LENGTH, &row_length);
        glGetIntegerv(GL_PACK_SKIP_ROWS, &skip_rows);
        glGetIntegerv(GL_PACK_SKIP_PIXELS, &skip_pixels);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glPixelStorei(GL_PACK_SKIP_ROWS, 0);
        glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
      }
      glBindFramebufferEXT(es3 ? GL_READ_FRAMEBUFFER : GL_FRAMEBUFFER, fbo_);
      glPixelStorei(GL_PACK_ALIGNMENT, 4);
      glReadPixels(0, 0, frame.width, frame.height, GL_RGBA, GL_UNSIGNED_BYTE,
                   buffer_.data());
      const GLenum error = glGetError();
      glPixelStorei(GL_PACK_ALIGNMENT, alignment);
      glBindFramebufferEXT(es3 ? GL_READ_FRAMEBUFFER : GL_FRAMEBUFFER,
                           framebuffer);
      if (es3) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, pack_buffer);
        glPixelStorei(GL_PACK_ROW_LENGTH, row_length);
        glPixelStorei(GL_PACK_SKIP_ROWS, skip_rows);
        glPixelStorei(GL_PACK_SKIP_PIXELS, skip_pixels);
      }
      if (error == GL_NO_ERROR)
        result = SubmitOhosPhoneGpuFrame(frame, buffer_.fd());
    }
    return result;
  }
  const gfx::AcceleratedWidget widget_;
  PhoneGpuVSyncProvider timing_;
  phone_gpu::Buffer buffer_;
  std::map<uint32_t, Imported> imports_;
  ImportTimeout import_timeout_;
  std::unique_ptr<base::Watchdog> import_watchdog_;
  gfx::Size output_size_{1, 1};
  gfx::Size fallback_size_;
  EGLContext owner_context_ = EGL_NO_CONTEXT;
  GLuint fbo_ = 0, fallback_texture_ = 0;
  uint64_t lease_ = 0;
  bool prepared_ = false;
  bool failed_ = false;
  bool selecting_config_ = false;
  bool sharing_disabled_ = false;
  bool checked_extensions_ = false;
  bool logged_shared_ = false;
};

class GLOzoneEGLOhos : public GLOzoneEGL {
 public:
  GLOzoneEGLOhos() = default;
  ~GLOzoneEGLOhos() override = default;

  scoped_refptr<gl::GLSurface> CreateViewGLSurface(
      gl::GLDisplay* display,
      gfx::AcceleratedWidget widget) override {
    if (IsOhosPhoneGpuChild()) {
      return gl::InitializeGLSurface(
          base::MakeRefCounted<OhosPhoneGpuGLSurface>(
              display->GetAs<gl::GLDisplayEGL>(), widget));
    }
    std::optional<OhosNativeSurface> surface = GetOhosNativeSurface(widget);
    if ((!surface || !surface->window) &&
        IsOhosNativeSurfaceExpected(widget)) {
      surface =
          WaitForOhosNativeSurface(widget, kNativeSurfaceWaitTimeout);
    }
    if (!surface || !surface->window) {
      if (IsOhosAnchoredWindow(widget) || IsOhosNativeSurfaceExpected(widget)) {
        // No XComponent for this popup or window. Failing here costs far
        // more than the one window: viz takes a failed view surface as GPU
        // compositing being broken and falls back to software for every
        // window, and software output on this platform reaches no screen --
        // the whole browser froze on its last frame. A second browser window
        // whose surface the shell never made did exactly that. Let it draw
        // nowhere instead.
        LOG(ERROR) << "No native surface for widget " << widget
                   << "; drawing it offscreen";
        return gl::InitializeGLSurface(
            base::MakeRefCounted<OhosDetachedGLSurface>(
                display->GetAs<gl::GLDisplayEGL>()));
      }
      return nullptr;
    }

    LogTenBitConfigs(display->GetAs<gl::GLDisplayEGL>()->GetDisplay());
    auto vsync_provider = std::make_unique<OhosVSyncProvider>(
        GetOhosApplicationWindowIdForWidget(widget));
    auto gl_surface = base::MakeRefCounted<OhosNativeViewGLSurfaceEGL>(
        display->GetAs<gl::GLDisplayEGL>(), widget,
        reinterpret_cast<EGLNativeWindowType>(surface->window),
        std::move(vsync_provider));
    return gl::InitializeGLSurface(std::move(gl_surface));
  }

  scoped_refptr<gl::GLSurface> CreateOffscreenGLSurface(
      gl::GLDisplay* display,
      const gfx::Size& size) override {
    return gl::InitializeGLSurface(
        base::MakeRefCounted<gl::PbufferGLSurfaceEGL>(
            display->GetAs<gl::GLDisplayEGL>(), size));
  }

  bool CanImportNativePixmap(viz::SharedImageFormat format) override {
    return IsOhosNativePixmapFormat(format) &&
           (!IsOhosPhoneGpuChild() || IsOffscreenNativeFormat(format));
  }

  // A YUV buffer is sampled whole, through GL_TEXTURE_EXTERNAL_OES with the
  // driver converting it, never plane by plane.
  std::unique_ptr<NativePixmapGLBinding> ImportNativePixmap(
      scoped_refptr<gfx::NativePixmap> pixmap,
      viz::SharedImageFormat plane_format,
      std::optional<int> plane_index,
      gfx::Size plane_size,
      const gfx::ColorSpace& color_space,
      GLenum target,
      GLuint texture_id) override {
    if (IsOhosPhoneGpuChild() &&
        !IsOffscreenNativeFormat(pixmap->GetSharedImageFormat()))
      return nullptr;
    if (plane_index.has_value() &&
        (!pixmap->GetSharedImageFormat().is_single_plane() || *plane_index != 0)) {
      LOG(ERROR) << "OHOS native pixmap: per-plane import of "
                 << plane_format.ToString() << " is not supported";
      return nullptr;
    }
    return ImportOhosNativePixmap(std::move(pixmap), color_space, target,
                                  texture_id);
  }

 protected:
  gl::EGLDisplayPlatform GetNativeDisplay() override {
    return gl::EGLDisplayPlatform(EGL_DEFAULT_DISPLAY);
  }

  bool LoadGLES2Bindings(
      const gl::GLImplementationParts& implementation) override {
    return LoadDefaultEGLGLES2Bindings(implementation);
  }
};

}  // namespace

OhosSurfaceFactory::OhosSurfaceFactory(base::FilePath base_path)
    : HeadlessSurfaceFactory(base_path),
      egl_implementation_(std::make_unique<GLOzoneEGLOhos>()) {}

OhosSurfaceFactory::~OhosSurfaceFactory() = default;

std::vector<gl::GLImplementationParts>
OhosSurfaceFactory::GetAllowedGLImplementations() {
  return {
      gl::GLImplementationParts(gl::kGLImplementationEGLANGLE),
      gl::GLImplementationParts(gl::kGLImplementationEGLGLES2),
  };
}

GLOzone* OhosSurfaceFactory::GetGLOzone(
    const gl::GLImplementationParts& implementation) {
  if (implementation.gl == gl::kGLImplementationEGLANGLE ||
      implementation.gl == gl::kGLImplementationEGLGLES2) {
    return egl_implementation_.get();
  }
  return nullptr;
}

scoped_refptr<gfx::NativePixmap> OhosSurfaceFactory::CreateNativePixmap(
    gfx::AcceleratedWidget widget,
    gpu::VulkanDeviceQueue* device_queue,
    gfx::Size size,
    viz::SharedImageFormat format,
    gfx::BufferUsage usage,
    std::optional<gfx::Size> framebuffer_size) {
  return IsOhosPhoneGpuChild() ? CreateOhosPhoneOffscreenPixmap(size, format)
                               : CreateOhosNativePixmap(size, format);
}

bool OhosSurfaceFactory::CanCreateNativePixmapForFormat(
    viz::SharedImageFormat format) {
  return IsOhosNativePixmapFormat(format) &&
         (!IsOhosPhoneGpuChild() || IsOffscreenNativeFormat(format));
}

scoped_refptr<gfx::NativePixmap>
OhosSurfaceFactory::CreateNativePixmapFromHandle(
    gfx::AcceleratedWidget widget,
    gfx::Size size,
    viz::SharedImageFormat format,
    gfx::NativePixmapHandle handle) {
  if (!handle.ohos_buffer_prefix.empty()) {
    return IsOffscreenNativeFormat(format)
               ? ImportOhosOffscreenPixmap(size, format, std::move(handle))
               : ImportOhosVideoNativePixmap(size, format, std::move(handle));
  }
  return IsOhosPhoneGpuChild() ? nullptr
                               : CreateOhosNativePixmapFromHandle(
                                     size, format, std::move(handle));
}

bool OhosSurfaceFactory::IsFormatSupportedForTexturing(
    viz::SharedImageFormat format) const {
  // Asked about YUV formats, for sampling video as one texture.
  return !IsOhosPhoneGpuChild() && format.is_multi_plane() &&
         IsOhosNativePixmapFormat(format);
}

#if BUILDFLAG(ENABLE_VULKAN)
namespace {
class VideoVulkanImplementation : public gpu::VulkanImplementationOhos {
 public:
  using VulkanImplementationOhos::VulkanImplementationOhos;

  std::vector<const char*> GetOptionalDeviceExtensions() override {
    auto extensions = VulkanImplementationOhos::GetOptionalDeviceExtensions();
    // Optional on non-fork devices: missing video import support must not
    // disable Vulkan page rendering or its existing window swapchain.
    extensions.push_back("VK_OHOS_external_memory");
    return extensions;
  }
  // Dawn Canvas and Skia share NativeBuffer allocations on every device.
  // Keep capability probing conditional on the import and sync extensions.
  bool CanImportGpuMemoryBuffer(gpu::VulkanDeviceQueue* queue,
                               gfx::GpuMemoryBufferType type) override {
    return type == gfx::NATIVE_PIXMAP && CanImportOhosVideoVulkan(queue);
  }
  std::unique_ptr<gpu::VulkanImage> CreateImageFromGpuMemoryHandle(
      gpu::VulkanDeviceQueue* queue,
      gfx::GpuMemoryBufferHandle handle,
      gfx::Size size,
      VkFormat format,
      const gfx::ColorSpace& color) override {
    if (format == VK_FORMAT_R8G8B8A8_UNORM ||
        format == VK_FORMAT_B8G8R8A8_UNORM ||
        format == VK_FORMAT_R16G16B16A16_SFLOAT) {
      if (handle.type != gfx::NATIVE_PIXMAP)
        return nullptr;
      const auto& prefix = handle.native_pixmap_handle().ohos_buffer_prefix;
      const uint32_t native_format =
          format == VK_FORMAT_R16G16B16A16_SFLOAT
              ? phone_gpu::kSharedImageRgba16Float
              : format == VK_FORMAT_R8G8B8A8_UNORM
                    ? phone_gpu::kSharedImageRgba8888
                    : phone_gpu::kSharedImageBgra8888;
      if (prefix.size() != 14 || prefix[8] != native_format)
        return nullptr;
      return ImportOhosOffscreenVulkan(queue, std::move(handle), size);
    }
    return ImportOhosVideoVulkan(queue, std::move(handle), size, color);
  }
};

class PhoneVulkanImplementation final : public VideoVulkanImplementation {
 public:
  PhoneVulkanImplementation() : VideoVulkanImplementation(true, {}, {}) {}
  std::vector<const char*> GetRequiredDeviceExtensions() override {
    auto extensions = VulkanImplementationOhos::GetRequiredDeviceExtensions();
    extensions.push_back("VK_OHOS_external_memory");
    extensions.push_back(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
    extensions.push_back(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
    return extensions;
  }
  std::vector<const char*> GetOptionalDeviceExtensions() override {
    std::vector<const char*> optional;
    for (const char* extension :
         VulkanImplementationOhos::GetOptionalDeviceExtensions()) {
      if (std::strcmp(extension, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME) &&
          std::strcmp(extension, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME)) {
        optional.push_back(extension);
      }
    }
    return optional;
  }
  std::unique_ptr<gpu::VulkanSurface> CreateViewSurface(
      gfx::AcceleratedWidget widget) override {
    // Phone Skia uses the shared-buffer output device, never a child window.
    return nullptr;
  }
};
}  // namespace
std::unique_ptr<gpu::VulkanImplementation>
OhosSurfaceFactory::CreateVulkanImplementation(bool use_swiftshader,
                                               bool allow_protected_memory) {
  // No SwiftShader in this build: it needs ANGLE's Vulkan backend, which
  // needs the very thing being created here. Asking for it gets nothing
  // rather than something that will fail later and less clearly.
  if (use_swiftshader) {
    LOG(WARNING) << "OHOS: no SwiftShader Vulkan in this build";
    return nullptr;
  }
  if (IsOhosPhoneGpuChild()) {
    if (!AllowOhosPhoneVulkan()) {
      LOG(WARNING) << "OHOS phone Vulkan: unavailable or failed earlier; use GL";
      return nullptr;
    }
    LOG(WARNING) << "OHOS phone Vulkan: enabling shared-buffer Skia output";
    return std::make_unique<PhoneVulkanImplementation>();
  }
  // A widget's window as the GL path finds it: from the registry -- the
  // mirror of the browser's, in a GPU process of its own -- waiting for it
  // when one is expected and has not arrived yet.
  return std::make_unique<VideoVulkanImplementation>(
      /*force_native=*/true,
      base::BindRepeating([](gfx::AcceleratedWidget widget) -> void* {
        std::optional<OhosNativeSurface> surface =
            GetOhosNativeSurface(widget);
        if ((!surface || !surface->window) &&
            IsOhosNativeSurfaceExpected(widget)) {
          surface = WaitForOhosNativeSurface(widget, kNativeSurfaceWaitTimeout);
        }
        return surface ? surface->window : nullptr;
      }),
      // The same NativeVSync the GL path gives its surface: timing for the
      // swap chain's feedback, and where the content frame rate goes.
      base::BindRepeating([](gfx::AcceleratedWidget widget)
                              -> std::unique_ptr<gfx::VSyncProvider> {
        return std::make_unique<OhosVSyncProvider>(
            GetOhosApplicationWindowIdForWidget(widget));
      }));
}
#endif  // BUILDFLAG(ENABLE_VULKAN)

}  // namespace ui
