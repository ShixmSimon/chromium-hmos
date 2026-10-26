// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef CHROME_BROWSER_UI_OHOS_SHELL_COOKIE_STORAGE_OHOS_H_
#define CHROME_BROWSER_UI_OHOS_SHELL_COOKIE_STORAGE_OHOS_H_

#include "chrome/browser/content_settings/host_content_settings_map_factory.h"
#include "chrome/browser/ui/ohos/shell_default_site_settings_ohos.h"
#include "chrome/browser/ui/ohos/shell_privacy_prefs_ohos.h"

namespace chrome::ohos::settings_internal {

inline std::optional<base::Value> ReadCookieStorage(Profile* profile) {
  auto* map = HostContentSettingsMapFactory::GetForProfile(profile);
  constexpr auto type = ContentSettingsType::COOKIES;
  if (!map || !content_settings::ContentSettingsRegistry::GetInstance()->Get(
                  type)) {
    return std::nullopt;
  }
  const char* mode;
  switch (map->GetDefaultContentSetting(type)) {
    case CONTENT_SETTING_ALLOW: mode = "allow"; break;
    case CONTENT_SETTING_BLOCK: mode = "block"; break;
    case CONTENT_SETTING_SESSION_ONLY: mode = "session_only"; break;
    default: return std::nullopt;
  }
  base::DictValue config;
  config.Set("mode", mode);
  config.Set("managed", DefaultSiteSettingManaged(map, type));
  return PrivacyConfig(std::move(config));
}

inline bool WriteCookieStorage(Profile* profile, const base::Value& value) {
  const auto mode = PrivacyMode(value);
  if (!mode) {
    return false;
  }
  ContentSetting setting;
  if (*mode == "allow") {
    setting = CONTENT_SETTING_ALLOW;
  } else if (*mode == "block") {
    setting = CONTENT_SETTING_BLOCK;
  } else if (*mode == "session_only") {
    setting = CONTENT_SETTING_SESSION_ONLY;
  } else {
    return false;
  }
  // Change only the default. Site exceptions, third-party Cookie controls
  // and stored data remain governed by the existing Chromium services.
  return WriteDefaultSiteSetting(
      HostContentSettingsMapFactory::GetForProfile(profile),
      ContentSettingsType::COOKIES, setting);
}

}  // namespace chrome::ohos::settings_internal
#endif  // CHROME_BROWSER_UI_OHOS_SHELL_COOKIE_STORAGE_OHOS_H_
