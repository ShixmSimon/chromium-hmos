// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SERVICES_DEVICE_USB_OHOS_USB_BRIDGE_OHOS_H_
#define SERVICES_DEVICE_USB_OHOS_USB_BRIDGE_OHOS_H_

#include <string>

#include "base/functional/callback.h"
#include "base/values.h"

namespace device {

using UsbCommandCallbackOhos =
    base::RepeatingCallback<void(const std::string& command_json)>;
using UsbEventCallbackOhos =
    base::RepeatingCallback<void(const base::DictValue& event)>;
using UsbResponseCallbackOhos =
    base::OnceCallback<void(base::DictValue response)>;

void SetUsbCommandCallbackOhos(UsbCommandCallbackOhos callback);
void DispatchUsbMessageOhos(const std::string& message_json);
void SetUsbEventCallbackOhos(UsbEventCallbackOhos callback);
void SetHidEventCallbackOhos(UsbEventCallbackOhos callback);
void SetSerialEventCallbackOhos(UsbEventCallbackOhos callback);
void SendUsbCommandOhos(base::DictValue command,
                        UsbResponseCallbackOhos callback);

}  // namespace device

#endif  // SERVICES_DEVICE_USB_OHOS_USB_BRIDGE_OHOS_H_
