// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/platform_util.h"

#include <string>
#include <utility>

#include "base/values.h"
#include "chrome/browser/ui/ohos/aura_shell_runtime_bridge.h"
#include "ui/gfx/native_widget_types.h"
#include "url/gurl.h"

namespace platform_util {

namespace {

// The engine cannot start another app or show the Files app itself; the
// shell does both through the system. `action` is "open" for a file,
// "openFolder" for a folder and "reveal" for a file to show in its folder.
void RequestShellFileOpen(const base::FilePath& path, const char* action) {
  base::DictValue event;
  event.Set("event", "fileOpenRequested");
  event.Set("path", path.value());
  event.Set("action", action);
  chrome::ohos::DispatchAuraShellRuntimeEventToWidget(
      gfx::kNullAcceleratedWidget, std::move(event));
}

}  // namespace

void ShowItemInFolder(Profile* profile, const base::FilePath& full_path) {
  RequestShellFileOpen(full_path, "reveal");
}

namespace internal {

void PlatformOpenVerifiedItem(const base::FilePath& path, OpenItemType type) {
  RequestShellFileOpen(path, type == OPEN_FOLDER ? "openFolder" : "open");
}

}  // namespace internal

// Links for other apps from the browser itself -- a mailto: in settings, a
// help link -- rather than from a page, which ExternalProtocolHandler sends
// to the shell with its origin.
void OpenExternal(const GURL& url) {
  chrome::ohos::RequestAuraShellExternalUrl(nullptr, url, std::string());
}

}  // namespace platform_util
