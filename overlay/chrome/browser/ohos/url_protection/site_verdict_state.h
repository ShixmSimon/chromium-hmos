// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#ifndef CHROME_BROWSER_OHOS_URL_PROTECTION_SITE_VERDICT_STATE_H_
#define CHROME_BROWSER_OHOS_URL_PROTECTION_SITE_VERDICT_STATE_H_

#include <cstdint>
#include <string>
#include <vector>

#include "chrome/browser/ohos/url_protection/url_protection.h"

namespace chrome::ohos::url_protection {

// Navigation/result identity, independent of WebContents and SDK lifetimes so
// that races between a pending load, History API commits and policy changes can
// be tested on the CI host using the production state transitions.
class SiteVerdictState {
 public:
  struct Snapshot {
    std::string navigation_id;
    GURL url;
    int revision = 0;
    std::string verdict = "unchecked";
    double checked_at = 0;
    std::vector<std::string> matched_source_ids;
    std::string rule_type;
  };

  const Snapshot& current() const { return current_; }
  const Snapshot& committed() const { return committed_; }
  bool pending_navigation() const { return pending_navigation_; }
  uint64_t generation() const { return generation_; }

  void Initialize(const GURL& url, int revision, bool enabled) {
    current_.url = url;
    Reset(current_, revision, enabled);
    committed_ = current_;
  }
  bool IsCurrent(const std::string& id) const {
    return !id.empty() && current_.navigation_id == id;
  }
  bool Begin(const std::string& id,
             const GURL& url,
             int revision,
             bool enabled) {
    if (IsCurrent(id))
      return false;
    ++generation_;
    pending_navigation_ = true;
    current_ = {};
    current_.navigation_id = id;
    current_.url = url;
    Reset(current_, revision, enabled);
    return true;
  }
  void Redirect(const GURL& url, int revision, bool enabled) {
    ++generation_;
    current_.url = url;
    Reset(current_, revision, enabled);
  }
  void Finish(bool committed, const GURL& url) {
    ++generation_;
    pending_navigation_ = false;
    if (committed) {
      // A blocked redirect retains the actual risk target.
      if (!IsDangerous(current_))
        current_.url = url;
      committed_ = current_;
    } else {
      current_ = committed_;
    }
  }
  void SameDocumentCommit(const std::string& id,
                          const GURL& url,
                          int revision,
                          bool enabled) {
    ++generation_;
    // Full URL identity, including query and fragment: local rules can match
    // these even though the cloud provider sends only origin/path.
    if (committed_.url != url || committed_.revision != revision ||
        committed_.verdict == "checking") {
      committed_.url = url;
      Reset(committed_, revision, enabled);
    }
    committed_.navigation_id = id;
    // A History API commit in the old document must not steal the identity of
    // an in-flight cross-document navigation. Keep it for cancellation restore.
    if (!pending_navigation_)
      current_ = committed_;
  }
  void PolicyChanged(int revision,
                     bool current_enabled,
                     bool committed_enabled) {
    ++generation_;
    Reset(current_, revision, current_enabled);
    Reset(committed_, revision, committed_enabled);
  }
  void Report(const Result& result, bool checking, bool enabled, double now) {
    if (result.dangerous())
      current_.url = result.url;
    current_.matched_source_ids = result.matched_source_ids;
    current_.rule_type = result.rule_type;
    current_.checked_at = 0;
    if (!enabled || result.reason == "one_navigation_bypass") {
      current_.verdict = "skipped";
    } else if (checking && !result.dangerous()) {
      current_.verdict = "checking";
    } else {
      switch (result.verdict) {
        case Verdict::kNoMatch:
          current_.verdict = "no_match";
          break;
        case Verdict::kPhishing:
          current_.verdict = "phishing";
          break;
        case Verdict::kMalware:
          current_.verdict = "malware";
          break;
        case Verdict::kOther:
          current_.verdict = "other";
          break;
        case Verdict::kUnknown:
          current_.verdict = "unknown";
          break;
      }
      current_.checked_at = now;
    }
  }
  bool NeedsCommittedCheck() const {
    return !pending_navigation_ &&
           (current_.verdict == "unchecked" || current_.verdict == "checking");
  }
  uint64_t StartCommittedCheck() {
    ++generation_;
    current_.verdict = "checking";
    current_.checked_at = 0;
    committed_ = current_;
    return generation_;
  }
  bool CompleteCommittedCheck(uint64_t generation,
                              int revision,
                              const Result& result,
                              double now) {
    if (pending_navigation_ || generation != generation_ ||
        revision != current_.revision || result.url != current_.url)
      return false;
    Report(result, false, true, now);
    committed_ = current_;
    return true;
  }

 private:
  static bool IsDangerous(const Snapshot& snapshot) {
    return snapshot.verdict == "phishing" || snapshot.verdict == "malware" ||
           !snapshot.matched_source_ids.empty();
  }
  static void Reset(Snapshot& snapshot, int revision, bool enabled) {
    snapshot.revision = revision;
    snapshot.verdict = enabled ? "unchecked" : "skipped";
    snapshot.checked_at = 0;
    snapshot.matched_source_ids.clear();
    snapshot.rule_type.clear();
  }
  Snapshot current_;
  Snapshot committed_;
  bool pending_navigation_ = false;
  uint64_t generation_ = 0;
};

}  // namespace chrome::ohos::url_protection
#endif
