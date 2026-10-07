// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/system_media_controls/ohos/system_media_controls_ohos.h"

#include <bundle/native_interface_bundle.h>
#include <multimedia/av_session/native_avmetadata.h>
#include <multimedia/av_session/native_avplaybackstate.h>
#include <multimedia/av_session/native_avsession_errors.h>

#include <algorithm>
#include <memory>
#include <utility>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/scoped_refptr.h"
#include "base/no_destructor.h"
#include "base/notimplemented.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "base/unguessable_token.h"
#include "base/values.h"
#include "components/ohos_system_service/system_service_ohos.h"
#include "components/system_media_controls/system_media_controls_observer.h"
#include "services/media_session/public/cpp/media_position.h"
#include "skia/ext/image_operations.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/gfx/codec/png_codec.h"

namespace system_media_controls {

namespace internal {

namespace {

constexpr char kSessionTag[] = "chromium";

// The ArkTS service that holds the audio-playback continuous task
// (engine HAR, BackgroundAudioService.ets).
constexpr char kBackgroundAudioService[] = "backgroundaudio";

void LogBackgroundAudioFailure(ohos_system_service::Reply reply) {
  if (!reply.ok) {
    LOG(WARNING) << "HarmonyOS background audio task failed: " << reply.error;
  }
}

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

struct AppElement {
  std::string bundle_name;
  std::string ability_name;
};

// Read once and kept: the app's main element does not change while it runs.
//
// OH_NativeBundle_GetMainElementName's strings come from the system allocator
// and the caller is told to free them, but free() in this library is
// PartitionAlloc's shim. Handing it a pointer it never allocated crashed the
// browser in PartitionRoot::FreeInUnknownRoot the first time a page played
// media. The strings are left alone instead: a few bytes, once per process.
const AppElement& MainElement() {
  static const base::NoDestructor<AppElement> element([] {
    const OH_NativeBundle_ElementName raw =
        OH_NativeBundle_GetMainElementName();
    AppElement result;
    if (raw.bundleName) {
      result.bundle_name = raw.bundleName;
    }
    if (raw.abilityName) {
      result.ability_name = raw.abilityName;
    }
    return result;
  }());
  return *element;
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

struct SystemMediaControlsOhos::ArtworkFile {
  base::FilePath path;
  std::string uri;
  scoped_refptr<base::SequencedTaskRunner> task_runner;

  ~ArtworkFile() {
    if (!path.empty()) {
      task_runner->PostTask(
          FROM_HERE, base::BindOnce(
                         [](base::FilePath path) {
                           if (!base::DeleteFile(path)) {
                             LOG(WARNING) << "Media artwork cleanup failed";
                           }
                         },
                         path));
    }
  }

  static std::shared_ptr<ArtworkFile> Encode(
      SkBitmap bitmap,
      base::FilePath directory,
      std::string uri_directory,
      scoped_refptr<base::SequencedTaskRunner> task_runner,
      std::shared_ptr<std::atomic<uint64_t>> current_generation,
      uint64_t generation) {
    if (current_generation->load() != generation || bitmap.drawsNothing()) {
      return nullptr;
    }
    constexpr int kMaxDimension = 512;
    const int longest = std::max(bitmap.width(), bitmap.height());
    if (longest > kMaxDimension) {
      bitmap = skia::ImageOperations::Resize(
          bitmap, skia::ImageOperations::RESIZE_GOOD,
          std::max(1, static_cast<int>(
                          int64_t{bitmap.width()} * kMaxDimension / longest)),
          std::max(1, static_cast<int>(
                          int64_t{bitmap.height()} * kMaxDimension / longest)));
    }
    if (bitmap.drawsNothing()) {
      return nullptr;
    }
    SkBitmap pixels;
    if (!pixels.tryAllocN32Pixels(bitmap.width(), bitmap.height()) ||
        !bitmap.readPixels(pixels.info(), pixels.getPixels(), pixels.rowBytes(),
                           0, 0)) {
      return nullptr;
    }
    auto encoded = gfx::PNGCodec::EncodeBGRASkBitmap(pixels, false);
    if (!encoded || current_generation->load() != generation) {
      return nullptr;
    }
    const std::string filename =
        "artwork-" + base::UnguessableToken::Create().ToString() + ".png";
    auto artwork = std::make_shared<ArtworkFile>();
    artwork->task_runner = std::move(task_runner);
    artwork->path = directory.AppendASCII(filename);
    artwork->uri = uri_directory + "/" + filename;
    if (!base::WriteFile(artwork->path, *encoded) ||
        current_generation->load() != generation) {
      return nullptr;
    }
    return artwork;
  }
};

SystemMediaControlsOhos::SystemMediaControlsOhos()
    : artwork_task_runner_(base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
           base::TaskShutdownBehavior::BLOCK_SHUTDOWN})),
      artwork_generation_(std::make_shared<std::atomic<uint64_t>>(0)) {}

SystemMediaControlsOhos::~SystemMediaControlsOhos() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  ++*artwork_generation_;
  artwork_.reset();
  published_artwork_.reset();
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
  SetBackgroundAudio(false);
  OH_AVSession_Destroy(session_);
}

bool SystemMediaControlsOhos::Initialize() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // AVSession ties the session to an ability so the control center can bring
  // the app back when the user taps "now playing".
  const AppElement& element = MainElement();
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
  SetBackgroundAudio(enabled);
}

// Without an audio-playback continuous task HarmonyOS mutes the app a moment
// after it leaves the foreground and freezes it a few seconds later, so the
// page falls silent while the control center still says it is playing. The
// task is held for as long as the session is active, paused included, so
// "play" from the control center reaches a process that is still running.
void SystemMediaControlsOhos::SetBackgroundAudio(bool running) {
  if (running == background_audio_ || !ohos_system_service::IsAvailable()) {
    return;
  }
  background_audio_ = running;
  ohos_system_service::Call(kBackgroundAudioService, running ? "start" : "stop",
                            base::DictValue(),
                            base::BindOnce(&LogBackgroundAudioFailure));
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
  // SystemMediaControlsNotifier calls SetEnabled only on Windows, for the
  // lock screen; other platforms show their controls from creation. AVSession
  // drops every command sent to an inactive session, so the session follows
  // whether a page has an active media session instead.
  SetEnabled(value != PlaybackStatus::kStopped);
  if (value == PlaybackStatus::kStopped) {
    ClearThumbnail();
  }
}

void SystemMediaControlsOhos::SetID(const std::string* value) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const std::string id = value ? *value : std::string();
  if (id != media_id_) {
    media_id_ = id;
    ClearThumbnail();
  }
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
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (bitmap.drawsNothing()) {
    ClearThumbnail();
    return;
  }
  const uint64_t generation = ++*artwork_generation_;
  ohos_system_service::Call(
      "mediaartwork", "location", base::DictValue(),
      base::BindOnce(&SystemMediaControlsOhos::EncodeThumbnail,
                     weak_factory_.GetWeakPtr(), generation, bitmap));
}

void SystemMediaControlsOhos::EncodeThumbnail(
    uint64_t generation,
    SkBitmap bitmap,
    ohos_system_service::Reply reply) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (generation != artwork_generation_->load()) {
    return;
  }
  const std::string* directory = reply.result_dict().FindString("directory");
  const std::string* uri_directory =
      reply.result_dict().FindString("uriDirectory");
  if (!reply.ok || !directory || !uri_directory || directory->empty() ||
      !base::FilePath(*directory).IsAbsolute() ||
      !uri_directory->starts_with("file://")) {
    ClearThumbnail();
    LOG(WARNING) << "Media artwork cache unavailable";
    return;
  }
  artwork_task_runner_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&ArtworkFile::Encode, std::move(bitmap),
                     base::FilePath(*directory), *uri_directory,
                     artwork_task_runner_, artwork_generation_, generation),
      base::BindOnce(&SystemMediaControlsOhos::OnThumbnailReady,
                     weak_factory_.GetWeakPtr(), generation));
}

void SystemMediaControlsOhos::OnThumbnailReady(
    uint64_t generation,
    std::shared_ptr<ArtworkFile> artwork) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (generation != artwork_generation_->load()) {
    return;
  }
  artwork_ = std::move(artwork);
  image_uri_ = artwork_ ? artwork_->uri : std::string();
  UpdateDisplay();
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

void SystemMediaControlsOhos::ClearThumbnail() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  ++*artwork_generation_;
  image_uri_.clear();
  artwork_.reset();
  published_artwork_.reset();
  if (session_) {
    UpdateDisplay();
  }
}

void SystemMediaControlsOhos::ClearMetadata() {
  title_.clear();
  artist_.clear();
  album_.clear();
  duration_ms_ = -1;
  ClearThumbnail();
}

void SystemMediaControlsOhos::UpdateDisplay() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  OH_AVMetadataBuilder* builder = nullptr;
  if (OH_AVMetadataBuilder_Create(&builder) != AVMETADATA_SUCCESS ||
      !builder) {
    return;
  }
  // AVSession requires an asset id; a page has one media session at a time.
  OH_AVMetadataBuilder_SetAssetId(
      builder, media_id_.empty() ? kSessionTag : media_id_.c_str());
  OH_AVMetadataBuilder_SetTitle(builder, title_.c_str());
  OH_AVMetadataBuilder_SetArtist(builder, artist_.c_str());
  OH_AVMetadataBuilder_SetAlbum(builder, album_.c_str());
  if (OH_AVMetadataBuilder_SetMediaImageUri(builder, image_uri_.c_str()) !=
      AVMETADATA_SUCCESS) {
    OH_AVMetadataBuilder_Destroy(builder);
    LOG(WARNING) << "Media artwork metadata rejected";
    return;
  }
  if (duration_ms_ >= 0) {
    OH_AVMetadataBuilder_SetDuration(builder, duration_ms_);
  }
  OH_AVMetadata* metadata = nullptr;
  if (OH_AVMetadataBuilder_GenerateAVMetadata(builder, &metadata) ==
          AVMETADATA_SUCCESS &&
      metadata) {
    if (OH_AVSession_SetAVMetadata(session_, metadata) == AV_SESSION_ERR_SUCCESS) {
      published_artwork_ = artwork_;
    } else {
      LOG(WARNING) << "Media artwork metadata update failed";
    }
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
