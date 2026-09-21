// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/capture/video/ohos/video_capture_device_ohos.h"

#include <dlfcn.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <native_buffer/native_buffer.h>
#include <native_image/native_image.h>
#include <native_window/external_window.h>
#include <ohcamera/camera.h>
#include <ohcamera/camera_device.h>
#include <ohcamera/camera_input.h>
#include <ohcamera/camera_manager.h>
#include <ohcamera/capture_session.h>
#include <ohcamera/preview_output.h>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/memory/weak_ptr.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "media/base/video_frame.h"

namespace media {

namespace {

constexpr int kFenceWaitTimeoutMs = 1000;
constexpr float kDefaultFrameRate = 30.0f;
constexpr char kCameraPermission[] = "ohos.permission.CAMERA";
constexpr base::TimeDelta kPermissionPollInterval = base::Milliseconds(100);
constexpr base::TimeDelta kPermissionWaitTimeout = base::Seconds(60);

bool HasCameraPermission() {
  using CheckSelfPermission = bool (*)(const char*);
  static CheckSelfPermission check_permission = []() {
    void* library =
        dlopen("libability_access_control.so", RTLD_NOW | RTLD_LOCAL);
    if (!library) {
      LOG(ERROR) << "OHOS camera: failed to load ability_access_control";
      return static_cast<CheckSelfPermission>(nullptr);
    }
    return reinterpret_cast<CheckSelfPermission>(
        dlsym(library, "OH_AT_CheckSelfPermission"));
  }();
  return check_permission && check_permission(kCameraPermission);
}

bool CopyPlane(const uint8_t* mapped,
               size_t mapped_size,
               const OH_NativeBuffer_Plane& plane,
               int width,
               int height,
               uint8_t* destination,
               bool* corrected_stride) {
  if (!mapped || !destination || width <= 0 || height <= 0) {
    return false;
  }

  size_t row_stride = plane.rowStride;
  size_t column_stride = plane.columnStride;
  if (row_stride == 0 || column_stride == 0) {
    return false;
  }

  // Some CameraKit YUV_420_SP buffers report chroma rowStride and
  // columnStride in the opposite fields. Accept that only when the reported
  // values cannot describe one row and the swapped values can.
  const size_t row_bytes = (static_cast<size_t>(width) - 1) * column_stride + 1;
  if (row_stride < row_bytes) {
    const size_t swapped_row_bytes =
        (static_cast<size_t>(width) - 1) * row_stride + 1;
    if (column_stride < swapped_row_bytes) {
      return false;
    }
    std::swap(row_stride, column_stride);
    *corrected_stride = true;
  }

  size_t last_byte = plane.offset;
  const size_t last_row = static_cast<size_t>(height) - 1;
  const size_t last_column = static_cast<size_t>(width) - 1;
  if (last_byte >= mapped_size ||
      (last_row && row_stride > (mapped_size - 1 - last_byte) / last_row)) {
    return false;
  }
  last_byte += last_row * row_stride;
  if (last_column &&
      column_stride > (mapped_size - 1 - last_byte) / last_column) {
    return false;
  }

  const uint8_t* source = mapped + plane.offset;
  if (column_stride <= 1) {
    for (int row = 0; row < height; ++row) {
      std::memcpy(destination + row * width, source + row * row_stride, width);
    }
    return true;
  }

  for (int row = 0; row < height; ++row) {
    const uint8_t* source_row = source + row * row_stride;
    uint8_t* destination_row = destination + row * width;
    for (int column = 0; column < width; ++column) {
      destination_row[column] = source_row[column * column_stride];
    }
  }
  return true;
}

bool CopyInterleavedPlane(const uint8_t* mapped,
                          size_t mapped_size,
                          const OH_NativeBuffer_Plane& plane,
                          int width,
                          int height,
                          uint8_t* destination,
                          bool* corrected_stride) {
  size_t row_stride = plane.rowStride;
  if (row_stride < static_cast<size_t>(width) &&
      plane.columnStride >= static_cast<uint32_t>(width)) {
    row_stride = plane.columnStride;
    *corrected_stride = true;
  }
  if (!mapped || !destination || width <= 0 || height <= 0 ||
      row_stride < static_cast<size_t>(width) || plane.offset >= mapped_size ||
      mapped_size - plane.offset < static_cast<size_t>(width)) {
    return false;
  }
  const size_t last_row = static_cast<size_t>(height) - 1;
  if (last_row &&
      row_stride > (mapped_size - plane.offset - width) / last_row) {
    return false;
  }
  const uint8_t* source = mapped + plane.offset;
  for (int row = 0; row < height; ++row) {
    std::memcpy(destination + static_cast<size_t>(row) * width,
                source + static_cast<size_t>(row) * row_stride, width);
  }
  return true;
}

bool IsCrCbFormat(int32_t format) {
  return format == NATIVEBUFFER_PIXEL_FMT_YCRCB_420_SP ||
         format == NATIVEBUFFER_PIXEL_FMT_YCRCB_420_P;
}

bool WaitForFence(int fence_fd) {
  if (fence_fd < 0) {
    return true;
  }
  pollfd descriptor = {.fd = fence_fd, .events = POLLIN, .revents = 0};
  const int result = poll(&descriptor, 1, kFenceWaitTimeoutMs);
  close(fence_fd);
  return result > 0;
}

}  // namespace

class CaptureDelegateOhos {
 public:
  CaptureDelegateOhos(
      VideoCaptureDeviceDescriptor descriptor,
      VideoCaptureParams params,
      scoped_refptr<base::SingleThreadTaskRunner> capture_task_runner)
      : descriptor_(std::move(descriptor)),
        params_(std::move(params)),
        capture_task_runner_(std::move(capture_task_runner)) {}

  CaptureDelegateOhos(const CaptureDelegateOhos&) = delete;
  CaptureDelegateOhos& operator=(const CaptureDelegateOhos&) = delete;

  ~CaptureDelegateOhos() { ReleaseResources(); }

  void AllocateAndStart(std::unique_ptr<VideoCaptureDevice::Client> client) {
    client_ = std::move(client);
    permission_wait_deadline_ = base::TimeTicks::Now() + kPermissionWaitTimeout;
    StartWhenCameraPermissionIsReady();
  }

  void StartWhenCameraPermissionIsReady() {
    if (!client_) {
      return;
    }
    if (!HasCameraPermission()) {
      if (base::TimeTicks::Now() < permission_wait_deadline_) {
        capture_task_runner_->PostDelayedTask(
            FROM_HERE,
            base::BindOnce(
                &CaptureDelegateOhos::StartWhenCameraPermissionIsReady,
                weak_factory_.GetWeakPtr()),
            kPermissionPollInterval);
        return;
      }
      client_->OnError(VideoCaptureError::kWebRtcStartCaptureFailed, FROM_HERE,
                       "HarmonyOS camera permission was not granted");
      return;
    }
    if (!InitializeCamera()) {
      ReleaseResources();
      if (client_) {
        client_->OnError(VideoCaptureError::kWebRtcStartCaptureFailed,
                         FROM_HERE,
                         "CameraKit failed to start the OHOS camera");
      }
      return;
    }
    first_reference_time_ = base::TimeTicks();
    client_->OnStarted();
  }

  void StopAndDeAllocate() {
    weak_factory_.InvalidateWeakPtrs();
    ReleaseResources();
    client_.reset();
  }

  void InvalidateBuffers() {
    if (client_) {
      client_->InvalidateBuffers();
    }
  }

  void ProcessFrame() {
    if (frame_signal_) {
      frame_signal_->task_pending.store(false, std::memory_order_release);
    }
    if (!client_ || !native_image_ || !started_) {
      return;
    }

    OHNativeWindowBuffer* window_buffer = nullptr;
    int fence_fd = -1;
    if (OH_NativeImage_AcquireNativeWindowBuffer(native_image_, &window_buffer,
                                                 &fence_fd) != 0 ||
        !window_buffer) {
      if (fence_fd >= 0) {
        close(fence_fd);
      }
      return;
    }
    if (OH_NativeWindow_NativeObjectReference(window_buffer) != 0) {
      if (fence_fd >= 0) {
        close(fence_fd);
      }
      OH_NativeImage_ReleaseNativeWindowBuffer(native_image_, window_buffer,
                                               -1);
      return;
    }

    auto release_buffer = [this, window_buffer]() {
      if (OH_NativeImage_ReleaseNativeWindowBuffer(native_image_, window_buffer,
                                                   -1) != 0) {
        // A failed release does not drop the reference acquired by
        // AcquireNativeWindowBuffer.
        OH_NativeWindow_NativeObjectUnreference(window_buffer);
      }
      // Drop the explicit reference taken above.
      OH_NativeWindow_NativeObjectUnreference(window_buffer);
    };

    if (!WaitForFence(fence_fd)) {
      release_buffer();
      return;
    }

    OH_NativeBuffer* native_buffer = nullptr;
    if (OH_NativeBuffer_FromNativeWindowBuffer(window_buffer, &native_buffer) !=
            0 ||
        !native_buffer) {
      release_buffer();
      return;
    }

    OH_NativeBuffer_Config config = {};
    OH_NativeBuffer_GetConfig(native_buffer, &config);
    BufferHandle* buffer_handle =
        OH_NativeWindow_GetBufferHandleFromNative(window_buffer);
    if (config.width <= 0 || config.height <= 0 || (config.width & 1) ||
        (config.height & 1) || !buffer_handle || buffer_handle->size <= 0) {
      release_buffer();
      return;
    }

    void* mapped_address = nullptr;
    OH_NativeBuffer_Planes planes = {};
    if (OH_NativeBuffer_MapPlanes(native_buffer, &mapped_address, &planes) !=
            0 ||
        !mapped_address || planes.planeCount == 0) {
      release_buffer();
      return;
    }

    const int width = config.width;
    const int height = config.height;
    const size_t y_size = static_cast<size_t>(width) * height;
    const size_t frame_size = y_size + y_size / 2;
    if (frame_size > static_cast<size_t>(std::numeric_limits<int>::max())) {
      OH_NativeBuffer_Unmap(native_buffer);
      release_buffer();
      return;
    }

    std::vector<uint8_t> frame(frame_size);
    const uint8_t* mapped = static_cast<const uint8_t*>(mapped_address);
    const size_t mapped_size = static_cast<size_t>(buffer_handle->size);
    bool corrected_stride = false;
    bool copied = CopyPlane(mapped, mapped_size, planes.planes[0], width,
                            height, frame.data(), &corrected_stride);

    VideoPixelFormat pixel_format = PIXEL_FORMAT_NV21;
    if (copied && planes.planeCount >= 3) {
      const uint32_t u_plane = IsCrCbFormat(config.format) ? 2 : 1;
      const uint32_t v_plane = IsCrCbFormat(config.format) ? 1 : 2;
      copied =
          CopyPlane(mapped, mapped_size, planes.planes[u_plane], width / 2,
                    height / 2, frame.data() + y_size, &corrected_stride) &&
          CopyPlane(mapped, mapped_size, planes.planes[v_plane], width / 2,
                    height / 2, frame.data() + y_size + y_size / 4,
                    &corrected_stride);
      pixel_format = PIXEL_FORMAT_I420;
    } else if (copied && planes.planeCount == 2) {
      copied = CopyInterleavedPlane(mapped, mapped_size, planes.planes[1],
                                    width, height / 2, frame.data() + y_size,
                                    &corrected_stride);
      pixel_format =
          IsCrCbFormat(config.format) ? PIXEL_FORMAT_NV21 : PIXEL_FORMAT_NV12;
    } else if (copied) {
      const uint32_t stride = config.stride > 0 ? config.stride : width;
      OH_NativeBuffer_Plane chroma_plane = {
          .offset = static_cast<uint64_t>(stride) * height,
          .rowStride = stride,
          .columnStride = 1,
      };
      copied = CopyInterleavedPlane(mapped, mapped_size, chroma_plane, width,
                                    height / 2, frame.data() + y_size,
                                    &corrected_stride);
      if (copied) {
        pixel_format =
            IsCrCbFormat(config.format) ? PIXEL_FORMAT_NV21 : PIXEL_FORMAT_NV12;
      }
    }

    OH_NativeBuffer_Unmap(native_buffer);
    release_buffer();
    if (!copied) {
      LOG(ERROR) << "OHOS camera: invalid plane layout for " << width << "x"
                 << height << " buffer of " << mapped_size << " bytes";
      return;
    }
    if (corrected_stride && !logged_stride_correction_) {
      logged_stride_correction_ = true;
      LOG(WARNING) << "OHOS camera: corrected swapped CameraKit YUV strides";
    }

    const base::TimeTicks now = base::TimeTicks::Now();
    if (first_reference_time_.is_null()) {
      first_reference_time_ = now;
    }
    VideoCaptureFormat format(gfx::Size(width, height), frame_rate_,
                              pixel_format);
    client_->OnIncomingCapturedData(frame, format, gfx::ColorSpace(), rotation_,
                                    false, now, now - first_reference_time_,
                                    std::nullopt, std::nullopt);
  }

 private:
  struct FrameSignal {
    scoped_refptr<base::SingleThreadTaskRunner> task_runner;
    base::WeakPtr<CaptureDelegateOhos> delegate;
    std::atomic<bool> task_pending{false};
  };

  static void OnFrameAvailable(void* context) {
    auto* signal = static_cast<FrameSignal*>(context);
    if (!signal) {
      return;
    }
    bool expected = false;
    if (!signal->task_pending.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
      return;
    }
    if (!signal->task_runner->PostTask(
            FROM_HERE, base::BindOnce(&CaptureDelegateOhos::ProcessFrame,
                                      signal->delegate))) {
      signal->task_pending.store(false, std::memory_order_release);
    }
  }

  bool InitializeCamera() {
    if (OH_Camera_GetCameraManager(&manager_) != CAMERA_OK || !manager_) {
      return false;
    }

    Camera_Device* cameras = nullptr;
    uint32_t camera_count = 0;
    if (OH_CameraManager_GetSupportedCameras(manager_, &cameras,
                                             &camera_count) != CAMERA_OK ||
        !cameras) {
      return false;
    }

    Camera_Device* selected_camera = nullptr;
    for (uint32_t i = 0; i < camera_count; ++i) {
      if (cameras[i].cameraId && descriptor_.device_id == cameras[i].cameraId) {
        selected_camera = &cameras[i];
        break;
      }
    }
    if (!selected_camera) {
      OH_CameraManager_DeleteSupportedCameras(manager_, cameras, camera_count);
      return false;
    }

    Camera_OutputCapability* capability = nullptr;
    if (OH_CameraManager_GetSupportedCameraOutputCapability(
            manager_, selected_camera, &capability) != CAMERA_OK ||
        !capability) {
      OH_CameraManager_DeleteSupportedCameras(manager_, cameras, camera_count);
      return false;
    }

    const Camera_Profile* selected_profile = nullptr;
    int64_t best_distance = std::numeric_limits<int64_t>::max();
    const int requested_width = params_.requested_format.frame_size.width();
    const int requested_height = params_.requested_format.frame_size.height();
    for (uint32_t i = 0;
         capability->previewProfiles && i < capability->previewProfilesSize;
         ++i) {
      const Camera_Profile* profile = capability->previewProfiles[i];
      if (!profile || profile->format != CAMERA_FORMAT_YUV_420_SP ||
          profile->size.width == 0 || profile->size.height == 0 ||
          (profile->size.width & 1) || (profile->size.height & 1)) {
        continue;
      }
      const int64_t distance =
          std::abs(static_cast<int64_t>(profile->size.width) -
                   requested_width) +
          std::abs(static_cast<int64_t>(profile->size.height) -
                   requested_height);
      if (distance < best_distance) {
        best_distance = distance;
        selected_profile = profile;
      }
    }

    bool initialized = false;
    if (selected_profile &&
        OH_CameraManager_CreateCameraInput(manager_, selected_camera,
                                           &camera_input_) == CAMERA_OK &&
        camera_input_) {
      uint32_t orientation = 0;
      if (OH_CameraDevice_GetCameraOrientation(selected_camera, &orientation) ==
          CAMERA_OK) {
        const int normalized_orientation = static_cast<int>(orientation % 360);
        if (normalized_orientation == 0 || normalized_orientation == 90 ||
            normalized_orientation == 180 || normalized_orientation == 270) {
          rotation_ = normalized_orientation;
        }
      }

      native_image_ = OH_ConsumerSurface_Create();
      if (native_image_ &&
          OH_ConsumerSurface_SetDefaultUsage(
              native_image_, NATIVEBUFFER_USAGE_CPU_READ |
                                 NATIVEBUFFER_USAGE_CPU_READ_OFTEN) == 0 &&
          OH_ConsumerSurface_SetDefaultSize(
              native_image_, selected_profile->size.width,
              selected_profile->size.height) == 0) {
        uint64_t surface_id = 0;
        if (OH_NativeImage_GetSurfaceId(native_image_, &surface_id) == 0) {
          const std::string surface_id_string = std::to_string(surface_id);
          if (OH_CameraManager_CreatePreviewOutput(
                  manager_, selected_profile, surface_id_string.c_str(),
                  &preview_output_) == CAMERA_OK &&
              preview_output_) {
            frame_signal_ = std::make_unique<FrameSignal>();
            frame_signal_->task_runner = capture_task_runner_;
            frame_signal_->delegate = weak_factory_.GetWeakPtr();
            OH_OnFrameAvailableListener listener = {
                .context = frame_signal_.get(),
                .onFrameAvailable = &CaptureDelegateOhos::OnFrameAvailable};
            if (OH_NativeImage_SetOnFrameAvailableListener(native_image_,
                                                           listener) == 0 &&
                OH_CameraInput_Open(camera_input_) == CAMERA_OK &&
                OH_CameraManager_CreateCaptureSession(manager_, &session_) ==
                    CAMERA_OK &&
                session_ &&
                OH_CaptureSession_BeginConfig(session_) == CAMERA_OK &&
                OH_CaptureSession_AddInput(session_, camera_input_) ==
                    CAMERA_OK &&
                OH_CaptureSession_AddPreviewOutput(session_, preview_output_) ==
                    CAMERA_OK &&
                OH_CaptureSession_CommitConfig(session_) == CAMERA_OK &&
                OH_CaptureSession_Start(session_) == CAMERA_OK) {
              frame_rate_ = params_.requested_format.frame_rate > 0
                                ? params_.requested_format.frame_rate
                                : kDefaultFrameRate;
              started_ = true;
              initialized = true;
            }
          }
        }
      }
    }

    OH_CameraManager_DeleteSupportedCameraOutputCapability(manager_,
                                                           capability);
    OH_CameraManager_DeleteSupportedCameras(manager_, cameras, camera_count);
    return initialized;
  }

  void ReleaseResources() {
    if (native_image_) {
      OH_NativeImage_UnsetOnFrameAvailableListener(native_image_);
    }
    weak_factory_.InvalidateWeakPtrs();
    if (session_) {
      if (started_) {
        OH_CaptureSession_Stop(session_);
      }
      OH_CaptureSession_Release(session_);
      session_ = nullptr;
    }
    started_ = false;
    if (preview_output_) {
      OH_PreviewOutput_Release(preview_output_);
      preview_output_ = nullptr;
    }
    if (camera_input_) {
      OH_CameraInput_Release(camera_input_);
      camera_input_ = nullptr;
    }
    if (native_image_) {
      OH_NativeImage_Destroy(&native_image_);
    }
    frame_signal_.reset();
    if (manager_) {
      OH_Camera_DeleteCameraManager(manager_);
      manager_ = nullptr;
    }
  }

  const VideoCaptureDeviceDescriptor descriptor_;
  const VideoCaptureParams params_;
  const scoped_refptr<base::SingleThreadTaskRunner> capture_task_runner_;
  std::unique_ptr<VideoCaptureDevice::Client> client_;
  Camera_Manager* manager_ = nullptr;
  Camera_Input* camera_input_ = nullptr;
  Camera_CaptureSession* session_ = nullptr;
  Camera_PreviewOutput* preview_output_ = nullptr;
  OH_NativeImage* native_image_ = nullptr;
  std::unique_ptr<FrameSignal> frame_signal_;
  base::TimeTicks first_reference_time_;
  base::TimeTicks permission_wait_deadline_;
  float frame_rate_ = kDefaultFrameRate;
  int rotation_ = 0;
  bool started_ = false;
  bool logged_stride_correction_ = false;
  base::WeakPtrFactory<CaptureDelegateOhos> weak_factory_{this};
};

VideoCaptureDeviceOhos::VideoCaptureDeviceOhos(
    const VideoCaptureDeviceDescriptor& device_descriptor)
    : device_descriptor_(device_descriptor) {}

VideoCaptureDeviceOhos::~VideoCaptureDeviceOhos() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (capture_thread_.IsRunning()) {
    StopAndDeAllocate();
  }
}

void VideoCaptureDeviceOhos::AllocateAndStart(
    const VideoCaptureParams& params,
    std::unique_ptr<VideoCaptureDevice::Client> client) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (capture_thread_.IsRunning() || !client) {
    if (client) {
      client->OnError(VideoCaptureError::kWebRtcStartCaptureFailed, FROM_HERE,
                      "OHOS camera is already running");
    }
    return;
  }
  if (!capture_thread_.Start()) {
    client->OnError(VideoCaptureError::kWebRtcStartCaptureFailed, FROM_HERE,
                    "Failed to start the OHOS camera capture thread");
    return;
  }

  capture_delegate_ = std::make_unique<CaptureDelegateOhos>(
      device_descriptor_, params, capture_thread_.task_runner());
  capture_thread_.task_runner()->PostTask(
      FROM_HERE, base::BindOnce(&CaptureDelegateOhos::AllocateAndStart,
                                base::Unretained(capture_delegate_.get()),
                                std::move(client)));
}

void VideoCaptureDeviceOhos::StopAndDeAllocate() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!capture_thread_.IsRunning()) {
    return;
  }
  if (capture_delegate_) {
    capture_thread_.task_runner()->PostTask(
        FROM_HERE, base::BindOnce(&CaptureDelegateOhos::StopAndDeAllocate,
                                  base::Unretained(capture_delegate_.get())));
    capture_thread_.task_runner()->DeleteSoon(FROM_HERE,
                                              capture_delegate_.release());
  }
  capture_thread_.Stop();
}

void VideoCaptureDeviceOhos::InvalidateBuffers() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!capture_thread_.IsRunning() || !capture_delegate_) {
    return;
  }
  capture_thread_.task_runner()->PostTask(
      FROM_HERE, base::BindOnce(&CaptureDelegateOhos::InvalidateBuffers,
                                base::Unretained(capture_delegate_.get())));
}

}  // namespace media
