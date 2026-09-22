// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DEVICE_BLUETOOTH_OHOS_BLUETOOTH_BRIDGE_OHOS_H_
#define DEVICE_BLUETOOTH_OHOS_BLUETOOTH_BRIDGE_OHOS_H_

#include <string>

#include "base/functional/callback.h"
#include "base/values.h"

namespace device {

using BluetoothCommandCallbackOhos =
    base::RepeatingCallback<void(const std::string &command_json)>;
using BluetoothEventCallbackOhos =
    base::RepeatingCallback<void(const base::DictValue &event)>;
using BluetoothResponseCallbackOhos =
    base::OnceCallback<void(base::DictValue response)>;

// Installs the N-API dispatcher which forwards native commands to ArkTS.
void SetBluetoothCommandCallbackOhos(BluetoothCommandCallbackOhos callback);

// Delivers a response or unsolicited platform event from ArkTS. This entry
// point is thread-safe; delivery to the adapter happens on its UI sequence.
void DispatchBluetoothMessageOhos(const std::string &message_json);

// Registers the adapter-side event receiver and captures its UI sequence.
void SetBluetoothEventCallbackOhos(BluetoothEventCallbackOhos callback);

// Sends one command to ArkTS. The bridge adds a unique request id.
void SendBluetoothCommandOhos(base::DictValue command,
                              BluetoothResponseCallbackOhos callback);

} // namespace device

#endif // DEVICE_BLUETOOTH_OHOS_BLUETOOTH_BRIDGE_OHOS_H_
