// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "ui/ozone/platform/ohos/ohos_phone_gpu_shared_transport.h"

#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <cstring>
#include "ui/ozone/platform/ohos/ohos_phone_gpu_transport.h"

namespace ui::phone_gpu {
namespace {
bool WaitShared(int fd, short events, int timeout_ms) {
  auto end =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  for (;;) {
    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                    end - std::chrono::steady_clock::now())
                    .count();
    pollfd item{fd, events, 0};
    int result = poll(&item, 1, left > 0 ? static_cast<int>(left) : 0);
    if (result > 0) {
      if (item.revents & events)
        return true;
      errno = item.revents & POLLNVAL ? EBADF : ECONNRESET;
      return false;
    }
    if (!result) {
      errno = ETIMEDOUT;
      return false;
    }
    if (errno != EINTR)
      return false;
  }
}
}  // namespace
SharedMessage::~SharedMessage() {
  CloseFds();
}
void SharedMessage::CloseFds() {
  for (int fd : fds)
    close(fd);
  fds.clear();
}
int SharedMessage::TakeFence() {
  if (!packet.has_fence || fds.empty())
    return -1;
  int fd = fds.back();
  fds.pop_back();
  packet.has_fence = 0;
  return fd;
}
bool ValidSharedPacket(const SharedPacket& p, size_t count) {
  if (p.magic != kSharedMagic || !ValidOhosOutputColor(p.color) ||
      (p.color.mode != OhosOutputMode::kSdr8 && !p.color_generation) ||
      !p.sequence || !p.widget || p.has_fence > 1 ||
      p.fd_count > kSharedMaxFds || count > kSharedMaxFds ||
      p.fd_count + p.has_fence != count || p.tail_bytes > p.tail.size())
    return false;
  if (p.operation != SharedOp::kHdrDisplayState && p.display_id)
    return false;
  switch (p.operation) {
    case SharedOp::kAllocateImage:
      if (count || p.lease || p.tail_bytes || !p.width || !p.height ||
          p.width > 8192 || p.height > 8192 ||
          uint64_t{p.width} * p.height * SharedImageBytesPerPixel(p.prefix[8]) >
              kMaxFrameBytes ||
          p.color.mode != OhosOutputMode::kSdr8 || p.color_generation ||
          !IsSharedImageFormat(p.prefix[8]))
        return false;
      for (size_t i = 0; i < p.prefix.size(); ++i) {
        if (i != 8 && p.prefix[i])
          return false;
      }
      return true;
    case SharedOp::kHdrDisplayQuery:
    case SharedOp::kHdrDisplayState:
      return !count && !p.lease && !p.tail_bytes && !p.width && !p.height &&
             p.color.mode == OhosOutputMode::kSdr8 && !p.color_generation &&
             p.result <= 2;
    case SharedOp::kAcquire:
      return !count && !p.lease && !p.tail_bytes && p.width && p.height &&
             p.width <= 8192 && p.height <= 8192 &&
             uint64_t{p.width} * p.height * 4 <= kMaxFrameBytes;
    case SharedOp::kImage:
    case SharedOp::kBuffer: {
      const auto& v = p.prefix;
      const bool image = p.operation == SharedOp::kImage;
      if (image && (p.lease || p.has_fence ||
                    p.color.mode != OhosOutputMode::kSdr8 ||
                    p.color_generation || !IsSharedImageFormat(v[8])))
        return false;
      const uint32_t bytes_per_pixel =
          image ? SharedImageBytesPerPixel(v[8]) : 4;
      // These HW_RENDER buffers are imported as optimal-tiled Vulkan images,
      // never mapped as stride * height bytes. Native allocation size need not
      // include that hypothetical linear padding (e.g. 1617x175 on Maleoon).
      // Keep the conservative RGBA pixel-size floor and allocation cap; the
      // native Vulkan properties provide the actual import allocation size.
      return (image || p.lease) && p.fd_count && v[1] == 1 &&
             v[2] < kSharedMaxFds - 1 &&
             p.fd_count == v[2] + 1 && v[3] <= 256 && v[4] == p.width &&
             v[6] == p.height &&
             (image || v[8] == OhosOutputNativeFormat(p.color)) &&
             v[13] == 1 && p.width && p.height && p.width <= 8192 &&
             p.height <= 8192 && v[5] >= uint64_t{p.width} * bytes_per_pixel &&
             v[5] <= kMaxFrameBytes && v[7] <= kMaxFrameBytes &&
             uint64_t{p.width} * p.height * bytes_per_pixel <= v[7] &&
             (p.tail_bytes == v[3] * 4 || p.tail_bytes == v[3] * 4 + 20);
    }
    case SharedOp::kColorProbe:
      return !count && !p.lease && !p.tail_bytes && p.color_generation;
    case SharedOp::kColorReady:
      return !count && !p.lease && !p.tail_bytes && !p.width && !p.height &&
             p.color_generation && p.color.mode == OhosOutputMode::kHdr10;
    case SharedOp::kPresent:
      return p.lease && !p.fd_count && !p.tail_bytes;
    case SharedOp::kDiscard:
    case SharedOp::kCancel:
      return p.lease && !count && !p.tail_bytes;
    case SharedOp::kReply:
    case SharedOp::kUnavailable:
    case SharedOp::kVulkanState:
    case SharedOp::kVulkanFailed:
    case SharedOp::kColorFailed:
    case SharedOp::kTiming:
      return !count && !p.tail_bytes && p.result <= 2;
  }
  return false;
}
bool SendSharedMessage(int socket, const SharedMessage& m) {
  if (!ValidSharedPacket(m.packet, m.fds.size())) {
    errno = EINVAL;
    return false;
  }
  if (!WaitShared(socket, POLLOUT, kFrameTimeoutMs))
    return false;
  iovec io{const_cast<SharedPacket*>(&m.packet), sizeof(m.packet)};
  alignas(cmsghdr) char control[CMSG_SPACE(kSharedMaxFds * sizeof(int))]{};
  msghdr msg{};
  msg.msg_iov = &io;
  msg.msg_iovlen = 1;
  if (!m.fds.empty()) {
    msg.msg_control = control;
    msg.msg_controllen = CMSG_SPACE(m.fds.size() * sizeof(int));
    auto* c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(m.fds.size() * sizeof(int));
    memcpy(CMSG_DATA(c), m.fds.data(), m.fds.size() * sizeof(int));
  }
  return sendmsg(socket, &msg, MSG_NOSIGNAL | MSG_DONTWAIT) == sizeof(m.packet);
}
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
bool ReceiveSharedMessage(int socket, SharedMessage* m, int timeout_ms) {
  m->CloseFds();
  if (!WaitShared(socket, POLLIN, timeout_ms))
    return false;
  iovec io{&m->packet, sizeof(m->packet)};
  alignas(cmsghdr) char control[CMSG_SPACE(16 * sizeof(int))]{};
  msghdr msg{};
  msg.msg_iov = &io;
  msg.msg_iovlen = 1;
  msg.msg_control = control;
  msg.msg_controllen = sizeof(control);
  ssize_t count = recvmsg(socket, &msg, MSG_CMSG_CLOEXEC | MSG_DONTWAIT);
  for (auto* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
    if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS ||
        c->cmsg_len < CMSG_LEN(0))
      continue;
    const size_t bytes = c->cmsg_len - CMSG_LEN(0);
    for (size_t offset = 0; offset + sizeof(int) <= bytes;
         offset += sizeof(int)) {
      int fd;
      memcpy(&fd, reinterpret_cast<char*>(CMSG_DATA(c)) + offset, sizeof(fd));
      m->fds.push_back(fd);
    }
  }
  if (count != sizeof(m->packet) || msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC) ||
      !ValidSharedPacket(m->packet, m->fds.size())) {
    m->CloseFds();
    errno = EPROTO;
    return false;
  }
  return true;
}
#pragma GCC diagnostic pop
bool PeekPhoneGpuMessage(int socket, uint32_t* magic, int timeout_ms) {
  if (!WaitShared(socket, POLLIN, timeout_ms))
    return false;
  const ssize_t received =
      recv(socket, magic, sizeof(*magic), MSG_PEEK | MSG_DONTWAIT);
  if (received == static_cast<ssize_t>(sizeof(*magic)))
    return true;
  if (received >= 0)
    errno = received == 0 ? ECONNRESET : EPROTO;
  return false;
}
}  // namespace ui::phone_gpu
