// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/ohos/screen_orientation_delegate_ohos.h"

#include <optional>
#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/values.h"
#include "chrome/browser/ui/ohos/aura_shell_runtime_bridge.h"
#include "components/ohos_system_service/system_service_ohos.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"
#include "services/device/public/mojom/screen_orientation_lock_types.mojom-shared.h"
#include "ui/gfx/geometry/size.h"

namespace {

constexpr char kService[] = "orientation";

// The names OrientationService maps to window.Orientation.
const char* LockTypeName(device::mojom::ScreenOrientationLockType type) {
  using Type = device::mojom::ScreenOrientationLockType;
  switch (type) {
    case Type::PORTRAIT_PRIMARY:
      return "portrait-primary";
    case Type::PORTRAIT_SECONDARY:
      return "portrait-secondary";
    case Type::LANDSCAPE_PRIMARY:
      return "landscape-primary";
    case Type::LANDSCAPE_SECONDARY:
      return "landscape-secondary";
    case Type::PORTRAIT:
      return "portrait";
    case Type::LANDSCAPE:
      return "landscape";
    case Type::ANY:
      return "any";
    case Type::NATURAL:
      // ScreenOrientationProvider resolves NATURAL before calling Lock.
      return "natural";
    case Type::DEFAULT:
      return nullptr;
  }
  return nullptr;
}

void LogFailure(const char* what, ohos_system_service::Reply reply) {
  if (!reply.ok) {
    // The lock promise stays pending until the orientation actually
    // changes; a refused request leaves it pending, which is what the page
    // would see on a device that cannot rotate.
    LOG(WARNING) << "HarmonyOS orientation " << what
                 << " failed: " << reply.error;
  }
}

}  // namespace

ScreenOrientationDelegateOhos::ScreenOrientationDelegateOhos() = default;
ScreenOrientationDelegateOhos::~ScreenOrientationDelegateOhos() = default;

bool ScreenOrientationDelegateOhos::FullScreenRequired(
    content::WebContents* web_contents) {
  return true;
}

void ScreenOrientationDelegateOhos::Lock(
    content::WebContents* web_contents,
    device::mojom::ScreenOrientationLockType lock_orientation) {
  const char* name = LockTypeName(lock_orientation);
  if (!name) {
    Unlock(web_contents);
    return;
  }
  base::DictValue args;
  args.Set("type", name);
  ohos_system_service::Call(kService, "lock", std::move(args),
                            base::BindOnce(&LogFailure, "lock"));
}

bool ScreenOrientationDelegateOhos::ScreenOrientationProviderSupported(
    content::WebContents* web_contents) {
  return !chrome::ohos::IsAuraShellDesktopUi() &&
         ohos_system_service::IsAvailable();
}

void ScreenOrientationDelegateOhos::Unlock(content::WebContents* web_contents) {
  ohos_system_service::Call(kService, "unlock", base::DictValue(),
                            base::BindOnce(&LogFailure, "unlock"));
}

namespace chrome::ohos {
namespace {

class FullscreenVideoOrientation
    : public content::WebContentsObserver,
      public content::WebContentsUserData<FullscreenVideoOrientation> {
 public:
  ~FullscreenVideoOrientation() override { Release(); }

  // content::WebContentsObserver:
  //
  // Media effectively fullscreen means the fullscreen element is a video, or
  // is mostly one -- a player's own wrapper with its controls counts -- and
  // by then the video's size is known.
  void MediaEffectivelyFullscreenChanged(bool is_fullscreen) override {
    if (!is_fullscreen) {
      Release();
      return;
    }
    if (locked_ || !IsAuraShellMobilePhoneUi() ||
        !ohos_system_service::IsAvailable()) {
      return;
    }
    const std::optional<gfx::Size> size =
        web_contents()->GetFullscreenVideoSize();
    if (!size || size->width() <= size->height()) {
      return;
    }
    base::DictValue args;
    args.Set("type", "landscape");
    ohos_system_service::Call(kService, "lock", std::move(args),
                              base::BindOnce(&LogFailure, "lock"));
    locked_ = true;
  }

  void DidToggleFullscreenModeForTab(bool entered_fullscreen,
                                     bool will_cause_resize) override {
    if (!entered_fullscreen) {
      Release();
    }
  }

 private:
  friend class content::WebContentsUserData<FullscreenVideoOrientation>;

  explicit FullscreenVideoOrientation(content::WebContents* web_contents)
      : content::WebContentsObserver(web_contents),
        content::WebContentsUserData<FullscreenVideoOrientation>(
            *web_contents) {}

  // Only a lock this made: a page that locked the orientation itself is
  // unlocked by ScreenOrientationProvider when it leaves fullscreen.
  void Release() {
    if (!locked_) {
      return;
    }
    locked_ = false;
    ohos_system_service::Call(kService, "unlock", base::DictValue(),
                              base::BindOnce(&LogFailure, "unlock"));
  }

  bool locked_ = false;

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

WEB_CONTENTS_USER_DATA_KEY_IMPL(FullscreenVideoOrientation);

}  // namespace

void WatchFullscreenVideoOrientation(content::WebContents* web_contents) {
  FullscreenVideoOrientation::CreateForWebContents(web_contents);
}

}  // namespace chrome::ohos
