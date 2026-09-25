// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/ohos/device_authenticator_ohos.h"

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

// What the shell last said about enrolled credentials. Remembered because the
// Can... methods answer synchronously and the shell is a round trip away.
bool& DeviceCanAuthenticate() {
  static bool can = false;
  return can;
}

void RememberAvailability(bool available) {
  if (DeviceCanAuthenticate() != available) {
    LOG(WARNING) << "OHOS password auth: the device "
                 << (available ? "can" : "cannot")
                 << " verify the user (lock screen or biometric)";
  }
  DeviceCanAuthenticate() = available;
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
    if (!DeviceCanAuthenticate()) {
      // No lock screen and nothing enrolled. Refusing is the point: opening
      // to whoever is holding the phone is worse than not opening.
      LOG(WARNING) << "OHOS password auth: refused, nothing enrolled to "
                      "verify against";
      std::move(callback).Run(false);
      return;
    }
    if (!ohos_system_service::IsAvailable()) {
      LOG(WARNING) << "OHOS password auth: refused, the shell is not attached";
      std::move(callback).Run(false);
      return;
    }

    base::DictValue args;
    args.Set("title", message);
    pending_ = std::move(callback);
    ohos_system_service::Call(
        kService, "verify", std::move(args),
        base::BindOnce(&DeviceAuthenticatorOhos::OnVerified,
                       weak_factory_.GetWeakPtr()));
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

  void OnVerified(ohos_system_service::Reply reply) {
    const bool ok =
        reply.ok && reply.result_dict().FindBool("ok").value_or(false);
    LOG(WARNING) << "OHOS password auth: verified=" << ok
                 << (reply.ok ? std::string()
                              : " transportError=" + reply.error);
    if (ok) {
      LastGoodAuth() = base::TimeTicks::Now();
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
    RememberAvailability(false);
    return;
  }
  ohos_system_service::Call(
      kService, "available", base::DictValue(),
      base::BindOnce([](ohos_system_service::Reply reply) {
        RememberAvailability(
            reply.ok &&
            reply.result_dict().FindBool("available").value_or(false));
      }));
}

void ForgetRecentAuthentication(const char* reason) {
  if (!LastGoodAuth().has_value()) {
    return;
  }
  LOG(WARNING) << "OHOS password auth: the recent check no longer counts ("
               << reason << ")";
  LastGoodAuth().reset();
}

}  // namespace chrome::ohos
