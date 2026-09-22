// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/device/usb/ohos/usb_service_ohos.h"

#include <set>
#include <utility>

#include "base/functional/bind.h"
#include "components/device_event_log/device_event_log.h"
#include "services/device/usb/ohos/usb_bridge_ohos.h"
#include "services/device/usb/ohos/usb_device_ohos.h"

namespace device {

UsbServiceOhos::UsbServiceOhos() {
  SetUsbEventCallbackOhos(base::BindRepeating(&UsbServiceOhos::OnPlatformEvent,
                                              weak_factory_.GetWeakPtr()));
  RefreshDevices();
}

UsbServiceOhos::~UsbServiceOhos() {
  NotifyWillDestroyUsbService();
  SetUsbEventCallbackOhos(UsbEventCallbackOhos());
}

void UsbServiceOhos::GetDevices(GetDevicesCallback callback) {
  if (!initialized_ || refresh_in_flight_) {
    pending_enumerations_.push_back(std::move(callback));
    if (!refresh_in_flight_) {
      RefreshDevices();
    }
    return;
  }
  UsbService::GetDevices(std::move(callback));
}

void UsbServiceOhos::RefreshDevices() {
  if (refresh_in_flight_) {
    refresh_again_ = true;
    return;
  }
  refresh_in_flight_ = true;
  base::DictValue command;
  command.Set("method", "enumerate");
  SendUsbCommandOhos(std::move(command),
                     base::BindOnce(&UsbServiceOhos::OnRefreshComplete,
                                    weak_factory_.GetWeakPtr()));
}

void UsbServiceOhos::OnRefreshComplete(base::DictValue response) {
  refresh_in_flight_ = false;
  initialized_ = true;
  if (response.FindBool("ok").value_or(false)) {
    std::set<std::string> seen;
    if (const base::ListValue* platform_devices =
            response.FindList("devices")) {
      for (const base::Value& value : *platform_devices) {
        const base::DictValue* info = value.GetIfDict();
        const std::string* platform_id =
            info ? info->FindString("name") : nullptr;
        if (!platform_id || platform_id->empty()) {
          continue;
        }
        seen.insert(*platform_id);
        if (devices_by_platform_id_.contains(*platform_id)) {
          continue;
        }
        scoped_refptr<UsbDeviceOhos> device =
            UsbDeviceOhos::Create(weak_factory_.GetWeakPtr(), *info);
        if (!device) {
          continue;
        }
        devices_by_platform_id_.emplace(*platform_id, device);
        devices()[device->guid()] = device;
        USB_LOG(USER) << "USB device added: " << *platform_id
                      << " vendor=" << device->vendor_id()
                      << " product=" << device->product_id();
        NotifyDeviceAdded(device);
      }
    }

    for (auto it = devices_by_platform_id_.begin();
         it != devices_by_platform_id_.end();) {
      if (seen.contains(it->first)) {
        ++it;
        continue;
      }
      scoped_refptr<UsbDeviceOhos> device = it->second;
      it = devices_by_platform_id_.erase(it);
      devices().erase(device->guid());
      device->OnDisconnect();
      USB_LOG(USER) << "USB device removed: " << device->platform_id();
      NotifyDeviceRemoved(device);
    }
  } else {
    const std::string* error = response.FindString("error");
    USB_LOG(ERROR) << "USB enumeration failed: "
                   << (error ? *error : "unknown error");
  }

  CompletePendingEnumerations();
  if (refresh_again_) {
    refresh_again_ = false;
    RefreshDevices();
  }
}

void UsbServiceOhos::OnPlatformEvent(const base::DictValue& event) {
  const std::string* type = event.FindString("event");
  if (type && *type == "devicesChanged") {
    RefreshDevices();
  }
}

void UsbServiceOhos::CompletePendingEnumerations() {
  std::vector<GetDevicesCallback> callbacks;
  callbacks.swap(pending_enumerations_);
  for (auto& callback : callbacks) {
    UsbService::GetDevices(std::move(callback));
  }
}

}  // namespace device
