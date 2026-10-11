// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "chrome/browser/ohos/url_protection/navigation_throttle.h"

#include <memory>
#include <utility>
#include "base/functional/bind.h"
#include "base/strings/escape.h"
#include "base/strings/string_number_conversions.h"
#include "base/supports_user_data.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/common/webui_url_constants.h"
#include "components/security_interstitials/content/security_interstitial_controller_client.h"
#include "components/security_interstitials/content/security_interstitial_page.h"
#include "components/security_interstitials/content/security_interstitial_tab_helper.h"
#include "components/security_interstitials/content/settings_page_helper.h"
#include "components/security_interstitials/core/metrics_helper.h"
#include "content/public/browser/back_forward_cache.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"
#include "net/base/net_errors.h"

namespace chrome::ohos::url_protection {
namespace {
constexpr char kBypassKey[] = "ohos.url_protection.one_navigation";
// Keep activation paths from bypassing the network-navigation throttle. This
// observer exists even while protection is off, so enabling it also evicts
// old BFCache entries and cancels already prepared prerenders in each tab.
class TabProtection : public content::WebContentsObserver,
                      public content::WebContentsUserData<TabProtection> {
 public:
  ~TabProtection() override = default;
  void DidFinishNavigation(content::NavigationHandle* handle) override {
    if (handle->HasCommitted() && handle->IsInPrimaryMainFrame())
      ProtectCurrentDocument();
  }

 private:
  friend class content::WebContentsUserData<TabProtection>;
  explicit TabProtection(content::WebContents* contents)
      : WebContentsObserver(contents),
        WebContentsUserData<TabProtection>(*contents),
        service_(Service::Get(
                     Profile::FromBrowserContext(contents->GetBrowserContext()))
                     ->GetWeakPtr()) {
    subscription_ = service_->Observe(
        base::BindRepeating(&TabProtection::OnPolicy, base::Unretained(this)));
    ProtectCurrentDocument();
  }
  void ProtectCurrentDocument() {
    if (!service_ || !service_->enabled() ||
        !web_contents()->GetPrimaryMainFrame())
      return;
    content::BackForwardCache::DisableForRenderFrameHost(
        web_contents()->GetPrimaryMainFrame(),
        content::BackForwardCache::DisabledReason(
            content::BackForwardCache::DisabledSource::kEmbedder, 65001,
            "OHOS URL protection requires navigation checks", "",
            "ohos-url-protection"));
  }
  void OnPolicy() {
    web_contents()->RemoveUserData(kBypassKey);
    if (service_ && service_->enabled()) {
      web_contents()->GetController().GetBackForwardCache().Flush();
      web_contents()->CancelAllPrerendering();
      ProtectCurrentDocument();
    }
  }
  base::WeakPtr<Service> service_;
  base::CallbackListSubscription subscription_;
  WEB_CONTENTS_USER_DATA_KEY_DECL();
};
WEB_CONTENTS_USER_DATA_KEY_IMPL(TabProtection);
struct OneNavigationBypass : base::SupportsUserData::Data {
  GURL url;
  int revision = 0;
  base::TimeTicks expires;
};
bool TakeBypass(content::WebContents* contents, const GURL& url, int revision) {
  auto* bypass =
      static_cast<OneNavigationBypass*>(contents->GetUserData(kBypassKey));
  bool allowed = bypass && bypass->url == url && bypass->revision == revision &&
                 bypass->expires > base::TimeTicks::Now();
  // An unrelated navigation consumes the grant too; no host-wide exception.
  contents->RemoveUserData(kBypassKey);
  return allowed;
}
class Controller
    : public security_interstitials::SecurityInterstitialControllerClient {
 public:
  Controller(content::WebContents* contents,
             GURL url,
             bool can_proceed,
             base::WeakPtr<Service> service,
             int revision)
      : SecurityInterstitialControllerClient(
            contents,
            std::make_unique<security_interstitials::MetricsHelper>(
                GURL(),
                security_interstitials::MetricsHelper::ReportDetails(),
                nullptr),
            nullptr,
            g_browser_process->GetApplicationLocale(),
            chrome::ChromeUINewTabURLAsGURL(),
            nullptr),
        url_(std::move(url)),
        can_proceed_(can_proceed),
        service_(service),
        revision_(revision) {}
  void Proceed() override {
    if (!can_proceed_ || used_ || !service_)
      return;
    used_ = true;
    if (service_->revision() == revision_) {
      auto bypass = std::make_unique<OneNavigationBypass>();
      bypass->url = url_;
      bypass->revision = revision_;
      bypass->expires = base::TimeTicks::Now() + base::Seconds(10);
      web_contents()->SetUserData(kBypassKey, std::move(bypass));
    }
    // A stale page can retry, but the new policy checks it normally.
    // Explicit GET to the blocked hop, not Reload() of a different final URL
    // and never an automatic POST replay.
    OpenUrlInCurrentTab(url_);
  }

 private:
  GURL url_;
  bool can_proceed_;
  bool used_ = false;
  base::WeakPtr<Service> service_;
  int revision_;
};
class BlockingPage : public security_interstitials::SecurityInterstitialPage {
 public:
  BlockingPage(content::WebContents* contents,
               const Result& risk,
               bool can_proceed,
               base::WeakPtr<Service> service)
      : SecurityInterstitialPage(
            contents,
            risk.url,
            std::make_unique<Controller>(contents,
                                         risk.url,
                                         can_proceed,
                                         service,
                                         service ? service->revision() : 0)),
        risk_(risk),
        can_proceed_(can_proceed) {}
  void OnInterstitialClosing() override {}
  void CommandReceived(const std::string& command) override {
    int value = -1;
    if (!base::StringToInt(command, &value))
      return;
    if (value == security_interstitials::CMD_DONT_PROCEED)
      controller()->GoBackAfterNavigationCommitted();
    else if (value == security_interstitials::CMD_PROCEED && can_proceed_)
      controller()->Proceed();
  }

 protected:
  void PopulateInterstitialStrings(base::DictValue& data) override {
    const bool malware = risk_.verdict == Verdict::kMalware;
    data.Set("type", "SAFEBROWSING");
    data.Set("tabTitle", "网站安全警告");
    data.Set("heading",
             malware ? "此网站可能传播恶意软件" : "此网站可能是钓鱼网站");
    data.Set("primaryParagraph", "检测到 <strong>" +
                                     base::EscapeForHTML(risk_.url.host()) +
                                     "</strong> 存在风险，建议返回安全页面。");
    data.Set("primaryButtonText", "返回安全页面");
    data.Set("openDetails", "详细信息");
    data.Set("closeDetails", "隐藏详细信息");
    data.Set("explanationParagraph",
             risk_.source == "huawei"
                 ? "检测来源：华为安全检测。"
                 : "检测来源：Phishing.Database 本地规则。");
    data.Set("finalParagraph",
             can_proceed_
                 ? "如果了解风险，可以<a href=\"#\" "
                   "id=\"proceed-link\">仍要访问</a>。本次允许仅适用于此网址。"
                 : "此请求包含提交数据，无法从警告页自动重新提交。");
    data.Set("overridable", can_proceed_);
    data.Set("hide_primary_button", false);
    data.Set("disableKeyboardOverride", true);
    data.Set("billing", false);
    data.Set("optInLink", "");
    data.Set("enhancedProtectionMessage", "");
    data.Set("displaycheckbox", false);
    data.Set("displayEnhancedProtectionMessage", false);
    data.Set("phishing", !malware);
  }

 private:
  Result risk_;
  bool can_proceed_;
};
}  // namespace

void NavigationThrottle::MaybeCreateAndAdd(
    content::NavigationThrottleRegistry& registry) {
  auto& handle = registry.GetNavigationHandle();
  if (!handle.IsInMainFrame())
    return;
  TabProtection::CreateForWebContents(handle.GetWebContents());
  registry.AddThrottle(std::make_unique<NavigationThrottle>(registry));
}
NavigationThrottle::NavigationThrottle(
    content::NavigationThrottleRegistry& registry)
    : content::NavigationThrottle(registry),
      service_(Service::Get(
                   Profile::FromBrowserContext(registry.GetNavigationHandle()
                                                   .GetWebContents()
                                                   ->GetBrowserContext()))
                   ->GetWeakPtr()) {
  subscription_ = service_->Observe(base::BindRepeating(
      &NavigationThrottle::OnPolicyChanged, base::Unretained(this)));
}
NavigationThrottle::~NavigationThrottle() {
  if (service_) {
    for (const auto& id : requests_)
      service_->Cancel(id);
  }
}
const char* NavigationThrottle::GetNameForLogging() {
  return "OhosUrlProtection";
}

NavigationThrottle::ThrottleCheckResult NavigationThrottle::WillStartRequest() {
  if (service_ && service_->enabled() &&
      navigation_handle()->IsInPrerenderedMainFrame())
    return CANCEL_AND_IGNORE;
  deadline_ = base::TimeTicks::Now() + base::Milliseconds(1500);
  CheckCurrentUrl();
  return risk_.dangerous() ? Block() : ThrottleCheckResult(PROCEED);
}
NavigationThrottle::ThrottleCheckResult
NavigationThrottle::WillRedirectRequest() {
  if (risk_.dangerous())
    return Block();
  CheckCurrentUrl();
  return risk_.dangerous() ? Block() : ThrottleCheckResult(PROCEED);
}
NavigationThrottle::ThrottleCheckResult
NavigationThrottle::WillProcessResponse() {
  if (risk_.dangerous())
    return Block();
  if (pending_ > 0) {
    deferred_ = true;
    return DEFER;
  }
  return PROCEED;
}
void NavigationThrottle::CheckCurrentUrl() {
  const GURL url = navigation_handle()->GetURL();
  urls_.push_back(url);
  if (!service_ || !service_->enabled())
    return;
  if (TakeBypass(navigation_handle()->GetWebContents(), url,
                 service_->revision()))
    return;
  Result known = service_->KnownRisk(url);
  if (known.dangerous()) {
    risk_ = std::move(known);
    return;
  }
  ++pending_;
  requests_.push_back(service_->Check(
      url, deadline_ - base::TimeTicks::Now(),
      base::BindOnce(&NavigationThrottle::OnResult, weak_factory_.GetWeakPtr(),
                     service_->revision())));
}
void NavigationThrottle::OnResult(int revision, Result result) {
  if (!service_ || revision != service_->revision())
    return;
  --pending_;
  if (result.dangerous() && !risk_.dangerous())
    risk_ = std::move(result);
  if (deferred_ && (risk_.dangerous() || pending_ == 0)) {
    deferred_ = false;
    if (risk_.dangerous()) {
      auto blocked = Block();
      CancelDeferredNavigation(blocked);
    } else {
      Resume();
    }
  }
}
void NavigationThrottle::OnPolicyChanged() {
  weak_factory_.InvalidateWeakPtrs();
  pending_ = 0;
  risk_ = {};
  navigation_handle()->GetWebContents()->RemoveUserData(kBypassKey);
  // A changed policy invalidates every old result. Recheck all observed hops
  // under the remaining navigation budget, including the current URL.
  const auto urls = urls_;
  if (service_ && service_->enabled()) {
    for (const auto& url : urls) {
      ++pending_;
      requests_.push_back(service_->Check(
          url, deadline_ - base::TimeTicks::Now(),
          base::BindOnce(&NavigationThrottle::OnResult,
                         weak_factory_.GetWeakPtr(), service_->revision())));
    }
  }
  if (deferred_ && !pending_) {
    deferred_ = false;
    Resume();
  }
}
NavigationThrottle::ThrottleCheckResult NavigationThrottle::Block() {
  auto page = std::make_unique<BlockingPage>(
      navigation_handle()->GetWebContents(), risk_,
      !navigation_handle()->IsPost(), service_);
  auto html = page->GetHTMLContents();
  security_interstitials::SecurityInterstitialTabHelper::AssociateBlockingPage(
      navigation_handle(), std::move(page));
  return {CANCEL, net::ERR_BLOCKED_BY_CLIENT, std::move(html)};
}
}  // namespace chrome::ohos::url_protection
