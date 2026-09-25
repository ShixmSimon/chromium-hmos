// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/ohos/shell_tab_groups_ohos.h"

#include <vector>

#include "base/logging.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/sessions/session_service.h"
#include "chrome/browser/sessions/session_service_factory.h"
#include "chrome/browser/ui/ohos/aura_shell_runtime_bridge.h"
#include "components/tabs/public/tab_group.h"
#include "chrome/browser/ui/tabs/tab_group_model.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "components/sessions/content/session_tab_helper.h"
#include "content/public/browser/web_contents.h"

namespace chrome::ohos {

const char kOhosOpenerSessionKey[] = "ohos.opener";

namespace {

// Who opened whom, by the opened page's session id -- which is stable for the
// tab's life and never reused -- pointing weakly at the opener, so a closed
// opener reads as none without anyone having to notice it close.
//
// Entries for tabs that have since closed are left behind. They are a pointer
// and an integer each, bounded by the number of pages opened from other pages
// in one run of the browser, which is not a quantity worth a tab-strip
// observer to keep tidy.
using OpenerMap = std::map<int, base::WeakPtr<content::WebContents>>;

OpenerMap& Openers() {
  static base::NoDestructor<OpenerMap> map;
  return *map;
}

// Restored tabs, by the session id they had before the browser was shut down,
// and the openers they are still waiting to be matched with. Resolved on
// demand rather than at the end of the restore, because a tab can be restored
// before the tab that opened it.
std::map<int, base::WeakPtr<content::WebContents>>& RestoredByFormerId() {
  static base::NoDestructor<std::map<int, base::WeakPtr<content::WebContents>>>
      map;
  return *map;
}

std::map<int, int>& PendingRestoredOpeners() {
  static base::NoDestructor<std::map<int, int>> map;
  return *map;
}

int SessionIdOf(content::WebContents* contents) {
  if (!contents) {
    return 0;
  }
  const SessionID id = sessions::SessionTabHelper::IdForTab(contents);
  return id.is_valid() ? id.id() : 0;
}

// Writes the pair into the session file so it survives the browser being
// closed. Chromium does not persist its own openers -- nothing in the session
// service writes them -- and this one is drawn on screen, so it has to be
// written by hand. Stored as the opener's session id of the moment; the ids
// change on restore and NoteRestoredPageOpener() translates them.
void RememberOpenerAcrossRestarts(content::WebContents* opened,
                                  content::WebContents* opener) {
  auto* helper = sessions::SessionTabHelper::FromWebContents(opened);
  if (!helper || !helper->window_id().is_valid() ||
      !helper->session_id().is_valid()) {
    return;
  }
  Profile* profile =
      Profile::FromBrowserContext(opened->GetBrowserContext());
  if (!profile || profile->IsOffTheRecord()) {
    // Nothing about an incognito tab is written down, which is the point.
    return;
  }
  SessionService* service = SessionServiceFactory::GetForProfile(profile);
  if (!service) {
    return;
  }
  service->AddTabExtraData(helper->window_id(), helper->session_id(),
                           kOhosOpenerSessionKey,
                           base::NumberToString(SessionIdOf(opener)));
}

// Turns a pending restored pair into a live one, if both ends have arrived.
void ResolveRestoredOpener(int session_id) {
  auto pending = PendingRestoredOpeners().find(session_id);
  if (pending == PendingRestoredOpeners().end()) {
    return;
  }
  auto opener = RestoredByFormerId().find(pending->second);
  if (opener == RestoredByFormerId().end()) {
    return;
  }
  if (opener->second) {
    Openers()[session_id] = opener->second;
  }
  PendingRestoredOpeners().erase(pending);
}

}  // namespace

bool ShouldGroupTabsWithTheirOpener() {
  return IsAuraShellMobilePhoneUi();
}

std::optional<tab_groups::TabGroupId> GroupForPageOpenedFrom(
    TabStripModel* tabs,
    content::WebContents* source) {
  if (!tabs || !source || !tabs->SupportsTabGroups()) {
    return std::nullopt;
  }
  const std::optional<int> index = tabs->GetIndexOfWebContents(source);
  if (!index) {
    // The page that opened this one is not in this window, and a group cannot
    // span windows.
    return std::nullopt;
  }
  if (const std::optional<tab_groups::TabGroupId> existing =
          tabs->GetTabGroupForTab(*index)) {
    return existing;
  }
  if (!ShouldGroupTabsWithTheirOpener()) {
    return std::nullopt;
  }
  // The source is on its own so far. The group starts with it, so the reader
  // can get back to the page they were on from inside the group rather than
  // out of it.
  return tabs->AddToNewGroup({*index});
}

void DissolveSingleTabGroups(TabStripModel* tabs) {
  if (!tabs || !tabs->SupportsTabGroups() || !tabs->group_model()) {
    return;
  }
  for (const tab_groups::TabGroupId& group :
       tabs->group_model()->ListTabGroups()) {
    const TabGroup* model = tabs->group_model()->GetTabGroup(group);
    if (!model || model->tab_count() != 1) {
      continue;
    }
    const gfx::Range range = model->ListTabs();
    if (range.is_empty()) {
      continue;
    }
    tabs->RemoveFromGroup({static_cast<int>(range.start())});
  }
}

void RecordPageOpener(content::WebContents* opened,
                      content::WebContents* opener) {
  const int id = SessionIdOf(opened);
  if (!id || !opener || opened == opener) {
    return;
  }
  Openers()[id] = opener->GetWeakPtr();
  RememberOpenerAcrossRestarts(opened, opener);
}

content::WebContents* PageOpenerOf(content::WebContents* opened) {
  const int id = SessionIdOf(opened);
  if (!id) {
    return nullptr;
  }
  ResolveRestoredOpener(id);
  auto found = Openers().find(id);
  if (found == Openers().end()) {
    return nullptr;
  }
  if (!found->second) {
    // The opener has been closed. The link is gone for good, so it goes.
    Openers().erase(found);
    return nullptr;
  }
  return found->second.get();
}

std::string PageOpenerIdOf(content::WebContents* opened) {
  content::WebContents* opener = PageOpenerOf(opened);
  if (!opener) {
    return std::string();
  }
  const SessionID id = sessions::SessionTabHelper::IdForTab(opener);
  return id.is_valid() ? base::NumberToString(id.id()) : std::string();
}

void AnnouncePageOpenedInGroup(content::WebContents* opened) {
  if (!opened) {
    return;
  }
  const SessionID id = sessions::SessionTabHelper::IdForTab(opened);
  if (!id.is_valid()) {
    return;
  }
  base::DictValue event;
  event.Set("event", "openedInGroup");
  event.Set("id", base::NumberToString(id.id()));
  DispatchAuraShellRuntimeEvent(opened, std::move(event));
}

void NoteRestoredPageOpener(
    content::WebContents* restored,
    SessionID former_id,
    const std::map<std::string, std::string>& extra) {
  const int id = SessionIdOf(restored);
  if (!id || !former_id.is_valid()) {
    return;
  }
  RestoredByFormerId()[former_id.id()] = restored->GetWeakPtr();
  auto stored = extra.find(kOhosOpenerSessionKey);
  int former_opener = 0;
  if (stored != extra.end() &&
      base::StringToInt(stored->second, &former_opener) && former_opener) {
    PendingRestoredOpeners()[id] = former_opener;
  }
  ResolveRestoredOpener(id);
}

}  // namespace chrome::ohos
