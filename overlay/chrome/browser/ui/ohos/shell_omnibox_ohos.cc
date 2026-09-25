// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Suggestions for the shell's address bar and tiles for its new tab page. The
// shell draws both; Chromium's omnibox machinery supplies the data, so
// history, bookmark, open-tab and search-engine suggestions rank as they do
// on desktop.

#include "chrome/browser/ui/ohos/shell_omnibox_ohos.h"

#include <stddef.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/scoped_observation.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "chrome/browser/autocomplete/chrome_autocomplete_provider_client.h"
#include "chrome/browser/autocomplete/chrome_autocomplete_scheme_classifier.h"
#include "chrome/browser/history/top_sites_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/ohos/shell_services_ohos.h"
#include "components/history/core/browser/history_types.h"
#include "components/history/core/browser/top_sites.h"
#include "components/omnibox/browser/autocomplete_controller.h"
#include "components/omnibox/browser/autocomplete_controller_config.h"
#include "components/omnibox/browser/autocomplete_enums.h"
#include "components/omnibox/browser/autocomplete_input.h"
#include "components/omnibox/browser/autocomplete_match.h"
#include "components/omnibox/browser/autocomplete_match_type.h"
#include "components/omnibox/browser/autocomplete_provider.h"
#include "components/omnibox/browser/autocomplete_result.h"
#include "third_party/metrics_proto/omnibox_event.pb.h"
#include "url/gurl.h"

namespace chrome::ohos {

namespace {

constexpr char kAutocompleteResultsEvent[] = "autocompleteResults";
constexpr char kTopSitesEvent[] = "topSites";

constexpr size_t kMaxSuggestions = 10;

// Longer input is not something a person typed into an address bar; reject it
// rather than run every provider over it.
constexpr size_t kMaxInputLength = 2048;

constexpr int kDefaultTopSitesCount = 8;
constexpr int kMinTopSitesCount = 1;
constexpr int kMaxTopSitesCount = 20;

// What the shell's suggestion list can show. Deliberately not
// AutocompleteClassifier::DefaultOmniboxProviders(): that set brings keyword
// mode, zero-suggest, document and history-cluster providers whose results
// need desktop omnibox UI (chips, keyword mode, action buttons) the shell
// does not draw.
constexpr int kProviderTypes = AutocompleteProvider::TYPE_BOOKMARK |
                               AutocompleteProvider::TYPE_BUILTIN |
                               AutocompleteProvider::TYPE_HISTORY_QUICK |
                               AutocompleteProvider::TYPE_HISTORY_URL |
                               AutocompleteProvider::TYPE_SEARCH |
                               AutocompleteProvider::TYPE_SHORTCUTS |
                               AutocompleteProvider::TYPE_OPEN_TAB;

// --- Chromium API wrappers: a rename in a future milestone touches only
// these. ---------------------------------------------------------------------

std::unique_ptr<AutocompleteController> CreateController(Profile* profile) {
  AutocompleteControllerConfig config;
  config.provider_types = kProviderTypes;
  // Desktop only runs OpenTabProvider inside the @tabs scope; the shell has
  // no scopes, so open tabs are suggested alongside everything else.
  config.unscoped_open_tab_suggestions = true;
  return std::make_unique<AutocompleteController>(
      std::make_unique<ChromeAutocompleteProviderClient>(profile), config);
}

// Typed into the address bar while some page (not the NTP) is showing. The
// classifier is only consulted while the input is parsed.
AutocompleteInput MakeInput(Profile* profile, const std::u16string& text) {
  ChromeAutocompleteSchemeClassifier classifier(profile);
  return AutocompleteInput(text, metrics::OmniboxEventProto::OTHER,
                           classifier);
}

void StopController(AutocompleteController* controller) {
  controller->Stop(AutocompleteStopReason::kClobbered);
}

const char* ShellMatchType(AutocompleteMatchType::Type type) {
  switch (type) {
    case AutocompleteMatchType::SEARCH_WHAT_YOU_TYPED:
    case AutocompleteMatchType::SEARCH_HISTORY:
    case AutocompleteMatchType::SEARCH_SUGGEST:
    case AutocompleteMatchType::SEARCH_SUGGEST_ENTITY:
    case AutocompleteMatchType::SEARCH_SUGGEST_TAIL:
    case AutocompleteMatchType::SEARCH_SUGGEST_PERSONALIZED:
    case AutocompleteMatchType::SEARCH_SUGGEST_PROFILE:
    case AutocompleteMatchType::SEARCH_OTHER_ENGINE:
      return "search";
    case AutocompleteMatchType::HISTORY_URL:
    case AutocompleteMatchType::HISTORY_TITLE:
    case AutocompleteMatchType::HISTORY_BODY:
    case AutocompleteMatchType::HISTORY_KEYWORD:
      return "history";
    case AutocompleteMatchType::BOOKMARK_TITLE:
      return "bookmark";
    case AutocompleteMatchType::OPEN_TAB:
      return "openTab";
    default:
      return "url";
  }
}

// For a search the query is the headline; for a page, its title is, with the
// URL underneath -- as desktop's popup lays them out.
base::DictValue ToShellSuggestion(const AutocompleteMatch& match) {
  const std::string type = ShellMatchType(match.type);
  const bool headline_is_contents =
      type == "search" || match.description.empty();
  const std::u16string& title =
      headline_is_contents ? match.contents : match.description;
  const std::u16string& detail =
      headline_is_contents ? match.description : match.contents;

  base::DictValue item;
  item.Set("type", type);
  item.Set("title", base::UTF16ToUTF8(title));
  item.Set("url", match.destination_url.spec());
  item.Set("description", base::UTF16ToUTF8(detail));
  return item;
}

base::ListValue ToShellSuggestions(const AutocompleteResult& result) {
  base::ListValue items;
  for (const AutocompleteMatch& match : result) {
    if (items.size() >= kMaxSuggestions) {
      break;
    }
    // Pedals, messages and the like have nowhere to go when tapped.
    if (!match.destination_url.is_valid()) {
      continue;
    }
    items.Append(base::Value(ToShellSuggestion(match)));
  }
  return items;
}

base::DictValue MakeResultsEvent(int request_id,
                                 base::ListValue items,
                                 bool done) {
  base::DictValue event;
  event.Set("event", kAutocompleteResultsEvent);
  event.Set("requestId", request_id);
  event.Set("items", std::move(items));
  event.Set("done", done);
  return event;
}

// --- Per-profile state. ------------------------------------------------------

// One address bar's worth of suggestions per profile: a new request
// supersedes the previous one, as typing another letter does on desktop.
class OmniboxSession : public AutocompleteController::Observer {
 public:
  explicit OmniboxSession(Profile* profile) : profile_(profile) {}
  OmniboxSession(const OmniboxSession&) = delete;
  OmniboxSession& operator=(const OmniboxSession&) = delete;
  ~OmniboxSession() override = default;

  void Start(const ShellCommandContext& context,
             int request_id,
             const std::u16string& text) {
    if (text.empty()) {
      // Nothing typed: nothing to suggest (zero-suggest is not enabled).
      // Stopped before the new id is taken, so whatever the stop flushes is
      // labelled with the request it belongs to.
      if (controller_) {
        StopController(controller_.get());
      }
      context_ = context;
      request_id_ = request_id;
      ReplyToShell(context_, MakeResultsEvent(request_id_, {}, true));
      return;
    }
    context_ = context;
    request_id_ = request_id;
    // Start() notifies synchronously at least once, so everything above
    // must be in place first.
    Controller()->Start(MakeInput(profile_, text));
  }

  base::WeakPtr<OmniboxSession> GetWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }

  // AutocompleteController::Observer:
  void OnResultChanged(AutocompleteController* controller,
                       bool default_match_changed) override {
    ReplyToShell(context_,
                 MakeResultsEvent(request_id_,
                                  ToShellSuggestions(controller->result()),
                                  controller->done()));
  }

 private:
  // Built on first use: the providers load history and bookmark indexes that
  // a profile whose shell never types in the address bar does not need.
  AutocompleteController* Controller() {
    if (!controller_) {
      controller_ = CreateController(profile_);
      observation_.Observe(controller_.get());
    }
    return controller_.get();
  }

  const raw_ptr<Profile> profile_;
  ShellCommandContext context_;
  int request_id_ = 0;
  std::unique_ptr<AutocompleteController> controller_;
  // Declared after the controller so it stops observing before the
  // controller goes away.
  base::ScopedObservation<AutocompleteController,
                          AutocompleteController::Observer>
      observation_{this};
  base::WeakPtrFactory<OmniboxSession> weak_factory_{this};
};

PerProfile<OmniboxSession>& Sessions() {
  static base::NoDestructor<PerProfile<OmniboxSession>> sessions(
      base::BindRepeating(
          [](Profile* profile) {
            return std::make_unique<OmniboxSession>(profile);
          }));
  return *sessions;
}

// --- Top sites. --------------------------------------------------------------

int ReadTopSitesCount(const base::DictValue& command) {
  const std::optional<double> raw = command.FindDouble("count");
  if (!raw) {
    return kDefaultTopSitesCount;
  }
  if (*raw < kMinTopSitesCount || *raw > kMaxTopSitesCount) {
    LOG(WARNING) << "OHOS shell omnibox: top sites count " << *raw
                 << " clamped";
  }
  return static_cast<int>(
      std::clamp(*raw, static_cast<double>(kMinTopSitesCount),
                 static_cast<double>(kMaxTopSitesCount)));
}

void SendTopSites(const ShellCommandContext& context,
                  int request_id,
                  size_t count,
                  const history::MostVisitedURLList& urls) {
  base::ListValue items;
  for (const history::MostVisitedURL& entry : urls) {
    if (items.size() >= count) {
      break;
    }
    base::DictValue item;
    item.Set("url", entry.url.spec());
    item.Set("title", base::UTF16ToUTF8(entry.title));
    items.Append(base::Value(std::move(item)));
  }
  base::DictValue event;
  event.Set("event", kTopSitesEvent);
  event.Set("requestId", request_id);
  event.Set("items", std::move(items));
  ReplyToShell(context, std::move(event));
}

// Answers through the session's weak pointer: TopSites may answer later, after
// the profile is gone.
void OnTopSites(base::WeakPtr<OmniboxSession> session,
                ShellCommandContext context,
                int request_id,
                size_t count,
                const history::MostVisitedURLList& urls) {
  if (session) {
    SendTopSites(context, request_id, count, urls);
  }
}

// --- Commands. ---------------------------------------------------------------

// {requestId, text} -> autocompleteResults, once per result change.
void RunAutocomplete(const ShellCommandContext& context,
                     const base::DictValue& command) {
  const std::string* text = command.FindString("text");
  const int request_id = ReadRequestId(command);
  if (!text || text->size() > kMaxInputLength) {
    LOG(WARNING) << "OHOS shell omnibox: autocomplete needs text of at most "
                 << kMaxInputLength << " bytes";
    ReplyToShell(context, MakeResultsEvent(request_id, {}, true));
    return;
  }
  Sessions().Get(context.profile)->Start(context, request_id,
                                         base::UTF8ToUTF16(*text));
}

// {requestId, count?} -> topSites. Incognito has no top sites: the factory
// gives it no service, and the answer is an empty list.
void RunGetTopSites(const ShellCommandContext& context,
                    const base::DictValue& command) {
  const int request_id = ReadRequestId(command);
  const size_t count = static_cast<size_t>(ReadTopSitesCount(command));
  scoped_refptr<history::TopSites> top_sites =
      TopSitesFactory::GetForProfile(context.profile);
  if (!top_sites) {
    SendTopSites(context, request_id, count, {});
    return;
  }
  top_sites->GetMostVisitedURLs(
      base::BindOnce(&OnTopSites, Sessions().Get(context.profile)->GetWeakPtr(),
                     context, request_id, count));
}

}  // namespace

// The user has told the new tab page to stop suggesting a site. TopSites
// keeps its own blocklist, so the site stays gone across restarts without the
// shell remembering anything.
void RunRemoveTopSite(const ShellCommandContext& context,
                      const base::DictValue& command) {
  const std::string* url = command.FindString("url");
  if (!url || url->empty()) {
    return;
  }
  const GURL parsed(*url);
  scoped_refptr<history::TopSites> top_sites =
      TopSitesFactory::GetForProfile(context.profile);
  if (parsed.is_valid() && top_sites) {
    top_sites->AddBlockedUrl(parsed);
  }
}

bool HandleOmniboxCommand(const ShellCommandContext& context,
                          std::string_view name,
                          const base::DictValue& command) {
  if (name != "autocomplete" && name != "getTopSites" &&
      name != "removeTopSite") {
    return false;
  }
  if (!context.profile) {
    LOG(WARNING) << "OHOS shell omnibox: " << name << " without a profile";
    return true;
  }
  if (name == "autocomplete") {
    RunAutocomplete(context, command);
  } else if (name == "removeTopSite") {
    RunRemoveTopSite(context, command);
  } else {
    RunGetTopSites(context, command);
  }
  return true;
}

}  // namespace chrome::ohos
