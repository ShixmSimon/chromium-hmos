// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SERVICES_DEVICE_SERIAL_SERIAL_DEVICE_ENUMERATOR_OHOS_H_
#define SERVICES_DEVICE_SERIAL_SERIAL_DEVICE_ENUMERATOR_OHOS_H_

#include <map>

#include "base/memory/weak_ptr.h"
#include "services/device/serial/serial_device_enumerator.h"

namespace base {
class DictValue;
}

namespace device {

class SerialDeviceEnumeratorOhos : public SerialDeviceEnumerator {
 public:
  SerialDeviceEnumeratorOhos();
  ~SerialDeviceEnumeratorOhos() override;

  SerialDeviceEnumeratorOhos(const SerialDeviceEnumeratorOhos&) = delete;
  SerialDeviceEnumeratorOhos& operator=(const SerialDeviceEnumeratorOhos&) =
      delete;

 private:
  void RefreshPorts();
  void OnRefreshComplete(base::DictValue response);
  void OnPlatformEvent(const base::DictValue& event);

  bool refresh_in_flight_ = false;
  bool refresh_again_ = false;
  std::map<int, base::UnguessableToken> tokens_by_port_id_;
  base::WeakPtrFactory<SerialDeviceEnumeratorOhos> weak_factory_{this};
};

}  // namespace device

#endif  // SERVICES_DEVICE_SERIAL_SERIAL_DEVICE_ENUMERATOR_OHOS_H_
