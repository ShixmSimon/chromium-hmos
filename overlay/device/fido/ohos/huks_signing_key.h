// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DEVICE_FIDO_OHOS_HUKS_SIGNING_KEY_H_
#define DEVICE_FIDO_OHOS_HUKS_SIGNING_KEY_H_

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "base/containers/span.h"
#include "base/memory/scoped_refptr.h"

namespace base {
class SequencedTaskRunner;
}

namespace device::fido::ohos {

// P-256 signing keys held by HUKS (the HarmonyOS Universal Keystore) that can
// only be used after the device user has authenticated.
//
// Every function below except GetHuksTaskRunner() and KeyAliasForCredentialId()
// makes a synchronous IPC to the HUKS service, so they must run on
// GetHuksTaskRunner() and never on the UI thread.

// The one sequence on which all HUKS calls and credential-file I/O happen.
// Serialising them means two tabs creating passkeys at once cannot interleave
// a read-modify-write of the credential file.
scoped_refptr<base::SequencedTaskRunner> GetHuksTaskRunner();

// The HUKS alias of the key for `credential_id`. HUKS aliases are global to
// the app, not per profile; credential IDs are random, so they do not collide.
std::string KeyAliasForCredentialId(base::span<const uint8_t> credential_id);

// Generates a key under `alias` and returns its public half as a DER
// SubjectPublicKeyInfo, or nullopt on failure. `error_code` receives the HUKS
// error code when non-null.
std::optional<std::vector<uint8_t>> GenerateSigningKey(const std::string& alias,
                                                       int32_t* error_code);

// Deletes the key under `alias`. Deleting a key that does not exist succeeds.
bool DeleteSigningKey(const std::string& alias);

// An initialised HUKS signing operation that is waiting for an AuthToken.
struct SigningSession {
  SigningSession();
  SigningSession(const SigningSession&);
  SigningSession& operator=(const SigningSession&);
  ~SigningSession();

  // HUKS session handles are 64-bit.
  std::array<uint8_t, sizeof(uint64_t)> handle = {};
  // What the user authentication must be bound to: pass it as the userAuth
  // challenge, and the resulting AuthToken unlocks exactly this session.
  std::vector<uint8_t> challenge;
};

// Starts an ECDSA-SHA256 signing operation with the key under `alias`.
std::optional<SigningSession> BeginSigning(const std::string& alias,
                                           int32_t* error_code);

enum class SignStatus {
  kSuccess,
  // HUKS rejected the AuthToken: wrong, expired or for another challenge.
  kAuthRejected,
  // The key was invalidated, e.g. because the lock-screen password was
  // cleared after the credential was created.
  kKeyInvalidated,
  kError,
};

struct SignResult {
  SignResult();
  SignResult(SignResult&&);
  SignResult& operator=(SignResult&&);
  ~SignResult();

  SignStatus status = SignStatus::kError;
  // A DER-encoded ECDSA signature when `status` is kSuccess.
  std::vector<uint8_t> signature;
};

// Completes `session` over `data` using `auth_token` from
// UserAuthenticationKit.
// The session is consumed whether or not this succeeds.
SignResult FinishSigning(const SigningSession& session,
                         base::span<const uint8_t> auth_token,
                         base::span<const uint8_t> data);

// Releases a session that will not be finished. HUKS has a small per-process
// session limit, so an abandoned request must not leak one.
void AbortSigning(const SigningSession& session);

}  // namespace device::fido::ohos

#endif  // DEVICE_FIDO_OHOS_HUKS_SIGNING_KEY_H_
