// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef CHROME_BROWSER_UI_OHOS_SHELL_DEFAULT_SITE_SETTINGS_OHOS_H_
#define CHROME_BROWSER_UI_OHOS_SHELL_DEFAULT_SITE_SETTINGS_OHOS_H_

#include <optional>
#include <string_view>
#include <utility>

#include "base/values.h"
#include "components/content_settings/core/browser/content_settings_info.h"
#include "components/content_settings/core/browser/content_settings_registry.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"

namespace chrome::ohos::settings_internal {

// The effective default may be controlled by policy or an extension. Writing
// a hidden user preference in that case must not be presented as success.
inline bool DefaultSiteSettingManaged(HostContentSettingsMap* map,
                                      ContentSettingsType type) {
  content_settings::ProviderType source = content_settings::ProviderType::kNone;
  map->GetDefaultContentSetting(type, &source);
  return source != content_settings::ProviderType::kPrefProvider &&
         source != content_settings::ProviderType::kDefaultProvider;
}

inline std::optional<base::DictValue> ReadDefaultSiteSetting(
    HostContentSettingsMap* map, std::string_view name,
    ContentSettingsType type) {
  const auto* info =
      content_settings::ContentSettingsRegistry::GetInstance()->Get(type);
  if (!map || !info) {
    return std::nullopt;
  }
  struct Choice { const char* name; ContentSetting value; };
  constexpr Choice choices[] = {{"allow", CONTENT_SETTING_ALLOW},
                                 {"block", CONTENT_SETTING_BLOCK},
                                 {"ask", CONTENT_SETTING_ASK}};
  const auto current = map->GetDefaultContentSetting(type);
  const char* setting = nullptr;
  base::ListValue allowed;
  for (const auto& choice : choices) {
    if (choice.value == current) {
      setting = choice.name;
    }
    if (info->IsDefaultSettingValid(choice.value)) {
      allowed.Append(choice.name);
    }
  }
  if (!setting || allowed.empty()) {
    return std::nullopt;
  }
  base::DictValue item;
  item.Set("type", name);
  item.Set("setting", setting);
  item.Set("managed", DefaultSiteSettingManaged(map, type));
  item.Set("choices", std::move(allowed));
  return item;
}

inline bool WriteDefaultSiteSetting(HostContentSettingsMap* map,
                                    ContentSettingsType type,
                                    ContentSetting setting) {
  const auto* info =
      content_settings::ContentSettingsRegistry::GetInstance()->Get(type);
  if (!map || !info || !info->IsDefaultSettingValid(setting) ||
      DefaultSiteSettingManaged(map, type)) {
    return false;
  }
  map->SetDefaultContentSetting(type, setting);
  return true;
}

}  // namespace chrome::ohos::settings_internal
#endif  // CHROME_BROWSER_UI_OHOS_SHELL_DEFAULT_SITE_SETTINGS_OHOS_H_
