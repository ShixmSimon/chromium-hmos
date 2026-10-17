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
#include "ui/gfx/native_ui_types.h"
struct OH_NativeBuffer;
namespace ui {
// Browser-side state, populated before ChromeMain and updated by ArkTS events.
void UpdateOhosDisplayBrightness(
    std::optional<OhosDisplayBrightness> brightness);
std::optional<OhosDisplayBrightness> GetOhosDisplayBrightness();
void SetOhosDisplayBrightnessChangedCallback(base::RepeatingClosure callback);
void ApplyOhosDisplayBrightness(gfx::DisplayColorSpaces& spaces);
// Browser-only capability, set after the current GPU imports an HDR probe.
// Reset on color/Vulkan failure or replacement of that GPU generation.
void SetOhosVerifiedPhoneHdrOutput(bool available);
bool HasOhosVerifiedPhoneHdrOutput();
void SetOhosVerifiedPhoneP3Output(bool available);
bool HasOhosVerifiedPhoneP3Output();
// Browser-side ordinary WSI capabilities, scoped to live opaque outputs.
// The native-child channel validates its GPU generation before updating these.
void SetOhosVerifiedWsiOutput(gfx::AcceleratedWidget widget, bool hdr, bool p3);
void ResetOhosVerifiedWsiOutputs();
bool HasOhosVerifiedWsiHdrOutput();
bool HasOhosVerifiedWsiP3Output();
// Browser-side display-service query; never called from a fork GPU.
bool OhosSingleDisplaySupportsP3();
bool OhosDynamicOutputLuminance();
inline constexpr char kOhosOutputColorFailedMarker[] = "GpuOutputColorFailed";
// Desired mode only. Output devices must validate an actual allocation before
// publishing it through their per-output capabilities and browser screen info.
OhosOutputColor GetOhosRequestedOutputColor();
// Phone fork GPU queries are brokered by the browser. Other processes retain
// the native display-service path. Details are logged only for startup probes.
bool OhosSingleDisplaySupportsHdr10(uint64_t* display_id = nullptr,
                                    bool log_details = false);
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
