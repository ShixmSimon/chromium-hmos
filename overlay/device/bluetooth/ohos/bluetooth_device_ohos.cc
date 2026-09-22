// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "device/bluetooth/ohos/bluetooth_device_ohos.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/location.h"
#include "base/task/single_thread_task_runner.h"
#include "base/values.h"
#include "device/bluetooth/ohos/bluetooth_adapter_ohos.h"
#include "device/bluetooth/ohos/bluetooth_bridge_ohos.h"
#include "device/bluetooth/ohos/bluetooth_gatt_ohos.h"

namespace device {

BluetoothDeviceOhos::BluetoothDeviceOhos(BluetoothAdapterOhos *adapter,
                                         std::string address,
                                         std::optional<std::string> name,
                                         bool connectable)
    : BluetoothDevice(adapter), address_(std::move(address)),
      name_(std::move(name)), connectable_(connectable) {}

BluetoothDeviceOhos::~BluetoothDeviceOhos() = default;

uint32_t BluetoothDeviceOhos::GetBluetoothClass() const { return 0; }

BluetoothTransport BluetoothDeviceOhos::GetType() const {
  return BLUETOOTH_TRANSPORT_LE;
}

std::string BluetoothDeviceOhos::GetAddress() const { return address_; }

BluetoothDevice::AddressType BluetoothDeviceOhos::GetAddressType() const {
  return ADDR_TYPE_UNKNOWN;
}

BluetoothDevice::VendorIDSource BluetoothDeviceOhos::GetVendorIDSource() const {
  return VENDOR_ID_UNKNOWN;
}

uint16_t BluetoothDeviceOhos::GetVendorID() const { return 0; }

uint16_t BluetoothDeviceOhos::GetProductID() const { return 0; }

uint16_t BluetoothDeviceOhos::GetDeviceID() const { return 0; }

uint16_t BluetoothDeviceOhos::GetAppearance() const { return 0; }

std::optional<std::string> BluetoothDeviceOhos::GetName() const {
  return name_;
}

bool BluetoothDeviceOhos::IsPaired() const { return false; }

bool BluetoothDeviceOhos::IsConnected() const { return gatt_connected_; }

bool BluetoothDeviceOhos::IsGattConnected() const { return gatt_connected_; }

bool BluetoothDeviceOhos::IsConnectable() const { return connectable_; }

bool BluetoothDeviceOhos::IsConnecting() const { return connecting_; }

bool BluetoothDeviceOhos::ExpectingPinCode() const { return false; }

bool BluetoothDeviceOhos::ExpectingPasskey() const { return false; }

bool BluetoothDeviceOhos::ExpectingConfirmation() const { return false; }

void BluetoothDeviceOhos::GetConnectionInfo(ConnectionInfoCallback callback) {
  std::move(callback).Run(ConnectionInfo());
}

void BluetoothDeviceOhos::SetConnectionLatency(
    ConnectionLatency connection_latency, base::OnceClosure callback,
    ErrorCallback error_callback) {
  std::move(error_callback).Run();
}

void BluetoothDeviceOhos::Connect(PairingDelegate *pairing_delegate,
                                  ConnectCallback callback) {
  std::move(callback).Run(ERROR_UNSUPPORTED_DEVICE);
}

void BluetoothDeviceOhos::SetPinCode(const std::string &pincode) {}

void BluetoothDeviceOhos::SetPasskey(uint32_t passkey) {}

void BluetoothDeviceOhos::ConfirmPairing() {}

void BluetoothDeviceOhos::RejectPairing() {}

void BluetoothDeviceOhos::CancelPairing() {}

void BluetoothDeviceOhos::Disconnect(base::OnceClosure callback,
                                     ErrorCallback error_callback) {
  DisconnectGatt();
  std::move(callback).Run();
}

void BluetoothDeviceOhos::Forget(base::OnceClosure callback,
                                 ErrorCallback error_callback) {
  DisconnectGatt();
  std::move(callback).Run();
}

void BluetoothDeviceOhos::ConnectToService(
    const BluetoothUUID &uuid, ConnectToServiceCallback callback,
    ConnectToServiceErrorCallback error_callback) {
  std::move(error_callback).Run("Classic Bluetooth sockets are unsupported");
}

void BluetoothDeviceOhos::ConnectToServiceInsecurely(
    const BluetoothUUID &uuid, ConnectToServiceCallback callback,
    ConnectToServiceErrorCallback error_callback) {
  std::move(error_callback).Run("Classic Bluetooth sockets are unsupported");
}

void BluetoothDeviceOhos::UpdateScanData(std::optional<std::string> name,
                                         bool connectable) {
  if (name && !name->empty()) {
    name_ = std::move(name);
  }
  connectable_ = connectable;
  UpdateTimestamp();
}

void BluetoothDeviceOhos::OnGattConnectionState(bool connected) {
  const bool was_connected = gatt_connected_;
  connecting_ = false;
  gatt_connected_ = connected;

  if (connected) {
    DidConnectGatt(std::nullopt);
    DiscoverServices();
  } else if (!create_gatt_connection_callbacks_.empty()) {
    DidConnectGatt(ERROR_FAILED);
  } else if (was_connected) {
    gatt_services_.clear();
    device_uuids_.ClearServiceUUIDs();
    SetGattServicesDiscoveryComplete(false);
    DidDisconnectGatt();
  }
  GetOhosAdapter()->NotifyDeviceChanged(this);
}

void BluetoothDeviceOhos::OnCharacteristicValue(
    const std::string &identifier, const std::vector<uint8_t> &value) {
  for (const auto &entry : gatt_services_) {
    auto *service =
        static_cast<BluetoothRemoteGattServiceOhos *>(entry.second.get());
    if (service->DispatchCharacteristicValue(identifier, value)) {
      return;
    }
  }
}

void BluetoothDeviceOhos::CreateGattConnectionImpl(
    std::optional<BluetoothUUID> service_uuid) {
  if (gatt_connected_) {
    DidConnectGatt(std::nullopt);
    return;
  }
  if (connecting_) {
    return;
  }
  connecting_ = true;
  base::DictValue command;
  command.Set("method", "connect");
  command.Set("address", address_);
  SendBluetoothCommandOhos(
      std::move(command), base::BindOnce(&BluetoothDeviceOhos::OnConnectCommand,
                                         weak_ptr_factory_.GetWeakPtr()));
}

void BluetoothDeviceOhos::DisconnectGatt() {
  base::DictValue command;
  command.Set("method", "disconnect");
  command.Set("address", address_);
  SendBluetoothCommandOhos(std::move(command), base::DoNothing());
}

void BluetoothDeviceOhos::OnConnectCommand(base::DictValue response) {
  if (response.FindBool("ok").value_or(false)) {
    return;
  }
  connecting_ = false;
  DidConnectGatt(ERROR_FAILED);
}

void BluetoothDeviceOhos::DiscoverServices() {
  base::DictValue command;
  command.Set("method", "getServices");
  command.Set("address", address_);
  SendBluetoothCommandOhos(std::move(command),
                           base::BindOnce(&BluetoothDeviceOhos::OnServices,
                                          weak_ptr_factory_.GetWeakPtr()));
}

void BluetoothDeviceOhos::OnServices(base::DictValue response) {
  if (!gatt_connected_ || !response.FindBool("ok").value_or(false)) {
    return;
  }

  gatt_services_.clear();
  const base::ListValue *services = response.FindList("services");
  if (services) {
    for (const base::Value &value : *services) {
      if (!value.is_dict()) {
        continue;
      }
      auto service = std::make_unique<BluetoothRemoteGattServiceOhos>(
          GetOhosAdapter(), this, address_, value.GetDict());
      const std::string identifier = service->GetIdentifier();
      BluetoothRemoteGattServiceOhos *service_ptr = service.get();
      gatt_services_[identifier] = std::move(service);
      GetOhosAdapter()->NotifyGattServiceAdded(service_ptr);
    }
  }

  device_uuids_.ReplaceServiceUUIDs(gatt_services_);
  SetGattServicesDiscoveryComplete(true);
  GetOhosAdapter()->NotifyGattServicesDiscovered(this);
  GetOhosAdapter()->NotifyDeviceChanged(this);
}

BluetoothAdapterOhos *BluetoothDeviceOhos::GetOhosAdapter() const {
  return static_cast<BluetoothAdapterOhos *>(adapter_);
}

} // namespace device
