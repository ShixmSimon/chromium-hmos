// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DEVICE_BLUETOOTH_OHOS_BLUETOOTH_ADAPTER_OHOS_H_
#define DEVICE_BLUETOOTH_OHOS_BLUETOOTH_ADAPTER_OHOS_H_

#include <memory>
#include <string>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "device/bluetooth/bluetooth_adapter.h"

namespace device {

class BluetoothAdapterOhos final : public BluetoothAdapter {
public:
  static scoped_refptr<BluetoothAdapterOhos> Create();

  BluetoothAdapterOhos(const BluetoothAdapterOhos &) = delete;
  BluetoothAdapterOhos &operator=(const BluetoothAdapterOhos &) = delete;

  void Initialize(base::OnceClosure callback) override;
  std::string GetAddress() const override;
  std::string GetName() const override;
  void SetName(const std::string &name, base::OnceClosure callback,
               ErrorCallback error_callback) override;
  bool IsInitialized() const override;
  bool IsPresent() const override;
  bool IsPowered() const override;
  PermissionStatus GetOsPermissionStatus() const override;
  bool IsDiscoverable() const override;
  void SetDiscoverable(bool discoverable, base::OnceClosure callback,
                       ErrorCallback error_callback) override;
  bool IsDiscovering() const override;
  UUIDList GetUUIDs() const override;
  void CreateRfcommService(const BluetoothUUID &uuid,
                           const ServiceOptions &options,
                           CreateServiceCallback callback,
                           CreateServiceErrorCallback error_callback) override;
  void CreateL2capService(const BluetoothUUID &uuid,
                          const ServiceOptions &options,
                          CreateServiceCallback callback,
                          CreateServiceErrorCallback error_callback) override;
  void RegisterAdvertisement(
      std::unique_ptr<BluetoothAdvertisement::Data> advertisement_data,
      CreateAdvertisementCallback callback,
      AdvertisementErrorCallback error_callback) override;
  BluetoothLocalGattService *
  GetGattService(const std::string &identifier) const override;

protected:
  BluetoothAdapterOhos();
  ~BluetoothAdapterOhos() override;

  base::WeakPtr<BluetoothAdapter> GetWeakPtr() override;
  bool SetPoweredImpl(bool powered) override;
  void StartScanWithFilter(
      std::unique_ptr<BluetoothDiscoveryFilter> discovery_filter,
      DiscoverySessionResultCallback callback) override;
  void UpdateFilter(std::unique_ptr<BluetoothDiscoveryFilter> discovery_filter,
                    DiscoverySessionResultCallback callback) override;
  void StopScan(DiscoverySessionResultCallback callback) override;
  void RemovePairingDelegateInternal(
      BluetoothDevice::PairingDelegate *pairing_delegate) override;

private:
  void OnInitialized(base::OnceClosure callback, base::DictValue response);
  void OnScanStarted(DiscoverySessionResultCallback callback,
                     base::DictValue response);
  void OnScanStopped(DiscoverySessionResultCallback callback,
                     base::DictValue response);
  void OnPlatformEvent(const base::DictValue &event);
  void OnScanResult(const base::DictValue &event);

  bool initialized_ = false;
  bool present_ = false;
  bool powered_ = false;
  bool discovering_ = false;
  base::WeakPtrFactory<BluetoothAdapterOhos> weak_ptr_factory_{this};
};

} // namespace device

#endif // DEVICE_BLUETOOTH_OHOS_BLUETOOTH_ADAPTER_OHOS_H_
