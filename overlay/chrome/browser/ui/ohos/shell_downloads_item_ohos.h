// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_OHOS_SHELL_DOWNLOADS_ITEM_OHOS_H_
#define CHROME_BROWSER_UI_OHOS_SHELL_DOWNLOADS_ITEM_OHOS_H_

#include <string>

#include "base/values.h"

namespace download {
class DownloadItem;
}

namespace chrome::ohos {

// How one download reads to the ArkUI shell. Internal to the shell downloads
// service (shell_downloads_ohos.cc); split out to keep that file small.

// Transient downloads (updates, extension internals) and temporary ones
// (drag-out, printing) are not the user's and never show in download UI.
bool IsUserVisibleDownload(const download::DownloadItem& item);

// Whether the download waits for the user to accept or discard it: a
// dangerous file, or one fetched insecurely.
bool IsAwaitingUserVerdict(const download::DownloadItem& item);

// What makes an update worth sending at once rather than on the progress
// clock: a new state, the file name being decided, or a new danger verdict.
std::string DownloadChangeSignature(const download::DownloadItem& item);

// The item as the shell's downloadUpdated / downloadList entries carry it.
// `removed` is true only in the event announcing its removal.
base::DictValue ToShellDownloadItem(const download::DownloadItem& item,
                                    bool removed);

}  // namespace chrome::ohos

#endif  // CHROME_BROWSER_UI_OHOS_SHELL_DOWNLOADS_ITEM_OHOS_H_
