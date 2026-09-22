// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SERVICES_DEVICE_HID_HID_SERVICE_OHOS_H_
#define SERVICES_DEVICE_HID_HID_SERVICE_OHOS_H_

#include <map>
#include <string>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "services/device/hid/hid_service.h"

namespace device {

class HidConnectionOhos;

class HidServiceOhos : public HidService {
 public:
  HidServiceOhos();
  HidServiceOhos(const HidServiceOhos&) = delete;
  HidServiceOhos& operator=(const HidServiceOhos&) = delete;
  ~HidServiceOhos() override;

  void Connect(const std::string& device_guid,
               bool allow_protected_reports,
               bool allow_fido_reports,
               ConnectCallback callback) override;
  base::WeakPtr<HidService> GetWeakPtr() override;

 private:
  void RefreshDevices();
  void OnRefreshComplete(base::DictValue response);
  void OnPlatformEvent(const base::DictValue& event);
  void OnConnectionOpened(scoped_refptr<HidDeviceInfo> device_info,
                          bool allow_protected_reports,
                          bool allow_fido_reports,
                          ConnectCallback callback,
                          base::DictValue response);
  void OnConnectionClosed(int connection_id);

  bool initialized_ = false;
  bool refresh_in_flight_ = false;
  bool refresh_again_ = false;
  std::map<std::string, scoped_refptr<HidDeviceInfo>> devices_by_platform_id_;
  std::map<int, base::WeakPtr<HidConnectionOhos>> connections_;

  base::WeakPtrFactory<HidServiceOhos> weak_factory_{this};
};

}  // namespace device

#endif  // SERVICES_DEVICE_HID_HID_SERVICE_OHOS_H_
