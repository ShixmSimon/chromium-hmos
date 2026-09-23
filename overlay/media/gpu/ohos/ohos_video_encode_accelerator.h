// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MEDIA_GPU_OHOS_OHOS_VIDEO_ENCODE_ACCELERATOR_H_
#define MEDIA_GPU_OHOS_OHOS_VIDEO_ENCODE_ACCELERATOR_H_

#include <multimedia/player_framework/native_avcodec_base.h>
#include <stdint.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/containers/circular_deque.h"
#include "base/containers/flat_map.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "media/base/bitrate.h"
#include "media/base/bitstream_buffer.h"
#include "media/base/media_log.h"
#include "media/base/video_encoder.h"
#include "media/base/video_frame.h"
#include "media/base/video_frame_converter.h"
#include "media/gpu/media_gpu_export.h"
#include "media/video/video_encode_accelerator.h"

namespace media {

// H.264 encoder on top of an AVCodecKit hardware encoder in buffer mode.
// Modelled on NdkVideoEncodeAccelerator: every input frame, whatever its
// format, is converted into the codec's NV12 input buffer with libyuv (via
// VideoFrameConverter), and SPS/PPS from the codec-config output are prepended
// to each key frame so every key frame is independently decodable.
class MEDIA_GPU_EXPORT OhosVideoEncodeAccelerator final
    : public VideoEncodeAccelerator {
 public:
  // All calls and client callbacks happen on `task_runner`.
  explicit OhosVideoEncodeAccelerator(
      scoped_refptr<base::SequencedTaskRunner> task_runner);
  OhosVideoEncodeAccelerator(const OhosVideoEncodeAccelerator&) = delete;
  OhosVideoEncodeAccelerator& operator=(const OhosVideoEncodeAccelerator&) =
      delete;

  // VideoEncodeAccelerator implementation.
  SupportedProfiles GetSupportedProfiles() override;
  EncoderStatus Initialize(const Config& config,
                           Client* client,
                           std::unique_ptr<MediaLog> media_log) override;
  void Encode(scoped_refptr<VideoFrame> frame, bool force_keyframe) override;
  void Encode(scoped_refptr<VideoFrame> frame,
              const VideoEncoder::EncodeOptions& options) override;
  void UseOutputBitstreamBuffer(BitstreamBuffer buffer) override;
  void RequestEncodingParametersChange(
      const Bitrate& bitrate,
      uint32_t framerate,
      const std::optional<gfx::Size>& size) override;
  void Destroy() override;
  bool IsFlushSupported() override;

 private:
  struct CallbackRelay;

  struct CodecDeleter {
    void operator()(OH_AVCodec* codec) const;
  };

  // A buffer AVCodecKit lent us through a callback. Bound into tasks as a
  // struct so the opaque OH_AVBuffer pointer is not a bare bound pointer.
  struct CodecBuffer {
    uint32_t index;
    // Owned by the codec; valid until the index is pushed or freed.
    RAW_PTR_EXCLUSION OH_AVBuffer* buffer;
  };

  struct PendingEncode {
    PendingEncode(scoped_refptr<VideoFrame> frame, bool key_frame);
    PendingEncode(PendingEncode&&);
    PendingEncode& operator=(PendingEncode&&);
    ~PendingEncode();

    scoped_refptr<VideoFrame> frame;
    bool key_frame;
  };

  ~OhosVideoEncodeAccelerator() override;

  // AVCodecKit invokes these on its own threads. They only post to
  // `task_runner_`; `user_data` is the CallbackRelay.
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

  void OnCodecError(int32_t error_code);
  void OnNeedInputBuffer(CodecBuffer input);
  void OnNewOutputBuffer(CodecBuffer output);

  EncoderStatus CreateCodec();
  void ReadInputLayout();
  void NotifyEncoderInfo(const std::string& codec_name);
  void FeedInput();
  bool FeedInputBuffer(const CodecBuffer& input,
                       const VideoFrame& frame,
                       base::TimeDelta timestamp);
  void DrainOutput();
  // Generates a timestamp that advances at the configured frame rate, since
  // the encoder's rate control reads pts and real capture timestamps jitter.
  base::TimeDelta RecordFrameTimestamp(base::TimeDelta real_timestamp);
  void NotifyErrorStatus(EncoderStatus status);

  const scoped_refptr<base::SequencedTaskRunner> task_runner_;

  // Client callbacks go through this; reset on Destroy() so that no callback
  // reaches a client that has gone away.
  std::unique_ptr<base::WeakPtrFactory<Client>> client_ptr_factory_;
  std::unique_ptr<MediaLog> log_;
  Config config_;
  bool error_occurred_ = false;
  Bitrate effective_bitrate_;
  uint32_t effective_framerate_ = 0;

  // Declared before `codec_` so it is destroyed after it: the codec's threads
  // dereference the relay until OH_VideoEncoder_Destroy() returns.
  std::unique_ptr<CallbackRelay> relay_;
  std::unique_ptr<OH_AVCodec, CodecDeleter> codec_;

  // NV12 layout of the codec's input buffers.
  int32_t input_stride_ = 0;
  int32_t input_slice_height_ = 0;

  base::circular_deque<CodecBuffer> free_inputs_;
  base::circular_deque<PendingEncode> pending_frames_;
  // Encoded outputs held in the codec until the client supplies a bitstream
  // buffer to copy them into.
  base::circular_deque<CodecBuffer> pending_outputs_;
  std::vector<BitstreamBuffer> available_bitstream_buffers_;

  // SPS and PPS from the codec-config output.
  std::vector<uint8_t> config_data_;

  base::TimeDelta next_timestamp_;
  base::flat_map<int64_t, base::TimeDelta> generated_to_real_timestamp_;

  VideoFrameConverter video_frame_converter_;

  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<OhosVideoEncodeAccelerator> weak_factory_{this};
};

}  // namespace media

#endif  // MEDIA_GPU_OHOS_OHOS_VIDEO_ENCODE_ACCELERATOR_H_
