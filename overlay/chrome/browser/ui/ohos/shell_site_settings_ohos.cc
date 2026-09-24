// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Per-site permissions and content settings for the shell's "Site settings"
// pages, read from and written to HostContentSettingsMap.
//
//   getSiteSettings {requestId, type?}
//     -> "siteSettings" {requestId, items: [{origin, type, setting,
//                                            embeddingOrigin?}]}
//        The user's site exceptions (not defaults, not policy) for `type`, or
//        for every type below when absent. `origin` is Chromium's pattern
//        ("https://a.com:443", "[*.]a.com"); embeddingOrigin is only set for
//        storageAccess, whose exceptions pair an embedded site with the
//        top-level site.
//   getSiteSettingsForOrigin {requestId, origin}
//     -> "siteSettings" {requestId, items: [{origin, type, setting}]}
//        The effective setting of every per-origin type for `origin`.
//   setSiteSetting {origin, type, setting}   setting may also be 'default',
//                                            which removes the exception.
//   setDefaultSiteSetting {type, setting}
//   resetSiteSettings {origin}
//        Removes this origin's exceptions for every type below and clears its
//        cookies and storage.
//
// type: 'location' | 'camera' | 'microphone' | 'notifications' | 'javascript'
//       | 'popups' | 'sound' | 'clipboard' | 'storageAccess'
// setting: 'allow' | 'block' | 'ask' (not every type takes 'ask')
// origin: an http(s) origin such as "https://example.com".
//
// There is no 'autoplay': ContentSettingsType::AUTOPLAY exists, but nothing
// in desktop Chromium enforces it -- 'sound' is the control that works.

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "base/functional/callback_helpers.h"
#include "base/logging.h"
#include "base/values.h"
#include "chrome/browser/content_settings/host_content_settings_map_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/ohos/shell_services_ohos.h"
#include "chrome/browser/ui/ohos/shell_settings_ohos_internal.h"
#include "components/content_settings/core/browser/content_settings_info.h"
#include "components/content_settings/core/browser/content_settings_registry.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/content_settings/core/common/content_settings.h"
#include "components/content_settings/core/common/content_settings_pattern.h"
#include "components/content_settings/core/common/content_settings_types.h"
#include "content/public/browser/storage_partition.h"
#include "net/base/is_potentially_trustworthy.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace chrome::ohos::settings_internal {

namespace {

struct SiteSettingType {
  std::string_view name;
  ContentSettingsType type;
  // Chromium honours these exceptions on secure origins only.
  bool secure_origins_only;
  // False for types scoped to a pair of sites, which one origin cannot name:
  // they are listed and reset, but not read or set per origin.
  bool per_origin;
};

// The site settings the shell exposes. Adding one is one line.
constexpr SiteSettingType kSiteSettingTypes[] = {
    {"location", ContentSettingsType::GEOLOCATION, true, true},
    {"camera", ContentSettingsType::MEDIASTREAM_CAMERA, true, true},
    {"microphone", ContentSettingsType::MEDIASTREAM_MIC, true, true},
    {"notifications", ContentSettingsType::NOTIFICATIONS, true, true},
    {"javascript", ContentSettingsType::JAVASCRIPT, false, true},
    {"popups", ContentSettingsType::POPUPS, false, true},
    {"sound", ContentSettingsType::SOUND, false, true},
    {"clipboard", ContentSettingsType::CLIPBOARD_READ_WRITE, true, true},
    {"storageAccess", ContentSettingsType::STORAGE_ACCESS, false, false},
};

struct SettingName {
  std::string_view name;
  ContentSetting setting;
};

constexpr SettingName kSettingNames[] = {
    {"allow", CONTENT_SETTING_ALLOW},
    {"block", CONTENT_SETTING_BLOCK},
    {"ask", CONTENT_SETTING_ASK},
};

// Accepted by setSiteSetting only: drops the exception.
constexpr std::string_view kDefaultSettingName = "default";

const SiteSettingType* FindType(const std::string* name) {
  for (const SiteSettingType& entry : kSiteSettingTypes) {
    if (name && entry.name == *name) {
      return &entry;
    }
  }
  return nullptr;
}

std::optional<std::string_view> SettingToShell(ContentSetting setting) {
  for (const SettingName& entry : kSettingNames) {
    if (entry.setting == setting) {
      return entry.name;
    }
  }
  return std::nullopt;
}

std::optional<ContentSetting> SettingFromShell(const std::string* name) {
  for (const SettingName& entry : kSettingNames) {
    if (name && entry.name == *name) {
      return entry.setting;
    }
  }
  return std::nullopt;
}

// The command's "origin" as a URL, when it is an http(s) origin.
std::optional<GURL> ReadOrigin(const base::DictValue& command) {
  const std::string* spec = command.FindString("origin");
  const GURL url = spec ? GURL(*spec) : GURL();
  if (!url.is_valid() || !url.SchemeIsHTTPOrHTTPS()) {
    LOG(WARNING) << "OHOS shell sent bad site origin "
                 << (spec ? *spec : std::string("(absent)"));
    return std::nullopt;
  }
  return url::Origin::Create(url).GetURL();
}

HostContentSettingsMap* SettingsMap(const ShellCommandContext& context) {
  Profile* profile = SettingsProfile(context);
  return profile ? HostContentSettingsMapFactory::GetForProfile(profile)
                 : nullptr;
}

void ReplySiteSettings(const ShellCommandContext& context,
                       const base::DictValue& command,
                       base::ListValue items) {
  base::DictValue event;
  event.Set("event", "siteSettings");
  event.Set("requestId", ReadRequestId(command));
  event.Set("items", std::move(items));
  ReplyToShell(context, std::move(event));
}

// Only what the user set here: policy, extension and default rules are not
// the shell's to show as removable exceptions.
bool IsUserException(const ContentSettingPatternSource& rule) {
  return rule.source == content_settings::ProviderType::kPrefProvider &&
         !rule.IsExpired() &&
         !(rule.primary_pattern == ContentSettingsPattern::Wildcard() &&
           rule.secondary_pattern == ContentSettingsPattern::Wildcard());
}

void AppendExceptions(HostContentSettingsMap* map,
                      const SiteSettingType& type,
                      base::ListValue& items) {
  for (const ContentSettingPatternSource& rule :
       map->GetSettingsForOneType(type.type)) {
    const std::optional<std::string_view> setting =
        SettingToShell(rule.GetContentSetting());
    if (!IsUserException(rule) || !setting) {
      continue;
    }
    base::DictValue item;
    item.Set("origin", rule.primary_pattern.ToString());
    item.Set("type", type.name);
    item.Set("setting", *setting);
    if (!type.per_origin &&
        rule.secondary_pattern != ContentSettingsPattern::Wildcard()) {
      item.Set("embeddingOrigin", rule.secondary_pattern.ToString());
    }
    items.Append(std::move(item));
  }
}

// Whether `rule` is one of `origin`'s own exceptions. Site-wide wildcards
// ("[*.]a.com") that cover the origin count; the catch-all does not.
bool RuleCoversOrigin(const ContentSettingPatternSource& rule,
                      const GURL& origin) {
  auto covers = [&origin](const ContentSettingsPattern& pattern) {
    return !pattern.MatchesAllHosts() && pattern.Matches(origin);
  };
  return covers(rule.primary_pattern) || covers(rule.secondary_pattern);
}

}  // namespace

void GetSiteSettings(const ShellCommandContext& context,
                     const base::DictValue& command) {
  HostContentSettingsMap* map = SettingsMap(context);
  const std::string* type_name = command.FindString("type");
  const SiteSettingType* only = FindType(type_name);
  if (type_name && !only) {
    LOG(WARNING) << "OHOS shell asked for unknown site setting " << *type_name;
  }
  base::ListValue items;
  if (map && (!type_name || only)) {
    for (const SiteSettingType& type : kSiteSettingTypes) {
      if (!only || only == &type) {
        AppendExceptions(map, type, items);
      }
    }
  }
  ReplySiteSettings(context, command, std::move(items));
}

void GetSiteSettingsForOrigin(const ShellCommandContext& context,
                              const base::DictValue& command) {
  HostContentSettingsMap* map = SettingsMap(context);
  const std::optional<GURL> origin = ReadOrigin(command);
  base::ListValue items;
  if (map && origin) {
    const std::string serialized = url::Origin::Create(*origin).Serialize();
    for (const SiteSettingType& type : kSiteSettingTypes) {
      const std::optional<std::string_view> setting =
          type.per_origin ? SettingToShell(map->GetContentSetting(
                                *origin, *origin, type.type))
                          : std::nullopt;
      if (!setting) {
        continue;
      }
      base::DictValue item;
      item.Set("origin", serialized);
      item.Set("type", type.name);
      item.Set("setting", *setting);
      items.Append(std::move(item));
    }
  }
  ReplySiteSettings(context, command, std::move(items));
}

void SetSiteSetting(const ShellCommandContext& context,
                    const base::DictValue& command) {
  HostContentSettingsMap* map = SettingsMap(context);
  const std::optional<GURL> origin = ReadOrigin(command);
  const SiteSettingType* type = FindType(command.FindString("type"));
  const std::string* name = command.FindString("setting");
  std::optional<ContentSetting> setting = SettingFromShell(name);
  if (!setting && name && *name == kDefaultSettingName) {
    setting = CONTENT_SETTING_DEFAULT;
  }
  if (!map || !origin || !type || !type->per_origin || !setting) {
    LOG(WARNING) << "OHOS shell setSiteSetting: bad type or setting";
    return;
  }
  // Chromium's registry honours these exceptions on secure origins only
  // (EXCEPTIONS_ON_SECURE_ORIGINS_ONLY); storing one for an insecure origin
  // would show the user a setting that does nothing.
  if (type->secure_origins_only &&
      !net::IsUrlPotentiallyTrustworthy(*origin)) {
    LOG(WARNING) << "OHOS shell setSiteSetting: " << type->name
                 << " needs a secure origin";
    return;
  }
  const content_settings::ContentSettingsInfo* info =
      content_settings::ContentSettingsRegistry::GetInstance()->Get(
          type->type);
  if (*setting != CONTENT_SETTING_DEFAULT &&
      (!info || !info->IsSettingValid(*setting))) {
    LOG(WARNING) << "OHOS shell setSiteSetting: " << *name
                 << " is not valid for " << type->name;
    return;
  }
  map->SetContentSettingDefaultScope(*origin, *origin, type->type, *setting);
}

void SetDefaultSiteSetting(const ShellCommandContext& context,
                           const base::DictValue& command) {
  HostContentSettingsMap* map = SettingsMap(context);
  const SiteSettingType* type = FindType(command.FindString("type"));
  const std::optional<ContentSetting> setting =
      SettingFromShell(command.FindString("setting"));
  const content_settings::ContentSettingsInfo* info =
      type ? content_settings::ContentSettingsRegistry::GetInstance()->Get(
                 type->type)
           : nullptr;
  if (!map || !info || !setting || !info->IsDefaultSettingValid(*setting)) {
    LOG(WARNING) << "OHOS shell setDefaultSiteSetting: bad type or setting";
    return;
  }
  map->SetDefaultContentSetting(type->type, *setting);
}

void ResetSiteSettings(const ShellCommandContext& context,
                       const base::DictValue& command) {
  Profile* profile = SettingsProfile(context);
  HostContentSettingsMap* map = SettingsMap(context);
  const std::optional<GURL> origin = ReadOrigin(command);
  if (!profile || !map || !origin) {
    return;
  }
  for (const SiteSettingType& type : kSiteSettingTypes) {
    map->ClearSettingsForOneTypeWithPredicate(
        type.type, [&origin](const ContentSettingPatternSource& rule) {
          return rule.source ==
                     content_settings::ProviderType::kPrefProvider &&
                 RuleCoversOrigin(rule, *origin);
        });
  }
  // Then the origin's cookies, storage and caches; the reset is "forget this
  // site", not only its permissions.
  profile->GetDefaultStoragePartition()->ClearDataForOrigin(
      content::StoragePartition::REMOVE_DATA_MASK_ALL, *origin,
      base::DoNothing());
}

}  // namespace chrome::ohos::settings_internal
