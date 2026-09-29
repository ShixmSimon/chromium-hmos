// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_OHOS_SHELL_EXTENSIONS_OHOS_H_
#define CHROME_BROWSER_UI_OHOS_SHELL_EXTENSIONS_OHOS_H_

#include <string_view>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "base/values.h"
#include "ui/gfx/image/image.h"
#include "ui/gfx/native_ui_types.h"

class BrowserWindowInterface;
class Profile;

namespace extensions {
class Extension;
}

namespace chrome::ohos {

// What a shell needs to draw an extensions page of its own -- the phone has
// no chrome://extensions it can use -- on top of the toolbar list the bridge
// already sends as extensionActions.

// Adds userEnabled, hasPopup, optionsUrl, version, description and author to
// one extensionActions item. `enabled` in the same item is something else:
// whether the action is available on the current tab.
void AddExtensionPageFields(Profile* profile,
                            const extensions::Extension& extension,
                            base::DictValue* item);

// Extensions the page lists and the toolbar model does not, because the
// model holds only enabled ones: the ones the reader switched off, and the
// ones disabled for them.
std::vector<scoped_refptr<const extensions::Extension>>
DisabledExtensionsForPage(Profile* profile);

// The icon from the extension's manifest, for an extension the toolbar has
// no action for -- a disabled one. Loaded from disk the first time, so the
// first call returns an empty image and extensionActionsChanged follows
// once it is there; later calls return it at once.
gfx::Image ExtensionManifestIcon(Profile* profile,
                                 const extensions::Extension& extension,
                                 int size_px);

// setExtensionEnabled, uninstallExtension, getExtensionDetails,
// setExtensionSiteAccess and openExtensionOptions. Returns false for any
// other command name.
bool HandleShellExtensionCommand(std::string_view name,
                                 gfx::AcceleratedWidget widget,
                                 BrowserWindowInterface* browser,
                                 const base::DictValue& command);

}  // namespace chrome::ohos

#endif  // CHROME_BROWSER_UI_OHOS_SHELL_EXTENSIONS_OHOS_H_
