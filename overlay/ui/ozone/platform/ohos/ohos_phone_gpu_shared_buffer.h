// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef UI_OZONE_PLATFORM_OHOS_OHOS_PHONE_GPU_SHARED_BUFFER_H_
#define UI_OZONE_PLATFORM_OHOS_OHOS_PHONE_GPU_SHARED_BUFFER_H_
#include <memory>
#include "ui/ozone/platform/ohos/ohos_phone_gpu_shared_transport.h"

struct OH_NativeBuffer;
namespace ui::phone_gpu {
// Browser only, BEFORE StartOhosForkServer; never initializes EGL.
bool PrepareSharedBufferAllocator();
bool SharedBufferAllocatorReady();
// Reconstructs only the verified parcel schema; owns a native reference on
// success.
OH_NativeBuffer* ImportSharedBuffer(const SharedMessage& message);
class SharedBufferLease {
 public:
  SharedBufferLease();
  ~SharedBufferLease();
  SharedBufferLease(const SharedBufferLease&) = delete;
  SharedBufferLease& operator=(const SharedBufferLease&) = delete;
  bool Acquire(void* window, SharedMessage* message);
  // Consumes completion_fd. Flush failure does not return an in-flight buffer
  // to the queue; a disconnected lease is detached instead.
  bool Present(int completion_fd);
  void CancelBeforeDrawing();
  void* window() const { return window_; }

 private:
  void* window_ = nullptr;
  void* buffer_ = nullptr;
  bool resolved_ = false;
};
}  // namespace ui::phone_gpu
#endif
