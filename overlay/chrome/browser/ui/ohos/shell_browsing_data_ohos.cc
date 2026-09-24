// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Clearing browsing data for the shell's "Clear browsing data" page.
//
//   clearBrowsingData {requestId, types: [...], timeRange}
//     types: 'history' | 'cookies' | 'cache' | 'siteSettings' | 'formData' |
//            'passwords' | 'downloads'
//     timeRange: 'lastHour' | 'lastDay' | 'lastWeek' | 'last4Weeks' | 'all'
//     -> "clearBrowsingDataDone" {requestId, ok}
//
// "ok" is false when the command was rejected or some data could not be
// removed; the shell gets an answer either way.

#include <stdint.h>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/logging.h"
#include "base/memory/raw_ptr.h"
#include "base/no_destructor.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/browsing_data/chrome_browsing_data_remover_constants.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/ohos/shell_services_ohos.h"
#include "chrome/browser/ui/ohos/shell_settings_ohos_internal.h"
#include "components/browsing_data/core/browsing_data_utils.h"
#include "content/public/browser/browsing_data_remover.h"

namespace chrome::ohos::settings_internal {

namespace {

using content::BrowsingDataRemover;

struct ShellDataType {
  std::string_view name;
  uint64_t remove_mask;
};

// What each checkbox of the shell's page clears. They follow Chromium's own
// "Clear browsing data" dialog: "cookies" is "cookies and other site data".
// "passwords" is the password store in this profile only; there is no
// account store to clear on OHOS.
constexpr ShellDataType kShellDataTypes[] = {
    {"history", chrome_browsing_data_remover::DATA_TYPE_HISTORY},
    {"cookies", chrome_browsing_data_remover::DATA_TYPE_SITE_DATA},
    {"cache", BrowsingDataRemover::DATA_TYPE_CACHE},
    {"siteSettings", chrome_browsing_data_remover::DATA_TYPE_CONTENT_SETTINGS},
    {"formData", chrome_browsing_data_remover::DATA_TYPE_FORM_DATA},
    {"passwords", chrome_browsing_data_remover::DATA_TYPE_PASSWORDS},
    {"downloads", BrowsingDataRemover::DATA_TYPE_DOWNLOADS},
};

struct ShellTimeRange {
  std::string_view name;
  browsing_data::TimePeriod period;
};

constexpr ShellTimeRange kShellTimeRanges[] = {
    {"lastHour", browsing_data::TimePeriod::LAST_HOUR},
    {"lastDay", browsing_data::TimePeriod::LAST_DAY},
    {"lastWeek", browsing_data::TimePeriod::LAST_WEEK},
    {"last4Weeks", browsing_data::TimePeriod::FOUR_WEEKS},
    {"all", browsing_data::TimePeriod::ALL_TIME},
};

// Like Chromium's dialog: data of installed (protected) web apps stays.
constexpr uint64_t kOriginTypeMask =
    BrowsingDataRemover::ORIGIN_TYPE_UNPROTECTED_WEB;

// The union of the masks the command names. Unknown names are logged and
// skipped so an older engine still clears what it knows.
uint64_t ReadRemoveMask(const base::DictValue& command) {
  const base::ListValue* types = command.FindList("types");
  if (!types) {
    LOG(WARNING) << "OHOS shell clearBrowsingData without types";
    return 0;
  }
  uint64_t mask = 0;
  for (const base::Value& type : *types) {
    const std::string* name = type.GetIfString();
    bool known = false;
    for (const ShellDataType& entry : kShellDataTypes) {
      if (name && entry.name == *name) {
        mask |= entry.remove_mask;
        known = true;
        break;
      }
    }
    if (!known) {
      LOG(WARNING) << "OHOS shell asked to clear unknown data type "
                   << type.DebugString();
    }
  }
  return mask;
}

void ReplyClearDone(const ShellCommandContext& context,
                    int request_id,
                    bool ok) {
  base::DictValue event;
  event.Set("event", "clearBrowsingDataDone");
  event.Set("requestId", request_id);
  event.Set("ok", ok);
  ReplyToShell(context, std::move(event));
}

// Waits for one removal task and answers the shell, then runs `on_done` so
// its owner can drop it.
class ClearDoneObserver : public BrowsingDataRemover::Observer {
 public:
  ClearDoneObserver(BrowsingDataRemover* remover,
                    const ShellCommandContext& context,
                    int request_id,
                    base::OnceClosure on_done)
      : remover_(remover),
        context_(context),
        request_id_(request_id),
        on_done_(std::move(on_done)) {
    remover_->AddObserver(this);
  }

  ClearDoneObserver(const ClearDoneObserver&) = delete;
  ClearDoneObserver& operator=(const ClearDoneObserver&) = delete;

  // Unregisters when the asking profile goes first, so the remover never
  // calls into a window that is gone.
  ~ClearDoneObserver() override {
    if (remover_) {
      remover_->RemoveObserver(this);
    }
  }

  // BrowsingDataRemover::Observer:
  void OnBrowsingDataRemoverDone(uint64_t failed_data_types) override {
    remover_->RemoveObserver(this);
    remover_ = nullptr;
    if (failed_data_types != 0) {
      LOG(WARNING) << "OHOS shell clearBrowsingData could not remove types 0x"
                   << std::hex << failed_data_types;
    }
    ReplyClearDone(context_, request_id_, failed_data_types == 0);
    // Deletes `this`; nothing may follow.
    std::move(on_done_).Run();
  }

 private:
  raw_ptr<BrowsingDataRemover> remover_;
  const ShellCommandContext context_;
  const int request_id_;
  base::OnceClosure on_done_;
};

// The removals in flight for the windows of one profile. Keyed by the
// profile that asked: when it is destroyed (an incognito one can go long
// before the regular profile whose data is being removed) its observers
// unregister instead of answering a window that no longer exists. The
// regular profile's own entry goes before its remover does.
class ClearTasks {
 public:
  ClearTasks() = default;
  ClearTasks(const ClearTasks&) = delete;
  ClearTasks& operator=(const ClearTasks&) = delete;
  ~ClearTasks() = default;

  void Start(Profile* settings_profile,
             const ShellCommandContext& context,
             int request_id,
             browsing_data::TimePeriod period,
             uint64_t remove_mask) {
    BrowsingDataRemover* remover = settings_profile->GetBrowsingDataRemover();
    const int id = next_id_++;
    auto observer = std::make_unique<ClearDoneObserver>(
        remover, context, request_id,
        base::BindOnce(&ClearTasks::Remove, base::Unretained(this), id));
    ClearDoneObserver* started = observer.get();
    observers_.emplace(id, std::move(observer));
    remover->RemoveAndReply(browsing_data::CalculateBeginDeleteTime(period),
                            browsing_data::CalculateEndDeleteTime(period),
                            remove_mask, kOriginTypeMask, started);
  }

 private:
  // `this` owns every observer, so it is alive whenever one reports.
  void Remove(int id) { observers_.erase(id); }

  int next_id_ = 0;
  std::map<int, std::unique_ptr<ClearDoneObserver>> observers_;
};

PerProfile<ClearTasks>& ClearTasksPerProfile() {
  static base::NoDestructor<PerProfile<ClearTasks>> tasks(base::BindRepeating(
      [](Profile*) { return std::make_unique<ClearTasks>(); }));
  return *tasks;
}

}  // namespace

std::optional<browsing_data::TimePeriod> ReadTimePeriod(
    const base::DictValue& command) {
  const std::string* name = command.FindString("timeRange");
  if (name) {
    for (const ShellTimeRange& entry : kShellTimeRanges) {
      if (entry.name == *name) {
        return entry.period;
      }
    }
  }
  LOG(WARNING) << "OHOS shell sent bad timeRange "
               << (name ? *name : std::string("(absent)"));
  return std::nullopt;
}

void ClearBrowsingData(const ShellCommandContext& context,
                       const base::DictValue& command) {
  const int request_id = ReadRequestId(command);
  Profile* profile = SettingsProfile(context);
  const std::optional<browsing_data::TimePeriod> period =
      ReadTimePeriod(command);
  const uint64_t remove_mask = ReadRemoveMask(command);
  if (!profile || !period || remove_mask == 0) {
    ReplyClearDone(context, request_id, /*ok=*/false);
    return;
  }
  ClearTasksPerProfile().Get(context.profile)->Start(
      profile, context, request_id, *period, remove_mask);
}

}  // namespace chrome::ohos::settings_internal
