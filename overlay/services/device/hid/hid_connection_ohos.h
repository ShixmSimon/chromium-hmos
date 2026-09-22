// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SERVICES_DEVICE_HID_HID_CONNECTION_OHOS_H_
#define SERVICES_DEVICE_HID_HID_CONNECTION_OHOS_H_

#include <stdint.h>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "services/device/hid/hid_connection.h"

namespace device {

class HidConnectionOhos : public HidConnection {
 public:
  HidConnectionOhos(scoped_refptr<HidDeviceInfo> device_info,
                    int connection_id,
                    bool allow_protected_reports,
                    bool allow_fido_reports,
                    base::OnceCallback<void(int)> on_closed);

  void OnInputReport(const base::DictValue& event);
  base::WeakPtr<HidConnectionOhos> GetWeakPtr();

 private:
  friend class base::RefCountedThreadSafe<HidConnectionOhos>;

  ~HidConnectionOhos() override;

  void PlatformClose() override;
  void PlatformWrite(scoped_refptr<base::RefCountedBytes> buffer,
                     WriteCallback callback) override;
  void PlatformGetFeatureReport(uint8_t report_id,
                                ReadCallback callback) override;
  void PlatformSendFeatureReport(
      scoped_refptr<base::RefCountedBytes> buffer,
      WriteCallback callback) override;

  static void OnWriteComplete(WriteCallback callback,
                              base::DictValue response);
  static void OnFeatureReportComplete(bool has_report_id,
                                      ReadCallback callback,
                                      base::DictValue response);
  base::ListValue MakeReportData(const base::RefCountedBytes& buffer) const;

  const int connection_id_;
  base::OnceCallback<void(int)> on_closed_;

  base::WeakPtrFactory<HidConnectionOhos> weak_factory_{this};
};

}  // namespace device

#endif  // SERVICES_DEVICE_HID_HID_CONNECTION_OHOS_H_
