// Copyright 2026 The Chromium Authors. All rights reserved.
#include "ui/ozone/platform/ohos/ohos_overlay_completion.h"

#include <cassert>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <utility>

int main() {
  using namespace ui::ohos_overlay;
  // Model the server's exact context/timestamp key, including unrelated ArkUI
  // transactions, duplicate clocks, and the handler's max(timestamp) clamp.
  using Key = std::pair<uint64_t, uint64_t>;
  std::map<Key, std::function<void()>> callbacks;
  auto state = std::make_shared<TransactionCompletion>();
  assert(!state->Complete());
  uint64_t handler_timestamp = 500;
  const auto first = NextCompletionTimestamp(100, handler_timestamp, 0);
  assert(first == 501);
  state->expected = first;
  callbacks[{42, first}] = [state, first] { state->Acknowledge(first); };
  auto process = [&](uint64_t context, uint64_t timestamp) {
    const auto key = Key{context, timestamp};
    auto it = callbacks.find(key);
    if (it != callbacks.end()) {
      it->second();
      callbacks.erase(it);
    }
  };
  process(42, 500);  // An older ArkUI batch cannot acknowledge this frame.
  process(43, first);  // Nor can another content host.
  process(42, first + 1);  // The server does not acknowledge by >= timestamp.
  assert(!state->Complete());
  process(42, std::max(first, handler_timestamp));
  assert(state->Complete());
  const auto second = NextCompletionTimestamp(100, first, first);
  assert(second == first + 1);
  state->expected = second;
  state->Acknowledge(first);
  assert(!state->Complete());
  state->Acknowledge(second);
  state->Acknowledge(first);  // Out-of-order IPC cannot erase a newer ack.
  assert(state->Complete());
  assert(NextCompletionTimestamp(1000, first, second) == 1000);
  assert(!NextCompletionTimestamp(1000, UINT64_MAX, second));
  assert(!NextCompletionTimestamp(1000, second, UINT64_MAX));
  // Callback storage remains safe after the backend drops its own state.
  callbacks[{42, second}] = [state, second] { state->Acknowledge(second); };
  state.reset();
  process(42, second);
  std::puts("PASS exact transaction completion keys, timestamp clamp, stale callbacks and lifetime");
}
