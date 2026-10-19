// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "components/viz/service/display_embedder/skia_output_device_ohos_graphite.h"

#include <linux/sync_file.h>
#include <native_buffer/native_buffer.h>
#include <native_window/external_window.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vulkan/vulkan_core.h>
#include <cstring>
#include <map>
#include <utility>
#include "base/compiler_specific.h"
#include "base/files/scoped_file.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/ohos_startup_probe.h"
#include "base/threading/watchdog.h"
#include "base/trace_event/trace_event.h"
#include "gpu/command_buffer/service/dawn_context_provider.h"
#include "gpu/command_buffer/service/graphite_shared_context.h"
#include "gpu/command_buffer/service/memory_tracking.h"
#include "gpu/command_buffer/service/shared_context_state.h"
#include "third_party/skia/include/core/SkSurface.h"
#include "third_party/skia/include/gpu/graphite/BackendTexture.h"
#include "third_party/skia/include/gpu/graphite/Context.h"
#include "third_party/skia/include/gpu/graphite/Surface.h"
#include "third_party/skia/include/gpu/graphite/dawn/DawnTypes.h"
#include "ui/gfx/presentation_feedback.h"
#include "ui/ozone/platform/ohos/ohos_gpu_child_channel.h"
#include "ui/ozone/platform/ohos/ohos_native_window_registry.h"
#include "ui/ozone/platform/ohos/ohos_output_color_config.h"
#include "ui/ozone/platform/ohos/ohos_phone_gpu_channel.h"
#include "ui/ozone/platform/ohos/ohos_phone_gpu_shared_buffer.h"

namespace viz {
namespace {
[[noreturn]] void Fail(const char* stage) {
  LOG(ERROR) << "OHOS Graphite: " << stage << "; restarting on Ganesh Vulkan";
  ui::NoteOhosGraphiteFailure(stage);
  _exit(77);
}
class ImportTimeout final : public base::Watchdog::Delegate {
 public:
  void Alarm() override { Fail("native buffer import timed out"); }
};
constexpr wgpu::TextureUsage kUsage =
    wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding |
    wgpu::TextureUsage::CopySrc | wgpu::TextureUsage::CopyDst;
}  // namespace

struct SkiaOutputDeviceOhosGraphite::State {
  struct Imported {
    OH_NativeBuffer* native = nullptr;
    wgpu::SharedTextureMemory memory;
    uint64_t bytes = 0;
  };
  scoped_refptr<gpu::SharedContextState> context;
  raw_ptr<gpu::MemoryTypeTracker> tracker;
  uint64_t widget = 0;
  ui::OhosOutputColor color;
  bool hdr_capable = false, p3_capable = false;
  ui::OhosHdrDisplayGuard display_guard;
  uint64_t color_generation = 0;
  wgpu::TextureFormat imported_format = wgpu::TextureFormat::Undefined;
  SkColorType color_type() const {
    return color.mode == ui::OhosOutputMode::kSdr8 ? kRGBA_8888_SkColorType
                                                   : kRGBA_1010102_SkColorType;
  }
  [[noreturn]] void ColorFailure(const char* stage) { Fail(stage); }
  SkImageInfo info;
  int samples = 1;
  std::map<uint32_t, Imported> imports;
  sk_sp<SkSurface> current;
  wgpu::Texture texture;
  wgpu::SharedTextureMemory memory;
  uint64_t lease = 0;
  bool submitted = false, logged = false;
  ImportTimeout timeout;
  base::Watchdog watchdog{base::Seconds(2), "OhosGraphiteImport", true,
                          &timeout};
  // The same queue contract on both devices. The Pad owns its NativeWindow;
  // the phone broker owns it and exchanges only native handles and fences.
  ui::phone_gpu::SharedBufferColorState local_colors;
  std::unique_ptr<ui::phone_gpu::SharedBufferLease> local_lease;
  uint64_t local_sequence = 0;
  void* local_window = nullptr;  // Referenced by local_colors after Prepare.

  wgpu::Device device() {
    return context->dawn_context_provider()->GetDevice();
  }
  void Clear() {
    if (!context->context_lost())
      context->graphite_shared_context()->submit(
          skgpu::graphite::SyncToCpu::kYes);
    current.reset();
    texture = nullptr;
    memory = nullptr;
    for (auto& [key, item] : imports) {
      item.memory = nullptr;
      OH_NativeBuffer_Unreference(item.native);
      tracker->TrackMemFree(item.bytes);
    }
    imports.clear();
  }
  bool AcquireLocal(const ui::OhosOutputColor& requested,
                    bool probe,
                    ui::phone_gpu::SharedMessage* message) {
    CHECK(!local_lease);
    auto surface = ui::AcquireOhosNativeSurfaceForPhoneGpu(widget);
    if (!surface && probe && ui::IsOhosNativeSurfaceExpected(widget)) {
      ui::WaitForOhosNativeSurface(widget, base::Seconds(2));
      surface = ui::AcquireOhosNativeSurfaceForPhoneGpu(widget);
    }
    if (!surface || !surface->window)
      return false;
    base::ScopedClosureRunner unref(base::BindOnce(
        [](void* window) { OH_NativeWindow_NativeObjectUnreference(window); },
        surface->window));
    if (local_window && local_window != surface->window)
      Clear();  // A same-sized replacement must not reuse old queue imports.
    local_window = surface->window;
    auto& p = message->packet;
    p.operation = ui::phone_gpu::SharedOp::kBuffer;
    p.widget = widget;
    p.sequence = ++local_sequence;
    p.lease = p.sequence;
    p.color = requested;
    p.color_generation = probe ? p.sequence : color_generation;
    p.width = probe ? surface->bounds.width() : info.width();
    p.height = probe ? surface->bounds.height() : info.height();
    if (!p.width || !p.height || p.width > 8192 || p.height > 8192 ||
        (!probe && (surface->bounds.width() != info.width() ||
                    surface->bounds.height() != info.height())))
      return false;
    if (!local_colors.Prepare(surface->window, widget, requested, true))
      Fail("native window color preparation");
    auto pending = std::make_unique<ui::phone_gpu::SharedBufferLease>();
    auto result = pending->Acquire(surface->window, message);
    if (result == ui::phone_gpu::SharedBufferLease::AcquireResult::kFailed)
      Fail("native window allocation");
    if (result != ui::phone_gpu::SharedBufferLease::AcquireResult::kAcquired)
      return false;
    local_lease = std::move(pending);
    return true;
  }
  bool Probe(const ui::OhosOutputColor& requested,
             ui::phone_gpu::SharedMessage* message) {
    if (ui::IsOhosPhoneGpuChild())
      return ui::ProbeOhosPhoneOutputColor(widget, requested, message);
    if ((requested.mode == ui::OhosOutputMode::kP3Sdr10 &&
         !ui::OhosSingleDisplaySupportsP3()) ||
        (requested.mode == ui::OhosOutputMode::kHdr10 &&
         !display_guard.Validate()))
      return false;
    return AcquireLocal(requested, true, message);
  }
  bool Acquire(ui::phone_gpu::Frame frame,
               ui::phone_gpu::SharedMessage* message) {
    if (ui::IsOhosPhoneGpuChild())
      return ui::AcquireOhosPhoneGpuBuffer(frame, message, color,
                                           color_generation);
    if (AcquireLocal(color, false, message))
      return true;
    message->packet.operation = ui::phone_gpu::SharedOp::kUnavailable;
    message->packet.result =
        static_cast<uint32_t>(ui::phone_gpu::Result::kDropped);
    return false;
  }
  void Cancel(uint64_t id) {
    if (ui::IsOhosPhoneGpuChild()) {
      ui::CancelOhosPhoneGpuBuffer(widget, id);
    } else {
      CHECK(local_lease);
      local_lease->CancelBeforeDrawing();
      local_lease.reset();
    }
  }
  bool Ready(const ui::OhosOutputColor& verified) {
    return ui::IsOhosPhoneGpuChild()
               ? ui::ReportOhosPhoneOutputColorReady(widget, verified,
                                                     color_generation)
               : ui::ReportOhosWsiOutputCapabilities(
                     widget,
                     hdr_capable || verified.mode == ui::OhosOutputMode::kHdr10,
                     p3_capable ||
                         verified.mode == ui::OhosOutputMode::kP3Sdr10);
  }
  ui::phone_gpu::Result Present(int fd) {
    if (ui::IsOhosPhoneGpuChild())
      return ui::PresentOhosPhoneGpuBuffer(widget, std::exchange(lease, 0), fd,
                                           color, color_generation);
    CHECK(local_lease);
    auto surface = ui::AcquireOhosNativeSurfaceForPhoneGpu(widget);
    const bool same =
        surface && surface->window == local_lease->window() &&
        surface->bounds.size() == gfx::Size(info.width(), info.height());
    if (surface && surface->window)
      OH_NativeWindow_NativeObjectUnreference(surface->window);
    auto result = ui::phone_gpu::Result::kDropped;
    if (same) {
      result = local_lease->Present(fd) ? ui::phone_gpu::Result::kPresented
                                        : ui::phone_gpu::Result::kFailed;
    } else if (fd >= 0) {
      close(fd);
    }
    local_lease.reset();  // Detaches an unfinished/stale window allocation.
    lease = 0;
    return result;
  }
  void Import(ui::phone_gpu::SharedMessage& message, Imported& item) {
    item.native = ui::phone_gpu::ImportSharedBuffer(message);
    if (!item.native)
      Fail("NativeBuffer parcel import");
    wgpu::SharedTextureMemoryAHardwareBufferDescriptor native;
    native.handle = item.native;
    wgpu::SharedTextureMemoryDescriptor descriptor;
    descriptor.nextInChain = &native;
    descriptor.label = "OhosGraphiteOutput";
    item.memory = device().ImportSharedTextureMemory(&descriptor);
    wgpu::SharedTextureMemoryProperties properties;
    if (!item.memory ||
        item.memory.GetProperties(&properties) != wgpu::Status::Success)
      Fail("Dawn NativeBuffer import");
    const auto expected = color.mode == ui::OhosOutputMode::kSdr8
                              ? wgpu::TextureFormat::RGBA8Unorm
                              : wgpu::TextureFormat::RGB10A2Unorm;
    if (properties.format != expected ||
        properties.size.width != message.packet.width ||
        properties.size.height != message.packet.height ||
        (properties.usage & kUsage) != kUsage)
      Fail("Dawn output format/usage contract");
    imported_format = properties.format;
    item.bytes = message.packet.prefix[7];
    tracker->TrackMemAlloc(item.bytes);
  }
  sk_sp<SkSurface> Wrap(wgpu::Texture target) {
    return SkSurfaces::WrapBackendTexture(
        context->gpu_main_graphite_recorder(),
        skgpu::graphite::BackendTextures::MakeDawn(target.Get()), color_type(),
        info.refColorSpace(), nullptr);
  }
};
SkiaOutputDeviceOhosGraphite::SkiaOutputDeviceOhosGraphite(
    scoped_refptr<gpu::SharedContextState> context_state,
    gpu::SurfaceHandle widget,
    bool requires_alpha,
    gpu::MemoryTracker* memory_tracker,
    DidSwapBufferCompleteCallback callback)
    : SkiaOutputDevice(nullptr,
                       context_state->graphite_shared_context(),
                       memory_tracker,
                       callback),
      state_(std::make_unique<State>()) {
  auto& s = *state_;
  s.context = std::move(context_state);
  s.tracker = memory_type_tracker_.get();
  s.widget = widget;
  if (!ui::IsOhosPhoneGpuChild() &&
      !ui::phone_gpu::PrepareSharedBufferAllocator())
    Fail("native allocator initialization");
  // Native anchored windows (including fullscreen controls) are composited
  // over the main output and must not publish independent HDR capabilities.
  const bool sdr_only = requires_alpha || ui::IsOhosAnchoredWindow(widget);
  s.color = sdr_only ? ui::OhosOutputColor{}
                     : ui::GetOhosRequestedOutputColor();
  if (s.color.mode == ui::OhosOutputMode::kHdr10 && !s.display_guard.Validate())
    s.color = {};
  {
    ui::phone_gpu::SharedMessage probe;
    if (s.Probe(s.color, &probe)) {
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
      s.Cancel(probe.packet.lease);
    } else {
      LOG(WARNING) << "OHOS Graphite: color probe unavailable/rejected; SDR8";
      s.color = {};
    }
  }
  capabilities_.ohos_output_color_spaces =
      ui::MakeOhosOutputColorSpaces(s.color);
  s.hdr_capable = s.color.mode == ui::OhosOutputMode::kHdr10;
  if (s.hdr_capable) {
    if (!s.Ready(s.color))
      s.ColorFailure("publish verified HDR capability");
    // Wide gamut alone does not make content HDR. Keep both SDR usages out
    // of the system's HDR tone mapper; only kHDR retains the PQ output.
    // Start WCG at sRGB; the separate verified P3 probe below may upgrade it.
    // The Skia surface and NativeBuffer tags must agree on that gamut.
    for (auto usage : {gfx::ContentColorUsage::kSRGB,
                       gfx::ContentColorUsage::kWideColorGamut}) {
      for (bool alpha : {false, true}) {
        capabilities_.ohos_output_color_spaces->SetOutputColorSpaceAndFormat(
            usage, alpha, gfx::ColorSpace::CreateSRGB(),
            SinglePlaneFormat::kRGBA_1010102);
      }
    }
  }
  if (!sdr_only && s.color.mode != ui::OhosOutputMode::kSdr8) {
    // Probe a real P3-tagged 10-bit buffer and import it before advertising
    // WCG. A rejected optional P3 probe leaves the proven SDR/HDR route
    // available.
    const auto original_color = s.color;
    ui::phone_gpu::SharedMessage probe;
    const ui::OhosOutputColor p3{ui::OhosOutputMode::kP3Sdr10};
    if (s.Probe(p3, &probe)) {
      s.color = p3;
      s.color_generation = probe.packet.color_generation;
      s.info = SkImageInfo::Make(
          probe.packet.width, probe.packet.height, s.color_type(),
          kOpaque_SkAlphaType,
          gfx::ColorSpace::CreateDisplayP3D65().ToSkColorSpace());
      s.watchdog.Arm();
      s.Import(probe, s.imports[probe.packet.prefix[0]]);
      s.watchdog.Disarm();
      s.Clear();
      s.Cancel(probe.packet.lease);
      if (!s.Ready(p3))
        s.ColorFailure("publish verified P3 capability");
      s.p3_capable = true;
      for (bool alpha : {false, true}) {
        capabilities_.ohos_output_color_spaces->SetOutputColorSpaceAndFormat(
            gfx::ContentColorUsage::kWideColorGamut, alpha,
            gfx::ColorSpace::CreateDisplayP3D65(),
            SinglePlaneFormat::kRGBA_1010102);
      }
      capabilities_.ohos_output_color_spaces->SetPrimaries(
          gfx::DisplayColorSpaces(gfx::ColorSpace::CreateDisplayP3D65())
              .GetPrimaries());
    } else {
      // A partially tagged probe may retire old buffers. Re-establish the
      // previous generation even when P3 cannot be used on this device.
      if (!s.Probe(original_color, &probe))
        s.ColorFailure("restore output after rejected P3 probe");
      s.color_generation = probe.packet.color_generation;
      s.Cancel(probe.packet.lease);
    }
  }
  capabilities_.ohos_dynamic_output_luminance =
      s.hdr_capable && ui::OhosDynamicOutputLuminance();
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
  LOG(WARNING) << "OHOS Graphite: negotiated output mode="
               << static_cast<uint32_t>(s.color.mode)
               << " native_format=" << ui::OhosOutputNativeFormat(s.color)
               << " vk_format=" << static_cast<uint32_t>(s.imported_format)
               << " sk_color_type=" << s.color_type()
               << " color_generation=" << s.color_generation
               << " white_millinits=" << s.color.white_millinits
               << " peak_millinits=" << s.color.peak_millinits;
}

SkiaOutputDeviceOhosGraphite::~SkiaOutputDeviceOhosGraphite() {
  auto& s = *state_;
  if (s.lease) {
    // An interrupted frame may still be in flight. Never return it for reuse.
    if (ui::IsOhosPhoneGpuChild())
      ui::DiscardOhosPhoneGpuBuffer(s.widget, s.lease);
    s.local_lease.reset();
  }
  s.Clear();
  if (!ui::IsOhosPhoneGpuChild())
    ui::ReportOhosWsiOutputCapabilities(s.widget, false, false);
}
bool SkiaOutputDeviceOhosGraphite::Reshape(const ReshapeParams& params) {
  auto& s = *state_;
  CHECK(!s.lease);
  if (params.ohos_output_color && *params.ohos_output_color != s.color) {
    const auto color = *params.ohos_output_color;
    if (!ui::ValidOhosOutputColor(color))
      s.ColorFailure("output brightness contract changed");
    if (color.mode != s.color.mode) {
      if (!ui::CanSwitchOhosPhoneOutputColor(s.color, color, s.hdr_capable,
                                             s.p3_capable))
        s.ColorFailure("unsupported output mode change");
      // Retire imported images before the browser cleans HDR-tagged buffers.
      // This happens on SDR/HDR content transitions, never per ordinary frame.
      s.Clear();
      ui::phone_gpu::SharedMessage probe;
      if (!s.Probe(color, &probe))
        s.ColorFailure("content output color probe");
      s.Cancel(probe.packet.lease);
      s.color_generation = probe.packet.color_generation;
      s.logged = false;  // Log the first presented frame of the new mode.
      LOG(WARNING) << "OHOS Graphite: content output mode="
                   << static_cast<uint32_t>(color.mode)
                   << " color_generation=" << s.color_generation;
    }
    s.color = color;
    LOG(WARNING) << "OHOS Graphite: output luminance updated white_millinits="
                 << color.white_millinits
                 << " peak_millinits=" << color.peak_millinits;
  }
  if ((s.color.mode == ui::OhosOutputMode::kP3Sdr10 &&
       params.color_space != gfx::ColorSpace::CreateDisplayP3D65()) ||
      params.image_info.colorType() != s.color_type() ||
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
SkSurface* SkiaOutputDeviceOhosGraphite::BeginPaint(
    std::vector<GrBackendSemaphore>* semaphores) {
  auto& s = *state_;
  CHECK(!s.lease);
  s.submitted = false;
  if (s.color.mode == ui::OhosOutputMode::kHdr10 && !s.display_guard.Validate())
    Fail("HDR display changed or unavailable");
  ui::phone_gpu::Frame frame;
  frame.widget = s.widget;
  frame.width = s.info.width();
  frame.height = s.info.height();
  ui::phone_gpu::SharedMessage message;
  if (!s.Acquire(frame, &message)) {
    if (message.packet.operation != ui::phone_gpu::SharedOp::kUnavailable ||
        message.packet.result !=
            static_cast<uint32_t>(ui::phone_gpu::Result::kDropped))
      Fail("acquire native output buffer");
    wgpu::TextureDescriptor descriptor;
    descriptor.format = s.color.mode == ui::OhosOutputMode::kSdr8
                            ? wgpu::TextureFormat::RGBA8Unorm
                            : wgpu::TextureFormat::RGB10A2Unorm;
    descriptor.usage = kUsage;
    descriptor.size = {frame.width, frame.height, 1};
    s.texture = s.device().CreateTexture(&descriptor);
    s.current = s.Wrap(s.texture);
    if (!s.current)
      Fail("temporary Graphite surface");
    return s.current.get();
  }
  if (message.packet.color != s.color ||
      message.packet.color_generation != s.color_generation)
    Fail("stale output color generation");
  s.lease = message.packet.lease;
  s.watchdog.Arm();
  base::ScopedClosureRunner disarm(
      base::BindOnce(&base::Watchdog::Disarm, base::Unretained(&s.watchdog)));
  const auto key = message.packet.prefix[0];
  if (!s.imports.contains(key)) {
    if (s.imports.size() >= 8)
      s.Clear();
    s.Import(message, s.imports[key]);
  }
  s.memory = s.imports.at(key).memory;
  base::ScopedFD acquire(message.TakeFence());
  wgpu::TextureDescriptor descriptor;
  descriptor.format = s.imported_format;
  descriptor.usage = kUsage;
  descriptor.size = {frame.width, frame.height, 1};
  s.texture = s.memory.CreateTexture(&descriptor);
  wgpu::SharedTextureMemoryBeginAccessDescriptor begin;
  // Full redraw, no reliance on old queue pixels. Dawn initializes safely.
  begin.initialized = false;
  wgpu::SharedTextureMemoryVkImageLayoutBeginState layout;
  layout.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  layout.newLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  begin.nextInChain = &layout;
  wgpu::SharedFence fence;
  uint64_t signaled = 1;
  if (acquire.is_valid()) {
    wgpu::SharedFenceSyncFDDescriptor sync;
    sync.handle = acquire.get();
    wgpu::SharedFenceDescriptor fence_descriptor;
    fence_descriptor.nextInChain = &sync;
    fence = s.device().ImportSharedFence(&fence_descriptor);
    begin.fenceCount = begin.signaledValueCount = 1;
    begin.fences = &fence;
    begin.signaledValues = &signaled;
  }
  if (s.memory.BeginAccess(s.texture, &begin) != wgpu::Status::Success)
    Fail("Dawn acquire fence/access");
  s.current = s.Wrap(s.texture);
  if (!s.current)
    Fail("Graphite wrap native output");
  return s.current.get();
}
void SkiaOutputDeviceOhosGraphite::EndPaint() {}
void SkiaOutputDeviceOhosGraphite::Submit(
    scoped_refptr<gpu::SharedContextState> context_state,
    bool sync_cpu,
    base::OnceClosure callback) {
  state_->submitted = true;
  SkiaOutputDevice::Submit(std::move(context_state), sync_cpu,
                           std::move(callback));
}
void SkiaOutputDeviceOhosGraphite::Present(
    const std::optional<gfx::Rect>& update_rect,
    BufferPresentedCallback feedback,
    OutputSurfaceFrame frame) {
  auto& s = *state_;
  auto result = ui::phone_gpu::Result::kDropped;
  if (s.lease) {
    CHECK(s.submitted);
    // The recording has been submitted. EndAccess releases foreign ownership
    // and exports the GPU completion fence, without a CPU wait or pixel copy.
    wgpu::SharedTextureMemoryVkImageLayoutEndState layout;
    layout.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    layout.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    wgpu::SharedTextureMemoryEndAccessState end;
    end.nextInChain = &layout;
    if (s.memory.EndAccess(s.texture, &end) != wgpu::Status::Success ||
        layout.newLayout != VK_IMAGE_LAYOUT_GENERAL)
      Fail("Dawn release native output");
    if (!end.fenceCount || end.fenceCount != end.signaledValueCount)
      Fail("missing output completion fence");
    base::ScopedFD completion;
    for (size_t i = 0; i < end.fenceCount; ++i) {
      if (UNSAFE_TODO(end.signaledValues[i]) != 1)
        Fail("invalid completion fence signal");
      wgpu::SharedFenceSyncFDExportInfo sync;
      wgpu::SharedFenceExportInfo info;
      UNSAFE_TODO(end.fences[i]).ExportInfo(&info);
      if (info.type != wgpu::SharedFenceType::SyncFD)
        Fail("unexpected completion fence type");
      info.nextInChain = &sync;
      UNSAFE_TODO(end.fences[i]).ExportInfo(&info);
      if (sync.handle == -1)
        continue;  // Already signaled sync fd.
      if (sync.handle < -1)
        Fail("invalid completion fence");
      if (!completion.is_valid()) {
        completion.reset(dup(sync.handle));
      } else {
        sync_merge_data merge{};
        strncpy(merge.name, "OhosGraphite", sizeof(merge.name) - 1);
        merge.fd2 = sync.handle;
        if (ioctl(completion.get(), SYNC_IOC_MERGE, &merge) < 0)
          Fail("merge completion fences");
        completion.reset(merge.fence);
      }
      if (!completion.is_valid())
        Fail("duplicate output completion fence");
    }
    result = s.Present(completion.release());
    if (result == ui::phone_gpu::Result::kFailed)
      Fail("present native output");
    if (result == ui::phone_gpu::Result::kPresented &&
        !std::exchange(s.logged, true)) {
      OHOS_STARTUP_MARK("Frame.GraphiteNativeAccepted",
                        static_cast<int64_t>(s.color.mode));
      LOG(WARNING) << "OHOS Graphite: first native frame presented; mode="
                   << static_cast<uint32_t>(s.color.mode)
                   << " phone_broker=" << ui::IsOhosPhoneGpuChild();
    }
  }
  s.current.reset();
  if (s.texture)
    s.texture.Destroy();
  s.texture = nullptr;
  s.memory = nullptr;
  base::TimeTicks timebase;
  base::TimeDelta interval;
  if (ui::IsOhosPhoneGpuChild())
    ui::GetOhosPhoneGpuTiming(s.widget, &timebase, &interval);
  auto presentation =
      result == ui::phone_gpu::Result::kPresented
          ? gfx::PresentationFeedback(base::TimeTicks::Now(), interval, 0)
          : gfx::PresentationFeedback::Failure();
  StartSwapBuffers({});
  FinishSwapBuffers(gfx::SwapCompletionResult(
                        result == ui::phone_gpu::Result::kPresented
                            ? gfx::SwapResult::SWAP_ACK
                            : gfx::SwapResult::SWAP_NAK_RECREATE_BUFFERS),
                    gfx::Size(s.info.width(), s.info.height()),
                    std::move(frame));
  std::move(feedback).Run(presentation);
}
void SkiaOutputDeviceOhosGraphite::SetPreferredFrameInterval(
    base::TimeDelta interval) {
  if (ui::IsOhosPhoneGpuChild())
    ui::SetOhosPhoneGpuFrameInterval(state_->widget, interval);
}
}  // namespace viz
