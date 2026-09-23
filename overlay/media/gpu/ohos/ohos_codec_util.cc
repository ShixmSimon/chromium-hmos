// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/gpu/ohos/ohos_codec_util.h"

#include <multimedia/player_framework/native_avcapability.h>
#include <multimedia/player_framework/native_avcodec_base.h>
#include <multimedia/player_framework/native_averrors.h>

#include <algorithm>
#include <array>
#include <vector>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/logging.h"
#include "ui/gfx/geometry/size.h"

namespace media {

namespace {

// Profiles to assume when a capability reports none. Every AVCodecKit H.264
// hardware codec handles these three, and an empty list would otherwise hide
// a working codec.
constexpr std::array<VideoCodecProfile, 3> kFallbackH264Profiles = {
    H264PROFILE_BASELINE, H264PROFILE_MAIN, H264PROFILE_HIGH};

OH_AVCapability* GetHardwareCapability(VideoCodec codec, bool is_encoder) {
  const char* mime = OhosMimeTypeForCodec(codec);
  if (!mime) {
    return nullptr;
  }
  OH_AVCapability* capability =
      OH_AVCodec_GetCapabilityByCategory(mime, is_encoder, HARDWARE);
  // The category is a preference, not a filter, on some releases.
  if (!capability || !OH_AVCapability_IsHardware(capability)) {
    return nullptr;
  }
  return capability;
}

std::optional<VideoCodecProfile> OhosProfileToVideoCodecProfile(
    VideoCodec codec,
    int32_t profile) {
  if (codec == VideoCodec::kH264) {
    switch (profile) {
      case AVC_PROFILE_BASELINE:
        return H264PROFILE_BASELINE;
      case AVC_PROFILE_MAIN:
        return H264PROFILE_MAIN;
      case AVC_PROFILE_HIGH:
        return H264PROFILE_HIGH;
      default:
        return std::nullopt;
    }
  }
  if (codec == VideoCodec::kHEVC) {
    switch (profile) {
      case HEVC_PROFILE_MAIN:
        return HEVCPROFILE_MAIN;
      case HEVC_PROFILE_MAIN_10:
        return HEVCPROFILE_MAIN10;
      default:
        return std::nullopt;
    }
  }
  return std::nullopt;
}

std::vector<VideoCodecProfile> GetProfiles(OH_AVCapability* capability,
                                           VideoCodec codec) {
  std::vector<VideoCodecProfile> result;
  const int32_t* profiles = nullptr;
  uint32_t profile_count = 0;
  if (OH_AVCapability_GetSupportedProfiles(capability, &profiles,
                                           &profile_count) == AV_ERR_OK &&
      profiles) {
    // SAFETY: AVCodecKit returns an array of `profile_count` entries owned by
    // the capability, which outlives this call.
    auto profile_span = UNSAFE_BUFFERS(base::span(profiles, profile_count));
    for (int32_t profile : profile_span) {
      if (auto mapped = OhosProfileToVideoCodecProfile(codec, profile)) {
        result.push_back(*mapped);
      }
    }
  }
  if (result.empty() && codec == VideoCodec::kH264) {
    result.assign(kFallbackH264Profiles.begin(), kFallbackH264Profiles.end());
  }
  return result;
}

struct SizeRange {
  gfx::Size min;
  gfx::Size max;
};

std::optional<SizeRange> GetSizeRange(OH_AVCapability* capability) {
  OH_AVRange width = {};
  OH_AVRange height = {};
  if (OH_AVCapability_GetVideoWidthRange(capability, &width) != AV_ERR_OK ||
      OH_AVCapability_GetVideoHeightRange(capability, &height) != AV_ERR_OK ||
      width.maxVal <= 0 || height.maxVal <= 0) {
    return std::nullopt;
  }
  return SizeRange{
      gfx::Size(std::max(width.minVal, 1), std::max(height.minVal, 1)),
      gfx::Size(width.maxVal, height.maxVal)};
}

}  // namespace

void OhosAVFormatDeleter::operator()(OH_AVFormat* format) const {
  OH_AVFormat_Destroy(format);
}

const char* OhosMimeTypeForCodec(VideoCodec codec) {
  switch (codec) {
    case VideoCodec::kH264:
      return OH_AVCODEC_MIMETYPE_VIDEO_AVC;
    case VideoCodec::kHEVC:
      return OH_AVCODEC_MIMETYPE_VIDEO_HEVC;
    default:
      // VP8, VP9 and AV1 MIME types only exist from API 23, above the API 20
      // floor this port supports; the renderer's software decoders cover them.
      return nullptr;
  }
}

std::optional<std::string> GetOhosHardwareCodecName(VideoCodec codec,
                                                    bool is_encoder) {
  OH_AVCapability* capability = GetHardwareCapability(codec, is_encoder);
  if (!capability) {
    return std::nullopt;
  }
  const char* name = OH_AVCapability_GetName(capability);
  if (!name || !*name) {
    return std::nullopt;
  }
  return std::string(name);
}

std::optional<int32_t> VideoCodecProfileToOhosProfile(
    VideoCodecProfile profile) {
  switch (profile) {
    case H264PROFILE_BASELINE:
      return AVC_PROFILE_BASELINE;
    case H264PROFILE_MAIN:
      return AVC_PROFILE_MAIN;
    case H264PROFILE_HIGH:
      return AVC_PROFILE_HIGH;
    case HEVCPROFILE_MAIN:
      return HEVC_PROFILE_MAIN;
    case HEVCPROFILE_MAIN10:
      return HEVC_PROFILE_MAIN_10;
    default:
      return std::nullopt;
  }
}

SupportedVideoDecoderConfigs GetOhosSupportedDecoderConfigs() {
  SupportedVideoDecoderConfigs configs;
  for (VideoCodec codec : {VideoCodec::kH264, VideoCodec::kHEVC}) {
    OH_AVCapability* capability =
        GetHardwareCapability(codec, /*is_encoder=*/false);
    if (!capability) {
      continue;
    }
    std::optional<SizeRange> sizes = GetSizeRange(capability);
    if (!sizes) {
      continue;
    }
    for (VideoCodecProfile profile : GetProfiles(capability, codec)) {
      configs.emplace_back(profile, profile, sizes->min, sizes->max,
                           /*allow_encrypted=*/false,
                           /*require_encrypted=*/false);
    }
  }
  VLOG(1) << "AVCodecKit hardware decoder configs: " << configs.size();
  return configs;
}

VideoEncodeAccelerator::SupportedProfiles GetOhosSupportedEncoderProfiles() {
  VideoEncodeAccelerator::SupportedProfiles result;
  OH_AVCapability* capability =
      GetHardwareCapability(VideoCodec::kH264, /*is_encoder=*/true);
  if (!capability) {
    return result;
  }
  std::optional<SizeRange> sizes = GetSizeRange(capability);
  if (!sizes) {
    return result;
  }

  OH_AVRange frame_rate = {};
  uint32_t max_framerate = VideoEncodeAccelerator::kDefaultFramerate;
  if (OH_AVCapability_GetVideoFrameRateRange(capability, &frame_rate) ==
          AV_ERR_OK &&
      frame_rate.maxVal > 0) {
    max_framerate = static_cast<uint32_t>(frame_rate.maxVal);
  }

  VideoEncodeAccelerator::SupportedRateControlMode rate_modes =
      VideoEncodeAccelerator::kNoMode;
  if (OH_AVCapability_IsEncoderBitrateModeSupported(capability,
                                                    BITRATE_MODE_CBR)) {
    rate_modes |= VideoEncodeAccelerator::kConstantMode;
  }
  if (OH_AVCapability_IsEncoderBitrateModeSupported(capability,
                                                    BITRATE_MODE_VBR)) {
    rate_modes |= VideoEncodeAccelerator::kVariableMode;
  }
  if (rate_modes == VideoEncodeAccelerator::kNoMode) {
    // kNoMode marks an uninitialized profile, and every hardware encoder
    // rate-controls somehow, so assume the constant mode it defaults to.
    rate_modes = VideoEncodeAccelerator::kConstantMode;
  }

  for (VideoCodecProfile profile : GetProfiles(capability, VideoCodec::kH264)) {
    VideoEncodeAccelerator::SupportedProfile supported(
        profile, sizes->max, max_framerate, /*max_framerate_denominator=*/1,
        rate_modes);
    supported.min_resolution = sizes->min;
    result.push_back(supported);
  }
  return result;
}

}  // namespace media
