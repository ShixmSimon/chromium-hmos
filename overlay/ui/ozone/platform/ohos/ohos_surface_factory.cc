#include "ui/ozone/platform/ohos/ohos_surface_factory.h"

#include "base/logging.h"
#include "base/memory/ref_counted.h"
#include "base/time/time.h"
#include "ui/gfx/vsync_provider.h"
#include "ui/gl/gl_display.h"
#include "ui/gl/gl_implementation.h"
#include "ui/gl/gl_surface_egl.h"
#include "ui/ozone/common/egl_util.h"
#include "ui/ozone/common/gl_ozone_egl.h"
#include "ui/ozone/platform/ohos/ohos_native_window_registry.h"
#include "ui/ozone/platform/ohos/ohos_vsync_provider.h"

namespace ui {
namespace {

constexpr base::TimeDelta kNativeSurfaceWaitTimeout = base::Seconds(3);

class OhosNativeViewGLSurfaceEGL final : public gl::NativeViewGLSurfaceEGL {
 public:
  OhosNativeViewGLSurfaceEGL(gl::GLDisplayEGL* display,
                             gfx::AcceleratedWidget widget,
                             EGLNativeWindowType window,
                             std::unique_ptr<gfx::VSyncProvider> vsync_provider)
      : gl::NativeViewGLSurfaceEGL(display, window, std::move(vsync_provider)),
        widget_(widget) {}

  bool InitializeNativeWindow() override {
    std::optional<OhosNativeSurface> surface = GetOhosNativeSurface(widget_);
    if (!surface || !surface->window) {
      return false;
    }
    window_ = reinterpret_cast<EGLNativeWindowType>(surface->window);
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
    if (refresh == WindowRefreshResult::kChanged) {
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

}  // namespace ui
