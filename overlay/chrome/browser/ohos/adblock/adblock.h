// Copyright 2026 The Chromium Authors. BSD-style license in LICENSE.
#ifndef CHROME_BROWSER_OHOS_ADBLOCK_ADBLOCK_H_
#define CHROME_BROWSER_OHOS_ADBLOCK_ADBLOCK_H_
#include <memory>
#include <string_view>
#include "base/functional/callback.h"
#include "base/values.h"
#include "chrome/common/ohos/adblock/adblock.mojom.h"
#include "content/public/browser/global_routing_id.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "third_party/blink/public/common/loader/url_loader_throttle.h"
#include "url/origin.h"
namespace content {
class BrowserContext;
class WebContents;
}  // namespace content
namespace network {
struct ResourceRequest;
}
class Profile;
namespace chrome::ohos {
struct ShellCommandContext;
}
namespace chrome::ohos::adblock {
void CheckWebSocket(const content::GlobalRenderFrameHostId& frame_id,
                    const url::Origin& origin,
                    const GURL& url,
                    base::OnceCallback<void(bool)> callback);
void EnsureService(Profile* profile);
void ObserveContents(content::WebContents* contents);
bool HandleCommand(const ShellCommandContext& context,
                   std::string_view name,
                   const base::DictValue& command);
void BindFilter(int process_id, mojo::PendingReceiver<mojom::Filter> receiver);
std::unique_ptr<blink::URLLoaderThrottle> NavigationThrottle(
    Profile* profile,
    const network::ResourceRequest& request,
    const base::RepeatingCallback<content::WebContents*()>& getter);
}  // namespace chrome::ohos::adblock
#endif
