#!/usr/bin/env python3
"""Exercise production embedded-window geometry and shared ArkTS layout.

Only platform types are stubbed for the isolated geometry test. The kernel
itself must still compile and run through push-triggered native CI.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
platform = (ROOT / 'overlay/ui/ozone/platform/ohos/ohos_platform_window.cc').read_text()
start = platform.index('void OhosPlatformWindow::UpdateEmbeddedMoveBounds(')
opening = platform.index('{', start)
depth = 1
end = opening + 1
while depth:
    depth += (platform[end] == '{') - (platform[end] == '}')
    end += 1
method = platform[start:end]

def extract_function(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

origin_method = extract_function(platform, 'void OhosPlatformWindow::OnNativeSurfaceBoundsChanged(')
origin_stubs = r'''
#include <cassert>
namespace gfx {
struct Point {int x=0,y=0; bool operator!=(Point p)const{return x!=p.x||y!=p.y;}};
struct Size {int width,height;};
struct Rect {Point p; Size s; Rect(int x,int y,int w,int h):p{x,y},s{w,h}{}
 explicit Rect(Size v):p{},s(v){} bool IsEmpty()const{return s.width<=0||s.height<=0;}
 Point origin()const{return p;} Size size()const{return s;} void set_origin(Point v){p=v;}
};
}
bool system_callback=false;
bool HasOhosWindowActionCallback(int){return system_callback;}
struct Adapter {gfx::Rect bounds{100,200,600,500};
 int GetAcceleratedWidget(){return 2;} gfx::Rect GetBounds(){return bounds;}
 void SetBounds(gfx::Rect r){bounds=r;}};
struct Delegate {struct Change {bool changed;};void OnBoundsChanged(Change){}
 void OnDamageRect(gfx::Rect){} };
struct OhosPlatformWindow {Adapter adapter_;Delegate d;bool anchored_=false;
 Delegate* delegate(){return &d;}
 void OnNativeSurfaceBoundsChanged(gfx::Rect,float);
};
int main(){
 OhosPlatformWindow w;
 w.OnNativeSurfaceBoundsChanged(gfx::Rect(0,0,500,450),1.5f);
 assert(w.adapter_.bounds.p.x==100 && w.adapter_.bounds.p.y==200);
 assert(w.adapter_.bounds.s.width==500 && w.adapter_.bounds.s.height==450);
 system_callback=true;
 w.OnNativeSurfaceBoundsChanged(gfx::Rect(30,293,700,800),1.5f);
 assert(w.adapter_.bounds.p.x==30 && w.adapter_.bounds.p.y==293);
 w.anchored_=true;
 w.OnNativeSurfaceBoundsChanged(gfx::Rect(0,0,300,400),1.5f);
 assert(w.adapter_.bounds.p.x==30 && w.adapter_.bounds.p.y==293);
}
'''
patch = (ROOT / 'patches/migration-155/chromium.patch').read_text()
name = 'chrome/browser/ui/views/frame/opaque_browser_frame_view.cc'
section = re.search(r'^diff --git a/' + re.escape(name) + r' .*?(?=^diff --git |\Z)',
                    patch, re.M | re.S).group()
caption_source = '\n'.join(line[1:] for line in section.splitlines()
                           if line[:1] in (' ', '+') and not line.startswith('+++'))
caption_method = extract_function(caption_source, 'bool OpaqueBrowserFrameView::ShouldShowCaptionButtons() const')
runtime = (ROOT / 'overlay/chrome/browser/ui/ohos/aura_shell_runtime_bridge.cc').read_text()
popup_policy = extract_function(runtime, 'bool SupportsAuraShellFloatingWebPopups()')
popup_policy_stubs = r'''
#include <cassert>
#include <string>
namespace base {struct AutoLock {explicit AutoLock(int){}};}
struct RuntimeBridgeState {int lock=0; std::string ui_family; bool free_window_mode=false;};
RuntimeBridgeState state;
RuntimeBridgeState& GetState(){return state;}
int main(){
 for(bool mode:{false,true}){
  state.free_window_mode=mode;
  state.ui_family="mobile_phone"; assert(!SupportsAuraShellFloatingWebPopups());
  state.ui_family="aura_tablet"; assert(SupportsAuraShellFloatingWebPopups()==mode);
  state.ui_family="aura_pc"; assert(SupportsAuraShellFloatingWebPopups());
  state.ui_family="unknown"; assert(!SupportsAuraShellFloatingWebPopups());
 }
 // Changing the system setting affects the next window.open without restart.
 state.ui_family="aura_tablet";
 state.free_window_mode=true; assert(SupportsAuraShellFloatingWebPopups());
 state.free_window_mode=false; assert(!SupportsAuraShellFloatingWebPopups());
}
'''
caption_stubs = r'''
#include <cassert>
#define IS_OHOS 1
#define BUILDFLAG(x) x
struct BrowserWindowInterface {enum class Type {TYPE_NORMAL,TYPE_POPUP,TYPE_APP_POPUP};
 Type type;Type GetType(){return type;}};
struct View {BrowserWindowInterface b;auto* browser(){return &b;}};
struct OpaqueBrowserFrameView {mutable View v;auto* GetBrowserView()const{return &v;}
 bool ShouldShowCaptionButtons()const;};
int main(){
 OpaqueBrowserFrameView v;
 v.v.b.type=BrowserWindowInterface::Type::TYPE_POPUP;assert(v.ShouldShowCaptionButtons());
 v.v.b.type=BrowserWindowInterface::Type::TYPE_NORMAL;assert(!v.ShouldShowCaptionButtons());
 v.v.b.type=BrowserWindowInterface::Type::TYPE_APP_POPUP;assert(!v.ShouldShowCaptionButtons());
}
'''
stubs = r'''
#include <algorithm>
#include <cassert>
#include <optional>
enum { HTCAPTION=2, HTLEFT=10, HTRIGHT=11, HTTOP=12, HTTOPLEFT=13,
       HTTOPRIGHT=14, HTBOTTOM=15, HTBOTTOMLEFT=16, HTBOTTOMRIGHT=17 };
namespace gfx {
struct Vector2d { int a,b; int x() const{return a;} int y()const{return b;} };
struct Point { int a=0,b=0; Vector2d operator-(Point p)const{return {a-p.a,b-p.b};} };
struct Size { int a=0,b=0; Size()=default; Size(int x,int y):a(x),b(y){}
 int width()const{return a;} int height()const{return b;} };
struct Rect { int a=0,b=0,c=0,d=0; Rect()=default;
 Rect(int x,int y,int w,int h):a(x),b(y),c(w),d(h){}
 explicit Rect(Size s):c(s.a),d(s.b){}
 int x()const{return a;} int y()const{return b;}
 int width()const{return c;} int height()const{return d;}
 int right()const{return a+c;} int bottom()const{return b+d;}
 Size size()const{return {c,d};} void Offset(Vector2d p){a+=p.a;b+=p.b;}
};
}
struct Delegate {
 int scale=1; std::optional<gfx::Size> min,max;
 auto GetMinimumSizeForWindow(){return min;}
 auto GetMaximumSizeForWindow(){return max;}
 gfx::Rect ConvertRectToPixels(gfx::Rect r){
  return {r.a*scale,r.b*scale,r.c*scale,r.d*scale};}
};
struct OhosPlatformWindow {
 gfx::Rect move_initial_bounds_{100,200,600,500}, result;
 gfx::Point move_initial_pointer_{20,30}; int move_hittest_=HTCAPTION;
 Delegate policy; Delegate* delegate(){return &policy;}
 void SetBoundsInPixels(gfx::Rect r){result=r;}
 void UpdateEmbeddedMoveBounds(const gfx::Point& pointer);
};
void expect(OhosPlatformWindow& w,int x,int y,int width,int height){
 assert(w.result.a==x && w.result.b==y && w.result.c==width && w.result.d==height);
}
int main(){
 OhosPlatformWindow w;
 w.UpdateEmbeddedMoveBounds({70,10}); expect(w,150,180,600,500);
 w.move_hittest_=HTLEFT; w.UpdateEmbeddedMoveBounds({120,30}); expect(w,200,200,500,500);
 w.policy.min=gfx::Size(200,150);
 w.UpdateEmbeddedMoveBounds({1020,30}); expect(w,500,200,200,500);
 w.move_hittest_=HTRIGHT; w.policy.max=gfx::Size(800,700);
 w.UpdateEmbeddedMoveBounds({1020,30}); expect(w,100,200,800,500);
 w.move_hittest_=HTTOPLEFT; w.policy.scale=2;
 w.UpdateEmbeddedMoveBounds({1020,1030}); expect(w,300,400,400,300);
 w.move_hittest_=HTBOTTOMRIGHT; w.policy.scale=1;
 w.UpdateEmbeddedMoveBounds({220,230}); expect(w,100,200,800,700);
 w.move_hittest_=HTTOP; w.policy.min=gfx::Size(1000,150);
 w.UpdateEmbeddedMoveBounds({20,130}); expect(w,100,300,600,400);
}
'''

layout = (ROOT / 'overlay/chromium-ui/engine/src/main/ets/utils/AuxiliaryWindowLayout.ets').read_text()
# This module is plain typed JavaScript, without ArkUI decorators. Strip only
# its explicit type declarations so the production functions run on hosted CI
# without the proprietary SDK or a package download.
layout = re.sub(r'^import .*?;\s*', '', layout, flags=re.M)
layout = re.sub(r'export interface \w+\s*\{[^}]*\}', '', layout)
layout = re.sub(r'^export type .*?;\s*', '', layout, flags=re.M)
layout = re.sub(r'(\w+)\?\s*:', r'\1:', layout)
layout = re.sub(r':\s*(?:AuxiliaryWindowState|AuxiliaryWindowView|ScreenSizeVp|PxToVp|number|boolean)\b', '', layout)
layout = re.sub(r'\bexport\s+', '', layout).replace('previous!', 'previous')
js_cases = r'''
const assert=require('node:assert/strict');
const screen={width:1200,height:800};
const state={widget:2,windowRole:'browser',anchored:false,x:150,y:120,width:900,height:975};
const a=layoutAuxiliaryWindow(state,false,screen,p=>p/1.5);
assert.equal(a.widthVp,600); assert.equal(a.heightVp,640);
assert.equal(a.leftVp,100); assert.equal(a.topVp,80);
const embedded=layoutAuxiliaryWindow({...state,windowRole:'auxiliary',browserPopup:true},false,screen,p=>p/1.5);
assert.equal(embedded.widthVp,600); assert.equal(embedded.heightVp,640);
assert.equal(embedded.leftVp,100); assert.equal(embedded.topVp,80);
const moved=layoutAuxiliaryWindow({...state,x:300,y:225},false,screen,p=>p/1.5,a);
assert.equal(moved.leftVp,200); assert.equal(moved.topVp,150);
const clamped=layoutAuxiliaryWindow({...state,x:9999,y:9999},false,screen,p=>p/1.5);
assert.equal(clamped.leftVp,600); assert.equal(clamped.topVp,160);
const bar=layoutAuxiliaryWindow({...state,windowRole:'auxiliary'},false,screen,p=>p/1.5);
assert.equal(bar.heightVp,240);
const modal=layoutAuxiliaryWindow({...state,windowRole:'auxiliary'},true,screen,p=>p/1.5);
assert.equal(modal.heightVp,640);
const menu=layoutAuxiliaryWindow({...state,windowRole:'auxiliary',anchored:true},false,screen,p=>p/1.5);
assert.equal(menu.heightVp,650); assert.equal(menu.topVp,80);
'''

with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'test.cc').write_text(stubs.replace('int main(){', method + '\nint main(){'))
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    str(path / 'test.cc'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
    for name, body, cases in [('origin', origin_method, origin_stubs),
                              ('caption', caption_method, caption_stubs),
                              ('popup_policy', popup_policy, popup_policy_stubs)]:
        code = cases.replace('int main(){', body + '\nint main(){')
        (path / (name + '.cc')).write_text(code)
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-Wall', '-Wextra', '-Werror',
                        str(path / (name + '.cc')), '-o', str(path / name)], check=True)
        subprocess.run([str(path / name)], check=True)
    (path / 'layout.cjs').write_text(layout + '\n' + js_cases)
    subprocess.run(['node', str(path / 'layout.cjs')], check=True)
print('PASS: embedded popup drag/resize, density constraints, browser geometry and dialog/bar isolation')
