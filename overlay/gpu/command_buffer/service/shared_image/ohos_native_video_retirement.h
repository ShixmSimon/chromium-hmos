// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#ifndef GPU_COMMAND_BUFFER_SERVICE_SHARED_IMAGE_OHOS_NATIVE_VIDEO_RETIREMENT_H_
#define GPU_COMMAND_BUFFER_SERVICE_SHARED_IMAGE_OHOS_NATIVE_VIDEO_RETIREMENT_H_
#include <errno.h>
#include <linux/sync_file.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include "base/files/scoped_file.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/synchronization/lock.h"
#include "base/task/single_thread_task_runner.h"
#include "base/trace_event/trace_event.h"
#include "gpu/command_buffer/service/shared_context_state.h"
#include "ui/gfx/gpu_fence_handle.h"
#include "ui/gfx/native_pixmap.h"
namespace gpu {
namespace ohos_native_video {

// A decoder queue slot must not be reused if a completion fence was lost.
inline void Quarantine(scoped_refptr<gfx::NativePixmap> pixmap,
                       scoped_refptr<SharedContextState> context) {
  struct Leases {
    base::Lock lock;
    std::vector<scoped_refptr<gfx::NativePixmap>> pixmaps;
  };
  static base::NoDestructor<Leases> leases;
  {
    base::AutoLock lock(leases->lock);
    leases->pixmaps.push_back(std::move(pixmap));
  }
  if (!context->context_lost())
    context->MarkContextLost();
  LOG(ERROR) << "OHOS native video: completion unknown; decoder lease retained";
}

// Read-lock sync tokens order command submission. Retain the actual decoder
// lease separately until Dawn's GPU completion fence signals, without waiting
// on the GPU main thread or copying any pixels.
inline void Retire(scoped_refptr<gfx::NativePixmap> pixmap,
                   scoped_refptr<SharedContextState> context,
                   gfx::GpuFenceHandle fence) {
  if (fence.is_null())
    return;
  pollfd descriptor{fence.Peek(), POLLIN, 0};
  const int result = poll(&descriptor, 1, 0);
  if (result > 0 && (descriptor.revents & POLLIN) &&
      !(descriptor.revents & (POLLERR | POLLNVAL | POLLHUP))) {
    TRACE_EVENT_INSTANT("gpu", "OhosNativeVideoLeaseCompleted");
    return;
  }
  if (context->context_lost() ||
      (result < 0 && errno != EINTR) ||
      (descriptor.revents & (POLLERR | POLLNVAL | POLLHUP))) {
    Quarantine(std::move(pixmap), std::move(context));
    return;
  }
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&Retire, std::move(pixmap), std::move(context),
                     std::move(fence)),
      base::Milliseconds(4));
}

inline bool Merge(base::ScopedFD& merged, int fd) {
  if (fd == -1)
    return true;  // An already-signaled sync-file fence.
  if (fd < -1)
    return false;
  if (!merged.is_valid()) {
    merged.reset(dup(fd));
    return merged.is_valid();
  }
  sync_merge_data data{};
  data.fd2 = fd;
  if (ioctl(merged.get(), SYNC_IOC_MERGE, &data) != 0)
    return false;
  merged.reset(data.fence);
  return merged.is_valid();
}

}  // namespace ohos_native_video
}  // namespace gpu
#endif
