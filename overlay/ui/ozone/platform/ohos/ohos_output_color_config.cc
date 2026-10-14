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
#include "base/strings/string_number_conversions.h"
#include "components/viz/common/resources/shared_image_format.h"

namespace ui {
namespace {
std::atomic<bool> failed{false};
base::FilePath FailurePath() {
  auto directory = base::CommandLine::ForCurrentProcess()->GetSwitchValuePath(
      "user-data-dir");
  return directory.empty()
             ? base::FilePath()
             : directory.AppendASCII(kOhosOutputColorFailedMarker);
}
// Metadata describes the composited output envelope, not an arbitrary video's
// mastering display. The peak and reference white are explicit lab settings.
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
  if (!target || !ValidOhosOutputColor(color))
    return false;
  bool hdr = color.mode == OhosOutputMode::kHdr10;
  if (set_color(target,
                hdr ? OH_COLORSPACE_BT2020_PQ_FULL : OH_COLORSPACE_SRGB_FULL))
    return false;
  // NativeBuffer metadata type is encoded as the documented enum value. OHOS
  // consumes its first byte; use the same one-byte representation as GL.
  uint8_t type = static_cast<uint8_t>(hdr ? OH_VIDEO_HDR_HDR10 : OH_VIDEO_NONE);
  if (set_metadata(target, OH_HDR_METADATA_TYPE, sizeof(type), &type))
    return false;
  if (!hdr)
    return true;  // NONE explicitly disables stale HDR metadata.
  auto metadata = OutputMetadata(color);
  return !set_metadata(target, OH_HDR_STATIC_METADATA, sizeof(metadata),
                       reinterpret_cast<uint8_t*>(&metadata));
}
}  // namespace
bool OhosOutputColorFailed() {
  auto path = FailurePath();
  return failed.load() || (!path.empty() && base::PathExists(path));
}
void NoteOhosOutputColorFailure(const char* reason) {
  failed = true;
  auto path = FailurePath();
  if (!path.empty() && !base::WriteFile(path, "SDR8"))
    LOG(ERROR) << "OHOS output color: could not persist session fallback";
  LOG(ERROR) << "OHOS output color: " << reason
             << "; this session returns to SDR8";
}
OhosOutputColor GetOhosRequestedOutputColor() {
  auto* command = base::CommandLine::ForCurrentProcess();
  if (OhosOutputColorFailed())
    return {};
  const auto mode = command->GetSwitchValueASCII("ohos-vulkan-output");
  if (mode == "sdr10")
    return {OhosOutputMode::kSdr10};
  if (mode != "hdr10" && mode != "auto")
    return {};
  OhosOutputColor hdr{OhosOutputMode::kHdr10};
  base::StringToUint(command->GetSwitchValueASCII("ohos-hdr-white-millinits"),
                     &hdr.white_millinits);
  base::StringToUint(command->GetSwitchValueASCII("ohos-hdr-peak-millinits"),
                     &hdr.peak_millinits);
  if (ValidOhosOutputColor(hdr))
    return mode == "auto" && !OhosSingleDisplaySupportsHdr10()
               ? OhosOutputColor{OhosOutputMode::kSdr10}
               : hdr;
  LOG(WARNING) << "OHOS output color: HDR requires explicit valid "
                  "reference-white/peak lab settings";
  return mode == "auto" ? OhosOutputColor{OhosOutputMode::kSdr10}
                        : OhosOutputColor{};
}
bool OhosSingleDisplaySupportsHdr10(uint64_t* display_id) {
  NativeDisplayManager_DisplaysInfo* displays = nullptr;
  if (OH_NativeDisplayManager_CreateAllDisplays(&displays) !=
          DISPLAY_MANAGER_OK ||
      !displays)
    return false;
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
  OH_NativeBuffer_ColorSpace actual;
  return !OH_NativeBuffer_GetColorSpace(buffer, &actual) &&
         actual == (color.mode == OhosOutputMode::kHdr10
                        ? OH_COLORSPACE_BT2020_PQ_FULL
                        : OH_COLORSPACE_SRGB_FULL);
}
}  // namespace ui
