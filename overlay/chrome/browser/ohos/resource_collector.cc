// Copyright 2026 The Chromium Authors. BSD-style license in LICENSE.
#include "chrome/browser/ohos/resource_collector.h"

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "base/strings/utf_string_conversions.h"
#include "base/strings/string_number_conversions.h"
#include "base/timer/timer.h"
#include "base/unguessable_token.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/ohos/shell_services_ohos.h"
#include "chrome/browser/ui/ohos/aura_shell_runtime_bridge.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/common/ohos/resource_policy.h"
#include "components/download/public/common/download_interrupt_reasons.h"
#include "components/download/public/common/download_item.h"
#include "components/download/public/common/download_url_parameters.h"
#include "components/sessions/content/session_tab_helper.h"
#include "content/public/browser/document_service.h"
#include "content/public/browser/download_manager.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/page.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/weak_document_ptr.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"
#include "net/http/http_content_disposition.h"
#include "net/http/http_util.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "url/gurl.h"

namespace chrome::ohos::resources {
namespace {
constexpr size_t kMaxEntries = 128;
constexpr size_t kMaxImages = 32;
constexpr size_t kMaxMetadataBytes = 512 * 1024;
std::u16string& ResourceMenuLabel() {
  static base::NoDestructor<std::u16string> label;
  return *label;
}
constexpr net::NetworkTrafficAnnotationTag kDownloadAnnotation =
    net::DefineNetworkTrafficAnnotation("ohos_detected_resource_download", R"(
      semantics {
        sender: "Lumie resource list"
        description: "Download a resource observed by the current page."
        trigger: "The user selects Download for a detected resource."
        data: "The resource URL and its original request context."
        destination: WEBSITE
      }
      policy {
        cookies_allowed: YES
        cookies_store: "user"
        setting: "Only triggered by an explicit user action."
        policy_exception_justification: "Not implemented."
      })");

struct Entry {
  std::string id, kind, name;
  adblock::mojom::ResourceResponsePtr response;
  content::WeakDocumentPtr document;
  int64_t total = -1;
  size_t metadata_bytes = 0;
  bool downloadable() const { return kind != "hls" && kind != "dash"; }
  base::DictValue Value() const {
    return base::DictValue().Set("id", id).Set("kind", kind).Set("name", name)
        .Set("url", response->url.spec()).Set("mimeType", response->mime_type)
        .Set("totalBytes", static_cast<double>(total))
        .Set("downloadable", downloadable());
  }
};

class PageResources : public content::WebContentsObserver,
                      public content::WebContentsUserData<PageResources> {
 public:
  static PageResources* Get(content::WebContents* contents) {
    CreateForWebContents(contents);
    return FromWebContents(contents);
  }
  std::string TabId() const {
    return base::NumberToString(sessions::SessionTabHelper::IdForTab(web_contents()).id());
  }
  const std::string& navigation() const { return navigation_; }
  void Record(content::RenderFrameHost& frame, adblock::mojom::ResourceResponsePtr r) {
    if (!frame.IsActive() || frame.GetOutermostMainFrame() != web_contents()->GetPrimaryMainFrame() ||
        !web_contents()->GetLastCommittedURL().SchemeIsHTTPOrHTTPS() ||
        !r->url.is_valid() || !r->url.SchemeIsHTTPOrHTTPS() ||
        r->url.has_username() || r->url.has_password() || r->url.spec().size() > 8192 ||
        r->mime_type.size() > 256 || r->disposition.size() > 2048 ||
        r->authorization.size() > 8192 || r->referrer.spec().size() > 8192 ||
        (!r->referrer.is_empty() && !r->referrer.SchemeIsHTTPOrHTTPS()) ||
        r->content_range.size() > 128 || r->referrer_policy < 0 ||
        r->referrer_policy > static_cast<int32_t>(net::ReferrerPolicy::MAX) ||
        !net::HttpUtil::IsValidHeaderValue(r->authorization) ||
        r->mode == network::mojom::RequestMode::kNavigate) return;
    auto kind = Classify(r->url.path(), r->mime_type, r->disposition, r->status, r->method);
    if (kind.empty()) return;
    Prune();
    r->url = r->url.GetWithoutRef();
    const auto origin = frame.GetLastCommittedOrigin().Serialize();
    const std::string key = origin + "\n" + r->url.spec();
    auto found = entries_.find(key);
    // A URL can change from a direct file to a manifest within one document.
    // Retire its old capability instead of retaining a stale Download button.
    if (found != entries_.end() && found->second.kind != kind) {
      metadata_bytes_ -= found->second.metadata_bytes;
      if (found->second.kind == "image") --images_;
      entries_.erase(found);
      found = entries_.end();
      Changed();
    }
    const size_t bytes = key.size() + r->url.spec().size() + r->mime_type.size() +
        r->disposition.size() + r->referrer.spec().size() + r->authorization.size() + 512;
    if (found != entries_.end()) {
      auto& entry = found->second;
      if (metadata_bytes_ - entry.metadata_bytes + bytes > kMaxMetadataBytes) return;
      metadata_bytes_ = metadata_bytes_ - entry.metadata_bytes + bytes;
      entry.metadata_bytes = bytes;
      const auto total = TotalBytes(r->status, r->content_length, r->content_range);
      // Keep a valid frame/request context after an iframe reload, including
      // refreshed authorization. Unknown partial totals never erase known ones.
      const bool changed = (total >= 0 && entry.total != total) ||
          entry.document.AsRenderFrameHostIfValid() != &frame;
      if (total >= 0) entry.total = total;
      entry.document = frame.GetWeakDocumentPtr();
      entry.response = std::move(r);
      if (changed) Changed();
      return;
    }
    if (entries_.size() >= kMaxEntries || metadata_bytes_ + bytes > kMaxMetadataBytes ||
        (kind == "image" && images_ >= kMaxImages)) {
      if (!truncated_) { truncated_ = true; Changed(); }
      return;
    }
    Entry entry;
    entry.id = base::UnguessableToken::Create().ToString();
    entry.kind = std::move(kind);
    entry.name = net::HttpContentDisposition(r->disposition, "UTF-8").filename();
    if (entry.name.empty()) entry.name = r->url.ExtractFileName();
    if (entry.name.empty()) entry.name = r->url.host();
    entry.name = base::UTF16ToUTF8(base::UTF8ToUTF16(entry.name).substr(0, 128));
    entry.total = TotalBytes(r->status, r->content_length, r->content_range);
    entry.metadata_bytes = bytes;
    entry.document = frame.GetWeakDocumentPtr();
    entry.response = std::move(r);
    if (entry.kind == "image") ++images_;
    metadata_bytes_ += bytes;
    entries_.emplace(key, std::move(entry));
    Changed();
  }
  base::ListValue List() const {
    base::ListValue list;
    for (const auto& [key, entry] : entries_)
      if (entry.document.AsRenderFrameHostIfValid()) list.Append(entry.Value());
    return list;
  }
  Entry* Find(const std::string& id) {
    for (auto& [key, entry] : entries_)
      if (entry.id == id) return &entry;
    return nullptr;
  }
  void Clear() {
    entries_.clear(); metadata_bytes_ = 0; images_ = 0; truncated_ = false;
    navigation_ = base::UnguessableToken::Create().ToString();
    Changed();
  }
  bool truncated() const { return truncated_; }
  void PrimaryPageChanged(content::Page&) override { Clear(); }
  void DidFinishNavigation(content::NavigationHandle* handle) override {
    if (handle->HasCommitted() && !handle->IsSameDocument()) Prune();
  }
  void RenderFrameDeleted(content::RenderFrameHost* frame) override { Prune(frame); }

 private:
  explicit PageResources(content::WebContents* contents)
      : content::WebContentsObserver(contents), content::WebContentsUserData<PageResources>(*contents) {}
  friend class content::WebContentsUserData<PageResources>;
  void Prune(content::RenderFrameHost* removed = nullptr) {
    bool changed = false;
    for (auto it = entries_.begin(); it != entries_.end();) {
      auto* frame = it->second.document.AsRenderFrameHostIfValid();
      if (!frame || frame == removed) {
        metadata_bytes_ -= it->second.metadata_bytes;
        if (it->second.kind == "image") --images_;
        it = entries_.erase(it);
        changed = true;
      } else { ++it; }
    }
    if (changed) Changed();
  }
  void Changed() {
    if (!notification_.IsRunning()) notification_.Start(FROM_HERE, base::Milliseconds(500),
        base::BindOnce(&PageResources::Notify, base::Unretained(this)));
  }
  void Notify() {
    base::DictValue event;
    event.Set("event", "resourcesChanged");
    event.Set("tabId", TabId()); event.Set("navigationId", navigation_);
    BroadcastToShell(Profile::FromBrowserContext(web_contents()->GetBrowserContext()), std::move(event));
  }
  std::map<std::string, Entry> entries_;
  std::string navigation_ = base::UnguessableToken::Create().ToString();
  size_t metadata_bytes_ = 0, images_ = 0;
  bool truncated_ = false;
  base::OneShotTimer notification_;
  WEB_CONTENTS_USER_DATA_KEY_DECL();
};
WEB_CONTENTS_USER_DATA_KEY_IMPL(PageResources);

class Observer : public content::DocumentService<adblock::mojom::ResourceObserver> {
 public:
  Observer(content::RenderFrameHost& frame,
           mojo::PendingReceiver<adblock::mojom::ResourceObserver> receiver)
      : DocumentService(frame, std::move(receiver)) {}
  void Record(adblock::mojom::ResourceResponsePtr response) override {
    auto& frame = render_frame_host();
    auto* contents = content::WebContents::FromRenderFrameHost(&frame);
    if (contents) PageResources::Get(contents)->Record(frame, std::move(response));
  }
};
}  // namespace

void Bind(content::RenderFrameHost* frame,
          mojo::PendingReceiver<adblock::mojom::ResourceObserver> receiver) {
  if (frame) new Observer(*frame, std::move(receiver));
}

std::u16string MenuLabel() { return ResourceMenuLabel(); }

void RequestOpen(BrowserWindowInterface* browser) {
  auto* contents = browser ? browser->GetTabStripModel()->GetActiveWebContents() : nullptr;
  if (!contents) return;
  base::DictValue event;
  event.Set("event", "resourcesRequested");
  DispatchAuraShellRuntimeEvent(contents, std::move(event));
}

bool HandleCommand(const ShellCommandContext& context, std::string_view name,
                   const base::DictValue& command) {
  if (name == "setResourceMenuLabel") {
    if (const auto* label = command.FindString("name"); label && label->size() <= 160)
      ResourceMenuLabel() = base::UTF8ToUTF16(*label);
    return true;
  }
  if (name != "listResources" && name != "downloadResource" && name != "clearResources") return false;
  auto* contents = context.browser ? context.browser->GetTabStripModel()->GetActiveWebContents() : nullptr;
  auto* page = contents ? PageResources::Get(contents) : nullptr;
  const auto* tab = command.FindString("tabId");
  base::DictValue event;
  event.Set("event", name == "listResources" ? "resourceList" : "resourceActionResult");
  event.Set("requestId", ReadRequestId(command));
  event.Set("ok", false);
  if (!page || !tab || *tab != page->TabId()) {
    event.Set("reason", "staleTab"); ReplyToShell(context, std::move(event)); return true;
  }
  event.Set("tabId", page->TabId()); event.Set("navigationId", page->navigation());
  event.Set("protocolVersion", 1);
  if (name == "listResources") {
    event.Set("resources", page->List()); event.Set("truncated", page->truncated());
    event.Set("ok", true);
  } else if (auto* nav = command.FindString("navigationId"); !nav || *nav != page->navigation()) {
    event.Set("reason", "staleDocument");
  } else if (name == "clearResources") {
    page->Clear(); event.Set("navigationId", page->navigation()); event.Set("ok", true);
  } else {
    auto* id = command.FindString("id");
    auto* entry = id ? page->Find(*id) : nullptr;
    auto* frame = entry ? entry->document.AsRenderFrameHostIfValid() : nullptr;
    if (!entry || !frame || !frame->IsActive() ||
        frame->GetOutermostMainFrame() != contents->GetPrimaryMainFrame()) {
      event.Set("reason", "staleResource");
    } else if (!entry->downloadable()) {
      event.Set("reason", "manifestOnly");
    } else {
      // Frame association preserves Chromium's download security checks and
      // storage partition. Never accept a URL/path/headers from the shell.
      const auto& r = entry->response;
      auto params = frame->CreateDownloadUrlParameters(r->url, kDownloadAnnotation);
      params->set_content_initiated(false);
      params->set_has_user_gesture(true);
      params->set_referrer(r->referrer);
      params->set_referrer_policy(static_cast<net::ReferrerPolicy>(r->referrer_policy));
      params->set_credentials_mode(r->credentials_mode);
      if (!r->authorization.empty()) params->add_request_header("Authorization", r->authorization);
      params->set_callback(base::BindOnce(
          [](content::WeakDocumentPtr document, gfx::AcceleratedWidget widget,
             base::DictValue result, download::DownloadItem* item,
             download::DownloadInterruptReason reason) {
            auto* source = document.AsRenderFrameHostIfValid();
            if (!source) return;
            const bool ok = item && reason == download::DOWNLOAD_INTERRUPT_REASON_NONE;
            result.Set("ok", ok);
            result.Set("reason", ok ? "requested" : "downloadRejected");
            ShellCommandContext reply;
            reply.widget = widget;
            reply.profile = Profile::FromBrowserContext(source->GetBrowserContext());
            ReplyToShell(reply, std::move(result));
          }, frame->GetWeakDocumentPtr(), context.widget, std::move(event)));
      // Do not replay Range: request the full file, retaining signed URL bytes.
      context.profile->GetDownloadManager()->DownloadUrl(std::move(params));
      return true;
    }
  }
  ReplyToShell(context, std::move(event));
  return true;
}
}  // namespace chrome::ohos::resources
