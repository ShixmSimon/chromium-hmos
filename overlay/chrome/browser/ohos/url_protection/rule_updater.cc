// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#include "chrome/browser/ohos/url_protection/rule_updater.h"

#include <algorithm>
#include "base/base_paths.h"
#include "base/files/file_util.h"
#include "base/files/important_file_writer.h"
#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/rand_util.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/timer/timer.h"
#include "chrome/browser/ohos/url_protection/rule_update_format.h"
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
constexpr char kWatermarkPref[] = "ohos.url_protection.rules_high_water";
struct InitialState {
  UpdateConfig config;
  LocalRules rules;
  int sequence = 0;
  std::string digest;
  std::string source = "package";
};
InitialState Initialize(base::FilePath assets,
                        base::FilePath cache,
                        int minimum,
                        std::string digest,
                        bool load_rules) {
  InitialState state;
  std::string config_json;
  const auto config_path = assets.AppendASCII("ohos_phishing_updates.json");
  if (base::PathExists(config_path) &&
      !base::ReadFileToStringWithMaxSize(config_path, &config_json,
                                         kMaxManifestBytes))
    config_json = "invalid";
  state.config = ParseUpdateConfig(config_json);
  if (!load_rules)
    return state;
  state.rules = LoadLocalRules(assets.AppendASCII("ohos_phishing_rules.json"));
  std::string raw, manifest_json, payload;
  if (state.config.valid() &&
      base::ReadFileToStringWithMaxSize(
          cache, &raw, kMaxRuleBytes + kMaxManifestBytes + 16) &&
      DecodeRuleCache(raw, manifest_json, payload)) {
    auto manifest = VerifyRuleManifest(manifest_json, state.config, minimum,
                                       base::Time::Now());
    if (manifest && AcceptRuleSequence(*manifest, minimum, digest)) {
      auto rules = VerifyRulePayload(payload, *manifest, base::Time::Now());
      if (rules) {
        state.sequence = manifest->sequence;
        state.digest = manifest->sha256;
        if (!state.rules.IsUsable(base::Time::Now()) ||
            rules->generated >= state.rules.generated) {
          state.rules = std::move(*rules);
          state.source = "download";
        }
      }
    }
  }
  return state;
}
struct InstalledRules {
  std::optional<LocalRules> rules;
  std::string error;
};
InstalledRules VerifyAndInstall(base::FilePath temporary,
                                base::FilePath cache,
                                std::string manifest_json,
                                RuleManifest manifest) {
  std::string payload;
  const bool read =
      base::ReadFileToStringWithMaxSize(temporary, &payload, kMaxRuleBytes);
  base::DeleteFile(temporary);
  if (!read)
    return {std::nullopt, "read_failed"};
  auto rules = VerifyRulePayload(payload, manifest, base::Time::Now());
  if (!rules)
    return {std::nullopt, "invalid_payload"};
  if (!base::CreateDirectory(cache.DirName()) ||
      !base::ImportantFileWriter::WriteFileAtomically(
          cache, EncodeRuleCache(manifest_json, payload)))
    return {std::nullopt, "write_failed"};
  return {std::move(rules), {}};
}
constexpr net::NetworkTrafficAnnotationTag kTraffic =
    net::DefineNetworkTrafficAnnotation("ohos_phishing_rule_update", R"(
      semantics {
        sender: "HarmonyOS URL protection"
        description: "Downloads public phishing rules and a signed manifest."
        trigger: "An enabled local-protection profile, or an explicit update request."
        data: "No visited URLs, cookies, page data or account identifiers."
        destination: OTHER
        destination_other: "HTTPS distribution origin configured in the signed application package."
      }
      policy {
        cookies_allowed: NO
        setting: "Disabled when no profile selects enabled local protection."
        policy_exception_justification: "Unavailable unless the product supplies a trusted configuration."
      })");
}  // namespace

class RuleUpdater::Impl {
 public:
  explicit Impl(Profile* profile)
      : profile_(profile),
        cache_(profile->GetPath()
                   .AppendASCII("OhosUrlProtection")
                   .AppendASCII("rules.cache")),
        worker_(base::ThreadPool::CreateSequencedTaskRunner(
            {base::MayBlock(), base::TaskPriority::USER_VISIBLE})),
        sequence_(std::max(0,
                           profile->GetPrefs()
                               ->GetDict(kWatermarkPref)
                               .FindInt("sequence")
                               .value_or(0))),
        rules_(std::make_shared<LocalRules>()) {
    if (const auto* digest =
            profile->GetPrefs()->GetDict(kWatermarkPref).FindString("sha256"))
      digest_ = *digest;
    if (!base::PathService::Get(base::DIR_ASSETS, &assets_)) {
      ready_ = true;
      rules_loaded_ = true;
      rules_ = std::make_shared<LocalRules>();
      reason_ = "assets_unavailable";
      return;
    }
    InitializeState(false);
  }
  void InitializeState(bool load_rules) {
    initializing_rules_ = load_rules;
    worker_->PostTaskAndReplyWithResult(
        FROM_HERE,
        base::BindOnce(&Initialize, assets_, cache_, sequence_, digest_,
                       load_rules),
        base::BindOnce(&Impl::OnInitialized, weak_factory_.GetWeakPtr(),
                       load_rules));
  }
  void OnInitialized(bool loaded_rules, InitialState initial) {
    ready_ = true;
    config_ = std::move(initial.config);
    if (loaded_rules) {
      rules_loaded_ = true;
      initializing_rules_ = false;
      rules_ = std::make_shared<LocalRules>(std::move(initial.rules));
      source_ =
          rules_->status == "missing" ? "none" : std::move(initial.source);
      if (initial.sequence >= sequence_ && !initial.digest.empty()) {
        sequence_ = initial.sequence;
        digest_ = std::move(initial.digest);
        PersistWatermark();
      }
    }
    changed_.Notify();
    if (clients_ && !rules_loaded_ && !initializing_rules_) {
      InitializeState(true);
      return;
    }
    Schedule(base::Seconds(base::RandIntInclusive(10, 30)));
  }
  void Acquire() {
    ++clients_;
    if (!rules_loaded_ && !initializing_rules_) {
      rules_ = std::make_shared<LocalRules>(LocalRules{.status = "loading"});
      changed_.Notify();
      if (ready_)
        InitializeState(true);
    }
    if (clients_ == 1)
      Schedule(std::max(base::Seconds(base::RandIntInclusive(10, 30)),
                        automatic_due_ - base::TimeTicks::Now()));
  }
  void PersistWatermark() {
    profile_->GetPrefs()->SetDict(
        kWatermarkPref,
        base::DictValue().Set("sequence", sequence_).Set("sha256", digest_));
  }
  void Release() {
    if (clients_ <= 0 || --clients_ != 0)
      return;
    timer_.Stop();
    next_check_ = {};
    if (loader_) {
      loader_.reset();
      updating_ = false;
      reason_ = "cancelled";
    }
    // A completed download already being verified on the worker may finish
    // its atomic public-cache write, but cannot start another network request.
    changed_.Notify();
  }
  void Schedule(base::TimeDelta delay) {
    if (!ready_ || !rules_loaded_ || !config_.valid() || !clients_ || updating_)
      return;
    delay = std::max(delay, not_before_ - base::TimeTicks::Now());
    delay = std::max(delay, base::Seconds(1));
    next_check_ = base::Time::Now() + delay;
    automatic_due_ = base::TimeTicks::Now() + delay;
    timer_.Start(
        FROM_HERE, delay,
        base::BindOnce(&Impl::ScheduledUpdate, weak_factory_.GetWeakPtr()));
  }
  void ScheduledUpdate() { RequestUpdate(); }
  std::string RequestUpdate() {
    if (!ready_ || !rules_loaded_)
      return "initializing";
    if (!clients_)
      return "disabled";
    if (!config_.valid())
      return config_.status;
    if (updating_)
      return "already_updating";
    if (base::TimeTicks::Now() < not_before_)
      return "rate_limited";
    timer_.Stop();
    next_check_ = {};
    updating_ = true;
    reason_.clear();
    last_check_ = base::Time::Now();
    not_before_ = base::TimeTicks::Now() + base::Minutes(1);
    loader_ = CreateLoader(config_.manifest_url);
    loader_->DownloadToString(
        profile_->GetDefaultStoragePartition()
            ->GetURLLoaderFactoryForBrowserProcess()
            .get(),
        base::BindOnce(&Impl::OnManifest, weak_factory_.GetWeakPtr()),
        kMaxManifestBytes);
    changed_.Notify();
    return {};
  }
  std::unique_ptr<network::SimpleURLLoader> CreateLoader(const GURL& url) {
    auto request = std::make_unique<network::ResourceRequest>();
    request->url = url;
    request->credentials_mode = network::mojom::CredentialsMode::kOmit;
    request->redirect_mode = network::mojom::RedirectMode::kError;
    request->do_not_prompt_for_login = true;
    request->load_flags = net::LOAD_DISABLE_CACHE | net::LOAD_BYPASS_CACHE;
    auto loader =
        network::SimpleURLLoader::Create(std::move(request), kTraffic);
    loader->SetURLLoaderFactoryOptions(
        network::mojom::kURLLoadOptionBlockLocalRequest |
        network::mojom::kURLLoadOptionBlockAllCookies);
    loader->SetTimeoutDuration(base::Seconds(30));
    return loader;
  }
  void OnManifest(std::optional<std::string> body) {
    loader_.reset();
    if (!body) {
      Fail("manifest_download_failed");
      return;
    }
    auto manifest =
        VerifyRuleManifest(*body, config_, sequence_, base::Time::Now());
    if (!manifest) {
      Fail("invalid_manifest");
      return;
    }
    if (manifest->generated < rules_->generated) {
      Fail("stale_source");
      return;
    }
    if (!AcceptRuleSequence(*manifest, sequence_, digest_)) {
      Fail("rollback");
      return;
    }
    if (manifest->sequence == sequence_ && source_ == "download" &&
        manifest->version == rules_->version &&
        rules_->IsUsable(base::Time::Now())) {
      Finish();
      return;
    }
    manifest_json_ = std::move(*body);
    manifest_ = std::move(*manifest);
    loader_ = CreateLoader(manifest_.rules_url);
    loader_->DownloadToTempFile(
        profile_->GetDefaultStoragePartition()
            ->GetURLLoaderFactoryForBrowserProcess()
            .get(),
        base::BindOnce(&Impl::OnPayload, weak_factory_.GetWeakPtr()),
        manifest_.size);
  }
  void OnPayload(base::FilePath path) {
    loader_.reset();
    if (path.empty()) {
      Fail("rules_download_failed");
      return;
    }
    worker_->PostTaskAndReplyWithResult(
        FROM_HERE,
        base::BindOnce(&VerifyAndInstall, std::move(path), cache_,
                       std::move(manifest_json_), manifest_),
        base::BindOnce(&Impl::OnInstalled, weak_factory_.GetWeakPtr(),
                       manifest_.sequence));
  }
  void OnInstalled(int sequence, InstalledRules installed) {
    if (!installed.rules) {
      Fail(std::move(installed.error));
      return;
    }
    sequence_ = sequence;
    digest_ = manifest_.sha256;
    PersistWatermark();
    rules_ = std::make_shared<LocalRules>(std::move(*installed.rules));
    source_ = "download";
    Finish();
  }
  void Finish() {
    updating_ = false;
    failures_ = 0;
    reason_.clear();
    last_success_ = base::Time::Now();
    Schedule(base::Hours(6) + base::Minutes(base::RandIntInclusive(0, 30)));
    changed_.Notify();
  }
  void Fail(std::string reason) {
    updating_ = false;
    reason_ = std::move(reason);
    failures_ = std::min(failures_ + 1, 7);
    auto delay =
        std::min(base::Minutes(5 * (1 << (failures_ - 1))), base::Hours(6));
    delay += base::Seconds(base::RandIntInclusive(0, 60));
    not_before_ = base::TimeTicks::Now() + delay;
    Schedule(delay);
    changed_.Notify();
  }
  base::DictValue State() const {
    return base::DictValue()
        .Set("rulesUpdateSupported", config_.valid())
        .Set("rulesUpdateStatus", !ready_            ? "initializing"
                                  : !config_.valid() ? config_.status
                                  : !clients_        ? "disabled"
                                  : updating_        ? "updating"
                                  : failures_        ? "backoff"
                                                     : "idle")
        .Set("rulesUpdateReason", reason_)
        .Set("rulesLastCheck",
             last_check_.is_null()
                 ? -1.0
                 : last_check_.InMillisecondsFSinceUnixEpoch())
        .Set("rulesLastSuccess",
             last_success_.is_null()
                 ? -1.0
                 : last_success_.InMillisecondsFSinceUnixEpoch())
        .Set("rulesNextCheck",
             next_check_.is_null()
                 ? -1.0
                 : next_check_.InMillisecondsFSinceUnixEpoch())
        .Set("rulesSequence", sequence_)
        .Set("rulesSource", source_);
  }
  raw_ptr<Profile> profile_;
  base::FilePath cache_;
  base::FilePath assets_;
  scoped_refptr<base::SequencedTaskRunner> worker_;
  int sequence_ = 0;
  std::string digest_;
  int clients_ = 0;
  int failures_ = 0;
  bool ready_ = false;
  bool rules_loaded_ = false;
  bool initializing_rules_ = false;
  bool updating_ = false;
  UpdateConfig config_;
  RuleManifest manifest_;
  std::string manifest_json_;
  std::shared_ptr<const LocalRules> rules_;
  std::string source_ = "none";
  std::string reason_;
  base::Time last_check_, last_success_, next_check_;
  base::TimeTicks not_before_;
  base::TimeTicks automatic_due_;
  base::OneShotTimer timer_;
  base::RepeatingClosureList changed_;
  std::unique_ptr<network::SimpleURLLoader> loader_;
  base::WeakPtrFactory<Impl> weak_factory_{this};
};
RuleUpdater* RuleUpdater::Get(Profile* profile) {
  static base::NoDestructor<PerProfile<RuleUpdater>> stores(base::BindRepeating(
      [](Profile* profile) { return std::make_unique<RuleUpdater>(profile); }));
  return stores->Get(profile->GetOriginalProfile());
}
void RuleUpdater::RegisterPrefs(PrefRegistrySimple* registry) {
  registry->RegisterDictionaryPref(kWatermarkPref);
}
RuleUpdater::RuleUpdater(Profile* profile)
    : impl_(std::make_unique<Impl>(profile)) {}
RuleUpdater::~RuleUpdater() = default;
void RuleUpdater::Acquire() {
  impl_->Acquire();
}
void RuleUpdater::Release() {
  impl_->Release();
}
std::string RuleUpdater::RequestUpdate() {
  return impl_->RequestUpdate();
}
std::shared_ptr<const LocalRules> RuleUpdater::rules() const {
  return impl_->rules_;
}
base::DictValue RuleUpdater::State() const {
  return impl_->State();
}
base::CallbackListSubscription RuleUpdater::Observe(
    base::RepeatingClosure callback) {
  return impl_->changed_.Add(std::move(callback));
}
base::WeakPtr<RuleUpdater> RuleUpdater::GetWeakPtr() {
  return weak_factory_.GetWeakPtr();
}
}  // namespace chrome::ohos::url_protection
