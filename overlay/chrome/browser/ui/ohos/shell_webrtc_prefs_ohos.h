// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef CHROME_BROWSER_UI_OHOS_SHELL_WEBRTC_PREFS_OHOS_H_
#define CHROME_BROWSER_UI_OHOS_SHELL_WEBRTC_PREFS_OHOS_H_

#include <initializer_list>

#include "chrome/browser/ui/ohos/shell_privacy_prefs_ohos.h"
#include "third_party/blink/public/common/peerconnection/webrtc_ip_handling_policy.h"

namespace chrome::ohos::settings_internal {

inline bool IsWebRtcIpMode(const std::string& mode) {
  for (const char* value : {blink::kWebRTCIPHandlingDefault,
       blink::kWebRTCIPHandlingDefaultPublicAndPrivateInterfaces,
       blink::kWebRTCIPHandlingDefaultPublicInterfaceOnly,
       blink::kWebRTCIPHandlingDisableNonProxiedUdp}) {
    if (mode == value) {
      return true;
    }
  }
  return false;
}

inline std::optional<base::Value> ReadWebRtcIpHandling(Profile* profile) {
  PrefService* state = profile->GetPrefs();
  if (!state->FindPreference(prefs::kWebRTCIPHandlingPolicy)) {
    return std::nullopt;
  }
  base::DictValue config;
  // Match the renderer's handling of empty or unrecognized old pref values.
  config.Set("mode", blink::ToString(blink::ToWebRTCIPHandlingPolicy(
                         state->GetString(prefs::kWebRTCIPHandlingPolicy))));
  config.Set("managed", !state->IsUserModifiablePreference(
                            prefs::kWebRTCIPHandlingPolicy));
  config.Set("hasUrlOverrides",
      state->FindPreference(prefs::kWebRTCIPHandlingUrl) &&
      !state->GetList(prefs::kWebRTCIPHandlingUrl).empty());
  return PrivacyConfig(std::move(config));
}

inline bool WriteWebRtcIpHandling(Profile* profile, const base::Value& value) {
  PrefService* state = profile->GetPrefs();
  const auto mode = PrivacyMode(value);
  if (!mode || !IsWebRtcIpMode(*mode) ||
      !state->FindPreference(prefs::kWebRTCIPHandlingPolicy) ||
      !state->IsUserModifiablePreference(prefs::kWebRTCIPHandlingPolicy)) {
    return false;
  }
  // Leave per-URL handling, mDNS, port ranges and media permissions alone.
  state->SetString(prefs::kWebRTCIPHandlingPolicy, *mode);
  return true;
}

}  // namespace chrome::ohos::settings_internal
#endif  // CHROME_BROWSER_UI_OHOS_SHELL_WEBRTC_PREFS_OHOS_H_
