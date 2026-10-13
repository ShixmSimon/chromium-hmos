// Copyright (c) 2026
// Licensed under the Apache License, Version 2.0.

#include "ui/ozone/platform/ohos/ohos_vsync_provider.h"

#include <native_vsync/native_vsync.h>

#include <algorithm>
#include <cmath>
#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/ref_counted.h"
#include "base/synchronization/lock.h"
#include "base/task/bind_post_task.h"
#include "base/trace_event/trace_event.h"

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

// How far after VSync Chromium starts its frame.
//
// ArkUI hands touch moves to the XComponent at VSync, resampled once per
// frame, and they reach the renderer compositor 1.9 ms later (p50; 3.5 ms
// p90). A frame that begins exactly at VSync has always just missed that
// frame's input, and because the previous frame's event is still queued the
// scheduler never waits for the new one: every scroll update sat in the queue
// for ~6 ms and the page trailed the finger by a frame.
//
// Starting 2 ms late lets most events arrive first; the ones after that are
// caught by cc's wait-for-late-scroll deadline, which only engages once the
// queue is empty at the start of a frame. The frame still finishes well before
// the render service's next VSync.
constexpr base::TimeDelta kBeginFramePhaseOffset = base::Milliseconds(2);

// HarmonyOS runs an app at 60 Hz unless it asks for more, which leaves a
// 120 Hz panel showing every frame twice.
constexpr int32_t kMinFrameRate = 60;
constexpr int32_t kMaxFrameRate = 120;

// NATIVE_ERROR_INVALID_ARGUMENTS from native_window/graphic_error_code.h:
// the range itself was rejected, and asking again will not change that.
// Any other failure -- an IPC that did not get through -- may pass next
// time.
constexpr int kNativeErrorInvalidArguments = 40001000;

// How long to wait before retrying a request that failed for a reason other
// than its range. The rate is asked for on every frame, and a failing IPC
// should not be repeated -- or logged -- that often.
constexpr base::TimeDelta kTransientFailureRetryAfter = base::Seconds(1);

}  // namespace

class OhosVSyncProvider::CallbackTarget
    : public base::RefCountedThreadSafe<CallbackTarget> {
 public:
  explicit CallbackTarget(OhosVSyncProvider* provider) : provider_(provider) {}
  CallbackTarget(const CallbackTarget&) = delete;
  CallbackTarget& operator=(const CallbackTarget&) = delete;

  // Holds the lock for the whole callback, so Detach() cannot return while
  // one is still using the provider.
  void Run(base::TimeTicks timebase) {
    base::AutoLock lock(lock_);
    if (provider_) {
      provider_->OnVSyncOnAnyThread(timebase);
    }
  }

  void Detach() {
    base::AutoLock lock(lock_);
    provider_ = nullptr;
  }

 private:
  friend class base::RefCountedThreadSafe<CallbackTarget>;
  ~CallbackTarget() = default;

  base::Lock lock_;
  raw_ptr<OhosVSyncProvider> provider_ GUARDED_BY(lock_);
};

OhosVSyncProvider::OhosVSyncProvider(int32_t window_id, bool polling)
    : callback_target_(base::MakeRefCounted<CallbackTarget>(this)),
      interval_(kFallbackInterval) {
  if (!polling) {
    idle_callback_ = base::BindPostTaskToCurrentDefault(base::BindRepeating(
        &OhosVSyncProvider::ReleaseFrameRateIfIdle, weak_factory_.GetWeakPtr()));
  }
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
  ApplyFrameRate(kMaxFrameRate);

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
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // First, so a callback already running finishes before anything here goes
  // away, and any that come later do nothing. A request that never fires
  // after OH_NativeVSync_Destroy keeps the small target alive; that is the
  // price of not knowing whether the system drops or delivers it.
  callback_target_->Detach();
  weak_factory_.InvalidateWeakPtrs();
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
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  {
    base::AutoLock lock(lock_);
    last_query_ = base::TimeTicks::Now();
  }
  // The first draw after idle restores the most recent content request.
  ApplyFrameRate(preferred_frame_rate_);
  RequestFrameIfIdle();
  base::AutoLock lock(lock_);
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

void OhosVSyncProvider::SetPreferredFrameInterval(base::TimeDelta interval) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // Viz supplies an interval chosen for content cadence within 60..120 Hz.
  // Round fractional video cadences (e.g. 59.94) for the native integer API.
  preferred_frame_rate_ =
      interval.is_positive()
          ? static_cast<int32_t>(std::lround(std::clamp(
                1.0 / interval.InSecondsF(), static_cast<double>(kMinFrameRate),
                static_cast<double>(kMaxFrameRate))))
          : kMaxFrameRate;
  {
    base::AutoLock lock(lock_);
    last_query_ = base::TimeTicks::Now();
  }
  ApplyFrameRate(preferred_frame_rate_);
}

void OhosVSyncProvider::ApplyFrameRate(int32_t expected) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!native_vsync_ || rate_control_failed_ || expected == applied_frame_rate_) {
    return;
  }
  const base::TimeTicks now = base::TimeTicks::Now();
  if (!last_transient_failure_.is_null() &&
      now - last_transient_failure_ < kTransientFailureRetryAfter) {
    return;
  }
  OH_NativeVSync_ExpectedRateRange range = {
      expected ? kMinFrameRate : 0, expected ? kMaxFrameRate : 0, expected};
  const int result =
      OH_NativeVSync_SetExpectedFrameRateRange(native_vsync_, &range);
  if (result != 0 && result != kNativeErrorInvalidArguments) {
    // Not a verdict on the request: leave it unapplied and try again a
    // little later, when the next frame asks.
    last_transient_failure_ = now;
    if (++transient_failures_ <= 3 || transient_failures_ % 100 == 0) {
      LOG(WARNING) << "OHOS VSync: rate request failed expected=" << expected
                   << " result=" << result << "; retrying ("
                   << transient_failures_ << ")";
    }
    return;
  }
  last_transient_failure_ = base::TimeTicks();
  if (result != 0) {
    // The range was rejected: this system does not take it. Stop asking
    // every frame and keep the previous high-rate policy as the
    // compatibility fallback for this provider.
    rate_control_failed_ = true;
    OH_NativeVSync_ExpectedRateRange fallback = {
        kMinFrameRate, kMaxFrameRate, kMaxFrameRate};
    const int fallback_result =
        OH_NativeVSync_SetExpectedFrameRateRange(native_vsync_, &fallback);
    LOG(WARNING) << "OHOS VSync: rate request failed expected=" << expected
                 << " result=" << result
                 << "; legacy 120 Hz request result=" << fallback_result;
    return;
  }
  applied_frame_rate_ = expected;
  {
    base::AutoLock lock(lock_);
    // A requested rate is not necessarily granted on the very next callback.
    // Poll actual period during transition, then return to the normal cadence.
    recheck_period_until_ = base::TimeTicks::Now() + base::Seconds(1);
    last_period_read_ = base::TimeTicks();
  }
  TRACE_EVENT_INSTANT("viz", "OhosVSyncRateRequest", "expected_hz", expected);
  LOG(WARNING) << "OHOS VSync: requested rate=" << expected
               << " range=" << range.min << ".." << range.max
               << (expected ? " (content policy)" : " (idle, request released)");
}

void OhosVSyncProvider::PollIdle() {
  ReleaseFrameRateIfIdle();
}

void OhosVSyncProvider::ReleaseFrameRateIfIdle() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  {
    base::AutoLock lock(lock_);
    if (base::TimeTicks::Now() - last_query_ < kKeepRequestingFor) {
      return;  // A queued idle notification must not cancel a new draw's boost.
    }
  }
  ApplyFrameRate(0);
}

// static
void OhosVSyncProvider::OnVSync(long long timestamp_ns, void* data) {
  auto* const target = static_cast<CallbackTarget*>(data);
  // The timestamp shares CLOCK_MONOTONIC with base::TimeTicks.
  target->Run(base::TimeTicks() + base::Nanoseconds(timestamp_ns));
  // Drops the reference RequestFrameIfIdle() took for this request.
  target->Release();
}

void OhosVSyncProvider::OnVSyncOnAnyThread(base::TimeTicks timebase) {
  bool keep_requesting = false;
  bool recheck_period = false;
  const base::TimeTicks now = base::TimeTicks::Now();
  {
    base::AutoLock lock(lock_);
    timebase_ = timebase + kBeginFramePhaseOffset;
    has_parameters_ = true;
    frame_requested_ = false;
    keep_requesting = now - last_query_ < kKeepRequestingFor;
    recheck_period = now < recheck_period_until_ ||
                     now - last_period_read_ >= kPeriodRecheckEvery;
    if (recheck_period) {
      last_period_read_ = now;
    }
  }
  if (recheck_period) {
    const base::TimeDelta hardware_interval = ReadHardwareInterval();
    if (!hardware_interval.is_zero()) {
      bool changed;
      {
        base::AutoLock lock(lock_);
        changed = (interval_ - hardware_interval).magnitude() >
                  base::Milliseconds(0.5);
        interval_ = hardware_interval;
      }
      if (changed) {
        TRACE_EVENT_INSTANT("viz", "OhosVSyncActualInterval", "interval_us",
                            hardware_interval.InMicroseconds());
        LOG(WARNING) << "OHOS VSync: actual interval="
                     << hardware_interval.InMillisecondsF() << " ms";
      }
    }
  }
  if (keep_requesting) {
    RequestFrameIfIdle();
  } else if (idle_callback_) {
    idle_callback_.Run();
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
  // One reference per pending request, handed to OnVSync().
  CallbackTarget* const target = callback_target_.get();
  target->AddRef();
  if (OH_NativeVSync_RequestFrame(native_vsync_, &OhosVSyncProvider::OnVSync,
                                  target) != 0) {
    target->Release();
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
  if (period < kMinInterval || period > kMaxInterval) {
    LOG(WARNING) << "OHOS NativeVSync reported an implausible period of "
                 << period.InMillisecondsF() << " ms";
    return base::TimeDelta();
  }
  return period;
}

}  // namespace ui
