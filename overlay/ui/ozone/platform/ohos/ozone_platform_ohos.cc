#include "ui/ozone/platform/ohos/ozone_platform_ohos.h"

#include <memory>

#include "base/files/file_path.h"
#include "ui/base/cursor/cursor_factory.h"
#include "ui/display/types/native_display_delegate.h"
#include "ui/events/ozone/layout/keyboard_layout_engine_manager.h"
#include "ui/events/ozone/layout/stub/stub_keyboard_layout_engine.h"
#include "ui/ozone/common/bitmap_cursor_factory.h"
#include "ui/ozone/common/stub_client_native_pixmap_factory.h"
#include "ui/ozone/common/stub_overlay_manager.h"
#include "ui/ozone/platform/ohos/ohos_clipboard.h"
#include "ui/ozone/platform/ohos/ohos_event_source.h"
#include "ui/ozone/platform/ohos/ohos_input_method.h"
#include "ui/ozone/platform/ohos/ohos_platform_window.h"
#include "ui/ozone/platform/ohos/ohos_screen.h"
#include "ui/ozone/platform/ohos/ohos_surface_factory.h"
#include "ui/ozone/public/gpu_platform_support_host.h"
#include "ui/ozone/public/input_controller.h"
#include "ui/ozone/public/ozone_platform.h"
#include "ui/ozone/public/stub_input_controller.h"
#include "ui/ozone/public/system_input_injector.h"
#include "ui/platform_window/platform_window_init_properties.h"

namespace ui {

namespace {

class OzonePlatformOhos : public OzonePlatform {
 public:
  OzonePlatformOhos() = default;
  ~OzonePlatformOhos() override = default;

  SurfaceFactoryOzone* GetSurfaceFactoryOzone() override {
    return surface_factory_.get();
  }
  OverlayManagerOzone* GetOverlayManager() override {
    return overlay_manager_.get();
  }
  CursorFactory* GetCursorFactory() override { return cursor_factory_.get(); }
  InputController* GetInputController() override {
    return input_controller_.get();
  }
  GpuPlatformSupportHost* GetGpuPlatformSupportHost() override {
    return gpu_platform_support_host_.get();
  }
  std::unique_ptr<SystemInputInjector> CreateSystemInputInjector() override {
    return nullptr;
  }
  std::unique_ptr<PlatformWindow> CreatePlatformWindow(
      PlatformWindowDelegate* delegate,
      PlatformWindowInitProperties properties) override {
    const bool expects_native_surface =
        properties.type == PlatformWindowType::kWindow &&
        properties.parent_widget == gfx::kNullAcceleratedWidget;
    return std::make_unique<OhosPlatformWindow>(
        delegate, properties.bounds, expects_native_surface);
  }
  bool IsWindowCompositingSupported() const override { return true; }
  PlatformClipboard* GetPlatformClipboard() override {
    if (!clipboard_) {
      clipboard_ = std::make_unique<OhosClipboard>();
    }
    return clipboard_.get();
  }
  std::unique_ptr<display::NativeDisplayDelegate> CreateNativeDisplayDelegate()
      override {
    return nullptr;
  }
  std::unique_ptr<PlatformScreen> CreateScreen() override {
    return std::make_unique<OhosScreen>();
  }
  void InitScreen(PlatformScreen* screen) override { (void)screen; }
  std::unique_ptr<InputMethod> CreateInputMethod(
      ImeKeyEventDispatcher* ime_key_event_dispatcher,
      gfx::AcceleratedWidget widget) override {
    return std::make_unique<OhosInputMethod>(ime_key_event_dispatcher, widget);
  }

  bool InitializeUI(const InitParams& params) override {
    (void)params;
    surface_factory_ = std::make_unique<OhosSurfaceFactory>(base::FilePath());
    if (!PlatformEventSource::GetInstance()) {
      platform_event_source_ = std::make_unique<OhosEventSource>();
    }
    keyboard_layout_engine_ = std::make_unique<StubKeyboardLayoutEngine>();
    KeyboardLayoutEngineManager::SetKeyboardLayoutEngine(
        keyboard_layout_engine_.get());

    overlay_manager_ = std::make_unique<StubOverlayManager>();
    input_controller_ = std::make_unique<StubInputController>();
    cursor_factory_ = std::make_unique<BitmapCursorFactory>();
    gpu_platform_support_host_.reset(CreateStubGpuPlatformSupportHost());
    return true;
  }

  void InitializeGPU(const InitParams& params) override {
    (void)params;
    if (!surface_factory_) {
      surface_factory_ = std::make_unique<OhosSurfaceFactory>(base::FilePath());
    }
  }

 private:
  std::unique_ptr<KeyboardLayoutEngine> keyboard_layout_engine_;
  std::unique_ptr<OhosSurfaceFactory> surface_factory_;
  std::unique_ptr<PlatformEventSource> platform_event_source_;
  std::unique_ptr<CursorFactory> cursor_factory_;
  std::unique_ptr<InputController> input_controller_;
  std::unique_ptr<GpuPlatformSupportHost> gpu_platform_support_host_;
  std::unique_ptr<OverlayManagerOzone> overlay_manager_;
  std::unique_ptr<OhosClipboard> clipboard_;
};

}  // namespace

OzonePlatform* CreateOzonePlatformOhos() {
  return new OzonePlatformOhos();
}

gfx::ClientNativePixmapFactory* CreateClientNativePixmapFactoryOhos() {
  return CreateStubClientNativePixmapFactory();
}

}  // namespace ui
