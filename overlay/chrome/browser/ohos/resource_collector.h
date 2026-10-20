// Copyright 2026 The Chromium Authors. BSD-style license in LICENSE.
#ifndef CHROME_BROWSER_OHOS_RESOURCE_COLLECTOR_H_
#define CHROME_BROWSER_OHOS_RESOURCE_COLLECTOR_H_
#include <string_view>
#include "base/values.h"
#include "chrome/common/ohos/adblock/adblock.mojom.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
namespace content { class RenderFrameHost; }
class BrowserWindowInterface;
namespace chrome::ohos {
struct ShellCommandContext;
namespace resources {
std::u16string MenuLabel();
void RequestOpen(BrowserWindowInterface* browser);
void Bind(content::RenderFrameHost* frame,
          mojo::PendingReceiver<adblock::mojom::ResourceObserver> receiver);
bool HandleCommand(const ShellCommandContext& context, std::string_view name,
                   const base::DictValue& command);
}  // namespace resources
}  // namespace chrome::ohos
#endif
