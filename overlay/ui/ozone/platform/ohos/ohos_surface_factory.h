#ifndef UI_OZONE_PLATFORM_OHOS_OHOS_SURFACE_FACTORY_H_
#define UI_OZONE_PLATFORM_OHOS_OHOS_SURFACE_FACTORY_H_

#include <memory>

#include "base/files/file_path.h"
#include "gpu/vulkan/buildflags.h"
#include "ui/ozone/platform/headless/headless_surface_factory.h"

namespace ui {

class OhosSurfaceFactory : public HeadlessSurfaceFactory {
 public:
  explicit OhosSurfaceFactory(base::FilePath base_path);
  ~OhosSurfaceFactory() override;

  std::vector<gl::GLImplementationParts> GetAllowedGLImplementations() override;
  GLOzone* GetGLOzone(const gl::GLImplementationParts& implementation) override;

  // Native pixmaps: OH_NativeBuffers for video frames, ohos_native_pixmap.h.
  scoped_refptr<gfx::NativePixmap> CreateNativePixmap(
      gfx::AcceleratedWidget widget,
      gpu::VulkanDeviceQueue* device_queue,
      gfx::Size size,
      viz::SharedImageFormat format,
      gfx::BufferUsage usage,
      std::optional<gfx::Size> framebuffer_size = std::nullopt) override;
  bool CanCreateNativePixmapForFormat(viz::SharedImageFormat format) override;
  scoped_refptr<gfx::NativePixmap> CreateNativePixmapFromHandle(
      gfx::AcceleratedWidget widget,
      gfx::Size size,
      viz::SharedImageFormat format,
      gfx::NativePixmapHandle handle) override;
  bool IsFormatSupportedForTexturing(
      viz::SharedImageFormat format) const override;

#if BUILDFLAG(ENABLE_VULKAN)
  // Ozone is asked before the per-platform factory in
  // gpu/vulkan/init, so this is where a Vulkan implementation is chosen on
  // this port -- not the BUILDFLAG(IS_ANDROID) ladder there.
  std::unique_ptr<gpu::VulkanImplementation> CreateVulkanImplementation(
      bool use_swiftshader,
      bool allow_protected_memory) override;
#endif

 private:
  std::unique_ptr<GLOzone> egl_implementation_;
};

}  // namespace ui

#endif  // UI_OZONE_PLATFORM_OHOS_OHOS_SURFACE_FACTORY_H_
