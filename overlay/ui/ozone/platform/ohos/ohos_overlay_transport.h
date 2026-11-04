// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#ifndef UI_OZONE_PLATFORM_OHOS_OHOS_OVERLAY_TRANSPORT_H_
#define UI_OZONE_PLATFORM_OHOS_OHOS_OVERLAY_TRANSPORT_H_
#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>
#include "ohos_overlay_bridge.h"

namespace ui::ohos_overlay {
// Dedicated SOCK_SEQPACKET channel. No pointers or pixels cross this channel.
// The browser resolves video_token to the ORIGINAL decoder read lease, so HDR
// metadata is not reconstructed from stream/container metadata in the GPU.
inline constexpr uint32_t kWireMagic = 0x4c4f5631;
inline constexpr size_t kWireMaxFds =
    16;  // Two parcels of up to 7 handles, then 2 acquire fences.
enum class WireOp : uint32_t {
  kFrame = 1,
  kAccepted,
  kReleased,
  kProcessed,
  kRejected,
  kClose,
  kClosed
};
struct WirePlane {
  uint64_t content =
      0;  // Token of immutable content; changes on producer write.
  uint32_t reuse = 0;
  uint32_t has_fence = 0;
  OhosOverlayRect source{};
  OhosOverlayRect destination{};
};
struct WirePacket {
  uint32_t magic = kWireMagic;
  uint32_t version = 2;
  WireOp operation = WireOp::kFrame;
  uint32_t result = 0;
  uint64_t sequence = 0;
  uint64_t video_token = 0;
  uint32_t plane = 0;  // Only kReleased: 0 primary, 1 video.
  uint32_t has_video = 0;
  uint32_t fd_count =
      0;  // Native parcel FDs, followed by primary/video fences.
  uint32_t tail_bytes = 0;
  WirePlane primary, video;
  std::array<uint32_t, 14> prefix{};
  std::array<uint8_t, 1044> tail{};
  // Software SDR video carries its own GPU-written buffer, rather than a
  // decoder token. 0 = decoder; 1 = RGB sRGB; 2 = RGB BT.709 full range.
  uint32_t video_color = 0;
  uint32_t video_fd_count = 0;
  uint32_t video_tail_bytes = 0;
  std::array<uint32_t, 14> video_prefix{};
  std::array<uint8_t, 1044> video_tail{};
};
static_assert(sizeof(WirePlane) == 48 && sizeof(WirePacket) == 2360);
static_assert(offsetof(WirePacket, prefix) == 144 &&
              offsetof(WirePacket, tail) == 200);
static_assert(std::is_trivially_copyable_v<WirePacket>);

struct WireMessage {
  WirePacket packet;
  std::vector<int> fds;
  WireMessage() = default;
  ~WireMessage();
  WireMessage(const WireMessage&) = delete;
  WireMessage& operator=(const WireMessage&) = delete;
  void CloseFds();
};
bool ValidWirePacket(const WirePacket&, size_t fd_count);
// Nonblocking. EAGAIN leaves the message owned by the caller, for retry.
// Receive closes all received descriptors on malformed/truncated packets.
bool SendWireMessage(int socket, const WireMessage&);
bool ReceiveWireMessage(int socket, WireMessage*);
}  // namespace ui::ohos_overlay
#endif
