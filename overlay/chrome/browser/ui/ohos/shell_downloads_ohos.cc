// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The shell's downloads page: lists the profile's downloads for ArkUI, runs
// the buttons on it (pause, resume, cancel, retry, remove, keep or discard a
// dangerous file) and pushes every change as a downloadUpdated event.
// Chromium still does the downloading; the shell only draws it.

#include "chrome/browser/ui/ohos/shell_downloads_ohos.h"

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/scoped_multi_source_observation.h"
#include "base/scoped_observation.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/ohos/shell_downloads_item_ohos.h"
#include "chrome/browser/ui/ohos/shell_services_ohos.h"
#include "chrome/common/pref_names.h"
#include "components/download/public/common/download_interrupt_reasons.h"
#include "components/download/public/common/download_item.h"
#include "components/download/public/common/download_source.h"
#include "components/download/public/common/download_url_parameters.h"
#include "components/history/core/common/pref_names.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/download_manager.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "url/gurl.h"

namespace chrome::ohos {

namespace {

using download::DownloadItem;

constexpr char kListDownloadsCommand[] = "listDownloads";
constexpr char kDownloadActionCommand[] = "downloadAction";
constexpr char kSetDownloadDirectoryCommand[] = "setDownloadDirectory";
constexpr char kDownloadUpdatedEvent[] = "downloadUpdated";
constexpr char kDownloadListEvent[] = "downloadList";

// Progress arrives several times a second per item; the shell only needs
// enough to move a progress bar.
constexpr base::TimeDelta kProgressInterval = base::Milliseconds(500);

// Same semantics as chrome://downloads' retry (downloads_dom_handler.cc),
// under an id of its own because the trigger is the shell's page.
constexpr net::NetworkTrafficAnnotationTag kRetryTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("ohos_shell_download_retry", R"(
        semantics {
          sender: "The HarmonyOS shell's downloads page."
          description: "Retrying a download."
          trigger:
            "The user selects 'Retry' for a cancelled or failed download on "
            "the downloads page drawn by the HarmonyOS shell."
          data: "None."
          destination: WEBSITE
        }
        policy {
          cookies_allowed: YES
          cookies_store: "user"
          setting:
            "This feature cannot be disabled by settings, but it's only "
            "triggered by user request."
          policy_exception_justification: "Not implemented."
        })");

// --- Chromium API access, one helper per call so a rename touches one place.

content::DownloadManager* ManagerFor(Profile* profile) {
  // Each profile, incognito included, has a manager of its own.
  return profile ? profile->GetDownloadManager() : nullptr;
}

bool IsDeletingHistoryAllowed(Profile* profile) {
  return profile->GetPrefs()->GetBoolean(prefs::kAllowDeletingBrowserHistory);
}

// --- The per-profile observer. --------------------------------------------

// Watches one profile's DownloadManager and each of its items, and pushes
// changes to the profile's shell windows.
class DownloadsWatcher : public content::DownloadManager::Observer,
                         public DownloadItem::Observer {
 public:
  DownloadsWatcher(Profile* profile, content::DownloadManager* manager)
      : profile_(profile), manager_(manager) {
    manager_observation_.Observe(manager);
    // Items loaded before the watcher existed; later ones arrive through
    // OnDownloadCreated, history included.
    content::DownloadManager::DownloadVector items;
    manager->GetAllDownloads(&items);
    for (DownloadItem* item : items) {
      Watch(item);
    }
  }

  DownloadsWatcher(const DownloadsWatcher&) = delete;
  DownloadsWatcher& operator=(const DownloadsWatcher&) = delete;
  ~DownloadsWatcher() override = default;

  Profile* profile() { return profile_; }
  // Null once the manager has gone down.
  content::DownloadManager* manager() { return manager_; }

  DownloadItem* Find(const std::string& guid) {
    return manager_ ? manager_->GetDownloadByGuid(guid) : nullptr;
  }

  // Deleting is asynchronous; the entry goes once the file is gone, so a
  // failure leaves the user something to try again from.
  void DeleteFileThenRemove(DownloadItem& item) {
    item.DeleteFile(base::BindOnce(&DownloadsWatcher::OnFileDeleted,
                                   weak_factory_.GetWeakPtr(),
                                   item.GetGuid()));
  }

  // content::DownloadManager::Observer:
  void OnDownloadCreated(content::DownloadManager*,
                         DownloadItem* item) override {
    if (!Watch(item)) {
      return;
    }
    // History items are not news; the shell lists them when it asks.
    if (item->GetDownloadCreationType() != DownloadItem::TYPE_HISTORY_IMPORT) {
      LOG(INFO) << "OHOS download started: " << item->GetURL().spec();
      LogMilestones(*item);
      Send(*item, /*removed=*/false);
    }
  }

  void ManagerGoingDown(content::DownloadManager*) override {
    // The items are destroyed right after this; nothing may touch them.
    flush_timer_.Stop();
    pending_.clear();
    item_observations_.RemoveAllObservations();
    manager_observation_.Reset();
    manager_ = nullptr;
    last_sent_.clear();
  }

  // DownloadItem::Observer:
  void OnDownloadUpdated(DownloadItem* item) override {
    LogMilestones(*item);
    auto it = last_sent_.find(item->GetGuid());
    if (it == last_sent_.end() ||
        it->second.signature != DownloadChangeSignature(*item)) {
      Send(*item, /*removed=*/false);
      return;
    }
    const base::TimeDelta since = base::TimeTicks::Now() - it->second.time;
    if (since >= kProgressInterval) {
      Send(*item, /*removed=*/false);
      return;
    }
    // Held back, not dropped: a download that stalls or finishes its last
    // chunk would otherwise show stale progress until its next change.
    pending_.insert(item->GetGuid());
    if (!flush_timer_.IsRunning()) {
      flush_timer_.Start(FROM_HERE, kProgressInterval - since, this,
                         &DownloadsWatcher::FlushPending);
    }
  }

  void OnDownloadRemoved(DownloadItem* item) override {
    Send(*item, /*removed=*/true);
    Forget(item->GetGuid());
  }

  void OnDownloadDestroyed(DownloadItem* item) override {
    if (item_observations_.IsObservingSource(item)) {
      item_observations_.RemoveObservation(item);
    }
    Forget(item->GetGuid());
  }

 private:
  struct LastSent {
    std::string signature;
    base::TimeTicks time;
  };

  // False for items the shell never shows.
  bool Watch(DownloadItem* item) {
    if (!item || !IsUserVisibleDownload(*item)) {
      return false;
    }
    if (!item_observations_.IsObservingSource(item)) {
      item_observations_.AddObservation(item);
    }
    return true;
  }

  void Send(const DownloadItem& item, bool removed) {
    pending_.erase(item.GetGuid());
    last_sent_[item.GetGuid()] = {DownloadChangeSignature(item),
                                  base::TimeTicks::Now()};
    base::DictValue event = ToShellDownloadItem(item, removed);
    event.Set("event", kDownloadUpdatedEvent);
    BroadcastToShell(profile_, std::move(event));
  }

  // Sends the updates held back since the timer started, with the items'
  // current values.
  void FlushPending() {
    const std::set<std::string> pending = std::move(pending_);
    pending_.clear();
    for (const std::string& guid : pending) {
      DownloadItem* item = Find(guid);
      if (item && item_observations_.IsObservingSource(item)) {
        Send(*item, /*removed=*/false);
      }
    }
  }

  void Forget(const std::string& guid) {
    last_sent_.erase(guid);
    pending_.erase(guid);
    logged_.erase(guid);
  }

  // One log line when a download's target path is decided and one when it
  // fails, so a failed save can be traced in hilog without a debugger.
  void LogMilestones(const DownloadItem& item) {
    Logged& logged = logged_[item.GetGuid()];
    if (!logged.target && !item.GetTargetFilePath().empty()) {
      logged.target = true;
      LOG(INFO) << "OHOS download target: " << item.GetTargetFilePath()
                << " for " << item.GetURL().spec();
    }
    if (!logged.failure &&
        item.GetState() == DownloadItem::INTERRUPTED) {
      logged.failure = true;
      LOG(WARNING) << "OHOS download failed: "
                   << download::DownloadInterruptReasonToString(
                          item.GetLastReason())
                   << " writing "
                   << (item.GetTargetFilePath().empty()
                           ? item.GetFullPath()
                           : item.GetTargetFilePath());
    }
  }

  void OnFileDeleted(const std::string& guid, bool deleted) {
    if (!deleted) {
      LOG(WARNING) << "OHOS shell downloads: could not delete the file of "
                   << guid;
      return;
    }
    if (DownloadItem* item = Find(guid)) {
      item->Remove();
    }
  }

  const raw_ptr<Profile> profile_;
  raw_ptr<content::DownloadManager> manager_;
  base::ScopedObservation<content::DownloadManager,
                          content::DownloadManager::Observer>
      manager_observation_{this};
  base::ScopedMultiSourceObservation<DownloadItem, DownloadItem::Observer>
      item_observations_{this};
  // Keyed by GUID, what was last sent for each item.
  std::map<std::string, LastSent> last_sent_;
  // GUIDs with an update held back by the throttle.
  std::set<std::string> pending_;
  struct Logged {
    bool target = false;
    bool failure = false;
  };
  // Keyed by GUID, which milestones have been logged.
  std::map<std::string, Logged> logged_;
  base::OneShotTimer flush_timer_;
  base::WeakPtrFactory<DownloadsWatcher> weak_factory_{this};
};

// Chromium's download bubble delegate turns the "where to save" prompt on for
// every incognito profile when the profile's download manager is created
// (DownloadUIController, via DownloadCoreService). ManagerFor() has created it
// by the time this runs, so turning the prompt back off here sticks. Only the
// prompt needs this: incognito reads the directories through from the
// original profile.
void SuppressIncognitoSavePrompt(Profile* profile) {
  if (profile->IsOffTheRecord() && ShouldShellDrawDownloadUi()) {
    profile->GetPrefs()->SetBoolean(prefs::kPromptForDownload, false);
  }
}

std::unique_ptr<DownloadsWatcher> CreateWatcher(Profile* profile) {
  content::DownloadManager* manager = ManagerFor(profile);
  SuppressIncognitoSavePrompt(profile);
  return std::make_unique<DownloadsWatcher>(profile, manager);
}

PerProfile<DownloadsWatcher>& Watchers() {
  static base::NoDestructor<PerProfile<DownloadsWatcher>> watchers(
      base::BindRepeating(&CreateWatcher));
  return *watchers;
}

// --- Actions. -------------------------------------------------------------

// Re-downloads the URL of a download that cannot pick up where it stopped.
// Not tied to a frame, as Android's retry is: the URL was already fetched
// once for this profile (see DownloadUrlParameters).
void Redownload(content::DownloadManager& manager, const DownloadItem& item) {
  if (!item.GetURL().is_valid()) {
    LOG(WARNING) << "OHOS shell downloads: nothing to retry for "
                 << item.GetGuid();
    return;
  }
  auto params = std::make_unique<download::DownloadUrlParameters>(
      item.GetURL(), kRetryTrafficAnnotation);
  params->set_content_initiated(true);
  params->set_initiator(item.GetRequestInitiator());
  params->set_download_source(download::DownloadSource::RETRY);
  manager.DownloadUrl(std::move(params));
}

void PauseDownload(DownloadItem& item) {
  item.Pause();
}

void ResumeDownload(DownloadItem& item) {
  item.Resume(/*user_resume=*/true);
}

void CancelDownload(DownloadItem& item) {
  item.Cancel(/*user_cancel=*/true);
}

// chrome://downloads' "keep": an insecure verdict first, as it does, since
// accepting one may leave the other to show next.
void KeepDangerousDownload(DownloadItem& item) {
  if (item.IsDone() || !IsAwaitingUserVerdict(item)) {
    LOG(WARNING) << "OHOS shell downloads: nothing to keep for "
                 << item.GetGuid();
    return;
  }
  // Either call may destroy `item`.
  if (item.IsInsecure()) {
    item.ValidateInsecureDownload();
  } else {
    item.ValidateDangerousDownload();
  }
}

// chrome://downloads' "discard": a dangerous download is removed outright,
// never merely hidden, and is not subject to the history-deletion policy.
void DiscardDangerousDownload(DownloadItem& item) {
  if (!IsAwaitingUserVerdict(item)) {
    LOG(WARNING) << "OHOS shell downloads: nothing to discard for "
                 << item.GetGuid();
    return;
  }
  item.Remove();
}

// Whether what stopped the download was the place it was being written to
// rather than the transfer. Resuming keeps the target path, so a download
// that failed because the directory could not be written would fail again
// the moment it resumed -- which is what the user saw as "retry cancels it".
// Fetching afresh picks a target from the download directory as it is now.
bool FailedOnTheDestination(const DownloadItem& item) {
  switch (item.GetLastReason()) {
    case download::DOWNLOAD_INTERRUPT_REASON_FILE_FAILED:
    case download::DOWNLOAD_INTERRUPT_REASON_FILE_ACCESS_DENIED:
    case download::DOWNLOAD_INTERRUPT_REASON_FILE_NO_SPACE:
    case download::DOWNLOAD_INTERRUPT_REASON_FILE_NAME_TOO_LONG:
    case download::DOWNLOAD_INTERRUPT_REASON_FILE_TOO_LARGE:
    case download::DOWNLOAD_INTERRUPT_REASON_FILE_TRANSIENT_ERROR:
    case download::DOWNLOAD_INTERRUPT_REASON_FILE_BLOCKED:
    case download::DOWNLOAD_INTERRUPT_REASON_FILE_SECURITY_CHECK_FAILED:
    case download::DOWNLOAD_INTERRUPT_REASON_FILE_SAME_AS_SOURCE:
      return true;
    default:
      return false;
  }
}

void RetryDownload(DownloadsWatcher& watcher, DownloadItem& item) {
  if (item.CanResume() && !FailedOnTheDestination(item)) {
    item.Resume(/*user_resume=*/true);
    return;
  }
  const DownloadItem::DownloadState state = item.GetState();
  if (state != DownloadItem::CANCELLED &&
      state != DownloadItem::INTERRUPTED &&
      // A destination failure can leave the item still nominally in progress
      // for a moment; it is stopped as far as the user is concerned.
      !FailedOnTheDestination(item)) {
    LOG(WARNING) << "OHOS shell downloads: retry of a download that has not "
                    "stopped";
    return;
  }
  Redownload(*watcher.manager(), item);
}

void RemoveDownload(DownloadsWatcher& watcher, DownloadItem& item) {
  // chrome://downloads honours the same policy.
  if (!IsDeletingHistoryAllowed(watcher.profile())) {
    LOG(WARNING) << "OHOS shell downloads: removal is disabled by policy";
    return;
  }
  item.Remove();
}

void RemoveDownloadAndFile(DownloadsWatcher& watcher, DownloadItem& item) {
  if (!IsDeletingHistoryAllowed(watcher.profile())) {
    LOG(WARNING) << "OHOS shell downloads: removal is disabled by policy";
    return;
  }
  // Only a finished download owns a file at its target; Remove() already
  // deletes the partial file of an unfinished one.
  if (item.GetState() == DownloadItem::COMPLETE &&
      !item.GetFileExternallyRemoved()) {
    watcher.DeleteFileThenRemove(item);
    return;
  }
  item.Remove();
}

// Exactly one of the runners is set: most actions need only the item, the
// rest also the profile's watcher (its manager, prefs or weak pointer).
struct ShellDownloadAction {
  std::string_view name;
  void (*run_on_item)(DownloadItem&);
  void (*run_with_watcher)(DownloadsWatcher&, DownloadItem&);
};

constexpr ShellDownloadAction kShellDownloadActions[] = {
    {"pause", &PauseDownload, nullptr},
    {"resume", &ResumeDownload, nullptr},
    {"cancel", &CancelDownload, nullptr},
    {"keepDangerous", &KeepDangerousDownload, nullptr},
    {"discardDangerous", &DiscardDangerousDownload, nullptr},
    {"retry", nullptr, &RetryDownload},
    {"remove", nullptr, &RemoveDownload},
    {"removeAndDeleteFile", nullptr, &RemoveDownloadAndFile},
};

const ShellDownloadAction* FindAction(std::string_view name) {
  const auto* it = std::ranges::find(kShellDownloadActions, name,
                                     &ShellDownloadAction::name);
  return it == std::ranges::end(kShellDownloadActions) ? nullptr : it;
}

// --- Commands. ------------------------------------------------------------

void ListDownloads(const ShellCommandContext& context,
                   const base::DictValue& command) {
  DownloadsWatcher* watcher = Watchers().Get(context.profile);
  content::DownloadManager::DownloadVector all;
  if (watcher && watcher->manager()) {
    watcher->manager()->GetAllDownloads(&all);
  }
  std::vector<const DownloadItem*> shown;
  for (const DownloadItem* item : all) {
    if (IsUserVisibleDownload(*item)) {
      shown.push_back(item);
    }
  }
  std::ranges::sort(shown, [](const DownloadItem* a, const DownloadItem* b) {
    return a->GetStartTime() > b->GetStartTime();
  });

  base::ListValue items;
  for (const DownloadItem* item : shown) {
    items.Append(ToShellDownloadItem(*item, /*removed=*/false));
  }
  base::DictValue event;
  event.Set("event", kDownloadListEvent);
  event.Set("requestId", ReadRequestId(command));
  event.Set("items", std::move(items));
  ReplyToShell(context, std::move(event));
}

void RunDownloadAction(const ShellCommandContext& context,
                       const base::DictValue& command) {
  const std::string* id = command.FindString("id");
  const std::string* action = command.FindString("action");
  const ShellDownloadAction* entry = action ? FindAction(*action) : nullptr;
  if (!id || !entry) {
    LOG(WARNING) << "OHOS shell downloads: bad downloadAction "
                 << (action ? *action : std::string("(none)"));
    return;
  }
  DownloadsWatcher* watcher = Watchers().Get(context.profile);
  DownloadItem* item = watcher ? watcher->Find(*id) : nullptr;
  if (!item || !IsUserVisibleDownload(*item)) {
    LOG(WARNING) << "OHOS shell downloads: no download " << *id;
    return;
  }
  if (entry->run_on_item) {
    entry->run_on_item(*item);
  } else {
    entry->run_with_watcher(*watcher, *item);
  }
}

}  // namespace

bool HandleDownloadsCommand(const ShellCommandContext& context,
                            std::string_view name,
                            const base::DictValue& command) {
  if (name == kListDownloadsCommand) {
    ListDownloads(context, command);
    return true;
  }
  if (name == kDownloadActionCommand) {
    RunDownloadAction(context, command);
    return true;
  }
  if (name == kSetDownloadDirectoryCommand) {
    const std::string* uri = command.FindString("uri");
    if (!uri || !SetShellDownloadDirectory(context.profile, *uri)) {
      LOG(WARNING) << "OHOS shell downloads: bad setDownloadDirectory";
    }
    return true;
  }
  return false;
}

void EnsureDownloadsObserver(Profile* profile) {
  if (ManagerFor(profile)) {
    Watchers().Get(profile);
  }
}

}  // namespace chrome::ohos
