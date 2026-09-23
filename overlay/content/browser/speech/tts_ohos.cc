// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// speechSynthesis on CoreSpeechKit, which exists only in ArkTS: every call
// goes to the engine HAR's TtsService over //components/ohos_system_service.
//
// CoreSpeechKit has no pause and no word boundaries. Pause stops, as on
// Android; voices advertise start, end and error only.

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/functional/callback_helpers.h"
#include "base/logging.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "components/ohos_system_service/system_service_ohos.h"
#include "content/browser/speech/tts_platform_impl.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/tts_controller.h"

namespace content {

namespace {

constexpr char kService[] = "tts";
// ArkTS attaches once the XComponent loads, which can be after Chromium first
// asks for voices. Retry until it does rather than stay "loading" forever,
// which would hold queued utterances indefinitely.
constexpr base::TimeDelta kFirstRetry = base::Milliseconds(250);
constexpr base::TimeDelta kMaxRetry = base::Seconds(5);

// Chromium sends -1 for "not set".
double OrDefault(double value) {
  return value < 0 ? 1.0 : value;
}

}  // namespace

class TtsPlatformImplOhos : public TtsPlatformImpl {
 public:
  TtsPlatformImplOhos() = default;
  TtsPlatformImplOhos(const TtsPlatformImplOhos&) = delete;
  TtsPlatformImplOhos& operator=(const TtsPlatformImplOhos&) = delete;

  static TtsPlatformImplOhos* GetInstance() {
    static base::NoDestructor<TtsPlatformImplOhos> instance;
    return instance.get();
  }

  // Constant, as TtsController requires: HarmonyOS always has the engine.
  bool PlatformImplSupported() override { return true; }

  bool PlatformImplInitialized() override {
    StartIfNeeded();
    return initialized_;
  }

  void Speak(int utterance_id,
             const std::string& utterance,
             const std::string& lang,
             const VoiceData& voice,
             const UtteranceContinuousParameters& params,
             base::OnceCallback<void(bool)> on_speak_finished) override {
    // Markup would be read aloud; strip it the way Android does.
    TtsController::GetInstance()->StripSSML(
        utterance,
        base::BindOnce(&TtsPlatformImplOhos::SpeakPlainText,
                       weak_factory_.GetWeakPtr(), utterance_id, lang,
                       voice.native_voice_identifier, params,
                       std::move(on_speak_finished)));
  }

  bool StopSpeaking() override {
    // TtsController reports the interruption itself.
    speaking_utterance_ = std::nullopt;
    ohos_system_service::Call(kService, "stop", base::DictValue(),
                              base::DoNothing());
    return true;
  }

  // Tracked here: the bridge is asynchronous and this must answer now.
  bool IsSpeaking() override { return speaking_utterance_.has_value(); }

  void GetVoices(std::vector<VoiceData>* out_voices) override {
    *out_voices = voices_;
  }

  void Pause() override { StopSpeaking(); }
  void Resume() override {}

 private:
  void StartIfNeeded() {
    if (started_) {
      return;
    }
    started_ = true;
    events_ = ohos_system_service::SubscribeToEvents(
        kService, base::BindRepeating(&TtsPlatformImplOhos::OnEvent,
                                      weak_factory_.GetWeakPtr()));
    RequestVoices();
  }

  void RequestVoices() {
    ohos_system_service::Call(
        kService, "voices", base::DictValue(),
        base::BindOnce(&TtsPlatformImplOhos::OnVoices,
                       weak_factory_.GetWeakPtr()));
  }

  void OnVoices(ohos_system_service::Reply reply) {
    if (!reply.ok) {
      // Most likely ArkTS is not attached yet.
      retry_delay_ = retry_delay_.is_zero()
                         ? kFirstRetry
                         : std::min(retry_delay_ * 2, kMaxRetry);
      retry_timer_.Start(FROM_HERE, retry_delay_,
                         base::BindOnce(&TtsPlatformImplOhos::RequestVoices,
                                        weak_factory_.GetWeakPtr()));
      return;
    }
    voices_.clear();
    if (const base::ListValue* list = reply.result_dict().FindList("voices")) {
      for (const base::Value& entry : *list) {
        if (!entry.is_dict()) {
          continue;
        }
        const base::DictValue& dict = entry.GetDict();
        VoiceData voice;
        if (const std::string* name = dict.FindString("name")) {
          voice.name = *name;
        }
        if (const std::string* lang = dict.FindString("lang")) {
          voice.lang = *lang;
        }
        if (std::optional<int> person = dict.FindInt("person")) {
          voice.native_voice_identifier = base::NumberToString(*person);
        }
        voice.native = true;
        voice.remote = false;
        voice.events = {TTS_EVENT_START, TTS_EVENT_END, TTS_EVENT_ERROR,
                        TTS_EVENT_INTERRUPTED, TTS_EVENT_CANCELLED};
        voices_.push_back(std::move(voice));
      }
    }
    initialized_ = true;
    TtsController::GetInstance()->VoicesChanged();
  }

  void SpeakPlainText(int utterance_id,
                      const std::string& lang,
                      const std::string& voice_identifier,
                      const UtteranceContinuousParameters& params,
                      base::OnceCallback<void(bool)> on_speak_finished,
                      const std::string& text) {
    base::DictValue args;
    args.Set("utteranceId", utterance_id);
    args.Set("text", text);
    args.Set("lang", lang);
    int person = 0;
    if (!voice_identifier.empty() &&
        base::StringToInt(voice_identifier, &person)) {
      args.Set("person", person);
    }
    args.Set("rate", OrDefault(params.rate));
    args.Set("pitch", OrDefault(params.pitch));
    // Chromium's 1.0 is full volume; CoreSpeechKit's 1.0 is its normal level.
    args.Set("volume", OrDefault(params.volume));
    utterance_length_ = static_cast<int>(text.size());
    speaking_utterance_ = utterance_id;
    ohos_system_service::Call(
        kService, "speak", std::move(args),
        base::BindOnce(
            [](base::WeakPtr<TtsPlatformImplOhos> self, int utterance_id,
               base::OnceCallback<void(bool)> done,
               ohos_system_service::Reply reply) {
              if (!reply.ok) {
                LOG(WARNING) << "HarmonyOS TTS could not speak: "
                             << reply.error;
                if (self && self->speaking_utterance_ == utterance_id) {
                  self->speaking_utterance_ = std::nullopt;
                }
              }
              std::move(done).Run(reply.ok);
            },
            weak_factory_.GetWeakPtr(), utterance_id,
            std::move(on_speak_finished)));
  }

  void OnEvent(const std::string& event, const base::DictValue& data) {
    const std::optional<int> utterance_id = data.FindInt("utteranceId");
    if (!utterance_id) {
      return;
    }
    TtsController* controller = TtsController::GetInstance();
    if (event == "start") {
      controller->OnTtsEvent(*utterance_id, TTS_EVENT_START, 0, -1,
                             std::string());
    } else if (event == "end") {
      if (speaking_utterance_ == *utterance_id) {
        speaking_utterance_ = std::nullopt;
      }
      controller->OnTtsEvent(*utterance_id, TTS_EVENT_END, utterance_length_,
                             0, std::string());
    } else if (event == "error") {
      if (speaking_utterance_ == *utterance_id) {
        speaking_utterance_ = std::nullopt;
      }
      const std::string* message = data.FindString("message");
      controller->OnTtsEvent(*utterance_id, TTS_EVENT_ERROR, -1, -1,
                             message ? *message : std::string());
    }
  }

  bool started_ = false;
  bool initialized_ = false;
  std::vector<VoiceData> voices_;
  std::optional<int> speaking_utterance_;
  int utterance_length_ = 0;
  base::TimeDelta retry_delay_;
  base::OneShotTimer retry_timer_;
  std::unique_ptr<ohos_system_service::EventSubscription> events_;
  base::WeakPtrFactory<TtsPlatformImplOhos> weak_factory_{this};
};

TtsPlatformImpl* TtsPlatformImpl::GetInstance() {
  return TtsPlatformImplOhos::GetInstance();
}

}  // namespace content
