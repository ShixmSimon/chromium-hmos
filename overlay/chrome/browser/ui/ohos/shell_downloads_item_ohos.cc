// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/ohos/shell_downloads_item_ohos.h"

#include <stdint.h>

#include <string_view>

#include "base/files/file_path.h"
#include "chrome/browser/ui/ohos/shell_services_ohos.h"
#include "components/download/public/common/download_danger_type.h"
#include "components/download/public/common/download_interrupt_reasons.h"
#include "components/download/public/common/download_item.h"
#include "url/gurl.h"

namespace chrome::ohos {

namespace {

using download::DownloadItem;

// Chromium has no danger type for an insecurely fetched file; it is a
// separate flag (IsInsecure), so the shell gets a name of our own for it.
constexpr char kInsecureDangerType[] = "Insecure";

// Until the name is decided only the partial file's path exists.
const base::FilePath& PathOf(const DownloadItem& item) {
  return item.GetTargetFilePath().empty() ? item.GetFullPath()
                                          : item.GetTargetFilePath();
}

std::string_view StateName(const DownloadItem& item) {
  switch (item.GetState()) {
    case DownloadItem::IN_PROGRESS:
      return item.IsPaused() ? "paused" : "inProgress";
    case DownloadItem::COMPLETE:
      return "completed";
    case DownloadItem::CANCELLED:
      return "cancelled";
    case DownloadItem::INTERRUPTED:
    case DownloadItem::MAX_DOWNLOAD_STATE:
      break;
  }
  return "failed";
}

std::string FailReason(const DownloadItem& item) {
  const download::DownloadInterruptReason reason = item.GetLastReason();
  return reason == download::DOWNLOAD_INTERRUPT_REASON_NONE
             ? std::string()
             : download::DownloadInterruptReasonToString(reason);
}

// Chromium's own names ("DangerousFile", "DangerousURL", ...), "" when the
// download needs no verdict.
std::string DangerTypeName(const DownloadItem& item) {
  if (item.IsDangerous()) {
    return download::GetDownloadDangerTypeString(item.GetDangerType());
  }
  return item.IsInsecure() ? kInsecureDangerType : std::string();
}

}  // namespace

bool IsUserVisibleDownload(const DownloadItem& item) {
  return !item.IsTransient() && !item.IsTemporary();
}

bool IsAwaitingUserVerdict(const DownloadItem& item) {
  return item.IsDangerous() || item.IsInsecure();
}

std::string DownloadChangeSignature(const DownloadItem& item) {
  return std::string(StateName(item)) + '\n' + DangerTypeName(item) + '\n' +
         PathOf(item).value();
}

base::DictValue ToShellDownloadItem(const DownloadItem& item, bool removed) {
  const int64_t total = item.GetTotalBytes();
  base::DictValue result;
  result.Set("id", item.GetGuid());
  result.Set("url", item.GetURL().is_valid() ? item.GetURL().spec()
                                             : std::string());
  result.Set("fileName", item.GetFileNameToReportUser().AsUTF8Unsafe());
  result.Set("filePath", PathOf(item).AsUTF8Unsafe());
  result.Set("mimeType", item.GetMimeType());
  // Doubles: byte counts outgrow base::Value's 32-bit ints.
  result.Set("receivedBytes", static_cast<double>(item.GetReceivedBytes()));
  // Chromium reports an unknown size as 0.
  result.Set("totalBytes", total > 0 ? static_cast<double>(total) : -1.0);
  result.Set("state", StateName(item));
  result.Set("failReason", FailReason(item));
  result.Set("canResume", item.CanResume());
  result.Set("dangerous", IsAwaitingUserVerdict(item));
  result.Set("dangerType", DangerTypeName(item));
  result.Set("startTime", ToShellTime(item.GetStartTime()));
  result.Set("endTime", ToShellTime(item.GetEndTime()));
  result.Set("removed", removed);
  return result;
}

}  // namespace chrome::ohos
