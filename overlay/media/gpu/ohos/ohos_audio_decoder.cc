// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/gpu/ohos/ohos_audio_decoder.h"

#include <multimedia/native_audio_channel_layout.h>
#include <multimedia/player_framework/native_avbuffer.h>
#include <multimedia/player_framework/native_avcodec_audiocodec.h>
#include <multimedia/player_framework/native_averrors.h>
#include <multimedia/player_framework/native_avformat.h>

#include <array>
#include <utility>
#include <vector>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/numerics/checked_math.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/bind_post_task.h"
#include "media/base/audio_buffer.h"
#include "media/base/audio_codecs.h"
#include "media/base/decrypt_config.h"
#include "media/base/limits.h"
#include "media/base/waiting.h"
#include "media/gpu/ohos/ohos_cenc_info.h"
#include "media/gpu/ohos/ohos_codec_util.h"

namespace media {

namespace {

// Decode callbacks complete as soon as a buffer is inside the codec, so this
// only bounds how far the demuxer can run ahead of free codec input slots.
constexpr size_t kMaxPendingDecodes = 4;

int32_t ReadIntOr(OH_AVFormat* format, const char* key, int32_t fallback) {
  int32_t value = 0;
  if (format && OH_AVFormat_GetIntValue(format, key, &value) && value > 0) {
    return value;
  }
  return fallback;
}

// The system's channel bits are FFmpeg's (front left 1 << 0, front right
// 1 << 1, ... side right 1 << 10), so the common layouts map one to one.
// Anything else is taken as the usual layout for its channel count.
ChannelLayout ChannelLayoutFromOhos(int64_t mask, int channels) {
  struct Entry {
    uint64_t mask;
    ChannelLayout layout;
  };
  static constexpr uint64_t kFL = CH_SET_FRONT_LEFT;
  static constexpr uint64_t kFR = CH_SET_FRONT_RIGHT;
  static constexpr uint64_t kFC = CH_SET_FRONT_CENTER;
  static constexpr uint64_t kLFE = CH_SET_LOW_FREQUENCY;
  static constexpr uint64_t kBL = CH_SET_BACK_LEFT;
  static constexpr uint64_t kBR = CH_SET_BACK_RIGHT;
  static constexpr uint64_t kBC = CH_SET_BACK_CENTER;
  static constexpr uint64_t kSL = CH_SET_SIDE_LEFT;
  static constexpr uint64_t kSR = CH_SET_SIDE_RIGHT;
  static constexpr auto kLayouts = std::to_array<Entry>({
      {kFC, CHANNEL_LAYOUT_MONO},
      {kFL | kFR, CHANNEL_LAYOUT_STEREO},
      {kFL | kFR | kLFE, CHANNEL_LAYOUT_2POINT1},
      {kFL | kFR | kFC, CHANNEL_LAYOUT_SURROUND},
      {kFL | kFR | kFC | kLFE, CHANNEL_LAYOUT_3_1},
      {kFL | kFR | kFC | kBC, CHANNEL_LAYOUT_4_0},
      {kFL | kFR | kBL | kBR, CHANNEL_LAYOUT_QUAD},
      {kFL | kFR | kSL | kSR, CHANNEL_LAYOUT_2_2},
      {kFL | kFR | kFC | kSL | kSR, CHANNEL_LAYOUT_5_0},
      {kFL | kFR | kFC | kBL | kBR, CHANNEL_LAYOUT_5_0_BACK},
      {kFL | kFR | kFC | kLFE | kSL | kSR, CHANNEL_LAYOUT_5_1},
      {kFL | kFR | kFC | kLFE | kBL | kBR, CHANNEL_LAYOUT_5_1_BACK},
      {kFL | kFR | kFC | kLFE | kBL | kBR | kSL | kSR, CHANNEL_LAYOUT_7_1},
  });
  for (const Entry& entry : kLayouts) {
    if (static_cast<uint64_t>(mask) == entry.mask &&
        ChannelLayoutToChannelCount(entry.layout) == channels) {
      return entry.layout;
    }
  }
  return GuessChannelLayout(channels);
}

std::optional<SampleFormat> SampleFormatFromOhos(int32_t format) {
  switch (format) {
    case SAMPLE_S16LE:
      return kSampleFormatS16;
    case SAMPLE_S32LE:
      return kSampleFormatS32;
    case SAMPLE_F32LE:
      return kSampleFormatF32;
    default:
      return std::nullopt;
  }
}

}  // namespace

struct OhosAudioDecoder::CallbackRelay {
  CallbackRelay(scoped_refptr<base::SequencedTaskRunner> task_runner,
                base::WeakPtr<OhosAudioDecoder> decoder,
                uint32_t generation)
      : task_runner(std::move(task_runner)),
        decoder(std::move(decoder)),
        generation(generation) {}

  const scoped_refptr<base::SequencedTaskRunner> task_runner;
  // Only dereferenced on `task_runner`; copying it on codec threads is safe.
  const base::WeakPtr<OhosAudioDecoder> decoder;
  // Mirrors OhosAudioDecoder::generation_ so codec threads can read it.
  std::atomic<uint32_t> generation;
};

OhosAudioDecoder::PendingDecode::PendingDecode(
    scoped_refptr<DecoderBuffer> buffer,
    DecodeCB decode_cb)
    : buffer(std::move(buffer)), decode_cb(std::move(decode_cb)) {}
OhosAudioDecoder::PendingDecode::PendingDecode(PendingDecode&&) = default;
OhosAudioDecoder::PendingDecode& OhosAudioDecoder::PendingDecode::operator=(
    PendingDecode&&) = default;
OhosAudioDecoder::PendingDecode::~PendingDecode() = default;

void OhosAudioDecoder::CodecDeleter::operator()(OH_AVCodec* codec) const {
  OH_AudioCodec_Destroy(codec);
}

OhosAudioDecoder::OhosAudioDecoder(
    scoped_refptr<base::SequencedTaskRunner> task_runner,
    std::unique_ptr<MediaLog> media_log)
    : task_runner_(std::move(task_runner)), media_log_(std::move(media_log)) {
  DETACH_FROM_SEQUENCE(sequence_checker_);
}

OhosAudioDecoder::~OhosAudioDecoder() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  AbortPendingDecodes(DecoderStatus::Codes::kAborted);
  DestroyCodec();
}

AudioDecoderType OhosAudioDecoder::GetDecoderType() const {
  // No enum value exists for AVCodecKit; adding one means changing the mojom
  // and UMA enums, which playback does not need. OhosVideoDecoder does the
  // same.
  return AudioDecoderType::kUnknown;
}

void OhosAudioDecoder::Initialize(const AudioDecoderConfig& config,
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
  // session.
  if (config.is_encrypted() &&
      (!cdm_context || !cdm_context->GetOhosMediaKeySession())) {
    std::move(bound_init_cb)
        .Run(DecoderStatus::Codes::kUnsupportedEncryptionMode);
    return;
  }
  // Only what Chromium cannot do itself: decode AC-3 and DTS, or decrypt.
  // Clear AAC is left to the renderer's own decoders, which DecoderSelector
  // tries first anyway.
  const char* mime = OhosMimeTypeForAudioCodec(config.codec());
  if (config.codec() == AudioCodec::kAAC && config.is_encrypted()) {
    mime = OH_AVCODEC_MIMETYPE_AUDIO_AAC;
  }
  if (!mime) {
    std::move(bound_init_cb).Run(DecoderStatus::Codes::kUnsupportedConfig);
    return;
  }

  // Chromium re-initializes on a mid-stream config change; a fresh codec is
  // the simplest way to the new configuration.
  AbortPendingDecodes(DecoderStatus::Codes::kAborted);
  DestroyCodec();
  config_ = config;
  output_cb_ = output_cb;
  waiting_cb_ = waiting_cb;
  output_format_.reset();
  timestamp_helper_.reset();
  logged_first_output_ = false;
  waiting_for_key_ = false;
  cdm_event_registration_.reset();
  cdm_context_ = config.is_encrypted() ? cdm_context : nullptr;
  if (cdm_context_) {
    // CdmContext posts the event back to this sequence.
    cdm_event_registration_ = cdm_context_->RegisterEventCB(
        base::BindRepeating(&OhosAudioDecoder::OnCdmEvent,
                            weak_factory_.GetWeakPtr()));
  }

  if (!CreateCodec(mime)) {
    DestroyCodec();
    state_ = State::kUninitialized;
    LOG(WARNING) << "OHOS audio decoder: " << GetCodecName(config_.codec())
                 << " initialization failed";
    std::move(bound_init_cb)
        .Run(DecoderStatus::Codes::kFailedToCreateDecoder);
    return;
  }
  state_ = State::kDecoding;
  std::move(bound_init_cb).Run(DecoderStatus::Codes::kOk);
}

void OhosAudioDecoder::Decode(scoped_refptr<DecoderBuffer> buffer,
                              DecodeCB decode_cb) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DecodeCB bound_decode_cb =
      base::BindPostTaskToCurrentDefault(std::move(decode_cb));
  if (state_ == State::kError || state_ == State::kUninitialized ||
      pending_decodes_.size() >= kMaxPendingDecodes) {
    std::move(bound_decode_cb)
        .Run(state_ == State::kUninitialized
                 ? DecoderStatus::Codes::kNotInitialized
                 : DecoderStatus::Codes::kPlatformDecodeFailure);
    return;
  }
  pending_decodes_.emplace_back(std::move(buffer), std::move(bound_decode_cb));
  PumpInput();
}

void OhosAudioDecoder::Reset(base::OnceClosure closure) {
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

bool OhosAudioDecoder::NeedsBitstreamConversion() const {
  // AAC goes in as ADTS, as Android's MediaCodecAudioDecoder has it.
  return config_.codec() == AudioCodec::kAAC;
}

bool OhosAudioDecoder::IsPlatformDecoder() const {
  return true;
}

// static
void OhosAudioDecoder::OnCodecErrorThunk(OH_AVCodec* codec,
                                         int32_t error_code,
                                         void* user_data) {
  auto* relay = static_cast<CallbackRelay*>(user_data);
  relay->task_runner->PostTask(
      FROM_HERE, base::BindOnce(&OhosAudioDecoder::OnCodecError, relay->decoder,
                                relay->generation.load(), error_code));
}

// static
void OhosAudioDecoder::OnStreamChangedThunk(OH_AVCodec* codec,
                                            OH_AVFormat* format,
                                            void* user_data) {
  // `format` is only valid during this call; the new format is read back
  // through OH_AudioCodec_GetOutputDescription() on the decoder sequence.
  auto* relay = static_cast<CallbackRelay*>(user_data);
  relay->task_runner->PostTask(
      FROM_HERE, base::BindOnce(&OhosAudioDecoder::OnStreamChanged,
                                relay->decoder, relay->generation.load()));
}

// static
void OhosAudioDecoder::OnNeedInputBufferThunk(OH_AVCodec* codec,
                                              uint32_t index,
                                              OH_AVBuffer* buffer,
                                              void* user_data) {
  auto* relay = static_cast<CallbackRelay*>(user_data);
  relay->task_runner->PostTask(
      FROM_HERE,
      base::BindOnce(&OhosAudioDecoder::OnNeedInputBuffer, relay->decoder,
                     relay->generation.load(), CodecBuffer{index, buffer}));
}

// static
void OhosAudioDecoder::OnNewOutputBufferThunk(OH_AVCodec* codec,
                                              uint32_t index,
                                              OH_AVBuffer* buffer,
                                              void* user_data) {
  auto* relay = static_cast<CallbackRelay*>(user_data);
  relay->task_runner->PostTask(
      FROM_HERE,
      base::BindOnce(&OhosAudioDecoder::OnNewOutputBuffer, relay->decoder,
                     relay->generation.load(), CodecBuffer{index, buffer}));
}

void OhosAudioDecoder::OnCodecError(uint32_t generation, int32_t error_code) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (generation != generation_) {
    return;
  }
  MEDIA_LOG(ERROR, media_log_) << "AVCodecKit audio decoder error "
                               << error_code;
  EnterErrorState("codec reported an error");
}

void OhosAudioDecoder::OnStreamChanged(uint32_t generation) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (generation != generation_) {
    return;
  }
  output_format_.reset();
}

void OhosAudioDecoder::OnNeedInputBuffer(uint32_t generation,
                                         CodecBuffer input) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (generation != generation_) {
    return;
  }
  free_inputs_.push_back(input);
  PumpInput();
}

void OhosAudioDecoder::OnNewOutputBuffer(uint32_t generation,
                                         CodecBuffer output) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // A stale index belongs to a destroyed codec or was invalidated by a flush;
  // handing it back would be rejected, so it is simply forgotten.
  if (generation != generation_ || !codec_) {
    return;
  }

  OH_AVCodecBufferAttr attr = {};
  if (OH_AVBuffer_GetBufferAttr(output.buffer, &attr) != AV_ERR_OK) {
    OH_AudioCodec_FreeOutputBuffer(codec_.get(), output.index);
    EnterErrorState("output buffer has no attributes");
    return;
  }

  if (attr.flags & AVCODEC_BUFFER_FLAGS_EOS) {
    OH_AudioCodec_FreeOutputBuffer(codec_.get(), output.index);
    if (state_ != State::kDraining) {
      return;
    }
    // The codec takes no input after end of stream; restart it so decoding
    // can go on without a Reset(), as the AudioDecoder contract allows.
    if (!FlushAndRestartCodec()) {
      EnterErrorState("restart after end of stream failed");
      return;
    }
    state_ = State::kDecoding;
    std::move(eos_decode_cb_).Run(DecoderStatus::Codes::kOk);
    PumpInput();
    return;
  }

  if (state_ != State::kError && attr.size > 0 &&
      !(attr.flags & AVCODEC_BUFFER_FLAGS_CODEC_DATA)) {
    DeliverOutput(attr, output.buffer);
  }
  if (codec_) {
    OH_AudioCodec_FreeOutputBuffer(codec_.get(), output.index);
  }
}

bool OhosAudioDecoder::CreateCodec(const char* mime) {
  if (!CanUseOhosNativeCodecs()) {
    return false;
  }
  codec_.reset(OH_AudioCodec_CreateByMime(mime, /*isEncoder=*/false));
  if (!codec_) {
    MEDIA_LOG(ERROR, media_log_) << "OH_AudioCodec_CreateByMime failed";
    return false;
  }

  ++generation_;
  relay_ = std::make_unique<CallbackRelay>(
      task_runner_, weak_factory_.GetWeakPtr(), generation_);
  OH_AVCodecCallback callbacks = {
      &OhosAudioDecoder::OnCodecErrorThunk,
      &OhosAudioDecoder::OnStreamChangedThunk,
      &OhosAudioDecoder::OnNeedInputBufferThunk,
      &OhosAudioDecoder::OnNewOutputBufferThunk,
  };
  if (OH_AudioCodec_RegisterCallback(codec_.get(), callbacks, relay_.get()) !=
      AV_ERR_OK) {
    MEDIA_LOG(ERROR, media_log_) << "OH_AudioCodec_RegisterCallback failed";
    return false;
  }

  // Interleaved 16-bit is asked for first; a decoder that will not take the
  // request gets its own default instead. Either way the format actually
  // produced is read back from the output description.
  OH_AVErrCode result = AV_ERR_UNKNOWN;
  for (const bool ask_for_s16 : {true, false}) {
    ScopedOhosAVFormat format(OH_AVFormat_Create());
    if (!format) {
      return false;
    }
    OH_AVFormat_SetIntValue(format.get(), OH_MD_KEY_AUD_CHANNEL_COUNT,
                            config_.channels());
    OH_AVFormat_SetIntValue(format.get(), OH_MD_KEY_AUD_SAMPLE_RATE,
                            config_.samples_per_second());
    if (ask_for_s16) {
      OH_AVFormat_SetIntValue(format.get(), OH_MD_KEY_AUDIO_SAMPLE_FORMAT,
                              SAMPLE_S16LE);
    }
    if (config_.codec() == AudioCodec::kAAC) {
      // Samples come with ADTS headers (NeedsBitstreamConversion()), except
      // xHE-AAC, which ADTS cannot describe; the AudioSpecificConfig is
      // passed either way.
      OH_AVFormat_SetIntValue(
          format.get(), OH_MD_KEY_AAC_IS_ADTS,
          config_.profile() == AudioCodecProfile::kXHE_AAC ? 0 : 1);
      std::vector<uint8_t> extra_data = config_.extra_data();
      if (!extra_data.empty()) {
        OH_AVFormat_SetBuffer(format.get(), OH_MD_KEY_CODEC_CONFIG,
                              extra_data.data(), extra_data.size());
      }
    }
    result = OH_AudioCodec_Configure(codec_.get(), format.get());
    if (result == AV_ERR_OK) {
      break;
    }
    OH_AudioCodec_Reset(codec_.get());
  }
  if (result != AV_ERR_OK) {
    MEDIA_LOG(ERROR, media_log_)
        << "OH_AudioCodec_Configure failed: "
        << base::NumberToString(static_cast<int>(result));
    return false;
  }
  if (cdm_context_) {
    // Must come before Prepare(). Software crypto only: the output stays in
    // ordinary memory.
    result = OH_AudioCodec_SetDecryptionConfig(
        codec_.get(), cdm_context_->GetOhosMediaKeySession(),
        /*secureAudio=*/false);
    if (result != AV_ERR_OK) {
      MEDIA_LOG(ERROR, media_log_)
          << "OH_AudioCodec_SetDecryptionConfig failed: "
          << base::NumberToString(static_cast<int>(result));
      return false;
    }
  }
  if (OH_AudioCodec_Prepare(codec_.get()) != AV_ERR_OK ||
      OH_AudioCodec_Start(codec_.get()) != AV_ERR_OK) {
    MEDIA_LOG(ERROR, media_log_) << "OH_AudioCodec_Prepare/Start failed";
    return false;
  }
  LOG(WARNING) << "OHOS audio decoder: " << GetCodecName(config_.codec())
               << " " << config_.channels() << " ch "
               << config_.samples_per_second() << " Hz on the system decoder"
               << (cdm_context_ ? ", decrypting" : "");
  return true;
}

void OhosAudioDecoder::DestroyCodec() {
  // Invalidate before destroying so that callbacks already posted by this
  // codec are ignored even if a new codec reuses their indices.
  ++generation_;
  free_inputs_.clear();
  codec_.reset();
  relay_.reset();
  output_format_.reset();
}

bool OhosAudioDecoder::FlushAndRestartCodec() {
  if (!codec_) {
    return false;
  }
  // No callback fires between Flush() and Start(), so bumping the generation
  // in between cleanly separates the callbacks from before and after it.
  if (OH_AudioCodec_Flush(codec_.get()) != AV_ERR_OK) {
    return false;
  }
  ++generation_;
  relay_->generation.store(generation_);
  free_inputs_.clear();
  timestamp_helper_.reset();
  return OH_AudioCodec_Start(codec_.get()) == AV_ERR_OK;
}

void OhosAudioDecoder::PumpInput() {
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

bool OhosAudioDecoder::QueueInput(const CodecBuffer& input,
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
          << "Audio input of " << buffer.size()
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
      MEDIA_LOG(ERROR, media_log_) << "Unsupported audio encryption layout";
      return false;
    }
    if (!timestamp_helper_) {
      timestamp_sample_rate_ = output_format_ ? output_format_->sample_rate
                                              : config_.samples_per_second();
      timestamp_helper_ =
          std::make_unique<AudioTimestampHelper>(timestamp_sample_rate_);
      timestamp_helper_->SetBaseTimestamp(buffer.timestamp());
    }
  }
  if (OH_AVBuffer_SetBufferAttr(input.buffer, &attr) != AV_ERR_OK) {
    return false;
  }
  return OH_AudioCodec_PushInputBuffer(codec_.get(), input.index) ==
         AV_ERR_OK;
}

void OhosAudioDecoder::OnCdmEvent(CdmContext::Event event) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (event != CdmContext::Event::kHasAdditionalUsableKey ||
      !waiting_for_key_) {
    return;
  }
  waiting_for_key_ = false;
  PumpInput();
}

bool OhosAudioDecoder::UpdateOutputFormat() {
  ScopedOhosAVFormat format(OH_AudioCodec_GetOutputDescription(codec_.get()));
  const int channels =
      ReadIntOr(format.get(), OH_MD_KEY_AUD_CHANNEL_COUNT, config_.channels());
  const int sample_rate = ReadIntOr(format.get(), OH_MD_KEY_AUD_SAMPLE_RATE,
                                    config_.samples_per_second());
  int32_t raw_format = SAMPLE_S16LE;
  if (format) {
    OH_AVFormat_GetIntValue(format.get(), OH_MD_KEY_AUDIO_SAMPLE_FORMAT,
                            &raw_format);
  }
  const std::optional<SampleFormat> sample_format =
      SampleFormatFromOhos(raw_format);
  int64_t mask = 0;
  if (format) {
    OH_AVFormat_GetLongValue(format.get(), OH_MD_KEY_CHANNEL_LAYOUT, &mask);
  }
  if (!sample_format || channels <= 0 || channels > limits::kMaxChannels ||
      sample_rate < limits::kMinSampleRate ||
      sample_rate > limits::kMaxSampleRate) {
    MEDIA_LOG(ERROR, media_log_)
        << "Unusable audio decoder output: format " << raw_format << ", "
        << channels << " ch, " << sample_rate << " Hz";
    return false;
  }
  output_format_ = OutputFormat{*sample_format,
                                ChannelLayoutFromOhos(mask, channels),
                                channels, sample_rate};
  if (timestamp_helper_ &&
      timestamp_sample_rate_ != output_format_->sample_rate) {
    // The first output may report another rate than the container did;
    // timestamps run on from the same base at the rate actually produced.
    const base::TimeDelta base = timestamp_helper_->GetTimestamp();
    timestamp_helper_ =
        std::make_unique<AudioTimestampHelper>(output_format_->sample_rate);
    timestamp_helper_->SetBaseTimestamp(base);
    timestamp_sample_rate_ = output_format_->sample_rate;
  }
  return true;
}

void OhosAudioDecoder::DeliverOutput(const OH_AVCodecBufferAttr& attr,
                                     OH_AVBuffer* buffer) {
  if (!output_format_ && !UpdateOutputFormat()) {
    EnterErrorState("unusable output format");
    return;
  }
  const OutputFormat& out = *output_format_;
  uint8_t* address = OH_AVBuffer_GetAddr(buffer);
  const int32_t capacity = OH_AVBuffer_GetCapacity(buffer);
  base::CheckedNumeric<size_t> end = attr.offset;
  end += attr.size;
  if (!address || capacity <= 0 || attr.offset < 0 || !end.IsValid() ||
      end.ValueOrDie() > static_cast<size_t>(capacity)) {
    EnterErrorState("output buffer out of range");
    return;
  }
  const size_t bytes_per_frame =
      static_cast<size_t>(SampleFormatToBytesPerChannel(out.sample_format)) *
      static_cast<size_t>(out.channels);
  const size_t frames = static_cast<size_t>(attr.size) / bytes_per_frame;
  if (frames == 0) {
    return;
  }
  // SAFETY: AVCodecKit guarantees `capacity` bytes at `address` until the
  // output index is freed, which happens after this copy.
  auto data =
      UNSAFE_BUFFERS(base::span(address, static_cast<size_t>(capacity)))
          .subspan(static_cast<size_t>(attr.offset), frames * bytes_per_frame);

  if (!timestamp_helper_) {
    // Output without input since the last reset: start from the codec's own
    // time rather than from zero.
    timestamp_sample_rate_ = out.sample_rate;
    timestamp_helper_ =
        std::make_unique<AudioTimestampHelper>(timestamp_sample_rate_);
    timestamp_helper_->SetBaseTimestamp(base::Microseconds(attr.pts));
  }
  const std::array<base::span<const uint8_t>, 1> planes = {data};
  scoped_refptr<AudioBuffer> audio_buffer = AudioBuffer::CopyFrom(
      out.sample_format, out.channel_layout, out.channels, out.sample_rate,
      static_cast<int>(frames), planes, timestamp_helper_->GetTimestamp());
  timestamp_helper_->AddFrames(static_cast<int>(frames));
  if (!audio_buffer) {
    EnterErrorState("could not copy decoded audio");
    return;
  }
  if (!logged_first_output_) {
    logged_first_output_ = true;
    LOG(WARNING) << "OHOS audio decoder: first output "
                 << SampleFormatToString(out.sample_format) << " "
                 << ChannelLayoutToString(out.channel_layout) << " "
                 << out.channels << " ch " << out.sample_rate << " Hz";
  }
  output_cb_.Run(std::move(audio_buffer));
}

void OhosAudioDecoder::EnterErrorState(const char* reason) {
  LOG(ERROR) << "OhosAudioDecoder error: " << reason;
  MEDIA_LOG(ERROR, media_log_) << "OhosAudioDecoder: " << reason;
  state_ = State::kError;
  DestroyCodec();
  AbortPendingDecodes(DecoderStatus::Codes::kPlatformDecodeFailure);
}

void OhosAudioDecoder::AbortPendingDecodes(DecoderStatus status) {
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
