// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/ohos/screen_orientation_delegate_ohos.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/values.h"
#include "chrome/browser/ui/ohos/aura_shell_runtime_bridge.h"
#include "components/ohos_system_service/system_service_ohos.h"
#include "services/device/public/mojom/screen_orientation_lock_types.mojom-shared.h"

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
