// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "device/bluetooth/ohos/bluetooth_bridge_ohos.h"

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

class BluetoothBridgeState {
public:
  void SetCommandCallback(BluetoothCommandCallbackOhos callback) {
    base::AutoLock lock(lock_);
    command_callback_ = std::move(callback);
  }

  void SetEventCallback(BluetoothEventCallbackOhos callback) {
    CHECK(base::SingleThreadTaskRunner::HasCurrentDefault());
    {
      base::AutoLock lock(lock_);
      ui_task_runner_ = base::SingleThreadTaskRunner::GetCurrentDefault();
    }
    event_callback_ = std::move(callback);
  }

  void SendCommand(base::DictValue command,
                   BluetoothResponseCallbackOhos callback) {
    {
      base::AutoLock lock(lock_);
      CHECK(ui_task_runner_);
    }
    const int request_id = ++next_request_id_;
    command.Set("id", request_id);
    pending_responses_.emplace(request_id, std::move(callback));

    std::string command_json;
    if (!base::JSONWriter::Write(command, &command_json)) {
      FailRequest(request_id, "Could not encode Bluetooth command");
      return;
    }

    BluetoothCommandCallbackOhos dispatcher;
    {
      base::AutoLock lock(lock_);
      dispatcher = command_callback_;
    }
    if (!dispatcher) {
      FailRequest(request_id, "HarmonyOS Bluetooth bridge is unavailable");
      return;
    }
    dispatcher.Run(command_json);
  }

  void DispatchMessage(const std::string &message_json) {
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
    runner->PostTask(FROM_HERE,
                     base::BindOnce(&BluetoothBridgeState::DispatchMessageOnUi,
                                    base::Unretained(this),
                                    std::move(value->GetDict())));
  }

private:
  void FailRequest(int request_id, const std::string &message) {
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
      BluetoothResponseCallbackOhos callback = std::move(it->second);
      pending_responses_.erase(it);
      std::move(callback).Run(std::move(message));
      return;
    }

    if (event_callback_) {
      event_callback_.Run(message);
    }
  }

  base::Lock lock_;
  BluetoothCommandCallbackOhos command_callback_ GUARDED_BY(lock_);

  scoped_refptr<base::SingleThreadTaskRunner> ui_task_runner_ GUARDED_BY(lock_);
  BluetoothEventCallbackOhos event_callback_;
  int next_request_id_ = 0;
  std::map<int, BluetoothResponseCallbackOhos> pending_responses_;
};

BluetoothBridgeState &GetBridge() {
  static base::NoDestructor<BluetoothBridgeState> bridge;
  return *bridge;
}

} // namespace

void SetBluetoothCommandCallbackOhos(BluetoothCommandCallbackOhos callback) {
  GetBridge().SetCommandCallback(std::move(callback));
}

void DispatchBluetoothMessageOhos(const std::string &message_json) {
  GetBridge().DispatchMessage(message_json);
}

void SetBluetoothEventCallbackOhos(BluetoothEventCallbackOhos callback) {
  GetBridge().SetEventCallback(std::move(callback));
}

void SendBluetoothCommandOhos(base::DictValue command,
                              BluetoothResponseCallbackOhos callback) {
  GetBridge().SendCommand(std::move(command), std::move(callback));
}

} // namespace device
