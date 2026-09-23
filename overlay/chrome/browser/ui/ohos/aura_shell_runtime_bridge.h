// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_OHOS_AURA_SHELL_RUNTIME_BRIDGE_H_
#define CHROME_BROWSER_UI_OHOS_AURA_SHELL_RUNTIME_BRIDGE_H_

#include <optional>
#include <string>

#include "base/functional/callback_forward.h"
#include "ui/gfx/native_ui_types.h"

namespace content {
class WebContents;
}

namespace chrome::ohos {

struct AuraShellWindowMetadata {
  bool is_pwa = false;
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
bool IsAuraShellDesktopUi();
bool RequestAuraShellSystemPrint(content::WebContents* contents);
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

}  // namespace chrome::ohos

#endif  // CHROME_BROWSER_UI_OHOS_AURA_SHELL_RUNTIME_BRIDGE_H_
