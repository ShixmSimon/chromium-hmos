// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_OHOS_SHELL_OMNIBOX_OHOS_H_
#define CHROME_BROWSER_UI_OHOS_SHELL_OMNIBOX_OHOS_H_

#include <string_view>

#include "base/values.h"
#include "chrome/browser/ui/ohos/shell_services_ohos.h"

namespace chrome::ohos {

// The data behind the shell's address bar and new tab page: "autocomplete"
// (suggestions as the user types) and "getTopSites" (most visited tiles).
// Reached through HandleHistoryCommand, since both are built on history.
// Returns true when it recognised and handled `name`.
bool HandleOmniboxCommand(const ShellCommandContext& context,
                          std::string_view name,
                          const base::DictValue& command);

}  // namespace chrome::ohos

#endif  // CHROME_BROWSER_UI_OHOS_SHELL_OMNIBOX_OHOS_H_
