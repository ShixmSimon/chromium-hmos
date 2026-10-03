// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_OHOS_AURA_SHELL_RUNTIME_BRIDGE_H_
#define CHROME_BROWSER_UI_OHOS_AURA_SHELL_RUNTIME_BRIDGE_H_

#include <optional>
#include <string>

#include "base/functional/callback_forward.h"
#include "base/values.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "third_party/blink/public/mojom/choosers/date_time_chooser.mojom-forward.h"
#include "third_party/blink/public/mojom/webshare/webshare.mojom-forward.h"
#include "third_party/blink/public/common/user_agent/user_agent_metadata.h"
#include "ui/gfx/geometry/insets.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/native_ui_types.h"

class BrowserWindowInterface;
class GURL;
class Profile;

namespace content {
class RenderFrameHost;
class WebContents;
}

namespace chrome::ohos {

struct AuraShellWindowMetadata {
  bool is_pwa = false;
  // A second browser window -- an incognito one, today. The shell hosts it the
  // way it hosts the first: full screen, not as a popup.
  bool is_browser = false;
  bool is_incognito = false;
  std::string app_id;
  std::string title;
  std::string url;
  std::string start_url;
};

using AuraShellBrowserStateCallback =
    base::RepeatingCallback<void(gfx::AcceleratedWidget widget,
                                 const std::string& state_json)>;
using AuraShellDefaultBrowserStateCallback =
    base::RepeatingCallback<void(std::optional<bool> is_default)>;

// Registers the platform pieces Chromium consults while building a profile.
// Called from PreProfileInit, which is before the first PermissionManager and
// so before anything can read them.
void EnsureAuraShellSystemPermissions();

// Called on the Chromium UI thread as the browser main loop starts and stops.
void NotifyAuraShellBrowserStarted();
void NotifyAuraShellBrowserStopped();

// These entry points are safe to call from the ArkUI/N-API thread. Requests
// made during startup are held until Chromium's UI thread is ready.
bool NavigateAuraShellBrowser(const std::string& url);
bool NavigateAuraShellBrowser(gfx::AcceleratedWidget widget,
                              const std::string& url);
bool ExecuteAuraShellBrowserCommand(const std::string& command_json);
bool ExecuteAuraShellBrowserCommand(gfx::AcceleratedWidget widget,
                                    const std::string& command_json);
void SetAuraShellBrowserStateCallback(AuraShellBrowserStateCallback callback);
std::optional<bool> GetAuraShellDefaultBrowserState();
void SetAuraShellDefaultBrowserStateCallback(
    AuraShellDefaultBrowserStateCallback callback);
bool IsAuraShellHuaweiWalletAvailable();
// Whether the top-level Views widget hosted by `widget` is modal: the media
// source picker is, the "sharing your screen" bar is not. The shell uses it
// to decide which auxiliary windows may block the page. nullopt when it
// cannot be told -- off the UI thread, or before the widget has a host.
// Whether this window is Chromium's picture-in-picture overlay. The shell
// needs to tell it from a page's own popup: both float over the page without
// a close button of their own, but back should close the popup and leave the
// video alone, and the video wants a corner rather than the middle.
std::optional<bool> IsAuraShellPictureInPictureWindow(
    gfx::AcceleratedWidget widget);

std::optional<bool> IsAuraShellWindowModal(gfx::AcceleratedWidget widget);
std::optional<AuraShellWindowMetadata> GetAuraShellWindowMetadata(
    gfx::AcceleratedWidget widget);
void UpdateAuraShellUiFamily(const std::string& ui_family);
void UpdateAuraShellColorScheme(const std::string& color_scheme);
void UpdateAuraShellPrintOutputDirectory(const std::string& output_directory);
bool IsAuraShellMobilePhoneUi();
// Whether the shell draws the browser and Chromium draws only the page. Set
// once at startup and unchanged afterwards, so unfolding a foldable or
// docking a 2-in-1 does not put Chromium's tab strip back on screen.
//
// Distinct from IsAuraShellMobilePhoneUi(), which still decides the things
// that do follow the screen: user agent, scrollbars, pointer and touch.
bool IsAuraShellChromeHiddenByShell();
// Whether the shell draws one particular surface rather than Chromium.
//
// Hiding the browser frame is not the same as taking over everything inside
// it: a shell adopts dialogs and bubbles one at a time, and until it has,
// Chromium's own must still appear or the feature simply vanishes. `name` is
// one of the values the shellSurfaces startup option accepts.
bool ShellDrawsSurface(std::string_view name);

// A keyboard shortcut whose UI belongs to the shell: the location bar, find
// bar, bookmark and history pages, the app menu. Sends shellAccelerator and
// returns true when the shell took it, which means Chromium must not also run
// its own command for that key.
bool DispatchAuraShellAccelerator(BrowserWindowInterface* browser,
                                  int command_id);

// The link the pointer is over, for the label a shell draws in the corner of
// the page. Returns true when the shell took it, which means Chromium should
// not also look for a status bubble it is not drawing.
bool DispatchAuraShellLinkHovered(content::WebContents* contents,
                                  const GURL& url);

// Where the shell drew the button a bubble should point at, in the window's
// own coordinates (vp, which is what Aura calls DIP). Empty when the shell has
// not said, and a bubble with no anchor goes to the top right of the page
// area rather than to a toolbar that is not there.
gfx::Rect GetAuraShellAnchorRect(gfx::AcceleratedWidget widget,
                                 std::string_view anchor_id);
void UpdateAuraShellBrowserChrome(const std::string& browser_chrome);
bool IsAuraShellDesktopUi();

// Whether Chromium's own UI is on screen because someone asked for it.
//
// Distinct from "the shell is not drawing": a phone gets the native UI by
// default and has no setting to come back from, so the way back belongs
// only where the choice was made.
bool IsAuraShellNativeChromeChosen();

// What the app menu's way back to the shell's UI is called, empty when the
// shell did not ask for the item. The shell owns the word: it is the
// shell's name, the shell has the translations, and a string compiled into
// the engine came out in English on a Chinese device.
void SetAuraShellUiMenuLabel(const std::string& label);
// The product's name as the shell gives it (productName in the startup
// config), for the few places Chromium names itself to the reader. Empty
// when the shell gave none.
void SetAuraShellProductName(const std::string& name);
// `text` with "Chromium" replaced by that name, or unchanged when there is
// none. Chromium's strings carry the brand untranslated in every language,
// so this works on any of them.
std::u16string WithAuraShellProductName(std::u16string text);
std::u16string AuraShellUiMenuLabel();

// Asks the shell to change which browser UI is drawn, without changing it
// here. The shell keeps this setting and writes it back at the next launch,
// so it has to be the one that changes it, or the two disagree after a
// restart.
void RequestAuraShellBrowserChrome(const std::string& mode);
bool RequestAuraShellSystemPrint(content::WebContents* contents);
// Sends `event` to the shell window hosting `widget`.
void DispatchAuraShellRuntimeEventToWidget(gfx::AcceleratedWidget widget,
                                           base::DictValue event);
// Sends a copy of `event` to every shell window showing `profile`.
void DispatchAuraShellRuntimeEventToProfile(Profile* profile,
                                            const base::DictValue& event);
// Sends `event` to the shell hosting `contents`. False when no browser window
// holds it.
bool DispatchAuraShellRuntimeEvent(content::WebContents* contents,
                                   base::DictValue event);
bool RequestAuraShellSystemShare(content::WebContents* contents);
// navigator.share(): hands the page's title, text and URL to the shell's
// share sheet as a shareRequested event.
void BindAuraShellShareService(
    content::RenderFrameHost* frame,
    mojo::PendingReceiver<blink::mojom::ShareService> receiver);
// <input type=date|time|datetime-local|month|week> on a phone: asks the shell
// for its own picker with dateTimePickerRequested.
void BindAuraShellDateTimeChooser(
    content::RenderFrameHost* frame,
    mojo::PendingReceiver<blink::mojom::DateTimeChooser> receiver);
bool RequestAuraShellSystemCast(content::WebContents* contents);
bool RequestAuraShellSystemAction(const std::string& action);

// A link for another app -- bilibili://, weixin://, mailto:, tel:, or an
// Android intent:// one -- that Chromium has let through its own checks. The
// shell asks the reader and opens it through the system, which starts
// whichever app claims it. `initiator` is the page's origin, for the prompt.
void RequestAuraShellExternalUrl(content::WebContents* contents,
                                 const GURL& url,
                                 const std::string& initiator);
// The popup blocker stopped a window.open() on `contents`' page. The shell
// shows its notice; "show" answers with the showBlockedPopups command and
// "always allow" with setSiteSetting { type: "popups", setting: "allow" }.
void NotifyAuraShellPopupBlocked(content::WebContents* contents,
                                 const GURL& popup_url,
                                 int blocked_on_page);
void NotifyAuraShellWebAppInstalled(const std::string& app_id,
                                    const std::string& title,
                                    const std::string& start_url);
void SetAuraShellBrowserVisible(bool visible);
void SetAuraShellBrowserVisible(gfx::AcceleratedWidget widget, bool visible);
void SetAuraShellBrowserFocused(bool focused);
void SetAuraShellBrowserFocused(gfx::AcceleratedWidget widget, bool focused);
void NotifyAuraShellThemeFontChanged(const std::string& font_id);
void ShutdownAuraShellBrowser();

// Browser controls: the shell's own top bar, which Chromium makes room for at
// the top of the page and slides away as the page scrolls, the way Chrome on
// Android does. The shell tracks the height and minimum height in DIP; these
// getters return device pixels, which is what BrowserWebContentsDelegate
// reports to the renderer. 0 means the shell has none. The minimum height is
// how much of the bar stays on screen when it is fully collapsed -- 0 means
// it can hide completely. BrowserWebContentsDelegate reports both to the
// renderer and forwards the renderer's shown ratio back here, which passes it
// on to the shell so the bar can follow the page.
int GetAuraShellTopControlsHeight();
int GetAuraShellTopControlsMinHeight();
// A phone's dock, which hides with the top bar, is not reported as bottom
// controls but counted into the top ones; see GetAuraShellTopControlsHeight()
// in the .cc.
// Whether this tab's controls have slid away completely, down to the
// minimum height. The renderer then lays the page out in their room too, as
// Android's does, rather than only showing more of it.
bool AreAuraShellBrowserControlsHidden(content::WebContents* contents);

// What of `browser`'s window the shell covers while its bars show, in DIP:
// the top bar at the top; the dock, and whatever stays covered however the
// page scrolls (setViewportInsets), at the bottom. A tab-modal dialog is
// kept between them (ohos-tab-modal-shell-insets.patch).
gfx::Insets GetAuraShellModalDialogInsets(BrowserWindowInterface* browser);
// The same for the browser window `window` belongs to, for code that has
// the window but not the browser: browser-modal dialogs, which BrowserView
// places through its layout rather than a tab's dialog host.
gfx::Insets GetAuraShellModalDialogInsetsForWindow(gfx::NativeWindow window);

// A tab-modal dialog started or stopped blocking `contents`. While one is up
// the bars are held shown, so they cover exactly what the dialog avoids.
void OnAuraShellWebContentsBlocked(content::WebContents* contents,
                                   bool blocked);
// Whether this tab's user agent was set deliberately and must be left alone.
//
// The shell's state poll re-applies the device's user agent to every tab so
// a fold or a window change is followed, and it did that by comparing the
// override against the one it would install -- which cannot tell a
// deliberate override from a stale one, because "desktop site" installs
// exactly the string a tablet uses by default. Every poll therefore undid
// both "desktop site" and the web store's user agent within about a tenth
// of a second of them being set. Pinning records the intent the string
// cannot carry.
void SetAuraShellUserAgentPinned(content::WebContents* contents, bool pinned);
bool IsAuraShellUserAgentPinned(content::WebContents* contents);

// The user agent "desktop site" sends. On a phone, the OpenHarmony string
// without its Mobile token, which Chinese sites take for a desktop. A tablet
// or a 2in1 already sends that string by default, so there "desktop site"
// is Chrome's desktop Linux string, as Chrome for Android's desktop site is,
// with client hints to match.
blink::UserAgentOverride AuraShellDesktopSiteUserAgent();

void OnAuraShellTopControlsShownRatio(content::WebContents* contents,
                                      float ratio);

}  // namespace chrome::ohos

#endif  // CHROME_BROWSER_UI_OHOS_AURA_SHELL_RUNTIME_BRIDGE_H_
