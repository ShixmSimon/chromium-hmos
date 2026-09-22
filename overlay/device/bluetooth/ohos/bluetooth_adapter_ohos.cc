// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "device/bluetooth/ohos/bluetooth_adapter_ohos.h"

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/task/single_thread_task_runner.h"
#include "base/values.h"
#include "device/bluetooth/bluetooth_discovery_session_outcome.h"
#include "device/bluetooth/ohos/bluetooth_bridge_ohos.h"
#include "device/bluetooth/ohos/bluetooth_device_ohos.h"

namespace device {
namespace {

std::vector<uint8_t> ReadBytes(const base::Value *value) {
  std::vector<uint8_t> bytes;
  if (!value || !value->is_list()) {
    return bytes;
  }
  for (const base::Value &byte : value->GetList()) {
    if (byte.is_int() && byte.GetInt() >= 0 && byte.GetInt() <= 255) {
      bytes.push_back(static_cast<uint8_t>(byte.GetInt()));
    }
  }
  return bytes;
}

int8_t ClampPower(int value) {
  return static_cast<int8_t>(std::clamp(value, -128, 127));
}

} // namespace

scoped_refptr<BluetoothAdapter> BluetoothAdapter::CreateAdapter() {
  return BluetoothAdapterOhos::Create();
}

scoped_refptr<BluetoothAdapterOhos> BluetoothAdapterOhos::Create() {
  auto adapter = base::WrapRefCounted(new BluetoothAdapterOhos());
  adapter->ui_task_runner_ = base::SingleThreadTaskRunner::GetCurrentDefault();
  return adapter;
}

BluetoothAdapterOhos::BluetoothAdapterOhos() = default;

BluetoothAdapterOhos::~BluetoothAdapterOhos() {
  SetBluetoothEventCallbackOhos(BluetoothEventCallbackOhos());
}

void BluetoothAdapterOhos::Initialize(base::OnceClosure callback) {
  SetBluetoothEventCallbackOhos(base::BindRepeating(
      &BluetoothAdapterOhos::OnPlatformEvent, weak_ptr_factory_.GetWeakPtr()));
  base::DictValue command;
  command.Set("method", "getAdapterState");
  SendBluetoothCommandOhos(std::move(command),
                           base::BindOnce(&BluetoothAdapterOhos::OnInitialized,
                                          weak_ptr_factory_.GetWeakPtr(),
                                          std::move(callback)));
}

std::string BluetoothAdapterOhos::GetAddress() const {
  return "00:00:00:00:00:00";
}

std::string BluetoothAdapterOhos::GetName() const {
  return "HarmonyOS Bluetooth";
}

void BluetoothAdapterOhos::SetName(const std::string &name,
                                   base::OnceClosure callback,
                                   ErrorCallback error_callback) {
  std::move(error_callback).Run();
}

bool BluetoothAdapterOhos::IsInitialized() const { return initialized_; }

bool BluetoothAdapterOhos::IsPresent() const { return present_; }

bool BluetoothAdapterOhos::IsPowered() const { return powered_; }

BluetoothAdapter::PermissionStatus
BluetoothAdapterOhos::GetOsPermissionStatus() const {
  // The ArkTS bridge requests ACCESS_BLUETOOTH immediately before the first
  // scan, where a UIAbilityContext is available to show the system dialog.
  return PermissionStatus::kAllowed;
}

bool BluetoothAdapterOhos::IsDiscoverable() const { return false; }

void BluetoothAdapterOhos::SetDiscoverable(bool discoverable,
                                           base::OnceClosure callback,
                                           ErrorCallback error_callback) {
  std::move(error_callback).Run();
}

bool BluetoothAdapterOhos::IsDiscovering() const { return discovering_; }

BluetoothAdapter::UUIDList BluetoothAdapterOhos::GetUUIDs() const { return {}; }

void BluetoothAdapterOhos::CreateRfcommService(
    const BluetoothUUID &uuid, const ServiceOptions &options,
    CreateServiceCallback callback, CreateServiceErrorCallback error_callback) {
  std::move(error_callback)
      .Run("RFCOMM is unsupported on the OHOS BLE backend");
}

void BluetoothAdapterOhos::CreateL2capService(
    const BluetoothUUID &uuid, const ServiceOptions &options,
    CreateServiceCallback callback, CreateServiceErrorCallback error_callback) {
  std::move(error_callback).Run("L2CAP is unsupported on the OHOS BLE backend");
}

void BluetoothAdapterOhos::RegisterAdvertisement(
    std::unique_ptr<BluetoothAdvertisement::Data> advertisement_data,
    CreateAdvertisementCallback callback,
    AdvertisementErrorCallback error_callback) {
  std::move(error_callback)
      .Run(BluetoothAdvertisement::ERROR_UNSUPPORTED_PLATFORM);
}

BluetoothLocalGattService *
BluetoothAdapterOhos::GetGattService(const std::string &identifier) const {
  return nullptr;
}

base::WeakPtr<BluetoothAdapter> BluetoothAdapterOhos::GetWeakPtr() {
  return weak_ptr_factory_.GetWeakPtr();
}

bool BluetoothAdapterOhos::SetPoweredImpl(bool powered) { return false; }

void BluetoothAdapterOhos::StartScanWithFilter(
    std::unique_ptr<BluetoothDiscoveryFilter> discovery_filter,
    DiscoverySessionResultCallback callback) {
  base::DictValue command;
  command.Set("method", "startScan");
  SendBluetoothCommandOhos(std::move(command),
                           base::BindOnce(&BluetoothAdapterOhos::OnScanStarted,
                                          weak_ptr_factory_.GetWeakPtr(),
                                          std::move(callback)));
}

void BluetoothAdapterOhos::UpdateFilter(
    std::unique_ptr<BluetoothDiscoveryFilter> discovery_filter,
    DiscoverySessionResultCallback callback) {
  std::move(callback).Run(/*is_error=*/false,
                          UMABluetoothDiscoverySessionOutcome::SUCCESS);
}

void BluetoothAdapterOhos::StopScan(DiscoverySessionResultCallback callback) {
  base::DictValue command;
  command.Set("method", "stopScan");
  SendBluetoothCommandOhos(std::move(command),
                           base::BindOnce(&BluetoothAdapterOhos::OnScanStopped,
                                          weak_ptr_factory_.GetWeakPtr(),
                                          std::move(callback)));
}

void BluetoothAdapterOhos::RemovePairingDelegateInternal(
    BluetoothDevice::PairingDelegate *pairing_delegate) {}

void BluetoothAdapterOhos::OnInitialized(base::OnceClosure callback,
                                         base::DictValue response) {
  initialized_ = true;
  present_ = response.FindBool("ok").value_or(false);
  powered_ = response.FindBool("powered").value_or(false);
  std::move(callback).Run();
}

void BluetoothAdapterOhos::OnScanStarted(
    DiscoverySessionResultCallback callback, base::DictValue response) {
  const bool success = response.FindBool("ok").value_or(false);
  discovering_ = success;
  std::move(callback).Run(
      /*is_error=*/!success, success
                                 ? UMABluetoothDiscoverySessionOutcome::SUCCESS
                                 : UMABluetoothDiscoverySessionOutcome::FAILED);
}

void BluetoothAdapterOhos::OnScanStopped(
    DiscoverySessionResultCallback callback, base::DictValue response) {
  discovering_ = false;
  std::move(callback).Run(/*is_error=*/false,
                          UMABluetoothDiscoverySessionOutcome::SUCCESS);
  for (const auto &entry : devices_) {
    entry.second->ClearAdvertisementData();
  }
}

void BluetoothAdapterOhos::OnPlatformEvent(const base::DictValue &event) {
  const std::string *type = event.FindString("event");
  if (!type) {
    return;
  }
  if (*type == "scanResult") {
    OnScanResult(event);
    return;
  }
  if (*type == "adapterState") {
    const bool powered = event.FindBool("powered").value_or(false);
    if (powered_ != powered) {
      powered_ = powered;
      NotifyAdapterPoweredChanged(powered_);
    }
    return;
  }
  if (*type == "connectionState") {
    const std::string *address = event.FindString("address");
    if (!address) {
      return;
    }
    auto it = devices_.find(*address);
    if (it != devices_.end()) {
      static_cast<BluetoothDeviceOhos *>(it->second.get())
          ->OnGattConnectionState(event.FindBool("connected").value_or(false));
    }
    return;
  }
  if (*type == "characteristicValue") {
    const std::string *address = event.FindString("address");
    const std::string *identifier = event.FindString("characteristicId");
    if (!address || !identifier) {
      return;
    }
    auto it = devices_.find(*address);
    if (it != devices_.end()) {
      static_cast<BluetoothDeviceOhos *>(it->second.get())
          ->OnCharacteristicValue(*identifier, ReadBytes(event.Find("value")));
    }
  }
}

void BluetoothAdapterOhos::OnScanResult(const base::DictValue &event) {
  const std::string *address = event.FindString("address");
  if (!address || address->empty()) {
    return;
  }
  const std::string *name_value = event.FindString("name");
  std::optional<std::string> name;
  if (name_value && !name_value->empty()) {
    name = *name_value;
  }
  const bool connectable = event.FindBool("connectable").value_or(true);

  BluetoothDeviceOhos *device = nullptr;
  bool is_new = false;
  auto it = devices_.find(*address);
  if (it == devices_.end()) {
    is_new = true;
    auto new_device = std::make_unique<BluetoothDeviceOhos>(this, *address,
                                                            name, connectable);
    device = new_device.get();
    devices_[*address] = std::move(new_device);
  } else {
    device = static_cast<BluetoothDeviceOhos *>(it->second.get());
    device->UpdateScanData(name, connectable);
  }

  BluetoothDevice::UUIDList uuids;
  if (const base::ListValue *values = event.FindList("uuids")) {
    for (const base::Value &value : *values) {
      if (value.is_string()) {
        uuids.emplace_back(value.GetString());
      }
    }
  }

  BluetoothDevice::ServiceDataMap service_data;
  if (const base::ListValue *values = event.FindList("serviceData")) {
    for (const base::Value &value : *values) {
      if (!value.is_dict()) {
        continue;
      }
      const std::string *uuid = value.GetDict().FindString("uuid");
      if (uuid) {
        service_data.emplace(BluetoothUUID(*uuid),
                             ReadBytes(value.GetDict().Find("value")));
      }
    }
  }

  BluetoothDevice::ManufacturerDataMap manufacturer_data;
  if (const base::ListValue *values = event.FindList("manufacturerData")) {
    for (const base::Value &value : *values) {
      if (!value.is_dict()) {
        continue;
      }
      const std::optional<int> id = value.GetDict().FindInt("id");
      if (id && *id >= 0 && *id <= 0xffff) {
        manufacturer_data.emplace(static_cast<uint16_t>(*id),
                                  ReadBytes(value.GetDict().Find("value")));
      }
    }
  }

  std::optional<uint8_t> flags;
  if (const std::optional<int> value = event.FindInt("flags");
      value && *value >= 0 && *value <= 255) {
    flags = static_cast<uint8_t>(*value);
  }
  std::optional<int8_t> tx_power;
  if (const std::optional<int> value = event.FindInt("txPower")) {
    tx_power = ClampPower(*value);
  }

  device->UpdateAdvertisementData(
      ClampPower(event.FindInt("rssi").value_or(-128)), flags, std::move(uuids),
      tx_power, std::move(service_data), std::move(manufacturer_data));

  if (is_new) {
    for (auto &observer : observers_) {
      observer.DeviceAdded(this, device);
    }
  } else {
    NotifyDeviceChanged(device);
  }
}

} // namespace device
