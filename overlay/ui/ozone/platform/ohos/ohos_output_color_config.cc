// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "ui/ozone/platform/ohos/ohos_output_color_config.h"
#include <native_buffer/native_buffer.h>
#include <native_window/external_window.h>
#include <window_manager/oh_display_manager.h>
#include <atomic>
#include "base/command_line.h"
#include "base/files/file_util.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/synchronization/lock.h"
#include "components/viz/common/resources/shared_image_format.h"
#include "ui/ozone/platform/ohos/ohos_buffer_color_policy.h"
#include "ui/ozone/platform/ohos/ohos_buffer_color_space.h"
#include "ui/ozone/platform/ohos/ohos_phone_gpu_channel.h"

namespace ui {
namespace {
struct BrightnessState {
  base::Lock lock;
  std::optional<OhosDisplayBrightness> value GUARDED_BY(lock);
  base::RepeatingClosure changed GUARDED_BY(lock);
};
BrightnessState& Brightness() {
  static base::NoDestructor<BrightnessState> state;
  return *state;
}
std::atomic<bool> failed{false};
std::atomic<bool> verified_phone_hdr{false};
std::atomic<bool> verified_phone_p3{false};
base::FilePath FailurePath() {
  auto directory = base::CommandLine::ForCurrentProcess()->GetSwitchValuePath(
      "user-data-dir");
  return directory.empty()
             ? base::FilePath()
             : directory.AppendASCII(kOhosOutputColorFailedMarker);
}
// Metadata describes the composited output envelope, not an arbitrary video's
// mastering display. Brightness comes from the system or a debug override.
OH_NativeBuffer_StaticMetadata OutputMetadata(const OhosOutputColor& color) {
  OH_NativeBuffer_StaticMetadata metadata{};
  metadata.smpte2086.displayPrimaryRed = {0.708f, 0.292f};
  metadata.smpte2086.displayPrimaryGreen = {0.170f, 0.797f};
  metadata.smpte2086.displayPrimaryBlue = {0.131f, 0.046f};
  metadata.smpte2086.whitePoint = {0.3127f, 0.3290f};
  metadata.smpte2086.maxLuminance = color.peak_millinits / 1000.f;
  metadata.smpte2086.minLuminance = 0.f;
  metadata.cta861.maxContentLightLevel = color.peak_millinits / 1000.f;
  metadata.cta861.maxFrameAverageLightLevel = color.peak_millinits / 1000.f;
  return metadata;
}
template <typename Target, typename SetColorFn, typename SetMetadataFn>
bool SetColor(Target* target,
              const OhosOutputColor& color,
              SetColorFn set_color,
              SetMetadataFn set_metadata) {
  if (!target)
    return false;
  const auto success = [&color](const char* step, int32_t result) {
    if (result)
      LOG(ERROR) << "OHOS buffer color: step=" << step
                 << " mode=" << static_cast<uint32_t>(color.mode)
                 << " native_error=" << result;
    return result == 0;
  };
  return SetOhosBufferColorTags(
      color,
      [&](bool hdr) {
        return success("color-space",
                       set_color(target, hdr ? OH_COLORSPACE_BT2020_PQ_FULL
                                             : (color.mode == OhosOutputMode::kP3Sdr10
                                                    ? OH_COLORSPACE_P3_FULL
                                                    : OH_COLORSPACE_SRGB_FULL)));
      },
      [&] {
        uint8_t type = OH_VIDEO_HDR_HDR10;
        if (!success("hdr-type", set_metadata(target, OH_HDR_METADATA_TYPE,
                                              sizeof(type), &type)))
          return false;
        auto metadata = OutputMetadata(color);
        return success(
            "hdr-static",
            set_metadata(target, OH_HDR_STATIC_METADATA, sizeof(metadata),
                         reinterpret_cast<uint8_t*>(&metadata)));
      });
}
bool SingleDisplaySupportsHdr10(uint64_t* display_id, bool log_details) {
  NativeDisplayManager_DisplaysInfo* displays = nullptr;
  const auto result = OH_NativeDisplayManager_CreateAllDisplays(&displays);
  if (log_details) {
    LOG(WARNING) << "OHOS HDR display probe: result=" << result
                 << " count=" << (displays ? displays->displaysLength : 0)
                 << " has_infos=" << (displays && displays->displaysInfo);
  }
  if (result != DISPLAY_MANAGER_OK || !displays)
    return false;
  if (log_details && displays->displaysInfo) {
    for (uint32_t i = 0; i < displays->displaysLength; ++i) {
      const auto& info = displays->displaysInfo[i];
      std::string formats;
      if (info.hdrFormat && info.hdrFormat->hdrFormats) {
        for (uint32_t j = 0; j < info.hdrFormat->hdrFormatLength; ++j) {
          formats += " " + base::NumberToString(info.hdrFormat->hdrFormats[j]);
        }
      }
      LOG(WARNING) << "OHOS HDR display probe: id=" << info.id
                   << " state=" << info.state << " formats=[" << formats
                   << " ]";
    }
  }
  bool supported = false;
  // No permission-heavy enumeration of other apps' windows. Multiple displays
  // remain SDR until per-window display IDs can be verified by the shell.
  if (displays->displaysLength == 1 && displays->displaysInfo) {
    if (display_id)
      *display_id = displays->displaysInfo[0].id;
    const auto* formats = displays->displaysInfo[0].hdrFormat;
    if (formats && formats->hdrFormats) {
      for (uint32_t i = 0; i < formats->hdrFormatLength; ++i)
        supported |=
            formats->hdrFormats[i] == 2;  // hdrCapability.HDRFormat.VIDEO_HDR10
    }
  }
  OH_NativeDisplayManager_DestroyAllDisplays(displays);
  return supported;
}
}  // namespace
void UpdateOhosDisplayBrightness(std::optional<OhosDisplayBrightness> value) {
  base::RepeatingClosure changed;
  {
    base::AutoLock lock(Brightness().lock);
    if (Brightness().value == value)
      return;
    Brightness().value = value;
    changed = Brightness().changed;
  }
  if (value) {
    LOG(WARNING) << "OHOS brightness: display=" << value->display_id
                 << " white_nits=" << value->hdr.white_millinits / 1000.0
                 << " peak_nits=" << value->hdr.peak_millinits / 1000.0
                 << " current_headroom="
                 << static_cast<double>(value->hdr.peak_millinits) /
                        value->hdr.white_millinits
                 << " max_headroom=" << value->max_headroom
                 << " content_peak_millinits="
                 << OhosContentOutputColor(*value).peak_millinits;
  }
  if (changed)
    changed.Run();
}
std::optional<OhosDisplayBrightness> GetOhosDisplayBrightness() {
  base::AutoLock lock(Brightness().lock);
  return Brightness().value;
}
void SetOhosDisplayBrightnessChangedCallback(base::RepeatingClosure callback) {
  base::AutoLock lock(Brightness().lock);
  Brightness().changed = std::move(callback);
}
void SetOhosVerifiedPhoneHdrOutput(bool available) {
  if (!available)
    SetOhosVerifiedPhoneP3Output(false);
  if (verified_phone_hdr.exchange(available) == available)
    return;
  base::RepeatingClosure changed;
  {
    base::AutoLock lock(Brightness().lock);
    changed = Brightness().changed;
  }
  LOG(WARNING) << "OHOS phone HDR capability: verified=" << available;
  if (changed)
    changed.Run();
}
bool HasOhosVerifiedPhoneHdrOutput() {
  return verified_phone_hdr.load();
}
bool OhosSingleDisplaySupportsP3() {
  NativeDisplayManager_DisplaysInfo* displays = nullptr;
  if (OH_NativeDisplayManager_CreateAllDisplays(&displays) != DISPLAY_MANAGER_OK ||
      !displays)
    return false;
  bool supported = false;
  if (displays->displaysLength == 1 && displays->displaysInfo) {
    const auto* spaces = displays->displaysInfo[0].colorSpace;
    if (spaces && spaces->colorSpaces) {
      for (uint32_t i = 0; i < spaces->colorSpaceLength; ++i) {
        // Native display-manager values use DM_ColorSpace, which differs from
        // both colorSpaceManager.ColorSpace and NativeBuffer's color enum.
        // DM_ColorSpace::DISPLAY_P3 = 9 (sRGB transfer, D65 white).
        LOG(WARNING) << "OHOS P3 display probe: color_space="
                     << spaces->colorSpaces[i];
        supported |= spaces->colorSpaces[i] == 9;
      }
    }
  }
  OH_NativeDisplayManager_DestroyAllDisplays(displays);
  LOG(WARNING) << "OHOS P3 display probe: supported=" << supported;
  return supported;
}
void SetOhosVerifiedPhoneP3Output(bool available) {
  if (verified_phone_p3.exchange(available) == available)
    return;
  base::RepeatingClosure changed;
  {
    base::AutoLock lock(Brightness().lock);
    changed = Brightness().changed;
  }
  LOG(WARNING) << "OHOS phone P3 capability: verified=" << available;
  if (changed)
    changed.Run();
}
bool HasOhosVerifiedPhoneP3Output() {
  return verified_phone_p3.load();
}
void ApplyOhosDisplayBrightness(gfx::DisplayColorSpaces& spaces) {
  if (auto value = GetOhosDisplayBrightness()) {
    const auto color = OhosContentOutputColor(*value);
    spaces.SetSDRMaxLuminanceNits(color.white_millinits / 1000.f);
    spaces.SetHDRMaxLuminanceRelative(
        static_cast<float>(color.peak_millinits) / color.white_millinits);
  }
}
bool OhosDynamicOutputLuminance() {
  return !base::CommandLine::ForCurrentProcess()->HasSwitch(
      "ohos-hdr-fixed-luminance");
}
bool OhosOutputColorFailed() {
  auto path = FailurePath();
  return failed.load() || (!path.empty() && base::PathExists(path));
}
void NoteOhosOutputColorFailure(const char* reason) {
  failed = true;
  SetOhosVerifiedPhoneHdrOutput(false);
  auto path = FailurePath();
  if (!path.empty() && !base::WriteFile(path, "SDR8"))
    LOG(ERROR) << "OHOS output color: could not persist session fallback";
  LOG(ERROR) << "OHOS output color: " << reason
             << "; this session returns to SDR8";
}
OhosOutputColor GetOhosRequestedOutputColor() {
  auto* command = base::CommandLine::ForCurrentProcess();
  const auto mode = command->GetSwitchValueASCII("ohos-vulkan-output");
  const auto white = command->GetSwitchValueASCII("ohos-hdr-white-millinits");
  const auto peak = command->GetSwitchValueASCII("ohos-hdr-peak-millinits");
  const auto selected = [&](OhosOutputColor color, const char* reason) {
    // Output construction only. Distinguish startup inputs from the selected
    // SDR packet, whose luminance fields are deliberately zero.
    LOG(WARNING) << "OHOS output color selection: requested=" << mode
                 << " startup_white_millinits=" << white
                 << " startup_peak_millinits=" << peak
                 << " selected=" << static_cast<uint32_t>(color.mode)
                 << " reason=" << reason;
    return color;
  };
  if (OhosOutputColorFailed())
    return selected({}, "session-fallback");
  if (mode == "sdr10")
    return selected({OhosOutputMode::kSdr10}, "explicit-sdr10");
  if (mode != "hdr10" && mode != "auto")
    return selected({}, "sdr8-or-disabled");
  OhosOutputColor hdr{OhosOutputMode::kHdr10};
  base::StringToUint(white, &hdr.white_millinits);
  base::StringToUint(peak, &hdr.peak_millinits);
  if (ValidOhosOutputColor(hdr)) {
    if (mode == "auto" &&
        !OhosSingleDisplaySupportsHdr10(nullptr, /*log_details=*/true)) {
      return selected({OhosOutputMode::kSdr10}, "display-capability-gate");
    }
    return selected(hdr, "hdr10-request");
  }
  return selected(mode == "auto" ? OhosOutputColor{OhosOutputMode::kSdr10}
                                 : OhosOutputColor{},
                  "startup-brightness-unavailable-or-invalid");
}
bool OhosSingleDisplaySupportsHdr10(uint64_t* display_id, bool log_details) {
  if (IsOhosPhoneGpuChild()) {
    uint64_t id = 0;
    const bool supported = QueryOhosPhoneHdrDisplay(&id);
    if (log_details) {
      LOG(WARNING) << "OHOS HDR display probe: via=browser supported="
                   << supported << " id=" << id;
    }
    if (supported && display_id)
      *display_id = id;
    return supported;
  }
  return SingleDisplaySupportsHdr10(display_id, log_details);
}
bool OhosHdrDisplayGuard::Validate() {
  const auto now = base::TimeTicks::Now();
  if (!checked_.is_null() && now - checked_ < base::Seconds(1))
    return valid_;
  checked_ = now;
  uint64_t id = 0;
  valid_ = OhosSingleDisplaySupportsHdr10(&id) &&
           (!display_id_ || *display_id_ == id);
  if (valid_)
    display_id_ = id;
  return valid_;
}
gfx::DisplayColorSpaces MakeOhosOutputColorSpaces(
    const OhosOutputColor& color) {
  const bool hdr = color.mode == OhosOutputMode::kHdr10;
  gfx::DisplayColorSpaces spaces(hdr ? gfx::ColorSpace::CreateHDR10()
                                     : gfx::ColorSpace::CreateSRGB());
  const auto format = color.mode == OhosOutputMode::kSdr8
                          ? viz::SinglePlaneFormat::kRGBA_8888
                          : viz::SinglePlaneFormat::kRGBA_1010102;
  // This is per output, after filtering transparent windows. Both alpha entries
  // describe this output's HDR blending space; popup outputs have their own SDR
  // caps.
  spaces.SetOutputFormats(format, format);
  if (hdr) {
    spaces.SetSDRMaxLuminanceNits(color.white_millinits / 1000.f);
    spaces.SetHDRMaxLuminanceRelative(static_cast<float>(color.peak_millinits) /
                                      color.white_millinits);
  }
  return spaces;
}
bool SetOhosBufferOutputColor(OH_NativeBuffer* buffer,
                              const OhosOutputColor& color) {
  if (!SetColor(buffer, color, OH_NativeBuffer_SetColorSpace,
                OH_NativeBuffer_SetMetadataValue))
    return false;
  const auto expected = color.mode == OhosOutputMode::kHdr10
                            ? OH_COLORSPACE_BT2020_PQ_FULL
                            : (color.mode == OhosOutputMode::kP3Sdr10
                                   ? OH_COLORSPACE_P3_FULL
                                   : OH_COLORSPACE_SRGB_FULL);
  OH_NativeBuffer_ColorSpace actual = OH_COLORSPACE_NONE;
  const int result = OH_NativeBuffer_GetColorSpace(buffer, &actual);
  if (!IsOhosBufferColorReadbackValid(result, expected, actual)) {
    LOG(ERROR) << "OHOS buffer color: step=color-space-readback mode="
               << static_cast<uint32_t>(color.mode)
               << " native_error=" << result << " expected=" << expected
               << " actual=" << actual;
    return false;
  }
  return true;
}
}  // namespace ui
