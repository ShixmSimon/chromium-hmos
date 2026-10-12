// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#include "chrome/browser/ohos/url_protection/site_verdict_state.h"

#include <iostream>

int RunSiteVerdictTests() {
  using namespace chrome::ohos::url_protection;
  int failures = 0;
  auto expect = [&](bool condition, const char* label) {
    if (!condition) {
      ++failures;
      std::cerr << "FAIL site verdict: " << label << '\n';
    }
  };
  const GURL a("https://example.com/watch?v=1#start");
  const GURL b("https://example.com/watch?v=2#start");
  auto result = [](const GURL& url) {
    return Result{Verdict::kNoMatch, "huawei", "no_match", url};
  };
  auto loaded = [&] {
    SiteVerdictState state;
    state.Initialize(GURL(), 1, true);
    state.Begin("1", a, 1, true);
    state.Report(result(a), false, true, 1000);
    state.Finish(true, a);
    return state;
  };
  {
    auto state = loaded();
    state.SameDocumentCommit("2", a, 1, true);
    expect(state.current().verdict == "no_match" &&
               state.current().checked_at == 1000 &&
               state.current().navigation_id == "2" &&
               !state.NeedsCommittedCheck(),
           "identical replaceState keeps verdict/time but advances navigation "
           "identity");
    expect(!state.IsCurrent("1"), "old UI navigation identity becomes stale");
  }
  for (const auto& changed : {b, GURL("https://example.com/watch?v=1#new"),
                              GURL("https://example.com/other?v=1#start")}) {
    auto state = loaded();
    state.SameDocumentCommit("2", changed, 1, true);
    expect(state.current().verdict == "unchecked" &&
               state.current().checked_at == 0 && state.NeedsCommittedCheck(),
           "query, hash and path changes do not reuse same-origin results");
    auto generation = state.StartCommittedCheck();
    expect(state.current().verdict == "checking",
           "committed URL enters checking");
    expect(!state.CompleteCommittedCheck(generation, 1, result(a), 2000),
           "wrong URL result rejected");
    expect(state.CompleteCommittedCheck(generation, 1, result(changed), 2000) &&
               state.current().verdict == "no_match" &&
               state.committed().checked_at == 2000,
           "new URL result retained in committed snapshot");
  }
  {
    auto state = loaded();
    state.SameDocumentCommit("2", b, 1, true);
    auto old = state.StartCommittedCheck();
    state.SameDocumentCommit("3", b, 1, true);
    auto latest = state.StartCommittedCheck();
    expect(!state.CompleteCommittedCheck(old, 1, result(b), 2000),
           "cancelled request rejected even for identical URL");
    expect(state.CompleteCommittedCheck(latest, 1, result(b), 3000),
           "replacement request completes");
  }
  {
    auto state = loaded();
    state.PolicyChanged(2, true, true);
    auto old = state.StartCommittedCheck();
    state.PolicyChanged(3, true, true);
    auto latest = state.StartCommittedCheck();
    expect(!state.CompleteCommittedCheck(old, 2, result(a), 2000),
           "mode change rejects previous policy result");
    expect(!state.CompleteCommittedCheck(latest, 2, result(a), 2000),
           "policy identity checked independently of request generation");
    expect(state.CompleteCommittedCheck(latest, 3, result(a), 3000),
           "current policy rechecks the already loaded page");
    state.PolicyChanged(4, false, false);
    expect(state.current().verdict == "skipped" &&
               state.current().checked_at == 0 && !state.NeedsCommittedCheck(),
           "global/site disable clears result and schedules no check");
    expect(!state.CompleteCommittedCheck(latest, 3, result(a), 4000),
           "disabled policy cannot be repainted by a late reply");
    state.PolicyChanged(5, true, true);
    expect(state.NeedsCommittedCheck(),
           "re-enable schedules a current-page check");
    auto token = state.StartCommittedCheck();
    Result unknown{Verdict::kUnknown, "huawei", "consent_required", a};
    expect(state.CompleteCommittedCheck(token, 5, unknown, 5000) &&
               state.current().verdict == "unknown",
           "unavailable SDK or absent consent never becomes no_match");
  }
  {
    auto state = loaded();
    state.PolicyChanged(2, true, true);
    auto token = state.StartCommittedCheck();
    state.Begin("4", b, 2, true);
    state.SameDocumentCommit("5", a, 2, true);
    expect(state.current().navigation_id == "4" && state.current().url == b &&
               !state.NeedsCommittedCheck(),
           "old document History API cannot steal an in-flight navigation");
    expect(!state.CompleteCommittedCheck(token, 2, result(a), 2000),
           "in-flight navigation rejects old committed-page result");
    state.Finish(false, b);
    expect(state.current().navigation_id == "5" && state.current().url == a &&
               state.NeedsCommittedCheck(),
           "aborted network navigation restores and rechecks old document");
    expect(!state.CompleteCommittedCheck(token, 2, result(a), 2000),
           "restore to same URL does not resurrect cancelled reply");
  }
  {
    auto state = loaded();
    // One-navigation bypass is preserved only for the exact checked URL.
    state.Begin("6", a, 1, true);
    Result bypass{Verdict::kUnknown, {}, "one_navigation_bypass", a};
    state.Report(bypass, false, true, 1000);
    state.Finish(true, a);
    state.SameDocumentCommit("7", a, 1, true);
    expect(state.current().verdict == "skipped" && !state.NeedsCommittedCheck(),
           "same-URL history update does not revoke a consumed bypass");
    state.SameDocumentCommit("8", b, 1, true);
    expect(state.NeedsCommittedCheck(),
           "bypass does not cover new History API URL");
    auto token = state.StartCommittedCheck();
    Result risk{Verdict::kOther, "local",    "matched", b,
                {"source"},      "url_exact"};
    expect(state.CompleteCommittedCheck(token, 1, risk, 2000) &&
               state.current().verdict == "other" &&
               state.current().matched_source_ids == risk.matched_source_ids,
           "committed subscription risk retains source and rule metadata");
  }
  {
    auto state = loaded();
    state.Begin("9", b, 1, true);
    state.Redirect(a, 1, true);
    Result risk{Verdict::kPhishing, "huawei", "matched", b};
    state.Report(risk, false, true, 2000);
    state.Finish(true, a);
    expect(state.current().url == b && state.committed().verdict == "phishing",
           "redirect warning retains actual risk target");
    state.Begin("10", b, 1, true);
    expect(state.current().verdict == "unchecked",
           "reload never reuses old snapshot");
  }
  std::cout << "Site verdict state tests: " << (failures ? "FAIL" : "PASS")
            << '\n';
  return failures;
}
