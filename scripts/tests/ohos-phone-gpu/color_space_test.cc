// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include <cassert>
#include <cstdio>
#include <initializer_list>
#include "ui/ozone/platform/ohos/ohos_buffer_color_space.h"

int main() {
  using ui::IsOhosBufferColorReadbackValid;
  // ce4ac7d phone trace: both SDR10 and SDR8 set 11, then read back 25 with
  // native_error=0. These are different integers but SDK-equivalent spaces.
  static_assert(OH_COLORSPACE_SRGB_FULL == 11);
  static_assert(OH_COLORSPACE_DISPLAY_SRGB == 25);
  for (auto actual : {OH_COLORSPACE_SRGB_FULL, OH_COLORSPACE_DISPLAY_SRGB}) {
    assert(IsOhosBufferColorReadbackValid(0, OH_COLORSPACE_SRGB_FULL, actual));
    assert(
        IsOhosBufferColorReadbackValid(0, OH_COLORSPACE_DISPLAY_SRGB, actual));
    for (int error : {-13, 1, 7400101})
      assert(!IsOhosBufferColorReadbackValid(error, OH_COLORSPACE_SRGB_FULL,
                                             actual));
  }
  for (auto actual :
       {OH_COLORSPACE_BT2020_PQ_FULL, OH_COLORSPACE_DISPLAY_BT2020_PQ}) {
    assert(IsOhosBufferColorReadbackValid(0, OH_COLORSPACE_BT2020_PQ_FULL,
                                          actual));
    assert(IsOhosBufferColorReadbackValid(0, OH_COLORSPACE_DISPLAY_BT2020_PQ,
                                          actual));
    assert(!IsOhosBufferColorReadbackValid(-13, OH_COLORSPACE_BT2020_PQ_FULL,
                                           actual));
  }
  // Keep rejecting missing tags, different primaries/transfer/range, and
  // unsupported values. Accepting all successful reads would fail this test.
  for (auto actual :
       {OH_COLORSPACE_NONE, OH_COLORSPACE_BT709_FULL, OH_COLORSPACE_SRGB_LIMIT,
        OH_COLORSPACE_LINEAR_SRGB, OH_COLORSPACE_P3_FULL,
        OH_COLORSPACE_DISPLAY_P3_SRGB, OH_COLORSPACE_BT2020_PQ_FULL,
        OH_COLORSPACE_DISPLAY_BT2020_PQ, OH_COLORSPACE_DISPLAY_BT2020_SRGB})
    assert(!IsOhosBufferColorReadbackValid(0, OH_COLORSPACE_SRGB_FULL, actual));
  for (auto actual :
       {OH_COLORSPACE_NONE, OH_COLORSPACE_SRGB_FULL, OH_COLORSPACE_DISPLAY_SRGB,
        OH_COLORSPACE_P3_FULL, OH_COLORSPACE_BT2020_HLG_FULL,
        OH_COLORSPACE_DISPLAY_BT2020_HLG, OH_COLORSPACE_BT2020_PQ_LIMIT,
        OH_COLORSPACE_DISPLAY_BT2020_SRGB})
    assert(!IsOhosBufferColorReadbackValid(0, OH_COLORSPACE_BT2020_PQ_FULL,
                                           actual));
  assert(!IsOhosBufferColorReadbackValid(0, OH_COLORSPACE_NONE,
                                         OH_COLORSPACE_NONE));
  puts(
      "PASS SDK-equivalent SDR/HDR color readback; reject API errors and "
      "non-equivalent spaces");
}
