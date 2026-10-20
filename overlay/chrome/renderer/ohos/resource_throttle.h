// Copyright 2026 The Chromium Authors. BSD-style license in LICENSE.
#ifndef CHROME_RENDERER_OHOS_RESOURCE_THROTTLE_H_
#define CHROME_RENDERER_OHOS_RESOURCE_THROTTLE_H_
#include <memory>
#include "base/types/optional_ref.h"
#include "third_party/blink/public/common/tokens/tokens.h"
namespace blink { class URLLoaderThrottle; }
namespace network { struct ResourceRequest; }
namespace chrome::ohos::resources {
std::unique_ptr<blink::URLLoaderThrottle> CreateThrottle(
    base::optional_ref<const blink::LocalFrameToken> token,
    const network::ResourceRequest& request);
}
#endif
