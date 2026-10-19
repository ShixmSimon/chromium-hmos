// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef UI_OZONE_PLATFORM_OHOS_OHOS_BUFFER_COLOR_POLICY_H_
#define UI_OZONE_PLATFORM_OHOS_OHOS_BUFFER_COLOR_POLICY_H_
#include "ui/ozone/platform/ohos/ohos_output_color.h"

namespace ui {
// NativeBuffer consumes an unsigned byte for the metadata type. NONE (-1)
// cannot be represented by that setter on affected systems. SDR needs only its
// color space; reuse of former HDR buffers is handled by the queue policy
// below.
template <typename SetSpace, typename SetHdr>
bool SetOhosBufferColorTags(const OhosOutputColor& color,
                            SetSpace set_space,
                            SetHdr set_hdr) {
  if (!ValidOhosOutputColor(color))
    return false;
  const bool hdr = color.mode == OhosOutputMode::kHdr10;
  return set_space(hdr) && (!hdr || set_hdr());
}

class OhosHdrBufferState {
 public:
  template <typename Clean>
  bool Prepare(const OhosOutputColor& color, bool idle, Clean clean) {
    if (color.mode == OhosOutputMode::kHdr10) {
      // Include failed/partial HDR tagging. Mark before touching the buffer.
      dirty_ = true;
      return true;
    }
    if (!dirty_)
      return true;
    if (!idle || !clean())
      return false;
    dirty_ = false;
    return true;
  }
  bool dirty() const { return dirty_; }

 private:
  bool dirty_ = false;
};
}  // namespace ui
#endif
