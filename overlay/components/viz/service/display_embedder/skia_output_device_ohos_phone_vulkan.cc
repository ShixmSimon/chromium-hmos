// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "components/viz/service/display_embedder/skia_output_device_ohos_phone_vulkan.h"

#include <native_buffer/native_buffer.h>
#include <unistd.h>
#include <map>
#include <utility>

#include "base/files/scoped_file.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/threading/watchdog.h"
#include "components/viz/common/gpu/vulkan_context_provider.h"
#include "gpu/command_buffer/service/memory_tracking.h"
#include "gpu/command_buffer/service/shared_context_state.h"
#include "gpu/vulkan/vulkan_device_queue.h"
#include "gpu/vulkan/vulkan_fence_helper.h"
#include "gpu/vulkan/vulkan_function_pointers.h"
#include "third_party/skia/include/core/SkSurface.h"
#include "third_party/skia/include/gpu/ganesh/GrBackendSurface.h"
#include "third_party/skia/include/gpu/ganesh/GrDirectContext.h"
#include "third_party/skia/include/gpu/ganesh/SkSurfaceGanesh.h"
#include "third_party/skia/include/gpu/ganesh/vk/GrVkBackendSemaphore.h"
#include "third_party/skia/include/gpu/ganesh/vk/GrVkBackendSurface.h"
#include "third_party/skia/include/gpu/ganesh/vk/GrVkTypes.h"
#include "third_party/skia/include/gpu/vk/VulkanMutableTextureState.h"
#include "ui/gfx/presentation_feedback.h"
#include "ui/ozone/platform/ohos/ohos_output_color_config.h"
#include "ui/ozone/platform/ohos/ohos_phone_gpu_channel.h"
#include "ui/ozone/platform/ohos/ohos_phone_gpu_shared_buffer.h"

namespace viz {
namespace {
// VK_OHOS_external_memory ABI from the SDK. Chromium's Khronos headers do not
// declare it. Keep these structures in sync with vulkan_ohos.h, like Dawn's
// OHOS SharedTextureMemory importer.
struct NativeFormat {
  VkStructureType sType = static_cast<VkStructureType>(1000452002);
  void* pNext = nullptr;
  VkFormat format = VK_FORMAT_UNDEFINED;
  uint64_t externalFormat = 0;
  VkFormatFeatureFlags formatFeatures = 0;
  VkComponentMapping components{};
  VkSamplerYcbcrModelConversion model{};
  VkSamplerYcbcrRange range{};
  VkChromaLocation x{}, y{};
};
struct NativeProperties {
  VkStructureType sType = static_cast<VkStructureType>(1000452001);
  void* pNext = nullptr;
  VkDeviceSize allocationSize = 0;
  uint32_t memoryTypeBits = 0;
};
struct ImportNative {
  VkStructureType sType = static_cast<VkStructureType>(1000452003);
  const void* pNext = nullptr;
  OH_NativeBuffer* buffer = nullptr;
};
using GetNativeProperties = VkResult(VKAPI_PTR*)(VkDevice,
                                                 const OH_NativeBuffer*,
                                                 NativeProperties*);

[[noreturn]] void Fail(const char* stage, VkResult result = VK_ERROR_UNKNOWN) {
  LOG(ERROR) << "OHOS phone Vulkan: " << stage << " failed (" << result
             << "); restarting GPU on GL";
  ui::ReportOhosPhoneVulkanFailure();
  // The browser detaches outstanding leases, never reuses an unfinished image.
  _exit(75);
}
void Check(VkResult result, const char* stage) {
  if (result != VK_SUCCESS)
    Fail(stage, result);
}
class ImportTimeout final : public base::Watchdog::Delegate {
 public:
  void Alarm() override {
    LOG(ERROR)
        << "OHOS phone Vulkan: buffer import timed out; restarting on GL";
    // Do not take the channel lock from this thread. Disconnect also latches
    // the browser's session fallback flag.
    _exit(75);
  }
};
constexpr VkImageUsageFlags kUsage =
    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
}  // namespace

struct SkiaOutputDeviceOhosPhoneVulkan::State {
  struct Imported {
    OH_NativeBuffer* native = nullptr;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize bytes = 0;
    sk_sp<SkSurface> surface;
  };
  raw_ptr<VulkanContextProvider> provider;
  raw_ptr<gpu::MemoryTypeTracker> tracker;
  uint64_t widget;
  ui::OhosOutputColor color;
  ui::OhosHdrDisplayGuard display_guard;
  uint64_t color_generation = 0;
  VkFormat imported_format = VK_FORMAT_UNDEFINED;
  SkColorType imported_color_type = kRGBA_1010102_SkColorType;
  SkColorType color_type() const {
    return color.mode == ui::OhosOutputMode::kSdr8 ? kRGBA_8888_SkColorType
                                                   : imported_color_type;
  }
  [[noreturn]] void ColorFailure(const char* stage) {
    if (color.mode == ui::OhosOutputMode::kSdr8)
      Fail(stage);
    ui::NoteOhosOutputColorFailure(stage);
    ui::ReportOhosPhoneOutputColorFailure();
    _exit(76);  // Browser preserves Vulkan, the next GPU negotiates SDR8.
  }
  void CheckImport(VkResult result, const char* stage) {
    if (result == VK_ERROR_DEVICE_LOST)
      Fail(stage, result);
    if (result != VK_SUCCESS)
      ColorFailure(stage);
  }
  SkImageInfo info;
  int samples = 1;
  std::map<uint32_t, Imported> imports;
  sk_sp<SkSurface> scratch;
  sk_sp<SkSurface> current;
  uint64_t lease = 0;
  VkSemaphore begin = VK_NULL_HANDLE, end = VK_NULL_HANDLE;
  bool submitted = false, logged = false;
  ImportTimeout timeout;
  base::Watchdog watchdog{base::Seconds(2), "PhoneVulkanImport", true,
                          &timeout};
  GetNativeProperties get_properties = nullptr;
  PFN_vkImportSemaphoreFdKHR import_fence = nullptr;
  PFN_vkGetSemaphoreFdKHR export_fence = nullptr;

  VkDevice device() { return provider->GetDeviceQueue()->GetVulkanDevice(); }
  gpu::VulkanFenceHelper* helper() {
    return provider->GetDeviceQueue()->GetFenceHelper();
  }
  void RetireSemaphores() {
    if (begin)
      helper()->EnqueueSemaphoreCleanupForSubmittedWork(
          std::exchange(begin, VK_NULL_HANDLE));
    if (end)
      helper()->EnqueueSemaphoreCleanupForSubmittedWork(
          std::exchange(end, VK_NULL_HANDLE));
    helper()->GenerateCleanupFence();
    helper()->ProcessCleanupTasks();
  }
  void Clear() {
    // Only resize/cache eviction/teardown. Normal frames never wait on the CPU.
    provider->GetGrContext()->flushAndSubmit(GrSyncCpu::kYes);
    current.reset();
    scratch.reset();
    for (auto& [key, item] : imports) {
      item.surface.reset();
      vkDestroyImage(device(), item.image, nullptr);
      vkFreeMemory(device(), item.memory, nullptr);
      OH_NativeBuffer_Unreference(item.native);
      tracker->TrackMemFree(item.bytes);
    }
    imports.clear();
  }
  void Import(ui::phone_gpu::SharedMessage& message, Imported& item) {
    // Import before TakeFence: the parcel decoder validates the FD count.
    item.native = ui::phone_gpu::ImportSharedBuffer(message);
    if (!item.native)
      ColorFailure("NativeBuffer parcel import");
    NativeFormat format;
    NativeProperties properties;
    properties.pNext = &format;
    CheckImport(get_properties(device(), item.native, &properties),
                "buffer properties");
    const auto required = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                          VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    const bool matching =
        color.mode == ui::OhosOutputMode::kSdr8
            ? format.format == VK_FORMAT_R8G8B8A8_UNORM
            : (format.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ||
               format.format == VK_FORMAT_A2R10G10B10_UNORM_PACK32);
    if (!matching ||
        (imported_format != VK_FORMAT_UNDEFINED &&
         imported_format != format.format) ||
        (format.formatFeatures & required) != required ||
        !properties.allocationSize || !properties.memoryTypeBits)
      ColorFailure("matching renderable native format");

    imported_format = format.format;
    if (color.mode != ui::OhosOutputMode::kSdr8) {
      imported_color_type = format.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32
                                ? kRGBA_1010102_SkColorType
                                : kBGRA_1010102_SkColorType;
      info = info.makeColorType(imported_color_type);
    }
    VkExternalMemoryImageCreateInfo external{
        VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    external.handleTypes = 0x00008000;  // OH_NATIVE_BUFFER_BIT_OHOS
    VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    create.pNext = &external;
    create.imageType = VK_IMAGE_TYPE_2D;
    create.format = format.format;
    create.extent = {static_cast<uint32_t>(info.width()),
                     static_cast<uint32_t>(info.height()), 1};
    create.mipLevels = create.arrayLayers = 1;
    create.samples = VK_SAMPLE_COUNT_1_BIT;
    create.tiling = VK_IMAGE_TILING_OPTIMAL;
    create.usage = kUsage;
    create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult result = vkCreateImage(device(), &create, nullptr, &item.image);
    if (result == VK_ERROR_INVALID_EXTERNAL_HANDLE ||
        result == VK_ERROR_FORMAT_NOT_SUPPORTED) {
      external.handleTypes = 0x00002000;  // older OHOS_NATIVE_BUFFER_BIT
      result = vkCreateImage(device(), &create, nullptr, &item.image);
    }
    CheckImport(result, "create imported image");
    // Like AHardwareBuffer import, the native buffer supplies allocation size
    // and memory type bits. Do not query unbound-image memory requirements.
    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(
        provider->GetDeviceQueue()->GetVulkanPhysicalDevice(), &memory);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
      if (properties.memoryTypeBits & (1u << i)) {
        type = i;
        if (memory.memoryTypes[i].propertyFlags &
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
          break;
      }
    }
    if (type == UINT32_MAX)
      ColorFailure("buffer memory type");
    ImportNative native;
    native.buffer = item.native;
    VkMemoryDedicatedAllocateInfo dedicated{
        VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.pNext = &native;
    dedicated.image = item.image;
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.pNext = &dedicated;
    allocate.allocationSize = properties.allocationSize;
    allocate.memoryTypeIndex = type;
    CheckImport(vkAllocateMemory(device(), &allocate, nullptr, &item.memory),
                "import memory");
    CheckImport(vkBindImageMemory(device(), item.image, item.memory, 0),
                "bind memory");
    item.bytes = properties.allocationSize;
    tracker->TrackMemAlloc(item.bytes);

    GrVkImageInfo image;
    image.fImage = item.image;
    image.fImageTiling = VK_IMAGE_TILING_OPTIMAL;
    image.fImageLayout = VK_IMAGE_LAYOUT_GENERAL;
    image.fFormat = format.format;
    image.fImageUsageFlags = kUsage;
    image.fSampleCount = image.fLevelCount = 1;
    image.fCurrentQueueFamily = VK_QUEUE_FAMILY_FOREIGN_EXT;
    auto texture =
        GrBackendTextures::MakeVk(info.width(), info.height(), image);
    item.surface = SkSurfaces::WrapBackendTexture(
        provider->GetGrContext(), texture, kTopLeft_GrSurfaceOrigin, samples,
        color_type(), info.refColorSpace(), nullptr);
    if (!item.surface)
      ColorFailure("Skia wrap imported image");
  }
};

SkiaOutputDeviceOhosPhoneVulkan::SkiaOutputDeviceOhosPhoneVulkan(
    VulkanContextProvider* provider,
    gpu::SurfaceHandle widget,
    bool requires_alpha,
    gpu::MemoryTracker* memory_tracker,
    DidSwapBufferCompleteCallback callback)
    : SkiaOutputDevice(provider->GetGrContext(),
                       nullptr,
                       memory_tracker,
                       callback),
      state_(std::make_unique<State>()) {
  CHECK(ui::IsOhosPhoneGpuChild());
  auto& s = *state_;
  s.provider = provider;
  s.tracker = memory_type_tracker_.get();
  s.widget = widget;
  s.get_properties = reinterpret_cast<GetNativeProperties>(
      vkGetDeviceProcAddr(s.device(), "vkGetNativeBufferPropertiesOHOS"));
  s.import_fence = reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(
      vkGetDeviceProcAddr(s.device(), "vkImportSemaphoreFdKHR"));
  s.export_fence = reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(
      vkGetDeviceProcAddr(s.device(), "vkGetSemaphoreFdKHR"));
  if (!s.get_properties || !s.import_fence || !s.export_fence)
    Fail("required device entry points");
  // Probe the real queue before advertising this output's precision to Viz.
  // No pre-fork Vulkan work and no assumption that a panel flag is sufficient.
  s.color = requires_alpha ? ui::OhosOutputColor{}
                           : ui::GetOhosRequestedOutputColor();
  if (s.color.mode == ui::OhosOutputMode::kHdr10 && !s.display_guard.Validate())
    s.color = {};
  {
    ui::phone_gpu::SharedMessage probe;
    if (ui::ProbeOhosPhoneOutputColor(s.widget, s.color, &probe)) {
      s.color_generation = probe.packet.color_generation;
      auto space = s.color.mode == ui::OhosOutputMode::kHdr10
                       ? gfx::ColorSpace::CreateHDR10()
                       : gfx::ColorSpace::CreateSRGB();
      s.info = SkImageInfo::Make(probe.packet.width, probe.packet.height,
                                 s.color_type(), kOpaque_SkAlphaType,
                                 space.ToSkColorSpace());
      s.watchdog.Arm();
      s.Import(probe, s.imports[probe.packet.prefix[0]]);
      s.watchdog.Disarm();
      // Only imported/wrapped, never drawn: safe to return with its original
      // acquire fence. First actual Present is logged separately as active.
      s.Clear();
      ui::CancelOhosPhoneGpuBuffer(s.widget, probe.packet.lease);
    } else {
      LOG(WARNING)
          << "OHOS phone Vulkan: color probe unavailable/rejected; SDR8";
      s.color = {};
    }
  }
  capabilities_.ohos_output_color_spaces =
      ui::MakeOhosOutputColorSpaces(s.color);
  capabilities_.ohos_content_output_luminance = true;
  capabilities_.ohos_dynamic_output_luminance =
      s.color.mode == ui::OhosOutputMode::kHdr10 &&
      ui::OhosDynamicOutputLuminance();
  capabilities_.uses_default_gl_framebuffer = false;
  capabilities_.pending_swap_params.max_pending_swaps = 1;
  capabilities_.output_surface_origin = gfx::SurfaceOrigin::kTopLeft;
  capabilities_.orientation_mode = OutputSurface::OrientationMode::kLogic;
  // Each lease may be a different buffer: request a full redraw, not preserved
  // damage. Cache only Vulkan imports, never assume previous pixels survive.
  capabilities_.supports_post_sub_buffer = false;
  capabilities_.supports_target_damage = false;
  capabilities_.sk_color_type_map[SinglePlaneFormat::kRGBA_8888] =
      kRGBA_8888_SkColorType;
  // The shared NativeBuffer is RGBA8 even for an opaque RGBX/BGRX
  // request. Reshape keeps the caller's alpha type independently.
  capabilities_.sk_color_type_map[SinglePlaneFormat::kRGBX_8888] =
      kRGBA_8888_SkColorType;
  capabilities_.sk_color_type_map[SinglePlaneFormat::kBGRA_8888] =
      kRGBA_8888_SkColorType;
  capabilities_.sk_color_type_map[SinglePlaneFormat::kBGRX_8888] =
      kRGBA_8888_SkColorType;
  if (s.color.mode != ui::OhosOutputMode::kSdr8)
    capabilities_.sk_color_type_map[SinglePlaneFormat::kRGBA_1010102] =
        s.color_type();
  LOG(WARNING) << "OHOS phone Vulkan: negotiated output mode="
               << static_cast<uint32_t>(s.color.mode)
               << " native_format=" << ui::OhosOutputNativeFormat(s.color)
               << " vk_format=" << s.imported_format
               << " sk_color_type=" << s.color_type()
               << " color_generation=" << s.color_generation
               << " white_millinits=" << s.color.white_millinits
               << " peak_millinits=" << s.color.peak_millinits;
}

SkiaOutputDeviceOhosPhoneVulkan::~SkiaOutputDeviceOhosPhoneVulkan() {
  auto& s = *state_;
  if (s.lease)
    ui::DiscardOhosPhoneGpuBuffer(s.widget, std::exchange(s.lease, 0));
  s.Clear();
  s.RetireSemaphores();
}
bool SkiaOutputDeviceOhosPhoneVulkan::Reshape(const ReshapeParams& params) {
  auto& s = *state_;
  CHECK(!s.lease);
  if (params.ohos_output_color && *params.ohos_output_color != s.color) {
    const auto color = *params.ohos_output_color;
    if (!ui::ValidOhosOutputColor(color) || color.mode != s.color.mode)
      s.ColorFailure("output brightness contract changed");
    s.color = color;
    LOG(WARNING) << "OHOS phone Vulkan: HDR luminance updated white_millinits="
                 << color.white_millinits
                 << " peak_millinits=" << color.peak_millinits;
  }
  if (params.image_info.colorType() != s.color_type() ||
      params.color_space.IsHDR() !=
          (s.color.mode == ui::OhosOutputMode::kHdr10)) {
    s.ColorFailure("output color contract changed");
  }
  // A brightness update changes color conversion/metadata, not the buffer's
  // dimensions or VkFormat. Preserve imports and avoid a CPU/GPU wait.
  if (s.info != params.image_info || s.samples != params.sample_count)
    s.Clear();
  s.info = params.image_info;
  s.samples = params.sample_count;
  return !s.info.isEmpty();
}
SkSurface* SkiaOutputDeviceOhosPhoneVulkan::BeginPaint(
    std::vector<GrBackendSemaphore>* semaphores) {
  auto& s = *state_;
  CHECK(!s.lease && !s.begin && !s.end);
  if (s.color.mode == ui::OhosOutputMode::kHdr10 && !s.display_guard.Validate())
    s.ColorFailure("HDR display changed or unavailable");
  s.submitted = false;
  ui::phone_gpu::Frame frame;
  frame.widget = s.widget;
  frame.width = s.info.width();
  frame.height = s.info.height();
  ui::phone_gpu::SharedMessage message;
  if (!ui::AcquireOhosPhoneGpuBuffer(frame, &message, s.color,
                                     s.color_generation)) {
    if (message.packet.operation != ui::phone_gpu::SharedOp::kUnavailable ||
        message.packet.result !=
            static_cast<uint32_t>(ui::phone_gpu::Result::kDropped))
      s.ColorFailure("acquire shared buffer");
    // Surface registration/rotation can temporarily precede the native window.
    // Keep Viz alive and drop this frame; retry the real queue next frame.
    if (!s.scratch)
      s.scratch = SkSurfaces::RenderTarget(
          s.provider->GetGrContext(), skgpu::Budgeted::kYes, s.info, s.samples,
          kTopLeft_GrSurfaceOrigin, nullptr);
    s.current = s.scratch;
    if (!s.current)
      Fail("temporary render target");
    return s.current.get();
  }
  if (message.packet.color != s.color ||
      message.packet.color_generation != s.color_generation)
    s.ColorFailure("stale color generation");
  s.lease = message.packet.lease;
  s.watchdog.Arm();
  base::ScopedClosureRunner disarm(
      base::BindOnce(&base::Watchdog::Disarm, base::Unretained(&s.watchdog)));
  const uint32_t key = message.packet.prefix[0];
  if (!s.imports.contains(key)) {
    if (s.imports.size() >= 8)
      s.Clear();
    s.Import(message, s.imports[key]);
  }
  s.current = s.imports.at(key).surface;
  base::ScopedFD fence(message.TakeFence());
  if (fence.is_valid()) {
    VkSemaphoreCreateInfo create{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    Check(vkCreateSemaphore(s.device(), &create, nullptr, &s.begin),
          "acquire semaphore");
    VkImportSemaphoreFdInfoKHR import{
        VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
    import.semaphore = s.begin;
    import.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
    import.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    import.fd = fence.get();
    Check(s.import_fence(s.device(), &import), "acquire fence import");
    (void)fence.release();  // Vulkan owns the fd after successful import.
    auto semaphore = GrBackendSemaphores::MakeVk(s.begin);
    if (!s.current->wait(1, &semaphore, false))
      Fail("Skia acquire fence wait");
  }
  VkExportSemaphoreCreateInfo export_info{
      VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
  export_info.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
  VkSemaphoreCreateInfo create{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  create.pNext = &export_info;
  Check(vkCreateSemaphore(s.device(), &create, nullptr, &s.end),
        "completion semaphore");
  return s.current.get();
}
void SkiaOutputDeviceOhosPhoneVulkan::Submit(
    scoped_refptr<gpu::SharedContextState> context_state,
    bool sync_cpu,
    base::OnceClosure callback) {
  auto& s = *state_;
  if (s.lease) {
    auto state = skgpu::MutableTextureStates::MakeVulkan(
        VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_FOREIGN_EXT);
    auto semaphore = GrBackendSemaphores::MakeVk(s.end);
    GrFlushInfo flush{};
    flush.fNumSemaphores = 1;
    flush.fSignalSemaphores = &semaphore;
    // Release ownership before signaling; base Submit then sends both drawing
    // and this release barrier to the Vulkan queue without a CPU wait.
    if (context_state->gr_context()->flush(s.current.get(), flush, &state) !=
        GrSemaphoresSubmitted::kYes)
      Fail("release image and signal fence");
  }
  s.submitted = true;
  SkiaOutputDevice::Submit(std::move(context_state), sync_cpu,
                           std::move(callback));
}
void SkiaOutputDeviceOhosPhoneVulkan::EndPaint() {}
void SkiaOutputDeviceOhosPhoneVulkan::Present(
    const std::optional<gfx::Rect>& update_rect,
    BufferPresentedCallback feedback,
    OutputSurfaceFrame frame) {
  auto& s = *state_;
  auto result = ui::phone_gpu::Result::kDropped;
  if (s.lease) {
    CHECK(s.submitted);
    VkSemaphoreGetFdInfoKHR get{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
    get.semaphore = s.end;
    get.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    int fd = -1;
    Check(s.export_fence(s.device(), &get, &fd), "completion fence export");
    // A successful Vulkan export may return -1 for an already signaled fence.
    result = ui::PresentOhosPhoneGpuBuffer(s.widget, std::exchange(s.lease, 0),
                                           fd, s.color, s.color_generation);
    if (result == ui::phone_gpu::Result::kFailed)
      s.ColorFailure("present shared buffer");
    if (result == ui::phone_gpu::Result::kPresented &&
        !std::exchange(s.logged, true))
      LOG(WARNING) << "OHOS phone Vulkan: first shared NativeBuffer frame "
                      "presented; active mode="
                   << static_cast<uint32_t>(s.color.mode)
                   << " native_format=" << ui::OhosOutputNativeFormat(s.color)
                   << " color_generation=" << s.color_generation;
  }
  s.current.reset();
  s.RetireSemaphores();
  base::TimeTicks timebase;
  base::TimeDelta interval;
  ui::GetOhosPhoneGpuTiming(s.widget, &timebase, &interval);
  // Submission acknowledgement, not a hardware display timestamp.
  auto presentation =
      result == ui::phone_gpu::Result::kPresented
          ? gfx::PresentationFeedback(base::TimeTicks::Now(), interval, 0)
          : gfx::PresentationFeedback::Failure();
  const gfx::Size size(s.info.width(), s.info.height());
  StartSwapBuffers({});
  FinishSwapBuffers(gfx::SwapCompletionResult(gfx::SwapResult::SWAP_ACK), size,
                    std::move(frame));
  std::move(feedback).Run(presentation);
}
void SkiaOutputDeviceOhosPhoneVulkan::SetPreferredFrameInterval(
    base::TimeDelta interval) {
  ui::SetOhosPhoneGpuFrameInterval(state_->widget, interval);
}
}  // namespace viz
