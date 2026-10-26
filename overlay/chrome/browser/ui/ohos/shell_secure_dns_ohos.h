// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_OHOS_SHELL_SECURE_DNS_OHOS_H_
#define CHROME_BROWSER_UI_OHOS_SHELL_SECURE_DNS_OHOS_H_

#include <optional>
#include <string>
#include <utility>

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/values.h"
#include "chrome/browser/browser_process.h"
#include "chrome/common/pref_names.h"
#include "components/prefs/pref_service.h"
#include "net/dns/public/dns_over_https_config.h"
#include "url/gurl.h"

class Profile;

namespace chrome::ohos::settings_internal {

// Local State, shared by regular and incognito windows, not Profile prefs.
// Keep mode/templates in one shell value so invalid/locked writes cannot
// partially change the configuration. Chromium observes the underlying prefs.
inline PrefService* SecureDnsLocalState() {
  PrefService* state = g_browser_process ? g_browser_process->local_state()
                                       : nullptr;
  return state && state->FindPreference(prefs::kDnsOverHttpsMode) &&
                 state->FindPreference(prefs::kDnsOverHttpsTemplates)
             ? state
             : nullptr;
}

inline bool SecureDnsManaged(const PrefService& state) {
  return !state.IsUserModifiablePreference(prefs::kDnsOverHttpsMode) ||
         !state.IsUserModifiablePreference(prefs::kDnsOverHttpsTemplates);
}

// A JSON string keeps the existing ShellPrefValue string/number/bool wire
// format compatible with older HARs. A missing key means unsupported.
inline std::optional<base::Value> ReadSecureDns(Profile*) {
  PrefService* state = SecureDnsLocalState();
  if (!state) {
    return std::nullopt;
  }
  std::string mode = state->GetString(prefs::kDnsOverHttpsMode);
  if (mode.empty()) {
    mode = "off";
  }
  base::DictValue config;
  config.Set("mode", mode);
  config.Set("templates", state->GetString(prefs::kDnsOverHttpsTemplates));
  config.Set("managed", SecureDnsManaged(*state));
  auto json = base::WriteJson(config);
  return json ? std::make_optional(base::Value(std::move(*json)))
              : std::nullopt;
}

inline bool WriteSecureDns(Profile*, const base::Value& value) {
  PrefService* state = SecureDnsLocalState();
  if (!state || SecureDnsManaged(*state) || !value.is_string() ||
      value.GetString().size() > 8192) {
    return false;
  }
  auto config = base::JSONReader::ReadDict(value.GetString(),
                                          base::JSON_PARSE_RFC);
  if (!config) {
    return false;
  }
  const std::string* mode = config->FindString("mode");
  const std::string* templates = config->FindString("templates");
  if (!mode || !templates || templates->size() > 4096 ||
      (*mode != "off" && *mode != "automatic" && *mode != "secure")) {
    return false;
  }
  if (*mode == "secure" && templates->empty()) {
    return false;
  }
  if (!templates->empty()) {
    auto parsed = net::DnsOverHttpsConfig::FromString(*templates);
    if (!parsed || parsed->servers().empty()) {
      return false;
    }
    for (const auto& server : parsed->servers()) {
      const GURL url(server.server_template());
      if (!url.SchemeIs("https") || url.has_username() ||
          url.has_password() || url.has_ref()) {
        return false;
      }
    }
  }
  // Validate EVERYTHING before writing either pref. While leaving strict
  // mode, change the mode before clearing its servers. Otherwise install
  // servers first so strict mode never observes an empty configuration.
  if (*mode != "secure") {
    state->SetString(prefs::kDnsOverHttpsMode, *mode);
  }
  state->SetString(prefs::kDnsOverHttpsTemplates, *templates);
  if (*mode == "secure") {
    state->SetString(prefs::kDnsOverHttpsMode, *mode);
  }
  return true;
}

}  // namespace chrome::ohos::settings_internal

#endif  // CHROME_BROWSER_UI_OHOS_SHELL_SECURE_DNS_OHOS_H_
