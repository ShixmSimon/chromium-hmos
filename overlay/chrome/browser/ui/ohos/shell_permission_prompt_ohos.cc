// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/ohos/shell_permission_prompt_ohos.h"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/values.h"
#include "chrome/browser/ui/ohos/aura_shell_runtime_bridge.h"
#include "components/permissions/permission_request.h"
#include "components/permissions/permission_uma_constants.h"
#include "components/permissions/request_type.h"
#include "content/public/browser/web_contents.h"
#include "net/base/is_potentially_trustworthy.h"
#include "url/gurl.h"

namespace chrome::ohos {

namespace {

constexpr char kRequestedEvent[] = "sitePermissionRequested";
constexpr char kCancelledEvent[] = "sitePermissionCancelled";

// The permission names the shell knows, which are the web-facing ones rather
// than Chromium's request types: the shell writes the sentence the user reads
// and has its own wording for each.
std::string_view ShellPermissionName(permissions::RequestType type) {
  switch (type) {
    case permissions::RequestType::kGeolocation:
      return "location";
    case permissions::RequestType::kCameraStream:
    case permissions::RequestType::kCameraPanTiltZoom:
      return "camera";
    case permissions::RequestType::kMicStream:
      return "microphone";
    case permissions::RequestType::kNotifications:
      return "notifications";
    case permissions::RequestType::kClipboard:
      return "clipboard";
    default:
      return {};
  }
}

// Every request in the group, as names the shell understands. Empty when the
// group holds something the shell has no wording for, in which case Chromium
// keeps the request and shows its own UI.
std::vector<std::string_view> ShellPermissionNames(
    permissions::PermissionPrompt::Delegate* delegate) {
  std::vector<std::string_view> names;
  for (const auto& request : delegate->Requests()) {
    const std::string_view name = ShellPermissionName(request->request_type());
    if (name.empty()) {
      return {};
    }
    if (std::ranges::find(names, name) == names.end()) {
      names.push_back(name);
    }
  }
  return names;
}

int& NextRequestId() {
  static int next = 0;
  return next;
}

class ShellPermissionPrompt;

// The prompts waiting for an answer, by request id. A prompt removes itself
// when it is answered or destroyed, so an answer that arrives late -- the user
// tapped as the tab closed -- finds nothing and is dropped.
std::map<int, ShellPermissionPrompt*>& PendingPrompts() {
  static base::NoDestructor<std::map<int, ShellPermissionPrompt*>> prompts;
  return *prompts;
}

class ShellPermissionPrompt : public permissions::PermissionPrompt {
 public:
  ShellPermissionPrompt(content::WebContents* web_contents,
                        Delegate* delegate,
                        std::vector<std::string_view> permissions)
      : web_contents_(web_contents),
        delegate_(delegate),
        request_id_(++NextRequestId()) {
    PendingPrompts()[request_id_] = this;

    const GURL origin = delegate_->GetRequestingOrigin();
    base::ListValue names;
    for (std::string_view name : permissions) {
      names.Append(name);
    }
    base::DictValue event;
    event.Set("event", kRequestedEvent);
    event.Set("requestId", request_id_);
    event.Set("origin", origin.DeprecatedGetOriginAsURL().spec());
    // A site reached over plain http cannot be trusted with a lasting grant,
    // so the shell does not offer "always" there. Enforced again when the
    // answer comes back, in case it does.
    event.Set("isSecure", net::IsUrlPotentiallyTrustworthy(origin));
    event.Set("webPermissions", std::move(names));
    DispatchAuraShellRuntimeEvent(web_contents_, std::move(event));
  }

  ShellPermissionPrompt(const ShellPermissionPrompt&) = delete;
  ShellPermissionPrompt& operator=(const ShellPermissionPrompt&) = delete;

  ~ShellPermissionPrompt() override {
    if (PendingPrompts().erase(request_id_) == 0) {
      return;
    }
    // Chromium gave up on the request -- the page navigated away, the tab
    // closed -- while the shell still has a sheet on screen for it.
    base::DictValue event;
    event.Set("event", kCancelledEvent);
    event.Set("requestId", request_id_);
    DispatchAuraShellRuntimeEvent(web_contents_, std::move(event));
  }

  // Answers the delegate and stops waiting. Destroys this object by way of
  // the delegate, so nothing may touch `this` afterwards.
  void Resolve(std::string_view decision) {
    PendingPrompts().erase(request_id_);
    Delegate* delegate = delegate_;
    const bool secure =
        net::IsUrlPotentiallyTrustworthy(delegate->GetRequestingOrigin());

    if (decision == "allowAlways" && secure) {
      delegate->Accept(std::monostate());
    } else if (decision == "allowAlways" || decision == "allowOnce") {
      // An insecure origin gets this once and is asked again next time: a
      // grant written against http://example.com is a grant to whoever is on
      // the wire.
      delegate->AcceptThisTime(std::monostate());
    } else if (decision == "block") {
      delegate->Deny(std::monostate());
    } else {
      // "dismiss", and anything unrecognised: refuse this time, remember
      // nothing, ask again later.
      delegate->Dismiss(std::monostate());
    }
  }

  // permissions::PermissionPrompt:
  bool UpdateAnchor() override { return true; }
  TabSwitchingBehavior GetTabSwitchingBehavior() override {
    // The shell hangs its sheet on the window, not on the tab, and hides it
    // with the tab. Keeping the prompt keeps the request and the sheet in
    // step; destroying it would cancel a question the user can still see.
    return TabSwitchingBehavior::kKeepPromptAlive;
  }
  permissions::PermissionPromptDisposition GetPromptDisposition()
      const override {
    return permissions::PermissionPromptDisposition::MODAL_DIALOG;
  }
  bool IsAskPrompt() const override { return true; }
  std::optional<gfx::Rect> GetViewBoundsInScreen() const override {
    // Drawn by the shell, so Chromium does not know where it is.
    return std::nullopt;
  }
  std::vector<permissions::ElementAnchoredBubbleVariant> GetPromptVariants()
      const override {
    return {};
  }
  std::optional<permissions::feature_params::PermissionElementPromptPosition>
  GetPromptPosition() const override {
    return std::nullopt;
  }

 private:
  const raw_ptr<content::WebContents> web_contents_;
  const raw_ptr<Delegate> delegate_;
  const int request_id_;
};

}  // namespace

bool ShellDrawsPermissionPrompts() {
  // Follows whoever draws the browser UI: where Chromium shows no location
  // bar there is nothing for its bubble to hang off.
  return IsAuraShellMobilePhoneUi();
}

std::unique_ptr<permissions::PermissionPrompt> CreateShellPermissionPrompt(
    content::WebContents* web_contents,
    permissions::PermissionPrompt::Delegate* delegate) {
  if (!web_contents || !delegate) {
    return nullptr;
  }
  std::vector<std::string_view> permissions = ShellPermissionNames(delegate);
  if (permissions.empty()) {
    // Something the shell has no sheet for. Chromium's own UI is a worse fit
    // here but it is a real one, and silence would leave the page waiting.
    return nullptr;
  }
  return std::make_unique<ShellPermissionPrompt>(web_contents, delegate,
                                                 std::move(permissions));
}

void ResolveShellPermissionRequest(int request_id, std::string_view decision) {
  auto it = PendingPrompts().find(request_id);
  if (it == PendingPrompts().end()) {
    // Answered twice, or answered after Chromium dropped the request.
    return;
  }
  it->second->Resolve(decision);
}

}  // namespace chrome::ohos
