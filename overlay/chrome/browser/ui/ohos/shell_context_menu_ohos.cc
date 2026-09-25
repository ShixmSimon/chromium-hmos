// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/ohos/shell_context_menu_ohos.h"

#include <string_view>
#include <utility>

#include "base/command_line.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/ui/ohos/aura_shell_runtime_bridge.h"
#include "components/renderer_context_menu/render_view_context_menu_base.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/context_menu_params.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "third_party/blink/public/common/context_menu_data/context_menu_data.h"
#include "third_party/blink/public/mojom/context_menu/context_menu.mojom-shared.h"
#include "ui/aura/window.h"
#include "ui/gfx/geometry/point.h"

namespace chrome::ohos {

namespace {

constexpr char kContextMenuSwitch[] = "ohos-context-menu";

// What the shell may ask for, and the Chromium command behind each. The list
// is also the order supportedActions is reported in. Inspect, cast, picture in
// picture and image search are left out on purpose: phones do not offer them.
struct ShellAction {
  std::string_view name;
  int command_id;
};

constexpr ShellAction kShellActions[] = {
    // Links.
    {"openLinkInNewTab", IDC_CONTENT_CONTEXT_OPENLINKNEWTAB},
    {"copyLinkAddress", IDC_CONTENT_CONTEXT_COPYLINKLOCATION},
    {"copyLinkText", IDC_CONTENT_CONTEXT_COPYLINKTEXT},
    {"saveLinkAs", IDC_CONTENT_CONTEXT_SAVELINKAS},
    // Images.
    {"openImageInNewTab", IDC_CONTENT_CONTEXT_OPENIMAGENEWTAB},
    {"saveImageAs", IDC_CONTENT_CONTEXT_SAVEIMAGEAS},
    {"copyImage", IDC_CONTENT_CONTEXT_COPYIMAGE},
    {"copyImageAddress", IDC_CONTENT_CONTEXT_COPYIMAGELOCATION},
    // Video and audio.
    {"toggleLoop", IDC_CONTENT_CONTEXT_LOOP},
    {"toggleControls", IDC_CONTENT_CONTEXT_CONTROLS},
    {"openMediaInNewTab", IDC_CONTENT_CONTEXT_OPENAVNEWTAB},
    {"saveMediaAs", IDC_CONTENT_CONTEXT_SAVEAVAS},
    {"copyMediaAddress", IDC_CONTENT_CONTEXT_COPYAVLOCATION},
    {"copyVideoFrame", IDC_CONTENT_CONTEXT_COPYVIDEOFRAME},
    // Text.
    {"copy", IDC_CONTENT_CONTEXT_COPY},
    {"cut", IDC_CONTENT_CONTEXT_CUT},
    {"paste", IDC_CONTENT_CONTEXT_PASTE},
    {"selectAll", IDC_CONTENT_CONTEXT_SELECTALL},
    {"searchSelection", IDC_CONTENT_CONTEXT_SEARCHWEBFOR},
};

// The menu model is only handed out const, but MenuClosed and OnMenuWillShow
// match the notification against it by address.
ui::SimpleMenuModel* ModelOf(RenderViewContextMenuBase& menu) {
  return const_cast<ui::SimpleMenuModel*>(&menu.menu_model());
}

// Chromium put the item in this menu and it can run now.
bool IsOffered(RenderViewContextMenuBase& menu, int command_id) {
  return menu.menu_model().GetIndexOfCommandId(command_id).has_value() &&
         menu.IsCommandIdEnabled(command_id);
}

const char* MediaTypeName(blink::mojom::ContextMenuDataMediaType type) {
  switch (type) {
    case blink::mojom::ContextMenuDataMediaType::kImage:
      return "image";
    case blink::mojom::ContextMenuDataMediaType::kVideo:
      return "video";
    case blink::mojom::ContextMenuDataMediaType::kAudio:
      return "audio";
    default:
      return "none";
  }
}

base::DictValue MediaFlags(int flags) {
  auto has = [flags](int flag) { return (flags & flag) != 0; };
  base::DictValue result;
  result.Set("paused", has(blink::ContextMenuData::kMediaPaused));
  result.Set("muted", has(blink::ContextMenuData::kMediaMuted));
  result.Set("loop", has(blink::ContextMenuData::kMediaLoop));
  result.Set("canLoop", has(blink::ContextMenuData::kMediaCanLoop));
  result.Set("controls", has(blink::ContextMenuData::kMediaControls));
  result.Set("canToggleControls",
             has(blink::ContextMenuData::kMediaCanToggleControls));
  result.Set("canSave", has(blink::ContextMenuData::kMediaCanSave));
  result.Set("hasAudio", has(blink::ContextMenuData::kMediaHasAudio));
  result.Set("inError", has(blink::ContextMenuData::kMediaInError));
  return result;
}

// Where the finger was, in the window's coordinates. On OHOS a DIP is a vp.
gfx::Point PointInWindow(content::WebContents* web_contents,
                         const content::ContextMenuParams& params) {
  gfx::Point point(params.x, params.y);
  content::RenderWidgetHostView* view = web_contents->GetRenderWidgetHostView();
  aura::Window* native = view ? view->GetNativeView() : nullptr;
  aura::Window* root = native ? native->GetRootWindow() : nullptr;
  if (root) {
    aura::Window::ConvertPointToTarget(native, root, &point);
  }
  return point;
}

// One long press the shell is drawing. A new press replaces it, as a new
// native menu would; the page going away drops it.
class ShellContextMenuSession : public content::WebContentsObserver {
 public:
  ShellContextMenuSession(int request_id,
                          content::WebContents* web_contents,
                          std::unique_ptr<RenderViewContextMenuBase> menu)
      : content::WebContentsObserver(web_contents),
        request_id_(request_id),
        menu_(std::move(menu)) {
    menu_->OnMenuWillShow(ModelOf(*menu_));
  }

  ShellContextMenuSession(const ShellContextMenuSession&) = delete;
  ShellContextMenuSession& operator=(const ShellContextMenuSession&) = delete;

  ~ShellContextMenuSession() override {
    // Lets the page know its menu closed (selection handles, the
    // contextmenu-closed bookkeeping) whether or not an item was chosen.
    if (menu_ && web_contents()) {
      menu_->MenuClosed(ModelOf(*menu_));
    }
  }

  int request_id() const { return request_id_; }
  RenderViewContextMenuBase* menu() { return menu_.get(); }

  // content::WebContentsObserver:
  void WebContentsDestroyed() override {
    menu_.reset();
    Observe(nullptr);
  }

 private:
  const int request_id_;
  std::unique_ptr<RenderViewContextMenuBase> menu_;
};

std::unique_ptr<ShellContextMenuSession>& CurrentSession() {
  static base::NoDestructor<std::unique_ptr<ShellContextMenuSession>> session;
  return *session;
}

int NextRequestId() {
  static int next_id = 0;
  return ++next_id;
}

base::DictValue BuildRequestEvent(int request_id,
                                  content::WebContents* web_contents,
                                  RenderViewContextMenuBase& menu) {
  const content::ContextMenuParams& params = menu.params();
  const gfx::Point point = PointInWindow(web_contents, params);

  base::ListValue supported;
  for (const ShellAction& action : kShellActions) {
    if (IsOffered(menu, action.command_id)) {
      supported.Append(std::string(action.name));
    }
  }

  base::DictValue event;
  event.Set("event", "contextMenuRequested");
  event.Set("requestId", request_id);
  event.Set("x", point.x());
  event.Set("y", point.y());
  event.Set("linkUrl", params.link_url.is_valid() ? params.link_url.spec()
                                                  : std::string());
  event.Set("linkText", base::UTF16ToUTF8(params.link_text));
  event.Set("srcUrl",
            params.src_url.is_valid() ? params.src_url.spec() : std::string());
  event.Set("mediaType", MediaTypeName(params.media_type));
  event.Set("mediaFlags", MediaFlags(params.media_flags));
  event.Set("selectionText", base::UTF16ToUTF8(params.selection_text));
  event.Set("isEditable", params.is_editable);
  event.Set("frameUrl", params.frame_url.is_valid() ? params.frame_url.spec()
                                                    : std::string());
  event.Set("pageUrl", params.page_url.is_valid() ? params.page_url.spec()
                                                  : std::string());
  event.Set("incognito", web_contents->GetBrowserContext()->IsOffTheRecord());
  event.Set("supportedActions", std::move(supported));
  return event;
}

}  // namespace

bool ShouldShellDrawContextMenu() {
  const std::string mode =
      base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
          kContextMenuSwitch);
  if (mode == "shell") {
    return true;
  }
  if (mode == "native") {
    return false;
  }
  return IsAuraShellChromeHiddenByShell();
}

bool HandOffContextMenuToShell(
    content::WebContents* web_contents,
    std::unique_ptr<RenderViewContextMenuBase>& menu) {
  if (!web_contents || !menu || !ShouldShellDrawContextMenu()) {
    return false;
  }
  const int request_id = NextRequestId();
  base::DictValue event = BuildRequestEvent(request_id, web_contents, *menu);
  // Replacing the session closes the menu the shell was still showing.
  CurrentSession() = std::make_unique<ShellContextMenuSession>(
      request_id, web_contents, std::move(menu));
  if (!DispatchAuraShellRuntimeEvent(web_contents, std::move(event))) {
    LOG(WARNING) << "OHOS shell context menu has no window to go to";
    CurrentSession().reset();
  }
  return true;
}

void RunShellContextMenuAction(int request_id, const std::string& action) {
  std::unique_ptr<ShellContextMenuSession> session =
      std::move(CurrentSession());
  if (!session || session->request_id() != request_id || !session->menu()) {
    CurrentSession() = std::move(session);
    return;
  }
  for (const ShellAction& entry : kShellActions) {
    if (entry.name != action) {
      continue;
    }
    // Checked again: the page may have changed since the menu was built.
    if (IsOffered(*session->menu(), entry.command_id)) {
      session->menu()->ExecuteCommand(entry.command_id, /*event_flags=*/0);
    }
    return;
  }
  LOG(WARNING) << "OHOS shell asked for unknown context menu action "
               << action;
}

void DismissShellContextMenu(int request_id) {
  std::unique_ptr<ShellContextMenuSession>& session = CurrentSession();
  if (session && session->request_id() == request_id) {
    session.reset();
  }
}

}  // namespace chrome::ohos
