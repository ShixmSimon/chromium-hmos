// Debug experiment only. Compiled with the system libc++ __h ABI namespace.
// Function objects use the legacy libc++ virtual callable ABI; no app string,
// shared_ptr, or allocator-owned object is passed to the system.
#include <atomic>
#include <functional>
#include <queue>
#include <dlfcn.h>
#include <hilog/log.h>
#include <poll.h>
#include <unistd.h>
#include <pthread.h>

namespace {
constexpr unsigned kSlots = 12;
struct Slot {
  std::atomic<int> fence{-2};
  std::atomic<uint32_t> sequence{0};
  std::atomic<bool> serverRelease{false};
  void* node = nullptr;
  std::function<void(int)> callback;
};
Slot slots[kSlots];
pthread_mutex_t routingMutex = PTHREAD_MUTEX_INITIALIZER;
std::atomic<uint64_t> completedTransactions{0};
static_assert(sizeof(std::function<void(int)>) == 48);
}

extern "C" void RsTrackBuffer(unsigned slot, uint32_t sequence, void* node) {
  if (slot >= kSlots) return;
  slots[slot].node = node;
  slots[slot].sequence.store(sequence, std::memory_order_release);
}

// Borrowed view of the system OnCompletedRet queue. Its original elements and
// fence references remain owned by the system throughout this callback. Only
// known test sequences are forwarded; no foreign node is touched.
struct ReleaseRecord {
  uint64_t nodeId;
  int32_t clientPid;
  uint64_t queueId;
  void* fence;
  uint32_t sequence;
  bool activeGame;
};
static_assert(sizeof(ReleaseRecord) == 40);

extern "C" bool RsRegisterRelease(void* library, void* remoteArgument) {
  using Queue = std::queue<ReleaseRecord>;
  static_assert(sizeof(Queue) == 48);
  static bool attempted = false;
  static bool registered = false;
  if (attempted) return registered;
  attempted = true;
  auto instance = reinterpret_cast<void*(*)()>(dlsym(library,
      "_ZN4OHOS5Rosen32SurfaceNodeBufferReleaseListener11GetInstanceEv"));
  auto connect = reinterpret_cast<bool(*)(void*,void*)>(dlsym(library,
      "_ZN4OHOS5Rosen32SurfaceNodeBufferReleaseListener18SetConnectToRenderENS_4sptrINS_13IRemoteObjectEEE"));
  auto reg = reinterpret_cast<bool(*)(void*,const void*)>(dlsym(library,
      "_ZN4OHOS5Rosen32SurfaceNodeBufferReleaseListener29RegisterReleaseBufferCallBackENSt3__h8functionIFvNS2_5queueINS0_14OnCompletedRetENS2_5dequeIS5_NS2_9allocatorIS5_EEEEEEEEE"));
  static auto release = reinterpret_cast<int(*)(void*,uint32_t,void*)>(dlsym(library,
      "_ZN4OHOS5Rosen13RSSurfaceNode13ReleaseBufferEjNS_4sptrINS_9SyncFenceEEE"));
  if (!instance || !connect || !reg || !release) {
    OH_LOG_Print(LOG_APP, LOG_INFO, 0xD003F00, "LumieRsProbe", "release_api_missing instance=%{public}d connect=%{public}d register=%{public}d release=%{public}d",
      instance!=nullptr,connect!=nullptr,reg!=nullptr,release!=nullptr);
    return false;
  }
  static std::function<void(Queue)> callback = [](const Queue& borrowed) {
    if (borrowed.size() > 64) return;
    Queue copy = borrowed; // Copy plain records, without taking fence ownership.
    while (!copy.empty()) {
      auto record = copy.front(); copy.pop();
      OH_LOG_Print(LOG_APP, LOG_INFO, 0xD003F00, "LumieRsProbe",
        "release_record node=%{public}llu pid=%{public}d queue=%{public}llu seq=%{public}u",
        (unsigned long long)record.nodeId, record.clientPid, (unsigned long long)record.queueId, record.sequence);
      if ((record.nodeId >> 32) != static_cast<uint64_t>(getpid())) continue;
      pthread_mutex_lock(&routingMutex);
      for (unsigned slot = 0; slot < kSlots; ++slot) {
        if (slots[slot].sequence.load(std::memory_order_acquire) == record.sequence) {
          slots[slot].serverRelease.store(true, std::memory_order_release);
          int rc = release(slots[slot].node, record.sequence, &record.fence);
          OH_LOG_Print(LOG_APP, LOG_INFO, 0xD003F00, "LumieRsProbe",
            "release_forward slot=%{public}u rc=%{public}d", slot, rc);
          break;
        }
      }
      pthread_mutex_unlock(&routingMutex);
    }
  };
  void* listener = instance();
  // Do not replace a listener already installed by the application or engine.
  if (!connect(listener, remoteArgument)) return false;
  registered = reg(listener, &callback);
  OH_LOG_Print(LOG_APP, LOG_INFO, 0xD003F00, "LumieRsProbe",
               "release_listener_registered=%{public}d", registered);
  return registered;
}

// Serialize against ReleaseBuffer itself, not just its nested callback: the
// system manager still touches the node after delivering the callback.
extern "C" bool RsTakeRetiredBuffer(unsigned slot) {
  if (slot >= kSlots) return false;
  pthread_mutex_lock(&routingMutex);
  int fence = slots[slot].fence.load(std::memory_order_acquire);
  bool ready = fence == -1 || fence == -3;
  if (fence >= 0) {
    pollfd p{fence, POLLIN, 0};
    ready = poll(&p, 1, 0) == 1 && (p.revents & POLLIN);
  }
  ready = ready && slots[slot].serverRelease.load(std::memory_order_acquire);
  if (ready) {
    if (fence >= 0) close(fence);
    slots[slot].fence.store(-3, std::memory_order_release);
    slots[slot].sequence.store(0, std::memory_order_release);
    slots[slot].node = nullptr;
  }
  pthread_mutex_unlock(&routingMutex);
  return ready;
}

extern "C" const void* RsMakeTransactionCallback() {
  using Queue = std::queue<uint64_t>;
  static_assert(sizeof(Queue) == 48);
  static std::function<void(uint64_t, uint64_t, Queue)> callback =
      [](uint64_t timestamp, uint64_t source, const Queue& sequences) {
    Queue copy = sequences;
    while (!copy.empty()) {
      uint64_t sequence = copy.front(); copy.pop();
      if (sequence < 64)
        completedTransactions.fetch_or(uint64_t{1} << sequence, std::memory_order_release);
    }
    OH_LOG_Print(LOG_APP, LOG_INFO, 0xD003F00, "LumieRsProbe",
                 "transaction_complete timestamp=%{public}llu source=%{public}llu count=%{public}zu first=%{public}llu last=%{public}llu",
                 (unsigned long long)timestamp, (unsigned long long)source, sequences.size(),
                 sequences.empty() ? 0ULL : (unsigned long long)sequences.front(),
                 sequences.empty() ? 0ULL : (unsigned long long)sequences.back());
  };
  return &callback;
}

extern "C" bool RsHasCompletedTransaction(uint64_t sequence) {
  return sequence < 64 &&
      (completedTransactions.load(std::memory_order_acquire) & (uint64_t{1} << sequence));
}

extern "C" const void* RsMakeBufferCallback(unsigned slot) {
  if (slot >= kSlots || slots[slot].callback) return nullptr;
  slots[slot].callback = [slot](int fence) {
    slots[slot].fence.store(fence, std::memory_order_release);
    OH_LOG_Print(LOG_APP, LOG_INFO, 0xD003F00, "LumieRsProbe",
                 "buffer_release slot=%{public}u fence=%{public}d", slot, fence);
  };
  return &slots[slot].callback;
}

extern "C" void RsDiscardUnsubmittedCallback(unsigned slot) {
  if (slot >= kSlots || slots[slot].sequence.load(std::memory_order_acquire) != 0 ||
      slots[slot].fence.load(std::memory_order_acquire) != -2) return;
  slots[slot].callback = nullptr;
}

extern "C" void RsPollBufferCallbacks() {
  for (unsigned i = 0; i < kSlots; ++i) {
    int fence = slots[i].fence.load(std::memory_order_acquire);
    if (fence == -2 || fence == -3) continue;
    bool complete = fence == -1;
    if (fence >= 0) {
      pollfd p{fence, POLLIN, 0};
      complete = poll(&p, 1, 0) == 1 && (p.revents & POLLIN);
    }
    OH_LOG_Print(LOG_APP, LOG_INFO, 0xD003F00, "LumieRsProbe",
                 "buffer_retire slot=%{public}u signaled=%{public}d", i, complete);
    if (complete) {
      if (fence >= 0) close(fence);
      slots[i].fence.store(-3, std::memory_order_release);
    }
  }
}
