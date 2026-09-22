// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DEVICE_BLUETOOTH_OHOS_BLUETOOTH_DEVICE_OHOS_H_
#define DEVICE_BLUETOOTH_OHOS_BLUETOOTH_DEVICE_OHOS_H_

#include <optional>
#include <string>
#include <vector>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "device/bluetooth/bluetooth_device.h"

namespace device {

class BluetoothAdapterOhos;

class BluetoothDeviceOhos final : public BluetoothDevice {
public:
  BluetoothDeviceOhos(BluetoothAdapterOhos *adapter, std::string address,
                      std::optional<std::string> name, bool connectable);
  ~BluetoothDeviceOhos() override;

  uint32_t GetBluetoothClass() const override;
  BluetoothTransport GetType() const override;
  std::string GetAddress() const override;
  AddressType GetAddressType() const override;
  VendorIDSource GetVendorIDSource() const override;
  uint16_t GetVendorID() const override;
  uint16_t GetProductID() const override;
  uint16_t GetDeviceID() const override;
  uint16_t GetAppearance() const override;
  std::optional<std::string> GetName() const override;
  bool IsPaired() const override;
  bool IsConnected() const override;
  bool IsGattConnected() const override;
  bool IsConnectable() const override;
  bool IsConnecting() const override;
  bool ExpectingPinCode() const override;
  bool ExpectingPasskey() const override;
  bool ExpectingConfirmation() const override;
  void GetConnectionInfo(ConnectionInfoCallback callback) override;
  void SetConnectionLatency(ConnectionLatency connection_latency,
                            base::OnceClosure callback,
                            ErrorCallback error_callback) override;
  void Connect(PairingDelegate *pairing_delegate,
               ConnectCallback callback) override;
  void SetPinCode(const std::string &pincode) override;
  void SetPasskey(uint32_t passkey) override;
  void ConfirmPairing() override;
  void RejectPairing() override;
  void CancelPairing() override;
  void Disconnect(base::OnceClosure callback,
                  ErrorCallback error_callback) override;
  void Forget(base::OnceClosure callback,
              ErrorCallback error_callback) override;
  void ConnectToService(const BluetoothUUID &uuid,
                        ConnectToServiceCallback callback,
                        ConnectToServiceErrorCallback error_callback) override;
  void ConnectToServiceInsecurely(
      const BluetoothUUID &uuid, ConnectToServiceCallback callback,
      ConnectToServiceErrorCallback error_callback) override;

  void UpdateScanData(std::optional<std::string> name, bool connectable);
  void OnGattConnectionState(bool connected);
  void OnCharacteristicValue(const std::string &identifier,
                             const std::vector<uint8_t> &value);

private:
  void
  CreateGattConnectionImpl(std::optional<BluetoothUUID> service_uuid) override;
  void DisconnectGatt() override;
  void OnConnectCommand(base::DictValue response);
  void DiscoverServices();
  void OnServices(base::DictValue response);

  BluetoothAdapterOhos *GetOhosAdapter() const;

  const std::string address_;
  std::optional<std::string> name_;
  bool connectable_;
  bool connecting_ = false;
  bool gatt_connected_ = false;
  base::WeakPtrFactory<BluetoothDeviceOhos> weak_ptr_factory_{this};
};

} // namespace device

#endif // DEVICE_BLUETOOTH_OHOS_BLUETOOTH_DEVICE_OHOS_H_
