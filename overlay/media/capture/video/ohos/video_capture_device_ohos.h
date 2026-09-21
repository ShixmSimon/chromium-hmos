// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MEDIA_CAPTURE_VIDEO_OHOS_VIDEO_CAPTURE_DEVICE_OHOS_H_
#define MEDIA_CAPTURE_VIDEO_OHOS_VIDEO_CAPTURE_DEVICE_OHOS_H_

#include <memory>

#include "base/threading/thread.h"
#include "media/capture/video/video_capture_device.h"

namespace media {

class CaptureDelegateOhos;

class VideoCaptureDeviceOhos final : public VideoCaptureDevice {
 public:
  explicit VideoCaptureDeviceOhos(
      const VideoCaptureDeviceDescriptor& device_descriptor);
  ~VideoCaptureDeviceOhos() override;

  VideoCaptureDeviceOhos(const VideoCaptureDeviceOhos&) = delete;
  VideoCaptureDeviceOhos& operator=(const VideoCaptureDeviceOhos&) = delete;

  void AllocateAndStart(const VideoCaptureParams& params,
                        std::unique_ptr<Client> client) override;
  void StopAndDeAllocate() override;

 private:
  const VideoCaptureDeviceDescriptor device_descriptor_;
  base::Thread capture_thread_{"OHOSCameraCaptureThread"};
  std::unique_ptr<CaptureDelegateOhos> capture_delegate_;

  SEQUENCE_CHECKER(sequence_checker_);
};

}  // namespace media

#endif  // MEDIA_CAPTURE_VIDEO_OHOS_VIDEO_CAPTURE_DEVICE_OHOS_H_
