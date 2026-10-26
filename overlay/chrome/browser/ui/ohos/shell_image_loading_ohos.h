// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef CHROME_BROWSER_UI_OHOS_SHELL_IMAGE_LOADING_OHOS_H_
#define CHROME_BROWSER_UI_OHOS_SHELL_IMAGE_LOADING_OHOS_H_

#include "chrome/browser/content_settings/host_content_settings_map_factory.h"
#include "chrome/browser/ui/ohos/shell_default_site_settings_ohos.h"
#include "chrome/browser/ui/ohos/shell_privacy_prefs_ohos.h"

namespace chrome::ohos::settings_internal {

inline std::optional<base::Value> ReadImageLoading(Profile* profile) {
  auto* map = HostContentSettingsMapFactory::GetForProfile(profile);
  constexpr auto type = ContentSettingsType::IMAGES;
  if (!map || !content_settings::ContentSettingsRegistry::GetInstance()->Get(
                  type)) {
    return std::nullopt;
  }
  const char* mode;
  switch (map->GetDefaultContentSetting(type)) {
    case CONTENT_SETTING_ALLOW: mode = "allow"; break;
    case CONTENT_SETTING_BLOCK: mode = "block"; break;
    default: return std::nullopt;
  }
  base::DictValue config;
  config.Set("mode", mode);
  config.Set("managed", DefaultSiteSettingManaged(map, type));
  return PrivacyConfig(std::move(config));
}

inline bool WriteImageLoading(Profile* profile, const base::Value& value) {
  const auto mode = PrivacyMode(value);
  if (!mode) {
    return false;
  }
  ContentSetting setting;
  if (*mode == "allow") {
    setting = CONTENT_SETTING_ALLOW;
  } else if (*mode == "block") {
    setting = CONTENT_SETTING_BLOCK;
  } else {
    return false;
  }
  // Change only the image default; preserve existing site exceptions.
  return WriteDefaultSiteSetting(
      HostContentSettingsMapFactory::GetForProfile(profile),
      ContentSettingsType::IMAGES, setting);
}

}  // namespace chrome::ohos::settings_internal
#endif  // CHROME_BROWSER_UI_OHOS_SHELL_IMAGE_LOADING_OHOS_H_
