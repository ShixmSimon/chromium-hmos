// Copyright 2026 The Chromium Authors. BSD-style license in LICENSE.
#include "chrome/renderer/ohos/resource_throttle.h"

#include "chrome/common/ohos/adblock/adblock.mojom.h"
#include "chrome/common/ohos/resource_policy.h"
#include "content/public/renderer/render_frame.h"
#include "content/public/renderer/render_frame_observer.h"
#include "content/public/renderer/render_frame_observer_tracker.h"
#include "content/public/renderer/render_thread.h"
#include "mojo/public/cpp/bindings/shared_remote.h"
#include "net/http/http_response_headers.h"
#include "net/url_request/redirect_info.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "third_party/blink/public/platform/browser_interface_broker_proxy.h"
#include "third_party/blink/public/common/loader/url_loader_throttle.h"
#include "third_party/blink/public/common/loader/resource_type_util.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/public/web/web_document.h"
#include "url/origin.h"

namespace chrome::ohos::resources {
namespace {
// Share one document-bound pipe across its loads. SharedRemote also supports
// synchronous XHR throttles moved to a loading sequence by Blink.
class Reporter : public content::RenderFrameObserver,
                 public content::RenderFrameObserverTracker<Reporter> {
 public:
  explicit Reporter(content::RenderFrame* frame)
      : RenderFrameObserver(frame), RenderFrameObserverTracker<Reporter>(frame) {}
  void OnDestruct() override { delete this; }
  mojo::SharedRemote<adblock::mojom::ResourceObserver> Remote() {
    const auto document = render_frame()->GetWebFrame()->GetDocument().Token();
    if (!remote_.is_bound() || token_ != document) {
      token_ = document;
      mojo::PendingRemote<adblock::mojom::ResourceObserver> pending;
      render_frame()->GetBrowserInterfaceBroker().GetInterface(pending.InitWithNewPipeAndPassReceiver());
      remote_ = mojo::SharedRemote<adblock::mojom::ResourceObserver>(std::move(pending));
    }
    return remote_;
  }
 private:
  blink::DocumentToken token_;
  mojo::SharedRemote<adblock::mojom::ResourceObserver> remote_;
};

class Throttle : public blink::URLLoaderThrottle {
 public:
  Throttle(mojo::SharedRemote<adblock::mojom::ResourceObserver> remote,
           const network::ResourceRequest& request)
      : remote_(std::move(remote)), request_(request) {}
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
    const bool same_request_origin =
        url::Origin::Create(request_.url) == url::Origin::Create(url);
    value->referrer = same_request_origin ? request_.referrer : GURL();
    value->referrer_policy = static_cast<int32_t>(request_.referrer_policy);
    value->mode = request_.mode;
    value->credentials_mode = request_.credentials_mode;
    value->authorization = same_request_origin
        ? request_.headers.GetHeader("Authorization").value_or("") : "";
    if (value->authorization.size() > 8192 || value->content_range.size() > 128) return;
    if (remote_.is_bound()) remote_->Record(std::move(value));
  }
 private:
  mojo::SharedRemote<adblock::mojom::ResourceObserver> remote_;
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
  auto* reporter = Reporter::Get(frame);
  if (!reporter) reporter = new Reporter(frame);
  return std::make_unique<Throttle>(reporter->Remote(), request);
}
}  // namespace chrome::ohos::resources
