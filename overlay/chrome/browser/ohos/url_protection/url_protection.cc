// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "chrome/browser/ohos/url_protection/url_protection.h"

#include <algorithm>
#include <map>
#include <set>
#include <utility>
#include "base/base_paths.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/memory/raw_ptr.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/strings/string_util.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/unguessable_token.h"
#include "chrome/browser/ohos/url_protection/local_rules.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/ohos/shell_services_ohos.h"
#include "components/ohos_system_service/system_service_ohos.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/pref_service.h"
#include "net/base/ip_address.h"
#include "url/origin.h"

namespace chrome::ohos::url_protection {
namespace {
constexpr char kConfigPref[] = "ohos.url_protection";
constexpr auto kBudget = base::Milliseconds(1500);
Result Unknown(const GURL& url, std::string reason) {
  return {Verdict::kUnknown, {}, std::move(reason), url};
}
bool ValidProvider(std::string_view value) {
  return value == "local" || value == "huawei" || value == "both";
}
void PostResult(Service::Callback callback, Result result) {
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(std::move(callback), std::move(result)));
}
}  // namespace

bool IsPublicHttpUrl(const GURL& url) {
  if (!url.is_valid() || !url.SchemeIsHTTPOrHTTPS() || url.has_username() ||
      url.has_password() || url.spec().size() > 4096)
    return false;
  std::string host = url.HostNoBrackets();
  while (host.ends_with('.'))
    host.pop_back();
  net::IPAddress address;
  if (address.AssignFromIPLiteral(host)) {
    if (address.IsIPv4MappedIPv6())
      address = net::ConvertIPv4MappedIPv6ToIPv4(address);
    return address.IsPubliclyRoutable();
  }
  return host.find('.') != std::string::npos && host != "localhost" &&
         !host.ends_with(".localhost") && !host.ends_with(".local") &&
         !host.ends_with(".internal") && !host.ends_with(".home.arpa");
}

GURL CloudCheckUrl(const GURL& url) {
  if (!IsPublicHttpUrl(url))
    return {};
  // Conservative v1: paths with encoded or token-like segments stay local.
  // No promise to recognize every sensitive path or split-DNS hostname.
  const std::string path = base::ToLowerASCII(url.path());
  if (path.find('%') != std::string::npos ||
      path.find('@') != std::string::npos ||
      path.find("token") != std::string::npos ||
      path.find("oauth") != std::string::npos || path.size() > 256)
    return {};
  size_t segment = 0;
  for (char c : path) {
    segment = c == '/' ? 0 : segment + 1;
    if (segment > 48)
      return {};
  }
  GURL::Replacements replacements;
  replacements.ClearQuery();
  replacements.ClearRef();
  return url.ReplaceComponents(replacements);
}

void RegisterPrefs(PrefRegistrySimple* registry) {
  // Empty means not opted in. Legacy standard/enhanced is not cloud consent.
  registry->RegisterDictionaryPref(kConfigPref);
}

class Service::Impl {
 public:
  explicit Impl(Profile* profile) : profile_(profile) {
    const auto& config = profile_->GetPrefs()->GetDict(kConfigPref);
    enabled_ = config.FindBool("enabled").value_or(false);
    const auto* provider = config.FindString("provider");
    if (provider && ValidProvider(*provider))
      provider_ = *provider;
    // OTR never inherits ordinary-profile exceptions and never persists them.
    if (!profile_->IsOffTheRecord()) {
      if (const auto* exceptions = config.FindList("siteExceptions")) {
        for (const auto& item : *exceptions) {
          if (!item.is_string() || site_exceptions_.size() >= 1000)
            continue;
          GURL url(item.GetString());
          if (IsPublicHttpUrl(url) &&
              url::Origin::Create(url).Serialize() == item.GetString())
            site_exceptions_.insert(item.GetString());
        }
      }
    }
    // Cloud authorization is deliberately session-only, including OTR.
    // The shell reasserts its verified consent after reading this context.
    if (UsesLocal())
      EnsureRules();
  }
  ~Impl() {
    CancelAll("context_closed");
    SendControl("closeContext", {});
  }

  struct Pending {
    GURL url;
    GURL checked_url;
    Callback callback;
    Result result;
    bool waiting_local = false;
    bool waiting_cloud = false;
    int bridge_id = 0;
    base::TimeTicks deadline;
  };
  struct Cached {
    Result result;
    base::TimeTicks expires;
  };

  bool UsesLocal() const { return enabled_ && provider_ != "huawei"; }
  bool IsSiteEnabled(const GURL& url) const {
    return !site_exceptions_.contains(url::Origin::Create(url).Serialize());
  }
  bool CanSetSiteEnabled() const {
    return enabled_ &&
           (profile_->IsOffTheRecord() ||
            profile_->GetPrefs()->IsUserModifiablePreference(kConfigPref));
  }
  void Persist() {
    if (profile_->IsOffTheRecord())
      return;
    base::ListValue exceptions;
    for (const auto& origin : site_exceptions_)
      exceptions.Append(origin);
    profile_->GetPrefs()->SetDict(
        kConfigPref, base::DictValue()
                         .Set("enabled", enabled_)
                         .Set("provider", provider_)
                         .Set("siteExceptions", std::move(exceptions)));
  }
  std::string SetSiteEnabled(const GURL& url, bool enabled) {
    if (!IsPublicHttpUrl(url))
      return "invalid_origin";
    if (!CanSetSiteEnabled())
      return "disabled_or_managed";
    const std::string origin = url::Origin::Create(url).Serialize();
    if (!enabled && !site_exceptions_.contains(origin) &&
        site_exceptions_.size() >= 1000)
      return "exception_limit";
    CancelAll("stale_policy");
    ++revision_;
    if (enabled)
      site_exceptions_.erase(origin);
    else
      site_exceptions_.insert(origin);
    cache_.clear();
    Persist();
    SendControl("configure", State());
    changed_.Notify();
    Broadcast();
    return {};
  }
  bool UsesCloud() const {
    return enabled_ && provider_ != "local" && consent_;
  }
  base::DictValue Identity() const {
    return base::DictValue()
        .Set("protocolVersion", 1)
        .Set("contextId", context_id_)
        .Set("policyRevision", revision_);
  }
  base::DictValue State() const {
    auto state = Identity();
    state.Set("enabled", enabled_);
    state.Set("provider", provider_);
    state.Set("cloudConsent", consent_);
    state.Set("huaweiReason", cloud_reason_);
    state.Set("incognito", profile_->IsOffTheRecord());
    state.Set("huaweiStatus", !enabled_ || provider_ == "local" ? "disabled"
                              : !consent_ ? "consent_required"
                                          : cloud_status_);
    state.Set("localStatus",
              !UsesLocal() ? "disabled"
              : rules_.status == "ready" && !rules_.IsUsable(base::Time::Now())
                  ? "expired"
                  : rules_.status);
    state.Set("rulesVersion", rules_.version);
    state.Set("rulesUpdatedAt",
              rules_.generated.is_null()
                  ? -1.0
                  : rules_.generated.InMillisecondsFSinceUnixEpoch());
    state.Set("rulesExpiresAt",
              rules_.expires.is_null()
                  ? -1.0
                  : rules_.expires.InMillisecondsFSinceUnixEpoch());
    state.Set("rulesUpdateSupported", false);
    return state;
  }
  void Broadcast() {
    auto event = State();
    event.Set("event", "urlProtectionStateChanged");
    BroadcastToShell(profile_, std::move(event));
  }
  void SendControl(std::string_view method, base::DictValue args) {
    args.Merge(Identity());
    ohos_system_service::CallWithTimeout(
        "urlcheck", method, std::move(args), kBudget,
        base::BindOnce([](ohos_system_service::Reply) {}));
  }
  void CancelAll(std::string reason) {
    auto pending = std::move(pending_);
    pending_.clear();
    for (auto& [id, call] : pending) {
      if (call.bridge_id) {
        ohos_system_service::CancelCall(call.bridge_id);
        SendControl("cancel", base::DictValue().Set("requestId", id));
      }
      PostResult(std::move(call.callback), Unknown(call.url, reason));
    }
  }
  std::string Configure(const base::DictValue& command) {
    const auto* context_id = command.FindString("contextId");
    if (!context_id || *context_id != context_id_)
      return "invalid_context";
    if (command.FindInt("expectedRevision") != revision_)
      return "stale_policy";
    auto enabled = command.FindBool("enabled");
    auto consent = command.FindBool("cloudConsent");
    const auto* provider = command.FindString("provider");
    if (!enabled || !consent || !provider || !ValidProvider(*provider))
      return "invalid_request";
    if (!profile_->IsOffTheRecord() &&
        !profile_->GetPrefs()->IsUserModifiablePreference(kConfigPref))
      return "managed";
    CancelAll("stale_policy");
    ++revision_;
    enabled_ = *enabled;
    provider_ = *provider;
    consent_ = enabled_ && provider_ != "local" && *consent;
    cache_.clear();
    cloud_status_ = "unverified";
    cloud_reason_.clear();
    Persist();
    if (UsesLocal())
      EnsureRules();
    SendControl("configure", State());
    changed_.Notify();
    Broadcast();
    return {};
  }
  void EnsureRules() {
    if (rules_load_attempted_)
      return;
    rules_load_attempted_ = true;
    loading_ = true;
    rules_.status = "loading";
    base::FilePath assets;
    if (!base::PathService::Get(base::DIR_ASSETS, &assets)) {
      OnRules({});
      return;
    }
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(&LoadLocalRules,
                       assets.AppendASCII("ohos_phishing_rules.json")),
        base::BindOnce(&Impl::OnRules, weak_factory_.GetWeakPtr()));
  }
  void OnRules(LocalRules rules) {
    loading_ = false;
    rules_ = std::move(rules);
    std::vector<std::string> ids;
    for (auto& [id, call] : pending_) {
      if (call.waiting_local) {
        call.waiting_local = false;
        CheckLocal(call);
        ids.push_back(id);
      }
    }
    for (const auto& id : ids)
      MaybeComplete(id);
    Broadcast();
  }
  void CheckLocal(Pending& call) {
    if (!UsesLocal())
      return;
    if (rules_.Matches(call.url, base::Time::Now())) {
      call.result = {Verdict::kPhishing, "local", "matched", call.url};
    } else if (!call.result.dangerous() && rules_.IsUsable(base::Time::Now())) {
      call.result = {Verdict::kNoMatch, "local", "no_match", call.url};
    }
  }
  Result KnownRisk(const GURL& url) const {
    if (!IsSiteEnabled(url))
      return Unknown(url, "site_disabled");
    if (UsesLocal() && rules_.Matches(url, base::Time::Now()))
      return {Verdict::kPhishing, "local", "matched", url};
    if (UsesCloud()) {
      auto found = cache_.find(CloudCheckUrl(url).spec());
      if (found != cache_.end() &&
          found->second.expires > base::TimeTicks::Now() &&
          found->second.result.dangerous()) {
        Result result = found->second.result;
        result.url = url;
        return result;
      }
    }
    return Unknown(url, "not_checked");
  }
  std::string Check(const GURL& url,
                    base::TimeDelta timeout,
                    Callback callback) {
    if (!IsSiteEnabled(url)) {
      PostResult(std::move(callback), Unknown(url, "site_disabled"));
      return {};
    }
    Result known = KnownRisk(url);
    if (known.dangerous()) {
      PostResult(std::move(callback), std::move(known));
      return {};
    }
    if (!enabled_ || !IsPublicHttpUrl(url) || timeout <= base::TimeDelta() ||
        pending_.size() >= 128) {
      PostResult(std::move(callback), Unknown(url, "not_checked"));
      return {};
    }
    timeout = std::min(timeout, kBudget);
    const auto now = base::TimeTicks::Now();
    const std::string id = base::UnguessableToken::Create().ToString();
    Pending call;
    call.url = url;
    call.callback = std::move(callback);
    call.result = Unknown(url, "unavailable");
    call.deadline = now + timeout;
    if (UsesLocal()) {
      EnsureRules();
      call.waiting_local = loading_;
      if (!loading_)
        CheckLocal(call);
    }
    call.checked_url = CloudCheckUrl(url);
    if (!call.result.dangerous() && UsesCloud() &&
        call.checked_url.is_valid()) {
      auto cached = cache_.find(call.checked_url.spec());
      if (cached != cache_.end() && cached->second.expires > now) {
        call.result = cached->second.result;
        call.result.url = url;
      } else {
        call.waiting_cloud = true;
      }
    }
    pending_.emplace(id, std::move(call));
    auto& inserted = pending_.at(id);
    if (inserted.waiting_cloud) {
      auto args = Identity();
      args.Set("requestId", id);
      args.Set("url", inserted.checked_url.spec());
      args.Set("queryMode", "origin_path");
      args.Set("timeoutMs", static_cast<int>(timeout.InMilliseconds()));
      inserted.bridge_id = ohos_system_service::CallWithTimeout(
          "urlcheck", "check", std::move(args), timeout,
          base::BindOnce(&Impl::OnCloud, weak_factory_.GetWeakPtr(), id,
                         revision_));
    }
    base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&Impl::Timeout, weak_factory_.GetWeakPtr(), id),
        timeout);
    MaybeComplete(id);
    return id;
  }
  void OnCloud(std::string id, int revision, ohos_system_service::Reply reply) {
    auto it = pending_.find(id);
    if (it == pending_.end() || revision != revision_)
      return;
    auto& call = it->second;
    call.waiting_cloud = false;
    call.bridge_id = 0;
    const auto& data = reply.result_dict();
    Result cloud = Unknown(call.url, "unavailable");
    cloud.source = "huawei";
    const auto matches = [&](const char* key, const std::string& value) {
      const auto* field = data.FindString(key);
      return field && *field == value;
    };
    const bool valid_identity =
        reply.ok && data.FindInt("protocolVersion") == 1 &&
        data.FindInt("policyRevision") == revision &&
        matches("requestId", id) && matches("contextId", context_id_);
    cloud_reason_ = "unavailable";
    if (valid_identity) {
      const auto* reason = data.FindString("reason");
      constexpr std::string_view kReasons[] = {"consent_required",
                                               "disabled",
                                               "stale_policy",
                                               "unsupported",
                                               "permission_denied",
                                               "invalid_request",
                                               "network_unavailable",
                                               "cloud_unavailable",
                                               "concurrency_limited",
                                               "rate_limited",
                                               "timeout",
                                               "cloud_quota_exceeded",
                                               "internal_error",
                                               "cancelled",
                                               "context_closed",
                                               "unsupported_protocol",
                                               "unsupported_query_mode",
                                               "invalid_response"};
      if (reason && std::ranges::find(kReasons, *reason) != std::end(kReasons))
        cloud_reason_ = *reason;
    }
    if (valid_identity && matches("checkedUrl", call.checked_url.spec())) {
      const auto* verdict = data.FindString("verdict");
      if (verdict && *verdict == "phishing")
        cloud.verdict = Verdict::kPhishing;
      else if (verdict && *verdict == "malware")
        cloud.verdict = Verdict::kMalware;
      else if (verdict && *verdict == "no_match")
        cloud.verdict = Verdict::kNoMatch;
      else if (verdict && *verdict == "other")
        cloud.verdict = Verdict::kOther;
      if (cloud.verdict != Verdict::kUnknown)
        cloud.reason = cloud.dangerous()                  ? "matched"
                       : cloud.verdict == Verdict::kOther ? "other_category"
                                                          : "no_match";
    }
    if (cloud.verdict != Verdict::kUnknown) {
      cloud_status_ = "available";
      cloud_reason_.clear();
      if (cache_.size() >= 256)
        cache_.erase(cache_.begin());
      cache_[call.checked_url.spec()] = {
          cloud,
          base::TimeTicks::Now() + base::Minutes(cloud.dangerous() ? 30 : 5)};
      if (!call.result.dangerous() &&
          (cloud.dangerous() || call.result.verdict == Verdict::kUnknown))
        call.result = cloud;
    } else {
      cloud_status_ = "unavailable";
    }
    MaybeComplete(id);
    Broadcast();
  }
  void MaybeComplete(const std::string& id) {
    auto it = pending_.find(id);
    if (it == pending_.end())
      return;
    auto& call = it->second;
    if (!call.result.dangerous() && (call.waiting_local || call.waiting_cloud))
      return;
    Pending done = std::move(call);
    pending_.erase(it);
    if (done.bridge_id) {
      ohos_system_service::CancelCall(done.bridge_id);
      SendControl("cancel", base::DictValue().Set("requestId", id));
    }
    PostResult(std::move(done.callback), std::move(done.result));
  }
  void Timeout(const std::string& id) {
    auto it = pending_.find(id);
    if (it == pending_.end())
      return;
    it->second.waiting_local = it->second.waiting_cloud = false;
    MaybeComplete(id);
  }
  void Cancel(const std::string& id) {
    auto it = pending_.find(id);
    if (it == pending_.end())
      return;
    it->second.result = Unknown(it->second.url, "cancelled");
    it->second.waiting_local = it->second.waiting_cloud = false;
    MaybeComplete(id);
  }

  raw_ptr<Profile> profile_;
  bool enabled_ = false;
  bool consent_ = false;
  std::string provider_ = "local";
  std::set<std::string> site_exceptions_;
  std::string context_id_ = base::UnguessableToken::Create().ToString();
  int revision_ = 1;
  std::string cloud_status_ = "unverified";
  std::string cloud_reason_;
  LocalRules rules_;
  bool loading_ = false;
  bool rules_load_attempted_ = false;
  std::map<std::string, Pending> pending_;
  std::map<std::string, Cached> cache_;
  base::RepeatingClosureList changed_;
  base::WeakPtrFactory<Impl> weak_factory_{this};
};

Service* Service::Get(Profile* profile) {
  static base::NoDestructor<PerProfile<Service>> services(base::BindRepeating(
      [](Profile* profile) { return std::make_unique<Service>(profile); }));
  return services->Get(profile);
}
Service::Service(Profile* profile) : impl_(std::make_unique<Impl>(profile)) {}
Service::~Service() = default;
bool Service::enabled() const {
  return impl_->enabled_;
}
bool Service::uses_local() const {
  return impl_->UsesLocal();
}
bool Service::uses_huawei() const {
  return impl_->UsesCloud();
}
int Service::revision() const {
  return impl_->revision_;
}
bool Service::IsSiteEnabled(const GURL& url) const {
  return impl_->IsSiteEnabled(url);
}
bool Service::CanSetSiteEnabled() const {
  return impl_->CanSetSiteEnabled();
}
std::string Service::SetSiteEnabled(const GURL& url, bool enabled) {
  return impl_->SetSiteEnabled(url, enabled);
}
base::DictValue Service::State() const {
  return impl_->State();
}
std::string Service::Configure(const base::DictValue& command) {
  return impl_->Configure(command);
}
void Service::SetLegacyEnabled(bool enabled) {
  auto config = State();
  config.Set("enabled", enabled);
  config.Set("expectedRevision", revision());
  config.Set("cloudConsent", false);
  Configure(config);
}
base::CallbackListSubscription Service::Observe(
    base::RepeatingClosure callback) {
  return impl_->changed_.Add(std::move(callback));
}
Result Service::KnownRisk(const GURL& url) const {
  return impl_->KnownRisk(url);
}
std::string Service::Check(const GURL& url,
                           base::TimeDelta timeout,
                           Callback callback) {
  return impl_->Check(url, timeout, std::move(callback));
}
void Service::Cancel(const std::string& request_id) {
  impl_->Cancel(request_id);
}
base::WeakPtr<Service> Service::GetWeakPtr() {
  return weak_factory_.GetWeakPtr();
}

bool HandleCommand(const ShellCommandContext& context,
                   std::string_view name,
                   const base::DictValue& command) {
  auto* service = Service::Get(context.profile);
  if (!service)
    return false;
  auto event = service->State();
  if (name == "setUrlProtectionConfig") {
    std::string reason = service->Configure(command);
    event = service->State();
    event.Set("event", "urlProtectionConfigResult");
    event.Set("ok", reason.empty());
    event.Set("reason", reason);
  } else if (name == "getUrlProtectionState") {
    event.Set("event", "urlProtectionState");
  } else {
    return false;
  }
  event.Set("requestId", ReadRequestId(command));
  ReplyToShell(context, std::move(event));
  return true;
}
}  // namespace chrome::ohos::url_protection
