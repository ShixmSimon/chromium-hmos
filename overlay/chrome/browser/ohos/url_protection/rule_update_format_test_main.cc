// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
// Run on the Linux CI host against Python-signed fixtures. This executes the
// production C++ parser and BoringSSL verifier, not a second implementation.
#include <iostream>
#include "base/at_exit.h"
#include "base/command_line.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "chrome/browser/ohos/url_protection/rule_update_format.h"

namespace {
int failures = 0;
void Expect(bool value, std::string_view name) {
  if (!value) {
    ++failures;
    std::cerr << "FAIL: " << name << '\n';
  }
}
std::string Read(const base::FilePath& root, std::string_view name) {
  std::string value;
  Expect(base::ReadFileToString(root.AppendASCII(name), &value), name);
  return value;
}
}  // namespace
int main(int argc, char** argv) {
  base::AtExitManager exit_manager;
  base::CommandLine::Init(argc, argv);
  if (argc != 2)
    return 2;
  using namespace chrome::ohos::url_protection;
  const auto directory = base::FilePath::FromUTF8Unsafe(argv[1]);
  const auto config = ParseUpdateConfig(Read(directory, "config.json"));
  const auto payload = Read(directory, "rules.json");
  auto matrix = base::JSONReader::ReadDict(Read(directory, "cases.json"),
                                           base::JSON_PARSE_RFC);
  if (!matrix || !matrix->FindList("cases") || !matrix->FindDouble("now"))
    return 2;
  const auto now =
      base::Time::FromMillisecondsSinceUnixEpoch(*matrix->FindDouble("now"));
  for (const auto& item : *matrix->FindList("cases")) {
    const auto& row = item.GetDict();
    const auto name = *row.FindString("name");
    const auto manifest =
        VerifyRuleManifest(Read(directory, name + ".json"), config, 5, now);
    Expect(manifest.has_value() == *row.FindBool("manifest"),
           name + ": manifest");
    const bool accepted =
        manifest && VerifyRulePayload(payload, *manifest, now).has_value();
    Expect(accepted == *row.FindBool("payload"), name + ": payload");
  }
  const auto valid = Read(directory, "valid.json");
  const auto accepted_manifest = VerifyRuleManifest(valid, config, 5, now);
  if (!accepted_manifest)
    return 2;
  Expect(AcceptRuleSequence(*accepted_manifest, 10, accepted_manifest->sha256),
         "restore same authenticated payload after cache loss");
  Expect(!AcceptRuleSequence(*accepted_manifest, 10, std::string(64, 'b')),
         "same sequence cannot change payload");
  Expect(!AcceptRuleSequence(*accepted_manifest, 10, ""),
         "equal sequence needs known digest");
  Expect(!AcceptRuleSequence(*accepted_manifest, 11, accepted_manifest->sha256),
         "older sequence cannot restore");
  Expect(!VerifyRuleManifest(valid, config, 11, now),
         "persisted rollback floor");
  Expect(!VerifyRuleManifest(valid, config, 5, now + base::Days(8)),
         "expired on restart");
  Expect(!VerifyRuleManifest(valid, ParseUpdateConfig(""), 5, now),
         "no product configuration");
  Expect(!ParseUpdateConfig("{}").valid(), "invalid config");
  const auto rules = ParseLocalRules(payload, now);
  Expect(rules.Matches(GURL("https://example.com/login?a=1#one"), now),
         "exact match");
  Expect(!rules.Matches(GURL("https://example.com/login?a=1#two"), now),
         "fragment scope");
  Expect(!rules.Matches(GURL("https://example.com/login?a=3"), now),
         "query scope");
  Expect(!rules.Matches(GURL("https://sub.example.com/login?a=2"), now),
         "subdomain scope");
  Expect(!rules.Matches(GURL("http://example.com/login?a=2"), now),
         "scheme scope");
  Expect(!rules.Matches(GURL("https://example.com/login?a=2"),
                        now + base::Days(8)),
         "expired rules");
  std::string decoded_manifest, decoded_payload;
  Expect(DecodeRuleCache(EncodeRuleCache(valid, payload), decoded_manifest,
                         decoded_payload) &&
             decoded_manifest == valid && decoded_payload == payload,
         "cache round trip");
  Expect(
      !DecodeRuleCache("999999999\ninvalid", decoded_manifest, decoded_payload),
      "cache size bound");
  Expect(!DecodeRuleCache("100\nshort", decoded_manifest, decoded_payload),
         "truncated cache");
  std::cout << "Native rule validation: " << failures << " failures\n";
  return failures ? 1 : 0;
}
