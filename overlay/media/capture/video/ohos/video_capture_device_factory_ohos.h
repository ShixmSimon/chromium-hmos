// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MEDIA_CAPTURE_VIDEO_OHOS_VIDEO_CAPTURE_DEVICE_FACTORY_OHOS_H_
#define MEDIA_CAPTURE_VIDEO_OHOS_VIDEO_CAPTURE_DEVICE_FACTORY_OHOS_H_

#include "media/capture/video/video_capture_device_factory.h"

namespace media {

class VideoCaptureDeviceFactoryOhos final : public VideoCaptureDeviceFactory {
 public:
  VideoCaptureDeviceFactoryOhos();
  ~VideoCaptureDeviceFactoryOhos() override;

  VideoCaptureDeviceFactoryOhos(const VideoCaptureDeviceFactoryOhos&) = delete;
  VideoCaptureDeviceFactoryOhos& operator=(
      const VideoCaptureDeviceFactoryOhos&) = delete;

  VideoCaptureErrorOrDevice CreateDevice(
      const VideoCaptureDeviceDescriptor& device_descriptor) override;
  void GetDevicesInfo(GetDevicesInfoCallback callback) override;
};

}  // namespace media

#endif  // MEDIA_CAPTURE_VIDEO_OHOS_VIDEO_CAPTURE_DEVICE_FACTORY_OHOS_H_
