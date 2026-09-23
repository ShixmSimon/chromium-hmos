// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MEDIA_GPU_OHOS_OHOS_VIDEO_DECODER_H_
#define MEDIA_GPU_OHOS_OHOS_VIDEO_DECODER_H_

#include <multimedia/player_framework/native_avcodec_base.h>
#include <stdint.h>

#include <memory>
#include <optional>

#include "base/containers/circular_deque.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/task/sequenced_task_runner.h"
#include "media/base/decoder_buffer.h"
#include "media/base/media_log.h"
#include "media/base/supported_video_decoder_config.h"
#include "media/base/video_decoder.h"
#include "media/base/video_decoder_config.h"
#include "media/gpu/media_gpu_export.h"

namespace media {

class VideoFrame;

// Decodes through an AVCodecKit hardware decoder in buffer mode and copies
// each NV12 picture into a VideoFrame in system memory.
//
// Buffer mode, not surface mode, because the port has no NativePixmap or
// GpuMemoryBuffer support to wrap an OH_NativeBuffer as a SharedImage.
// Memory-backed frames cross to the renderer as shared memory
// (media/mojo/mojom/video_frame_mojom_traits.cc), which uploads them the same
// way it uploads FFmpeg output.
class MEDIA_GPU_EXPORT OhosVideoDecoder final : public VideoDecoder {
 public:
  OhosVideoDecoder(scoped_refptr<base::SequencedTaskRunner> task_runner,
                   std::unique_ptr<MediaLog> media_log,
                   SupportedVideoDecoderConfigs supported_configs);
  OhosVideoDecoder(const OhosVideoDecoder&) = delete;
  OhosVideoDecoder& operator=(const OhosVideoDecoder&) = delete;
  ~OhosVideoDecoder() override;

  // VideoDecoder implementation.
  void Initialize(const VideoDecoderConfig& config,
                  bool low_delay,
                  CdmContext* cdm_context,
                  InitCB init_cb,
                  const OutputCB& output_cb,
                  const WaitingCB& waiting_cb) override;
  void Decode(scoped_refptr<DecoderBuffer> buffer, DecodeCB decode_cb) override;
  void Reset(base::OnceClosure closure) override;
  bool NeedsBitstreamConversion() const override;
  bool CanReadWithoutStalling() const override;
  int GetMaxDecodeRequests() const override;
  bool IsPlatformDecoder() const override;
  VideoDecoderType GetDecoderType() const override;

 private:
  struct CallbackRelay;

  struct PendingDecode {
    PendingDecode(scoped_refptr<DecoderBuffer> buffer, DecodeCB decode_cb);
    PendingDecode(PendingDecode&&);
    PendingDecode& operator=(PendingDecode&&);
    ~PendingDecode();

    scoped_refptr<DecoderBuffer> buffer;
    DecodeCB decode_cb;
  };

  struct CodecDeleter {
    void operator()(OH_AVCodec* codec) const;
  };

  // A buffer AVCodecKit lent us through a callback. Bound into tasks as a
  // struct so the opaque OH_AVBuffer pointer is not a bare bound pointer.
  struct CodecBuffer {
    uint32_t index;
    // Owned by the codec; valid until the index is pushed or freed, or the
    // codec is flushed, which invalidates every outstanding index.
    RAW_PTR_EXCLUSION OH_AVBuffer* buffer;
  };

  struct OutputLayout {
    int32_t width;
    int32_t height;
    int32_t stride;
    int32_t slice_height;
  };

  enum class State {
    kUninitialized,
    kDecoding,
    // An end-of-stream buffer is in the codec; its decode callback runs once
    // the codec hands back the matching end-of-stream output.
    kDraining,
    kError,
  };

  // AVCodecKit invokes these on its own threads. They only post to
  // `task_runner_`; `user_data` is the CallbackRelay of the codec instance.
  static void OnCodecErrorThunk(OH_AVCodec* codec,
                                int32_t error_code,
                                void* user_data);
  static void OnStreamChangedThunk(OH_AVCodec* codec,
                                   OH_AVFormat* format,
                                   void* user_data);
  static void OnNeedInputBufferThunk(OH_AVCodec* codec,
                                     uint32_t index,
                                     OH_AVBuffer* buffer,
                                     void* user_data);
  static void OnNewOutputBufferThunk(OH_AVCodec* codec,
                                     uint32_t index,
                                     OH_AVBuffer* buffer,
                                     void* user_data);

  void OnCodecError(uint32_t generation, int32_t error_code);
  void OnStreamChanged(uint32_t generation);
  void OnNeedInputBuffer(uint32_t generation, CodecBuffer input);
  void OnNewOutputBuffer(uint32_t generation, CodecBuffer output);

  DecoderStatus CreateCodec();
  void DestroyCodec();
  // Drops every buffer index the codec has handed out and restarts it. Used
  // after end of stream (the codec takes no input in that state) and on
  // Reset().
  bool FlushAndRestartCodec();
  void PumpInput();
  bool QueueInput(const CodecBuffer& input, const DecoderBuffer& buffer);
  bool UpdateOutputLayout();
  scoped_refptr<VideoFrame> CopyOutput(OH_AVBuffer* buffer,
                                       const OH_AVCodecBufferAttr& attr);
  void OnDrainComplete();
  void EnterErrorState(const char* reason);
  void AbortPendingDecodes(DecoderStatus status);

  const scoped_refptr<base::SequencedTaskRunner> task_runner_;
  const std::unique_ptr<MediaLog> media_log_;
  const SupportedVideoDecoderConfigs supported_configs_;

  State state_ = State::kUninitialized;
  VideoDecoderConfig config_;
  OutputCB output_cb_;

  // Identifies the current codec instance and flush epoch. Every callback
  // carries the value current when AVCodecKit invoked it, so tasks that refer
  // to a destroyed codec or to indices a flush invalidated are dropped.
  uint32_t generation_ = 0;
  // Declared before `codec_` so it is destroyed after it: the codec's threads
  // dereference the relay until OH_VideoDecoder_Destroy() returns.
  std::unique_ptr<CallbackRelay> relay_;
  std::unique_ptr<OH_AVCodec, CodecDeleter> codec_;

  base::circular_deque<CodecBuffer> free_inputs_;
  base::circular_deque<PendingDecode> pending_decodes_;
  DecodeCB eos_decode_cb_;
  std::optional<OutputLayout> output_layout_;

  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<OhosVideoDecoder> weak_factory_{this};
};

}  // namespace media

#endif  // MEDIA_GPU_OHOS_OHOS_VIDEO_DECODER_H_
