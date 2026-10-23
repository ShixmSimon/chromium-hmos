#!/usr/bin/env python3
"""Compile the production desktop-site classifier against minimal browser stubs.

An inherited phone UA override must keep the mobile viewport and leave the
Desktop site menu unchecked. This exercises the real classifier, without a
full Chromium checkout; it does not replace device viewport acceptance.
"""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
bridge = ROOT / 'overlay/chrome/browser/ui/ohos/aura_shell_runtime_bridge.cc'
source = bridge.read_text()
def production_function(signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

classifier = production_function('bool IsAuraShellRequestingDesktopSite(')
inherit = production_function('void InheritAuraShellUserAgent(')
apply = production_function('bool ApplyUserAgentToWebContents(')

STUBS = r'''
#include <cassert>
#include <optional>
#include <string>
namespace blink {
struct UserAgentMetadata { bool mobile; bool operator==(const UserAgentMetadata&) const = default; };
struct UserAgentOverride {
  std::string ua_string_override;
  std::optional<UserAgentMetadata> ua_metadata_override;
  bool operator==(const UserAgentOverride&) const = default;
};
}
namespace content {
enum class ReloadType { NORMAL };
struct NavigationEntry {
  bool overriding = false;
  bool GetIsOverridingUserAgent() const { return overriding; }
};
struct NavigationController {
  NavigationEntry* visible = nullptr;
  NavigationEntry* GetVisibleEntry() { return visible; }
  NavigationEntry* GetLastCommittedEntry() { return visible; }
  bool NeedsReload() { return false; }
  void Reload(ReloadType, bool) {}
};
struct WebContents {
  NavigationController controller;
  blink::UserAgentOverride ua;
  bool pinned = false;
  bool inherits = false;
  void SetUserAgentOverride(const blink::UserAgentOverride& value, bool inherit) {
    ua = value;
    inherits = inherit;
  }
  NavigationController& GetController() { return controller; }
  const blink::UserAgentOverride& GetUserAgentOverride() const { return ua; }
};
}
blink::UserAgentOverride AuraShellDesktopSiteUserAgent() {
  return {"desktop Linux", blink::UserAgentMetadata{false}};
}
bool IsAuraShellMobilePhoneUi() { return true; }
blink::UserAgentOverride BuildOhosUserAgentOverride(bool mobile) {
  return {mobile ? "Chrome Mobile" : "tablet", blink::UserAgentMetadata{mobile}};
}
bool IsAuraShellUserAgentPinned(content::WebContents* tab) { return tab->pinned; }
void SetAuraShellUserAgentPinned(content::WebContents* tab, bool pin) { tab->pinned = pin; }
'''
CASES = r'''
int main() {
  using content::WebContents;
  assert(!IsAuraShellRequestingDesktopSite(nullptr));
  WebContents tab;
  tab.ua = AuraShellDesktopSiteUserAgent();
  assert(!IsAuraShellRequestingDesktopSite(&tab)); // no visible navigation
  content::NavigationEntry entry;
  tab.controller.visible = &entry;
  assert(!IsAuraShellRequestingDesktopSite(&tab)); // UA override not in use
  entry.overriding = true;
  assert(IsAuraShellRequestingDesktopSite(&tab)); // explicit desktop mode
  tab.ua = {"Chrome Mobile", blink::UserAgentMetadata{true}};
  assert(!IsAuraShellRequestingDesktopSite(&tab)); // inherited phone override
  // Client hints decide when supplied, including for custom UA strings.
  tab.ua = {"custom string", blink::UserAgentMetadata{true}};
  assert(!IsAuraShellRequestingDesktopSite(&tab));
  tab.ua = {"store Linux", blink::UserAgentMetadata{false}};
  assert(IsAuraShellRequestingDesktopSite(&tab)); // web-store desktop override
  entry.overriding = false;
  assert(!IsAuraShellRequestingDesktopSite(&tab)); // back/forward mobile entry
  entry.overriding = true;
  tab.ua = {"", std::nullopt};
  assert(!IsAuraShellRequestingDesktopSite(&tab));
  tab.ua = {"unknown custom string", std::nullopt};
  assert(!IsAuraShellRequestingDesktopSite(&tab));
  tab.ua = {"Chrome Mobile", std::nullopt};
  assert(!IsAuraShellRequestingDesktopSite(&tab));
  tab.ua = {"desktop Linux", std::nullopt};
  assert(IsAuraShellRequestingDesktopSite(&tab)); // known desktop UA fallback

  WebContents child, grandchild;
  content::NavigationEntry child_entry{true}, grandchild_entry{true};
  child.controller.visible = &child_entry;
  grandchild.controller.visible = &grandchild_entry;
  for (bool desktop : {false, true}) {
    tab.ua = desktop ? AuraShellDesktopSiteUserAgent()
                     : BuildOhosUserAgentOverride(true);
    InheritAuraShellUserAgent(&tab, &child);
    assert(child.inherits);
    assert(child.pinned == desktop);
    assert(IsAuraShellRequestingDesktopSite(&child) == desktop);
    ApplyUserAgentToWebContents(&child, true, false);
    assert(child.ua == tab.ua); // shell poll preserves inherited desktop mode
    InheritAuraShellUserAgent(&child, &grandchild);
    assert(grandchild.ua == tab.ua);
    tab.ua = BuildOhosUserAgentOverride(true); // parent changes or closes
    ApplyUserAgentToWebContents(&grandchild, true, false);
    assert(IsAuraShellRequestingDesktopSite(&grandchild) == desktop);
  }
  tab.ua = {"store Linux", blink::UserAgentMetadata{false}};
  InheritAuraShellUserAgent(&tab, &child);
  assert(child.ua == AuraShellDesktopSiteUserAgent()); // normalize store spoof
  WebContents independent;
  ApplyUserAgentToWebContents(&independent, true, false);
  assert(independent.ua == BuildOhosUserAgentOverride(true));
  InheritAuraShellUserAgent(nullptr, &child);
  InheritAuraShellUserAgent(&child, nullptr);
  InheritAuraShellUserAgent(&child, &child);
  assert(child.ua == AuraShellDesktopSiteUserAgent());
}
'''
with tempfile.TemporaryDirectory(prefix='phone-ua-policy-') as directory:
    unit = Path(directory) / 'policy.cc'
    binary = Path(directory) / 'policy'
    unit.write_text(STUBS + classifier + '\n' + inherit + '\n' + apply + '\n' + CASES)
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-Wall', '-Wextra',
                    '-Werror', str(unit), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('Phone/desktop child and nested inheritance, state polling, store normalization, and viewport classification passed.')
