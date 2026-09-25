// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_OHOS_AURA_SHELL_RUNTIME_BRIDGE_H_
#define CHROME_BROWSER_UI_OHOS_AURA_SHELL_RUNTIME_BRIDGE_H_

#include <optional>
#include <string>

#include "base/functional/callback_forward.h"
#include "base/values.h"
#include "ui/gfx/native_ui_types.h"

class Profile;

namespace content {
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
void UpdateAuraShellBrowserChrome(const std::string& browser_chrome);
bool IsAuraShellDesktopUi();
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
bool RequestAuraShellSystemCast(content::WebContents* contents);
bool RequestAuraShellSystemAction(const std::string& action);
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
void OnAuraShellTopControlsShownRatio(content::WebContents* contents,
                                      float ratio);

}  // namespace chrome::ohos

#endif  // CHROME_BROWSER_UI_OHOS_AURA_SHELL_RUNTIME_BRIDGE_H_
