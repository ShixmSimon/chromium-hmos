// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The numbers the shell's "Clear browsing data" page shows next to each
// checkbox.
//
//   getBrowsingDataCounts {requestId, timeRange}
//     -> "browsingDataCounts" {requestId, historyCount, cacheBytes, siteCount}
//
// historyCount is what chrome://history lists for the range: each URL once
// per day. cacheBytes may be an upper bound (Chromium cannot always size the
// cache for a time range exactly). siteCount is the number of sites holding
// cookies or storage. Any number that could not be computed is -1.
//
// The counters are Chromium's own (//chrome/browser/browsing_data/counters),
// run without their prefs: those prefs belong to the WebUI dialog, and the
// shell passes the range with each query instead.

#include <map>
#include <memory>
#include <optional>
#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/location.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/task/cancelable_task_tracker.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "chrome/browser/browsing_data/counters/cache_counter.h"
#include "chrome/browser/browsing_data/counters/site_data_counter.h"
#include "chrome/browser/history/history_service_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/ohos/shell_services_ohos.h"
#include "chrome/browser/ui/ohos/shell_settings_ohos_internal.h"
#include "components/browsing_data/core/browsing_data_utils.h"
#include "components/browsing_data/core/counters/browsing_data_counter.h"
#include "components/history/core/browser/history_service.h"
#include "components/history/core/browser/history_types.h"
#include "components/keyed_service/core/service_access_type.h"

namespace chrome::ohos::settings_internal {

namespace {

using browsing_data::BrowsingDataCounter;

// What the shell reads as "unknown".
constexpr double kUnknownCount = -1;

// A counter whose backend fails (a cache backend error, for one) never
// reports; the shell still gets an answer, with -1 for what is missing.
constexpr base::TimeDelta kCountsTimeout = base::Seconds(10);

// One getBrowsingDataCounts in flight. Answers exactly once, then runs
// `on_done` from a fresh task so its owner can delete it -- a counter may
// still be on the stack when it reports.
class CountsQuery {
 public:
  CountsQuery(Profile* profile,
              const ShellCommandContext& context,
              int request_id,
              browsing_data::TimePeriod period,
              base::OnceClosure on_done)
      : profile_(profile),
        context_(context),
        request_id_(request_id),
        begin_(browsing_data::CalculateBeginDeleteTime(period)),
        end_(browsing_data::CalculateEndDeleteTime(period)),
        on_done_(std::move(on_done)) {}

  CountsQuery(const CountsQuery&) = delete;
  CountsQuery& operator=(const CountsQuery&) = delete;
  ~CountsQuery() = default;

  void Start() {
    timeout_.Start(FROM_HERE, kCountsTimeout, this, &CountsQuery::Report);
    StartHistoryCount();
    cache_counter_ = StartCounter(std::make_unique<CacheCounter>(profile_),
                                  &cache_bytes_);
    site_counter_ = StartCounter(std::make_unique<SiteDataCounter>(profile_),
                                 &site_count_);
  }

 private:
  void StartHistoryCount() {
    history::HistoryService* history = HistoryServiceFactory::GetForProfile(
        profile_, ServiceAccessType::EXPLICIT_ACCESS);
    if (!history) {
      history_count_ = kUnknownCount;
      return;
    }
    history->GetHistoryCount(
        begin_, end_, history::VisitQuery404sPolicy::kExclude404s,
        base::BindOnce(&CountsQuery::OnHistoryCount, base::Unretained(this)),
        &history_tracker_);
  }

  void OnHistoryCount(history::HistoryCountResult result) {
    history_count_ = result.success ? result.count : kUnknownCount;
    MaybeReport();
  }

  // The counter reports through a callback it owns, so binding `this`
  // unretained is safe: the counter dies with this query.
  std::unique_ptr<BrowsingDataCounter> StartCounter(
      std::unique_ptr<BrowsingDataCounter> counter,
      std::optional<double>* slot) {
    counter->InitWithoutPref(
        begin_, base::BindRepeating(&CountsQuery::OnCounterResult,
                                    base::Unretained(this),
                                    base::Unretained(slot)));
    counter->Restart();
    return counter;
  }

  void OnCounterResult(std::optional<double>* slot,
                       std::unique_ptr<BrowsingDataCounter::Result> result) {
    // Without a period pref the counters never show "calculating", but a
    // non-final result is harmless to skip.
    if (!result || !result->Finished()) {
      return;
    }
    *slot = static_cast<double>(
        static_cast<const BrowsingDataCounter::FinishedResult*>(result.get())
            ->Value());
    MaybeReport();
  }

  void MaybeReport() {
    if (history_count_ && cache_bytes_ && site_count_) {
      Report();
    }
  }

  void Report() {
    if (!on_done_) {
      return;  // Already answered; a counter restarted on a sync change.
    }
    timeout_.Stop();
    base::DictValue event;
    event.Set("event", "browsingDataCounts");
    event.Set("requestId", request_id_);
    event.Set("historyCount", history_count_.value_or(kUnknownCount));
    event.Set("cacheBytes", cache_bytes_.value_or(kUnknownCount));
    event.Set("siteCount", site_count_.value_or(kUnknownCount));
    ReplyToShell(context_, std::move(event));
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, std::move(on_done_));
  }

  const raw_ptr<Profile> profile_;
  const ShellCommandContext context_;
  const int request_id_;
  const base::Time begin_;
  const base::Time end_;
  base::OnceClosure on_done_;

  std::optional<double> history_count_;
  std::optional<double> cache_bytes_;
  std::optional<double> site_count_;

  base::CancelableTaskTracker history_tracker_;
  std::unique_ptr<BrowsingDataCounter> cache_counter_;
  std::unique_ptr<BrowsingDataCounter> site_counter_;
  base::OneShotTimer timeout_;
};

// The queries in flight for the windows of one profile. Keyed by the profile
// that asked, which is also what the reply marks as incognito or not: an
// incognito profile can go away before the regular one whose data is being
// counted, and its queries must go with it.
class CountsQueries {
 public:
  CountsQueries() = default;
  CountsQueries(const CountsQueries&) = delete;
  CountsQueries& operator=(const CountsQueries&) = delete;
  ~CountsQueries() = default;

  void Start(Profile* settings_profile,
             const ShellCommandContext& context,
             int request_id,
             browsing_data::TimePeriod period) {
    const int id = next_id_++;
    auto query = std::make_unique<CountsQuery>(
        settings_profile, context, request_id, period,
        base::BindOnce(&CountsQueries::Remove, weak_factory_.GetWeakPtr(),
                       id));
    CountsQuery* started = query.get();
    queries_.emplace(id, std::move(query));
    started->Start();
  }

 private:
  void Remove(int id) { queries_.erase(id); }

  int next_id_ = 0;
  std::map<int, std::unique_ptr<CountsQuery>> queries_;
  base::WeakPtrFactory<CountsQueries> weak_factory_{this};
};

PerProfile<CountsQueries>& QueriesPerProfile() {
  static base::NoDestructor<PerProfile<CountsQueries>> queries(
      base::BindRepeating(
          [](Profile*) { return std::make_unique<CountsQueries>(); }));
  return *queries;
}

void ReplyUnknownCounts(const ShellCommandContext& context, int request_id) {
  base::DictValue event;
  event.Set("event", "browsingDataCounts");
  event.Set("requestId", request_id);
  event.Set("historyCount", kUnknownCount);
  event.Set("cacheBytes", kUnknownCount);
  event.Set("siteCount", kUnknownCount);
  ReplyToShell(context, std::move(event));
}

}  // namespace

void GetBrowsingDataCounts(const ShellCommandContext& context,
                           const base::DictValue& command) {
  const int request_id = ReadRequestId(command);
  Profile* profile = SettingsProfile(context);
  const std::optional<browsing_data::TimePeriod> period =
      ReadTimePeriod(command);
  if (!profile || !period) {
    ReplyUnknownCounts(context, request_id);
    return;
  }
  QueriesPerProfile().Get(context.profile)->Start(profile, context,
                                                  request_id, *period);
}

}  // namespace chrome::ohos::settings_internal
