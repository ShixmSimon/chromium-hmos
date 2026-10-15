// Copyright 2026 The Chromium Authors. BSD-style license in LICENSE.
#ifndef CHROME_RENDERER_OHOS_ADBLOCK_H_
#define CHROME_RENDERER_OHOS_ADBLOCK_H_
#include <memory>
#include "base/types/optional_ref.h"
#include "chrome/common/ohos/adblock/adblock.mojom.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "third_party/blink/public/common/loader/url_loader_throttle.h"
namespace blink {
class ThreadSafeBrowserInterfaceBrokerProxy;
}
namespace content {
class RenderFrame;
}
namespace chrome::ohos::adblock {
class Provider {
 public:
  explicit Provider(blink::ThreadSafeBrowserInterfaceBrokerProxy* broker);
  explicit Provider(mojo::PendingRemote<mojom::Filter> pending);
  ~Provider();
  std::unique_ptr<Provider> Clone();
  std::unique_ptr<blink::URLLoaderThrottle> Create(
      base::optional_ref<const blink::LocalFrameToken> frame,
      const network::ResourceRequest& request);

 private:
  mojo::PendingRemote<mojom::Filter> pending_;
  mojo::Remote<mojom::Filter> remote_;
};
void ObserveFrame(content::RenderFrame* frame);
}  // namespace chrome::ohos::adblock
#endif
