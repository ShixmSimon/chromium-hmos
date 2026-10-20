// Copyright 2026 The Chromium Authors. BSD-style license in LICENSE.
#ifndef CHROME_BROWSER_OHOS_RESOURCE_COLLECTOR_H_
#define CHROME_BROWSER_OHOS_RESOURCE_COLLECTOR_H_
#include <string>
#include <string_view>
#include "base/values.h"
namespace content { class RenderFrameHost; }
namespace network { class URLLoaderFactoryBuilder; }
class BrowserWindowInterface;
namespace chrome::ohos {
struct ShellCommandContext;
namespace resources {
std::u16string MenuLabel();
void RequestOpen(BrowserWindowInterface* browser);
void InstallFactory(content::RenderFrameHost* frame, network::URLLoaderFactoryBuilder& builder);
bool HandleCommand(const ShellCommandContext& context, std::string_view name,
                   const base::DictValue& command);
}  // namespace resources
}  // namespace chrome::ohos
#endif
