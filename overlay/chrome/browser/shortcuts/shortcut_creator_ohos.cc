// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/shortcuts/shortcut_creator.h"

#include "base/files/file_path.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"

// HarmonyOS has no freedesktop-style desktop environment (no XDG, no
// on-screen "desktop" a .desktop file could be dropped onto), so there is
// nothing analogous to the Linux/Windows/Mac desktop-shortcut mechanisms to
// hook up here. Until/unless an OHOS-native equivalent (e.g. a home-screen
// shortcut API) is wired up, report failure rather than pulling in any of
// the X11/XDG/dbus desktop-Linux machinery those platforms use.

namespace shortcuts {

void CreateShortcutOnUserDesktop(ShortcutMetadata shortcut_metadata,
                                 ShortcutCreatorCallback complete) {
  std::move(complete).Run(/*created_shortcut_path=*/base::FilePath(),
                          ShortcutCreatorResult::kError);
}

scoped_refptr<base::SequencedTaskRunner> GetShortcutsTaskRunner() {
  return base::ThreadPool::CreateSequencedTaskRunner(
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
       base::TaskShutdownBehavior::BLOCK_SHUTDOWN});
}

}  // namespace shortcuts
