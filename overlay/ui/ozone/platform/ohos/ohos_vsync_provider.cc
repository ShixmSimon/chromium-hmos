// Copyright (c) 2026
// Licensed under the Apache License, Version 2.0.

#include "ui/ozone/platform/ohos/ohos_vsync_provider.h"

#include <native_vsync/native_vsync.h>

#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"

namespace ui {
namespace {

constexpr char kVSyncName[] = "chromium-display";

// A display refresh this engine would not know how to schedule for.
constexpr base::TimeDelta kMinInterval = base::Hertz(480);
constexpr base::TimeDelta kMaxInterval = base::Hertz(24);
constexpr base::TimeDelta kFallbackInterval = base::Hertz(60);

// Keeps asking for VSync while something is still drawing, so the timebase
// stays fresh without waking the device when nothing is on screen.
constexpr base::TimeDelta kKeepRequestingFor = base::Milliseconds(500);

// A variable refresh rate panel changes its period as content changes.
constexpr base::TimeDelta kPeriodRecheckEvery = base::Seconds(1);

// HarmonyOS runs an app at 60 Hz unless it asks for more, which leaves a
// 120 Hz panel showing every frame twice.
constexpr int32_t kMinFrameRate = 60;
constexpr int32_t kMaxFrameRate = 120;

}  // namespace

OhosVSyncProvider::OhosVSyncProvider(int32_t window_id)
    : interval_(kFallbackInterval) {
  // A connection bound to the window is what lets the system apply the
  // requested frame rate to it; an unassociated one gets whatever rate the
  // system picked for the app.
  native_vsync_ =
      window_id > 0
          ? OH_NativeVSync_Create_ForAssociatedWindow(
                static_cast<uint64_t>(window_id), kVSyncName,
                sizeof(kVSyncName) - 1)
          : OH_NativeVSync_Create(kVSyncName, sizeof(kVSyncName) - 1);
  if (!native_vsync_) {
    LOG(ERROR) << "OHOS NativeVSync unavailable; display timing falls back to "
               << kFallbackInterval.InMillisecondsF() << " ms";
    return;
  }
  OH_NativeVSync_ExpectedRateRange range = {kMinFrameRate, kMaxFrameRate,
                                            kMaxFrameRate};
  if (OH_NativeVSync_SetExpectedFrameRateRange(native_vsync_, &range) != 0) {
    LOG(WARNING) << "OHOS NativeVSync refused a " << kMaxFrameRate
                 << " Hz frame rate range";
  }

  // Decoupled VSync lets the system drive animation frames early, which it
  // documents as smoother for self-drawn content.
  if (OH_NativeVSync_DVSyncSwitch(native_vsync_, true) != 0) {
    LOG(WARNING) << "OHOS NativeVSync refused DVSync";
  }

  const base::TimeDelta hardware_interval = ReadHardwareInterval();
  base::AutoLock lock(lock_);
  last_period_read_ = base::TimeTicks::Now();
  if (!hardware_interval.is_zero()) {
    interval_ = hardware_interval;
  }
}

OhosVSyncProvider::~OhosVSyncProvider() {
  if (native_vsync_) {
    OH_NativeVSync_Destroy(native_vsync_);
  }
}

void OhosVSyncProvider::GetVSyncParameters(UpdateVSyncCallback callback) {
  base::TimeTicks timebase;
  base::TimeDelta interval;
  if (GetVSyncParametersIfAvailable(&timebase, &interval)) {
    std::move(callback).Run(timebase, interval);
  }
  // Without parameters yet the caller keeps its previous values and asks
  // again; the first VSync callback fills them in.
}

bool OhosVSyncProvider::GetVSyncParametersIfAvailable(
    base::TimeTicks* timebase,
    base::TimeDelta* interval) {
  RequestFrameIfIdle();

  base::AutoLock lock(lock_);
  last_query_ = base::TimeTicks::Now();
  if (!has_parameters_) {
    return false;
  }
  *timebase = timebase_;
  *interval = interval_;
  return true;
}

bool OhosVSyncProvider::SupportGetVSyncParametersIfAvailable() const {
  return true;
}

bool OhosVSyncProvider::IsHWClock() const {
  return native_vsync_ != nullptr;
}

// static
void OhosVSyncProvider::OnVSync(long long timestamp_ns, void* data) {
  // The timestamp shares CLOCK_MONOTONIC with base::TimeTicks.
  static_cast<OhosVSyncProvider*>(data)->OnVSyncOnAnyThread(
      base::TimeTicks() + base::Nanoseconds(timestamp_ns));
}

void OhosVSyncProvider::OnVSyncOnAnyThread(base::TimeTicks timebase) {
  bool keep_requesting = false;
  bool recheck_period = false;
  const base::TimeTicks now = base::TimeTicks::Now();
  {
    base::AutoLock lock(lock_);
    timebase_ = timebase;
    has_parameters_ = true;
    frame_requested_ = false;
    keep_requesting = now - last_query_ < kKeepRequestingFor;
    recheck_period = now - last_period_read_ >= kPeriodRecheckEvery;
    if (recheck_period) {
      last_period_read_ = now;
    }
  }
  if (recheck_period) {
    const base::TimeDelta hardware_interval = ReadHardwareInterval();
    if (!hardware_interval.is_zero()) {
      base::AutoLock lock(lock_);
      interval_ = hardware_interval;
    }
  }
  if (keep_requesting) {
    RequestFrameIfIdle();
  }
}

void OhosVSyncProvider::RequestFrameIfIdle() {
  if (!native_vsync_) {
    return;
  }
  {
    base::AutoLock lock(lock_);
    if (frame_requested_) {
      return;
    }
    frame_requested_ = true;
  }
  if (OH_NativeVSync_RequestFrame(native_vsync_, &OhosVSyncProvider::OnVSync,
                                  this) != 0) {
    base::AutoLock lock(lock_);
    frame_requested_ = false;
  }
}

base::TimeDelta OhosVSyncProvider::ReadHardwareInterval() {
  long long period_ns = 0;
  if (OH_NativeVSync_GetPeriod(native_vsync_, &period_ns) != 0 ||
      period_ns <= 0) {
    return base::TimeDelta();
  }
  const base::TimeDelta period = base::Nanoseconds(period_ns);
  {
    base::AutoLock lock(lock_);
    if (period != interval_) {
      VLOG(1) << "OHOS display period now " << period.InMillisecondsF()
              << " ms";
    }
  }
  if (period < kMinInterval || period > kMaxInterval) {
    LOG(WARNING) << "OHOS NativeVSync reported an implausible period of "
                 << period.InMillisecondsF() << " ms";
    return base::TimeDelta();
  }
  return period;
}

}  // namespace ui
