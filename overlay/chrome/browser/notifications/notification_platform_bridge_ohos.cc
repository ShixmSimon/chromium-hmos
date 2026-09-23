// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Web notifications as HarmonyOS notifications, published by the engine HAR's
// NotificationService over the ohos_system_service channel.
//
// The id HarmonyOS carries for a notification is everything needed to route a
// tap back into Chromium -- profile, incognito, handler type, origin and the
// Chromium id -- so a tap still lands after the app was killed and relaunched
// by it, when no in-memory record of the notification survives.

#include <algorithm>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/values.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/notifications/notification_common.h"
#include "chrome/browser/notifications/notification_display_service_impl.h"
#include "chrome/browser/notifications/notification_handler.h"
#include "chrome/browser/notifications/notification_platform_bridge.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "components/ohos_system_service/system_service_ohos.h"
#include "content/public/browser/browser_thread.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkImageInfo.h"
#include "ui/base/models/image_model.h"
#include "ui/gfx/image/image.h"
#include "ui/gfx/image/image_skia_operations.h"
#include "ui/gfx/skia_util.h"
#include "ui/message_center/public/cpp/notification.h"
#include "url/gurl.h"

namespace {

constexpr char kService[] = "notification";
// The largest icon HarmonyOS shows; it drops anything over 192 KB of pixels.
constexpr int kMaxIconSize = 128;
// Fields of the routing key, which HarmonyOS stores as the notification label.
constexpr char kKeySeparator = '\x1f';

struct RoutingKey {
  std::string profile_id;
  bool incognito = false;
  NotificationHandler::Type type = NotificationHandler::Type::WEB_PERSISTENT;
  GURL origin;
  std::string notification_id;
};

std::string EncodeKey(const RoutingKey& key) {
  return base::JoinString(
      {key.profile_id, key.incognito ? "1" : "0",
       base::NumberToString(static_cast<int>(key.type)), key.origin.spec(),
       key.notification_id},
      std::string(1, kKeySeparator));
}

std::optional<RoutingKey> DecodeKey(const std::string& encoded) {
  // The Chromium id comes last and is allowed to contain the separator.
  std::vector<std::string> parts =
      base::SplitString(encoded, std::string(1, kKeySeparator),
                        base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL);
  int type = 0;
  if (parts.size() < 5 || !base::StringToInt(parts[2], &type) ||
      type < 0 || type > static_cast<int>(NotificationHandler::Type::MAX)) {
    return std::nullopt;
  }
  RoutingKey key;
  key.profile_id = parts[0];
  key.incognito = parts[1] == "1";
  key.type = static_cast<NotificationHandler::Type>(type);
  key.origin = GURL(parts[3]);
  std::vector<std::string> id_parts(parts.begin() + 4, parts.end());
  key.notification_id =
      base::JoinString(id_parts, std::string(1, kKeySeparator));
  return key;
}

RoutingKey KeyFor(Profile* profile,
                  NotificationHandler::Type type,
                  const GURL& origin,
                  const std::string& notification_id) {
  RoutingKey key;
  key.profile_id = NotificationPlatformBridge::GetProfileId(profile);
  key.incognito = profile->IsOffTheRecord();
  key.type = type;
  key.origin = origin;
  key.notification_id = notification_id;
  return key;
}

// The icon as base64 RGBA, scaled to fit what the notification shade shows.
std::optional<base::DictValue> EncodeIcon(
    const message_center::Notification& notification) {
  if (!notification.icon().IsImage()) {
    return std::nullopt;
  }
  gfx::ImageSkia image = notification.icon().GetImage().AsImageSkia();
  if (image.isNull() || image.width() <= 0 || image.height() <= 0) {
    return std::nullopt;
  }
  if (image.width() > kMaxIconSize || image.height() > kMaxIconSize) {
    const float scale = static_cast<float>(kMaxIconSize) /
                        std::max(image.width(), image.height());
    image = gfx::ImageSkiaOperations::CreateResizedImage(
        image, skia::ImageOperations::RESIZE_GOOD,
        gfx::Size(std::max(1, static_cast<int>(image.width() * scale)),
                  std::max(1, static_cast<int>(image.height() * scale))));
  }
  const SkBitmap* source = image.bitmap();
  if (!source || source->drawsNothing()) {
    return std::nullopt;
  }
  const SkImageInfo info =
      SkImageInfo::Make(source->width(), source->height(),
                        kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
  std::vector<uint8_t> pixels(info.computeMinByteSize());
  if (!source->readPixels(info, pixels.data(), info.minRowBytes(), 0, 0)) {
    return std::nullopt;
  }
  base::DictValue icon;
  icon.Set("width", source->width());
  icon.Set("height", source->height());
  icon.Set("rgba", base::Base64Encode(pixels));
  return icon;
}

void ForwardClick(const RoutingKey& key) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (key.profile_id.empty() || !g_browser_process ||
      !g_browser_process->profile_manager()) {
    return;
  }
  g_browser_process->profile_manager()->LoadProfile(
      NotificationPlatformBridge::GetProfileBaseNameFromProfileId(
          key.profile_id),
      key.incognito,
      base::BindOnce(&NotificationDisplayServiceImpl::ProfileLoadedCallback,
                     NotificationOperation::kClick, key.type, key.origin,
                     key.notification_id, /*action_index=*/std::nullopt,
                     /*reply=*/std::nullopt, /*by_user=*/std::nullopt,
                     /*is_suspicious=*/false, base::DoNothing()));
}

void LogFailure(const char* what, ohos_system_service::Reply reply) {
  if (!reply.ok) {
    LOG(WARNING) << "HarmonyOS notification " << what
                 << " failed: " << reply.error;
  }
}

class NotificationPlatformBridgeOhos : public NotificationPlatformBridge {
 public:
  NotificationPlatformBridgeOhos() {
    events_ = ohos_system_service::SubscribeToEvents(
        kService, base::BindRepeating(&NotificationPlatformBridgeOhos::OnEvent));
  }
  ~NotificationPlatformBridgeOhos() override = default;

  void Display(NotificationHandler::Type notification_type,
               Profile* profile,
               const message_center::Notification& notification,
               std::unique_ptr<NotificationCommon::Metadata> metadata) override {
    base::DictValue args;
    args.Set("id", EncodeKey(KeyFor(profile, notification_type,
                                    notification.origin_url(),
                                    notification.id())));
    args.Set("title", base::UTF16ToUTF8(notification.title()));
    args.Set("body", base::UTF16ToUTF8(notification.message()));
    args.Set("origin", notification.origin_url().host());
    args.Set("silent", notification.silent());
    if (std::optional<base::DictValue> icon = EncodeIcon(notification)) {
      args.Set("icon", std::move(*icon));
    }
    ohos_system_service::Call(kService, "publish", std::move(args),
                              base::BindOnce(&LogFailure, "publish"));
  }

  void Close(Profile* profile, const std::string& notification_id) override {
    // The key needs the type and origin, which Close is not given; find the
    // displayed notification whose Chromium id and profile match.
    const std::string profile_id = GetProfileId(profile);
    const bool incognito = profile->IsOffTheRecord();
    ohos_system_service::Call(
        kService, "displayed", base::DictValue(),
        base::BindOnce(
            [](std::string profile_id, bool incognito,
               std::string notification_id,
               ohos_system_service::Reply reply) {
              for (const std::string& encoded : IdsFrom(reply)) {
                std::optional<RoutingKey> key = DecodeKey(encoded);
                if (key && key->profile_id == profile_id &&
                    key->incognito == incognito &&
                    key->notification_id == notification_id) {
                  base::DictValue args;
                  args.Set("id", encoded);
                  ohos_system_service::Call(
                      kService, "close", std::move(args),
                      base::BindOnce(&LogFailure, "close"));
                }
              }
            },
            profile_id, incognito, notification_id));
  }

  void GetDisplayed(Profile* profile,
                    GetDisplayedNotificationsCallback callback) const override {
    QueryDisplayed(profile, std::nullopt, std::move(callback));
  }

  void GetDisplayedForOrigin(
      Profile* profile,
      const GURL& origin,
      GetDisplayedNotificationsCallback callback) const override {
    QueryDisplayed(profile, origin, std::move(callback));
  }

  void SetReadyCallback(NotificationBridgeReadyCallback callback) override {
    std::move(callback).Run(true);
  }

  void DisplayServiceShutDown(Profile* profile) override {}

 private:
  static std::vector<std::string> IdsFrom(
      const ohos_system_service::Reply& reply) {
    std::vector<std::string> ids;
    if (!reply.ok) {
      return ids;
    }
    if (const base::ListValue* list = reply.result_dict().FindList("ids")) {
      for (const base::Value& id : *list) {
        if (id.is_string()) {
          ids.push_back(id.GetString());
        }
      }
    }
    return ids;
  }

  static void QueryDisplayed(Profile* profile,
                             std::optional<GURL> origin,
                             GetDisplayedNotificationsCallback callback) {
    ohos_system_service::Call(
        kService, "displayed", base::DictValue(),
        base::BindOnce(
            [](std::string profile_id, bool incognito,
               std::optional<GURL> origin,
               GetDisplayedNotificationsCallback callback,
               ohos_system_service::Reply reply) {
              std::set<std::string> displayed;
              for (const std::string& encoded : IdsFrom(reply)) {
                std::optional<RoutingKey> key = DecodeKey(encoded);
                if (!key || key->profile_id != profile_id ||
                    key->incognito != incognito) {
                  continue;
                }
                if (origin && key->origin != *origin) {
                  continue;
                }
                displayed.insert(key->notification_id);
              }
              // HarmonyOS keeps notifications across app restarts, so the
              // list is complete: Chromium may close what is not in it.
              std::move(callback).Run(std::move(displayed),
                                      /*supports_synchronization=*/reply.ok);
            },
            GetProfileId(profile), profile->IsOffTheRecord(), origin,
            std::move(callback)));
  }

  static void OnEvent(const std::string& event, const base::DictValue& data) {
    if (event != "click") {
      return;
    }
    const std::string* encoded = data.FindString("id");
    if (!encoded) {
      return;
    }
    if (std::optional<RoutingKey> key = DecodeKey(*encoded)) {
      ForwardClick(*key);
    }
  }

  std::unique_ptr<ohos_system_service::EventSubscription> events_;
};

}  // namespace

std::unique_ptr<NotificationPlatformBridge>
NotificationPlatformBridge::Create() {
  return std::make_unique<NotificationPlatformBridgeOhos>();
}

// Everything goes to HarmonyOS: there is no message center on this platform
// to fall back to (enable_chrome_notifications is false here).
bool NotificationPlatformBridge::CanHandleType(
    NotificationHandler::Type notification_type) {
  return true;
}
