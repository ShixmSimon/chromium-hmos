#!/usr/bin/env python3
"""Run production DNS pref guards with controlled Chromium dependency stubs.

Checks policy rejection, all-or-nothing validation and pref notification order.
Does not validate Chromium's JSON/DoH parser, TLS or NetConn runtime behavior.
"""
from pathlib import Path
import re
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / 'overlay/chrome/browser/ui/ohos/shell_secure_dns_ohos.h').read_text()
STUBS = r'''
#include <cassert>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
namespace base {
struct Value {
 std::optional<std::string> text;
 Value() = default;
 explicit Value(std::string value):text(std::move(value)){}
 bool is_string() const {return text.has_value();}
 const std::string& GetString() const {return *text;}
};
struct DictValue {
 std::map<std::string,std::string> strings;
 std::map<std::string,bool> booleans;
 void Set(std::string key,std::string value){strings[key]=value;}
 void Set(std::string key,bool value){booleans[key]=value;}
 const std::string* FindString(const std::string& key) const {
  auto i=strings.find(key);return i==strings.end()?nullptr:&i->second;
 }
};
constexpr int JSON_PARSE_RFC=0;
// Deterministic Chromium parser boundary. Tests below exercise production
// validation/commit ordering, not reimplement JSON or DoH template parsing.
std::optional<DictValue> next_json;
struct JSONReader {static std::optional<DictValue> ReadDict(const std::string&,int){return next_json;}};
std::optional<std::string> WriteJson(const DictValue& value){return value.strings.at("mode");}
}
namespace prefs {
constexpr char kDnsOverHttpsMode[]="mode", kDnsOverHttpsTemplates[]="templates";
}
struct PrefService {
 std::map<std::string,std::string> values={{"mode","automatic"},{"templates",""}};
 std::set<std::string> locked;
 std::vector<std::pair<std::string,std::string>> writes;
 bool FindPreference(const std::string& key) const {return values.contains(key);}
 bool IsUserModifiablePreference(const std::string& key) const {return !locked.contains(key);}
 std::string GetString(const std::string& key) const {return values.at(key);}
 void SetString(const std::string& key,const std::string& value){
  values[key]=value;writes.emplace_back(key,value);
  // The downstream network observer must never see strict mode without servers.
  assert(values.at("mode")!="secure" || !values.at("templates").empty());
 }
};
struct BrowserProcess {PrefService* state=nullptr;PrefService* local_state(){return state;}};
BrowserProcess* g_browser_process=nullptr;
namespace net {
struct Server {std::string value;const std::string& server_template() const{return value;}};
struct DnsOverHttpsConfig {
 std::vector<Server> values;
 static std::optional<DnsOverHttpsConfig> FromString(const std::string& value){
  if(value=="bad-template")return std::nullopt;
  return DnsOverHttpsConfig{{{value}}};
 }
 const std::vector<Server>& servers() const{return values;}
};
}
struct GURL {
 std::string text;explicit GURL(std::string value):text(std::move(value)){}
 bool SchemeIs(std::string_view value)const{return text.starts_with(std::string(value)+"://");}
 bool has_username()const{return text.find('@')!=std::string::npos;}
 bool has_password()const{return has_username();}
 bool has_ref()const{return text.find('#')!=std::string::npos;}
};
'''
CHECKS = r'''
int main(){
 using namespace chrome::ohos::settings_internal;
 base::Value input(std::string("wire-json"));
 assert(!ReadSecureDns(nullptr));assert(!WriteSecureDns(nullptr,input));
 PrefService state;BrowserProcess browser{&state};g_browser_process=&browser;
 assert(ReadSecureDns(nullptr));
 auto config=[](std::string mode,std::string templates){
  base::DictValue d;d.Set("mode",std::move(mode));d.Set("templates",std::move(templates));base::next_json=d;
 };
 config("secure","https://dns.google/dns-query");
 for(const char* key:{"mode","templates"}){
  state.locked.insert(key);assert(!WriteSecureDns(nullptr,input));assert(state.writes.empty());state.locked.clear();
 }
 auto unchanged=state.values;
 for(auto [mode,server]:std::vector<std::pair<std::string,std::string>>{
  {"unknown","https://dns.google/dns-query"},{"secure",""},{"secure","bad-template"},
  {"secure","http://dns.google/query"},{"secure","https://u:p@dns.google/query"},
  {"secure","https://dns.google/query#fragment"},{"automatic","bad-template"}}){
  config(mode,server);assert(!WriteSecureDns(nullptr,input));assert(state.values==unchanged);assert(state.writes.empty());
 }
 base::next_json.reset();assert(!WriteSecureDns(nullptr,input));
 base::next_json=base::DictValue{};assert(!WriteSecureDns(nullptr,input));
 config("secure","https://dns.google/dns-query");
 assert(WriteSecureDns(nullptr,input));assert(state.writes.size()==2);
 assert(state.writes[0].first=="templates" && state.writes[1].first=="mode");
 state.writes.clear();config("secure","https://cloudflare-dns.com/dns-query");
 assert(WriteSecureDns(nullptr,input));assert(state.values.at("mode")=="secure");
 state.writes.clear();config("off","");assert(WriteSecureDns(nullptr,input));
 assert(state.writes[0].first=="mode" && state.writes[1].first=="templates");
 assert(state.values.at("templates").empty());
 state.writes.clear();config("secure",std::string(4097,'x'));
 assert(!WriteSecureDns(nullptr,input));assert(state.writes.empty());
 assert(!WriteSecureDns(nullptr,base::Value(std::string(8193,'x'))));
 assert(!WriteSecureDns(nullptr,base::Value()));
 state.values.erase("mode");assert(!ReadSecureDns(nullptr));assert(!WriteSecureDns(nullptr,input));
}
'''
with tempfile.TemporaryDirectory(prefix='secure-dns-') as folder:
    cpp, binary = Path(folder)/'check.cc', Path(folder)/'check'
    source = re.sub(r'^#include .*$', '', SOURCE, flags=re.M)
    cpp.write_text(STUBS + source + CHECKS)
    subprocess.run(['clang++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('PASS: native DNS validation, policy guards and strict-mode update order')
