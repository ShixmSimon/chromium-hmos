// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GPU_IPC_SERVICE_OHOS_GPU_CONTEXT_RECOVERY_H_
#define GPU_IPC_SERVICE_OHOS_GPU_CONTEXT_RECOVERY_H_

#include "gpu/command_buffer/common/context_result.h"
#include "gpu/ipc/service/gpu_channel_manager_delegate.h"

namespace gpu {

// Never terminate a browser-hosted GPU. Transient surface failures can recover
// locally; a fatal shared context failure in a GPU child needs a fresh process.
inline void RestartOhosGpuAfterSharedContextFailure(
    bool gpu_child,
    ContextResult failure,
    GpuChannelManagerDelegate* delegate) {
  if (gpu_child && failure == ContextResult::kFatalFailure &&
      !delegate->IsExiting()) {
    delegate->MaybeExitOnContextLost(error::kMakeCurrentFailed);
  }
}

}  // namespace gpu

#endif  // GPU_IPC_SERVICE_OHOS_GPU_CONTEXT_RECOVERY_H_
