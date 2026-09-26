// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/ohos/aura_shell_runtime_bridge.h"
#include "chrome/browser/ui/ohos/device_authenticator_ohos.h"
#include "chrome/browser/ui/ohos/screen_orientation_delegate_ohos.h"
#include "chrome/browser/ui/ohos/shell_permission_prompt_ohos.h"
#include "chrome/browser/ui/ohos/shell_context_menu_image_ohos.h"
#include "chrome/browser/ui/ohos/shell_page_position_ohos.h"
#include "chrome/browser/ui/ohos/shell_tab_groups_ohos.h"
#include "chrome/browser/ui/navigator/browser_navigator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "chrome/browser/ui/tabs/tab_group_model.h"
#include "components/tabs/public/tab_group.h"

#include <algorithm>
#include <cmath>
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
#include "base/strings/string_split.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/synchronization/lock.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "base/values.h"
#include "cc/input/browser_controls_offset_tag_modifications.h"
#include "cc/input/browser_controls_state.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/content_settings/host_content_settings_map_factory.h"
#include "chrome/browser/lifetime/application_lifetime.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/profiles/profile_manager_observer.h"
#include "chrome/browser/sessions/tab_restore_service_factory.h"
#include "components/sessions/core/tab_restore_service.h"
#include "components/sessions/core/tab_restore_service_observer.h"
#include "base/base64.h"
#include "base/strings/stringprintf.h"
#include "base/scoped_observation.h"
#include "chrome/browser/extensions/extension_action_runner.h"
#include "chrome/browser/ui/toolbar/toolbar_actions_model.h"
#include "extensions/browser/extension_action.h"
#include "extensions/browser/extension_action_manager.h"
#include "extensions/browser/extension_registry.h"
#include "base/containers/lru_cache.h"
#include "chrome/browser/dom_distiller/tab_utils.h"
#include "ui/gfx/codec/png_codec.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_live_tab_context.h"
#include "components/viz/common/frame_sinks/copy_output_result.h"
#include "base/strings/stringprintf.h"
#include "components/dom_distiller/content/browser/distillability_driver.h"
#include "components/dom_distiller/content/browser/distillable_page_utils.h"
#include "components/dom_distiller/core/url_constants.h"
#include "components/dom_distiller/core/url_utils.h"
#include "components/sessions/core/tab_restore_types.h"
#include "components/zoom/zoom_controller.h"
#include "third_party/blink/public/common/page/page_zoom.h"
#include "chrome/browser/permissions/system/system_permission_common.h"
#include "chrome/browser/permissions/system/system_permission_settings_ohos.h"
#include "chrome/browser/ui/ohos/shell_context_menu_ohos.h"
#include "chrome/browser/ui/ohos/shell_downloads_ohos.h"
#include "chrome/browser/ui/ohos/shell_services_ohos.h"
#include "chrome/browser/ui/ohos/system_geolocation_source_ohos.h"
#include "services/device/public/cpp/geolocation/buildflags.h"
#include "services/device/public/cpp/geolocation/geolocation_system_permission_manager.h"
#include "services/device/public/cpp/geolocation/location_system_permission_status.h"
#include "components/content_settings/core/browser/content_settings_observer.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/content_settings/core/common/content_settings.h"
#include "components/content_settings/core/common/content_settings_types.h"
#include "components/embedder_support/user_agent_utils.h"
#include "components/sessions/content/session_tab_helper.h"
#include "chrome/common/chrome_isolated_world_ids.h"
#include "chrome/common/chrome_switches.h"
#include "components/keep_alive_registry/keep_alive_types.h"
#include "components/keep_alive_registry/scoped_keep_alive.h"
#include "chrome/browser/printing/print_view_manager.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_window/public/create_browser_window.h"
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
#include "base/scoped_multi_source_observation.h"
#include "components/find_in_page/find_notification_details.h"
#include "components/find_in_page/find_result_observer.h"
#include "components/find_in_page/find_tab_helper.h"
#include "components/find_in_page/find_types.h"
#include "components/printing/browser/print_to_pdf/pdf_print_result.h"
#include "components/printing/browser/print_to_pdf/pdf_print_utils.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/page_navigator.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/content_switches.h"
#include "content/public/common/referrer.h"
#include "content/public/renderer/render_thread.h"
#include "skia/ext/font_utils.h"
#include "ui/aura/window.h"
#include "ui/aura/window_tree_host.h"
#include "ui/base/base_window.h"
#include "ui/base/clipboard/scoped_clipboard_writer.h"
#include "ui/base/models/button_menu_item_model.h"
#include "ui/base/models/menu_model.h"
#include "ui/base/page_transition_types.h"
#include "ui/base/pointer/touch_ui_controller.h"
#include "ui/base/resource/resource_bundle.h"
#include "ui/base/window_open_disposition.h"
#include "ui/compositor/layer.h"
#include "ui/display/screen.h"
#include "ui/gfx/geometry/insets.h"
#include "ui/gfx/geometry/transform.h"
#include "ui/native_theme/native_theme.h"
#include "ui/ozone/platform/ohos/ohos_event_source.h"
#include "ui/shell_dialogs/select_file_dialog_ohos.h"
#include "ui/views/controls/menu/menu_runner.h"
#include "ui/views/focus/focus_manager.h"
#include "ui/views/widget/widget.h"
#include "ui/views/widget/widget_delegate.h"
#include "url/gurl.h"
#include "url/url_constants.h"

namespace chrome::ohos {

// Defined further down, past this namespace: the settings switch that says
// who draws the browser, as opposed to the screen changing shape.
void SetAuraShellBrowserChrome(const std::string& mode);

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
// Enough for a shell's startup burst; anything past it is dropped and logged.
constexpr size_t kMaxPendingCommands = 64;

struct RuntimeBridgeState {
  base::Lock lock;
  scoped_refptr<base::SingleThreadTaskRunner> ui_task_runner GUARDED_BY(lock);
  // Commands the shell sent before Chromium's UI thread was up -- the shell
  // starts talking as its page appears, which is earlier -- run once it is.
  std::vector<std::pair<gfx::AcceleratedWidget, base::DictValue>>
      pending_commands GUARDED_BY(lock);
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
  // How much of the bottom of each window's page the shell covers with its
  // own floating bar, in DIP. See ApplyViewportInsets().
  std::map<gfx::AcceleratedWidget, int> viewport_bottom_inset GUARDED_BY(lock);
  // The shell's top bar height and minimum (collapsed) height, in DIP; see
  // GetAuraShellTopControlsHeight() and GetAuraShellTopControlsMinHeight().
  int top_controls_height GUARDED_BY(lock) = 0;
  int top_controls_min_height GUARDED_BY(lock) = 0;
  // The view each window last had its controls shown on. Compared, never
  // dereferenced: it tells the poll that a different view is now current -- a
  // tab switch, or a navigation that swapped in a new renderer, which starts
  // over with the renderer's default constraints.
  std::map<gfx::AcceleratedWidget, uintptr_t> controls_shown_for
      GUARDED_BY(lock);
  float last_shown_ratio GUARDED_BY(lock) = -1.0f;
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

// Reports find-in-page results to the shell's find bar: the match count and
// which one is active, for whichever tab the shell last searched in.
class FindResultRelay : public find_in_page::FindResultObserver {
 public:
  static FindResultRelay& Get() {
    static base::NoDestructor<FindResultRelay> relay;
    return *relay;
  }

  void Watch(find_in_page::FindTabHelper* helper) {
    if (helper && !observation_.IsObservingSource(helper)) {
      observation_.AddObservation(helper);
    }
  }

  void OnFindResultAvailable(content::WebContents* web_contents) override;

  void OnFindTabHelperDestroyed(find_in_page::FindTabHelper* helper) override {
    if (observation_.IsObservingSource(helper)) {
      observation_.RemoveObservation(helper);
    }
  }

 private:
  friend class base::NoDestructor<FindResultRelay>;
  FindResultRelay() = default;

  base::ScopedMultiSourceObservation<find_in_page::FindTabHelper,
                                     find_in_page::FindResultObserver>
      observation_{this};
};

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

// The content settings whose OS permission Chromium must wait for before it
// uses the capability.
//
// system_permission_settings treats a type it has never heard of as allowed,
// which is right for a desktop where the OS does not gate these at all, and
// wrong here: Chromium started the camera the moment the site was allowed,
// while the HarmonyOS prompt was still on screen, and the first getUserMedia
// failed. Registering them makes IsAllowed() honest, so the media path asks
// through Request() and waits for the answer.
//
// Registering a permission the app already holds costs one round trip and no
// UI: requestPermissionsFromUser returns a granted permission immediately.
void RegisterOhosManagedPermissions() {
  for (ContentSettingsType type :
       {ContentSettingsType::MEDIASTREAM_CAMERA,
        ContentSettingsType::MEDIASTREAM_MIC}) {
    if (::system_permission_settings::GetOhosSystemPermission(type) ==
        ::system_permission_settings::SystemPermission::kNotDetermined) {
      ::system_permission_settings::SetOhosSystemPermission(
          type, ::system_permission_settings::SystemPermission::kNotDetermined);
    }
  }
}

// The content setting an OHOS permission name belongs to, for turning the
// shell's answer back into something Chromium understands.
std::optional<ContentSettingsType> ContentTypeForOhosPermission(
    std::string_view permission) {
  if (permission == "ohos.permission.CAMERA") {
    return ContentSettingsType::MEDIASTREAM_CAMERA;
  }
  if (permission == "ohos.permission.MICROPHONE") {
    return ContentSettingsType::MEDIASTREAM_MIC;
  }
  return std::nullopt;
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

// The URL as the shell sees it. The state is rebuilt, serialised and compared
// on the UI thread every 200 ms, so its cost is paid in touch latency: one
// 137 KB data: URL made each poll take 25 ms (48 ms worst) instead of 0.7 ms,
// holding touch input back for up to six frames. The shell only displays it,
// so a data: URL is reported by its header and anything else is capped.
std::string ShellVisibleUrl(const GURL& url) {
  constexpr size_t kMaxShellUrlLength = 2048;
  const std::string& spec = url.possibly_invalid_spec();
  if (url.SchemeIs(url::kDataScheme)) {
    const size_t comma = spec.find(',');
    return spec.substr(0, std::min({comma, spec.size(), kMaxShellUrlLength}));
  }
  return spec.size() <= kMaxShellUrlLength
             ? spec
             : spec.substr(0, kMaxShellUrlLength);
}

// A tab's stable id as the shell sees it. Empty when the tab has no session
// id yet, which the shell reads as "not addressable".
std::string ShellTabId(content::WebContents* contents) {
  if (!contents) {
    return std::string();
  }
  const SessionID id = sessions::SessionTabHelper::IdForTab(contents);
  return id.is_valid() ? base::NumberToString(id.id()) : std::string();
}

content::WebContents* FindTabById(TabStripModel* tabs,
                                  const std::string* id) {
  if (!tabs || !id || id->empty()) {
    return nullptr;
  }
  for (int index = 0; index < tabs->count(); ++index) {
    content::WebContents* contents = tabs->GetWebContentsAt(index);
    if (ShellTabId(contents) == *id) {
      return contents;
    }
  }
  return nullptr;
}

// The index a command means: by id when it gives one, since an index the
// shell read before someone else closed a tab now points at the wrong tab.
std::optional<int> ReadTabIndex(TabStripModel* tabs,
                                const base::DictValue& command) {
  if (content::WebContents* contents =
          FindTabById(tabs, command.FindString("id"))) {
    const std::optional<int> index = tabs->GetIndexOfWebContents(contents);
    if (index) {
      return index;
    }
  }
  const std::optional<int> index = command.FindInt("index");
  return index && tabs->ContainsIndex(*index) ? index : std::nullopt;
}

// Whether this tab is asking sites for their desktop pages. Phones send a
// mobile user agent by default, so an override means desktop.
bool IsRequestingDesktopSite(content::WebContents* contents) {
  if (!contents) {
    return false;
  }
  content::NavigationEntry* entry =
      contents->GetController().GetLastCommittedEntry();
  return entry && entry->GetIsOverridingUserAgent();
}

// Ask this tab's site for its desktop pages, or stop. Chrome for Android
// does the same three things: swap the user agent, mark the entry so a
// back-forward step keeps the choice, and reload from the original URL so a
// server that redirected us to its mobile host gets another say.
void SetRequestDesktopSite(content::WebContents* contents, bool enabled) {
  if (!contents) {
    return;
  }
  const bool mobile = !enabled;
  blink::UserAgentOverride override;
  if (enabled) {
    override.ua_string_override = embedder_support::GetUserAgentForOhos(mobile);
    override.ua_metadata_override =
        embedder_support::GetUserAgentMetadataForOhos(mobile);
  }
  contents->SetUserAgentOverride(override, /*override_in_new_tabs=*/false);
  content::NavigationController& controller = contents->GetController();
  if (content::NavigationEntry* entry = controller.GetLastCommittedEntry()) {
    entry->SetIsOverridingUserAgent(enabled);
  }
  // Not a plain reload: a site that redirected us to its mobile host would
  // just redirect again. Chromium own ToggleRequestTabletSite does the same
  // thing for the same reason.
  controller.LoadOriginalRequestURL();
}

// Anchor rectangles the shell reported, per window. Small, rewritten whenever
// the shell's layout changes, and read when a bubble is about to be shown.
using AnchorRects = std::map<std::string, gfx::Rect>;

std::map<gfx::AcceleratedWidget, AnchorRects>& AnchorStore() {
  static base::NoDestructor<std::map<gfx::AcceleratedWidget, AnchorRects>>
      store;
  return *store;
}

void SetAuraShellAnchorRects(gfx::AcceleratedWidget widget,
                             const base::DictValue& command) {
  AnchorRects rects;
  if (const base::ListValue* anchors = command.FindList("anchors")) {
    for (const base::Value& value : *anchors) {
      const base::DictValue* anchor = value.GetIfDict();
      const std::string* id = anchor ? anchor->FindString("id") : nullptr;
      if (!id || id->empty()) {
        continue;
      }
      rects[*id] = gfx::Rect(
          static_cast<int>(anchor->FindDouble("x").value_or(0.0)),
          static_cast<int>(anchor->FindDouble("y").value_or(0.0)),
          static_cast<int>(anchor->FindDouble("width").value_or(0.0)),
          static_cast<int>(anchor->FindDouble("height").value_or(0.0)));
    }
  }
  // Replaces wholesale: the shell reports its whole set on every layout, so
  // an anchor it stops sending is one it stopped drawing.
  AnchorStore()[widget] = std::move(rects);
}

// --- Extensions. ----------------------------------------------------------
//
// A shell drawing its own toolbar needs the buttons that would have been on
// Chromium's. The popup a button opens stays Chromium's -- it is the
// extension's own page, and it points at wherever the shell said it drew the
// button (see setAnchorRects).

std::string EncodeExtensionIcon(const gfx::Image& image) {
  if (image.IsEmpty()) {
    return std::string();
  }
  std::optional<std::vector<uint8_t>> png = gfx::PNGCodec::EncodeBGRASkBitmap(
      image.AsBitmap(), /*discard_transparency=*/false);
  return png ? base::Base64Encode(*png) : std::string();
}

// Tells every window of a profile that a list it may be drawing has changed,
// so it refetches rather than showing a stale one. One watcher per profile,
// created the first time that profile is asked for the list.
class ShellListWatcher : public sessions::TabRestoreServiceObserver,
                         public ToolbarActionsModel::Observer {
 public:
  explicit ShellListWatcher(Profile* profile) : profile_(profile) {
    if (auto* restore = TabRestoreServiceFactory::GetForProfile(profile)) {
      restore_observation_.Observe(restore);
    }
    if (auto* toolbar = ToolbarActionsModel::Get(profile)) {
      toolbar_observation_.Observe(toolbar);
    }
  }
  ShellListWatcher(const ShellListWatcher&) = delete;
  ShellListWatcher& operator=(const ShellListWatcher&) = delete;
  ~ShellListWatcher() override = default;

  // sessions::TabRestoreServiceObserver:
  void TabRestoreServiceChanged(sessions::TabRestoreService*) override {
    Notify("recentlyClosedChanged");
  }
  void TabRestoreServiceDestroyed(sessions::TabRestoreService*) override {
    restore_observation_.Reset();
  }

  // ToolbarActionsModel::Observer:
  void OnToolbarActionAdded(const ToolbarActionsModel::ActionId&) override {
    Notify("extensionActionsChanged");
  }
  void OnToolbarActionRemoved(const ToolbarActionsModel::ActionId&) override {
    Notify("extensionActionsChanged");
  }
  void OnToolbarActionUpdated(const ToolbarActionsModel::ActionId&) override {
    Notify("extensionActionsChanged");
  }
  void OnToolbarModelInitialized() override {
    Notify("extensionActionsChanged");
  }
  void OnToolbarPinnedActionsChanged() override {
    Notify("extensionActionsChanged");
  }

 private:
  void Notify(std::string_view name) {
    base::DictValue event;
    event.Set("event", name);
    DispatchAuraShellRuntimeEventToProfile(profile_, event);
  }

  const raw_ptr<Profile> profile_;
  base::ScopedObservation<sessions::TabRestoreService,
                          sessions::TabRestoreServiceObserver>
      restore_observation_{this};
  base::ScopedObservation<ToolbarActionsModel, ToolbarActionsModel::Observer>
      toolbar_observation_{this};
};

void EnsureShellListWatcher(Profile* profile) {
  static base::NoDestructor<
      std::map<Profile*, std::unique_ptr<ShellListWatcher>>>
      watchers;
  if (!profile || watchers->contains(profile)) {
    return;
  }
  watchers->emplace(profile, std::make_unique<ShellListWatcher>(profile));
}

void SendExtensionActions(gfx::AcceleratedWidget widget,
                          BrowserWindowInterface* browser,
                          const base::DictValue& command) {
  base::ListValue items;
  Profile* profile = browser ? browser->GetProfile() : nullptr;
  EnsureShellListWatcher(profile);
  ToolbarActionsModel* model = profile ? ToolbarActionsModel::Get(profile)
                                       : nullptr;
  if (model) {
    extensions::ExtensionActionManager* actions =
        extensions::ExtensionActionManager::Get(profile);
    extensions::ExtensionRegistry* registry =
        extensions::ExtensionRegistry::Get(profile);
    content::WebContents* active =
        browser->GetTabStripModel()->GetActiveWebContents();
    // Per-tab state -- a badge, a greyed-out icon -- is keyed on the tab the
    // user is looking at.
    const int tab_id =
        active ? sessions::SessionTabHelper::IdForTab(active).id() : -1;

    for (const ToolbarActionsModel::ActionId& id : model->action_ids()) {
      const extensions::Extension* extension =
          registry ? registry->enabled_extensions().GetByID(id) : nullptr;
      if (!extension) {
        continue;
      }
      extensions::ExtensionAction* action =
          actions ? actions->GetExtensionAction(*extension) : nullptr;
      base::DictValue item;
      item.Set("id", id);
      item.Set("name", extension->name());
      item.Set("iconPngBase64",
               action ? EncodeExtensionIcon(action->GetExplicitlySetIcon(tab_id))
                      : std::string());
      item.Set("badgeText",
               action ? action->GetDisplayBadgeText(tab_id) : std::string());
      // #AARRGGBB, which is what the shell's colour parser takes.
      item.Set("badgeColor",
               base::StringPrintf(
                   "#%08X",
                   action ? action->GetBadgeBackgroundColor(tab_id) : 0u));
      item.Set("enabled", action ? action->GetIsVisible(tab_id) : true);
      item.Set("pinned", model->IsActionPinned(id));
      items.Append(std::move(item));
    }
  }
  base::DictValue event;
  event.Set("event", "extensionActions");
  event.Set("requestId", command.FindInt("requestId").value_or(0));
  event.Set("items", std::move(items));
  DispatchRuntimeEvent(widget, std::move(event));
}

// The same thing as clicking the button on Chromium's toolbar: the extension
// gets its onClicked event, or its popup opens -- drawn by Chromium, anchored
// at wherever the shell said it put the button.
void RunExtensionAction(BrowserWindowInterface* browser,
                        const base::DictValue& command) {
  Profile* profile = browser ? browser->GetProfile() : nullptr;
  const std::string* id = command.FindString("id");
  content::WebContents* active =
      browser ? browser->GetTabStripModel()->GetActiveWebContents() : nullptr;
  if (!profile || !id || id->empty() || !active) {
    return;
  }
  extensions::ExtensionRegistry* registry =
      extensions::ExtensionRegistry::Get(profile);
  const extensions::Extension* extension =
      registry ? registry->enabled_extensions().GetByID(*id) : nullptr;
  extensions::ExtensionActionRunner* runner =
      extensions::ExtensionActionRunner::GetForWebContents(active);
  if (extension && runner) {
    runner->RunAction(extension, /*grant_tab_permissions=*/true);
  }
}

void SetExtensionPinned(BrowserWindowInterface* browser,
                        const base::DictValue& command) {
  Profile* profile = browser ? browser->GetProfile() : nullptr;
  ToolbarActionsModel* model = profile ? ToolbarActionsModel::Get(profile)
                                       : nullptr;
  const std::string* id = command.FindString("id");
  if (model && id && !id->empty()) {
    model->SetActionVisibility(*id,
                               command.FindBool("pinned").value_or(false));
  }
}

// --- Tab thumbnails. ------------------------------------------------------
//
// The tab grid draws a picture of each tab. The current one can be captured on
// demand; a background tab has no live surface to capture, so the last picture
// taken of it -- when it was last on screen -- is kept and handed back.

// Twelve of them. A phone's grid is two columns of about 170vp at 4:3, which
// at three times density is roughly 510x382 and 100-200KB of PNG each, so
// this is a couple of megabytes. Tabs past that draw a placeholder, which is
// what the shell shows for a tab it has never seen either.
constexpr size_t kMaxCachedThumbnails = 12;

// Wide enough to look right on the grid without paying for the whole screen.
constexpr int kMaxThumbnailWidthPx = 720;
constexpr double kThumbnailAspect = 4.0 / 3.0;

base::LRUCache<std::string, std::string>& ThumbnailCache() {
  static base::NoDestructor<base::LRUCache<std::string, std::string>> cache(
      kMaxCachedThumbnails);
  return *cache;
}

// Incognito pictures live here and nowhere else: this cache is in memory and
// is emptied when the last incognito window goes.
std::set<std::string>& IncognitoThumbnailIds() {
  static base::NoDestructor<std::set<std::string>> ids;
  return *ids;
}

std::string EncodeThumbnail(const SkBitmap& bitmap) {
  if (bitmap.drawsNothing()) {
    return std::string();
  }
  std::optional<std::vector<uint8_t>> png =
      gfx::PNGCodec::EncodeBGRASkBitmap(bitmap, /*discard_transparency=*/true);
  return png ? base::Base64Encode(*png) : std::string();
}

gfx::Size ThumbnailSizeFor(content::RenderWidgetHostView* view,
                           double width_vp,
                           float scale) {
  const int width = std::clamp(
      static_cast<int>(width_vp * scale), 64, kMaxThumbnailWidthPx);
  return gfx::Size(width, static_cast<int>(width / kThumbnailAspect));
}

// The top of the page, cropped to the card's shape rather than squashed into
// it: a squashed screenshot reads as a broken image.
gfx::Rect ThumbnailSourceRect(const gfx::Size& view_size) {
  const int height = std::min(
      view_size.height(),
      static_cast<int>(view_size.width() / kThumbnailAspect));
  return gfx::Rect(0, 0, view_size.width(), std::max(height, 1));
}

void RememberThumbnail(content::WebContents* contents,
                       const std::string& id,
                       const std::string& png_base64) {
  if (png_base64.empty()) {
    return;
  }
  ThumbnailCache().Put(id, png_base64);
  if (contents && contents->GetBrowserContext() &&
      contents->GetBrowserContext()->IsOffTheRecord()) {
    IncognitoThumbnailIds().insert(id);
  }
}

void ForgetIncognitoThumbnails() {
  for (const std::string& id : IncognitoThumbnailIds()) {
    ThumbnailCache().Erase(ThumbnailCache().Peek(id));
  }
  IncognitoThumbnailIds().clear();
}

// Collects a batch of thumbnails and answers exactly once, the same bargain
// the favicon batch makes: a capture that never comes back must not leave the
// grid empty forever.
class ThumbnailBatch : public base::RefCounted<ThumbnailBatch> {
 public:
  ThumbnailBatch(gfx::AcceleratedWidget widget, int request_id, size_t expected)
      : widget_(widget), request_id_(request_id), remaining_(expected) {}

  ThumbnailBatch(const ThumbnailBatch&) = delete;
  ThumbnailBatch& operator=(const ThumbnailBatch&) = delete;

  void Add(std::string id, std::string png_base64) {
    if (!sent_ && !png_base64.empty()) {
      base::DictValue item;
      item.Set("id", std::move(id));
      item.Set("pngBase64", std::move(png_base64));
      items_.Append(std::move(item));
    }
    if (remaining_ > 0 && --remaining_ == 0) {
      Send();
    }
  }

  void SendWhatArrived() { Send(); }

 private:
  friend class base::RefCounted<ThumbnailBatch>;
  ~ThumbnailBatch() = default;

  void Send() {
    if (std::exchange(sent_, true)) {
      return;
    }
    base::DictValue event;
    event.Set("event", "tabThumbnails");
    event.Set("requestId", request_id_);
    event.Set("items", std::move(items_));
    DispatchRuntimeEvent(widget_, std::move(event));
  }

  const gfx::AcceleratedWidget widget_;
  const int request_id_;
  size_t remaining_;
  bool sent_ = false;
  base::ListValue items_;
};

void OnThumbnailCaptured(scoped_refptr<ThumbnailBatch> batch,
                         base::WeakPtr<content::WebContents> contents,
                         std::string id,
                         const content::CopyFromSurfaceResult& result) {
  std::string png;
  if (result.has_value()) {
    png = EncodeThumbnail(result.value().bitmap);
    RememberThumbnail(contents.get(), id, png);
  }
  if (png.empty()) {
    // Fall back to whatever was last seen of this tab rather than nothing.
    auto cached = ThumbnailCache().Get(id);
    if (cached != ThumbnailCache().end()) {
      png = cached->second;
    }
  }
  batch->Add(std::move(id), std::move(png));
}

void SendTabThumbnails(gfx::AcceleratedWidget widget,
                       TabStripModel* tabs,
                       const base::DictValue& command) {
  const int request_id = command.FindInt("requestId").value_or(0);
  const double width_vp = command.FindDouble("widthVp").value_or(170.0);
  const base::ListValue* ids = command.FindList("ids");
  std::vector<std::pair<std::string, content::WebContents*>> wanted;
  if (ids) {
    for (const base::Value& value : *ids) {
      const std::string* id = value.GetIfString();
      if (id && !id->empty()) {
        wanted.emplace_back(*id, FindTabById(tabs, id));
      }
    }
  }

  auto batch = base::MakeRefCounted<ThumbnailBatch>(widget, request_id,
                                                    wanted.size());
  if (wanted.empty()) {
    batch->SendWhatArrived();
    return;
  }

  display::Screen* screen = display::Screen::Get();
  const float scale =
      screen ? screen->GetPrimaryDisplay().device_scale_factor() : 1.0f;
  content::WebContents* active = tabs ? tabs->GetActiveWebContents() : nullptr;

  for (auto& [id, contents] : wanted) {
    content::RenderWidgetHostView* view =
        contents ? contents->GetRenderWidgetHostView() : nullptr;
    // Only the tab on screen has a surface to copy. A background tab answers
    // from the picture taken when it was last current, which is what the grid
    // is showing anyway.
    if (contents != active || !view || !view->IsSurfaceAvailableForCopy()) {
      auto cached = ThumbnailCache().Get(id);
      batch->Add(id, cached != ThumbnailCache().end() ? cached->second
                                                      : std::string());
      continue;
    }
    view->CopyFromSurface(
        ThumbnailSourceRect(view->GetVisibleViewportSize()),
        ThumbnailSizeFor(view, width_vp, scale), base::Seconds(2),
        base::BindOnce(&OnThumbnailCaptured, batch, contents->GetWeakPtr(),
                       id));
  }
}

// --- Tab groups. ----------------------------------------------------------

// Both defined further down, with the recently-closed helpers.
sessions::TabRestoreService* RestoreServiceFor(BrowserWindowInterface* browser);
void RememberOpenerOfClosedTab(sessions::TabRestoreService* service,
                               content::WebContents* opener);

// How the shell closes a tab. CLOSE_USER_GESTURE alone closes it and forgets
// it: nothing reaches TabRestoreService, so the tab is missing from recently
// closed and cannot be restored, which is what the group's undo depends on.
constexpr uint32_t kCloseAndRemember =
    TabCloseTypes::CLOSE_USER_GESTURE | TabCloseTypes::CLOSE_CREATE_HISTORICAL_TAB;

// Defined further down, beside the rest of the navigation helpers.
void NavigateOnUiThread(gfx::AcceleratedWidget widget, GURL url, int attempt);

// Dissolves this window's one-tab groups, if it is still open. Takes a widget
// rather than a TabStripModel because it runs after the snapshot that asked
// for it, by which time the window may have gone.
void DissolveSingleTabGroupsForWidget(gfx::AcceleratedWidget widget) {
  if (BrowserWindowInterface* browser = FindBrowserForWidget(widget)) {
    chrome::ohos::DissolveSingleTabGroups(browser->GetTabStripModel());
  }
}

// The group the shell named, or nullopt when it named none or named one this
// window does not have -- a group the reader closed while the command was in
// flight, or one belonging to another window.
std::optional<tab_groups::TabGroupId> FindGroupById(TabStripModel* tabs,
                                                    const std::string* id) {
  if (!tabs || !id || id->empty() || !tabs->SupportsTabGroups() ||
      !tabs->group_model()) {
    return std::nullopt;
  }
  for (const tab_groups::TabGroupId& group :
       tabs->group_model()->ListTabGroups()) {
    if (group.ToString() == *id) {
      return group;
    }
  }
  return std::nullopt;
}

// Puts tabs the shell names into one group, in the order it names them.
//
// For a shell that keeps its own list of pages and reopens them one by one on
// startup: Chromium's session has the groups, but nothing restored through it,
// so they have to be made again. Tabs already in a group leave it for this
// one. Fewer than two tabs is not a group and does nothing.
void GroupTabsById(TabStripModel* tabs, const base::DictValue& command) {
  const base::ListValue* ids = command.FindList("ids");
  if (!tabs || !ids || !tabs->SupportsTabGroups()) {
    return;
  }
  // Optional, and as long as `ids` when it is there: the tab that opened each
  // one, or an empty string for the pages the reader opened themselves. The
  // engine cannot work these out after a restart -- it tracks openers by
  // session id, and every one of these tabs is new -- so the shell, which
  // wrote down the relation before the shutdown, hands it back.
  const base::ListValue* opener_ids = command.FindList("openerIds");
  if (opener_ids && opener_ids->size() != ids->size()) {
    LOG(WARNING) << "OHOS tab groups: openerIds has " << opener_ids->size()
                 << " entries for " << ids->size() << " tabs, ignoring them";
    opener_ids = nullptr;
  }
  std::vector<int> indices;
  for (size_t at = 0; at < ids->size(); ++at) {
    const base::Value& id = (*ids)[at];
    if (!id.is_string()) {
      continue;
    }
    const std::string value = id.GetString();
    content::WebContents* contents = FindTabById(tabs, &value);
    if (!contents) {
      continue;
    }
    if (const std::optional<int> index =
            tabs->GetIndexOfWebContents(contents)) {
      indices.push_back(*index);
    }
    if (!opener_ids) {
      continue;
    }
    const base::Value& opener_id = (*opener_ids)[at];
    if (!opener_id.is_string() || opener_id.GetString().empty()) {
      continue;
    }
    const std::string opener_value = opener_id.GetString();
    chrome::ohos::RecordPageOpener(contents,
                                   FindTabById(tabs, &opener_value));
  }
  if (indices.size() < 2) {
    return;
  }
  // AddToNewGroup wants them in ascending order and moves them together
  // afterwards, so the group ends up in tab-strip order rather than in the
  // order the shell listed them. That is the same order the shell restored
  // them in, so the two agree.
  std::ranges::sort(indices);
  indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
  tabs->AddToNewGroup(indices);
}

// Close a tab the shell named, and afterwards go where it asked.
void CloseTabById(gfx::AcceleratedWidget widget,
                  BrowserWindowInterface* browser,
                  TabStripModel* tabs,
                  const base::DictValue& command) {
  content::WebContents* target = FindTabById(tabs, command.FindString("id"));
  if (!target) {
    return;
  }
  const std::optional<int> index = tabs->GetIndexOfWebContents(target);
  if (!index) {
    return;
  }
  // Read before the close: the tab is gone by the time it returns, and with
  // it the entry saying who opened it.
  content::WebContents* opener =
      command.FindBool("returnToOpener").value_or(false)
          ? chrome::ohos::PageOpenerOf(target)
          : nullptr;
  if (tabs->count() <= 1) {
    // The last tab closing would close the window. The shell expects a
    // browser to still be there, so the tab empties instead, which is what
    // closeTab has always done.
    NavigateOnUiThread(widget, GURL("chrome://newtab/"), 0);
    return;
  }
  // Read before the close, and paired with the entry it produces, so undoing
  // the close can put the reader back where they were.
  content::WebContents* had_opener = chrome::ohos::PageOpenerOf(target);
  tabs->CloseWebContentsAt(*index, kCloseAndRemember);
  RememberOpenerOfClosedTab(RestoreServiceFor(browser), had_opener);
  if (!opener) {
    // Whatever Chromium picked. Its rule -- the tab to the right, or the one
    // that opened this one if it knows -- is the same rule every browser
    // uses and there is no reason to have a different one.
    return;
  }
  if (const std::optional<int> back = tabs->GetIndexOfWebContents(opener)) {
    tabs->ActivateTabAt(*back);
  }
}

// --- Reader mode. ---------------------------------------------------------

// Whether this tab is showing a distilled page rather than the original.
bool IsInReaderMode(content::WebContents* contents) {
  return contents && contents->GetLastCommittedURL().SchemeIs(
                         dom_distiller::kDomDistillerScheme);
}

// Whether the distiller thinks this page is an article. Blink reports this
// after each navigation and the driver keeps the last answer, so reading it
// costs nothing -- which matters when the state is polled every 200ms.
bool IsReaderModeAvailable(content::WebContents* contents) {
  if (!contents || IsInReaderMode(contents)) {
    return false;
  }
  // Through the helper rather than DistillabilityDriver::FromWebContents:
  // the helper creates the driver, and nothing else here ever did. Without
  // it BindDistillabilityService returns early, the renderer's service is
  // never bound, Blink never reports anything, and reader mode was offered
  // on no page at all.
  const std::optional<dom_distiller::DistillabilityResult> result =
      dom_distiller::GetLatestResult(contents);

  // Nothing is logged here. This runs at the shell's poll rate, and the
  // deduplication that was supposed to keep it quiet did not: the answer
  // arrives in two stages and flips between them. The renderer says the same
  // thing once per page, with the numbers it decided on, which is the line
  // worth having.
  return result && result->is_distillable;
}

void ToggleReaderMode(content::WebContents* contents) {
  if (!contents) {
    return;
  }
  if (IsInReaderMode(contents)) {
    // Back to the page it was distilled from. The distilled URL carries the
    // original inside it, which is how the back button finds its way home
    // too.
    const GURL original = dom_distiller::url_utils::GetOriginalUrlFromDistillerUrl(
        contents->GetLastCommittedURL());
    if (original.is_valid()) {
      contents->GetController().LoadURL(original, content::Referrer(),
                                        ui::PAGE_TRANSITION_AUTO_BOOKMARK,
                                        std::string());
    }
    return;
  }
  // DistillCurrentPage() alone distils and stops: nothing navigates, so the
  // menu item did nothing visible even when it did not crash. This is the
  // entry point that shows the result, and it says whether there was one --
  // an article that distils to nothing leaves the reader on the page they
  // were on, with no explanation unless the log gives one.
  LOG(WARNING) << "OHOS reader mode: asked to distil "
               << contents->GetLastCommittedURL().possibly_invalid_spec();
  DistillCurrentPageAndViewIfSuccessful(
      contents, base::BindOnce([](bool success) {
        LOG(WARNING) << "OHOS reader mode: distilled=" << success;
      }));
}

// --- Recently closed tabs. ------------------------------------------------
//
// Windows are left out on purpose: the shell has one window per browser and
// nothing to restore a window into.

// Who opened the tab behind each recently-closed entry.
//
// The opener does not travel with the entry. TabRestoreService builds it from
// the live tab through PopulateTab(), which copies navigations, the user
// agent and a few other things but not the tab's session extra data -- that
// is only filled in when an entry is read back from the session file, which
// is the restart case rather than this one. So the pair is kept here, keyed
// by the entry the close produced, for as long as that entry exists.
//
// Only closes the engine performs are recorded. A page closing itself with
// window.close() is not, which is the honest limit: the shell's undo is what
// this is for, and that always comes through here.
std::map<int, base::WeakPtr<content::WebContents>>& OpenersByClosedEntry() {
  static base::NoDestructor<std::map<int, base::WeakPtr<content::WebContents>>>
      map;
  return *map;
}

// Call with the opener read before the close; pairs it with whatever entry
// the close just produced.
void RememberOpenerOfClosedTab(sessions::TabRestoreService* service,
                               content::WebContents* opener) {
  if (!service || !opener || service->entries().empty()) {
    return;
  }
  OpenersByClosedEntry()[service->entries().front()->id.id()] =
      opener->GetWeakPtr();
}

sessions::TabRestoreService* RestoreServiceFor(
    BrowserWindowInterface* browser) {
  Profile* profile = browser ? browser->GetProfile() : nullptr;
  if (!profile || profile->IsOffTheRecord()) {
    // An incognito tab that closes leaves no trace, which is the point.
    return nullptr;
  }
  return TabRestoreServiceFactory::GetForProfile(profile);
}

void SendRecentlyClosed(gfx::AcceleratedWidget widget,
                        BrowserWindowInterface* browser,
                        const base::DictValue& command) {
  base::ListValue items;
  sessions::TabRestoreService* service = RestoreServiceFor(browser);
  if (service) {
    EnsureShellListWatcher(browser->GetProfile());
    // The service reads the last session lazily; without this a restart shows
    // an empty list until something else closes a tab.
    service->LoadTabsFromLastSession();
    const int max = std::clamp(
        static_cast<int>(command.FindDouble("maxCount").value_or(25.0)), 1,
        100);
    for (const auto& entry : service->entries()) {
      if (static_cast<int>(items.size()) >= max) {
        break;
      }
      if (entry->type != sessions::tab_restore::Type::TAB) {
        continue;
      }
      const auto& tab = static_cast<const sessions::tab_restore::Tab&>(*entry);
      if (tab.navigations.empty()) {
        continue;
      }
      const sessions::SerializedNavigationEntry& current =
          tab.navigations[tab.normalized_navigation_index()];
      base::DictValue item;
      item.Set("id", base::NumberToString(entry->id.id()));
      item.Set("title", base::UTF16ToUTF8(current.title()));
      item.Set("url", ShellVisibleUrl(current.virtual_url()));
      item.Set("closedTime", entry->timestamp.is_null()
                                 ? -1.0
                                 : entry->timestamp
                                       .InMillisecondsFSinceUnixEpochIgnoringNull());
      items.Append(std::move(item));
    }
  }
  base::DictValue event;
  event.Set("event", "recentlyClosed");
  event.Set("requestId", command.FindInt("requestId").value_or(0));
  event.Set("items", std::move(items));
  DispatchRuntimeEvent(widget, std::move(event));
}

void RestoreRecentlyClosed(BrowserWindowInterface* browser,
                           const base::DictValue& command) {
  sessions::TabRestoreService* service = RestoreServiceFor(browser);
  if (!service) {
    return;
  }
  service->LoadTabsFromLastSession();
  sessions::LiveTabContext* context =
      BrowserLiveTabContext::FindContextForWebContents(
          browser->GetTabStripModel()->GetActiveWebContents());
  const std::string* id = command.FindString("id");
  SessionID entry = SessionID::InvalidValue();
  if (!id || id->empty()) {
    // The most recent one, which is what a single "reopen" button does.
    //
    // Not RestoreMostRecentEntry(): it restores with disposition UNKNOWN, and
    // UNKNOWN means "put it back where it came from" -- TabRestoreServiceHelper
    // throws away the context it was given and looks the tab's original window
    // up by id instead. That window is this one, but it is not registered under
    // the id the closed tab remembers, so the lookup fails, and a failed lookup
    // means a brand new browser window is created to hold the tab. On a phone
    // that window is nowhere the reader can see, so the reopen looked like it
    // did nothing.
    if (service->entries().empty()) {
      return;
    }
    entry = service->entries().front()->id;
  } else if (const std::optional<int64_t> value = FromShellId(id)) {
    entry = SessionID::FromSerializedValue(*value);
  }
  if (!entry.is_valid()) {
    return;
  }
  auto remembered = OpenersByClosedEntry().find(entry.id());
  content::WebContents* opener =
      remembered != OpenersByClosedEntry().end() ? remembered->second.get()
                                                 : nullptr;
  if (remembered != OpenersByClosedEntry().end()) {
    OpenersByClosedEntry().erase(remembered);
  }
  service->RestoreEntryById(context, entry,
                            WindowOpenDisposition::NEW_FOREGROUND_TAB);
  // The restored tab is the active one, and it is a new tab with a new id, so
  // the relation has to be made again rather than found. Skipped when the
  // opener has itself been closed in the meantime.
  if (opener) {
    chrome::ohos::RecordPageOpener(
        browser->GetTabStripModel()->GetActiveWebContents(), opener);
  }
}

// What the zoom menu shows, and what Ctrl+0 returns to. 100 when the tab has
// no zoom controller, which is the same thing the user would read as "normal".
int ShellZoomPercent(content::WebContents* contents) {
  zoom::ZoomController* controller =
      contents ? zoom::ZoomController::FromWebContents(contents) : nullptr;
  return controller ? controller->GetZoomPercent() : 100;
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
  // 0..1, and 1 when nothing is loading, so the shell can draw one progress
  // bar without special-casing the idle state.
  state.Set("loadProgress", 1.0);
  state.Set("requestDesktopSite", false);
  state.Set("zoomPercent", 100);
  state.Set("inReaderMode", false);
  state.Set("readerModeAvailable", false);
  state.Set("canGoBack", false);
  state.Set("canGoForward", false);
  state.Set("isPwaWindow", false);
  // Whether newIncognitoWindow will do anything on this device. False on
  // phones, where --single-process rules out a second profile entirely.
  state.Set("incognitoSupported",
            !content::RenderProcessHost::run_renderer_in_process());
  state.Set("isIncognito", browser && browser->GetProfile() &&
                               browser->GetProfile()->IsOffTheRecord());
  state.Set("pwaAppId", "");
  state.Set("pwaStartUrl", "");
  state.Set("bookmarked", false);

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
  // A group with one tab left is not a group. Checked here rather than on
  // every close, because a tab leaves a group in more ways than the shell
  // asking it to -- the page can close itself, a crash can take it, a drag
  // can move it out -- and this runs after all of them. The change is made
  // after this snapshot rather than during it, so the strip is not edited
  // while it is being read; the next snapshot carries the result.
  if (tabs->SupportsTabGroups()) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&DissolveSingleTabGroupsForWidget, widget));
  }

  // Where the reader is in the page, for app continuation. Attached to the
  // tab in front of them, which is the one the system will ask about; doing
  // it here rather than on tab creation covers tabs that existed before this
  // build and costs a map lookup.
  chrome::ohos::WatchPageScroll(tabs->GetActiveWebContents());

  // Images written for a share that never happened do not outlive the run
  // that wrote them. Once, on the first snapshot of the first window.
  static bool cleared_shared_images = false;
  if (!cleared_shared_images) {
    cleared_shared_images = true;
    chrome::ohos::ClearSharedImageDirectory();
  }

  base::ListValue tab_values;
  for (int index = 0; index < tabs->count(); ++index) {
    content::WebContents* contents = tabs->GetWebContentsAt(index);
    base::DictValue tab;
    tab.Set("index", index);
    tab.Set("active", index == tabs->active_index());
    tab.Set("url", "");
    tab.Set("title", "");
    tab.Set("loading", false);
    // Stable for the tab's life and never reused. An index shifts whenever a
    // neighbour closes or moves, so anything asynchronous -- a thumbnail
    // arriving, a drag finishing -- has to name the tab by this instead.
    tab.Set("id", ShellTabId(contents));
    // Which group this tab is in, and which tab opened it. Both empty for a
    // tab that stands on its own, which is every tab until a page opens
    // another one. The engine decides membership so that one answer serves
    // every window and survives a restart; the shell decides how to draw it.
    const std::optional<tab_groups::TabGroupId> group =
        tabs->SupportsTabGroups() ? tabs->GetTabGroupForTab(index)
                                  : std::nullopt;
    tab.Set("groupId", group ? group->ToString() : std::string());
    tab.Set("openerId", chrome::ohos::PageOpenerIdOf(contents));
    if (contents) {
      const GURL url = contents->GetVisibleURL();
      tab.Set("url", ShellVisibleUrl(url));
      tab.Set("title", base::UTF16ToUTF8(contents->GetTitle()));
      tab.Set("loading", contents->IsLoading());
      // The tab strip shows a speaker on a tab making noise and a crossed-out
      // one on a tab the user silenced; they are different states and a tab
      // can be muted without ever having made a sound.
      tab.Set("audible", contents->IsCurrentlyAudible());
      tab.Set("muted", contents->IsAudioMuted());
    }
    tab_values.Append(std::move(tab));
  }
  state.Set("tabs", std::move(tab_values));

  // The snapshot is built for every window regularly, which makes it the
  // place the shell services' observers get started early enough to see the
  // first download or bookmark change.
  EnsureShellServices(browser->GetProfile());
  content::WebContents* active = tabs->GetActiveWebContents();
  if (active) {
    const GURL url = active->GetVisibleURL();
    state.Set("bookmarked", IsUrlBookmarked(browser->GetProfile(), url));
    const std::string visible_url = ShellVisibleUrl(url);
    state.Set("url", visible_url);
    state.Set("domain", url.host().empty() ? visible_url : url.host());
    state.Set("title", base::UTF16ToUTF8(active->GetTitle()));
    state.Set("loading", active->IsLoading());
    state.Set("loadProgress",
              active->IsLoading() ? active->GetLoadProgress() : 1.0);
    state.Set("requestDesktopSite", IsRequestingDesktopSite(active));
    state.Set("zoomPercent", ShellZoomPercent(active));
    state.Set("inReaderMode", IsInReaderMode(active));
    state.Set("readerModeAvailable", IsReaderModeAvailable(active));
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

void FindResultRelay::OnFindResultAvailable(content::WebContents* web_contents) {
  auto* helper = find_in_page::FindTabHelper::FromWebContents(web_contents);
  BrowserWindowInterface* browser = FindBrowserForWebContents(web_contents);
  if (!helper || !browser) {
    return;
  }
  const find_in_page::FindNotificationDetails& result = helper->find_result();
  base::DictValue event;
  event.Set("event", "findResult");
  event.Set("matches", result.number_of_matches());
  event.Set("activeMatch", result.active_match_ordinal());
  event.Set("finalUpdate", result.final_update());
  DispatchRuntimeEvent(GetBrowserWidget(browser), std::move(event));
}

// A shell may float its own bar over the bottom of the page and still let
// the page draw behind it. The inset shrinks the visible viewport by that much,
// the way an on-screen keyboard does, so the end of the page can scroll up
// above the bar instead of staying underneath it. Setting the same insets again
// is a no-op in the view, which is what lets the poll below re-apply them to
// whatever view is current -- a tab switch, a new tab and a cross-site
// navigation each bring a different one.
// Chromium's browser controls height is in physical pixels (see the comment
// on RenderViewHostDelegateView), but the shell hands it over in vp, so this
// converts using the primary display's scale factor.
int DipToPixels(int dip) {
  const float scale =
      display::Screen::Get()->GetPrimaryDisplay().device_scale_factor();
  return static_cast<int>(std::lround(dip * scale));
}

// Defined next to OnAuraShellTopControlsShownRatio.
void ApplyTopControlsOffset(content::WebContents* contents, float ratio);

// The page's bottom is covered by the shell's floating bar (bottom_dip) and,
// once ApplyTopControlsOffset() moves the page down, also pushed off the
// screen by the controls: the renderer lets the viewport grow as the controls
// hide but never shrinks it for the controls it shows, so the end of the page
// sits one full controls height too low. Inset the bottom by both so the last
// line still scrolls up to just above the floating bar.
void ApplyViewportInsets(content::WebContents* contents, int bottom_dip) {
  if (!contents) {
    return;
  }
  int top_dip = 0;
  {
    RuntimeBridgeState& state = GetState();
    base::AutoLock lock(state.lock);
    top_dip = state.top_controls_height;
  }
  if (content::RenderWidgetHostView* view =
          contents->GetRenderWidgetHostView()) {
    view->SetInsets(gfx::Insets::TLBR(
        0, 0, std::max(0, bottom_dip) + std::max(0, top_dip), 0));
  }
}

// Makes the renderer pick up a new top controls height and starts the tab with
// its controls showing, free to hide them as the page scrolls. Without the
// explicit kShown the renderer would start from a shown ratio of 0 -- controls
// hidden -- and the top of the page would sit under the shell's bar.
void ShowBrowserControls(content::WebContents* contents) {
  if (!contents) {
    return;
  }
  if (content::RenderWidgetHostView* view =
          contents->GetRenderWidgetHostView()) {
    view->GetRenderWidgetHost()->SynchronizeVisualProperties();
  }
  contents->UpdateBrowserControlsState(cc::BrowserControlsState::kBoth,
                                       cc::BrowserControlsState::kShown,
                                       /*animate=*/false, std::nullopt);
}

void PollBrowserStateOnUiThread(uint64_t generation) {
  std::string ui_family;
  std::map<gfx::AcceleratedWidget, int> bottom_insets;
  int top_controls_height = 0;
  {
    RuntimeBridgeState& state = GetState();
    base::AutoLock lock(state.lock);
    if (!state.ui_task_runner || state.browser_generation != generation) {
      return;
    }
    ui_family = state.ui_family;
    bottom_insets = state.viewport_bottom_inset;
    top_controls_height = state.top_controls_height;
  }

  const bool mobile = IsMobileUiFamily(ui_family);
  ApplyUserAgentToAllTabs(mobile, /*reload=*/false);

  std::vector<std::pair<gfx::AcceleratedWidget, std::string>> snapshots;
  if (GlobalBrowserCollection* browsers =
          GlobalBrowserCollection::GetInstance()) {
    browsers->ForEach(
        [&snapshots, &ui_family, &bottom_insets, top_controls_height](
            BrowserWindowInterface* browser) {
          const gfx::AcceleratedWidget widget = GetBrowserWidget(browser);
          if (widget != gfx::kNullAcceleratedWidget) {
            const auto inset = bottom_insets.find(widget);
            TabStripModel* tabs = browser->GetTabStripModel();
            if (inset != bottom_insets.end() && tabs) {
              ApplyViewportInsets(tabs->GetActiveWebContents(), inset->second);
            }
            if (top_controls_height > 0 && tabs) {
              content::WebContents* active = tabs->GetActiveWebContents();
              content::RenderWidgetHostView* view =
                  active ? active->GetRenderWidgetHostView() : nullptr;
              bool switched = false;
              {
                RuntimeBridgeState& state = GetState();
                base::AutoLock lock(state.lock);
                uintptr_t& shown = state.controls_shown_for[widget];
                switched = view && shown != reinterpret_cast<uintptr_t>(view);
                if (view) {
                  shown = reinterpret_cast<uintptr_t>(view);
                }
              }
              if (switched) {
                LOG(WARNING) << "OHOS browser controls: showing on new view";
                ShowBrowserControls(active);
              }
            }
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

// Whether a widget belonging to the browser window -- a permission prompt, the
// Bluetooth device chooser -- holds activation right now. Owned and child
// widgets are both checked: bubbles are parented to the browser's native view,
// and which of the two relations aura records for them depends on how they
// were created.
bool BrowserOwnedWidgetIsActive(BrowserWindowInterface* browser) {
  BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(browser);
  views::Widget* browser_widget =
      browser_view ? browser_view->GetWidget() : nullptr;
  if (!browser_widget) {
    return false;
  }
  const gfx::NativeView native_view = browser_widget->GetNativeView();
  for (const views::Widget::Widgets& widgets :
       {views::Widget::GetAllOwnedWidgets(native_view),
        views::Widget::GetAllChildWidgets(native_view)}) {
    for (views::Widget* candidate : widgets) {
      if (candidate != browser_widget && candidate->IsActive()) {
        return true;
      }
    }
  }
  return false;
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
    // Regaining focus must not take activation from the browser's own bubble.
    // A desktop window manager hands activation back to whichever of the
    // app's windows had it; this port does not, so activating the browser
    // window here deactivated the bubble instead. That is how the first
    // requestDevice() of the app's life failed: the OS Bluetooth dialog
    // closes, the chooser opens, the app's focus report arrives a moment
    // later, and the chooser -- which closes on deactivation -- was gone with
    // "NotFoundError: User cancelled".
    if (!BrowserOwnedWidgetIsActive(browser)) {
      window->Activate();
      if (TabStripModel* tabs = browser->GetTabStripModel()) {
        if (content::WebContents* active = tabs->GetActiveWebContents()) {
          active->Focus();
        }
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
    // Record the outcome before running the continuation: the thing waiting
    // on it is about to ask whether the permission is allowed.
    for (const auto& [answered, outcome] :
         {std::make_pair(granted_values,
                         ::system_permission_settings::SystemPermission::
                             kAllowed),
          std::make_pair(denied_values,
                         ::system_permission_settings::SystemPermission::
                             kDenied)}) {
      if (!answered) {
        continue;
      }
      for (const base::Value& entry : *answered) {
        const std::string* permission = entry.GetIfString();
        std::optional<ContentSettingsType> type =
            permission ? ContentTypeForOhosPermission(*permission)
                       : std::nullopt;
        if (type) {
          ::system_permission_settings::SetOhosSystemPermission(*type,
                                                                outcome);
        }
      }
    }
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

  if (*name == "contextMenuAction") {
    const std::optional<int> request_id = command.FindInt("requestId");
    const std::string* action = command.FindString("action");
    if (request_id && action) {
      RunShellContextMenuAction(*request_id, *action);
    }
    return;
  }

  if (*name == "contextMenuDismissed") {
    if (const std::optional<int> request_id = command.FindInt("requestId")) {
      DismissShellContextMenu(*request_id);
    }
    return;
  }

  if (*name == "passwordAuthReset") {
    // The shell saw something that should end the reuse window early: it went
    // to the background, or moved between an ordinary and an incognito
    // window. Screen lock the engine watches for itself.
    const std::string* why = command.FindString("reason");
    ForgetRecentAuthentication(why ? why->c_str() : "the shell asked");
    return;
  }

  if (*name == "setBrowserChrome") {
    if (const std::string* mode = command.FindString("mode")) {
      SetAuraShellBrowserChrome(*mode);
    }
    return;
  }

  if (*name == "setAnchorRects") {
    SetAuraShellAnchorRects(widget, command);
    return;
  }

  if (*name == "sitePermissionDecision") {
    // The user answered the sheet the shell drew for
    // sitePermissionRequested. Handled before the browser lookup below: a
    // question can outlive the window that asked it.
    const std::optional<int> id = command.FindInt("requestId");
    const std::string* decision = command.FindString("decision");
    if (id && decision) {
      ResolveShellPermissionRequest(*id, *decision);
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
  if (*name == "setDownloadDirectory" && !browser) {
    // Sent as the shell's page appears, often before the browser exists; the
    // directory is kept and applied once a profile is there.
    if (const std::string* uri = command.FindString("uri")) {
      SetShellDownloadDirectory(nullptr, *uri);
    }
    return;
  }
  if (!browser || !tabs) {
    LOG(ERROR) << "OHOS Aura shell command has no active browser: " << *name;
    return;
  }

  if (IsShellServiceCommand(*name)) {
    HandleShellServiceCommand({widget, browser, browser->GetProfile()}, *name,
                              command);
    return;
  }

  content::WebContents* active = tabs->GetActiveWebContents();
  if (*name == "findInPage" && active) {
    const std::string* text = command.FindString("text");
    auto* helper = find_in_page::FindTabHelper::FromWebContents(active);
    if (text && helper) {
      FindResultRelay::Get().Watch(helper);
      helper->StartFinding(base::UTF8ToUTF16(*text),
                           command.FindBool("forward").value_or(true),
                           /*case_sensitive=*/false, /*find_match=*/true);
    }
    return;
  }
  if (*name == "stopFind" && active) {
    if (auto* helper = find_in_page::FindTabHelper::FromWebContents(active)) {
      helper->StopFinding(find_in_page::SelectionAction::kClear);
    }
    return;
  }
  if (*name == "getPageText" && active) {
    // The shell's summarizer reads the page's text. An isolated world keeps
    // the page's own scripts from seeing, or tampering with, the read.
    const int request_id = command.FindInt("requestId").value_or(0);
    active->GetPrimaryMainFrame()->ExecuteJavaScriptInIsolatedWorld(
        u"(document.body && document.body.innerText || '').slice(0, 20000)",
        base::BindOnce(
            [](gfx::AcceleratedWidget widget, int request_id,
               base::Value result) {
              base::DictValue event;
              event.Set("event", "pageText");
              event.Set("requestId", request_id);
              event.Set("text", result.is_string() ? result.GetString()
                                                   : std::string());
              DispatchRuntimeEvent(widget, std::move(event));
            },
            widget, request_id),
        ISOLATED_WORLD_ID_CHROME_INTERNAL);
    return;
  }
  if (*name == "setBrowserControls") {
    const int top = std::max(0, command.FindInt("top").value_or(0));
    const int min_top =
        std::clamp(command.FindInt("minTop").value_or(0), 0, top);
    int bottom_inset = 0;
    {
      RuntimeBridgeState& state = GetState();
      base::AutoLock lock(state.lock);
      state.top_controls_height = top;
      state.top_controls_min_height = min_top;
      state.controls_shown_for[widget] = reinterpret_cast<uintptr_t>(
          active ? active->GetRenderWidgetHostView() : nullptr);
      state.last_shown_ratio = -1.0f;
      bottom_inset = state.viewport_bottom_inset[widget];
    }
    LOG(WARNING) << "OHOS browser controls: top=" << top << " min=" << min_top;
    // The bottom inset includes the controls height; see ApplyViewportInsets().
    ApplyViewportInsets(active, bottom_inset);
    if (top == 0) {
      // No controls any more: put the page back at the top of the window.
      ApplyTopControlsOffset(active, 0.0f);
    }
    ShowBrowserControls(active);
    return;
  }
  if (*name == "setBrowserControlsState" && active) {
    const std::string* value = command.FindString("state");
    cc::BrowserControlsState current = cc::BrowserControlsState::kBoth;
    if (value && *value == "shown") {
      current = cc::BrowserControlsState::kShown;
    } else if (value && *value == "hidden") {
      current = cc::BrowserControlsState::kHidden;
    }
    active->UpdateBrowserControlsState(
        cc::BrowserControlsState::kBoth, current,
        command.FindBool("animate").value_or(true), std::nullopt);
    return;
  }
  if (*name == "setViewportInsets") {
    const int bottom = std::max(0, command.FindInt("bottom").value_or(0));
    {
      RuntimeBridgeState& state = GetState();
      base::AutoLock lock(state.lock);
      state.viewport_bottom_inset[widget] = bottom;
    }
    ApplyViewportInsets(active, bottom);
    return;
  }
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
  } else if (*name == "newIncognitoWindow") {
    // A Browser is bound to one Profile, so incognito tabs cannot join this
    // window's tab strip -- desktop Chrome opens a second window for them and
    // so does this. The off-the-record profile is derived from the one already
    // open, and Chromium clears it when the last window using it closes.
    //
    // Not everywhere, though. A second profile needs a second renderer
    // process, and a device that denies native child processes leaves Chromium
    // in --single-process, where RenderProcessHostImpl asserts outright that
    // "Single-process mode does not support multiple browser contexts." Asking
    // anyway is a guaranteed crash, so refuse here and say so in the state the
    // shell already reads (incognitoSupported), which lets it disable the menu
    // item rather than offer a button that kills the browser.
    if (content::RenderProcessHost::run_renderer_in_process()) {
      LOG(WARNING) << "OHOS Aura shell refused newIncognitoWindow: this device "
                      "runs single-process, which cannot hold a second profile";
      return;
    }
    Profile* profile = browser->GetProfile();
    if (profile) {
      Profile* otr = profile->GetOffTheRecordProfile(
          Profile::OTRProfileID::PrimaryID(), /*create_if_needed=*/true);
      if (otr) {
        BrowserWindowCreateParams create_params(otr, /*from_user_gesture=*/true);
        if (BrowserWindowInterface* incognito =
                CreateBrowserWindow(std::move(create_params))) {
          incognito->OpenGURL(GURL("chrome://newtab/"),
                              WindowOpenDisposition::NEW_FOREGROUND_TAB);

        }
      }
    }
  } else if (*name == "newTab") {
    const std::string* url_string = command.FindString("url");
    GURL url = url_string && !url_string->empty() ? GURL(*url_string)
                                                  : GURL("chrome://newtab/");
    if (!url.is_valid()) {
      url = GURL("chrome://newtab/");
    }
    const bool background = command.FindBool("background").value_or(false);
    const std::optional<tab_groups::TabGroupId> group =
        FindGroupById(tabs, command.FindString("groupId"));
    const WindowOpenDisposition disposition =
        background ? WindowOpenDisposition::NEW_BACKGROUND_TAB
                   : WindowOpenDisposition::NEW_FOREGROUND_TAB;
    if (group) {
      // Into the named group, at its end. PAGE_TRANSITION_LINK rather than
      // TYPED: this came from a link in a page, and the transition is what
      // history and the back button read afterwards.
      NavigateParams params(browser, url, ui::PAGE_TRANSITION_LINK);
      params.disposition = disposition;
      params.group = group;
      params.source_contents = active;
      // The end of the group, not beside the tab that asked. Left to itself
      // TabStripModel puts a new tab next to its opener, which is right for a
      // strip and wrong for a row of favicons the reader reads left to right:
      // pages should arrive at the end in the order they were opened.
      if (TabGroup* model = tabs->group_model()->GetTabGroup(*group)) {
        const gfx::Range range = model->ListTabs();
        if (!range.is_empty()) {
          params.tabstrip_index = static_cast<int>(range.end());
        }
      }
      Navigate(&params);
      content::WebContents* opened = params.navigated_or_inserted_contents;
      chrome::ohos::RecordPageOpener(opened, active);
      chrome::ohos::AnnouncePageOpenedInGroup(opened);
    } else {
      content::OpenURLParams params(url, content::Referrer(), disposition,
                                    ui::PAGE_TRANSITION_TYPED,
                                    /*is_renderer_initiated=*/false);
      content::WebContents* opened = browser->OpenURL(params, {});
      // Continuing a page from another device: the same article, in the same
      // place, in a window of a different width -- so a fraction of the
      // document rather than a pixel offset.
      if (const std::optional<double> ratio =
              command.FindDouble("scrollRatio")) {
        chrome::ohos::RestoreScrollRatioOnce(opened, *ratio);
      }
    }
  } else if (*name == "handleBack") {
    // The system's back gesture, offered to the page before the tab acts on
    // it. A page with a <dialog> open, in fullscreen, or with a CloseWatcher
    // of its own expects back to close that first -- which is what it does
    // in every other browser -- and the shell cannot know that from outside.
    base::DictValue event;
    event.Set("event", "backHandled");
    event.Set("requestId", command.FindInt("requestId").value_or(0));
    event.Set("handled", active && active->SignalCloseWatcherIfActive());
    DispatchRuntimeEvent(widget, std::move(event));
  } else if (*name == "insertText" && active) {
    // The shell read the system pasteboard for us. Chromium cannot: reading
    // it needs READ_PASTEBOARD, a restricted permission this app does not
    // declare, so ui::Clipboard only ever serves back what the browser itself
    // copied. A paste control in the shell is the way the platform intends an
    // app to get at it without that permission, and this puts what it read
    // into whatever the reader has focused.
    const std::string* text = command.FindString("text");
    if (text && !text->empty()) {
      // Put it in the browser's own clipboard and then paste, rather than
      // inject it: paste is the editing path, so the page sees the input
      // events it expects, undo works, and a contenteditable behaves the
      // same as an <input>. Writing the clipboard on the way through is not
      // a side effect to apologise for -- the reader just pasted, and the
      // browser's idea of the clipboard should agree with the system's.
      {
        ui::ScopedClipboardWriter writer(ui::ClipboardBuffer::kCopyPaste);
        writer.WriteText(base::UTF8ToUTF16(*text));
      }
      active->Paste();
    }
  } else if (*name == "groupTabs") {
    GroupTabsById(tabs, command);
  } else if (*name == "getPageContinuation") {
    chrome::ohos::ReadPageContinuation(
        active, command.FindInt("requestId").value_or(0));
  } else if (*name == "activateTabById") {
    if (content::WebContents* target =
            FindTabById(tabs, command.FindString("id"))) {
      if (const std::optional<int> index =
              tabs->GetIndexOfWebContents(target)) {
        tabs->ActivateTabAt(*index);
      }
    }
  } else if (*name == "closeTabById") {
    CloseTabById(widget, browser, tabs, command);
  } else if (*name == "activateTab") {
    if (const std::optional<int> index = ReadTabIndex(tabs, command)) {
      tabs->ActivateTabAt(*index);
    }
  } else if (*name == "closeTab") {
    const int index =
        ReadTabIndex(tabs, command).value_or(tabs->active_index());
    if (tabs->ContainsIndex(index) && tabs->count() > 1) {
      content::WebContents* had_opener =
          chrome::ohos::PageOpenerOf(tabs->GetWebContentsAt(index));
      tabs->CloseWebContentsAt(index, kCloseAndRemember);
      RememberOpenerOfClosedTab(RestoreServiceFor(browser), had_opener);
    } else if (active) {
      NavigateOnUiThread(widget, GURL("chrome://newtab/"), 0);
    }
  } else if (*name == "moveTabToNewWindow") {
    // A tab dragged off the strip. Chromium's own command does the work --
    // detaching the tab, opening a window for it, keeping the session id --
    // and the shell hears about the window the same way it hears about an
    // incognito one: an auxiliary window event with windowRole "browser" and
    // an aura_win_<widget> surface to draw it in.
    //
    // Same profile as the window it left, so this works on a phone too: the
    // assertion single-process trips over is about holding two profiles, not
    // two windows.
    const std::optional<int> index = ReadTabIndex(tabs, command);
    if (index && tabs->count() > 1 &&
        chrome::CanMoveTabsToNewWindow(browser, {*index})) {
      chrome::MoveTabsToNewWindow(browser, {*index});
    }
  } else if (*name == "moveTab") {
    // to_position is where the tab ends up, which is what a finished drag
    // knows; TabStripModel takes the same meaning.
    const std::optional<int> from = ReadTabIndex(tabs, command);
    const std::optional<int> to = command.FindInt("toIndex");
    if (from && to) {
      tabs->MoveWebContentsAt(*from, std::clamp(*to, 0, tabs->count() - 1),
                              /*select_after_move=*/false);
    }
  } else if (*name == "setRequestDesktopSite" && active) {
    SetRequestDesktopSite(active, command.FindBool("enabled").value_or(false));
  } else if (*name == "setTabMuted") {
    content::WebContents* target = FindTabById(tabs, command.FindString("id"));
    if (!target) {
      target = active;
    }
    if (target) {
      target->SetAudioMuted(command.FindBool("muted").value_or(false));
    }
  } else if (*name == "setZoom" && active) {
    if (zoom::ZoomController* controller =
            zoom::ZoomController::FromWebContents(active)) {
      const std::optional<double> percent = command.FindDouble("percent");
      if (percent) {
        // Clamped rather than refused: the shell's + and - walk a list of
        // steps and the ends of that list are not this code's business.
        controller->SetZoomLevel(blink::ZoomFactorToZoomLevel(
            std::clamp(*percent, 25.0, 500.0) / 100.0));
      } else {
        // No percent means "back to normal", which is what Ctrl+0 does.
        controller->SetZoomLevel(blink::ZoomFactorToZoomLevel(1.0));
      }
    }
  } else if (*name == "getExtensionActions") {
    SendExtensionActions(widget, browser, command);
  } else if (*name == "runExtensionAction") {
    RunExtensionAction(browser, command);
  } else if (*name == "setExtensionPinned") {
    SetExtensionPinned(browser, command);
  } else if (*name == "getTabThumbnails") {
    SendTabThumbnails(widget, tabs, command);
  } else if (*name == "toggleReaderMode" && active) {
    ToggleReaderMode(active);
  } else if (*name == "getRecentlyClosed") {
    SendRecentlyClosed(widget, browser, command);
  } else if (*name == "restoreRecentlyClosed") {
    RestoreRecentlyClosed(browser, command);
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

int GetAuraShellTopControlsHeight() {
  RuntimeBridgeState& state = GetState();
  base::AutoLock lock(state.lock);
  return DipToPixels(state.top_controls_height);
}

int GetAuraShellTopControlsMinHeight() {
  RuntimeBridgeState& state = GetState();
  base::AutoLock lock(state.lock);
  return DipToPixels(state.top_controls_min_height);
}

namespace {

// Chrome on Android moves the page itself: the browser draws the tab's content
// layer lower by the part of the controls that is showing (the content offset
// its scene layers apply). Aura has no such step, so the renderer shrinks the
// viewport for the controls while the page still starts at the top of the
// window, under the shell's bar. Translate the view's window by the same
// amount so the page begins right below the visible part of the bar and
// follows it as it slides. Chromium is given the height in physical pixels
// while a window's transform is in its DIPs, so divide by the window's scale.
void ApplyTopControlsOffset(content::WebContents* contents, float ratio) {
  content::RenderWidgetHostView* view =
      contents ? contents->GetRenderWidgetHostView() : nullptr;
  aura::Window* window = view ? view->GetNativeView() : nullptr;
  if (!window) {
    return;
  }
  const int height = GetAuraShellTopControlsHeight();
  const float scale = window->layer()->device_scale_factor();
  const float offset = height > 0 && scale > 0.0f
                           ? height / scale * std::clamp(ratio, 0.0f, 1.0f)
                           : 0.0f;
  gfx::Transform transform;
  transform.Translate(0, offset);
  if (window->transform() != transform) {
    window->SetTransform(transform);
    VLOG(1) << "OHOS browser controls: page offset " << offset;
  }
}

}  // namespace

void OnAuraShellTopControlsShownRatio(content::WebContents* contents,
                                      float ratio) {
  // Before the early return below: a new view after a navigation reports the
  // same ratio as the old one, and still has to be moved.
  ApplyTopControlsOffset(contents, ratio);
  {
    RuntimeBridgeState& state = GetState();
    base::AutoLock lock(state.lock);
    if (state.top_controls_height <= 0 || ratio == state.last_shown_ratio) {
      return;
    }
    if (state.last_shown_ratio < 0.0f) {
      LOG(WARNING) << "OHOS browser controls: first shown ratio " << ratio;
    }
    state.last_shown_ratio = ratio;
  }
  BrowserWindowInterface* browser = FindBrowserForWebContents(contents);
  if (!browser || browser->GetTabStripModel()->GetActiveWebContents() !=
                      contents) {
    return;
  }
  base::DictValue event;
  event.Set("event", "browserControlsRatio");
  event.Set("ratio", static_cast<double>(ratio));
  DispatchRuntimeEvent(GetBrowserWidget(browser), std::move(event));
}

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
  // Only Android installs one upstream; without it screen.orientation.lock()
  // rejects with NotSupportedError.
  static base::NoDestructor<ScreenOrientationDelegateOhos>
      orientation_delegate;
  content::WebContents::SetScreenOrientationDelegate(
      orientation_delegate.get());

  // PlatformHandle is built below this target, so it cannot call into the
  // bridge; the bridge hands it these instead. Without them its OpenSystemSettings
  // and Request would be the default NOTREACHED(), which this build traps on.
  ::system_permission_settings::SetOhosPermissionRequester(
      base::BindRepeating([](ContentSettingsType type, base::OnceClosure done) {
        RequestOhosPermissionsFor(type, std::move(done));
      }));
  RegisterOhosManagedPermissions();
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
  std::vector<std::pair<gfx::AcceleratedWidget, base::DictValue>>
      pending_commands;
  uint64_t browser_generation = 0;
  {
    base::AutoLock lock(state.lock);
    state.ui_task_runner = base::SingleThreadTaskRunner::GetCurrentDefault();
    browser_generation = ++state.browser_generation;
    state.last_browser_state_json.clear();
    ui_task_runner = state.ui_task_runner;
    pending_commands = std::move(state.pending_commands);
    state.pending_commands.clear();
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
  for (auto& [widget, command] : pending_commands) {
    ui_task_runner->PostTask(
        FROM_HERE, base::BindOnce(&ExecuteBrowserCommandOnUiThread, widget,
                                  std::move(command)));
  }
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
  content::WebContents::SetScreenOrientationDelegate(nullptr);
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
      "newIncognitoWindow",
      "activateTab",
      "activateTabById",
      "getPageContinuation",
      "groupTabs",
      "insertText",
      "handleBack",
      "closeTab",
      "closeTabById",
      "moveTab",
      "moveTabToNewWindow",
      "setRequestDesktopSite",
      "setTabMuted",
      "setZoom",
      "getExtensionActions",
      "runExtensionAction",
      "setExtensionPinned",
      "getTabThumbnails",
      "toggleReaderMode",
      "getRecentlyClosed",
      "restoreRecentlyClosed",
      "print",
      "share",
      "pwaMenu",
      "pwaMenuAction",
      "pwaMenuDismiss",
      "pwaClose",
      "recoverInput",
      "defaultBrowserState",
      "systemCapabilities",
      "sitePermissionDecision",
      "setAnchorRects",
      "setBrowserChrome",
      "passwordAuthReset",
      "requestState",
      "filePickerResult",
      "permissionResult",
      "systemPermissionState",
      "setViewportInsets",
      "setBrowserControls",
      "setBrowserControlsState",
      "findInPage",
      "stopFind",
      "getPageText",
      "contextMenuAction",
      "contextMenuDismissed",
  };
  if (!name || (!IsShellServiceCommand(*name) &&
                std::ranges::find(kSupportedCommands, *name) ==
                    std::ranges::end(kSupportedCommands))) {
    LOG(ERROR) << "OHOS Aura shell rejected browser command JSON";
    return false;
  }

  scoped_refptr<base::SingleThreadTaskRunner> ui_task_runner;
  {
    RuntimeBridgeState& state = GetState();
    base::AutoLock lock(state.lock);
    ui_task_runner = state.ui_task_runner;
    if (!ui_task_runner) {
      if (state.pending_commands.size() >= kMaxPendingCommands) {
        LOG(ERROR) << "OHOS Aura shell dropped a command sent before startup";
        return false;
      }
      state.pending_commands.emplace_back(widget, std::move(command));
      return true;
    }
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

std::optional<bool> IsAuraShellWindowModal(gfx::AcceleratedWidget widget) {
  // Reports can arrive on the ArkTS thread too -- the surface-binding path in
  // the window registry fires them -- and Views may only be read on the UI
  // thread. The shell keeps its last answer when this one is missing.
  scoped_refptr<base::SingleThreadTaskRunner> ui_task_runner;
  {
    RuntimeBridgeState& state = GetState();
    base::AutoLock lock(state.lock);
    ui_task_runner = state.ui_task_runner;
  }
  if (!ui_task_runner || !ui_task_runner->BelongsToCurrentThread()) {
    return std::nullopt;
  }

  aura::WindowTreeHost* host =
      aura::WindowTreeHost::GetForAcceleratedWidget(widget);
  if (!host || !host->window()) {
    return std::nullopt;
  }
  for (views::Widget* candidate :
       views::Widget::GetAllChildWidgets(host->window())) {
    if (candidate != candidate->GetTopLevelWidget()) {
      continue;
    }
    const views::WidgetDelegate* delegate = candidate->widget_delegate();
    if (!delegate) {
      return std::nullopt;
    }
    return delegate->GetModalType() != ui::mojom::ModalType::kNone;
  }
  return std::nullopt;
}

std::optional<bool> IsAuraShellPictureInPictureWindow(
    gfx::AcceleratedWidget widget) {
  // Same thread rule as IsAuraShellWindowModal(): Views is the UI thread's.
  scoped_refptr<base::SingleThreadTaskRunner> ui_task_runner;
  {
    RuntimeBridgeState& state = GetState();
    base::AutoLock lock(state.lock);
    ui_task_runner = state.ui_task_runner;
  }
  if (!ui_task_runner || !ui_task_runner->BelongsToCurrentThread()) {
    return std::nullopt;
  }
  aura::WindowTreeHost* host =
      aura::WindowTreeHost::GetForAcceleratedWidget(widget);
  if (!host || !host->window()) {
    return std::nullopt;
  }
  for (views::Widget* candidate :
       views::Widget::GetAllChildWidgets(host->window())) {
    if (candidate != candidate->GetTopLevelWidget()) {
      continue;
    }
    // The name VideoOverlayWindowViews gives its widget. Asking the window
    // what it is beats asking PictureInPictureWindowManager which window it
    // owns: the manager answers for the tab, and this runs for a widget.
    const bool is_overlay = candidate->GetName() == "PictureInPictureWindow";
    if (is_overlay) {
      // Said once per report for the one window that is one, which is rare.
      // A shell that leaves picture-in-picture in the middle of the screen,
      // or closes it with back, can be told apart from an engine that never
      // marked it.
      LOG(WARNING) << "OHOS auxiliary window " << widget
                   << " is picture-in-picture";
    }
    return is_overlay;
  }
  return std::nullopt;
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
  metadata.is_browser =
      browser->GetType() == BrowserWindowInterface::Type::TYPE_NORMAL;
  metadata.is_incognito =
      browser->GetProfile() && browser->GetProfile()->IsOffTheRecord();
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

// "shell", "native", or empty for the old rule. Read far more often than it
// is written, and written once before the browser starts.
std::string& BrowserChromeMode() {
  static base::NoDestructor<std::string> mode;
  return *mode;
}

// Keyboard shortcuts whose UI the shell owns. Chromium still resolves the key
// to a command -- its accelerator table stays the single source of which keys
// do what -- and this only decides who carries the command out.
//
// Only the command id travels. What the keys are called for a menu is the
// shell's business and lives in the HAR, so neither side writes the other's
// list down.
struct ShellAcceleratorAction {
  int command_id;
  std::string_view action;
};

constexpr ShellAcceleratorAction kShellAccelerators[] = {
    {IDC_FOCUS_LOCATION, "focusLocation"},
    {IDC_FIND, "find"},
    {IDC_BOOKMARK_THIS_TAB, "bookmarkPage"},
    {IDC_SHOW_BOOKMARK_MANAGER, "showBookmarks"},
    {IDC_SHOW_BOOKMARK_BAR, "toggleBookmarkBar"},
    {IDC_SHOW_HISTORY, "showHistory"},
    {IDC_SHOW_DOWNLOADS, "showDownloads"},
    {IDC_SHOW_APP_MENU, "showMenu"},
};

bool DispatchAuraShellAccelerator(BrowserWindowInterface* browser,
                                  int command_id) {
  if (!browser || !IsAuraShellChromeHiddenByShell()) {
    return false;
  }
  std::string_view action;
  for (const ShellAcceleratorAction& entry : kShellAccelerators) {
    if (entry.command_id == command_id) {
      action = entry.action;
      break;
    }
  }
  // Escape is not in the table because it has a second meaning Chromium must
  // keep: it stops a load. The shell hears it so it can close whatever it has
  // open, and Chromium stops the page as well, which is what a browser does.
  const bool escape = command_id == IDC_STOP;
  if (action.empty() && !escape) {
    return false;
  }

  base::DictValue event;
  event.Set("event", "shellAccelerator");
  event.Set("action", escape ? std::string_view("escape") : action);
  DispatchRuntimeEvent(GetBrowserWidget(browser), std::move(event));
  return !escape;
}

gfx::Rect GetAuraShellAnchorRect(gfx::AcceleratedWidget widget,
                                 std::string_view anchor_id) {
  auto window = AnchorStore().find(widget);
  if (window == AnchorStore().end()) {
    return gfx::Rect();
  }
  auto anchor = window->second.find(std::string(anchor_id));
  return anchor == window->second.end() ? gfx::Rect() : anchor->second;
}

bool DispatchAuraShellLinkHovered(content::WebContents* contents,
                                  const GURL& url) {
  if (!contents || !IsAuraShellChromeHiddenByShell()) {
    return false;
  }
  base::DictValue event;
  event.Set("event", "linkHovered");
  // Empty when the pointer left the link, which is the shell's cue to hide
  // the label.
  event.Set("url", url.is_valid() ? ShellVisibleUrl(url) : std::string());
  return DispatchAuraShellRuntimeEvent(contents, std::move(event));
}

void UpdateAuraShellBrowserChrome(const std::string& browser_chrome) {
  BrowserChromeMode() = browser_chrome;
}

// The settings switch, which is a different thing from the screen changing
// shape. ui_family moving must never put Chromium's frame back on screen --
// unfolding a phone is not a request to change browsers -- but the user asking
// for the classic UI is exactly that request, and it should not need a
// restart.
void SetAuraShellBrowserChrome(const std::string& mode) {
  if (mode != "shell" && mode != "native") {
    return;
  }
  if (BrowserChromeMode() == mode) {
    return;
  }
  BrowserChromeMode() = mode;
  // Reuses the path that already hides and restores the frame when the screen
  // changes: BrowserView and ToolbarView remember what they hid, so coming
  // back is not lossy and the tabs are untouched.
  if (GlobalBrowserCollection* browsers =
          GlobalBrowserCollection::GetInstance()) {
    browsers->ForEach([](BrowserWindowInterface* browser) {
      if (BrowserView* browser_view =
              BrowserView::GetBrowserViewForBrowser(browser)) {
        browser_view->OnOhosUiFamilyChanged();
      }
      return true;
    });
  }
}

// The surfaces a phone's shell has always drawn. Hiding the frame on a tablet
// does not imply these, because that shell may not have them yet.
constexpr std::string_view kPhoneDefaultSurfaces[] = {
    "contextMenu", "downloadUi", "permissionPrompt"};

bool ShellDrawsSurface(std::string_view name) {
  static const base::NoDestructor<std::set<std::string>> listed([] {
    std::set<std::string> names;
    const std::string value =
        base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
            "ohos-shell-surfaces");
    for (std::string_view piece : base::SplitStringPiece(
             value, ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
      names.emplace(piece);
    }
    return names;
  }());
  if (listed->contains(std::string(name))) {
    return true;
  }
  // A phone drew these before there was a list, and still does when the list
  // says nothing about them.
  return std::ranges::find(kPhoneDefaultSurfaces, name) !=
             std::ranges::end(kPhoneDefaultSurfaces) &&
         IsAuraShellMobilePhoneUi();
}

bool IsAuraShellChromeHiddenByShell() {
  const std::string& mode = BrowserChromeMode();
  if (mode == "shell") {
    return true;
  }
  if (mode == "native") {
    return false;
  }
  // Unset: what this meant before there was a setting.
  return IsAuraShellMobilePhoneUi();
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

void DispatchAuraShellRuntimeEventToWidget(gfx::AcceleratedWidget widget,
                                           base::DictValue event) {
  DispatchRuntimeEvent(widget, std::move(event));
}

void DispatchAuraShellRuntimeEventToProfile(Profile* profile,
                                            const base::DictValue& event) {
  GlobalBrowserCollection* browsers = GlobalBrowserCollection::GetInstance();
  if (!browsers || !profile) {
    return;
  }
  browsers->ForEach([profile, &event](BrowserWindowInterface* browser) {
    if (browser->GetProfile() == profile) {
      DispatchRuntimeEvent(GetBrowserWidget(browser), event.Clone());
    }
    return true;
  });
}

bool DispatchAuraShellRuntimeEvent(content::WebContents* contents,
                                   base::DictValue event) {
  BrowserWindowInterface* browser =
      contents ? FindBrowserForWebContents(contents) : nullptr;
  if (!browser) {
    return false;
  }
  DispatchRuntimeEvent(GetBrowserWidget(browser), std::move(event));
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
