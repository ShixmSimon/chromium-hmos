// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Favicons for the shell's bookmarks and history pages. The shell asks for a
// batch of page URLs and gets one answer holding every icon the favicon
// database has; it draws a letter placeholder for the rest.

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/logging.h"
#include "base/memory/ref_counted_memory.h"
#include "base/no_destructor.h"
#include "base/numerics/safe_conversions.h"
#include "base/task/cancelable_task_tracker.h"
#include "base/values.h"
#include "chrome/browser/favicon/favicon_service_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/ohos/shell_services_ohos.h"
#include "components/favicon/core/favicon_service.h"
#include "components/favicon_base/favicon_callback.h"
#include "components/favicon_base/favicon_types.h"
#include "components/keyed_service/core/service_access_type.h"
#include "ui/display/display.h"
#include "ui/display/screen.h"
#include "base/memory/ref_counted.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "url/gurl.h"

namespace chrome::ohos {

namespace {

constexpr char kGetFaviconsCommand[] = "getFavicons";
constexpr char kFaviconsEvent[] = "favicons";

// Bounds one request's work; a page of history is well under this.
constexpr size_t kMaxUrlsPerRequest = 100;

// A list row's icon when the shell does not say; the clamp keeps a bad value
// from asking the database to resize to something absurd.
constexpr double kDefaultSizeVp = 24;
constexpr double kMinSizeVp = 1;
constexpr double kMaxSizeVp = 256;

constexpr float kFallbackScale = 1.0f;

// How long a batch waits for the favicon database before answering with
// whatever it has. The shell gives up at five seconds and draws letter tiles;
// answering just before that at least fills in the icons that did arrive.
constexpr base::TimeDelta kBatchDeadline = base::Seconds(3);

// A URL as the shell sent it, which is what the answer must echo: the parsed
// spec can differ (a trailing slash, say) and the shell matches by string.
struct PageUrl {
  std::string requested;
  GURL url;
};

// One URL's answer; an empty `png_base64` is a miss.
struct FaviconAnswer {
  std::string url;
  std::string png_base64;
};

// --- Chromium API access, one helper per call so a rename touches one place.

favicon::FaviconService* FaviconServiceFor(Profile* profile) {
  // Explicit access: an incognito window may read the original profile's
  // icons, as its omnibox does, but never writes them.
  return profile ? FaviconServiceFactory::GetForProfile(
                       profile, ServiceAccessType::EXPLICIT_ACCESS)
                 : nullptr;
}

float PrimaryDisplayScale() {
  display::Screen* screen = display::Screen::Get();
  return screen ? screen->GetPrimaryDisplay().device_scale_factor()
                : kFallbackScale;
}

void RequestRawFavicon(favicon::FaviconService* service,
                       const GURL& page_url,
                       int size_px,
                       favicon_base::FaviconRawBitmapCallback callback,
                       base::CancelableTaskTracker* tracker) {
  // Touch and manifest icons count too: at phone densities they are often
  // the only ones large enough, and some sites declare nothing else. Only the
  // page's own icon, though; the host's would put one page's favicon on
  // another of the same site.
  const favicon_base::IconTypeSet icon_types = {
      favicon_base::IconType::kFavicon, favicon_base::IconType::kTouchIcon,
      favicon_base::IconType::kTouchPrecomposedIcon,
      favicon_base::IconType::kWebManifestIcon};
  service->GetRawFaviconForPageURL(page_url, icon_types, size_px,
                                   /*fallback_to_host=*/false,
                                   std::move(callback), tracker);
}

std::string EncodePng(const favicon_base::FaviconRawBitmapResult& result) {
  if (!result.is_valid()) {
    return std::string();
  }
  return base::Base64Encode(base::span<const uint8_t>(*result.bitmap_data));
}

// Outlives every lookup it tracks, and cancels them when the profile goes.
PerProfile<base::CancelableTaskTracker>& Trackers() {
  static base::NoDestructor<PerProfile<base::CancelableTaskTracker>> trackers(
      base::BindRepeating([](Profile*) {
        return std::make_unique<base::CancelableTaskTracker>();
      }));
  return *trackers;
}

// --- Input. ---------------------------------------------------------------

// Valid, distinct page URLs, at most kMaxUrlsPerRequest of them.
std::vector<PageUrl> ReadPageUrls(const base::DictValue& command) {
  std::vector<PageUrl> urls;
  const base::ListValue* list = command.FindList("urls");
  if (!list) {
    LOG(WARNING) << "OHOS shell favicons: missing urls";
    return urls;
  }
  if (list->size() > kMaxUrlsPerRequest) {
    LOG(WARNING) << "OHOS shell favicons: " << list->size()
                 << " urls, answering the first " << kMaxUrlsPerRequest;
  }
  std::set<std::string> seen;
  for (const base::Value& value : *list) {
    if (urls.size() == kMaxUrlsPerRequest) {
      break;
    }
    const std::string* spec = value.GetIfString();
    GURL url = spec ? GURL(*spec) : GURL();
    if (!url.is_valid()) {
      LOG(WARNING) << "OHOS shell favicons: skipping an invalid url";
      continue;
    }
    if (seen.insert(*spec).second) {
      urls.push_back({*spec, std::move(url)});
    }
  }
  return urls;
}

int ReadSizePx(const base::DictValue& command) {
  const double size_vp = std::clamp(
      command.FindDouble("sizeVp").value_or(kDefaultSizeVp), kMinSizeVp,
      kMaxSizeVp);
  return base::ClampRound<int>(size_vp * PrimaryDisplayScale());
}

// --- Answer. --------------------------------------------------------------

void SendFavicons(const ShellCommandContext& context,
                  int request_id,
                  std::vector<FaviconAnswer> answers) {
  base::ListValue items;
  for (FaviconAnswer& answer : answers) {
    if (answer.png_base64.empty()) {
      continue;
    }
    base::DictValue item;
    item.Set("url", std::move(answer.url));
    item.Set("pngBase64", std::move(answer.png_base64));
    items.Append(std::move(item));
  }
  base::DictValue event;
  event.Set("event", kFaviconsEvent);
  event.Set("requestId", request_id);
  event.Set("items", std::move(items));
  ReplyToShell(context, std::move(event));
}

// One request's answers, and the promise that exactly one event goes back.
//
// A BarrierCallback was simpler but had no floor: it answers only when every
// lookup has, so a single lookup that never calls back leaves the shell
// waiting forever and its list permanently empty. A row with no icon is a far
// better outcome than a list that never arrives, so the batch also answers on
// a deadline with whatever it has.
class FaviconBatch : public base::RefCounted<FaviconBatch> {
 public:
  FaviconBatch(const ShellCommandContext& context,
               int request_id,
               size_t expected)
      : context_(context), request_id_(request_id), remaining_(expected) {}

  FaviconBatch(const FaviconBatch&) = delete;
  FaviconBatch& operator=(const FaviconBatch&) = delete;

  void Add(FaviconAnswer answer) {
    if (sent_) {
      return;
    }
    answers_.push_back(std::move(answer));
    if (remaining_ > 0 && --remaining_ == 0) {
      Send(/*on_deadline=*/false);
    }
  }

  void SendWhatArrived() { Send(/*on_deadline=*/true); }

 private:
  friend class base::RefCounted<FaviconBatch>;
  ~FaviconBatch() = default;

  void Send(bool on_deadline) {
    if (std::exchange(sent_, true)) {
      return;
    }
    if (on_deadline) {
      // Worth a line: it means a lookup never came back, which is the engine's
      // problem and not the shell's.
      LOG(WARNING) << "OHOS shell favicons: request " << request_id_
                   << " timed out with " << remaining_
                   << " of its lookups unanswered";
    }
    SendFavicons(context_, request_id_, std::move(answers_));
  }

  const ShellCommandContext context_;
  const int request_id_;
  size_t remaining_;
  bool sent_ = false;
  std::vector<FaviconAnswer> answers_;
};

void OnRawFavicon(std::string page_url,
                  scoped_refptr<FaviconBatch> batch,
                  const favicon_base::FaviconRawBitmapResult& result) {
  batch->Add(FaviconAnswer{std::move(page_url), EncodePng(result)});
}

void GetFavicons(const ShellCommandContext& context,
                 const base::DictValue& command) {
  const int request_id = ReadRequestId(command);
  favicon::FaviconService* service = FaviconServiceFor(context.profile);
  base::CancelableTaskTracker* tracker = Trackers().Get(context.profile);
  if (!service || !tracker) {
    // Answered anyway (empty) so the shell falls back to placeholders.
    LOG(WARNING) << "OHOS shell favicons: profile has no favicon service";
    SendFavicons(context, request_id, {});
    return;
  }
  const std::vector<PageUrl> urls = ReadPageUrls(command);
  const int size_px = ReadSizePx(command);
  if (urls.empty()) {
    SendFavicons(context, request_id, {});
    return;
  }
  auto batch =
      base::MakeRefCounted<FaviconBatch>(context, request_id, urls.size());
  for (const PageUrl& page : urls) {
    RequestRawFavicon(service, page.url, size_px,
                      base::BindOnce(&OnRawFavicon, page.requested, batch),
                      tracker);
  }
  base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE, base::BindOnce(&FaviconBatch::SendWhatArrived, batch),
      kBatchDeadline);
}

}  // namespace

bool HandleFaviconsCommand(const ShellCommandContext& context,
                           std::string_view name,
                           const base::DictValue& command) {
  if (name != kGetFaviconsCommand) {
    return false;
  }
  GetFavicons(context, command);
  return true;
}

}  // namespace chrome::ohos
