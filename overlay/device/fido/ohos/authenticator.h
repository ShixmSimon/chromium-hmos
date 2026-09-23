// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DEVICE_FIDO_OHOS_AUTHENTICATOR_H_
#define DEVICE_FIDO_OHOS_AUTHENTICATOR_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "base/component_export.h"
#include "base/functional/callback_forward.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "device/fido/authenticator_data.h"
#include "device/fido/fido_authenticator.h"
#include "device/fido/ohos/authenticator_config.h"
#include "device/fido/ohos/huks_signing_key.h"

namespace device::fido::ohos {

namespace internal {
struct PreparedCredential;
struct PreparedAssertion;
struct MakeCredentialResult;
}  // namespace internal

// The HarmonyOS platform authenticator: passkeys whose private keys are P-256
// keys in HUKS, usable only after the device user authenticates with face,
// fingerprint or PIN through UserAuthenticationKit.
//
// Modelled on the macOS Touch ID authenticator. Every credential is
// discoverable, every operation performs user verification, only ES256 is
// offered, and attestation is self-attestation (as on macOS; relying parties
// that ask for "none" get none, because Chrome strips it).
//
// Every signature needs its own authentication: HUKS hands out a challenge
// per signing session, the system prompt binds the user's AuthToken to it, and
// HUKS signs only with that token. So user verification is enforced by the
// keystore, not merely asserted by this code.
class COMPONENT_EXPORT(DEVICE_FIDO) OhosPlatformAuthenticator
    : public FidoAuthenticator {
 public:
  // Whether the device can do user verification, i.e. has a lock screen.
  // Replies on the calling sequence. Backs
  // isUserVerifyingPlatformAuthenticatorAvailable().
  static void IsAvailable(base::OnceCallback<void(bool)> callback);

  explicit OhosPlatformAuthenticator(AuthenticatorConfig config);
  OhosPlatformAuthenticator(const OhosPlatformAuthenticator&) = delete;
  OhosPlatformAuthenticator& operator=(const OhosPlatformAuthenticator&) =
      delete;
  ~OhosPlatformAuthenticator() override;

  // FidoAuthenticator:
  void InitializeAuthenticator(base::OnceClosure callback) override;
  void MakeCredential(CtapMakeCredentialRequest request,
                      MakeCredentialOptions options,
                      MakeCredentialCallback callback) override;
  void GetAssertion(CtapGetAssertionRequest request,
                    CtapGetAssertionOptions options,
                    GetAssertionCallback callback) override;
  void GetPlatformCredentialInfoForRequest(
      const CtapGetAssertionRequest& request,
      const CtapGetAssertionOptions& options,
      GetPlatformCredentialInfoForRequestCallback callback) override;
  void Cancel() override;
  AuthenticatorType GetType() const override;
  std::string GetId() const override;
  const AuthenticatorSupportedOptions& Options() const override;
  std::optional<FidoTransportProtocol> AuthenticatorTransport() const override;
  base::WeakPtr<FidoAuthenticator> GetWeakPtr() override;

 private:
  using PreparedCredential = internal::PreparedCredential;
  using PreparedAssertion = internal::PreparedAssertion;
  using MakeCredentialResult = internal::MakeCredentialResult;

  // MakeCredential, in order. The HUKS steps run on GetHuksTaskRunner(); the
  // rest on this authenticator's sequence.
  static void OnCredentialPrepared(
      base::WeakPtr<OhosPlatformAuthenticator> authenticator,
      CtapMakeCredentialRequest request,
      MakeCredentialCallback callback,
      std::optional<PreparedCredential> prepared);
  void OnMakeCredentialUserAuthenticated(
      CtapMakeCredentialRequest request,
      MakeCredentialCallback callback,
      PreparedCredential prepared,
      std::optional<std::vector<uint8_t>> auth_token);
  void OnCredentialCommitted(MakeCredentialCallback callback,
                             AuthenticatorData authenticator_data,
                             MakeCredentialResult result);

  // GetAssertion, in order.
  static void OnAssertionPrepared(
      base::WeakPtr<OhosPlatformAuthenticator> authenticator,
      CtapGetAssertionRequest request,
      GetAssertionCallback callback,
      std::optional<PreparedAssertion> prepared);
  void OnGetAssertionUserAuthenticated(
      CtapGetAssertionRequest request,
      GetAssertionCallback callback,
      PreparedAssertion prepared,
      std::optional<std::vector<uint8_t>> auth_token);
  void OnAssertionSigned(GetAssertionCallback callback,
                         PreparedAssertion prepared,
                         AuthenticatorData authenticator_data,
                         SignResult result);

  // Aborts the outstanding HUKS session, if any, and deletes the key of a
  // credential that was generated but never committed.
  void ReleasePendingOperation();

  const AuthenticatorConfig config_;

  // The HUKS session waiting for user authentication, so that Cancel() can
  // release it; and, during MakeCredential, the alias of the not yet committed
  // key, so that Cancel() can delete it.
  std::optional<SigningSession> pending_session_;
  std::optional<std::string> pending_new_key_alias_;

  SEQUENCE_CHECKER(sequence_checker_);

  // Handed out through GetWeakPtr() to the request handler; never invalidated
  // while the authenticator lives.
  base::WeakPtrFactory<OhosPlatformAuthenticator> weak_factory_{this};
  // Bound into the steps of the current operation. Cancel() invalidates these
  // so that a late reply from HUKS or the prompt is dropped.
  base::WeakPtrFactory<OhosPlatformAuthenticator> operation_weak_factory_{
      this};
};

}  // namespace device::fido::ohos

#endif  // DEVICE_FIDO_OHOS_AUTHENTICATOR_H_
