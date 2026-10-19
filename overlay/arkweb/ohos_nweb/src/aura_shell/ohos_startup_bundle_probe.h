// Copyright (c) 2026
// Licensed under the Apache License, Version 2.0.

#ifndef OHOS_NWEB_SRC_AURA_SHELL_OHOS_STARTUP_BUNDLE_PROBE_H_
#define OHOS_NWEB_SRC_AURA_SHELL_OHOS_STARTUP_BUNDLE_PROBE_H_

#include <bundle/native_interface_bundle.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include "base/logging.h"

namespace ohos_nweb {

inline constexpr char kStartupBundleProbeSwitch[] = "ohos-startup-bundle-probe";

namespace startup_bundle_probe {
inline int64_t MonoUs() {
  timespec ts{};
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    return -1;
  return static_cast<int64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

struct Completion {
  std::mutex mutex;
  std::condition_variable changed;
  bool done = false;
};
}  // namespace startup_bundle_probe

// Explicit diagnostic only. Run the browser side AFTER creating the early fork
// server, so loading BundleMgr here cannot prime the server's IPC snapshot.
// This public API queries the caller's own BundleInfo. It does NOT expose the
// CheckSystemAbility/Binder return code. errno is diagnostic context only.
// Neither the caller nor startup readiness waits for this potentially blocking
// API. A deadline reports an outstanding call; it cannot safely cancel it.
inline void StartStartupBundleProbe(const char* role) {
  static std::atomic<bool> started{false};
  if (started.exchange(true, std::memory_order_relaxed))
    return;
  const int saved_errno = errno;
  auto completion = std::make_shared<startup_bundle_probe::Completion>();
  std::thread([completion, role] {
    const int64_t begin = startup_bundle_probe::MonoUs();
    LOG(WARNING) << "OHOS BundleIpcProbe phase=begin role=" << role
                 << " pid=" << getpid() << " tid=" << syscall(SYS_gettid)
                 << " mono_us=" << begin << " api=IsDebugMode";
    errno = 0;
    // Keep this handle for process lifetime: the system library may retain IPC
    // registrations. Missing API is reported, not replaced by a private ABI.
    void* library = dlopen("libbundle_ndk.z.so", RTLD_NOW | RTLD_LOCAL);
    auto query = library
                     ? reinterpret_cast<decltype(&OH_NativeBundle_IsDebugMode)>(
                           dlsym(library, "OH_NativeBundle_IsDebugMode"))
                     : nullptr;
    const int loader_errno = errno;
    bool debug_mode = false;
    errno = 0;
    const bool ok = query && query(&debug_mode);
    const int api_errno = errno;
    const int64_t end = startup_bundle_probe::MonoUs();
    LOG(WARNING) << "OHOS BundleIpcProbe phase=end role=" << role
                 << " pid=" << getpid() << " tid=" << syscall(SYS_gettid)
                 << " mono_us=" << end << " elapsed_us="
                 << (begin >= 0 && end >= 0 ? end - begin : -1)
                 << " library_available=" << (library != nullptr)
                 << " api_available=" << (query != nullptr) << " api_ok=" << ok
                 << " api_errno=" << api_errno
                 << " loader_errno=" << loader_errno
                 << " binder_status=unavailable";
    {
      std::lock_guard<std::mutex> lock(completion->mutex);
      completion->done = true;
    }
    completion->changed.notify_one();
  }).detach();
  std::thread([completion, role] {
    std::unique_lock<std::mutex> lock(completion->mutex);
    if (!completion->changed.wait_for(lock, std::chrono::seconds(1),
                                      [&] { return completion->done; })) {
      LOG(WARNING) << "OHOS BundleIpcProbe phase=deadline role=" << role
                   << " pid=" << getpid()
                   << " mono_us=" << startup_bundle_probe::MonoUs()
                   << " budget_ms=1000 call_pending=1 cancelled=0";
    }
  }).detach();
  errno = saved_errno;
}

}  // namespace ohos_nweb

#endif  // OHOS_NWEB_SRC_AURA_SHELL_OHOS_STARTUP_BUNDLE_PROBE_H_
