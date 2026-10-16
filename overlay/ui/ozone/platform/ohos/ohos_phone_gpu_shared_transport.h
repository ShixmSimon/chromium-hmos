// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef UI_OZONE_PLATFORM_OHOS_OHOS_PHONE_GPU_SHARED_TRANSPORT_H_
#define UI_OZONE_PLATFORM_OHOS_OHOS_PHONE_GPU_SHARED_TRANSPORT_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>
#include "ui/ozone/platform/ohos/ohos_output_color.h"

namespace ui::phone_gpu {
inline constexpr uint32_t kSharedMagic = 0x50475335;
inline constexpr size_t kSharedMaxFds = 8;
// NativeBuffer format values, checked against the SDK by the allocator.
inline constexpr uint32_t kSharedImageRgba8888 = 12;
inline constexpr uint32_t kSharedImageBgra8888 = 20;
inline constexpr uint32_t kSharedImageRgba16Float = 39;
inline constexpr bool IsSharedImageFormat(uint32_t format) {
  return format == kSharedImageRgba8888 || format == kSharedImageBgra8888 ||
         format == kSharedImageRgba16Float;
}
inline constexpr uint32_t SharedImageBytesPerPixel(uint32_t format) {
  if (format == kSharedImageRgba16Float)
    return 8;
  return IsSharedImageFormat(format) ? 4 : 0;
}
enum class SharedOp : uint32_t {
  kAcquire = 1,
  kBuffer,
  kPresent,
  kCancel,
  kReply,
  kUnavailable,
  kTiming,
  kDiscard,
  kVulkanState,
  kVulkanFailed,
  kColorProbe,
  kColorFailed,
  kHdrDisplayQuery,
  kHdrDisplayState,
  kColorReady,
  // Offscreen images have no window queue or lease. The allocation request
  // carries its native format in prefix[8]; the reply carries the full parcel.
  kAllocateImage,
  kImage
};
struct SharedPacket {
  uint32_t magic = kSharedMagic;
  SharedOp operation = SharedOp::kAcquire;
  uint64_t sequence = 0;
  uint64_t widget = 0;
  uint64_t lease = 0;
  OhosOutputColor color;
  uint64_t color_generation = 0;
  // Only kHdrDisplayState carries an ID. Zero is a valid primary display ID;
  // result=kPresented means one HDR10-capable display was found by the host.
  uint64_t display_id = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t fd_count = 0;
  uint32_t has_fence = 0;
  uint32_t tail_bytes = 0;
  uint32_t result = 2;
  int64_t timebase_us = 0;
  int64_t interval_us = 0;
  // OpenHarmony SurfaceBuffer parcel v1: sequence, presence, BufferHandle.
  // This is NOT a public ABI. Encode/decode reject unknown layouts.
  std::array<uint32_t, 14> prefix{};
  std::array<uint8_t, 1044> tail{};
};
struct SharedMessage {
  SharedPacket packet;
  std::vector<int> fds;
  SharedMessage() = default;
  ~SharedMessage();
  SharedMessage(const SharedMessage&) = delete;
  SharedMessage& operator=(const SharedMessage&) = delete;
  void CloseFds();
  int TakeFence();
};
bool ValidSharedPacket(const SharedPacket& packet, size_t fd_count);
bool SendSharedMessage(int socket, const SharedMessage& message);
bool ReceiveSharedMessage(int socket, SharedMessage* message, int timeout_ms);
// Peeks only the discriminator; no descriptors are taken from the socket.
bool PeekPhoneGpuMessage(int socket, uint32_t* magic, int timeout_ms);
}  // namespace ui::phone_gpu
#endif
