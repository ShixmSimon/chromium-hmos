// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "device/fido/ohos/user_auth.h"

#include <utility>

#include "base/base64.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/values.h"
#include "components/device_event_log/device_event_log.h"
#include "components/ohos_system_service/system_service_ohos.h"

namespace device::fido::ohos {

namespace {

// Must match the registration in the engine's SystemServices.ets.
constexpr char kService[] = "userauth";

}  // namespace

void IsUserAuthAvailable(base::OnceCallback<void(bool)> callback) {
  ohos_system_service::Call(
      kService, "available", base::DictValue(),
      base::BindOnce(
          [](base::OnceCallback<void(bool)> callback,
             ohos_system_service::Reply reply) {
            if (!reply.ok) {
              FIDO_LOG(DEBUG) << "userauth.available failed: " << reply.error;
            }
            std::move(callback).Run(
                reply.ok &&
                reply.result_dict().FindBool("available").value_or(false));
          },
          std::move(callback)));
}

void AuthenticateUser(
    const std::vector<uint8_t>& challenge,
    const std::string& title,
    base::OnceCallback<void(std::optional<std::vector<uint8_t>>)> callback) {
  base::DictValue args;
  args.Set("challenge", base::Base64Encode(challenge));
  args.Set("title", title);
  ohos_system_service::Call(
      kService, "authenticate", std::move(args),
      base::BindOnce(
          [](base::OnceCallback<void(std::optional<std::vector<uint8_t>>)>
                 callback,
             ohos_system_service::Reply reply) {
            if (!reply.ok) {
              // Cancelling the prompt lands here too; that is the common case,
              // not an error worth more than a debug line.
              FIDO_LOG(DEBUG) << "userauth.authenticate: " << reply.error;
              std::move(callback).Run(std::nullopt);
              return;
            }
            const std::string* token =
                reply.result_dict().FindString("token");
            std::optional<std::vector<uint8_t>> decoded =
                token ? base::Base64Decode(*token) : std::nullopt;
            if (!decoded || decoded->empty()) {
              FIDO_LOG(ERROR) << "userauth.authenticate returned no token";
              std::move(callback).Run(std::nullopt);
              return;
            }
            std::move(callback).Run(std::move(decoded));
          },
          std::move(callback)));
}

}  // namespace device::fido::ohos
