// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_OHOS_SHELL_EXTENSION_UPDATES_OHOS_H_
#define CHROME_BROWSER_UI_OHOS_SHELL_EXTENSION_UPDATES_OHOS_H_

#include <string_view>

#include "base/functional/callback_forward.h"
#include "base/values.h"
#include "ui/gfx/native_ui_types.h"

class BrowserWindowInterface;
class Profile;

namespace chrome::ohos {

bool HandleShellExtensionUpdateCommand(std::string_view name,
                                       gfx::AcceleratedWidget widget,
                                       BrowserWindowInterface* browser,
                                       const base::DictValue& command);

// chrome://extensions and the phone shell share one native update task per
// regular Profile. False means unavailable; the callback is then not invoked.
bool StartNativeExtensionUpdates(Profile* profile, base::OnceClosure completed);

}  // namespace chrome::ohos

#endif  // CHROME_BROWSER_UI_OHOS_SHELL_EXTENSION_UPDATES_OHOS_H_
