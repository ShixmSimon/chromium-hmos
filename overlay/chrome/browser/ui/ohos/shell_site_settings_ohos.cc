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

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/values.h"
#include "chrome/browser/content_settings/host_content_settings_map_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/ohos/shell_default_site_settings_ohos.h"
#include "chrome/browser/ui/ohos/shell_services_ohos.h"
#include "chrome/browser/ui/ohos/shell_settings_ohos_internal.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "components/blocked_content/popup_blocker_tab_helper.h"  // nogncheck
#include "components/browsing_data/content/browsing_data_model.h"
#include "components/content_settings/core/browser/content_settings_info.h"
#include "components/content_settings/core/browser/content_settings_registry.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/content_settings/core/common/content_settings.h"
#include "components/content_settings/core/common/content_settings_pattern.h"
#include "components/content_settings/core/common/content_settings_types.h"
#include "components/sessions/content/session_tab_helper.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/ssl_status.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents.h"
#include "net/base/ip_address.h"
#include "net/base/is_potentially_trustworthy.h"
#include "net/base/schemeful_site.h"
#include "net/cert/cert_status_flags.h"
#include "net/cert/x509_certificate.h"
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

std::optional<base::Value> ReadDefaultSiteSettings(Profile* profile) {
  auto* map = profile
      ? HostContentSettingsMapFactory::GetForProfile(profile) : nullptr;
  if (!map) {
    return std::nullopt;
  }
  base::ListValue items;
  for (const auto& type : kSiteSettingTypes) {
    auto item = ReadDefaultSiteSetting(map, type.name, type.type);
    if (item) {
      items.Append(std::move(*item));
    }
  }
  base::DictValue config;
  config.Set("items", std::move(items));
  auto json = base::WriteJson(config);
  return json ? std::make_optional(base::Value(std::move(*json)))
              : std::nullopt;
}

bool WriteDefaultSiteSettings(Profile* profile, const base::Value& value) {
  if (!profile || !value.is_string() || value.GetString().size() > 1024) {
    return false;
  }
  auto config = base::JSONReader::ReadDict(value.GetString(),
                                          base::JSON_PARSE_RFC);
  const auto* type = config ? FindType(config->FindString("type")) : nullptr;
  const auto setting = config
      ? SettingFromShell(config->FindString("setting")) : std::nullopt;
  return type && setting && WriteDefaultSiteSetting(
      HostContentSettingsMapFactory::GetForProfile(profile), type->type,
      *setting);
}

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
  if (!type || !setting ||
      !WriteDefaultSiteSetting(map, type->type, *setting)) {
    LOG(WARNING) << "OHOS shell setDefaultSiteSetting: invalid or controlled";
  }
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

// Page-info commands are scoped to the active committed tab/document. They
// intentionally use the sending window's Profile, including off-the-record,
// rather than SettingsProfile (the settings pages edit regular preferences).
namespace {

content::WebContents* PageForInfo(const ShellCommandContext& context,
                               const base::DictValue& command,
                               bool require_navigation) {
  auto* contents =
      context.browser
          ? context.browser->GetTabStripModel()->GetActiveWebContents()
          : nullptr;
  const auto* expected_tab = command.FindString("tabId");
  if (!contents || !context.profile ||
      contents->GetBrowserContext() != context.profile || !expected_tab ||
      *expected_tab !=
          base::NumberToString(
              sessions::SessionTabHelper::IdForTab(contents).id())) {
    return nullptr;
  }
  auto* entry = contents->GetController().GetLastCommittedEntry();
  if (!entry || !entry->GetURL().SchemeIsHTTPOrHTTPS()) {
    return nullptr;
  }
  if (require_navigation) {
    const auto* expected_navigation = command.FindString("navigationId");
    if (!expected_navigation ||
        *expected_navigation != base::NumberToString(entry->GetUniqueID())) {
      return nullptr;
    }
  }
  return contents;
}

base::DictValue PageInfoReply(const base::DictValue& command,
                             std::string_view event_name) {
  base::DictValue event;
  event.Set("event", event_name);
  event.Set("requestId", ReadRequestId(command));
  event.Set("protocolVersion", 1);
  event.Set("ok", false);
  event.Set("reason", "stalePage");
  return event;
}

base::ListValue PagePermissions(Profile* profile, const GURL& origin) {
  auto* map = HostContentSettingsMapFactory::GetForProfile(profile);
  base::ListValue items;
  if (!map) {
    return items;
  }
  for (const SiteSettingType& type : kSiteSettingTypes) {
    if (!type.per_origin) {
      continue;
    }
    const auto setting =
        SettingToShell(map->GetContentSetting(origin, origin, type.type));
    if (!setting) {
      continue;
    }
    base::DictValue item;
    item.Set("type", type.name);
    item.Set("setting", *setting);
    items.Append(std::move(item));
  }
  return items;
}

std::string_view PageConnection(const GURL& address,
                                const content::SSLStatus& ssl) {
  if (!address.SchemeIs("https")) {
    return "insecure";
  }
  if (!ssl.initialized || !ssl.certificate) {
    return "unknown";
  }
  if (net::IsCertStatusError(ssl.cert_status)) {
    return "invalid";
  }
  if (ssl.content_status != content::SSLStatus::NORMAL_CONTENT) {
    return "mixed";
  }
  return "secure";
}

base::DictValue PageCertificate(const content::SSLStatus& ssl) {
  base::DictValue result;
  if (!ssl.certificate) {
    return result;
  }
  const auto& cert = *ssl.certificate;
  result.Set("subject", cert.subject().GetDisplayName());
  result.Set("issuer", cert.issuer().GetDisplayName());
  result.Set("organization",
             base::JoinString(cert.subject().organization_names, ", "));
  result.Set("validFrom", ToShellTime(cert.valid_start()));
  result.Set("validTo", ToShellTime(cert.valid_expiry()));
  result.Set("serialNumber", base::HexEncode(cert.serial_number()));
  result.Set("fingerprint",
             base::HexEncode(net::X509Certificate::CalculateFingerprint256(
                 cert.cert_buffer())));
  std::vector<std::string> dns, ips;
  cert.GetSubjectAltName(&dns, &ips);
  base::ListValue names;
  for (const auto& name : dns) {
    names.Append(name);
  }
  for (const auto& name : ips) {
    names.Append(net::IPAddress(base::as_byte_span(name)).ToString());
  }
  result.Set("names", std::move(names));
  return result;
}

}  // namespace

void GetPageInfo(const ShellCommandContext& context,
                 const base::DictValue& command) {
  auto reply = PageInfoReply(command, "pageInfo");
  auto* contents = PageForInfo(context, command, false);
  if (!contents) {
    ReplyToShell(context, std::move(reply));
    return;
  }
  auto* entry = contents->GetController().GetLastCommittedEntry();
  const auto origin = url::Origin::Create(entry->GetURL());
  const auto& ssl = entry->GetSSL();
  // This is the committed connection state, never an inference from the URL.
  reply.Set("connection", PageConnection(entry->GetURL(), ssl));
  reply.Set("certificate", PageCertificate(ssl));
  reply.Set("origin", origin.Serialize());
  reply.Set("tabId", *command.FindString("tabId"));
  const int navigation = entry->GetUniqueID();
  reply.Set("navigationId", base::NumberToString(navigation));
  reply.Set("permissions", PagePermissions(context.profile, origin.GetURL()));
  reply.Set("cookieCount", -1);
  reply.Set("storageBytes", -1.0);
  reply.Set("ok", true);
  reply.Remove("reason");
  // Resolve against the same window/document after asynchronous storage reads.
  // Weak WebContents also keeps us from dereferencing a destroyed Profile.
  auto completed = base::BindOnce(
      [](base::WeakPtr<content::WebContents> contents,
         ShellCommandContext context, int navigation, url::Origin origin,
         base::DictValue reply, std::unique_ptr<BrowsingDataModel> model) {
        if (!contents) {
          return;
        }
        const auto* entry = contents->GetController().GetLastCommittedEntry();
        if (!entry || entry->GetUniqueID() != navigation) {
          reply.Set("ok", false);
          reply.Set("reason", "stalePage");
        } else if (model) {
          uint64_t cookies = 0, bytes = 0;
          const net::SchemefulSite top_site(origin);
          for (const auto& item : *model) {
            if (const auto* cookie =
                    std::get_if<net::CanonicalCookie>(&item.data_key.get())) {
              if (cookie->IsDomainMatch(origin.host()) &&
                  (!cookie->IsPartitioned() ||
                   cookie->PartitionKey()->site() == top_site)) {
                cookies += item.data_details->cookie_count;
              }
            } else if (item.Matches(origin) &&
                       !item.GetThirdPartyPartitioningSite()) {
              bytes += item.data_details->storage_size;
            }
          }
          reply.Set("cookieCount", static_cast<double>(cookies));
          reply.Set("storageBytes", static_cast<double>(bytes));
        }
        ReplyToShell(context, std::move(reply));
      },
      contents->GetWeakPtr(), context, navigation, origin, std::move(reply));
  auto* partition = contents->GetPrimaryMainFrame()->GetStoragePartition();
  if (partition == context.profile->GetDefaultStoragePartition()) {
    BrowsingDataModel::BuildFromDisk(context.profile, nullptr,
                                     std::move(completed));
  } else {
    BrowsingDataModel::BuildFromNonDefaultStoragePartition(
        partition, nullptr, std::move(completed));
  }
}

void SetPagePermission(const ShellCommandContext& context,
                       const base::DictValue& command) {
  auto reply = PageInfoReply(command, "pageInfoActionResult");
  auto* contents = PageForInfo(context, command, true);
  const auto* type = FindType(command.FindString("type"));
  const auto* name = command.FindString("setting");
  auto setting = SettingFromShell(name);
  if (name && *name == kDefaultSettingName) {
    setting = CONTENT_SETTING_DEFAULT;
  }
  if (contents && type && type->per_origin && setting) {
    const GURL origin =
        url::Origin::Create(contents->GetLastCommittedURL()).GetURL();
    const auto* info =
        content_settings::ContentSettingsRegistry::GetInstance()->Get(
            type->type);
    if ((!type->secure_origins_only ||
         net::IsUrlPotentiallyTrustworthy(origin)) &&
        (*setting == CONTENT_SETTING_DEFAULT ||
         (info && info->IsSettingValid(*setting)))) {
      HostContentSettingsMapFactory::GetForProfile(context.profile)
          ->SetContentSettingDefaultScope(origin, origin, type->type, *setting);
      reply.Set("ok", true);
      reply.Remove("reason");
    }
  }
  ReplyToShell(context, std::move(reply));
}

void ClearPageSiteData(const ShellCommandContext& context,
                       const base::DictValue& command) {
  auto reply = PageInfoReply(command, "pageInfoActionResult");
  auto* contents = PageForInfo(context, command, true);
  if (!contents) {
    ReplyToShell(context, std::move(reply));
    return;
  }
  const GURL origin =
      url::Origin::Create(contents->GetLastCommittedURL()).GetURL();
  contents->GetPrimaryMainFrame()->GetStoragePartition()->ClearDataForOrigin(
      content::StoragePartition::REMOVE_DATA_MASK_ALL, origin,
      base::BindOnce(
          [](base::WeakPtr<content::WebContents> contents,
             ShellCommandContext context, base::DictValue reply) {
            if (!contents) {
              return;
            }
            reply.Set("ok", true);
            reply.Remove("reason");
            ReplyToShell(context, std::move(reply));
          },
          contents->GetWeakPtr(), context, std::move(reply)));
}

// Opens what the popup blocker held back on the window's current tab -- the
// "show" on the shell's popup-blocked notice.
void ShowBlockedPopups(const ShellCommandContext& context,
                       const base::DictValue& command) {
  content::WebContents* contents =
      context.browser
          ? context.browser->GetTabStripModel()->GetActiveWebContents()
          : nullptr;
  auto* popups = contents
                     ? blocked_content::PopupBlockerTabHelper::FromWebContents(
                           contents)
                     : nullptr;
  if (popups) {
    popups->ShowAllBlockedPopups();
  }
}

}  // namespace chrome::ohos::settings_internal
