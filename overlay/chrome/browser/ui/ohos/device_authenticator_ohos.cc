// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/ohos/device_authenticator_ohos.h"

#include <algorithm>
#include <optional>
#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/logging.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/time/time.h"
#include "base/values.h"
#include "components/device_reauth/device_authenticator_common.h"
#include <BasicServicesKit/oh_commonevent.h>
#include <BasicServicesKit/oh_commonevent_support.h>

#include "components/ohos_system_service/system_service_ohos.h"

namespace chrome::ohos {

namespace {

// The name the HAR registers, not "userAuth": the shell cannot add a service
// of its own, the engine's SystemServices.ets owns this list, and there was
// already a UserAuthService there for WebAuthn. Passwords reuse it.
constexpr char kService[] = "userauth";

// When the user last proved who they are, and the window during which that
// still counts.
//
// Kept here rather than in DeviceAuthenticatorProxy, whose timestamp can be
// set but never cleared: going to the background, locking the screen and
// moving between ordinary and incognito windows all have to end the window
// early, and that is the whole point of it.
//
// A monotonic clock, so changing the system time cannot extend it.
std::optional<base::TimeTicks>& LastGoodAuth() {
  static std::optional<base::TimeTicks> when;
  return when;
}

// How long the last authentication counts for the shell's own password
// commands. The engine's prompts carry their own window in their params.
constexpr base::TimeDelta kMaxShellGrant = base::Seconds(60);
base::TimeDelta& ShellGrantValidity() {
  static base::TimeDelta validity = kMaxShellGrant;
  return validity;
}

// What the shell last said about enrolled credentials, or nothing when it has
// not answered: before the first answer, and after a failed question.
// Remembered because the Can... methods answer synchronously and the shell is
// a round trip away. "Nothing enrolled" opens the passwords without a prompt,
// so it has to be an answer, never a default.
std::optional<bool>& KnownAvailability() {
  static std::optional<bool> known;
  return known;
}

bool DeviceCanAuthenticate() {
  return KnownAvailability().value_or(false);
}

void RememberAvailability(std::optional<bool> available) {
  if (KnownAvailability() != available) {
    LOG(WARNING) << "OHOS password auth: the device "
                 << (!available ? "has not said whether it can"
                     : *available ? "can"
                                  : "cannot")
                 << " verify the user (lock screen or biometric)";
  }
  KnownAvailability() = available;
}

std::optional<bool> ReadAvailability(ohos_system_service::Reply& reply) {
  if (!reply.ok) {
    return std::nullopt;
  }
  return reply.result_dict().FindBool("available");
}

// Locking the screen ends the reuse window. Watched here rather than asked of
// the shell, because the shell may not be running when it happens and because
// the window is this side's to keep.
void WatchForScreenLock() {
  static bool watching = false;
  if (watching) {
    return;
  }
  static const char* const kEvents[] = {COMMON_EVENT_SCREEN_OFF,
                                        COMMON_EVENT_SCREEN_LOCKED};
  CommonEvent_SubscribeInfo* info = OH_CommonEvent_CreateSubscribeInfo(
      const_cast<const char**>(kEvents), std::size(kEvents));
  if (!info) {
    return;
  }
  CommonEvent_Subscriber* subscriber = OH_CommonEvent_CreateSubscriber(
      info, [](const CommonEvent_RcvData* data) {
        const char* event = data ? OH_CommonEvent_GetEventFromRcvData(data) : "";
        ForgetRecentAuthentication(event && *event ? event : "screen locked");
      });
  if (!subscriber) {
    OH_CommonEvent_DestroySubscribeInfo(info);
    return;
  }
  // Leaked on purpose: it lives as long as the browser does, and the callback
  // must outlive any teardown ordering.
  watching = OH_CommonEvent_Subscribe(subscriber) == COMMONEVENT_ERR_OK;
  LOG(WARNING) << "OHOS password auth: watching for screen lock=" << watching;
}

class DeviceAuthenticatorOhos
    : public DeviceAuthenticatorCommon {
 public:
  DeviceAuthenticatorOhos(DeviceAuthenticatorProxy* proxy,
                          const device_reauth::DeviceAuthParams& params)
      : DeviceAuthenticatorCommon(proxy,
                                  params.GetAuthenticationValidityPeriod(),
                                  params.GetAuthResultHistogram()),
        validity_(params.GetAuthenticationValidityPeriod()) {}

  // device_reauth::DeviceAuthenticator:
  bool CanAuthenticateWithBiometrics() override {
    // The shell reports one capability rather than two: whatever the user
    // enrolled at ATL3, which may be a PIN. Callers use this to choose
    // wording, and claiming biometrics we might not have would read worse
    // than the general phrasing.
    return false;
  }

  bool CanAuthenticateWithBiometricOrScreenLock() override {
    return DeviceCanAuthenticate();
  }

  void AuthenticateWithMessage(const std::u16string& message,
                               AuthenticateCallback callback) override {
    if (WithinReuseWindow()) {
      LOG(WARNING) << "OHOS password auth: granted from the recent check";
      std::move(callback).Run(true);
      return;
    }
    if (!ohos_system_service::IsAvailable()) {
      LOG(WARNING) << "OHOS password auth: refused, the shell is not attached";
      std::move(callback).Run(false);
      return;
    }
    pending_ = std::move(callback);
    if (!KnownAvailability().has_value()) {
      // Not known yet: ask first. Treating "not known" as "nothing enrolled"
      // would open the passwords of a phone that has a lock screen.
      ohos_system_service::Call(
          kService, "available", base::DictValue(),
          base::BindOnce(&DeviceAuthenticatorOhos::OnAvailability,
                         weak_factory_.GetWeakPtr(), message));
      return;
    }
    VerifyOrAllow(message);
  }

  void Cancel() override {
    if (!pending_) {
      return;
    }
    // Drop the answer rather than deliver it: the caller has gone, and a late
    // success would unlock something nobody is looking at.
    pending_.Reset();
    weak_factory_.InvalidateWeakPtrs();
    if (ohos_system_service::IsAvailable()) {
      ohos_system_service::Call(kService, "cancel", base::DictValue(),
                                base::DoNothing());
    }
  }

 private:
  bool WithinReuseWindow() const {
    const std::optional<base::TimeTicks> last = LastGoodAuth();
    // A zero window is how "always ask" is expressed -- exporting, and
    // turning the fill-time check off -- so it never reuses anything.
    return !validity_.is_zero() && last.has_value() &&
           base::TimeTicks::Now() - *last < validity_;
  }

  void OnAvailability(const std::u16string& message,
                      ohos_system_service::Reply reply) {
    RememberAvailability(ReadAvailability(reply));
    if (!KnownAvailability().has_value()) {
      LOG(WARNING) << "OHOS password auth: refused, the shell did not say "
                      "whether anything is enrolled";
      if (pending_) {
        std::move(pending_).Run(false);
      }
      return;
    }
    VerifyOrAllow(message);
  }

  void VerifyOrAllow(const std::u16string& message) {
    if (!*KnownAvailability()) {
      // No lock screen, no face, no fingerprint: there is nothing to verify
      // against, and the product's answer is to let the user in, as on a
      // desktop without a password. A device that has one still asks.
      LOG(WARNING) << "OHOS password auth: allowed, nothing enrolled to "
                      "verify against";
      if (pending_) {
        std::move(pending_).Run(true);
      }
      return;
    }
    base::DictValue args;
    args.Set("title", message);
    ohos_system_service::Call(
        kService, "verify", std::move(args),
        base::BindOnce(&DeviceAuthenticatorOhos::OnVerified,
                       weak_factory_.GetWeakPtr()));
  }

  void OnVerified(ohos_system_service::Reply reply) {
    const bool ok =
        reply.ok && reply.result_dict().FindBool("ok").value_or(false);
    LOG(WARNING) << "OHOS password auth: verified=" << ok
                 << (reply.ok ? std::string()
                              : " transportError=" + reply.error);
    if (ok) {
      LastGoodAuth() = base::TimeTicks::Now();
      ShellGrantValidity() = kMaxShellGrant;
      RememberAvailability(true);
    }
    if (pending_) {
      std::move(pending_).Run(ok);
    }
  }

  const base::TimeDelta validity_;
  AuthenticateCallback pending_;
  base::WeakPtrFactory<DeviceAuthenticatorOhos> weak_factory_{this};
};

}  // namespace

std::unique_ptr<device_reauth::DeviceAuthenticator> MakeOhosDeviceAuthenticator(
    const device_reauth::DeviceAuthParams& params) {
  if (!ohos_system_service::IsAvailable()) {
    return nullptr;
  }
  // The base class wants a proxy; the clock that matters is the one above.
  static base::NoDestructor<DeviceAuthenticatorProxy> proxy;
  return std::make_unique<DeviceAuthenticatorOhos>(proxy.get(), params);
}

bool CanAuthenticateOnThisDevice() {
  return DeviceCanAuthenticate();
}

void RefreshAuthenticationAvailability() {
  WatchForScreenLock();
  if (!ohos_system_service::IsAvailable()) {
    RememberAvailability(std::nullopt);
    return;
  }
  ohos_system_service::Call(
      kService, "available", base::DictValue(),
      base::BindOnce([](ohos_system_service::Reply reply) {
        RememberAvailability(ReadAvailability(reply));
      }));
}

void GrantAuthenticationFromShell(base::TimeDelta valid) {
  const base::TimeDelta validity =
      std::clamp(valid, base::TimeDelta(), kMaxShellGrant);
  LastGoodAuth() = base::TimeTicks::Now();
  ShellGrantValidity() = validity;
  LOG(WARNING) << "OHOS passwords: auth granted for "
               << validity.InMilliseconds() << "ms";
}

bool IsAuthenticationFresh() {
  const std::optional<base::TimeTicks> last = LastGoodAuth();
  return last.has_value() &&
         base::TimeTicks::Now() - *last < ShellGrantValidity();
}

void ForgetRecentAuthentication(const char* reason) {
  if (!LastGoodAuth().has_value()) {
    return;
  }
  LOG(WARNING) << "OHOS passwords: auth reset (" << reason << ")";
  LastGoodAuth().reset();
}

}  // namespace chrome::ohos
