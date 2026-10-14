// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/task/sequenced_task_runner.h"
#include "media/base/audio_decoder.h"
#include "media/base/cdm_factory.h"
#include "media/base/media_log.h"
#include "media/base/provision_fetcher.h"
#include "media/base/supported_audio_decoder_config.h"
#include "media/base/supported_video_decoder_config.h"
#include "media/base/video_decoder.h"
#include "media/gpu/ohos/ohos_audio_decoder.h"
#include "media/gpu/ohos/ohos_cdm.h"
#include "media/gpu/ohos/ohos_codec_util.h"
#include "media/gpu/ohos/ohos_video_decoder.h"
#include "media/mojo/mojom/frame_interface_factory.mojom.h"
#include "media/mojo/services/gpu_mojo_media_client.h"
#include "media/mojo/services/mojo_provision_fetcher.h"
#include "mojo/public/cpp/bindings/pending_remote.h"

namespace media {

namespace {

// Device certificates are downloaded by the browser process for the frame
// that asked for the CDM, as for Android's MediaDrmBridge.
std::unique_ptr<ProvisionFetcher> CreateProvisionFetcher(
    mojom::FrameInterfaceFactory* frame_interfaces) {
  mojo::PendingRemote<mojom::ProvisionFetcher> provision_fetcher;
  frame_interfaces->CreateProvisionFetcher(
      provision_fetcher.InitWithNewPipeAndPassReceiver());
  return std::make_unique<MojoProvisionFetcher>(std::move(provision_fetcher));
}

class OhosCdmFactory final : public CdmFactory {
 public:
  explicit OhosCdmFactory(CreateFetcherCB create_fetcher_cb)
      : create_fetcher_cb_(std::move(create_fetcher_cb)) {}
  ~OhosCdmFactory() final = default;

  void Create(const CdmConfig& cdm_config,
              const SessionMessageCB& session_message_cb,
              const SessionClosedCB& session_closed_cb,
              const SessionKeysChangeCB& session_keys_change_cb,
              const SessionExpirationUpdateCB& session_expiration_update_cb,
              CdmCreatedCB cdm_created_cb) final {
    OhosCdm::Create(
        cdm_config, create_fetcher_cb_, session_message_cb, session_closed_cb,
        session_keys_change_cb, session_expiration_update_cb,
        base::BindOnce(
            [](CdmCreatedCB cdm_created_cb, scoped_refptr<OhosCdm> cdm,
               const std::string& error_message) {
              if (!cdm) {
                LOG(WARNING) << "OHOS CDM: not created: " << error_message;
              }
              const CreateCdmStatus status =
                  cdm ? CreateCdmStatus::kSuccess
                      : CreateCdmStatus::kInitCdmFailed;
              std::move(cdm_created_cb).Run(std::move(cdm), status);
            },
            std::move(cdm_created_cb)));
  }

 private:
  const CreateFetcherCB create_fetcher_cb_;
};

}  // namespace

class GpuMojoMediaClientOhos final : public GpuMojoMediaClient {
 public:
  explicit GpuMojoMediaClientOhos(GpuMojoMediaClientTraits& traits)
      : GpuMojoMediaClient(traits) {}
  ~GpuMojoMediaClientOhos() final = default;

 protected:
  std::unique_ptr<VideoDecoder> CreatePlatformVideoDecoder(
      VideoDecoderTraits& traits) final {
    if (!CanUseOhosNativeCodecs()) {
      return nullptr;
    }
    // The configs are cached by GpuMojoMediaClient, so every decoder checks
    // its config against the same capability snapshot the renderer saw.
    return std::make_unique<OhosVideoDecoder>(
        traits.task_runner, std::move(traits.media_log),
        traits.get_cached_configs_cb.Run(), gpu_task_runner_,
        traits.get_command_buffer_stub_cb, gpu_workarounds_);
  }

  std::optional<SupportedVideoDecoderConfigs>
  GetPlatformSupportedVideoDecoderConfigs() final {
    return GetOhosSupportedDecoderConfigs();
  }

  VideoDecoderType GetPlatformDecoderImplementationType() final {
    // Matches OhosVideoDecoder::GetDecoderType(); see the note there.
    return VideoDecoderType::kUnknown;
  }

  // The renderer asks this once when it starts and answers canPlayType and
  // MSE for AC-3 and DTS from it; it decodes everything else itself.
  std::optional<SupportedAudioDecoderConfigs>
  GetPlatformSupportedAudioDecoderConfigs() final {
    return GetOhosSupportedAudioDecoderConfigs();
  }

  std::unique_ptr<AudioDecoder> CreatePlatformAudioDecoder(
      scoped_refptr<base::SequencedTaskRunner> task_runner,
      std::unique_ptr<MediaLog> media_log) final {
    if (!CanUseOhosNativeCodecs()) {
      return nullptr;
    }
    return std::make_unique<OhosAudioDecoder>(std::move(task_runner),
                                              std::move(media_log));
  }

  // WisePlay through DRM Kit; the AVCodecKit decoders above decrypt with it.
  std::unique_ptr<CdmFactory> CreatePlatformCdmFactory(
      mojom::FrameInterfaceFactory* frame_interfaces) final {
    // This CDM depends on AVCodecKit to decrypt protected samples.
    if (!CanUseOhosNativeCodecs()) {
      return nullptr;
    }
    return std::make_unique<OhosCdmFactory>(
        base::BindRepeating(&CreateProvisionFetcher, frame_interfaces));
  }
};

std::unique_ptr<GpuMojoMediaClient> CreateGpuMediaService(
    GpuMojoMediaClientTraits& traits) {
  return std::make_unique<GpuMojoMediaClientOhos>(traits);
}

}  // namespace media
