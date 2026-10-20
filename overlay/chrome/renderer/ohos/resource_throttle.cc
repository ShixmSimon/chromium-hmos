// Copyright 2026 The Chromium Authors. BSD-style license in LICENSE.
#include "chrome/renderer/ohos/resource_throttle.h"

#include "chrome/common/ohos/adblock/adblock.mojom.h"
#include "chrome/common/ohos/resource_policy.h"
#include "content/public/renderer/render_frame.h"
#include "content/public/renderer/render_thread.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "net/http/http_response_headers.h"
#include "net/url_request/redirect_info.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "third_party/blink/public/platform/browser_interface_broker_proxy.h"
#include "third_party/blink/public/common/loader/url_loader_throttle.h"
#include "third_party/blink/public/common/loader/resource_type_util.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "url/origin.h"

namespace chrome::ohos::resources {
namespace {
class Throttle : public blink::URLLoaderThrottle {
 public:
  Throttle(mojo::PendingRemote<adblock::mojom::ResourceObserver> remote,
           const network::ResourceRequest& request)
      : pending_(std::move(remote)), request_(request) {}
  void DetachFromCurrentSequence() override {}
  void WillStartRequest(network::ResourceRequest* request, bool*) override {
    request_ = *request;
  }
  void WillRedirectRequest(net::RedirectInfo* info,
      const network::mojom::URLResponseHead&, bool*,
      network::HttpRequestHeadersUpdateParams*) override {
    // Authorization must not be replayed across a redirect to a new origin.
    if (url::Origin::Create(request_.url) != url::Origin::Create(info->new_url))
      request_.headers.RemoveHeader("Authorization");
    request_.UpdateOnRedirect(*info);
  }
  void WillProcessResponse(const GURL& url, network::mojom::URLResponseHead* head,
                           bool*) override {
    if (!head->headers || !url.SchemeIsHTTPOrHTTPS()) return;
    auto value = adblock::mojom::ResourceResponse::New();
    value->status = head->headers->response_code();
    value->mime_type = head->mime_type;
    value->disposition = head->headers->GetNormalizedHeader("content-disposition").value_or("");
    if (Classify(url.path(), value->mime_type, value->disposition,
                 value->status, request_.method).empty()) return;
    if (url.spec().size() > 8192 || value->mime_type.size() > 256 ||
        value->disposition.size() > 2048) return;
    value->url = url;
    value->content_range = head->headers->GetNormalizedHeader("content-range").value_or("");
    value->content_length = head->content_length;
    value->method = request_.method;
    value->referrer = request_.referrer;
    value->referrer_policy = static_cast<int32_t>(request_.referrer_policy);
    value->mode = request_.mode;
    value->credentials_mode = request_.credentials_mode;
    value->authorization = request_.headers.GetHeader("Authorization").value_or("");
    if (value->authorization.size() > 8192 || value->content_range.size() > 128) return;
    if (pending_) remote_.Bind(std::move(pending_));
    if (remote_.is_connected()) remote_->Record(std::move(value));
  }
 private:
  mojo::PendingRemote<adblock::mojom::ResourceObserver> pending_;
  mojo::Remote<adblock::mojom::ResourceObserver> remote_;
  network::ResourceRequest request_;
};
}  // namespace
std::unique_ptr<blink::URLLoaderThrottle> CreateThrottle(
    base::optional_ref<const blink::LocalFrameToken> token,
    const network::ResourceRequest& request) {
  // Unattributed Worker requests are intentionally excluded in v1.
  if (!token.has_value() || !content::RenderThread::IsMainThread() ||
      blink::IsRequestDestinationFrame(request.destination) ||
      !request.url.SchemeIsHTTPOrHTTPS() || request.method != "GET") return nullptr;
  using D = network::mojom::RequestDestination;
  if (request.destination != D::kEmpty && request.destination != D::kVideo &&
      request.destination != D::kAudio && request.destination != D::kImage &&
      request.destination != D::kObject && request.destination != D::kEmbed) return nullptr;
  auto* web_frame = blink::WebLocalFrame::FromFrameToken(token.value());
  auto* frame = web_frame ? content::RenderFrame::FromWebFrame(web_frame) : nullptr;
  if (!frame) return nullptr;
  mojo::PendingRemote<adblock::mojom::ResourceObserver> remote;
  frame->GetBrowserInterfaceBroker().GetInterface(remote.InitWithNewPipeAndPassReceiver());
  return std::make_unique<Throttle>(std::move(remote), request);
}
}  // namespace chrome::ohos::resources
