// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/device/hid/hid_connection_ohos.h"

#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
#include "base/memory/ref_counted_memory.h"
#include "services/device/usb/ohos/usb_bridge_ohos.h"

namespace device {
namespace {

std::vector<uint8_t> ReadBytes(const base::DictValue& value) {
  std::vector<uint8_t> bytes;
  const base::ListValue* source = value.FindList("data");
  if (!source) {
    return bytes;
  }
  bytes.reserve(source->size());
  for (const base::Value& item : *source) {
    if (item.is_int()) {
      bytes.push_back(static_cast<uint8_t>(std::clamp(item.GetInt(), 0, 255)));
    }
  }
  return bytes;
}

}  // namespace

HidConnectionOhos::HidConnectionOhos(
    scoped_refptr<HidDeviceInfo> device_info,
    int connection_id,
    bool allow_protected_reports,
    bool allow_fido_reports,
    base::OnceCallback<void(int)> on_closed)
    : HidConnection(std::move(device_info),
                    allow_protected_reports,
                    allow_fido_reports),
      connection_id_(connection_id),
      on_closed_(std::move(on_closed)) {}

HidConnectionOhos::~HidConnectionOhos() = default;

void HidConnectionOhos::OnInputReport(const base::DictValue& event) {
  if (closed()) {
    return;
  }
  std::vector<uint8_t> bytes = ReadBytes(event);
  if (bytes.empty()) {
    return;
  }
  if (!device_info()->has_report_id()) {
    bytes.insert(bytes.begin(), 0);
  }
  auto buffer = base::MakeRefCounted<base::RefCountedBytes>(std::move(bytes));
  ProcessInputReport(buffer, buffer->size());
}

base::WeakPtr<HidConnectionOhos> HidConnectionOhos::GetWeakPtr() {
  return weak_factory_.GetWeakPtr();
}

void HidConnectionOhos::PlatformClose() {
  base::DictValue command;
  command.Set("method", "hidClose");
  command.Set("connectionId", connection_id_);
  SendUsbCommandOhos(
      std::move(command),
      base::BindOnce([](base::DictValue response) { (void)response; }));
  if (on_closed_) {
    std::move(on_closed_).Run(connection_id_);
  }
  weak_factory_.InvalidateWeakPtrs();
}

void HidConnectionOhos::PlatformWrite(
    scoped_refptr<base::RefCountedBytes> buffer,
    WriteCallback callback) {
  base::DictValue command;
  command.Set("method", "hidWrite");
  command.Set("connectionId", connection_id_);
  command.Set("reportId", static_cast<int>(buffer->data()[0]));
  command.Set("data", MakeReportData(*buffer));
  SendUsbCommandOhos(
      std::move(command),
      base::BindOnce(&HidConnectionOhos::OnWriteComplete, std::move(callback)));
}

void HidConnectionOhos::PlatformGetFeatureReport(uint8_t report_id,
                                                 ReadCallback callback) {
  base::DictValue command;
  command.Set("method", "hidGetFeature");
  command.Set("connectionId", connection_id_);
  command.Set("reportId", static_cast<int>(report_id));
  command.Set("size",
              static_cast<int>(device_info()->max_feature_report_size() + 1));
  SendUsbCommandOhos(
      std::move(command),
      base::BindOnce(&HidConnectionOhos::OnFeatureReportComplete,
                     device_info()->has_report_id(), std::move(callback)));
}

void HidConnectionOhos::PlatformSendFeatureReport(
    scoped_refptr<base::RefCountedBytes> buffer,
    WriteCallback callback) {
  base::DictValue command;
  command.Set("method", "hidSendFeature");
  command.Set("connectionId", connection_id_);
  command.Set("reportId", static_cast<int>(buffer->data()[0]));
  command.Set("data", MakeReportData(*buffer));
  SendUsbCommandOhos(
      std::move(command),
      base::BindOnce(&HidConnectionOhos::OnWriteComplete, std::move(callback)));
}

// static
void HidConnectionOhos::OnWriteComplete(WriteCallback callback,
                                        base::DictValue response) {
  std::move(callback).Run(response.FindBool("ok").value_or(false));
}

// static
void HidConnectionOhos::OnFeatureReportComplete(bool has_report_id,
                                                ReadCallback callback,
                                                base::DictValue response) {
  if (!response.FindBool("ok").value_or(false)) {
    std::move(callback).Run(false, nullptr, 0);
    return;
  }
  std::vector<uint8_t> bytes = ReadBytes(response);
  if (!has_report_id) {
    bytes.insert(bytes.begin(), 0);
  }
  if (bytes.empty()) {
    std::move(callback).Run(false, nullptr, 0);
    return;
  }
  auto buffer = base::MakeRefCounted<base::RefCountedBytes>(std::move(bytes));
  const size_t size = buffer->size();
  std::move(callback).Run(true, std::move(buffer), size);
}

base::ListValue HidConnectionOhos::MakeReportData(
    const base::RefCountedBytes& buffer) const {
  base::ListValue data;
  const size_t first_byte = buffer.data()[0] == 0 ? 1 : 0;
  for (size_t index = first_byte; index < buffer.size(); ++index) {
    data.Append(static_cast<int>(buffer.data()[index]));
  }
  return data;
}

}  // namespace device
