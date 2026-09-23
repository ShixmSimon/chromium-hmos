// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DEVICE_FIDO_OHOS_CREDENTIAL_STORE_H_
#define DEVICE_FIDO_OHOS_CREDENTIAL_STORE_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/time/time.h"
#include "device/fido/public/public_key_credential_descriptor.h"
#include "device/fido/public/public_key_credential_user_entity.h"

namespace device::fido::ohos {

// What the platform authenticator remembers about one passkey. The private key
// itself stays in HUKS under KeyAliasForCredentialId(credential_id).
struct StoredCredential {
  StoredCredential();
  StoredCredential(const StoredCredential&);
  StoredCredential(StoredCredential&&);
  StoredCredential& operator=(const StoredCredential&);
  StoredCredential& operator=(StoredCredential&&);
  ~StoredCredential();

  std::vector<uint8_t> credential_id;
  std::string rp_id;
  PublicKeyCredentialUserEntity user;
  base::Time created;
};

// The passkeys of one profile, kept in a JSON file in the profile directory.
//
// Unlike the macOS Touch ID store, which shares the login keychain with every
// other app and profile and so encrypts its metadata, this file sits in the
// app's own sandbox on credential-encrypted storage, and holds nothing that
// signs: user names and handles, like the rest of the profile's data.
//
// All methods do file I/O (and deletion does HUKS IPC), so they must run on
// GetHuksTaskRunner().
class CredentialStore {
 public:
  explicit CredentialStore(const base::FilePath& profile_path);
  CredentialStore(const CredentialStore&) = delete;
  CredentialStore& operator=(const CredentialStore&) = delete;
  ~CredentialStore();

  // All stored credentials, an empty list if there is no file yet, or nullopt
  // if the file exists but cannot be read. Callers must not write back after
  // nullopt, or a transient read error would erase every passkey.
  std::optional<std::vector<StoredCredential>> ReadAll() const;

  // Credentials for `rp_id`: those in `allow_list` if it is non-empty,
  // otherwise all of them (all are discoverable). Newest first.
  std::optional<std::vector<StoredCredential>> Find(
      const std::string& rp_id,
      const std::vector<PublicKeyCredentialDescriptor>& allow_list) const;

  // Adds `credential`, replacing any existing credential for the same RP and
  // user handle, whose HUKS keys are deleted. A relying party that registers
  // the same account twice expects the new passkey to take over, as on macOS.
  bool Add(const StoredCredential& credential);

  // Removes the credential with `credential_id` and its key, e.g. after HUKS
  // reports the key invalidated because the lock screen was removed.
  bool Remove(const std::vector<uint8_t>& credential_id);

 private:
  bool WriteAll(const std::vector<StoredCredential>& credentials) const;

  const base::FilePath file_path_;
};

}  // namespace device::fido::ohos

#endif  // DEVICE_FIDO_OHOS_CREDENTIAL_STORE_H_
