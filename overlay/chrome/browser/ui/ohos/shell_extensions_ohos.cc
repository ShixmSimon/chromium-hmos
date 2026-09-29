// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/ohos/shell_extensions_ohos.h"

#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <utility>

#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/no_destructor.h"
#include "base/scoped_observation.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/browser/extensions/extension_tab_util.h"
#include "chrome/browser/extensions/extension_uninstall_dialog.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/ohos/aura_shell_runtime_bridge.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "extensions/browser/disable_reason.h"
#include "extensions/browser/extension_action.h"
#include "extensions/browser/extension_action_manager.h"
#include "extensions/browser/extension_prefs.h"
#include "extensions/browser/extension_registrar.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/browser/extension_registry_observer.h"
#include "extensions/browser/extension_system.h"
#include "extensions/browser/image_loader.h"
#include "extensions/browser/management_policy.h"
#include "extensions/browser/permissions/scripting_permissions_modifier.h"
#include "extensions/browser/permissions_manager.h"
#include "extensions/browser/ui_util.h"
#include "extensions/browser/uninstall_reason.h"
#include "extensions/common/extension.h"
#include "extensions/common/manifest.h"
#include "extensions/common/icons/extension_icon_set.h"
#include "extensions/common/extension_resource.h"
#include "extensions/common/manifest_handlers/icons_handler.h"
#include "extensions/common/manifest_handlers/options_page_info.h"
#include "extensions/common/permissions/permission_message_provider.h"
#include "extensions/common/permissions/permission_set.h"
#include "extensions/common/permissions/permissions_data.h"
#include "ui/views/widget/widget.h"

namespace chrome::ohos {
namespace {

using extensions::Extension;
using extensions::ExtensionRegistry;

void Notify(Profile* profile, std::string_view name) {
  base::DictValue event;
  event.Set("event", name);
  DispatchAuraShellRuntimeEventToProfile(profile, event);
}

// A command that could not be carried out. The shell has no other way to
// learn that a switch it flipped did not move.
void DispatchFailed(gfx::AcceleratedWidget widget,
                    std::string_view command,
                    const std::string& id,
                    std::string_view reason,
                    const std::u16string& message = std::u16string()) {
  base::DictValue event;
  event.Set("event", "extensionCommandFailed");
  event.Set("command", command);
  event.Set("id", id);
  event.Set("reason", reason);
  event.Set("message", base::UTF16ToUTF8(message));
  DispatchAuraShellRuntimeEventToWidget(widget, std::move(event));
}

// The toolbar model says when an enabled extension changes, but a disabled
// one is not in it: switching one off, back on, or uninstalling it while
// off would otherwise leave the shell's list as it was. The registry sees
// all of them.
class RegistryWatcher : public extensions::ExtensionRegistryObserver {
 public:
  explicit RegistryWatcher(Profile* profile) : profile_(profile) {
    observation_.Observe(ExtensionRegistry::Get(profile));
  }
  RegistryWatcher(const RegistryWatcher&) = delete;
  RegistryWatcher& operator=(const RegistryWatcher&) = delete;
  ~RegistryWatcher() override = default;

  // extensions::ExtensionRegistryObserver:
  void OnExtensionLoaded(content::BrowserContext*, const Extension*) override {
    Notify(profile_, "extensionActionsChanged");
  }
  void OnExtensionUnloaded(content::BrowserContext*,
                           const Extension*,
                           extensions::UnloadedExtensionReason) override {
    Notify(profile_, "extensionActionsChanged");
  }
  void OnExtensionUninstalled(content::BrowserContext*,
                              const Extension*,
                              extensions::UninstallReason) override {
    Notify(profile_, "extensionActionsChanged");
  }
  void OnShutdown(ExtensionRegistry*) override { observation_.Reset(); }

 private:
  const raw_ptr<Profile> profile_;
  base::ScopedObservation<ExtensionRegistry,
                          extensions::ExtensionRegistryObserver>
      observation_{this};
};

void EnsureRegistryWatcher(Profile* profile) {
  static base::NoDestructor<
      std::map<Profile*, std::unique_ptr<RegistryWatcher>>>
      watchers;
  if (!profile || watchers->contains(profile)) {
    return;
  }
  watchers->emplace(profile, std::make_unique<RegistryWatcher>(profile));
}

// Why Chromium holds an extension disabled, by name. chrome.management
// reports most of these as "unknown", which is what the shell saw for two
// extensions nobody had switched off.
std::string_view DisableReasonName(
    extensions::disable_reason::DisableReason reason) {
  namespace reason_ns = extensions::disable_reason;
  switch (reason) {
    case reason_ns::DISABLE_USER_ACTION:
      return "userAction";
    case reason_ns::DISABLE_PERMISSIONS_INCREASE:
      return "permissionsIncrease";
    case reason_ns::DISABLE_RELOAD:
      return "reload";
    case reason_ns::DISABLE_UNSUPPORTED_REQUIREMENT:
      return "unsupportedRequirement";
    case reason_ns::DISABLE_SIDELOAD_WIPEOUT:
      return "sideloadWipeout";
    case reason_ns::DISABLE_NOT_VERIFIED:
      return "notVerified";
    case reason_ns::DISABLE_GREYLIST:
      return "greylist";
    case reason_ns::DISABLE_CORRUPTED:
      return "corrupted";
    case reason_ns::DISABLE_REMOTE_INSTALL:
      return "remoteInstall";
    case reason_ns::DISABLE_EXTERNAL_EXTENSION:
      return "externalExtension";
    case reason_ns::DISABLE_UPDATE_REQUIRED_BY_POLICY:
      return "updateRequiredByPolicy";
    case reason_ns::DISABLE_CUSTODIAN_APPROVAL_REQUIRED:
      return "custodianApprovalRequired";
    case reason_ns::DISABLE_BLOCKED_BY_POLICY:
      return "blockedByPolicy";
    case reason_ns::DISABLE_REINSTALL:
      return "reinstall";
    case reason_ns::DISABLE_NOT_ALLOWLISTED:
      return "notAllowlisted";
    case reason_ns::DISABLE_PUBLISHED_IN_STORE_REQUIRED_BY_POLICY:
      return "publishedInStoreRequiredByPolicy";
    case reason_ns::DISABLE_UNSUPPORTED_MANIFEST_VERSION:
      return "unsupportedManifestVersion";
    case reason_ns::DISABLE_UNSUPPORTED_DEVELOPER_EXTENSION:
      return "unsupportedDeveloperExtension";
    case reason_ns::DISABLE_BLOCKED_BY_CLOUD_POLICY_CHECK:
      return "blockedByCloudPolicyCheck";
    default:
      return "other";
  }
}

const Extension* FindExtension(Profile* profile, const std::string& id) {
  ExtensionRegistry* registry = ExtensionRegistry::Get(profile);
  return registry ? registry->GetExtensionById(id, ExtensionRegistry::EVERYTHING)
                  : nullptr;
}

// --- setExtensionEnabled. -------------------------------------------------

// The same checks chrome.management.setEnabled makes for the extensions
// page. Enabling does not grant anything: an extension disabled because its
// permissions grew stays disabled, and the failure says so, rather than
// taking the new permissions without the prompt that should come with them.
void SetEnabled(gfx::AcceleratedWidget widget,
                Profile* profile,
                const base::DictValue& command) {
  const std::string* id = command.FindString("id");
  const std::optional<bool> enabled = command.FindBool("enabled");
  const Extension* extension = id ? FindExtension(profile, *id) : nullptr;
  if (!extension || !enabled) {
    DispatchFailed(widget, "setExtensionEnabled", id ? *id : "", "notFound");
    return;
  }
  const extensions::ManagementPolicy* policy =
      extensions::ExtensionSystem::Get(profile)->management_policy();
  std::u16string error;
  if (!policy->UserMayModifySettings(extension, &error)) {
    DispatchFailed(widget, "setExtensionEnabled", *id, "notAllowed", error);
    return;
  }
  extensions::ExtensionRegistrar* registrar =
      extensions::ExtensionRegistrar::Get(profile);
  if (*enabled) {
    if (policy->MustRemainDisabled(extension, /*reason=*/nullptr)) {
      DispatchFailed(widget, "setExtensionEnabled", *id, "notAllowed");
      return;
    }
    registrar->EnableExtension(*id);
    if (!ExtensionRegistry::Get(profile)->enabled_extensions().Contains(*id)) {
      DispatchFailed(widget, "setExtensionEnabled", *id, "stillDisabled");
    }
  } else {
    if (policy->MustRemainEnabled(extension, &error)) {
      DispatchFailed(widget, "setExtensionEnabled", *id, "notAllowed", error);
      return;
    }
    registrar->DisableExtension(
        *id, {extensions::disable_reason::DISABLE_USER_ACTION});
  }
  Notify(profile, "extensionActionsChanged");
}

// --- uninstallExtension. --------------------------------------------------

// Chromium's own confirmation, as the extensions page shows it. Owns the
// dialog and goes when it closes; the dialog is still on the stack when it
// calls back, so the deletion waits a turn.
class UninstallRequest : public extensions::ExtensionUninstallDialog::Delegate {
 public:
  UninstallRequest(gfx::AcceleratedWidget widget,
                   Profile* profile,
                   std::string id)
      : widget_(widget), profile_(profile), id_(std::move(id)) {}
  UninstallRequest(const UninstallRequest&) = delete;
  UninstallRequest& operator=(const UninstallRequest&) = delete;
  ~UninstallRequest() override = default;

  void Start(gfx::NativeWindow parent,
             const scoped_refptr<const Extension>& extension) {
    dialog_ =
        extensions::ExtensionUninstallDialog::Create(profile_, parent, this);
    dialog_->ConfirmUninstall(
        extension, extensions::UNINSTALL_REASON_USER_INITIATED,
        extensions::UNINSTALL_SOURCE_CHROME_EXTENSIONS_PAGE);
  }

  // extensions::ExtensionUninstallDialog::Delegate:
  void OnExtensionUninstallDialogClosed(bool did_start_uninstall,
                                        const std::u16string& error) override {
    // Cancelled is not a failure the shell needs to hear about: the reader
    // chose it. An error is -- a policy that keeps the extension installed.
    // The dialog reports a cancel as an error too, in these exact words
    // (ExtensionUninstallDialog::OnDialogClosed), so they are what tells
    // the two apart.
    if (!did_start_uninstall && !error.empty() &&
        error != u"User canceled uninstall dialog") {
      DispatchFailed(widget_, "uninstallExtension", id_, "notAllowed", error);
    }
    base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(FROM_HERE,
                                                               this);
  }

 private:
  const gfx::AcceleratedWidget widget_;
  const raw_ptr<Profile> profile_;
  const std::string id_;
  std::unique_ptr<extensions::ExtensionUninstallDialog> dialog_;
};

void Uninstall(gfx::AcceleratedWidget widget,
               BrowserWindowInterface* browser,
               const base::DictValue& command) {
  Profile* profile = browser->GetProfile();
  const std::string* id = command.FindString("id");
  const Extension* extension = id ? FindExtension(profile, *id) : nullptr;
  if (!extension) {
    DispatchFailed(widget, "uninstallExtension", id ? *id : "", "notFound");
    return;
  }
  BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(browser);
  gfx::NativeWindow parent = browser_view && browser_view->GetWidget()
                                 ? browser_view->GetWidget()->GetNativeWindow()
                                 : gfx::NativeWindow();
  // Owned by itself from here: see UninstallRequest.
  auto* request = new UninstallRequest(widget, profile, *id);
  request->Start(parent, extension);
}

// --- Site access. ---------------------------------------------------------

std::unique_ptr<const extensions::PermissionSet> OrEmpty(
    std::unique_ptr<const extensions::PermissionSet> permissions) {
  return permissions ? std::move(permissions)
                     : std::make_unique<extensions::PermissionSet>();
}

// The three answers chrome://extensions gives, read the way it reads them
// (ExtensionInfoGenerator's CreateRuntimeHostPermissionsInfo).
std::string_view SiteAccess(Profile* profile, const Extension& extension) {
  if (!extensions::PermissionsManager::Get(profile)->HasWithheldHostPermissions(
          extension)) {
    return "allSites";
  }
  std::unique_ptr<const extensions::PermissionSet> granted =
      OrEmpty(extensions::ExtensionPrefs::Get(profile)
                  ->GetRuntimeGrantedPermissions(extension.id()));
  if (granted->effective_hosts().is_empty()) {
    return "onClick";
  }
  return granted->ShouldWarnAllHosts(/*include_api_permissions=*/false)
             ? "allSites"
             : "specificSites";
}

void SetSiteAccess(gfx::AcceleratedWidget widget,
                   Profile* profile,
                   const base::DictValue& command) {
  const std::string* id = command.FindString("id");
  const std::string* mode = command.FindString("mode");
  const Extension* extension = id ? FindExtension(profile, *id) : nullptr;
  if (!extension || !mode) {
    DispatchFailed(widget, "setExtensionSiteAccess", id ? *id : "",
                   "notFound");
    return;
  }
  extensions::PermissionsManager* manager =
      extensions::PermissionsManager::Get(profile);
  if (!manager->CanAffectExtension(*extension)) {
    DispatchFailed(widget, "setExtensionSiteAccess", *id, "notAllowed");
    return;
  }
  // As DeveloperPrivateUpdateExtensionConfigurationFunction does it.
  extensions::ScriptingPermissionsModifier modifier(profile, extension);
  if (*mode == "onClick") {
    modifier.SetWithholdHostPermissions(true);
    modifier.RemoveAllGrantedHostPermissions();
  } else if (*mode == "specificSites") {
    if (manager->HasBroadGrantedHostPermissions(*extension)) {
      modifier.RemoveBroadGrantedHostPermissions();
    }
    modifier.SetWithholdHostPermissions(true);
  } else if (*mode == "allSites") {
    modifier.SetWithholdHostPermissions(false);
  } else {
    DispatchFailed(widget, "setExtensionSiteAccess", *id, "badMode");
    return;
  }
  Notify(profile, "extensionActionsChanged");
}

// --- getExtensionDetails. -------------------------------------------------

// The permission lines chrome://extensions shows, already in the reader's
// language. Where site access can be changed the host lines are left out,
// as that page leaves them out: site access says it instead. A line with
// detail below it -- a list of sites, of devices -- keeps the detail on
// following lines of the same string.
base::ListValue PermissionLines(Profile* profile, const Extension& extension) {
  std::unique_ptr<const extensions::PermissionSet> granted =
      OrEmpty(extensions::ExtensionPrefs::Get(profile)->GetGrantedPermissions(
          extension.id()));
  const extensions::PermissionMessageProvider* provider =
      extensions::PermissionMessageProvider::Get();
  extensions::PermissionMessages messages;
  if (extensions::PermissionsManager::Get(profile)->CanAffectExtension(
          extension)) {
    extensions::PermissionSet api_only(
        granted->apis().Clone(), granted->manifest_permissions().Clone(),
        extensions::URLPatternSet(), extensions::URLPatternSet());
    messages = provider->GetPermissionMessages(
        provider->GetAllPermissionIDs(api_only, extension.GetType()));
  } else {
    messages = provider->GetPermissionMessages(
        provider->GetAllPermissionIDs(*granted, extension.GetType()));
  }
  base::ListValue lines;
  for (const extensions::PermissionMessage& message : messages) {
    std::u16string line = message.message();
    for (const std::u16string& detail : message.submessages()) {
      line += u"\n";
      line += detail;
    }
    lines.Append(base::UTF16ToUTF8(line));
  }
  return lines;
}

void SendDetails(gfx::AcceleratedWidget widget,
                 Profile* profile,
                 const base::DictValue& command) {
  const std::string* id = command.FindString("id");
  const Extension* extension = id ? FindExtension(profile, *id) : nullptr;
  base::DictValue event;
  event.Set("event", "extensionDetails");
  event.Set("requestId", command.FindInt("requestId").value_or(0));
  event.Set("id", id ? *id : std::string());
  if (extension) {
    event.Set("permissions", PermissionLines(profile, *extension));
    event.Set("siteAccess", SiteAccess(profile, *extension));
    event.Set("siteAccessChangeable",
              extensions::PermissionsManager::Get(profile)->CanAffectExtension(
                  *extension));
  } else {
    event.Set("permissions", base::ListValue());
    event.Set("siteAccess", "onClick");
    event.Set("siteAccessChangeable", false);
    event.Set("error", "notFound");
  }
  DispatchAuraShellRuntimeEventToWidget(widget, std::move(event));
}

// --- openExtensionOptions. ------------------------------------------------

// Upstream decides between a tab of its own and the dialog inside
// chrome://extensions; either way it is a tab here.
void OpenOptions(gfx::AcceleratedWidget widget,
                 BrowserWindowInterface* browser,
                 const base::DictValue& command) {
  const std::string* id = command.FindString("id");
  const Extension* extension =
      id ? FindExtension(browser->GetProfile(), *id) : nullptr;
  if (!extension ||
      !extensions::OptionsPageInfo::HasOptionsPage(extension)) {
    DispatchFailed(widget, "openExtensionOptions", id ? *id : "",
                   "notFound");
    return;
  }
  if (!extensions::ExtensionTabUtil::OpenOptionsPage(extension, browser)) {
    DispatchFailed(widget, "openExtensionOptions", *id, "failed");
  }
}

}  // namespace

gfx::Image ExtensionManifestIcon(Profile* profile,
                                 const Extension& extension,
                                 int size_px) {
  // Keyed on the version too, so an update's new icon is loaded afresh.
  using Key = std::tuple<const Profile*, std::string, std::string, int>;
  // Present and empty while a load is in flight, so it is started once.
  static base::NoDestructor<std::map<Key, gfx::Image>> icons;
  const Key key{profile, extension.id(), extension.VersionString(), size_px};
  auto it = icons->find(key);
  if (it != icons->end()) {
    return it->second;
  }
  icons->emplace(key, gfx::Image());
  const extensions::ExtensionResource resource =
      extensions::IconsInfo::GetIconResource(
          &extension, size_px, ExtensionIconSet::Match::kBigger);
  if (resource.empty()) {
    return gfx::Image();
  }
  // The loader belongs to the profile and drops the reply with it, so the
  // profile is still there when this runs.
  extensions::ImageLoader::Get(profile)->LoadImageAsync(
      &extension, resource, gfx::Size(size_px, size_px),
      base::BindOnce(
          [](Profile* profile, Key key, const gfx::Image& image) {
            (*icons)[key] = image;
            if (!image.IsEmpty()) {
              Notify(profile, "extensionActionsChanged");
            }
          },
          base::Unretained(profile), key));
  return gfx::Image();
}

void AddExtensionPageFields(Profile* profile,
                            const Extension& extension,
                            base::DictValue* item) {
  EnsureRegistryWatcher(profile);
  ExtensionRegistry* registry = ExtensionRegistry::Get(profile);
  const bool user_enabled =
      registry->enabled_extensions().Contains(extension.id()) ||
      registry->terminated_extensions().Contains(extension.id());
  item->Set("userEnabled", user_enabled);
  // For a disabled one, why: Chromium's own reasons, which may be several.
  // "userAction" is the reader's switch; anything else Chromium did.
  base::ListValue reasons;
  if (!user_enabled) {
    for (const extensions::disable_reason::DisableReason reason :
         extensions::ExtensionPrefs::Get(profile)->GetDisableReasons(
             extension.id())) {
      reasons.Append(DisableReasonName(reason));
    }
  }
  item->Set("disableReasons", std::move(reasons));
  extensions::ExtensionActionManager* actions =
      extensions::ExtensionActionManager::Get(profile);
  const extensions::ExtensionAction* action =
      actions ? actions->GetExtensionAction(extension) : nullptr;
  item->Set("hasPopup",
            action && action->HasPopup(extensions::ExtensionAction::
                                           kDefaultTabId));
  item->Set("optionsUrl",
            extensions::OptionsPageInfo::HasOptionsPage(&extension)
                ? extensions::OptionsPageInfo::GetOptionsPage(&extension)
                      .spec()
                : std::string());
  item->Set("version", extension.VersionString());
  // From the manifest, which has had its __MSG_ placeholders replaced in
  // the reader's language by the time the extension is loaded.
  const std::string* description =
      extension.manifest()->FindStringPath("description");
  item->Set("description", description ? *description : std::string());
  // A string in almost every manifest; an object ({"email": ...}) in a
  // few, which has no name to show.
  const std::string* author = extension.manifest()->FindStringPath("author");
  item->Set("author", author ? *author : std::string());
}

std::vector<scoped_refptr<const Extension>> DisabledExtensionsForPage(
    Profile* profile) {
  std::vector<scoped_refptr<const Extension>> disabled;
  ExtensionRegistry* registry =
      profile ? ExtensionRegistry::Get(profile) : nullptr;
  if (!registry) {
    return disabled;
  }
  for (const scoped_refptr<const Extension>& extension :
       registry->disabled_extensions()) {
    if (extensions::ui_util::ShouldDisplayInExtensionSettings(*extension)) {
      disabled.push_back(extension);
    }
  }
  return disabled;
}

bool HandleShellExtensionCommand(std::string_view name,
                                 gfx::AcceleratedWidget widget,
                                 BrowserWindowInterface* browser,
                                 const base::DictValue& command) {
  Profile* profile = browser ? browser->GetProfile() : nullptr;
  if (!profile) {
    return false;
  }
  EnsureRegistryWatcher(profile);
  if (name == "setExtensionEnabled") {
    SetEnabled(widget, profile, command);
  } else if (name == "uninstallExtension") {
    Uninstall(widget, browser, command);
  } else if (name == "getExtensionDetails") {
    SendDetails(widget, profile, command);
  } else if (name == "setExtensionSiteAccess") {
    SetSiteAccess(widget, profile, command);
  } else if (name == "openExtensionOptions") {
    OpenOptions(widget, browser, command);
  } else {
    return false;
  }
  return true;
}

}  // namespace chrome::ohos
