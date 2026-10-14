// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/gpu/ohos/ohos_video_decoder.h"

#include <multimedia/player_framework/native_avbuffer.h>
#include <multimedia/player_framework/native_avcodec_videodecoder.h>
#include <multimedia/player_framework/native_averrors.h>
#include <multimedia/player_framework/native_avformat.h>
#include <native_buffer/native_buffer.h>

#include <poll.h>
#include <sys/socket.h>
#include <algorithm>
#include <atomic>
#include <limits>
#include <string>
#include <utility>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/feature_list.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/functional/callback_helpers.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/memory/read_only_shared_memory_region.h"
#include "base/numerics/checked_math.h"
#include "base/posix/eintr_wrapper.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/bind_post_task.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "base/trace_event/trace_event.h"
#include "gpu/command_buffer/client/client_shared_image.h"
#include "gpu/command_buffer/client/shared_image_interface.h"
#include "media/base/decrypt_config.h"
#include "media/base/format_utils.h"
#include "media/base/video_codecs.h"
#include "media/base/video_frame.h"
#include "media/base/video_types.h"
#include "media/base/waiting.h"
#include "media/gpu/ohos/ohos_cenc_info.h"
#include "media/gpu/ohos/ohos_codec_util.h"
#include "third_party/libyuv/include/libyuv/convert.h"
#include "third_party/libyuv/include/libyuv/planar_functions.h"
#include "ui/gfx/color_space.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/gpu_fence.h"
#include "ui/ozone/platform/ohos/ohos_native_pixmap.h"

namespace media {

namespace {

// Decode callbacks complete as soon as a buffer is inside the codec, so this
// only bounds how far the demuxer can run ahead of free codec input slots.
constexpr int kMaxDecodeRequests = 4;
constexpr size_t kMaxSurfaceFramesInFlight = 4;

BASE_FEATURE(kOhosZeroCopyVideo,
             "OhosZeroCopyVideo",
             base::FEATURE_ENABLED_BY_DEFAULT);

int32_t ReadIntOr(OH_AVFormat* format, const char* key, int32_t fallback) {
  int32_t value = 0;
  if (format && OH_AVFormat_GetIntValue(format, key, &value) && value > 0) {
    return value;
  }
  return fallback;
}

// A key's value as reported, zero included; -1 when it was not reported.
int32_t ReadIntOrMissing(OH_AVFormat* format, const char* key) {
  int32_t value = 0;
  return format && OH_AVFormat_GetIntValue(format, key, &value) ? value : -1;
}

bool RequiresP010(VideoCodecProfile profile) {
  return profile == HEVCPROFILE_MAIN10 || profile == VP9PROFILE_PROFILE2;
}

}  // namespace

struct OhosVideoDecoder::CallbackRelay {
  CallbackRelay(scoped_refptr<base::SequencedTaskRunner> task_runner,
                base::WeakPtr<OhosVideoDecoder> decoder,
                uint32_t generation)
      : task_runner(std::move(task_runner)),
        decoder(std::move(decoder)),
        generation(generation) {}

  const scoped_refptr<base::SequencedTaskRunner> task_runner;
  // Only dereferenced on `task_runner`; copying it on codec threads is safe.
  const base::WeakPtr<OhosVideoDecoder> decoder;
  // Mirrors OhosVideoDecoder::generation_ so codec threads can read it.
  std::atomic<uint32_t> generation;
};

OhosVideoDecoder::PendingDecode::PendingDecode(
    scoped_refptr<DecoderBuffer> buffer,
    DecodeCB decode_cb)
    : buffer(std::move(buffer)), decode_cb(std::move(decode_cb)) {}
OhosVideoDecoder::PendingDecode::PendingDecode(PendingDecode&&) = default;
OhosVideoDecoder::PendingDecode& OhosVideoDecoder::PendingDecode::operator=(
    PendingDecode&&) = default;
OhosVideoDecoder::PendingDecode::~PendingDecode() = default;

void OhosVideoDecoder::CodecDeleter::operator()(OH_AVCodec* codec) const {
  OH_VideoDecoder_Destroy(codec);
}

OhosVideoDecoder::OhosVideoDecoder(
    scoped_refptr<base::SequencedTaskRunner> task_runner,
    std::unique_ptr<MediaLog> media_log,
    SupportedVideoDecoderConfigs supported_configs,
    scoped_refptr<base::SequencedTaskRunner> gpu_task_runner,
    OhosVideoFrameConverter::GetCommandBufferStubCB get_stub_cb,
    const gpu::GpuDriverBugWorkarounds& workarounds,
    OhosGetSharedImageInterfaceCB get_browser_sii)
    : task_runner_(std::move(task_runner)),
      media_log_(std::move(media_log)),
      supported_configs_(std::move(supported_configs)),
      gpu_task_runner_(std::move(gpu_task_runner)),
      frame_converter_(gpu_task_runner_
                           ? base::MakeRefCounted<OhosVideoFrameConverter>(
                                 gpu_task_runner_,
                                 std::move(get_stub_cb),
                                 workarounds)
                           : nullptr),
      get_browser_sii_(std::move(get_browser_sii)) {
  DETACH_FROM_SEQUENCE(sequence_checker_);
}

OhosVideoDecoder::~OhosVideoDecoder() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  AbortPendingDecodes(DecoderStatus::Codes::kAborted);
  DestroyCodec();
}

void OhosVideoDecoder::Initialize(const VideoDecoderConfig& config,
                                  bool low_delay,
                                  CdmContext* cdm_context,
                                  InitCB init_cb,
                                  const OutputCB& output_cb,
                                  const WaitingCB& waiting_cb) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  InitCB bound_init_cb = base::BindPostTaskToCurrentDefault(std::move(init_cb));

  if (!config.IsValidConfig()) {
    std::move(bound_init_cb).Run(DecoderStatus::Codes::kUnsupportedConfig);
    return;
  }
  // An encrypted stream needs a DRM Kit CDM: the codec decrypts with its
  // session. H.264 and HEVC only; DRM Kit decrypts nothing else.
  if (config.is_encrypted() &&
      (!cdm_context || !cdm_context->GetOhosMediaKeySession() ||
       (config.codec() != VideoCodec::kH264 &&
        config.codec() != VideoCodec::kHEVC))) {
    std::move(bound_init_cb)
        .Run(DecoderStatus::Codes::kUnsupportedEncryptionMode);
    return;
  }
  // WebM alpha is separate auxiliary data; AVCodecKit's NV12/P010 output
  // cannot preserve it. Let Chromium select its software decoder instead.
  if (config.alpha_mode() != VideoDecoderConfig::AlphaMode::kIsOpaque) {
    std::move(bound_init_cb).Run(DecoderStatus::Codes::kUnsupportedConfig);
    return;
  }
  if (!std::ranges::any_of(supported_configs_,
                           [&config](const SupportedVideoDecoderConfig& s) {
                             return s.Matches(config);
                           })) {
    std::move(bound_init_cb).Run(DecoderStatus::Codes::kUnsupportedConfig);
    return;
  }

  // Chromium re-initializes on a mid-stream config change. AVCodecKit only
  // takes new dimensions through Reset() plus Configure(), so a fresh codec
  // is the simpler path to the same state.
  AbortPendingDecodes(DecoderStatus::Codes::kAborted);
  DestroyCodec();
  config_ = config;
  output_cb_ = output_cb;
  waiting_cb_ = waiting_cb;
  output_layout_.reset();
  surface_frame_count_ = 0;
  buffer_frame_logged_ = false;
  waiting_for_key_ = false;
  cdm_event_registration_.reset();
  cdm_context_ = config.is_encrypted() ? cdm_context : nullptr;
  if (cdm_context_) {
    // CdmContext posts the event back to this sequence.
    cdm_event_registration_ = cdm_context_->RegisterEventCB(
        base::BindRepeating(&OhosVideoDecoder::OnCdmEvent,
                            weak_factory_.GetWeakPtr()));
  }

  state_ = State::kUninitialized;
  surface_enabled_ = false;
  const bool eligible = config_.codec() == VideoCodec::kH264 ||
                        config_.profile() == HEVCPROFILE_MAIN ||
                        config_.profile() == HEVCPROFILE_MAIN10 ||
                        config_.profile() == VP9PROFILE_PROFILE0 ||
                        config_.profile() == VP9PROFILE_PROFILE2 ||
                        config_.profile() == AV1PROFILE_PROFILE_MAIN;
  // Protected Vulkan memory/import is not enabled on the phone GPU device.
  if (get_browser_sii_ && base::FeatureList::IsEnabled(kOhosZeroCopyVideo) &&
      eligible && !config_.is_encrypted()) {
    get_browser_sii_.Run(base::BindPostTask(
        task_runner_, base::BindOnce(&OhosVideoDecoder::OnBrowserGpuInitialized,
                                     weak_factory_.GetWeakPtr(), generation_,
                                     std::move(bound_init_cb))));
    return;
  }
  if (frame_converter_ &&
      base::FeatureList::IsEnabled(kOhosZeroCopyVideo) && eligible) {
    gpu_task_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(
            &OhosVideoFrameConverter::Initialize, frame_converter_,
            // AV1 Main can be either 8 or 10 bit. Require P010 import support
            // up front; select the actual frame format from each NativeBuffer.
            (RequiresP010(config_.profile()) ||
             config_.codec() == VideoCodec::kAV1)
                ? viz::MultiPlaneFormat::kP010
                : viz::MultiPlaneFormat::kNV12,
            config_.is_encrypted(),
            base::BindPostTask(
                task_runner_,
                base::BindOnce(&OhosVideoDecoder::OnGpuInitialized,
                               weak_factory_.GetWeakPtr(), generation_,
                               std::move(bound_init_cb)))));
    return;
  }
  if (base::FeatureList::IsEnabled(kOhosZeroCopyVideo)) {
    LOG(WARNING) << "OHOS video zero-copy: "
                 << (frame_converter_ ? "profile ineligible" : "browser media")
                 << "; buffer mode";
  }
  FinishInitialize(std::move(bound_init_cb));
}

void OhosVideoDecoder::OnGpuInitialized(uint32_t generation,
                                        InitCB init_cb,
                                        bool supported) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (generation != generation_) {
    std::move(init_cb).Run(DecoderStatus::Codes::kAborted);
    return;
  }
  surface_enabled_ = supported;
  if (!supported) {
    LOG(WARNING)
        << "OHOS video zero-copy: GPU import/fences unavailable; buffer mode";
  }
  FinishInitialize(std::move(init_cb));
}

void OhosVideoDecoder::OnBrowserGpuInitialized(
    uint32_t generation,
    InitCB init_cb,
    scoped_refptr<gpu::SharedImageInterface> sii) {
  if (generation != generation_) {
    std::move(init_cb).Run(DecoderStatus::Codes::kAborted);
    return;
  }
  browser_sii_ = std::move(sii);
  surface_enabled_ =
      browser_sii_ && !browser_sii_->IsLost() &&
      browser_sii_->GetCapabilities().supports_ohos_video_native_buffers;
  LOG(WARNING) << "OHOS phone video: Vulkan native-buffer path="
               << surface_enabled_;
  FinishInitialize(std::move(init_cb));
}

void OhosVideoDecoder::FinishInitialize(InitCB init_cb) {
  DecoderStatus status = CreateCodec();
  if (!status.is_ok() && surface_enabled_) {
    // No compressed input has been consumed; retry safely with the old path.
    LOG(WARNING)
        << "OHOS video zero-copy: surface initialization failed; buffer mode: "
        << status.message();
    DestroyCodec();
    surface_enabled_ = false;
    status = CreateCodec();
  }
  if (!status.is_ok()) {
    MEDIA_LOG(ERROR, media_log_)
        << "AVCodecKit decoder creation failed: " << status.message();
    LOG(WARNING) << "OHOS video decoder: " << GetProfileName(config_.profile())
                 << " hardware initialization failed; trying next decoder: "
                 << status.message();
    DestroyCodec();
    state_ = State::kUninitialized;
    std::move(init_cb).Run(std::move(status));
    return;
  }
  state_ = State::kDecoding;
  std::move(init_cb).Run(DecoderStatus::Codes::kOk);
}

void OhosVideoDecoder::Decode(scoped_refptr<DecoderBuffer> buffer,
                              DecodeCB decode_cb) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DecodeCB bound_decode_cb =
      base::BindPostTaskToCurrentDefault(std::move(decode_cb));
  if (state_ == State::kError || state_ == State::kUninitialized) {
    std::move(bound_decode_cb)
        .Run(state_ == State::kError
                 ? DecoderStatus::Codes::kPlatformDecodeFailure
                 : DecoderStatus::Codes::kNotInitialized);
    return;
  }
  pending_decodes_.emplace_back(std::move(buffer), std::move(bound_decode_cb));
  PumpInput();
}

void OhosVideoDecoder::Reset(base::OnceClosure closure) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  AbortPendingDecodes(DecoderStatus::Codes::kAborted);
  waiting_for_key_ = false;
  if (state_ == State::kDecoding || state_ == State::kDraining) {
    if (FlushAndRestartCodec()) {
      state_ = State::kDecoding;
    } else {
      EnterErrorState("flush on reset failed");
    }
  }
  task_runner_->PostTask(FROM_HERE, std::move(closure));
}

bool OhosVideoDecoder::NeedsBitstreamConversion() const {
  // AVCodecKit wants Annex B with in-band parameter sets; this makes the
  // demuxer convert AVCC and repeat SPS/PPS on key frames, which also
  // restores them after a flush clears the codec's copy.
  // VP9 packets and AV1 OBUs must not be passed through Annex B conversion.
  return config_.codec() == VideoCodec::kH264 ||
         config_.codec() == VideoCodec::kHEVC;
}

bool OhosVideoDecoder::CanReadWithoutStalling() const {
  // Surface mode uses a fixed native queue. Our in-flight limit is only an
  // upper bound: codec DPB requirements and the producer's queue capacity may
  // leave fewer free buffers. Do not promise a new frame during preroll.
  return !surface_enabled_;
}

int OhosVideoDecoder::GetMaxDecodeRequests() const {
  return kMaxDecodeRequests;
}

bool OhosVideoDecoder::IsPlatformDecoder() const {
  return true;
}

VideoDecoderType OhosVideoDecoder::GetDecoderType() const {
  // No enum value exists for AVCodecKit; adding one means changing the mojom
  // enum and UMA enums, which the renderer does not need for playback.
  return VideoDecoderType::kUnknown;
}

// static
void OhosVideoDecoder::OnCodecErrorThunk(OH_AVCodec* codec,
                                         int32_t error_code,
                                         void* user_data) {
  auto* relay = static_cast<CallbackRelay*>(user_data);
  relay->task_runner->PostTask(
      FROM_HERE, base::BindOnce(&OhosVideoDecoder::OnCodecError, relay->decoder,
                                relay->generation.load(), error_code));
}

// static
void OhosVideoDecoder::OnStreamChangedThunk(OH_AVCodec* codec,
                                            OH_AVFormat* format,
                                            void* user_data) {
  // `format` is only valid during this call; the new layout is read back
  // through OH_VideoDecoder_GetOutputDescription() on the decoder sequence.
  auto* relay = static_cast<CallbackRelay*>(user_data);
  relay->task_runner->PostTask(
      FROM_HERE, base::BindOnce(&OhosVideoDecoder::OnStreamChanged,
                                relay->decoder, relay->generation.load()));
}

// static
void OhosVideoDecoder::OnNeedInputBufferThunk(OH_AVCodec* codec,
                                              uint32_t index,
                                              OH_AVBuffer* buffer,
                                              void* user_data) {
  auto* relay = static_cast<CallbackRelay*>(user_data);
  relay->task_runner->PostTask(
      FROM_HERE,
      base::BindOnce(&OhosVideoDecoder::OnNeedInputBuffer, relay->decoder,
                     relay->generation.load(), CodecBuffer{index, buffer}));
}

// static
void OhosVideoDecoder::OnNewOutputBufferThunk(OH_AVCodec* codec,
                                              uint32_t index,
                                              OH_AVBuffer* buffer,
                                              void* user_data) {
  auto* relay = static_cast<CallbackRelay*>(user_data);
  relay->task_runner->PostTask(
      FROM_HERE,
      base::BindOnce(&OhosVideoDecoder::OnNewOutputBuffer, relay->decoder,
                     relay->generation.load(), CodecBuffer{index, buffer}));
}

void OhosVideoDecoder::OnCodecError(uint32_t generation, int32_t error_code) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (generation != generation_) {
    return;
  }
  MEDIA_LOG(ERROR, media_log_) << "AVCodecKit decoder error " << error_code;
  EnterErrorState("codec reported an error");
}

void OhosVideoDecoder::OnStreamChanged(uint32_t generation) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (generation != generation_) {
    return;
  }
  output_layout_.reset();
}

void OhosVideoDecoder::OnNeedInputBuffer(uint32_t generation,
                                         CodecBuffer input) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (generation != generation_) {
    return;
  }
  free_inputs_.push_back(input);
  PumpInput();
}

void OhosVideoDecoder::OnNewOutputBuffer(uint32_t generation,
                                         CodecBuffer output) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // A stale index belongs to a destroyed codec or was invalidated by a
  // flush; handing it back would be rejected, so it is simply forgotten.
  if (generation != generation_ || !codec_) {
    return;
  }

  OH_AVCodecBufferAttr attr = {};
  if (OH_AVBuffer_GetBufferAttr(output.buffer, &attr) != AV_ERR_OK) {
    OH_VideoDecoder_FreeOutputBuffer(codec_.get(), output.index);
    EnterErrorState("output buffer has no attributes");
    return;
  }

  if (attr.flags & AVCODEC_BUFFER_FLAGS_EOS) {
    OH_VideoDecoder_FreeOutputBuffer(codec_.get(), output.index);
    if (surface_enabled_) {
      surface_eos_ = true;
      MaybeCompleteSurfaceDrain();
    } else {
      OnDrainComplete();
    }
    return;
  }

  if (state_ == State::kError) {
    OH_VideoDecoder_FreeOutputBuffer(codec_.get(), output.index);
    return;
  }

  if (surface_enabled_) {
    // Surface callbacks contain metadata, not mapped pixel data: size=0 is
    // valid. Codec-data-only output, if supplied, is not a picture.
    if (attr.flags & AVCODEC_BUFFER_FLAGS_CODEC_DATA) {
      OH_VideoDecoder_FreeOutputBuffer(codec_.get(), output.index);
      return;
    }
    if ((!output_layout_ && !UpdateOutputLayout()) ||
        surface_outputs_.size() >= 64) {
      OH_VideoDecoder_FreeOutputBuffer(codec_.get(), output.index);
      EnterErrorState("invalid surface layout or excessive output queue");
      return;
    }
    const gfx::Size picture_size(output_layout_->width, output_layout_->height);
    gfx::Rect visible_rect(picture_size);
    if (picture_size == config_.coded_size()) {
      visible_rect = config_.visible_rect();
    }
    gfx::ColorSpace color_space = output_layout_->color_space;
    if (!color_space.IsValid()) {
      color_space = gfx::ColorSpace::CreateREC709();
    }
    surface_outputs_.push_back(
        SurfaceOutput{output.index, base::Microseconds(attr.pts), visible_rect,
                      config_.aspect_ratio().GetNaturalSize(visible_rect),
                      color_space, config_.hdr_metadata()});
    RenderNextSurfaceOutput();
    return;
  }

  scoped_refptr<VideoFrame> frame;
  if (attr.size > 0) {
    frame = CopyOutput(output.buffer, attr);
  }
  OH_VideoDecoder_FreeOutputBuffer(codec_.get(), output.index);
  if (attr.size <= 0) {
    return;
  }
  if (!frame) {
    EnterErrorState("could not copy a decoded picture");
    return;
  }
  output_cb_.Run(std::move(frame));
}

DecoderStatus OhosVideoDecoder::CreateCodec() {
  std::optional<std::string> name =
      GetOhosHardwareCodecName(config_.codec(), /*is_encoder=*/false);
  if (!name) {
    return DecoderStatus::Codes::kUnsupportedCodec;
  }
  codec_.reset(OH_VideoDecoder_CreateByName(name->c_str()));
  if (!codec_) {
    return {DecoderStatus::Codes::kFailedToCreateDecoder,
            "OH_VideoDecoder_CreateByName failed"};
  }

  AdvanceGeneration();
  relay_ = std::make_unique<CallbackRelay>(
      task_runner_, weak_factory_.GetWeakPtr(), generation_);
  OH_AVCodecCallback callbacks = {
      &OhosVideoDecoder::OnCodecErrorThunk,
      &OhosVideoDecoder::OnStreamChangedThunk,
      &OhosVideoDecoder::OnNeedInputBufferThunk,
      &OhosVideoDecoder::OnNewOutputBufferThunk,
  };
  if (OH_VideoDecoder_RegisterCallback(codec_.get(), callbacks, relay_.get()) !=
      AV_ERR_OK) {
    return {DecoderStatus::Codes::kFailedToCreateDecoder,
            "OH_VideoDecoder_RegisterCallback failed"};
  }

  ScopedOhosAVFormat format(OH_AVFormat_Create());
  if (!format) {
    return DecoderStatus::Codes::kOutOfMemory;
  }
  const gfx::Size& coded_size = config_.coded_size();
  OH_AVFormat_SetIntValue(format.get(), OH_MD_KEY_WIDTH, coded_size.width());
  OH_AVFormat_SetIntValue(format.get(), OH_MD_KEY_HEIGHT, coded_size.height());
  if (config_.codec() == VideoCodec::kVP9 ||
      config_.codec() == VideoCodec::kAV1) {
    const auto profile = VideoCodecProfileToOhosProfile(config_.profile());
    if (!profile ||
        !OH_AVFormat_SetIntValue(format.get(), OH_MD_KEY_PROFILE, *profile)) {
      return DecoderStatus::Codes::kUnsupportedConfig;
    }
  }
  // AVCodec's NV12 enum also describes its 10-bit P010 output (UV order).
  // The codec updates the native graphic format after parsing the stream.
  // Never infer output precision from this AV enum; validate the NativeBuffer.
  OH_AVFormat_SetIntValue(format.get(), OH_MD_KEY_PIXEL_FORMAT,
                          AV_PIXEL_FORMAT_NV12);

  OH_AVErrCode result = OH_VideoDecoder_Configure(codec_.get(), format.get());
  if (result != AV_ERR_OK) {
    return {DecoderStatus::Codes::kUnsupportedConfig,
            "OH_VideoDecoder_Configure failed: " +
                base::NumberToString(static_cast<int>(result))};
  }
  if (surface_enabled_) {
    surface_ = OhosVideoSurface::Create(
        task_runner_, coded_size,
        base::BindPostTask(
            task_runner_,
            base::BindRepeating(&OhosVideoDecoder::OnSurfaceFrameAvailable,
                                weak_factory_.GetWeakPtr(), generation_)));
    if (!surface_ || OH_VideoDecoder_SetSurface(
                         codec_.get(), surface_->window()) != AV_ERR_OK) {
      return {DecoderStatus::Codes::kFailedToCreateDecoder,
              "consumer surface/SetSurface failed"};
    }
  }
  if (cdm_context_) {
    // Must come before Prepare(). Software crypto only, so frames stay in
    // ordinary buffers and the zero-copy path is unchanged; a secure video
    // path would render past the GPU.
    result = OH_VideoDecoder_SetDecryptionConfig(
        codec_.get(), cdm_context_->GetOhosMediaKeySession(),
        /*secureVideoPath=*/false);
    if (result != AV_ERR_OK) {
      return {DecoderStatus::Codes::kFailedToCreateDecoder,
              "OH_VideoDecoder_SetDecryptionConfig failed: " +
                  base::NumberToString(static_cast<int>(result))};
    }
  }
  if (OH_VideoDecoder_Prepare(codec_.get()) != AV_ERR_OK ||
      OH_VideoDecoder_Start(codec_.get()) != AV_ERR_OK) {
    return {DecoderStatus::Codes::kFailedToCreateDecoder,
            "OH_VideoDecoder_Prepare/Start failed"};
  }
  LOG(WARNING) << "OHOS video decoder: selected hardware " << *name
               << " profile=" << GetProfileName(config_.profile())
               << " output=" << (surface_enabled_ ? "surface" : "buffer")
               << (cdm_context_ ? ", decrypting" : "");
  return DecoderStatus::Codes::kOk;
}

void OhosVideoDecoder::AdvanceGeneration() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // Every epoch change must reach the converter before that epoch's frames.
  // CreateCodec() advances independently of DestroyCodec(), including on the
  // first initialization and after seek/EOS. Updating only on destruction
  // leaves the cache one epoch behind and rejects every first Surface frame.
  ++generation_;
  if (!frame_converter_) {
    return;
  }
  gpu_task_runner_->PostTask(
      FROM_HERE, base::BindOnce(&OhosVideoFrameConverter::Reset,
                               frame_converter_, generation_));
}

void OhosVideoDecoder::DestroyCodec() {
  // Invalidate before destroying so that callbacks already posted by this
  // codec are ignored even if a new codec reuses their indices.
  AdvanceGeneration();
  surface_timeout_.Stop();
  free_inputs_.clear();
  surface_outputs_.clear();
  rendered_output_.reset();
  surface_frames_in_flight_ = 0;
  pending_conversions_ = 0;
  surface_eos_ = false;
  if (surface_) {
    surface_->Retire();
  }
  codec_.reset();
  relay_.reset();
  surface_.reset();
  output_layout_.reset();
}

bool OhosVideoDecoder::FlushAndRestartCodec() {
  if (!codec_) {
    return false;
  }
  if (surface_enabled_) {
    // A fresh consumer queue prevents late pre-seek frames from being paired
    // with new timestamps. Outstanding GPU frames keep the old queue alive.
    DestroyCodec();
    return CreateCodec().is_ok();
  }
  // No callback fires between Flush() and Start(), so bumping the generation
  // in between cleanly separates pre-flush callbacks from post-flush ones.
  if (OH_VideoDecoder_Flush(codec_.get()) != AV_ERR_OK) {
    return false;
  }
  AdvanceGeneration();
  relay_->generation.store(generation_);
  free_inputs_.clear();
  return OH_VideoDecoder_Start(codec_.get()) == AV_ERR_OK;
}

void OhosVideoDecoder::PumpInput() {
  while (state_ == State::kDecoding && !pending_decodes_.empty() &&
         !free_inputs_.empty()) {
    if (cdm_context_ && pending_decodes_.front().buffer->decrypt_config() &&
        !cdm_context_->OhosHasUsableKey()) {
      // Without a key the codec would fail the sample; wait for the license.
      if (!waiting_for_key_) {
        waiting_for_key_ = true;
        waiting_cb_.Run(WaitingReason::kNoDecryptionKey);
      }
      return;
    }
    PendingDecode pending = std::move(pending_decodes_.front());
    pending_decodes_.pop_front();
    CodecBuffer input = free_inputs_.front();
    free_inputs_.pop_front();

    if (!QueueInput(input, *pending.buffer)) {
      std::move(pending.decode_cb)
          .Run(DecoderStatus::Codes::kPlatformDecodeFailure);
      EnterErrorState("could not queue input");
      return;
    }
    if (pending.buffer->end_of_stream()) {
      state_ = State::kDraining;
      eos_decode_cb_ = std::move(pending.decode_cb);
      return;
    }
    std::move(pending.decode_cb).Run(DecoderStatus::Codes::kOk);
  }
}

bool OhosVideoDecoder::QueueInput(const CodecBuffer& input,
                                  const DecoderBuffer& buffer) {
  OH_AVCodecBufferAttr attr = {};
  if (buffer.end_of_stream()) {
    attr.flags = AVCODEC_BUFFER_FLAGS_EOS;
  } else {
    uint8_t* address = OH_AVBuffer_GetAddr(input.buffer);
    const int32_t capacity = OH_AVBuffer_GetCapacity(input.buffer);
    if (!address || capacity <= 0 ||
        buffer.size() > static_cast<size_t>(capacity)) {
      MEDIA_LOG(ERROR, media_log_)
          << "Input of " << buffer.size()
          << " bytes does not fit a codec buffer of " << capacity;
      return false;
    }
    // SAFETY: AVCodecKit guarantees `capacity` bytes at `address` for as long
    // as the index is not pushed back.
    auto destination =
        UNSAFE_BUFFERS(base::span(address, static_cast<size_t>(capacity)));
    destination.first(buffer.size())
        .copy_from(buffer.subspan(0, buffer.size()));
    attr.pts = buffer.timestamp().InMicroseconds();
    attr.size = static_cast<int32_t>(buffer.size());
    attr.flags = AVCODEC_BUFFER_FLAGS_NONE;
    if (cdm_context_ && !AttachOhosCencInfo(buffer.decrypt_config(),
                                            buffer.size(), input.buffer)) {
      MEDIA_LOG(ERROR, media_log_) << "Unsupported video encryption layout";
      return false;
    }
  }
  if (OH_AVBuffer_SetBufferAttr(input.buffer, &attr) != AV_ERR_OK) {
    return false;
  }
  return OH_VideoDecoder_PushInputBuffer(codec_.get(), input.index) ==
         AV_ERR_OK;
}

void OhosVideoDecoder::OnCdmEvent(CdmContext::Event event) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (event != CdmContext::Event::kHasAdditionalUsableKey ||
      !waiting_for_key_) {
    return;
  }
  waiting_for_key_ = false;
  PumpInput();
}

bool OhosVideoDecoder::UpdateOutputLayout() {
  ScopedOhosAVFormat format(OH_VideoDecoder_GetOutputDescription(codec_.get()));
  if (!format) {
    return false;
  }
  // The picture keys exclude alignment padding; WIDTH/HEIGHT may include it
  // on some decoders, so they are only the fallback.
  const int32_t width = ReadIntOr(
      format.get(), OH_MD_KEY_VIDEO_PIC_WIDTH,
      ReadIntOr(format.get(), OH_MD_KEY_WIDTH, config_.coded_size().width()));
  const int32_t height = ReadIntOr(
      format.get(), OH_MD_KEY_VIDEO_PIC_HEIGHT,
      ReadIntOr(format.get(), OH_MD_KEY_HEIGHT, config_.coded_size().height()));
  const int32_t stride = ReadIntOr(format.get(), OH_MD_KEY_VIDEO_STRIDE, width);
  const int32_t slice_height =
      ReadIntOr(format.get(), OH_MD_KEY_VIDEO_SLICE_HEIGHT, height);
  if (width <= 0 || height <= 0 ||
      (!surface_enabled_ && (stride < width || slice_height < height))) {
    MEDIA_LOG(ERROR, media_log_)
        << "Unusable decoder output layout " << width << "x" << height
        << " stride " << stride << " slice height " << slice_height;
    return false;
  }
  const int32_t range = ReadIntOrMissing(format.get(), OH_MD_KEY_RANGE_FLAG);
  const auto output_color_space =
      VideoColorSpace(
          ReadIntOrMissing(format.get(), OH_MD_KEY_COLOR_PRIMARIES),
          ReadIntOrMissing(format.get(), OH_MD_KEY_TRANSFER_CHARACTERISTICS),
          ReadIntOrMissing(format.get(), OH_MD_KEY_MATRIX_COEFFICIENTS),
          range == 0 ? gfx::ColorSpace::RangeID::LIMITED
                     : (range == 1 ? gfx::ColorSpace::RangeID::FULL
                                   : gfx::ColorSpace::RangeID::INVALID))
          .ToGfxColorSpace();
  output_layout_ =
      OutputLayout{width, height, stride, slice_height,
                   output_color_space.IsValid()
                       ? output_color_space
                       : config_.color_space_info().ToGfxColorSpace()};
  // Both paths use decoder output colorimetry when complete. Surface frames
  // can additionally obtain more specific per-buffer color metadata.
  // -1 is a key the decoder did not report; range 1 is full, 0 limited.
  LOG(WARNING)
      << "OHOS video decoder output: " << GetProfileName(config_.profile())
      << ", pixel format "
      << ReadIntOrMissing(format.get(), OH_MD_KEY_PIXEL_FORMAT) << ", range "
      << ReadIntOrMissing(format.get(), OH_MD_KEY_RANGE_FLAG) << ", primaries "
      << ReadIntOrMissing(format.get(), OH_MD_KEY_COLOR_PRIMARIES)
      << ", transfer "
      << ReadIntOrMissing(format.get(), OH_MD_KEY_TRANSFER_CHARACTERISTICS)
      << ", matrix "
      << ReadIntOrMissing(format.get(), OH_MD_KEY_MATRIX_COEFFICIENTS)
      << "; surface color space " << output_layout_->color_space.ToString();
  return true;
}

scoped_refptr<VideoFrame> OhosVideoDecoder::CopyOutput(
    OH_AVBuffer* buffer,
    const OH_AVCodecBufferAttr& attr) {
  TRACE_EVENT0("media", "OhosVideoDecoder::CopyOutput");
  if (!output_layout_ && !UpdateOutputLayout()) {
    return nullptr;
  }
  const OutputLayout& layout = *output_layout_;

  // AV_PIXEL_FORMAT_NV12 alone does not distinguish NV12 from P010. Buffer
  // mode also needs the actual native allocation, even with zero-copy off.
  VideoPixelFormat pixel_format = PIXEL_FORMAT_NV12;
  OH_NativeBuffer* native_buffer = OH_AVBuffer_GetNativeBuffer(buffer);
  if (native_buffer) {
    OH_NativeBuffer_Config native_config = {};
    OH_NativeBuffer_GetConfig(native_buffer, &native_config);
    OH_NativeBuffer_Unreference(native_buffer);
    if (native_config.format == NATIVEBUFFER_PIXEL_FMT_YCBCR_P010) {
      pixel_format = PIXEL_FORMAT_P010LE;
    } else if (native_config.format != NATIVEBUFFER_PIXEL_FMT_YCBCR_420_SP) {
      LOG(ERROR) << "OHOS video decoder: unsupported buffer format "
                 << native_config.format;
      return nullptr;
    }
    if (native_config.stride != layout.stride) {
      LOG(ERROR) << "OHOS video decoder: native/output stride mismatch";
      return nullptr;
    }
  } else if (RequiresP010(config_.profile()) ||
             config_.codec() == VideoCodec::kAV1) {
    LOG(ERROR) << "OHOS video decoder: cannot verify buffer output bit depth";
    return nullptr;
  }
  if (RequiresP010(config_.profile()) && pixel_format != PIXEL_FORMAT_P010LE) {
    LOG(ERROR) << "OHOS video decoder: high bit depth profile requires P010";
    return nullptr;
  }

  // Validate source row widths before libyuv (whose strides are int).
  const int bytes_per_sample = pixel_format == PIXEL_FORMAT_P010LE ? 2 : 1;
  base::CheckedNumeric<int> y_row_bytes = layout.width;
  y_row_bytes *= bytes_per_sample;
  base::CheckedNumeric<int> uv_row_bytes = layout.width / 2 + layout.width % 2;
  uv_row_bytes *= 2 * bytes_per_sample;
  if (!y_row_bytes.IsValid() || !uv_row_bytes.IsValid() ||
      layout.stride < y_row_bytes.ValueOrDie() ||
      layout.stride < uv_row_bytes.ValueOrDie() ||
      layout.width == std::numeric_limits<int>::max() ||
      layout.height == std::numeric_limits<int>::max()) {
    return nullptr;
  }

  uint8_t* address = OH_AVBuffer_GetAddr(buffer);
  const int32_t capacity = OH_AVBuffer_GetCapacity(buffer);
  base::CheckedNumeric<size_t> data_end = attr.offset;
  data_end += attr.size;
  if (!address || capacity <= 0 || attr.offset < 0 || !data_end.IsValid() ||
      data_end.ValueOrDie() > static_cast<size_t>(capacity)) {
    return nullptr;
  }
  // SAFETY: AVCodecKit guarantees `capacity` bytes at `address` until the
  // output index is freed, which happens after this copy.
  auto data = UNSAFE_BUFFERS(base::span(address, static_cast<size_t>(capacity)))
                  .subspan(static_cast<size_t>(attr.offset),
                           static_cast<size_t>(attr.size));

  const size_t stride = static_cast<size_t>(layout.stride);
  const size_t uv_rows = static_cast<size_t>((layout.height + 1) / 2);
  base::CheckedNumeric<size_t> uv_offset = stride;
  uv_offset *= static_cast<size_t>(layout.slice_height);
  base::CheckedNumeric<size_t> required = stride;
  required *= uv_rows - 1;
  required += uv_row_bytes.ValueOrDie();
  required += uv_offset;
  if (!required.IsValid() || required.ValueOrDie() > data.size()) {
    MEDIA_LOG(ERROR, media_log_) << "Decoded picture of " << data.size()
                                 << " bytes is smaller than its layout needs";
    return nullptr;
  }

  const bool is_p010 = pixel_format == PIXEL_FORMAT_P010LE;
  if (is_p010 &&
      (layout.stride % sizeof(uint16_t) != 0 ||
       reinterpret_cast<uintptr_t>(data.data()) % alignof(uint16_t) != 0)) {
    MEDIA_LOG(ERROR, media_log_) << "Unaligned P010 decoder buffer";
    return nullptr;
  }

  const gfx::Size visible_size(layout.width, layout.height);
  const gfx::Rect visible_rect = visible_size == config_.coded_size()
                                     ? config_.visible_rect()
                                     : gfx::Rect(visible_size);
  // CPU P010 is not supported by VideoResourceUpdater's software-frame
  // upload path. Convert to planar, low-bit-aligned I010 while copying the
  // decoder output. This preserves all ten bits and the original PQ/HLG
  // metadata, unlike reducing the frame to NV12 or RGB8.
  const VideoPixelFormat frame_format =
      is_p010 ? PIXEL_FORMAT_YUV420P10 : pixel_format;
  // Both output formats have 2x2 chroma subsampling.
  const gfx::Size coded_size((layout.width + 1) & ~1, (layout.height + 1) & ~1);
  const gfx::Size natural_size =
      config_.aspect_ratio().GetNaturalSize(visible_rect);
  const auto timestamp = base::Microseconds(attr.pts);
  std::unique_ptr<base::MappedReadOnlyRegion> shared_memory;
  scoped_refptr<VideoFrame> frame;
  if (!gpu_task_runner_) {
    // Browser-hosted decoding sends CPU frames across Mojo. Write the final
    // planes directly into the IPC backing rather than allocating a heap frame
    // that VideoFrameDataPtr::CreateRegion must copy again. Each frame owns its
    // region; it is never recycled while a remote reader may still retain it.
    shared_memory = std::make_unique<base::MappedReadOnlyRegion>(
        base::ReadOnlySharedMemoryRegion::Create(
            VideoFrame::AllocationSize(frame_format, coded_size)));
    if (!shared_memory->IsValid()) {
      return nullptr;
    }
    frame = VideoFrame::WrapExternalData(
        frame_format, coded_size, visible_rect, natural_size,
        shared_memory->mapping.GetMemoryAsSpan<uint8_t>(), timestamp);
  } else {
    frame = VideoFrame::CreateFrame(frame_format, coded_size, visible_rect,
                                    natural_size, timestamp);
  }
  if (!frame) {
    return nullptr;
  }

  const auto uv_plane = data.subspan(uv_offset.ValueOrDie());
  if (is_p010) {
    if (libyuv::P010ToI010(
            reinterpret_cast<const uint16_t*>(data.data()), layout.stride / 2,
            reinterpret_cast<const uint16_t*>(uv_plane.data()),
            layout.stride / 2,
            reinterpret_cast<uint16_t*>(frame->writable_data(VideoFrame::kY)),
            frame->stride(VideoFrame::kY) / 2,
            reinterpret_cast<uint16_t*>(frame->writable_data(VideoFrame::kU)),
            frame->stride(VideoFrame::kU) / 2,
            reinterpret_cast<uint16_t*>(frame->writable_data(VideoFrame::kV)),
            frame->stride(VideoFrame::kV) / 2, layout.width, layout.height)) {
      return nullptr;
    }
  } else {
    libyuv::CopyPlane(data.data(), layout.stride,
                     frame->writable_data(VideoFrame::Plane::kY),
                     static_cast<int>(frame->stride(VideoFrame::Plane::kY)),
                     y_row_bytes.ValueOrDie(), layout.height);
    libyuv::CopyPlane(uv_plane.data(), layout.stride,
                     frame->writable_data(VideoFrame::Plane::kUV),
                     static_cast<int>(frame->stride(VideoFrame::Plane::kUV)),
                     uv_row_bytes.ValueOrDie(), static_cast<int>(uv_rows));
  }

  if (shared_memory) {
    // Publish the read-only backing only after the last write; writable_data()
    // intentionally rejects STORAGE_SHMEM. The destruction observer keeps the
    // original writable mapping alive for the local frame's whole lifetime.
    frame->BackWithSharedMemory(&shared_memory->region);
    frame->AddDestructionObserver(base::BindOnce(
        [](std::unique_ptr<base::MappedReadOnlyRegion>) {},
        std::move(shared_memory)));
  }

  const gfx::ColorSpace& color_space = layout.color_space;
  if (color_space.IsValid()) {
    frame->set_color_space(color_space);
  }
  if (color_space.IsHDR()) {
    frame->set_hdr_metadata(config_.hdr_metadata());
  }
  if (!buffer_frame_logged_) {
    buffer_frame_logged_ = true;
    LOG(WARNING) << "OHOS video decoder: first buffer frame format="
                 << VideoPixelFormatToString(frame_format)
                 << " color_space=" << color_space.ToString();
  }
  // MojoVideoDecoderService DCHECKs this for every frame it ships.
  frame->metadata().power_efficient = true;
  return frame;
}

void OhosVideoDecoder::RenderNextSurfaceOutput() {
  if (!surface_ || rendered_output_ || surface_outputs_.empty() ||
      surface_frames_in_flight_ >= kMaxSurfaceFramesInFlight ||
      state_ == State::kError) {
    return;
  }
  rendered_output_ = std::move(surface_outputs_.front());
  surface_outputs_.pop_front();
  if (OH_VideoDecoder_RenderOutputBuffer(
          codec_.get(), rendered_output_->index) != AV_ERR_OK) {
    EnterErrorState("RenderOutputBuffer failed");
    return;
  }
  surface_timeout_.Start(FROM_HERE, base::Seconds(10),
                         base::BindOnce(&OhosVideoDecoder::EnterErrorState,
                                        weak_factory_.GetWeakPtr(),
                                        "surface frame arrival timed out"));
}

void OhosVideoDecoder::OnSurfaceFrameAvailable(uint32_t generation) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (generation != generation_ || !surface_ || !rendered_output_) {
    return;
  }
  OHNativeWindowBuffer* buffer = nullptr;
  base::ScopedFD acquire_fence;
  base::OnceClosure release;
  if (!surface_->Acquire(&buffer, &acquire_fence, &release)) {
    EnterErrorState("could not acquire surface buffer");
    return;
  }
  surface_timeout_.Stop();
  SurfaceOutput output = std::move(*rendered_output_);
  rendered_output_.reset();
  ++surface_frames_in_flight_;
  auto released = base::BindPostTask(
      task_runner_, base::BindOnce(&OhosVideoDecoder::OnSurfaceFrameReleased,
                                   weak_factory_.GetWeakPtr(), generation));
  // Both callbacks run even if conversion is cancelled before import.
  auto pixmap = ui::CreateOhosVideoNativePixmap(
      buffer, &output.color_space,
      base::BindOnce(
          [](base::ScopedClosureRunner lease, base::ScopedClosureRunner done) {
            lease.RunAndReset();
            done.RunAndReset();
          },
          base::ScopedClosureRunner(std::move(release)),
          base::ScopedClosureRunner(std::move(released))));
  if (!pixmap) {
    EnterErrorState("unsupported decoder NativeBuffer");
    return;
  }
  auto native_format = pixmap->GetSharedImageFormat();
  native_format.ClearPrefersExternalSampler();
  if (RequiresP010(config_.profile()) &&
      native_format != viz::MultiPlaneFormat::kP010) {
    LOG(ERROR) << "OHOS video zero-copy: " << GetProfileName(config_.profile())
               << " requires P010 but received " << native_format.ToString();
    EnterErrorState("decoder did not preserve high bit depth output precision");
    return;
  }
  ++pending_conversions_;
  gfx::GpuFenceHandle fence;
  fence.Adopt(std::move(acquire_fence));
  if (browser_sii_) {
    ConvertBrowserFrame(generation, std::move(pixmap), std::move(fence),
                        std::move(output));
    RenderNextSurfaceOutput();
    return;
  }
  gpu_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(
          &OhosVideoFrameConverter::Convert, frame_converter_,
          generation, std::move(pixmap), std::move(fence), output.visible_rect,
          output.natural_size, output.color_space, output.hdr_metadata,
          output.timestamp,
          base::BindPostTask(
              task_runner_,
              base::BindOnce(&OhosVideoDecoder::OnSurfaceFrameConverted,
                             weak_factory_.GetWeakPtr(), generation))));
  RenderNextSurfaceOutput();
}

void OhosVideoDecoder::ConvertBrowserFrame(
    uint32_t generation,
    scoped_refptr<gfx::NativePixmap> pixmap,
    gfx::GpuFenceHandle fence,
    SurfaceOutput output) {
  TRACE_EVENT0("media", "OhosVideoDecoder::ConvertBrowserFrame");
  int endpoints[2];
  if (!browser_sii_ || browser_sii_->IsLost() ||
      socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, endpoints)) {
    OnSurfaceFrameConverted(generation, nullptr);
    return;
  }
  base::ScopedFD receiver(endpoints[0]), sender(endpoints[1]);
  auto handle = ui::ExportOhosVideoNativePixmap(*pixmap, std::move(sender));
  if (handle.ohos_buffer_prefix.empty()) {
    OnSurfaceFrameConverted(generation, nullptr);
    return;
  }
  auto pixel_format = SharedImageFormatToVideoPixelFormat([&] {
    auto format = pixmap->GetSharedImageFormat();
    format.ClearPrefersExternalSampler();
    return format;
  }());
  CHECK(pixel_format);
  const gpu::SharedImageInfo info(pixmap->GetSharedImageFormat(),
                                  pixmap->GetBufferSize(), output.color_space,
                                  kTopLeft_GrSurfaceOrigin, kOpaque_SkAlphaType,
                                  gpu::SHARED_IMAGE_USAGE_DISPLAY_READ |
                                      gpu::SHARED_IMAGE_USAGE_RASTER_READ,
                                  "OhosPhoneVideo");
  auto image = browser_sii_->CreateSharedImage(
      info, gfx::GpuMemoryBufferHandle(std::move(handle)));
  // This independent lease survives VideoFrame release and decode reset. The
  // GPU VulkanImage keeps its endpoint through fence-helper deferred cleanup.
  // Consequently EOF means no import can still read the decoder allocation.
  base::ThreadPool::PostTask(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(
          [](base::ScopedFD receiver, scoped_refptr<gfx::NativePixmap> lease) {
            pollfd pfd{receiver.get(), POLLIN | POLLHUP, 0};
            int result = HANDLE_EINTR(poll(&pfd, 1, -1));
            // A wait failure must never return a buffer still used by the GPU
            // to the decoder's reusable queue.
            PCHECK(result == 1);
            CHECK(pfd.revents & POLLHUP);
          },
          std::move(receiver), std::move(pixmap)));
  if (!image) {
    OnSurfaceFrameConverted(generation, nullptr);
    return;
  }
  gpu::SyncToken ready = image->creation_sync_token();
  if (!fence.is_null())
    ready = image->BackingWasExternallyUpdated(
        std::make_unique<gfx::GpuFence>(std::move(fence)));
  browser_sii_->VerifySyncToken(ready);
  auto frame = VideoFrame::WrapSharedImage(
      *pixel_format, image, ready,
      base::BindOnce(
          [](scoped_refptr<gpu::SharedImageInterface> sii,
             scoped_refptr<gpu::ClientSharedImage> image,
             const gpu::SyncToken& token) {
            image->UpdateDestructionSyncToken(token);
            image.reset();
            sii->VerifyFlush();
          },
          browser_sii_, image),
      output.visible_rect, output.natural_size, output.timestamp);
  if (frame) {
    frame->set_color_space(output.color_space);
    frame->set_hdr_metadata(output.hdr_metadata);
    frame->metadata().power_efficient = true;
    frame->metadata().read_lock_fences_enabled = true;
  }
  OnSurfaceFrameConverted(generation, std::move(frame));
}

void OhosVideoDecoder::OnSurfaceFrameConverted(
    uint32_t generation,
    scoped_refptr<VideoFrame> frame) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (generation != generation_) {
    return;
  }
  --pending_conversions_;
  if (!frame) {
    EnterErrorState("surface SharedImage conversion failed");
    return;
  }
  if (++surface_frame_count_ == 1) {
    LOG(WARNING) << "OHOS video zero-copy: first SharedImage frame "
                 << frame->coded_size().ToString()
                 << " format=" << VideoPixelFormatToString(frame->format())
                 << " color_space=" << frame->ColorSpace().ToString()
                 << " hdr_metadata=" << !frame->hdr_metadata().IsEmpty()
                 << "; CPU output copies=0";
  }
  auto weak_this = weak_factory_.GetWeakPtr();
  output_cb_.Run(std::move(frame));
  if (weak_this) {
    MaybeCompleteSurfaceDrain();
  }
}

void OhosVideoDecoder::OnSurfaceFrameReleased(uint32_t generation) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (generation != generation_) {
    return;
  }
  DCHECK_GT(surface_frames_in_flight_, 0u);
  --surface_frames_in_flight_;
  RenderNextSurfaceOutput();
}

void OhosVideoDecoder::MaybeCompleteSurfaceDrain() {
  if (surface_eos_ && surface_outputs_.empty() && !rendered_output_ &&
      pending_conversions_ == 0) {
    OnDrainComplete();
  }
}

void OhosVideoDecoder::OnDrainComplete() {
  if (state_ != State::kDraining) {
    return;
  }
  // The codec accepts no input after end of stream; restart it so decoding
  // can continue without a Reset(), as the VideoDecoder contract allows.
  if (!FlushAndRestartCodec()) {
    EnterErrorState("restart after end of stream failed");
    return;
  }
  state_ = State::kDecoding;
  std::move(eos_decode_cb_).Run(DecoderStatus::Codes::kOk);
  PumpInput();
}

void OhosVideoDecoder::EnterErrorState(const char* reason) {
  LOG(ERROR) << "OhosVideoDecoder error: " << reason;
  MEDIA_LOG(ERROR, media_log_) << "OhosVideoDecoder: " << reason;
  state_ = State::kError;
  DestroyCodec();
  AbortPendingDecodes(DecoderStatus::Codes::kPlatformDecodeFailure);
}

void OhosVideoDecoder::AbortPendingDecodes(DecoderStatus status) {
  base::circular_deque<PendingDecode> pending;
  pending.swap(pending_decodes_);
  for (PendingDecode& decode : pending) {
    std::move(decode.decode_cb).Run(status);
  }
  if (eos_decode_cb_) {
    std::move(eos_decode_cb_).Run(status);
  }
}

}  // namespace media
