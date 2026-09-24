// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The shell's history page: queries and deletes the profile's local history on
// behalf of ArkUI, and tells every shell window of the profile when history
// changes so an open page can refetch. The omnibox commands built on history
// (autocomplete, top sites) live in shell_omnibox_ohos.cc.

#include <stddef.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/memory/raw_ptr.h"
#include "base/no_destructor.h"
#include "base/scoped_observation.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/cancelable_task_tracker.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "chrome/browser/history/history_service_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/ohos/shell_omnibox_ohos.h"
#include "chrome/browser/ui/ohos/shell_services_ohos.h"
#include "components/history/core/browser/history_service.h"
#include "components/history/core/browser/history_service_observer.h"
#include "components/history/core/browser/history_types.h"
#include "components/history/core/browser/url_row.h"
#include "components/keyed_service/core/service_access_type.h"
#include "url/gurl.h"

namespace chrome::ohos {

namespace {

using history::HistoryService;

constexpr char kHistoryChangedEvent[] = "historyChanged";
constexpr char kHistoryResultsEvent[] = "historyResults";

constexpr int kDefaultMaxCount = 100;
constexpr int kMinMaxCount = 1;
constexpr int kMaxMaxCount = 500;

// A sync pass or a page load can add many visits in a burst; the shell only
// needs to know "refetch", so it hears about it at most this often.
constexpr base::TimeDelta kChangeCoalescing = base::Seconds(1);

// Bounds one removeHistoryItems command, so a bad shell message cannot queue
// an unbounded expiry list on the history thread.
constexpr size_t kMaxItemsPerRemove = 1000;

// --- Chromium API wrappers: a rename in a future milestone touches only
// these. ---------------------------------------------------------------------

// Explicit access: every command here is the user acting on their own
// history. Incognito profiles are redirected to their original profile's
// service by the factory, as desktop's history page is.
HistoryService* HistoryServiceFor(Profile* profile) {
  return profile ? HistoryServiceFactory::GetForProfile(
                       profile, ServiceAccessType::EXPLICIT_ACCESS)
                 : nullptr;
}

// The options chrome://history uses (BrowsingHistoryHandler::
// SendHistoryQuery): one row per URL per day, newest first, no 404 visits.
history::QueryOptions MakeQueryOptions(base::Time before, int max_count) {
  history::QueryOptions options;
  options.end_time = before;
  options.max_count = max_count;
  options.duplicate_policy = history::QueryOptions::REMOVE_DUPLICATES_PER_DAY;
  options.policy_for_404_visits = history::VisitQuery404sPolicy::kExclude404s;
  options.visit_order = history::QueryOptions::RECENT_FIRST;
  return options;
}

// Mirrors BrowsingHistoryService::RemoveVisits: a row stands for every visit
// to its URL on its day, so removing the row expires that whole day for that
// URL. Sync delete directives are left out; this port has no history sync.
history::ExpireHistoryArgs MakeExpireArgs(const GURL& url,
                                          base::Time visit_time) {
  history::ExpireHistoryArgs args;
  args.SetTimeRangeForOneDay(visit_time);
  args.urls.insert(url);
  return args;
}

// --- Per-profile state. ------------------------------------------------------

// Watches the profile's history and keeps the task tracker for its queries
// and deletions, so none of their callbacks outlives the profile.
class HistoryWatcher : public history::HistoryServiceObserver {
 public:
  explicit HistoryWatcher(Profile* profile) : profile_(profile) {
    if (HistoryService* service = HistoryServiceFor(profile)) {
      observation_.Observe(service);
    }
  }
  HistoryWatcher(const HistoryWatcher&) = delete;
  HistoryWatcher& operator=(const HistoryWatcher&) = delete;
  ~HistoryWatcher() override = default;

  base::CancelableTaskTracker* tracker() { return &tracker_; }

  // history::HistoryServiceObserver:
  void OnURLVisited(HistoryService* history_service,
                    const history::VisitedURLInfo& visited_url_info) override {
    NotifyChanged();
  }
  // A page's title usually lands after its visit; the list shows titles.
  void OnURLsModified(HistoryService* history_service,
                      const history::URLRows& changed_urls) override {
    NotifyChanged();
  }
  void OnHistoryDeletions(HistoryService* history_service,
                          const history::DeletionInfo& deletion_info) override {
    NotifyChanged();
  }
  void HistoryServiceBeingDeleted(HistoryService* history_service) override {
    observation_.Reset();
    timer_.Stop();
    tracker_.TryCancelAll();
  }

 private:
  // Leading edge right away, then at most one trailing event per window
  // carrying whatever happened meanwhile.
  void NotifyChanged() {
    if (timer_.IsRunning()) {
      pending_ = true;
      return;
    }
    Broadcast();
  }

  void OnWindowEnded() {
    if (pending_) {
      Broadcast();
    }
  }

  void Broadcast() {
    pending_ = false;
    base::DictValue event;
    event.Set("event", kHistoryChangedEvent);
    event.Set("revision", ++revision_);
    BroadcastToShell(profile_, std::move(event));
    timer_.Start(FROM_HERE, kChangeCoalescing, this,
                 &HistoryWatcher::OnWindowEnded);
  }

  const raw_ptr<Profile> profile_;
  int revision_ = 0;
  bool pending_ = false;
  base::OneShotTimer timer_;
  base::CancelableTaskTracker tracker_;
  base::ScopedObservation<HistoryService, history::HistoryServiceObserver>
      observation_{this};
};

PerProfile<HistoryWatcher>& Watchers() {
  static base::NoDestructor<PerProfile<HistoryWatcher>> watchers(
      base::BindRepeating(
          [](Profile* profile) {
            return std::make_unique<HistoryWatcher>(profile);
          }));
  return *watchers;
}

// --- Input. ------------------------------------------------------------------

int ReadMaxCount(const base::DictValue& command) {
  const std::optional<double> raw = command.FindDouble("maxCount");
  if (!raw) {
    return kDefaultMaxCount;
  }
  if (*raw < kMinMaxCount || *raw > kMaxMaxCount) {
    LOG(WARNING) << "OHOS shell history: maxCount " << *raw << " clamped";
  }
  return static_cast<int>(
      std::clamp(*raw, static_cast<double>(kMinMaxCount),
                 static_cast<double>(kMaxMaxCount)));
}

// A URL the history database could hold; the shell sends it as a string.
std::optional<GURL> ReadUrl(const base::DictValue& dict) {
  const std::string* spec = dict.FindString("url");
  if (!spec) {
    return std::nullopt;
  }
  GURL url(*spec);
  if (!url.is_valid()) {
    return std::nullopt;
  }
  return url;
}

std::optional<history::ExpireHistoryArgs> ReadRemoveItem(
    const base::Value& item) {
  const base::DictValue* dict = item.GetIfDict();
  if (!dict) {
    return std::nullopt;
  }
  std::optional<GURL> url = ReadUrl(*dict);
  const base::Time visit_time = FromShellTime(dict->FindDouble("visitTime"));
  if (!url || visit_time.is_null()) {
    return std::nullopt;
  }
  return MakeExpireArgs(*url, visit_time);
}

// --- Output. -----------------------------------------------------------------

base::DictValue ToShellItem(const history::URLResult& result) {
  base::DictValue item;
  item.Set("url", result.url().spec());
  item.Set("title", base::UTF16ToUTF8(result.title()));
  item.Set("visitTime", ToShellTime(result.visit_time()));
  item.Set("visitCount", result.visit_count());
  return item;
}

void SendQueryResults(ShellCommandContext context,
                      int request_id,
                      history::QueryResults results) {
  base::ListValue items;
  for (const history::URLResult& result : results) {
    items.Append(base::Value(ToShellItem(result)));
  }
  base::DictValue event;
  event.Set("event", kHistoryResultsEvent);
  event.Set("requestId", request_id);
  event.Set("items", std::move(items));
  event.Set("reachedEnd", results.reached_beginning());
  ReplyToShell(context, std::move(event));
}

// --- Commands. ---------------------------------------------------------------

using CommandRunner = void (*)(const ShellCommandContext&,
                               HistoryService*,
                               base::CancelableTaskTracker*,
                               const base::DictValue&);

// {requestId, text, beforeTime?, maxCount?} -> historyResults.
void RunQueryHistory(const ShellCommandContext& context,
                     HistoryService* service,
                     base::CancelableTaskTracker* tracker,
                     const base::DictValue& command) {
  const std::string* text = command.FindString("text");
  const base::Time before = FromShellTime(command.FindDouble("beforeTime"));
  service->QueryHistory(
      text ? base::UTF8ToUTF16(*text) : std::u16string(),
      MakeQueryOptions(before, ReadMaxCount(command)),
      base::BindOnce(&SendQueryResults, context, ReadRequestId(command)),
      tracker);
}

// {items:[{url, visitTime}]}. The observer reports the change.
void RunRemoveHistoryItems(const ShellCommandContext& context,
                           HistoryService* service,
                           base::CancelableTaskTracker* tracker,
                           const base::DictValue& command) {
  const base::ListValue* items = command.FindList("items");
  if (!items || items->empty() || items->size() > kMaxItemsPerRemove) {
    LOG(WARNING) << "OHOS shell history: removeHistoryItems needs 1.."
                 << kMaxItemsPerRemove << " items";
    return;
  }
  std::vector<history::ExpireHistoryArgs> expire_list;
  for (const base::Value& item : *items) {
    std::optional<history::ExpireHistoryArgs> args = ReadRemoveItem(item);
    if (!args) {
      LOG(WARNING) << "OHOS shell history: skipping malformed item to remove";
      continue;
    }
    expire_list.push_back(std::move(*args));
  }
  if (!expire_list.empty()) {
    service->ExpireHistory(expire_list, base::DoNothing(), tracker);
  }
}

// {url}: every visit to the URL, and the URL itself.
void RunRemoveHistoryForUrl(const ShellCommandContext& context,
                            HistoryService* service,
                            base::CancelableTaskTracker* tracker,
                            const base::DictValue& command) {
  std::optional<GURL> url = ReadUrl(command);
  if (!url) {
    LOG(WARNING) << "OHOS shell history: removeHistoryForUrl needs a url";
    return;
  }
  service->DeleteURLs({*url});
}

// {beginTime?, endTime?}: local history in [begin, end); absent ends are
// unbounded, so {} clears everything.
void RunClearHistory(const ShellCommandContext& context,
                     HistoryService* service,
                     base::CancelableTaskTracker* tracker,
                     const base::DictValue& command) {
  const base::Time begin = FromShellTime(command.FindDouble("beginTime"));
  const base::Time end = FromShellTime(command.FindDouble("endTime"));
  if (!begin.is_null() && !end.is_null() && begin > end) {
    LOG(WARNING) << "OHOS shell history: clearHistory beginTime after endTime";
    return;
  }
  service->ExpireHistoryBetween(/*restrict_urls=*/std::set<GURL>(),
                                /*restrict_app_id=*/std::nullopt, begin, end,
                                /*user_initiated=*/true, base::DoNothing(),
                                tracker);
}

struct HistoryCommand {
  std::string_view name;
  CommandRunner run;
};

constexpr HistoryCommand kHistoryCommands[] = {
    {"queryHistory", &RunQueryHistory},
    {"removeHistoryItems", &RunRemoveHistoryItems},
    {"removeHistoryForUrl", &RunRemoveHistoryForUrl},
    {"clearHistory", &RunClearHistory},
};

CommandRunner FindCommand(std::string_view name) {
  const auto* it =
      std::ranges::find(kHistoryCommands, name, &HistoryCommand::name);
  return it == std::ranges::end(kHistoryCommands) ? nullptr : it->run;
}

}  // namespace

bool HandleHistoryCommand(const ShellCommandContext& context,
                          std::string_view name,
                          const base::DictValue& command) {
  if (HandleOmniboxCommand(context, name, command)) {
    return true;
  }
  CommandRunner run = FindCommand(name);
  if (!run) {
    return false;
  }
  HistoryService* service = HistoryServiceFor(context.profile);
  if (!service) {
    LOG(WARNING) << "OHOS shell history: profile has no history service";
    return true;
  }
  run(context, service, Watchers().Get(context.profile)->tracker(), command);
  return true;
}

void EnsureHistoryObserver(Profile* profile) {
  if (HistoryServiceFor(profile)) {
    Watchers().Get(profile);
  }
}

}  // namespace chrome::ohos
