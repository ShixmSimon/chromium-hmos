// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/device/wake_lock/power_save_blocker/power_save_blocker.h"

#include <cstddef>
#include <cstdint>
#include <map>

#include <window_manager/oh_window.h>

#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/synchronization/lock.h"
#include "ui/ozone/platform/ohos/ohos_native_window_registry.h"

namespace device {

namespace {

struct ScreenWakeLockState {
  base::Lock lock;
  std::map<int32_t, std::size_t> counts GUARDED_BY(lock);
};

ScreenWakeLockState& GetScreenWakeLockState() {
  static base::NoDestructor<ScreenWakeLockState> state;
  return *state;
}

bool PreventsDisplaySleep(mojom::WakeLockType type) {
  switch (type) {
    case mojom::WakeLockType::kPreventAppSuspension:
      return false;
    case mojom::WakeLockType::kPreventDisplaySleep:
    case mojom::WakeLockType::kPreventDisplaySleepAllowDimming:
      return true;
  }
}

int32_t AcquireScreenWakeLock() {
  const int32_t window_id = ui::GetOhosApplicationWindowId();
  if (window_id <= 0) {
    LOG(ERROR) << "Cannot acquire OHOS screen wake lock without a valid "
                  "application window ID";
    return 0;
  }

  auto& state = GetScreenWakeLockState();
  base::AutoLock lock(state.lock);
  auto [it, inserted] = state.counts.try_emplace(window_id, 0u);
  if (!inserted) {
    ++it->second;
    return window_id;
  }

  const int32_t result =
      OH_WindowManager_SetWindowKeepScreenOn(window_id, true);
  if (result != 0) {
    state.counts.erase(it);
    LOG(ERROR) << "Failed to acquire OHOS screen wake lock for window "
               << window_id << ": " << result;
    return 0;
  }

  it->second = 1u;
  return window_id;
}

void ReleaseScreenWakeLock(int32_t window_id) {
  auto& state = GetScreenWakeLockState();
  base::AutoLock lock(state.lock);
  auto it = state.counts.find(window_id);
  if (it == state.counts.end()) {
    LOG(ERROR) << "Cannot release unknown OHOS screen wake lock for window "
               << window_id;
    return;
  }

  if (it->second > 1u) {
    --it->second;
    return;
  }

  state.counts.erase(it);
  const int32_t result =
      OH_WindowManager_SetWindowKeepScreenOn(window_id, false);
  if (result != 0) {
    LOG(ERROR) << "Failed to release OHOS screen wake lock for window "
               << window_id << ": " << result;
  }
}

}  // namespace

class PowerSaveBlocker::Delegate {
 public:
  explicit Delegate(mojom::WakeLockType type) {
    if (PreventsDisplaySleep(type)) {
      window_id_ = AcquireScreenWakeLock();
    }
  }

  Delegate(const Delegate&) = delete;
  Delegate& operator=(const Delegate&) = delete;

  ~Delegate() {
    if (window_id_ > 0) {
      ReleaseScreenWakeLock(window_id_);
    }
  }

 private:
  int32_t window_id_ = 0;
};

PowerSaveBlocker::PowerSaveBlocker(
    mojom::WakeLockType type,
    mojom::WakeLockReason reason,
    const std::string& description,
    scoped_refptr<base::SequencedTaskRunner> ui_task_runner)
    : delegate_(ui_task_runner, type) {}

PowerSaveBlocker::~PowerSaveBlocker() = default;

}  // namespace device
