// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Permission prompts drawn by the ArkUI shell instead of by Chromium.
//
// Chromium's own bubble hangs off the location bar, which a shell that draws
// its own browser UI does not show. The shell draws a bottom sheet instead, so
// the engine hands it the request and waits for an answer.
//
// This is not the same thing as the "permissionsRequested" event, which asks
// the shell for a HarmonyOS application permission after the user has already
// said yes to a site. This is the question put to the user.

#ifndef CHROME_BROWSER_UI_OHOS_SHELL_PERMISSION_PROMPT_OHOS_H_
#define CHROME_BROWSER_UI_OHOS_SHELL_PERMISSION_PROMPT_OHOS_H_

#include <memory>
#include <string_view>

#include "components/permissions/permission_prompt.h"

namespace content {
class WebContents;
}

namespace chrome::ohos {

// Whether the shell, rather than Chromium, puts permission questions to the
// user. True when the shell draws the browser UI.
bool ShellDrawsPermissionPrompts();

// A prompt that asks the shell. Null when there is no window to ask through,
// which leaves the caller to fall back to Chromium's own UI.
std::unique_ptr<permissions::PermissionPrompt> CreateShellPermissionPrompt(
    content::WebContents* web_contents,
    permissions::PermissionPrompt::Delegate* delegate);

// The shell's answer to one request: "allowOnce", "allowAlways", "block" or
// "dismiss". Anything else, or an id that is no longer waiting, is ignored.
void ResolveShellPermissionRequest(int request_id, std::string_view decision);

}  // namespace chrome::ohos

#endif  // CHROME_BROWSER_UI_OHOS_SHELL_PERMISSION_PROMPT_OHOS_H_
