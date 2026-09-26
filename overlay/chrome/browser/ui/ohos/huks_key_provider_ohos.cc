// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/ohos/huks_key_provider_ohos.h"

#include <huks/native_huks_api.h>
#include <huks/native_huks_param.h>
#include <huks/native_huks_type.h>

#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/base_paths.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "chrome/common/chrome_paths.h"
#include "base/files/file_util.h"
#include "base/rand_util.h"
#include "base/task/task_traits.h"
#include "base/task/thread_pool.h"
#include "components/os_crypt/async/common/algorithm.mojom.h"

namespace chrome::ohos {

namespace {

// The tag OSCrypt writes in front of anything this key encrypted. Data written
// by PosixKeyProvider carries "v10" and stays readable, which is what lets the
// two coexist while old values are re-encrypted.
constexpr char kHuksTag[] = "ohv1";

// The key HUKS holds and never hands out. One per app; deleting the app takes
// it with it, which also makes the wrapped file on disk permanently useless.
constexpr char kHuksAlias[] = "chromium_oscrypt_v1";

constexpr size_t kKeyBytes = 32;    // AES-256, matching Encryptor::Key.
constexpr size_t kNonceBytes = 12;  // GCM.
constexpr size_t kAeTagBytes = 16;

// Where the wrapped key lives.
//
// The user data directory, which is the one the shell passes on the command
// line and the one the app can write to. DIR_HOME was the first choice --
// per-installation rather than per-profile, which is what this key is -- but
// on a device it is not a directory this app may write to, so every start
// made a new wrapping key, failed to store what it wrapped, and lost every
// password saved in the run before.
base::FilePath WrappedKeyPath() {
  base::FilePath dir;
  if (!base::PathService::Get(chrome::DIR_USER_DATA, &dir) &&
      !base::PathService::Get(base::DIR_HOME, &dir)) {
    return base::FilePath();
  }
  return dir.AppendASCII("oscrypt_key.huks");
}

OH_Huks_Blob ToBlob(base::span<const uint8_t> bytes) {
  return OH_Huks_Blob{static_cast<uint32_t>(bytes.size()),
                      const_cast<uint8_t*>(bytes.data())};
}

OH_Huks_Blob AliasBlob() {
  return OH_Huks_Blob{
      static_cast<uint32_t>(sizeof(kHuksAlias) - 1),
      const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(kHuksAlias))};
}

bool Ok(OH_Huks_Result result) {
  return result.errorCode == OH_HUKS_SUCCESS;
}

// Builds a parameter set and hands ownership to the caller, or null.
struct ParamSetDeleter {
  void operator()(OH_Huks_ParamSet* set) const { OH_Huks_FreeParamSet(&set); }
};
using ScopedParamSet = std::unique_ptr<OH_Huks_ParamSet, ParamSetDeleter>;

ScopedParamSet BuildParams(base::span<const OH_Huks_Param> params) {
  OH_Huks_ParamSet* set = nullptr;
  if (!Ok(OH_Huks_InitParamSet(&set))) {
    return nullptr;
  }
  ScopedParamSet scoped(set);
  if (!Ok(OH_Huks_AddParams(scoped.get(),
                            const_cast<OH_Huks_Param*>(params.data()),
                            static_cast<uint32_t>(params.size())))) {
    return nullptr;
  }
  OH_Huks_ParamSet* built = scoped.release();
  if (!Ok(OH_Huks_BuildParamSet(&built))) {
    OH_Huks_FreeParamSet(&built);
    return nullptr;
  }
  return ScopedParamSet(built);
}

// Set when the wrapped key could not be opened and a new one was made.
bool& KeyWasReplaced() {
  static bool replaced = false;
  return replaced;
}

// Creates the wrapping key if this device does not have one yet.
//
// Deliberately without any user-authentication tag: this key is opened once
// at startup, and requiring a face or a fingerprint for that would put a
// prompt in front of the browser every time it launches. Authentication
// before a password is shown or filled is a separate layer -- the
// DeviceAuthenticator -- and the two are kept apart on purpose.
bool EnsureHuksKey() {
  OH_Huks_Blob alias = AliasBlob();
  if (Ok(OH_Huks_IsKeyItemExist(&alias, nullptr))) {
    LOG(WARNING) << "OHOS HUKS: wrapping key present";
    return true;
  }
  const std::array<OH_Huks_Param, 5> params = {{
      {.tag = OH_HUKS_TAG_ALGORITHM, .uint32Param = OH_HUKS_ALG_AES},
      {.tag = OH_HUKS_TAG_KEY_SIZE, .uint32Param = OH_HUKS_AES_KEY_SIZE_256},
      {.tag = OH_HUKS_TAG_PURPOSE,
       .uint32Param = OH_HUKS_KEY_PURPOSE_ENCRYPT | OH_HUKS_KEY_PURPOSE_DECRYPT},
      {.tag = OH_HUKS_TAG_BLOCK_MODE, .uint32Param = OH_HUKS_MODE_GCM},
      {.tag = OH_HUKS_TAG_PADDING, .uint32Param = OH_HUKS_PADDING_NONE},
  }};
  ScopedParamSet set = BuildParams(params);
  if (!set) {
    return false;
  }
  const OH_Huks_Result result =
      OH_Huks_GenerateKeyItem(&alias, set.get(), nullptr);
  if (!Ok(result)) {
    LOG(ERROR) << "OHOS HUKS: could not create the wrapping key, error "
               << result.errorCode;
    return false;
  }
  LOG(WARNING) << "OHOS HUKS: created the wrapping key";
  return true;
}

// One pass of AES-GCM through HUKS. `purpose` decides the direction.
std::optional<std::vector<uint8_t>> Transform(
    uint32_t purpose,
    base::span<const uint8_t> nonce,
    base::span<const uint8_t> input,
    base::span<const uint8_t> ae_tag) {
  std::vector<OH_Huks_Param> params = {
      {.tag = OH_HUKS_TAG_ALGORITHM, .uint32Param = OH_HUKS_ALG_AES},
      {.tag = OH_HUKS_TAG_KEY_SIZE, .uint32Param = OH_HUKS_AES_KEY_SIZE_256},
      {.tag = OH_HUKS_TAG_PURPOSE, .uint32Param = purpose},
      {.tag = OH_HUKS_TAG_BLOCK_MODE, .uint32Param = OH_HUKS_MODE_GCM},
      {.tag = OH_HUKS_TAG_PADDING, .uint32Param = OH_HUKS_PADDING_NONE},
      {.tag = OH_HUKS_TAG_NONCE, .blob = ToBlob(nonce)},
  };
  if (purpose == OH_HUKS_KEY_PURPOSE_DECRYPT) {
    params.push_back({.tag = OH_HUKS_TAG_AE_TAG, .blob = ToBlob(ae_tag)});
  }
  ScopedParamSet set = BuildParams(params);
  if (!set || !EnsureHuksKey()) {
    return std::nullopt;
  }

  std::array<uint8_t, 64> handle_bytes = {};
  OH_Huks_Blob handle{static_cast<uint32_t>(handle_bytes.size()),
                      handle_bytes.data()};
  OH_Huks_Blob alias = AliasBlob();
  if (!Ok(OH_Huks_InitSession(&alias, set.get(), &handle, nullptr))) {
    return std::nullopt;
  }

  // Room for the ciphertext and, when encrypting, the tag HUKS appends.
  std::vector<uint8_t> output(input.size() + kAeTagBytes + 64);
  OH_Huks_Blob in = ToBlob(input);
  OH_Huks_Blob out{static_cast<uint32_t>(output.size()), output.data()};
  const OH_Huks_Result result =
      OH_Huks_FinishSession(&handle, set.get(), &in, &out);
  if (!Ok(result)) {
    OH_Huks_AbortSession(&handle, set.get());
    LOG(ERROR) << "OHOS HUKS: "
               << (purpose == OH_HUKS_KEY_PURPOSE_ENCRYPT ? "wrapping"
                                                          : "unwrapping")
               << " failed, error " << result.errorCode;
    return std::nullopt;
  }
  output.resize(out.size);
  return output;
}

// The file is nonce | tag | wrapped key. Fixed widths, so no framing.
std::optional<std::vector<uint8_t>> ReadKeyFromDisk() {
  std::string blob;
  const base::FilePath path = WrappedKeyPath();
  if (path.empty() || !base::ReadFileToString(path, &blob) ||
      blob.size() <= kNonceBytes + kAeTagBytes) {
    return std::nullopt;
  }
  const auto bytes = base::as_byte_span(blob);
  return Transform(OH_HUKS_KEY_PURPOSE_DECRYPT, bytes.first(kNonceBytes),
                   bytes.subspan(kNonceBytes + kAeTagBytes),
                   bytes.subspan(kNonceBytes, kAeTagBytes));
}

bool WriteKeyToDisk(base::span<const uint8_t> key) {
  std::vector<uint8_t> nonce(kNonceBytes);
  base::RandBytes(nonce);
  std::optional<std::vector<uint8_t>> wrapped =
      Transform(OH_HUKS_KEY_PURPOSE_ENCRYPT, nonce, key, {});
  if (!wrapped || wrapped->size() < kAeTagBytes) {
    return false;
  }
  // HUKS appends the tag; split it out so the layout is fixed.
  const size_t cipher_len = wrapped->size() - kAeTagBytes;
  std::vector<uint8_t> file;
  file.insert(file.end(), nonce.begin(), nonce.end());
  file.insert(file.end(), wrapped->begin() + cipher_len, wrapped->end());
  file.insert(file.end(), wrapped->begin(), wrapped->begin() + cipher_len);

  const base::FilePath path = WrappedKeyPath();
  return !path.empty() &&
         base::WriteFile(path, base::as_string_view(base::span(file)));
}

// Reads the key, creating it the first time. Runs off the UI thread: HUKS
// talks to another process and the file is on disk.
std::optional<std::vector<uint8_t>> LoadOrCreateKey() {
  if (!EnsureHuksKey()) {
    return std::nullopt;
  }
  const bool had_file = base::PathExists(WrappedKeyPath());
  if (std::optional<std::vector<uint8_t>> key = ReadKeyFromDisk()) {
    LOG(WARNING) << "OHOS HUKS: opened the wrapped OSCrypt key";
    return key;
  }
  if (had_file) {
    // The file is there and will not open. HUKS keys do not travel, so this
    // is a new device, a restored backup or a reinstall: the key that wrapped
    // it is gone for good. Start again rather than fail every launch, and let
    // the password store know that what it holds can no longer be read.
    KeyWasReplaced() = true;
    LOG(WARNING) << "OHOS HUKS: the stored key will not open on this device "
                    "-- starting a new one, and anything it protected is gone";
  }
  std::vector<uint8_t> key(kKeyBytes);
  base::RandBytes(key);
  if (!WriteKeyToDisk(key)) {
    // Better no provider than one whose key is forgotten on restart: OSCrypt
    // would encrypt with it and nothing could read the result again.
    LOG(ERROR) << "OHOS HUKS: could not store the wrapped key at "
               << WrappedKeyPath();
    return std::nullopt;
  }
  LOG(WARNING) << "OHOS HUKS: created and wrapped a new OSCrypt key";
  return key;
}

class HuksKeyProvider : public os_crypt_async::KeyProvider {
 public:
  HuksKeyProvider() = default;

 private:
  void GetKey(KeyCallback callback) override {
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_BLOCKING},
        base::BindOnce(&LoadOrCreateKey),
        base::BindOnce(
            [](KeyCallback callback,
               std::optional<std::vector<uint8_t>> key) {
              if (!key) {
                // Temporary rather than permanent: HUKS can be unavailable
                // while the device is still starting, and saying permanent
                // invites callers to treat old data as lost.
                std::move(callback).Run(
                    kHuksTag,
                    base::unexpected(KeyError::kTemporarilyUnavailable));
                return;
              }
              std::move(callback).Run(
                  kHuksTag,
                  os_crypt_async::Encryptor::Key(
                      *key, os_crypt_async::mojom::Algorithm::kAES256GCM));
            },
            std::move(callback)));
  }

  bool UseForEncryption() override { return true; }
};

}  // namespace

std::unique_ptr<os_crypt_async::KeyProvider> MakeHuksKeyProvider() {
  return std::make_unique<HuksKeyProvider>();
}

bool PasswordsAreProtectedByHuks() {
  return true;
}

bool WrappedKeyWasReplaced() {
  return KeyWasReplaced();
}

}  // namespace chrome::ohos
