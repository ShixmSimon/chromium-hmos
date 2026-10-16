// Copyright 2026 The Chromium Authors. BSD-style license in LICENSE.
#include "chrome/browser/ohos/translation/translation.h"
#include <map>
#include <memory>
#include <set>
#include <utility>
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/json/json_writer.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/utf_string_conversions.h"
#include "base/supports_user_data.h"
#include "base/timer/timer.h"
#include "base/unguessable_token.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/ohos/shell_services_ohos.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/common/chrome_isolated_world_ids.h"
#include "chrome/renderer/ohos/translation_resources/runtime.h"
#include "components/sessions/content/session_tab_helper.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/weak_document_ptr.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"

namespace chrome::ohos::translation {
namespace {
constexpr int kMaxSegments = 32, kMaxText = 12000, kMaxResult = 256000;
const char kProfileKey = 0;
std::string Id() {
  return base::UnguessableToken::Create().ToString();
}
std::string Str(const base::DictValue& d, std::string_view key) {
  const auto* value = d.FindString(key);
  return value ? *value : "";
}
size_t Units(const std::string& s) {
  return base::UTF8ToUTF16(s).size();
}
bool Style(const std::string& s) {
  return s == "standard" || s == "quote" || s == "gray";
}
bool Language(const std::string& s, bool empty) {
  if (s.empty())
    return empty;
  return s.size() <= 64 &&
         s.find_first_not_of(
             "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVW"
             "XYZ0123456789-") == std::string::npos;
}
struct ProfileIdentity : base::SupportsUserData::Data {
  std::string id = Id();
};
std::string Context(Profile* profile) {
  if (!profile->GetUserData(&kProfileKey))
    profile->SetUserData(&kProfileKey, std::make_unique<ProfileIdentity>());
  return static_cast<ProfileIdentity*>(profile->GetUserData(&kProfileKey))->id;
}
std::string EventName(std::string_view name) {
  if (name == "getTranslationState")
    return "translationState";
  if (name == "takeTranslationBatch")
    return "translationBatch";
  if (name == "applyTranslationBatch" || name == "releaseTranslationBatch")
    return "translationBatchResult";
  return "translationSessionResult";
}
base::DictValue Limits() {
  return base::DictValue()
      .Set("maxSegments", 32)
      .Set("maxTextUnits", 12000)
      .Set("maxSegmentUnits", 2000)
      .Set("maxResultSegmentUnits", 8000)
      .Set("maxResultBatchUnits", 256000)
      .Set("maxRetainedTextUnits", 1048576)
      .Set("maxSessionSegments", 5000)
      .Set("maxTrackedNodes", 5000)
      .Set("maxInFlightBatches", 2)
      .Set("leaseMs", 45000)
      .Set("maxRetries", 2)
      .Set("scrollSettleMs", 180)
      .Set("taskBudgetMs", 4)
      .Set("maxPrefetchViewportRatio", 0.5);
}
struct Batch {
  base::TimeTicks expires;
  std::map<std::string, int> revisions;
};
class Page : public content::WebContentsObserver,
             public content::WebContentsUserData<Page> {
 public:
  static Page* Get(content::WebContents* wc) {
    CreateForWebContents(wc);
    return FromWebContents(wc);
  }
  std::string Tab() const {
    return ToShellId(sessions::SessionTabHelper::IdForTab(web_contents()).id());
  }
  std::string Navigation() const {
    auto* frame = web_contents()->GetPrimaryMainFrame();
    if (current_document_.AsRenderFrameHostIfValid() != frame) {
      current_document_ = frame->GetWeakDocumentPtr();
      current_navigation_ = Id();
    }
    return current_navigation_;
  }
  bool Supported() const {
    auto* f = web_contents()->GetPrimaryMainFrame();
    return f && f->IsActive() && f->IsRenderFrameLive() &&
           f->GetLastCommittedURL().SchemeIsHTTPOrHTTPS() &&
           !f->GetLastCommittedOrigin().opaque() &&
           web_contents()->GetContentsMimeType() == "text/html";
  }
  base::DictValue Envelope() const {
    return base::DictValue()
        .Set("protocolVersion", 1)
        .Set("contextId", context_)
        .Set("tabId", Tab())
        .Set("navigationId", navigation_.empty() ? Navigation() : navigation_)
        .Set("sessionId", session_)
        .Set("sequence", static_cast<double>(sequence_));
  }
  base::DictValue State() const {
    auto e = Envelope();
    e.Set("supported", Supported());
    e.Set("frameScope", "mainFrame");
    e.Set("state", session_.empty() ? "idle"
                   : stopped_       ? "stopped"
                                    : "waiting");
    for (const auto [k, v] : state_)
      e.Set(k, v.Clone());
    if (!stopped_ &&
        web_contents()->GetVisibility() != content::Visibility::VISIBLE) {
      e.Set("state", "paused").Set("hasMore", false);
    }
    return e;
  }
  void Send(gfx::AcceleratedWidget widget, base::DictValue event) {
    ReplyToShell(
        {widget, nullptr,
         Profile::FromBrowserContext(web_contents()->GetBrowserContext())},
        std::move(event));
  }
  void Notify(std::string event,
              std::string reason = "",
              std::string batch = "") {
    auto e = State();
    e.Set("event", event);
    e.Set("reason", reason);
    e.Set("sequence", static_cast<double>(++sequence_));
    if (!batch.empty())
      e.Set("batchId", batch);
    Send(widget_, std::move(e));
  }
  void End(std::string reason, bool cleanup) {
    if (session_.empty() || stopped_)
      return;
    if (cleanup && navigation_ == Navigation() &&
        web_contents()->GetPrimaryMainFrame()->IsRenderFrameLive()) {
      base::DictValue c;
      c.Set("op", "stop").Set("sessionId", session_);
      Execute(std::move(c), base::DoNothing());
    }
    operation_timeout_.Stop();
    ++operation_;
    if (!pending_reply_.empty()) {
      pending_reply_.Set("ok", false).Set("reason", "sessionEnded");
      Send(pending_widget_, std::move(pending_reply_));
      pending_reply_.clear();
    }
    stopped_ = true;
    busy_ = false;
    batches_.clear();
    timer_.Stop();
    state_.clear();
    state_.Set("state", "stopped").Set("hasMore", false);
    Notify("translationSessionEnded", reason);
  }
  void DidStartNavigation(content::NavigationHandle* h) override {
    if (h->IsInPrimaryMainFrame() && !h->IsSameDocument())
      End("navigation", true);
  }
  void PrimaryMainFrameRenderProcessGone(base::TerminationStatus) override {
    End("rendererGone", false);
  }
  void WebContentsDestroyed() override { End("tabClosed", false); }
  void Command(const ShellCommandContext& ctx,
               std::string_view name,
               const base::DictValue& input) {
    const auto event = EventName(name);
    const int request = ReadRequestId(input);
    auto reject = [&](std::string reason) {
      auto e = State();
      e.Set("event", event)
          .Set("requestId", request)
          .Set("ok", false)
          .Set("reason", reason);
      Send(ctx.widget, std::move(e));
    };
    if (!input.FindInt("requestId")) {
      reject("invalidRequest");
      return;
    }
    if (name == "getTranslationState") {
      auto e = State();
      e.Set("event", event).Set("requestId", request).Set("ok", true);
      // State discovery must always expose the current document after
      // navigation.
      e.Set("navigationId", Navigation());
      Send(ctx.widget, std::move(e));
      return;
    }
    if (Str(input, "contextId") != context_ ||
        Str(input, "navigationId") != Navigation()) {
      reject("staleDocument");
      return;
    }
    widget_ = ctx.widget;
    base::DictValue c;
    if (name == "startTranslation") {
      if (!Supported()) {
        reject("unsupportedPage");
        return;
      }
      const auto target = Str(input, "targetLanguage"),
                 source = Str(input, "sourceLanguage"),
                 style = Str(input, "style");
      const double prefetch =
          input.FindDouble("prefetchViewportRatio").value_or(0.0);
      if (!Language(target, false) || !Language(source, true) ||
          !Style(style) || prefetch < 0 || prefetch > 0.5) {
        reject("invalidRequest");
        return;
      }
      End("replaced", true);
      session_ = Id();
      navigation_ = Navigation();
      stopped_ = false;
      widget_ = ctx.widget;
      state_.clear();
      batches_.clear();
      sequence_ = 0;
      source_units_ = 0;
      result_units_ = 0;
      source_attempts_.clear();
      c.Set("op", "start")
          .Set("targetLanguage", target)
          .Set("style", style)
          .Set("prefetchViewportRatio", prefetch);
      timer_.Start(FROM_HERE, base::Milliseconds(300), this, &Page::Poll);
    } else {
      if (Str(input, "sessionId") != session_ || session_.empty()) {
        reject("sessionEnded");
        return;
      }
      if (name == "stopTranslation") {
        End("userStopped", true);
        auto e = State();
        e.Set("event", event).Set("requestId", request).Set("ok", true);
        Send(ctx.widget, std::move(e));
        return;
      }
      if (stopped_) {
        reject("sessionEnded");
        return;
      }
      if (busy_) {
        reject("busy");
        return;
      }
      if (name == "setTranslationPresentation") {
        if (!Style(Str(input, "style")) || !input.FindBool("showTranslation")) {
          reject("invalidRequest");
          return;
        }
        c.Set("op", "presentation")
            .Set("style", Str(input, "style"))
            .Set("showTranslation", *input.FindBool("showTranslation"));
      } else if (name == "takeTranslationBatch") {
        const int n = input.FindInt("maxSegments").value_or(kMaxSegments),
                  units = input.FindInt("maxTextUnits").value_or(kMaxText);
        if (n < 1 || n > kMaxSegments || units < 2000 || units > kMaxText) {
          reject("invalidRequest");
          return;
        }
        Expire();
        if (batches_.size() >= 2) {
          reject("concurrencyLimit");
          return;
        }
        if (web_contents()->GetVisibility() != content::Visibility::VISIBLE) {
          reject("paused");
          return;
        }
        c.Set("op", "take")
            .Set("batchId", Id())
            .Set("maxSegments", n)
            .Set("maxTextUnits", units);
      } else {
        const std::string id = Str(input, "batchId");
        auto it = batches_.find(id);
        if (it == batches_.end() ||
            it->second.expires <= base::TimeTicks::Now()) {
          reject("batchExpired");
          return;
        }
        c.Set("batchId", id);
        if (name == "releaseTranslationBatch") {
          const std::string reason = Str(input, "reason");
          if (reason != "providerError" && reason != "rateLimited" &&
              reason != "authentication" && reason != "quota" &&
              reason != "cancelled" && reason != "timeout") {
            reject("invalidRequest");
            return;
          }
          c.Set("op", "release")
              .Set("retryable", input.FindBool("retryable").value_or(false))
              .Set("reason", reason);
        } else {
          const auto* values = input.FindList("segments");
          if (!values || values->size() > kMaxSegments) {
            reject("invalidResult");
            return;
          }
          size_t units = 0;
          std::set<std::string> seen;
          for (const auto& value : *values) {
            const auto* d = value.GetIfDict();
            if (!d) {
              reject("invalidResult");
              return;
            }
            const auto id = Str(*d, "segmentId");
            const auto known = it->second.revisions.find(id);
            if (known == it->second.revisions.end() ||
                !seen.insert(id).second || Str(*d, "frameId") != navigation_ ||
                Str(*d, "frameDocumentId") != navigation_ ||
                d->FindInt("segmentRevision") != known->second ||
                !d->FindString("translatedText")) {
              reject("invalidResult");
              return;
            }
            size_t n = Units(Str(*d, "translatedText"));
            units += n;
            if (n > 8000 || units > kMaxResult ||
                source_units_ + result_units_ + units > 1048576) {
              reject("tooLarge");
              return;
            }
          }
          c.Set("op", "apply").Set("segments", values->Clone());
        }
      }
    }
    c.Set("sessionId", session_);
    busy_ = true;
    const uint64_t operation = ++operation_;
    pending_widget_ = ctx.widget;
    pending_reply_ = Envelope();
    pending_reply_.Set("event", event).Set("requestId", request);
    operation_timeout_.Start(FROM_HERE, base::Seconds(5), this,
                             &Page::TimedOut);
    auto parameters = c.Clone();
    Execute(std::move(parameters),
            base::BindOnce(&Page::Complete, weak_.GetWeakPtr(), operation,
                           ctx.widget, request, event, session_, navigation_,
                           std::move(c)));
  }

 private:
  friend class content::WebContentsUserData<Page>;
  explicit Page(content::WebContents* wc)
      : content::WebContentsObserver(wc),
        content::WebContentsUserData<Page>(*wc),
        context_(
            Context(Profile::FromBrowserContext(wc->GetBrowserContext()))) {}
  void Execute(base::DictValue c, base::OnceCallback<void(base::Value)> cb) {
    std::string json;
    base::JSONWriter::Write(c, &json);
    const std::string script =
        (Str(c, "op") == "start" ? std::string(kTranslationRuntime)
                                 : std::string()) +
        ";globalThis.__lumieTranslation ? globalThis.__lumieTranslation.call(" +
        json + ") : ({ok:false,reason:'sessionEnded'})";
    web_contents()->GetPrimaryMainFrame()->ExecuteJavaScriptInIsolatedWorld(
        base::UTF8ToUTF16(script), std::move(cb),
        ISOLATED_WORLD_ID_LUMIE_TRANSLATION);
  }
  void Complete(uint64_t operation,
                gfx::AcceleratedWidget widget,
                int request,
                std::string event,
                std::string session,
                std::string nav,
                base::DictValue command,
                base::Value value) {
    if (operation != operation_)
      return;
    operation_timeout_.Stop();
    pending_reply_.clear();
    auto reply = Envelope();
    reply.Set("sessionId", session)
        .Set("navigationId", nav)
        .Set("event", event)
        .Set("requestId", request)
        .Set("ok", false);
    if (session != session_ || nav != Navigation() || stopped_) {
      reply.Set("reason", "sessionEnded");
      Send(widget, std::move(reply));
      return;
    }
    busy_ = false;
    auto* d = value.GetIfDict();
    if (!d) {
      reply.Set("reason", "rendererGone");
      Send(widget, std::move(reply));
      End("rendererGone", true);
      return;
    }
    const std::string op = Str(command, "op"), id = Str(command, "batchId");
    bool ok = d->FindBool("ok").value_or(op == "start");
    if (op == "take" && ok) {
      auto* segments = d->FindList("segments");
      size_t units = 0, new_units = 0, new_segments = 0;
      Batch b;
      b.expires = base::TimeTicks::Now() + base::Seconds(45);
      if (!segments ||
          segments->size() >
              static_cast<size_t>(command.FindInt("maxSegments").value_or(32)))
        ok = false;
      if (segments)
        for (auto& item : *segments) {
          auto* part = item.GetIfDict();
          if (!part) {
            ok = false;
            break;
          }
          const auto text = Str(*part, "text"), sid = Str(*part, "segmentId");
          size_t n = Units(text);
          units += n;
          auto attempt = source_attempts_.find(sid);
          if (attempt == source_attempts_.end()) {
            new_units += n;
            ++new_segments;
          } else if (attempt->second >= 3) {
            ok = false;
            break;
          }
          auto rev = part->FindInt("segmentRevision");
          if (n == 0 || n > 2000 || sid.empty() || sid.size() > 100 || !rev ||
              *rev < 1 || !b.revisions.emplace(sid, *rev).second) {
            ok = false;
            break;
          }
          part->Set("frameId", navigation_).Set("frameDocumentId", navigation_);
        }
      if (units > static_cast<size_t>(
                      command.FindInt("maxTextUnits").value_or(kMaxText)) ||
          source_units_ + result_units_ + new_units > 1048576 ||
          source_attempts_.size() + new_segments > 5000)
        ok = false;
      if (!ok) {
        reply.Set("reason", "tooLarge");
        Send(widget, std::move(reply));
        End("limitReached", true);
        return;
      }
      if (!b.revisions.empty()) {
        source_units_ += new_units;
        for (const auto& [sid, rev] : b.revisions)
          ++source_attempts_[sid];
        batches_.emplace(id, std::move(b));
        reply.Set("batchId", id).Set("leaseMs", 45000);
      }
      reply.Set("segments", segments->Clone());
    }
    if (op == "apply" || op == "release") {
      batches_.erase(id);
      reply.Set("batchId", id);
      if (op == "apply" && ok) {
        for (const auto& v : *command.FindList("segments"))
          result_units_ += Units(Str(v.GetDict(), "translatedText"));
        for (auto key : {"applied", "rejected", "results"})
          if (const auto* v = d->Find(key))
            reply.Set(key, v->Clone());
      }
    }
    Update(*d);
    for (const auto [k, v] : state_)
      reply.Set(k, v.Clone());
    reply.Set("sequence", static_cast<double>(++sequence_))
        .Set("ok", ok)
        .Set("reason", ok ? "" : Str(*d, "reason"));
    Send(widget, std::move(reply));
    Notify("translationStateChanged");
  }
  void Update(const base::DictValue& data) {
    for (auto key :
         {"state", "discovered", "inFlight", "translated", "failed", "hasMore",
          "limitReached", "retainedTextUnits", "style", "showTranslation"})
      if (const auto* value = data.Find(key))
        state_.Set(key, value->Clone());
  }
  void Expire() {
    for (auto it = batches_.begin(); it != batches_.end();) {
      if (it->second.expires > base::TimeTicks::Now()) {
        ++it;
        continue;
      }
      Notify("translationBatchInvalidated", "batchExpired", it->first);
      it = batches_.erase(it);
    }
  }
  void TimedOut() {
    if (!pending_reply_.empty()) {
      pending_reply_.Set("ok", false).Set("reason", "rendererTimeout");
      Send(pending_widget_, std::move(pending_reply_));
      pending_reply_.clear();
    }
    End("rendererTimeout", true);
  }
  void Poll() {
    if (stopped_ || busy_)
      return;
    if (navigation_ != Navigation()) {
      End("navigation", false);
      return;
    }
    Expire();
    // Never execute into a cached or prerendered document; only the primary,
    // visible live document receives polling. DOM visibility pauses its work.
    if (web_contents()->GetVisibility() != content::Visibility::VISIBLE)
      return;
    busy_ = true;
    auto c = base::DictValue()
                 .Set("op", "status")
                 .Set("sessionId", session_)
                 .Set("paused", false);
    const uint64_t operation = ++operation_;
    operation_timeout_.Start(FROM_HERE, base::Seconds(5), this,
                             &Page::TimedOut);
    Execute(std::move(c), base::BindOnce(&Page::Polled, weak_.GetWeakPtr(),
                                         operation, session_));
  }
  void Polled(uint64_t operation, std::string session, base::Value value) {
    if (operation != operation_)
      return;
    operation_timeout_.Stop();
    if (session != session_ || stopped_)
      return;
    busy_ = false;
    if (navigation_ != Navigation()) {
      End("navigation", false);
      return;
    }
    auto* d = value.GetIfDict();
    if (!d) {
      End("rendererGone", false);
      return;
    }
    const auto previous = state_.Clone();
    Update(*d);
    if (auto* ids = d->FindList("invalidated"))
      for (const auto& id : *ids)
        if (id.is_string() && batches_.contains(id.GetString()))
          Notify("translationBatchInvalidated", "staleSegment", id.GetString());
    if (previous != state_)
      Notify("translationStateChanged");
    if (previous != state_ && state_.FindBool("hasMore").value_or(false))
      Notify("translationWorkAvailable");
  }
  mutable content::WeakDocumentPtr current_document_;
  mutable std::string current_navigation_;
  std::string context_, session_, navigation_;
  uint64_t operation_ = 0;
  base::DictValue pending_reply_;
  gfx::AcceleratedWidget pending_widget_ = gfx::kNullAcceleratedWidget;
  base::OneShotTimer operation_timeout_;
  gfx::AcceleratedWidget widget_ = gfx::kNullAcceleratedWidget;
  bool stopped_ = true, busy_ = false;
  uint64_t sequence_ = 0;
  size_t source_units_ = 0, result_units_ = 0;
  std::map<std::string, int> source_attempts_;
  base::DictValue state_;
  std::map<std::string, Batch> batches_;
  base::RepeatingTimer timer_;
  base::WeakPtrFactory<Page> weak_{this};
  WEB_CONTENTS_USER_DATA_KEY_DECL();
};
WEB_CONTENTS_USER_DATA_KEY_IMPL(Page);
}  // namespace
bool HandleCommand(const ShellCommandContext& ctx,
                   std::string_view name,
                   const base::DictValue& input) {
  if (name == "getTranslationCapabilities") {
    auto e = base::DictValue()
                 .Set("event", "translationCapabilities")
                 .Set("requestId", ReadRequestId(input))
                 .Set("protocolVersion", 1)
                 .Set("supported", true)
                 .Set("frameScope", "mainFrame")
                 .Set("styles", base::ListValue()
                                    .Append("standard")
                                    .Append("quote")
                                    .Append("gray"))
                 .Set("limits", Limits());
    ReplyToShell(ctx, std::move(e));
    return true;
  }
  auto* tabs = ctx.browser ? ctx.browser->GetTabStripModel() : nullptr;
  if (tabs)
    for (int i = 0; i < tabs->count(); ++i) {
      auto* wc = tabs->GetWebContentsAt(i);
      if (wc->GetBrowserContext() != ctx.profile)
        continue;
      if (ToShellId(sessions::SessionTabHelper::IdForTab(wc).id()) !=
          Str(input, "tabId"))
        continue;
      Page::Get(wc)->Command(ctx, name, input);
      return true;
    }
  auto e = base::DictValue()
               .Set("event", EventName(name))
               .Set("requestId", ReadRequestId(input))
               .Set("protocolVersion", 1)
               .Set("ok", false)
               .Set("reason", "invalidTab");
  for (auto key : {"contextId", "tabId", "navigationId", "sessionId"})
    e.Set(key, Str(input, key));
  ReplyToShell(ctx, std::move(e));
  return true;
}
}  // namespace chrome::ohos::translation
