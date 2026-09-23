// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "device/fido/ohos/huks_signing_key.h"

#include <huks/native_huks_api.h>
#include <huks/native_huks_param.h>

#include <utility>

#include "base/memory/raw_ptr_exclusion.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "components/device_event_log/device_event_log.h"
#include "third_party/boringssl/src/include/openssl/bn.h"
#include "third_party/boringssl/src/include/openssl/ecdsa.h"
#include "third_party/boringssl/src/include/openssl/mem.h"

namespace device::fido::ohos {

namespace {

// Keeps these keys apart from anything else the app stores in HUKS.
constexpr char kKeyAliasPrefix[] = "chromium.webauthn.";

// A DER SubjectPublicKeyInfo for P-256 is 91 bytes; leave room.
constexpr size_t kPublicKeyBufferSize = 256;
// A DER ECDSA P-256 signature is at most 72 bytes.
constexpr size_t kSignatureBufferSize = 128;
// P-256 raw r||s, in case HUKS does not DER-encode.
constexpr size_t kRawP256SignatureSize = 64;

// Owns an OH_Huks_ParamSet. HUKS allocates it and must free it, which needs
// the address of the pointer, so it cannot live in a raw_ptr.
class ScopedParamSet {
 public:
  ScopedParamSet() = default;
  ScopedParamSet(const ScopedParamSet&) = delete;
  ScopedParamSet& operator=(const ScopedParamSet&) = delete;
  ~ScopedParamSet() {
    if (param_set_) {
      OH_Huks_FreeParamSet(&param_set_);
    }
  }

  bool Build(base::span<const OH_Huks_Param> params) {
    if (OH_Huks_InitParamSet(&param_set_).errorCode != OH_HUKS_SUCCESS) {
      return false;
    }
    if (OH_Huks_AddParams(param_set_, params.data(),
                          static_cast<uint32_t>(params.size()))
            .errorCode != OH_HUKS_SUCCESS) {
      return false;
    }
    return OH_Huks_BuildParamSet(&param_set_).errorCode == OH_HUKS_SUCCESS;
  }

  const OH_Huks_ParamSet* get() const { return param_set_; }

 private:
  RAW_PTR_EXCLUSION OH_Huks_ParamSet* param_set_ = nullptr;
};

OH_Huks_Param UintParam(uint32_t tag, uint32_t value) {
  OH_Huks_Param param = {};
  param.tag = tag;
  param.uint32Param = value;
  return param;
}

// HUKS takes non-const blobs even for inputs it only reads.
OH_Huks_Blob InputBlob(base::span<const uint8_t> data) {
  return {static_cast<uint32_t>(data.size()),
          const_cast<uint8_t*>(data.data())};
}

OH_Huks_Blob AliasBlob(const std::string& alias) {
  return InputBlob(base::as_byte_span(alias));
}

// The parameters of every signing operation. Initialising and finishing a
// session take the same set, plus the AuthToken on finish.
std::vector<OH_Huks_Param> SigningParams() {
  return {
      UintParam(OH_HUKS_TAG_ALGORITHM, OH_HUKS_ALG_ECC),
      UintParam(OH_HUKS_TAG_PURPOSE, OH_HUKS_KEY_PURPOSE_SIGN),
      UintParam(OH_HUKS_TAG_KEY_SIZE, OH_HUKS_ECC_KEY_SIZE_256),
      UintParam(OH_HUKS_TAG_DIGEST, OH_HUKS_DIGEST_SHA256),
  };
}

// WebAuthn requires ASN.1 DER signatures for ES256. HUKS documents DER output
// for ECC, but convert raw r||s defensively: a malformed signature would only
// show up as every relying party rejecting every assertion.
std::optional<std::vector<uint8_t>> ToDerSignature(
    base::span<const uint8_t> signature) {
  bssl::UniquePtr<ECDSA_SIG> parsed(
      ECDSA_SIG_from_bytes(signature.data(), signature.size()));
  if (parsed) {
    return std::vector<uint8_t>(signature.begin(), signature.end());
  }
  if (signature.size() != kRawP256SignatureSize) {
    return std::nullopt;
  }
  bssl::UniquePtr<ECDSA_SIG> sig(ECDSA_SIG_new());
  BIGNUM* r = BN_bin2bn(signature.data(), kRawP256SignatureSize / 2, nullptr);
  BIGNUM* s = BN_bin2bn(signature.data() + kRawP256SignatureSize / 2,
                        kRawP256SignatureSize / 2, nullptr);
  if (!sig || !r || !s || !ECDSA_SIG_set0(sig.get(), r, s)) {
    BN_free(r);
    BN_free(s);
    return std::nullopt;
  }
  uint8_t* der = nullptr;
  size_t der_len = 0;
  if (!ECDSA_SIG_to_bytes(&der, &der_len, sig.get())) {
    return std::nullopt;
  }
  bssl::UniquePtr<uint8_t> der_owner(der);
  return std::vector<uint8_t>(der, der + der_len);
}

}  // namespace

SigningSession::SigningSession() = default;
SigningSession::SigningSession(const SigningSession&) = default;
SigningSession& SigningSession::operator=(const SigningSession&) = default;
SigningSession::~SigningSession() = default;

SignResult::SignResult() = default;
SignResult::SignResult(SignResult&&) = default;
SignResult& SignResult::operator=(SignResult&&) = default;
SignResult::~SignResult() = default;

scoped_refptr<base::SequencedTaskRunner> GetHuksTaskRunner() {
  // USER_BLOCKING: the user is looking at a WebAuthn prompt. SKIP_ON_SHUTDOWN:
  // the credential file is written atomically, so skipping a write loses at
  // most one new passkey, and blocking shutdown on HUKS IPC is worse.
  static base::NoDestructor<scoped_refptr<base::SequencedTaskRunner>> runner(
      base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::USER_BLOCKING,
           base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN}));
  return *runner;
}

std::string KeyAliasForCredentialId(base::span<const uint8_t> credential_id) {
  return kKeyAliasPrefix + base::HexEncode(credential_id);
}

std::optional<std::vector<uint8_t>> GenerateSigningKey(const std::string& alias,
                                                       int32_t* error_code) {
  std::vector<OH_Huks_Param> params = SigningParams();
  // Bind the key to the device user. Any of face, fingerprint or the
  // lock-screen PIN may unlock it, matching what UserAuthService offers.
  params.push_back(UintParam(OH_HUKS_TAG_USER_AUTH_TYPE,
                             OH_HUKS_USER_AUTH_TYPE_FINGERPRINT |
                                 OH_HUKS_USER_AUTH_TYPE_FACE |
                                 OH_HUKS_USER_AUTH_TYPE_PIN));
  // Enrolling another fingerprint should not destroy passkeys, but removing
  // the lock screen should: without it, "user verification" would mean
  // nothing.
  params.push_back(UintParam(OH_HUKS_TAG_KEY_AUTH_ACCESS_TYPE,
                             OH_HUKS_AUTH_ACCESS_INVALID_CLEAR_PASSWORD));
  // One authentication per signature, bound to that session's challenge,
  // rather than a time window in which any signature is allowed.
  params.push_back(
      UintParam(OH_HUKS_TAG_CHALLENGE_TYPE, OH_HUKS_CHALLENGE_TYPE_NORMAL));
  params.push_back(
      UintParam(OH_HUKS_TAG_KEY_AUTH_PURPOSE, OH_HUKS_KEY_PURPOSE_SIGN));

  ScopedParamSet param_set;
  if (!param_set.Build(params)) {
    FIDO_LOG(ERROR) << "HUKS: building key generation params failed";
    return std::nullopt;
  }
  OH_Huks_Blob alias_blob = AliasBlob(alias);
  OH_Huks_Result result =
      OH_Huks_GenerateKeyItem(&alias_blob, param_set.get(), nullptr);
  if (error_code) {
    *error_code = result.errorCode;
  }
  if (result.errorCode != OH_HUKS_SUCCESS) {
    FIDO_LOG(ERROR) << "HUKS: GenerateKeyItem failed: " << result.errorCode;
    return std::nullopt;
  }

  // The export takes a parameter set too; the documentation does not say it
  // may be null, so describe the key instead of relying on that.
  ScopedParamSet export_params;
  if (!export_params.Build(SigningParams())) {
    DeleteSigningKey(alias);
    return std::nullopt;
  }
  std::vector<uint8_t> public_key(kPublicKeyBufferSize);
  OH_Huks_Blob public_key_blob = {static_cast<uint32_t>(public_key.size()),
                                  public_key.data()};
  result = OH_Huks_ExportPublicKeyItem(&alias_blob, export_params.get(),
                                       &public_key_blob);
  if (error_code) {
    *error_code = result.errorCode;
  }
  if (result.errorCode != OH_HUKS_SUCCESS) {
    FIDO_LOG(ERROR) << "HUKS: ExportPublicKeyItem failed: "
                    << result.errorCode;
    DeleteSigningKey(alias);
    return std::nullopt;
  }
  public_key.resize(public_key_blob.size);
  return public_key;
}

bool DeleteSigningKey(const std::string& alias) {
  OH_Huks_Blob alias_blob = AliasBlob(alias);
  OH_Huks_Result result = OH_Huks_DeleteKeyItem(&alias_blob, nullptr);
  if (result.errorCode != OH_HUKS_SUCCESS &&
      result.errorCode != OH_HUKS_ERR_CODE_ITEM_NOT_EXIST) {
    FIDO_LOG(ERROR) << "HUKS: DeleteKeyItem failed: " << result.errorCode;
    return false;
  }
  return true;
}

std::optional<SigningSession> BeginSigning(const std::string& alias,
                                           int32_t* error_code) {
  ScopedParamSet param_set;
  if (!param_set.Build(SigningParams())) {
    FIDO_LOG(ERROR) << "HUKS: building signing params failed";
    return std::nullopt;
  }
  SigningSession session;
  // HUKS writes the challenge into `token`; TOKEN_CHALLENGE_LEN is the most it
  // produces, and also the most UserAuthenticationKit accepts.
  std::vector<uint8_t> challenge(TOKEN_CHALLENGE_LEN);
  OH_Huks_Blob alias_blob = AliasBlob(alias);
  OH_Huks_Blob handle_blob = {static_cast<uint32_t>(session.handle.size()),
                              session.handle.data()};
  OH_Huks_Blob challenge_blob = {static_cast<uint32_t>(challenge.size()),
                                 challenge.data()};
  OH_Huks_Result result = OH_Huks_InitSession(&alias_blob, param_set.get(),
                                              &handle_blob, &challenge_blob);
  if (error_code) {
    *error_code = result.errorCode;
  }
  if (result.errorCode != OH_HUKS_SUCCESS) {
    FIDO_LOG(ERROR) << "HUKS: InitSession failed: " << result.errorCode;
    return std::nullopt;
  }
  challenge.resize(challenge_blob.size);
  if (challenge.empty()) {
    // Without a challenge the AuthToken is not bound to this signature. The
    // key was generated with CHALLENGE_TYPE_NORMAL, so this should not happen.
    FIDO_LOG(ERROR) << "HUKS: InitSession returned no challenge";
    AbortSigning(session);
    return std::nullopt;
  }
  session.challenge = std::move(challenge);
  return session;
}

SignResult FinishSigning(const SigningSession& session,
                         base::span<const uint8_t> auth_token,
                         base::span<const uint8_t> data) {
  SignResult sign_result;
  std::vector<OH_Huks_Param> params = SigningParams();
  OH_Huks_Param token_param = {};
  token_param.tag = OH_HUKS_TAG_AUTH_TOKEN;
  token_param.blob = InputBlob(auth_token);
  params.push_back(token_param);

  ScopedParamSet param_set;
  if (!param_set.Build(params)) {
    FIDO_LOG(ERROR) << "HUKS: building finish params failed";
    AbortSigning(session);
    return sign_result;
  }

  std::array<uint8_t, sizeof(uint64_t)> handle = session.handle;
  OH_Huks_Blob handle_blob = {static_cast<uint32_t>(handle.size()),
                              handle.data()};
  OH_Huks_Blob in_blob = InputBlob(data);
  std::vector<uint8_t> signature(kSignatureBufferSize);
  OH_Huks_Blob out_blob = {static_cast<uint32_t>(signature.size()),
                           signature.data()};
  OH_Huks_Result result =
      OH_Huks_FinishSession(&handle_blob, param_set.get(), &in_blob, &out_blob);
  switch (result.errorCode) {
    case OH_HUKS_SUCCESS:
      break;
    case OH_HUKS_ERR_CODE_KEY_AUTH_VERIFY_FAILED:
    case OH_HUKS_ERR_CODE_KEY_AUTH_TIME_OUT:
      FIDO_LOG(ERROR) << "HUKS: AuthToken rejected: " << result.errorCode;
      sign_result.status = SignStatus::kAuthRejected;
      return sign_result;
    case OH_HUKS_ERR_CODE_KEY_AUTH_PERMANENTLY_INVALIDATED:
    case OH_HUKS_ERR_CODE_ITEM_NOT_EXIST:
      FIDO_LOG(ERROR) << "HUKS: key no longer usable: " << result.errorCode;
      sign_result.status = SignStatus::kKeyInvalidated;
      return sign_result;
    default:
      FIDO_LOG(ERROR) << "HUKS: FinishSession failed: " << result.errorCode;
      return sign_result;
  }
  signature.resize(out_blob.size);
  std::optional<std::vector<uint8_t>> der = ToDerSignature(signature);
  if (!der) {
    FIDO_LOG(ERROR) << "HUKS: unrecognised signature encoding, "
                    << signature.size() << " bytes";
    return sign_result;
  }
  sign_result.status = SignStatus::kSuccess;
  sign_result.signature = std::move(*der);
  return sign_result;
}

void AbortSigning(const SigningSession& session) {
  // As with the export, pass the operation's parameters rather than null.
  ScopedParamSet param_set;
  if (!param_set.Build(SigningParams())) {
    return;
  }
  std::array<uint8_t, sizeof(uint64_t)> handle = session.handle;
  OH_Huks_Blob handle_blob = {static_cast<uint32_t>(handle.size()),
                              handle.data()};
  OH_Huks_Result result = OH_Huks_AbortSession(&handle_blob, param_set.get());
  if (result.errorCode != OH_HUKS_SUCCESS) {
    FIDO_LOG(DEBUG) << "HUKS: AbortSession: " << result.errorCode;
  }
}

}  // namespace device::fido::ohos
