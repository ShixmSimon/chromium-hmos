// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_OZONE_PLATFORM_OHOS_OHOS_NATIVE_BUFFER_PROBE_H_
#define UI_OZONE_PLATFORM_OHOS_OHOS_NATIVE_BUFFER_PROBE_H_

namespace gl {
class GLDisplayEGL;
}

namespace ui {

// Whether software-decoded video could reach the GPU through HarmonyOS
// native buffers instead of a per-frame texture upload: allocates 1080p P010
// and NV12 OH_NativeBuffers, logs their planes, and tries to import them as
// EGLImages through ANGLE (dma-buf and EGL_NATIVE_BUFFER_OHOS) and through
// the system EGL. Logs "OHOS native buffer probe" lines, once per process.
void ProbeOhosNativeBufferImport(gl::GLDisplayEGL* display);

}  // namespace ui

#endif  // UI_OZONE_PLATFORM_OHOS_OHOS_NATIVE_BUFFER_PROBE_H_
