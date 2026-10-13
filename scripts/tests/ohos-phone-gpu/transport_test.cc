// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "ui/ozone/platform/ohos/ohos_phone_gpu_transport.h"

#include <dirent.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

namespace p = ui::phone_gpu;
bool deny_stat = false;
extern "C" int __real_fstat(int, struct stat*);
extern "C" int __wrap_fstat(int fd, struct stat* info) {
  if (deny_stat) {
    errno = EACCES;
    return -1;
  }
  return __real_fstat(fd, info);
}
int FdCount() {
  DIR* dir = opendir("/proc/self/fd");
  assert(dir);
  int count = 0;
  while (readdir(dir))
    ++count;
  closedir(dir);
  return count;
}
void SendRaw(int socket, const p::Frame& frame, int fd, int count) {
  iovec io{const_cast<p::Frame*>(&frame), sizeof(frame)};
  alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * 32)] = {};
  msghdr msg{};
  msg.msg_iov = &io;
  msg.msg_iovlen = 1;
  if (count) {
    msg.msg_control = control;
    msg.msg_controllen = CMSG_SPACE(sizeof(int) * count);
    auto* c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int) * count);
    for (int i = 0; i < count; ++i)
      memcpy(CMSG_DATA(c) + sizeof(int) * i, &fd, sizeof(fd));
  }
  assert(sendmsg(socket, &msg, MSG_NOSIGNAL) == sizeof(frame));
}
int main() {
  int s[2];
  assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, s) == 0);
  p::Frame frame;
  frame.widget = 42;
  frame.sequence = 1;
  frame.width = 65;
  frame.height = 33;
  p::Buffer buffer;
  assert(buffer.Resize(p::FrameBytes(frame), 256));
  assert(buffer.fd() >= 256);
  memset(buffer.data(), 0x5a, p::FrameBytes(frame));
  assert(p::SendFrame(s[0], frame, buffer.fd()));
  p::Frame got;
  int fd = -1;
  assert(p::ReceiveFrame(s[1], &got, &fd, 100));
  std::vector<uint8_t> pixels;
  assert(p::ReadPixels(fd, got, &pixels));
  assert(pixels.size() == p::FrameBytes(frame));
  for (auto byte : pixels)
    assert(byte == 0x5a);
  // HarmonyOS phone getattr denial must not reject a readable memfd.
  deny_stat = true;
  assert(p::ReadPixels(fd, got, &pixels));
  deny_stat = false;
  close(fd);
  p::Reply reply;
  reply.sequence = 1;
  reply.result = p::Result::kPresented;
  assert(p::SendReply(s[1], reply));
  p::Result result;
  assert(p::ReceiveReply(s[0], 1, &result) && result == p::Result::kPresented);

  // Malformed/truncated ancillary messages must not leak even installed FDs.
  const int before = FdCount();
  for (int count : {0, 2, 32}) {
    SendRaw(s[0], frame, buffer.fd(), count);
    assert(!p::ReceiveFrame(s[1], &got, &fd, 100));
    assert(fd == -1 && FdCount() == before);
  }
  auto invalid = frame;
  invalid.width = UINT32_MAX;
  SendRaw(s[0], invalid, buffer.fd(), 1);
  assert(!p::ReceiveFrame(s[1], &got, &fd, 100));
  assert(fd == -1 && FdCount() == before);
  invalid = frame;
  invalid.reserved = 1;
  SendRaw(s[0], invalid, buffer.fd(), 1);
  assert(!p::ReceiveFrame(s[1], &got, &fd, 100));
  assert(fd == -1 && FdCount() == before);
  // The receiver checks file length, and a later truncation fails safely.
  assert(p::SendFrame(s[0], frame, buffer.fd()));
  assert(p::ReceiveFrame(s[1], &got, &fd, 100));
  assert(ftruncate(buffer.fd(), 1) == 0);
  assert(!p::ReadPixels(fd, got, &pixels));
  close(fd);
  SendRaw(s[0], frame, buffer.fd(), 1);
  assert(!p::ReceiveFrame(s[1], &got, &fd, 100));
  assert(fd == -1 && FdCount() == before);

  // A reply from a previous frame must never release the next frame's buffer.
  assert(p::SendReply(s[1], reply));
  assert(!p::ReceiveReply(s[0], 2, &result));
  const auto start = std::chrono::steady_clock::now();
  assert(!p::ReceiveFrame(s[1], &got, &fd, 20));
  assert(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
  close(s[0]);
  assert(!p::ReceiveFrame(s[1], &got, &fd, 100));
  close(s[1]);
  std::cout << "PASS: RGBA transfer, FD ownership/truncation, bounds, short "
               "file, stale reply, timeout, EOF\n";
}
