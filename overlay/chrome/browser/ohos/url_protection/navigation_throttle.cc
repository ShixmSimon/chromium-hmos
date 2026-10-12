// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "chrome/browser/ohos/url_protection/navigation_throttle.h"

#include <memory>
#include <utility>
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/strings/escape.h"
#include "base/strings/string_number_conversions.h"
#include "base/supports_user_data.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/ohos/url_protection/site_verdict_state.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/ohos/shell_services_ohos.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/common/webui_url_constants.h"
#include "components/security_interstitials/content/security_interstitial_controller_client.h"
#include "components/security_interstitials/content/security_interstitial_page.h"
#include "components/security_interstitials/content/security_interstitial_tab_helper.h"
#include "components/security_interstitials/content/settings_page_helper.h"
#include "components/security_interstitials/core/metrics_helper.h"
#include "components/sessions/content/session_tab_helper.h"
#include "content/public/browser/back_forward_cache.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"
#include "net/base/net_errors.h"
#include "url/origin.h"

namespace chrome::ohos::url_protection {
namespace {
constexpr char kBypassKey[] = "ohos.url_protection.one_navigation";
void ShowCommittedRisk(content::WebContents* contents,
                       const Result& risk,
                       base::WeakPtr<Service> service);
// Keep activation paths from bypassing the network-navigation throttle. This
// observer exists even while protection is off, so enabling it also evicts
// old BFCache entries and cancels already prepared prerenders in each tab.
class TabProtection : public content::WebContentsObserver,
                      public content::WebContentsUserData<TabProtection> {
 public:
  ~TabProtection() override { CancelCommittedCheck(); }
  void Begin(content::NavigationHandle* handle) {
    if (!handle->IsInPrimaryMainFrame() || handle->IsSameDocument())
      return;
    if (!state_.Begin(ToShellId(handle->GetNavigationId()), handle->GetURL(),
                      Revision(), IsEnabled(handle->GetURL())))
      return;
    CancelCommittedCheck();
    Notify("navigation_start");
  }
  void DidStartNavigation(content::NavigationHandle* handle) override {
    Begin(handle);
  }
  void DidRedirectNavigation(content::NavigationHandle* handle) override {
    if (IsCurrent(handle)) {
      state_.Redirect(handle->GetURL(), Revision(),
                      IsEnabled(handle->GetURL()));
      Notify("redirect");
    }
  }
  void DidFinishNavigation(content::NavigationHandle* handle) override {
    if (!handle->IsInPrimaryMainFrame())
      return;
    if (handle->IsSameDocument()) {
      // Same-document navigations do not run the network throttle. Wait until
      // commit; an aborted History API/hash navigation must not discard
      // results.
      if (!handle->HasCommitted())
        return;
      CancelCommittedCheck();
      state_.SameDocumentCommit(ToShellId(handle->GetNavigationId()),
                                handle->GetURL(), Revision(),
                                IsEnabled(handle->GetURL()));
      Notify("same_document_commit");
      CheckCommittedDocument();
      return;
    }
    if (!IsCurrent(handle))
      return;
    state_.Finish(handle->HasCommitted(), handle->GetURL());
    if (handle->HasCommitted()) {
      committed_error_page_ = handle->IsErrorPage();
      ProtectCurrentDocument();
    }
    Notify(handle->HasCommitted() ? "navigation_commit" : "navigation_abort");
    CheckCommittedDocument();
  }
  void Report(content::NavigationHandle* handle,
              const Result& result,
              bool checking) {
    if (!IsCurrent(handle))
      return;
    state_.Report(
        result, checking,
        IsEnabled(result.dangerous() ? result.url : state_.current().url),
        base::Time::Now().InMillisecondsFSinceUnixEpoch());
    Notify("navigation_result");
  }
  base::DictValue State() const {
    auto state = service_ ? service_->State() : base::DictValue();
    state.Set("tabId", TabId());
    state.Set("navigationId", state_.current().navigation_id);
    state.Set("origin",
              state_.current().url.SchemeIsHTTPOrHTTPS()
                  ? url::Origin::Create(state_.current().url).Serialize()
                  : "");
    state.Set("revision", service_ ? service_->revision() : 0);
    state.Set("siteEnabled",
              service_ && service_->IsSiteEnabled(state_.current().url));
    state.Set("canOverride", service_ && service_->CanSetSiteEnabled() &&
                                 !state_.current().navigation_id.empty() &&
                                 IsPublicHttpUrl(state_.current().url));
    state.Set("verdict", state_.current().verdict);
    state.Set("checkedAt", state_.current().checked_at);
    base::ListValue sources;
    for (const auto& id : state_.current().matched_source_ids)
      sources.Append(id);
    state.Set("matchedSourceIds", std::move(sources));
    state.Set("ruleType", state_.current().rule_type);
    return state;
  }
  const GURL& url() const { return state_.current().url; }
  std::string TabId() const {
    const auto id = sessions::SessionTabHelper::IdForTab(web_contents());
    return id.is_valid() ? base::NumberToString(id.id()) : std::string();
  }

 private:
  bool IsCurrent(content::NavigationHandle* handle) const {
    return handle->IsInPrimaryMainFrame() &&
           state_.IsCurrent(ToShellId(handle->GetNavigationId()));
  }
  int Revision() const { return service_ ? service_->revision() : 0; }
  bool IsEnabled(const GURL& url) const {
    return service_ && service_->enabled() && service_->IsSiteEnabled(url);
  }
  void Notify(const char* reason) {
    if (!service_)
      return;
    // Verbose opt-in diagnostics: identity/state only, never URL/origin.
    VLOG(1) << "OHOS site verdict reason=" << reason
            << " navigation=" << state_.current().navigation_id
            << " policy=" << state_.current().revision
            << " verdict=" << state_.current().verdict
            << " checkedAt=" << state_.current().checked_at
            << " pendingNavigation=" << state_.pending_navigation();
    auto event = service_->State();
    event.Set("event", "siteProtectionStateChanged");
    event.Set("tabId", TabId());
    event.Set("navigationId", state_.current().navigation_id);
    BroadcastToShell(
        Profile::FromBrowserContext(web_contents()->GetBrowserContext()),
        std::move(event));
  }
  void CancelCommittedCheck() {
    weak_factory_.InvalidateWeakPtrs();
    if (service_ && !committed_request_.empty())
      service_->Cancel(committed_request_);
    committed_request_.clear();
  }
  void CheckCommittedDocument() {
    const GURL& url = state_.current().url;
    if (!state_.NeedsCommittedCheck() || !committed_request_.empty() ||
        !IsEnabled(url) || !IsPublicHttpUrl(url) || committed_error_page_ ||
        url != web_contents()->GetLastCommittedURL())
      return;
    auto* interstitial =
        security_interstitials::SecurityInterstitialTabHelper::FromWebContents(
            web_contents());
    if (interstitial && interstitial->HasPendingOrActiveInterstitial())
      return;
    const uint64_t generation = state_.StartCommittedCheck();
    Notify("committed_check_start");
    committed_request_ = service_->Check(
        url, base::Milliseconds(1500),
        base::BindOnce(&TabProtection::OnCommittedResult,
                       weak_factory_.GetWeakPtr(), generation, Revision()));
  }
  void OnCommittedResult(uint64_t generation, int revision, Result result) {
    if (!service_ || revision != Revision() ||
        result.url != web_contents()->GetLastCommittedURL() ||
        !state_.CompleteCommittedCheck(
            generation, revision, result,
            base::Time::Now().InMillisecondsFSinceUnixEpoch()))
      return;
    committed_request_.clear();
    Notify("committed_check_result");
    if (result.dangerous()) {
      web_contents()->RemoveUserData(kBypassKey);
      ShowCommittedRisk(web_contents(), result, service_);
    }
  }
  friend class content::WebContentsUserData<TabProtection>;
  explicit TabProtection(content::WebContents* contents)
      : WebContentsObserver(contents),
        WebContentsUserData<TabProtection>(*contents),
        service_(Service::Get(
                     Profile::FromBrowserContext(contents->GetBrowserContext()))
                     ->GetWeakPtr()) {
    subscription_ = service_->Observe(
        base::BindRepeating(&TabProtection::OnPolicy, base::Unretained(this)));
    state_.Initialize(contents->GetLastCommittedURL(), Revision(),
                      IsEnabled(contents->GetLastCommittedURL()));
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
    CancelCommittedCheck();
    state_.PolicyChanged(Revision(), IsEnabled(state_.current().url),
                         IsEnabled(state_.committed().url));
    Notify("policy_change");
    if (service_ && service_->enabled()) {
      web_contents()->GetController().GetBackForwardCache().Flush();
      web_contents()->CancelAllPrerendering();
      ProtectCurrentDocument();
    }
    CheckCommittedDocument();
  }
  base::WeakPtr<Service> service_;
  base::CallbackListSubscription subscription_;
  SiteVerdictState state_;
  std::string committed_request_;
  bool committed_error_page_ = false;
  base::WeakPtrFactory<TabProtection> weak_factory_{this};
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
               base::WeakPtr<Service> service,
               bool post_commit = false)
      : SecurityInterstitialPage(
            contents,
            risk.url,
            std::make_unique<Controller>(contents,
                                         risk.url,
                                         can_proceed,
                                         service,
                                         service ? service->revision() : 0)),
        risk_(risk),
        can_proceed_(can_proceed),
        post_commit_(post_commit) {}
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
    data.Set("heading", malware ? "此网站可能传播恶意软件"
                        : risk_.verdict == Verdict::kPhishing
                            ? "此网站可能是钓鱼网站"
                            : "此网站存在安全风险");
    data.Set("primaryParagraph", "检测到 <strong>" +
                                     base::EscapeForHTML(risk_.url.host()) +
                                     "</strong> 存在风险，建议返回安全页面。");
    data.Set("primaryButtonText", "返回安全页面");
    data.Set("openDetails", "详细信息");
    data.Set("closeDetails", "隐藏详细信息");
    data.Set("explanationParagraph",
             risk_.source == "huawei" ? "检测来源：华为安全检测。"
             : risk_.matched_source_ids.empty()
                 ? "检测来源：Phishing.Database 本地规则。"
                 : "检测来源：已启用的本地威胁规则订阅，风险类型未分类。");
    data.Set("finalParagraph",
             can_proceed_
                 ? "如果了解风险，可以<a href=\"#\" "
                   "id=\"proceed-link\">仍要访问</a>。本次允许仅适用于此网址。"
             : post_commit_ ? "当前页面加载后检测到风险，请返回安全页面。"
                            : "此请求包含提交数据，无法从警告页自动重新提交。");
    data.Set("overridable", can_proceed_);
    data.Set("hide_primary_button", false);
    data.Set("disableKeyboardOverride", true);
    data.Set("billing", false);
    data.Set("optInLink", "");
    data.Set("enhancedProtectionMessage", "");
    data.Set("displaycheckbox", false);
    data.Set("displayEnhancedProtectionMessage", false);
    data.Set("phishing", risk_.verdict == Verdict::kPhishing);
  }

 private:
  Result risk_;
  bool can_proceed_;
  bool post_commit_;
};

void ShowCommittedRisk(content::WebContents* contents,
                       const Result& risk,
                       base::WeakPtr<Service> service) {
  // Use Chromium's post-commit interstitial navigation, without a network
  // request or automatic replay of a previously submitted POST.
  auto page =
      std::make_unique<BlockingPage>(contents, risk, false, service, true);
  auto handle = contents->GetController().LoadPostCommitErrorPage(
      contents->GetPrimaryMainFrame(), risk.url, page->GetHTMLContents());
  if (!handle)
    return;
  security_interstitials::SecurityInterstitialTabHelper::AssociateBlockingPage(
      handle.get(), std::move(page));
  auto* tab = TabProtection::FromWebContents(contents);
  tab->Begin(handle.get());
  tab->Report(handle.get(), risk, false);
}
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
  TabProtection::FromWebContents(navigation_handle()->GetWebContents())
      ->Begin(navigation_handle());
  if (service_ && service_->enabled() &&
      navigation_handle()->IsInPrerenderedMainFrame())
    return CANCEL_AND_IGNORE;
  deadline_ = base::TimeTicks::Now() + base::Milliseconds(1500);
  CheckCurrentUrl();
  ReportState();
  return risk_.dangerous() ? Block() : ThrottleCheckResult(PROCEED);
}
NavigationThrottle::ThrottleCheckResult
NavigationThrottle::WillRedirectRequest() {
  if (risk_.dangerous())
    return Block();
  CheckCurrentUrl();
  ReportState();
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
  latest_result_ = {Verdict::kUnknown, {}, "not_checked", url};
  if (!service_ || !service_->enabled())
    return;
  if (TakeBypass(navigation_handle()->GetWebContents(), url,
                 service_->revision())) {
    latest_result_.reason = "one_navigation_bypass";
    return;
  }
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
  if (!urls_.empty() && result.url == urls_.back())
    latest_result_ = result;
  if (result.dangerous() && !risk_.dangerous())
    risk_ = std::move(result);
  ReportState();
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
  latest_result_ = {};
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
  ReportState();
  if (deferred_ && !pending_) {
    deferred_ = false;
    Resume();
  }
}
void NavigationThrottle::ReportState() {
  TabProtection::FromWebContents(navigation_handle()->GetWebContents())
      ->Report(navigation_handle(), risk_.dangerous() ? risk_ : latest_result_,
               pending_ > 0);
}
NavigationThrottle::ThrottleCheckResult NavigationThrottle::Block() {
  ReportState();
  auto page = std::make_unique<BlockingPage>(
      navigation_handle()->GetWebContents(), risk_,
      !navigation_handle()->IsPost(), service_);
  auto html = page->GetHTMLContents();
  security_interstitials::SecurityInterstitialTabHelper::AssociateBlockingPage(
      navigation_handle(), std::move(page));
  return {CANCEL, net::ERR_BLOCKED_BY_CLIENT, std::move(html)};
}
bool HandleSiteCommand(const ShellCommandContext& context,
                       std::string_view name,
                       const base::DictValue& command) {
  if (name != "getSiteProtectionState" && name != "setSiteProtectionEnabled")
    return false;
  auto* contents =
      context.browser
          ? context.browser->GetTabStripModel()->GetActiveWebContents()
          : nullptr;
  auto event = Service::Get(context.profile)->State();
  std::string reason;
  TabProtection* tab = nullptr;
  if (!contents || contents->GetBrowserContext() != context.profile) {
    reason = "invalid_tab";
  } else {
    TabProtection::CreateForWebContents(contents);
    tab = TabProtection::FromWebContents(contents);
    event = tab->State();
    const auto* tab_id = command.FindString("tabId");
    if (!tab_id || tab->TabId().empty() || *tab_id != tab->TabId())
      reason = "invalid_tab";
  }
  if (reason.empty() && name == "setSiteProtectionEnabled") {
    auto matches = [&](const char* key) {
      const auto* supplied = command.FindString(key);
      const auto* actual = event.FindString(key);
      return supplied && actual && !actual->empty() && *supplied == *actual;
    };
    auto* service = Service::Get(context.profile);
    if (!matches("contextId"))
      reason = "invalid_context";
    else if (!matches("navigationId") || !matches("origin"))
      reason = "stale_navigation";
    else if (command.FindInt("expectedRevision") != service->revision())
      reason = "stale_policy";
    else if (!command.FindBool("siteEnabled"))
      reason = "invalid_request";
    else
      reason =
          service->SetSiteEnabled(tab->url(), *command.FindBool("siteEnabled"));
    event = tab->State();
  }
  event.Set("event", name == "getSiteProtectionState"
                         ? "siteProtectionState"
                         : "siteProtectionConfigResult");
  event.Set("requestId", ReadRequestId(command));
  event.Set("ok", reason.empty());
  event.Set("reason", reason);
  ReplyToShell(context, std::move(event));
  return true;
}
}  // namespace chrome::ohos::url_protection
