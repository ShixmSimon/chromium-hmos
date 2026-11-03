// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
// Native API experiment, not an overlay capability advertisement. Both
// consumers are owned by this probe; no buffers are sent to RenderService.
#include <native_buffer/native_buffer.h>
#include <native_image/native_image.h>
#include <native_window/external_window.h>
#include <hilog/log.h>
#include <napi/native_api.h>

#include <cerrno>
#include <chrono>
#include <initializer_list>
#include <poll.h>
#include <stdexcept>
#include <unistd.h>

namespace {
constexpr unsigned kDomain = 0xD003F00;
constexpr char kTag[] = "LumieOverlayProbe";

void Check(const char* step, int result) {
  if (result != 0) {
    OH_LOG_Print(LOG_APP, LOG_ERROR, kDomain, kTag,
                 "step=%{public}s result=%{public}d", step, result);
    throw std::runtime_error(step);
  }
}

class Fence {
 public:
  ~Fence() { if (fd >= 0) close(fd); }
  int Take() { int result = fd; fd = -1; return result; }
  void Wait() {
    if (fd < 0) return;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(2);
    for (;;) {
      auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now()).count();
      if (remaining <= 0) throw std::runtime_error("fence timeout");
      pollfd entry{fd, POLLIN, 0};
      int result = poll(&entry, 1, static_cast<int>(remaining));
      if (result < 0 && errno == EINTR) continue;
      if (result != 1 || (entry.revents & (POLLERR | POLLNVAL)) ||
          !(entry.revents & POLLIN))
        throw std::runtime_error("fence wait");
      close(Take());
      return;
    }
  }
  int fd = -1;
};

class Consumer {
 public:
  Consumer() {
    image = OH_ConsumerSurface_Create();
    if (!image) throw std::runtime_error("create consumer");
    // Do not throw out of the constructor after acquiring the native object.
  }
  void Initialize(int format) {
    Check("listener", OH_NativeImage_SetOnFrameAvailableListener(
        image, {nullptr, [](void*) {}}));
    listening = true;
    window = OH_NativeImage_AcquireNativeWindow(image);
    if (!window) throw std::runtime_error("acquire window");
    Resize(64);
    Check("format", OH_NativeWindow_NativeWindowHandleOpt(
        window, SET_FORMAT, format));
    const uint64_t usage = NATIVEBUFFER_USAGE_HW_TEXTURE |
        NATIVEBUFFER_USAGE_HW_RENDER | NATIVEBUFFER_USAGE_MEM_DMA;
    Check("usage", OH_NativeWindow_NativeWindowHandleOpt(
        window, SET_USAGE, usage));
    Check("timeout", OH_NativeWindow_NativeWindowHandleOpt(
        window, SET_TIMEOUT, int32_t{0}));
  }
  void Resize(int width) {
    Check("geometry", OH_NativeWindow_NativeWindowHandleOpt(
        window, SET_BUFFER_GEOMETRY, width, 64));
  }
  ~Consumer() {
    if (listening) OH_NativeImage_UnsetOnFrameAvailableListener(image);
    OH_NativeImage_Destroy(&image);
  }
  Consumer(const Consumer&) = delete;
  Consumer& operator=(const Consumer&) = delete;
  OH_NativeImage* image = nullptr;
  OHNativeWindow* window = nullptr;  // Borrowed from image.
  bool listening = false;
};

class AcquiredBuffer {
 public:
  explicit AcquiredBuffer(Consumer& owner) : owner_(owner) {}
  void Acquire() {
    Check("acquire", OH_NativeImage_AcquireNativeWindowBuffer(
        owner_.image, &buffer, &fence_.fd));
    if (!buffer) throw std::runtime_error("null acquired buffer");
    Check("reference", OH_NativeWindow_NativeObjectReference(buffer));
    referenced_ = true;
    fence_.Wait();
  }
  int Release() {
    if (!buffer) return 0;
    const int result = OH_NativeImage_ReleaseNativeWindowBuffer(
        owner_.image, buffer, fence_.Take());
    if (referenced_) OH_NativeWindow_NativeObjectUnreference(buffer);
    buffer = nullptr;
    return result;
  }
  ~AcquiredBuffer() { Release(); }
  OHNativeWindowBuffer* buffer = nullptr;
 private:
  Consumer& owner_;
  Fence fence_;
  bool referenced_ = false;
};

uint32_t Identity(OHNativeWindowBuffer* buffer) {
  OH_NativeBuffer* native = nullptr;
  Check("native buffer", OH_NativeBuffer_FromNativeWindowBuffer(buffer, &native));
  if (!native) throw std::runtime_error("null native buffer");
  return OH_NativeBuffer_GetSeqNum(native);
}

bool RunCase(int format, bool resize_target) {
  Consumer source;
  source.Initialize(format);
  OHNativeWindowBuffer* produced = nullptr;  // Borrowed from source window.
  Fence producer_fence;
  Check("source request", OH_NativeWindow_NativeWindowRequestBuffer(
      source.window, &produced, &producer_fence.fd));
  producer_fence.Wait();
  Check("source flush", OH_NativeWindow_NativeWindowFlushBuffer(
      source.window, produced, -1, {nullptr, 0}));
  AcquiredBuffer source_frame(source);
  source_frame.Acquire();
  const uint32_t original_id = Identity(source_frame.buffer);

  // Destruction order matters: target and its queue go away before releasing
  // the source lease, including on an error. Neither queue has a GPU consumer.
  Consumer target;
  target.Initialize(format);
  Check("target attach", OH_NativeWindow_NativeWindowAttachBuffer(
      target.window, source_frame.buffer));
  Check("target flush", OH_NativeWindow_NativeWindowFlushBuffer(
      target.window, source_frame.buffer, -1, {nullptr, 0}));
  AcquiredBuffer target_frame(target);
  target_frame.Acquire();
  if (Identity(target_frame.buffer) != original_id)
    throw std::runtime_error("target did not receive original allocation");
  Check("target release", target_frame.Release());

  if (resize_target) target.Resize(128);
  OHNativeWindowBuffer* returned = nullptr;  // Borrowed from target window.
  Fence release_fence;
  Check("target request return", OH_NativeWindow_NativeWindowRequestBuffer(
      target.window, &returned, &release_fence.fd));
  release_fence.Wait();
  const uint32_t returned_id = Identity(returned);
  Check("target detach", OH_NativeWindow_NativeWindowDetachBuffer(
      target.window, returned));
  const bool same_allocation = original_id == returned_id;
  OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag,
      "format=%{public}d resize=%{public}d original=%{public}u "
      "returned=%{public}u same_allocation=%{public}d",
      format, resize_target, original_id, returned_id, same_allocation);
  // The resize experiment demonstrates why merely requesting another buffer
  // cannot acknowledge release of a specific decoded frame. In this probe we
  // explicitly performed the consumer Release above, so cleanup is safe.
  return same_allocation != resize_target;
}

int RunProbe() {
  int failed = 0;
  for (int format : {NATIVEBUFFER_PIXEL_FMT_RGBA_8888,
                     NATIVEBUFFER_PIXEL_FMT_YCBCR_420_SP,
                     NATIVEBUFFER_PIXEL_FMT_YCBCR_P010}) {
    for (bool resized : {false, true}) {
      try {
        if (!RunCase(format, resized)) ++failed;
      } catch (const std::exception& error) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, kDomain, kTag,
            "format=%{public}d resize=%{public}d failed=%{public}s",
            format, resized, error.what());
        ++failed;
      }
    }
  }
  OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag,
               "cases=6 failed=%{public}d", failed);
  return failed;
}
}  // namespace

// Can be linked into an existing *debug-only* NAPI harness. Call explicitly
// after app/engine startup, not from static constructors or before phone fork.
extern "C" napi_value ProbeOverlay(napi_env env, napi_callback_info) {
  napi_value result;
  napi_create_int32(env, RunProbe(), &result);
  return result;
}
