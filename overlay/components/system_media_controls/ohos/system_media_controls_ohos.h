// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_SYSTEM_MEDIA_CONTROLS_OHOS_SYSTEM_MEDIA_CONTROLS_OHOS_H_
#define COMPONENTS_SYSTEM_MEDIA_CONTROLS_OHOS_SYSTEM_MEDIA_CONTROLS_OHOS_H_

#include <multimedia/av_session/native_avsession.h>

#include <string>

#include "base/memory/weak_ptr.h"
#include "base/observer_list.h"
#include "base/sequence_checker.h"
#include "components/system_media_controls/system_media_controls.h"

namespace system_media_controls::internal {

// The page's media session as a HarmonyOS AVSession: what the control center
// and lock screen show as "now playing", and where their play, pause, next,
// previous and seek buttons come back from.
//
// Lives on the UI thread. AVSession calls back on its own IPC threads; those
// callbacks only post here.
class SystemMediaControlsOhos : public SystemMediaControls {
 public:
  SystemMediaControlsOhos();
  SystemMediaControlsOhos(const SystemMediaControlsOhos&) = delete;
  SystemMediaControlsOhos& operator=(const SystemMediaControlsOhos&) = delete;
  ~SystemMediaControlsOhos() override;

  // Whether the AVSession was created; Create() returns null otherwise.
  bool Initialize();

  // SystemMediaControls:
  void AddObserver(SystemMediaControlsObserver* observer) override;
  void RemoveObserver(SystemMediaControlsObserver* observer) override;
  void SetEnabled(bool enabled) override;
  void SetIsNextEnabled(bool value) override;
  void SetIsPreviousEnabled(bool value) override;
  void SetIsPlayPauseEnabled(bool value) override;
  void SetIsStopEnabled(bool value) override;
  void SetIsSeekToEnabled(bool value) override;
  void SetPlaybackStatus(PlaybackStatus value) override;
  void SetTitle(const std::u16string& value) override;
  void SetArtist(const std::u16string& value) override;
  void SetAlbum(const std::u16string& value) override;
  void SetThumbnail(const SkBitmap& bitmap) override;
  void SetPosition(const media_session::MediaPosition& position) override;
  void ClearPosition() override;
  void ClearThumbnail() override;
  void ClearMetadata() override;
  void UpdateDisplay() override;
  bool GetVisibilityForTesting() const override;

  // Called on the UI thread for a command AVSession delivered.
  void OnCommand(AVSession_ControlCommand command);
  void OnSeekTo(uint64_t milliseconds);

 private:
  void SetBackgroundAudio(bool running);
  void SetCommandEnabled(AVSession_ControlCommand command, bool enabled);

  SEQUENCE_CHECKER(sequence_checker_);

  OH_AVSession* session_ = nullptr;
  bool active_ = false;
  // Whether the audio-playback continuous task is held; see SetEnabled.
  bool background_audio_ = false;
  bool seek_enabled_ = false;
  // Which commands have a callback registered; the control center shows a
  // button only for those.
  uint32_t registered_commands_ = 0;

  std::string title_;
  std::string artist_;
  std::string album_;
  int64_t duration_ms_ = -1;

  base::ObserverList<SystemMediaControlsObserver> observers_;

  // Owned by AVSession's callbacks for the life of the process; see the .cc.
  struct Relay;
  Relay* relay_ = nullptr;

  base::WeakPtrFactory<SystemMediaControlsOhos> weak_factory_{this};
};

}  // namespace system_media_controls::internal

#endif  // COMPONENTS_SYSTEM_MEDIA_CONTROLS_OHOS_SYSTEM_MEDIA_CONTROLS_OHOS_H_
