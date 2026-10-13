// Copyright (c) 2026
// Licensed under the Apache License, Version 2.0.

#ifndef UI_OZONE_PLATFORM_OHOS_OHOS_VSYNC_PROVIDER_H_
#define UI_OZONE_PLATFORM_OHOS_OHOS_VSYNC_PROVIDER_H_

#include <cstdint>

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/synchronization/lock.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "ui/gfx/vsync_provider.h"

struct OH_NativeVSync;

namespace ui {

// Reports the display's real refresh timing, from HarmonyOS NativeVSync, to
// the display compositor. Without it the compositor runs on a fixed 60 Hz
// guess whose phase is unrelated to the panel, which drops frames while
// scrolling on a 120 Hz display.
class OhosVSyncProvider final : public gfx::VSyncProvider {
 public:
  // |window_id| ties the connection to the window whose frame rate is being
  // requested; 0 falls back to an unassociated connection.
  explicit OhosVSyncProvider(int32_t window_id, bool polling = false);
  // Blocking phone channel workers poll idle policy on their owning sequence.
  void PollIdle();
  OhosVSyncProvider(const OhosVSyncProvider&) = delete;
  OhosVSyncProvider& operator=(const OhosVSyncProvider&) = delete;
  ~OhosVSyncProvider() override;

  // gfx::VSyncProvider:
  void GetVSyncParameters(UpdateVSyncCallback callback) override;
  bool GetVSyncParametersIfAvailable(base::TimeTicks* timebase,
                                     base::TimeDelta* interval) override;
  bool SupportGetVSyncParametersIfAvailable() const override;
  bool IsHWClock() const override;
  void SetPreferredFrameInterval(base::TimeDelta interval) override;

 private:
  class CallbackTarget;

  // Runs on the NativeVSync thread. `data` is a CallbackTarget with a
  // reference taken for this one request.
  static void OnVSync(long long timestamp_ns, void* data);

  void RequestFrameIfIdle();
  void OnVSyncOnAnyThread(base::TimeTicks timebase);
  base::TimeDelta ReadHardwareInterval();
  void ApplyFrameRate(int32_t expected);
  void ReleaseFrameRateIfIdle();

  OH_NativeVSync* native_vsync_ = nullptr;

  // What NativeVSync calls back into. Each pending request holds a
  // reference, so a callback queued or running while this provider is
  // destroyed finds the target detached rather than freed memory.
  scoped_refptr<CallbackTarget> callback_target_;

  mutable base::Lock lock_;
  base::TimeTicks timebase_ GUARDED_BY(lock_);
  base::TimeDelta interval_ GUARDED_BY(lock_);
  base::TimeTicks last_query_ GUARDED_BY(lock_);
  base::TimeTicks last_period_read_ GUARDED_BY(lock_);
  base::TimeTicks recheck_period_until_ GUARDED_BY(lock_);
  bool has_parameters_ GUARDED_BY(lock_) = false;
  bool frame_requested_ GUARDED_BY(lock_) = false;

  // Only the owning GPU sequence changes native frame-rate requests. The
  // NativeVSync callback posts idle work back here, never mutating policy.
  int32_t preferred_frame_rate_ = 120;
  int32_t applied_frame_rate_ = -1;
  bool rate_control_failed_ = false;
  // When a request last failed for a reason other than a rejected range --
  // an IPC that did not get through -- so it is retried, but not every
  // frame.
  base::TimeTicks last_transient_failure_;
  int transient_failures_ = 0;
  base::RepeatingClosure idle_callback_;
  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<OhosVSyncProvider> weak_factory_{this};
};

}  // namespace ui

#endif  // UI_OZONE_PLATFORM_OHOS_OHOS_VSYNC_PROVIDER_H_
