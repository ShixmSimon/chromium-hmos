// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_OHOS_SYSTEM_SERVICE_SYSTEM_SERVICE_OHOS_H_
#define COMPONENTS_OHOS_SYSTEM_SERVICE_SYSTEM_SERVICE_OHOS_H_

#include <memory>
#include <string>
#include <string_view>

#include "base/functional/callback.h"
#include "base/values.h"

// A request/response channel from Chromium to HarmonyOS services that exist
// only in ArkTS: text-to-speech, notifications, barcode and face detection,
// biometric authentication, window orientation.
//
// Every message is JSON. Chromium sends
//   {"id": N, "service": "...", "method": "...", "args": {...}}
// and ArkTS answers each one exactly once with
//   {"id": N, "ok": true, "result": <any>}  or  {"id": N, "ok": false,
//                                                "error": "..."}
// ArkTS may also push events nobody asked for:
//   {"service": "...", "event": "...", "data": {...}}
//
// ArkTS lives only in the app's main process, so this works only in the
// browser process. In a child process there is no dispatcher and every call
// fails immediately.
namespace ohos_system_service {

// The reply to one call. `ok` false carries `error`; otherwise `result`.
struct Reply {
  Reply();
  Reply(Reply&&);
  Reply& operator=(Reply&&);
  ~Reply();

  bool ok = false;
  std::string error;
  base::Value result;

  // `result` as a dict, or an empty one when it is anything else.
  const base::DictValue& result_dict() const;
};

using ReplyCallback = base::OnceCallback<void(Reply)>;
using EventCallback =
    base::RepeatingCallback<void(const std::string& event,
                                 const base::DictValue& data)>;

// Installed by the N-API layer once ArkTS has registered its handler, and
// cleared (with a null callback) when it goes away. Thread-safe. The
// dispatcher must accept calls from any thread.
using Dispatcher = base::RepeatingCallback<void(const std::string& json)>;
void SetDispatcher(Dispatcher dispatcher);

// Whether ArkTS is attached right now.
bool IsAvailable();

// Sends one request. `reply` runs on the calling sequence, exactly once: with
// the ArkTS answer, or with an error if no dispatcher is installed or it is
// removed while the call is outstanding. Must be called on a sequence.
void Call(std::string_view service,
          std::string_view method,
          base::DictValue args,
          ReplyCallback reply);

// Receives one message from ArkTS. Thread-safe.
void DeliverMessage(const std::string& json);

// Receives the events one service pushes, on the sequence that subscribed,
// for as long as the returned object lives.
class EventSubscription {
 public:
  virtual ~EventSubscription() = default;
};
[[nodiscard]] std::unique_ptr<EventSubscription> SubscribeToEvents(
    std::string_view service,
    EventCallback callback);

}  // namespace ohos_system_service

#endif  // COMPONENTS_OHOS_SYSTEM_SERVICE_SYSTEM_SERVICE_OHOS_H_
