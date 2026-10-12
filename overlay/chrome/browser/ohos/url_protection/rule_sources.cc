// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#include "chrome/browser/ohos/url_protection/rule_sources.h"

#include <algorithm>
#include <map>
#include "base/files/file_util.h"
#include "base/files/important_file_writer.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/memory/raw_ptr.h"
#include "base/no_destructor.h"
#include "base/rand_util.h"
#include "base/strings/string_util.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/timer/timer.h"
#include "base/unguessable_token.h"
#include "chrome/browser/ohos/url_protection/rule_update_format.h"
#include "chrome/browser/ohos/url_protection/subscription_rules.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/ohos/shell_services_ohos.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/storage_partition.h"
#include "net/base/load_flags.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/url_loader_factory.mojom.h"

namespace chrome::ohos::url_protection {
namespace {
constexpr char kSourcesPref[] = "ohos.url_protection.subscriptions";
struct Recommendation {
  const char* id;
  const char* name;
  const char* url;
  const char* license;
  const char* attribution;
  const char* license_url;
};
constexpr Recommendation kRecommendations[] = {
    {"hagezi-tif-mini", "HaGeZi Threat Intelligence Feeds Mini",
     "https://raw.githubusercontent.com/hagezi/dns-blocklists/main/wildcard/"
     "tif.mini-onlydomains.txt",
     "GPL-3.0", "HaGeZi DNS Blocklists",
     "https://github.com/hagezi/dns-blocklists/blob/main/LICENSE"},
    {"phishdestroy-content", "PhishDestroy Active Content",
     "https://raw.githubusercontent.com/phishdestroy/destroylist/main/dns/"
     "content_active.txt",
     "MIT", "PhishDestroy contributors",
     "https://github.com/phishdestroy/destroylist/blob/main/LICENSE"}};
const Recommendation* FindRecommendation(std::string_view id) {
  for (const auto& item : kRecommendations)
    if (item.id == id)
      return &item;
  return nullptr;
}
std::string String(const base::DictValue& value, const char* key) {
  const auto* found = value.FindString(key);
  return found ? *found : "";
}
struct Config {
  std::string id, name, url, format, rule_type, recommendation;
  bool enabled = false;
  base::DictValue ToValue() const {
    auto value = base::DictValue()
                     .Set("sourceId", id)
                     .Set("name", name)
                     .Set("url", url)
                     .Set("format", format)
                     .Set("ruleType", rule_type)
                     .Set("enabled", enabled)
                     .Set("recommendationId", recommendation);
    const auto* item = FindRecommendation(recommendation);
    value.Set("kind", item ? "recommended" : "custom");
    value.Set("trust", "user_https");
    value.Set("signed", false);
    value.Set("license", item ? item->license : "unknown");
    value.Set("attribution", item ? item->attribution : "");
    value.Set("licenseUrl", item ? item->license_url : "");
    return value;
  }
};
bool Validate(const Config& config) {
  if (config.id.empty() || config.id.size() > 64 ||
      !std::ranges::all_of(
          config.id,
          [](char c) { return base::IsAsciiAlphaNumeric(c) || c == '-'; }) ||
      config.name.empty() || config.name.size() > 120 ||
      !base::IsStringUTF8(config.name) ||
      std::ranges::any_of(config.name,
                          [](unsigned char c) { return c < 32; }) ||
      !ValidSubscriptionEndpoint(GURL(config.url)) ||
      !ValidSubscriptionFormat(config.format, config.rule_type))
    return false;
  if (config.recommendation.empty())
    return true;
  const auto* item = FindRecommendation(config.recommendation);
  return item && config.url == item->url && config.format == "domains" &&
         config.rule_type == "host_suffix";
}
Config FromValue(const base::DictValue& value) {
  return {String(value, "sourceId"),
          String(value, "name"),
          String(value, "url"),
          String(value, "format"),
          String(value, "ruleType"),
          String(value, "recommendationId"),
          value.FindBool("enabled").value_or(false)};
}
using Index = std::shared_ptr<const SubscriptionRules>;
base::FilePath CachePath(const base::FilePath& directory,
                         const Config& config) {
  return directory.AppendASCII(config.id + ".cache");
}
std::map<std::string, Index> Restore(base::FilePath directory,
                                     std::vector<Config> configs) {
  std::map<std::string, Index> restored;
  size_t total = 0;
  for (const auto& config : configs) {
    std::string cache;
    const auto path = CachePath(directory, config);
    if (!base::PathExists(path))
      continue;
    SubscriptionRules rules;
    rules.error = "cache_invalid";
    if (base::ReadFileToStringWithMaxSize(
            path, &cache, kSubscriptionMaxBytes + kMaxManifestBytes + 16)) {
      rules = DecodeSubscriptionCache(cache, GURL(config.url), config.format,
                                      config.rule_type, base::Time::Now());
    }
    if (total + rules.hashes.capacity() > kSubscriptionTotalRecords) {
      rules = {};
      rules.error = "total_record_limit";
    }
    total += rules.hashes.capacity();
    restored.emplace(config.id,
                     std::make_shared<SubscriptionRules>(std::move(rules)));
  }
  return restored;
}
struct ParsedDownload {
  Index rules;
  std::string raw;
  std::string error;
};
ParsedDownload ParseDownload(base::FilePath path, Config config) {
  ParsedDownload result;
  const bool read = base::ReadFileToStringWithMaxSize(path, &result.raw,
                                                      kSubscriptionMaxBytes);
  base::DeleteFile(path);
  if (!read) {
    result.error = "read_failed";
    return result;
  }
  auto rules = ParseSubscriptionRules(result.raw, config.format,
                                      config.rule_type, base::Time::Now());
  result.error = rules.error;
  if (result.error.empty())
    result.rules = std::make_shared<SubscriptionRules>(std::move(rules));
  return result;
}
bool Install(base::FilePath path, Config config, Index rules, std::string raw) {
  auto cache =
      EncodeSubscriptionCache(GURL(config.url), config.format, *rules, raw);
  return !cache.empty() && base::CreateDirectory(path.DirName()) &&
         base::ImportantFileWriter::WriteFileAtomically(path, cache);
}
constexpr net::NetworkTrafficAnnotationTag kTraffic =
    net::DefineNetworkTrafficAnnotation("ohos_user_protection_subscription", R"(
      semantics {
        sender: "HarmonyOS local threat protection subscriptions"
        description: "Downloads a threat list explicitly selected by the user."
        trigger: "Enabled local protection and a user-enabled subscription."
        data: "Subscription URL only; no browsing URLs, cookies or account data."
        destination: OTHER
        destination_other: "User-selected public HTTPS subscription server."
      }
      policy {
        cookies_allowed: NO
        setting: "Disabled until the user adds and enables a subscription."
        policy_exception_justification: "Explicit user-managed subscription."
      })");
}  // namespace

class RuleSources::Impl {
 public:
  struct Entry {
    Config config;
    Index rules;
    std::string error;
    int revision = 1;
    int generation = 0;
    int failures = 0;
    base::Time last_check;
    base::Time due = base::Time::Now() + base::Seconds(15);
    base::Time not_before;
  };
  explicit Impl(Profile* profile)
      : profile_(profile),
        directory_(profile->GetPath().AppendASCII("OhosRuleSubscriptions")),
        worker_(base::ThreadPool::CreateSequencedTaskRunner(
            {base::MayBlock(), base::TaskPriority::USER_VISIBLE})) {
    std::vector<Config> configs;
    for (const auto& value : profile->GetPrefs()->GetList(kSourcesPref)) {
      if (!value.is_dict() || entries_.size() >= kSubscriptionMaxSources)
        continue;
      auto config = FromValue(value.GetDict());
      if (!Validate(config) || entries_.contains(config.id))
        continue;
      configs.push_back(config);
      const auto id = config.id;
      entries_.emplace(id, Entry{.config = std::move(config)});
    }
    worker_->PostTaskAndReplyWithResult(
        FROM_HERE, base::BindOnce(&Restore, directory_, std::move(configs)),
        base::BindOnce(&Impl::OnRestored, weak_factory_.GetWeakPtr()));
  }
  void OnRestored(std::map<std::string, Index> restored) {
    for (auto& [id, rules] : restored) {
      auto& entry = entries_.at(id);
      if (!rules->error.empty()) {
        entry.error = rules->error;
        continue;
      }
      entry.rules = std::move(rules);
      entry.due = std::max(base::Time::Now() + base::Seconds(15),
                           entry.rules->fetched + base::Hours(8));
    }
    ready_ = true;
    Schedule();
    changed_.Notify(true);
  }
  void Persist() {
    base::ListValue values;
    for (const auto& [id, entry] : entries_)
      values.Append(entry.config.ToValue());
    profile_->GetPrefs()->SetList(kSourcesPref, std::move(values));
  }
  void Acquire() {
    ++clients_;
    Schedule();
  }
  void Release() {
    if (clients_ <= 0 || --clients_)
      return;
    timer_.Stop();
    if (!active_.empty()) {
      auto& entry = entries_.at(active_);
      ++entry.generation;
      entry.error = "cancelled";
      active_.clear();
      loader_.reset();
    }
    changed_.Notify(false);
  }
  void Schedule() {
    timer_.Stop();
    if (!ready_ || !clients_ || !active_.empty())
      return;
    base::Time next;
    for (const auto& [id, entry] : entries_) {
      if (entry.config.enabled && (next.is_null() || entry.due < next))
        next = entry.due;
    }
    if (!next.is_null())
      timer_.Start(FROM_HERE,
                   std::max(base::Seconds(1), next - base::Time::Now()),
                   base::BindOnce(&Impl::Tick, weak_factory_.GetWeakPtr()));
  }
  void Tick() {
    for (const auto& [id, entry] : entries_) {
      if (entry.config.enabled && entry.due <= base::Time::Now()) {
        Refresh(id);
        return;
      }
    }
    Schedule();
  }
  Entry* Current(const std::string& id, int generation) {
    auto found = entries_.find(id);
    return found != entries_.end() && found->second.generation == generation &&
                   active_ == id && clients_ && found->second.config.enabled
               ? &found->second
               : nullptr;
  }
  std::string Refresh(const std::string& id) {
    auto& entry = entries_.at(id);
    if (!clients_ || !entry.config.enabled)
      return "disabled";
    if (active_ == id)
      return "already_updating";
    if (!active_.empty())
      return "busy";
    if (entry.not_before > base::Time::Now())
      return "rate_limited";
    timer_.Stop();
    active_ = id;
    const int generation = ++entry.generation;
    entry.last_check = base::Time::Now();
    entry.not_before = entry.last_check + base::Minutes(1);
    entry.due = entry.not_before;
    entry.error.clear();
    auto request = std::make_unique<network::ResourceRequest>();
    request->url = GURL(entry.config.url);
    request->credentials_mode = network::mojom::CredentialsMode::kOmit;
    request->redirect_mode = network::mojom::RedirectMode::kError;
    request->do_not_prompt_for_login = true;
    request->load_flags = net::LOAD_DISABLE_CACHE | net::LOAD_BYPASS_CACHE;
    loader_ = network::SimpleURLLoader::Create(std::move(request), kTraffic);
    // Checked against the runner's actual network service: applied to the
    // resolved peer, independent of LNA feature flags, not just URL spelling.
    loader_->SetURLLoaderFactoryOptions(
        network::mojom::kURLLoadOptionBlockLocalRequest |
        network::mojom::kURLLoadOptionBlockAllCookies);
    loader_->SetTimeoutDuration(base::Seconds(30));
    loader_->DownloadToTempFile(
        profile_->GetDefaultStoragePartition()
            ->GetURLLoaderFactoryForBrowserProcess()
            .get(),
        base::BindOnce(&Impl::OnDownloaded, weak_factory_.GetWeakPtr(), id,
                       generation),
        kSubscriptionMaxBytes);
    changed_.Notify(false);
    return {};
  }
  void OnDownloaded(std::string id, int generation, base::FilePath path) {
    auto* entry = Current(id, generation);
    if (!entry) {
      if (!path.empty())
        worker_->PostTask(FROM_HERE, base::BindOnce(
                                         [](base::FilePath temporary) {
                                           base::DeleteFile(temporary);
                                         },
                                         std::move(path)));
      return;
    }
    loader_.reset();
    if (path.empty()) {
      Fail(id, "download_failed");
      return;
    }
    worker_->PostTaskAndReplyWithResult(
        FROM_HERE,
        base::BindOnce(&ParseDownload, std::move(path), entry->config),
        base::BindOnce(&Impl::OnParsed, weak_factory_.GetWeakPtr(), id,
                       generation));
  }
  void OnParsed(std::string id, int generation, ParsedDownload parsed) {
    auto* entry = Current(id, generation);
    if (!entry)
      return;
    if (!parsed.rules) {
      Fail(id, std::move(parsed.error));
      return;
    }
    size_t total = parsed.rules->hashes.capacity();
    for (const auto& [other_id, other] : entries_)
      if (other_id != id && other.rules)
        total += other.rules->hashes.capacity();
    if (total > kSubscriptionTotalRecords) {
      Fail(id, "total_record_limit");
      return;
    }
    worker_->PostTaskAndReplyWithResult(
        FROM_HERE,
        base::BindOnce(&Install, CachePath(directory_, entry->config),
                       entry->config, parsed.rules, std::move(parsed.raw)),
        base::BindOnce(&Impl::OnInstalled, weak_factory_.GetWeakPtr(), id,
                       generation, parsed.rules));
  }
  void OnInstalled(std::string id, int generation, Index rules, bool success) {
    auto* entry = Current(id, generation);
    if (!entry)
      return;
    if (!success) {
      Fail(id, "write_failed");
      return;
    }
    entry->rules = std::move(rules);
    entry->error.clear();
    entry->failures = 0;
    ++entry->revision;
    ++revision_;
    entry->due = base::Time::Now() + base::Hours(8) +
                 base::Minutes(base::RandIntInclusive(0, 30));
    active_.clear();
    Schedule();
    changed_.Notify(true);
  }
  void Fail(const std::string& id, std::string error) {
    auto& entry = entries_.at(id);
    entry.error = std::move(error);
    entry.failures = std::min(entry.failures + 1, 7);
    entry.not_before = base::Time::Now() +
                       std::min(base::Minutes(5 * (1 << (entry.failures - 1))),
                                base::Hours(6));
    entry.due = entry.not_before + base::Seconds(base::RandIntInclusive(0, 60));
    active_.clear();
    Schedule();
    changed_.Notify(false);
  }
  void DeleteCache(const Config& config) {
    worker_->PostTask(
        FROM_HERE,
        base::BindOnce([](base::FilePath path) { base::DeleteFile(path); },
                       CachePath(directory_, config)));
  }
  std::string Mutate(std::string_view action, const base::DictValue& command) {
    if (!ready_)
      return "initializing";
    if (command.FindInt("expectedRevision") != revision_)
      return "stale_sources";
    if (!profile_->GetPrefs()->IsUserModifiablePreference(kSourcesPref))
      return "managed";
    const auto id = String(command, "sourceId");
    auto found = entries_.find(id);
    if (action == "addProtectionRuleSource") {
      if (entries_.size() >= kSubscriptionMaxSources)
        return "source_limit";
      Config config;
      config.id = base::UnguessableToken::Create().ToString();
      config.recommendation = String(command, "recommendationId");
      if (!config.recommendation.empty()) {
        const auto* item = FindRecommendation(config.recommendation);
        if (!item)
          return "invalid_recommendation";
        config.name = item->name;
        config.url = item->url;
        config.format = "domains";
        config.rule_type = "host_suffix";
      } else {
        config.name = String(command, "name");
        config.url = GURL(String(command, "url")).spec();
        config.format = String(command, "format");
        config.rule_type = String(command, "ruleType");
      }
      if (!command.FindBool("enabled"))
        return "invalid_request";
      config.enabled = *command.FindBool("enabled");
      if (!Validate(config))
        return "invalid_request";
      for (const auto& [other_id, entry] : entries_)
        if (entry.config.url == config.url &&
            entry.config.rule_type == config.rule_type)
          return "duplicate_source";
      const auto key = config.id;
      entries_.emplace(key, Entry{.config = std::move(config)});
    } else {
      if (found == entries_.end())
        return "unknown_source";
      if (action == "refreshProtectionRuleSource")
        return Refresh(id);
      auto& entry = found->second;
      Config updated = entry.config;
      if (action == "updateProtectionRuleSource") {
        if (const auto* value = command.FindString("name"))
          updated.name = *value;
        if (const auto* value = command.FindString("url"))
          updated.url = GURL(*value).spec();
        if (const auto* value = command.FindString("format"))
          updated.format = *value;
        if (const auto* value = command.FindString("ruleType"))
          updated.rule_type = *value;
        if (auto value = command.FindBool("enabled"))
          updated.enabled = *value;
        for (const auto* key : {"name", "url", "format", "ruleType"})
          if (command.contains(key) && !command.FindString(key))
            return "invalid_request";
        if ((command.contains("enabled") && !command.FindBool("enabled")) ||
            !Validate(updated))
          return "invalid_request";
        for (const auto& [other_id, other] : entries_)
          if (other_id != id && other.config.url == updated.url &&
              other.config.rule_type == updated.rule_type)
            return "duplicate_source";
      } else if (action != "removeProtectionRuleSource") {
        return "invalid_request";
      }
      ++entry.generation;
      if (active_ == id) {
        active_.clear();
        loader_.reset();
        // Queue after any worker write; a deleted source cannot reappear on
        // disk.
        DeleteCache(entry.config);
      }
      if (action == "removeProtectionRuleSource") {
        DeleteCache(entry.config);
        entries_.erase(found);
      } else {
        if (updated.url != entry.config.url ||
            updated.format != entry.config.format ||
            updated.rule_type != entry.config.rule_type) {
          DeleteCache(entry.config);
          entry.rules.reset();
        }
        entry.config = std::move(updated);
        ++entry.revision;
        entry.error.clear();
        entry.due =
            std::max(entry.not_before, base::Time::Now() + base::Seconds(15));
      }
    }
    ++revision_;
    Persist();
    Schedule();
    changed_.Notify(true);
    return {};
  }
  bool usable() const {
    return std::ranges::any_of(entries_, [](const auto& pair) {
      return pair.second.config.enabled && pair.second.rules &&
             pair.second.rules->usable(base::Time::Now());
    });
  }
  Result Match(const GURL& url) const {
    Result result{Verdict::kUnknown, "local", "unavailable", url};
    for (const auto& [id, entry] : entries_) {
      if (entry.config.enabled && entry.rules &&
          entry.rules->Matches(url, base::Time::Now())) {
        result.verdict = Verdict::kOther;
        result.reason = "matched";
        result.matched_source_ids.push_back(id);
        if (result.rule_type.empty())
          result.rule_type = entry.config.rule_type;
        else if (result.rule_type != entry.config.rule_type)
          result.rule_type = "multiple";
      }
    }
    return result;
  }
  base::DictValue State() const {
    base::ListValue sources, recommendations;
    const auto now = base::Time::Now();
    for (const auto& [id, entry] : entries_) {
      auto row = entry.config.ToValue();
      row.Set("revision", entry.revision);
      row.Set("status", !ready_                                   ? "loading"
                        : !entry.config.enabled                   ? "disabled"
                        : active_ == id                           ? "updating"
                        : entry.rules && entry.rules->usable(now) ? "ready"
                        : entry.rules                             ? "expired"
                        : !entry.error.empty()                    ? "error"
                                                                  : "missing");
      row.Set("available",
              entry.config.enabled && entry.rules && entry.rules->usable(now));
      row.Set("ruleCount",
              entry.rules ? static_cast<int>(entry.rules->hashes.size()) : 0);
      row.Set("inputCount",
              entry.rules ? static_cast<int>(entry.rules->input_count) : 0);
      row.Set("rejectedCount",
              entry.rules ? static_cast<int>(entry.rules->rejected_broad_count)
                          : 0);
      row.Set("warning", entry.rules && entry.rules->rejected_broad_count
                             ? "broad_rules_rejected"
                             : "");
      row.Set("indexBytes",
              entry.rules
                  ? static_cast<int>(entry.rules->hashes.capacity() * 32)
                  : 0);
      row.Set("error", entry.error);
      // Fetch time is not upstream validation/provenance time.
      row.Set("sourceUpdatedAt", -1.0);
      row.Set("fetchedAt",
              entry.rules ? ToShellTime(entry.rules->fetched) : -1.0);
      row.Set("expiresAt",
              entry.rules ? ToShellTime(entry.rules->fetched + base::Days(7))
                          : -1.0);
      row.Set("lastCheck", ToShellTime(entry.last_check));
      row.Set("nextUpdate", clients_ && entry.config.enabled && active_ != id
                                ? ToShellTime(entry.due)
                                : -1.0);
      sources.Append(std::move(row));
    }
    for (const auto& item : kRecommendations) {
      Config config{item.id,   item.name,     item.url,
                    "domains", "host_suffix", item.id};
      recommendations.Append(config.ToValue());
    }
    return base::DictValue()
        .Set("sourcesRevision", revision_)
        .Set("ready", ready_)
        .Set("localSourcesAvailable", usable())
        .Set("sources", std::move(sources))
        .Set("recommendations", std::move(recommendations))
        .Set("maxSources", static_cast<int>(kSubscriptionMaxSources))
        .Set("maxRecordsPerSource", static_cast<int>(kSubscriptionMaxRecords))
        .Set("maxTotalRecords", static_cast<int>(kSubscriptionTotalRecords));
  }
  raw_ptr<Profile> profile_;
  base::FilePath directory_;
  scoped_refptr<base::SequencedTaskRunner> worker_;
  std::map<std::string, Entry> entries_;
  int clients_ = 0, revision_ = 1;
  bool ready_ = false;
  std::string active_;
  std::unique_ptr<network::SimpleURLLoader> loader_;
  base::OneShotTimer timer_;
  base::RepeatingCallbackList<void(bool)> changed_;
  base::WeakPtrFactory<Impl> weak_factory_{this};
};
RuleSources* RuleSources::Get(Profile* profile) {
  static base::NoDestructor<PerProfile<RuleSources>> stores(base::BindRepeating(
      [](Profile* profile) { return std::make_unique<RuleSources>(profile); }));
  return stores->Get(profile->GetOriginalProfile());
}
void RuleSources::RegisterPrefs(PrefRegistrySimple* registry) {
  registry->RegisterListPref(kSourcesPref);
}
RuleSources::RuleSources(Profile* profile)
    : impl_(std::make_unique<Impl>(profile)) {}
RuleSources::~RuleSources() = default;
void RuleSources::Acquire() {
  impl_->Acquire();
}
void RuleSources::Release() {
  impl_->Release();
}
bool RuleSources::ready() const {
  return impl_->ready_;
}
bool RuleSources::usable() const {
  return impl_->usable();
}
Result RuleSources::Match(const GURL& url) const {
  return impl_->Match(url);
}
base::DictValue RuleSources::State() const {
  return impl_->State();
}
std::string RuleSources::Mutate(std::string_view action,
                                const base::DictValue& command) {
  return impl_->Mutate(action, command);
}
base::CallbackListSubscription RuleSources::Observe(
    base::RepeatingCallback<void(bool)> cb) {
  return impl_->changed_.Add(std::move(cb));
}
base::WeakPtr<RuleSources> RuleSources::GetWeakPtr() {
  return weak_factory_.GetWeakPtr();
}
bool HandleRuleSourcesCommand(const ShellCommandContext& context,
                              std::string_view name,
                              const base::DictValue& command) {
  auto* service = Service::Get(context.profile);
  auto* sources = RuleSources::Get(context.profile);
  const auto state = service->State();
  std::string error;
  if (name != "getProtectionRuleSources") {
    if (String(command, "contextId") != String(state, "contextId"))
      error = "invalid_context";
    else if (context.profile->IsOffTheRecord())
      error = "read_only";
    else if (name == "refreshProtectionRuleSource" && !service->uses_local())
      error = "disabled";
    else
      error = sources->Mutate(name, command);
  }
  auto result = sources->State();
  result.Set("contextId", String(state, "contextId"));
  result.Set("readOnly", context.profile->IsOffTheRecord());
  result.Set("event", name == "getProtectionRuleSources"
                          ? "protectionRuleSources"
                          : "protectionRuleSourceResult");
  result.Set("requestId", ReadRequestId(command));
  result.Set("ok", error.empty() || error == "already_updating");
  result.Set("reason", error);
  ReplyToShell(context, std::move(result));
  return true;
}
}  // namespace chrome::ohos::url_protection
