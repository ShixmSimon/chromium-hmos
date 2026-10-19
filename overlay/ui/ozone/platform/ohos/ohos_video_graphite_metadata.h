// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef UI_OZONE_PLATFORM_OHOS_OHOS_VIDEO_GRAPHITE_METADATA_H_
#define UI_OZONE_PLATFORM_OHOS_OHOS_VIDEO_GRAPHITE_METADATA_H_

#include <array>
#include <chrono>
#include <cstdint>
#include <cerrno>
#include <poll.h>
#include <sys/socket.h>

namespace ui {
// Versioned, fixed-size metadata only. Pixels and process-local pointers never
// travel over this socket. The existing endpoint still owns the decoder lease
// until EOF; receiving metadata does not release the queue slot.
struct OhosVideoGraphiteMetadata {
  std::array<uint32_t, 12> words{};
  static constexpr uint32_t kMagic = 0x47595631;  // GYV1
  bool IsValid(uint32_t width, uint32_t height) const {
    return words[0] == kMagic && words[1] == 1 && words[2] == width &&
           words[3] == height && width && height &&
           (words[4] || words[5] || words[6]) &&
           words[7] >= 2 && words[7] <= 4 && words[8] <= 1 &&
           words[9] <= 1 && words[10] <= 1 && (words[11] & 1);
  }
};

inline bool SendOhosVideoGraphiteMetadata(
    int fd, const OhosVideoGraphiteMetadata& metadata) {
  // A fixed 48-byte record fits in an empty per-frame socket. Never block the
  // GPU sequence when a producer has disappeared or stopped consuming replies.
  ssize_t count;
  do {
    count = send(fd, metadata.words.data(), sizeof(metadata.words),
                 MSG_NOSIGNAL | MSG_DONTWAIT);
  } while (count < 0 && errno == EINTR);
  return count == static_cast<ssize_t>(sizeof(metadata.words));
}

inline bool ReceiveOhosVideoGraphiteMetadata(
    int fd, OhosVideoGraphiteMetadata* metadata) {
  // Runs on a MayBlock worker, with one deadline for the complete record.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  size_t received = 0;
  auto* bytes = reinterpret_cast<unsigned char*>(metadata->words.data());
  while (received < sizeof(metadata->words)) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now()).count();
    if (remaining <= 0)
      return false;
    pollfd poll_fd{fd, POLLIN, 0};
    const int ready = poll(&poll_fd, 1, static_cast<int>(remaining));
    if (ready < 0 && errno == EINTR)
      continue;
    if (ready <= 0 || !(poll_fd.revents & POLLIN) ||
        (poll_fd.revents & (POLLERR | POLLNVAL)))
      return false;
    const ssize_t count = recv(fd, bytes + received,
                               sizeof(metadata->words) - received, MSG_DONTWAIT);
    if (count < 0 && (errno == EINTR || errno == EAGAIN))
      continue;
    if (count <= 0)
      return false;
    received += static_cast<size_t>(count);
  }
  return true;
}
}  // namespace ui
#endif
