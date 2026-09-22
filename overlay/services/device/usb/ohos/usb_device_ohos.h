// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SERVICES_DEVICE_USB_OHOS_USB_DEVICE_OHOS_H_
#define SERVICES_DEVICE_USB_OHOS_USB_DEVICE_OHOS_H_

#include <string>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "services/device/usb/usb_device.h"

namespace device {

class UsbServiceOhos;

class UsbDeviceOhos final : public UsbDevice {
 public:
  static scoped_refptr<UsbDeviceOhos> Create(
      base::WeakPtr<UsbServiceOhos> service,
      const base::DictValue& device);

  void RequestPermission(ResultCallback callback) override;
  bool permission_granted() const override;
  void Open(OpenCallback callback) override;

  const std::string& platform_id() const { return platform_id_; }

 private:
  UsbDeviceOhos(base::WeakPtr<UsbServiceOhos> service,
                mojom::UsbDeviceInfoPtr device_info,
                std::string platform_id,
                bool permission_granted);
  ~UsbDeviceOhos() override;

  void OnPermissionResponse(ResultCallback callback, base::DictValue response);
  void OnOpenResponse(OpenCallback callback, base::DictValue response);

  base::WeakPtr<UsbServiceOhos> service_;
  const std::string platform_id_;
  bool permission_granted_;
  base::WeakPtrFactory<UsbDeviceOhos> weak_factory_{this};
};

}  // namespace device

#endif  // SERVICES_DEVICE_USB_OHOS_USB_DEVICE_OHOS_H_
