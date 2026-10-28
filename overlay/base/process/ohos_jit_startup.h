// Copyright (c) 2026
// Licensed under the Apache License, Version 2.0.

#ifndef BASE_PROCESS_OHOS_JIT_STARTUP_H_
#define BASE_PROCESS_OHOS_JIT_STARTUP_H_

#include <cerrno>
#include <string_view>

namespace base::internal {

struct JitMemoryProbe {
  bool available = false;
  int map_error = 0;
  int protect_error = 0;
  int unmap_error = 0;
  int discard_error = 0;
  const char* failure_stage = nullptr;
};

struct JitCompatibilityRequest {
  int result = -1;
  int error = 0;
};

struct JitStartupResult {
  bool disabled = false;
  bool request_attempted = false;
  JitMemoryProbe before;
  JitMemoryProbe after;
  JitCompatibilityRequest request;
};

// Recognize explicit V8 boolean flags before making a compatibility request.
// Their order matters, just as it does to V8. Unrelated flags are preserved.
inline bool JitlessRequested(std::string_view flags) {
  bool disabled = false;
  while (!flags.empty()) {
    const size_t start = flags.find_first_not_of(" ,\t\r\n");
    if (start == std::string_view::npos) {
      break;
    }
    flags.remove_prefix(start);
    const size_t end = flags.find_first_of(" ,\t\r\n");
    const std::string_view flag = flags.substr(0, end);
    if (flag == "--jitless" || flag == "--jitless=true" ||
        flag == "--jitless=1") {
      disabled = true;
    } else if (flag == "--nojitless" || flag == "--no-jitless" ||
               flag == "--jitless=false" || flag == "--jitless=0") {
      disabled = false;
    }
    if (end == std::string_view::npos) {
      break;
    }
    flags.remove_prefix(end);
  }
  return disabled;
}

// Called before V8 and the phone fork server. Only executable-protection
// failures may trigger the firmware-specific request; resource/cleanup errors
// retain the interpreter fallback. A successful request alone proves nothing.
template <typename Probe, typename Request>
JitStartupResult PrepareJitStartup(bool disabled,
                                  bool allow_compatibility,
                                  Probe probe,
                                  Request request) {
  JitStartupResult result;
  result.disabled = disabled;
  if (disabled) {
    return result;
  }
  result.before = result.after = probe();
  const int error = result.before.protect_error;
  if (result.before.available || !allow_compatibility ||
      result.before.map_error || result.before.unmap_error ||
      result.before.discard_error ||
      (error != EINVAL && error != EACCES && error != EPERM)) {
    return result;
  }
  result.request_attempted = true;
  result.request = request();
  // Re-probe even after request failure: shared system state can also change.
  result.after = probe();
  return result;
}

}  // namespace base::internal

#endif  // BASE_PROCESS_OHOS_JIT_STARTUP_H_
