// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/gpu/ohos/ohos_video_surface.h"

#include <native_buffer/native_buffer.h>
#include <native_image/graphic_error_code.h>
#include <native_window/external_window.h>

#include <unistd.h>

#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/logging.h"
#include "base/posix/eintr_wrapper.h"
#include "base/task/bind_post_task.h"
#include "base/task/thread_pool.h"
#include "base/trace_event/trace_event.h"

namespace media {

OhosVideoSurface::OhosVideoSurface(
    scoped_refptr<base::SequencedTaskRunner> task_runner,
    base::RepeatingClosure frame_available,
    bool release_on_worker)
    : base::RefCountedDeleteOnSequence<OhosVideoSurface>(task_runner),
      task_runner_(std::move(task_runner)),
      release_task_runner_(
          release_on_worker
              ? base::ThreadPool::CreateSequencedTaskRunner(
                    {base::MayBlock(), base::TaskPriority::USER_VISIBLE})
              : task_runner_),
      frame_available_(std::move(frame_available)) {}

OhosVideoSurface::~OhosVideoSurface() {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  StopListening();
  if (image_) {
    OH_NativeImage_Destroy(&image_);
  }
  window_ = nullptr;
}

scoped_refptr<OhosVideoSurface> OhosVideoSurface::Create(
    scoped_refptr<base::SequencedTaskRunner> task_runner,
    const gfx::Size& size,
    base::RepeatingClosure frame_available,
    bool release_on_worker) {
  DCHECK(task_runner->RunsTasksInCurrentSequence());
  auto surface = base::WrapRefCounted(
      new OhosVideoSurface(std::move(task_runner), std::move(frame_available),
                           release_on_worker));
  surface->image_ = OH_ConsumerSurface_Create();
  if (!surface->image_) {
    return nullptr;
  }
  // GPU sampling only: never CPU-map the decoded surface (it may use HEBC).
  if (OH_ConsumerSurface_SetDefaultUsage(surface->image_,
                                         NATIVEBUFFER_USAGE_HW_TEXTURE) != 0 ||
      OH_ConsumerSurface_SetDefaultSize(surface->image_, size.width(),
                                        size.height()) != 0) {
    return nullptr;
  }
  surface->window_ = OH_NativeImage_AcquireNativeWindow(surface->image_);
  if (!surface->window_) {
    return nullptr;
  }
  OH_OnFrameAvailableListener listener = {surface.get(), &OnFrameAvailable};
  if (OH_NativeImage_SetOnFrameAvailableListener(surface->image_, listener) !=
      0) {
    return nullptr;
  }
  surface->listening_ = true;
  return surface;
}

int OhosVideoSurface::GetBufferQueueSize() const {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  int size = 0;
  if (!window_ || OH_NativeWindow_NativeWindowHandleOpt(
                      window_, GET_BUFFERQUEUE_SIZE, &size) != 0) {
    return 0;
  }
  return size;
}

void OhosVideoSurface::StopListening() {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  if (listening_) {
    OH_NativeImage_UnsetOnFrameAvailableListener(image_);
    listening_ = false;
  }
}

void OhosVideoSurface::OnFrameAvailable(void* context) {
  // This callback only posts. NativeImage serializes buffer acquire/release;
  // the phone can return buffers without waiting for a blocking codec render.
  static_cast<OhosVideoSurface*>(context)->frame_available_.Run();
}

void OhosVideoSurface::Retire() {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  StopListening();
  // Finish any release already using the queue before codec teardown clears
  // its cache. Later worker releases see retired_ and only drop references.
  base::AutoLock lock(release_lock_);
  retired_ = true;
}

OhosVideoSurface::AcquireResult OhosVideoSurface::Acquire(
    OHNativeWindowBuffer** buffer,
    base::ScopedFD* acquire_fence,
    base::OnceClosure* release,
    base::OnceClosure on_released) {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  {
    base::AutoLock lock(release_lock_);
    if (retired_) {
      return AcquireResult::kError;
    }
  }
  int fence = -1;
  *buffer = nullptr;
  const int result =
      OH_NativeImage_AcquireNativeWindowBuffer(image_, buffer, &fence);
  acquire_fence->reset(fence);
  if (result == NATIVE_ERROR_NO_BUFFER) {
    return AcquireResult::kEmpty;
  }
  if (result != NATIVE_ERROR_OK || !*buffer) {
    LOG(ERROR) << "OHOS NativeImage acquire failed; status=" << result
               << " has_buffer=" << (*buffer != nullptr);
    return AcquireResult::kError;
  }
  if (OH_NativeWindow_NativeObjectReference(*buffer) != 0) {
    if (OH_NativeImage_ReleaseNativeWindowBuffer(
            image_, *buffer, acquire_fence->release()) != 0) {
      OH_NativeWindow_NativeObjectUnreference(*buffer);
    }
    *buffer = nullptr;
    return AcquireResult::kError;
  }
  // Preserve the producer fence even if conversion is cancelled before GPU
  // import. Returning an unfinished decode buffer with fence=-1 is unsafe.
  base::ScopedFD producer_fence;
  if (acquire_fence->is_valid()) {
    producer_fence.reset(HANDLE_EINTR(dup(acquire_fence->get())));
    if (!producer_fence.is_valid()) {
      ReleaseBuffer(BufferToRelease{*buffer, std::move(*acquire_fence)}, {});
      *buffer = nullptr;
      return AcquireResult::kError;
    }
  }
  // ScopedClosureRunner also releases when a posted conversion task is dropped.
  auto lease = base::ScopedClosureRunner(base::BindPostTask(
      release_task_runner_,
      base::BindOnce(&OhosVideoSurface::ReleaseBuffer,
                     base::WrapRefCounted(this),
                     BufferToRelease{*buffer, std::move(producer_fence)},
                     std::move(on_released))));
  *release = base::BindOnce([](base::ScopedClosureRunner) {}, std::move(lease));
  return AcquireResult::kAcquired;
}

void OhosVideoSurface::ReleaseBuffer(BufferToRelease acquired,
                                     base::OnceClosure on_released) {
  TRACE_EVENT0("media", "OhosVideoSurface::ReleaseBuffer");
  // Post the decoder's in-flight decrement only after the native release and
  // after dropping the retirement lock. Enqueueing release is not completion.
  base::ScopedClosureRunner released(std::move(on_released));
  base::AutoLock lock(release_lock_);
  OHNativeWindowBuffer* buffer = acquired.buffer;
  if (retired_) {
    // Codec teardown already removed the buffer from the queue's cache. The
    // caller still waits for GPU reads before reaching here. Drop both the
    // AcquireNativeWindowBuffer reference and our extra reference; ScopedFD
    // closes the producer fence without transferring it to a dead queue.
    OH_NativeWindow_NativeObjectUnreference(buffer);
    OH_NativeWindow_NativeObjectUnreference(buffer);
    return;
  }
  // The caller waits the VideoFrame release token when the GPU used the frame.
  // The original producer fence also covers frames dropped before import.
  if (OH_NativeImage_ReleaseNativeWindowBuffer(
          image_, buffer, acquired.producer_fence.release()) != 0) {
    LOG(ERROR) << "OHOS video zero-copy: surface release failed";
    OH_NativeWindow_NativeObjectUnreference(buffer);
  }
  OH_NativeWindow_NativeObjectUnreference(buffer);
}

}  // namespace media
