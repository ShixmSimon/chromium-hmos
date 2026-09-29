// Copyright (c) 2026
// Licensed under the Apache License, Version 2.0.

#include <dlfcn.h>
#include <hilog/log.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "AbilityKit/native_child_process.h"

namespace {

constexpr char kEngineLibrary[] = "libweb_engine.so";

void* OpenEngineLibrary() {
  Dl_info module_info = {};
  if (!dladdr(reinterpret_cast<const void*>(&OpenEngineLibrary),
              &module_info) ||
      !module_info.dli_fname) {
    return nullptr;
  }

  const char* separator = strrchr(module_info.dli_fname, '/');
  if (!separator) {
    return dlopen(kEngineLibrary, RTLD_NOW | RTLD_LOCAL);
  }

  char engine_path[PATH_MAX] = {};
  const int directory_length =
      static_cast<int>(separator - module_info.dli_fname);
  const int written =
      snprintf(engine_path, sizeof(engine_path), "%.*s/%s", directory_length,
               module_info.dli_fname, kEngineLibrary);
  if (written <= 0 || static_cast<size_t>(written) >= sizeof(engine_path)) {
    return nullptr;
  }
  return dlopen(engine_path, RTLD_NOW | RTLD_LOCAL);
}

const char* LoaderError() {
  const char* error = dlerror();
  return error ? error : "unknown dynamic loader error";
}

void* ResolveEngineEntry(const char* symbol) {
  static void* const engine = OpenEngineLibrary();
  if (!engine) {
    OH_LOG_Print(LOG_APP, LOG_ERROR, 0xc233, "ChromiumChild",
                 "Unable to load libweb_engine.so: %{public}s", LoaderError());
    return nullptr;
  }
  void* entry = dlsym(engine, symbol);
  if (!entry) {
    OH_LOG_Print(LOG_APP, LOG_ERROR, 0xc233, "ChromiumChild",
                 "Missing Chromium child entry %{public}s: %{public}s", symbol,
                 LoaderError());
  }
  return entry;
}

}  // namespace

extern "C" __attribute__((visibility("default"))) void NWebRenderMain(
    const char* args) {
  using Entry = void (*)(const char*);
  Entry entry =
      reinterpret_cast<Entry>(ResolveEngineEntry("ChromiumNWebRenderMain"));
  if (entry) {
    entry(args);
  }
}

// The GPU process's entry points, the pair OH_Ability_CreateNativeChildProcess
// looks up by these exact names. The first returns an OHIPCRemoteStub*, the
// same as a pointer at this boundary.
extern "C" __attribute__((visibility("default"))) void*
NativeChildProcess_OnConnect() {
  using Entry = void* (*)();
  Entry entry = reinterpret_cast<Entry>(
      ResolveEngineEntry("ChromiumHarmonyOSGpuChildOnConnect"));
  return entry ? entry() : nullptr;
}

extern "C" __attribute__((visibility("default"))) void
NativeChildProcess_MainProc() {
  using Entry = void (*)();
  Entry entry = reinterpret_cast<Entry>(
      ResolveEngineEntry("ChromiumHarmonyOSGpuChildMainProc"));
  if (entry) {
    entry();
  }
}

extern "C" __attribute__((visibility("default"))) void ChromiumNativeChildMain(
    NativeChildProcess_Args args) {
  using Entry = void (*)(NativeChildProcess_Args);
  Entry entry = reinterpret_cast<Entry>(
      ResolveEngineEntry("ChromiumHarmonyOSNativeChildMain"));
  if (entry) {
    entry(args);
  }
}
