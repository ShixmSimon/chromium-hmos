// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GPU_IPC_CLIENT_OHOS_COMMAND_BUFFER_STATE_H_
#define GPU_IPC_CLIENT_OHOS_COMMAND_BUFFER_STATE_H_

#include "gpu/command_buffer/common/command_buffer_shared.h"
#include "gpu/command_buffer/common/constants.h"

namespace gpu {

// Shared memory is not Mojo enum-validated. Reject a malformed snapshot before
// its offsets, release counts or error enums reach clients and loss metrics.
template <typename SharedStateReader>
inline void ReadOhosCommandBufferSharedState(SharedStateReader* shared,
                                           CommandBuffer::State* state) {
  CommandBuffer::State candidate = *state;
  shared->Read(&candidate);
  const int error_value = static_cast<int>(candidate.error);
  const int reason = static_cast<int>(candidate.context_lost_reason);
  if (error_value < 0 || error_value > error::kErrorLast || reason < 0 ||
      reason > error::kContextLostReasonLast) {
    // Keep the last known offsets and fence progress; never acknowledge work
    // using corrupted shared memory. Existing client error handling tears down
    // the channel and notifies the compositor to recreate its context.
    state->error = error::kLostContext;
    state->context_lost_reason = error::kInvalidGpuMessage;
    return;
  }
  *state = candidate;
}

}  // namespace gpu

#endif  // GPU_IPC_CLIENT_OHOS_COMMAND_BUFFER_STATE_H_
