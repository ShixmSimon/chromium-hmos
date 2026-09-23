// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/gpu/ohos/ohos_video_decoder.h"

#include <multimedia/player_framework/native_avbuffer.h>
#include <multimedia/player_framework/native_avcodec_videodecoder.h>
#include <multimedia/player_framework/native_averrors.h>
#include <multimedia/player_framework/native_avformat.h>

#include <algorithm>
#include <atomic>
#include <string>
#include <utility>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/numerics/checked_math.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/bind_post_task.h"
#include "base/time/time.h"
#include "media/base/video_frame.h"
#include "media/base/video_types.h"
#include "media/gpu/ohos/ohos_codec_util.h"
#include "third_party/libyuv/include/libyuv/planar_functions.h"
#include "ui/gfx/color_space.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"

namespace media {

namespace {

// Decode callbacks complete as soon as a buffer is inside the codec, so this
// only bounds how far the demuxer can run ahead of free codec input slots.
constexpr int kMaxDecodeRequests = 4;

int32_t ReadIntOr(OH_AVFormat* format, const char* key, int32_t fallback) {
  int32_t value = 0;
  if (format && OH_AVFormat_GetIntValue(format, key, &value) && value > 0) {
    return value;
  }
  return fallback;
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
    SupportedVideoDecoderConfigs supported_configs)
    : task_runner_(std::move(task_runner)),
      media_log_(std::move(media_log)),
      supported_configs_(std::move(supported_configs)) {
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
  if (config.is_encrypted()) {
    std::move(bound_init_cb)
        .Run(DecoderStatus::Codes::kUnsupportedEncryptionMode);
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
  output_layout_.reset();

  DecoderStatus status = CreateCodec();
  if (!status.is_ok()) {
    MEDIA_LOG(ERROR, media_log_)
        << "AVCodecKit decoder creation failed: " << status.message();
    DestroyCodec();
    state_ = State::kUninitialized;
    std::move(bound_init_cb).Run(std::move(status));
    return;
  }
  state_ = State::kDecoding;
  std::move(bound_init_cb).Run(DecoderStatus::Codes::kOk);
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
  return true;
}

bool OhosVideoDecoder::CanReadWithoutStalling() const {
  // Output is copied out and returned to the codec at once, so no decoded
  // picture is ever held against the renderer.
  return true;
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
    OnDrainComplete();
    return;
  }

  if (state_ == State::kError) {
    OH_VideoDecoder_FreeOutputBuffer(codec_.get(), output.index);
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

  ++generation_;
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
  OH_AVFormat_SetIntValue(format.get(), OH_MD_KEY_PIXEL_FORMAT,
                          AV_PIXEL_FORMAT_NV12);

  OH_AVErrCode result = OH_VideoDecoder_Configure(codec_.get(), format.get());
  if (result != AV_ERR_OK) {
    return {DecoderStatus::Codes::kUnsupportedConfig,
            "OH_VideoDecoder_Configure failed: " +
                base::NumberToString(static_cast<int>(result))};
  }
  if (OH_VideoDecoder_Prepare(codec_.get()) != AV_ERR_OK ||
      OH_VideoDecoder_Start(codec_.get()) != AV_ERR_OK) {
    return {DecoderStatus::Codes::kFailedToCreateDecoder,
            "OH_VideoDecoder_Prepare/Start failed"};
  }
  return DecoderStatus::Codes::kOk;
}

void OhosVideoDecoder::DestroyCodec() {
  // Invalidate before destroying so that callbacks already posted by this
  // codec are ignored even if a new codec reuses their indices.
  ++generation_;
  free_inputs_.clear();
  codec_.reset();
  relay_.reset();
}

bool OhosVideoDecoder::FlushAndRestartCodec() {
  if (!codec_) {
    return false;
  }
  // No callback fires between Flush() and Start(), so bumping the generation
  // in between cleanly separates pre-flush callbacks from post-flush ones.
  if (OH_VideoDecoder_Flush(codec_.get()) != AV_ERR_OK) {
    return false;
  }
  ++generation_;
  relay_->generation.store(generation_);
  free_inputs_.clear();
  return OH_VideoDecoder_Start(codec_.get()) == AV_ERR_OK;
}

void OhosVideoDecoder::PumpInput() {
  while (state_ == State::kDecoding && !pending_decodes_.empty() &&
         !free_inputs_.empty()) {
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
  }
  if (OH_AVBuffer_SetBufferAttr(input.buffer, &attr) != AV_ERR_OK) {
    return false;
  }
  return OH_VideoDecoder_PushInputBuffer(codec_.get(), input.index) ==
         AV_ERR_OK;
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
  if (width <= 0 || height <= 0 || stride < width || slice_height < height) {
    MEDIA_LOG(ERROR, media_log_)
        << "Unusable decoder output layout " << width << "x" << height
        << " stride " << stride << " slice height " << slice_height;
    return false;
  }
  output_layout_ = OutputLayout{width, height, stride, slice_height};
  return true;
}

scoped_refptr<VideoFrame> OhosVideoDecoder::CopyOutput(
    OH_AVBuffer* buffer,
    const OH_AVCodecBufferAttr& attr) {
  if (!output_layout_ && !UpdateOutputLayout()) {
    return nullptr;
  }
  const OutputLayout& layout = *output_layout_;

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
  const size_t uv_row_bytes = static_cast<size_t>((layout.width + 1) / 2) * 2;
  base::CheckedNumeric<size_t> uv_offset = stride;
  uv_offset *= static_cast<size_t>(layout.slice_height);
  base::CheckedNumeric<size_t> required = stride;
  required *= uv_rows - 1;
  required += uv_row_bytes;
  required += uv_offset;
  if (!required.IsValid() || required.ValueOrDie() > data.size()) {
    MEDIA_LOG(ERROR, media_log_) << "Decoded picture of " << data.size()
                                 << " bytes is smaller than its layout needs";
    return nullptr;
  }

  const gfx::Size visible_size(layout.width, layout.height);
  const gfx::Rect visible_rect(visible_size);
  // NV12 chroma is subsampled 2x2, so the backing store must be even.
  const gfx::Size coded_size((layout.width + 1) & ~1, (layout.height + 1) & ~1);
  scoped_refptr<VideoFrame> frame = VideoFrame::CreateFrame(
      PIXEL_FORMAT_NV12, coded_size, visible_rect,
      config_.aspect_ratio().GetNaturalSize(visible_rect),
      base::Microseconds(attr.pts));
  if (!frame) {
    return nullptr;
  }

  const auto uv_plane = data.subspan(uv_offset.ValueOrDie());
  if (libyuv::NV12Copy(data.data(), layout.stride, uv_plane.data(),
                       layout.stride,
                       frame->writable_data(VideoFrame::Plane::kY),
                       static_cast<int>(frame->stride(VideoFrame::Plane::kY)),
                       frame->writable_data(VideoFrame::Plane::kUV),
                       static_cast<int>(frame->stride(VideoFrame::Plane::kUV)),
                       layout.width, layout.height) != 0) {
    return nullptr;
  }

  const gfx::ColorSpace color_space =
      config_.color_space_info().ToGfxColorSpace();
  if (color_space.IsValid()) {
    frame->set_color_space(color_space);
  }
  // MojoVideoDecoderService DCHECKs this for every frame it ships.
  frame->metadata().power_efficient = true;
  return frame;
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
  DVLOG(1) << "OhosVideoDecoder error: " << reason;
  MEDIA_LOG(ERROR, media_log_) << "OhosVideoDecoder: " << reason;
  state_ = State::kError;
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
