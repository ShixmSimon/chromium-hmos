// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SERVICES_DEVICE_USB_OHOS_USB_SERVICE_OHOS_H_
#define SERVICES_DEVICE_USB_OHOS_USB_SERVICE_OHOS_H_

#include <map>
#include <string>
#include <vector>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "services/device/usb/usb_service.h"

namespace device {

class UsbDeviceOhos;

class UsbServiceOhos final : public UsbService {
 public:
  UsbServiceOhos();
  ~UsbServiceOhos() override;

  void GetDevices(GetDevicesCallback callback) override;

 private:
  void RefreshDevices();
  void OnRefreshComplete(base::DictValue response);
  void OnPlatformEvent(const base::DictValue& event);
  void CompletePendingEnumerations();

  bool initialized_ = false;
  bool refresh_in_flight_ = false;
  bool refresh_again_ = false;
  std::vector<GetDevicesCallback> pending_enumerations_;
  std::map<std::string, scoped_refptr<UsbDeviceOhos>> devices_by_platform_id_;
  base::WeakPtrFactory<UsbServiceOhos> weak_factory_{this};
};

}  // namespace device

#endif  // SERVICES_DEVICE_USB_OHOS_USB_SERVICE_OHOS_H_
