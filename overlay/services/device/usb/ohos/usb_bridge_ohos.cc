// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/device/usb/ohos/usb_bridge_ohos.h"

#include <map>
#include <optional>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/location.h"
#include "base/no_destructor.h"
#include "base/synchronization/lock.h"
#include "base/task/single_thread_task_runner.h"

namespace device {
namespace {

class UsbBridgeState {
 public:
  void SetCommandCallback(UsbCommandCallbackOhos callback) {
    base::AutoLock lock(lock_);
    command_callback_ = std::move(callback);
  }

  void SetEventCallback(UsbEventCallbackOhos callback) {
    CHECK(base::SingleThreadTaskRunner::HasCurrentDefault());
    {
      base::AutoLock lock(lock_);
      ui_task_runner_ = base::SingleThreadTaskRunner::GetCurrentDefault();
    }
    event_callback_ = std::move(callback);
  }

  void SetHidEventCallback(UsbEventCallbackOhos callback) {
    CHECK(base::SingleThreadTaskRunner::HasCurrentDefault());
    {
      base::AutoLock lock(lock_);
      ui_task_runner_ = base::SingleThreadTaskRunner::GetCurrentDefault();
    }
    hid_event_callback_ = std::move(callback);
  }

  void SendCommand(base::DictValue command, UsbResponseCallbackOhos callback) {
    const int request_id = ++next_request_id_;
    command.Set("id", request_id);
    pending_responses_.emplace(request_id, std::move(callback));

    std::string command_json;
    if (!base::JSONWriter::Write(command, &command_json)) {
      FailRequest(request_id, "Could not encode USB command");
      return;
    }

    UsbCommandCallbackOhos dispatcher;
    {
      base::AutoLock lock(lock_);
      dispatcher = command_callback_;
    }
    if (!dispatcher) {
      FailRequest(request_id, "HarmonyOS USB bridge is unavailable");
      return;
    }
    dispatcher.Run(command_json);
  }

  void DispatchMessage(const std::string& message_json) {
    std::optional<base::Value> value =
        base::JSONReader::Read(message_json, base::JSON_PARSE_RFC);
    if (!value || !value->is_dict()) {
      return;
    }

    scoped_refptr<base::SingleThreadTaskRunner> runner;
    {
      base::AutoLock lock(lock_);
      runner = ui_task_runner_;
    }
    if (!runner) {
      return;
    }
    runner->PostTask(
        FROM_HERE,
        base::BindOnce(&UsbBridgeState::DispatchMessageOnUi,
                       base::Unretained(this), std::move(value->GetDict())));
  }

 private:
  void FailRequest(int request_id, const std::string& message) {
    base::DictValue response;
    response.Set("id", request_id);
    response.Set("ok", false);
    response.Set("error", message);
    DispatchMessageOnUi(std::move(response));
  }

  void DispatchMessageOnUi(base::DictValue message) {
    if (const std::optional<int> request_id = message.FindInt("id");
        request_id && *request_id > 0) {
      auto it = pending_responses_.find(*request_id);
      if (it == pending_responses_.end()) {
        return;
      }
      UsbResponseCallbackOhos callback = std::move(it->second);
      pending_responses_.erase(it);
      std::move(callback).Run(std::move(message));
      return;
    }
    const std::string* event_type = message.FindString("event");
    if (event_type && event_type->starts_with("hid")) {
      if (hid_event_callback_) {
        hid_event_callback_.Run(message);
      }
      return;
    }
    if (event_callback_) {
      event_callback_.Run(message);
    }
    if (hid_event_callback_) {
      hid_event_callback_.Run(message);
    }
  }

  base::Lock lock_;
  UsbCommandCallbackOhos command_callback_ GUARDED_BY(lock_);
  scoped_refptr<base::SingleThreadTaskRunner> ui_task_runner_ GUARDED_BY(lock_);
  UsbEventCallbackOhos event_callback_;
  UsbEventCallbackOhos hid_event_callback_;
  int next_request_id_ = 0;
  std::map<int, UsbResponseCallbackOhos> pending_responses_;
};

UsbBridgeState& GetBridge() {
  static base::NoDestructor<UsbBridgeState> bridge;
  return *bridge;
}

}  // namespace

void SetUsbCommandCallbackOhos(UsbCommandCallbackOhos callback) {
  GetBridge().SetCommandCallback(std::move(callback));
}

void DispatchUsbMessageOhos(const std::string& message_json) {
  GetBridge().DispatchMessage(message_json);
}

void SetUsbEventCallbackOhos(UsbEventCallbackOhos callback) {
  GetBridge().SetEventCallback(std::move(callback));
}

void SetHidEventCallbackOhos(UsbEventCallbackOhos callback) {
  GetBridge().SetHidEventCallback(std::move(callback));
}

void SendUsbCommandOhos(base::DictValue command,
                        UsbResponseCallbackOhos callback) {
  GetBridge().SendCommand(std::move(command), std::move(callback));
}

}  // namespace device
