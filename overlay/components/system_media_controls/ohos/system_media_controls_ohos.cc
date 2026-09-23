// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/system_media_controls/ohos/system_media_controls_ohos.h"

#include <bundle/native_interface_bundle.h>
#include <multimedia/av_session/native_avmetadata.h>
#include <multimedia/av_session/native_avplaybackstate.h>
#include <multimedia/av_session/native_avsession_errors.h>

#include <cstdlib>
#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/scoped_refptr.h"
#include "base/notimplemented.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "components/system_media_controls/system_media_controls_observer.h"
#include "services/media_session/public/cpp/media_position.h"

namespace system_media_controls {

namespace internal {

namespace {

constexpr char kSessionTag[] = "chromium";

// One bit per AVSession_ControlCommand we register.
uint32_t Bit(AVSession_ControlCommand command) {
  return 1u << static_cast<uint32_t>(command);
}

AVSession_PlaybackState ToAVSessionState(
    SystemMediaControls::PlaybackStatus status) {
  switch (status) {
    case SystemMediaControls::PlaybackStatus::kPlaying:
      return PLAYBACK_STATE_PLAYING;
    case SystemMediaControls::PlaybackStatus::kPaused:
      return PLAYBACK_STATE_PAUSED;
    case SystemMediaControls::PlaybackStatus::kStopped:
      return PLAYBACK_STATE_STOPPED;
  }
  return PLAYBACK_STATE_STOPPED;
}

// OH_NativeBundle_GetMainElementName hands back strings the caller frees.
struct AppElement {
  std::string bundle_name;
  std::string ability_name;
};

AppElement ReadMainElement() {
  OH_NativeBundle_ElementName element = OH_NativeBundle_GetMainElementName();
  AppElement result;
  if (element.bundleName) {
    result.bundle_name = element.bundleName;
  }
  if (element.abilityName) {
    result.ability_name = element.abilityName;
  }
  free(element.bundleName);
  free(element.moduleName);
  free(element.abilityName);
  return result;
}

}  // namespace

// What AVSession's callbacks point at. They run on AVSession IPC threads and
// may still be in flight after the controls are destroyed, so this is never
// freed: the controls are created once per browser process, and a dangling
// pointer here would be a use-after-free on a thread Chromium does not own.
struct SystemMediaControlsOhos::Relay {
  scoped_refptr<base::SequencedTaskRunner> task_runner;
  base::WeakPtr<SystemMediaControlsOhos> controls;

  static AVSessionCallback_Result OnCommand(OH_AVSession* session,
                                            AVSession_ControlCommand command,
                                            void* user_data) {
    auto* relay = static_cast<Relay*>(user_data);
    relay->task_runner->PostTask(
        FROM_HERE, base::BindOnce(&SystemMediaControlsOhos::OnCommand,
                                  relay->controls, command));
    return AVSESSION_CALLBACK_RESULT_SUCCESS;
  }

  static AVSessionCallback_Result OnSeek(OH_AVSession* session,
                                         uint64_t seek_time,
                                         void* user_data) {
    auto* relay = static_cast<Relay*>(user_data);
    relay->task_runner->PostTask(
        FROM_HERE, base::BindOnce(&SystemMediaControlsOhos::OnSeekTo,
                                  relay->controls, seek_time));
    return AVSESSION_CALLBACK_RESULT_SUCCESS;
  }
};

SystemMediaControlsOhos::SystemMediaControlsOhos() = default;

SystemMediaControlsOhos::~SystemMediaControlsOhos() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!session_) {
    return;
  }
  for (AVSession_ControlCommand command :
       {CONTROL_CMD_PLAY, CONTROL_CMD_PAUSE, CONTROL_CMD_STOP,
        CONTROL_CMD_PLAY_NEXT, CONTROL_CMD_PLAY_PREVIOUS}) {
    SetCommandEnabled(command, false);
  }
  if (seek_enabled_) {
    OH_AVSession_UnregisterSeekCallback(session_, &Relay::OnSeek);
  }
  if (active_) {
    OH_AVSession_Deactivate(session_);
  }
  OH_AVSession_Destroy(session_);
}

bool SystemMediaControlsOhos::Initialize() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // AVSession ties the session to an ability so the control center can bring
  // the app back when the user taps "now playing".
  const AppElement element = ReadMainElement();
  if (element.bundle_name.empty() || element.ability_name.empty()) {
    LOG(ERROR) << "AVSession needs the app's bundle and ability name";
    return false;
  }
  const AVSession_ErrCode result = OH_AVSession_Create(
      SESSION_TYPE_AUDIO, kSessionTag, element.bundle_name.c_str(),
      element.ability_name.c_str(), &session_);
  if (result != AV_SESSION_ERR_SUCCESS || !session_) {
    LOG(ERROR) << "OH_AVSession_Create failed: " << result;
    session_ = nullptr;
    return false;
  }
  relay_ = new Relay{base::SequencedTaskRunner::GetCurrentDefault(),
                     weak_factory_.GetWeakPtr()};
  return true;
}

void SystemMediaControlsOhos::AddObserver(
    SystemMediaControlsObserver* observer) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  observers_.AddObserver(observer);
  // Ready as soon as it exists; there is no asynchronous setup to wait for.
  observer->OnServiceReady();
}

void SystemMediaControlsOhos::RemoveObserver(
    SystemMediaControlsObserver* observer) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  observers_.RemoveObserver(observer);
}

void SystemMediaControlsOhos::SetEnabled(bool enabled) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (enabled == active_) {
    return;
  }
  const AVSession_ErrCode result = enabled ? OH_AVSession_Activate(session_)
                                           : OH_AVSession_Deactivate(session_);
  if (result != AV_SESSION_ERR_SUCCESS) {
    LOG(WARNING) << "AVSession " << (enabled ? "activate" : "deactivate")
                 << " failed: " << result;
    return;
  }
  active_ = enabled;
}

void SystemMediaControlsOhos::SetIsNextEnabled(bool value) {
  SetCommandEnabled(CONTROL_CMD_PLAY_NEXT, value);
}

void SystemMediaControlsOhos::SetIsPreviousEnabled(bool value) {
  SetCommandEnabled(CONTROL_CMD_PLAY_PREVIOUS, value);
}

void SystemMediaControlsOhos::SetIsPlayPauseEnabled(bool value) {
  SetCommandEnabled(CONTROL_CMD_PLAY, value);
  SetCommandEnabled(CONTROL_CMD_PAUSE, value);
}

void SystemMediaControlsOhos::SetIsStopEnabled(bool value) {
  SetCommandEnabled(CONTROL_CMD_STOP, value);
}

void SystemMediaControlsOhos::SetIsSeekToEnabled(bool value) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (value == seek_enabled_) {
    return;
  }
  const AVSession_ErrCode result =
      value ? OH_AVSession_RegisterSeekCallback(session_, &Relay::OnSeek,
                                                relay_)
            : OH_AVSession_UnregisterSeekCallback(session_, &Relay::OnSeek);
  if (result == AV_SESSION_ERR_SUCCESS) {
    seek_enabled_ = value;
  }
}

void SystemMediaControlsOhos::SetPlaybackStatus(PlaybackStatus value) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  OH_AVSession_SetPlaybackState(session_, ToAVSessionState(value));
}

void SystemMediaControlsOhos::SetTitle(const std::u16string& value) {
  title_ = base::UTF16ToUTF8(value);
}

void SystemMediaControlsOhos::SetArtist(const std::u16string& value) {
  artist_ = base::UTF16ToUTF8(value);
}

void SystemMediaControlsOhos::SetAlbum(const std::u16string& value) {
  album_ = base::UTF16ToUTF8(value);
}

void SystemMediaControlsOhos::SetThumbnail(const SkBitmap& bitmap) {
  // AVSession's C API takes artwork only as a URI; the bitmap Chromium has
  // here has none. Now-playing shows without artwork.
}

void SystemMediaControlsOhos::SetPosition(
    const media_session::MediaPosition& position) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  duration_ms_ = position.duration().InMilliseconds();
  AVSession_PlaybackPosition playback_position = {
      .elapsedTime = position.GetPosition().InMilliseconds(),
      .updateTime = base::Time::Now().InMillisecondsSinceUnixEpoch(),
  };
  OH_AVSession_SetPlaybackPosition(session_, &playback_position);
}

void SystemMediaControlsOhos::ClearPosition() {
  duration_ms_ = -1;
}

void SystemMediaControlsOhos::ClearThumbnail() {}

void SystemMediaControlsOhos::ClearMetadata() {
  title_.clear();
  artist_.clear();
  album_.clear();
  duration_ms_ = -1;
}

void SystemMediaControlsOhos::UpdateDisplay() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  OH_AVMetadataBuilder* builder = nullptr;
  if (OH_AVMetadataBuilder_Create(&builder) != AVMETADATA_SUCCESS ||
      !builder) {
    return;
  }
  // AVSession requires an asset id; a page has one media session at a time.
  OH_AVMetadataBuilder_SetAssetId(builder, kSessionTag);
  OH_AVMetadataBuilder_SetTitle(builder, title_.c_str());
  OH_AVMetadataBuilder_SetArtist(builder, artist_.c_str());
  OH_AVMetadataBuilder_SetAlbum(builder, album_.c_str());
  if (duration_ms_ >= 0) {
    OH_AVMetadataBuilder_SetDuration(builder, duration_ms_);
  }
  OH_AVMetadata* metadata = nullptr;
  if (OH_AVMetadataBuilder_GenerateAVMetadata(builder, &metadata) ==
          AVMETADATA_SUCCESS &&
      metadata) {
    OH_AVSession_SetAVMetadata(session_, metadata);
    OH_AVMetadata_Destroy(metadata);
  }
  OH_AVMetadataBuilder_Destroy(builder);
}

bool SystemMediaControlsOhos::GetVisibilityForTesting() const {
  return active_;
}

void SystemMediaControlsOhos::OnCommand(AVSession_ControlCommand command) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  for (SystemMediaControlsObserver& observer : observers_) {
    switch (command) {
      case CONTROL_CMD_PLAY:
        observer.OnPlay(this);
        break;
      case CONTROL_CMD_PAUSE:
        observer.OnPause(this);
        break;
      case CONTROL_CMD_STOP:
        observer.OnStop(this);
        break;
      case CONTROL_CMD_PLAY_NEXT:
        observer.OnNext(this);
        break;
      case CONTROL_CMD_PLAY_PREVIOUS:
        observer.OnPrevious(this);
        break;
      default:
        break;
    }
  }
}

void SystemMediaControlsOhos::OnSeekTo(uint64_t milliseconds) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const base::TimeDelta time =
      base::Milliseconds(static_cast<int64_t>(milliseconds));
  for (SystemMediaControlsObserver& observer : observers_) {
    observer.OnSeekTo(this, time);
  }
}

void SystemMediaControlsOhos::SetCommandEnabled(
    AVSession_ControlCommand command,
    bool enabled) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const bool registered = registered_commands_ & Bit(command);
  if (enabled == registered) {
    return;
  }
  const AVSession_ErrCode result =
      enabled ? OH_AVSession_RegisterCommandCallback(session_, command,
                                                     &Relay::OnCommand, relay_)
              : OH_AVSession_UnregisterCommandCallback(session_, command,
                                                       &Relay::OnCommand);
  if (result != AV_SESSION_ERR_SUCCESS) {
    return;
  }
  if (enabled) {
    registered_commands_ |= Bit(command);
  } else {
    registered_commands_ &= ~Bit(command);
  }
}

}  // namespace internal

// static
std::unique_ptr<SystemMediaControls> SystemMediaControls::Create(
    const std::string& product_name,
    int window) {
  auto controls = std::make_unique<internal::SystemMediaControlsOhos>();
  if (!controls->Initialize()) {
    return nullptr;
  }
  return controls;
}

// static
void SystemMediaControls::SetVisibilityChangedCallbackForTesting(
    base::RepeatingCallback<void(bool)>*) {
  NOTIMPLEMENTED();
}

}  // namespace system_media_controls
