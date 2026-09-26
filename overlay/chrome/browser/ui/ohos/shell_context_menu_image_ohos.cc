// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/ohos/shell_context_menu_image_ohos.h"

#include <string>
#include <utility>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/ui/ohos/aura_shell_runtime_bridge.h"
#include "chrome/common/chrome_paths.h"
#include "content/public/browser/web_contents.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/gfx/codec/png_codec.h"
#include "url/gurl.h"

namespace chrome::ohos {

namespace {

// One directory, emptied at startup. Not the system temp directory: this app
// cannot be sure who else writes there, and a shared image is the reader's
// picture rather than scratch space.
base::FilePath SharedImageDirectory() {
  base::FilePath dir;
  if (!base::PathService::Get(chrome::DIR_USER_DATA, &dir)) {
    return base::FilePath();
  }
  return dir.AppendASCII("shared-images");
}

// The bitmap the renderer decoded, as a PNG on disk. PNG rather than the
// original bytes: what comes back is the decoded image, and re-encoding it
// losslessly is both simpler and safer than trying to pass through whatever
// the site served, which may be AVIF or WebP that the share target cannot
// read.
base::FilePath WritePng(SkBitmap bitmap, base::FilePath path) {
  std::optional<std::vector<uint8_t>> png =
      gfx::PNGCodec::EncodeBGRASkBitmap(bitmap, /*discard_transparency=*/false);
  if (!png || png->empty()) {
    return base::FilePath();
  }
  if (!base::CreateDirectory(path.DirName())) {
    return base::FilePath();
  }
  if (!base::WriteFile(path, *png)) {
    return base::FilePath();
  }
  return path;
}

void ReplyWithPath(base::WeakPtr<content::WebContents> contents,
                   int request_id,
                   base::FilePath path) {
  if (!contents) {
    return;
  }
  base::DictValue event;
  event.Set("event", "contextMenuImageFile");
  event.Set("requestId", request_id);
  // Empty when there was no image to write. The shell is told either way, so
  // it is never left waiting for an answer that is not coming.
  event.Set("path", path.empty() ? std::string() : path.AsUTF8Unsafe());
  event.Set("mimeType", path.empty() ? std::string() : "image/png");
  if (path.empty()) {
    LOG(WARNING) << "OHOS context menu: no file for the long-pressed image";
  }
  DispatchAuraShellRuntimeEvent(contents.get(), std::move(event));
}

void OnImageDownloaded(base::WeakPtr<content::WebContents> contents,
                       int request_id,
                       int /*id*/,
                       int http_status_code,
                       const GURL& image_url,
                       const std::vector<SkBitmap>& bitmaps,
                       const std::vector<gfx::Size>& /*sizes*/) {
  if (bitmaps.empty() || bitmaps.front().drawsNothing()) {
    LOG(WARNING) << "OHOS context menu: the image did not come back, status="
                 << http_status_code;
    ReplyWithPath(contents, request_id, base::FilePath());
    return;
  }
  const base::FilePath dir = SharedImageDirectory();
  if (dir.empty()) {
    ReplyWithPath(contents, request_id, base::FilePath());
    return;
  }
  // Named after the moment rather than the URL: a site's filename is the
  // site's to choose, and this one becomes a path handed to another app.
  const base::FilePath path = dir.AppendASCII(
      "image-" +
      base::NumberToString(base::Time::Now().InMillisecondsSinceUnixEpoch()) +
      ".png");
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&WritePng, bitmaps.front(), path),
      base::BindOnce(&ReplyWithPath, contents, request_id));
}

}  // namespace

void WriteContextMenuImageToFile(content::WebContents* contents,
                                 const GURL& image_url,
                                 int request_id) {
  if (!contents || !image_url.is_valid()) {
    if (contents) {
      ReplyWithPath(contents->GetWeakPtr(), request_id, base::FilePath());
    }
    return;
  }
  // From the cache, at whatever size it was decoded: the reader long-pressed
  // the picture they can see, and going back to the network for a larger one
  // would share something they have not seen.
  contents->DownloadImage(
      image_url, /*is_favicon=*/false, /*preferred_size=*/gfx::Size(),
      /*max_bitmap_size=*/0, /*bypass_cache=*/false,
      base::BindOnce(&OnImageDownloaded, contents->GetWeakPtr(), request_id));
}

void ClearSharedImageDirectory() {
  const base::FilePath dir = SharedImageDirectory();
  if (dir.empty()) {
    return;
  }
  base::ThreadPool::PostTask(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::BEST_EFFORT,
       base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN},
      base::GetDeletePathRecursivelyCallback(dir));
}

}  // namespace chrome::ohos
