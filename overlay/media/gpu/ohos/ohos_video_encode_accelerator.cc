// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/gpu/ohos/ohos_video_encode_accelerator.h"

#include <multimedia/player_framework/native_avbuffer.h>
#include <multimedia/player_framework/native_avcodec_videoencoder.h>
#include <multimedia/player_framework/native_averrors.h>
#include <multimedia/player_framework/native_avformat.h>

#include <algorithm>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/memory/shared_memory_mapping.h"
#include "base/memory/unsafe_shared_memory_region.h"
#include "base/numerics/checked_math.h"
#include "base/numerics/safe_conversions.h"
#include "base/strings/string_number_conversions.h"
#include "media/base/color_plane_layout.h"
#include "media/base/video_codecs.h"
#include "media/base/video_frame_layout.h"
#include "media/base/video_types.h"
#include "media/gpu/ohos/ohos_codec_util.h"
#include "media/video/video_encoder_info.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"

namespace media {

namespace {

using EncoderType = VideoEncodeAccelerator::Config::EncoderType;

// Key frame distance when the client sets none: about 100 seconds at 30 fps,
// matching NdkVideoEncodeAccelerator. WebRTC and WebCodecs request key frames
// explicitly when they need them.
constexpr uint32_t kDefaultGopLength = 3000;

int32_t ReadIntOr(OH_AVFormat* format, const char* key, int32_t fallback) {
  int32_t value = 0;
  if (format && OH_AVFormat_GetIntValue(format, key, &value) && value > 0) {
    return value;
  }
  return fallback;
}

std::string WithCode(const char* message, int32_t code) {
  return std::string(message) + ": " + base::NumberToString(code);
}

}  // namespace

struct OhosVideoEncodeAccelerator::CallbackRelay {
  CallbackRelay(scoped_refptr<base::SequencedTaskRunner> task_runner,
                base::WeakPtr<OhosVideoEncodeAccelerator> encoder)
      : task_runner(std::move(task_runner)), encoder(std::move(encoder)) {}

  const scoped_refptr<base::SequencedTaskRunner> task_runner;
  // Only dereferenced on `task_runner`; copying it on codec threads is safe.
  const base::WeakPtr<OhosVideoEncodeAccelerator> encoder;
};

OhosVideoEncodeAccelerator::PendingEncode::PendingEncode(
    scoped_refptr<VideoFrame> frame,
    bool key_frame)
    : frame(std::move(frame)), key_frame(key_frame) {}
OhosVideoEncodeAccelerator::PendingEncode::PendingEncode(PendingEncode&&) =
    default;
OhosVideoEncodeAccelerator::PendingEncode&
OhosVideoEncodeAccelerator::PendingEncode::operator=(PendingEncode&&) = default;
OhosVideoEncodeAccelerator::PendingEncode::~PendingEncode() = default;

void OhosVideoEncodeAccelerator::CodecDeleter::operator()(
    OH_AVCodec* codec) const {
  OH_VideoEncoder_Destroy(codec);
}

OhosVideoEncodeAccelerator::OhosVideoEncodeAccelerator(
    scoped_refptr<base::SequencedTaskRunner> task_runner)
    : task_runner_(std::move(task_runner)) {}

OhosVideoEncodeAccelerator::~OhosVideoEncodeAccelerator() = default;

VideoEncodeAccelerator::SupportedProfiles
OhosVideoEncodeAccelerator::GetSupportedProfiles() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return GetOhosSupportedEncoderProfiles();
}

EncoderStatus OhosVideoEncodeAccelerator::Initialize(
    const Config& config,
    Client* client,
    std::unique_ptr<MediaLog> media_log) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK(client);
  DCHECK(!codec_);

  client_ptr_factory_ = std::make_unique<base::WeakPtrFactory<Client>>(client);
  config_ = config;
  log_ = std::move(media_log);

  if (VideoCodecProfileToVideoCodec(config.output_profile) !=
          VideoCodec::kH264 ||
      !VideoCodecProfileToOhosProfile(config.output_profile)) {
    return {EncoderStatus::Codes::kEncoderUnsupportedProfile,
            "Only H.264 baseline, main and high are supported"};
  }
  if (config.required_encoder_type == EncoderType::kSoftware) {
    return {EncoderStatus::Codes::kEncoderUnsupportedConfig,
            "Only a hardware encoder is available"};
  }
  if (config.HasSpatialLayer() || config.HasTemporalLayer()) {
    return {EncoderStatus::Codes::kEncoderUnsupportedConfig,
            "Scalable encoding is not supported"};
  }
  if (config.framerate == 0) {
    return {EncoderStatus::Codes::kEncoderUnsupportedConfig,
            "Framerate cannot be 0"};
  }
  if (config.input_visible_size.IsEmpty()) {
    return {EncoderStatus::Codes::kEncoderUnsupportedConfig,
            "Empty input size"};
  }
  if (config.bitrate.mode() == Bitrate::Mode::kExternal) {
    return {EncoderStatus::Codes::kEncoderUnsupportedConfig,
            "External rate control is not supported"};
  }

  effective_bitrate_ = config.bitrate;
  effective_framerate_ = config.framerate;

  EncoderStatus status = CreateCodec();
  if (!status.is_ok()) {
    MEDIA_LOG(ERROR, log_) << "AVCodecKit encoder creation failed: "
                           << status.message();
    codec_.reset();
    relay_.reset();
    return status;
  }

  const size_t bitstream_buffer_size = EstimateBitstreamBufferSize(
      config_.bitrate, config_.framerate, config_.input_format,
      config_.input_visible_size);
  task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&Client::RequireBitstreamBuffers,
                     client_ptr_factory_->GetWeakPtr(), 1u,
                     config_.input_visible_size, bitstream_buffer_size));
  return {EncoderStatus::Codes::kOk};
}

void OhosVideoEncodeAccelerator::Encode(scoped_refptr<VideoFrame> frame,
                                        bool force_keyframe) {
  Encode(std::move(frame), VideoEncoder::EncodeOptions(force_keyframe));
}

void OhosVideoEncodeAccelerator::Encode(
    scoped_refptr<VideoFrame> frame,
    const VideoEncoder::EncodeOptions& options) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK(codec_);
  pending_frames_.emplace_back(std::move(frame), options.key_frame);
  FeedInput();
}

void OhosVideoEncodeAccelerator::UseOutputBitstreamBuffer(
    BitstreamBuffer buffer) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  available_bitstream_buffers_.push_back(std::move(buffer));
  DrainOutput();
}

void OhosVideoEncodeAccelerator::RequestEncodingParametersChange(
    const Bitrate& bitrate,
    uint32_t framerate,
    const std::optional<gfx::Size>& size) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (size.has_value()) {
    NotifyErrorStatus({EncoderStatus::Codes::kEncoderUnsupportedConfig,
                       "Changing the frame size is not supported"});
    return;
  }
  if (framerate == 0) {
    NotifyErrorStatus({EncoderStatus::Codes::kEncoderUnsupportedConfig,
                       "Framerate cannot be 0"});
    return;
  }
  if (bitrate.mode() != effective_bitrate_.mode()) {
    NotifyErrorStatus({EncoderStatus::Codes::kEncoderUnsupportedConfig,
                       "The bitrate mode cannot change mid-stream"});
    return;
  }
  if (bitrate == effective_bitrate_ && framerate == effective_framerate_) {
    return;
  }

  ScopedOhosAVFormat format(OH_AVFormat_Create());
  if (!format) {
    return;
  }
  if (bitrate != effective_bitrate_) {
    OH_AVFormat_SetLongValue(format.get(), OH_MD_KEY_BITRATE,
                             bitrate.target_bps());
  }
  if (framerate != effective_framerate_) {
    OH_AVFormat_SetDoubleValue(format.get(), OH_MD_KEY_FRAME_RATE, framerate);
  }
  const OH_AVErrCode result =
      OH_VideoEncoder_SetParameter(codec_.get(), format.get());
  if (result != AV_ERR_OK) {
    // WebRTC adjusts rates every few seconds; failing the whole encoder over
    // a rejected hint would end the call, so keep the previous rates.
    MEDIA_LOG(WARNING, log_) << WithCode("AVCodecKit rejected a rate change",
                                         static_cast<int32_t>(result));
    return;
  }
  effective_bitrate_ = bitrate;
  effective_framerate_ = framerate;
}

void OhosVideoEncodeAccelerator::Destroy() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  client_ptr_factory_.reset();
  codec_.reset();
  relay_.reset();
  delete this;
}

bool OhosVideoEncodeAccelerator::IsFlushSupported() {
  // As with NdkVideoEncodeAccelerator, clients wait for outputs instead;
  // AVCodecKit needs a restart after end of stream, and every client that
  // flushes would pay for it.
  return false;
}

// static
void OhosVideoEncodeAccelerator::OnCodecErrorThunk(OH_AVCodec* codec,
                                                   int32_t error_code,
                                                   void* user_data) {
  auto* relay = static_cast<CallbackRelay*>(user_data);
  relay->task_runner->PostTask(
      FROM_HERE, base::BindOnce(&OhosVideoEncodeAccelerator::OnCodecError,
                                relay->encoder, error_code));
}

// static
void OhosVideoEncodeAccelerator::OnStreamChangedThunk(OH_AVCodec* codec,
                                                      OH_AVFormat* format,
                                                      void* user_data) {
  // The encoder never changes its output format on its own; the input layout
  // is fixed at Start().
}

// static
void OhosVideoEncodeAccelerator::OnNeedInputBufferThunk(OH_AVCodec* codec,
                                                        uint32_t index,
                                                        OH_AVBuffer* buffer,
                                                        void* user_data) {
  auto* relay = static_cast<CallbackRelay*>(user_data);
  relay->task_runner->PostTask(
      FROM_HERE, base::BindOnce(&OhosVideoEncodeAccelerator::OnNeedInputBuffer,
                                relay->encoder, CodecBuffer{index, buffer}));
}

// static
void OhosVideoEncodeAccelerator::OnNewOutputBufferThunk(OH_AVCodec* codec,
                                                        uint32_t index,
                                                        OH_AVBuffer* buffer,
                                                        void* user_data) {
  auto* relay = static_cast<CallbackRelay*>(user_data);
  relay->task_runner->PostTask(
      FROM_HERE, base::BindOnce(&OhosVideoEncodeAccelerator::OnNewOutputBuffer,
                                relay->encoder, CodecBuffer{index, buffer}));
}

void OhosVideoEncodeAccelerator::OnCodecError(int32_t error_code) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  NotifyErrorStatus({EncoderStatus::Codes::kEncoderHardwareDriverError,
                     WithCode("AVCodecKit encoder error", error_code)});
}

void OhosVideoEncodeAccelerator::OnNeedInputBuffer(CodecBuffer input) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  free_inputs_.push_back(input);
  FeedInput();
}

void OhosVideoEncodeAccelerator::OnNewOutputBuffer(CodecBuffer output) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  pending_outputs_.push_back(output);
  DrainOutput();
}

EncoderStatus OhosVideoEncodeAccelerator::CreateCodec() {
  std::optional<std::string> name =
      GetOhosHardwareCodecName(VideoCodec::kH264, /*is_encoder=*/true);
  if (!name) {
    return {EncoderStatus::Codes::kEncoderUnsupportedCodec,
            "No AVCodecKit hardware H.264 encoder"};
  }
  codec_.reset(OH_VideoEncoder_CreateByName(name->c_str()));
  if (!codec_) {
    return {EncoderStatus::Codes::kEncoderInitializationError,
            "OH_VideoEncoder_CreateByName failed"};
  }

  relay_ =
      std::make_unique<CallbackRelay>(task_runner_, weak_factory_.GetWeakPtr());
  OH_AVCodecCallback callbacks = {
      &OhosVideoEncodeAccelerator::OnCodecErrorThunk,
      &OhosVideoEncodeAccelerator::OnStreamChangedThunk,
      &OhosVideoEncodeAccelerator::OnNeedInputBufferThunk,
      &OhosVideoEncodeAccelerator::OnNewOutputBufferThunk,
  };
  if (OH_VideoEncoder_RegisterCallback(codec_.get(), callbacks, relay_.get()) !=
      AV_ERR_OK) {
    return {EncoderStatus::Codes::kEncoderInitializationError,
            "OH_VideoEncoder_RegisterCallback failed"};
  }

  ScopedOhosAVFormat format(OH_AVFormat_Create());
  if (!format) {
    return {EncoderStatus::Codes::kOutOfMemoryError,
            "OH_AVFormat_Create failed"};
  }
  const gfx::Size& size = config_.input_visible_size;
  const uint32_t gop_length = config_.gop_length.value_or(kDefaultGopLength);
  // AVCodecKit takes the key frame interval in milliseconds; 0 would mean
  // "every frame is a key frame", so a GOP of 0 or 1 maps to the minimum 1 ms.
  const int32_t i_frame_interval_ms = std::max(
      1, base::saturated_cast<int32_t>(static_cast<uint64_t>(gop_length) *
                                       1000 / config_.framerate));
  OH_AVFormat_SetIntValue(format.get(), OH_MD_KEY_WIDTH, size.width());
  OH_AVFormat_SetIntValue(format.get(), OH_MD_KEY_HEIGHT, size.height());
  OH_AVFormat_SetIntValue(format.get(), OH_MD_KEY_PIXEL_FORMAT,
                          AV_PIXEL_FORMAT_NV12);
  OH_AVFormat_SetDoubleValue(format.get(), OH_MD_KEY_FRAME_RATE,
                             config_.framerate);
  OH_AVFormat_SetLongValue(format.get(), OH_MD_KEY_BITRATE,
                           config_.bitrate.target_bps());
  OH_AVFormat_SetIntValue(format.get(), OH_MD_KEY_VIDEO_ENCODE_BITRATE_MODE,
                          config_.bitrate.mode() == Bitrate::Mode::kVariable
                              ? BITRATE_MODE_VBR
                              : BITRATE_MODE_CBR);
  OH_AVFormat_SetIntValue(
      format.get(), OH_MD_KEY_PROFILE,
      VideoCodecProfileToOhosProfile(config_.output_profile).value());
  OH_AVFormat_SetIntValue(format.get(), OH_MD_KEY_I_FRAME_INTERVAL,
                          i_frame_interval_ms);

  OH_AVErrCode result = OH_VideoEncoder_Configure(codec_.get(), format.get());
  if (result != AV_ERR_OK) {
    return {EncoderStatus::Codes::kEncoderUnsupportedConfig,
            WithCode("OH_VideoEncoder_Configure failed",
                     static_cast<int32_t>(result))};
  }
  result = OH_VideoEncoder_Prepare(codec_.get());
  if (result == AV_ERR_OK) {
    result = OH_VideoEncoder_Start(codec_.get());
  }
  if (result != AV_ERR_OK) {
    return {EncoderStatus::Codes::kEncoderInitializationError,
            WithCode("OH_VideoEncoder_Prepare/Start failed",
                     static_cast<int32_t>(result))};
  }
  ReadInputLayout();
  NotifyEncoderInfo(*name);
  return {EncoderStatus::Codes::kOk};
}

void OhosVideoEncodeAccelerator::ReadInputLayout() {
  const gfx::Size& size = config_.input_visible_size;
  ScopedOhosAVFormat format(OH_VideoEncoder_GetInputDescription(codec_.get()));
  // Without stride keys the codec expects tightly packed planes.
  input_stride_ =
      std::max(ReadIntOr(format.get(), OH_MD_KEY_VIDEO_STRIDE, size.width()),
               size.width());
  input_slice_height_ = std::max(
      ReadIntOr(format.get(), OH_MD_KEY_VIDEO_SLICE_HEIGHT, size.height()),
      size.height());
}

void OhosVideoEncodeAccelerator::NotifyEncoderInfo(
    const std::string& codec_name) {
  VideoEncoderInfo info;
  info.implementation_name = "OhosVideoEncodeAccelerator(" + codec_name + ")";
  info.supports_native_handle = false;
  info.has_trusted_rate_controller = false;
  info.is_hardware_accelerated = true;
  info.supports_simulcast = false;
  // BitstreamBufferMetadata::qp is never filled in.
  info.reports_average_qp = false;
  info.supports_frame_size_change = false;
  task_runner_->PostTask(
      FROM_HERE, base::BindOnce(&Client::NotifyEncoderInfoChange,
                                client_ptr_factory_->GetWeakPtr(), info));
}

void OhosVideoEncodeAccelerator::FeedInput() {
  while (!error_occurred_ && !pending_frames_.empty() &&
         !free_inputs_.empty()) {
    PendingEncode pending = std::move(pending_frames_.front());
    pending_frames_.pop_front();
    CodecBuffer input = free_inputs_.front();
    free_inputs_.pop_front();

    // Only CPU-visible frames can be converted; the supported profiles never
    // advertise shared-image input, so a texture frame is a client error.
    if (pending.frame->HasSharedImage()) {
      NotifyErrorStatus({EncoderStatus::Codes::kUnsupportedFrameFormat,
                         "Shared image frames are not supported"});
      return;
    }

    if (pending.key_frame) {
      ScopedOhosAVFormat format(OH_AVFormat_Create());
      if (format) {
        OH_AVFormat_SetIntValue(format.get(), OH_MD_KEY_REQUEST_I_FRAME, 1);
        if (OH_VideoEncoder_SetParameter(codec_.get(), format.get()) !=
            AV_ERR_OK) {
          MEDIA_LOG(WARNING, log_) << "AVCodecKit rejected a key frame request";
        }
      }
    }

    const base::TimeDelta timestamp =
        RecordFrameTimestamp(pending.frame->timestamp());
    if (!FeedInputBuffer(input, *pending.frame, timestamp)) {
      return;
    }
  }
}

bool OhosVideoEncodeAccelerator::FeedInputBuffer(const CodecBuffer& input,
                                                 const VideoFrame& frame,
                                                 base::TimeDelta timestamp) {
  uint8_t* address = OH_AVBuffer_GetAddr(input.buffer);
  const int32_t capacity = OH_AVBuffer_GetCapacity(input.buffer);
  if (!address || capacity <= 0) {
    NotifyErrorStatus({EncoderStatus::Codes::kEncoderHardwareDriverError,
                       "Codec input buffer has no memory"});
    return false;
  }

  const gfx::Size& visible_size = config_.input_visible_size;
  const size_t stride = static_cast<size_t>(input_stride_);
  const gfx::Size uv_plane_size = VideoFrame::PlaneSizeInSamples(
      PIXEL_FORMAT_NV12, VideoFrame::Plane::kUV, visible_size);

  base::CheckedNumeric<size_t> y_plane_len = stride;
  y_plane_len *= static_cast<size_t>(input_slice_height_);
  // Every UV row but the last is padded to the stride.
  base::CheckedNumeric<size_t> uv_plane_len = stride;
  uv_plane_len *= static_cast<size_t>(uv_plane_size.height() - 1);
  uv_plane_len += static_cast<size_t>(uv_plane_size.width()) * 2;
  base::CheckedNumeric<size_t> queued_size = y_plane_len;
  queued_size += uv_plane_len;
  // Codecs that publish a slice height expect the full padded frame.
  base::CheckedNumeric<size_t> full_size = y_plane_len;
  full_size *= 3u;
  full_size /= 2u;
  if (!queued_size.IsValid() || !full_size.IsValid() ||
      queued_size.ValueOrDie() > static_cast<size_t>(capacity)) {
    NotifyErrorStatus(
        {EncoderStatus::Codes::kInvalidInputFrame,
         WithCode("Frame does not fit the codec input buffer", capacity)});
    return false;
  }
  const size_t submit_size =
      std::clamp(full_size.ValueOrDie(), queued_size.ValueOrDie(),
                 static_cast<size_t>(capacity));

  const std::vector<ColorPlaneLayout> planes = {
      ColorPlaneLayout(stride, /*offset=*/0, y_plane_len.ValueOrDie()),
      ColorPlaneLayout(stride, y_plane_len.ValueOrDie(),
                       uv_plane_len.ValueOrDie()),
  };
  std::optional<VideoFrameLayout> layout = VideoFrameLayout::CreateWithPlanes(
      PIXEL_FORMAT_NV12, visible_size, planes);
  if (!layout) {
    NotifyErrorStatus({EncoderStatus::Codes::kInvalidOutputBuffer,
                       "Could not describe the codec input layout"});
    return false;
  }

  // SAFETY: AVCodecKit guarantees `capacity` bytes at `address` until the
  // index is pushed back below.
  auto codec_memory =
      UNSAFE_BUFFERS(base::span(address, static_cast<size_t>(capacity)));
  scoped_refptr<VideoFrame> destination =
      VideoFrame::WrapExternalDataWithLayout(*layout, gfx::Rect(visible_size),
                                             visible_size, codec_memory,
                                             timestamp);
  if (!destination) {
    NotifyErrorStatus({EncoderStatus::Codes::kInvalidOutputBuffer,
                       "Could not wrap the codec input buffer"});
    return false;
  }

  EncoderStatus convert_status =
      video_frame_converter_.ConvertAndScale(frame, *destination);
  if (!convert_status.is_ok()) {
    NotifyErrorStatus({EncoderStatus::Codes::kFormatConversionError,
                       std::string(convert_status.message())});
    return false;
  }

  OH_AVCodecBufferAttr attr = {};
  attr.pts = timestamp.InMicroseconds();
  attr.size = base::checked_cast<int32_t>(submit_size);
  attr.offset = 0;
  attr.flags = AVCODEC_BUFFER_FLAGS_NONE;
  if (OH_AVBuffer_SetBufferAttr(input.buffer, &attr) != AV_ERR_OK ||
      OH_VideoEncoder_PushInputBuffer(codec_.get(), input.index) != AV_ERR_OK) {
    NotifyErrorStatus({EncoderStatus::Codes::kEncoderHardwareDriverError,
                       "Could not queue an input frame"});
    return false;
  }
  return true;
}

void OhosVideoEncodeAccelerator::DrainOutput() {
  while (!error_occurred_ && !pending_outputs_.empty()) {
    const CodecBuffer output = pending_outputs_.front();
    OH_AVCodecBufferAttr attr = {};
    if (OH_AVBuffer_GetBufferAttr(output.buffer, &attr) != AV_ERR_OK) {
      NotifyErrorStatus({EncoderStatus::Codes::kEncoderHardwareDriverError,
                         "Output buffer has no attributes"});
      return;
    }

    uint8_t* address = OH_AVBuffer_GetAddr(output.buffer);
    const int32_t capacity = OH_AVBuffer_GetCapacity(output.buffer);
    base::CheckedNumeric<size_t> data_end = attr.offset;
    data_end += attr.size;
    if (attr.size > 0 &&
        (!address || capacity <= 0 || attr.offset < 0 || !data_end.IsValid() ||
         data_end.ValueOrDie() > static_cast<size_t>(capacity))) {
      NotifyErrorStatus({EncoderStatus::Codes::kEncoderFailedEncode,
                         "Output buffer attributes exceed its capacity"});
      return;
    }
    base::span<const uint8_t> payload;
    if (attr.size > 0) {
      // SAFETY: AVCodecKit guarantees `capacity` bytes at `address` until the
      // index is freed, and the attributes were checked against it above.
      payload =
          UNSAFE_BUFFERS(base::span(address, static_cast<size_t>(capacity)))
              .subspan(static_cast<size_t>(attr.offset),
                       static_cast<size_t>(attr.size));
    }

    if (attr.flags & AVCODEC_BUFFER_FLAGS_CODEC_DATA) {
      config_data_.assign(payload.begin(), payload.end());
      OH_VideoEncoder_FreeOutputBuffer(codec_.get(), output.index);
      pending_outputs_.pop_front();
      continue;
    }
    if ((attr.flags & AVCODEC_BUFFER_FLAGS_EOS) || payload.empty()) {
      OH_VideoEncoder_FreeOutputBuffer(codec_.get(), output.index);
      pending_outputs_.pop_front();
      continue;
    }
    if (available_bitstream_buffers_.empty()) {
      return;
    }
    pending_outputs_.pop_front();

    BitstreamBuffer bitstream_buffer =
        std::move(available_bitstream_buffers_.back());
    available_bitstream_buffers_.pop_back();

    const bool key_frame = attr.flags & AVCODEC_BUFFER_FLAGS_SYNC_FRAME;
    const size_t config_size = key_frame ? config_data_.size() : 0u;
    if (config_size + payload.size() > bitstream_buffer.size()) {
      OH_VideoEncoder_FreeOutputBuffer(codec_.get(), output.index);
      NotifyErrorStatus({EncoderStatus::Codes::kEncoderFailedEncode,
                         "Encoded frame is larger than the bitstream buffer"});
      return;
    }

    base::UnsafeSharedMemoryRegion region = bitstream_buffer.TakeRegion();
    base::WritableSharedMemoryMapping mapping =
        region.MapAt(bitstream_buffer.offset(), bitstream_buffer.size());
    if (!mapping.IsValid()) {
      OH_VideoEncoder_FreeOutputBuffer(codec_.get(), output.index);
      NotifyErrorStatus({EncoderStatus::Codes::kSystemAPICallError,
                         "Failed to map the bitstream buffer"});
      return;
    }
    base::span<uint8_t> destination = mapping.GetMemoryAsSpan<uint8_t>();
    if (config_size > 0) {
      destination.copy_prefix_from(config_data_);
      destination = destination.subspan(config_size);
    }
    destination.copy_prefix_from(payload);
    OH_VideoEncoder_FreeOutputBuffer(codec_.get(), output.index);

    base::TimeDelta real_timestamp = base::Microseconds(attr.pts);
    auto it = generated_to_real_timestamp_.find(attr.pts);
    if (it != generated_to_real_timestamp_.end()) {
      real_timestamp = it->second;
      // Anything older belongs to a frame the encoder dropped.
      generated_to_real_timestamp_.erase(generated_to_real_timestamp_.begin(),
                                         std::next(it));
    }

    BitstreamBufferMetadata metadata(config_size + payload.size(), key_frame,
                                     real_timestamp);
    task_runner_->PostTask(FROM_HERE,
                           base::BindOnce(&Client::BitstreamBufferReady,
                                          client_ptr_factory_->GetWeakPtr(),
                                          bitstream_buffer.id(), metadata));
  }
}

base::TimeDelta OhosVideoEncodeAccelerator::RecordFrameTimestamp(
    base::TimeDelta real_timestamp) {
  const base::TimeDelta result = next_timestamp_;
  generated_to_real_timestamp_[result.InMicroseconds()] = real_timestamp;
  next_timestamp_ += base::Seconds(1) / effective_framerate_;
  return result;
}

void OhosVideoEncodeAccelerator::NotifyErrorStatus(EncoderStatus status) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  CHECK(!status.is_ok());
  if (log_) {
    log_->NotifyError(status);
  }
  if (error_occurred_) {
    return;
  }
  error_occurred_ = true;
  if (client_ptr_factory_) {
    task_runner_->PostTask(
        FROM_HERE, base::BindOnce(&Client::NotifyErrorStatus,
                                  client_ptr_factory_->GetWeakPtr(), status));
  }
}

}  // namespace media
