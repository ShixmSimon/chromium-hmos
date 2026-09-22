// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SERVICES_DEVICE_USB_OHOS_USB_DEVICE_HANDLE_OHOS_H_
#define SERVICES_DEVICE_USB_OHOS_USB_DEVICE_HANDLE_OHOS_H_

#include "base/memory/scoped_refptr.h"
#include "services/device/usb/usb_device_handle_usbfs.h"

namespace device {

class UsbDevice;

class UsbDeviceHandleOhos : public UsbDeviceHandleUsbfs {
 public:
  static scoped_refptr<UsbDeviceHandleOhos>
  Create(scoped_refptr<UsbDevice> device, int file_descriptor, int pipe_id);

 private:
  UsbDeviceHandleOhos(scoped_refptr<UsbDevice> device,
                      base::ScopedFD file_descriptor,
                      int pipe_id);
  ~UsbDeviceHandleOhos() override;

  void FinishClose() override;
  void ClosePipe();

  const int pipe_id_;
};

}  // namespace device

#endif  // SERVICES_DEVICE_USB_OHOS_USB_DEVICE_HANDLE_OHOS_H_
