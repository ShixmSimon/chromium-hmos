// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SERVICES_DEVICE_GENERIC_SENSOR_OHOS_SENSOR_SUBSCRIPTION_OHOS_H_
#define SERVICES_DEVICE_GENERIC_SENSOR_OHOS_SENSOR_SUBSCRIPTION_OHOS_H_

#include <sensors/oh_sensor_type.h>

#include <cstdint>
#include <memory>

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"

namespace device::ohos {

class SensorEventRelay;

// Owns one OHOS sensor subscription and its entry in the process-wide callback
// registry. SensorServiceKit's callback carries no user data, so the registry
// is the only way to route an event back to the subscription that requested it.
class SensorSubscription {
 public:
  using EventCallback =
      base::RepeatingCallback<void(int64_t, double, double, double, double)>;

  static std::unique_ptr<SensorSubscription> Create(
      Sensor_Type type,
      int64_t sampling_interval_ns,
      EventCallback callback);

  SensorSubscription(const SensorSubscription&) = delete;
  SensorSubscription& operator=(const SensorSubscription&) = delete;

  ~SensorSubscription();

 private:
  struct AttributeDeleter {
    void operator()(Sensor_SubscriptionAttribute* attribute) const;
  };
  struct IdDeleter {
    void operator()(Sensor_SubscriptionId* id) const;
  };
  struct SubscriberDeleter {
    void operator()(Sensor_Subscriber* subscriber) const;
  };

  using AttributePtr =
      std::unique_ptr<Sensor_SubscriptionAttribute, AttributeDeleter>;
  using IdPtr = std::unique_ptr<Sensor_SubscriptionId, IdDeleter>;
  using SubscriberPtr = std::unique_ptr<Sensor_Subscriber, SubscriberDeleter>;

  SensorSubscription(Sensor_Type type, EventCallback callback);

  bool Initialize(int64_t sampling_interval_ns);
  bool CreateSubscriber();
  bool CreateSubscriptionId();
  bool CreateSubscriptionAttribute(int64_t sampling_interval_ns);
  bool Subscribe();

  const Sensor_Type type_;
  scoped_refptr<SensorEventRelay> relay_;

  // This declaration order makes C++ destroy the subscriber before its ID and
  // attributes, matching the lifetime expected by SensorServiceKit.
  AttributePtr attribute_;
  IdPtr id_;
  SubscriberPtr subscriber_;
  bool subscribed_ = false;
};

}  // namespace device::ohos

#endif  // SERVICES_DEVICE_GENERIC_SENSOR_OHOS_SENSOR_SUBSCRIPTION_OHOS_H_
