// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BASE_PROCESS_FORK_SERVER_OHOS_H_
#define BASE_PROCESS_FORK_SERVER_OHOS_H_

#include <sys/types.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/base_export.h"

namespace base::internal {

inline constexpr char kOhosPhoneFork[] = "ohos-phone-fork";

// Experimental, same-UID launcher. Start ONCE before ChromeMain/ICU/worker
// threads are initialized. The engine is already mapped; this is not an exec
// or a clean appspawn environment. Failure never falls back to a late fork.
BASE_EXPORT bool StartOhosForkServer(const std::string& child_library);
BASE_EXPORT void StopOhosForkServer();
BASE_EXPORT bool IsOhosForkServerStarted();
BASE_EXPORT pid_t LaunchOhosForkChild(
    const std::string& encoded_params,
    const std::vector<std::pair<int, int>>& fds);

struct OhosForkChildStatus {
  bool running = true;
  // POSIX waitpid status. -1 means the server was lost, not a clean exit.
  int wait_status = -1;
};
// nullopt: not one of our children; callers must use the platform path.
BASE_EXPORT std::optional<OhosForkChildStatus> GetOhosForkChildStatus(pid_t pid);
BASE_EXPORT std::optional<bool> SignalOhosForkChild(pid_t pid, int signal);
// milliseconds < 0 means no deadline; a lost server returns false.
BASE_EXPORT std::optional<bool> WaitForOhosForkChild(pid_t pid,
                                                 int64_t milliseconds,
                                                 int* exit_code);

}  // namespace base::internal

#endif  // BASE_PROCESS_FORK_SERVER_OHOS_H_
