// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#include "chrome/browser/ohos/url_protection/subscription_rules.h"

#include <iostream>
#include "base/files/file_util.h"
#include "base/strings/string_number_conversions.h"
#include "chrome/browser/ohos/url_protection/url_protection.h"

int RunSubscriptionTests(const base::FilePath& benchmarks) {
  using namespace chrome::ohos::url_protection;
  int failures = 0;
  auto expect = [&](bool ok, std::string_view name) {
    if (!ok) {
      ++failures;
      std::cerr << "FAIL subscription: " << name << '\n';
    }
  };
  const auto now = base::Time::Now();
  const auto suffix = ParseSubscriptionRules(
      "# comment\r\nEvil.Example.COM.\n\nevil.example.com\n", "domains",
      "host_suffix", now);
  expect(suffix.error.empty() && suffix.hashes.size() == 1 &&
             suffix.input_count == 2,
         "normalization and deduplication");
  for (const char* value :
       {"https://evil.example.com/", "http://a.evil.example.com:8080/q?x#y",
        "https://evil.example.com./"})
    expect(suffix.Matches(GURL(value), now), "suffix match");
  for (const char* value : {"https://notevil.example.com/",
                            "https://evil.example.com.attacker.test/",
                            "https://example.com/", "ftp://evil.example.com/"})
    expect(!suffix.Matches(GURL(value), now), "suffix boundary and scheme");
  const auto exact = ParseSubscriptionRules("evil.example.com\n", "domains",
                                            "host_exact", now);
  expect(exact.Matches(GURL("https://evil.example.com/a?b#c"), now),
         "host exact");
  expect(!exact.Matches(GURL("https://a.evil.example.com/"), now),
         "exact is not suffix");
  const auto idna =
      ParseSubscriptionRules("bücher.example\n", "domains", "host_suffix", now);
  expect(idna.Matches(GURL("https://xn--bcher-kva.example/"), now), "IDNA");
  for (const char* invalid :
       {"com", "co.uk", "github.io", "appspot.com", "*.example.com",
        "example.com/path", "example.com:443", "127.0.0.1", "a.local", "a..com",
        "_test.example.com", "-test.example.com", "https://example.com",
        "0.0.0.0 example.com", "||example.com^"}) {
    expect(!ParseSubscriptionRules(invalid, "domains", "host_suffix", now)
                .error.empty(),
           "reject broad, private or unsupported rules");
  }
  const auto tenant =
      ParseSubscriptionRules("tenant.github.io", "domains", "host_suffix", now);
  expect(tenant.Matches(GURL("https://tenant.github.io"), now) &&
             !tenant.Matches(GURL("https://other.github.io"), now),
         "private registry boundary");
  const auto urls = ParseSubscriptionRules(
      "https://example.com:8443/login?q=1#x\n", "urls", "url_exact", now);
  expect(urls.Matches(GURL("https://example.com:8443/login?q=1#x"), now),
         "URL exact");
  for (const char* value : {"https://example.com:8443/login?q=1#y",
                            "https://example.com:8443/login?q=2#x",
                            "http://example.com:8443/login?q=1#x",
                            "https://example.com/login?q=1#x"})
    expect(!urls.Matches(GURL(value), now), "URL scope preserved");
  expect(!urls.Matches(GURL("https://example.com:8443/login?q=1#x"),
                       now + base::Days(7)),
         "expiry");
  expect(!urls.usable(now - base::Seconds(1)), "clock rollback");
  expect(!ParseSubscriptionRules("https://example.com\nexample.com", "urls",
                                 "url_exact", now)
              .error.empty(),
         "mixed format rejected");
  expect(!ParseSubscriptionRules(std::string("a.com\n\0b.com", 12), "domains",
                                 "host_exact", now)
              .error.empty(),
         "NUL rejected");
  expect(!ParseSubscriptionRules("\xFF", "domains", "host_exact", now)
              .error.empty(),
         "invalid UTF8");
  expect(!ParseSubscriptionRules("# empty", "domains", "host_exact", now)
              .error.empty(),
         "empty rejected");
  expect(!ParseSubscriptionRules(std::string(kSubscriptionMaxBytes + 1, 'a'),
                                 "domains", "host_exact", now)
              .error.empty(),
         "byte bound");
  std::string oversized;
  oversized.reserve(6 * (kSubscriptionMaxRecords + 1));
  for (size_t i = 0; i <= kSubscriptionMaxRecords; ++i)
    oversized += "a.com\n";
  expect(
      ParseSubscriptionRules(oversized, "domains", "host_exact", now).error ==
          "record_limit",
      "record bound includes duplicates");
  for (const char* value : {"http://example.com/feed", "https://localhost/feed",
                            "https://127.0.0.1/feed", "https://[::1]/feed",
                            "https://user:pass@example.com/feed",
                            "https://example.com/feed#fragment"})
    expect(!ValidSubscriptionEndpoint(GURL(value)), "endpoint rejected");
  expect(ValidSubscriptionEndpoint(GURL("https://example.com/feed?token=x")),
         "explicit HTTPS subscription");
  Result generic{Verdict::kOther, "local", "matched",
                 GURL("https://example.com")};
  expect(!generic.dangerous(), "unclassified cloud result not reinterpreted");
  generic.matched_source_ids.push_back("source-1");
  expect(generic.dangerous(), "explicit local threat match blocks");
  for (const auto& [file, expected] : {std::pair{"hagezi.txt", 236628},
                                       std::pair{"phishdestroy.txt", 89449}}) {
    std::string raw;
    expect(base::ReadFileToString(benchmarks.AppendASCII(file), &raw),
           "read pinned full feed");
    const auto begin = base::TimeTicks::Now();
    const auto rules =
        ParseSubscriptionRules(raw, "domains", "host_suffix", now);
    const auto parsed_ms = (base::TimeTicks::Now() - begin).InMilliseconds();
    expect(rules.error.empty() &&
               rules.input_count == static_cast<size_t>(expected),
           "complete full feed without truncation");
    const auto lookups = base::TimeTicks::Now();
    for (int i = 0; i < 10000; ++i)
      rules.Matches(GURL("https://a.b.c.example.com/test?x#y"), now);
    std::cout << "Subscription benchmark " << file
              << ": input=" << rules.input_count
              << " unique=" << rules.hashes.size()
              << " raw_bytes=" << raw.size()
              << " index_bytes=" << rules.hashes.capacity() * 32
              << " parse_ms=" << parsed_ms << " lookup_10000_ms="
              << (base::TimeTicks::Now() - lookups).InMilliseconds()
              << " error=" << rules.error << '\n';
  }
  std::cout << "Subscription validation: " << failures << " failures\n";
  return failures;
}
