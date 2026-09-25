// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_OHOS_SHELL_TAB_GROUPS_OHOS_H_
#define CHROME_BROWSER_UI_OHOS_SHELL_TAB_GROUPS_OHOS_H_

#include <map>
#include <optional>
#include <string>

#include "components/sessions/core/session_id.h"
#include "components/tab_groups/tab_group_id.h"

class TabStripModel;

namespace content {
class WebContents;
}

namespace chrome::ohos {

// Whether a page that opens another page should keep the two together.
//
// Phones only. A phone hides its tabs behind a grid, so a link that opens one
// opens it somewhere the reader cannot see, and back does not return to where
// they came from. Grouping the two keeps the pair in front of them. A tablet
// or a PC shows its tabs in a strip, where a new one is already visible, so
// they behave like desktop Chrome.
//
// Read at the moment of the click, so unfolding a foldable changes it.
bool ShouldGroupTabsWithTheirOpener();

// The group a page opened from `source` belongs in, creating one around
// `source` when it has none yet. Nullopt when this window cannot group, or
// when `source` is not one of its tabs.
std::optional<tab_groups::TabGroupId> GroupForPageOpenedFrom(
    TabStripModel* tabs,
    content::WebContents* source);

// A group with one tab left is not a group. Dissolves any such group in this
// window. Safe to call at any time and does nothing when there is nothing to
// do, so the caller need not track closes itself.
void DissolveSingleTabGroups(TabStripModel* tabs);

// --- Who opened whom. -------------------------------------------------------
//
// TabStripModel keeps an opener of its own, but it keeps one chain at a time:
// inserting an active tab with ADD_INHERIT_OPENER calls ForgetAllOpeners()
// first, so opening a second page from the first erases the first pair. The
// shell needs every link, because it draws them.

// `opened` was opened by `opener`. Either may be null, which records nothing.
void RecordPageOpener(content::WebContents* opened,
                      content::WebContents* opener);

// The page that opened this one, or null when there was none or it has since
// been closed.
content::WebContents* PageOpenerOf(content::WebContents* opened);

// The opener's id as the shell names tabs, or empty.
std::string PageOpenerIdOf(content::WebContents* opened);

// Restores what RecordPageOpener() recorded before the browser was last shut
// down. Called once per restored tab, with the ids the session file carried:
// the tab's own id as it was then, and whatever this key was stored under.
// The pairs are resolved lazily, since a tab may be restored before its
// opener is.
void NoteRestoredPageOpener(content::WebContents* restored,
                            SessionID former_id,
                            const std::map<std::string, std::string>& extra);

// Tells the shell a page arrived in a group without the reader being taken
// to it, so it can mark the group's button. Does nothing when `opened` is
// null or has no window to tell.
void AnnouncePageOpenedInGroup(content::WebContents* opened);

// The key the opener is stored under in the session file.
extern const char kOhosOpenerSessionKey[];

}  // namespace chrome::ohos

#endif  // CHROME_BROWSER_UI_OHOS_SHELL_TAB_GROUPS_OHOS_H_
