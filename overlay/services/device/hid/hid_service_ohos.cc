// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/device/hid/hid_service_ohos.h"

#include <algorithm>
#include <set>
#include <utility>

#include "base/functional/bind.h"
#include "components/device_event_log/device_event_log.h"
#include "services/device/hid/hid_connection_ohos.h"
#include "services/device/usb/ohos/usb_bridge_ohos.h"

namespace device {
namespace {

int ReadInt(const base::DictValue& value, const char* key, int fallback = 0) {
  return value.FindInt(key).value_or(fallback);
}

std::string ReadString(const base::DictValue& value, const char* key) {
  const std::string* text = value.FindString(key);
  return text ? *text : std::string();
}

std::vector<uint8_t> ReadBytes(const base::DictValue& value,
                               const char* key) {
  std::vector<uint8_t> bytes;
  const base::ListValue* source = value.FindList(key);
  if (!source) {
    return bytes;
  }
  bytes.reserve(source->size());
  for (const base::Value& item : *source) {
    if (item.is_int()) {
      bytes.push_back(static_cast<uint8_t>(std::clamp(item.GetInt(), 0, 255)));
    }
  }
  return bytes;
}

}  // namespace

HidServiceOhos::HidServiceOhos() {
  SetHidEventCallbackOhos(base::BindRepeating(
      &HidServiceOhos::OnPlatformEvent, weak_factory_.GetWeakPtr()));
  RefreshDevices();
}

HidServiceOhos::~HidServiceOhos() {
  SetHidEventCallbackOhos(UsbEventCallbackOhos());
}

void HidServiceOhos::Connect(const std::string& device_guid,
                             bool allow_protected_reports,
                             bool allow_fido_reports,
                             ConnectCallback callback) {
  const auto it = devices().find(device_guid);
  if (it == devices().end() || it->second->platform_device_id_map().empty()) {
    std::move(callback).Run(nullptr);
    return;
  }

  scoped_refptr<HidDeviceInfo> device_info = it->second;
  base::DictValue command;
  command.Set("method", "hidOpen");
  command.Set("platformId",
              device_info->platform_device_id_map().front().platform_device_id);
  command.Set("size",
              static_cast<int>(device_info->max_input_report_size() + 1));
  SendUsbCommandOhos(
      std::move(command),
      base::BindOnce(&HidServiceOhos::OnConnectionOpened,
                     weak_factory_.GetWeakPtr(), std::move(device_info),
                     allow_protected_reports, allow_fido_reports,
                     std::move(callback)));
}

base::WeakPtr<HidService> HidServiceOhos::GetWeakPtr() {
  return weak_factory_.GetWeakPtr();
}

void HidServiceOhos::RefreshDevices() {
  if (refresh_in_flight_) {
    refresh_again_ = true;
    return;
  }
  refresh_in_flight_ = true;
  base::DictValue command;
  command.Set("method", "hidEnumerate");
  SendUsbCommandOhos(std::move(command),
                     base::BindOnce(&HidServiceOhos::OnRefreshComplete,
                                    weak_factory_.GetWeakPtr()));
}

void HidServiceOhos::OnRefreshComplete(base::DictValue response) {
  refresh_in_flight_ = false;
  if (response.FindBool("ok").value_or(false)) {
    std::set<std::string> seen;
    if (const base::ListValue* platform_devices =
            response.FindList("hidDevices")) {
      for (const base::Value& value : *platform_devices) {
        const base::DictValue* source = value.GetIfDict();
        if (!source) {
          continue;
        }
        const std::string platform_id = ReadString(*source, "platformId");
        std::vector<uint8_t> descriptor =
            ReadBytes(*source, "reportDescriptor");
        if (platform_id.empty() || descriptor.empty()) {
          continue;
        }
        seen.insert(platform_id);
        if (devices_by_platform_id_.contains(platform_id)) {
          continue;
        }

        auto device_info = base::MakeRefCounted<HidDeviceInfo>(
            platform_id, ReadString(*source, "physicalDeviceId"),
            static_cast<uint16_t>(
                std::clamp(ReadInt(*source, "vendorId"), 0, 65535)),
            static_cast<uint16_t>(
                std::clamp(ReadInt(*source, "productId"), 0, 65535)),
            ReadString(*source, "productName"),
            ReadString(*source, "serialNumber"),
            mojom::HidBusType::kHIDBusTypeUSB, descriptor);
        devices_by_platform_id_.emplace(platform_id, device_info);
        AddDevice(std::move(device_info));
      }
    }

    for (auto it = devices_by_platform_id_.begin();
         it != devices_by_platform_id_.end();) {
      if (seen.contains(it->first)) {
        ++it;
        continue;
      }
      const std::string platform_id = it->first;
      it = devices_by_platform_id_.erase(it);
      RemoveDevice(platform_id);
    }
  } else {
    const std::string* error = response.FindString("error");
    HID_LOG(ERROR) << "HID enumeration failed: "
                   << (error ? *error : "unknown error");
  }

  if (!initialized_) {
    initialized_ = true;
    FirstEnumerationComplete();
  }
  if (refresh_again_) {
    refresh_again_ = false;
    RefreshDevices();
  }
}

void HidServiceOhos::OnPlatformEvent(const base::DictValue& event) {
  const std::string* type = event.FindString("event");
  if (!type) {
    return;
  }
  if (*type == "devicesChanged") {
    RefreshDevices();
    return;
  }
  if (*type != "hidInput") {
    return;
  }
  const int connection_id = event.FindInt("connectionId").value_or(0);
  const auto it = connections_.find(connection_id);
  if (it != connections_.end() && it->second) {
    it->second->OnInputReport(event);
  }
}

void HidServiceOhos::OnConnectionOpened(
    scoped_refptr<HidDeviceInfo> device_info,
    bool allow_protected_reports,
    bool allow_fido_reports,
    ConnectCallback callback,
    base::DictValue response) {
  const int connection_id = response.FindInt("connectionId").value_or(0);
  if (!response.FindBool("ok").value_or(false) || connection_id <= 0) {
    std::move(callback).Run(nullptr);
    return;
  }
  auto connection = base::MakeRefCounted<HidConnectionOhos>(
      std::move(device_info), connection_id, allow_protected_reports,
      allow_fido_reports,
      base::BindOnce(&HidServiceOhos::OnConnectionClosed,
                     weak_factory_.GetWeakPtr()));
  connections_[connection_id] = connection->GetWeakPtr();
  std::move(callback).Run(std::move(connection));
}

void HidServiceOhos::OnConnectionClosed(int connection_id) {
  connections_.erase(connection_id);
}

}  // namespace device
