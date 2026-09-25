// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The settings the shell's own pages show as switches and sliders. Only the
// keys in the tables below cross; each maps a shell key to the Chromium pref
// or setting behind it.
//
//   getPrefs {requestId, keys?: [key...]}   (absent keys: every known key)
//     -> "prefs" {requestId, values: [{key, value}]}
//   setPref {key, value}
//     -> broadcast "prefsChanged" {keys: [key]} once written.
//
// Keys and value types:
//   blockThirdPartyCookies  bool
//   doNotTrack              bool
//   safeBrowsing            'off' | 'standard' | 'enhanced'
//   preloadPages            bool
//   popupsBlocked           bool
//   javascriptEnabled       bool
//   textScale               number, percent, 50..200 (the default page zoom:
//                           desktop Chromium has no text-only scale)
//   autofillAddresses       bool
//   autofillCards           bool
//   downloadAskWhereToSave  bool
// Unknown keys and values of the wrong type are logged and ignored. A pref
// that policy controls is read but not written.

#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "base/logging.h"
#include "base/values.h"
#include "chrome/browser/content_settings/host_content_settings_map_factory.h"
#include "chrome/browser/preloading/preloading_prefs.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/ohos/shell_services_ohos.h"
#include "chrome/browser/ui/ohos/shell_settings_ohos_internal.h"
#include "chrome/browser/ui/zoom/chrome_zoom_level_prefs.h"
#include "chrome/common/pref_names.h"
#include "components/autofill/core/common/autofill_prefs.h"
#include "components/content_settings/core/browser/cookie_settings.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/content_settings/core/common/content_settings.h"
#include "components/content_settings/core/common/content_settings_types.h"
#include "components/content_settings/core/common/pref_names.h"
#include "components/prefs/pref_service.h"
#include "components/safe_browsing/core/common/safe_browsing_prefs.h"
#include "third_party/blink/public/common/page/page_zoom.h"

namespace chrome::ohos::settings_internal {

namespace {

// Registered in shell_password_cleanup_ohos.cc alongside the other OHOS
// password prefs, since that is where this platform's password state lives.
constexpr char kPasswordFillRequiresAuth[] = "ohos.password_fill_requires_auth";

// --- Plain boolean prefs: one line each. ----------------------------------

struct BoolPref {
  std::string_view key;
  const char* pref_name;
};

constexpr BoolPref kBoolPrefs[] = {
    {"doNotTrack", prefs::kEnableDoNotTrack},
    {"autofillAddresses", autofill::prefs::kAutofillProfileEnabled},
    {"autofillCards", autofill::prefs::kAutofillCreditCardEnabled},
    {"downloadAskWhereToSave", prefs::kPromptForDownload},
    // Whether filling a saved password into a page asks the user to prove who
    // they are first. Showing, copying, editing and exporting always ask and
    // have no setting; this covers filling alone, which is the one the
    // product wanted to be able to turn off.
    {"passwordFillRequiresAuth", kPasswordFillRequiresAuth},
};

// --- Settings that need translating. --------------------------------------

// Null when the setting cannot be read on this profile.
using Reader = std::optional<base::Value> (*)(Profile* profile);
// False when `value` has the wrong type or range, or cannot be written.
using Writer = bool (*)(Profile* profile, const base::Value& value);

// The shell's textScale bounds, in percent.
constexpr double kMinTextScalePercent = 50;
constexpr double kMaxTextScalePercent = 200;
constexpr double kPercent = 100;

std::optional<base::Value> ReadBlockThirdPartyCookies(Profile* profile) {
  const int mode = profile->GetPrefs()->GetInteger(prefs::kCookieControlsMode);
  return base::Value(
      mode ==
      static_cast<int>(content_settings::CookieControlsMode::kBlockThirdParty));
}

bool WriteBlockThirdPartyCookies(Profile* profile, const base::Value& value) {
  const std::optional<bool> block = value.GetIfBool();
  PrefService* pref_service = profile->GetPrefs();
  if (!block ||
      !pref_service->IsUserModifiablePreference(prefs::kCookieControlsMode)) {
    return false;
  }
  // "Not blocking" is written as kIncognitoOnly; CookieControlsMode documents
  // kOff as behaving the same, so either reads back as false.
  const content_settings::CookieControlsMode mode =
      *block ? content_settings::CookieControlsMode::kBlockThirdParty
             : content_settings::CookieControlsMode::kIncognitoOnly;
  pref_service->SetInteger(prefs::kCookieControlsMode, static_cast<int>(mode));
  return true;
}

struct SafeBrowsingName {
  std::string_view name;
  safe_browsing::SafeBrowsingState state;
};

constexpr SafeBrowsingName kSafeBrowsingNames[] = {
    {"off", safe_browsing::SafeBrowsingState::NO_SAFE_BROWSING},
    {"standard", safe_browsing::SafeBrowsingState::STANDARD_PROTECTION},
    {"enhanced", safe_browsing::SafeBrowsingState::ENHANCED_PROTECTION},
};

// OHOS builds with safe_browsing_mode = 0, which may leave the Safe Browsing
// prefs unregistered; reading an unregistered pref is a CHECK failure.
bool IsRegistered(const PrefService& pref_service, const char* pref_name) {
  return pref_service.FindPreference(pref_name) != nullptr;
}

std::optional<base::Value> ReadSafeBrowsing(Profile* profile) {
  if (!IsRegistered(*profile->GetPrefs(), prefs::kSafeBrowsingEnabled) ||
      !IsRegistered(*profile->GetPrefs(), prefs::kSafeBrowsingEnhanced)) {
    return std::nullopt;
  }
  const safe_browsing::SafeBrowsingState state =
      safe_browsing::GetSafeBrowsingState(*profile->GetPrefs());
  for (const SafeBrowsingName& entry : kSafeBrowsingNames) {
    if (entry.state == state) {
      return base::Value(entry.name);
    }
  }
  return std::nullopt;
}

bool WriteSafeBrowsing(Profile* profile, const base::Value& value) {
  const std::string* name = value.GetIfString();
  PrefService* pref_service = profile->GetPrefs();
  if (!name || !IsRegistered(*pref_service, prefs::kSafeBrowsingEnabled) ||
      !IsRegistered(*pref_service, prefs::kSafeBrowsingEnhanced) ||
      safe_browsing::IsSafeBrowsingPolicyManaged(*pref_service)) {
    return false;
  }
  for (const SafeBrowsingName& entry : kSafeBrowsingNames) {
    if (entry.name == *name) {
      safe_browsing::SetSafeBrowsingState(pref_service, entry.state);
      return true;
    }
  }
  return false;
}

std::optional<base::Value> ReadPreloadPages(Profile* profile) {
  return base::Value(prefetch::GetPreloadPagesState(*profile->GetPrefs()) !=
                     prefetch::PreloadPagesState::kNoPreloading);
}

bool WritePreloadPages(Profile* profile, const base::Value& value) {
  const std::optional<bool> enabled = value.GetIfBool();
  if (!enabled) {
    return false;
  }
  PrefService* pref_service = profile->GetPrefs();
  const prefetch::PreloadPagesState current =
      prefetch::GetPreloadPagesState(*pref_service);
  // The shell has one switch; turning it on keeps extended preloading if the
  // user chose that elsewhere.
  if (*enabled == (current != prefetch::PreloadPagesState::kNoPreloading)) {
    return true;
  }
  prefetch::SetPreloadPagesState(
      pref_service, *enabled ? prefetch::PreloadPagesState::kStandardPreloading
                             : prefetch::PreloadPagesState::kNoPreloading);
  return true;
}

// A boolean shell key over the default of a content setting: true means
// `on_setting`, false means the other of allow/block.
template <ContentSettingsType kType, ContentSetting kOnSetting>
std::optional<base::Value> ReadDefaultSetting(Profile* profile) {
  return base::Value(HostContentSettingsMapFactory::GetForProfile(profile)
                         ->GetDefaultContentSetting(kType) == kOnSetting);
}

template <ContentSettingsType kType, ContentSetting kOnSetting>
bool WriteDefaultSetting(Profile* profile, const base::Value& value) {
  const std::optional<bool> on = value.GetIfBool();
  if (!on) {
    return false;
  }
  constexpr ContentSetting kOffSetting = kOnSetting == CONTENT_SETTING_ALLOW
                                             ? CONTENT_SETTING_BLOCK
                                             : CONTENT_SETTING_ALLOW;
  HostContentSettingsMapFactory::GetForProfile(profile)
      ->SetDefaultContentSetting(kType, *on ? kOnSetting : kOffSetting);
  return true;
}

std::optional<base::Value> ReadTextScale(Profile* profile) {
  ChromeZoomLevelPrefs* zoom = profile->GetZoomLevelPrefs();
  if (!zoom) {
    return std::nullopt;
  }
  const double factor =
      blink::ZoomLevelToZoomFactor(zoom->GetDefaultZoomLevelPref());
  return base::Value(std::round(factor * kPercent));
}

bool WriteTextScale(Profile* profile, const base::Value& value) {
  const std::optional<double> percent = value.GetIfDouble();
  ChromeZoomLevelPrefs* zoom = profile->GetZoomLevelPrefs();
  if (!percent || !zoom || !std::isfinite(*percent) ||
      *percent < kMinTextScalePercent || *percent > kMaxTextScalePercent) {
    return false;
  }
  zoom->SetDefaultZoomLevelPref(
      blink::ZoomFactorToZoomLevel(*percent / kPercent));
  return true;
}

struct CustomPref {
  std::string_view key;
  Reader read;
  Writer write;
};

constexpr CustomPref kCustomPrefs[] = {
    {"blockThirdPartyCookies", &ReadBlockThirdPartyCookies,
     &WriteBlockThirdPartyCookies},
    {"safeBrowsing", &ReadSafeBrowsing, &WriteSafeBrowsing},
    {"preloadPages", &ReadPreloadPages, &WritePreloadPages},
    {"popupsBlocked",
     &ReadDefaultSetting<ContentSettingsType::POPUPS, CONTENT_SETTING_BLOCK>,
     &WriteDefaultSetting<ContentSettingsType::POPUPS, CONTENT_SETTING_BLOCK>},
    {"javascriptEnabled",
     &ReadDefaultSetting<ContentSettingsType::JAVASCRIPT,
                         CONTENT_SETTING_ALLOW>,
     &WriteDefaultSetting<ContentSettingsType::JAVASCRIPT,
                          CONTENT_SETTING_ALLOW>},
    {"textScale", &ReadTextScale, &WriteTextScale},
};

// Keys the shell knows but this engine does not serve yet. They are logged as
// unsupported rather than unknown, and left out of the answer.
constexpr std::string_view kUnsupportedKeys[] = {"forceDarkWebContents"};

// --- Lookup. ---------------------------------------------------------------

const BoolPref* FindBoolPref(std::string_view key) {
  for (const BoolPref& entry : kBoolPrefs) {
    if (entry.key == key) {
      return &entry;
    }
  }
  return nullptr;
}

const CustomPref* FindCustomPref(std::string_view key) {
  for (const CustomPref& entry : kCustomPrefs) {
    if (entry.key == key) {
      return &entry;
    }
  }
  return nullptr;
}

void LogUnknownKey(std::string_view key) {
  for (std::string_view unsupported : kUnsupportedKeys) {
    if (unsupported == key) {
      LOG(WARNING) << "OHOS shell pref " << key << " is not supported";
      return;
    }
  }
  LOG(WARNING) << "OHOS shell asked for unknown pref " << key;
}

std::optional<base::Value> ReadShellPref(Profile* profile,
                                         std::string_view key) {
  if (const BoolPref* entry = FindBoolPref(key)) {
    if (!IsRegistered(*profile->GetPrefs(), entry->pref_name)) {
      return std::nullopt;
    }
    return base::Value(profile->GetPrefs()->GetBoolean(entry->pref_name));
  }
  if (const CustomPref* entry = FindCustomPref(key)) {
    return entry->read(profile);
  }
  LogUnknownKey(key);
  return std::nullopt;
}

bool WriteShellPref(Profile* profile,
                    std::string_view key,
                    const base::Value& value) {
  if (const BoolPref* entry = FindBoolPref(key)) {
    PrefService* pref_service = profile->GetPrefs();
    if (!value.is_bool() || !IsRegistered(*pref_service, entry->pref_name) ||
        !pref_service->IsUserModifiablePreference(entry->pref_name)) {
      return false;
    }
    pref_service->SetBoolean(entry->pref_name, value.GetBool());
    return true;
  }
  if (const CustomPref* entry = FindCustomPref(key)) {
    return entry->write(profile, value);
  }
  LogUnknownKey(key);
  return false;
}

void AppendValue(Profile* profile,
                 std::string_view key,
                 base::ListValue& values) {
  std::optional<base::Value> value = ReadShellPref(profile, key);
  if (!value) {
    return;
  }
  base::DictValue item;
  item.Set("key", key);
  item.Set("value", std::move(*value));
  values.Append(std::move(item));
}

}  // namespace

void GetPrefs(const ShellCommandContext& context,
              const base::DictValue& command) {
  Profile* profile = SettingsProfile(context);
  base::ListValue values;
  const base::ListValue* keys = command.FindList("keys");
  if (profile && keys) {
    for (const base::Value& key : *keys) {
      if (key.is_string()) {
        AppendValue(profile, key.GetString(), values);
      } else {
        LOG(WARNING) << "OHOS shell getPrefs key is not a string";
      }
    }
  } else if (profile) {
    for (const BoolPref& entry : kBoolPrefs) {
      AppendValue(profile, entry.key, values);
    }
    for (const CustomPref& entry : kCustomPrefs) {
      AppendValue(profile, entry.key, values);
    }
  }
  base::DictValue event;
  event.Set("event", "prefs");
  event.Set("requestId", ReadRequestId(command));
  event.Set("values", std::move(values));
  ReplyToShell(context, std::move(event));
}

void SetPref(const ShellCommandContext& context,
             const base::DictValue& command) {
  Profile* profile = SettingsProfile(context);
  const std::string* key = command.FindString("key");
  const base::Value* value = command.Find("value");
  if (!profile || !key || !value) {
    LOG(WARNING) << "OHOS shell setPref without key or value";
    return;
  }
  if (!WriteShellPref(profile, *key, *value)) {
    LOG(WARNING) << "OHOS shell setPref " << *key << " rejected value "
                 << value->DebugString();
    return;
  }
  base::ListValue keys;
  keys.Append(*key);
  BroadcastPrefsChanged(context, std::move(keys));
}

}  // namespace chrome::ohos::settings_internal
