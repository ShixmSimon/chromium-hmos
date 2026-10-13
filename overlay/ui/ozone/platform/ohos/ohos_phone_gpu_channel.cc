// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "ui/ozone/platform/ohos/ohos_phone_gpu_channel.h"

#include <errno.h>
#include <native_window/external_window.h>
#include <sys/socket.h>

#include <atomic>
#include <memory>

#include "base/files/scoped_file.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/process/fork_server_ohos.h"
#include "base/synchronization/lock.h"
#include "base/task/thread_pool.h"
#include "ui/ozone/platform/ohos/ohos_native_window_registry.h"
#include "ui/ozone/platform/ohos/ohos_phone_gpu_presenter.h"

namespace ui {
namespace {
struct Connection {
  explicit Connection(base::ScopedFD fd) : socket(std::move(fd)) {}
  base::ScopedFD socket;
  std::atomic<bool> active{true};
  std::atomic<bool> presented{false};
};
struct BrowserState {
  // Serializes presentation with replacing a GPU generation. Never held by
  // the UI thread; registry changes only use the registry's own lock.
  base::Lock lock;
  std::shared_ptr<Connection> current GUARDED_BY(lock);
};
BrowserState& Browser() {
  static base::NoDestructor<BrowserState> state;
  return *state;
}
struct ChildState {
  base::Lock lock;
  base::ScopedFD socket;
  bool initialized = false;
  uint64_t sequence GUARDED_BY(lock) = 0;
};
ChildState& Child() {
  static base::NoDestructor<ChildState> state;
  return *state;
}

void ReceiveFrames(std::shared_ptr<Connection> connection) {
  uint64_t last_sequence = 0;
  std::vector<uint8_t> pixels;
  while (connection->active.load()) {
    phone_gpu::Frame frame;
    int received = -1;
    if (!phone_gpu::ReceiveFrame(connection->socket.get(), &frame, &received,
                                 200)) {
      if (errno == ETIMEDOUT)
        continue;
      break;
    }
    base::ScopedFD buffer_fd(received);
    if (frame.sequence <= last_sequence)
      break;
    last_sequence = frame.sequence;
    phone_gpu::Reply reply;
    reply.sequence = frame.sequence;
    reply.result = phone_gpu::Result::kDropped;
    {
      base::AutoLock lock(Browser().lock);
      if (Browser().current != connection || !connection->active.load())
        break;
      // Acquired under the registry lock, so XComponent destruction cannot
      // free the window between the lookup and taking our native reference.
      auto surface = AcquireOhosNativeSurfaceForPhoneGpu(frame.widget);
      if (surface && surface->window) {
        // A frame queued before resize must not reset the new window's size.
        if (surface->bounds.width() == static_cast<int>(frame.width) &&
            surface->bounds.height() == static_cast<int>(frame.height)) {
          reply.result = phone_gpu::Result::kFailed;
          if (phone_gpu::ReadPixels(received, frame, &pixels) &&
              phone_gpu::Present(surface->window, frame, pixels.data())) {
            reply.result = phone_gpu::Result::kPresented;
          }
        }
        OH_NativeWindow_NativeObjectUnreference(surface->window);
      }
    }
    if (reply.result == phone_gpu::Result::kPresented &&
        !connection->presented.exchange(true)) {
      LOG(WARNING) << "OHOS phone GPU: first frame presented widget="
                   << frame.widget << " size=" << frame.width << "x"
                   << frame.height;
    }
    if (!phone_gpu::SendReply(connection->socket.get(), reply))
      break;
  }
  connection->active = false;
  shutdown(connection->socket.get(), SHUT_RDWR);
  LOG(WARNING) << "OHOS phone GPU: frame channel closed";
}
}  // namespace

base::ProcessId LaunchOhosPhoneGpuChild(
    const std::string& params,
    const std::vector<std::pair<int, int>>& fds) {
  if (fds.size() >= 16)
    return base::kNullProcessId;
  for (const auto& mapping : fds) {
    if (mapping.second == phone_gpu::kChannelDescriptor)
      return base::kNullProcessId;
  }
  int pair[2];
  if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair))
    return base::kNullProcessId;
  base::ScopedFD browser_fd(pair[0]), child_fd(pair[1]);
  auto mappings = fds;
  mappings.emplace_back(child_fd.get(), phone_gpu::kChannelDescriptor);
  const auto pid = base::internal::LaunchOhosForkChild(params, mappings);
  if (pid <= 0)
    return base::kNullProcessId;
  auto connection = std::make_shared<Connection>(std::move(browser_fd));
  {
    base::AutoLock lock(Browser().lock);
    if (Browser().current) {
      Browser().current->active = false;
      shutdown(Browser().current->socket.get(), SHUT_RDWR);
    }
    Browser().current = connection;
  }
  base::ThreadPool::PostTask(FROM_HERE, {base::MayBlock()},
                             base::BindOnce(&ReceiveFrames, connection));
  LOG(WARNING) << "OHOS phone GPU: fork child pid=" << pid
               << " using GL readback presentation";
  return pid;
}
bool OhosPhoneGpuHasPresented() {
  base::AutoLock lock(Browser().lock);
  return Browser().current && Browser().current->active.load() &&
         Browser().current->presented.load();
}
bool InitializeOhosPhoneGpuChild(int received_fd) {
  if (Child().initialized)
    return false;
  Child().socket.reset(
      base::internal::DuplicateOhosChildDescriptor(received_fd));
  Child().initialized = Child().socket.is_valid();
  return Child().initialized;
}
bool IsOhosPhoneGpuChild() {
  return Child().initialized;
}
int OhosPhoneGpuMinimumBufferFd() {
  base::AutoLock lock(Child().lock);
  return Child().socket.get() + 1;
}
phone_gpu::Result SubmitOhosPhoneGpuFrame(phone_gpu::Frame frame,
                                          int buffer_fd) {
  auto& state = Child();
  base::AutoLock lock(state.lock);
  frame.sequence = ++state.sequence;
  phone_gpu::Result result = phone_gpu::Result::kFailed;
  if (!state.socket.is_valid() ||
      !phone_gpu::SendFrame(state.socket.get(), frame, buffer_fd) ||
      !phone_gpu::ReceiveReply(state.socket.get(), frame.sequence, &result)) {
    // Never reuse a buffer while the peer may still be reading it. Failed
    // submission makes the surface return SWAP_FAILED and Chromium recover.
    if (state.socket.is_valid())
      shutdown(state.socket.get(), SHUT_RDWR);
    state.socket.reset();
    LOG(ERROR) << "OHOS phone GPU: presentation channel failed";
    return phone_gpu::Result::kFailed;
  }
  return result;
}
}  // namespace ui
