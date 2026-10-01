#include "ui/ozone/platform/ohos/ohos_surface_factory.h"

#include <optional>
#include <string>
#include <vector>

#if BUILDFLAG(ENABLE_VULKAN)
#include "gpu/vulkan/ohos/vulkan_implementation_ohos.h"
#endif

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/ref_counted.h"
#include "base/strings/string_split.h"
#include "base/time/time.h"
#include "ui/gfx/color_space.h"
#include "ui/gfx/vsync_provider.h"
#include "ui/gl/gl_bindings.h"
#include "ui/gl/gl_display.h"
#include "ui/gl/gl_implementation.h"
#include "ui/gl/gl_surface_egl.h"
#include "ui/ozone/common/egl_util.h"
#include "ui/ozone/common/gl_ozone_egl.h"
#include <native_window/external_window.h>

#include "ui/ozone/platform/ohos/ohos_native_window_registry.h"
#include "ui/ozone/platform/ohos/ohos_screen.h"
#include "ui/ozone/platform/ohos/ohos_vsync_provider.h"

namespace ui {
namespace {

constexpr base::TimeDelta kNativeSurfaceWaitTimeout = base::Seconds(3);

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
        hdr_panel_(OhosDisplaySupportsHdr()),
        // Whether the window has alpha is only known at the first Resize;
        // until then, a popup is the window that draws it.
        ten_bit_(hdr_panel_ && !IsOhosAnchoredWindow(widget)) {}

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
  bool Initialize(gl::GLSurfaceFormat format) override {
    // Settle 8 or 10 bits before the base class asks the native window to be
    // set up, which is where its buffers are given the matching format.
    GetConfig();
    if (gl::NativeViewGLSurfaceEGL::Initialize(format)) {
      return true;
    }
    // Without a native window there is nothing to learn about the config.
    std::optional<OhosNativeSurface> surface = GetOhosNativeSurface(widget_);
    if (!ten_bit_ || !surface || !surface->window) {
      return false;
    }
    ten_bit_ = false;
    ten_bit_unavailable_ = true;
    config_ = nullptr;
    LogConfig("8-bit (the 10-bit window could not be created)");
    return gl::NativeViewGLSurfaceEGL::Initialize(format);
  }

  bool InitializeNativeWindow() override {
    std::optional<OhosNativeSurface> surface = GetOhosNativeSurface(widget_);
    if (!surface || !surface->window) {
      return false;
    }
    const EGLNativeWindowType window =
        reinterpret_cast<EGLNativeWindowType>(surface->window);
    if (window != window_) {
      tagged_pq_ = false;
      original_format_.reset();
    }
    window_ = window;
    SetBufferFormat();
    TagNativeWindowColorSpace();
    return true;
  }

  bool Resize(const gfx::Size& size,
              float scale_factor,
              const gfx::ColorSpace& color_space,
              bool has_alpha) override {
    const WindowRefreshResult refresh = RefreshNativeWindow();
    if (refresh == WindowRefreshResult::kUnavailable) {
      return false;
    }
    pq_output_ =
        color_space.GetTransferID() == gfx::ColorSpace::TransferID::PQ;
    TagNativeWindowColorSpace();
    const bool ten_bit = hdr_panel_ && !has_alpha && !ten_bit_unavailable_;
    const bool config_changed = ten_bit != ten_bit_;
    if (config_changed) {
      ten_bit_ = ten_bit;
      config_ = nullptr;
    }
    if (refresh == WindowRefreshResult::kChanged || config_changed) {
      size_ = size;
      return Recreate();
    }
    return gl::NativeViewGLSurfaceEGL::Resize(size, scale_factor, color_space,
                                              has_alpha);
  }

  gfx::SwapResult SwapBuffers(PresentationCallback callback,
                              gfx::FrameData data) override {
    if (!RefreshAndRecreateNativeWindow()) {
      std::move(callback).Run(gfx::PresentationFeedback::Failure());
      return gfx::SwapResult::SWAP_FAILED;
    }
    return gl::NativeViewGLSurfaceEGL::SwapBuffers(std::move(callback),
                                                   std::move(data));
  }

  gfx::SwapResult PostSubBuffer(int x,
                                int y,
                                int width,
                                int height,
                                PresentationCallback callback,
                                gfx::FrameData data) override {
    if (!RefreshAndRecreateNativeWindow()) {
      std::move(callback).Run(gfx::PresentationFeedback::Failure());
      return gfx::SwapResult::SWAP_FAILED;
    }
    return gl::NativeViewGLSurfaceEGL::PostSubBuffer(
        x, y, width, height, std::move(callback), std::move(data));
  }

 private:
  enum class WindowRefreshResult { kUnavailable, kUnchanged, kChanged };

  // Chromium's EGL surface ignores the colour space it is asked to draw in.
  // On an HDR panel the compositor draws the whole output in Rec. 2020 PQ
  // while HDR video plays; untagged, the system showed those values as sRGB
  // and the page went grey. The native window keeps a colour space and an
  // HDR metadata type that the system stamps on every buffer it hands out
  // from then on, so tagging it before the next frame is drawn is enough --
  // no surface has to be recreated. Back to sRGB once PQ output stops; a
  // window that never drew PQ is left as the system made it.
  void TagNativeWindowColorSpace() {
    auto* native_window = reinterpret_cast<OHNativeWindow*>(window_);
    if (!native_window || pq_output_ == tagged_pq_) {
      return;
    }
    tagged_pq_ = pq_output_;
    const int32_t color_space_result = OH_NativeWindow_SetColorSpace(
        native_window,
        pq_output_ ? OH_COLORSPACE_BT2020_PQ_FULL : OH_COLORSPACE_SRGB_FULL);
    // The system reads the type from the first byte. OH_VIDEO_NONE (-1) is
    // no type it maps, which leaves the window with none.
    uint8_t metadata_type = static_cast<uint8_t>(
        pq_output_ ? OH_VIDEO_HDR_HDR10 : OH_VIDEO_NONE);
    const int32_t metadata_result = OH_NativeWindow_SetMetadataValue(
        native_window, OH_HDR_METADATA_TYPE, sizeof(metadata_type),
        &metadata_type);
    LOG(WARNING) << "OHOS surface colour space: native window tagged "
                 << (pq_output_ ? "Rec. 2020 PQ, HDR10" : "sRGB")
                 << " (colour space result " << color_space_result
                 << ", metadata result " << metadata_result << ")";
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
    if (IsOhosAnchoredWindow(widget_)) {
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
  bool tagged_pq_ = false;
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
    tagged_pq_ = false;
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
    return refresh != WindowRefreshResult::kChanged || Recreate();
  }

  const gfx::AcceleratedWidget widget_;
  const bool hdr_panel_;
  bool ten_bit_;
  // The driver had no 10-bit window config, or could not make a window from
  // it; asking again on every resize would recreate the surface each time.
  bool ten_bit_unavailable_ = false;
};

class GLOzoneEGLOhos : public GLOzoneEGL {
 public:
  GLOzoneEGLOhos() = default;
  ~GLOzoneEGLOhos() override = default;

  scoped_refptr<gl::GLSurface> CreateViewGLSurface(
      gl::GLDisplay* display,
      gfx::AcceleratedWidget widget) override {
    std::optional<OhosNativeSurface> surface = GetOhosNativeSurface(widget);
    if ((!surface || !surface->window) &&
        IsOhosNativeSurfaceExpected(widget)) {
      surface =
          WaitForOhosNativeSurface(widget, kNativeSurfaceWaitTimeout);
    }
    if (!surface || !surface->window) {
      if (IsOhosAnchoredWindow(widget)) {
        // No XComponent for this popup. Failing here costs far more than the
        // popup: viz takes a failed view surface as GPU compositing being
        // broken and falls back to software for every window, and software
        // output on this platform reaches no screen -- the whole browser
        // froze on its last frame. Let the popup draw nowhere instead.
        LOG(ERROR) << "No native surface for popup widget " << widget
                   << "; drawing it offscreen";
        return CreateOffscreenGLSurface(display, gfx::Size(1, 1));
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

#if BUILDFLAG(ENABLE_VULKAN)
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
  // A widget's window as the GL path finds it: from the registry -- the
  // mirror of the browser's, in a GPU process of its own -- waiting for it
  // when one is expected and has not arrived yet.
  return std::make_unique<gpu::VulkanImplementationOhos>(
      /*force_native=*/true,
      base::BindRepeating([](gfx::AcceleratedWidget widget) -> void* {
        std::optional<OhosNativeSurface> surface =
            GetOhosNativeSurface(widget);
        if ((!surface || !surface->window) &&
            IsOhosNativeSurfaceExpected(widget)) {
          surface = WaitForOhosNativeSurface(widget, kNativeSurfaceWaitTimeout);
        }
        return surface ? surface->window : nullptr;
      }));
}
#endif  // BUILDFLAG(ENABLE_VULKAN)

}  // namespace ui
