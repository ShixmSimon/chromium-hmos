// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/device/vibration/vibration_manager_ohos.h"

#include <algorithm>
#include <cstdint>
#include <utility>

#include <sensors/vibrator.h>

#include "base/logging.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"

namespace device {

namespace {

constexpr int64_t kMaxVibrationDurationMs = 10'000;
constexpr int32_t kVibratorSuccess = 0;

}  // namespace

VibrationManagerOhos::VibrationManagerOhos(
    mojo::PendingRemote<mojom::VibrationManagerListener> listener)
    : VibrationManagerImpl(std::move(listener)) {}

VibrationManagerOhos::~VibrationManagerOhos() = default;

void VibrationManagerOhos::PlatformVibrate(int64_t milliseconds) {
  if (milliseconds <= 0) {
    PlatformCancel();
    return;
  }

  const int32_t duration =
      static_cast<int32_t>(std::min(milliseconds, kMaxVibrationDurationMs));
  Vibrator_Attribute attribute{};
  attribute.vibratorId = 0;
  attribute.usage = VIBRATOR_USAGE_TOUCH;

  const int32_t result = OH_Vibrator_PlayVibration(duration, attribute);
  if (result != kVibratorSuccess) {
    LOG(ERROR) << "OHOS vibration failed to start: " << result;
  }
}

void VibrationManagerOhos::PlatformCancel() {
  const int32_t result = OH_Vibrator_Cancel();
  if (result != kVibratorSuccess) {
    LOG(ERROR) << "OHOS vibration failed to cancel: " << result;
  }
}

// static
void VibrationManagerOhos::Create(
    mojo::PendingReceiver<mojom::VibrationManager> receiver,
    mojo::PendingRemote<mojom::VibrationManagerListener> listener) {
  mojo::MakeSelfOwnedReceiver(
      std::make_unique<VibrationManagerOhos>(std::move(listener)),
      std::move(receiver));
}

}  // namespace device
