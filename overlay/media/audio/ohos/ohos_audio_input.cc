// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/audio/ohos/ohos_audio_input.h"

#include <dlfcn.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include <ohaudio/native_audiostreambuilder.h>

#include "base/check.h"
#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/task/sequenced_task_runner.h"
#include "media/audio/audio_manager_base.h"
#include "media/base/audio_block_fifo.h"
#include "media/base/audio_timestamp_helper.h"
#include "media/base/sample_format.h"

namespace media {

namespace {

constexpr char kMicrophonePermission[] = "ohos.permission.MICROPHONE";
constexpr base::TimeDelta kPermissionPollInterval = base::Milliseconds(100);
constexpr base::TimeDelta kPermissionWaitTimeout = base::Seconds(60);
constexpr int kFifoBlocks = 8;
constexpr double kVolumeEpsilon = 1e-6;

bool IsSuccess(OH_AudioStream_Result result) {
  return result == AUDIOSTREAM_SUCCESS;
}

bool HasMicrophonePermission() {
  using CheckSelfPermission = bool (*)(const char*);
  static CheckSelfPermission check_permission = []() {
    void* library =
        dlopen("libability_access_control.so", RTLD_NOW | RTLD_LOCAL);
    if (!library) {
      LOG(ERROR) << "OHAudio input failed to load ability_access_control";
      return static_cast<CheckSelfPermission>(nullptr);
    }
    CheckSelfPermission function = reinterpret_cast<CheckSelfPermission>(
        dlsym(library, "OH_AT_CheckSelfPermission"));
    if (!function) {
      LOG(ERROR) << "OHAudio input failed to resolve CheckSelfPermission";
    }
    return function;
  }();
  return check_permission && check_permission(kMicrophonePermission);
}

}  // namespace

OhosAudioInputStream::OhosAudioInputStream(AudioManagerBase* audio_manager,
                                           const AudioParameters& params)
    : audio_manager_(audio_manager), params_(params) {
  CHECK(audio_manager_);
  CHECK(params_.IsValid());
}

OhosAudioInputStream::~OhosAudioInputStream() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK(!capturer_);
  base::AutoLock auto_lock(lock_);
  DCHECK(!callback_);
}

AudioInputStream::OpenOutcome OhosAudioInputStream::Open() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (opened_) {
    return OpenOutcome::kAlreadyOpen;
  }
  if (params_.format() != AudioParameters::AUDIO_PCM_LOW_LATENCY &&
      params_.format() != AudioParameters::AUDIO_PCM_LINEAR) {
    return OpenOutcome::kFailed;
  }
  opened_ = true;
  return OpenOutcome::kSuccess;
}

void OhosAudioInputStream::Start(AudioInputCallback* callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  CHECK(callback);
  if (!opened_) {
    callback->OnError(AudioInputCallback::Error::kStartupFailed);
    return;
  }

  {
    base::AutoLock auto_lock(lock_);
    if (callback_) {
      callback->OnError(AudioInputCallback::Error::kStartupFailed);
      return;
    }
    callback_ = callback;
    last_capture_time_ = base::TimeTicks();
  }

  permission_wait_deadline_ = base::TimeTicks::Now() + kPermissionWaitTimeout;
  StartWhenMicrophonePermissionIsReady();
}

void OhosAudioInputStream::StartWhenMicrophonePermissionIsReady() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  {
    base::AutoLock auto_lock(lock_);
    if (!callback_) {
      return;
    }
  }

  if (!HasMicrophonePermission()) {
    if (base::TimeTicks::Now() < permission_wait_deadline_) {
      base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
          FROM_HERE,
          base::BindOnce(
              &OhosAudioInputStream::StartWhenMicrophonePermissionIsReady,
              weak_factory_.GetWeakPtr()),
          kPermissionPollInterval);
      return;
    }
    ReportError(AudioInputCallback::Error::kStartupFailed);
    return;
  }

  if (!capturer_ && !InitializeCapturer()) {
    ReportError(AudioInputCallback::Error::kStartupFailed);
    return;
  }
  if (!IsSuccess(OH_AudioCapturer_Start(capturer_))) {
    LOG(ERROR) << "OHAudio failed to start an input stream";
    ReportError(AudioInputCallback::Error::kStartupFailed);
    return;
  }
  started_ = true;
}

bool OhosAudioInputStream::InitializeCapturer() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK(!capturer_);

  OH_AudioStreamBuilder* builder = nullptr;
  if (!IsSuccess(
          OH_AudioStreamBuilder_Create(&builder, AUDIOSTREAM_TYPE_CAPTURER))) {
    LOG(ERROR) << "OHAudio failed to create a capturer builder";
    return false;
  }

  const OH_AudioCapturer_Callbacks callbacks = {
      &OhosAudioInputStream::OnReadData,
      &OhosAudioInputStream::OnStreamEvent,
      &OhosAudioInputStream::OnInterruptEvent,
      &OhosAudioInputStream::OnError,
  };
  const OH_AudioStream_LatencyMode latency_mode =
      params_.format() == AudioParameters::AUDIO_PCM_LOW_LATENCY
          ? AUDIOSTREAM_LATENCY_MODE_FAST
          : AUDIOSTREAM_LATENCY_MODE_NORMAL;

  const bool configured =
      IsSuccess(OH_AudioStreamBuilder_SetSamplingRate(builder,
                                                      params_.sample_rate())) &&
      IsSuccess(
          OH_AudioStreamBuilder_SetChannelCount(builder, params_.channels())) &&
      IsSuccess(OH_AudioStreamBuilder_SetSampleFormat(
          builder, AUDIOSTREAM_SAMPLE_S16LE)) &&
      IsSuccess(OH_AudioStreamBuilder_SetEncodingType(
          builder, AUDIOSTREAM_ENCODING_TYPE_RAW)) &&
      IsSuccess(OH_AudioStreamBuilder_SetLatencyMode(builder, latency_mode)) &&
      IsSuccess(OH_AudioStreamBuilder_SetCapturerInfo(
          builder, AUDIOSTREAM_SOURCE_TYPE_VOICE_COMMUNICATION)) &&
      IsSuccess(OH_AudioStreamBuilder_SetCapturerCallback(builder, callbacks,
                                                          this)) &&
      IsSuccess(OH_AudioStreamBuilder_GenerateCapturer(builder, &capturer_));

  const OH_AudioStream_Result destroy_result =
      OH_AudioStreamBuilder_Destroy(builder);
  if (!IsSuccess(destroy_result)) {
    LOG(WARNING) << "OHAudio failed to destroy a capturer builder";
  }

  if (!configured) {
    LOG(ERROR) << "OHAudio failed to configure an input stream";
    ReleaseCapturer();
    return false;
  }

  audio_fifo_ = std::make_unique<AudioBlockFifo>(
      params_.channels(), params_.frames_per_buffer(), kFifoBlocks);
  return true;
}

void OhosAudioInputStream::Stop() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  weak_factory_.InvalidateWeakPtrs();

  if (capturer_ && started_ && !IsSuccess(OH_AudioCapturer_Stop(capturer_))) {
    LOG(WARNING) << "OHAudio failed to stop an input stream";
  }
  started_ = false;

  base::AutoLock auto_lock(lock_);
  callback_ = nullptr;
  last_capture_time_ = base::TimeTicks();
  if (audio_fifo_) {
    audio_fifo_->Clear();
  }
}

void OhosAudioInputStream::Close() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  Stop();
  ReleaseCapturer();
  opened_ = false;

  // ReleaseInputStream() deletes this object.
  audio_manager_->ReleaseInputStream(this);
}

double OhosAudioInputStream::GetMaxVolume() {
  return 1.0;
}

void OhosAudioInputStream::SetVolume(double volume) {
  if (volume < 0.0 || volume > 1.0) {
    return;
  }
  base::AutoLock auto_lock(lock_);
  volume_ = volume;
}

double OhosAudioInputStream::GetVolume() {
  base::AutoLock auto_lock(lock_);
  return volume_;
}

bool OhosAudioInputStream::SetAutomaticGainControl(bool enabled) {
  automatic_gain_control_ = enabled;
  return true;
}

bool OhosAudioInputStream::GetAutomaticGainControl() {
  return automatic_gain_control_;
}

bool OhosAudioInputStream::IsMuted() {
  base::AutoLock auto_lock(lock_);
  return volume_ < kVolumeEpsilon;
}

void OhosAudioInputStream::SetOutputDeviceForAec(
    const std::string& output_device_id) {}

// static
int32_t OhosAudioInputStream::OnReadData(OH_AudioCapturer* capturer,
                                         void* user_data,
                                         void* buffer,
                                         int32_t length) {
  return static_cast<OhosAudioInputStream*>(user_data)->CaptureData(
      capturer, buffer, length);
}

// static
int32_t OhosAudioInputStream::OnStreamEvent(OH_AudioCapturer* capturer,
                                            void* user_data,
                                            OH_AudioStream_Event event) {
  return AUDIOSTREAM_SUCCESS;
}

// static
int32_t OhosAudioInputStream::OnInterruptEvent(OH_AudioCapturer* capturer,
                                               void* user_data,
                                               OH_AudioInterrupt_ForceType type,
                                               OH_AudioInterrupt_Hint hint) {
  return AUDIOSTREAM_SUCCESS;
}

// static
int32_t OhosAudioInputStream::OnError(OH_AudioCapturer* capturer,
                                      void* user_data,
                                      OH_AudioStream_Result error) {
  static_cast<OhosAudioInputStream*>(user_data)->ReportError(
      AudioInputCallback::Error::kRuntimeError);
  return AUDIOSTREAM_SUCCESS;
}

int32_t OhosAudioInputStream::CaptureData(OH_AudioCapturer* capturer,
                                          void* buffer,
                                          int32_t length) {
  const size_t bytes_per_frame =
      static_cast<size_t>(params_.channels()) * sizeof(int16_t);
  if (!buffer || length <= 0 ||
      static_cast<size_t>(length) % bytes_per_frame != 0) {
    return AUDIOSTREAM_ERROR_INVALID_PARAM;
  }

  const int frames =
      static_cast<int>(static_cast<size_t>(length) / bytes_per_frame);
  auto bytes = UNSAFE_BUFFERS(base::span(static_cast<const uint8_t*>(buffer),
                                         static_cast<size_t>(length)));

  int64_t frame_position = 0;
  int64_t timestamp_ns = 0;
  base::TimeTicks packet_end = base::TimeTicks::Now();
  if (IsSuccess(OH_AudioCapturer_GetTimestamp(
          capturer, CLOCK_MONOTONIC, &frame_position, &timestamp_ns)) &&
      timestamp_ns > 0) {
    const base::TimeTicks native_time =
        base::TimeTicks() + base::Nanoseconds(timestamp_ns);
    if ((native_time - packet_end).magnitude() < base::Seconds(10)) {
      packet_end = native_time;
    }
  }

  base::AutoLock auto_lock(lock_);
  if (!callback_ || !audio_fifo_) {
    return AUDIOSTREAM_SUCCESS;
  }

  if (frames > audio_fifo_->GetUnfilledFrames()) {
    const int missing_frames = frames - audio_fifo_->GetUnfilledFrames();
    const int additional_blocks =
        (missing_frames + params_.frames_per_buffer() - 1) /
        params_.frames_per_buffer();
    audio_fifo_->IncreaseCapacity(std::max(1, additional_blocks));
  }

  const int buffered_frames = audio_fifo_->GetAvailableFrames();
  base::TimeTicks capture_time =
      packet_end - AudioTimestampHelper::FramesToTime(frames + buffered_frames,
                                                      params_.sample_rate());
  audio_fifo_->Push(bytes, frames, kSampleFormatS16);

  const base::TimeDelta block_duration = AudioTimestampHelper::FramesToTime(
      params_.frames_per_buffer(), params_.sample_rate());
  while (audio_fifo_->available_blocks()) {
    if (!last_capture_time_.is_null() && capture_time <= last_capture_time_) {
      capture_time = last_capture_time_ + base::Microseconds(1);
    }
    callback_->OnData(audio_fifo_->Consume(), capture_time, volume_, {});
    last_capture_time_ = capture_time;
    capture_time += block_duration;
  }
  return AUDIOSTREAM_SUCCESS;
}

void OhosAudioInputStream::ReportError(AudioInputCallback::Error error) {
  base::AutoLock auto_lock(lock_);
  if (callback_) {
    callback_->OnError(error);
  }
}

void OhosAudioInputStream::ReleaseCapturer() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (capturer_) {
    if (!IsSuccess(OH_AudioCapturer_Release(capturer_))) {
      LOG(WARNING) << "OHAudio failed to release an input stream";
    }
    capturer_ = nullptr;
  }
  audio_fifo_.reset();
}

}  // namespace media
