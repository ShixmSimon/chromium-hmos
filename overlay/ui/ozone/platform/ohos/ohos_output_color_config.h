// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef UI_OZONE_PLATFORM_OHOS_OHOS_OUTPUT_COLOR_CONFIG_H_
#define UI_OZONE_PLATFORM_OHOS_OHOS_OUTPUT_COLOR_CONFIG_H_
#include <optional>
#include "base/functional/callback.h"
#include "base/time/time.h"
#include "ui/gfx/display_color_spaces.h"
#include "ui/ozone/platform/ohos/ohos_display_brightness.h"
#include "ui/ozone/platform/ohos/ohos_output_color.h"
struct OH_NativeBuffer;
namespace ui {
// Browser-side state, populated before ChromeMain and updated by ArkTS events.
void UpdateOhosDisplayBrightness(
    std::optional<OhosDisplayBrightness> brightness);
std::optional<OhosDisplayBrightness> GetOhosDisplayBrightness();
void SetOhosDisplayBrightnessChangedCallback(base::RepeatingClosure callback);
void ApplyOhosDisplayBrightness(gfx::DisplayColorSpaces& spaces);
bool OhosDynamicOutputLuminance();
inline constexpr char kOhosOutputColorFailedMarker[] = "GpuOutputColorFailed";
// Desired mode only. Output devices must validate an actual allocation before
// publishing it through their per-output capabilities. No global HDR promise.
OhosOutputColor GetOhosRequestedOutputColor();
bool OhosSingleDisplaySupportsHdr10(uint64_t* display_id = nullptr);
// Poll at most once a second while drawing; hotplug revokes HDR on the next
// check. Display-manager IPC is not added to every 120 Hz frame.
class OhosHdrDisplayGuard {
 public:
  bool Validate();

 private:
  std::optional<uint64_t> display_id_;
  base::TimeTicks checked_;
  bool valid_ = false;
};
gfx::DisplayColorSpaces MakeOhosOutputColorSpaces(const OhosOutputColor& color);
bool SetOhosBufferOutputColor(OH_NativeBuffer* buffer,
                              const OhosOutputColor& color);
bool OhosOutputColorFailed();
void NoteOhosOutputColorFailure(const char* reason);
}  // namespace ui
#endif
