// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#ifndef UI_OZONE_PLATFORM_OHOS_OHOS_OVERLAY_PRESENTER_H_
#define UI_OZONE_PLATFORM_OHOS_OHOS_OVERLAY_PRESENTER_H_
#include <memory>
#include "base/memory/scoped_refptr.h"
#include "ui/gfx/native_ui_types.h"
namespace gl {
class Presenter;
}
namespace ui {
class OverlayManagerOzone;
scoped_refptr<gl::Presenter> CreateOhosOverlayPresenter(
    gfx::AcceleratedWidget widget);
std::unique_ptr<OverlayManagerOzone> CreateOhosOverlayManager();
}  // namespace ui
#endif
