// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_OHOS_SHELL_CONTEXT_MENU_OHOS_H_
#define CHROME_BROWSER_UI_OHOS_SHELL_CONTEXT_MENU_OHOS_H_

#include <memory>
#include <string>

class RenderViewContextMenuBase;

namespace content {
class WebContents;
}

namespace chrome::ohos {

// Long-press (context) menus drawn by the ArkUI shell instead of Chromium's
// desktop menu. Chromium still builds the menu -- which items apply, which are
// enabled, what each one does -- and the shell only draws it: it receives a
// contextMenuRequested event with the page context and the actions that
// apply, and answers with contextMenuAction or contextMenuDismissed.
//
// Phones hand menus to the shell; other devices keep Chromium's menu.
// --ohos-context-menu=shell|native overrides that either way.
bool ShouldShellDrawContextMenu();

// Called with a menu Chromium has built and is about to show. Takes it and
// asks the shell to draw it when the shell draws menus; otherwise leaves
// `menu` alone and returns false.
bool HandOffContextMenuToShell(content::WebContents* web_contents,
                               std::unique_ptr<RenderViewContextMenuBase>& menu);

// The shell's answers. Stale or unknown request ids are ignored.
void RunShellContextMenuAction(int request_id, const std::string& action);
void DismissShellContextMenu(int request_id);

}  // namespace chrome::ohos

#endif  // CHROME_BROWSER_UI_OHOS_SHELL_CONTEXT_MENU_OHOS_H_
