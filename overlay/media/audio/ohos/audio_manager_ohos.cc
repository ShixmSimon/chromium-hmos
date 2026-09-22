// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/audio/ohos/audio_manager_ohos.h"

#include <memory>
#include <utility>

#include "base/command_line.h"
#include "media/audio/audio_device_description.h"
#include "media/audio/audio_device_name.h"
#include "media/audio/fake_audio_manager.h"
#include "media/audio/ohos/ohos_audio_input.h"
#include "media/audio/ohos/ohos_audio_output.h"
#include "media/base/channel_layout.h"
#include "media/base/media_switches.h"

namespace media {

namespace {

constexpr int kDefaultSampleRate = 48000;
constexpr int kDefaultFramesPerBuffer = 480;

}  // namespace

AudioManagerOhos::AudioManagerOhos(std::unique_ptr<AudioThread> audio_thread,
                                   AudioLogFactory* audio_log_factory)
    : AudioManagerBase(std::move(audio_thread), audio_log_factory) {}

AudioManagerOhos::~AudioManagerOhos() = default;

bool AudioManagerOhos::HasAudioOutputDevices() {
  return true;
}

bool AudioManagerOhos::HasAudioInputDevices() {
  return true;
}

AudioParameters AudioManagerOhos::GetInputStreamParameters(
    const std::string& device_id) {
  return AudioParameters(AudioParameters::AUDIO_PCM_LOW_LATENCY,
                         ChannelLayoutConfig::Mono(), kDefaultSampleRate,
                         kDefaultFramesPerBuffer);
}

const std::string_view AudioManagerOhos::GetName() {
  return "OHAudio";
}

AudioOutputStream* AudioManagerOhos::MakeLinearOutputStream(
    const AudioParameters& params,
    const LogCallback& log_callback) {
  return new OhosAudioOutputStream(this, params);
}

AudioOutputStream* AudioManagerOhos::MakeLowLatencyOutputStream(
    const AudioParameters& params,
    const std::string& device_id,
    const LogCallback& log_callback) {
  return new OhosAudioOutputStream(this, params);
}

AudioInputStream* AudioManagerOhos::MakeLinearInputStream(
    const AudioParameters& params,
    const std::string& device_id,
    const LogCallback& log_callback) {
  if (!AudioDeviceDescription::IsDefaultDevice(device_id)) {
    return nullptr;
  }
  return new OhosAudioInputStream(this, params);
}

AudioInputStream* AudioManagerOhos::MakeLowLatencyInputStream(
    const AudioParameters& params,
    const std::string& device_id,
    const LogCallback& log_callback) {
  if (!AudioDeviceDescription::IsDefaultDevice(device_id)) {
    return nullptr;
  }
  return new OhosAudioInputStream(this, params);
}

bool AudioManagerOhos::GetAudioInputDeviceNames(
    AudioDeviceNames* device_names) {
  DCHECK(GetTaskRunner()->BelongsToCurrentThread());
  if (base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kDisableAudioInput)) {
    return true;
  }
  if (!device_names) {
    return false;
  }
  *device_names = {AudioDeviceName::CreateDefault()};
  return true;
}

AudioParameters AudioManagerOhos::GetPreferredOutputStreamParameters(
    const std::string& output_device_id,
    const AudioParameters& input_params) {
  int frames_per_buffer = GetUserBufferSize();
  if (frames_per_buffer <= 0) {
    frames_per_buffer = kDefaultFramesPerBuffer;
  }
  return AudioParameters(AudioParameters::AUDIO_PCM_LOW_LATENCY,
                         ChannelLayoutConfig::Stereo(), kDefaultSampleRate,
                         frames_per_buffer);
}

std::unique_ptr<AudioManager> CreateAudioManager(
    std::unique_ptr<AudioThread> audio_thread,
    AudioLogFactory* audio_log_factory) {
  if (base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kDisableAudioOutput)) {
    return std::make_unique<FakeAudioManager>(std::move(audio_thread),
                                              audio_log_factory);
  }
  return std::make_unique<AudioManagerOhos>(std::move(audio_thread),
                                            audio_log_factory);
}

}  // namespace media
