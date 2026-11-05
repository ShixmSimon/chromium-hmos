// Copyright 2026 The Chromium Authors. All rights reserved.
// Firmware-pinned RS compatibility boundary. No app-owned C++ object is passed
// as a system shared_ptr/sptr; only the verified legacy std::function ABI
// crosses.
#include "ohos_rs_overlay_backend.h"
#include "ohos_rs_abi.h"
#include "ohos_overlay_completion.h"
#include "ohos_overlay_geometry.h"

#include <arkui/native_render.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <hilog/log.h>
#include <link.h>
#include <native_buffer/native_buffer.h>
#include <poll.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <queue>
#ifdef OHOS_OVERLAY_DIAGNOSTICS
#include <hilog/log.h>
#define TRACE(...) \
  OH_LOG_Print(LOG_APP, LOG_INFO, 0xD003F01, "LumieRealVideo", __VA_ARGS__)
#else
#define TRACE(...) ((void)0)
#endif

#if !defined(__aarch64__)
#error The RS compatibility backend is restricted to the verified AArch64 ABI.
#endif

extern "C" void OhosRsSret3(void*, void*, uintptr_t, uintptr_t, uintptr_t);
__asm__(
    ".text\n.balign 4\n.global OhosRsSret3\n"
#if !defined(OHOS_OVERLAY_DIAGNOSTICS)
    ".hidden OhosRsSret3\n"
#endif
    ".type "
    "OhosRsSret3,%function\n"
    "OhosRsSret3:\n stp x29,x30,[sp,#-16]!\n mov x29,sp\n mov x8,x0\n mov "
    "x16,x1\n"
    "mov x0,x2\n mov x1,x3\n mov x2,x4\n blr x16\n ldp x29,x30,[sp],#16\n ret\n"
    ".size OhosRsSret3,.-OhosRsSret3\n");

namespace ui::ohos_overlay {
namespace {
// This opt-in bridge can stall independently of the GPU's swap queue. Resolve
// the logger from the application rather than adding an NDK link dependency.
// Only bounded routing samples and a throttled backlog are recorded; no page
// data, native pointers or buffer contents are included.
template <typename... Args>
void BacklogLog(const char* format, Args... args) {
  static auto print = reinterpret_cast<decltype(&OH_LOG_Print)>(
      dlsym(RTLD_DEFAULT, "OH_LOG_Print"));
  if (print)
    print(LOG_APP, LOG_WARN, 0xD003F01, "LumieOverlayQueue", format, args...);
}
std::atomic<unsigned> release_samples{0};
struct SystemShared {
  void* object = nullptr;
  void* control = nullptr;
};
static_assert(sizeof(SystemShared) == 16);
static_assert(sizeof(std::function<void(int)>) == 48);
uint32_t Hash(const void* ptr, size_t size) {
  struct Range {
    uintptr_t address;
    size_t size;
    bool valid;
  } range{reinterpret_cast<uintptr_t>(ptr), size, false};
  dl_iterate_phdr(
      [](dl_phdr_info* info, size_t, void* data) {
        auto& r = *static_cast<Range*>(data);
        for (unsigned i = 0; i < info->dlpi_phnum; ++i) {
          const auto& p = info->dlpi_phdr[i];
          const auto start = info->dlpi_addr + p.p_vaddr;
          if (p.p_type == PT_LOAD &&
              (p.p_flags & (PF_R | PF_X)) == (PF_R | PF_X) &&
              r.address >= start && r.address - start <= p.p_memsz &&
              r.size <= p.p_memsz - (r.address - start)) {
            r.valid = true;
            return 1;
          }
        }
        return 0;
      },
      &range);
  if (!range.valid)
    return 0;
  uint32_t hash = 2166136261u;
  const auto* bytes = static_cast<const uint8_t*>(ptr);
  for (size_t i = 0; i < size; ++i)
    hash = (hash ^ bytes[i]) * 16777619u;
  return hash;
}
bool Ready(int fd) {
  if (fd == -1)
    return true;
  if (fd < 0)
    return false;
  pollfd poll_fd{fd, POLLIN, 0};
  return poll(&poll_fd, 1, 0) == 1 && (poll_fd.revents & POLLIN);
}
struct Fd {
  int fd = -1;
  ~Fd() {
    if (fd >= 0)
      close(fd);
  }
  Fd() = default;
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
};
struct Api {
  const RsAbiProfile* profile = nullptr;
  void* rs = nullptr;
  void* surface = nullptr;
  void* create = nullptr;
  void* canvas_create = nullptr;
  void* context_get = nullptr;
  void* node_get = nullptr;
  void* get_surface = nullptr;
  void* manager = nullptr;
  void (*weak_release)(void*) = nullptr;
  void (*hardware)(void*, bool, int, bool) = nullptr;
  bool (*delegate)(void*, bool) = nullptr;
  void (*add_child)(void*, SystemShared*, int) = nullptr;
  void (*remove_child)(void*, SystemShared*) = nullptr;
  void (*bounds)(void*, float, float, float, float) = nullptr;
  void (*frame)(void*, float, float, float, float) = nullptr;
  void (*source)(void*, float, float, float, float) = nullptr;
  void (*destination)(void*, float, float, float, float) = nullptr;
  void (*gravity)(void*, int) = nullptr;
  void (*z)(void*, float) = nullptr;
  void (*visible)(void*, bool) = nullptr;
  void (*clip_to_bounds)(void*, bool) = nullptr;
  void (*clean)(void*, bool) = nullptr;
  void (*set_buffer)(void*, void*, void*, const void*) = nullptr;
  void (*flush)(void*, uint64_t, const void*, bool, uint64_t) = nullptr;
  void (*vsync)(void*) = nullptr;
  bool (*transaction_callback)(void*, uint64_t, uint64_t, const void*) = nullptr;
  void* (*release_instance)() = nullptr;
  bool (*release_connect)(void*, void*) = nullptr;
  bool (*release_register)(void*, const void*) = nullptr;
  int (*release_buffer)(void*, uint32_t, void*) = nullptr;
  void (*surface_drop)(void*, const void*) = nullptr;
  int (*background)(void*) = nullptr;
  template <typename T>
  bool Get(T& target,
           const char* symbol,
           RsCheckedFunction function = RsCheckedFunction::kCount) {
    void* value = dlsym(rs, symbol);
    const RsFingerprint* fingerprint =
        function == RsCheckedFunction::kCount
            ? nullptr
            : &RsFunctionFingerprint(*profile, function);
    if (!value ||
        (fingerprint && Hash(value, fingerprint->bytes) != fingerprint->hash)) {
      BacklogLog("abi rejected profile=%{public}s symbol=%{public}s",
                 profile ? profile->name : "unknown", symbol);
      return false;
    }
    target = reinterpret_cast<T>(value);
    return true;
  }
  bool Load() {
    const uint32_t render_hash = Hash(
        reinterpret_cast<void*>(&OH_ArkUI_RenderNodeUtils_CreateNode), 148);
    profile = SelectRsAbiProfile(render_hash);
    if (!profile) {
      BacklogLog("unsupported RenderNode ABI fingerprint=%{public}u",
                 render_hash);
      return false;
    }
    rs = dlopen("/system/lib64/librender_service_client.z.so",
                RTLD_NOW | RTLD_NOLOAD);
    surface = dlopen("/system/lib64/chipset-pub-sdk/libsurface.z.so",
                     RTLD_NOW | RTLD_NOLOAD);
    if (!rs || !surface)
      return false;
    void* check = nullptr;
    if (!Get(check,
             "_ZN4OHOS5Rosen13RSSurfaceNodeC2ERKNS0_"
             "19RSSurfaceNodeConfigEbNSt3__h10shared_ptrINS0_11RSUIContextEEE",
             RsCheckedFunction::kSurfaceConstructor) ||
        !Get(check,
             "_ZN4OHOS5Rosen13RSSurfaceNode6CreateERKNS0_"
             "19RSSurfaceNodeConfigENS0_17RSSurfaceNodeTypeEbbNSt3__h10shared_"
             "ptrINS0_11RSUIContextEEE",
             RsCheckedFunction::kSurfaceFactory) ||
        !Get(check, "_ZN4OHOS5Rosen13RSSurfaceNodeD2Ev",
             RsCheckedFunction::kSurfaceDestructor) ||
        !Get(check,
             "_ZN4OHOS5Rosen11RSUIContextC2EmRNS_4sptrINS_13IRemoteObjectEEE",
             RsCheckedFunction::kContextConstructor) ||
        !Get(check,
             "_ZN4OHOS5Rosen13RSSurfaceNode26SetBufferAvailableCallbackENSt3__"
             "h8functionIFvvEEE",
             RsCheckedFunction::kBufferCallback))
      return false;
    auto identity = dlsym(
        surface, "_ZN4OHOS17SurfaceBufferImpl27SurfaceBufferToNativeBufferEv");
    uint32_t instruction = 0;
    if (identity)
      std::memcpy(&instruction, identity, sizeof(instruction));
    if (instruction != 0xd65f03c0)
      return false;
#define RESOLVE(field, symbol) \
  if (!Get(field, symbol))     \
  return false
    if (!Get(create,
             "_ZN4OHOS5Rosen13RSSurfaceNode6CreateERKNS0_"
             "19RSSurfaceNodeConfigEbNSt3__h10shared_ptrINS0_11RSUIContextEEE",
             RsCheckedFunction::kContextSurfaceFactory))
      return false;
    if (!Get(canvas_create,
             "_ZN4OHOS5Rosen12RSCanvasNode6CreateEbbNSt3__h10shared_ptrINS0_11RSUIContextEEE",
             RsCheckedFunction::kCanvasFactory))
      return false;
    RESOLVE(context_get,
            "_ZNK4OHOS5Rosen18RSUIContextManager14GetRSUIContextEm");
    RESOLVE(node_get,
            "_ZNK4OHOS5Rosen11RSNodeMapV27GetNodeINS0_"
            "6RSNodeEEEKNSt3__h10shared_ptrIT_EEm");
    void* (*get_manager)() = nullptr;
    RESOLVE(get_manager,
            "_ZN4OHOS5Rosen18RSUIContextManager15MutableInstanceEv");
    manager = get_manager();
    if (!manager)
      return false;
    RESOLVE(weak_release, "_ZNSt3__h19__shared_weak_count14__release_weakEv");
    RESOLVE(hardware,
            "_ZN4OHOS5Rosen13RSSurfaceNode18SetHardwareEnabledEbNS0_"
            "19SelfDrawingNodeTypeEb");
    RESOLVE(delegate, "_ZN4OHOS5Rosen13RSSurfaceNode15SetDelegateModeEb");
    RESOLVE(add_child,
            "_ZN4OHOS5Rosen6RSNode8AddChildENSt3__h10shared_ptrIS1_EEi");
    if (!Get(remove_child,
             "_ZN4OHOS5Rosen6RSNode11RemoveChildENSt3__h10shared_ptrIS1_EE",
             RsCheckedFunction::kRemoveChild))
      return false;
    RESOLVE(bounds, "_ZN4OHOS5Rosen6RSNode9SetBoundsEffff");
    RESOLVE(frame, "_ZN4OHOS5Rosen6RSNode8SetFrameEffff");
    RESOLVE(source, "_ZN4OHOS5Rosen13RSSurfaceNode18SetDelegateSrcRectEffff");
    RESOLVE(destination,
            "_ZN4OHOS5Rosen13RSSurfaceNode18SetDelegateDstRectEffff");
    RESOLVE(gravity, "_ZN4OHOS5Rosen6RSNode15SetFrameGravityENS0_7GravityE");
    RESOLVE(z, "_ZN4OHOS5Rosen6RSNode12SetPositionZEf");
    RESOLVE(visible, "_ZN4OHOS5Rosen6RSNode10SetVisibleEb");
    RESOLVE(clip_to_bounds, "_ZN4OHOS5Rosen6RSNode15SetClipToBoundsEb");
    if (!Get(clean, "_ZN4OHOS5Rosen13RSSurfaceNode11CleanBufferEb",
             RsCheckedFunction::kCleanBuffer))
      return false;
    RESOLVE(set_buffer,
            "_ZN4OHOS5Rosen13RSSurfaceNode9SetBufferENS_4sptrINS_"
            "13SurfaceBufferEEENS_18UniqueFdAddDeletorINS_"
            "14DefaultDeleterEEERKNSt3__h8functionIFviEEE");
    RESOLVE(
        flush,
        "_ZN4OHOS5Rosen20RSTransactionHandler24FlushImplicitTransactionEmRKNSt3"
        "__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEEbm");
    RESOLVE(vsync, "_ZN4OHOS5Rosen11RSUIContext20RequestVsyncCallbackEv");
    if (!Get(transaction_callback,
             "_ZN4OHOS5Rosen17RSRenderInterface31RegisterTransactionDataCallback"
             "EmmNSt3__h8functionIFvvEEE",
             RsCheckedFunction::kTransactionCallbackRegister))
      return false;
    // The timestamp read below is allowed only for this inspected base-library
    // implementation. Other base builds fail closed, even if the client ABI
    // matches a known profile. Do not guess the handler layout from OS version.
    if (Hash(reinterpret_cast<void*>(flush), kRsCompletionFlush.bytes) !=
        kRsCompletionFlush.hash) {
      BacklogLog("transaction completion ABI unavailable");
      return false;
    }
    RESOLVE(release_instance,
            "_ZN4OHOS5Rosen32SurfaceNodeBufferReleaseListener11GetInstanceEv");
    RESOLVE(release_connect,
            "_ZN4OHOS5Rosen32SurfaceNodeBufferReleaseListener18SetConnectToRend"
            "erENS_4sptrINS_13IRemoteObjectEEE");
    RESOLVE(release_register,
            "_ZN4OHOS5Rosen32SurfaceNodeBufferReleaseListener29RegisterReleaseB"
            "ufferCallBackENSt3__h8functionIFvNS2_5queueINS0_"
            "14OnCompletedRetENS2_5dequeIS5_NS2_9allocatorIS5_EEEEEEEEE");
    RESOLVE(release_buffer,
            "_ZN4OHOS5Rosen13RSSurfaceNode13ReleaseBufferEjNS_"
            "4sptrINS_9SyncFenceEEE");
    if (!Get(get_surface, "_ZNK4OHOS5Rosen13RSSurfaceNode10GetSurfaceEv",
             RsCheckedFunction::kGetSurface))
      return false;
    surface_drop = reinterpret_cast<decltype(surface_drop)>(
        dlsym(surface, "_ZN4OHOS7RefBase12DecStrongRefEPKv"));
    background = reinterpret_cast<decltype(background)>(
        dlsym(surface, "_ZN4OHOS15ProducerSurface12GoBackgroundEv"));
    const auto& background_fingerprint =
        RsFunctionFingerprint(*profile, RsCheckedFunction::kGoBackground);
    return surface_drop && background &&
           Hash(reinterpret_cast<void*>(background),
                background_fingerprint.bytes) == background_fingerprint.hash;
#undef RESOLVE
  }
  void Drop(SystemShared& ref) {
    if (!ref.control)
      return;
    void* control = ref.control;
    ref = {};
    auto* owners = reinterpret_cast<long*>(static_cast<char*>(control) + 8);
    if (__atomic_fetch_sub(owners, 1, __ATOMIC_ACQ_REL) == 0) {
      auto** table = *static_cast<void***>(control);
      reinterpret_cast<void (*)(void*)>(table[2])(control);
      weak_release(control);
    }
  }
};

// Shared process dispatcher, installed once without replacing another owner.
// Routes retain no foreign node; an allocation may be submitted to one node at
// a time. The routing lock covers the entire system ReleaseBuffer call.
struct Slot {
  uint64_t submission = 0, allocation = 0;
  unsigned plane = 0;
  uint32_t sequence = 0;
  void* node = nullptr;
  void* context = nullptr;
  Api* api = nullptr;
  std::atomic<int> fence{-2};
  bool server_returned = false;
  std::function<void(int)> callback;
  ~Slot() {
    const int fd = fence.load();
    if (fd >= 0)
      close(fd);
  }
};
struct ReleaseRecord {
  uint64_t node_id;
  int32_t client_pid;
  uint64_t queue_id;
  void* fence;
  uint32_t sequence;
  bool active_game;
};
static_assert(sizeof(ReleaseRecord) == 40);
pthread_mutex_t route_mutex = PTHREAD_MUTEX_INITIALIZER;
std::map<uint32_t, Slot*> routes;
bool dispatcher_registered = false;
bool RegisterDispatcher(Api& api, void* remote) {
  pthread_mutex_lock(&route_mutex);
  if (dispatcher_registered) {
    pthread_mutex_unlock(&route_mutex);
    return true;
  }
  using Queue = std::queue<ReleaseRecord>;
  static_assert(sizeof(Queue) == 48);
  static const std::function<void(Queue)> callback = [](const Queue& borrowed) {
    Queue copy = borrowed;
    while (!copy.empty()) {
      const auto record = copy.front();
      copy.pop();
      if ((record.node_id >> 32) != static_cast<uint64_t>(getpid()))
        continue;
      TRACE("release_record node=%{public}llu pid=%{public}d seq=%{public}u",
            static_cast<unsigned long long>(record.node_id), record.client_pid,
            record.sequence);
      pthread_mutex_lock(&route_mutex);
      auto found = routes.find(record.sequence);
      const bool sample = release_samples.fetch_add(1) < 128;
      if (sample)
        BacklogLog("release seq=%{public}u routed=%{public}d", record.sequence,
                   found != routes.end());
      if (found != routes.end()) {
        Slot& slot = *found->second;
        // client_pid is zero in verified delegate-composition notifications.
        // Bind the record's exact node via its context map; PID/sequence alone
        // must not route a release for another node using the same allocation.
        SystemShared recorded_node;
        OhosRsSret3(&recorded_node, slot.api->node_get,
                    reinterpret_cast<uintptr_t>(slot.context) + 40,
                    record.node_id, 0);
        const bool matches = recorded_node.object == slot.node;
        if (sample)
          BacklogLog(
              "match seq=%{public}u plane=%{public}u node=%{public}d "
              "returned=%{public}d",
              record.sequence, slot.plane, matches, slot.server_returned);
        slot.api->Drop(recorded_node);
        if (matches && !slot.server_returned) {
          void* fence = record.fence;
          const int rc =
              slot.api->release_buffer(slot.node, record.sequence, &fence);
          TRACE("release_forward rc=%{public}d fence=%{public}d", rc,
                slot.fence.load());
          slot.server_returned = rc == 0;
          if (sample)
            BacklogLog(
                "forward seq=%{public}u rc=%{public}d fence=%{public}d "
                "ready=%{public}d",
                record.sequence, rc, slot.fence.load(),
                Ready(slot.fence.load()));
        }
      }
      pthread_mutex_unlock(&route_mutex);
    }
  };
  void* listener = api.release_instance();
  dispatcher_registered = listener && api.release_connect(listener, remote) &&
                          api.release_register(listener, &callback);
  pthread_mutex_unlock(&route_mutex);
  return dispatcher_registered;
}

struct Batch : Backend::Prepared {
  uint64_t submission = 0;
  unsigned count = 0;
  struct Item {
    std::unique_ptr<Slot> slot;
    OH_NativeBuffer* buffer = nullptr;  // Borrowed from Session's read lease.
    Rect source, destination;
    Fd acquire;
  } items[2];
};

class RsBackend final : public Backend {
 public:
  ~RsBackend() override {
    if (!slots_.empty())
      std::abort();
    api_.Drop(nodes_[0]);
    api_.Drop(nodes_[1]);
    api_.Drop(video_clip_);
    if (handler_)
      Flush();
    api_.Drop(parent_);
    api_.Drop(context_);
    if (api_.surface)
      dlclose(api_.surface);
    if (api_.rs)
      dlclose(api_.rs);
  }
  bool Initialize(uint64_t token, uint64_t parent_id) {
    owner_ = pthread_self();
    if ((token >> 32) != static_cast<uint64_t>(gettid()) ||
        (parent_id >> 32) != static_cast<uint64_t>(getpid()) || !api_.Load())
      return false;
    OhosRsSret3(&context_, api_.context_get,
                reinterpret_cast<uintptr_t>(api_.manager), token, 0);
    if (!context_.object)
      return false;
    std::memcpy(&handler_, static_cast<char*>(context_.object) + 120,
                sizeof(handler_));
    std::memcpy(&render_interface_, static_cast<char*>(context_.object) + 136,
                sizeof(render_interface_));
    if (!handler_ || !render_interface_)
      return false;
    context_token_ = token;
    completion_ = std::make_shared<TransactionCompletion>();
    OhosRsSret3(&parent_, api_.node_get,
                reinterpret_cast<uintptr_t>(context_.object) + 40, parent_id,
                0);
    if (!parent_.object ||
        !RegisterDispatcher(api_, static_cast<char*>(context_.object) + 480))
      return false;
    for (unsigned i = 0; i < 2; ++i) {
      if (!CreateLayer(i))
        return false;
    }
    OhosRsSret3(&video_clip_, api_.canvas_create, 0, 0,
                reinterpret_cast<uintptr_t>(&context_));
    if (!video_clip_.object)
      return false;
    api_.clip_to_bounds(video_clip_.object, true);
    api_.z(video_clip_.object, 0.f);
    api_.add_child(parent_.object, &nodes_[0], -1);
    api_.add_child(parent_.object, &video_clip_, -1);
    api_.add_child(video_clip_.object, &nodes_[1], -1);
    attached_ = true;
    // Prove normal transaction completion before accepting any pixel buffers.
    startup_pending_ = true;
    ConfirmedFlush();
    if (drain_failed_) {
      api_.remove_child(video_clip_.object, &nodes_[1]);
      api_.remove_child(parent_.object, &video_clip_);
      api_.remove_child(parent_.object, &nodes_[0]);
      attached_ = false;
      Flush();
    }
    return !drain_failed_;
  }
  bool Supports(const Frame& f) const override {
    if (!OnOwner() || draining_ || destroyed_ || drain_failed_)
      return false;
    return f.primary.lease->BorrowAcquireFence() >= -1 &&
           (!f.video || f.video->lease->BorrowAcquireFence() >= -1);
  }
  bool ReadyForFrame() const override {
    return !startup_pending_ && video_cleanup_ == 0;
  }
  std::unique_ptr<Prepared> Prepare(uint64_t submission,
                                    const Frame& f) override {
    if (!Supports(f))
      return nullptr;
    auto batch = std::make_unique<Batch>();
    batch->submission = submission;
    batch->count = f.video ? 2 : 1;
    for (unsigned i = 0; i < batch->count; ++i) {
      const auto& plane = i ? *f.video : f.primary;
      auto& item = batch->items[i];
      item.buffer = static_cast<OH_NativeBuffer*>(plane.lease->native_buffer());
      OH_NativeBuffer_Config config = {};
      OH_NativeBuffer_GetConfig(item.buffer, &config);
      const int format = plane.format == Format::kP010
          ? NATIVEBUFFER_PIXEL_FMT_YCBCR_P010
          : plane.format == Format::kNv12 ? NATIVEBUFFER_PIXEL_FMT_YCBCR_420_SP
          : plane.format == Format::kBgra8 ? NATIVEBUFFER_PIXEL_FMT_BGRA_8888
                                          : NATIVEBUFFER_PIXEL_FMT_RGBA_8888;
      if (config.width != static_cast<int>(plane.width) ||
          config.height != static_cast<int>(plane.height) ||
          config.format != format)
        return nullptr;
      const int acquire = plane.lease->BorrowAcquireFence();
      if (acquire >= 0) {
        item.acquire.fd = fcntl(acquire, F_DUPFD_CLOEXEC, 0);
        if (item.acquire.fd < 0)
          return nullptr;
      }
      item.source = plane.source;
      item.destination = plane.destination;
      // Validate before emitting any commands, including extreme zoom/crops.
      if (i && (plane.format == Format::kRgba8 ||
                plane.format == Format::kBgra8) &&
          !RgbVideoFitsPrimary(
              {f.primary.destination.x, f.primary.destination.y,
               f.primary.destination.width, f.primary.destination.height},
              {plane.destination.x, plane.destination.y,
               plane.destination.width, plane.destination.height},
              {plane.source.x, plane.source.y, plane.source.width,
               plane.source.height}, plane.width, plane.height))
        return nullptr;
      if (plane.reuse)
        continue;
      item.slot = std::make_unique<Slot>();
      auto& slot = *item.slot;
      slot.submission = submission;
      slot.plane = i;
      slot.allocation = plane.lease->allocation_id();
      slot.sequence = OH_NativeBuffer_GetSeqNum(item.buffer);
      slot.node = nodes_[i].object;
      slot.context = context_.object;
      slot.api = &api_;
      slot.callback = [ptr = &slot](int fence) {
        int expected = -2;
        if (!ptr->fence.compare_exchange_strong(expected, fence) && fence >= 0)
          close(fence);
      };
      pthread_mutex_lock(&route_mutex);
      const bool available = !routes.count(slot.sequence);
      pthread_mutex_unlock(&route_mutex);
      if (!available || (i && batch->items[0].slot &&
                         slot.sequence == batch->items[0].slot->sequence))
        return nullptr;
    }
    return batch;
  }
  void Commit(std::unique_ptr<Prepared> prepared) override {
    if (!OnOwner())
      std::abort();
    auto* batch = static_cast<Batch*>(prepared.get());
    // All bridges run on the same owning UI sequence. Install all routes before
    // the first command can be observed by an implicit UI flush.
    pthread_mutex_lock(&route_mutex);
    for (unsigned i = 0; i < batch->count; ++i) {
      auto& item = batch->items[i];
      if (!item.slot)
        continue;
      if (routes.count(item.slot->sequence))
        std::abort();
      routes.emplace(item.slot->sequence, item.slot.get());
    }
    pthread_mutex_unlock(&route_mutex);
    for (unsigned i = 0; i < batch->count; ++i) {
      auto& item = batch->items[i];
      auto* node = nodes_[i].object;
      const auto& d = item.destination;
      const auto& s = item.source;
      if (i == 0)
        primary_destination_ = d;
      auto bounds = d;
      auto source = s;
      auto destination = d;
      if (i) {
        OH_NativeBuffer_Config config{};
        OH_NativeBuffer_GetConfig(item.buffer, &config);
        const bool rgb = config.format == NATIVEBUFFER_PIXEL_FMT_RGBA_8888 ||
                         config.format == NATIVEBUFFER_PIXEL_FMT_BGRA_8888;
        if (rgb) {
          const auto child = RgbVideoChildGeometry(
              {d.x, d.y, d.width, d.height}, {s.x, s.y, s.width, s.height},
              config.width, config.height);
          if (!child)
            std::abort();  // Prepare validated this immutable geometry.
          api_.bounds(video_clip_.object, d.x, d.y, d.width, d.height);
          api_.frame(video_clip_.object, d.x, d.y, d.width, d.height);
          bounds = {child->x, child->y, child->width, child->height};
          source = {0, 0, float(config.width), float(config.height)};
          destination = {d.x + bounds.x, d.y + bounds.y, bounds.width,
                         bounds.height};
        } else {
          // Native YUV already honors delegate cropping; keep its existing
          // global geometry under an origin-aligned, full-primary parent.
          const auto& root = batch->items[0].destination;
          api_.bounds(video_clip_.object, 0, 0, root.x + root.width,
                      root.y + root.height);
          api_.frame(video_clip_.object, 0, 0, root.x + root.width,
                     root.y + root.height);
        }
      }
      api_.bounds(node, bounds.x, bounds.y, bounds.width, bounds.height);
      api_.frame(node, bounds.x, bounds.y, bounds.width, bounds.height);
      api_.destination(node, destination.x, destination.y, destination.width,
                       destination.height);
      api_.source(node, source.x, source.y, source.width, source.height);
      // Sibling order inside the owned webpage host, not window-global z.
      api_.z(node, i ? 0.f : 1.f);
      api_.visible(node, true);
      if (!item.slot)
        continue;  // Same content, geometry-only transaction.
      void* buffer = item.buffer;
      OH_NativeBuffer_Reference(item.buffer);
      api_.set_buffer(node, &buffer, &item.acquire.fd, &item.slot->callback);
      OH_NativeBuffer_Unreference(item.buffer);
      slots_.push_back(std::move(item.slot));
    }
    if (batch->count == 1) {
      api_.visible(nodes_[1].object, false);
      if (video_visible_)
        video_cleanup_ = 1;
    }
    video_visible_ = batch->count == 2;
    last_submission_ = batch->submission;
    ConfirmedFlush();
  }
  std::vector<Release> PollReleases() override {
    if (!OnOwner())
      std::abort();
    if (startup_pending_ && Acknowledged())
      startup_pending_ = false;
    std::vector<Release> result;
    pthread_mutex_lock(&route_mutex);
    for (auto it = slots_.begin(); it != slots_.end();) {
      auto& slot = **it;
      if (slot.server_returned &&
          Ready(slot.fence.load(std::memory_order_acquire))) {
        result.push_back(
            {slot.submission, slot.allocation, slot.plane, true, true});
        routes.erase(slot.sequence);
        it = slots_.erase(it);
      } else
        ++it;
    }
    pthread_mutex_unlock(&route_mutex);
    const auto now = std::chrono::steady_clock::now();
    if (slots_.size() >= 4 &&
        now - last_backlog_log_ >= std::chrono::seconds(2)) {
      last_backlog_log_ = now;
      pthread_mutex_lock(&route_mutex);
      for (const auto& slot : slots_)
        BacklogLog(
            "held seq=%{public}u plane=%{public}u returned=%{public}d "
            "fence=%{public}d ready=%{public}d cleanup=%{public}u",
            slot->sequence, slot->plane, slot->server_returned,
            slot->fence.load(), Ready(slot->fence.load()), video_cleanup_);
      pthread_mutex_unlock(&route_mutex);
    }
    if (!draining_ && video_cleanup_ == 1 && Acknowledged()) {
      // The no-hole primary has reached RS before the retained video is
      // cleaned. Block new frames until this queue epoch has fully drained.
      api_.clean(nodes_[1].object, true);
      Flush();
      Background(nodes_[1]);
      ConfirmedFlush();
      video_cleanup_ = 2;
    }
    if (!draining_ && video_cleanup_ == 2 && !drain_failed_ && Acknowledged()) {
      bool pending = false;
      for (const auto& slot : slots_)
        pending |= slot->plane == 1;
      if (!pending) {
        // GoBackground drains the old producer epoch and marks its producer
        // cache dirty. That state prevents the sequence/fence listener from
        // acquiring released buffers in the reused delegate queue. Retire
        // every old read before replacing the node, then use a fresh queue.
        SystemShared retired = nodes_[1];
        nodes_[1] = {};
        if (!CreateLayer(1)) {
          api_.Drop(nodes_[1]);
          nodes_[1] = retired;
          drain_failed_ = true;
          return result;
        }
        api_.remove_child(video_clip_.object, &retired);
        api_.Drop(retired);
        api_.add_child(video_clip_.object, &nodes_[1], -1);
        ConfirmedFlush();
        video_cleanup_ = 3;
      }
    }
    if (!draining_ && video_cleanup_ == 3 && Acknowledged())
      video_cleanup_ = 0;
    return result;
  }
  bool CommandsProcessed(uint64_t submission) const override {
    return submission == last_submission_ && Acknowledged();
  }
  void BeginDrain() override {
    if (!OnOwner() || draining_)
      std::abort();
    draining_ = true;
    for (unsigned i = 0; i < 2; ++i) {
      auto& node = nodes_[i];
      api_.visible(node.object, false);
      api_.remove_child(i ? video_clip_.object : parent_.object, &node);
      api_.clean(node.object, true);
    }
    api_.remove_child(parent_.object, &video_clip_);
    attached_ = false;
    Flush();
    for (auto& node : nodes_)
      Background(node);
    ConfirmedFlush();
  }
  bool DrainCommandsProcessed() const override {
    return draining_ && !drain_failed_ && Acknowledged();
  }
  void DestroyLayers() override {
    if (!OnOwner() || attached_ || !slots_.empty())
      std::abort();
    for (auto& node : nodes_)
      api_.Drop(node);
    api_.Drop(video_clip_);
    Flush();
    destroyed_ = true;
  }

 private:
  bool CreateLayer(unsigned i) {
    alignas(8) uint8_t config[88] = {};
    const char* name = i ? "LumieVideoLayer" : "LumieWebLayer";
    const size_t n = std::strlen(name);
    config[0] = static_cast<uint8_t>(n << 1);
    std::memcpy(config + 1, name, n);
    config[48] = 1;
    config[72] = 1;
    OhosRsSret3(&nodes_[i], api_.create, reinterpret_cast<uintptr_t>(config), 0,
                reinterpret_cast<uintptr_t>(&context_));
    if (!nodes_[i].object)
      return false;
    api_.hardware(nodes_[i].object, true, 0, true);
    if (!api_.delegate(nodes_[i].object, true))
      return false;
    api_.gravity(nodes_[i].object, 5);
    api_.visible(nodes_[i].object, false);
    return true;
  }
  void Background(const SystemShared& node) {
    void* surface = nullptr;
    OhosRsSret3(&surface, api_.get_surface,
                reinterpret_cast<uintptr_t>(node.object), 0, 0);
    if (!surface) {
      drain_failed_ = true;
      return;
    }
    if (api_.background(surface) != 0)
      drain_failed_ = true;
    api_.surface_drop(surface, &surface);
  }
  bool OnOwner() const { return pthread_equal(owner_, pthread_self()); }
  bool Acknowledged() const {
    return !drain_failed_ && completion_ && completion_->Complete();
  }
  void ConfirmedFlush() {
    // Reapplying the owned primary rectangle always emits a legal node command,
    // even after GoBackground or for an otherwise empty drain transaction.
    // No SurfaceTransactionListener is registered: ordinary ArkUI updates must
    // never acquire the delegate-only sender identity.
    const auto& d = primary_destination_;
    api_.destination(nodes_[0].object, d.x, d.y, d.width, d.height);
    uint64_t handler_timestamp = 0;
    std::memcpy(&handler_timestamp,
                static_cast<char*>(handler_) + kRsHandlerTimestampOffset,
                sizeof(handler_timestamp));
    timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
      drain_failed_ = true;
      return;
    }
    const uint64_t timestamp = NextCompletionTimestamp(
        static_cast<uint64_t>(now.tv_sec) * 1000000000u + now.tv_nsec,
        handler_timestamp, completion_->expected.load());
    if (!timestamp) {
      drain_failed_ = true;
      return;
    }
    auto state = completion_;
    state->expected.store(timestamp, std::memory_order_release);
    const std::function<void()> callback = [state, timestamp] {
      state->Acknowledge(timestamp);
    };
    // Register before flushing, with a unique key in the existing context.
    // A callback confirms command processing only; pixel leases still require
    // their real buffer-release callback and a signaled release fence.
    if (!api_.transaction_callback(render_interface_, context_token_, timestamp,
                                   &callback)) {
      drain_failed_ = true;
      BacklogLog("transaction completion registration failed");
      return;
    }
    uint64_t empty_string[3] = {};
    api_.flush(handler_, timestamp, empty_string, false, 0);
    api_.vsync(context_.object);
  }
  void Flush() {
    uint64_t empty_string[3] = {};
    api_.flush(handler_, 0, empty_string, false, 0);
    api_.vsync(context_.object);
  }
  Api api_;
  pthread_t owner_{};
  SystemShared context_, parent_, nodes_[2], video_clip_;
  void* handler_ = nullptr;
  void* render_interface_ = nullptr;
  uint64_t context_token_ = 0;
  Rect primary_destination_{};
  bool attached_ = false, startup_pending_ = false;
  bool draining_ = false, destroyed_ = false, drain_failed_ = false;
  bool video_visible_ = false;
  unsigned video_cleanup_ = 0;
  std::chrono::steady_clock::time_point last_backlog_log_{};
  uint64_t last_submission_ = 0;
  std::shared_ptr<TransactionCompletion> completion_;
  std::vector<std::unique_ptr<Slot>> slots_;
};
}  // namespace

uint64_t ResolveOwnedRenderNode(uint64_t token, void* node) {
  if (!node || (token >> 32) != static_cast<uint64_t>(gettid()))
    return 0;
  Api api;
  if (!api.Load()) {
    if (api.surface)
      dlclose(api.surface);
    if (api.rs)
      dlclose(api.rs);
    return 0;
  }
  SystemShared context, found, owned;
  void* impl = nullptr;
  std::memcpy(&impl, node, sizeof(impl));
  uint32_t id = 0;
  if (impl) {
    std::memcpy(&owned, impl, sizeof(owned));
    std::memcpy(&id, static_cast<char*>(impl) + 16, sizeof(id));
  }
  const uint64_t full_id = (static_cast<uint64_t>(getpid()) << 32) | id;
  OhosRsSret3(&context, api.context_get,
              reinterpret_cast<uintptr_t>(api.manager), token, 0);
  if (context.object && owned.object && owned.control)
    OhosRsSret3(&found, api.node_get,
                reinterpret_cast<uintptr_t>(context.object) + 40, full_id, 0);
  const bool match = found.object && found.object == owned.object &&
                     found.control == owned.control;
  TRACE(
      "render_parent id=%{public}llu context=%{public}d owned=%{public}d "
      "found=%{public}d match=%{public}d",
      static_cast<unsigned long long>(full_id), !!context.object,
      !!owned.object, !!found.object, match);
  api.Drop(found);
  api.Drop(context);
  if (api.surface)
    dlclose(api.surface);
  if (api.rs)
    dlclose(api.rs);
  return match ? full_id : 0;
}
std::unique_ptr<Backend> CreateRsOverlayBackend(uint64_t token,
                                                uint64_t parent) {
  auto backend = std::make_unique<RsBackend>();
  if (!backend->Initialize(token, parent))
    return nullptr;
  return backend;
}
}  // namespace ui::ohos_overlay
