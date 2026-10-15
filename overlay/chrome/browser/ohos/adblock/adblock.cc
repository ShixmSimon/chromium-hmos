// Copyright 2026 The Chromium Authors. BSD-style license in LICENSE.
#include "chrome/browser/ohos/adblock/adblock.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cstdlib>
#include <deque>
#include <map>
#include <set>
#include "base/callback_list.h"
#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/files/important_file_writer.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/timer/timer.h"
#include "base/unguessable_token.h"
#include "chrome/browser/ohos/adblock/engine.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/ohos/shell_services_ohos.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "components/sessions/content/session_tab_helper.h"
#include "content/public/browser/global_routing_id.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "net/base/load_flags.h"
#include "net/base/net_errors.h"
#include "net/base/network_change_notifier.h"
#include "net/http/http_response_headers.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "net/url_request/redirect_info.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "third_party/blink/public/common/loader/resource_type_util.h"
#include "url/gurl.h"

namespace chrome::ohos::adblock {
namespace {
constexpr size_t kMaxBytes = 16 * 1024 * 1024;
constexpr size_t kTotalBytes = 64 * 1024 * 1024;
using Reply = base::OnceCallback<void(std::string, int)>;
std::string Str(const base::DictValue& v, std::string_view key) {
  auto* s = v.FindString(key);
  return s ? *s : "";
}
bool SubscriptionUrl(const GURL& url) {
  return url.is_valid() && url.SchemeIs("https") && !url.has_username() &&
         !url.has_password() && !url.has_ref() && url.spec().size() <= 4096;
}
std::string Host(const GURL& url) {
  if (!url.SchemeIsHTTPOrHTTPS())
    return {};
  std::string host(url.host());
  while (!host.empty() && host.back() == '.')
    host.pop_back();
  return host;
}
std::string NormalizeHost(std::string_view text) {
  if (text.empty() || text.find_first_of("/\\?#@ \t\r\n") != text.npos)
    return {};
  GURL url("https://" + std::string(text) + "/");
  if (url.has_port() || !url.is_valid())
    return {};
  return Host(url);
}
struct Source {
  std::string id, name, kind, url, text, status = "missing", etag, modified;
  bool enabled = true;
  int rules = 0, unsupported = 0;
  double updated = 0;
  base::DictValue Value(bool disk = false) const {
    auto v = base::DictValue()
                 .Set("id", id)
                 .Set("name", name)
                 .Set("kind", kind)
                 .Set("url", url)
                 .Set("enabled", enabled)
                 .Set("status", status)
                 .Set("ruleCount", rules)
                 .Set("unsupportedCount", unsupported)
                 .Set("updatedAt", updated);
    if (disk) {
      v.Set("text", text);
      v.Set("etag", etag);
      v.Set("modified", modified);
    }
    return v;
  }
};
struct Config {
  bool enabled = true, auto_update = true;
  std::vector<Source> sources;
  std::set<std::string> exceptions;
  std::string user;
  int user_rule_count = 0;
  Config() {
    sources.push_back({.id = "easylist",
                       .name = "基础广告过滤 · EasyList",
                       .kind = "builtin",
                       .url = "https://easylist.to/easylist/easylist.txt"});
    sources.push_back(
        {.id = "easylist-china",
         .name = "中文补充过滤 · EasyList China",
         .kind = "builtin",
         .url =
             "https://easylist-downloads.adblockplus.org/easylistchina.txt"});
    sources.push_back({.id = "easyprivacy",
                       .name = "隐私保护 · EasyPrivacy",
                       .kind = "builtin",
                       .url = "https://easylist.to/easylist/easyprivacy.txt",
                       .enabled = false});
  }
  Source* Find(std::string_view id) {
    for (auto& s : sources)
      if (s.id == id)
        return &s;
    return nullptr;
  }
  base::DictValue Value(bool disk) const {
    base::ListValue lists, hosts;
    for (const auto& source : sources)
      lists.Append(source.Value(disk));
    for (const auto& host : exceptions)
      hosts.Append(host);
    return base::DictValue()
        .Set("enabled", enabled)
        .Set("autoUpdate", auto_update)
        .Set("sources", std::move(lists))
        .Set("exceptions", std::move(hosts))
        .Set("userRules", user);
  }
};
Config Restore(const base::FilePath& path) {
  Config config;
  std::string json;
  if (!base::ReadFileToStringWithMaxSize(path, &json,
                                         kTotalBytes + 1024 * 1024))
    return config;
  auto root = base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  if (!root)
    return config;
  config.enabled = root->FindBool("enabled").value_or(true);
  config.auto_update = root->FindBool("autoUpdate").value_or(true);
  config.user = Str(*root, "userRules");
  if (base::UTF8ToUTF16(config.user).size() > 65536)
    config.user.clear();
  if (auto* hosts = root->FindList("exceptions")) {
    for (const auto& host : *hosts) {
      if (!host.is_string() || config.exceptions.size() >= 1000)
        continue;
      auto normalized = NormalizeHost(host.GetString());
      if (!normalized.empty())
        config.exceptions.insert(normalized);
    }
  }
  if (auto* sources = root->FindList("sources")) {
    for (const auto& value : *sources) {
      if (!value.is_dict())
        continue;
      const auto& v = value.GetDict();
      auto id = Str(v, "id");
      auto* source = config.Find(id);
      if (!source) {
        if (config.sources.size() >= 32 || id.empty() || id.size() > 64)
          continue;
        auto kind = Str(v, "kind");
        if (kind != "file" && kind != "subscription")
          continue;
        if (kind == "subscription" && !SubscriptionUrl(GURL(Str(v, "url"))))
          continue;
        config.sources.push_back({.id = id,
                                  .name = Str(v, "name"),
                                  .kind = kind,
                                  .url = Str(v, "url")});
        source = &config.sources.back();
      }
      auto text = Str(v, "text");
      if (text.size() > kMaxBytes || !base::IsStringUTF8(text))
        continue;
      source->enabled = v.FindBool("enabled").value_or(true);
      source->text = std::move(text);
      source->updated = v.FindDouble("updatedAt").value_or(0);
      source->etag = Str(v, "etag");
      source->modified = Str(v, "modified");
    }
  }
  return config;
}
struct Compiled {
  Config config;
  std::shared_ptr<Engine> engine;
  base::DictValue info;
};
Compiled Compile(Config config) {
  base::ListValue sources;
  size_t bytes = config.user.size();
  for (const auto& s : config.sources)
    bytes += s.text.size();
  if (bytes > kTotalBytes)
    return {std::move(config), nullptr,
            base::DictValue().Set("reason", "limit")};
  for (const auto& s : config.sources) {
    if (!s.enabled)
      continue;
    sources.Append(base::DictValue()
                       .Set("id", s.id)
                       .Set("text", s.text)
                       .Set("bundled", s.kind == "builtin"));
  }
  sources.Append(base::DictValue()
                     .Set("id", "user")
                     .Set("text", config.user)
                     .Set("strict", true));
  base::DictValue info;
  auto engine = Engine::Compile(
      base::DictValue().Set("sources", std::move(sources)), &info);
  if (auto* counts = info.FindList("counts")) {
    for (const auto& count : *counts) {
      if (!count.is_dict())
        continue;
      const auto& v = count.GetDict();
      if (Str(v, "id") == "user")
        config.user_rule_count = v.FindInt("ruleCount").value_or(0);
      if (auto* s = config.Find(Str(v, "id"))) {
        s->rules = v.FindInt("ruleCount").value_or(0);
        s->unsupported = v.FindInt("unsupportedCount").value_or(0);
        s->status =
            (s->kind == "builtin" || !s->text.empty()) ? "ready" : "missing";
      }
    }
  }
  return {std::move(config), std::move(engine), std::move(info)};
}
// The shell removes the name immediately after acknowledgement. Open a private
// read-only descriptor before replying; never reopen the filename on the
// worker.
base::File OpenImport(const std::string& path) {
  base::FilePath file(path);
  if (!file.IsAbsolute() || file.ReferencesParent())
    return {};
  auto name = file.BaseName().value();
  if (!base::StartsWith(name, "adblock-import-") ||
      !base::EndsWith(name, ".txt"))
    return {};
  auto dir = file.DirName();
  const auto& d = dir.value();
  bool approved = d == "/data/storage/el2/base/cache";
  const std::string prefix = "/data/storage/el2/base/haps/";
  if (base::StartsWith(d, prefix) && base::EndsWith(d, "/cache")) {
    auto module = d.substr(prefix.size(), d.size() - prefix.size() - 6);
    approved = !module.empty() && module.find('/') == module.npos &&
               module != "." && module != "..";
  }
  if (!approved)
    return {};
  // The protocol requires acquiring the FD before acknowledgement. Only these
  // bounded local metadata operations are synchronous; all reads/parse are on
  // the worker. Reject symlink parents as well as the final directory entry.
  char* resolved = realpath(d.c_str(), nullptr);
  const bool canonical = resolved && d == resolved;
  free(resolved);
  if (!canonical)
    return {};
  int directory =
      open(d.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (directory < 0)
    return {};
  int fd = openat(directory, name.c_str(),
                  O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  close(directory);
  if (fd < 0)
    return {};
  struct stat st;
  if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
      st.st_size > static_cast<int64_t>(kMaxBytes)) {
    close(fd);
    return {};
  }
  return base::File(fd);
}
class Service {
 public:
  static Service* Get(Profile* profile) {
    static base::NoDestructor<PerProfile<Service>> services(base::BindRepeating(
        [](Profile* p) { return std::make_unique<Service>(p); }));
    return services->Get(profile);
  }
  explicit Service(Profile* profile)
      : profile_(profile),
        worker_(base::ThreadPool::CreateSequencedTaskRunner(
            {base::MayBlock(), base::TaskPriority::USER_VISIBLE})),
        path_(profile->GetPath().AppendASCII("LumieAdBlock.json")) {
    if (profile->IsOffTheRecord()) {
      parent_ = Get(profile->GetOriginalProfile())->weak_.GetWeakPtr();
      subscription_ = parent_->Observe(
          base::BindRepeating(&Service::Changed, weak_.GetWeakPtr(), false));
      return;
    }
    busy_ = true;
    worker_->PostTaskAndReplyWithResult(
        FROM_HERE,
        base::BindOnce(
            [](base::FilePath path) { return Compile(Restore(path)); }, path_),
        base::BindOnce(&Service::Installed, weak_.GetWeakPtr(), false,
                       Reply()));
    timer_.Start(FROM_HERE, base::Minutes(5), this, &Service::AutoUpdate);
  }
  ~Service() = default;
  base::WeakPtr<Service> weak() { return weak_.GetWeakPtr(); }
  base::CallbackListSubscription Observe(base::RepeatingClosure cb) {
    return changed_.Add(std::move(cb));
  }
  const Config& config() const { return parent_ ? parent_->config() : config_; }
  std::shared_ptr<Engine> engine() const {
    return parent_ ? parent_->engine() : engine_;
  }
  bool busy() const { return parent_ ? parent_->busy() : busy_; }
  bool SiteEnabled(const GURL& url) const {
    auto host = Host(url);
    if (auto it = site_overrides_.find(host); it != site_overrides_.end())
      return it->second;
    return !config().exceptions.contains(host);
  }
  bool Eligible(const GURL& url) const {
    return config().enabled && !Host(url).empty() && SiteEnabled(url);
  }
  bool Active(const GURL& url) const {
    return Eligible(url) && engine() &&
           (config().user_rule_count > 0 ||
            std::ranges::any_of(config().sources, [](const Source& source) {
              return source.enabled && source.rules > 0;
            }));
  }
  uint64_t revision() const { return revision_; }
  const std::string& context() const { return context_; }
  Profile* profile() const { return profile_; }
  base::DictValue State() const {
    auto v = config().Value(false);
    v.Set("protocolVersion", 1);
    v.Set("supported", true);
    v.Set("engine", "adblock-rust");
    v.Set("capabilities",
          base::ListValue().Append("network").Append("cosmetic"));
    v.Set("ok", true);
    v.Set("reason", error_);
    v.Set("errorLine", 0);
    v.Set("incognito", profile_->IsOffTheRecord());
    v.Set("contextId", context_);
    v.Set("revision", static_cast<double>(revision_));
    v.Set("ready", !!engine());
    v.Set("updating", busy() || (parent_ ? parent_->loader_ != nullptr
                                         : loader_ != nullptr));
    if (profile_->IsOffTheRecord()) {
      auto hosts = config().exceptions;
      for (const auto& [host, enabled] : site_overrides_) {
        if (enabled)
          hosts.erase(host);
        else
          hosts.insert(host);
      }
      base::ListValue list;
      for (const auto& host : hosts)
        list.Append(host);
      v.Set("exceptions", std::move(list));
    }
    auto* sources = v.FindList("sources");
    auto* owner = parent_ ? parent_.get() : this;
    for (auto& item : *sources) {
      auto& s = item.GetDict();
      if (owner->active_ == Str(s, "id"))
        s.Set("status", owner->busy_ ? "compiling" : "updating");
      else if (owner->busy_ && s.FindBool("enabled").value_or(false))
        s.Set("status", "compiling");
    }
    return v;
  }
  std::string CheckRevision(const base::DictValue& cmd) const {
    if (Str(cmd, "contextId") != context_ ||
        cmd.FindDouble("expectedRevision") != static_cast<double>(revision_))
      return "stale";
    return {};
  }
  std::string SetSite(const GURL& url, bool enabled) {
    auto host = Host(url);
    if (host.empty())
      return "invalidSite";
    if (profile_->IsOffTheRecord()) {
      if (!site_overrides_.contains(host) && site_overrides_.size() >= 1000)
        return "limit";
      site_overrides_[host] = enabled;
    } else {
      if (!enabled && !config_.exceptions.contains(host) &&
          config_.exceptions.size() >= 1000)
        return "limit";
      if (enabled)
        config_.exceptions.erase(host);
      else
        config_.exceptions.insert(host);
    }
    Changed(true);
    return {};
  }
  void Query(base::DictValue query,
             const GURL& site,
             base::OnceCallback<void(base::DictValue)> callback) {
    if (parent_ && !parent_->initialized_) {
      parent_->Await(base::BindOnce(&Service::Query, weak_.GetWeakPtr(),
                                    std::move(query), site,
                                    std::move(callback)));
      return;
    }
    if (!initialized_ && !parent_ && waiting_.size() < 4096) {
      Await(base::BindOnce(&Service::Query, weak_.GetWeakPtr(),
                           std::move(query), site, std::move(callback)));
      return;
    }
    if (!Active(site)) {
      std::move(callback).Run({});
      return;
    }
    auto snapshot = engine();
    matcher_->PostTaskAndReplyWithResult(
        FROM_HERE,
        base::BindOnce([](std::shared_ptr<Engine> e,
                          base::DictValue q) { return e->Query(q); },
                       std::move(snapshot), std::move(query)),
        std::move(callback));
  }
  std::string SourceName(std::string_view id) const {
    if (id == "user")
      return "个人规则";
    for (const auto& s : config().sources)
      if (s.id == id)
        return s.name;
    return "广告过滤规则";
  }
  void Mutate(std::string_view name, const base::DictValue& cmd, Reply reply);

 private:
  void Await(base::OnceClosure callback) {
    waiting_.push_back(std::move(callback));
  }
  void Changed(bool persist) {
    ++revision_;
    if (persist && !profile_->IsOffTheRecord()) {
      std::string json;
      base::JSONWriter::Write(config_.Value(true), &json);
      worker_->PostTask(FROM_HERE,
                        base::BindOnce(
                            [](base::FilePath path, std::string data) {
                              base::ImportantFileWriter::WriteFileAtomically(
                                  path, data);
                            },
                            path_, std::move(json)));
    }
    BroadcastToShell(profile_,
                     base::DictValue()
                         .Set("event", "adBlockStateChanged")
                         .Set("contextId", context_)
                         .Set("revision", static_cast<double>(revision_)));
    changed_.Notify();
  }
  void Rebuild(Config config, Reply reply = Reply()) {
    busy_ = true;
    error_.clear();
    Changed(false);
    worker_->PostTaskAndReplyWithResult(
        FROM_HERE, base::BindOnce(&Compile, std::move(config)),
        base::BindOnce(&Service::Installed, weak_.GetWeakPtr(), true,
                       std::move(reply)));
  }
  void Installed(bool persist, Reply reply, Compiled compiled) {
    busy_ = false;
    int line = compiled.info.FindInt("errorLine").value_or(0);
    if (compiled.engine) {
      // Exceptions/toggles can change while compilation runs. Preserve those
      // newer values.
      if (initialized_) {
        compiled.config.enabled = config_.enabled;
        compiled.config.auto_update = config_.auto_update;
        compiled.config.exceptions = config_.exceptions;
      }
      config_ = std::move(compiled.config);
      engine_ = std::move(compiled.engine);
      error_.clear();
    } else {
      error_ = Str(compiled.info, "reason");
      if (error_.empty())
        error_ = "compileFailed";
      if (auto* s = config_.Find(active_)) {
        s->status = "failed";
        int& failures = failures_[active_];
        failures = std::min(failures + 1, 7);
        retry_[active_] =
            base::Time::Now() + base::Minutes(5 * (1 << failures));
      }
    }
    initialized_ = true;
    active_.clear();
    Changed(persist && error_.empty());
    if (reply)
      std::move(reply).Run(error_, line);
    auto waiting = std::move(waiting_);
    for (auto& callback : waiting)
      std::move(callback).Run();
    if (persist && error_.empty())
      for (const auto& source : config_.sources) {
        if (source.enabled && source.kind == "subscription" &&
            source.status == "missing")
          Queue(source.id);
      }
    StartNext();
  }
  void AutoUpdate() {
    if (!initialized_ || busy_ || !config_.enabled || !config_.auto_update ||
        net::NetworkChangeNotifier::IsOffline())
      return;
    const double now = base::Time::Now().InMillisecondsFSinceUnixEpoch();
    for (const auto& s : config_.sources) {
      if (s.enabled && s.kind != "file" &&
          now - s.updated >= base::Days(1).InMillisecondsF() &&
          (!retry_.contains(s.id) || retry_[s.id] <= base::Time::Now()))
        Queue(s.id);
    }
    StartNext();
  }
  void Queue(const std::string& id) {
    if (id != active_ && std::ranges::find(queue_, id) == queue_.end())
      queue_.push_back(id);
  }
  void StartNext();
  void Downloaded(std::optional<std::string> data);
  void Failed() {
    loader_.reset();
    if (auto* s = config_.Find(active_))
      s->status = "failed";
    int& failures = failures_[active_];
    failures = std::min(failures + 1, 7);
    retry_[active_] = base::Time::Now() + base::Minutes(5 * (1 << failures));
    active_.clear();
    error_ = "downloadFailed";
    Changed(false);
    StartNext();
  }
  std::vector<base::OnceClosure> waiting_;
  raw_ptr<Profile> profile_;
  scoped_refptr<base::SequencedTaskRunner> worker_;
  // Updating a list must not queue existing-page requests behind compilation.
  scoped_refptr<base::SequencedTaskRunner> matcher_ =
      base::ThreadPool::CreateSequencedTaskRunner(
          {base::TaskPriority::USER_VISIBLE});
  base::FilePath path_;
  Config config_;
  std::shared_ptr<Engine> engine_;
  bool initialized_ = false, busy_ = false;
  std::string error_, active_;
  std::deque<std::string> queue_;
  std::map<std::string, base::Time> retry_;
  std::map<std::string, int> failures_;
  std::unique_ptr<network::SimpleURLLoader> loader_;
  base::RepeatingTimer timer_;
  uint64_t revision_ = 1;
  const std::string context_ = base::UnguessableToken::Create().ToString();
  std::map<std::string, bool> site_overrides_;
  base::WeakPtr<Service> parent_;
  base::RepeatingClosureList changed_;
  base::CallbackListSubscription subscription_;
  base::WeakPtrFactory<Service> weak_{this};
};
void Service::Mutate(std::string_view name,
                     const base::DictValue& cmd,
                     Reply reply) {
  auto error = CheckRevision(cmd);
  if (error.empty() && profile_->IsOffTheRecord())
    error = "readOnly";
  if (error.empty() && !initialized_)
    error = "initializing";
  if (!error.empty()) {
    std::move(reply).Run(error, 0);
    return;
  }
  auto boolean = cmd.FindBool("enabled");
  if (name == "setAdBlockEnabled" || name == "setAdBlockAutoUpdate") {
    if (!boolean) {
      std::move(reply).Run("invalidRequest", 0);
      return;
    }
    if (name == "setAdBlockEnabled")
      config_.enabled = *boolean;
    else
      config_.auto_update = *boolean;
    Changed(true);
    std::move(reply).Run({}, 0);
    return;
  }
  if (name == "removeAdBlockException") {
    auto host = NormalizeHost(Str(cmd, "text"));
    if (host.empty()) {
      std::move(reply).Run("invalidSite", 0);
      return;
    }
    config_.exceptions.erase(host);
    Changed(true);
    std::move(reply).Run({}, 0);
    return;
  }
  const auto id = Str(cmd, "sourceId");
  if (name == "updateAdBlockSources") {
    if (!id.empty() &&
        (!config_.Find(id) || config_.Find(id)->kind == "file")) {
      std::move(reply).Run("invalidSource", 0);
      return;
    }
    for (const auto& s : config_.sources) {
      if (s.kind != "file" && ((id.empty() && s.enabled) || s.id == id))
        Queue(s.id);
    }
    StartNext();
    std::move(reply).Run({}, 0);
    return;
  }
  if (busy_ || loader_) {
    std::move(reply).Run("busy", 0);
    return;
  }
  Config next = config_;
  auto* source = next.Find(id);
  if (name == "setAdBlockSourceEnabled") {
    if (!source || !boolean)
      error = "invalidSource";
    else
      source->enabled = *boolean;
  } else if (name == "saveAdBlockSource") {
    auto title = Str(cmd, "name");
    GURL url(Str(cmd, "url"));
    if (title.empty() || title.size() > 240 || !SubscriptionUrl(url))
      error = "invalidSource";
    else if (source && source->kind != "subscription")
      error = "readOnly";
    else if (!source && (!id.empty() || next.sources.size() >= 32))
      error = "limit";
    else {
      if (!source) {
        next.sources.push_back(
            {.id = base::UnguessableToken::Create().ToString(),
             .kind = "subscription"});
        source = &next.sources.back();
      }
      if (source->url != url.spec()) {
        source->text.clear();
        source->updated = 0;
        source->etag.clear();
        source->modified.clear();
        source->rules = 0;
        source->unsupported = 0;
        source->status = "missing";
      }
      source->name = title;
      source->url = url.spec();
    }
  } else if (name == "removeAdBlockSource") {
    if (!source)
      error = "invalidSource";
    else if (source->kind == "builtin")
      error = "readOnly";
    else
      std::erase_if(next.sources,
                    [&id](const Source& s) { return s.id == id; });
  } else if (name == "setAdBlockUserRules") {
    if (!cmd.FindString("text"))
      error = "invalidRules";
    else {
      next.user = Str(cmd, "text");
      if (!base::IsStringUTF8(next.user) ||
          base::UTF8ToUTF16(next.user).size() > 65536)
        error = "limit";
    }
  } else if (name == "importAdBlockFile") {
    if (next.sources.size() >= 32) {
      std::move(reply).Run("limit", 0);
      return;
    }
    auto file = OpenImport(Str(cmd, "path"));
    if (!file.IsValid()) {
      std::move(reply).Run("invalidFile", 0);
      return;
    }
    std::string title = Str(cmd, "name");
    if (title.empty() || title.size() > 240) {
      std::move(reply).Run("invalidSource", 0);
      return;
    }
    next.sources.push_back({.id = base::UnguessableToken::Create().ToString(),
                            .name = title,
                            .kind = "file"});
    busy_ = true;
    Changed(false);
    worker_->PostTaskAndReplyWithResult(
        FROM_HERE,
        base::BindOnce(
            [](base::File file, Config config) {
              auto size = file.GetLength();
              if (size <= 0 || size > static_cast<int64_t>(kMaxBytes))
                return Compiled{std::move(config), nullptr,
                                base::DictValue().Set("reason", "invalidFile")};
              std::string text(static_cast<size_t>(size), '\0');
              auto bytes = file.Read(0, base::as_writable_byte_span(text));
              if (!bytes || *bytes != text.size() || !base::IsStringUTF8(text))
                return Compiled{std::move(config), nullptr,
                                base::DictValue()
                                    .Set("reason", "invalidRules")
                                    .Set("errorLine", 1)};
              config.sources.back().text = std::move(text);
              config.sources.back().updated =
                  base::Time::Now().InMillisecondsFSinceUnixEpoch();
              return Compile(std::move(config));
            },
            std::move(file), std::move(next)),
        base::BindOnce(&Service::Installed, weak_.GetWeakPtr(), true,
                       std::move(reply)));
    return;
  } else
    error = "invalidRequest";
  if (!error.empty()) {
    std::move(reply).Run(error, 0);
    return;
  }
  Rebuild(std::move(next), std::move(reply));
}
void Service::StartNext() {
  if (busy_ || loader_ || queue_.empty())
    return;
  active_ = queue_.front();
  queue_.pop_front();
  auto* source = config_.Find(active_);
  if (!source || source->kind == "file") {
    active_.clear();
    StartNext();
    return;
  }
  auto request = std::make_unique<network::ResourceRequest>();
  request->url = GURL(source->url);
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  request->do_not_prompt_for_login = true;
  request->load_flags = net::LOAD_DISABLE_CACHE | net::LOAD_BYPASS_CACHE;
  if (!source->etag.empty())
    request->headers.SetHeader("If-None-Match", source->etag);
  if (!source->modified.empty())
    request->headers.SetHeader("If-Modified-Since", source->modified);
  constexpr auto traffic =
      net::DefineNetworkTrafficAnnotation("ohos_adblock_subscriptions", R"(
    semantics {
      sender: "Lumie adblock-rust subscriptions"
      description: "Downloads user-selected public advertisement filter lists."
      trigger: "Explicit update or enabled automatic update once per day."
      data: "List URL and conditional cache validators; no browsing URLs or cookies."
      destination: OTHER
      destination_other: "User-selected HTTPS list server"
    }
    policy { cookies_allowed: NO
      setting: "Advertisement blocking auto-update and individual source settings."
      policy_exception_justification: "Not implemented on this platform." }
  )");
  loader_ = network::SimpleURLLoader::Create(std::move(request), traffic);
  loader_->SetTimeoutDuration(base::Seconds(30));
  loader_->SetURLLoaderFactoryOptions(
      network::mojom::kURLLoadOptionBlockLocalRequest |
      network::mojom::kURLLoadOptionBlockAllCookies);
  loader_->SetOnRedirectCallback(base::BindRepeating(
      [](base::WeakPtr<Service> self, const GURL&,
         const net::RedirectInfo& redirect,
         const network::mojom::URLResponseHead&,
         std::vector<std::string>* removed) {
        if (!self)
          return;
        removed->push_back("If-None-Match");
        removed->push_back("If-Modified-Since");
        if (!SubscriptionUrl(redirect.new_url))
          self->Failed();
      },
      weak_.GetWeakPtr()));
  loader_->DownloadToString(
      profile_->GetDefaultStoragePartition()
          ->GetURLLoaderFactoryForBrowserProcess()
          .get(),
      base::BindOnce(&Service::Downloaded, weak_.GetWeakPtr()), kMaxBytes);
  Changed(false);
}
void Service::Downloaded(std::optional<std::string> data) {
  auto* source = config_.Find(active_);
  const auto* head = loader_->ResponseInfo();
  auto headers = head ? head->headers : nullptr;
  int status = headers ? headers->response_code() : 0;
  if (source && status == 304 &&
      (!source->text.empty() || source->kind == "builtin")) {
    source->updated = base::Time::Now().InMillisecondsFSinceUnixEpoch();
    source->status = "ready";
    loader_.reset();
    failures_.erase(active_);
    active_.clear();
    error_.clear();
    Changed(true);
    StartNext();
    return;
  }
  if (!source || !data || status != 200 || data->empty() ||
      !base::IsStringUTF8(*data)) {
    Failed();
    return;
  }
  Config next = config_;
  auto* updated = next.Find(active_);
  updated->text = std::move(*data);
  updated->updated = base::Time::Now().InMillisecondsFSinceUnixEpoch();
  updated->etag = headers->GetNormalizedHeader("etag").value_or("");
  updated->modified =
      headers->GetNormalizedHeader("last-modified").value_or("");
  if (updated->etag.size() > 4096)
    updated->etag.clear();
  if (updated->modified.size() > 4096)
    updated->modified.clear();
  failures_.erase(active_);
  loader_.reset();
  Rebuild(std::move(next));
}

class Page : public content::WebContentsObserver,
             public content::WebContentsUserData<Page> {
 public:
  static Page* Get(content::WebContents* contents) {
    if (!contents)
      return nullptr;
    CreateForWebContents(contents);
    return FromWebContents(contents);
  }
  std::string TabId() const {
    return ToShellId(sessions::SessionTabHelper::IdForTab(web_contents()).id());
  }
  const std::string& navigation() const { return navigation_; }
  const GURL& url() const { return url_; }
  base::WeakPtr<Page> weak() { return weak_.GetWeakPtr(); }
  base::DictValue State() const {
    auto* service = Service::Get(
        Profile::FromBrowserContext(web_contents()->GetBrowserContext()));
    return base::DictValue()
        .Set("protocolVersion", 1)
        .Set("supported", true)
        .Set("ok", true)
        .Set("contextId", service->context())
        .Set("revision", static_cast<double>(service->revision()))
        .Set("tabId", TabId())
        .Set("navigationId", navigation_)
        .Set("documentUrl", url_.spec())
        .Set("host", Host(url_))
        .Set("enabled", service->config().enabled)
        .Set("siteEnabled", service->SiteEnabled(url_))
        .Set("active", service->Active(url_))
        .Set("canToggle", !Host(url_).empty() && !pending_)
        .Set("blockedRequests", static_cast<double>(blocked_))
        .Set("reloadRequired", reload_)
        .Set("hits", hits_.Clone());
  }
  void DidStartNavigation(content::NavigationHandle* handle) override {
    if (!handle->IsInPrimaryMainFrame() || handle->IsSameDocument())
      return;
    pending_ = true;
    navigation_ = ToShellId(handle->GetNavigationId());
    url_ = handle->GetURL();
    blocked_ = 0;
    hits_.clear();
    reload_ = false;
    Notify();
  }
  void DidRedirectNavigation(content::NavigationHandle* handle) override {
    if (handle->IsInPrimaryMainFrame() && !handle->IsSameDocument()) {
      url_ = handle->GetURL();
      Notify();
    }
  }
  void DidFinishNavigation(content::NavigationHandle* handle) override {
    if (!handle->IsInPrimaryMainFrame())
      return;
    if (handle->IsSameDocument()) {
      if (handle->HasCommitted()) {
        url_ = handle->GetURL();
        Notify();
      }
      return;
    }
    pending_ = false;
    if (handle->HasCommitted()) {
      url_ = handle->GetURL();
      navigation_ = ToShellId(handle->GetNavigationId());
    } else {
      url_ = web_contents()->GetLastCommittedURL();
      navigation_ = base::UnguessableToken::Create().ToString();
      blocked_ = 0;
      hits_.clear();
    }
    Notify();
  }
  void Record(const std::string& navigation,
              const GURL& url,
              const std::string& type,
              const std::string& source) {
    if (navigation != navigation_)
      return;
    ++blocked_;
    if (hits_.size() == 100)
      hits_.erase(hits_.begin());
    hits_.Append(base::DictValue()
                     .Set("domain", Host(url))
                     .Set("resourceType", type)
                     .Set("sourceName", source));
    Notify();
  }
  void Notify() {
    if (notify_.IsRunning())
      return;
    notify_.Start(
        FROM_HERE, base::Milliseconds(300),
        base::BindOnce(
            [](base::WeakPtr<Page> page) {
              if (!page)
                return;
              auto* service = Service::Get(Profile::FromBrowserContext(
                  page->web_contents()->GetBrowserContext()));
              BroadcastToShell(service->profile(),
                               base::DictValue()
                                   .Set("event", "adBlockPageChanged")
                                   .Set("contextId", service->context())
                                   .Set("tabId", page->TabId())
                                   .Set("navigationId", page->navigation_));
            },
            weak_.GetWeakPtr()));
  }

 private:
  friend class content::WebContentsUserData<Page>;
  explicit Page(content::WebContents* contents)
      : content::WebContentsObserver(contents),
        content::WebContentsUserData<Page>(*contents),
        url_(contents->GetLastCommittedURL()) {
    auto* service = Service::Get(
        Profile::FromBrowserContext(contents->GetBrowserContext()));
    subscription_ = service->Observe(base::BindRepeating(
        [](base::WeakPtr<Page> page) {
          if (page) {
            page->reload_ = true;
            page->Notify();
          }
        },
        weak_.GetWeakPtr()));
  }
  std::string navigation_ = base::UnguessableToken::Create().ToString();
  GURL url_;
  uint64_t blocked_ = 0;
  bool reload_ = false, pending_ = false;
  base::ListValue hits_;
  base::OneShotTimer notify_;
  base::CallbackListSubscription subscription_;
  base::WeakPtrFactory<Page> weak_{this};
  WEB_CONTENTS_USER_DATA_KEY_DECL();
};
WEB_CONTENTS_USER_DATA_KEY_IMPL(Page);

void CheckRequest(Service* service,
                  Page* page,
                  const GURL& url,
                  const GURL& site,
                  const std::string& type,
                  const std::string& method,
                  base::RepeatingCallback<bool()> alive,
                  base::OnceCallback<void(bool)> callback) {
  if (!url.SchemeIsHTTPOrHTTPS() || !service->Eligible(site)) {
    std::move(callback).Run(false);
    return;
  }
  auto navigation = page ? page->navigation() : "";
  auto weak_page = page ? page->weak() : base::WeakPtr<Page>();
  const auto revision = service->revision();
  service->Query(
      base::DictValue()
          .Set("url", url.spec())
          .Set("source", site.spec())
          .Set("type", type)
          .Set("method", method),
      site,
      base::BindOnce(
          [](base::WeakPtr<Service> owner, base::WeakPtr<Page> page,
             std::string navigation, uint64_t revision, GURL site, GURL url,
             std::string type, std::string method,
             base::RepeatingCallback<bool()> alive,
             base::OnceCallback<void(bool)> callback, base::DictValue result) {
            if (!alive.Run())
              return;
            if (page && page->navigation() != navigation) {
              std::move(callback).Run(false);
              return;
            }
            if (owner && owner->revision() != revision) {
              CheckRequest(owner.get(), page.get(), url, site, type, method,
                           alive, std::move(callback));
              return;
            }
            bool blocked = owner && owner->Active(site) &&
                           owner->revision() == revision &&
                           result.FindBool("blocked").value_or(false);
            if (blocked && page)
              page->Record(navigation, url, type,
                           owner->SourceName(Str(result, "sourceId")));
            std::move(callback).Run(blocked);
          },
          service->weak(), weak_page, navigation, revision, site, url, type,
          method, alive, std::move(callback)));
}
class FilterHost : public mojom::Filter {
 public:
  explicit FilterHost(int process) : process_(process) {}
  void Clone(mojo::PendingReceiver<mojom::Filter> receiver) override {
    mojo::MakeSelfOwnedReceiver(std::make_unique<FilterHost>(process_),
                                std::move(receiver));
  }
  content::RenderFrameHost* Frame(const blink::LocalFrameToken& token) {
    auto* process = content::RenderProcessHost::FromID(process_);
    if (!process)
      return nullptr;
    return content::RenderFrameHost::FromFrameToken({process->GetID(), token});
  }
  void Check(const std::optional<blink::LocalFrameToken>& token,
             const GURL& url,
             const GURL& initiator,
             const std::string& type,
             const std::string& method,
             CheckCallback callback) override {
    // Background/shared workers do not have one unambiguous top-level site.
    // Document loads are checked before service-worker interception. Do not
    // invent a tab/site for independent worker traffic or apply its exceptions.
    auto* frame = token ? Frame(*token) : nullptr;
    if (!frame || (initiator.is_valid() && !initiator.SchemeIsHTTPOrHTTPS()) ||
        type.size() > 32 || method.size() > 32 ||
        !frame->GetLastCommittedURL().SchemeIsHTTPOrHTTPS()) {
      std::move(callback).Run(false);
      return;
    }
    auto* contents = content::WebContents::FromRenderFrameHost(frame);
    auto* service =
        Service::Get(Profile::FromBrowserContext(frame->GetBrowserContext()));
    CheckRequest(
        service,
        frame->GetOutermostMainFrame() == contents->GetPrimaryMainFrame()
            ? Page::Get(contents)
            : nullptr,
        url, frame->GetOutermostMainFrame()->GetLastCommittedURL(), type,
        method,
        base::BindRepeating(
            [](base::WeakPtr<FilterHost> self) { return !!self; },
            weak_.GetWeakPtr()),
        std::move(callback));
  }
  void Cosmetics(const blink::LocalFrameToken& token,
                 const GURL& url,
                 uint64_t known,
                 CosmeticsCallback callback) override {
    auto* frame = Frame(token);
    if (!frame || frame->GetLastCommittedURL() != url ||
        !url.SchemeIsHTTPOrHTTPS()) {
      std::move(callback).Run("", 0);
      return;
    }
    auto* service =
        Service::Get(Profile::FromBrowserContext(frame->GetBrowserContext()));
    // One outstanding long poll per frame observer; disconnect destroys it.
    if (known == service->revision()) {
      subscription_ = service->Observe(
          base::BindRepeating(&FilterHost::Changed, weak_.GetWeakPtr()));
      waiting_ = std::move(callback);
      token_ = token;
      url_ = url;
      return;
    }
    SendCosmetics(token, url, std::move(callback));
  }

 private:
  void Changed() {
    subscription_ = {};
    if (waiting_)
      SendCosmetics(token_, url_, std::move(waiting_));
  }
  void SendCosmetics(const blink::LocalFrameToken& token,
                     const GURL& url,
                     CosmeticsCallback callback) {
    auto* frame = Frame(token);
    if (!frame || frame->GetLastCommittedURL() != url) {
      std::move(callback).Run("", 0);
      return;
    }
    auto* service =
        Service::Get(Profile::FromBrowserContext(frame->GetBrowserContext()));
    const auto revision = service->revision();
    service->Query(
        base::DictValue().Set("op", "cosmetic").Set("url", url.spec()),
        frame->GetOutermostMainFrame()->GetLastCommittedURL(),
        base::BindOnce(
            [](CosmeticsCallback callback, uint64_t revision,
               base::DictValue value) {
              std::move(callback).Run(Str(value, "css"), revision);
            },
            std::move(callback), revision));
  }
  int process_;
  blink::LocalFrameToken token_;
  GURL url_;
  CosmeticsCallback waiting_;
  base::CallbackListSubscription subscription_;
  base::WeakPtrFactory<FilterHost> weak_{this};
};
class BrowserThrottle : public blink::URLLoaderThrottle {
 public:
  BrowserThrottle(Service* service,
                  base::RepeatingCallback<content::WebContents*()> getter,
                  bool main)
      : service_(service->weak()), getter_(std::move(getter)), main_(main) {}
  void WillStartRequest(network::ResourceRequest* request,
                        bool* defer) override {
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
    auto* contents = getter_.Run();
    if (!service_ || !contents || !url.SchemeIsHTTPOrHTTPS())
      return;
    auto* page = Page::Get(contents);
    GURL site = main_ ? url : contents->GetLastCommittedURL();
    if (!service_->Eligible(site))
      return;
    *defer = true;
    CheckRequest(service_.get(), page, url, site,
                 main_ ? "document" : "subdocument", method_,
                 base::BindRepeating(
                     [](base::WeakPtr<BrowserThrottle> self) { return !!self; },
                     weak_.GetWeakPtr()),
                 base::BindOnce(
                     [](base::WeakPtr<BrowserThrottle> throttle, bool blocked) {
                       if (!throttle)
                         return;
                       if (blocked)
                         throttle->delegate_->CancelWithError(
                             net::ERR_BLOCKED_BY_CLIENT);
                       else
                         throttle->delegate_->Resume();
                     },
                     weak_.GetWeakPtr()));
  }
  base::WeakPtr<Service> service_;
  base::RepeatingCallback<content::WebContents*()> getter_;
  bool main_;
  std::string method_;
  base::WeakPtrFactory<BrowserThrottle> weak_{this};
};
}  // namespace
void ObserveContents(content::WebContents* contents) {
  Page::Get(contents);
}
void EnsureService(Profile* profile) {
  if (profile)
    Service::Get(profile);
}
void BindFilter(int process_id, mojo::PendingReceiver<mojom::Filter> receiver) {
  if (!content::RenderProcessHost::FromID(process_id))
    return;
  mojo::MakeSelfOwnedReceiver(std::make_unique<FilterHost>(process_id),
                              std::move(receiver));
}
std::unique_ptr<blink::URLLoaderThrottle> NavigationThrottle(
    Profile* profile,
    const network::ResourceRequest& request,
    const base::RepeatingCallback<content::WebContents*()>& getter) {
  if (!blink::IsRequestDestinationFrame(request.destination) || !getter.Run())
    return nullptr;
  Page::Get(getter.Run());
  return std::make_unique<BrowserThrottle>(Service::Get(profile), getter,
                                           request.is_outermost_main_frame);
}
bool HandleCommand(const ShellCommandContext& context,
                   std::string_view name,
                   const base::DictValue& command) {
  auto* service = Service::Get(context.profile);
  if (!service)
    return false;
  const int request = ReadRequestId(command);
  if (name == "getAdBlockPageState" || name == "setAdBlockSiteEnabled") {
    Page* page = nullptr;
    auto* tabs =
        context.browser ? context.browser->GetTabStripModel() : nullptr;
    if (tabs)
      for (int i = 0; i < tabs->count(); ++i) {
        auto* contents = tabs->GetWebContentsAt(i);
        if (contents->GetBrowserContext() != context.profile)
          continue;
        auto* candidate = Page::Get(contents);
        if (candidate->TabId() == Str(command, "tabId")) {
          page = candidate;
          break;
        }
      }
    std::string error;
    if (!page)
      error = "invalidTab";
    else if (name == "setAdBlockSiteEnabled") {
      error = service->CheckRevision(command);
      if (error.empty() &&
          (page->navigation() != Str(command, "navigationId") ||
           !command.FindBool("enabled") ||
           !page->State().FindBool("canToggle").value_or(false)))
        error = "stale";
      if (error.empty())
        error = service->SetSite(page->url(), *command.FindBool("enabled"));
    }
    auto state =
        page ? page->State()
             : base::DictValue()
                   .Set("protocolVersion", 1)
                   .Set("supported", true)
                   .Set("contextId", service->context())
                   .Set("revision", static_cast<double>(service->revision()))
                   .Set("tabId", Str(command, "tabId"));
    state.Set("event", name == "getAdBlockPageState" ? "adBlockPageState"
                                                     : "adBlockSiteResult");
    state.Set("requestId", request);
    state.Set("ok", error.empty());
    state.Set("reason", error);
    ReplyToShell(context, std::move(state));
    return true;
  }
  auto reply = base::BindOnce(
      [](base::WeakPtr<Service> service, ShellCommandContext context,
         int request, std::string event, std::string reason, int error_line) {
        if (!service)
          return;
        auto state = service->State();
        state.Set("event", event);
        state.Set("requestId", request);
        state.Set("ok", reason.empty());
        state.Set("reason", reason);
        state.Set("errorLine", error_line);
        ReplyToShell(context, std::move(state));
      },
      service->weak(), context, request,
      name == "getAdBlockState" ? "adBlockState" : "adBlockConfigResult");
  if (name == "getAdBlockState")
    std::move(reply).Run({}, 0);
  else
    service->Mutate(name, command, std::move(reply));
  return true;
}
}  // namespace chrome::ohos::adblock
