// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_OHOS_EXTENSION_UPDATE_RESULT_OHOS_H_
#define CHROME_BROWSER_UI_OHOS_EXTENSION_UPDATE_RESULT_OHOS_H_

#include <string_view>

namespace chrome::ohos {

// Independent of the downloader: only the registry can establish that an
// update became active. A finished check alone is not proof of success.
inline std::string_view ResolveExtensionUpdateResult(
    std::string_view observed_status,
    bool removed,
    bool newer_installed,
    bool permissions_required,
    bool pending_install,
    bool finished) {
  if (removed) {
    return "failed";
  }
  if (observed_status == "manual") {
    return "manual";
  }
  if (newer_installed) {
    return permissions_required ? "permissionsRequired" : "updated";
  }
  if (pending_install) {
    return "pendingReload";
  }
  if (finished && observed_status != "upToDate") {
    return "failed";
  }
  return observed_status;
}

}  // namespace chrome::ohos
#endif  // CHROME_BROWSER_UI_OHOS_EXTENSION_UPDATE_RESULT_OHOS_H_
