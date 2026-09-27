// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_OHOS_SHELL_DRAG_DROP_OHOS_H_
#define CHROME_BROWSER_UI_OHOS_SHELL_DRAG_DROP_OHOS_H_

#include "base/values.h"
#include <string>

#include "ui/gfx/native_ui_types.h"

namespace chrome::ohos {

// Something is being dragged over, or has been dropped on, a page.
//
// The shell sees HarmonyOS's drag on the XComponent and sends it here as
// { stage, x, y, urls, text, html, files }. What comes back is the operation
// the page will accept -- "copy", "move" or "none" -- so the shell can show
// the right badge before the finger lifts.
//
// Files arrive as paths the engine can already read: the shell has copied
// them out of the sending app's URI while the drop's temporary permission
// was still good. Everything else is the data itself.
std::string HandleShellDragEvent(gfx::AcceleratedWidget widget,
                                 const base::DictValue& event);

// Lets the page start a system drag of its own.
//
// Chromium asks its platform window to start the drag; the window cannot,
// because HarmonyOS wants that done from the app's UI thread, so it comes
// back out here as a `pageDragStarted` event and waits. The shell shows the
// drag, and says how it ended with `pageDragFinished`.
//
// Called once, at startup.
void WatchPageDragsOut();

// The drag the page started has ended: `operation` is what the thing it
// landed on took, as "copy", "move", "link" or "none". Whatever is waiting
// inside StartDrag stops waiting.
void FinishPageDragOut(gfx::AcceleratedWidget widget,
                       const std::string* operation);

// Empties the directory dropped files are copied into. Called once at
// startup: a file dragged in for an upload that never happened should not
// outlive the run it arrived in.
void ClearDroppedFileDirectory();

}  // namespace chrome::ohos

#endif  // CHROME_BROWSER_UI_OHOS_SHELL_DRAG_DROP_OHOS_H_
