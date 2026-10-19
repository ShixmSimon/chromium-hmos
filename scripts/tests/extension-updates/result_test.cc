// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "chrome/browser/ui/ohos/extension_update_result_ohos.h"

#include <cstdlib>
#include <iostream>
#include <string_view>

int main() {
  using chrome::ohos::ResolveExtensionUpdateResult;
  struct Case {
    std::string_view name;
    std::string_view status;
    bool removed;
    bool installed;
    bool permissions;
    bool pending;
    bool finished;
    std::string_view expected;
  };
  const Case cases[] = {
      {"queue acceptance is not success", "checking", false, false, false,
       false, true, "failed"},
      {"download alone is not success", "downloading", false, false, false,
       false, true, "failed"},
      {"installer callback alone is not success", "installing", false, false,
       false, false, true, "failed"},
      {"network failure is not latest", "failed", false, false, false, false,
       true, "failed"},
      {"explicit no-update result", "upToDate", false, false, false, false,
       true, "upToDate"},
      {"installed version is authoritative", "installing", false, true, false,
       false, true, "updated"},
      {"new permissions remain unapproved", "installing", false, true, true,
       false, true, "permissionsRequired"},
      {"pending install is not current", "installing", false, false, false,
       true, true, "pendingReload"},
      {"old pending update beats no-update", "upToDate", false, false, false,
       true, true, "pendingReload"},
      {"uninstall wins over late install", "installing", true, true, false,
       false, true, "failed"},
      {"uninstall wins over old pending", "installing", true, false, false,
       true, true, "failed"},
      {"unpacked remains manual", "manual", false, false, false, false, true,
       "manual"},
      {"manual reload is not updater success", "manual", false, true, false,
       false, true, "manual"},
      {"disabled already-current extension", "upToDate", false, false, true,
       false, true, "upToDate"},
      {"progress stays progress", "downloading", false, false, false, false,
       false, "downloading"},
      {"registry install can precede callback", "downloading", false, true,
       false, false, false, "updated"},
  };
  for (const auto& test : cases) {
    const auto actual = ResolveExtensionUpdateResult(
        test.status, test.removed, test.installed, test.permissions,
        test.pending, test.finished);
    if (actual != test.expected) {
      std::cerr << test.name << ": expected " << test.expected << ", got "
                << actual << '\n';
      return EXIT_FAILURE;
    }
  }
  std::cout << "PASS: 16 extension update result scenarios\n";
}
