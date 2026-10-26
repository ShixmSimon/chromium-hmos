// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_OHOS_SHELL_PRIVACY_PREFS_OHOS_H_
#define CHROME_BROWSER_UI_OHOS_SHELL_PRIVACY_PREFS_OHOS_H_

#include <optional>
#include <string>
#include <utility>

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/values.h"
#include "chrome/browser/prefetch/pref_names.h"
#include "chrome/browser/preloading/preloading_prefs.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/safe_browsing/advanced_protection_status_manager.h"
#include "chrome/browser/safe_browsing/advanced_protection_status_manager_factory.h"
#include "chrome/browser/ssl/https_first_mode_settings_tracker.h"
#include "chrome/browser/ssl/https_upgrades_util.h"
#include "chrome/common/pref_names.h"
#include "components/autofill/core/common/autofill_prefs.h"
#include "components/content_settings/core/browser/cookie_settings.h"
#include "components/content_settings/core/common/pref_names.h"
#include "components/prefs/pref_service.h"

namespace chrome::ohos::settings_internal {

inline std::optional<base::Value> PrivacyConfig(base::DictValue config) {
  auto json = base::WriteJson(config);
  return json ? std::make_optional(base::Value(std::move(*json)))
              : std::nullopt;
}

inline std::optional<std::string> PrivacyMode(const base::Value& value) {
  if (!value.is_string() || value.GetString().size() > 1024) {
    return std::nullopt;
  }
  auto config = base::JSONReader::ReadDict(value.GetString(),
                                          base::JSON_PARSE_RFC);
  const std::string* mode = config ? config->FindString("mode") : nullptr;
  return mode ? std::make_optional(*mode) : std::nullopt;
}

inline std::optional<base::Value> ReadThirdPartyCookies(Profile* profile) {
  PrefService* state = profile->GetPrefs();
  if (!state->FindPreference(prefs::kCookieControlsMode)) {
    return std::nullopt;
  }
  const bool block = state->GetInteger(prefs::kCookieControlsMode) ==
      static_cast<int>(content_settings::CookieControlsMode::kBlockThirdParty);
  base::DictValue config;
  config.Set("mode", block ? "block" : "incognito");
  config.Set("managed", !state->IsUserModifiablePreference(
                            prefs::kCookieControlsMode));
  return PrivacyConfig(std::move(config));
}

inline bool WriteThirdPartyCookies(Profile* profile, const base::Value& value) {
  PrefService* state = profile->GetPrefs();
  const auto mode = PrivacyMode(value);
  if (!state->FindPreference(prefs::kCookieControlsMode) ||
      !state->IsUserModifiablePreference(prefs::kCookieControlsMode) ||
      !mode || (*mode != "block" && *mode != "incognito")) {
    return false;
  }
  state->SetInteger(prefs::kCookieControlsMode, static_cast<int>(
      *mode == "block" ? content_settings::CookieControlsMode::kBlockThirdParty
                      : content_settings::CookieControlsMode::kIncognitoOnly));
  return true;
}

// Address and payment preferences are independent. Metadata describes the
// configured opt-in, not whether a particular field can currently be filled.
inline std::optional<base::Value> ReadAutofillPreference(Profile* profile,
                                                       bool payments) {
  const PrefService* state = profile->GetPrefs();
  const char* name = payments ? autofill::prefs::kAutofillCreditCardEnabled
                              : autofill::prefs::kAutofillProfileEnabled;
  if (!state->FindPreference(name)) {
    return std::nullopt;
  }
  const bool enabled = payments
      ? autofill::prefs::IsAutofillPaymentMethodsEnabled(state)
      : autofill::prefs::IsAutofillProfileEnabled(state);
  base::DictValue config;
  config.Set("mode", enabled ? "on" : "off");
  config.Set("managed", !state->IsUserModifiablePreference(name));
  return PrivacyConfig(std::move(config));
}

inline bool WriteAutofillPreference(Profile* profile, const base::Value& value,
                                   bool payments) {
  PrefService* state = profile->GetPrefs();
  const char* name = payments ? autofill::prefs::kAutofillCreditCardEnabled
                              : autofill::prefs::kAutofillProfileEnabled;
  const auto mode = PrivacyMode(value);
  if (!state->FindPreference(name) || !state->IsUserModifiablePreference(name) ||
      !mode || (*mode != "on" && *mode != "off")) {
    return false;
  }
  if (payments) {
    autofill::prefs::SetAutofillPaymentMethodsEnabled(state, *mode == "on");
  } else {
    autofill::prefs::SetAutofillProfileEnabled(state, *mode == "on");
  }
  return true;
}

inline std::optional<base::Value> ReadAddressAutofill(Profile* profile) {
  return ReadAutofillPreference(profile, false);
}
inline bool WriteAddressAutofill(Profile* profile, const base::Value& value) {
  return WriteAutofillPreference(profile, value, false);
}
inline std::optional<base::Value> ReadPaymentAutofill(Profile* profile) {
  return ReadAutofillPreference(profile, true);
}
inline bool WritePaymentAutofill(Profile* profile, const base::Value& value) {
  return WriteAutofillPreference(profile, value, true);
}

inline bool PreloadingRegistered(const PrefService& state) {
  return state.FindPreference(prefetch::prefs::kNetworkPredictionOptions);
}

inline bool PreloadingWritable(const PrefService& state) {
  return PreloadingRegistered(state) && state.IsUserModifiablePreference(
      prefetch::prefs::kNetworkPredictionOptions);
}

inline std::optional<base::Value> ReadPreloading(Profile* profile) {
  const PrefService& state = *profile->GetPrefs();
  if (!PreloadingRegistered(state)) {
    return std::nullopt;
  }
  const auto setting = prefetch::GetPreloadPagesState(state);
  base::DictValue config;
  config.Set("mode",
      setting == prefetch::PreloadPagesState::kExtendedPreloading ? "extended" :
      setting == prefetch::PreloadPagesState::kStandardPreloading ? "standard" :
      "off");
  config.Set("managed", !PreloadingWritable(state));
  return PrivacyConfig(std::move(config));
}

inline bool WritePreloading(Profile* profile, const base::Value& value) {
  PrefService* state = profile->GetPrefs();
  const auto mode = PrivacyMode(value);
  if (!mode || !PreloadingWritable(*state)) {
    return false;
  }
  prefetch::PreloadPagesState setting;
  if (*mode == "off") {
    setting = prefetch::PreloadPagesState::kNoPreloading;
  } else if (*mode == "standard") {
    setting = prefetch::PreloadPagesState::kStandardPreloading;
  } else if (*mode == "extended") {
    setting = prefetch::PreloadPagesState::kExtendedPreloading;
  } else {
    return false;
  }
  prefetch::SetPreloadPagesState(state, setting);
  return true;
}

// Keep the old boolean wire contract. Enabling an already-enabled extended
// mode must not downgrade it; both interfaces enforce the same policy guard.
inline std::optional<base::Value> ReadPreloadPages(Profile* profile) {
  const PrefService& state = *profile->GetPrefs();
  if (!PreloadingRegistered(state)) {
    return std::nullopt;
  }
  return base::Value(prefetch::GetPreloadPagesState(state) !=
                    prefetch::PreloadPagesState::kNoPreloading);
}

inline bool WritePreloadPages(Profile* profile, const base::Value& value) {
  const std::optional<bool> enabled = value.GetIfBool();
  PrefService* state = profile->GetPrefs();
  if (!enabled || !PreloadingWritable(*state)) {
    return false;
  }
  const auto current = prefetch::GetPreloadPagesState(*state);
  if (*enabled != (current != prefetch::PreloadPagesState::kNoPreloading)) {
    prefetch::SetPreloadPagesState(state,
        *enabled ? prefetch::PreloadPagesState::kStandardPreloading
                 : prefetch::PreloadPagesState::kNoPreloading);
  }
  return true;
}

inline bool HttpsFirstRegistered(const PrefService& state) {
  return state.FindPreference(prefs::kHttpsOnlyModeEnabled) &&
         state.FindPreference(prefs::kHttpsFirstBalancedMode);
}

inline bool HttpsFirstManaged(Profile* profile) {
  const PrefService* state = profile->GetPrefs();
  // Advanced Protection can force the effective mode independently of prefs.
  auto* protection =
      safe_browsing::AdvancedProtectionStatusManagerFactory::GetForProfile(
          profile);
  return !state->IsUserModifiablePreference(prefs::kHttpsOnlyModeEnabled) ||
         (IsBalancedModeAvailable() && !state->IsUserModifiablePreference(
                                          prefs::kHttpsFirstBalancedMode)) ||
         (protection && protection->IsUnderAdvancedProtection());
}

inline std::optional<base::Value> ReadHttpsFirst(Profile* profile) {
  if (!HttpsFirstRegistered(*profile->GetPrefs())) {
    return std::nullopt;
  }
  auto* service = HttpsFirstModeServiceFactory::GetForProfile(profile);
  if (!service) {
    return std::nullopt;
  }
  const auto setting = service->GetCurrentSetting();
  base::DictValue config;
  config.Set("mode", setting == HttpsFirstModeSetting::kEnabledFull ? "strict" :
      setting == HttpsFirstModeSetting::kEnabledBalanced ? "balanced" : "off");
  config.Set("managed", HttpsFirstManaged(profile));
  config.Set("balancedAvailable", IsBalancedModeAvailable());
  return PrivacyConfig(std::move(config));
}

inline bool WriteHttpsFirst(Profile* profile, const base::Value& value) {
  const auto mode = PrivacyMode(value);
  if (!mode || !HttpsFirstRegistered(*profile->GetPrefs()) ||
      HttpsFirstManaged(profile)) {
    return false;
  }
  HttpsFirstModeSetting setting;
  if (*mode == "off") {
    setting = HttpsFirstModeSetting::kDisabled;
  } else if (*mode == "strict") {
    setting = HttpsFirstModeSetting::kEnabledFull;
  } else if (*mode == "balanced" && IsBalancedModeAvailable()) {
    setting = HttpsFirstModeSetting::kEnabledBalanced;
  } else {
    return false;
  }
  auto* service = HttpsFirstModeServiceFactory::GetForProfile(profile);
  return service && service->UpdatePrefs(setting);
}

}  // namespace chrome::ohos::settings_internal

#endif  // CHROME_BROWSER_UI_OHOS_SHELL_PRIVACY_PREFS_OHOS_H_
