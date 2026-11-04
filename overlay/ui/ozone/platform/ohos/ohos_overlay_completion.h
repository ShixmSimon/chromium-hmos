// Copyright 2026 The Chromium Authors. All rights reserved.
#ifndef UI_OZONE_PLATFORM_OHOS_OHOS_OVERLAY_COMPLETION_H_
#define UI_OZONE_PLATFORM_OHOS_OHOS_OVERLAY_COMPLETION_H_

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>

namespace ui::ohos_overlay {
// RS keys normal transaction callbacks by the exact (context, timestamp) pair.
// FlushImplicitTransaction clamps to the handler's previous timestamp. Choose
// a fresh value above both that watermark and any earlier bridge submission;
// wall-clock/vsync timestamps alone can repeat or move backwards.
inline uint64_t NextCompletionTimestamp(uint64_t now, uint64_t handler,
                                        uint64_t previous) {
  const auto floor = std::max(handler, previous);
  if (floor == std::numeric_limits<uint64_t>::max())
    return 0;
  return std::max(now, floor + 1);
}

struct TransactionCompletion {
  std::atomic<uint64_t> expected{0};
  std::atomic<uint64_t> acknowledged{0};
  void Acknowledge(uint64_t timestamp) {
    // Late IPC callbacks may outlive a submission or even its backend. They
    // only update this independently owned state, never a node or buffer.
    auto old = acknowledged.load(std::memory_order_acquire);
    while (old < timestamp &&
           !acknowledged.compare_exchange_weak(old, timestamp)) {
    }
  }
  bool Complete() const {
    const auto value = expected.load(std::memory_order_acquire);
    return value && acknowledged.load(std::memory_order_acquire) == value;
  }
};
}  // namespace ui::ohos_overlay
#endif
