// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MEDIA_AUDIO_OHOS_AUDIO_MANAGER_OHOS_H_
#define MEDIA_AUDIO_OHOS_AUDIO_MANAGER_OHOS_H_

#include <memory>
#include <string>

#include "media/audio/audio_manager_base.h"

namespace media {

class MEDIA_EXPORT AudioManagerOhos final : public AudioManagerBase {
 public:
  AudioManagerOhos(std::unique_ptr<AudioThread> audio_thread,
                   AudioLogFactory* audio_log_factory);

  AudioManagerOhos(const AudioManagerOhos&) = delete;
  AudioManagerOhos& operator=(const AudioManagerOhos&) = delete;

  ~AudioManagerOhos() override;

  bool HasAudioOutputDevices() override;
  bool HasAudioInputDevices() override;
  AudioParameters GetInputStreamParameters(
      const std::string& device_id) override;
  const std::string_view GetName() override;

  AudioOutputStream* MakeLinearOutputStream(
      const AudioParameters& params,
      const LogCallback& log_callback) override;
  AudioOutputStream* MakeLowLatencyOutputStream(
      const AudioParameters& params,
      const std::string& device_id,
      const LogCallback& log_callback) override;
  AudioInputStream* MakeLinearInputStream(
      const AudioParameters& params,
      const std::string& device_id,
      const LogCallback& log_callback) override;
  AudioInputStream* MakeLowLatencyInputStream(
      const AudioParameters& params,
      const std::string& device_id,
      const LogCallback& log_callback) override;

 protected:
  bool GetAudioInputDeviceNames(AudioDeviceNames* device_names) override;
  AudioParameters GetPreferredOutputStreamParameters(
      const std::string& output_device_id,
      const AudioParameters& input_params) override;
};

}  // namespace media

#endif  // MEDIA_AUDIO_OHOS_AUDIO_MANAGER_OHOS_H_
