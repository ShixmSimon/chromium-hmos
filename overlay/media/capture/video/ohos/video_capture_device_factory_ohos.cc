// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/capture/video/ohos/video_capture_device_factory_ohos.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include <ohcamera/camera.h>
#include <ohcamera/camera_manager.h>

#include "base/logging.h"
#include "media/capture/video/ohos/video_capture_device_ohos.h"

namespace media {

namespace {

constexpr float kDefaultFrameRate = 30.0f;

VideoFacingMode ToFacingMode(Camera_Position position) {
  switch (position) {
    case CAMERA_POSITION_FRONT:
      return MEDIA_VIDEO_FACING_USER;
    case CAMERA_POSITION_BACK:
      return MEDIA_VIDEO_FACING_ENVIRONMENT;
    default:
      return MEDIA_VIDEO_FACING_NONE;
  }
}

std::string DisplayName(const Camera_Device& camera) {
  switch (camera.cameraPosition) {
    case CAMERA_POSITION_FRONT:
      return "Front camera";
    case CAMERA_POSITION_BACK:
      return "Back camera";
    default:
      return "Camera " + std::string(camera.cameraId ? camera.cameraId : "");
  }
}

std::vector<VideoCaptureDeviceInfo> EnumerateDevices() {
  std::vector<VideoCaptureDeviceInfo> devices;
  Camera_Manager* manager = nullptr;
  if (OH_Camera_GetCameraManager(&manager) != CAMERA_OK || !manager) {
    LOG(ERROR) << "OHOS camera: failed to create camera manager";
    return devices;
  }

  Camera_Device* cameras = nullptr;
  uint32_t camera_count = 0;
  const Camera_ErrorCode enumerate_result =
      OH_CameraManager_GetSupportedCameras(manager, &cameras, &camera_count);
  if (enumerate_result != CAMERA_OK || !cameras) {
    LOG(ERROR) << "OHOS camera: failed to enumerate devices: "
               << enumerate_result;
    OH_Camera_DeleteCameraManager(manager);
    return devices;
  }

  for (uint32_t i = 0; i < camera_count; ++i) {
    const Camera_Device& camera = cameras[i];
    if (!camera.cameraId) {
      continue;
    }

    VideoCaptureDeviceInfo info(VideoCaptureDeviceDescriptor(
        DisplayName(camera), camera.cameraId, std::string(),
        VideoCaptureApi::UNKNOWN, VideoCaptureControlSupport(),
        VideoCaptureTransportType::OTHER_TRANSPORT,
        ToFacingMode(camera.cameraPosition)));

    Camera_OutputCapability* capability = nullptr;
    if (OH_CameraManager_GetSupportedCameraOutputCapability(
            manager, &camera, &capability) == CAMERA_OK &&
        capability) {
      for (uint32_t profile_index = 0;
           capability->previewProfiles &&
           profile_index < capability->previewProfilesSize;
           ++profile_index) {
        const Camera_Profile* profile =
            capability->previewProfiles[profile_index];
        if (!profile || profile->format != CAMERA_FORMAT_YUV_420_SP ||
            profile->size.width == 0 || profile->size.height == 0 ||
            (profile->size.width & 1) || (profile->size.height & 1)) {
          continue;
        }
        VideoCaptureFormat format(
            gfx::Size(static_cast<int>(profile->size.width),
                      static_cast<int>(profile->size.height)),
            kDefaultFrameRate, PIXEL_FORMAT_NV21);
        if (std::find(info.supported_formats.begin(),
                      info.supported_formats.end(),
                      format) == info.supported_formats.end()) {
          info.supported_formats.push_back(format);
        }
      }
      OH_CameraManager_DeleteSupportedCameraOutputCapability(manager,
                                                             capability);
    }

    if (!info.supported_formats.empty()) {
      devices.push_back(std::move(info));
    }
  }

  OH_CameraManager_DeleteSupportedCameras(manager, cameras, camera_count);
  OH_Camera_DeleteCameraManager(manager);

  std::stable_sort(devices.begin(), devices.end(),
                   [](const VideoCaptureDeviceInfo& left,
                      const VideoCaptureDeviceInfo& right) {
                     return left.descriptor.facing == MEDIA_VIDEO_FACING_USER &&
                            right.descriptor.facing != MEDIA_VIDEO_FACING_USER;
                   });
  return devices;
}

}  // namespace

VideoCaptureDeviceFactoryOhos::VideoCaptureDeviceFactoryOhos() = default;

VideoCaptureDeviceFactoryOhos::~VideoCaptureDeviceFactoryOhos() = default;

VideoCaptureErrorOrDevice VideoCaptureDeviceFactoryOhos::CreateDevice(
    const VideoCaptureDeviceDescriptor& device_descriptor) {
  DCHECK(thread_checker_.CalledOnValidThread());
  const std::vector<VideoCaptureDeviceInfo> devices = EnumerateDevices();
  const auto device = std::find_if(
      devices.begin(), devices.end(),
      [&device_descriptor](const VideoCaptureDeviceInfo& info) {
        return info.descriptor.device_id == device_descriptor.device_id;
      });
  if (device == devices.end()) {
    return VideoCaptureErrorOrDevice(
        VideoCaptureError::kVideoCaptureSystemDeviceIdNotFound);
  }
  return VideoCaptureErrorOrDevice(
      std::make_unique<VideoCaptureDeviceOhos>(device_descriptor));
}

void VideoCaptureDeviceFactoryOhos::GetDevicesInfo(
    GetDevicesInfoCallback callback) {
  DCHECK(thread_checker_.CalledOnValidThread());
  std::move(callback).Run(EnumerateDevices());
}

}  // namespace media
