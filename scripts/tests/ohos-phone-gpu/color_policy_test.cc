// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include <cassert>
#include <cstdio>
#include <initializer_list>
#include "ui/ozone/platform/ohos/ohos_buffer_color_policy.h"

int main() {
  using namespace ui;
  const OhosOutputColor hdr{OhosOutputMode::kHdr10, 203000, 1000000};
  const OhosOutputColor sdr10{OhosOutputMode::kSdr10};
  const OhosOutputColor sdr8;
  // SDR/HDR transitions cannot create capabilities that were never probed,
  // upgrade an SDR8 fallback, or accept malformed luminance metadata.
  assert(CanSwitchOhosPhoneOutputColor(hdr, sdr10, true));
  assert(CanSwitchOhosPhoneOutputColor(sdr10, hdr, true));
  assert(!CanSwitchOhosPhoneOutputColor(sdr10, hdr, false));
  assert(!CanSwitchOhosPhoneOutputColor(sdr8, hdr, true));
  assert(!CanSwitchOhosPhoneOutputColor(hdr, sdr8, true));
  assert(!CanSwitchOhosPhoneOutputColor(hdr, hdr, true));
  assert(!CanSwitchOhosPhoneOutputColor(
      sdr10, {OhosOutputMode::kHdr10, 0, 1000000}, true));
  int metadata_calls = 0;
  // Model a device rejecting HDR metadata: both SDR formats must still work.
  auto reject_metadata = [&] { ++metadata_calls; return false; };
  for (auto color : {sdr8, sdr10}) {
    assert(SetOhosBufferColorTags(color, [](bool pq) { return !pq; },
                                  reject_metadata));
    assert(metadata_calls == 0);
  }
  assert(!SetOhosBufferColorTags(hdr, [](bool pq) { return pq; },
                                 reject_metadata));
  assert(metadata_calls == 1);
  // A failed color-space tag must not be reported as successful SDR/HDR.
  assert(!SetOhosBufferColorTags(sdr8, [](bool) { return false; },
                                 reject_metadata));
  assert(metadata_calls == 1);
  assert(SetOhosBufferColorTags(hdr, [](bool pq) { return pq; },
                                [] { return true; }));

  OhosHdrBufferState state;
  int clean_calls = 0;
  auto clean = [&] { ++clean_calls; return true; };
  assert(state.Prepare(sdr10, true, clean));
  assert(state.Prepare(sdr8, true, clean));
  assert(clean_calls == 0);  // Failed SDR10 probe does not poison SDR8.
  assert(state.Prepare(hdr, true, clean));
  assert(state.dirty());  // Also covers a failed/partial HDR metadata write.
  assert(!state.Prepare(sdr8, false, clean));
  assert(clean_calls == 0 && state.dirty());  // In-flight lease cannot be cleaned.
  assert(!state.Prepare(sdr8, true, [] { return false; }));
  assert(state.dirty());  // Never relabel stale HDR as safe SDR after failure.
  assert(state.Prepare(sdr8, true, clean));
  assert(clean_calls == 1 && !state.dirty());
  for (int frame = 0; frame < 120; ++frame)
    assert(state.Prepare(sdr8, true, clean));
  assert(clean_calls == 1);  // No per-frame queue rebuild.
  assert(state.Prepare(hdr, true, clean));
  assert(state.Prepare(hdr, false, clean));
  assert(clean_calls == 1);  // Brightness changes keep HDR buffers.
  assert(state.Prepare(sdr10, true, clean));
  assert(clean_calls == 2 && !state.dirty());
  puts("PASS phone SDR metadata fallback, HDR retirement and steady-state reuse");
}
