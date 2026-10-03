// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_BASE_IME_OHOS_TEXT_INPUT_ACTION_H_
#define UI_BASE_IME_OHOS_TEXT_INPUT_ACTION_H_

#include "ui/base/ime/text_input_action.h"

namespace ui {

// A field's enterkeyhint, for the HarmonyOS IME's enter key.
//
// TextInputClient has no way to report it -- Android reads it from the
// TextInputState directly -- and adding a method there rebuilds everything
// that includes the header. On OHOS RenderWidgetHostViewAura's
// GetTextInputFlags() carries it above the real flags, which end at 1 << 14,
// and OhosInputMethod reads it back.
inline constexpr int kOhosTextInputActionShift = 24;
inline constexpr int kOhosTextInputActionMask = 0xf
                                                << kOhosTextInputActionShift;

inline int PackOhosTextInputAction(TextInputAction action) {
  return static_cast<int>(action) << kOhosTextInputActionShift;
}

inline TextInputAction UnpackOhosTextInputAction(int flags) {
  const int value =
      (flags & kOhosTextInputActionMask) >> kOhosTextInputActionShift;
  return value <= static_cast<int>(TextInputAction::kMaxValue)
             ? static_cast<TextInputAction>(value)
             : TextInputAction::kDefault;
}

}  // namespace ui

#endif  // UI_BASE_IME_OHOS_TEXT_INPUT_ACTION_H_
