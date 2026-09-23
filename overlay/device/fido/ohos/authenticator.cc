// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "device/fido/ohos/authenticator.h"

#include <algorithm>
#include <array>
#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/location.h"
#include "base/no_destructor.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "components/device_event_log/device_event_log.h"
#include "crypto/hash.h"
#include "crypto/random.h"
#include "device/fido/attestation_object.h"
#include "device/fido/attestation_statement_formats.h"
#include "device/fido/attested_credential_data.h"
#include "device/fido/authenticator_get_assertion_response.h"
#include "device/fido/authenticator_make_credential_response.h"
#include "device/fido/ctap_get_assertion_request.h"
#include "device/fido/ctap_make_credential_request.h"
#include "device/fido/discoverable_credential_metadata.h"
#include "device/fido/ohos/credential_store.h"
#include "device/fido/ohos/user_auth.h"
#include "device/fido/public/fido_constants.h"
#include "device/fido/public/fido_transport_protocol.h"
#include "device/fido/public/public_key_credential_params.h"
#include "device/fido/public_key.h"

namespace device::fido::ohos {

namespace internal {

// A new key, generated and with a signing session open, waiting for the user
// to authenticate. Nothing is written to the credential file until the
// signature succeeds, so an abandoned registration leaves no passkey behind.
struct PreparedCredential {
  std::vector<uint8_t> credential_id;
  std::string key_alias;
  std::vector<uint8_t> public_key_spki;
  SigningSession session;
};

// The credential chosen for an assertion, with its signing session open.
struct PreparedAssertion {
  StoredCredential credential;
  SigningSession session;
};

struct MakeCredentialResult {
  MakeCredentialStatus status =
      MakeCredentialStatus::kAuthenticatorResponseInvalid;
  std::vector<uint8_t> signature;
};

}  // namespace internal

namespace {

using internal::MakeCredentialResult;
using internal::PreparedAssertion;
using internal::PreparedCredential;

// 16 random bytes are ample for uniqueness, and keep the HUKS alias (prefix +
// 32 hex digits) inside OH_HUKS_MAX_KEY_ALIAS_LEN.
constexpr size_t kCredentialIdLength = 16;

// There is no attestation certificate, so the AAGUID carries no information;
// all zeros is what WebAuthn prescribes for that case.
constexpr std::array<uint8_t, kAaguidLength> kZeroAaguid = {};

constexpr int32_t kEs256 =
    static_cast<int32_t>(CoseAlgorithmIdentifier::kEs256);

// Sign counters are fixed at zero, as for macOS passkeys: the key cannot keep
// a counter itself, and zero tells relying parties not to check one.
constexpr uint32_t kSignCounter = 0;

std::unique_ptr<PublicKey> ParsePublicKey(base::span<const uint8_t> exported) {
  // HUKS exports X.509 SubjectPublicKeyInfo. Accept a bare X9.62 point as
  // well, which is the other format keystores commonly produce.
  std::unique_ptr<PublicKey> key = PublicKey::FromSpkiDer(kEs256, exported);
  if (!key) {
    key = PublicKey::FromRawP256UncompressedPoint(kEs256, exported);
  }
  return key;
}

AuthenticatorData MakeAuthenticatorData(
    const std::string& rp_id,
    std::optional<AttestedCredentialData> attested_credential_data) {
  // User presence and verification are both true: the system prompt is an
  // explicit user action, and HUKS refuses to sign without it. Backup
  // eligibility is false because HUKS keys never leave the device.
  return AuthenticatorData(crypto::hash::Sha256(rp_id), /*user_present=*/true,
                           /*user_verified=*/true, /*backup_eligible=*/false,
                           /*backup_state=*/false, kSignCounter,
                           std::move(attested_credential_data),
                           /*extensions=*/std::nullopt);
}

// What WebAuthn signs: authenticatorData || clientDataHash.
std::vector<uint8_t> SignedData(
    const AuthenticatorData& authenticator_data,
    base::span<const uint8_t, kClientDataHashLength> client_data_hash) {
  std::vector<uint8_t> data = authenticator_data.SerializeToByteArray();
  data.insert(data.end(), client_data_hash.begin(), client_data_hash.end());
  return data;
}

// --- Steps that run on GetHuksTaskRunner(). ---

std::optional<PreparedCredential> PrepareCredential() {
  PreparedCredential prepared;
  prepared.credential_id = crypto::RandBytesAsVector(kCredentialIdLength);
  prepared.key_alias = KeyAliasForCredentialId(prepared.credential_id);
  std::optional<std::vector<uint8_t>> public_key =
      GenerateSigningKey(prepared.key_alias, /*error_code=*/nullptr);
  if (!public_key) {
    return std::nullopt;
  }
  std::optional<SigningSession> session =
      BeginSigning(prepared.key_alias, /*error_code=*/nullptr);
  if (!session) {
    DeleteSigningKey(prepared.key_alias);
    return std::nullopt;
  }
  prepared.public_key_spki = std::move(*public_key);
  prepared.session = std::move(*session);
  return prepared;
}

void DiscardPreparedCredential(PreparedCredential prepared) {
  AbortSigning(prepared.session);
  DeleteSigningKey(prepared.key_alias);
}

MakeCredentialResult CommitCredential(
    base::FilePath profile_path,
    std::vector<PublicKeyCredentialDescriptor> exclude_list,
    StoredCredential credential,
    PreparedCredential prepared,
    std::vector<uint8_t> auth_token,
    std::vector<uint8_t> signed_data) {
  MakeCredentialResult result;
  CredentialStore store(profile_path);

  // Checked only now, after the user has authenticated: an excluded
  // credential tells the relying party that this device is already
  // registered, which WebAuthn allows only with the user's consent.
  if (!exclude_list.empty()) {
    std::optional<std::vector<StoredCredential>> excluded =
        store.Find(credential.rp_id, exclude_list);
    if (!excluded || !excluded->empty()) {
      DiscardPreparedCredential(std::move(prepared));
      result.status =
          excluded ? MakeCredentialStatus::kUserConsentButCredentialExcluded
                   : MakeCredentialStatus::kAuthenticatorResponseInvalid;
      return result;
    }
  }

  SignResult sign_result =
      FinishSigning(prepared.session, auth_token, signed_data);
  if (sign_result.status != SignStatus::kSuccess) {
    DeleteSigningKey(prepared.key_alias);
    result.status = sign_result.status == SignStatus::kAuthRejected
                        ? MakeCredentialStatus::kUserConsentDenied
                        : MakeCredentialStatus::kAuthenticatorResponseInvalid;
    return result;
  }

  if (!store.Add(credential)) {
    DeleteSigningKey(prepared.key_alias);
    return result;
  }
  result.status = MakeCredentialStatus::kSuccess;
  result.signature = std::move(sign_result.signature);
  return result;
}

std::optional<PreparedAssertion> PrepareAssertion(
    base::FilePath profile_path,
    std::string rp_id,
    std::vector<PublicKeyCredentialDescriptor> allow_list) {
  CredentialStore store(profile_path);
  std::optional<std::vector<StoredCredential>> credentials =
      store.Find(rp_id, allow_list);
  if (!credentials || credentials->empty()) {
    return std::nullopt;
  }
  // With an empty allow list the account picker has normally narrowed the
  // request to one credential already (GetAssertionRequestHandler's
  // preselection). If not, use the newest, since every other credential would
  // need its own authentication.
  StoredCredential& credential = credentials->front();
  const std::string alias = KeyAliasForCredentialId(credential.credential_id);
  int32_t error_code = 0;
  std::optional<SigningSession> session = BeginSigning(alias, &error_code);
  if (!session) {
    return std::nullopt;
  }
  PreparedAssertion prepared;
  prepared.credential = std::move(credential);
  prepared.session = std::move(*session);
  return prepared;
}

void AbortPreparedAssertion(PreparedAssertion prepared) {
  AbortSigning(prepared.session);
}

AuthenticatorSupportedOptions MakeOptions() {
  AuthenticatorSupportedOptions options;
  options.is_platform_device =
      AuthenticatorSupportedOptions::PlatformDevice::kYes;
  options.supports_resident_key = true;
  // Every device that passes IsAvailable() has at least a lock-screen PIN.
  options.user_verification_availability = AuthenticatorSupportedOptions::
      UserVerificationAvailability::kSupportedAndConfigured;
  options.supports_user_presence = true;
  return options;
}

}  // namespace

// static
void OhosPlatformAuthenticator::IsAvailable(
    base::OnceCallback<void(bool)> callback) {
  IsUserAuthAvailable(std::move(callback));
}

OhosPlatformAuthenticator::OhosPlatformAuthenticator(AuthenticatorConfig config)
    : config_(std::move(config)) {}

OhosPlatformAuthenticator::~OhosPlatformAuthenticator() {
  ReleasePendingOperation();
}

void OhosPlatformAuthenticator::InitializeAuthenticator(
    base::OnceClosure callback) {
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
                                                           std::move(callback));
}

void OhosPlatformAuthenticator::MakeCredential(
    CtapMakeCredentialRequest request,
    MakeCredentialOptions options,
    MakeCredentialCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!std::ranges::contains(
          request.public_key_credential_params.public_key_credential_params(),
          kEs256, &PublicKeyCredentialParams::CredentialInfo::algorithm)) {
    FIDO_LOG(ERROR) << "OHOS platform authenticator supports only ES256";
    std::move(callback).Run(MakeCredentialStatus::kNoCommonAlgorithms,
                            std::nullopt);
    return;
  }
  GetHuksTaskRunner()->PostTaskAndReplyWithResult(
      FROM_HERE, base::BindOnce(&PrepareCredential),
      base::BindOnce(&OhosPlatformAuthenticator::OnCredentialPrepared,
                     operation_weak_factory_.GetWeakPtr(), std::move(request),
                     std::move(callback)));
}

// static
void OhosPlatformAuthenticator::OnCredentialPrepared(
    base::WeakPtr<OhosPlatformAuthenticator> authenticator,
    CtapMakeCredentialRequest request,
    MakeCredentialCallback callback,
    std::optional<PreparedCredential> prepared) {
  if (!authenticator) {
    // Cancelled while the key was being generated: nobody will commit it.
    if (prepared) {
      GetHuksTaskRunner()->PostTask(
          FROM_HERE,
          base::BindOnce(&DiscardPreparedCredential, std::move(*prepared)));
    }
    return;
  }
  if (!prepared) {
    std::move(callback).Run(MakeCredentialStatus::kAuthenticatorResponseInvalid,
                            std::nullopt);
    return;
  }
  authenticator->pending_session_ = prepared->session;
  authenticator->pending_new_key_alias_ = prepared->key_alias;
  const std::vector<uint8_t> challenge = prepared->session.challenge;
  // The prompt's title names the site. There is no localised Chromium string
  // for it outside macOS, and the system prompt supplies its own wording.
  const std::string title = request.rp.id;
  AuthenticateUser(
      challenge, title,
      base::BindOnce(
          &OhosPlatformAuthenticator::OnMakeCredentialUserAuthenticated,
          authenticator->operation_weak_factory_.GetWeakPtr(),
          std::move(request), std::move(callback), std::move(*prepared)));
}

void OhosPlatformAuthenticator::OnMakeCredentialUserAuthenticated(
    CtapMakeCredentialRequest request,
    MakeCredentialCallback callback,
    PreparedCredential prepared,
    std::optional<std::vector<uint8_t>> auth_token) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // From here the posted task owns the session and the key.
  pending_session_.reset();
  pending_new_key_alias_.reset();

  if (!auth_token) {
    GetHuksTaskRunner()->PostTask(
        FROM_HERE,
        base::BindOnce(&DiscardPreparedCredential, std::move(prepared)));
    std::move(callback).Run(MakeCredentialStatus::kUserConsentDenied,
                            std::nullopt);
    return;
  }

  std::unique_ptr<PublicKey> public_key =
      ParsePublicKey(prepared.public_key_spki);
  if (!public_key) {
    FIDO_LOG(ERROR) << "Cannot parse the HUKS public key";
    GetHuksTaskRunner()->PostTask(
        FROM_HERE,
        base::BindOnce(&DiscardPreparedCredential, std::move(prepared)));
    std::move(callback).Run(MakeCredentialStatus::kAuthenticatorResponseInvalid,
                            std::nullopt);
    return;
  }
  AuthenticatorData authenticator_data = MakeAuthenticatorData(
      request.rp.id,
      AttestedCredentialData(kZeroAaguid, prepared.credential_id,
                             std::move(public_key)));
  // The registration signature is a self-attestation, as on macOS. Making it
  // is also what proves, inside HUKS, that the user authenticated for this
  // key before it is ever stored.
  std::vector<uint8_t> signed_data =
      SignedData(authenticator_data, request.client_data_hash);

  StoredCredential credential;
  credential.credential_id = prepared.credential_id;
  credential.rp_id = request.rp.id;
  credential.user = request.user;
  credential.created = base::Time::Now();

  GetHuksTaskRunner()->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&CommitCredential, config_.profile_path,
                     request.exclude_list, std::move(credential),
                     std::move(prepared), std::move(*auth_token),
                     std::move(signed_data)),
      base::BindOnce(&OhosPlatformAuthenticator::OnCredentialCommitted,
                     operation_weak_factory_.GetWeakPtr(), std::move(callback),
                     std::move(authenticator_data)));
}

void OhosPlatformAuthenticator::OnCredentialCommitted(
    MakeCredentialCallback callback,
    AuthenticatorData authenticator_data,
    MakeCredentialResult result) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (result.status != MakeCredentialStatus::kSuccess) {
    std::move(callback).Run(result.status, std::nullopt);
    return;
  }
  AuthenticatorMakeCredentialResponse response(
      FidoTransportProtocol::kInternal,
      AttestationObject(
          std::move(authenticator_data),
          std::make_unique<PackedAttestationStatement>(
              CoseAlgorithmIdentifier::kEs256, std::move(result.signature),
              /*x509_certificates=*/std::vector<std::vector<uint8_t>>())));
  response.is_resident_key = true;
  response.transports.emplace();
  response.transports->insert(FidoTransportProtocol::kInternal);
  std::move(callback).Run(MakeCredentialStatus::kSuccess, std::move(response));
}

void OhosPlatformAuthenticator::GetAssertion(CtapGetAssertionRequest request,
                                             CtapGetAssertionOptions options,
                                             GetAssertionCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  GetHuksTaskRunner()->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&PrepareAssertion, config_.profile_path, request.rp_id,
                     request.allow_list),
      base::BindOnce(&OhosPlatformAuthenticator::OnAssertionPrepared,
                     operation_weak_factory_.GetWeakPtr(), std::move(request),
                     std::move(callback)));
}

// static
void OhosPlatformAuthenticator::OnAssertionPrepared(
    base::WeakPtr<OhosPlatformAuthenticator> authenticator,
    CtapGetAssertionRequest request,
    GetAssertionCallback callback,
    std::optional<PreparedAssertion> prepared) {
  if (!authenticator) {
    if (prepared) {
      GetHuksTaskRunner()->PostTask(
          FROM_HERE,
          base::BindOnce(&AbortPreparedAssertion, std::move(*prepared)));
    }
    return;
  }
  if (!prepared) {
    // No usable credential. Like macOS, say so before any prompt: the account
    // picker only offers credentials this authenticator listed, so reaching
    // here means the relying party's allow list matched nothing.
    std::move(callback).Run(
        GetAssertionStatus::kUserConsentButCredentialNotRecognized, {});
    return;
  }
  authenticator->pending_session_ = prepared->session;
  const std::vector<uint8_t> challenge = prepared->session.challenge;
  const std::string title = request.rp_id;
  AuthenticateUser(
      challenge, title,
      base::BindOnce(
          &OhosPlatformAuthenticator::OnGetAssertionUserAuthenticated,
          authenticator->operation_weak_factory_.GetWeakPtr(),
          std::move(request), std::move(callback), std::move(*prepared)));
}

void OhosPlatformAuthenticator::OnGetAssertionUserAuthenticated(
    CtapGetAssertionRequest request,
    GetAssertionCallback callback,
    PreparedAssertion prepared,
    std::optional<std::vector<uint8_t>> auth_token) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  pending_session_.reset();
  if (!auth_token) {
    GetHuksTaskRunner()->PostTask(
        FROM_HERE,
        base::BindOnce(&AbortPreparedAssertion, std::move(prepared)));
    std::move(callback).Run(GetAssertionStatus::kUserConsentDenied, {});
    return;
  }

  AuthenticatorData authenticator_data =
      MakeAuthenticatorData(request.rp_id, std::nullopt);
  std::vector<uint8_t> signed_data =
      SignedData(authenticator_data, request.client_data_hash);
  const SigningSession session = prepared.session;
  GetHuksTaskRunner()->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(
          [](SigningSession session, std::vector<uint8_t> auth_token,
             std::vector<uint8_t> signed_data) {
            return FinishSigning(session, auth_token, signed_data);
          },
          session, std::move(*auth_token), std::move(signed_data)),
      base::BindOnce(&OhosPlatformAuthenticator::OnAssertionSigned,
                     operation_weak_factory_.GetWeakPtr(), std::move(callback),
                     std::move(prepared), std::move(authenticator_data)));
}

void OhosPlatformAuthenticator::OnAssertionSigned(
    GetAssertionCallback callback,
    PreparedAssertion prepared,
    AuthenticatorData authenticator_data,
    SignResult result) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  switch (result.status) {
    case SignStatus::kSuccess:
      break;
    case SignStatus::kKeyInvalidated:
      // The lock screen was removed after this passkey was made, so HUKS
      // destroyed its key. Forget the metadata too, or the account picker
      // would keep offering a passkey that can never sign.
      GetHuksTaskRunner()->PostTask(
          FROM_HERE,
          base::BindOnce(
              [](base::FilePath profile_path,
                 std::vector<uint8_t> credential_id) {
                CredentialStore(profile_path).Remove(credential_id);
              },
              config_.profile_path, prepared.credential.credential_id));
      std::move(callback).Run(
          GetAssertionStatus::kUserConsentButCredentialNotRecognized, {});
      return;
    case SignStatus::kAuthRejected:
      std::move(callback).Run(GetAssertionStatus::kUserConsentDenied, {});
      return;
    case SignStatus::kError:
      std::move(callback).Run(GetAssertionStatus::kAuthenticatorResponseInvalid,
                              {});
      return;
  }

  AuthenticatorGetAssertionResponse response(std::move(authenticator_data),
                                             std::move(result.signature),
                                             FidoTransportProtocol::kInternal);
  response.credential = PublicKeyCredentialDescriptor(
      CredentialType::kPublicKey, prepared.credential.credential_id);
  // Always return the user handle: every credential here is discoverable, and
  // relying parties need it to find the account after an empty-allow-list
  // request.
  response.user_entity = prepared.credential.user;
  std::vector<AuthenticatorGetAssertionResponse> responses;
  responses.push_back(std::move(response));
  std::move(callback).Run(GetAssertionStatus::kSuccess, std::move(responses));
}

void OhosPlatformAuthenticator::GetPlatformCredentialInfoForRequest(
    const CtapGetAssertionRequest& request,
    const CtapGetAssertionOptions& options,
    GetPlatformCredentialInfoForRequestCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  GetHuksTaskRunner()->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(
          [](base::FilePath profile_path, std::string rp_id,
             std::vector<PublicKeyCredentialDescriptor> allow_list) {
            return CredentialStore(profile_path).Find(rp_id, allow_list);
          },
          config_.profile_path, request.rp_id, request.allow_list),
      base::BindOnce(
          [](GetPlatformCredentialInfoForRequestCallback callback,
             std::optional<std::vector<StoredCredential>> credentials) {
            // Listing every match, as macOS does, is what lets the account
            // picker offer these passkeys for an empty allow list.
            std::vector<DiscoverableCredentialMetadata> result;
            for (const StoredCredential& credential :
                 credentials.value_or(std::vector<StoredCredential>())) {
              result.emplace_back(AuthenticatorType::kChromeOS,
                                  credential.rp_id, credential.credential_id,
                                  credential.user,
                                  /*provider_name=*/std::nullopt);
            }
            const bool has_credentials = !result.empty();
            std::move(callback).Run(
                std::move(result),
                has_credentials ? FidoRequestHandlerBase::RecognizedCredential::
                                      kHasRecognizedCredential
                                : FidoRequestHandlerBase::RecognizedCredential::
                                      kNoRecognizedCredential);
          },
          std::move(callback)));
}

void OhosPlatformAuthenticator::Cancel() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // Drops the replies of whatever step is in flight. The system prompt, if
  // showing, cannot be dismissed from here; its answer is ignored.
  operation_weak_factory_.InvalidateWeakPtrs();
  ReleasePendingOperation();
}

void OhosPlatformAuthenticator::ReleasePendingOperation() {
  if (pending_session_) {
    GetHuksTaskRunner()->PostTask(
        FROM_HERE, base::BindOnce(&AbortSigning, *pending_session_));
    pending_session_.reset();
  }
  if (pending_new_key_alias_) {
    GetHuksTaskRunner()->PostTask(
        FROM_HERE,
        base::BindOnce(base::IgnoreResult(&DeleteSigningKey),
                       *pending_new_key_alias_));
    pending_new_key_alias_.reset();
  }
}

AuthenticatorType OhosPlatformAuthenticator::GetType() const {
  // There is no AuthenticatorType for HarmonyOS yet. kChromeOS, the other
  // "platform authenticator provided by the OS" type, is already treated the
  // right way everywhere it is switched on: its excluded credentials surface
  // as InvalidStateError, it is not a Google Password Manager authenticator,
  // and its passkeys are listed with an "other" source. Nothing that is
  // specific to ChromeOS is compiled on OHOS.
  return AuthenticatorType::kChromeOS;
}

std::string OhosPlatformAuthenticator::GetId() const {
  return "OhosPlatformAuthenticator";
}

const AuthenticatorSupportedOptions& OhosPlatformAuthenticator::Options()
    const {
  static const base::NoDestructor<AuthenticatorSupportedOptions> options(
      MakeOptions());
  return *options;
}

std::optional<FidoTransportProtocol>
OhosPlatformAuthenticator::AuthenticatorTransport() const {
  return FidoTransportProtocol::kInternal;
}

base::WeakPtr<FidoAuthenticator> OhosPlatformAuthenticator::GetWeakPtr() {
  return weak_factory_.GetWeakPtr();
}

}  // namespace device::fido::ohos
