// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef UI_OZONE_PLATFORM_OHOS_OHOS_PHONE_GPU_PRESENTER_H_
#define UI_OZONE_PLATFORM_OHOS_OHOS_PHONE_GPU_PRESENTER_H_

#include "ui/ozone/platform/ohos/ohos_phone_gpu_transport.h"

namespace ui::phone_gpu {
// Browser worker only, serialized per native window. The caller holds a native
// window reference for the entire call. Never dereferences a child pointer.
bool Present(void* window, const Frame& frame, const void* pixels);
}  // namespace ui::phone_gpu
#endif
