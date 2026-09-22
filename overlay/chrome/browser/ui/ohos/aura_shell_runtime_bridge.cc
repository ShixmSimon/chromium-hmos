// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/ohos/aura_shell_runtime_bridge.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "base/command_line.h"
#include "base/containers/span.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/memory/ref_counted_memory.h"
#include "base/memory/scoped_refptr.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/synchronization/lock.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/content_settings/host_content_settings_map_factory.h"
#include "chrome/browser/lifetime/application_lifetime.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/profiles/profile_manager_observer.h"
#include "chrome/browser/permissions/system/system_permission_common.h"
#include "chrome/browser/permissions/system/system_permission_settings_ohos.h"
#include "chrome/browser/ui/ohos/system_geolocation_source_ohos.h"
#include "services/device/public/cpp/geolocation/buildflags.h"
#include "services/device/public/cpp/geolocation/geolocation_system_permission_manager.h"
#include "services/device/public/cpp/geolocation/location_system_permission_status.h"
#include "components/content_settings/core/browser/content_settings_observer.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/content_settings/core/common/content_settings.h"
#include "components/content_settings/core/common/content_settings_types.h"
#include "chrome/common/chrome_switches.h"
#include "components/keep_alive_registry/keep_alive_types.h"
#include "components/keep_alive_registry/scoped_keep_alive.h"
#include "chrome/browser/printing/print_view_manager.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/global_browser_collection.h"
#include "chrome/browser/ui/side_panel/side_panel_entry_id.h"
#include "chrome/browser/ui/side_panel/side_panel_ui.h"
#include "chrome/browser/ui/tabs/tab_enums.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/side_panel/side_panel.h"
#include "chrome/browser/ui/views/toolbar/app_menu.h"
#include "chrome/browser/ui/web_applications/web_app_menu_model.h"
#include "chrome/browser/web_applications/web_app_provider.h"
#include "chrome/browser/web_applications/web_app_registrar.h"
#include "chrome/browser/web_applications/web_app_tab_helper.h"
#include "components/embedder_support/user_agent_utils.h"
#include "components/printing/browser/print_to_pdf/pdf_print_result.h"
#include "components/printing/browser/print_to_pdf/pdf_print_utils.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/page_navigator.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/content_switches.h"
#include "content/public/common/referrer.h"
#include "content/public/renderer/render_thread.h"
#include "skia/ext/font_utils.h"
#include "ui/aura/window.h"
#include "ui/aura/window_tree_host.h"
#include "ui/base/base_window.h"
#include "ui/base/models/button_menu_item_model.h"
#include "ui/base/models/menu_model.h"
#include "ui/base/page_transition_types.h"
#include "ui/base/pointer/touch_ui_controller.h"
#include "ui/base/resource/resource_bundle.h"
#include "ui/base/window_open_disposition.h"
#include "ui/native_theme/native_theme.h"
#include "ui/ozone/platform/ohos/ohos_event_source.h"
#include "ui/shell_dialogs/select_file_dialog_ohos.h"
#include "ui/views/controls/menu/menu_runner.h"
#include "ui/views/focus/focus_manager.h"
#include "ui/views/widget/widget.h"
#include "url/gurl.h"

namespace chrome::ohos {

namespace {

// A windowless runtime has no browser window to keep the process alive.
std::unique_ptr<ScopedKeepAlive>& WindowlessKeepAlive() {
  static base::NoDestructor<std::unique_ptr<ScopedKeepAlive>> keep_alive;
  return *keep_alive;
}

}  // namespace
namespace {

constexpr int kMaxBrowserLookupAttempts = 50;
constexpr base::TimeDelta kBrowserLookupDelay = base::Milliseconds(100);
constexpr base::TimeDelta kThemeFontRendererRestartDelay =
    base::Milliseconds(250);
constexpr base::TimeDelta kBrowserStatePollInterval = base::Milliseconds(200);

struct RuntimeBridgeState {
  base::Lock lock;
  scoped_refptr<base::SingleThreadTaskRunner> ui_task_runner GUARDED_BY(lock);
  std::optional<GURL> pending_url GUARDED_BY(lock);
  std::optional<std::string> pending_theme_font_id GUARDED_BY(lock);
  std::optional<std::string> requested_theme_font_id GUARDED_BY(lock);
  std::string ui_family GUARDED_BY(lock) = "mobile_phone";
  std::string color_scheme GUARDED_BY(lock) = "light";
  std::string print_output_directory GUARDED_BY(lock);
  AuraShellBrowserStateCallback browser_state_callback GUARDED_BY(lock);
  AuraShellDefaultBrowserStateCallback default_browser_state_callback
      GUARDED_BY(lock);
  std::optional<bool> is_default_browser GUARDED_BY(lock);
  bool huawei_wallet_available GUARDED_BY(lock) = false;
  std::map<gfx::AcceleratedWidget, std::string> last_browser_state_json
      GUARDED_BY(lock);
  std::map<gfx::AcceleratedWidget, std::string> activation_target_signature
      GUARDED_BY(lock);
  std::map<gfx::AcceleratedWidget, int> activation_epoch GUARDED_BY(lock);
  uint64_t browser_generation GUARDED_BY(lock) = 0;
  bool app_visible GUARDED_BY(lock) = true;
  bool app_focused GUARDED_BY(lock) = true;
  bool app_focus_applied GUARDED_BY(lock) = false;
  std::map<gfx::AcceleratedWidget, bool> window_visibility GUARDED_BY(lock);
  std::map<gfx::AcceleratedWidget, bool> window_focus GUARDED_BY(lock);
  bool pending_shutdown GUARDED_BY(lock) = false;
};

RuntimeBridgeState& GetState() {
  static base::NoDestructor<RuntimeBridgeState> state;
  return *state;
}

struct PwaMenuSession {
  int id = 0;
  gfx::AcceleratedWidget widget = gfx::kNullAcceleratedWidget;
  bool sheet = false;
  std::unique_ptr<WebAppMenuModel> model;
  // Keep the native menu after its model so destruction happens first.
  std::unique_ptr<AppMenu> popup;
};

std::map<int, std::unique_ptr<PwaMenuSession>>& GetPwaMenuSessions() {
  static base::NoDestructor<std::map<int, std::unique_ptr<PwaMenuSession>>>
      sessions;
  return *sessions;
}

int NextPwaMenuSessionId() {
  static int next_session_id = 1;
  if (next_session_id == std::numeric_limits<int>::max()) {
    next_session_id = 1;
  }
  return next_session_id++;
}

BrowserWindowInterface* GetActiveBrowser() {
  GlobalBrowserCollection* browsers = GlobalBrowserCollection::GetInstance();
  return browsers ? browsers->GetLastActiveBrowser() : nullptr;
}

gfx::AcceleratedWidget GetBrowserWidget(BrowserWindowInterface* browser) {
  if (!browser) {
    return gfx::kNullAcceleratedWidget;
  }
  BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(
      browser);
  views::Widget* widget = browser_view ? browser_view->GetWidget() : nullptr;
  aura::Window* native_window = widget ? widget->GetNativeWindow() : nullptr;
  return native_window && native_window->GetHost()
             ? native_window->GetHost()->GetAcceleratedWidget()
             : gfx::kNullAcceleratedWidget;
}

BrowserWindowInterface* FindBrowserForWidget(gfx::AcceleratedWidget widget) {
  if (widget == gfx::kNullAcceleratedWidget) {
    return GetActiveBrowser();
  }
  GlobalBrowserCollection* browsers = GlobalBrowserCollection::GetInstance();
  BrowserWindowInterface* match = nullptr;
  if (browsers) {
    browsers->ForEach([widget, &match](BrowserWindowInterface* browser) {
      if (GetBrowserWidget(browser) == widget) {
        match = browser;
        return false;
      }
      return true;
    });
  }
  return match;
}

BrowserWindowInterface* FindBrowserForWebContents(
    content::WebContents* contents) {
  if (!contents) {
    return nullptr;
  }
  GlobalBrowserCollection* browsers = GlobalBrowserCollection::GetInstance();
  BrowserWindowInterface* match = nullptr;
  if (browsers) {
    browsers->ForEach([contents, &match](BrowserWindowInterface* browser) {
      TabStripModel* tabs = browser->GetTabStripModel();
      if (tabs &&
          tabs->GetIndexOfWebContents(contents) != TabStripModel::kNoTab) {
        match = browser;
        return false;
      }
      return true;
    });
  }
  return match;
}

void DispatchRuntimeEvent(gfx::AcceleratedWidget widget,
                          base::DictValue event) {
  event.Set("version", 1);
  std::string event_json;
  if (!base::JSONWriter::Write(event, &event_json)) {
    return;
  }

  AuraShellBrowserStateCallback callback;
  {
    RuntimeBridgeState& state = GetState();
    base::AutoLock lock(state.lock);
    callback = state.browser_state_callback;
  }
  if (callback) {
    callback.Run(widget, event_json);
  }
}

void DispatchRuntimeEvent(base::DictValue event) {
  DispatchRuntimeEvent(gfx::kNullAcceleratedWidget, std::move(event));
}

const char* MenuItemTypeName(ui::MenuModel::ItemType type) {
  switch (type) {
    case ui::MenuModel::TYPE_COMMAND:
      return "command";
    case ui::MenuModel::TYPE_CHECK:
      return "check";
    case ui::MenuModel::TYPE_RADIO:
      return "radio";
    case ui::MenuModel::TYPE_SEPARATOR:
      return "separator";
    case ui::MenuModel::TYPE_BUTTON_ITEM:
      return "buttonRow";
    case ui::MenuModel::TYPE_SUBMENU:
      return "submenu";
    case ui::MenuModel::TYPE_ACTIONABLE_SUBMENU:
      return "actionableSubmenu";
    case ui::MenuModel::TYPE_HIGHLIGHTED:
      return "highlighted";
    case ui::MenuModel::TYPE_TITLE:
      return "title";
  }
  return "command";
}

base::ListValue SerializeButtonMenuItem(ui::ButtonMenuItemModel* model) {
  base::ListValue buttons;
  if (!model) {
    return buttons;
  }
  for (size_t index = 0; index < model->GetItemCount(); ++index) {
    base::DictValue button;
    switch (model->GetTypeAt(index)) {
      case ui::ButtonMenuItemModel::TYPE_SPACE:
        button.Set("type", "space");
        break;
      case ui::ButtonMenuItemModel::TYPE_BUTTON:
        button.Set("type", "button");
        break;
      case ui::ButtonMenuItemModel::TYPE_BUTTON_LABEL:
        button.Set("type", "label");
        break;
    }
    button.Set("commandId", model->GetCommandIdAt(index));
    button.Set("label", base::UTF16ToUTF8(model->GetLabelAt(index)));
    button.Set("enabled", model->IsEnabledAt(index));
    button.Set("dismissesMenu", model->DismissesMenuAt(index));
    buttons.Append(std::move(button));
  }
  return buttons;
}

base::ListValue SerializeMenuModel(ui::MenuModel* model) {
  base::ListValue items;
  if (!model) {
    return items;
  }
  for (size_t index = 0; index < model->GetItemCount(); ++index) {
    if (!model->IsVisibleAt(index)) {
      continue;
    }
    const ui::MenuModel::ItemType type = model->GetTypeAt(index);
    base::DictValue item;
    item.Set("type", MenuItemTypeName(type));
    item.Set("commandId", model->GetCommandIdAt(index));
    item.Set("label", base::UTF16ToUTF8(model->GetLabelAt(index)));
    item.Set("secondaryLabel",
             base::UTF16ToUTF8(model->GetSecondaryLabelAt(index)));
    item.Set("minorText", base::UTF16ToUTF8(model->GetMinorTextAt(index)));
    item.Set("enabled", model->IsEnabledAt(index));
    item.Set("checked", model->IsItemCheckedAt(index));
    item.Set("groupId", model->GetGroupIdAt(index));
    if (type == ui::MenuModel::TYPE_SEPARATOR) {
      item.Set("separatorType",
               static_cast<int>(model->GetSeparatorTypeAt(index)));
    } else if (type == ui::MenuModel::TYPE_BUTTON_ITEM) {
      item.Set("buttons",
               SerializeButtonMenuItem(model->GetButtonMenuItemAt(index)));
    } else if (type == ui::MenuModel::TYPE_SUBMENU ||
               type == ui::MenuModel::TYPE_ACTIONABLE_SUBMENU) {
      item.Set("children", SerializeMenuModel(model->GetSubmenuModelAt(index)));
    }
    items.Append(std::move(item));
  }
  return items;
}

bool ActivateMenuCommand(ui::MenuModel* model,
                         int command_id,
                         int event_flags) {
  if (!model) {
    return false;
  }
  for (size_t index = 0; index < model->GetItemCount(); ++index) {
    if (!model->IsVisibleAt(index)) {
      continue;
    }
    const ui::MenuModel::ItemType type = model->GetTypeAt(index);
    if (type == ui::MenuModel::TYPE_BUTTON_ITEM) {
      ui::ButtonMenuItemModel* buttons = model->GetButtonMenuItemAt(index);
      if (!buttons) {
        continue;
      }
      for (size_t button_index = 0; button_index < buttons->GetItemCount();
           ++button_index) {
        if (buttons->GetCommandIdAt(button_index) == command_id &&
            buttons->IsEnabledAt(button_index)) {
          buttons->ActivatedAt(button_index);
          return true;
        }
      }
      continue;
    }
    if (model->GetCommandIdAt(index) == command_id &&
        model->IsEnabledAt(index) && type != ui::MenuModel::TYPE_SEPARATOR &&
        type != ui::MenuModel::TYPE_TITLE) {
      model->ActivatedAt(index, event_flags);
      return true;
    }
    if ((type == ui::MenuModel::TYPE_SUBMENU ||
         type == ui::MenuModel::TYPE_ACTIONABLE_SUBMENU) &&
        ActivateMenuCommand(model->GetSubmenuModelAt(index), command_id,
                            event_flags)) {
      return true;
    }
  }
  return false;
}

void DispatchPwaMenuClosed(gfx::AcceleratedWidget widget,
                           int session_id,
                           bool activated) {
  ui::OhosEventSource::ResetPointerCaptures(widget);
  base::DictValue event;
  event.Set("event", "pwaMenuClosed");
  event.Set("menuSessionId", session_id);
  event.Set("activated", activated);
  DispatchRuntimeEvent(widget, std::move(event));
}

void FinishPwaMenuSession(int session_id, bool activated) {
  auto& sessions = GetPwaMenuSessions();
  auto found = sessions.find(session_id);
  if (found == sessions.end()) {
    return;
  }
  std::unique_ptr<PwaMenuSession> session = std::move(found->second);
  sessions.erase(found);
  if (session->sheet && session->model) {
    session->model->MenuWillClose();
  }
  const gfx::AcceleratedWidget widget = session->widget;
  session.reset();
  DispatchPwaMenuClosed(widget, session_id, activated);
}

void SchedulePwaMenuSessionFinish(int session_id) {
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&FinishPwaMenuSession, session_id, false));
}

void ClosePwaMenuSessionsForWidget(gfx::AcceleratedWidget widget) {
  std::vector<int> sheet_sessions;
  std::vector<AppMenu*> popup_menus;
  for (const auto& [session_id, session] : GetPwaMenuSessions()) {
    if (session->widget != widget) {
      continue;
    }
    if (session->sheet) {
      sheet_sessions.push_back(session_id);
    } else if (session->popup) {
      popup_menus.push_back(session->popup.get());
    }
  }
  for (AppMenu* popup : popup_menus) {
    popup->CloseMenu();
  }
  for (int session_id : sheet_sessions) {
    FinishPwaMenuSession(session_id, false);
  }
}

std::unique_ptr<WebAppMenuModel> CreatePwaMenuModel(
    BrowserWindowInterface* browser,
    BrowserView* browser_view) {
  if (!browser || !browser_view) {
    return nullptr;
  }
  auto model = std::make_unique<WebAppMenuModel>(
      browser_view, browser);
  model->Init();
  return model;
}

void ShowPwaMenuSheet(gfx::AcceleratedWidget widget,
                      BrowserWindowInterface* browser,
                      BrowserView* browser_view) {
  ClosePwaMenuSessionsForWidget(widget);
  auto model = CreatePwaMenuModel(browser, browser_view);
  if (!model) {
    return;
  }
  model->MenuWillShow();

  auto session = std::make_unique<PwaMenuSession>();
  session->id = NextPwaMenuSessionId();
  session->widget = widget;
  session->sheet = true;
  session->model = std::move(model);
  const int session_id = session->id;

  base::DictValue event;
  event.Set("event", "pwaMenuModel");
  event.Set("presentation", "sheet");
  event.Set("menuSessionId", session_id);
  event.Set("items", SerializeMenuModel(session->model.get()));
  GetPwaMenuSessions().emplace(session_id, std::move(session));
  DispatchRuntimeEvent(widget, std::move(event));
}

gfx::Rect GetPwaMenuAnchor(const base::DictValue& command,
                           views::Widget* parent) {
  const gfx::Rect window_bounds = parent->GetWindowBoundsInScreen();
  const int width = std::max(1, command.FindInt("anchorWidth").value_or(36));
  const int height = std::max(1, command.FindInt("anchorHeight").value_or(36));
  const int default_x = std::max(window_bounds.x(), window_bounds.right() - 52);
  const int default_y = window_bounds.y() + 8;
  return gfx::Rect(command.FindInt("anchorX").value_or(default_x),
                   command.FindInt("anchorY").value_or(default_y), width,
                   height);
}

void ShowPwaMenuPopup(gfx::AcceleratedWidget widget,
                      BrowserWindowInterface* browser,
                      BrowserView* browser_view,
                      const base::DictValue& command) {
  views::Widget* parent = browser_view ? browser_view->GetWidget() : nullptr;
  if (!parent) {
    return;
  }
  ClosePwaMenuSessionsForWidget(widget);
  auto model = CreatePwaMenuModel(browser, browser_view);
  if (!model) {
    return;
  }

  auto session = std::make_unique<PwaMenuSession>();
  session->id = NextPwaMenuSessionId();
  session->widget = widget;
  session->model = std::move(model);
  const int session_id = session->id;
  session->popup = std::make_unique<AppMenu>(
      browser, session->model.get(),
      views::MenuRunner::NO_FLAGS,
      base::BindRepeating(&SchedulePwaMenuSessionFinish, session_id));
  AppMenu* popup = session->popup.get();
  GetPwaMenuSessions().emplace(session_id, std::move(session));
  popup->RunMenu(parent, GetPwaMenuAnchor(command, parent));
}

void ActivatePwaMenuSession(int session_id, int command_id, int event_flags) {
  auto found = GetPwaMenuSessions().find(session_id);
  if (found == GetPwaMenuSessions().end() || !found->second->sheet ||
      !found->second->model) {
    return;
  }
  const bool activated =
      ActivateMenuCommand(found->second->model.get(), command_id, event_flags);
  FinishPwaMenuSession(session_id, activated);
}

void DispatchPrintFailure(gfx::AcceleratedWidget widget, std::string reason) {
  base::DictValue event;
  event.Set("event", "systemPrintFailed");
  event.Set("reason", std::move(reason));
  DispatchRuntimeEvent(widget, std::move(event));
}

const char* FilePickerTypeName(ui::SelectFileDialog::Type type) {
  switch (type) {
    case ui::SelectFileDialog::SELECT_FOLDER:
    case ui::SelectFileDialog::SELECT_UPLOAD_FOLDER:
    case ui::SelectFileDialog::SELECT_EXISTING_FOLDER:
      return "folder";
    case ui::SelectFileDialog::SELECT_SAVEAS_FILE:
      return "save";
    case ui::SelectFileDialog::SELECT_OPEN_MULTI_FILE:
      return "openMultiple";
    case ui::SelectFileDialog::SELECT_OPEN_FILE:
      return "open";
    case ui::SelectFileDialog::SELECT_NONE:
      return "none";
  }
}

void DispatchFilePickerRequest(const ui::OhosSelectFileDialogRequest& request) {
  base::DictValue event;
  event.Set("event", "filePickerRequested");
  event.Set("requestId", request.request_id);
  event.Set("pickerType", FilePickerTypeName(request.type));
  event.Set("title", request.title);
  event.Set("defaultName", request.default_name);
  event.Set("fileTypeIndex", request.file_type_index);
  base::ListValue filters;
  for (const std::string& filter : request.suffix_filters) {
    filters.Append(filter);
  }
  event.Set("suffixFilters", std::move(filters));
  DispatchRuntimeEvent(std::move(event));
}

// Declaring a permission in module.json5 only makes it requestable; until
// something asks the user, the process holds none of them and every
// hardware-backed web API fails as though the platform had no backend. Asking
// for all of them at startup is the other extreme -- the user meets four
// prompts before opening a page. These are the permissions each web permission
// actually needs, requested when the site is granted the web one.
std::vector<const char*> OhosPermissionsFor(ContentSettingsType content_type) {
  switch (content_type) {
    case ContentSettingsType::GEOLOCATION:
      return {"ohos.permission.LOCATION",
              "ohos.permission.APPROXIMATELY_LOCATION"};
    case ContentSettingsType::MEDIASTREAM_CAMERA:
      return {"ohos.permission.CAMERA"};
    case ContentSettingsType::MEDIASTREAM_MIC:
      return {"ohos.permission.MICROPHONE"};
    case ContentSettingsType::SENSORS:
      return {"ohos.permission.ACCELEROMETER", "ohos.permission.GYROSCOPE"};
    default:
      return {};
  }
}

constexpr ContentSettingsType kOhosBackedPermissions[] = {
    ContentSettingsType::GEOLOCATION,
    ContentSettingsType::MEDIASTREAM_CAMERA,
    ContentSettingsType::MEDIASTREAM_MIC,
    ContentSettingsType::SENSORS,
};

std::set<std::string>& PermissionsAlreadyRequested() {
  static base::NoDestructor<std::set<std::string>> requested;
  return *requested;
}

// Keyed by the request id the shell echoes back, so a caller that asked for a
// specific permission is answered when that request finishes rather than when
// any of them does.
std::map<int, base::OnceClosure>& PendingPermissionRequests() {
  static base::NoDestructor<std::map<int, base::OnceClosure>> pending;
  return *pending;
}

int& NextPermissionRequestId() {
  static int id = 0;
  return id;
}

// Ask the shell for the permissions this content setting needs. Each is asked
// for at most once per run; re-prompting for one the user declined is nagging.
//
// Whether the process already holds one is left to the shell. The NDK can read
// that (OH_AT_CheckSelfPermission) but lives in libability_runtime, which this
// target does not link, and the check buys nothing:
// requestPermissionsFromUser returns a granted permission immediately without
// showing anything, and does not re-prompt for a denied one either.
void RequestOhosPermissionsFor(ContentSettingsType content_type,
                               base::OnceClosure done = base::OnceClosure()) {
  base::ListValue wanted;
  for (const char* permission : OhosPermissionsFor(content_type)) {
    if (!PermissionsAlreadyRequested().insert(permission).second) {
      continue;
    }
    wanted.Append(permission);
  }
  if (wanted.empty()) {
    if (done) {
      std::move(done).Run();
    }
    return;
  }
  const int request_id = ++NextPermissionRequestId();
  if (done) {
    PendingPermissionRequests()[request_id] = std::move(done);
  }
  base::DictValue event;
  event.Set("event", "permissionsRequested");
  event.Set("requestId", request_id);
  event.Set("permissions", std::move(wanted));
  DispatchRuntimeEvent(std::move(event));
}

// Watches every profile's content settings so that granting a site one of the
// web permissions above pulls in the HarmonyOS permission behind it.
class OhosWebPermissionWatcher : public content_settings::Observer,
                                 public ProfileManagerObserver {
 public:
  static OhosWebPermissionWatcher& GetInstance() {
    static base::NoDestructor<OhosWebPermissionWatcher> instance;
    return *instance;
  }

  void Start() {
    if (std::exchange(started_, true)) {
      return;
    }
    ProfileManager* manager =
        g_browser_process ? g_browser_process->profile_manager() : nullptr;
    if (!manager) {
      return;
    }
    manager->AddObserver(this);
    for (Profile* profile : manager->GetLoadedProfiles()) {
      OnProfileAdded(profile);
    }
  }

  // ProfileManagerObserver:
  void OnProfileAdded(Profile* profile) override {
    HostContentSettingsMap* map =
        HostContentSettingsMapFactory::GetForProfile(profile);
    if (!map || !observed_maps_.insert(map).second) {
      return;
    }
    map->AddObserver(this);
  }

  // content_settings::Observer:
  void OnContentSettingChanged(
      const ContentSettingsPattern& primary_pattern,
      const ContentSettingsPattern& secondary_pattern,
      ContentSettingsTypeSet content_type_set) override {
    for (ContentSettingsType content_type : kOhosBackedPermissions) {
      if (content_type_set.Contains(content_type) &&
          AnySiteAllows(content_type)) {
        RequestOhosPermissionsFor(content_type);
      }
    }
  }

 private:
  friend class base::NoDestructor<OhosWebPermissionWatcher>;

  OhosWebPermissionWatcher() = default;
  ~OhosWebPermissionWatcher() override = default;

  // The change notification does not carry the new value, and a change to
  // BLOCK is not a reason to ask the platform for anything.
  bool AnySiteAllows(ContentSettingsType content_type) const {
    for (HostContentSettingsMap* map : observed_maps_) {
      for (const ContentSettingPatternSource& setting :
           map->GetSettingsForOneType(content_type)) {
        if (setting.GetContentSetting() == CONTENT_SETTING_ALLOW) {
          return true;
        }
      }
    }
    return false;
  }

  bool started_ = false;
  std::set<HostContentSettingsMap*> observed_maps_;
};

bool WritePdfFile(const base::FilePath& path,
                  scoped_refptr<base::RefCountedMemory> data) {
  return data && data->size() > 0 && base::CreateDirectory(path.DirName()) &&
         base::WriteFile(
             path, base::span<const uint8_t>(data->front(), data->size()));
}

void OnPdfFileWritten(gfx::AcceleratedWidget widget,
                      base::FilePath path,
                      std::string title,
                      std::string url,
                      bool written) {
  if (!written) {
    DispatchPrintFailure(widget, "writePdfFailed");
    return;
  }
  base::DictValue event;
  event.Set("event", "systemPrintReady");
  event.Set("filePath", path.value());
  event.Set("title", std::move(title));
  event.Set("url", std::move(url));
  DispatchRuntimeEvent(widget, std::move(event));
}

void OnPdfGenerated(gfx::AcceleratedWidget widget,
                    base::FilePath path,
                    std::string title,
                    std::string url,
                    print_to_pdf::PdfPrintResult result,
                    scoped_refptr<base::RefCountedMemory> data) {
  if (result != print_to_pdf::PdfPrintResult::kPrintSuccess || !data) {
    DispatchPrintFailure(widget, print_to_pdf::PdfPrintResultToString(result));
    return;
  }
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&WritePdfFile, path, std::move(data)),
      base::BindOnce(&OnPdfFileWritten, widget, std::move(path),
                     std::move(title), std::move(url)));
}

bool IsMobileUiFamily(std::string_view ui_family) {
  return ui_family == "mobile_phone";
}

blink::UserAgentOverride BuildOhosUserAgentOverride(bool mobile) {
  blink::UserAgentOverride ua_override;
  ua_override.ua_string_override =
      embedder_support::GetUserAgentForOhos(mobile);
  ua_override.ua_metadata_override =
      embedder_support::GetUserAgentMetadataForOhos(mobile);
  return ua_override;
}

bool ApplyUserAgentToWebContents(content::WebContents* contents,
                                 bool mobile,
                                 bool reload) {
  if (!contents) {
    return false;
  }
  const blink::UserAgentOverride desired = BuildOhosUserAgentOverride(mobile);
  if (contents->GetUserAgentOverride() == desired) {
    return false;
  }

  contents->SetUserAgentOverride(desired, /*override_in_new_tabs=*/true);
  if (reload && contents->GetController().GetLastCommittedEntry()) {
    contents->GetController().Reload(content::ReloadType::NORMAL,
                                     /*check_for_repost=*/false);
  }
  return true;
}

void ApplyUserAgentToAllTabs(bool mobile, bool reload) {
  GlobalBrowserCollection* browsers = GlobalBrowserCollection::GetInstance();
  if (!browsers) {
    return;
  }
  browsers->ForEach([mobile, reload](BrowserWindowInterface* browser) {
    TabStripModel* tabs = browser->GetTabStripModel();
    if (!tabs) {
      return true;
    }
    for (int index = 0; index < tabs->count(); ++index) {
      ApplyUserAgentToWebContents(tabs->GetWebContentsAt(index), mobile,
                                  reload);
    }
    return true;
  });
}

struct BrowserTargetState {
  bool side_panel_visible = false;
  std::string side_panel_entry_id;
  std::string focused_target = "webContents";
  int activation_epoch = 0;
};

BrowserTargetState BuildBrowserTargetState(gfx::AcceleratedWidget widget,
                                           BrowserWindowInterface* browser,
                                           int active_tab_index) {
  BrowserTargetState target;
  if (SidePanelUI* side_panel_ui = SidePanelUI::From(browser)) {
    target.side_panel_visible = side_panel_ui->IsSidePanelShowing();
    if (target.side_panel_visible) {
      if (std::optional<SidePanelEntryId> entry_id =
              side_panel_ui->GetCurrentEntryId()) {
        target.side_panel_entry_id = SidePanelEntryIdToString(*entry_id);
      }
      BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(
          browser);
      SidePanel* side_panel =
          browser_view ? browser_view->side_panel() : nullptr;
      views::FocusManager* focus_manager =
          browser_view ? browser_view->GetFocusManager() : nullptr;
      views::View* focused_view =
          focus_manager ? focus_manager->GetFocusedView() : nullptr;
      if (side_panel && focused_view && side_panel->Contains(focused_view)) {
        target.focused_target = "sidePanel";
      }
    }
  }

  const std::string signature = base::NumberToString(active_tab_index) + ":" +
                                target.side_panel_entry_id + ":" +
                                target.focused_target;
  RuntimeBridgeState& state = GetState();
  base::AutoLock lock(state.lock);
  std::string& previous = state.activation_target_signature[widget];
  int& epoch = state.activation_epoch[widget];
  if (epoch == 0 || previous != signature) {
    previous = signature;
    epoch = epoch == std::numeric_limits<int>::max() ? 1 : epoch + 1;
  }
  target.activation_epoch = epoch;
  return target;
}

std::string BuildBrowserStateJson(std::string_view ui_family,
                                  gfx::AcceleratedWidget widget,
                                  BrowserWindowInterface* browser) {
  base::DictValue state;
  state.Set("version", 1);
  state.Set("mobileUi", IsMobileUiFamily(ui_family));
  state.Set("uiFamily", std::string(ui_family));
  state.Set("ready", false);
  state.Set("tabCount", 0);
  state.Set("activeTabIndex", -1);
  state.Set("sidePanelVisible", false);
  state.Set("sidePanelEntryId", "");
  state.Set("focusedTarget", "webContents");
  state.Set("activationEpoch", 0);
  state.Set("tabs", base::ListValue());
  state.Set("url", "");
  state.Set("domain", "");
  state.Set("title", "");
  state.Set("loading", false);
  state.Set("canGoBack", false);
  state.Set("canGoForward", false);
  state.Set("isPwaWindow", false);
  state.Set("pwaAppId", "");
  state.Set("pwaStartUrl", "");

  TabStripModel* tabs = browser ? browser->GetTabStripModel() : nullptr;
  if (!tabs) {
    state.Set("ready", false);
    std::string json;
    base::JSONWriter::Write(state, &json);
    return json;
  }

  state.Set("ready", true);
  const bool is_pwa_window =
      browser->GetType() == BrowserWindowInterface::Type::TYPE_APP ||
      browser->GetType() == BrowserWindowInterface::Type::TYPE_APP_POPUP;
  state.Set("isPwaWindow", is_pwa_window);
  state.Set("tabCount", tabs->count());
  state.Set("activeTabIndex", tabs->active_index());
  const BrowserTargetState target =
      BuildBrowserTargetState(widget, browser, tabs->active_index());
  state.Set("sidePanelVisible", target.side_panel_visible);
  state.Set("sidePanelEntryId", target.side_panel_entry_id);
  state.Set("focusedTarget", target.focused_target);
  state.Set("activationEpoch", target.activation_epoch);
  base::ListValue tab_values;
  for (int index = 0; index < tabs->count(); ++index) {
    content::WebContents* contents = tabs->GetWebContentsAt(index);
    base::DictValue tab;
    tab.Set("index", index);
    tab.Set("active", index == tabs->active_index());
    tab.Set("url", "");
    tab.Set("title", "");
    tab.Set("loading", false);
    if (contents) {
      const GURL url = contents->GetVisibleURL();
      tab.Set("url", url.spec());
      tab.Set("title", base::UTF16ToUTF8(contents->GetTitle()));
      tab.Set("loading", contents->IsLoading());
    }
    tab_values.Append(std::move(tab));
  }
  state.Set("tabs", std::move(tab_values));

  content::WebContents* active = tabs->GetActiveWebContents();
  if (active) {
    const GURL url = active->GetVisibleURL();
    state.Set("url", url.spec());
    state.Set("domain", url.host().empty() ? url.spec() : url.host());
    state.Set("title", base::UTF16ToUTF8(active->GetTitle()));
    state.Set("loading", active->IsLoading());
    state.Set("canGoBack", active->GetController().CanGoBack());
    state.Set("canGoForward", active->GetController().CanGoForward());
    if (is_pwa_window) {
      state.Set("pwaStartUrl", url.spec());
      if (const webapps::AppId* app_id =
              web_app::WebAppTabHelper::GetAppId(active)) {
        state.Set("pwaAppId", *app_id);
        if (web_app::WebAppProvider* provider =
                web_app::WebAppProvider::GetForWebApps(browser->GetProfile())) {
          state.Set(
              "pwaStartUrl",
              provider->registrar_unsafe().GetAppStartUrl(*app_id).spec());
        }
      }
    }
  }

  std::string json;
  base::JSONWriter::Write(state, &json);
  return json;
}

void PollBrowserStateOnUiThread(uint64_t generation) {
  std::string ui_family;
  {
    RuntimeBridgeState& state = GetState();
    base::AutoLock lock(state.lock);
    if (!state.ui_task_runner || state.browser_generation != generation) {
      return;
    }
    ui_family = state.ui_family;
  }

  const bool mobile = IsMobileUiFamily(ui_family);
  ApplyUserAgentToAllTabs(mobile, /*reload=*/false);

  std::vector<std::pair<gfx::AcceleratedWidget, std::string>> snapshots;
  if (GlobalBrowserCollection* browsers =
          GlobalBrowserCollection::GetInstance()) {
    browsers->ForEach(
        [&snapshots, &ui_family](BrowserWindowInterface* browser) {
          const gfx::AcceleratedWidget widget = GetBrowserWidget(browser);
          if (widget != gfx::kNullAcceleratedWidget) {
            snapshots.emplace_back(
                widget, BuildBrowserStateJson(ui_family, widget, browser));
          }
          return true;
        });
  }

  AuraShellBrowserStateCallback callback;
  scoped_refptr<base::SingleThreadTaskRunner> task_runner;
  std::vector<std::pair<gfx::AcceleratedWidget, std::string>> changed;
  {
    RuntimeBridgeState& state = GetState();
    base::AutoLock lock(state.lock);
    if (!state.ui_task_runner || state.browser_generation != generation) {
      return;
    }
    for (const auto& [widget, state_json] : snapshots) {
      std::string& previous = state.last_browser_state_json[widget];
      if (state_json != previous) {
        previous = state_json;
        changed.emplace_back(widget, state_json);
      }
    }
    callback = state.browser_state_callback;
    task_runner = state.ui_task_runner;
  }
  if (callback) {
    for (const auto& [widget, state_json] : changed) {
      callback.Run(widget, state_json);
    }
  }
  task_runner->PostDelayedTask(
      FROM_HERE, base::BindOnce(&PollBrowserStateOnUiThread, generation),
      kBrowserStatePollInterval);
}

void NavigateOnUiThread(gfx::AcceleratedWidget widget, GURL url, int attempt) {
  BrowserWindowInterface* browser = FindBrowserForWidget(widget);
  if (!browser) {
    if (attempt >= kMaxBrowserLookupAttempts) {
      LOG(ERROR) << "OHOS Aura shell could not find a Chromium browser window";
      return;
    }
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&NavigateOnUiThread, widget, std::move(url),
                       attempt + 1),
        kBrowserLookupDelay);
    return;
  }

  content::OpenURLParams params(
      url, content::Referrer(), WindowOpenDisposition::CURRENT_TAB,
      ui::PAGE_TRANSITION_TYPED, /*is_renderer_initiated=*/false);
  browser->OpenURL(params, {});
}

void ShutdownOnUiThread() {
  WindowlessKeepAlive().reset();
  chrome::ExitIgnoreUnloadHandlers();
}

void ApplyWindowStateOnUiThread(gfx::AcceleratedWidget widget, int attempt) {
  BrowserWindowInterface* browser = FindBrowserForWidget(widget);
  if (!browser || !browser->GetWindow()) {
    if (attempt >= kMaxBrowserLookupAttempts) {
      LOG(ERROR) << "OHOS Aura shell could not apply browser window state";
      return;
    }
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&ApplyWindowStateOnUiThread, widget, attempt + 1),
        kBrowserLookupDelay);
    return;
  }

  bool visible = true;
  bool focused = true;
  {
    RuntimeBridgeState& state = GetState();
    base::AutoLock lock(state.lock);
    const auto visibility = state.window_visibility.find(widget);
    const auto focus = state.window_focus.find(widget);
    visible = visibility == state.window_visibility.end() ? state.app_visible
                                                          : visibility->second;
    focused =
        focus == state.window_focus.end() ? state.app_focused : focus->second;
  }

  ui::BaseWindow* window = browser->GetWindow();
  if (!visible) {
    window->Deactivate();
    window->Hide();
    return;
  }

  window->ShowInactive();
  if (focused) {
    window->Activate();
    if (TabStripModel* tabs = browser->GetTabStripModel()) {
      if (content::WebContents* active = tabs->GetActiveWebContents()) {
        active->Focus();
      }
    }
  } else {
    window->Deactivate();
  }

  if (BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(
          browser)) {
    browser_view->InvalidateLayout();
    browser_view->SchedulePaint();
  }
}

void ApplyUiFamilyOnUiThread(std::string ui_family) {
  const bool mobile = IsMobileUiFamily(ui_family);
  const bool use_touch_ui = ui_family != "aura_pc";
  ui::TouchUiController* controller = ui::TouchUiController::Get();
  if (controller->touch_ui() == use_touch_ui) {
    controller->OnTabletModeToggled(!use_touch_ui);
  }
  controller->OnTabletModeToggled(use_touch_ui);

  ApplyUserAgentToAllTabs(mobile, /*reload=*/true);

  if (GlobalBrowserCollection* browsers =
          GlobalBrowserCollection::GetInstance()) {
    browsers->ForEach([](BrowserWindowInterface* browser) {
      if (BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(
              browser)) {
        browser_view->OnOhosUiFamilyChanged();
      }
      return true;
    });
  }

  LOG(INFO) << "OHOS Aura shell applied UI family " << ui_family
            << " touch_ui=" << use_touch_ui;
}

void ApplyColorSchemeOnUiThread(std::string color_scheme) {
  const ui::NativeTheme::PreferredColorScheme preferred_scheme =
      color_scheme == "dark" ? ui::NativeTheme::PreferredColorScheme::kDark
                             : ui::NativeTheme::PreferredColorScheme::kLight;

  ui::NativeTheme* native_theme = ui::NativeTheme::GetInstanceForNativeUi();
  if (native_theme &&
      native_theme->preferred_color_scheme() != preferred_scheme) {
    native_theme->set_preferred_color_scheme(preferred_scheme);
    native_theme->NotifyOnNativeThemeUpdated();
  }

  ui::NativeTheme* web_theme = ui::NativeTheme::GetInstanceForWeb();
  if (web_theme && web_theme != native_theme &&
      web_theme->preferred_color_scheme() != preferred_scheme) {
    web_theme->set_preferred_color_scheme(preferred_scheme);
    web_theme->NotifyOnNativeThemeUpdated();
  }

  if (GlobalBrowserCollection* browsers =
          GlobalBrowserCollection::GetInstance()) {
    browsers->ForEach([](BrowserWindowInterface* browser) {
      TabStripModel* tabs = browser->GetTabStripModel();
      if (tabs) {
        for (int index = 0; index < tabs->count(); ++index) {
          if (content::WebContents* contents = tabs->GetWebContentsAt(index)) {
            contents->NotifyPreferencesChanged();
          }
        }
      }
      if (BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(
              browser)) {
        browser_view->InvalidateLayout();
        browser_view->SchedulePaint();
      }
      return true;
    });
  }

  LOG(INFO) << "OHOS Aura shell applied color scheme " << color_scheme;
}

void ExecuteBrowserCommandOnUiThread(gfx::AcceleratedWidget widget,
                                     base::DictValue command) {
  const std::string* name = command.FindString("command");
  if (!name || *name == "requestState") {
    return;
  }

  if (*name == "filePickerResult") {
    const std::optional<int> request_id = command.FindInt("requestId");
    const base::ListValue* path_values = command.FindList("paths");
    std::vector<std::string> paths;
    if (path_values) {
      for (const base::Value& path : *path_values) {
        if (path.is_string()) {
          paths.push_back(path.GetString());
        }
      }
    }
    if (request_id) {
      ui::CompleteOhosSelectFileDialog(
          *request_id, paths, command.FindInt("fileTypeIndex").value_or(0),
          command.FindBool("canceled").value_or(paths.empty()));
    }
    return;
  }

#if BUILDFLAG(OS_LEVEL_GEOLOCATION_PERMISSION_SUPPORTED)
  if (*name == "systemPermissionState") {
    // The shell reports the app's own location permission: at startup, and
    // again whenever a request it ran changes it.
    const std::string* location = command.FindString("location");
    if (location) {
      ::system_permission_settings::SystemPermission state =
          ::system_permission_settings::SystemPermission::kNotDetermined;
      if (*location == "allowed") {
        state = ::system_permission_settings::SystemPermission::kAllowed;
      } else if (*location == "denied") {
        state = ::system_permission_settings::SystemPermission::kDenied;
      }
      ::system_permission_settings::SetOhosSystemPermission(
          ContentSettingsType::GEOLOCATION, state);
      SystemGeolocationSourceOhos::NotifyPermissionChanged();
    }
    return;
  }
#endif

  if (*name == "permissionResult") {
    const base::ListValue* granted_values = command.FindList("granted");
    const base::ListValue* denied_values = command.FindList("denied");
    const size_t granted = granted_values ? granted_values->size() : 0u;
    const size_t denied = denied_values ? denied_values->size() : 0u;
    if (const std::optional<int> request_id = command.FindInt("requestId")) {
      auto pending = PendingPermissionRequests().find(*request_id);
      if (pending != PendingPermissionRequests().end()) {
        base::OnceClosure done = std::move(pending->second);
        PendingPermissionRequests().erase(pending);
        std::move(done).Run();
      }
    }
    // Nothing to retry on a denial: the permission stays on the asked-once
    // list, and the web API it backs will fail the way an absent one does.
    LOG_IF(WARNING, denied > 0)
        << "OHOS shell denied " << denied << " of " << (granted + denied)
        << " requested permissions";
    return;
  }

  if (*name == "pwaMenuAction") {
    const std::optional<int> session_id = command.FindInt("menuSessionId");
    const std::optional<int> command_id = command.FindInt("commandId");
    if (session_id && command_id) {
      ActivatePwaMenuSession(*session_id, *command_id,
                             command.FindInt("eventFlags").value_or(0));
    }
    return;
  }

  if (*name == "pwaMenuDismiss") {
    if (const std::optional<int> session_id =
            command.FindInt("menuSessionId")) {
      FinishPwaMenuSession(*session_id, false);
    }
    return;
  }

  if (*name == "defaultBrowserState") {
    std::optional<bool> is_default;
    if (command.FindBool("known").value_or(true)) {
      is_default = command.FindBool("isDefault");
    }
    AuraShellDefaultBrowserStateCallback callback;
    {
      RuntimeBridgeState& state = GetState();
      base::AutoLock lock(state.lock);
      state.is_default_browser = is_default;
      callback = state.default_browser_state_callback;
    }
    if (callback) {
      callback.Run(is_default);
    }
    return;
  }

  if (*name == "systemCapabilities") {
    std::optional<bool> is_default;
    if (command.FindBool("known").value_or(true)) {
      is_default = command.FindBool("isDefault");
    }
    AuraShellDefaultBrowserStateCallback callback;
    RuntimeBridgeState& state = GetState();
    {
      base::AutoLock lock(state.lock);
      state.huawei_wallet_available =
          command.FindBool("huaweiWalletAvailable").value_or(false);
      state.is_default_browser = is_default;
      callback = state.default_browser_state_callback;
    }
    if (callback) {
      callback.Run(is_default);
    }
    return;
  }

  BrowserWindowInterface* browser = FindBrowserForWidget(widget);
  TabStripModel* tabs = browser ? browser->GetTabStripModel() : nullptr;
  if (!browser || !tabs) {
    LOG(ERROR) << "OHOS Aura shell command has no active browser: " << *name;
    return;
  }

  content::WebContents* active = tabs->GetActiveWebContents();
  if (*name == "recoverInput") {
    ui::OhosEventSource::ResetPointerCaptures(widget);
    if (browser->GetWindow()) {
      browser->GetWindow()->ShowInactive();
      browser->GetWindow()->Activate();
    }
    const std::string* requested_target_value =
        command.FindString("focusedTarget");
    const std::string requested_target =
        requested_target_value ? *requested_target_value : "webContents";
    BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(
        browser);
    SidePanelUI* side_panel_ui = SidePanelUI::From(browser);
    const bool focus_side_panel = requested_target == "sidePanel" &&
                                  browser_view && browser_view->side_panel() &&
                                  side_panel_ui &&
                                  side_panel_ui->IsSidePanelShowing();
    if (focus_side_panel) {
      browser_view->side_panel()->RequestFocus();
    } else if (active) {
      active->Focus();
    }
    if (browser_view) {
      browser_view->InvalidateLayout();
      browser_view->SchedulePaint();
    }
    base::DictValue event;
    event.Set("event", "inputRecovered");
    event.Set("activationEpoch",
              command.FindInt("activationEpoch").value_or(0));
    event.Set("focusedTarget", focus_side_panel ? "sidePanel" : "webContents");
    const std::string* side_panel_entry_id =
        command.FindString("sidePanelEntryId");
    event.Set("sidePanelEntryId",
              side_panel_entry_id ? *side_panel_entry_id : "");
    DispatchRuntimeEvent(widget, std::move(event));
  } else if (*name == "back" && active && active->GetController().CanGoBack()) {
    active->GetController().GoBack();
  } else if (*name == "forward" && active &&
             active->GetController().CanGoForward()) {
    active->GetController().GoForward();
  } else if (*name == "reload" && active) {
    active->GetController().Reload(content::ReloadType::NORMAL,
                                   /*check_for_repost=*/false);
  } else if (*name == "stop" && active) {
    active->Stop();
  } else if (*name == "home") {
    NavigateOnUiThread(widget, GURL("chrome://newtab/"), 0);
  } else if (*name == "pwaHome") {
    const std::string* url = command.FindString("url");
    if (url && GURL(*url).is_valid()) {
      NavigateOnUiThread(widget, GURL(*url), 0);
    }
  } else if (*name == "navigate") {
    const std::string* url = command.FindString("url");
    if (url && GURL(*url).is_valid()) {
      NavigateOnUiThread(widget, GURL(*url), 0);
    }
  } else if (*name == "newTab") {
    content::OpenURLParams params(GURL("chrome://newtab/"), content::Referrer(),
                                  WindowOpenDisposition::NEW_FOREGROUND_TAB,
                                  ui::PAGE_TRANSITION_TYPED,
                                  /*is_renderer_initiated=*/false);
    browser->OpenURL(params, {});
  } else if (*name == "activateTab") {
    const std::optional<int> index = command.FindInt("index");
    if (index && tabs->ContainsIndex(*index)) {
      tabs->ActivateTabAt(*index);
    }
  } else if (*name == "closeTab") {
    const int index = command.FindInt("index").value_or(tabs->active_index());
    if (tabs->ContainsIndex(index) && tabs->count() > 1) {
      tabs->CloseWebContentsAt(index, TabCloseTypes::CLOSE_USER_GESTURE);
    } else if (active) {
      NavigateOnUiThread(widget, GURL("chrome://newtab/"), 0);
    }
  } else if (*name == "print" && active) {
    RequestAuraShellSystemPrint(active);
  } else if (*name == "share" && active) {
    RequestAuraShellSystemShare(active);
  } else if (*name == "pwaMenu") {
    BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(
        browser);
    const bool is_pwa_window =
        browser->GetType() == BrowserWindowInterface::Type::TYPE_APP ||
        browser->GetType() == BrowserWindowInterface::Type::TYPE_APP_POPUP;
    if (!is_pwa_window || !browser_view) {
      LOG(ERROR) << "OHOS Aura shell rejected PWA menu outside app window";
      return;
    }
    const std::string* presentation = command.FindString("presentation");
    if (presentation && *presentation == "sheet") {
      ShowPwaMenuSheet(widget, browser, browser_view);
    } else {
      ShowPwaMenuPopup(widget, browser, browser_view, command);
    }
  } else if (*name == "pwaClose" &&
             (browser->GetType() == BrowserWindowInterface::Type::TYPE_APP ||
              browser->GetType() ==
                  BrowserWindowInterface::Type::TYPE_APP_POPUP)) {
    ClosePwaMenuSessionsForWidget(widget);
    browser->GetWindow()->Close();
  }
}

void ReloadAllTabsAfterThemeFontChange() {
  GlobalBrowserCollection* browsers = GlobalBrowserCollection::GetInstance();
  if (!browsers) {
    return;
  }
  browsers->ForEach([](BrowserWindowInterface* browser) {
    TabStripModel* tabs = browser->GetTabStripModel();
    if (tabs) {
      for (int index = 0; index < tabs->count(); ++index) {
        if (content::WebContents* contents = tabs->GetWebContentsAt(index)) {
          contents->GetController().Reload(content::ReloadType::NORMAL,
                                           /*check_for_repost=*/false);
        }
      }
    }
    if (BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(
            browser)) {
      browser_view->InvalidateLayout();
      browser_view->SchedulePaint();
    }
    return true;
  });
}

void ReloadThemeFontsOnUiThread(std::string font_id) {
  const bool font_manager_reloaded = skia::ReloadOhosFontManager(font_id);
  ui::ResourceBundle::GetSharedInstance().ReloadFonts();

  const bool single_process = base::CommandLine::ForCurrentProcess()->HasSwitch(
      switches::kSingleProcess);
  bool renderer_cache_refresh_posted = false;
  if (single_process) {
    renderer_cache_refresh_posted =
        content::ReloadOhosThemeFontsForInProcessRenderer();
  } else {
    std::set<content::RenderProcessHost*> renderer_processes;
    if (GlobalBrowserCollection* browsers =
            GlobalBrowserCollection::GetInstance()) {
      browsers->ForEach([&renderer_processes](BrowserWindowInterface* browser) {
        TabStripModel* tabs = browser->GetTabStripModel();
        if (!tabs) {
          return true;
        }
        for (int index = 0; index < tabs->count(); ++index) {
          content::WebContents* contents = tabs->GetWebContentsAt(index);
          if (contents && contents->GetPrimaryMainFrame()) {
            renderer_processes.insert(
                contents->GetPrimaryMainFrame()->GetProcess());
          }
        }
        return true;
      });
    }
    for (content::RenderProcessHost* process : renderer_processes) {
      if (process && !process->ShutdownRequested()) {
        process->Shutdown(/*exit_code=*/0);
      }
    }
  }

  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE, base::BindOnce(&ReloadAllTabsAfterThemeFontChange),
      kThemeFontRendererRestartDelay);
  LOG(INFO) << "OHOS theme font refreshed fontId=" << font_id
            << " managerReloaded=" << font_manager_reloaded
            << " singleProcess=" << single_process
            << " rendererRefreshPosted=" << renderer_cache_refresh_posted;
}

}  // namespace

void NotifyAuraShellBrowserStarted() {
  if (base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kNoStartupWindow) &&
      !WindowlessKeepAlive()) {
    WindowlessKeepAlive() = std::make_unique<ScopedKeepAlive>(
        KeepAliveOrigin::BROWSER_PROCESS_OHOS,
        KeepAliveRestartOption::DISABLED);
  }
  ui::SetOhosSelectFileDialogRequestCallback(
      base::BindRepeating(&DispatchFilePickerRequest));
  OhosWebPermissionWatcher::GetInstance().Start();

  // PlatformHandle is built below this target, so it cannot call into the
  // bridge; the bridge hands it these instead. Without them its OpenSystemSettings
  // and Request would be the default NOTREACHED(), which this build traps on.
  ::system_permission_settings::SetOhosPermissionRequester(
      base::BindRepeating([](ContentSettingsType type, base::OnceClosure done) {
        RequestOhosPermissionsFor(type, std::move(done));
      }));
  ::system_permission_settings::SetOhosSettingsOpener(
      base::BindRepeating([]() {
        base::DictValue event;
        event.Set("event", "systemActionRequested");
        event.Set("action", "appSettings");
        DispatchRuntimeEvent(std::move(event));
      }));


  RuntimeBridgeState& state = GetState();
  std::optional<GURL> pending_url;
  std::optional<std::string> pending_theme_font_id;
  bool pending_shutdown = false;
  std::string ui_family;
  std::string color_scheme;
  scoped_refptr<base::SingleThreadTaskRunner> ui_task_runner;
  uint64_t browser_generation = 0;
  {
    base::AutoLock lock(state.lock);
    state.ui_task_runner = base::SingleThreadTaskRunner::GetCurrentDefault();
    browser_generation = ++state.browser_generation;
    state.last_browser_state_json.clear();
    ui_task_runner = state.ui_task_runner;
    pending_url = std::move(state.pending_url);
    pending_theme_font_id = std::move(state.pending_theme_font_id);
    ui_family = state.ui_family;
    color_scheme = state.color_scheme;
    pending_shutdown = state.pending_shutdown;
    state.pending_shutdown = false;
  }

  ApplyUiFamilyOnUiThread(std::move(ui_family));
  ApplyColorSchemeOnUiThread(std::move(color_scheme));
  ui_task_runner->PostTask(
      FROM_HERE,
      base::BindOnce(&PollBrowserStateOnUiThread, browser_generation));
  ui_task_runner->PostTask(FROM_HERE,
                           base::BindOnce(&ApplyWindowStateOnUiThread,
                                          gfx::kNullAcceleratedWidget, 0));
  if (pending_url) {
    ui_task_runner->PostTask(
        FROM_HERE,
        base::BindOnce(&NavigateOnUiThread, gfx::kNullAcceleratedWidget,
                       std::move(*pending_url), 0));
  }
  if (pending_theme_font_id) {
    ui_task_runner->PostTask(FROM_HERE,
                             base::BindOnce(&ReloadThemeFontsOnUiThread,
                                            std::move(*pending_theme_font_id)));
  }
  if (pending_shutdown) {
    ui_task_runner->PostTask(FROM_HERE, base::BindOnce(&ShutdownOnUiThread));
  }
}

void EnsureAuraShellSystemPermissions() {
#if BUILDFLAG(OS_LEVEL_GEOLOCATION_PERMISSION_SUPPORTED)
  // Before the first profile, not at PostBrowserStart. Building a profile
  // builds its PermissionManager, and GeolocationPermissionContextSystem's
  // constructor dereferences GeolocationSystemPermissionManager::GetInstance()
  // behind nothing but a DCHECK. Registering afterwards left that a null
  // dereference on CrBrowserMain, and the browser died during startup.
  if (!device::GeolocationSystemPermissionManager::GetInstance()) {
    device::GeolocationSystemPermissionManager::SetInstance(
        SystemGeolocationSourceOhos::CreateGeolocationSystemPermissionManager());
  }
#endif
}

void NotifyAuraShellBrowserStopped() {
  ui::SetOhosSelectFileDialogRequestCallback({});
  ui::CancelAllOhosSelectFileDialogs();
  GetPwaMenuSessions().clear();
  RuntimeBridgeState& state = GetState();
  base::AutoLock lock(state.lock);
  state.ui_task_runner.reset();
  ++state.browser_generation;
  state.last_browser_state_json.clear();
  state.activation_target_signature.clear();
  state.activation_epoch.clear();
  state.window_visibility.clear();
  state.window_focus.clear();
  state.app_focus_applied = false;
  state.default_browser_state_callback.Reset();
  state.is_default_browser.reset();
  state.huawei_wallet_available = false;
}

bool NavigateAuraShellBrowser(const std::string& url) {
  return NavigateAuraShellBrowser(gfx::kNullAcceleratedWidget, url);
}

bool NavigateAuraShellBrowser(gfx::AcceleratedWidget widget,
                              const std::string& url) {
  GURL target(url);
  if (!target.is_valid()) {
    LOG(ERROR) << "OHOS Aura shell rejected invalid URL: " << url;
    return false;
  }

  scoped_refptr<base::SingleThreadTaskRunner> ui_task_runner;
  {
    RuntimeBridgeState& state = GetState();
    base::AutoLock lock(state.lock);
    ui_task_runner = state.ui_task_runner;
    if (!ui_task_runner) {
      state.pending_url = std::move(target);
      return true;
    }
  }

  ui_task_runner->PostTask(
      FROM_HERE,
      base::BindOnce(&NavigateOnUiThread, widget, std::move(target), 0));
  return true;
}

bool ExecuteAuraShellBrowserCommand(const std::string& command_json) {
  return ExecuteAuraShellBrowserCommand(gfx::kNullAcceleratedWidget,
                                        command_json);
}

namespace {

// Posts one command, once its name is one this bridge knows. Split out
// because the shell may send several at once; see the batch below.
bool PostBrowserCommand(gfx::AcceleratedWidget widget,
                        base::DictValue command) {
  const std::string* name = command.FindString("command");
  static constexpr std::string_view kSupportedCommands[] = {
      "back",
      "forward",
      "reload",
      "stop",
      "home",
      "pwaHome",
      "navigate",
      "newTab",
      "activateTab",
      "closeTab",
      "print",
      "share",
      "pwaMenu",
      "pwaMenuAction",
      "pwaMenuDismiss",
      "pwaClose",
      "recoverInput",
      "defaultBrowserState",
      "systemCapabilities",
      "requestState",
      "filePickerResult",
      "permissionResult",
      "systemPermissionState",
  };
  if (!name || std::ranges::find(kSupportedCommands, *name) ==
                   std::ranges::end(kSupportedCommands)) {
    LOG(ERROR) << "OHOS Aura shell rejected browser command JSON";
    return false;
  }

  scoped_refptr<base::SingleThreadTaskRunner> ui_task_runner;
  {
    RuntimeBridgeState& state = GetState();
    base::AutoLock lock(state.lock);
    ui_task_runner = state.ui_task_runner;
  }
  if (!ui_task_runner) {
    return false;
  }
  ui_task_runner->PostTask(
      FROM_HERE, base::BindOnce(&ExecuteBrowserCommandOnUiThread, widget,
                                std::move(command)));
  return true;
}

}  // namespace

bool ExecuteAuraShellBrowserCommand(gfx::AcceleratedWidget widget,
                                    const std::string& command_json) {
  std::optional<base::Value> parsed =
      base::JSONReader::Read(command_json, base::JSON_PARSE_RFC);
  if (!parsed) {
    LOG(ERROR) << "OHOS Aura shell rejected browser command JSON";
    return false;
  }

  // A batch. The shell hands commands to this bridge through a pair of ArkUI
  // state fields, and ArkUI collapses everything written to them in one
  // synchronous block into a single notification: of two commands sent back to
  // back, only the later one was ever seen. Sending them as a list keeps both,
  // and keeps them in order -- which matters, because a shell that reports the
  // system permission and then answers the request that changed it is telling
  // Chromium two things that only make sense that way round.
  if (parsed->is_list()) {
    bool posted_all = true;
    for (base::Value& entry : parsed->GetList()) {
      if (!entry.is_dict()) {
        LOG(ERROR) << "OHOS Aura shell rejected browser command JSON";
        posted_all = false;
        continue;
      }
      posted_all &= PostBrowserCommand(widget, std::move(entry).TakeDict());
    }
    return posted_all;
  }

  if (!parsed->is_dict()) {
    LOG(ERROR) << "OHOS Aura shell rejected browser command JSON";
    return false;
  }
  return PostBrowserCommand(widget, std::move(*parsed).TakeDict());
}

void SetAuraShellBrowserStateCallback(AuraShellBrowserStateCallback callback) {
  RuntimeBridgeState& state = GetState();
  base::AutoLock lock(state.lock);
  state.browser_state_callback = std::move(callback);
  state.last_browser_state_json.clear();
}

std::optional<bool> GetAuraShellDefaultBrowserState() {
  RuntimeBridgeState& state = GetState();
  base::AutoLock lock(state.lock);
  return state.is_default_browser;
}

void SetAuraShellDefaultBrowserStateCallback(
    AuraShellDefaultBrowserStateCallback callback) {
  RuntimeBridgeState& state = GetState();
  base::AutoLock lock(state.lock);
  state.default_browser_state_callback = std::move(callback);
}

bool IsAuraShellHuaweiWalletAvailable() {
  RuntimeBridgeState& state = GetState();
  base::AutoLock lock(state.lock);
  return state.huawei_wallet_available;
}

std::optional<AuraShellWindowMetadata> GetAuraShellWindowMetadata(
    gfx::AcceleratedWidget widget) {
  BrowserWindowInterface* browser = FindBrowserForWidget(widget);
  if (!browser) {
    return std::nullopt;
  }

  AuraShellWindowMetadata metadata;
  metadata.is_pwa =
      browser->GetType() == BrowserWindowInterface::Type::TYPE_APP ||
      browser->GetType() == BrowserWindowInterface::Type::TYPE_APP_POPUP;
  TabStripModel* tabs = browser->GetTabStripModel();
  content::WebContents* active = tabs ? tabs->GetActiveWebContents() : nullptr;
  if (!active) {
    return metadata;
  }
  metadata.url = active->GetVisibleURL().spec();
  metadata.start_url = metadata.url;
  metadata.title = base::UTF16ToUTF8(active->GetTitle());
  if (metadata.is_pwa) {
    if (const webapps::AppId* app_id =
            web_app::WebAppTabHelper::GetAppId(active)) {
      metadata.app_id = *app_id;
      if (web_app::WebAppProvider* provider =
              web_app::WebAppProvider::GetForWebApps(browser->GetProfile())) {
        metadata.start_url =
            provider->registrar_unsafe().GetAppStartUrl(*app_id).spec();
      }
    }
  }
  return metadata;
}

void UpdateAuraShellUiFamily(const std::string& ui_family) {
  const std::string resolved_family =
      ui_family.empty() ? "mobile_phone" : ui_family;
  scoped_refptr<base::SingleThreadTaskRunner> ui_task_runner;
  {
    RuntimeBridgeState& state = GetState();
    base::AutoLock lock(state.lock);
    if (state.ui_family == resolved_family) {
      return;
    }
    state.ui_family = resolved_family;
    ui_task_runner = state.ui_task_runner;
  }

  if (ui_task_runner) {
    ui_task_runner->PostTask(
        FROM_HERE, base::BindOnce(&ApplyUiFamilyOnUiThread, resolved_family));
  }
}

void UpdateAuraShellColorScheme(const std::string& color_scheme) {
  const std::string resolved_scheme = color_scheme == "dark" ? "dark" : "light";
  scoped_refptr<base::SingleThreadTaskRunner> ui_task_runner;
  {
    RuntimeBridgeState& state = GetState();
    base::AutoLock lock(state.lock);
    if (state.color_scheme == resolved_scheme) {
      return;
    }
    state.color_scheme = resolved_scheme;
    ui_task_runner = state.ui_task_runner;
  }

  if (ui_task_runner) {
    ui_task_runner->PostTask(
        FROM_HERE,
        base::BindOnce(&ApplyColorSchemeOnUiThread, resolved_scheme));
  }
}

void UpdateAuraShellPrintOutputDirectory(const std::string& output_directory) {
  RuntimeBridgeState& state = GetState();
  base::AutoLock lock(state.lock);
  state.print_output_directory = output_directory;
}

bool RequestAuraShellSystemPrint(content::WebContents* contents) {
  if (!contents) {
    BrowserWindowInterface* browser = GetActiveBrowser();
    TabStripModel* tabs = browser ? browser->GetTabStripModel() : nullptr;
    contents = tabs ? tabs->GetActiveWebContents() : nullptr;
  }
  const gfx::AcceleratedWidget widget =
      GetBrowserWidget(FindBrowserForWebContents(contents));
  if (!contents || !contents->GetPrimaryMainFrame()) {
    DispatchPrintFailure(widget, "noActiveDocument");
    return true;
  }

  std::string output_directory;
  {
    RuntimeBridgeState& state = GetState();
    base::AutoLock lock(state.lock);
    output_directory = state.print_output_directory;
  }
  if (output_directory.empty()) {
    DispatchPrintFailure(widget, "printOutputDirectoryUnavailable");
    return true;
  }

  auto params_or_error = print_to_pdf::GetPrintPagesParams(
      contents->GetLastCommittedURL(), std::nullopt, std::nullopt, std::nullopt,
      std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
      std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
      std::nullopt, std::nullopt);
  if (std::holds_alternative<std::string>(params_or_error)) {
    DispatchPrintFailure(widget,
                         std::get<std::string>(std::move(params_or_error)));
    return true;
  }

  printing::PrintViewManager* manager =
      printing::PrintViewManager::FromWebContents(contents);
  if (!manager) {
    printing::PrintViewManager::CreateForWebContents(contents);
    manager = printing::PrintViewManager::FromWebContents(contents);
  }
  if (!manager) {
    DispatchPrintFailure(widget, "printManagerUnavailable");
    return true;
  }

  const base::FilePath path =
      base::FilePath(output_directory)
          .AppendASCII("chromium-print-" +
                       base::NumberToString(
                           base::Time::Now().InMillisecondsSinceUnixEpoch()) +
                       ".pdf");
  manager->PrintToPdf(contents->GetPrimaryMainFrame(), "",
                      std::get<printing::mojom::PrintPagesParamsPtr>(
                          std::move(params_or_error)),
                      base::BindOnce(&OnPdfGenerated, widget, path,
                                     base::UTF16ToUTF8(contents->GetTitle()),
                                     contents->GetLastCommittedURL().spec()));
  return true;
}

bool RequestAuraShellSystemShare(content::WebContents* contents) {
  if (!contents) {
    BrowserWindowInterface* browser = GetActiveBrowser();
    TabStripModel* tabs = browser ? browser->GetTabStripModel() : nullptr;
    contents = tabs ? tabs->GetActiveWebContents() : nullptr;
  }
  if (!contents || !contents->GetLastCommittedURL().is_valid()) {
    return false;
  }
  base::DictValue event;
  event.Set("event", "shareRequested");
  event.Set("url", contents->GetLastCommittedURL().spec());
  event.Set("title", base::UTF16ToUTF8(contents->GetTitle()));
  DispatchRuntimeEvent(GetBrowserWidget(FindBrowserForWebContents(contents)),
                       std::move(event));
  return true;
}

bool RequestAuraShellSystemCast(content::WebContents* contents) {
  if (!contents) {
    BrowserWindowInterface* browser = GetActiveBrowser();
    TabStripModel* tabs = browser ? browser->GetTabStripModel() : nullptr;
    contents = tabs ? tabs->GetActiveWebContents() : nullptr;
  }
  if (!contents) {
    return false;
  }
  base::DictValue event;
  event.Set("event", "castRequested");
  event.Set("url", contents->GetLastCommittedURL().spec());
  event.Set("title", base::UTF16ToUTF8(contents->GetTitle()));
  DispatchRuntimeEvent(GetBrowserWidget(FindBrowserForWebContents(contents)),
                       std::move(event));
  return true;
}

bool RequestAuraShellSystemAction(const std::string& action) {
  static constexpr std::string_view kSupportedActions[] = {
      "huaweiAccount", "huaweiWallet", "defaultBrowser", "checkForUpdates"};
  if (std::ranges::find(kSupportedActions, action) ==
      std::ranges::end(kSupportedActions)) {
    LOG(ERROR) << "OHOS Aura shell rejected system action: " << action;
    return false;
  }

  base::DictValue event;
  event.Set("event", "systemActionRequested");
  event.Set("action", action);
  DispatchRuntimeEvent(std::move(event));
  return true;
}

void NotifyAuraShellWebAppInstalled(const std::string& app_id,
                                    const std::string& title,
                                    const std::string& start_url) {
  base::DictValue event;
  event.Set("event", "pwaInstalled");
  event.Set("appId", app_id);
  event.Set("title", title);
  event.Set("url", start_url);
  event.Set("pwaStartUrl", start_url);
  DispatchRuntimeEvent(std::move(event));
}

bool IsAuraShellMobilePhoneUi() {
  RuntimeBridgeState& state = GetState();
  base::AutoLock lock(state.lock);
  return state.ui_family == "mobile_phone";
}

bool IsAuraShellDesktopUi() {
  RuntimeBridgeState& state = GetState();
  base::AutoLock lock(state.lock);
  return state.ui_family == "aura_pc";
}

void SetAuraShellBrowserVisible(bool visible) {
  SetAuraShellBrowserVisible(gfx::kNullAcceleratedWidget, visible);
}

void SetAuraShellBrowserVisible(gfx::AcceleratedWidget widget, bool visible) {
  scoped_refptr<base::SingleThreadTaskRunner> ui_task_runner;
  {
    RuntimeBridgeState& state = GetState();
    base::AutoLock lock(state.lock);
    if (widget == gfx::kNullAcceleratedWidget) {
      state.app_visible = visible;
      if (!visible) {
        state.app_focused = false;
      }
    } else {
      state.window_visibility[widget] = visible;
      if (!visible) {
        state.window_focus[widget] = false;
      }
    }
    ui_task_runner = state.ui_task_runner;
  }
  if (ui_task_runner) {
    ui_task_runner->PostTask(
        FROM_HERE, base::BindOnce(&ApplyWindowStateOnUiThread, widget, 0));
  }
}

void SetAuraShellBrowserFocused(bool focused) {
  SetAuraShellBrowserFocused(gfx::kNullAcceleratedWidget, focused);
}

void SetAuraShellBrowserFocused(gfx::AcceleratedWidget widget, bool focused) {
  scoped_refptr<base::SingleThreadTaskRunner> ui_task_runner;
  {
    RuntimeBridgeState& state = GetState();
    base::AutoLock lock(state.lock);
    // Re-asserting focus the window already has is not free: applying it runs
    // ShowInactive() then Activate() then focuses the web contents, and that
    // deactivates whatever Views bubble is open. The shell reports focus on
    // every touch press -- ohos_aura_shell_napi.cc calls OnFocusChanged(true)
    // on each DOWN -- so in the steady state this arrived once per tap, as a
    // task racing the tap's own dispatch. A bubble that closes on deactivation
    // died in that gap, including when the tap was inside the bubble.
    //
    // Only a change is worth applying. The first report for a window is a
    // change by definition, which is why the map's absent key and the app-wide
    // flag are treated as "not yet applied" rather than as a value.
    if (widget == gfx::kNullAcceleratedWidget) {
      if (state.app_focus_applied && state.app_focused == focused) {
        return;
      }
      state.app_focused = focused;
      state.app_focus_applied = true;
    } else {
      const auto [it, inserted] = state.window_focus.insert({widget, focused});
      if (!inserted) {
        if (it->second == focused) {
          return;
        }
        it->second = focused;
      }
    }
    ui_task_runner = state.ui_task_runner;
  }
  if (ui_task_runner) {
    ui_task_runner->PostTask(
        FROM_HERE, base::BindOnce(&ApplyWindowStateOnUiThread, widget, 0));
  }
}

void NotifyAuraShellThemeFontChanged(const std::string& font_id) {
  scoped_refptr<base::SingleThreadTaskRunner> ui_task_runner;
  bool duplicate_request = false;
  {
    RuntimeBridgeState& state = GetState();
    base::AutoLock lock(state.lock);
    if (state.requested_theme_font_id &&
        *state.requested_theme_font_id == font_id) {
      duplicate_request = true;
    } else {
      state.requested_theme_font_id = font_id;
      ui_task_runner = state.ui_task_runner;
      if (!ui_task_runner) {
        state.pending_theme_font_id = font_id;
      }
    }
  }
  if (duplicate_request) {
    LOG(INFO) << "OHOS theme font refresh skipped duplicate fontId=" << font_id;
    return;
  }
  if (!ui_task_runner) {
    return;
  }
  ui_task_runner->PostTask(
      FROM_HERE, base::BindOnce(&ReloadThemeFontsOnUiThread, font_id));
}

void ShutdownAuraShellBrowser() {
  scoped_refptr<base::SingleThreadTaskRunner> ui_task_runner;
  {
    RuntimeBridgeState& state = GetState();
    base::AutoLock lock(state.lock);
    ui_task_runner = state.ui_task_runner;
    if (!ui_task_runner) {
      state.pending_shutdown = true;
      return;
    }
  }

  ui_task_runner->PostTask(FROM_HERE, base::BindOnce(&ShutdownOnUiThread));
}

}  // namespace chrome::ohos
