// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#include "ohos_overlay_transport.h"
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cmath>
#include <cstring>
namespace ui::ohos_overlay {
namespace {
bool RectValid(const OhosOverlayRect& r) {
  return std::isfinite(r.x) && std::isfinite(r.y) && r.x >= 0 && r.y >= 0 &&
         std::isfinite(r.width) && std::isfinite(r.height) && r.width > 0 &&
         r.height > 0 && r.x + r.width <= 16384 && r.y + r.height <= 16384;
}
bool PlaneValid(const WirePlane& p) {
  return p.content && p.reuse <= 1 && p.has_fence <= 1 &&
         !(p.reuse && p.has_fence) && RectValid(p.source) &&
         RectValid(p.destination);
}
}  // namespace
WireMessage::~WireMessage() {
  CloseFds();
}
void WireMessage::CloseFds() {
  for (int fd : fds)
    close(fd);
  fds.clear();
}
namespace {
bool ValidParcel(const WirePlane& plane, uint32_t count, uint32_t tail,
                 const std::array<uint32_t, 14>& v) {
  if (plane.reuse) {
    if (count || tail)
      return false;
    for (uint32_t value : v)
      if (value)
        return false;
    return true;
  }
  constexpr uint64_t limit = 128 * 1024 * 1024;
  return count && count <= 7 && v[1] == 1 && v[2] <= 6 && v[3] <= 256 &&
      v[13] == 1 && count == v[2] + 1 && v[4] && v[6] &&
      v[4] <= 8192 && v[6] <= 8192 && (v[8] == 12 || v[8] == 20) &&
      v[5] >= uint64_t{v[4]} * 4 && v[5] <= limit &&
      v[7] >= uint64_t{v[4]} * v[6] * 4 && v[7] <= limit &&
      (tail == v[3] * 4 || tail == v[3] * 4 + 20) && tail <= 1044 &&
      plane.source.x + plane.source.width <= v[4] &&
      plane.source.y + plane.source.height <= v[6];
}
}  // namespace
bool ValidWirePacket(const WirePacket& p, size_t count) {
  if (p.magic != kWireMagic || p.version != 2 || !p.sequence ||
      count > kWireMaxFds || p.has_video > 1 || p.plane > 1 || p.video_color > 2)
    return false;
  if (!p.video_color) {
    if (p.video_fd_count || p.video_tail_bytes)
      return false;
    for (auto value : p.video_prefix)
      if (value)
        return false;
  }
  if (p.operation != WireOp::kFrame) {
    if (count || p.fd_count || p.tail_bytes || p.video_token || p.has_video ||
        p.video_color || p.primary.content || p.video.content || p.primary.has_fence ||
        p.video.has_fence || p.primary.reuse || p.video.reuse)
      return false;
    switch (p.operation) {
      case WireOp::kReleased:
        return !p.result;
      case WireOp::kRejected:
        return !p.plane && p.result >= 1 && p.result <= 4;
      case WireOp::kAccepted:
      case WireOp::kProcessed:
      case WireOp::kClose:
      case WireOp::kClosed:
        return !p.result && !p.plane;
      default:
        return false;
    }
  }
  if (p.result || p.plane || !PlaneValid(p.primary) ||
      (p.has_video ? !PlaneValid(p.video) ||
                         (p.video_color ? p.video_token != 0 : !p.video_token)
                   : p.video_token || p.video.content || p.video.has_fence ||
                         p.video.reuse || p.video_color) ||
      p.fd_count > 7 || p.video_fd_count > 7 ||
      count != p.fd_count + p.video_fd_count +
                   p.primary.has_fence + p.video.has_fence ||
      !ValidParcel(p.primary, p.fd_count, p.tail_bytes, p.prefix) ||
      (p.video_color && !ValidParcel(p.video, p.video_fd_count,
                                    p.video_tail_bytes, p.video_prefix)))
    return false;
  if (p.has_video) {
    const auto& a = p.primary.destination;
    const auto& b = p.video.destination;
    if (b.x < a.x || b.y < a.y || b.x + b.width > a.x + a.width ||
        b.y + b.height > a.y + a.height)
      return false;
  }
  return true;
}
bool SendWireMessage(int fd, const WireMessage& m) {
  if (!ValidWirePacket(m.packet, m.fds.size())) {
    errno = EINVAL;
    return false;
  }
  iovec io{const_cast<WirePacket*>(&m.packet), sizeof(m.packet)};
  alignas(cmsghdr) char control[CMSG_SPACE(kWireMaxFds * sizeof(int))]{};
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
  ssize_t n;
  do {
    n = sendmsg(fd, &msg, MSG_DONTWAIT | MSG_NOSIGNAL);
  } while (n < 0 && errno == EINTR);
  return n == static_cast<ssize_t>(sizeof(m.packet));
}
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
bool ReceiveWireMessage(int fd, WireMessage* m) {
  m->CloseFds();
  iovec io{&m->packet, sizeof(m->packet)};
  alignas(cmsghdr) char control[CMSG_SPACE(32 * sizeof(int))]{};
  msghdr msg{};
  msg.msg_iov = &io;
  msg.msg_iovlen = 1;
  msg.msg_control = control;
  msg.msg_controllen = sizeof(control);
  int flags = MSG_DONTWAIT;
#ifdef MSG_CMSG_CLOEXEC
  flags |= MSG_CMSG_CLOEXEC;
#endif
  ssize_t n;
  do {
    n = recvmsg(fd, &msg, flags);
  } while (n < 0 && errno == EINTR);
  if (n < 0)
    return false;
  bool bad = false;
  for (auto* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
    if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS ||
        c->cmsg_len < CMSG_LEN(0)) {
      bad = true;
      continue;
    }
    const size_t bytes = c->cmsg_len - CMSG_LEN(0);
    if (bytes % sizeof(int))
      bad = true;
    for (size_t i = 0; i + sizeof(int) <= bytes; i += sizeof(int)) {
      int value;
      memcpy(&value, reinterpret_cast<char*>(CMSG_DATA(c)) + i, sizeof(value));
      m->fds.push_back(value);
      if (fcntl(value, F_SETFD, FD_CLOEXEC) < 0)
        bad = true;
    }
  }
  if (bad || n != static_cast<ssize_t>(sizeof(m->packet)) ||
      (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) ||
      !ValidWirePacket(m->packet, m->fds.size())) {
    m->CloseFds();
    errno = n == 0 ? ECONNRESET : EPROTO;
    return false;
  }
  return true;
}
#pragma GCC diagnostic pop
}  // namespace ui::ohos_overlay
