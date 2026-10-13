// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef UI_OZONE_PLATFORM_OHOS_OHOS_PHONE_GPU_CHANNEL_H_
#define UI_OZONE_PLATFORM_OHOS_OHOS_PHONE_GPU_CHANNEL_H_

#include <string>
#include <utility>
#include <vector>

#include "base/process/process_handle.h"
#include "ui/ozone/platform/ohos/ohos_phone_gpu_transport.h"

namespace ui {
base::ProcessId LaunchOhosPhoneGpuChild(
    const std::string& params,
    const std::vector<std::pair<int, int>>& fds);
bool OhosPhoneGpuHasPresented();
// Takes a copy above the inherited FD high-water mark. Call before ChromeMain.
bool InitializeOhosPhoneGpuChild(int received_fd);
bool IsOhosPhoneGpuChild();
int OhosPhoneGpuMinimumBufferFd();
phone_gpu::Result SubmitOhosPhoneGpuFrame(phone_gpu::Frame frame,
                                          int buffer_fd);
}  // namespace ui
#endif
