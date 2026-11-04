// Copyright 2026 The Chromium Authors. All rights reserved.
#ifndef UI_OZONE_PLATFORM_OHOS_OHOS_OVERLAY_MARKER_THREAD_H_
#define UI_OZONE_PLATFORM_OHOS_OHOS_OVERLAY_MARKER_THREAD_H_

#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace ui::ohos_overlay {

// SurfaceTransactionListener registers its construction thread as a delegate
// compositor identity. Never construct it on ArkUI's transaction thread: RS
// would then classify ordinary UI updates as web-only/direct composition.
// Keep one real, idle worker alive for the process lifetime because RS retains
// the first registered thread identity even after a listener is destroyed.
// Only listener operations run here; RS nodes and transaction handlers stay on
// their owning UI thread. Run returns before any captured caller storage dies.
class OverlayMarkerThread {
 public:
  static OverlayMarkerThread& Get() {
    static auto* worker = new OverlayMarkerThread;
    return *worker;
  }

  void Run(std::function<void()> action) {
    std::lock_guard<std::mutex> request(request_mutex_);
    std::unique_lock<std::mutex> lock(mutex_);
    action_ = std::move(action);
    completed_ = false;
    ready_.notify_one();
    finished_.wait(lock, [this] { return completed_; });
  }

 private:
  OverlayMarkerThread() : thread_([this] { Work(); }) {}
  void Work() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
      ready_.wait(lock, [this] { return static_cast<bool>(action_); });
      auto action = std::move(action_);
      action_ = nullptr;
      lock.unlock();
      action();
      lock.lock();
      completed_ = true;
      finished_.notify_one();
    }
  }
  std::mutex request_mutex_, mutex_;
  std::condition_variable ready_, finished_;
  std::function<void()> action_;
  bool completed_ = false;
  std::thread thread_;
};

}  // namespace ui::ohos_overlay
#endif  // UI_OZONE_PLATFORM_OHOS_OHOS_OVERLAY_MARKER_THREAD_H_
