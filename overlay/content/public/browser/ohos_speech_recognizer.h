// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_PUBLIC_BROWSER_OHOS_SPEECH_RECOGNIZER_H_
#define CONTENT_PUBLIC_BROWSER_OHOS_SPEECH_RECOGNIZER_H_

#include <string>

#include "base/functional/callback.h"
#include "content/common/content_export.h"
#include "content/public/browser/global_routing_id.h"

namespace content {

// A page's SpeechRecognition on HarmonyOS runs on the system recognizer,
// which the shell drives (it captures the audio itself). Chrome sets the
// handler at startup; content calls it on the UI thread.
struct OhosSpeechRecognitionHandler {
  // Start listening for `session_id`, asked by a page in `frame`.
  base::RepeatingCallback<void(int session_id,
                               GlobalRenderFrameHostId frame,
                               const std::string& language,
                               bool continuous,
                               bool interim_results)>
      start;
  // Stop listening and deliver what was heard.
  base::RepeatingCallback<void(int session_id)> stop;
  // Stop without a result. The shell still reports kEnd.
  base::RepeatingCallback<void(int session_id)> abort;
};

enum class OhosSpeechEvent {
  kAudioStart,
  kSoundStart,
  kSoundEnd,
  kAudioEnd,
  kResult,
  kError,
  kEnd,
};

// Defined in speech_recognition_manager_impl.cc. Without a handler, pages
// keep Chromium's own network recognizer.
CONTENT_EXPORT void SetOhosSpeechRecognitionHandler(
    OhosSpeechRecognitionHandler handler);

// What the shell reported for `session_id`, from any thread. `transcript`,
// `is_final` and `confidence` are for kResult; `error_code` is a
// media::mojom::SpeechRecognitionErrorCode for kError. Every session ends
// with kEnd.
CONTENT_EXPORT void OnOhosSpeechRecognitionEvent(
    int session_id,
    OhosSpeechEvent type,
    const std::u16string& transcript,
    bool is_final,
    double confidence,
    int error_code);

}  // namespace content

#endif  // CONTENT_PUBLIC_BROWSER_OHOS_SPEECH_RECOGNIZER_H_
