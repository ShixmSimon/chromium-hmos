// Copyright 2026 The Chromium Authors. BSD-style license in LICENSE.
#include "chrome/renderer/ohos/adblock.h"
#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "content/public/renderer/render_frame.h"
#include "content/public/renderer/render_frame_observer.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "net/base/net_errors.h"
#include "net/url_request/redirect_info.h"
#include "services/network/public/cpp/resource_request.h"
#include "third_party/blink/public/common/loader/resource_type_util.h"
#include "third_party/blink/public/common/thread_safe_browser_interface_broker_proxy.h"
#include "third_party/blink/public/platform/platform.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/public/web/web_document.h"
#include "third_party/blink/public/web/web_local_frame.h"
namespace chrome::ohos::adblock {
namespace {
std::string ResourceType(network::mojom::RequestDestination destination) {
  using D = network::mojom::RequestDestination;
  switch (destination) {
    case D::kScript:
    case D::kWorker:
    case D::kSharedWorker:
    case D::kServiceWorker:
      return "script";
    case D::kStyle:
      return "stylesheet";
    case D::kImage:
      return "image";
    case D::kFont:
      return "font";
    case D::kAudio:
    case D::kVideo:
    case D::kTrack:
      return "media";
    case D::kObject:
    case D::kEmbed:
      return "object";
    case D::kEmpty:
      return "xmlhttprequest";
    default:
      return "other";
  }
}
class Throttle : public blink::URLLoaderThrottle {
 public:
  Throttle(mojo::PendingRemote<mojom::Filter> remote,
           blink::LocalFrameToken frame,
           std::string type)
      : pending_(std::move(remote)), frame_(frame), type_(std::move(type)) {}
  void WillStartRequest(network::ResourceRequest* request,
                        bool* defer) override {
    source_ = request->request_initiator ? request->request_initiator->GetURL()
                                         : GURL();
    method_ = request->method;
    Check(request->url, defer);
  }
  void WillRedirectRequest(net::RedirectInfo* redirect,
                           const network::mojom::URLResponseHead&,
                           bool* defer,
                           network::HttpRequestHeadersUpdateParams*) override {
    method_ = redirect->new_method;
    Check(redirect->new_url, defer);
  }

 private:
  void Check(const GURL& url, bool* defer) {
    if (!url.SchemeIsHTTPOrHTTPS())
      return;
    if (pending_)
      remote_.Bind(std::move(pending_));
    if (!remote_.is_connected())
      return;
    *defer = true;
    remote_->Check(frame_, url, source_, type_, method_,
                   mojo::WrapCallbackWithDefaultInvokeIfNotRun(
                       base::BindOnce(
                           [](base::WeakPtr<Throttle> self, bool blocked) {
                             if (!self)
                               return;
                             if (blocked)
                               self->delegate_->CancelWithError(
                                   net::ERR_BLOCKED_BY_CLIENT);
                             else
                               self->delegate_->Resume();
                           },
                           weak_.GetWeakPtr()),
                       false));
  }
  mojo::PendingRemote<mojom::Filter> pending_;
  mojo::Remote<mojom::Filter> remote_;
  blink::LocalFrameToken frame_;
  std::string type_, method_;
  GURL source_;
  base::WeakPtrFactory<Throttle> weak_{this};
};
class CosmeticObserver : public content::RenderFrameObserver {
 public:
  explicit CosmeticObserver(content::RenderFrame* frame)
      : content::RenderFrameObserver(frame) {}
  void OnDestruct() override { delete this; }
  void DidCreateDocumentElement() override { Start(); }
  void DidCommitProvisionalLoad(ui::PageTransition) override {
    weak_.InvalidateWeakPtrs();
    remote_.reset();
    retry_.Stop();
    key_ = blink::WebString();
    css_.clear();
  }
  void DidSetPageLifecycleState(blink::BFCacheStateChange) override { Start(); }

 private:
  void Start() {
    if (!render_frame() || !render_frame()->GetWebFrame())
      return;
    auto document = render_frame()->GetWebFrame()->GetDocument();
    GURL url = document.Url();
    if (!url.SchemeIsHTTPOrHTTPS())
      return;
    weak_.InvalidateWeakPtrs();
    remote_.reset();
    retry_.Stop();
    blink::Platform::Current()->GetBrowserInterfaceBroker()->GetInterface(
        remote_.BindNewPipeAndPassReceiver());
    Fetch(0);
  }
  void Fetch(uint64_t revision) {
    if (!render_frame() || !remote_.is_connected())
      return;
    auto* frame = render_frame()->GetWebFrame();
    auto document = frame->GetDocument();
    const auto token = document.Token();
    remote_->Cosmetics(
        frame->GetLocalFrameToken(), document.Url(), revision,
        base::BindOnce(&CosmeticObserver::Apply, weak_.GetWeakPtr(), token));
  }
  void Apply(blink::DocumentToken token,
             const std::string& css,
             uint64_t revision) {
    if (!render_frame() ||
        render_frame()->GetWebFrame()->GetDocument().Token() != token)
      return;
    auto document = render_frame()->GetWebFrame()->GetDocument();
    if (css != css_) {
      if (!key_.IsEmpty())
        document.RemoveInsertedStyleSheet(key_, blink::WebCssOrigin::kUser);
      key_ = blink::WebString();
      css_ = css;
      if (!css.empty())
        key_ = document.InsertStyleSheet(blink::WebString::FromUTF8(css),
                                         nullptr, blink::WebCssOrigin::kUser);
    }
    // A new document can reach the renderer before its browser-side commit.
    // Retry that race without a tight IPC loop; otherwise long-poll changes.
    if (revision == 0)
      retry_.Start(
          FROM_HERE, base::Milliseconds(250),
          base::BindOnce(&CosmeticObserver::Fetch, weak_.GetWeakPtr(), 0));
    else
      Fetch(revision);
  }
  mojo::Remote<mojom::Filter> remote_;
  blink::WebStyleSheetKey key_;
  std::string css_;
  base::OneShotTimer retry_;
  base::WeakPtrFactory<CosmeticObserver> weak_{this};
};
}  // namespace
Provider::Provider(blink::ThreadSafeBrowserInterfaceBrokerProxy* broker) {
  broker->GetInterface(pending_.InitWithNewPipeAndPassReceiver());
}
Provider::Provider(mojo::PendingRemote<mojom::Filter> pending)
    : pending_(std::move(pending)) {}
Provider::~Provider() = default;
std::unique_ptr<Provider> Provider::Clone() {
  if (pending_)
    remote_.Bind(std::move(pending_));
  mojo::PendingRemote<mojom::Filter> pending;
  remote_->Clone(pending.InitWithNewPipeAndPassReceiver());
  return std::make_unique<Provider>(std::move(pending));
}
std::unique_ptr<blink::URLLoaderThrottle> Provider::Create(
    base::optional_ref<const blink::LocalFrameToken> frame,
    const network::ResourceRequest& request) {
  if (!frame.has_value() ||
      blink::IsRequestDestinationFrame(request.destination))
    return nullptr;
  if (pending_)
    remote_.Bind(std::move(pending_));
  if (!remote_.is_connected())
    return nullptr;
  mojo::PendingRemote<mojom::Filter> pending;
  remote_->Clone(pending.InitWithNewPipeAndPassReceiver());
  return std::make_unique<Throttle>(std::move(pending), frame.value(),
                                    ResourceType(request.destination));
}
void ObserveFrame(content::RenderFrame* frame) {
  new CosmeticObserver(frame);
}
}  // namespace chrome::ohos::adblock
