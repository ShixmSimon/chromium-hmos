// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <optional>
#include <utility>

#include "base/task/sequenced_task_runner.h"
#include "media/base/media_log.h"
#include "media/base/supported_video_decoder_config.h"
#include "media/base/video_decoder.h"
#include "media/gpu/ohos/ohos_codec_util.h"
#include "media/gpu/ohos/ohos_video_decoder.h"
#include "media/mojo/services/gpu_mojo_media_client.h"

namespace media {

class GpuMojoMediaClientOhos final : public GpuMojoMediaClient {
 public:
  explicit GpuMojoMediaClientOhos(GpuMojoMediaClientTraits& traits)
      : GpuMojoMediaClient(traits) {}
  ~GpuMojoMediaClientOhos() final = default;

 protected:
  std::unique_ptr<VideoDecoder> CreatePlatformVideoDecoder(
      VideoDecoderTraits& traits) final {
    // The configs are cached by GpuMojoMediaClient, so every decoder checks
    // its config against the same capability snapshot the renderer saw.
    return std::make_unique<OhosVideoDecoder>(
        traits.task_runner, std::move(traits.media_log),
        traits.get_cached_configs_cb.Run());
  }

  std::optional<SupportedVideoDecoderConfigs>
  GetPlatformSupportedVideoDecoderConfigs() final {
    return GetOhosSupportedDecoderConfigs();
  }

  VideoDecoderType GetPlatformDecoderImplementationType() final {
    // Matches OhosVideoDecoder::GetDecoderType(); see the note there.
    return VideoDecoderType::kUnknown;
  }
};

std::unique_ptr<GpuMojoMediaClient> CreateGpuMediaService(
    GpuMojoMediaClientTraits& traits) {
  return std::make_unique<GpuMojoMediaClientOhos>(traits);
}

}  // namespace media
