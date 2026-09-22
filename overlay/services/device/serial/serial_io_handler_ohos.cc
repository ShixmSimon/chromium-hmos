// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/device/serial/serial_io_handler_ohos.h"

#include <algorithm>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/strings/string_number_conversions.h"
#include "services/device/usb/ohos/usb_bridge_ohos.h"

namespace device {
namespace {

int ToDataBits(mojom::SerialDataBits value) {
  return value == mojom::SerialDataBits::SEVEN ? 7 : 8;
}

int ToParity(mojom::SerialParityBit value) {
  switch (value) {
    case mojom::SerialParityBit::ODD:
      return 1;
    case mojom::SerialParityBit::EVEN:
      return 2;
    case mojom::SerialParityBit::NONE:
    case mojom::SerialParityBit::NO_PARITY:
      return 0;
  }
  return 0;
}

int ToStopBits(mojom::SerialStopBits value) {
  return value == mojom::SerialStopBits::TWO ? 1 : 0;
}

}  // namespace

// static
scoped_refptr<SerialIoHandler> SerialIoHandler::Create(
    const base::FilePath& port,
    scoped_refptr<base::SingleThreadTaskRunner> ui_thread_task_runner) {
  return new SerialIoHandlerOhos(port, std::move(ui_thread_task_runner));
}

SerialIoHandlerOhos::SerialIoHandlerOhos(
    const base::FilePath& port,
    scoped_refptr<base::SingleThreadTaskRunner> ui_thread_task_runner)
    : SerialIoHandler(port, std::move(ui_thread_task_runner)) {
  base::StringToInt(port.BaseName().AsUTF8Unsafe(), &port_id_);
}

SerialIoHandlerOhos::~SerialIoHandlerOhos() = default;

void SerialIoHandlerOhos::OpenImpl(OpenCompleteCallback callback) {
  if (port_id_ < 0 || open_ || options().cts_flow_control) {
    std::move(callback).Run(false);
    return;
  }
  base::DictValue command;
  command.Set("method", "serialOpen");
  command.Set("portId", port_id_);
  AddConfiguration(command);
  SendUsbCommandOhos(
      std::move(command),
      base::BindOnce(&SerialIoHandlerOhos::OnOpenComplete,
                     weak_factory_.GetWeakPtr(), std::move(callback)));
}

bool SerialIoHandlerOhos::IsOpen() const {
  return open_;
}

void SerialIoHandlerOhos::CloseImpl(base::OnceClosure callback) {
  open_ = false;
  ++read_generation_;
  ++write_generation_;
  base::DictValue command;
  command.Set("method", "serialClose");
  command.Set("portId", port_id_);
  SendUsbCommandOhos(
      std::move(command),
      base::BindOnce(&SerialIoHandlerOhos::OnCloseComplete,
                     weak_factory_.GetWeakPtr(), std::move(callback)));
}

void SerialIoHandlerOhos::ReadImpl() {
  const uint64_t generation = ++read_generation_;
  base::DictValue command;
  command.Set("method", "serialRead");
  command.Set("portId", port_id_);
  command.Set("size", static_cast<int>(std::min<size_t>(
                          pending_read_buffer().size(), 8192)));
  SendUsbCommandOhos(std::move(command),
                     base::BindOnce(&SerialIoHandlerOhos::OnReadComplete,
                                    weak_factory_.GetWeakPtr(), generation));
}

void SerialIoHandlerOhos::WriteImpl() {
  const uint64_t generation = ++write_generation_;
  base::ListValue data;
  const size_t size = std::min<size_t>(pending_write_buffer().size(), 4096);
  for (size_t index = 0; index < size; ++index) {
    data.Append(static_cast<int>(pending_write_buffer()[index]));
  }
  base::DictValue command;
  command.Set("method", "serialWrite");
  command.Set("portId", port_id_);
  command.Set("data", std::move(data));
  SendUsbCommandOhos(std::move(command),
                     base::BindOnce(&SerialIoHandlerOhos::OnWriteComplete,
                                    weak_factory_.GetWeakPtr(), generation));
}

void SerialIoHandlerOhos::CancelReadImpl() {
  ++read_generation_;
  ReadCompleted(0, read_cancel_reason());
}

void SerialIoHandlerOhos::CancelWriteImpl() {
  ++write_generation_;
  WriteCompleted(0, write_cancel_reason());
}

bool SerialIoHandlerOhos::ConfigurePortImpl() {
  if (options().cts_flow_control) {
    return false;
  }
  if (open_) {
    SendConfiguration();
  }
  return true;
}

void SerialIoHandlerOhos::Flush(mojom::SerialPortFlushMode mode) const {
  (void)mode;
}

void SerialIoHandlerOhos::Drain() {}

mojom::SerialPortControlSignalsPtr SerialIoHandlerOhos::GetControlSignals()
    const {
  auto signals = mojom::SerialPortControlSignals::New();
  signals->dcd = false;
  signals->cts = false;
  signals->ri = false;
  signals->dsr = false;
  return signals;
}

bool SerialIoHandlerOhos::SetControlSignals(
    const mojom::SerialHostControlSignals& control_signals) {
  return !control_signals.has_dtr && !control_signals.has_rts &&
         !control_signals.has_brk;
}

mojom::SerialConnectionInfoPtr SerialIoHandlerOhos::GetPortInfo() const {
  auto info = mojom::SerialConnectionInfo::New();
  info->bitrate = options().bitrate;
  info->data_bits = options().data_bits;
  info->parity_bit = options().parity_bit;
  info->stop_bits = options().stop_bits;
  info->cts_flow_control = false;
  return info;
}

void SerialIoHandlerOhos::OnOpenComplete(OpenCompleteCallback callback,
                                         base::DictValue response) {
  open_ = response.FindBool("ok").value_or(false);
  std::move(callback).Run(open_);
}

void SerialIoHandlerOhos::OnCloseComplete(base::OnceClosure callback,
                                          base::DictValue response) {
  (void)response;
  std::move(callback).Run();
}

void SerialIoHandlerOhos::OnReadComplete(uint64_t generation,
                                         base::DictValue response) {
  if (generation != read_generation_ || !IsReadPending()) {
    return;
  }
  if (!response.FindBool("ok").value_or(false)) {
    ReadCompleted(0, mojom::SerialReceiveError::SYSTEM_ERROR);
    return;
  }
  const base::ListValue* data = response.FindList("data");
  size_t count = 0;
  if (data) {
    count = std::min(data->size(), pending_read_buffer().size());
    for (size_t index = 0; index < count; ++index) {
      if ((*data)[index].is_int()) {
        pending_read_buffer()[index] =
            static_cast<uint8_t>(std::clamp((*data)[index].GetInt(), 0, 255));
      }
    }
  }
  ReadCompleted(static_cast<int>(count), mojom::SerialReceiveError::NONE);
}

void SerialIoHandlerOhos::OnWriteComplete(uint64_t generation,
                                          base::DictValue response) {
  if (generation != write_generation_ || !IsWritePending()) {
    return;
  }
  if (!response.FindBool("ok").value_or(false)) {
    WriteCompleted(0, mojom::SerialSendError::SYSTEM_ERROR);
    return;
  }
  const int transferred = response.FindInt("bytesTransferred").value_or(0);
  WriteCompleted(static_cast<int>(std::min<size_t>(
                     std::max(transferred, 0), pending_write_buffer().size())),
                 mojom::SerialSendError::NONE);
}

void SerialIoHandlerOhos::SendConfiguration() {
  base::DictValue command;
  command.Set("method", "serialConfigure");
  command.Set("portId", port_id_);
  AddConfiguration(command);
  SendUsbCommandOhos(
      std::move(command),
      base::BindOnce([](base::DictValue response) { (void)response; }));
}

void SerialIoHandlerOhos::AddConfiguration(base::DictValue& command) const {
  command.Set("baudRate", static_cast<int>(options().bitrate));
  command.Set("dataBits", ToDataBits(options().data_bits));
  command.Set("parity", ToParity(options().parity_bit));
  command.Set("stopBits", ToStopBits(options().stop_bits));
}

}  // namespace device
