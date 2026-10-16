// Copyright 2026 The Chromium Authors. BSD-style license in LICENSE.
#ifndef CHROME_BROWSER_OHOS_TRANSLATION_TRANSLATION_H_
#define CHROME_BROWSER_OHOS_TRANSLATION_TRANSLATION_H_
#include <string_view>
#include "base/values.h"
namespace chrome::ohos {
struct ShellCommandContext;
namespace translation {
bool HandleCommand(const ShellCommandContext& context,
                   std::string_view name,
                   const base::DictValue& command);
}
}  // namespace chrome::ohos
#endif
