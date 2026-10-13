// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/ozone/platform/ohos/ohos_phone_gpu_transport.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <chrono>
#include <cstring>

namespace ui::phone_gpu {
namespace {
bool Wait(int fd, short events, int timeout_ms) {
  const auto end =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  for (;;) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                          end - std::chrono::steady_clock::now())
                          .count();
    pollfd item{fd, events, 0};
    int result = poll(&item, 1, left > 0 ? static_cast<int>(left) : 0);
    if (result > 0)
      return (item.revents & events) != 0;
    if (result == 0) {
      errno = ETIMEDOUT;
      return false;
    }
    if (errno != EINTR)
      return false;
  }
}

bool Send(int socket, const void* data, size_t size, int fd) {
  if (!Wait(socket, POLLOUT, kFrameTimeoutMs))
    return false;
  iovec io{const_cast<void*>(data), size};
  alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] = {};
  msghdr message{};
  message.msg_iov = &io;
  message.msg_iovlen = 1;
  if (fd >= 0) {
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    auto* c = CMSG_FIRSTHDR(&message);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof(fd));
  }
  return sendmsg(socket, &message, MSG_NOSIGNAL | MSG_DONTWAIT) ==
         static_cast<ssize_t>(size);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
bool Receive(int socket, void* data, size_t size, int* fd, int timeout_ms) {
  if (fd)
    *fd = -1;
  if (!Wait(socket, POLLIN, timeout_ms))
    return false;
  iovec io{data, size};
  // Receive and close unexpected descriptors too, including truncated input.
  alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * 16)] = {};
  msghdr message{};
  message.msg_iov = &io;
  message.msg_iovlen = 1;
  message.msg_control = control;
  message.msg_controllen = sizeof(control);
  const ssize_t received =
      recvmsg(socket, &message, MSG_CMSG_CLOEXEC | MSG_DONTWAIT);
  int first = -1;
  size_t count = 0;
  for (auto* c = CMSG_FIRSTHDR(&message); c; c = CMSG_NXTHDR(&message, c)) {
    if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS ||
        c->cmsg_len < CMSG_LEN(0))
      continue;
    const size_t bytes = c->cmsg_len - CMSG_LEN(0);
    for (size_t offset = 0; offset + sizeof(int) <= bytes;
         offset += sizeof(int)) {
      int current;
      memcpy(&current, reinterpret_cast<char*>(CMSG_DATA(c)) + offset,
             sizeof(current));
      if (++count == 1)
        first = current;
      else
        close(current);
    }
  }
  if (received != static_cast<ssize_t>(size) ||
      (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) ||
      count != (fd ? 1u : 0u)) {
    if (first >= 0)
      close(first);
    errno = received != static_cast<ssize_t>(size)           ? EMSGSIZE
            : (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) ? EOVERFLOW
                                                             : ENOMSG;
    return false;
  }
  if (fd)
    *fd = first;
  return true;
}
#pragma GCC diagnostic pop
}  // namespace

size_t FrameBytes(const Frame& frame) {
  if (frame.magic != Frame{}.magic || frame.reserved || !frame.widget ||
      !frame.sequence || !frame.width || !frame.height || frame.width > 8192 ||
      frame.height > 8192)
    return 0;
  const uint64_t bytes = uint64_t{frame.width} * frame.height * 4;
  return bytes <= kMaxFrameBytes ? static_cast<size_t>(bytes) : 0;
}

bool SendFrame(int socket, const Frame& frame, int buffer_fd) {
  return FrameBytes(frame) && buffer_fd >= 0 &&
         Send(socket, &frame, sizeof(frame), buffer_fd);
}
bool ReceiveFrame(int socket, Frame* frame, int* buffer_fd, int timeout_ms) {
  if (!Receive(socket, frame, sizeof(*frame), buffer_fd, timeout_ms))
    return false;
  const size_t bytes = FrameBytes(*frame);
  if (!bytes || !ValidateBuffer(*buffer_fd, bytes)) {
    int error = !bytes ? EINVAL : errno;
    close(*buffer_fd);
    *buffer_fd = -1;
    errno = error;
    return false;
  }
  return true;
}
bool SendReply(int socket, const Reply& reply) {
  return Send(socket, &reply, sizeof(reply), -1);
}
bool ReceiveReply(int socket, uint64_t sequence, Result* result) {
  Reply reply;
  if (!Receive(socket, &reply, sizeof(reply), nullptr, kFrameTimeoutMs) ||
      reply.magic != Reply{}.magic || reply.sequence != sequence ||
      static_cast<uint32_t>(reply.result) >
          static_cast<uint32_t>(Result::kFailed))
    return false;
  *result = reply.result;
  return true;
}
bool ValidateBuffer(int fd, size_t bytes) {
  struct stat info {};
  if (!bytes || bytes > kMaxFrameBytes) {
    errno = EINVAL;
    return false;
  }
  if (fstat(fd, &info) == 0) {
    if (!S_ISREG(info.st_mode)) {
      errno = ENOTSUP;
      return false;
    }
    if (info.st_size != static_cast<off_t>(bytes)) {
      errno = EFBIG;
      return false;
    }
  } else {
    // Mate 70 / API 26 allows pread of the transferred memfd but denies
    // getattr (fstat: EACCES). A seekable file still exposes its exact length.
    // Pipes/sockets cannot pass this check. ReadPixels uses pread, not mmap,
    // so a concurrent truncate is a short read rather than browser SIGBUS.
    if (errno != EACCES || lseek(fd, 0, SEEK_END) != static_cast<off_t>(bytes))
      return false;
  }
  return true;
}
bool ReadPixels(int fd, const Frame& frame, std::vector<uint8_t>* pixels) {
  const size_t bytes = FrameBytes(frame);
  if (!ValidateBuffer(fd, bytes))
    return false;
  pixels->resize(bytes);
  size_t offset = 0;
  while (offset < bytes) {
    ssize_t count = pread(fd, pixels->data() + offset, bytes - offset, offset);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return false;
    offset += count;
  }
  return true;
}

Buffer::~Buffer() {
  Reset();
}
void Buffer::Reset() {
  if (data_)
    munmap(data_, bytes_);
  if (fd_ >= 0)
    close(fd_);
  data_ = nullptr;
  fd_ = -1;
  bytes_ = 0;
}
bool Buffer::Resize(size_t bytes, int minimum_fd) {
  if (!bytes || bytes > kMaxFrameBytes || minimum_fd < 3)
    return false;
  if (bytes == bytes_)
    return true;
  Reset();
  int raw = syscall(SYS_memfd_create, "ohos-phone-gpu", MFD_CLOEXEC);
  if (raw < 0)
    return false;
  fd_ = fcntl(raw, F_DUPFD_CLOEXEC, minimum_fd);
  close(raw);
  if (fd_ < 0 || ftruncate(fd_, bytes) != 0) {
    Reset();
    return false;
  }
  void* mapping =
      mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
  if (mapping == MAP_FAILED) {
    Reset();
    return false;
  }
  data_ = mapping;
  bytes_ = bytes;
  return true;
}
}  // namespace ui::phone_gpu
