// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "device/fido/ohos/credential_store.h"

#include <algorithm>
#include <utility>

#include "base/base64.h"
#include "base/files/file_util.h"
#include "base/files/important_file_writer.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/values.h"
#include "components/device_event_log/device_event_log.h"
#include "device/fido/ohos/huks_signing_key.h"

namespace device::fido::ohos {

namespace {

// Lives next to the profile's other files, so deleting the profile deletes
// the metadata. (The HUKS keys outlive it; see CredentialStore::Remove.)
constexpr base::FilePath::CharType kFileName[] =
    FILE_PATH_LITERAL("OHOS WebAuthn Credentials.json");

// A profile would need thousands of passkeys to approach this; anything
// larger is corrupt, and reading it whole would only waste memory.
constexpr size_t kMaxFileSize = 4 * 1024 * 1024;

constexpr int kFormatVersion = 1;

constexpr char kVersionKey[] = "version";
constexpr char kCredentialsKey[] = "credentials";
constexpr char kCredentialIdKey[] = "credential_id";
constexpr char kRpIdKey[] = "rp_id";
constexpr char kUserIdKey[] = "user_id";
constexpr char kUserNameKey[] = "user_name";
constexpr char kUserDisplayNameKey[] = "user_display_name";
constexpr char kCreatedKey[] = "created";

base::DictValue ToDict(const StoredCredential& credential) {
  base::DictValue dict;
  dict.Set(kCredentialIdKey, base::Base64Encode(credential.credential_id));
  dict.Set(kRpIdKey, credential.rp_id);
  dict.Set(kUserIdKey, base::Base64Encode(credential.user.id));
  if (credential.user.name) {
    dict.Set(kUserNameKey, *credential.user.name);
  }
  if (credential.user.display_name) {
    dict.Set(kUserDisplayNameKey, *credential.user.display_name);
  }
  dict.Set(kCreatedKey, credential.created.InSecondsFSinceUnixEpoch());
  return dict;
}

std::optional<StoredCredential> FromDict(const base::DictValue& dict) {
  const std::string* credential_id = dict.FindString(kCredentialIdKey);
  const std::string* rp_id = dict.FindString(kRpIdKey);
  const std::string* user_id = dict.FindString(kUserIdKey);
  if (!credential_id || !rp_id || !user_id) {
    return std::nullopt;
  }
  std::optional<std::vector<uint8_t>> decoded_id =
      base::Base64Decode(*credential_id);
  std::optional<std::vector<uint8_t>> decoded_user_id =
      base::Base64Decode(*user_id);
  if (!decoded_id || decoded_id->empty() || !decoded_user_id) {
    return std::nullopt;
  }
  StoredCredential credential;
  credential.credential_id = std::move(*decoded_id);
  credential.rp_id = *rp_id;
  credential.user.id = std::move(*decoded_user_id);
  if (const std::string* name = dict.FindString(kUserNameKey)) {
    credential.user.name = *name;
  }
  if (const std::string* display_name = dict.FindString(kUserDisplayNameKey)) {
    credential.user.display_name = *display_name;
  }
  credential.created = base::Time::FromSecondsSinceUnixEpoch(
      dict.FindDouble(kCreatedKey).value_or(0));
  return credential;
}

}  // namespace

StoredCredential::StoredCredential() = default;
StoredCredential::StoredCredential(const StoredCredential&) = default;
StoredCredential::StoredCredential(StoredCredential&&) = default;
StoredCredential& StoredCredential::operator=(const StoredCredential&) =
    default;
StoredCredential& StoredCredential::operator=(StoredCredential&&) = default;
StoredCredential::~StoredCredential() = default;

CredentialStore::CredentialStore(const base::FilePath& profile_path)
    : file_path_(profile_path.Append(kFileName)) {}

CredentialStore::~CredentialStore() = default;

std::optional<std::vector<StoredCredential>> CredentialStore::ReadAll() const {
  std::vector<StoredCredential> credentials;
  if (!base::PathExists(file_path_)) {
    return credentials;
  }
  std::string contents;
  if (!base::ReadFileToStringWithMaxSize(file_path_, &contents,
                                         kMaxFileSize)) {
    FIDO_LOG(ERROR) << "Cannot read " << file_path_;
    return std::nullopt;
  }
  std::optional<base::DictValue> root =
      base::JSONReader::ReadDict(contents, base::JSON_PARSE_RFC);
  if (!root || root->FindInt(kVersionKey).value_or(0) != kFormatVersion) {
    FIDO_LOG(ERROR) << "Unrecognised contents in " << file_path_;
    return std::nullopt;
  }
  const base::ListValue* list = root->FindList(kCredentialsKey);
  if (!list) {
    return std::nullopt;
  }
  for (const base::Value& item : *list) {
    const base::DictValue* dict = item.GetIfDict();
    std::optional<StoredCredential> credential =
        dict ? FromDict(*dict) : std::nullopt;
    if (!credential) {
      // Skip, rather than fail on, one bad entry, so that the others stay
      // usable. It is dropped on the next write.
      FIDO_LOG(ERROR) << "Skipping malformed credential in " << file_path_;
      continue;
    }
    credentials.push_back(std::move(*credential));
  }
  return credentials;
}

std::optional<std::vector<StoredCredential>> CredentialStore::Find(
    const std::string& rp_id,
    const std::vector<PublicKeyCredentialDescriptor>& allow_list) const {
  std::optional<std::vector<StoredCredential>> all = ReadAll();
  if (!all) {
    return std::nullopt;
  }
  std::vector<StoredCredential> matches;
  for (StoredCredential& credential : *all) {
    if (credential.rp_id != rp_id) {
      continue;
    }
    if (!allow_list.empty() &&
        !std::ranges::contains(allow_list, credential.credential_id,
                               &PublicKeyCredentialDescriptor::id)) {
      continue;
    }
    matches.push_back(std::move(credential));
  }
  std::ranges::sort(matches, [](const StoredCredential& a,
                                const StoredCredential& b) {
    return a.created > b.created;
  });
  return matches;
}

bool CredentialStore::Add(const StoredCredential& credential) {
  std::optional<std::vector<StoredCredential>> all = ReadAll();
  if (!all) {
    return false;
  }
  std::vector<StoredCredential> kept;
  for (StoredCredential& existing : *all) {
    if (existing.rp_id == credential.rp_id &&
        existing.user.id == credential.user.id) {
      DeleteSigningKey(KeyAliasForCredentialId(existing.credential_id));
      continue;
    }
    kept.push_back(std::move(existing));
  }
  kept.push_back(credential);
  return WriteAll(kept);
}

bool CredentialStore::Remove(const std::vector<uint8_t>& credential_id) {
  std::optional<std::vector<StoredCredential>> all = ReadAll();
  if (!all) {
    return false;
  }
  std::erase_if(*all, [&credential_id](const StoredCredential& credential) {
    return credential.credential_id == credential_id;
  });
  DeleteSigningKey(KeyAliasForCredentialId(credential_id));
  return WriteAll(*all);
}

bool CredentialStore::WriteAll(
    const std::vector<StoredCredential>& credentials) const {
  base::ListValue list;
  for (const StoredCredential& credential : credentials) {
    list.Append(ToDict(credential));
  }
  base::DictValue root;
  root.Set(kVersionKey, kFormatVersion);
  root.Set(kCredentialsKey, std::move(list));
  std::optional<std::string> json = base::WriteJson(root);
  if (!json) {
    return false;
  }
  // Atomic, so that a crash mid-write leaves the previous file rather than a
  // truncated one that ReadAll() would refuse.
  if (!base::ImportantFileWriter::WriteFileAtomically(file_path_, *json)) {
    FIDO_LOG(ERROR) << "Cannot write " << file_path_;
    return false;
  }
  return true;
}

}  // namespace device::fido::ohos
