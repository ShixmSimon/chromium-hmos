// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "ui/ozone/platform/ohos/ohos_phone_gpu_shared_buffer.h"
#include <IPCKit/ipc_cparcel.h>
#include <dlfcn.h>
#include <native_buffer/native_buffer.h>
#include <native_window/external_window.h>
#include <unistd.h>
#include <atomic>
#include <cstring>
#include <utility>
#include "base/logging.h"
#include "ui/ozone/platform/ohos/ohos_native_window_registry.h"
#include "ui/ozone/platform/ohos/ohos_output_color_config.h"

namespace ui::phone_gpu {
static_assert(kSharedImageRgba8888 == NATIVEBUFFER_PIXEL_FMT_RGBA_8888);
static_assert(kSharedImageBgra8888 == NATIVEBUFFER_PIXEL_FMT_BGRA_8888);
static_assert(kSharedImageRgba16Float == NATIVEBUFFER_PIXEL_FMT_RGBA16_FLOAT);
namespace {
std::atomic<bool> ready{false};
using WriteBuffer = int32_t (*)(OH_NativeBuffer*, OHIPCParcel*);
using ReadBuffer = int32_t (*)(OHIPCParcel*, OH_NativeBuffer**);
WriteBuffer Writer() {
  return reinterpret_cast<WriteBuffer>(
      dlsym(RTLD_DEFAULT, "OH_NativeBuffer_WriteToParcel"));
}
ReadBuffer Reader() {
  return reinterpret_cast<ReadBuffer>(
      dlsym(RTLD_DEFAULT, "OH_NativeBuffer_ReadFromParcel"));
}
using Parcel = std::unique_ptr<OHIPCParcel, decltype(&OH_IPCParcel_Destroy)>;

// Failures only: successful frames do not add logging to the 120 Hz path.
bool BufferStep(const SharedPacket& packet, const char* step, int result) {
  if (result)
    LOG(ERROR) << "OHOS shared buffer: step=" << step
               << " native_error=" << result << " widget=" << packet.widget
               << " sequence=" << packet.sequence
               << " mode=" << static_cast<uint32_t>(packet.color.mode)
               << " format="
               << (packet.operation == SharedOp::kImage
                       ? packet.prefix[8]
                       : OhosOutputNativeFormat(packet.color))
               << " size=" << packet.width << "x" << packet.height;
  return result == 0;
}
bool Encode(OH_NativeBuffer* native, SharedMessage* m) {
  auto& p = m->packet;
  const uint32_t native_format = p.operation == SharedOp::kImage
                                     ? p.prefix[8]
                                     : OhosOutputNativeFormat(p.color);
  auto writer = Writer();
  if (!writer) {
    LOG(ERROR) << "OHOS shared buffer: step=parcel-writer unavailable";
    return false;
  }
  Parcel parcel(OH_IPCParcel_Create(), &OH_IPCParcel_Destroy);
  if (!parcel) {
    LOG(ERROR) << "OHOS shared buffer: step=parcel-create null";
    return false;
  }
  if (!BufferStep(p, "parcel-write", writer(native, parcel.get())))
    return false;
  constexpr size_t prefix_bytes = sizeof(p.prefix);
  const int data_size = OH_IPCParcel_GetDataSize(parcel.get());
  if (data_size < static_cast<int>(prefix_bytes)) {
    LOG(ERROR) << "OHOS shared buffer: step=parcel-size bytes=" << data_size
               << " required=" << prefix_bytes;
    return false;
  }
  const auto* prefix = OH_IPCParcel_ReadBuffer(parcel.get(), prefix_bytes);
  if (!prefix) {
    LOG(ERROR) << "OHOS shared buffer: step=parcel-prefix null";
    return false;
  }
  memcpy(p.prefix.data(), prefix, prefix_bytes);
  const auto& v = p.prefix;
  OH_NativeBuffer_Config config{};
  OH_NativeBuffer_GetConfig(native, &config);
  if (v[1] != 1 || v[2] >= kSharedMaxFds - 1 || v[3] > 256 ||
      config.width != static_cast<int32_t>(p.width) ||
      config.height != static_cast<int32_t>(p.height) ||
      config.format != static_cast<int32_t>(native_format) ||
      v[4] != p.width || v[6] != p.height ||
      v[8] != native_format || v[13] != 1) {
    LOG(ERROR) << "OHOS shared buffer: step=parcel-schema mode="
               << static_cast<uint32_t>(p.color.mode)
               << " requested=" << p.width << "x" << p.height << "/"
               << native_format << " native=" << config.width
               << "x" << config.height << "/" << config.format
               << " parcel=" << v[4] << "x" << v[6] << "/" << v[8]
               << " present=" << v[1] << " reserve_fds=" << v[2]
               << " reserve_ints=" << v[3] << " fd_present=" << v[13];
    return false;
  }
  p.fd_count = v[2] + 1;
  for (uint32_t i = 0; i < p.fd_count; ++i) {
    int fd = -1;
    if (!BufferStep(p, "parcel-read-fd",
                    OH_IPCParcel_ReadFileDescriptor(parcel.get(), &fd)))
      return false;
    if (fd < 0) {
      LOG(ERROR) << "OHOS shared buffer: step=parcel-fd invalid index=" << i;
      return false;
    }
    m->fds.push_back(fd);
  }
  int remaining = OH_IPCParcel_GetReadableBytes(parcel.get());
  if (remaining < 0 || remaining > static_cast<int>(p.tail.size()) ||
      (remaining != static_cast<int>(v[3] * 4) &&
       remaining != static_cast<int>(v[3] * 4 + 20))) {
    LOG(ERROR) << "OHOS shared buffer: step=parcel-tail-schema bytes="
               << remaining << " reserve_ints=" << v[3];
    return false;
  }
  p.tail_bytes = remaining;
  if (remaining) {
    auto* tail = OH_IPCParcel_ReadBuffer(parcel.get(), remaining);
    if (!tail) {
      LOG(ERROR) << "OHOS shared buffer: step=parcel-tail null";
      return false;
    }
    memcpy(p.tail.data(), tail, remaining);
  }
  if (!ValidSharedPacket(p, m->fds.size())) {
    LOG(ERROR) << "OHOS shared buffer: step=packet-validation mode="
               << static_cast<uint32_t>(p.color.mode)
               << " fd_count=" << p.fd_count << " received=" << m->fds.size()
               << " size=" << p.width << "x" << p.height
               << " stride=" << v[5] << " allocation_bytes=" << v[7]
               << " required_bytes=" << uint64_t{v[5]} * p.height
               << " sequence=" << p.sequence << " widget=" << p.widget
               << " lease=" << p.lease
               << " color_generation=" << p.color_generation
               << " has_fence=" << p.has_fence
               << " tail_bytes=" << p.tail_bytes
               << " reserve_ints=" << v[3];
    return false;
  }
  return true;
}
}  // namespace
bool PrepareSharedBufferAllocator() {
  if (!Writer() || !Reader())
    return false;
  OH_NativeBuffer_Config config{
      64, 64, 12,
      NATIVEBUFFER_USAGE_MEM_DMA | NATIVEBUFFER_USAGE_HW_RENDER |
          NATIVEBUFFER_USAGE_HW_TEXTURE | NATIVEBUFFER_USAGE_CPU_READ,
      0};
  auto* buffer = OH_NativeBuffer_Alloc(&config);
  if (!buffer)
    return false;
  OH_NativeBuffer_Unreference(buffer);
  ready.store(true);
  return true;
}
bool SharedBufferAllocatorReady() {
  return ready.load();
}
bool AllocateSharedImage(uint32_t width,
                         uint32_t height,
                         uint32_t native_format,
                         SharedMessage* response) {
  if (!SharedBufferAllocatorReady() || !IsSharedImageFormat(native_format))
    return false;
  SharedPacket request;
  request.operation = SharedOp::kAllocateImage;
  request.sequence = response->packet.sequence;
  request.widget = response->packet.widget;
  request.width = width;
  request.height = height;
  request.prefix[8] = native_format;
  if (!ValidSharedPacket(request, 0))
    return false;
  OH_NativeBuffer_Config config{};
  config.width = static_cast<int32_t>(width);
  config.height = static_cast<int32_t>(height);
  config.format = static_cast<int32_t>(native_format);
  config.usage = NATIVEBUFFER_USAGE_MEM_DMA | NATIVEBUFFER_USAGE_HW_RENDER |
                 NATIVEBUFFER_USAGE_HW_TEXTURE;
  auto* native = OH_NativeBuffer_Alloc(&config);
  if (!native)
    return false;
  response->packet = request;
  response->packet.operation = SharedOp::kImage;
  const bool exported = Encode(native, response);
  // Parcel FDs own references to the allocation, including while in transit.
  // There is no decoder/window producer that can reuse this offscreen image.
  OH_NativeBuffer_Unreference(native);
  if (!exported)
    response->CloseFds();
  return exported;
}
SharedBufferColorState::~SharedBufferColorState() {
  Reset();
}
bool SharedBufferColorState::Prepare(void* target,
                                     uint64_t widget,
                                     const OhosOutputColor& color,
                                     bool idle) {
  auto* window = static_cast<OHNativeWindow*>(target);
  if (!window)
    return false;
  auto found = windows_.find(target);
  if (found == windows_.end()) {
    if (color.mode != OhosOutputMode::kHdr10)
      return true;
    Prune();
    OH_NativeWindow_NativeObjectReference(window);
    found = windows_.try_emplace(target, WindowState{widget, {}}).first;
  }
  const bool prepared = found->second.color.Prepare(color, idle, [window] {
    const int result = OH_NativeWindow_CleanCache(window);
    LOG(WARNING) << "OHOS phone output: retire HDR buffers before SDR, result="
                 << result;
    if (result)
      ready.store(false);
    return result == 0;
  });
  if (prepared && !found->second.color.dirty()) {
    OH_NativeWindow_NativeObjectUnreference(window);
    windows_.erase(found);
  }
  return prepared;
}
void SharedBufferColorState::Prune() {
  for (auto it = windows_.begin(); it != windows_.end();) {
    auto surface = AcquireOhosNativeSurfaceForPhoneGpu(it->second.widget);
    const bool current = surface && surface->window == it->first;
    if (surface && surface->window)
      OH_NativeWindow_NativeObjectUnreference(surface->window);
    if (current) {
      ++it;
    } else {
      // A destroyed/replaced queue will not be reused. Release our reference
      // without treating its disconnected consumer as a global GPU failure.
      OH_NativeWindow_NativeObjectUnreference(it->first);
      it = windows_.erase(it);
    }
  }
}
void SharedBufferColorState::Reset() {
  Prune();
  for (auto& [target, state] : windows_) {
    auto* window = static_cast<OHNativeWindow*>(target);
    const int result = OH_NativeWindow_CleanCache(window);
    LOG(WARNING)
        << "OHOS phone output: retire HDR buffers after GPU generation, result="
        << result;
    if (result)
      ready.store(false);
    OH_NativeWindow_NativeObjectUnreference(window);
  }
  windows_.clear();
}
OH_NativeBuffer* ImportSharedBuffer(const SharedMessage& m) {
  if ((m.packet.operation != SharedOp::kBuffer &&
       m.packet.operation != SharedOp::kImage) ||
      !ValidSharedPacket(m.packet, m.fds.size()))
    return nullptr;
  auto reader = Reader();
  Parcel parcel(OH_IPCParcel_Create(), &OH_IPCParcel_Destroy);
  if (!reader || !parcel ||
      OH_IPCParcel_WriteBuffer(
          parcel.get(),
          reinterpret_cast<const uint8_t*>(m.packet.prefix.data()),
          sizeof(m.packet.prefix)))
    return nullptr;
  for (uint32_t i = 0; i < m.packet.fd_count; ++i)
    if (OH_IPCParcel_WriteFileDescriptor(parcel.get(), m.fds[i]))
      return nullptr;
  if (m.packet.tail_bytes &&
      OH_IPCParcel_WriteBuffer(parcel.get(), m.packet.tail.data(),
                               m.packet.tail_bytes))
    return nullptr;
  OH_NativeBuffer* native = nullptr;
  if (reader(parcel.get(), &native))
    return nullptr;
  return native;
}
SharedBufferLease::SharedBufferLease() = default;
SharedBufferLease::~SharedBufferLease() {
  auto* window = static_cast<OHNativeWindow*>(window_);
  auto* buffer = static_cast<OHNativeWindowBuffer*>(buffer_);
  if (window && buffer && !resolved_) {
    // An unacknowledged GPU may still write. Never Abort/reuse that allocation.
    if (OH_NativeWindow_NativeWindowDetachBuffer(window, buffer)) {
      // Keep these two references until process exit and disable new leases.
      // The channel bounds outstanding leases; this cannot grow on retries.
      ready.store(false);
      return;
    }
  }
  if (buffer)
    OH_NativeWindow_NativeObjectUnreference(buffer);
  if (window)
    OH_NativeWindow_NativeObjectUnreference(window);
}
bool SharedBufferLease::Acquire(void* target, SharedMessage* m) {
  if (!target || window_ || !ready.load()) {
    LOG(ERROR) << "OHOS shared buffer: step=acquire-state target="
               << (target != nullptr)
               << " already_acquired=" << (window_ != nullptr)
               << " allocator_ready=" << ready.load();
    return false;
  }
  auto* window = static_cast<OHNativeWindow*>(target);
  const auto& p = m->packet;
  const uint64_t usage = NATIVEBUFFER_USAGE_MEM_DMA |
                         NATIVEBUFFER_USAGE_HW_RENDER |
                         NATIVEBUFFER_USAGE_HW_TEXTURE;
  if (!BufferStep(p, "set-format",
                  OH_NativeWindow_NativeWindowHandleOpt(
                      window, SET_FORMAT,
                      static_cast<int32_t>(OhosOutputNativeFormat(p.color)))) ||
      !BufferStep(
          p, "set-usage",
          OH_NativeWindow_NativeWindowHandleOpt(window, SET_USAGE, usage)) ||
      !BufferStep(
          p, "set-geometry",
          OH_NativeWindow_NativeWindowHandleOpt(
              window, SET_BUFFER_GEOMETRY, static_cast<int32_t>(p.width),
              static_cast<int32_t>(p.height))) ||
      !BufferStep(p, "set-timeout",
                  OH_NativeWindow_NativeWindowHandleOpt(window, SET_TIMEOUT,
                                                        int32_t{200})))
    return false;
  OHNativeWindowBuffer* buffer = nullptr;
  int fence = -1;
  if (!BufferStep(
          p, "request-buffer",
          OH_NativeWindow_NativeWindowRequestBuffer(window, &buffer, &fence)))
    return false;
  window_ = window;
  buffer_ = buffer;
  OH_NativeWindow_NativeObjectReference(window);
  OH_NativeWindow_NativeObjectReference(buffer);
  OH_NativeBuffer* native = nullptr;
  bool encoded =
      BufferStep(p, "from-window-buffer",
                 OH_NativeBuffer_FromNativeWindowBuffer(buffer, &native));
  if (encoded && !native) {
    LOG(ERROR) << "OHOS shared buffer: step=from-window-buffer null";
    encoded = false;
  }
  if (encoded)
    encoded = Encode(native, m);
  // Per-buffer tags travel with this lease. Never relabel an entire window
  // while its consumer may still hold older SDR/PQ frames.
  if (encoded) {
    encoded = SetOhosBufferOutputColor(native, m->packet.color);
  }
  if (!encoded) {
    if (fence >= 0)
      close(fence);
    m->CloseFds();
    CancelBeforeDrawing();
    // Rejecting a 10-bit allocation must not disable the proven SDR allocator.
    if (m->packet.color.mode == OhosOutputMode::kSdr8)
      ready.store(false);
    return false;
  }
  if (fence >= 0) {
    m->packet.has_fence = 1;
    m->fds.push_back(fence);
  }
  return true;
}
bool SharedBufferLease::Present(int completion_fd) {
  if (!window_ || !buffer_ || resolved_ || completion_fd < -1) {
    if (completion_fd >= 0)
      close(completion_fd);
    return false;
  }
  int result = OH_NativeWindow_NativeWindowFlushBuffer(
      static_cast<OHNativeWindow*>(window_),
      static_cast<OHNativeWindowBuffer*>(buffer_), completion_fd,
      Region{nullptr, 0});
  // Flush consumes the fence, including on error (SDK contract).
  resolved_ = result == 0;
  return resolved_;
}
void SharedBufferLease::CancelBeforeDrawing() {
  if (window_ && buffer_ && !resolved_)
    resolved_ = OH_NativeWindow_NativeWindowAbortBuffer(
                    static_cast<OHNativeWindow*>(window_),
                    static_cast<OHNativeWindowBuffer*>(buffer_)) == 0;
}
}  // namespace ui::phone_gpu
