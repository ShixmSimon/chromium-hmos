// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MEDIA_GPU_OHOS_OHOS_VIDEO_SURFACE_H_
#define MEDIA_GPU_OHOS_OHOS_VIDEO_SURFACE_H_

#include <native_image/native_image.h>

#include "base/files/scoped_file.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "base/memory/ref_counted_delete_on_sequence.h"
#include "base/memory/scoped_refptr.h"
#include "base/task/sequenced_task_runner.h"
#include "ui/gfx/geometry/size.h"

namespace media {

// One surface per codec epoch. Frames keep this alive across reset/destruction;
// the old queue is never reused by a new codec while the GPU can still read it.
class OhosVideoSurface
    : public base::RefCountedDeleteOnSequence<OhosVideoSurface> {
 public:
  static scoped_refptr<OhosVideoSurface> Create(
      scoped_refptr<base::SequencedTaskRunner> task_runner,
      const gfx::Size& size,
      base::RepeatingClosure frame_available);

  OHNativeWindow* window() const { return window_; }
  int GetBufferQueueSize() const;
  void StopListening();
  // Call before destroying the producer. Its teardown clears the queue cache;
  // outstanding frames must then drop their references without ReleaseBuffer.
  void Retire();

  // The returned closure owns the acquired buffer and the surface. Run or
  // destroy it only after GPU reads have completed. It releases on our
  // sequence.
  enum class AcquireResult { kAcquired, kEmpty, kError };
  AcquireResult Acquire(OHNativeWindowBuffer** buffer,
                        base::ScopedFD* acquire_fence,
                        base::OnceClosure* release);

 private:
  friend class base::RefCountedDeleteOnSequence<OhosVideoSurface>;
  friend class base::DeleteHelper<OhosVideoSurface>;
  OhosVideoSurface(scoped_refptr<base::SequencedTaskRunner> task_runner,
                   base::RepeatingClosure frame_available);
  ~OhosVideoSurface();
  static void OnFrameAvailable(void* context);
  struct BufferToRelease {
    RAW_PTR_EXCLUSION OHNativeWindowBuffer* buffer;
    base::ScopedFD producer_fence;
  };
  void ReleaseBuffer(BufferToRelease acquired);

  const scoped_refptr<base::SequencedTaskRunner> task_runner_;
  // Immutable: the native callback may run on another thread until Destroy.
  const base::RepeatingClosure frame_available_;
  RAW_PTR_EXCLUSION OH_NativeImage* image_ = nullptr;
  // Borrowed from image_; NativeImage_Destroy releases it.
  RAW_PTR_EXCLUSION OHNativeWindow* window_ = nullptr;
  bool listening_ = false;
  bool retired_ = false;
};

}  // namespace media
#endif
