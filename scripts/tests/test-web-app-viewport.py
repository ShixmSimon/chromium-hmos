#!/usr/bin/env python3
"""Compile actual viewport policies to check browser/PWA coexistence.

Uses production bridge functions and delegate methods from the maintained
patch, with minimal platform stubs. Does not replace on-device layout checks.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
bridge = (ROOT / 'overlay/chrome/browser/ui/ohos/aura_shell_runtime_bridge.cc').read_text()
patch = ROOT / 'patches/migration-155/chromium.patch'
if not patch.exists():
    patch = ROOT / 'patches/chromium-154-harmonyos.patch'
name = 'chrome/browser/ui/browser_web_contents_delegate/browser_web_contents_delegate.cc'
section = re.search(r'^diff --git a/' + re.escape(name) + r' .*?(?=^diff --git |\Z)',
                    patch.read_text(), re.M | re.S).group()
patched = '\n'.join(line[1:] for line in section.splitlines()
                    if line[:1] in (' ', '+') and not line.startswith('+++'))


def function(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


stubs = r'''
#include <algorithm>
#include <cassert>
#include <map>
#define BUILDFLAG(x) 1
namespace gfx {
using AcceleratedWidget = int;
struct Insets {
  int top, left, bottom, right;
  static Insets TLBR(int t, int l, int b, int r) { return {t,l,b,r}; }
};
}
struct BrowserWindowInterface {
  enum class Type { TYPE_NORMAL, TYPE_APP, TYPE_APP_POPUP, TYPE_POPUP };
  Type type;
  Type GetType() { return type; }
};
BrowserWindowInterface* browsers[3];
BrowserWindowInterface* FindBrowserForWidget(int id) { return browsers[id]; }
namespace base { struct AutoLock { explicit AutoLock(int&) {} }; }
struct RuntimeBridgeState {
  int lock = 0, top_controls_height = 90, bottom_controls_height = 60;
  std::map<int, int> viewport_bottom_inset;
} state;
RuntimeBridgeState& GetState() { return state; }
namespace content { struct WebContents { bool hidden = false; }; }
namespace chrome::ohos {
int minimum = 20;
int GetAuraShellTopControlsHeight() { return state.top_controls_height + state.bottom_controls_height; }
int GetAuraShellTopControlsMinHeight() { return minimum; }
bool AreAuraShellBrowserControlsHidden(content::WebContents* c) { return c->hidden; }
}
struct Window {
  int GetTopControlsHeight() { return 12; }
  bool DoBrowserControlsShrinkRendererSize(content::WebContents*) { return true; }
} window;
struct BrowserWebContentsDelegate {
  void* app_browser_controller_ = nullptr;
  Window* window_ = &window;
  int GetTopControlsHeight();
  int GetTopControlsMinHeight();
  bool DoBrowserControlsShrinkRendererSize(content::WebContents*);
};
'''
cases = r'''
int main() {
  BrowserWindowInterface normal{BrowserWindowInterface::Type::TYPE_NORMAL};
  BrowserWindowInterface app{BrowserWindowInterface::Type::TYPE_APP};
  BrowserWindowInterface popup{BrowserWindowInterface::Type::TYPE_APP_POPUP};
  browsers[0] = &normal; browsers[1] = &app; browsers[2] = &popup;
  assert(!IsWebAppBrowser(nullptr));
  assert(!IsWebAppBrowser(&normal));
  assert(IsWebAppBrowser(&app) && IsWebAppBrowser(&popup));
  BrowserWebContentsDelegate browser, pwa;
  pwa.app_browser_controller_ = &app;
  content::WebContents page;
  for (int height : {90, 40, 0}) {
    state.top_controls_height = height;
    for (bool hidden : {false, true}) {
      page.hidden = hidden;
      assert(browser.GetTopControlsHeight() == height + 60);
      assert(browser.GetTopControlsMinHeight() == 20);
      assert(browser.DoBrowserControlsShrinkRendererSize(&page) == !hidden);
      assert(pwa.GetTopControlsHeight() == 0);
      assert(pwa.GetTopControlsMinHeight() == 0);
      assert(!pwa.DoBrowserControlsShrinkRendererSize(&page));
      assert(state.top_controls_height == height && state.bottom_controls_height == 60);
    }
    for (int keyboard : {0, 300, 0}) {
      for (int id : {0, 1, 2}) state.viewport_bottom_inset[id] = keyboard;
      const auto browser_insets = ModalDialogInsetsForWidget(0);
      assert(browser_insets.top == height && browser_insets.bottom == keyboard + 60);
      for (int id : {1, 2}) {
        const auto app_insets = ModalDialogInsetsForWidget(id);
        assert(app_insets.top == 0 && app_insets.bottom == keyboard);
      }
    }
  }
  state.top_controls_height = state.bottom_controls_height = 0;
  assert(browser.GetTopControlsHeight() == 12); // preserve regular-window fallback
  assert(pwa.GetTopControlsHeight() == 0); // PWA must bypass even that fallback
}
'''
source = stubs + '\n' + '\n'.join([
    function(bridge, 'bool IsWebAppBrowser('),
    function(bridge, 'gfx::Insets ModalDialogInsetsForWidget('),
    *[function(patched, signature) for signature in (
        'int BrowserWebContentsDelegate::GetTopControlsHeight()',
        'int BrowserWebContentsDelegate::GetTopControlsMinHeight()',
        'bool BrowserWebContentsDelegate::DoBrowserControlsShrinkRendererSize(')]
]) + '\n' + cases
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'test.cc').write_text(source)
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    str(path / 'test.cc'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('PASS: browser/PWA controls, window isolation, dialog and keyboard insets')
