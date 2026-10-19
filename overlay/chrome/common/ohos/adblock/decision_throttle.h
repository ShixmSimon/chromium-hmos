// Copyright 2026 The Chromium Authors. BSD-style license in LICENSE.
#ifndef CHROME_COMMON_OHOS_ADBLOCK_DECISION_THROTTLE_H_
#define CHROME_COMMON_OHOS_ADBLOCK_DECISION_THROTTLE_H_
#include "chrome/common/ohos/adblock/adblock.mojom.h"
#include "services/network/public/cpp/resource_request.h"
#include "third_party/blink/public/common/loader/url_loader_throttle.h"
namespace chrome::ohos::adblock {
// Shared execution semantics for browser navigation and renderer subresources.
class DecisionThrottle : public blink::URLLoaderThrottle {
 public:
  void WillProcessResponse(const GURL& url,
                           network::mojom::URLResponseHead* head,
                           bool* defer) override;

 protected:
  void Capture(const network::ResourceRequest& request);
  void Redirect(const net::RedirectInfo& redirect);
  void Resolve(mojom::DecisionPtr decision);
  network::ResourceRequest request_;

 private:
  std::string csp_;
  unsigned rewrites_ = 0;
};
}  // namespace chrome::ohos::adblock
#endif
