// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/device/battery/battery_status_manager.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <utility>

#include <BasicServicesKit/oh_commonevent.h>
#include <BasicServicesKit/oh_commonevent_support.h>
#include <BasicServicesKit/ohbattery_info.h>

#include "base/logging.h"
#include "base/memory/ref_counted.h"
#include "base/no_destructor.h"
#include "base/synchronization/lock.h"

namespace device {

namespace {

class BatteryCallbackState
    : public base::RefCountedThreadSafe<BatteryCallbackState> {
 public:
  explicit BatteryCallbackState(
      BatteryStatusService::BatteryUpdateCallback callback)
      : callback_(std::move(callback)) {}

  void Notify(const mojom::BatteryStatus& status) {
    base::AutoLock lock(lock_);
    if (active_) {
      callback_.Run(status);
    }
  }

  void Deactivate() {
    base::AutoLock lock(lock_);
    active_ = false;
  }

 private:
  friend class base::RefCountedThreadSafe<BatteryCallbackState>;
  ~BatteryCallbackState() = default;

  base::Lock lock_;
  BatteryStatusService::BatteryUpdateCallback callback_;
  bool active_ GUARDED_BY(lock_) = true;
};

struct BatteryCallbackRegistry {
  base::Lock lock;
  scoped_refptr<BatteryCallbackState> state GUARDED_BY(lock);
};

BatteryCallbackRegistry& GetCallbackRegistry() {
  static base::NoDestructor<BatteryCallbackRegistry> registry;
  return *registry;
}

mojom::BatteryStatus MakeBatteryStatus(
    int capacity,
    BatteryInfo_BatteryPluggedType plugged_type) {
  mojom::BatteryStatus status;
  capacity = std::clamp(capacity, 0, 100);
  status.level = static_cast<double>(capacity) / 100.0;
  status.charging =
      plugged_type != PLUGGED_TYPE_NONE && plugged_type != PLUGGED_TYPE_BUTT;
  status.charging_time = status.charging && capacity == 100
                             ? 0.0
                             : std::numeric_limits<double>::infinity();
  status.discharging_time = std::numeric_limits<double>::infinity();
  return status;
}

mojom::BatteryStatus ReadBatteryStatus(
    const CommonEvent_RcvData* event_data = nullptr) {
  int capacity = OH_BatteryInfo_GetCapacity();
  int plugged_type = OH_BatteryInfo_GetPluggedType();

  if (event_data) {
    const CommonEvent_Parameters* parameters =
        OH_CommonEvent_GetParametersFromRcvData(event_data);
    if (parameters) {
      if (OH_CommonEvent_HasKeyInParameters(parameters,
                                            COMMON_EVENT_KEY_CAPACITY)) {
        capacity = OH_CommonEvent_GetIntFromParameters(
            parameters, COMMON_EVENT_KEY_CAPACITY, capacity);
      }
      if (OH_CommonEvent_HasKeyInParameters(parameters,
                                            COMMON_EVENT_KEY_PLUGGED_TYPE)) {
        plugged_type = OH_CommonEvent_GetIntFromParameters(
            parameters, COMMON_EVENT_KEY_PLUGGED_TYPE, plugged_type);
      }
    }
  }

  if (plugged_type < PLUGGED_TYPE_NONE || plugged_type > PLUGGED_TYPE_BUTT) {
    plugged_type = PLUGGED_TYPE_BUTT;
  }
  return MakeBatteryStatus(
      capacity, static_cast<BatteryInfo_BatteryPluggedType>(plugged_type));
}

void OnBatteryEvent(const CommonEvent_RcvData* event_data) {
  if (!event_data) {
    return;
  }

  const char* event_name = OH_CommonEvent_GetEventFromRcvData(event_data);
  if (!event_name ||
      std::strcmp(event_name, COMMON_EVENT_BATTERY_CHANGED) != 0) {
    return;
  }

  scoped_refptr<BatteryCallbackState> state;
  BatteryCallbackRegistry& registry = GetCallbackRegistry();
  {
    base::AutoLock lock(registry.lock);
    state = registry.state;
  }
  if (state) {
    state->Notify(ReadBatteryStatus(event_data));
  }
}

class BatteryStatusManagerOhos final : public BatteryStatusManager {
 public:
  explicit BatteryStatusManagerOhos(
      const BatteryStatusService::BatteryUpdateCallback& callback)
      : callback_(callback) {}

  BatteryStatusManagerOhos(const BatteryStatusManagerOhos&) = delete;
  BatteryStatusManagerOhos& operator=(const BatteryStatusManagerOhos&) = delete;

  ~BatteryStatusManagerOhos() override { StopListeningBatteryChange(); }

 private:
  bool StartListeningBatteryChange() override {
    if (listening_) {
      return true;
    }

    const char* events[] = {COMMON_EVENT_BATTERY_CHANGED};
    CommonEvent_SubscribeInfo* subscribe_info =
        OH_CommonEvent_CreateSubscribeInfo(events, std::size(events));
    if (!subscribe_info) {
      LOG(ERROR) << "OHOS battery failed to create subscribe info";
      return false;
    }

    CommonEvent_Subscriber* subscriber =
        OH_CommonEvent_CreateSubscriber(subscribe_info, OnBatteryEvent);
    if (!subscriber) {
      LOG(ERROR) << "OHOS battery failed to create subscriber";
      OH_CommonEvent_DestroySubscribeInfo(subscribe_info);
      return false;
    }

    callback_state_ = base::MakeRefCounted<BatteryCallbackState>(callback_);
    BatteryCallbackRegistry& registry = GetCallbackRegistry();
    {
      base::AutoLock lock(registry.lock);
      if (registry.state) {
        LOG(ERROR) << "OHOS battery callback registry is already occupied";
        callback_state_->Deactivate();
        callback_state_.reset();
        OH_CommonEvent_DestroySubscriber(subscriber);
        OH_CommonEvent_DestroySubscribeInfo(subscribe_info);
        return false;
      }
      registry.state = callback_state_;
    }

    const CommonEvent_ErrCode result = OH_CommonEvent_Subscribe(subscriber);
    if (result != COMMONEVENT_ERR_OK) {
      LOG(ERROR) << "OHOS battery subscription failed: " << result;
      ClearCallbackState();
      callback_state_->Deactivate();
      callback_state_.reset();
      OH_CommonEvent_DestroySubscriber(subscriber);
      OH_CommonEvent_DestroySubscribeInfo(subscribe_info);
      return false;
    }

    subscribe_info_ = subscribe_info;
    subscriber_ = subscriber;
    listening_ = true;
    callback_state_->Notify(ReadBatteryStatus());
    return true;
  }

  void StopListeningBatteryChange() override {
    if (!listening_) {
      return;
    }

    ClearCallbackState();
    callback_state_->Deactivate();

    const CommonEvent_ErrCode result = OH_CommonEvent_UnSubscribe(subscriber_);
    if (result != COMMONEVENT_ERR_OK) {
      LOG(ERROR) << "OHOS battery unsubscribe failed: " << result;
    }
    OH_CommonEvent_DestroySubscriber(subscriber_);
    OH_CommonEvent_DestroySubscribeInfo(subscribe_info_);

    subscriber_ = nullptr;
    subscribe_info_ = nullptr;
    callback_state_.reset();
    listening_ = false;
  }

  void ClearCallbackState() {
    BatteryCallbackRegistry& registry = GetCallbackRegistry();
    base::AutoLock lock(registry.lock);
    if (registry.state == callback_state_) {
      registry.state.reset();
    }
  }

  BatteryStatusService::BatteryUpdateCallback callback_;
  scoped_refptr<BatteryCallbackState> callback_state_;
  CommonEvent_SubscribeInfo* subscribe_info_ = nullptr;
  CommonEvent_Subscriber* subscriber_ = nullptr;
  bool listening_ = false;
};

}  // namespace

// static
std::unique_ptr<BatteryStatusManager> BatteryStatusManager::Create(
    const BatteryStatusService::BatteryUpdateCallback& callback) {
  return std::make_unique<BatteryStatusManagerOhos>(callback);
}

}  // namespace device
