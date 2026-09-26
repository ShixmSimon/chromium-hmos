// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_OHOS_SHELL_PAGE_POSITION_OHOS_H_
#define CHROME_BROWSER_UI_OHOS_SHELL_PAGE_POSITION_OHOS_H_

namespace content {
class WebContents;
}

namespace chrome::ohos {

// Where the reader is in the page, for HarmonyOS app continuation: the phone
// hands the tablet a URL and a position, and the tablet opens the same page in
// the same place.
//
// Two ways to ask, because onContinue is a synchronous callback the system
// wants back quickly. The shell should prefer the first.

// Starts telling this tab's window where the page has been scrolled to, once
// the scrolling stops. Event pageScrollSettled { url, scrollX, scrollY,
// pageWidth, pageHeight }, at most one per settle. Idempotent, so it can be
// called whenever a tab becomes the current one.
void WatchPageScroll(content::WebContents* contents);

// Asks now. Event pageContinuation { requestId, url, title, scrollX, scrollY,
// pageWidth, pageHeight }. Reading the position means asking the renderer, so
// the answer arrives a frame or two later -- which is why the settled event
// exists.
void ReadPageContinuation(content::WebContents* contents, int request_id);

// Scroll this tab to `ratio` of the way down the document once it has
// finished loading, and never again. Does nothing if the reader scrolls
// first: arriving somewhere and then being pulled away from it is worse than
// arriving at the top.
void RestoreScrollRatioOnce(content::WebContents* contents, double ratio);

}  // namespace chrome::ohos

#endif  // CHROME_BROWSER_UI_OHOS_SHELL_PAGE_POSITION_OHOS_H_
