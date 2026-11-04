// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#ifndef UI_OZONE_PLATFORM_OHOS_OHOS_OVERLAY_HOST_CHANNEL_H_
#define UI_OZONE_PLATFORM_OHOS_OHOS_OVERLAY_HOST_CHANNEL_H_

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <mutex>
#include <utility>

namespace ui::ohos_overlay {
// Connect runs on the browser IPC worker; every other operation is UI-owned.
// A replacement can queue while RS drains, but cannot receive old-generation
// release callbacks: Activate is called only after the old Session is closed.
class HostChannel {
 public:
  ~HostChannel() { Stop(); Disconnect(); }
  int Connect() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_ || (active_ >= 0 && !Lost(active_)) ||
        (pending_ >= 0 && !Lost(pending_)))
      return -1;
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC,
                   0, fds))
      return -1;
    if (pending_ >= 0)
      close(pending_);
    pending_ = fds[0];
    return fds[1];
  }
  bool Activate() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_ || active_ >= 0 || pending_ < 0)
      return false;
    active_ = std::exchange(pending_, -1);
    return true;
  }
  int socket() const { return active_; }  // UI thread only.
  bool PeerLost() const { return active_ >= 0 && Lost(active_); }
  void Disconnect() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_ >= 0)
      close(std::exchange(active_, -1));
  }
  void Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = true;
    if (pending_ >= 0)
      close(std::exchange(pending_, -1));
  }

 private:
  static bool Lost(int socket) {
    pollfd peer{socket, POLLIN, 0};
    return poll(&peer, 1, 0) > 0 &&
           (peer.revents & (POLLHUP | POLLERR | POLLNVAL));
  }
  std::mutex mutex_;
  int active_ = -1;
  int pending_ = -1;
  bool stopped_ = false;
};
}  // namespace ui::ohos_overlay
#endif
