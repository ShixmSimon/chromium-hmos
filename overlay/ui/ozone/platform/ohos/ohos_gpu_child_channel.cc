// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/ozone/platform/ohos/ohos_gpu_child_channel.h"

#include <dlfcn.h>
#include <string.h>
#include <unistd.h>

#include <atomic>
#include <map>
#include <memory>

#include "AbilityKit/native_child_process.h"
#include "IPCKit/ipc_kit.h"
#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/process/launch_ohos.h"
#include "base/process/process_handle.h"
#include "base/synchronization/condition_variable.h"
#include "base/synchronization/lock.h"
#include "base/synchronization/waitable_event.h"
#include "base/task/thread_pool.h"
#include "base/thread_annotations.h"
#include "native_window/external_window.h"
#include "vulkan/vulkan_core.h"
#include "ui/gfx/geometry/rect.h"

namespace ui {
namespace {

// The library the system loads in the child. It exports
// NativeChildProcess_OnConnect and NativeChildProcess_MainProc, which hand
// over to CreateOhosGpuChildStub and the child main in libweb_engine.so.
constexpr char kGpuChildLibrary[] = "libnweb_render.so";
constexpr char kStubDescriptor[] = "chromium.ohos.GpuChild";

// Request codes, browser to GPU process.
constexpr uint32_t kBootstrap = 1;
constexpr uint32_t kWidgetState = 2;

// What a request callback may return besides success is limited to the user
// range; anything else comes back to the sender as a generic error.
constexpr int kBadRequest = OH_IPC_USER_ERROR_CODE_MIN;

// Long enough for a cold start of a child on a loaded device; the browser's
// launcher thread is what waits.
constexpr base::TimeDelta kStartTimeout = base::Seconds(10);
constexpr base::TimeDelta kBootstrapTimeout = base::Seconds(30);

OH_IPC_MessageOption SyncOption() {
  return {.mode = OH_IPC_REQUEST_MODE_SYNC, .timeout = 0, .reserved = nullptr};
}

struct ParcelDeleter {
  void operator()(OHIPCParcel* parcel) const { OH_IPCParcel_Destroy(parcel); }
};
using ScopedParcel = std::unique_ptr<OHIPCParcel, ParcelDeleter>;

bool SameState(const OhosGpuChildWidgetState& a,
               const OhosGpuChildWidgetState& b) {
  return a.window == b.window && a.bounds == b.bounds &&
         a.density == b.density && a.anchored == b.anchored &&
         a.expected == b.expected && a.application_window_id ==
                                         b.application_window_id;
}

// --- Browser process. -----------------------------------------------------

struct BrowserSide {
  base::Lock lock;
  // The GPU child most recently started. Replaced when the GPU process is
  // restarted; a request to a dead one fails and is ignored.
  OHIPCRemoteProxy* proxy GUARDED_BY(lock) = nullptr;
  // What the GPU process was last told, per widget, so only changes go out.
  std::map<gfx::AcceleratedWidget, OhosGpuChildWidgetState> sent
      GUARDED_BY(lock);

  // The start callback has no user data, so the one start in flight lives
  // here. GPU processes are started one at a time.
  base::Lock start_lock;
  base::WaitableEvent started{base::WaitableEvent::ResetPolicy::MANUAL,
                              base::WaitableEvent::InitialState::NOT_SIGNALED};
  int start_error GUARDED_BY(start_lock) = 0;
  OHIPCRemoteProxy* start_proxy GUARDED_BY(start_lock) = nullptr;
};

BrowserSide& Browser() {
  static base::NoDestructor<BrowserSide> side;
  return *side;
}

void OnGpuChildStarted(int error, OHIPCRemoteProxy* proxy) {
  BrowserSide& side = Browser();
  {
    base::AutoLock lock(side.start_lock);
    side.start_error = error;
    side.start_proxy = proxy;
  }
  side.started.Signal();
}

// Writes one widget's state. The window itself goes only when it changed:
// every parcel read in the GPU process makes a new OHNativeWindow there, and
// a new pointer is what the surface code reads as "the surface was
// replaced" -- so resending an unchanged window would rebuild the EGL surface
// on every resize.
bool WriteWidgetState(OHIPCParcel* parcel,
                      const OhosGpuChildWidgetState& state,
                      bool present,
                      bool window_changed) {
  const bool send_window = present && window_changed && state.window;
  // Int64 rather than Uint64: the SDK this is built against has only the
  // signed pair. Both values are opaque here.
  return OH_IPCParcel_WriteInt64(parcel,
                                 static_cast<int64_t>(state.widget)) ==
             OH_IPC_SUCCESS &&
         OH_IPCParcel_WriteInt32(parcel, present) == OH_IPC_SUCCESS &&
         OH_IPCParcel_WriteInt64(
             parcel, static_cast<int64_t>(
                         reinterpret_cast<uintptr_t>(state.window))) ==
             OH_IPC_SUCCESS &&
         OH_IPCParcel_WriteInt32(parcel, state.bounds.x()) == OH_IPC_SUCCESS &&
         OH_IPCParcel_WriteInt32(parcel, state.bounds.y()) == OH_IPC_SUCCESS &&
         OH_IPCParcel_WriteInt32(parcel, state.bounds.width()) ==
             OH_IPC_SUCCESS &&
         OH_IPCParcel_WriteInt32(parcel, state.bounds.height()) ==
             OH_IPC_SUCCESS &&
         OH_IPCParcel_WriteFloat(parcel, state.density) == OH_IPC_SUCCESS &&
         OH_IPCParcel_WriteInt32(parcel, state.anchored) == OH_IPC_SUCCESS &&
         OH_IPCParcel_WriteInt32(parcel, state.expected) == OH_IPC_SUCCESS &&
         OH_IPCParcel_WriteInt32(parcel, state.application_window_id) ==
             OH_IPC_SUCCESS &&
         OH_IPCParcel_WriteInt32(parcel, send_window) == OH_IPC_SUCCESS &&
         (!send_window ||
          OH_NativeWindow_WriteToParcel(
              static_cast<OHNativeWindow*>(state.window), parcel) == 0);
}

void SendWidgetState(OHIPCRemoteProxy* proxy,
                     const OhosGpuChildWidgetState& state,
                     bool present,
                     bool window_changed) {
  ScopedParcel data(OH_IPCParcel_Create());
  ScopedParcel reply(OH_IPCParcel_Create());
  if (!data || !reply ||
      !WriteWidgetState(data.get(), state, present, window_changed)) {
    LOG(ERROR) << "OHOS GPU child: could not write widget " << state.widget;
    return;
  }
  const OH_IPC_MessageOption option = SyncOption();
  const int result = OH_IPCRemoteProxy_SendRequest(
      proxy, kWidgetState, data.get(), reply.get(), &option);
  if (result != OH_IPC_SUCCESS) {
    LOG(ERROR) << "OHOS GPU child: widget " << state.widget
               << " not delivered, error " << result;
  }
}

// Whether the GPU child works is noted on disk for the next launch, which
// keeps the GPU in the browser process once when it did not (see
// ohos_chrome_main_runner.cc) and removes the note, so the launch after
// that tries again.
//
// A child that cannot be started is only half of it. The other half is a
// child that starts and then dies -- the GPU crashing on initialisation --
// which Chromium answers by starting another, several times, and then one
// with --use-gl=disabled that survives and draws nothing. None of those
// launches fails. So the note is written whenever a child is started and
// taken back only when that child has been up for kGpuChildSettled with no
// other launch after it, and never once Chromium has fallen back to
// running without GL.
constexpr base::TimeDelta kGpuChildSettled = base::Seconds(10);

base::FilePath GpuChildFailedMarker() {
  const base::FilePath user_data_dir =
      base::CommandLine::ForCurrentProcess()->GetSwitchValuePath(
          "user-data-dir");
  return user_data_dir.empty()
             ? base::FilePath()
             : user_data_dir.Append(kOhosGpuChildFailedMarker);
}

struct LaunchHistory {
  // A start that failed outright: not tried again in this run. Chromium
  // retries a failed launch many times, and each retry here was a new
  // native child -- the system counted fifty.
  bool start_failed = false;
  // Chromium has given up on GL in the child for this run. Read from the
  // thread the settling check runs on, as is the generation.
  std::atomic<bool> fell_back{false};
  std::atomic<uint64_t> generation{0};
};

LaunchHistory& History() {
  static LaunchHistory history;
  return history;
}

void NoteGpuChildInDoubt() {
  const base::FilePath marker = GpuChildFailedMarker();
  if (!marker.empty() && !base::WriteFile(marker, "")) {
    LOG(ERROR) << "OHOS GPU child: could not note the attempt for next launch";
  }
}

void ClearGpuChildDoubtIfSettled(uint64_t generation) {
  // A stale generation only means a later launch took over.
  if (History().generation.load() != generation ||
      History().fell_back.load()) {
    return;
  }
  const base::FilePath marker = GpuChildFailedMarker();
  if (!marker.empty()) {
    base::DeleteFile(marker);
  }
  LOG(WARNING) << "OHOS GPU child: settled";
}

base::ProcessId StartGpuChild(const std::string& encoded_params,
                              const std::vector<std::pair<int, int>>& fds) {
  BrowserSide& side = Browser();
  OHIPCRemoteProxy* proxy = nullptr;
  {
    base::AutoLock start_lock(side.start_lock);
    side.started.Reset();
    side.start_error = 0;
    side.start_proxy = nullptr;
    const int result =
        OH_Ability_CreateNativeChildProcess(kGpuChildLibrary,
                                            &OnGpuChildStarted);
    if (result != NCP_NO_ERROR) {
      LOG(ERROR) << "OHOS GPU child: start refused, error " << result;
      return base::kNullProcessId;
    }
  }
  if (!side.started.TimedWait(kStartTimeout)) {
    LOG(ERROR) << "OHOS GPU child: no answer from the system";
    return base::kNullProcessId;
  }
  {
    base::AutoLock start_lock(side.start_lock);
    if (side.start_error != NCP_NO_ERROR || !side.start_proxy) {
      LOG(ERROR) << "OHOS GPU child: start failed, error "
                 << side.start_error;
      return base::kNullProcessId;
    }
    proxy = side.start_proxy;
  }

  // The command line and descriptors the other kind of child receives from
  // OH_Ability_StartNativeChildProcess, here sent after the fact. The reply
  // carries the child's pid, which the start callback does not.
  ScopedParcel data(OH_IPCParcel_Create());
  ScopedParcel reply(OH_IPCParcel_Create());
  bool written = data && reply &&
                 OH_IPCParcel_WriteString(data.get(), encoded_params.c_str()) ==
                     OH_IPC_SUCCESS &&
                 OH_IPCParcel_WriteInt32(data.get(),
                                         static_cast<int32_t>(fds.size())) ==
                     OH_IPC_SUCCESS;
  for (const auto& [source_fd, destination_fd] : fds) {
    written = written &&
              OH_IPCParcel_WriteInt32(data.get(), destination_fd) ==
                  OH_IPC_SUCCESS &&
              OH_IPCParcel_WriteFileDescriptor(data.get(), source_fd) ==
                  OH_IPC_SUCCESS;
  }
  const OH_IPC_MessageOption option = SyncOption();
  int32_t pid = 0;
  if (!written ||
      OH_IPCRemoteProxy_SendRequest(proxy, kBootstrap, data.get(), reply.get(),
                                    &option) != OH_IPC_SUCCESS ||
      OH_IPCParcel_ReadInt32(reply.get(), &pid) != OH_IPC_SUCCESS ||
      pid <= 0) {
    LOG(ERROR) << "OHOS GPU child: bootstrap not delivered";
    OH_IPCRemoteProxy_Destroy(proxy);
    return base::kNullProcessId;
  }

  {
    base::AutoLock lock(side.lock);
    if (side.proxy) {
      OH_IPCRemoteProxy_Destroy(side.proxy);
    }
    side.proxy = proxy;
    // A new GPU process knows nothing yet.
    side.sent.clear();
  }
  LOG(WARNING) << "OHOS GPU child: started pid=" << pid;
  ForwardOhosSurfacesToGpuChild();
  return pid;
}

// Registered with base as the way to start --type=gpu-process.
base::ProcessId LaunchGpuChild(const std::string& encoded_params,
                               const std::vector<std::pair<int, int>>& fds) {
  LaunchHistory& history = History();
  if (history.start_failed) {
    return base::kNullProcessId;
  }
  // The launch after the last GL mode has failed. It is still a child
  // worth starting -- without it Chromium gives up altogether -- but the
  // run counts as a failure.
  if (encoded_params.find("--use-gl=disabled") != std::string::npos) {
    history.fell_back = true;
  }
  NoteGpuChildInDoubt();
  const base::ProcessId pid = StartGpuChild(encoded_params, fds);
  if (pid == base::kNullProcessId) {
    history.start_failed = true;
    return pid;
  }
  const uint64_t generation = ++history.generation;
  base::ThreadPool::PostDelayedTask(
      FROM_HERE, {base::MayBlock()},
      base::BindOnce(&ClearGpuChildDoubtIfSettled, generation),
      kGpuChildSettled);
  return pid;
}

// --- GPU process. ---------------------------------------------------------

struct MirrorRecord {
  OhosNativeSurface surface;
  // The browser's pointer for the window this one was read from, to tell a
  // new window from the same one sent again.
  uint64_t browser_window = 0;
  bool anchored = false;
  bool expected = false;
  int32_t application_window_id = 0;
};

struct ChildSide {
  base::Lock lock;
  base::ConditionVariable changed{&lock};
  bool is_child GUARDED_BY(lock) = false;
  bool bootstrapped GUARDED_BY(lock) = false;
  std::string encoded_params GUARDED_BY(lock);
  std::vector<std::pair<int, int>> fds GUARDED_BY(lock);
  std::map<gfx::AcceleratedWidget, MirrorRecord> mirror GUARDED_BY(lock);
};

ChildSide& Child() {
  static base::NoDestructor<ChildSide> side;
  return *side;
}

int HandleBootstrap(const OHIPCParcel* data, OHIPCParcel* reply) {
  const char* params = OH_IPCParcel_ReadString(data);
  int32_t count = 0;
  if (!params || OH_IPCParcel_ReadInt32(data, &count) != OH_IPC_SUCCESS ||
      count < 0) {
    return kBadRequest;
  }
  std::vector<std::pair<int, int>> fds;
  for (int32_t index = 0; index < count; ++index) {
    int32_t destination_fd = -1;
    int32_t fd = -1;
    if (OH_IPCParcel_ReadInt32(data, &destination_fd) != OH_IPC_SUCCESS ||
        OH_IPCParcel_ReadFileDescriptor(data, &fd) != OH_IPC_SUCCESS) {
      return kBadRequest;
    }
    fds.emplace_back(destination_fd, fd);
  }
  ChildSide& side = Child();
  {
    base::AutoLock lock(side.lock);
    side.encoded_params = params;
    side.fds = std::move(fds);
    side.bootstrapped = true;
    side.changed.Broadcast();
  }
  return OH_IPCParcel_WriteInt32(reply, getpid());
}

int HandleWidgetState(const OHIPCParcel* data) {
  int64_t widget = 0;
  int32_t present = 0;
  int64_t browser_window = 0;
  int32_t x = 0, y = 0, width = 0, height = 0;
  float density = 1.0f;
  int32_t anchored = 0, expected = 0, application_window_id = 0;
  int32_t has_window = 0;
  if (OH_IPCParcel_ReadInt64(data, &widget) != OH_IPC_SUCCESS ||
      OH_IPCParcel_ReadInt32(data, &present) != OH_IPC_SUCCESS ||
      OH_IPCParcel_ReadInt64(data, &browser_window) != OH_IPC_SUCCESS ||
      OH_IPCParcel_ReadInt32(data, &x) != OH_IPC_SUCCESS ||
      OH_IPCParcel_ReadInt32(data, &y) != OH_IPC_SUCCESS ||
      OH_IPCParcel_ReadInt32(data, &width) != OH_IPC_SUCCESS ||
      OH_IPCParcel_ReadInt32(data, &height) != OH_IPC_SUCCESS ||
      OH_IPCParcel_ReadFloat(data, &density) != OH_IPC_SUCCESS ||
      OH_IPCParcel_ReadInt32(data, &anchored) != OH_IPC_SUCCESS ||
      OH_IPCParcel_ReadInt32(data, &expected) != OH_IPC_SUCCESS ||
      OH_IPCParcel_ReadInt32(data, &application_window_id) !=
          OH_IPC_SUCCESS ||
      OH_IPCParcel_ReadInt32(data, &has_window) != OH_IPC_SUCCESS) {
    return kBadRequest;
  }
  OHNativeWindow* window = nullptr;
  if (has_window && (OH_NativeWindow_ReadFromParcel(
                         const_cast<OHIPCParcel*>(data), &window) != 0 ||
                     !window)) {
    LOG(ERROR) << "OHOS GPU child: widget " << widget
               << " arrived without a readable window";
    window = nullptr;
  }

  ChildSide& side = Child();
  base::AutoLock lock(side.lock);
  const auto key = static_cast<gfx::AcceleratedWidget>(widget);
  if (!present) {
    // A window the GPU code may still hold is not destroyed here: the EGL
    // surface on it is torn down by its owner, and a window freed under it
    // is a use-after-free. One per window ever created.
    side.mirror.erase(key);
    side.changed.Broadcast();
    return OH_IPC_SUCCESS;
  }
  MirrorRecord& record = side.mirror[key];
  if (!browser_window) {
    record.surface.window = nullptr;
  } else if (window) {
    record.surface.window = window;
  }
  record.browser_window = static_cast<uint64_t>(browser_window);
  record.surface.bounds = gfx::Rect(x, y, width, height);
  record.surface.density = density;
  record.anchored = anchored;
  record.expected = expected;
  record.application_window_id = application_window_id;
  side.changed.Broadcast();
  return OH_IPC_SUCCESS;
}

int OnRemoteRequest(uint32_t code,
                    const OHIPCParcel* data,
                    OHIPCParcel* reply,
                    void*) {
  switch (code) {
    case kBootstrap:
      return HandleBootstrap(data, reply);
    case kWidgetState:
      return HandleWidgetState(data);
    default:
      return kBadRequest;
  }
}

}  // namespace

void InstallOhosGpuChildLauncher() {
  base::internal::SetOhosGpuChildLauncher(&LaunchGpuChild);
}

void ForwardOhosSurfacesToGpuChild() {
  BrowserSide& side = Browser();
  base::AutoLock lock(side.lock);
  if (!side.proxy) {
    return;
  }
  const std::vector<OhosGpuChildWidgetState> states =
      SnapshotOhosSurfacesForGpuChild();
  std::map<gfx::AcceleratedWidget, OhosGpuChildWidgetState> now;
  for (const OhosGpuChildWidgetState& state : states) {
    now[state.widget] = state;
    auto sent = side.sent.find(state.widget);
    const bool is_new = sent == side.sent.end();
    if (!is_new && SameState(sent->second, state)) {
      continue;
    }
    SendWidgetState(side.proxy, state, /*present=*/true,
                    is_new || sent->second.window != state.window);
  }
  for (const auto& [widget, state] : side.sent) {
    if (!now.contains(widget)) {
      SendWidgetState(side.proxy, state, /*present=*/false,
                      /*window_changed=*/false);
    }
  }
  side.sent = std::move(now);
}

bool IsOhosGpuChildProcess() {
  ChildSide& side = Child();
  base::AutoLock lock(side.lock);
  return side.is_child;
}

std::optional<OhosNativeSurface> GetOhosGpuChildSurface(
    gfx::AcceleratedWidget widget) {
  ChildSide& side = Child();
  base::AutoLock lock(side.lock);
  auto it = side.mirror.find(widget);
  if (it == side.mirror.end()) {
    return std::nullopt;
  }
  return it->second.surface;
}

bool IsOhosGpuChildSurfaceExpected(gfx::AcceleratedWidget widget) {
  ChildSide& side = Child();
  base::AutoLock lock(side.lock);
  auto it = side.mirror.find(widget);
  return it != side.mirror.end() && it->second.expected;
}

bool IsOhosGpuChildAnchored(gfx::AcceleratedWidget widget) {
  ChildSide& side = Child();
  base::AutoLock lock(side.lock);
  auto it = side.mirror.find(widget);
  return it != side.mirror.end() && it->second.anchored;
}

std::optional<OhosNativeSurface> WaitForOhosGpuChildSurface(
    gfx::AcceleratedWidget widget,
    base::TimeDelta timeout) {
  ChildSide& side = Child();
  base::AutoLock lock(side.lock);
  const base::TimeTicks deadline = base::TimeTicks::Now() + timeout;
  while (true) {
    auto it = side.mirror.find(widget);
    if (it != side.mirror.end() && it->second.surface.window) {
      return it->second.surface;
    }
    if (it != side.mirror.end() && !it->second.expected) {
      return std::nullopt;
    }
    const base::TimeDelta remaining = deadline - base::TimeTicks::Now();
    if (remaining <= base::TimeDelta()) {
      return std::nullopt;
    }
    side.changed.TimedWait(remaining);
  }
}

int32_t GetOhosGpuChildApplicationWindowId(gfx::AcceleratedWidget widget) {
  ChildSide& side = Child();
  base::AutoLock lock(side.lock);
  auto it = side.mirror.find(widget);
  return it == side.mirror.end() ? 0 : it->second.application_window_id;
}

void* CreateOhosGpuChildStub() {
  ChildSide& side = Child();
  {
    base::AutoLock lock(side.lock);
    side.is_child = true;
  }
  return OH_IPCRemoteStub_Create(kStubDescriptor, &OnRemoteRequest,
                                 /*destroyCallback=*/nullptr,
                                 /*userData=*/nullptr);
}

void ProbeOhosGpuChildEgl() {
  // Which process this is to the system: a GPU denied by the sandbox shows
  // up here, not as an EGL error.
  std::string security_context;
  base::ReadFileToString(base::FilePath("/proc/self/attr/current"),
                         &security_context);
  LOG(WARNING) << "OHOS GPU child probe: uid=" << getuid()
               << " context=" << security_context;

  // By dlopen, as ANGLE's FunctionsEGLDL loads them, so nothing here links
  // against the system's GL and nothing clashes with ANGLE's own libraries.
  void* egl = dlopen("libEGL.so", RTLD_NOW | RTLD_LOCAL);
  void* gles = dlopen("libGLESv3.so", RTLD_NOW | RTLD_LOCAL);
  LOG(WARNING) << "OHOS GPU child probe: libEGL=" << (egl ? "ok" : dlerror())
               << " libGLESv3=" << (gles ? "ok" : "missing");
  if (!egl || !gles) {
    return;
  }
  using GetDisplay = void* (*)(void*);
  using Initialize = unsigned (*)(void*, int*, int*);
  using GetError = int (*)();
  using ChooseConfig = unsigned (*)(void*, const int*, void**, int, int*);
  using CreatePbuffer = void* (*)(void*, void*, const int*);
  using CreateContext = void* (*)(void*, void*, void*, const int*);
  using MakeCurrent = unsigned (*)(void*, void*, void*, void*);
  using GetString = const unsigned char* (*)(unsigned);
  auto get_display = reinterpret_cast<GetDisplay>(dlsym(egl, "eglGetDisplay"));
  auto initialize = reinterpret_cast<Initialize>(dlsym(egl, "eglInitialize"));
  auto get_error = reinterpret_cast<GetError>(dlsym(egl, "eglGetError"));
  auto choose_config =
      reinterpret_cast<ChooseConfig>(dlsym(egl, "eglChooseConfig"));
  auto create_pbuffer =
      reinterpret_cast<CreatePbuffer>(dlsym(egl, "eglCreatePbufferSurface"));
  auto create_context =
      reinterpret_cast<CreateContext>(dlsym(egl, "eglCreateContext"));
  auto make_current =
      reinterpret_cast<MakeCurrent>(dlsym(egl, "eglMakeCurrent"));
  auto get_string = reinterpret_cast<GetString>(dlsym(gles, "glGetString"));
  if (!get_display || !initialize || !get_error || !choose_config ||
      !create_pbuffer || !create_context || !make_current || !get_string) {
    LOG(WARNING) << "OHOS GPU child probe: an EGL or GLES entry is missing";
    return;
  }

  // Where each entry point lives, reached the way this probe reaches it and
  // the way ANGLE did: through eglGetProcAddress. A different library for
  // the same name is a dispatch that bypasses the wrapper.
  using GetProcAddress = void* (*)(const char*);
  auto get_proc_address =
      reinterpret_cast<GetProcAddress>(dlsym(egl, "eglGetProcAddress"));
  auto where = [](void* function) {
    Dl_info info = {};
    return function && dladdr(function, &info) && info.dli_fname
               ? std::string(info.dli_fname)
               : std::string(function ? "?" : "null");
  };
  for (const char* name : {"eglMakeCurrent", "glGetString"}) {
    void* exported = dlsym(strncmp(name, "egl", 3) == 0 ? egl : gles, name);
    void* resolved = get_proc_address ? get_proc_address(name) : nullptr;
    LOG(WARNING) << "OHOS GPU child probe: " << name
                 << " exported=" << where(exported)
                 << " via_getprocaddress=" << where(resolved)
                 << " same=" << (exported == resolved);
  }

  constexpr int kNone = 0x3038;               // EGL_NONE
  constexpr int kSurfaceType = 0x3033;        // EGL_SURFACE_TYPE
  constexpr int kPbufferBit = 0x0001;         // EGL_PBUFFER_BIT
  constexpr int kRenderableType = 0x3040;     // EGL_RENDERABLE_TYPE
  constexpr int kOpenGLES3Bit = 0x0040;       // EGL_OPENGL_ES3_BIT
  constexpr int kWidth = 0x3057;              // EGL_WIDTH
  constexpr int kHeight = 0x3056;             // EGL_HEIGHT
  constexpr int kClientVersion = 0x3098;      // EGL_CONTEXT_CLIENT_VERSION
  constexpr unsigned kVendor = 0x1F00;        // GL_VENDOR
  constexpr unsigned kRenderer = 0x1F01;      // GL_RENDERER
  constexpr unsigned kVersion = 0x1F02;       // GL_VERSION

  void* display = get_display(nullptr);
  int major = 0;
  int minor = 0;
  const unsigned initialized =
      display ? initialize(display, &major, &minor) : 0;
  LOG(WARNING) << "OHOS GPU child probe: display=" << (display ? "ok" : "none")
               << " initialize=" << initialized << " version=" << major << "."
               << minor << " error=0x" << std::hex << get_error();
  if (!initialized) {
    return;
  }
  const int config_attributes[] = {kSurfaceType, kPbufferBit, kRenderableType,
                                   kOpenGLES3Bit, kNone};
  void* config = nullptr;
  int config_count = 0;
  const unsigned chosen =
      choose_config(display, config_attributes, &config, 1, &config_count);
  const int pbuffer_attributes[] = {kWidth, 1, kHeight, 1, kNone};
  void* surface =
      chosen && config_count ? create_pbuffer(display, config,
                                              pbuffer_attributes)
                             : nullptr;
  const int context_attributes[] = {kClientVersion, 3, kNone};
  void* context = chosen && config_count
                      ? create_context(display, config, nullptr,
                                       context_attributes)
                      : nullptr;
  const unsigned current =
      surface && context ? make_current(display, surface, surface, context)
                         : 0;
  LOG(WARNING) << "OHOS GPU child probe: configs=" << config_count
               << " pbuffer=" << (surface ? "ok" : "none")
               << " context=" << (context ? "ok" : "none")
               << " current=" << current << " error=0x" << std::hex
               << get_error();
  if (!current) {
    return;
  }
  auto text = [&](unsigned name) {
    const unsigned char* value = get_string(name);
    return value ? std::string(reinterpret_cast<const char*>(value))
                 : std::string("(null)");
  };
  LOG(WARNING) << "OHOS GPU child probe: vendor=" << text(kVendor)
               << " renderer=" << text(kRenderer)
               << " version=" << text(kVersion);
  // Released with the process; this ran once, before Chromium, and
  // Chromium's own context comes next.
  make_current(display, nullptr, nullptr, nullptr);
}

void ProbeOhosGpuChildVulkan() {
  void* loader = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
  if (!loader) {
    LOG(WARNING) << "OHOS GPU child Vulkan probe: no libvulkan.so: "
                 << dlerror();
    return;
  }
  auto get_instance_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
      dlsym(loader, "vkGetInstanceProcAddr"));
  if (!get_instance_proc) {
    LOG(WARNING) << "OHOS GPU child Vulkan probe: no vkGetInstanceProcAddr";
    return;
  }
  auto enumerate_instance_extensions =
      reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
          get_instance_proc(nullptr,
                            "vkEnumerateInstanceExtensionProperties"));
  auto create_instance = reinterpret_cast<PFN_vkCreateInstance>(
      get_instance_proc(nullptr, "vkCreateInstance"));
  if (!enumerate_instance_extensions || !create_instance) {
    LOG(WARNING) << "OHOS GPU child Vulkan probe: loader entry points missing";
    return;
  }

  uint32_t extension_count = 0;
  enumerate_instance_extensions(nullptr, &extension_count, nullptr);
  std::vector<VkExtensionProperties> extensions(extension_count);
  enumerate_instance_extensions(nullptr, &extension_count, extensions.data());
  bool has_surface = false;
  bool has_ohos_surface = false;
  for (const VkExtensionProperties& extension : extensions) {
    has_surface |= strcmp(extension.extensionName, "VK_KHR_surface") == 0;
    has_ohos_surface |=
        strcmp(extension.extensionName, "VK_OHOS_surface") == 0;
  }
  LOG(WARNING) << "OHOS GPU child Vulkan probe: instance extensions="
               << extension_count << " VK_KHR_surface=" << has_surface
               << " VK_OHOS_surface=" << has_ohos_surface;
  if (!has_surface || !has_ohos_surface) {
    return;
  }

  const char* const instance_extensions[] = {"VK_KHR_surface",
                                             "VK_OHOS_surface"};
  VkApplicationInfo application = {};
  application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  application.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo instance_info = {};
  instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  instance_info.pApplicationInfo = &application;
  instance_info.enabledExtensionCount = 2;
  instance_info.ppEnabledExtensionNames = instance_extensions;
  VkInstance instance = VK_NULL_HANDLE;
  const VkResult created = create_instance(&instance_info, nullptr, &instance);
  LOG(WARNING) << "OHOS GPU child Vulkan probe: vkCreateInstance=" << created;
  if (created != VK_SUCCESS) {
    return;
  }

  auto enumerate_devices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
      get_instance_proc(instance, "vkEnumeratePhysicalDevices"));
  auto get_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
      get_instance_proc(instance, "vkGetPhysicalDeviceProperties"));
  auto enumerate_device_extensions =
      reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
          get_instance_proc(instance, "vkEnumerateDeviceExtensionProperties"));
  auto destroy_instance = reinterpret_cast<PFN_vkDestroyInstance>(
      get_instance_proc(instance, "vkDestroyInstance"));
  uint32_t device_count = 0;
  if (enumerate_devices) {
    enumerate_devices(instance, &device_count, nullptr);
  }
  std::vector<VkPhysicalDevice> devices(device_count);
  if (device_count) {
    enumerate_devices(instance, &device_count, devices.data());
  }
  LOG(WARNING) << "OHOS GPU child Vulkan probe: physical devices="
               << device_count;
  for (VkPhysicalDevice device : devices) {
    VkPhysicalDeviceProperties properties = {};
    if (get_properties) {
      get_properties(device, &properties);
    }
    uint32_t count = 0;
    bool has_swapchain = false;
    if (enumerate_device_extensions) {
      enumerate_device_extensions(device, nullptr, &count, nullptr);
      std::vector<VkExtensionProperties> device_extensions(count);
      enumerate_device_extensions(device, nullptr, &count,
                                  device_extensions.data());
      for (const VkExtensionProperties& extension : device_extensions) {
        has_swapchain |=
            strcmp(extension.extensionName, "VK_KHR_swapchain") == 0;
      }
    }
    LOG(WARNING) << "OHOS GPU child Vulkan probe: device="
                 << properties.deviceName << " api="
                 << VK_API_VERSION_MAJOR(properties.apiVersion) << "."
                 << VK_API_VERSION_MINOR(properties.apiVersion) << "."
                 << VK_API_VERSION_PATCH(properties.apiVersion)
                 << " driver=0x" << std::hex << properties.driverVersion
                 << std::dec << " type=" << properties.deviceType
                 << " VK_KHR_swapchain=" << has_swapchain;
  }
  if (destroy_instance) {
    destroy_instance(instance, nullptr);
  }
}

bool WaitForOhosGpuChildBootstrap(std::string* encoded_params,
                                  std::vector<std::pair<int, int>>* fds) {
  ChildSide& side = Child();
  base::AutoLock lock(side.lock);
  const base::TimeTicks deadline = base::TimeTicks::Now() + kBootstrapTimeout;
  while (!side.bootstrapped) {
    const base::TimeDelta remaining = deadline - base::TimeTicks::Now();
    if (remaining <= base::TimeDelta()) {
      return false;
    }
    side.changed.TimedWait(remaining);
  }
  *encoded_params = side.encoded_params;
  *fds = side.fds;
  return true;
}

}  // namespace ui
