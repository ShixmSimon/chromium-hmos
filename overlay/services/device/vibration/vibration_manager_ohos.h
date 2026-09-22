// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SERVICES_DEVICE_VIBRATION_VIBRATION_MANAGER_OHOS_H_
#define SERVICES_DEVICE_VIBRATION_VIBRATION_MANAGER_OHOS_H_

#include "services/device/vibration/vibration_manager_impl.h"

namespace device {

class VibrationManagerOhos final : public VibrationManagerImpl {
 public:
  static void Create(
      mojo::PendingReceiver<mojom::VibrationManager> receiver,
      mojo::PendingRemote<mojom::VibrationManagerListener> listener);

  explicit VibrationManagerOhos(
      mojo::PendingRemote<mojom::VibrationManagerListener> listener);
  VibrationManagerOhos(const VibrationManagerOhos&) = delete;
  VibrationManagerOhos& operator=(const VibrationManagerOhos&) = delete;
  ~VibrationManagerOhos() override;

 protected:
  // VibrationManagerImpl:
  void PlatformVibrate(int64_t milliseconds) override;
  void PlatformCancel() override;
};

}  // namespace device

#endif  // SERVICES_DEVICE_VIBRATION_VIBRATION_MANAGER_OHOS_H_
