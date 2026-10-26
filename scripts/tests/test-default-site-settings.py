#!/usr/bin/env python3
"""Compile production default-rule helpers and wire handlers with browser stubs.

Exercises choices, policy/extension locks, payload validation and legacy writes.
Does not exercise the Chromium registry implementation or actual permissions.
"""
from pathlib import Path
import re
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[2]
source = (ROOT/'overlay/chrome/browser/ui/ohos/shell_site_settings_ohos.cc').read_text()
header = (ROOT/'overlay/chrome/browser/ui/ohos/shell_default_site_settings_ohos.h').read_text()
def function(signature, body=source):
    start = body.index(signature)
    opening = body.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (body[end] == '{') - (body[end] == '}')
        end += 1
    return body[start:end]
stubs = r'''
#include <cassert>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
namespace base {
struct ListValue;
struct DictValue {
 std::map<std::string,std::string> strings;
 std::map<std::string,bool> booleans;
 std::map<std::string,std::vector<std::string>> lists;
 std::map<std::string,std::vector<DictValue>> items;
 void Set(std::string key,std::string_view value){strings[key]=value;}
 void Set(std::string key,const char* value){strings[key]=value;}
 void Set(std::string key,bool value){booleans[key]=value;}
 void Set(std::string key,ListValue value);
 const std::string* FindString(const std::string& key) const {
  auto it=strings.find(key);return it==strings.end()?nullptr:&it->second;
 }
};
struct ListValue {
 std::vector<std::string> strings;std::vector<DictValue> items;
 bool empty() const{return strings.empty() && items.empty();}
 void Append(const char* value){strings.emplace_back(value);}
 void Append(DictValue value){items.push_back(std::move(value));}
};
void DictValue::Set(std::string key,ListValue value){lists[key]=value.strings;items[key]=value.items;}
struct Value {
 std::optional<std::string> text;std::optional<bool> flag;
 Value()=default;explicit Value(std::string value):text(std::move(value)){}
 explicit Value(bool value):flag(value){}
 std::optional<bool> GetIfBool()const{return flag;}
 bool is_string() const{return text.has_value();}
 const std::string& GetString() const{return *text;}
};
constexpr int JSON_PARSE_RFC=0;
std::optional<DictValue> next_json;
DictValue last_serialized;
struct JSONReader {static std::optional<DictValue> ReadDict(const std::string&,int){return next_json;}};
std::optional<std::string> WriteJson(const DictValue& value){last_serialized=value;return "wire-json";}
}
enum ContentSetting {CONTENT_SETTING_DEFAULT,CONTENT_SETTING_ALLOW,CONTENT_SETTING_BLOCK,CONTENT_SETTING_ASK,CONTENT_SETTING_SESSION_ONLY};
enum class ContentSettingsType {IMAGES,COOKIES,GEOLOCATION,MEDIASTREAM_CAMERA,MEDIASTREAM_MIC,NOTIFICATIONS,JAVASCRIPT,
 POPUPS,SOUND,CLIPBOARD_READ_WRITE,STORAGE_ACCESS};
namespace content_settings {
enum class ProviderType {kNone,kPrefProvider,kDefaultProvider,kPolicyProvider,kExtensionProvider};
struct ContentSettingsInfo {
 std::set<ContentSetting> allowed;
 bool IsDefaultSettingValid(ContentSetting setting)const{return allowed.contains(setting);}
};
struct ContentSettingsRegistry {
 std::map<ContentSettingsType,ContentSettingsInfo> entries;
 static ContentSettingsRegistry* GetInstance(){static ContentSettingsRegistry registry;return &registry;}
 const ContentSettingsInfo* Get(ContentSettingsType type)const{
  auto it=entries.find(type);return it==entries.end()?nullptr:&it->second;
 }
};
}
struct HostContentSettingsMap {
 std::map<ContentSettingsType,ContentSetting> settings;
 std::map<ContentSettingsType,content_settings::ProviderType> sources;
 std::map<std::string,ContentSetting> exceptions={{"https://example.test",CONTENT_SETTING_ALLOW}};
 int writes=0;
 ContentSetting GetDefaultContentSetting(ContentSettingsType type,content_settings::ProviderType* source=nullptr){
  if(source)*source=sources.at(type);return settings.at(type);
 }
 void SetDefaultContentSetting(ContentSettingsType type,ContentSetting setting){settings[type]=setting;++writes;}
};
struct Profile {HostContentSettingsMap* map=nullptr;};
struct HostContentSettingsMapFactory {static HostContentSettingsMap* GetForProfile(Profile* p){return p->map;}};
struct ShellCommandContext {Profile* profile;};
HostContentSettingsMap* SettingsMap(const ShellCommandContext& context){return context.profile?context.profile->map:nullptr;}
struct Logger {template<class T>Logger& operator<<(const T&){return *this;}} logger;
#define LOG(x) logger
'''
checks = r'''
int main(){
 assert(kDefaultSettingName=="default");
 auto* registry=content_settings::ContentSettingsRegistry::GetInstance();
 HostContentSettingsMap map;Profile profile{&map};ShellCommandContext context{&profile};
 auto camera=ContentSettingsType::MEDIASTREAM_CAMERA;
 auto js=ContentSettingsType::JAVASCRIPT;
 registry->entries[camera]={{CONTENT_SETTING_BLOCK,CONTENT_SETTING_ASK}};
 registry->entries[js]={{CONTENT_SETTING_ALLOW,CONTENT_SETTING_BLOCK}};
 map.settings={{camera,CONTENT_SETTING_ASK},{js,CONTENT_SETTING_ALLOW}};
 map.sources={{camera,content_settings::ProviderType::kDefaultProvider},{js,content_settings::ProviderType::kPrefProvider}};
 auto read=ReadDefaultSiteSetting(&map,"camera",camera);
 assert(read && read->strings.at("setting")=="ask" && !read->booleans.at("managed"));
 assert((read->lists.at("choices")==std::vector<std::string>{"block","ask"}));
 assert(!ReadDefaultSiteSetting(&map,"sound",ContentSettingsType::SOUND));
 assert(ReadDefaultSiteSettings(&profile));assert(base::last_serialized.items.at("items").size()==2);
 assert(map.writes==0);
 base::Value input(std::string("wire-json"));
 auto payload=[](std::string type,std::string value){base::DictValue d;d.Set("type",type);d.Set("setting",value);base::next_json=d;return d;};
 payload("camera","block");assert(WriteDefaultSiteSettings(&profile,input));assert(map.writes==1);
 assert(map.settings.at(js)==CONTENT_SETTING_ALLOW);
 int writes=map.writes;
 assert((ReadDefaultSetting<ContentSettingsType::JAVASCRIPT,CONTENT_SETTING_ALLOW>(&profile)->GetIfBool()==true));
 assert(!(ReadDefaultSetting<ContentSettingsType::POPUPS,CONTENT_SETTING_BLOCK>(&profile)));
 map.sources[js]=content_settings::ProviderType::kPolicyProvider;
 assert(!(WriteDefaultSetting<ContentSettingsType::JAVASCRIPT,CONTENT_SETTING_ALLOW>(&profile,base::Value(false))));
 assert(map.writes==writes);
 map.sources[js]=content_settings::ProviderType::kPrefProvider;
 assert((WriteDefaultSetting<ContentSettingsType::JAVASCRIPT,CONTENT_SETTING_ALLOW>(&profile,base::Value(true))));
 writes=map.writes;
 for(auto source:{content_settings::ProviderType::kPolicyProvider,content_settings::ProviderType::kExtensionProvider,
   content_settings::ProviderType::kNone}){
  map.sources[camera]=source;payload("camera","ask");
  assert(ReadDefaultSiteSetting(&map,"camera",camera)->booleans.at("managed"));
  assert(!WriteDefaultSiteSettings(&profile,input));
  auto command=payload("camera","ask");SetDefaultSiteSetting(context,command);
  assert(map.writes==writes);
 }
 map.sources[camera]=content_settings::ProviderType::kPrefProvider;
 for(auto [type,setting]:std::vector<std::pair<std::string,std::string>>{
  {"camera","allow"},{"camera","default"},{"javascript","ask"},{"unknown","block"},{"sound","block"},{"camera","bad"}}){
  auto command=payload(type,setting);assert(!WriteDefaultSiteSettings(&profile,input));
  SetDefaultSiteSetting(context,command);assert(map.writes==writes);
 }
 payload("javascript","block");assert(WriteDefaultSiteSettings(&profile,input));
 assert(map.settings.at(camera)==CONTENT_SETTING_BLOCK);
 auto command=payload("camera","ask");SetDefaultSiteSetting(context,command);
 assert(map.settings.at(camera)==CONTENT_SETTING_ASK);
 writes=map.writes;
 assert(!WriteDefaultSiteSettings(nullptr,input));assert(!ReadDefaultSiteSettings(nullptr));
 Profile missing;assert(!ReadDefaultSiteSettings(&missing));assert(!WriteDefaultSiteSettings(&missing,input));
 assert(!WriteDefaultSiteSettings(&profile,base::Value()));
 assert(!WriteDefaultSiteSettings(&profile,base::Value(std::string(1025,'x'))));
 base::next_json.reset();assert(!WriteDefaultSiteSettings(&profile,input));
 base::next_json=base::DictValue{};assert(!WriteDefaultSiteSettings(&profile,input));
 assert(map.writes==writes);
 auto cookies=ContentSettingsType::COOKIES;
 assert(!ReadCookieStorage(&profile));
 auto cookie_payload=[](std::string value){base::DictValue d;d.Set("mode",std::move(value));base::next_json=d;};
 cookie_payload("allow");assert(!WriteCookieStorage(&profile,input));
 registry->entries[cookies]={{CONTENT_SETTING_ALLOW,CONTENT_SETTING_BLOCK,CONTENT_SETTING_SESSION_ONLY}};
 map.settings[cookies]=CONTENT_SETTING_ALLOW;map.sources[cookies]=content_settings::ProviderType::kDefaultProvider;
 const auto exceptions=map.exceptions;
 const auto js_before=map.settings.at(js),camera_before=map.settings.at(camera);
 for(auto mode:{"allow","session_only","block"}){
  cookie_payload(mode);assert(WriteCookieStorage(&profile,input));
  assert(ReadCookieStorage(&profile));assert(base::last_serialized.strings.at("mode")==mode);
  assert(map.exceptions==exceptions);assert(map.settings.at(js)==js_before);
  assert(map.settings.at(camera)==camera_before);
 }
 writes=map.writes;
 for(auto provider:{content_settings::ProviderType::kPolicyProvider,content_settings::ProviderType::kExtensionProvider}){
  map.sources[cookies]=provider;assert(ReadCookieStorage(&profile));
  assert(base::last_serialized.booleans.at("managed"));
  cookie_payload("allow");assert(!WriteCookieStorage(&profile,input));assert(map.writes==writes);
 }
 map.sources[cookies]=content_settings::ProviderType::kPrefProvider;
 for(auto mode:{"ask","default","on","invalid"}){cookie_payload(mode);assert(!WriteCookieStorage(&profile,input));}
 assert(!WriteCookieStorage(&profile,base::Value(true)));
 assert(!WriteCookieStorage(&profile,base::Value(std::string(1025,'x'))));
 base::next_json.reset();assert(!WriteCookieStorage(&profile,input));
 base::next_json=base::DictValue{};assert(!WriteCookieStorage(&profile,input));
 Profile missing_cookie_map;assert(!ReadCookieStorage(&missing_cookie_map));
 cookie_payload("allow");assert(!WriteCookieStorage(&missing_cookie_map,input));
 assert(map.writes==writes);
 // Exercise the production image reader/writer, preserving other defaults.
 auto images=ContentSettingsType::IMAGES;
 assert(!ReadImageLoading(&profile));
 cookie_payload("allow");assert(!WriteImageLoading(&profile,input));
 registry->entries[images]={{CONTENT_SETTING_ALLOW,CONTENT_SETTING_BLOCK}};
 map.settings[images]=CONTENT_SETTING_ALLOW;
 map.sources[images]=content_settings::ProviderType::kDefaultProvider;
 const auto cookie_before=map.settings.at(cookies);
 for(auto mode:{"allow","block"}){
  cookie_payload(mode);assert(WriteImageLoading(&profile,input));
  assert(ReadImageLoading(&profile));assert(base::last_serialized.strings.at("mode")==mode);
  assert(!base::last_serialized.booleans.at("managed"));
  assert(map.exceptions==exceptions);assert(map.settings.at(cookies)==cookie_before);
  assert(map.settings.at(js)==js_before);assert(map.settings.at(camera)==camera_before);
 }
 writes=map.writes;
 for(auto provider:{content_settings::ProviderType::kPolicyProvider,content_settings::ProviderType::kExtensionProvider}){
  map.sources[images]=provider;assert(ReadImageLoading(&profile));
  assert(base::last_serialized.booleans.at("managed"));
  cookie_payload("allow");assert(!WriteImageLoading(&profile,input));assert(map.writes==writes);
 }
 map.sources[images]=content_settings::ProviderType::kPrefProvider;
 for(auto mode:{"ask","session_only","default","on","invalid"}){
  cookie_payload(mode);assert(!WriteImageLoading(&profile,input));
 }
 assert(!WriteImageLoading(&profile,base::Value(true)));
 assert(!WriteImageLoading(&profile,base::Value(std::string(1025,'x'))));
 base::next_json.reset();assert(!WriteImageLoading(&profile,input));
 base::next_json=base::DictValue{};assert(!WriteImageLoading(&profile,input));
 assert(!ReadImageLoading(&missing_cookie_map));
 cookie_payload("allow");assert(!WriteImageLoading(&missing_cookie_map,input));
 assert(map.writes==writes);
}
'''
# Keep production type whitelist and shell-value conversion, too.
types = source[source.index('struct SiteSettingType {'):source.index('// The command\'s "origin"')]
header = re.sub(r'^#include .*$', '', header, flags=re.M)
handlers = '\n'.join(function(sig) for sig in ['std::optional<base::Value> ReadDefaultSiteSettings(',
    'bool WriteDefaultSiteSettings(', 'void SetDefaultSiteSetting('])
prefs_source=(ROOT/'overlay/chrome/browser/ui/ohos/shell_prefs_ohos.cc').read_text()
legacy=prefs_source[prefs_source.index('template <ContentSettingsType kType, ContentSetting kOnSetting>'):
    prefs_source.index('std::optional<base::Value> ReadTextScale(')]
privacy_source=(ROOT/'overlay/chrome/browser/ui/ohos/shell_privacy_prefs_ohos.h').read_text()
helpers='namespace chrome::ohos::settings_internal {\n'+function('inline std::optional<base::Value> PrivacyConfig(',privacy_source)+'\n'+function('inline std::optional<std::string> PrivacyMode(',privacy_source)+'\n}\n'
cookie_header=(ROOT/'overlay/chrome/browser/ui/ohos/shell_cookie_storage_ohos.h').read_text()
cookie_header=re.sub(r'^#include .*$', '', cookie_header, flags=re.M)
image_header=(ROOT/'overlay/chrome/browser/ui/ohos/shell_image_loading_ohos.h').read_text()
image_header=re.sub(r'^#include .*$', '', image_header, flags=re.M)
with tempfile.TemporaryDirectory(prefix='default-sites-') as folder:
    cpp,binary=Path(folder)/'check.cc',Path(folder)/'check'
    cpp.write_text(stubs+header+helpers+cookie_header+image_header+'\nusing namespace chrome::ohos::settings_internal;\n'+types+handlers+legacy+checks)
    subprocess.run(['clang++','-std=c++20','-Wall','-Wextra','-Werror',str(cpp),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('PASS: default/Cookie/image rules, provider locks, session mode, wire and legacy guards')
