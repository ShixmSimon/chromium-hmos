// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/ohos_system_service/system_service_ohos.h"

#include <map>
#include <optional>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/memory/scoped_refptr.h"
#include "base/no_destructor.h"
#include "base/synchronization/lock.h"
#include "base/task/sequenced_task_runner.h"
#include "base/thread_annotations.h"

namespace ohos_system_service {

Reply::Reply() = default;
Reply::Reply(Reply&&) = default;
Reply& Reply::operator=(Reply&&) = default;
Reply::~Reply() = default;

const base::DictValue& Reply::result_dict() const {
  static const base::NoDestructor<base::DictValue> empty;
  return result.is_dict() ? result.GetDict() : *empty;
}

namespace {

struct PendingCall {
  scoped_refptr<base::SequencedTaskRunner> runner;
  ReplyCallback reply;
};

struct Subscriber {
  int id = 0;
  std::string service;
  scoped_refptr<base::SequencedTaskRunner> runner;
  EventCallback callback;
};

Reply MakeError(std::string message) {
  Reply reply;
  reply.error = std::move(message);
  return reply;
}

void RunReply(ReplyCallback callback, Reply reply) {
  std::move(callback).Run(std::move(reply));
}

class Bridge {
 public:
  void SetDispatcher(Dispatcher dispatcher) {
    std::map<int, PendingCall> orphaned;
    {
      base::AutoLock lock(lock_);
      dispatcher_ = std::move(dispatcher);
      if (!dispatcher_) {
        // Nobody is left to answer these.
        orphaned.swap(pending_);
      }
    }
    for (auto& [id, call] : orphaned) {
      call.runner->PostTask(
          FROM_HERE,
          base::BindOnce(&RunReply, std::move(call.reply),
                         MakeError("HarmonyOS system services went away")));
    }
  }

  bool IsAvailable() {
    base::AutoLock lock(lock_);
    return !dispatcher_.is_null();
  }

  void Call(std::string_view service,
            std::string_view method,
            base::DictValue args,
            ReplyCallback reply) {
    scoped_refptr<base::SequencedTaskRunner> runner =
        base::SequencedTaskRunner::GetCurrentDefault();
    Dispatcher dispatcher;
    int id = 0;
    {
      base::AutoLock lock(lock_);
      dispatcher = dispatcher_;
      if (dispatcher) {
        id = ++next_id_;
        pending_.emplace(id, PendingCall{runner, std::move(reply)});
      }
    }
    if (!dispatcher) {
      runner->PostTask(FROM_HERE,
                       base::BindOnce(&RunReply, std::move(reply),
                                      MakeError("HarmonyOS system services "
                                                "are unavailable here")));
      return;
    }

    base::DictValue request;
    request.Set("id", id);
    request.Set("service", service);
    request.Set("method", method);
    request.Set("args", std::move(args));
    std::string json;
    if (!base::JSONWriter::Write(request, &json)) {
      Complete(id, MakeError("Could not encode the request"));
      return;
    }
    dispatcher.Run(json);
  }

  void DeliverMessage(const std::string& json) {
    std::optional<base::Value> value =
        base::JSONReader::Read(json, base::JSON_PARSE_RFC);
    if (!value || !value->is_dict()) {
      LOG(ERROR) << "Dropped a malformed HarmonyOS system service message";
      return;
    }
    base::DictValue& message = value->GetDict();

    if (std::optional<int> id = message.FindInt("id"); id && *id > 0) {
      Reply reply;
      reply.ok = message.FindBool("ok").value_or(false);
      if (const std::string* error = message.FindString("error")) {
        reply.error = *error;
      }
      if (base::Value* result = message.Find("result")) {
        reply.result = std::move(*result);
      }
      if (!reply.ok && reply.error.empty()) {
        reply.error = "The HarmonyOS service reported a failure";
      }
      Complete(*id, std::move(reply));
      return;
    }

    const std::string* service = message.FindString("service");
    const std::string* event = message.FindString("event");
    if (!service || !event) {
      LOG(ERROR) << "Dropped a HarmonyOS system service message with neither "
                    "an id nor an event";
      return;
    }
    base::DictValue data;
    if (base::DictValue* found = message.FindDict("data")) {
      data = std::move(*found);
    }

    std::vector<Subscriber> targets;
    {
      base::AutoLock lock(lock_);
      for (const Subscriber& subscriber : subscribers_) {
        if (subscriber.service == *service) {
          targets.push_back(subscriber);
        }
      }
    }
    for (const Subscriber& target : targets) {
      target.runner->PostTask(
          FROM_HERE, base::BindOnce(&Bridge::RunSubscriber,
                                    base::Unretained(this), target.id,
                                    *event, data.Clone()));
    }
  }

  int AddSubscriber(std::string_view service, EventCallback callback) {
    base::AutoLock lock(lock_);
    const int id = ++next_subscriber_id_;
    subscribers_.push_back(Subscriber{
        id, std::string(service),
        base::SequencedTaskRunner::GetCurrentDefault(), std::move(callback)});
    return id;
  }

  void RemoveSubscriber(int id) {
    base::AutoLock lock(lock_);
    std::erase_if(subscribers_,
                  [id](const Subscriber& s) { return s.id == id; });
  }

 private:
  void Complete(int id, Reply reply) {
    std::optional<PendingCall> call;
    {
      base::AutoLock lock(lock_);
      auto it = pending_.find(id);
      if (it == pending_.end()) {
        return;
      }
      call = std::move(it->second);
      pending_.erase(it);
    }
    call->runner->PostTask(FROM_HERE, base::BindOnce(&RunReply,
                                                     std::move(call->reply),
                                                     std::move(reply)));
  }

  // Runs on the subscriber's sequence. Looked up again so that a subscription
  // destroyed while the event was in flight is not called.
  void RunSubscriber(int id, std::string event, base::DictValue data) {
    EventCallback callback;
    {
      base::AutoLock lock(lock_);
      for (const Subscriber& subscriber : subscribers_) {
        if (subscriber.id == id) {
          callback = subscriber.callback;
          break;
        }
      }
    }
    if (callback) {
      callback.Run(event, data);
    }
  }

  base::Lock lock_;
  Dispatcher dispatcher_ GUARDED_BY(lock_);
  int next_id_ GUARDED_BY(lock_) = 0;
  std::map<int, PendingCall> pending_ GUARDED_BY(lock_);
  int next_subscriber_id_ GUARDED_BY(lock_) = 0;
  std::vector<Subscriber> subscribers_ GUARDED_BY(lock_);
};

Bridge& GetBridge() {
  static base::NoDestructor<Bridge> bridge;
  return *bridge;
}

class SubscriptionImpl : public EventSubscription {
 public:
  explicit SubscriptionImpl(int id) : id_(id) {}
  ~SubscriptionImpl() override { GetBridge().RemoveSubscriber(id_); }

 private:
  const int id_;
};

}  // namespace

void SetDispatcher(Dispatcher dispatcher) {
  GetBridge().SetDispatcher(std::move(dispatcher));
}

bool IsAvailable() {
  return GetBridge().IsAvailable();
}

void Call(std::string_view service,
          std::string_view method,
          base::DictValue args,
          ReplyCallback reply) {
  GetBridge().Call(service, method, std::move(args), std::move(reply));
}

void DeliverMessage(const std::string& json) {
  GetBridge().DeliverMessage(json);
}

std::unique_ptr<EventSubscription> SubscribeToEvents(std::string_view service,
                                                     EventCallback callback) {
  return std::make_unique<SubscriptionImpl>(
      GetBridge().AddSubscriber(service, std::move(callback)));
}

}  // namespace ohos_system_service
