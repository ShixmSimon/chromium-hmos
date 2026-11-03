// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#include "ui/ozone/platform/ohos/ohos_overlay_api.h"
#include <dlfcn.h>
#include "base/no_destructor.h"
namespace ui {
const OhosOverlayApi* GetOhosOverlayApi() {
  static const base::NoDestructor<OhosOverlayApi> api([] {
    OhosOverlayApi result;
    // Keep the module loaded while callbacks/RS leases may still be in flight.
    void* library = dlopen("libohos_video_overlay.so", RTLD_NOW | RTLD_LOCAL);
    if (!library)
      return result;
#define LOAD(member, symbol) \
  result.member =            \
      reinterpret_cast<decltype(result.member)>(dlsym(library, #symbol))
    LOAD(attach, OhosOverlayAttachContent);
    LOAD(detach, OhosOverlayDetachContent);
    LOAD(connect, OhosOverlayConnect);
    LOAD(register_video, OhosOverlayRegisterVideo);
    LOAD(unregister_video, OhosOverlayUnregisterVideo);
#undef LOAD
    if (!result.attach || !result.detach || !result.connect ||
        !result.register_video || !result.unregister_video)
      return OhosOverlayApi{};
    return result;
  }());
  return api->attach ? api.get() : nullptr;
}
}  // namespace ui
