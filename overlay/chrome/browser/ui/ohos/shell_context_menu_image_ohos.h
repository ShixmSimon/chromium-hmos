// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_OHOS_SHELL_CONTEXT_MENU_IMAGE_OHOS_H_
#define CHROME_BROWSER_UI_OHOS_SHELL_CONTEXT_MENU_IMAGE_OHOS_H_

class GURL;

namespace content {
class WebContents;
}

namespace chrome::ohos {

// Puts the image the reader long-pressed into a file and tells the shell
// where, so it can hand the file to the system share sheet or to image
// recognition. Both want a path; neither can take a URL or a bitmap.
//
// Answers with contextMenuImageFile { requestId, path, mimeType }, and with
// an empty path when the image cannot be had -- it was never decoded, it has
// gone from the cache, or writing failed. The shell should expect that: the
// answer always comes, the file does not always exist.
//
// The file goes in a directory of its own under the profile, emptied at
// startup, so a share that never happens does not leave anything behind for
// longer than one run.
void WriteContextMenuImageToFile(content::WebContents* contents,
                                 const GURL& image_url,
                                 int request_id);

// Empties that directory. Called once when the browser starts.
void ClearSharedImageDirectory();

}  // namespace chrome::ohos

#endif  // CHROME_BROWSER_UI_OHOS_SHELL_CONTEXT_MENU_IMAGE_OHOS_H_
