// Copyright 2026 The Chromium Authors
#include <cassert>
#include <iostream>
#include "ui/ozone/platform/ohos/ohos_rs_abi.h"
using namespace ui::ohos_overlay;
int main() {
  // An unknown library, including a one-bit change to a known build, must not
  // select a nearest-version profile or silently fall through to the old ABI.
  assert(!SelectRsAbiProfile(0));
  for (const auto& profile : kRsAbiProfiles) {
    assert(SelectRsAbiProfile(profile.render_node_create_hash) == &profile);
    for (unsigned bit = 0; bit < 32; ++bit)
      assert(
          !SelectRsAbiProfile(profile.render_node_create_hash ^ (1u << bit)));
    for (const auto& fingerprint : profile.functions)
      assert(fingerprint.bytes > 0 && fingerprint.bytes % 4 == 0 &&
             fingerprint.hash);
  }
  const auto* old = SelectRsAbiProfile(0x2d4d8cab);
  const auto* current = SelectRsAbiProfile(0x18270173);
  assert(old && current && old != current);
  // These functions changed size. Hashing the old length in the new library
  // would include the next function or omit part of RemoveChild's new body.
  assert(RsFunctionFingerprint(*old, RsCheckedFunction::kSurfaceConstructor)
             .bytes == 436);
  assert(RsFunctionFingerprint(*current, RsCheckedFunction::kSurfaceConstructor)
             .bytes == 428);
  assert(RsFunctionFingerprint(*old, RsCheckedFunction::kRemoveChild).bytes ==
         1176);
  assert(
      RsFunctionFingerprint(*current, RsCheckedFunction::kRemoveChild).bytes ==
      1468);
  for (size_t i = 0; i < current->functions.size(); ++i)
    assert(current->functions[i].hash != old->functions[i].hash);
  std::cout << "RS ABI profile regression passed\n";
}
