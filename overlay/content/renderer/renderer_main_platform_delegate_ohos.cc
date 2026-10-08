// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/renderer/renderer_main_platform_delegate.h"

namespace content {

RendererMainPlatformDelegate::RendererMainPlatformDelegate(
    const MainFunctionParams& parameters) {}

RendererMainPlatformDelegate::~RendererMainPlatformDelegate() = default;

void RendererMainPlatformDelegate::PlatformInitialize() {}

void RendererMainPlatformDelegate::PlatformUninitialize() {}

bool RendererMainPlatformDelegate::EnableSandbox() {
  // This hook installs no Chromium sandbox. The appspawn launch policy is
  // chosen in base/process/launch_ohos.cc; its normal mode shares the app's
  // sandbox and UID. Returning true here is NOT evidence of renderer
  // isolation. The experimental isolated launch must be verified separately.
  return true;
}

}  // namespace content
