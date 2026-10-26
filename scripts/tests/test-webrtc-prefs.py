#!/usr/bin/env python3
"""Compile production WebRTC preference bridge with controlled API boundaries.

Tests validation, policy/registration guards and preservation of URL rules.
Does not validate real ICE traffic, Blink internals or Chromium JSON parsing.
"""
from pathlib import Path
import re
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[2]
source = (ROOT/'overlay/chrome/browser/ui/ohos/shell_privacy_prefs_ohos.h').read_text()
header = (ROOT/'overlay/chrome/browser/ui/ohos/shell_webrtc_prefs_ohos.h').read_text()
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
#include <initializer_list>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>
namespace base {
struct Value {
 std::optional<std::string> text;
 Value()=default;explicit Value(std::string value):text(std::move(value)){}
 bool is_string()const{return text.has_value();}
 const std::string& GetString()const{return *text;}
};
struct DictValue {
 std::map<std::string,std::string> strings;std::map<std::string,bool> booleans;
 void Set(std::string key,std::string value){strings[key]=value;}
 void Set(std::string key,const char* value){strings[key]=value;}
 void Set(std::string key,bool value){booleans[key]=value;}
 const std::string* FindString(const std::string& key)const{
  auto it=strings.find(key);return it==strings.end()?nullptr:&it->second;
 }
};
constexpr int JSON_PARSE_RFC=0;
std::optional<DictValue> next_json;
DictValue last_serialized;
struct JSONReader {static std::optional<DictValue> ReadDict(const std::string&,int){return next_json;}};
std::optional<std::string> WriteJson(const DictValue& value){last_serialized=value;return "wire-json";}
}
namespace prefs {
constexpr char kWebRTCIPHandlingPolicy[]="rtc", kWebRTCIPHandlingUrl[]="urls";
}
struct PrefService {
 std::map<std::string,std::string> values={{"rtc","default"},{"mdns","unchanged"}};
 std::map<std::string,std::vector<int>> lists={{"urls",{1,2}}};
 std::set<std::string> locked;
 int writes=0;
 bool FindPreference(const std::string& key)const{return values.contains(key)||lists.contains(key);}
 bool IsUserModifiablePreference(const std::string& key)const{return !locked.contains(key);}
 const std::string& GetString(const std::string& key)const{return values.at(key);}
 const std::vector<int>& GetList(const std::string& key)const{return lists.at(key);}
 void SetString(const std::string& key,std::string value){values[key]=std::move(value);++writes;}
};
struct Profile {PrefService state;PrefService* GetPrefs(){return &state;}};
namespace blink {
constexpr char kWebRTCIPHandlingDefault[]="default";
constexpr char kWebRTCIPHandlingDefaultPublicAndPrivateInterfaces[]="default_public_and_private_interfaces";
constexpr char kWebRTCIPHandlingDefaultPublicInterfaceOnly[]="default_public_interface_only";
constexpr char kWebRTCIPHandlingDisableNonProxiedUdp[]="disable_non_proxied_udp";
// A controlled Blink conversion boundary. Production uses Blink's own API.
const char* ToWebRTCIPHandlingPolicy(const std::string& value){
 for(const char* mode:{kWebRTCIPHandlingDefaultPublicAndPrivateInterfaces,
   kWebRTCIPHandlingDefaultPublicInterfaceOnly,kWebRTCIPHandlingDisableNonProxiedUdp}){
  if(value==mode)return mode;
 }return kWebRTCIPHandlingDefault;
}
const char* ToString(const char* value){return value;}
}
'''
checks = r'''
int main(){
 using namespace chrome::ohos::settings_internal;
 Profile profile;auto& state=profile.state;
 base::Value input(std::string("wire-json"));
 auto mode=[](std::string value){base::DictValue d;d.Set("mode",std::move(value));base::next_json=d;};
 assert(ReadWebRtcIpHandling(&profile));assert(state.writes==0);
 assert(base::last_serialized.strings.at("mode")=="default");
 assert(base::last_serialized.booleans.at("hasUrlOverrides"));
 auto overrides=state.lists;
 for(const char* value:{blink::kWebRTCIPHandlingDefault,
  blink::kWebRTCIPHandlingDefaultPublicAndPrivateInterfaces,
  blink::kWebRTCIPHandlingDefaultPublicInterfaceOnly,blink::kWebRTCIPHandlingDisableNonProxiedUdp}){
  mode(value);assert(WriteWebRtcIpHandling(&profile,input));assert(ReadWebRtcIpHandling(&profile));
  assert(base::last_serialized.strings.at("mode")==value);assert(state.values.at("rtc")==value);
  assert(state.lists==overrides);assert(state.values.at("mdns")=="unchanged");
 }
 int writes=state.writes;mode("default");state.locked.insert("rtc");
 assert(ReadWebRtcIpHandling(&profile));assert(base::last_serialized.booleans.at("managed"));
 assert(!WriteWebRtcIpHandling(&profile,input));assert(state.writes==writes);
 state.locked.clear();
 for(auto value:{"","unknown","off","relay","disable_udp"}){
  mode(value);assert(!WriteWebRtcIpHandling(&profile,input));
 }
 base::next_json.reset();assert(!WriteWebRtcIpHandling(&profile,input));
 base::next_json=base::DictValue{};assert(!WriteWebRtcIpHandling(&profile,input));
 assert(!WriteWebRtcIpHandling(&profile,base::Value()));
 assert(!WriteWebRtcIpHandling(&profile,base::Value(std::string(1025,'x'))));
 assert(state.writes==writes);
 // Normalize old invalid values exactly at the renderer conversion boundary,
 // without silently rewriting the persisted preference while reading.
 for(auto raw:{"","old_unknown"}){
  state.values["rtc"]=raw;assert(ReadWebRtcIpHandling(&profile));
  assert(base::last_serialized.strings.at("mode")=="default");assert(state.values.at("rtc")==raw);
 }
 state.lists["urls"].clear();assert(ReadWebRtcIpHandling(&profile));
 assert(!base::last_serialized.booleans.at("hasUrlOverrides"));
 state.lists.erase("urls");assert(ReadWebRtcIpHandling(&profile));
 assert(!base::last_serialized.booleans.at("hasUrlOverrides"));
 state.values.erase("rtc");mode("default");assert(!ReadWebRtcIpHandling(&profile));
 assert(!WriteWebRtcIpHandling(&profile,input));assert(state.writes==writes);
}
'''
helpers='namespace chrome::ohos::settings_internal {\n'+function('inline std::optional<base::Value> PrivacyConfig(')+'\n'+function('inline std::optional<std::string> PrivacyMode(')+'\n}\n'
header=re.sub(r'^#include .*$', '', header, flags=re.M)
with tempfile.TemporaryDirectory(prefix='webrtc-prefs-') as folder:
    cpp,binary=Path(folder)/'check.cc',Path(folder)/'check'
    cpp.write_text(stubs+helpers+header+checks)
    subprocess.run(['clang++','-std=c++20','-Wall','-Wextra','-Werror',str(cpp),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('PASS: WebRTC mode validation, policy/registration guards and URL-rule preservation')
