#!/usr/bin/env python3
"""Compile production privacy guards against controlled Chromium boundaries.

Verifies registration/policy/type validation and service delegation, not actual
Chromium TLS, CookieSettings enforcement, preloading execution or JSON parsing.
"""
from pathlib import Path
import re
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / 'overlay/chrome/browser/ui/ohos/shell_privacy_prefs_ohos.h').read_text()
STUBS = r'''
#include <cassert>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
namespace base {
struct Value {
 std::optional<std::string> text;
 std::optional<bool> flag;
 Value() = default;
 explicit Value(std::string value):text(std::move(value)){}
 explicit Value(bool value):flag(value){}
 std::optional<bool> GetIfBool() const{return flag;}
 bool is_string() const{return text.has_value();}
 const std::string& GetString() const{return *text;}
};
struct DictValue {
 std::map<std::string,std::string> strings;
 std::map<std::string,bool> booleans;
 void Set(std::string key,std::string value){strings[key]=value;}
 void Set(std::string key,const char* value){strings[key]=value;}
 void Set(std::string key,bool value){booleans[key]=value;}
 const std::string* FindString(const std::string& key) const{
  auto i=strings.find(key);return i==strings.end()?nullptr:&i->second;
 }
};
constexpr int JSON_PARSE_RFC=0;
std::optional<DictValue> next_json;
DictValue last_serialized;
struct JSONReader {static std::optional<DictValue> ReadDict(const std::string&,int){return next_json;}};
std::optional<std::string> WriteJson(const DictValue& value){last_serialized=value;return "wire-json";}
}
namespace prefs {
constexpr char kCookieControlsMode[]="cookies", kHttpsOnlyModeEnabled[]="strict",
 kHttpsFirstBalancedMode[]="balanced";
}
struct PrefService {
 std::map<std::string,int> values={{"cookies",0},{"strict",0},{"balanced",0},{"prediction",1},{"address",1},{"cards",1}};
 std::set<std::string> locked;
 int writes=0;
 bool FindPreference(const std::string& key) const{return values.contains(key);}
 bool IsUserModifiablePreference(const std::string& key) const{return !locked.contains(key);}
 int GetInteger(const std::string& key) const{return values.at(key);}
 bool GetBoolean(const std::string& key) const{return values.at(key)!=0;}
 void SetBoolean(const std::string& key,bool value){values[key]=value;++writes;}
 void SetInteger(const std::string& key,int value){values[key]=value;++writes;}
};
struct Profile {PrefService state;PrefService* GetPrefs(){return &state;}};
namespace prefetch {
namespace prefs {constexpr char kNetworkPredictionOptions[]="prediction";}
enum class PreloadPagesState {kNoPreloading,kStandardPreloading,kExtendedPreloading};
int preload_updates=0;
PreloadPagesState GetPreloadPagesState(const PrefService& state){
 return static_cast<PreloadPagesState>(state.GetInteger(prefs::kNetworkPredictionOptions));
}
void SetPreloadPagesState(PrefService* state,PreloadPagesState value){
 ++preload_updates;state->SetInteger(prefs::kNetworkPredictionOptions,static_cast<int>(value));
}
}
namespace autofill::prefs {
constexpr char kAutofillProfileEnabled[]="address",kAutofillCreditCardEnabled[]="cards";
bool IsAutofillProfileEnabled(const PrefService* state){return state->GetBoolean(kAutofillProfileEnabled);}
bool IsAutofillPaymentMethodsEnabled(const PrefService* state){return state->GetBoolean(kAutofillCreditCardEnabled);}
void SetAutofillProfileEnabled(PrefService* state,bool value){state->SetBoolean(kAutofillProfileEnabled,value);}
void SetAutofillPaymentMethodsEnabled(PrefService* state,bool value){state->SetBoolean(kAutofillCreditCardEnabled,value);}
}
namespace content_settings {enum class CookieControlsMode {kOff=0,kBlockThirdParty=1,kIncognitoOnly=2};}
namespace safe_browsing {
struct Manager {bool protected_user=false;bool IsUnderAdvancedProtection(){return protected_user;}};
Manager protection;
struct AdvancedProtectionStatusManagerFactory {static Manager* GetForProfile(Profile*){return &protection;}};
}
bool balanced_available=true;
bool IsBalancedModeAvailable(){return balanced_available;}
enum class HttpsFirstModeSetting {kDisabled,kEnabledFull,kEnabledBalanced};
struct HttpsFirstModeService {
 HttpsFirstModeSetting current=HttpsFirstModeSetting::kDisabled;
 int updates=0;
 bool reject=false;
 HttpsFirstModeSetting GetCurrentSetting(){return current;}
 bool UpdatePrefs(HttpsFirstModeSetting selection){++updates;if(reject)return false;current=selection;return true;}
};
HttpsFirstModeService service;
bool service_available=true;
struct HttpsFirstModeServiceFactory {
 static HttpsFirstModeService* GetForProfile(Profile*){return service_available?&service:nullptr;}
};
'''
CHECKS = r'''
int main(){
 using namespace chrome::ohos::settings_internal;
 Profile profile;
 base::Value input(std::string("wire-json"));
 auto mode=[](std::string value){base::DictValue d;d.Set("mode",std::move(value));base::next_json=d;};
 // Independent opt-ins: reading cannot write, policy applies per preference,
 // bad payloads and missing registration must not touch either setting.
 for(bool payments:{false,true}){
  Profile autofill_profile;
  auto read=payments?&ReadPaymentAutofill:&ReadAddressAutofill;
  auto write=payments?&WritePaymentAutofill:&WriteAddressAutofill;
  const char* key=payments?"cards":"address";
  const char* other=payments?"address":"cards";
  assert(read(&autofill_profile));assert(base::last_serialized.strings.at("mode")=="on");
  assert(!base::last_serialized.booleans.at("managed"));assert(autofill_profile.state.writes==0);
  for(auto value:{"off","on"}){
   mode(value);assert(write(&autofill_profile,input));assert(read(&autofill_profile));
   assert(base::last_serialized.strings.at("mode")==value);
   assert(autofill_profile.state.GetBoolean(other));
  }
  int writes=autofill_profile.state.writes;
  for(auto value:{"standard","strict","true","invalid"}){
   mode(value);assert(!write(&autofill_profile,input));
  }
  base::next_json.reset();assert(!write(&autofill_profile,input));
  base::next_json=base::DictValue{};assert(!write(&autofill_profile,input));
  assert(!write(&autofill_profile,base::Value(true)));
  assert(!write(&autofill_profile,base::Value(std::string(1025,'x'))));
  autofill_profile.state.locked.insert(key);mode("off");
  assert(read(&autofill_profile));assert(base::last_serialized.booleans.at("managed"));
  assert(!write(&autofill_profile,input));
  autofill_profile.state.locked.clear();autofill_profile.state.locked.insert(other);
  assert(read(&autofill_profile));assert(!base::last_serialized.booleans.at("managed"));
  autofill_profile.state.values.erase(key);
  assert(!read(&autofill_profile));assert(!write(&autofill_profile,input));
  assert(autofill_profile.state.writes==writes);
 }
 // Reading never writes or resets an existing extended choice.
 profile.state.values["prediction"]=2;
 assert(ReadPreloading(&profile));assert(base::last_serialized.strings.at("mode")=="extended");
 assert(prefetch::preload_updates==0);
 assert(ReadPreloadPages(&profile)->GetIfBool()==true);
 assert(WritePreloadPages(&profile,base::Value(true)));
 assert(prefetch::preload_updates==0);
 assert(profile.state.values.at("prediction")==2);
 for(auto value:{"off","standard","extended"}){
  mode(value);assert(WritePreloading(&profile,input));assert(ReadPreloading(&profile));
  assert(base::last_serialized.strings.at("mode")==value);
 }
 assert(WritePreloadPages(&profile,base::Value(false)));
 assert(ReadPreloadPages(&profile)->GetIfBool()==false);
 assert(WritePreloadPages(&profile,base::Value(true)));
 assert(ReadPreloading(&profile));assert(base::last_serialized.strings.at("mode")=="standard");
 int preload_updates=prefetch::preload_updates;
 profile.state.locked.insert("prediction");mode("off");
 assert(ReadPreloading(&profile));assert(base::last_serialized.booleans.at("managed"));
 assert(!WritePreloading(&profile,input));
 assert(!WritePreloadPages(&profile,base::Value(false)));
 assert(!WritePreloadPages(&profile,base::Value(true)));
 profile.state.locked.clear();
 for(auto value:{"invalid","strict","block"}){mode(value);assert(!WritePreloading(&profile,input));}
 assert(!WritePreloading(&profile,base::Value()));
 assert(!WritePreloading(&profile,base::Value(std::string(1025,'x'))));
 assert(!WritePreloadPages(&profile,input));
 profile.state.values.erase("prediction");mode("standard");
 assert(!ReadPreloading(&profile));assert(!ReadPreloadPages(&profile));
 assert(!WritePreloading(&profile,input));assert(!WritePreloadPages(&profile,base::Value(true)));
 assert(prefetch::preload_updates==preload_updates);
 assert(ReadThirdPartyCookies(&profile));
 assert(base::last_serialized.strings.at("mode")=="incognito");
 for(auto value:{"block","incognito"}){
  mode(value);assert(WriteThirdPartyCookies(&profile,input));
  assert(ReadThirdPartyCookies(&profile));assert(base::last_serialized.strings.at("mode")==value);
 }
 int writes=profile.state.writes;
 mode("block");profile.state.locked.insert("cookies");
 assert(ReadThirdPartyCookies(&profile));assert(base::last_serialized.booleans.at("managed"));
 assert(!WriteThirdPartyCookies(&profile,input));assert(profile.state.writes==writes);
 profile.state.locked.clear();profile.state.values.erase("cookies");
 assert(!ReadThirdPartyCookies(&profile));assert(!WriteThirdPartyCookies(&profile,input));
 profile.state.values["cookies"]=0;
 for(auto value:{"off","invalid","strict"}){mode(value);assert(!WriteThirdPartyCookies(&profile,input));}
 assert(profile.state.writes==writes);
 for(auto value:{"off","balanced","strict"}){
  mode(value);assert(WriteHttpsFirst(&profile,input));assert(ReadHttpsFirst(&profile));
  assert(base::last_serialized.strings.at("mode")==value);
 }
 int updates=service.updates;
 for(auto key:{"strict","balanced"}){
  profile.state.locked.insert(key);mode("off");assert(!WriteHttpsFirst(&profile,input));
  assert(ReadHttpsFirst(&profile));assert(base::last_serialized.booleans.at("managed"));
  profile.state.locked.clear();
 }
 safe_browsing::protection.protected_user=true;
 assert(!WriteHttpsFirst(&profile,input));safe_browsing::protection.protected_user=false;
 balanced_available=false;mode("balanced");assert(!WriteHttpsFirst(&profile,input));
 assert(ReadHttpsFirst(&profile));assert(!base::last_serialized.booleans.at("balancedAvailable"));
 mode("invalid");assert(!WriteHttpsFirst(&profile,input));assert(service.updates==updates);
 for(auto key:{"strict","balanced"}){
  profile.state.values.erase(key);mode("off");assert(!WriteHttpsFirst(&profile,input));
  assert(!ReadHttpsFirst(&profile));profile.state.values[key]=0;
 }
 service_available=false;assert(!ReadHttpsFirst(&profile));assert(!WriteHttpsFirst(&profile,input));
 service_available=true;service.reject=true;assert(!WriteHttpsFirst(&profile,input));
 base::next_json.reset();assert(!WriteHttpsFirst(&profile,input));assert(!WriteThirdPartyCookies(&profile,input));
 base::next_json=base::DictValue{};assert(!WriteHttpsFirst(&profile,input));
 assert(!WriteHttpsFirst(&profile,base::Value()));
 assert(!WriteThirdPartyCookies(&profile,base::Value(std::string(1025,'x'))));
}
'''
with tempfile.TemporaryDirectory(prefix='privacy-prefs-') as folder:
    cpp, binary = Path(folder)/'check.cc', Path(folder)/'check'
    source = re.sub(r'^#include .*$', '', SOURCE, flags=re.M)
    cpp.write_text(STUBS + source + CHECKS)
    subprocess.run(['clang++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('PASS: privacy/autofill policy guards, independent opt-ins and service delegation')
