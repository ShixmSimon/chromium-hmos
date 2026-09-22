// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MEDIA_AUDIO_OHOS_OHOS_AUDIO_INPUT_H_
#define MEDIA_AUDIO_OHOS_OHOS_AUDIO_INPUT_H_

#include <ohaudio/native_audiocapturer.h>
#include <stdint.h>

#include <memory>
#include <string>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/synchronization/lock.h"
#include "base/thread_annotations.h"
#include "base/time/time.h"
#include "media/audio/audio_io.h"
#include "media/base/audio_parameters.h"

namespace media {

class AudioBlockFifo;
class AudioManagerBase;

class OhosAudioInputStream final : public AudioInputStream {
 public:
  OhosAudioInputStream(AudioManagerBase* audio_manager,
                       const AudioParameters& params);

  OhosAudioInputStream(const OhosAudioInputStream&) = delete;
  OhosAudioInputStream& operator=(const OhosAudioInputStream&) = delete;

  ~OhosAudioInputStream() override;

  OpenOutcome Open() override;
  void Start(AudioInputCallback* callback) override;
  void Stop() override;
  void Close() override;
  double GetMaxVolume() override;
  void SetVolume(double volume) override;
  double GetVolume() override;
  bool SetAutomaticGainControl(bool enabled) override;
  bool GetAutomaticGainControl() override;
  bool IsMuted() override;
  void SetOutputDeviceForAec(const std::string& output_device_id) override;

 private:
  static int32_t OnReadData(OH_AudioCapturer* capturer,
                            void* user_data,
                            void* buffer,
                            int32_t length);
  static int32_t OnStreamEvent(OH_AudioCapturer* capturer,
                               void* user_data,
                               OH_AudioStream_Event event);
  static int32_t OnInterruptEvent(OH_AudioCapturer* capturer,
                                  void* user_data,
                                  OH_AudioInterrupt_ForceType type,
                                  OH_AudioInterrupt_Hint hint);
  static int32_t OnError(OH_AudioCapturer* capturer,
                         void* user_data,
                         OH_AudioStream_Result error);

  void StartWhenMicrophonePermissionIsReady();
  bool InitializeCapturer();
  int32_t CaptureData(OH_AudioCapturer* capturer, void* buffer, int32_t length);
  void ReportError(AudioInputCallback::Error error);
  void ReleaseCapturer();

  SEQUENCE_CHECKER(sequence_checker_);

  const raw_ptr<AudioManagerBase> audio_manager_;
  const AudioParameters params_;
  OH_AudioCapturer* capturer_ = nullptr;
  std::unique_ptr<AudioBlockFifo> audio_fifo_;
  base::TimeTicks permission_wait_deadline_;
  bool opened_ = false;
  bool started_ = false;
  bool automatic_gain_control_ = false;

  base::Lock lock_;
  raw_ptr<AudioInputCallback> callback_ GUARDED_BY(lock_) = nullptr;
  double volume_ GUARDED_BY(lock_) = 1.0;
  base::TimeTicks last_capture_time_ GUARDED_BY(lock_);

  base::WeakPtrFactory<OhosAudioInputStream> weak_factory_{this};
};

}  // namespace media

#endif  // MEDIA_AUDIO_OHOS_OHOS_AUDIO_INPUT_H_
