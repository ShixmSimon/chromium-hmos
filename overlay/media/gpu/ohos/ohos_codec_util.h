// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MEDIA_GPU_OHOS_OHOS_CODEC_UTIL_H_
#define MEDIA_GPU_OHOS_OHOS_CODEC_UTIL_H_

#include <multimedia/player_framework/native_avformat.h>
#include <stdint.h>

#include <memory>
#include <optional>
#include <string>

#include "media/base/audio_codecs.h"
#include "media/base/supported_audio_decoder_config.h"
#include "media/base/supported_video_decoder_config.h"
#include "media/base/video_codecs.h"
#include "media/gpu/media_gpu_export.h"
#include "media/video/video_encode_accelerator.h"

namespace media {

// AVCodecKit is unavailable in the experimental phone fork child: capability
// queries can crash its inherited FFRT runtime. Official NCP children and
// the browser retain the normal native codec path.
MEDIA_GPU_EXPORT bool CanUseOhosNativeCodecs();

struct OhosAVFormatDeleter {
  void operator()(OH_AVFormat* format) const;
};
using ScopedOhosAVFormat = std::unique_ptr<OH_AVFormat, OhosAVFormatDeleter>;

// Returns the AVCodecKit MIME type for `codec`, or nullptr when the port does
// not drive that codec through AVCodecKit.
MEDIA_GPU_EXPORT const char* OhosMimeTypeForCodec(VideoCodec codec);

// Returns the name of the hardware codec AVCodecKit offers for `codec`. Only
// hardware codecs are returned: a software AVCodecKit codec would be slower
// than FFmpeg in the renderer and would still be reported as power efficient.
MEDIA_GPU_EXPORT std::optional<std::string> GetOhosHardwareCodecName(
    VideoCodec codec,
    bool is_encoder);

// Maps a supported Chromium profile to the AVCodecKit codec-specific profile
// value, or nullopt when AVCodecKit has no equivalent.
MEDIA_GPU_EXPORT std::optional<int32_t> VideoCodecProfileToOhosProfile(
    VideoCodecProfile profile);

// Decoder configs the hardware can take, derived from AVCodecKit capability
// queries. Empty when the device has no hardware decoder.
MEDIA_GPU_EXPORT SupportedVideoDecoderConfigs GetOhosSupportedDecoderConfigs();

// Returns the AVCodecKit MIME type for `codec`, or nullptr when the system
// offers none or the port does not decode that codec through AVCodecKit.
// AC-3 and DTS only: everything else Chromium decodes itself.
MEDIA_GPU_EXPORT const char* OhosMimeTypeForAudioCodec(AudioCodec codec);

// Audio decoders the system has for codecs Chromium cannot decode itself,
// from AVCodecKit capability queries; the renderer reads this to answer
// canPlayType and MSE. Empty when there are none.
MEDIA_GPU_EXPORT SupportedAudioDecoderConfigs
GetOhosSupportedAudioDecoderConfigs();

// Encoder profiles the hardware can produce. H.264 only.
MEDIA_GPU_EXPORT VideoEncodeAccelerator::SupportedProfiles
GetOhosSupportedEncoderProfiles();

}  // namespace media

#endif  // MEDIA_GPU_OHOS_OHOS_CODEC_UTIL_H_
