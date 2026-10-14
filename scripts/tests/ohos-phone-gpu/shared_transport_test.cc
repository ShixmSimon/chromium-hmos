// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include "ui/ozone/platform/ohos/ohos_phone_gpu_shared_transport.h"
namespace p = ui::phone_gpu;
int CountFds() {
  DIR* dir = opendir("/proc/self/fd");
  assert(dir);
  int count = 0;
  while (readdir(dir))
    ++count;
  closedir(dir);
  return count;
}
void RawSend(int socket,
             const p::SharedPacket& packet,
             int fd,
             int n,
             size_t bytes = sizeof(p::SharedPacket)) {
  iovec io{const_cast<p::SharedPacket*>(&packet), bytes};
  alignas(cmsghdr) char control[CMSG_SPACE(32 * sizeof(int))]{};
  msghdr msg{};
  msg.msg_iov = &io;
  msg.msg_iovlen = 1;
  if (n) {
    msg.msg_control = control;
    msg.msg_controllen = CMSG_SPACE(n * sizeof(int));
    auto* c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(n * sizeof(int));
    for (int i = 0; i < n; ++i)
      memcpy(reinterpret_cast<char*>(CMSG_DATA(c)) + i * sizeof(int), &fd,
             sizeof(fd));
  }
  assert(sendmsg(socket, &msg, MSG_NOSIGNAL) == static_cast<ssize_t>(bytes));
}
int main() {
  int s[2];
  assert(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, s));
  int source = open("/dev/null", O_RDONLY | O_CLOEXEC);
  assert(source >= 0);
  p::SharedPacket buffer;
  buffer.operation = p::SharedOp::kBuffer;
  buffer.sequence = 1;
  buffer.widget = 42;
  buffer.lease = 1;
  buffer.width = 65;
  buffer.height = 33;
  buffer.fd_count = 1;
  buffer.prefix = {7, 1, 0, 0, 65, 320, 33, 320 * 33, 12, 0, 0, 0, 0, 1};
  assert(p::ValidSharedPacket(buffer, 1));
  const int before = CountFds();
  for (int i = 0; i < 100; ++i) {
    RawSend(s[0], buffer, source, 1);
    uint32_t magic;
    assert(p::PeekPhoneGpuMessage(s[1], &magic, 100) &&
           magic == p::kSharedMagic);
    assert(CountFds() == before);  // Peeking must not install SCM_RIGHTS FDs.
    p::SharedMessage got;
    assert(p::ReceiveSharedMessage(s[1], &got, 100));
    assert(got.fds.size() == 1 && (fcntl(got.fds[0], F_GETFD) & FD_CLOEXEC));
  }
  assert(CountFds() == before);
  for (int n : {0, 2, 32}) {
    RawSend(s[0], buffer, source, n);
    p::SharedMessage got;
    assert(!p::ReceiveSharedMessage(s[1], &got, 100));
    assert(got.fds.empty() && CountFds() == before);
  }
  auto reject = [&](p::SharedPacket bad) {
    RawSend(s[0], bad, source, 1);
    p::SharedMessage got;
    assert(!p::ReceiveSharedMessage(s[1], &got, 100));
    assert(got.fds.empty() && CountFds() == before);
  };
  auto bad = buffer;
  bad.magic = 0;
  reject(bad);
  bad = buffer;
  bad.operation = static_cast<p::SharedOp>(99);
  reject(bad);
  bad = buffer;
  bad.fd_count = UINT32_MAX;
  bad.has_fence = 1;
  reject(bad);
  bad = buffer;
  bad.width = UINT32_MAX;
  reject(bad);
  bad = buffer;
  bad.prefix[5] = 1;
  reject(bad);  // Invalid row stride.
  bad = buffer;
  bad.prefix[7] = 1;
  reject(bad);  // Undersized allocation.
  bad = buffer;
  bad.prefix[7] = UINT32_MAX;
  reject(bad);
  bad = buffer;
  bad.prefix[2] = 8;
  reject(bad);
  bad = buffer;
  bad.prefix[3] = UINT32_MAX;
  reject(bad);
  bad = buffer;
  bad.prefix[13] = 0;
  reject(bad);  // Unknown parcel schema.
  bad = buffer;
  bad.tail_bytes = 21;
  reject(bad);
  bad = buffer;
  bad.tail_bytes = 20;
  assert(p::ValidSharedPacket(bad, 1));
  {
    RawSend(s[0], buffer, source, 1, 8);
    p::SharedMessage got;
    assert(!p::ReceiveSharedMessage(s[1], &got, 100));
    assert(CountFds() == before);
  }
  {
    p::SharedMessage send, got;
    send.packet = buffer;
    send.packet.has_fence = 1;
    send.fds = {dup(source), dup(source)};
    assert(p::SendSharedMessage(s[0], send));
    assert(p::ReceiveSharedMessage(s[1], &got, 100));
    int fence = got.TakeFence();
    assert(fence >= 0 && got.TakeFence() == -1 && got.fds.size() == 1);
    close(fence);
  }
  assert(CountFds() == before);
  for (auto op :
       {p::SharedOp::kCancel, p::SharedOp::kDiscard, p::SharedOp::kReply,
        p::SharedOp::kVulkanState, p::SharedOp::kVulkanFailed}) {
    p::SharedMessage send, got;
    send.packet.operation = op;
    send.packet.sequence = 2;
    send.packet.widget = 42;
    send.packet.lease = 1;
    assert(p::SendSharedMessage(s[0], send));
    assert(p::ReceiveSharedMessage(s[1], &got, 100));
    assert(got.packet.operation == op);
  }
  // A 10-bit declaration must agree with the actual parcel and generation.
  auto ten = buffer;
  ten.color.mode = ui::OhosOutputMode::kSdr10;
  ten.color_generation = 17;
  assert(!p::ValidSharedPacket(ten, 1));  // RGBA8 parcel, not a 10-bit image.
  ten.prefix[8] = 34;
  assert(p::ValidSharedPacket(ten, 1));
  ten.color = {ui::OhosOutputMode::kHdr10, 203000, 1000000};
  {
    RawSend(s[0], ten, source, 1);
    p::SharedMessage got;
    assert(p::ReceiveSharedMessage(s[1], &got, 100));
    assert(got.packet.color == ten.color && got.packet.color_generation == 17);
  }
  auto malformed = ten;
  malformed.color_generation = 0;
  reject(malformed);
  malformed = ten;
  malformed.magic = 0x50475332;  // Old packet version is never reinterpreted.
  reject(malformed);
  malformed = ten;
  malformed.color.mode = static_cast<ui::OhosOutputMode>(99);
  reject(malformed);
  for (const ui::OhosOutputColor color :
       {ui::OhosOutputColor{ui::OhosOutputMode::kSdr8, 203000, 1000000},
        ui::OhosOutputColor{ui::OhosOutputMode::kSdr10, 0, 1000000},
        ui::OhosOutputColor{ui::OhosOutputMode::kHdr10, 0, 1000000},
        ui::OhosOutputColor{ui::OhosOutputMode::kHdr10, 203000, 202999},
        ui::OhosOutputColor{ui::OhosOutputMode::kHdr10, 203000, UINT32_MAX}}) {
    malformed = ten;
    malformed.color = color;
    reject(malformed);
  }
  assert(CountFds() == before);
  for (auto op : {p::SharedOp::kColorProbe, p::SharedOp::kColorFailed}) {
    p::SharedMessage send, got;
    send.packet.operation = op;
    send.packet.sequence = 4;
    send.packet.widget = 42;
    send.packet.color_generation = 1;
    assert(p::SendSharedMessage(s[0], send));
    assert(p::ReceiveSharedMessage(s[1], &got, 100));
    send.packet.has_fence = 1;
    assert(!p::ValidSharedPacket(send.packet, 1));
  }
  // Vulkan SYNC_FD export supports both pending and already-signaled fences.
  for (bool pending : {false, true}) {
    p::SharedMessage send, got;
    send.packet.operation = p::SharedOp::kPresent;
    send.packet.sequence = 3;
    send.packet.widget = 42;
    send.packet.lease = 1;
    send.packet.has_fence = pending;
    if (pending)
      send.fds.push_back(dup(source));
    assert(p::SendSharedMessage(s[0], send));
    assert(p::ReceiveSharedMessage(s[1], &got, 100));
    int fence = got.TakeFence();
    assert((fence >= 0) == pending);
    if (fence >= 0)
      close(fence);
    send.packet.has_fence = !pending;
    assert(!p::ValidSharedPacket(send.packet, send.fds.size()));
  }
  // Control messages may never smuggle an allocation or fence descriptor.
  for (auto op : {p::SharedOp::kVulkanState, p::SharedOp::kVulkanFailed}) {
    auto control = buffer;
    control.operation = op;
    reject(control);
  }
  assert(CountFds() == before);
  p::SharedMessage got;
  auto start = std::chrono::steady_clock::now();
  assert(!p::ReceiveSharedMessage(s[1], &got, 20));
  assert(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
  close(s[0]);
  uint32_t magic;
  errno = ETIMEDOUT;
  assert(!p::PeekPhoneGpuMessage(s[1], &magic, 20));
  assert(errno == ECONNRESET);
  assert(!p::ReceiveSharedMessage(s[1], &got, 20));
  close(s[1]);
  close(source);
  std::cout << "Shared NativeBuffer transport: schema, fences, FD leaks and "
               "disconnect passed\n";
}
