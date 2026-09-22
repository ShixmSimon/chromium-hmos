// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/device/usb/ohos/usb_device_ohos.h"

#include <algorithm>
#include <cstdint>
#include <utility>

#include "base/functional/bind.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "services/device/usb/ohos/usb_bridge_ohos.h"
#include "services/device/usb/ohos/usb_device_handle_ohos.h"
#include "services/device/usb/ohos/usb_service_ohos.h"
#include "services/device/usb/usb_descriptors.h"

namespace device {
namespace {

int ReadInt(const base::DictValue& value, const char* key, int fallback = 0) {
  return value.FindInt(key).value_or(fallback);
}

std::string ReadString(const base::DictValue& value, const char* key) {
  const std::string* text = value.FindString(key);
  return text ? *text : std::string();
}

uint8_t ToByte(int value) {
  return static_cast<uint8_t>(std::clamp(value, 0, 255));
}

uint16_t ToWord(int value) {
  return static_cast<uint16_t>(std::clamp(value, 0, 65535));
}

mojom::UsbDeviceInfoPtr BuildDeviceInfo(const base::DictValue& source) {
  auto info = mojom::UsbDeviceInfo::New();
  info->usb_version_major = 2;
  info->usb_version_minor = 0;
  info->usb_version_subminor = 0;
  info->class_code = ToByte(ReadInt(source, "classCode"));
  info->subclass_code = ToByte(ReadInt(source, "subclassCode"));
  info->protocol_code = ToByte(ReadInt(source, "protocolCode"));
  info->bus_number =
      static_cast<uint32_t>(std::max(ReadInt(source, "busNumber"), 0));
  info->port_number =
      static_cast<uint32_t>(std::max(ReadInt(source, "deviceAddress"), 0));
  info->vendor_id = ToWord(ReadInt(source, "vendorId"));
  info->product_id = ToWord(ReadInt(source, "productId"));
  info->device_version_major = 1;
  info->manufacturer_name =
      base::UTF8ToUTF16(ReadString(source, "manufacturerName"));
  info->product_name = base::UTF8ToUTF16(ReadString(source, "productName"));
  info->serial_number = base::UTF8ToUTF16(ReadString(source, "serialNumber"));

  const base::ListValue* configurations = source.FindList("configurations");
  if (!configurations) {
    return info;
  }
  for (const base::Value& configuration_value : *configurations) {
    const base::DictValue* configuration = configuration_value.GetIfDict();
    if (!configuration) {
      continue;
    }
    auto config = BuildUsbConfigurationInfoPtr(
        ToByte(ReadInt(*configuration, "configurationValue")),
        configuration->FindBool("selfPowered").value_or(false),
        configuration->FindBool("remoteWakeup").value_or(false),
        ToByte(ReadInt(*configuration, "maximumPower")));
    const std::string configuration_name =
        ReadString(*configuration, "configurationName");
    if (!configuration_name.empty()) {
      config->configuration_name = base::UTF8ToUTF16(configuration_name);
    }

    if (const base::ListValue* interfaces =
            configuration->FindList("interfaces")) {
      for (const base::Value& interface_value : *interfaces) {
        const base::DictValue* interface = interface_value.GetIfDict();
        if (!interface) {
          continue;
        }
        auto interface_info = BuildUsbInterfaceInfoPtr(
            ToByte(ReadInt(*interface, "interfaceNumber")),
            ToByte(ReadInt(*interface, "alternateSetting")),
            ToByte(ReadInt(*interface, "classCode")),
            ToByte(ReadInt(*interface, "subclassCode")),
            ToByte(ReadInt(*interface, "protocolCode")));
        const std::string interface_name =
            ReadString(*interface, "interfaceName");
        if (!interface_name.empty()) {
          interface_info->alternates[0]->interface_name =
              base::UTF8ToUTF16(interface_name);
        }
        if (const base::ListValue* endpoints =
                interface->FindList("endpoints")) {
          for (const base::Value& endpoint_value : *endpoints) {
            const base::DictValue* endpoint = endpoint_value.GetIfDict();
            if (!endpoint) {
              continue;
            }
            interface_info->alternates[0]->endpoints.push_back(
                BuildUsbEndpointInfoPtr(
                    ToByte(ReadInt(*endpoint, "address")),
                    ToByte(ReadInt(*endpoint, "attributes")),
                    ToWord(ReadInt(*endpoint, "maximumPacketSize")),
                    ToByte(ReadInt(*endpoint, "pollingInterval"))));
          }
        }
        config->interfaces.push_back(std::move(interface_info));
      }
    }
    AggregateInterfacesForConfig(config.get());
    info->configurations.push_back(std::move(config));
  }
  if (!info->configurations.empty()) {
    info->active_configuration =
        info->configurations.front()->configuration_value;
  }
  return info;
}

}  // namespace

// static
scoped_refptr<UsbDeviceOhos> UsbDeviceOhos::Create(
    base::WeakPtr<UsbServiceOhos> service,
    const base::DictValue& device) {
  const std::string platform_id = ReadString(device, "name");
  if (platform_id.empty()) {
    return nullptr;
  }
  return base::WrapRefCounted(new UsbDeviceOhos(
      std::move(service), BuildDeviceInfo(device), platform_id,
      device.FindBool("permissionGranted").value_or(false)));
}

UsbDeviceOhos::UsbDeviceOhos(base::WeakPtr<UsbServiceOhos> service,
                             mojom::UsbDeviceInfoPtr device_info,
                             std::string platform_id,
                             bool permission_granted)
    : UsbDevice(std::move(device_info)),
      service_(std::move(service)),
      platform_id_(std::move(platform_id)),
      permission_granted_(permission_granted) {}

UsbDeviceOhos::~UsbDeviceOhos() = default;

void UsbDeviceOhos::RequestPermission(ResultCallback callback) {
  if (permission_granted_) {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(std::move(callback), true));
    return;
  }
  base::DictValue command;
  command.Set("method", "requestPermission");
  command.Set("deviceName", platform_id_);
  SendUsbCommandOhos(
      std::move(command),
      base::BindOnce(&UsbDeviceOhos::OnPermissionResponse,
                     weak_factory_.GetWeakPtr(), std::move(callback)));
}

bool UsbDeviceOhos::permission_granted() const {
  return permission_granted_;
}

void UsbDeviceOhos::Open(OpenCallback callback) {
  if (!service_ || !permission_granted_) {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(std::move(callback), scoped_refptr<UsbDeviceHandle>()));
    return;
  }
  base::DictValue command;
  command.Set("method", "open");
  command.Set("deviceName", platform_id_);
  SendUsbCommandOhos(
      std::move(command),
      base::BindOnce(&UsbDeviceOhos::OnOpenResponse, weak_factory_.GetWeakPtr(),
                     std::move(callback)));
}

void UsbDeviceOhos::OnPermissionResponse(ResultCallback callback,
                                         base::DictValue response) {
  permission_granted_ = response.FindBool("ok").value_or(false) &&
                        response.FindBool("granted").value_or(false);
  std::move(callback).Run(permission_granted_);
}

void UsbDeviceOhos::OnOpenResponse(OpenCallback callback,
                                   base::DictValue response) {
  scoped_refptr<UsbDeviceHandle> handle;
  if (response.FindBool("ok").value_or(false)) {
    const int file_descriptor = response.FindInt("fileDescriptor").value_or(-1);
    const int pipe_id = response.FindInt("pipeId").value_or(0);
    handle = UsbDeviceHandleOhos::Create(this, file_descriptor, pipe_id);
    if (handle) {
      handles().push_back(handle.get());
    }
  }
  std::move(callback).Run(std::move(handle));
}

}  // namespace device
