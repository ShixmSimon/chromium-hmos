// Copyright 2026 The Chromium Authors. All rights reserved.
#include "ui/ozone/platform/ohos/ohos_overlay_marker_thread.h"

#include <cassert>
#include <cstdio>
#include <thread>
#include <vector>

// Model RS's sticky sender-thread identity and the real listener's affinity
// check. Listener teardown/recreation must never make ArkUI commands eligible
// for delegate-only composition, even with multiple concurrent content hosts.
int main() {
  using ui::ohos_overlay::OverlayMarkerThread;
  const auto ui_thread = std::this_thread::get_id();
  std::thread::id delegate_thread;
  unsigned sequence = 0;
  auto exercise = [&] {
    std::thread::id listener_owner;
    OverlayMarkerThread::Get().Run([&] {
      listener_owner = std::this_thread::get_id();
      if (delegate_thread == std::thread::id{})
        delegate_thread = listener_owner;
      assert(listener_owner == delegate_thread);
      assert(listener_owner != ui_thread);
    });
    for (int frame = 0; frame < 100; ++frame) {
      unsigned marker = 0;
      OverlayMarkerThread::Get().Run([&] {
        assert(std::this_thread::get_id() == listener_owner);
        marker = ++sequence;
      });
      // A caller may enqueue/flush only after command construction completes.
      assert(marker != 0);
      assert(std::this_thread::get_id() != delegate_thread);
    }
    OverlayMarkerThread::Get().Run([&] {
      assert(std::this_thread::get_id() == listener_owner);
    });
  };
  exercise();
  std::vector<std::thread> hosts;
  for (int host = 0; host < 4; ++host)
    hosts.emplace_back(exercise);
  for (auto& host : hosts)
    host.join();
  exercise();
  assert(sequence == 600);
  assert(delegate_thread != ui_thread);
  std::puts("PASS marker affinity, sticky identity isolation, concurrent hosts and completion ordering");
}
