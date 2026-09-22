// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "device/bluetooth/ohos/bluetooth_gatt_ohos.h"

#include <optional>
#include <utility>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/values.h"
#include "device/bluetooth/bluetooth_adapter.h"
#include "device/bluetooth/ohos/bluetooth_adapter_ohos.h"
#include "device/bluetooth/ohos/bluetooth_bridge_ohos.h"
#include "device/bluetooth/ohos/bluetooth_device_ohos.h"

namespace device {
namespace {

bool ResponseSucceeded(const base::DictValue &response) {
  return response.FindBool("ok").value_or(false);
}

BluetoothGattService::GattErrorCode
ResponseError(const base::DictValue &response) {
  switch (response.FindInt("errorCode").value_or(0)) {
  case 201:
    return BluetoothGattService::GattErrorCode::kNotAuthorized;
  case 401:
    return BluetoothGattService::GattErrorCode::kInvalidLength;
  case 801:
    return BluetoothGattService::GattErrorCode::kNotSupported;
  case 2900011:
    return BluetoothGattService::GattErrorCode::kInProgress;
  case 2901000:
  case 2901001:
    return BluetoothGattService::GattErrorCode::kNotPermitted;
  default:
    return BluetoothGattService::GattErrorCode::kFailed;
  }
}

std::vector<uint8_t> ReadBytes(const base::DictValue &response) {
  std::vector<uint8_t> bytes;
  const base::ListValue *values = response.FindList("value");
  if (!values) {
    return bytes;
  }
  bytes.reserve(values->size());
  for (const base::Value &value : *values) {
    if (value.is_int() && value.GetInt() >= 0 && value.GetInt() <= 255) {
      bytes.push_back(static_cast<uint8_t>(value.GetInt()));
    }
  }
  return bytes;
}

base::ListValue WriteBytes(base::span<const uint8_t> bytes) {
  base::ListValue values;
  values.reserve(bytes.size());
  for (uint8_t byte : bytes) {
    values.Append(static_cast<int>(byte));
  }
  return values;
}

BluetoothUUID ReadUuid(const base::DictValue &value) {
  const std::string *uuid = value.FindString("uuid");
  return BluetoothUUID(uuid ? *uuid : std::string());
}

} // namespace

BluetoothRemoteGattDescriptorOhos::BluetoothRemoteGattDescriptorOhos(
    BluetoothRemoteGattCharacteristicOhos *characteristic, std::string address,
    std::string identifier, BluetoothUUID uuid)
    : characteristic_(characteristic), address_(std::move(address)),
      identifier_(std::move(identifier)), uuid_(std::move(uuid)) {}

BluetoothRemoteGattDescriptorOhos::~BluetoothRemoteGattDescriptorOhos() =
    default;

std::string BluetoothRemoteGattDescriptorOhos::GetIdentifier() const {
  return identifier_;
}

BluetoothUUID BluetoothRemoteGattDescriptorOhos::GetUUID() const {
  return uuid_;
}

BluetoothRemoteGattCharacteristic::Permissions
BluetoothRemoteGattDescriptorOhos::GetPermissions() const {
  return BluetoothRemoteGattCharacteristic::PERMISSION_NONE;
}

const std::vector<uint8_t> &
BluetoothRemoteGattDescriptorOhos::GetValue() const {
  return value_;
}

BluetoothRemoteGattCharacteristic *
BluetoothRemoteGattDescriptorOhos::GetCharacteristic() const {
  return characteristic_;
}

void BluetoothRemoteGattDescriptorOhos::ReadRemoteDescriptor(
    ValueCallback callback) {
  base::DictValue command;
  command.Set("method", "readDescriptor");
  command.Set("address", address_);
  command.Set("descriptorId", identifier_);
  SendBluetoothCommandOhos(
      std::move(command),
      base::BindOnce(&BluetoothRemoteGattDescriptorOhos::OnRead,
                     weak_ptr_factory_.GetWeakPtr(), std::move(callback)));
}

void BluetoothRemoteGattDescriptorOhos::WriteRemoteDescriptor(
    base::span<const uint8_t> new_value, base::OnceClosure callback,
    ErrorCallback error_callback) {
  base::DictValue command;
  command.Set("method", "writeDescriptor");
  command.Set("address", address_);
  command.Set("descriptorId", identifier_);
  command.Set("value", WriteBytes(new_value));
  SendBluetoothCommandOhos(
      std::move(command),
      base::BindOnce(&BluetoothRemoteGattDescriptorOhos::OnWrite,
                     weak_ptr_factory_.GetWeakPtr(), std::move(callback),
                     std::move(error_callback)));
}

void BluetoothRemoteGattDescriptorOhos::OnRead(ValueCallback callback,
                                               base::DictValue response) {
  if (!ResponseSucceeded(response)) {
    std::move(callback).Run(ResponseError(response), std::vector<uint8_t>());
    return;
  }
  value_ = ReadBytes(response);
  std::move(callback).Run(std::nullopt, value_);
}

void BluetoothRemoteGattDescriptorOhos::OnWrite(base::OnceClosure callback,
                                                ErrorCallback error_callback,
                                                base::DictValue response) {
  if (ResponseSucceeded(response)) {
    std::move(callback).Run();
  } else {
    std::move(error_callback).Run(ResponseError(response));
  }
}

BluetoothRemoteGattCharacteristicOhos::BluetoothRemoteGattCharacteristicOhos(
    BluetoothAdapterOhos *adapter, BluetoothRemoteGattService *service,
    std::string address, const base::DictValue &characteristic)
    : adapter_(adapter), service_(service), address_(std::move(address)),
      identifier_(characteristic.FindString("identifier")
                      ? *characteristic.FindString("identifier")
                      : std::string()),
      uuid_(ReadUuid(characteristic)),
      properties_(static_cast<Properties>(
          characteristic.FindInt("properties").value_or(PROPERTY_NONE))) {
  const base::ListValue *descriptors = characteristic.FindList("descriptors");
  if (!descriptors) {
    return;
  }
  for (const base::Value &value : *descriptors) {
    if (!value.is_dict()) {
      continue;
    }
    const base::DictValue &descriptor = value.GetDict();
    const std::string *identifier = descriptor.FindString("identifier");
    if (!identifier) {
      continue;
    }
    AddDescriptor(std::make_unique<BluetoothRemoteGattDescriptorOhos>(
        this, address_, *identifier, ReadUuid(descriptor)));
  }
}

BluetoothRemoteGattCharacteristicOhos::
    ~BluetoothRemoteGattCharacteristicOhos() = default;

std::string BluetoothRemoteGattCharacteristicOhos::GetIdentifier() const {
  return identifier_;
}

BluetoothUUID BluetoothRemoteGattCharacteristicOhos::GetUUID() const {
  return uuid_;
}

const std::vector<uint8_t> &
BluetoothRemoteGattCharacteristicOhos::GetValue() const {
  return value_;
}

BluetoothRemoteGattService *
BluetoothRemoteGattCharacteristicOhos::GetService() const {
  return service_;
}

BluetoothRemoteGattCharacteristicOhos::Properties
BluetoothRemoteGattCharacteristicOhos::GetProperties() const {
  return properties_;
}

BluetoothRemoteGattCharacteristicOhos::Permissions
BluetoothRemoteGattCharacteristicOhos::GetPermissions() const {
  Permissions permissions = PERMISSION_NONE;
  if (properties_ & PROPERTY_READ) {
    permissions = static_cast<Permissions>(permissions | PERMISSION_READ);
  }
  if (properties_ & (PROPERTY_WRITE | PROPERTY_WRITE_WITHOUT_RESPONSE)) {
    permissions = static_cast<Permissions>(permissions | PERMISSION_WRITE);
  }
  return permissions;
}

void BluetoothRemoteGattCharacteristicOhos::ReadRemoteCharacteristic(
    ValueCallback callback) {
  base::DictValue command;
  command.Set("method", "readCharacteristic");
  command.Set("address", address_);
  command.Set("characteristicId", identifier_);
  SendBluetoothCommandOhos(
      std::move(command),
      base::BindOnce(&BluetoothRemoteGattCharacteristicOhos::OnRead,
                     weak_ptr_factory_.GetWeakPtr(), std::move(callback)));
}

void BluetoothRemoteGattCharacteristicOhos::WriteRemoteCharacteristic(
    base::span<const uint8_t> value, WriteType write_type,
    base::OnceClosure callback, ErrorCallback error_callback) {
  base::DictValue command;
  command.Set("method", "writeCharacteristic");
  command.Set("address", address_);
  command.Set("characteristicId", identifier_);
  command.Set("withResponse", write_type == WriteType::kWithResponse);
  command.Set("value", WriteBytes(value));
  SendBluetoothCommandOhos(
      std::move(command),
      base::BindOnce(&BluetoothRemoteGattCharacteristicOhos::OnWrite,
                     weak_ptr_factory_.GetWeakPtr(), std::move(callback),
                     std::move(error_callback)));
}

void BluetoothRemoteGattCharacteristicOhos::DeprecatedWriteRemoteCharacteristic(
    base::span<const uint8_t> value, base::OnceClosure callback,
    ErrorCallback error_callback) {
  WriteRemoteCharacteristic(value, WriteType::kWithResponse,
                            std::move(callback), std::move(error_callback));
}

void BluetoothRemoteGattCharacteristicOhos::OnValueChanged(
    std::vector<uint8_t> value) {
  value_ = std::move(value);
  adapter_->NotifyGattCharacteristicValueChanged(this, value_);
}

void BluetoothRemoteGattCharacteristicOhos::SubscribeToNotifications(
    BluetoothRemoteGattDescriptor *ccc_descriptor, base::OnceClosure callback,
    ErrorCallback error_callback) {
  SetNotifications(true, std::move(callback), std::move(error_callback));
}

void BluetoothRemoteGattCharacteristicOhos::UnsubscribeFromNotifications(
    BluetoothRemoteGattDescriptor *ccc_descriptor, base::OnceClosure callback,
    ErrorCallback error_callback) {
  SetNotifications(false, std::move(callback), std::move(error_callback));
}

void BluetoothRemoteGattCharacteristicOhos::OnRead(ValueCallback callback,
                                                   base::DictValue response) {
  if (!ResponseSucceeded(response)) {
    std::move(callback).Run(ResponseError(response), std::vector<uint8_t>());
    return;
  }
  value_ = ReadBytes(response);
  std::move(callback).Run(std::nullopt, value_);
}

void BluetoothRemoteGattCharacteristicOhos::OnWrite(
    base::OnceClosure callback, ErrorCallback error_callback,
    base::DictValue response) {
  if (ResponseSucceeded(response)) {
    std::move(callback).Run();
  } else {
    std::move(error_callback).Run(ResponseError(response));
  }
}

void BluetoothRemoteGattCharacteristicOhos::SetNotifications(
    bool enabled, base::OnceClosure callback, ErrorCallback error_callback) {
  base::DictValue command;
  command.Set("method", "setNotify");
  command.Set("address", address_);
  command.Set("characteristicId", identifier_);
  command.Set("enabled", enabled);
  SendBluetoothCommandOhos(
      std::move(command),
      base::BindOnce(&BluetoothRemoteGattCharacteristicOhos::OnWrite,
                     weak_ptr_factory_.GetWeakPtr(), std::move(callback),
                     std::move(error_callback)));
}

BluetoothRemoteGattServiceOhos::BluetoothRemoteGattServiceOhos(
    BluetoothAdapterOhos *adapter, BluetoothDeviceOhos *device,
    std::string address, const base::DictValue &service)
    : device_(device), identifier_(service.FindString("identifier")
                                       ? *service.FindString("identifier")
                                       : std::string()),
      uuid_(ReadUuid(service)),
      primary_(service.FindBool("primary").value_or(true)) {
  const base::ListValue *characteristics = service.FindList("characteristics");
  if (characteristics) {
    for (const base::Value &value : *characteristics) {
      if (value.is_dict()) {
        AddCharacteristic(
            std::make_unique<BluetoothRemoteGattCharacteristicOhos>(
                adapter, this, address, value.GetDict()));
      }
    }
  }
  SetDiscoveryComplete(true);
}

BluetoothRemoteGattServiceOhos::~BluetoothRemoteGattServiceOhos() = default;

std::string BluetoothRemoteGattServiceOhos::GetIdentifier() const {
  return identifier_;
}

BluetoothUUID BluetoothRemoteGattServiceOhos::GetUUID() const { return uuid_; }

bool BluetoothRemoteGattServiceOhos::IsPrimary() const { return primary_; }

BluetoothDevice *BluetoothRemoteGattServiceOhos::GetDevice() const {
  return device_;
}

std::vector<BluetoothRemoteGattService *>
BluetoothRemoteGattServiceOhos::GetIncludedServices() const {
  return {};
}

bool BluetoothRemoteGattServiceOhos::DispatchCharacteristicValue(
    const std::string &identifier, const std::vector<uint8_t> &value) {
  BluetoothRemoteGattCharacteristic *characteristic =
      GetCharacteristic(identifier);
  if (!characteristic) {
    return false;
  }
  static_cast<BluetoothRemoteGattCharacteristicOhos *>(characteristic)
      ->OnValueChanged(value);
  return true;
}

} // namespace device
