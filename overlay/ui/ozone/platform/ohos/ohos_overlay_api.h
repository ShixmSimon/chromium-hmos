// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#ifndef UI_OZONE_PLATFORM_OHOS_OHOS_OVERLAY_API_H_
#define UI_OZONE_PLATFORM_OHOS_OHOS_OVERLAY_API_H_
#include "ui/ozone/platform/ohos/ohos_overlay_bridge.h"
namespace ui {
// Only C functions cross the Chromium __Cr / system __h libc++ boundary.
struct OhosOverlayApi {
  decltype(&OhosOverlayAttachContent) attach = nullptr;
  decltype(&OhosOverlayDetachContent) detach = nullptr;
  decltype(&OhosOverlayConnect) connect = nullptr;
  decltype(&OhosOverlayRegisterVideo) register_video = nullptr;
  decltype(&OhosOverlayUnregisterVideo) unregister_video = nullptr;
};
const OhosOverlayApi* GetOhosOverlayApi();
}  // namespace ui
#endif
