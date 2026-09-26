// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/ohos/shell_context_menu_ohos.h"

#include <string_view>
#include <utility>

#include "base/command_line.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
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
#include "chrome/browser/ui/ohos/shell_context_menu_image_ohos.h"
#include "ui/base/mojom/menu_source_type.mojom-shared.h"
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
    {"openLinkInIncognito", IDC_CONTENT_CONTEXT_OPENLINKOFFTHERECORD},
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
    {"pasteAsPlainText", IDC_CONTENT_CONTEXT_PASTE_AND_MATCH_STYLE},
    {"undo", IDC_CONTENT_CONTEXT_UNDO},
    {"redo", IDC_CONTENT_CONTEXT_REDO},
    {"delete", IDC_CONTENT_CONTEXT_DELETE},
    {"copyLinkToHighlight", IDC_CONTENT_CONTEXT_COPYLINKTOTEXT},
    // Passwords. suggestPassword is the offer on a new-password field;
    // selectSavedPassword is the manual fallback that opens the list of saved
    // ones, which fills through the ordinary autofill popup and so asks for
    // the reader's identity first if the setting says to.
    {"suggestPassword", IDC_CONTENT_CONTEXT_GENERATEPASSWORD},
    {"selectSavedPassword",
     IDC_CONTENT_CONTEXT_AUTOFILL_FALLBACK_PASSWORDS_SELECT_PASSWORD},
    {"managePasswords", IDC_CONTENT_CONTEXT_SHOWALLSAVEDPASSWORDS},
    // Spelling. The numbered suggestions are not here -- their commands are
    // consecutive and chosen by index, which a fixed table cannot express --
    // but adding a word to the dictionary is an ordinary one.
    {"addToDictionary", IDC_SPELLCHECK_ADD_TO_DICTIONARY},
};

// One of the numbered spelling suggestions: "spellSuggestion:0" runs
// IDC_SPELLCHECK_SUGGESTION_0, and so on. Separate from the table above
// because the command is worked out from the number rather than looked up.
constexpr char kSpellSuggestionPrefix[] = "spellSuggestion:";

// Chromium offers at most five and the shell shows at most five; keeping the
// same number here means an index that arrives can always be checked.
constexpr size_t kMaxSpellSuggestions = 5;

// The menu model is only handed out const, but MenuClosed and OnMenuWillShow
// match the notification against it by address.
ui::SimpleMenuModel* ModelOf(RenderViewContextMenuBase& menu) {
  return const_cast<ui::SimpleMenuModel*>(&menu.menu_model());
}

// Chromium put the item in this menu and it can run now.
// Handing the link to the system share sheet, which the shell owns from end
// to end. Not in kShellActions because there is no Chromium command behind
// it: Chromium's own sharing entry is a desktop submenu that this build never
// puts in the menu, so keying off it would mean the action never appeared.
constexpr char kShareLinkAction[] = "shareLink";

// Put the long-pressed image in a file and say where. Not in kShellActions
// either: there is no Chromium command for it, the file is this platform's
// answer to a share sheet and an image-recognition service that both take a
// path and neither takes a URL. Offered whenever the renderer has the image
// decoded, and the shell does not show it as a menu entry -- it is what the
// shell's own "share image" and "look up image" run first.
constexpr char kImageToFileAction[] = "imageToFile";

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
// How the menu was asked for. The shell puts a touch menu above the finger
// and a pointer menu at the pointer, and a keyboard menu wants neither.
std::string_view MenuSourceName(ui::mojom::MenuSourceType source) {
  switch (source) {
    case ui::mojom::MenuSourceType::kMouse:
      return "mouse";
    case ui::mojom::MenuSourceType::kKeyboard:
      return "keyboard";
    case ui::mojom::MenuSourceType::kTouch:
    case ui::mojom::MenuSourceType::kTouchEditMenu:
    case ui::mojom::MenuSourceType::kLongPress:
    case ui::mojom::MenuSourceType::kLongTap:
    case ui::mojom::MenuSourceType::kTouchHandle:
    case ui::mojom::MenuSourceType::kStylus:
    case ui::mojom::MenuSourceType::kAdjustSelection:
    case ui::mojom::MenuSourceType::kAdjustSelectionReset:
      return "touch";
    case ui::mojom::MenuSourceType::kNone:
      break;
  }
  // Unrecorded. Guess from the device rather than report something the shell
  // would place wrongly: a phone has no pointer to put a menu at.
  return IsAuraShellMobilePhoneUi() ? "touch" : "mouse";
}

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
                          const GURL& image_url,
                          std::unique_ptr<RenderViewContextMenuBase> menu)
      : content::WebContentsObserver(web_contents),
        request_id_(request_id),
        image_url_(image_url),
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
  content::WebContents* web_contents_for_action() { return web_contents(); }
  const GURL& image_url() const { return image_url_; }

  // content::WebContentsObserver:
  void WebContentsDestroyed() override {
    menu_.reset();
    Observe(nullptr);
  }

 private:
  const int request_id_;
  // Remembered rather than read back off the menu: the file is written after
  // the menu has been dismissed and its params are gone.
  const GURL image_url_;
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
  if (params.link_url.is_valid()) {
    supported.Append(std::string(kShareLinkAction));
  }
  // Only when the renderer actually has the picture. Without this the shell
  // would offer "share image" on an image that failed to load and get an
  // empty path back.
  if (params.has_image_contents && params.src_url.is_valid()) {
    supported.Append(std::string(kImageToFileAction));
  }

  base::DictValue event;
  event.Set("event", "contextMenuRequested");
  event.Set("requestId", request_id);
  // Root-window coordinates, which Aura keeps in DIP and HarmonyOS calls vp.
  event.Set("x", point.x());
  event.Set("y", point.y());
  event.Set("source", MenuSourceName(params.source_type));
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
  // Spelling. Only when the renderer actually found something: Chromium's
  // spellchecker here is Hunspell, which needs a dictionary file, and there
  // is no Hunspell dictionary for Chinese in any Chromium build. So on a
  // zh-CN phone these are always absent and the shell shows nothing, which
  // is the honest outcome rather than an empty menu section.
  if (!params.misspelled_word.empty() &&
      !params.dictionary_suggestions.empty()) {
    base::ListValue suggestions;
    for (const std::u16string& word : params.dictionary_suggestions) {
      if (suggestions.size() >= kMaxSpellSuggestions) {
        break;
      }
      suggestions.Append(base::UTF16ToUTF8(word));
    }
    event.Set("misspelledWord", base::UTF16ToUTF8(params.misspelled_word));
    event.Set("spellSuggestions", std::move(suggestions));
  }

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
  return ShellDrawsSurface("contextMenu");
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
      request_id, web_contents, menu->params().src_url, std::move(menu));
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
  if (action == kShareLinkAction) {
    // Nothing for Chromium to run; the shell has already opened the sheet.
    // Falling through to the loop would log this as unknown.
    return;
  }
  if (action == kImageToFileAction) {
    // Answers separately, with contextMenuImageFile: writing the file means
    // asking the renderer for the bitmap and then touching the disk, neither
    // of which the shell should be kept waiting on with a menu still up.
    WriteContextMenuImageToFile(session->web_contents_for_action(),
                                session->image_url(), request_id);
    return;
  }
  if (action.starts_with(kSpellSuggestionPrefix)) {
    size_t index = 0;
    if (!base::StringToSizeT(
            std::string_view(action).substr(strlen(kSpellSuggestionPrefix)),
            &index) ||
        index >= kMaxSpellSuggestions) {
      LOG(WARNING) << "OHOS context menu: no such spelling suggestion "
                   << action;
      return;
    }
    const int command = IDC_SPELLCHECK_SUGGESTION_0 + static_cast<int>(index);
    if (IsOffered(*session->menu(), command)) {
      session->menu()->ExecuteCommand(command, /*event_flags=*/0);
    }
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
