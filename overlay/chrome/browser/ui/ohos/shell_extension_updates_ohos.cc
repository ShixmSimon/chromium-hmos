// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/ohos/shell_extension_updates_ohos.h"

#include <cmath>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/callback_list.h"
#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "base/supports_user_data.h"
#include "base/task/bind_post_task.h"
#include "base/timer/timer.h"
#include "base/unguessable_token.h"
#include "base/version.h"
#include "chrome/browser/extensions/extension_management.h"
#include "chrome/browser/extensions/updater/extension_updater.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_observer.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/ohos/aura_shell_runtime_bridge.h"
#include "chrome/browser/ui/ohos/extension_update_result_ohos.h"
#include "extensions/browser/delayed_install_manager.h"
#include "extensions/browser/disable_reason.h"
#include "extensions/browser/extension_prefs.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/browser/extension_registry_observer.h"
#include "extensions/browser/extension_system.h"
#include "extensions/browser/management_policy.h"
#include "extensions/browser/ui_util.h"
#include "extensions/common/extension.h"
#include "extensions/common/manifest.h"

namespace chrome::ohos {
namespace {

constexpr char kUpdateCoordinatorKey[] = "ohos-extension-updates-v1";

bool Ready(Profile* profile) {
  if (!profile || profile->IsOffTheRecord() || profile->IsGuestSession() ||
      profile->IsSystemProfile() || profile->ShutdownStarted()) {
    return false;
  }
  auto* system = extensions::ExtensionSystem::Get(profile);
  auto* updater = extensions::ExtensionUpdater::Get(profile);
  return system && system->is_ready() && updater &&
         updater->ready_for_manual_updates();
}

std::optional<double> RequestId(const base::DictValue& command) {
  auto id = command.FindDouble("requestId");
  if (!id || !std::isfinite(*id) || *id < 0 || *id > 9007199254740991.0 ||
      std::floor(*id) != *id) {
    return std::nullopt;
  }
  return id;
}

void SendFailure(gfx::AcceleratedWidget widget,
                 double request_id,
                 const std::string& context_id,
                 std::string_view error) {
  base::DictValue event;
  event.Set("event", "extensionUpdateProgress");
  event.Set("requestId", request_id);
  event.Set("contextId", context_id);
  event.Set("sequence", 1);
  event.Set("finished", true);
  event.Set("error", error);
  event.Set("items", base::ListValue());
  DispatchAuraShellRuntimeEventToWidget(widget, std::move(event));
}

// Owned by exactly one non-OTR Profile, not a process-global raw-pointer map.
// The task outlives settings pages/windows. Subscribers do not.
class UpdateCoordinator : public base::SupportsUserData::Data,
                          public ProfileObserver,
                          public extensions::ExtensionRegistryObserver {
 public:
  explicit UpdateCoordinator(Profile* profile) : profile_(profile) {
    profile_observation_.Observe(profile);
    registry_observation_.Observe(extensions::ExtensionRegistry::Get(profile));
  }

  static UpdateCoordinator* Get(Profile* profile) {
    auto* data = profile->GetUserData(&kUpdateCoordinatorKey);
    if (!data) {
      auto coordinator = std::make_unique<UpdateCoordinator>(profile);
      data = coordinator.get();
      profile->SetUserData(&kUpdateCoordinatorKey, std::move(coordinator));
    }
    return static_cast<UpdateCoordinator*>(data);
  }

  const std::string& context_id() const { return context_id_; }

  void Subscribe(gfx::AcceleratedWidget widget,
                 BrowserWindowInterface* browser,
                 double request_id) {
    // A retransmission uses the same sequence counter; a retry with a new ID
    // joins this task without calling the updater again.
    for (auto& [token, subscriber] : subscribers_) {
      if (subscriber.browser.get() == browser &&
          subscriber.request_id == request_id) {
        Send(subscriber);
        return;
      }
    }
    const int token = next_subscriber_++;
    auto& subscriber = subscribers_[token];
    subscriber.widget = widget;
    subscriber.browser = browser->GetWeakPtr();
    subscriber.request_id = request_id;
    subscriber.closed = browser->RegisterBrowserDidClose(
        base::BindRepeating(&UpdateCoordinator::RemoveSubscriber,
                            weak_factory_.GetWeakPtr(), token));
    if (running_) {
      Send(subscriber);
      if (timed_out_) {
        subscribers_.erase(token);
      }
    } else {
      Start();
    }
  }

  void StartNative(base::OnceClosure callback) {
    if (running_ && timed_out_) {
      std::move(callback).Run();
      return;
    }
    native_callbacks_.push_back(std::move(callback));
    if (!running_) {
      Start();
    }
  }

 private:
  struct Item {
    base::Version original_version;
    std::string version;
    std::string status = "checking";
    bool removed = false;
  };
  struct Subscriber {
    gfx::AcceleratedWidget widget = gfx::kNullAcceleratedWidget;
    base::WeakPtr<BrowserWindowInterface> browser;
    double request_id = 0;
    int sequence = 0;
    base::CallbackListSubscription closed;
  };

  void RemoveSubscriber(int token, BrowserWindowInterface*) {
    subscribers_.erase(token);
  }

  void Start() {
    items_.clear();
    error_.clear();
    running_ = true;
    finished_ = false;
    timed_out_ = false;
    extensions::ExtensionUpdater::CheckParams params;
    auto* registry = extensions::ExtensionRegistry::Get(profile_);
    auto* policy =
        extensions::ExtensionSystem::Get(profile_)->management_policy();
    auto* management =
        extensions::ExtensionManagementFactory::GetForBrowserContext(profile_);
    for (const auto& extension : registry->GenerateInstalledExtensionsSet()) {
      if (!extensions::ui_util::ShouldDisplayInExtensionSettings(*extension)) {
        continue;
      }
      Item& item = items_[extension->id()];
      item.original_version = extension->version();
      item.version = extension->VersionString();
      if (!extensions::Manifest::IsAutoUpdateableLocation(
              extension->location()) ||
          (management->GetEffectiveUpdateURL(*extension).is_empty() &&
           !extension->from_webstore())) {
        item.status = "manual";
      } else if (registry->blocked_extensions().Contains(extension->id()) ||
                 !policy->UserMayModifySettings(extension.get(), nullptr)) {
        item.status = "failed";
      } else {
        params.ids.push_back(extension->id());
      }
    }
    Publish();
    if (params.ids.empty()) {
      // Empty CheckParams::ids means ALL extensions, not none.
      Complete();
      return;
    }
    params.fetch_priority = extensions::DownloadFetchPriority::kForeground;
    params.install_immediately = true;
    params.stage_callback = base::BindRepeating(&UpdateCoordinator::OnStage,
                                                weak_factory_.GetWeakPtr());
    // Updater may synchronously complete a filtered request. Finish on the
    // next UI task so no subscriber can reenter its requests map on that stack.
    params.callback = base::BindPostTaskToCurrentDefault(base::BindOnce(
        &UpdateCoordinator::Complete, weak_factory_.GetWeakPtr()));
    deadline_.Start(FROM_HERE, base::Seconds(170),
                    base::BindOnce(&UpdateCoordinator::Timeout,
                                   weak_factory_.GetWeakPtr()));
    extensions::ExtensionUpdater::Get(profile_)->CheckNow(std::move(params));
  }

  void Reconcile(const std::string& id, Item& item, bool finished) {
    if (!profile_) {
      item.status = "failed";
      return;
    }
    auto* registry = extensions::ExtensionRegistry::Get(profile_);
    const auto* installed = registry->GetInstalledExtension(id);
    const bool newer =
        installed && installed->version().CompareTo(item.original_version) > 0;
    const bool permissions =
        extensions::ExtensionPrefs::Get(profile_)->HasDisableReason(
            id, extensions::disable_reason::DISABLE_PERMISSIONS_INCREASE);
    const bool pending = extensions::DelayedInstallManager::Get(profile_)
                             ->GetPendingExtensionUpdate(id) != nullptr;
    item.status =
        ResolveExtensionUpdateResult(item.status, item.removed || !installed,
                                     newer, permissions, pending, finished);
    if (installed && !item.removed) {
      item.version = installed->VersionString();
    }
  }

  void OnStage(const std::string& id, extensions::ExtensionUpdateStage stage) {
    auto it = items_.find(id);
    if (!running_ || it == items_.end() || it->second.removed) {
      return;
    }
    Item& item = it->second;
    using Stage = extensions::ExtensionUpdateStage;
    switch (stage) {
      case Stage::kChecking:
        item.status = "checking";
        break;
      case Stage::kDownloading:
        item.status = "downloading";
        break;
      case Stage::kInstalling:
      case Stage::kInstalled:
        item.status = "installing";
        break;
      case Stage::kUpToDate:
        item.status = "upToDate";
        break;
      case Stage::kFailed:
        item.status = "failed";
        break;
    }
    Reconcile(id, item, timed_out_);
    Publish();
  }

  void Complete() {
    if (!running_) {
      return;
    }
    deadline_.Stop();
    for (auto& [id, item] : items_) {
      Reconcile(id, item, true);
    }
    running_ = false;
    finished_ = true;
    Publish();
    subscribers_.clear();
    auto callbacks = std::move(native_callbacks_);
    for (auto& callback : callbacks) {
      std::move(callback).Run();
    }
  }

  void Timeout() {
    // Do not clear running_: Chromium is still doing this work. A retry must
    // not create another download/install, even after the shell's own timeout.
    timed_out_ = true;
    finished_ = true;
    error_ = "timeout";
    for (auto& [id, item] : items_) {
      Reconcile(id, item, true);
    }
    Publish();
    subscribers_.clear();
    auto callbacks = std::move(native_callbacks_);
    for (auto& callback : callbacks) {
      std::move(callback).Run();
    }
  }

  void Send(Subscriber& subscriber) {
    auto* browser = subscriber.browser.get();
    if (!browser || browser->IsDeleteScheduled() ||
        browser->GetProfile() != profile_) {
      return;
    }
    base::ListValue items;
    for (const auto& [id, item] : items_) {
      base::DictValue value;
      value.Set("id", id);
      value.Set("status", item.status);
      value.Set("version", item.version);
      items.Append(std::move(value));
    }
    base::DictValue event;
    event.Set("event", "extensionUpdateProgress");
    event.Set("requestId", subscriber.request_id);
    event.Set("contextId", context_id_);
    event.Set("sequence", ++subscriber.sequence);
    event.Set("finished", finished_);
    event.Set("error", error_);
    event.Set("items", std::move(items));
    DispatchAuraShellRuntimeEventToWidget(subscriber.widget, std::move(event));
  }

  void Publish() {
    for (auto& [token, subscriber] : subscribers_) {
      Send(subscriber);
    }
  }

  void OnExtensionInstalled(content::BrowserContext*,
                            const extensions::Extension* extension,
                            bool is_update) override {
    if (!profile_ || !is_update) {
      return;
    }
    auto it = items_.find(extension->id());
    if (running_ && it != items_.end()) {
      Reconcile(it->first, it->second, timed_out_);
      Publish();
    }
    base::DictValue event;
    event.Set("event", "extensionActionsChanged");
    DispatchAuraShellRuntimeEventToProfile(profile_, event);
  }

  void OnExtensionUninstalled(content::BrowserContext*,
                              const extensions::Extension* extension,
                              extensions::UninstallReason) override {
    if (auto it = items_.find(extension->id());
        running_ && it != items_.end()) {
      // Sticky even if a new install with the same ID arrives before this
      // request finishes. Native installers also reject uninstalled IDs.
      it->second.removed = true;
      it->second.status = "failed";
      Publish();
    }
  }

  void OnProfileWillBeDestroyed(Profile*) override {
    error_ = "invalidContext";
    for (auto& [id, item] : items_) {
      item.removed = true;
    }
    Complete();
    weak_factory_.InvalidateWeakPtrs();
    registry_observation_.Reset();
    profile_observation_.Reset();
    profile_ = nullptr;
  }

  void OnShutdown(extensions::ExtensionRegistry*) override {
    registry_observation_.Reset();
  }

  raw_ptr<Profile> profile_;
  const std::string context_id_ = base::UnguessableToken::Create().ToString();
  bool running_ = false;
  bool finished_ = false;
  bool timed_out_ = false;
  std::string error_;
  int next_subscriber_ = 0;
  std::map<std::string, Item> items_;
  std::map<int, Subscriber> subscribers_;
  std::vector<base::OnceClosure> native_callbacks_;
  base::OneShotTimer deadline_;
  base::ScopedObservation<Profile, ProfileObserver> profile_observation_{this};
  base::ScopedObservation<extensions::ExtensionRegistry,
                          extensions::ExtensionRegistryObserver>
      registry_observation_{this};
  base::WeakPtrFactory<UpdateCoordinator> weak_factory_{this};
};

}  // namespace

bool StartNativeExtensionUpdates(Profile* profile,
                                 base::OnceClosure completed) {
  if (!Ready(profile)) {
    return false;
  }
  UpdateCoordinator::Get(profile)->StartNative(std::move(completed));
  return true;
}

bool HandleShellExtensionUpdateCommand(std::string_view name,
                                       gfx::AcceleratedWidget widget,
                                       BrowserWindowInterface* browser,
                                       const base::DictValue& command) {
  if (name != "getExtensionUpdateCapabilities" &&
      name != "checkExtensionUpdates") {
    return false;
  }
  auto request_id = RequestId(command);
  if (!request_id) {
    return true;
  }
  Profile* profile = browser ? browser->GetProfile() : nullptr;
  const bool ready = Ready(profile);
  auto* coordinator = ready ? UpdateCoordinator::Get(profile) : nullptr;
  if (name == "getExtensionUpdateCapabilities") {
    base::DictValue event;
    event.Set("event", "extensionUpdateCapabilities");
    event.Set("requestId", *request_id);
    event.Set("protocolVersion", 1);
    event.Set("supported", ready);
    event.Set("contextId", coordinator ? coordinator->context_id() : "");
    DispatchAuraShellRuntimeEventToWidget(widget, std::move(event));
    return true;
  }
  const auto* context_id = command.FindString("contextId");
  if (!coordinator || !context_id || *context_id != coordinator->context_id()) {
    SendFailure(widget, *request_id, context_id ? *context_id : "",
                ready || (profile && profile->IsOffTheRecord())
                    ? "invalidContext"
                    : "unavailable");
    return true;
  }
  coordinator->Subscribe(widget, browser, *request_id);
  return true;
}

}  // namespace chrome::ohos
