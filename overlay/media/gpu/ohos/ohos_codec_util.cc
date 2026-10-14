// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/gpu/ohos/ohos_codec_util.h"

#include <dlfcn.h>
#include <multimedia/player_framework/native_avcapability.h>
#include <multimedia/player_framework/native_avcodec_base.h>
#include <multimedia/player_framework/native_averrors.h>

#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/logging.h"
#include "base/process/fork_server_ohos.h"
#include "ui/gfx/geometry/size.h"

namespace media {

namespace {

// Profiles to assume when a capability reports none. Every AVCodecKit H.264
// hardware codec handles these three, and an empty list would otherwise hide
// a working codec.
constexpr std::array<VideoCodecProfile, 3> kFallbackH264Profiles = {
    H264PROFILE_BASELINE, H264PROFILE_MAIN, H264PROFILE_HIGH};

// These data symbols were added in API 23. Resolve them at runtime instead of
// introducing a load-time dependency that would break API 20-22 devices.
//
// Look them up in the library that defines them. The engine is loaded as a
// module with RTLD_LOCAL, so its dependencies need not be in the global scope
// that RTLD_DEFAULT searches; a miss there would hide a hardware VP9 or AV1
// decoder the device has and send every such video to software.
const char* ResolveOptionalMime(const char* symbol_name) {
  static void* const library =
      dlopen("libnative_media_codecbase.so", RTLD_NOW | RTLD_LOCAL);
  void* symbol = library ? dlsym(library, symbol_name) : nullptr;
  if (!symbol) {
    symbol = dlsym(RTLD_DEFAULT, symbol_name);
  }
  return symbol ? *static_cast<const char* const*>(symbol) : nullptr;
}

OH_AVCapability* GetHardwareCapability(VideoCodec codec, bool is_encoder) {
  if (!CanUseOhosNativeCodecs()) {
    return nullptr;
  }
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
  // Only 4:2:0 profiles are supported by our NV12/P010 output paths. Do not
  // advertise 4:2:2/4:4:4 profiles just because the codec can decode them.
  if (codec == VideoCodec::kVP9) {
    switch (profile) {
      case VP9_PROFILE_0:
        return VP9PROFILE_PROFILE0;
      case VP9_PROFILE_2:
        return VP9PROFILE_PROFILE2;
      default:
        return std::nullopt;
    }
  }
  if (codec == VideoCodec::kAV1 && profile == AV1_PROFILE_MAIN) {
    return AV1PROFILE_PROFILE_MAIN;
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

bool CanUseOhosNativeCodecs() {
  if (!base::internal::IsOhosForkChild()) {
    return true;
  }
  // Do not probe once and cache: even the first AVCodecKit query can submit
  // HiAppEvent work into inherited FFRT state. libc already runs FFRT's
  // pthread_atfork child hook; calling ffrt_child_init again is not a repair.
  // Re-enable only with a codec service that has a clean system runtime.
  // Log once per child, not on every renderer capability request.
  static const bool logged = [] {
    LOG(WARNING) << "OHOS phone fork media: AVCodecKit disabled in fork child; "
                    "using Chromium software codecs (FFRT compatibility)";
    return true;
  }();
  (void)logged;
  return false;
}

void OhosAVFormatDeleter::operator()(OH_AVFormat* format) const {
  OH_AVFormat_Destroy(format);
}

const char* OhosMimeTypeForCodec(VideoCodec codec) {
  switch (codec) {
    case VideoCodec::kH264:
      return OH_AVCODEC_MIMETYPE_VIDEO_AVC;
    case VideoCodec::kHEVC:
      return OH_AVCODEC_MIMETYPE_VIDEO_HEVC;
    case VideoCodec::kVP9: {
      static const char* const mime =
          ResolveOptionalMime("OH_AVCODEC_MIMETYPE_VIDEO_VP9");
      return mime;
    }
    case VideoCodec::kAV1: {
      static const char* const mime =
          ResolveOptionalMime("OH_AVCODEC_MIMETYPE_VIDEO_AV1");
      return mime;
    }
    default:
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
    case VP9PROFILE_PROFILE0:
      return VP9_PROFILE_0;
    case VP9PROFILE_PROFILE2:
      return VP9_PROFILE_2;
    case AV1PROFILE_PROFILE_MAIN:
      return AV1_PROFILE_MAIN;
    default:
      return std::nullopt;
  }
}

SupportedVideoDecoderConfigs GetOhosSupportedDecoderConfigs() {
  if (!CanUseOhosNativeCodecs()) {
    return {};
  }
  SupportedVideoDecoderConfigs configs;
  for (VideoCodec codec : {VideoCodec::kH264, VideoCodec::kHEVC,
                           VideoCodec::kVP9, VideoCodec::kAV1}) {
    const char* mime = OhosMimeTypeForCodec(codec);
    if (!mime) {
      LOG(WARNING) << "OHOS video capability: " << GetCodecName(codec)
                   << " MIME unavailable; software fallback";
      continue;
    }
    OH_AVCapability* capability =
        GetHardwareCapability(codec, /*is_encoder=*/false);
    if (!capability) {
      LOG(WARNING) << "OHOS video capability: " << mime
                   << " no hardware decoder; software fallback if available";
      continue;
    }
    std::optional<SizeRange> sizes = GetSizeRange(capability);
    if (!sizes) {
      LOG(WARNING) << "OHOS video capability: " << mime
                   << " invalid size range; hardware decoder omitted";
      continue;
    }
    const auto profiles = GetProfiles(capability, codec);
    LOG(WARNING) << "OHOS video capability: " << mime
                 << " hardware decoder, usable profiles=" << profiles.size()
                 << " max=" << sizes->max.ToString();
    // DRM Kit decrypts H.264 and HEVC in the decoder (OhosCdm).
    const bool allow_encrypted =
        codec == VideoCodec::kH264 || codec == VideoCodec::kHEVC;
    for (VideoCodecProfile profile : profiles) {
      configs.emplace_back(profile, profile, sizes->min, sizes->max,
                           allow_encrypted,
                           /*require_encrypted=*/false);
    }
  }
  VLOG(1) << "AVCodecKit hardware decoder configs: " << configs.size();
  return configs;
}

const char* OhosMimeTypeForAudioCodec(AudioCodec codec) {
  // AC-3 came in API 22 and DTS in API 23; resolved like VP9 and AV1 so that
  // older systems still load the engine and simply report no decoder.
  switch (codec) {
    case AudioCodec::kAC3: {
      static const char* const mime =
          ResolveOptionalMime("OH_AVCODEC_MIMETYPE_AUDIO_AC3");
      return mime;
    }
    case AudioCodec::kDTS: {
      static const char* const mime =
          ResolveOptionalMime("OH_AVCODEC_MIMETYPE_AUDIO_DTS");
      return mime;
    }
    default:
      return nullptr;
  }
}

SupportedAudioDecoderConfigs GetOhosSupportedAudioDecoderConfigs() {
  if (!CanUseOhosNativeCodecs()) {
    return {};
  }
  SupportedAudioDecoderConfigs configs;
  // E-AC-3 is left out on purpose: the system has no decoder for it, and an
  // entry here is what makes canPlayType say yes.
  for (AudioCodec codec : {AudioCodec::kAC3, AudioCodec::kDTS}) {
    const char* mime = OhosMimeTypeForAudioCodec(codec);
    // Software codecs count here, unlike video: there is no FFmpeg decoder
    // for these to prefer instead.
    OH_AVCapability* capability =
        mime ? OH_AVCodec_GetCapability(mime, /*isEncoder=*/false) : nullptr;
    LOG(WARNING) << "OHOS audio capability: " << GetCodecName(codec) << " "
                 << (capability ? "system decoder" : "none");
    if (capability) {
      configs.emplace_back(codec, AudioCodecProfile::kUnknown);
    }
  }
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
