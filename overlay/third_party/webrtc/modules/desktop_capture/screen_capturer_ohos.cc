// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "modules/desktop_capture/desktop_capturer.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <multimedia/player_framework/native_avbuffer.h>
#include <multimedia/player_framework/native_avscreen_capture.h>
#include <native_buffer/native_buffer.h>
#include <window_manager/oh_display_manager.h>

#include "modules/desktop_capture/desktop_capture_options.h"
#include "modules/desktop_capture/desktop_frame.h"
#include "rtc_base/logging.h"

namespace webrtc {
namespace {

constexpr int32_t kDefaultCaptureWidth = 1920;
constexpr int32_t kDefaultCaptureHeight = 1080;

class ScreenCapturerOhos final : public DesktopCapturer {
 public:
  ScreenCapturerOhos() = default;
  ScreenCapturerOhos(const ScreenCapturerOhos&) = delete;
  ScreenCapturerOhos& operator=(const ScreenCapturerOhos&) = delete;

  ~ScreenCapturerOhos() override {
    if (!capture_) {
      return;
    }
    if (capture_started_) {
      OH_AVScreenCapture_StopScreenCapture(capture_);
    }
    OH_AVScreenCapture_Release(capture_);
  }

  bool Initialize() {
    int32_t width = kDefaultCaptureWidth;
    int32_t height = kDefaultCaptureHeight;
    if (OH_NativeDisplayManager_GetDefaultDisplayWidth(&width) !=
            DISPLAY_MANAGER_OK ||
        OH_NativeDisplayManager_GetDefaultDisplayHeight(&height) !=
            DISPLAY_MANAGER_OK ||
        width <= 0 || height <= 0) {
      width = kDefaultCaptureWidth;
      height = kDefaultCaptureHeight;
    }

    OH_AVScreenCaptureConfig config = {};
    config.captureMode = OH_CAPTURE_SPECIFIED_SCREEN;
    config.dataType = OH_ORIGINAL_STREAM;
    config.videoInfo.videoCapInfo.videoFrameWidth = width;
    config.videoInfo.videoCapInfo.videoFrameHeight = height;
    config.videoInfo.videoCapInfo.videoSource = OH_VIDEO_SOURCE_SURFACE_RGBA;

    capture_ = OH_AVScreenCapture_Create();
    if (!capture_) {
      RTC_LOG(LS_ERROR) << "OH_AVScreenCapture_Create failed";
      return false;
    }
    if (OH_AVScreenCapture_Init(capture_, config) != AV_SCREEN_CAPTURE_ERR_OK) {
      RTC_LOG(LS_ERROR) << "OH_AVScreenCapture_Init failed";
      OH_AVScreenCapture_Release(capture_);
      capture_ = nullptr;
      return false;
    }
    if (OH_AVScreenCapture_SetErrorCallback(capture_, &OnError, this) !=
            AV_SCREEN_CAPTURE_ERR_OK ||
        OH_AVScreenCapture_SetDataCallback(capture_, &OnBufferAvailable,
                                           this) != AV_SCREEN_CAPTURE_ERR_OK ||
        OH_AVScreenCapture_SetStateCallback(capture_, &OnStateChange, this) !=
            AV_SCREEN_CAPTURE_ERR_OK) {
      RTC_LOG(LS_ERROR) << "OH_AVScreenCapture callback setup failed";
      OH_AVScreenCapture_Release(capture_);
      capture_ = nullptr;
      return false;
    }
    return true;
  }

  void Start(Callback* callback) override {
    callback_ = callback;
    if (!capture_ || capture_started_) {
      return;
    }
    OH_AVScreenCapture_SetMicrophoneEnabled(capture_, false);
    OH_AVScreenCapture_SetCanvasRotation(capture_, true);
    if (OH_AVScreenCapture_StartScreenCapture(capture_) !=
        AV_SCREEN_CAPTURE_ERR_OK) {
      std::lock_guard<std::mutex> lock(frame_lock_);
      capture_failed_ = true;
      return;
    }
    capture_started_ = true;
  }

  void CaptureFrame() override {
    if (!callback_) {
      return;
    }

    FrameBuffer frame;
    bool failed = false;
    {
      std::lock_guard<std::mutex> lock(frame_lock_);
      failed = capture_failed_;
      frame = std::move(frame_);
    }
    if (failed) {
      callback_->OnCaptureResult(Result::ERROR_PERMANENT, nullptr);
      return;
    }
    if (frame.data.empty()) {
      callback_->OnCaptureResult(Result::ERROR_TEMPORARY, nullptr);
      return;
    }

    auto desktop_frame = std::make_unique<BasicDesktopFrame>(
        DesktopSize(frame.width, frame.height));
    const int32_t row_bytes = frame.width * DesktopFrame::kBytesPerPixel;
    for (int32_t row = 0; row < frame.height; ++row) {
      std::memcpy(desktop_frame->data() + row * desktop_frame->stride(),
                  frame.data.data() + row * frame.stride, row_bytes);
    }
    desktop_frame->mutable_updated_region()->SetRect(
        DesktopRect::MakeSize(desktop_frame->size()));
    callback_->OnCaptureResult(Result::SUCCESS, std::move(desktop_frame));
  }

  bool GetSourceList(SourceList* sources) override {
    if (!sources) {
      return false;
    }
    sources->push_back({0});
    return true;
  }

  bool SelectSource(SourceId id) override { return id == 0; }

 private:
  struct FrameBuffer {
    int32_t width = 0;
    int32_t height = 0;
    int32_t stride = 0;
    std::vector<uint8_t> data;
  };

  static void OnError(OH_AVScreenCapture* capture,
                      int32_t error_code,
                      void* user_data) {
    (void)capture;
    auto* self = static_cast<ScreenCapturerOhos*>(user_data);
    if (!self) {
      return;
    }
    RTC_LOG(LS_ERROR) << "OH_AVScreenCapture error: " << error_code;
    std::lock_guard<std::mutex> lock(self->frame_lock_);
    self->capture_failed_ = true;
  }

  static void OnStateChange(OH_AVScreenCapture* capture,
                            OH_AVScreenCaptureStateCode state_code,
                            void* user_data) {
    (void)capture;
    auto* self = static_cast<ScreenCapturerOhos*>(user_data);
    if (!self) {
      return;
    }
    switch (state_code) {
      case OH_SCREEN_CAPTURE_STATE_CANCELED:
      case OH_SCREEN_CAPTURE_STATE_STOPPED_BY_USER:
      case OH_SCREEN_CAPTURE_STATE_INTERRUPTED_BY_OTHER:
      case OH_SCREEN_CAPTURE_STATE_STOPPED_BY_CALL: {
        std::lock_guard<std::mutex> lock(self->frame_lock_);
        self->capture_failed_ = true;
        break;
      }
      default:
        break;
    }
  }

  static void OnBufferAvailable(OH_AVScreenCapture* capture,
                                OH_AVBuffer* buffer,
                                OH_AVScreenCaptureBufferType buffer_type,
                                int64_t timestamp,
                                void* user_data) {
    (void)capture;
    (void)timestamp;
    auto* self = static_cast<ScreenCapturerOhos*>(user_data);
    if (!self || !buffer || buffer_type != OH_SCREEN_CAPTURE_BUFFERTYPE_VIDEO) {
      return;
    }

    OH_NativeBuffer* native_buffer = OH_AVBuffer_GetNativeBuffer(buffer);
    if (!native_buffer) {
      return;
    }
    OH_NativeBuffer_Config config = {};
    OH_NativeBuffer_GetConfig(native_buffer, &config);
    OH_NativeBuffer_Unreference(native_buffer);
    if (config.width <= 0 || config.height <= 0 ||
        config.stride < config.width * DesktopFrame::kBytesPerPixel) {
      return;
    }

    const size_t required_size =
        static_cast<size_t>(config.height) * config.stride;
    const size_t capacity = OH_AVBuffer_GetCapacity(buffer);
    const auto* address =
        static_cast<const uint8_t*>(OH_AVBuffer_GetAddr(buffer));
    if (!address || capacity < required_size) {
      return;
    }

    FrameBuffer frame;
    frame.width = config.width;
    frame.height = config.height;
    frame.stride = config.stride;
    frame.data.assign(address, address + required_size);
    std::lock_guard<std::mutex> lock(self->frame_lock_);
    self->frame_ = std::move(frame);
  }

  OH_AVScreenCapture* capture_ = nullptr;
  Callback* callback_ = nullptr;
  bool capture_started_ = false;
  std::mutex frame_lock_;
  FrameBuffer frame_;
  bool capture_failed_ = false;
};

std::unique_ptr<DesktopCapturer> CreateOhosScreenCapturer() {
  auto capturer = std::make_unique<ScreenCapturerOhos>();
  if (!capturer->Initialize()) {
    return nullptr;
  }
  return capturer;
}

}  // namespace

std::unique_ptr<DesktopCapturer> DesktopCapturer::CreateRawScreenCapturer(
    const DesktopCaptureOptions& options) {
  (void)options;
  return CreateOhosScreenCapturer();
}

std::unique_ptr<DesktopCapturer> DesktopCapturer::CreateRawWindowCapturer(
    const DesktopCaptureOptions& options) {
  (void)options;
  return CreateOhosScreenCapturer();
}

}  // namespace webrtc
