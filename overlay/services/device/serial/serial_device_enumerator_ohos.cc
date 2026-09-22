// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/device/serial/serial_device_enumerator_ohos.h"

#include <set>
#include <string>
#include <utility>

#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/strings/string_number_conversions.h"
#include "components/device_event_log/device_event_log.h"
#include "services/device/usb/ohos/usb_bridge_ohos.h"

namespace device {

SerialDeviceEnumeratorOhos::SerialDeviceEnumeratorOhos() {
  SetSerialEventCallbackOhos(
      base::BindRepeating(&SerialDeviceEnumeratorOhos::OnPlatformEvent,
                          weak_factory_.GetWeakPtr()));
  RefreshPorts();
}

SerialDeviceEnumeratorOhos::~SerialDeviceEnumeratorOhos() {
  SetSerialEventCallbackOhos(UsbEventCallbackOhos());
}

void SerialDeviceEnumeratorOhos::RefreshPorts() {
  if (refresh_in_flight_) {
    refresh_again_ = true;
    return;
  }
  refresh_in_flight_ = true;
  base::DictValue command;
  command.Set("method", "serialEnumerate");
  SendUsbCommandOhos(
      std::move(command),
      base::BindOnce(&SerialDeviceEnumeratorOhos::OnRefreshComplete,
                     weak_factory_.GetWeakPtr()));
}

void SerialDeviceEnumeratorOhos::OnRefreshComplete(base::DictValue response) {
  refresh_in_flight_ = false;
  if (response.FindBool("ok").value_or(false)) {
    std::set<int> seen;
    if (const base::ListValue* ports = response.FindList("ports")) {
      for (const base::Value& value : *ports) {
        const base::DictValue* source = value.GetIfDict();
        if (!source) {
          continue;
        }
        const int port_id = source->FindInt("portId").value_or(-1);
        const std::string* device_name = source->FindString("deviceName");
        if (port_id < 0 || !device_name) {
          continue;
        }
        seen.insert(port_id);
        if (tokens_by_port_id_.contains(port_id)) {
          continue;
        }

        auto info = mojom::SerialPortInfo::New();
        info->token = base::UnguessableToken::Create();
        info->path = base::FilePath::FromASCII("/ohos/serial/" +
                                               base::NumberToString(port_id));
        info->type = mojom::SerialPortType::PLATFORM_SERIAL;
        info->display_name = *device_name;
        info->connected = true;
        tokens_by_port_id_.emplace(port_id, info->token);
        AddPort(std::move(info));
      }
    }

    for (auto it = tokens_by_port_id_.begin();
         it != tokens_by_port_id_.end();) {
      if (seen.contains(it->first)) {
        ++it;
        continue;
      }
      const base::UnguessableToken token = it->second;
      it = tokens_by_port_id_.erase(it);
      RemovePort(token);
    }
  } else {
    const std::string* error = response.FindString("error");
    SERIAL_LOG(ERROR) << "Serial enumeration failed: "
                      << (error ? *error : "unknown error");
  }

  if (refresh_again_) {
    refresh_again_ = false;
    RefreshPorts();
  }
}

void SerialDeviceEnumeratorOhos::OnPlatformEvent(const base::DictValue& event) {
  const std::string* type = event.FindString("event");
  if (type && *type == "devicesChanged") {
    RefreshPorts();
  }
}

}  // namespace device
