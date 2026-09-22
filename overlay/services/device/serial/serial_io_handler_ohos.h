// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SERVICES_DEVICE_SERIAL_SERIAL_IO_HANDLER_OHOS_H_
#define SERVICES_DEVICE_SERIAL_SERIAL_IO_HANDLER_OHOS_H_

#include <cstdint>

#include "base/memory/weak_ptr.h"
#include "services/device/serial/serial_io_handler.h"

namespace base {
class DictValue;
}

namespace device {

class SerialIoHandlerOhos final : public SerialIoHandler {
 public:
  SerialIoHandlerOhos(const SerialIoHandlerOhos&) = delete;
  SerialIoHandlerOhos& operator=(const SerialIoHandlerOhos&) = delete;

  void Flush(mojom::SerialPortFlushMode mode) const override;
  void Drain() override;
  mojom::SerialPortControlSignalsPtr GetControlSignals() const override;
  bool SetControlSignals(
      const mojom::SerialHostControlSignals& control_signals) override;
  mojom::SerialConnectionInfoPtr GetPortInfo() const override;

 protected:
  ~SerialIoHandlerOhos() override;
  void OpenImpl() override;
  bool IsOpen() const override;
  void CloseImpl(base::OnceClosure callback) override;
  void ReadImpl() override;
  void WriteImpl() override;
  void CancelReadImpl() override;
  void CancelWriteImpl() override;
  bool ConfigurePortImpl() override;

 private:
  friend class SerialIoHandler;

  SerialIoHandlerOhos(
      const base::FilePath& port,
      scoped_refptr<base::SingleThreadTaskRunner> ui_thread_task_runner);

  void OnOpenComplete(base::DictValue response);
  void OnCloseComplete(base::OnceClosure callback, base::DictValue response);
  void OnReadComplete(uint64_t generation, base::DictValue response);
  void OnWriteComplete(uint64_t generation, base::DictValue response);
  void SendConfiguration();
  void AddConfiguration(base::DictValue& command) const;

  int port_id_ = -1;
  bool open_ = false;
  uint64_t read_generation_ = 0;
  uint64_t write_generation_ = 0;
  base::WeakPtrFactory<SerialIoHandlerOhos> weak_factory_{this};
};

}  // namespace device

#endif  // SERVICES_DEVICE_SERIAL_SERIAL_IO_HANDLER_OHOS_H_
