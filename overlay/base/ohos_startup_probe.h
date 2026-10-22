// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BASE_OHOS_STARTUP_PROBE_H_
#define BASE_OHOS_STARTUP_PROBE_H_

#include "build/build_config.h"

#if BUILDFLAG(IS_OHOS)
#include <errno.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <atomic>

#include "base/logging.h"

namespace base {

// Startup-only diagnostics. Payload timestamps survive delayed GPU log relay.
// No EGL error queries: observing startup must not consume driver error state.
// Preserve errno across both clocks and logging. Thread CPU is not GPU time.
class OhosStartupProbe {
 public:
  OhosStartupProbe(const char* stage, std::atomic<unsigned>& count)
      : stage_(stage), attempt_(count.fetch_add(1, std::memory_order_relaxed)) {
    if (attempt_ >= 8)
      return;
    const int saved_errno = errno;
    start_us_ = ReadClock(CLOCK_MONOTONIC);
    cpu_us_ = ReadClock(CLOCK_THREAD_CPUTIME_ID);
    Write("begin", stage_, attempt_, start_us_, -1, -1);
    errno = saved_errno;
  }

  OhosStartupProbe(const OhosStartupProbe&) = delete;
  OhosStartupProbe& operator=(const OhosStartupProbe&) = delete;

  ~OhosStartupProbe() {
    if (attempt_ >= 8)
      return;
    const int saved_errno = errno;
    const int64_t now = ReadClock(CLOCK_MONOTONIC);
    const int64_t cpu = ReadClock(CLOCK_THREAD_CPUTIME_ID);
    Write("end", stage_, attempt_, now,
          now >= 0 && start_us_ >= 0 ? now - start_us_ : -1,
          cpu >= 0 && cpu_us_ >= 0 ? cpu - cpu_us_ : -1);
    errno = saved_errno;
  }

  static void Mark(const char* stage,
                   int64_t detail,
                   std::atomic<unsigned>& count) {
    if (count.fetch_add(1, std::memory_order_relaxed) >= 64)
      return;
    const int saved_errno = errno;
    Write("mark", stage, detail, ReadClock(CLOCK_MONOTONIC), -1, -1);
    errno = saved_errno;
  }

 private:
  static int64_t ReadClock(clockid_t clock) {
    timespec value{};
    if (clock_gettime(clock, &value) != 0)
      return -1;
    return static_cast<int64_t>(value.tv_sec) * 1000000 + value.tv_nsec / 1000;
  }

  static void Write(const char* phase,
                    const char* stage,
                    int64_t detail,
                    int64_t mono_us,
                    int64_t elapsed_us,
                    int64_t cpu_us) {
    const int64_t unix_us = ReadClock(CLOCK_REALTIME);
    LOG(WARNING) << "OHOS StartupProbe phase=" << phase << " stage=" << stage
                 << " detail=" << detail << " pid=" << getpid()
                 << " tid=" << syscall(SYS_gettid) << " mono_us=" << mono_us
                 << " unix_us=" << unix_us << " elapsed_us=" << elapsed_us
                 << " thread_cpu_us=" << cpu_us;
  }

  const char* const stage_;
  const unsigned attempt_;
  int64_t start_us_ = -1;
  int64_t cpu_us_ = -1;
};

}  // namespace base

#define OHOS_STARTUP_SCOPE(variable, stage)         \
  static std::atomic<unsigned> variable##_count{0}; \
  base::OhosStartupProbe variable(stage, variable##_count)
#define OHOS_STARTUP_MARK(stage, detail)                                  \
  do {                                                                    \
    static std::atomic<unsigned> ohos_startup_mark_count{0};              \
    base::OhosStartupProbe::Mark(stage, detail, ohos_startup_mark_count); \
  } while (false)
#else
#define OHOS_STARTUP_SCOPE(variable, stage)
#define OHOS_STARTUP_MARK(stage, detail) \
  do {                                   \
  } while (false)
#endif  // BUILDFLAG(IS_OHOS)

#endif  // BASE_OHOS_STARTUP_PROBE_H_
