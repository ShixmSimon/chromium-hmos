// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DEVICE_BLUETOOTH_OHOS_BLUETOOTH_GATT_OHOS_H_
#define DEVICE_BLUETOOTH_OHOS_BLUETOOTH_GATT_OHOS_H_

#include <memory>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "device/bluetooth/bluetooth_remote_gatt_characteristic.h"
#include "device/bluetooth/bluetooth_remote_gatt_descriptor.h"
#include "device/bluetooth/bluetooth_remote_gatt_service.h"

namespace device {

class BluetoothAdapterOhos;
class BluetoothDeviceOhos;
class BluetoothRemoteGattCharacteristicOhos;

class BluetoothRemoteGattDescriptorOhos final
    : public BluetoothRemoteGattDescriptor {
public:
  BluetoothRemoteGattDescriptorOhos(
      BluetoothRemoteGattCharacteristicOhos *characteristic,
      std::string address, std::string identifier, BluetoothUUID uuid);
  ~BluetoothRemoteGattDescriptorOhos() override;

  std::string GetIdentifier() const override;
  BluetoothUUID GetUUID() const override;
  BluetoothRemoteGattCharacteristic::Permissions
  GetPermissions() const override;
  const std::vector<uint8_t> &GetValue() const override;
  BluetoothRemoteGattCharacteristic *GetCharacteristic() const override;
  void ReadRemoteDescriptor(ValueCallback callback) override;
  void WriteRemoteDescriptor(base::span<const uint8_t> new_value,
                             base::OnceClosure callback,
                             ErrorCallback error_callback) override;

private:
  void OnRead(ValueCallback callback, base::DictValue response);
  void OnWrite(base::OnceClosure callback, ErrorCallback error_callback,
               base::DictValue response);

  raw_ptr<BluetoothRemoteGattCharacteristicOhos> characteristic_;
  const std::string address_;
  const std::string identifier_;
  const BluetoothUUID uuid_;
  std::vector<uint8_t> value_;
  base::WeakPtrFactory<BluetoothRemoteGattDescriptorOhos> weak_ptr_factory_{
      this};
};

class BluetoothRemoteGattCharacteristicOhos final
    : public BluetoothRemoteGattCharacteristic {
public:
  BluetoothRemoteGattCharacteristicOhos(BluetoothAdapterOhos *adapter,
                                        BluetoothRemoteGattService *service,
                                        std::string address,
                                        const base::DictValue &characteristic);
  ~BluetoothRemoteGattCharacteristicOhos() override;

  std::string GetIdentifier() const override;
  BluetoothUUID GetUUID() const override;
  const std::vector<uint8_t> &GetValue() const override;
  BluetoothRemoteGattService *GetService() const override;
  Properties GetProperties() const override;
  Permissions GetPermissions() const override;
  void ReadRemoteCharacteristic(ValueCallback callback) override;
  void WriteRemoteCharacteristic(base::span<const uint8_t> value,
                                 WriteType write_type,
                                 base::OnceClosure callback,
                                 ErrorCallback error_callback) override;
  void
  DeprecatedWriteRemoteCharacteristic(base::span<const uint8_t> value,
                                      base::OnceClosure callback,
                                      ErrorCallback error_callback) override;

  void OnValueChanged(std::vector<uint8_t> value);

protected:
  void SubscribeToNotifications(BluetoothRemoteGattDescriptor *ccc_descriptor,
                                base::OnceClosure callback,
                                ErrorCallback error_callback) override;
  void
  UnsubscribeFromNotifications(BluetoothRemoteGattDescriptor *ccc_descriptor,
                               base::OnceClosure callback,
                               ErrorCallback error_callback) override;

private:
  void OnRead(ValueCallback callback, base::DictValue response);
  void OnWrite(base::OnceClosure callback, ErrorCallback error_callback,
               base::DictValue response);
  void SetNotifications(bool enabled, base::OnceClosure callback,
                        ErrorCallback error_callback);

  raw_ptr<BluetoothAdapterOhos> adapter_;
  raw_ptr<BluetoothRemoteGattService> service_;
  const std::string address_;
  const std::string identifier_;
  const BluetoothUUID uuid_;
  const Properties properties_;
  std::vector<uint8_t> value_;
  base::WeakPtrFactory<BluetoothRemoteGattCharacteristicOhos> weak_ptr_factory_{
      this};
};

class BluetoothRemoteGattServiceOhos final : public BluetoothRemoteGattService {
public:
  BluetoothRemoteGattServiceOhos(BluetoothAdapterOhos *adapter,
                                 BluetoothDeviceOhos *device,
                                 std::string address,
                                 const base::DictValue &service);
  ~BluetoothRemoteGattServiceOhos() override;

  std::string GetIdentifier() const override;
  BluetoothUUID GetUUID() const override;
  bool IsPrimary() const override;
  BluetoothDevice *GetDevice() const override;
  std::vector<BluetoothRemoteGattService *>
  GetIncludedServices() const override;

  bool DispatchCharacteristicValue(const std::string &identifier,
                                   const std::vector<uint8_t> &value);

private:
  raw_ptr<BluetoothDeviceOhos> device_;
  const std::string identifier_;
  const BluetoothUUID uuid_;
  const bool primary_;
};

} // namespace device

#endif // DEVICE_BLUETOOTH_OHOS_BLUETOOTH_GATT_OHOS_H_
