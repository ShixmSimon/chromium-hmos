#!/usr/bin/env python3
"""Execute production page-info connection and document guards with browser stubs."""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'overlay/chrome/browser/ui/ohos/shell_site_settings_ohos.cc').read_text()
def function(signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

stubs = r'''
#include <cassert>
#include <map>
#include <string>
#include <string_view>
namespace base {
std::string NumberToString(int n) { return std::to_string(n); }
struct DictValue {
 std::map<std::string, std::string> values;
 const std::string* FindString(const char* key) const {
  auto it = values.find(key); return it == values.end() ? nullptr : &it->second;
 }
};
}
struct GURL {
 std::string scheme = "https";
 bool SchemeIs(std::string_view s) const { return scheme == s; }
 bool SchemeIsHTTPOrHTTPS() const { return scheme == "http" || scheme == "https"; }
};
namespace content {
struct SSLStatus { static constexpr int NORMAL_CONTENT=0; bool initialized=false,certificate=false; int cert_status=0,content_status=0; };
struct Entry {GURL address; int id=5; const GURL& GetURL() const { return address; } int GetUniqueID() const {return id;}};
struct Controller {Entry* entry=nullptr; Entry* GetLastCommittedEntry() { return entry; }};
struct WebContents {
 Controller controller; void* profile=nullptr; int tab=2;
 Controller& GetController() {return controller;} void* GetBrowserContext(){return profile;}
};
}
namespace net { bool IsCertStatusError(int status) {return status != 0;} }
struct TabStrip { content::WebContents* active=nullptr; content::WebContents* GetActiveWebContents(){return active;} };
struct Browser {TabStrip strip; TabStrip* GetTabStripModel(){return &strip;} };
namespace sessions {struct ID {int value; int id() const{return value;}}; struct SessionTabHelper {static ID IdForTab(content::WebContents* c){return {c->tab};}};}
struct ShellCommandContext { Browser* browser=nullptr; void* profile=nullptr; };
'''
checks = r'''
int main() {
 GURL https, http{"http"}; content::SSLStatus ssl;
 assert(PageConnection(https,ssl)=="unknown");
 assert(PageConnection(http,ssl)=="insecure");
 ssl.initialized=true; assert(PageConnection(https,ssl)=="unknown");
 ssl.certificate=true; assert(PageConnection(https,ssl)=="secure");
 ssl.content_status=1; assert(PageConnection(https,ssl)=="mixed");
 ssl.cert_status=1; assert(PageConnection(https,ssl)=="invalid");
 content::Entry entry; content::WebContents tab; tab.profile=&ssl; tab.controller.entry=&entry;
 Browser browser; browser.strip.active=&tab; ShellCommandContext context{&browser,&ssl};
 base::DictValue command; command.values={{"tabId","2"},{"navigationId","5"}};
 assert(PageForInfo(context,command,true)==&tab);
 command.values["navigationId"]="4"; assert(PageForInfo(context,command,true)==nullptr);
 assert(PageForInfo(context,command,false)==&tab);
 command.values["tabId"]="3"; assert(PageForInfo(context,command,false)==nullptr);
 command.values["tabId"]="2"; context.profile=&entry; assert(PageForInfo(context,command,false)==nullptr);
 context.profile=&ssl; entry.address.scheme="chrome"; assert(PageForInfo(context,command,false)==nullptr);
 entry.address.scheme="https"; tab.controller.entry=nullptr; assert(PageForInfo(context,command,false)==nullptr);
 browser.strip.active=nullptr; assert(PageForInfo(context,command,false)==nullptr);
}
'''
with tempfile.TemporaryDirectory(prefix='page-info-') as folder:
    cpp, binary = Path(folder)/'check.cc', Path(folder)/'check'
    cpp.write_text(stubs + function('std::string_view PageConnection(') + '\n' + function('content::WebContents* PageForInfo(') + checks)
    subprocess.run(['clang++','-std=c++20','-Wall','-Wextra','-Werror',str(cpp),'-o',str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('PASS: connection security, active tab, document and Profile guards')
