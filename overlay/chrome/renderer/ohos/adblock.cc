// Copyright 2026 The Chromium Authors. BSD-style license in LICENSE.
#include "chrome/renderer/ohos/adblock.h"
#include "base/check.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "chrome/common/chrome_isolated_world_ids.h"
#include "chrome/common/ohos/adblock/decision_throttle.h"
#include "chrome/renderer/ohos/adblock_resources/runtime.h"
#include "content/public/renderer/render_frame.h"
#include "content/public/renderer/render_frame_observer.h"
#include "content/public/renderer/render_frame_observer_tracker.h"
#include "gin/converter.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "mojo/public/cpp/bindings/sync_call_restrictions.h"
#include "net/base/net_errors.h"
#include "net/url_request/redirect_info.h"
#include "services/network/public/cpp/resource_request.h"
#include "third_party/blink/public/common/loader/resource_type_util.h"
#include "third_party/blink/public/common/thread_safe_browser_interface_broker_proxy.h"
#include "third_party/blink/public/platform/platform.h"
#include "third_party/blink/public/platform/scheduler/web_agent_group_scheduler.h"
#include "third_party/blink/public/platform/web_isolated_world_info.h"
#include "third_party/blink/public/platform/web_security_origin.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/public/web/web_document.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/public/web/web_local_frame_client.h"
#include "third_party/blink/public/web/web_script_source.h"
#include "v8/include/v8.h"
namespace chrome::ohos::adblock {
class EarlyCosmetics {
 public:
  static bool Fetch(mojom::Filter* filter,
                    const blink::LocalFrameToken& token,
                    const GURL& url,
                    std::string* payload,
                    uint64_t* revision) {
    mojo::SyncCallRestrictions::ScopedAllowSyncCall allow;
    return filter->StartCosmetics(token, url, payload, revision);
  }
};
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
class Throttle : public DecisionThrottle {
 public:
  Throttle(mojo::PendingRemote<mojom::Filter> remote,
           std::optional<blink::LocalFrameToken> frame,
           std::string type)
      : pending_(std::move(remote)), frame_(frame), type_(std::move(type)) {}
  void DetachFromCurrentSequence() override {
    // Sync worker loads move the throttle to a loading sequence before any
    // Will* callback. Keep the pipe pending until Check() runs there; no Mojo
    // remote or weak callback has been bound to the creating sequence yet.
    DCHECK(!remote_.is_bound());
  }
  void WillStartRequest(network::ResourceRequest* request,
                        bool* defer) override {
    Capture(*request);
    source_ = request->request_initiator ? request->request_initiator->GetURL()
                                         : GURL();
    method_ = request->method;
    Check(request->url, defer);
  }
  void WillRedirectRequest(net::RedirectInfo* redirect,
                           const network::mojom::URLResponseHead&,
                           bool* defer,
                           network::HttpRequestHeadersUpdateParams*) override {
    Redirect(*redirect);
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
    remote_->Check(
        frame_, url, source_, type_, method_,
        mojo::WrapCallbackWithDefaultInvokeIfNotRun(
            base::BindOnce(
                [](base::WeakPtr<Throttle> self, mojom::DecisionPtr decision) {
                  if (self)
                    self->Resolve(std::move(decision));
                },
                weak_.GetWeakPtr()),
            mojom::Decision::New()));
  }
  mojo::PendingRemote<mojom::Filter> pending_;
  mojo::Remote<mojom::Filter> remote_;
  std::optional<blink::LocalFrameToken> frame_;
  std::string type_, method_;
  GURL source_;
  base::WeakPtrFactory<Throttle> weak_{this};
};
class CosmeticObserver
    : public content::RenderFrameObserver,
      public content::RenderFrameObserverTracker<CosmeticObserver> {
 public:
  explicit CosmeticObserver(content::RenderFrame* frame)
      : content::RenderFrameObserver(frame),
        content::RenderFrameObserverTracker<CosmeticObserver>(frame) {}
  void OnDestruct() override { delete this; }
  void DidCreateNewDocument() override {
    blink::WebIsolatedWorldInfo info;
    info.security_origin =
        blink::WebSecurityOrigin::Create(GURL("chrome://lumie-adblock"));
    info.content_security_policy = blink::WebString::FromUtf8("");
    blink::SetIsolatedWorldInfo(ISOLATED_WORLD_ID_LUMIE_ADBLOCK, info);
  }
  void DocumentStart() { Start(true); }
  void DidFinishSameDocumentNavigation() override { Start(false); }
  void DidCommitProvisionalLoad(ui::PageTransition) override {
    weak_.InvalidateWeakPtrs();
    remote_.reset();
    retry_.Stop();
    key_ = blink::WebString();
    css_.clear();
    scriptlets_injected_ = false;
  }
  void DidSetPageLifecycleState(blink::BFCacheStateChange change) override {
    if (change == blink::BFCacheStateChange::kStoredToBFCache) {
      // pagehide suspends the JS observer. Drop native callbacks too: executing
      // a new payload while cached can evict the page or mutate frozen state.
      weak_.InvalidateWeakPtrs();
      remote_.reset();
      retry_.Stop();
    } else if (change == blink::BFCacheStateChange::kRestoredFromBFCache) {
      Start(false);
    }
  }

 private:
  void Start(bool early) {
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
    if (early) {
      std::string payload;
      uint64_t revision = 0;
      const auto token = document.Token();
      if (EarlyCosmetics::Fetch(
              remote_.get(),
              render_frame()->GetWebFrame()->GetLocalFrameToken(), url,
              &payload, &revision)) {
        Apply(token, payload, revision);
        return;
      }
    }
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
             const std::string& payload,
             uint64_t revision) {
    if (!render_frame() ||
        render_frame()->GetWebFrame()->GetDocument().Token() != token)
      return;
    auto* frame = render_frame()->GetWebFrame();
    auto data = base::JSONReader::ReadDict(payload, base::JSON_PARSE_RFC);
    auto* resources = data ? data->FindDict("resources") : nullptr;
    auto self = weak_.GetWeakPtr();
    if (!scriptlets_injected_ && resources) {
      scriptlets_injected_ = true;
      if (auto* script = resources->FindString("injected_script");
          script && !script->empty()) {
        // Native execution targets the page's main world before its first
        // inline script. It creates no script node / TrustedHTML sink and
        // changes neither the site's CSP nor its Trusted Types policy.
        frame->ExecuteScript(blink::WebScriptSource(
            blink::WebString::FromUtf8("(()=>{const scriptletGlobals=new "
                                       "Map();const deAmpEnabled=false;\n" +
                                       *script + "\n})()")));
        if (!self || frame->GetDocument().Token() != token)
          return;
      }
    }
    std::string css =
        data && data->FindString("css") ? *data->FindString("css") : "";
    v8::Isolate* isolate = frame->GetAgentGroupScheduler()->Isolate();
    v8::HandleScope scope(isolate);
    std::string command;
    if (resources) {
      // JSON is serialized by the engine, never interpolated rule source.
      command = std::string(kAdblockRuntime) +
                ";globalThis.__lumieAdblock.set(" + payload + ", '" +
                token.ToString() + "')";
    } else {
      command = "globalThis.__lumieAdblock?.stop();''";
    }
    auto result = frame->ExecuteScriptInIsolatedWorldAndReturnValue(
        ISOLATED_WORLD_ID_LUMIE_ADBLOCK,
        blink::WebScriptSource(blink::WebString::FromUtf8(command)),
        blink::BackForwardCacheAware::kAllow);
    if (!self || frame->GetDocument().Token() != token)
      return;
    std::string procedural_css;
    if (!result.IsEmpty() &&
        gin::ConvertFromV8(isolate, result, &procedural_css))
      css += procedural_css;
    auto document = frame->GetDocument();
    if (css != css_) {
      if (!key_.IsEmpty())
        document.RemoveInsertedStyleSheet(key_, blink::WebCssOrigin::kUser);
      key_ = blink::WebString();
      css_ = css;
      if (!css.empty())
        key_ = document.InsertStyleSheet(blink::WebString::FromUtf8(css),
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
  bool scriptlets_injected_ = false;
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
  if (blink::IsRequestDestinationFrame(request.destination))
    return nullptr;
  if (pending_)
    remote_.Bind(std::move(pending_));
  if (!remote_.is_connected())
    return nullptr;
  mojo::PendingRemote<mojom::Filter> pending;
  remote_->Clone(pending.InitWithNewPipeAndPassReceiver());
  return std::make_unique<Throttle>(
      std::move(pending),
      frame.has_value() ? std::make_optional(frame.value()) : std::nullopt,
      ResourceType(request.destination));
}
void ObserveFrame(content::RenderFrame* frame) {
  new CosmeticObserver(frame);
}
bool RunDocumentStart(content::RenderFrame* frame) {
  if (auto* observer = CosmeticObserver::Get(frame))
    observer->DocumentStart();
  return CosmeticObserver::Get(frame) != nullptr;
}
}  // namespace chrome::ohos::adblock
