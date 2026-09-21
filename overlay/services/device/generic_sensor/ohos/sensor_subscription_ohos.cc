// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/device/generic_sensor/ohos/sensor_subscription_ohos.h"

#include <sensors/oh_sensor.h>

#include <map>
#include <utility>

#include "base/logging.h"
#include "base/memory/ref_counted.h"
#include "base/no_destructor.h"
#include "base/synchronization/lock.h"
#include "base/thread_annotations.h"

namespace device::ohos {

class SensorEventRelay : public base::RefCountedThreadSafe<SensorEventRelay> {
 public:
  explicit SensorEventRelay(SensorSubscription::EventCallback callback)
      : callback_(std::move(callback)) {}

  void Deliver(int64_t timestamp_ns,
               double value1,
               double value2,
               double value3,
               double value4) {
    callback_.Run(timestamp_ns, value1, value2, value3, value4);
  }

 private:
  friend class base::RefCountedThreadSafe<SensorEventRelay>;
  ~SensorEventRelay() = default;

  const SensorSubscription::EventCallback callback_;
};

namespace {

struct SensorRegistry {
  base::Lock lock;
  std::map<Sensor_Type, scoped_refptr<SensorEventRelay>> relays
      GUARDED_BY(lock);
};

SensorRegistry& Registry() {
  static base::NoDestructor<SensorRegistry> registry;
  return *registry;
}

bool RegisterRelay(Sensor_Type type,
                   const scoped_refptr<SensorEventRelay>& relay) {
  SensorRegistry& registry = Registry();
  base::AutoLock auto_lock(registry.lock);
  return registry.relays.emplace(type, relay).second;
}

void UnregisterRelay(Sensor_Type type, const SensorEventRelay* relay) {
  SensorRegistry& registry = Registry();
  base::AutoLock auto_lock(registry.lock);
  auto it = registry.relays.find(type);
  if (it != registry.relays.end() && it->second.get() == relay) {
    registry.relays.erase(it);
  }
}

scoped_refptr<SensorEventRelay> FindRelay(Sensor_Type type) {
  SensorRegistry& registry = Registry();
  base::AutoLock auto_lock(registry.lock);
  auto it = registry.relays.find(type);
  return it == registry.relays.end() ? nullptr : it->second;
}

void OnSensorEvent(Sensor_Event* event) {
  if (!event) {
    LOG(ERROR) << "SensorServiceKit delivered a null event";
    return;
  }

  Sensor_Type type;
  const int32_t type_result = OH_SensorEvent_GetType(event, &type);
  if (type_result != SENSOR_SUCCESS) {
    LOG(ERROR) << "OH_SensorEvent_GetType failed: " << type_result;
    return;
  }

  scoped_refptr<SensorEventRelay> relay = FindRelay(type);
  if (!relay) {
    LOG(WARNING) << "Sensor event arrived without a subscriber for type "
                 << type;
    return;
  }

  float* data = nullptr;
  uint32_t length = 0;
  const int32_t data_result = OH_SensorEvent_GetData(event, &data, &length);
  if (data_result != SENSOR_SUCCESS || !data || (length != 3 && length != 4)) {
    LOG(ERROR) << "OH_SensorEvent_GetData failed for type " << type
               << ": result=" << data_result << ", length=" << length;
    return;
  }

  int64_t timestamp_ns = 0;
  const int32_t timestamp_result =
      OH_SensorEvent_GetTimestamp(event, &timestamp_ns);
  if (timestamp_result != SENSOR_SUCCESS) {
    LOG(ERROR) << "OH_SensorEvent_GetTimestamp failed: " << timestamp_result;
    return;
  }

  relay->Deliver(timestamp_ns, data[0], data[1], data[2],
                 length == 4 ? data[3] : 0.0);
}

void LogDestroyError(const char* function, int32_t result) {
  if (result != SENSOR_SUCCESS) {
    LOG(ERROR) << function << " failed: " << result;
  }
}

}  // namespace

std::unique_ptr<SensorSubscription> SensorSubscription::Create(
    Sensor_Type type,
    int64_t sampling_interval_ns,
    EventCallback callback) {
  if (sampling_interval_ns <= 0 || callback.is_null()) {
    LOG(ERROR) << "Invalid OHOS sensor subscription parameters";
    return nullptr;
  }

  auto subscription = std::unique_ptr<SensorSubscription>(
      new SensorSubscription(type, std::move(callback)));
  if (!subscription->Initialize(sampling_interval_ns)) {
    return nullptr;
  }
  return subscription;
}

SensorSubscription::SensorSubscription(Sensor_Type type, EventCallback callback)
    : type_(type),
      relay_(base::MakeRefCounted<SensorEventRelay>(std::move(callback))) {}

SensorSubscription::~SensorSubscription() {
  if (subscribed_) {
    const int32_t result = OH_Sensor_Unsubscribe(id_.get(), subscriber_.get());
    if (result != SENSOR_SUCCESS) {
      LOG(ERROR) << "OH_Sensor_Unsubscribe failed for type " << type_ << ": "
                 << result;
    }
    subscribed_ = false;
  }

  // SensorServiceKit stops dispatching when Unsubscribe returns. Keeping the
  // relay registered until then prevents an in-flight callback from observing
  // freed state.
  if (relay_) {
    UnregisterRelay(type_, relay_.get());
    relay_.reset();
  }
}

bool SensorSubscription::Initialize(int64_t sampling_interval_ns) {
  return CreateSubscriber() && CreateSubscriptionId() &&
         CreateSubscriptionAttribute(sampling_interval_ns) && Subscribe();
}

bool SensorSubscription::CreateSubscriber() {
  subscriber_.reset(OH_Sensor_CreateSubscriber());
  if (!subscriber_) {
    LOG(ERROR) << "OH_Sensor_CreateSubscriber returned null";
    return false;
  }
  int32_t result =
      OH_SensorSubscriber_SetCallback(subscriber_.get(), &OnSensorEvent);
  if (result != SENSOR_SUCCESS) {
    LOG(ERROR) << "OH_SensorSubscriber_SetCallback failed: " << result;
    return false;
  }
  return true;
}

bool SensorSubscription::CreateSubscriptionId() {
  id_.reset(OH_Sensor_CreateSubscriptionId());
  if (!id_) {
    LOG(ERROR) << "OH_Sensor_CreateSubscriptionId returned null";
    return false;
  }
  const int32_t result = OH_SensorSubscriptionId_SetType(id_.get(), type_);
  if (result != SENSOR_SUCCESS) {
    LOG(ERROR) << "OH_SensorSubscriptionId_SetType failed: " << result;
    return false;
  }
  return true;
}

bool SensorSubscription::CreateSubscriptionAttribute(
    int64_t sampling_interval_ns) {
  attribute_.reset(OH_Sensor_CreateSubscriptionAttribute());
  if (!attribute_) {
    LOG(ERROR) << "OH_Sensor_CreateSubscriptionAttribute returned null";
    return false;
  }
  const int32_t result = OH_SensorSubscriptionAttribute_SetSamplingInterval(
      attribute_.get(), sampling_interval_ns);
  if (result != SENSOR_SUCCESS) {
    LOG(ERROR) << "SetSamplingInterval failed: " << result;
    return false;
  }
  return true;
}

bool SensorSubscription::Subscribe() {
  if (!RegisterRelay(type_, relay_)) {
    LOG(ERROR) << "A subscription already exists for sensor type " << type_;
    return false;
  }
  const int32_t result =
      OH_Sensor_Subscribe(id_.get(), attribute_.get(), subscriber_.get());
  if (result != SENSOR_SUCCESS) {
    LOG(ERROR) << "OH_Sensor_Subscribe failed for type " << type_ << ": "
               << result;
    UnregisterRelay(type_, relay_.get());
    return false;
  }

  subscribed_ = true;
  return true;
}

void SensorSubscription::AttributeDeleter::operator()(
    Sensor_SubscriptionAttribute* attribute) const {
  LogDestroyError("OH_Sensor_DestroySubscriptionAttribute",
                  OH_Sensor_DestroySubscriptionAttribute(attribute));
}

void SensorSubscription::IdDeleter::operator()(
    Sensor_SubscriptionId* id) const {
  LogDestroyError("OH_Sensor_DestroySubscriptionId",
                  OH_Sensor_DestroySubscriptionId(id));
}

void SensorSubscription::SubscriberDeleter::operator()(
    Sensor_Subscriber* subscriber) const {
  LogDestroyError("OH_Sensor_DestroySubscriber",
                  OH_Sensor_DestroySubscriber(subscriber));
}

}  // namespace device::ohos
