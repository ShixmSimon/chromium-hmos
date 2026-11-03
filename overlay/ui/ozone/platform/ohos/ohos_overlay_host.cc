// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
// Standalone NDK (__h ABI), never linked as Chromium __Cr C++ objects.
#include <IPCKit/ipc_cparcel.h>
#include <arkui/native_interface.h>
#include <arkui/native_node.h>
#include <arkui/native_render.h>
#include <dlfcn.h>
#include <native_buffer/native_buffer.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <uv.h>
#include <cerrno>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include "ohos_overlay_transport.h"
#include "ohos_rs_overlay_backend.h"

namespace ui::ohos_overlay {
namespace {
struct Video {
  void* native;
  uint32_t flags;
  void* context;
  void (*release)(void*);
  ~Video() { release(context); }
};
std::mutex mutex;
std::map<uint64_t, std::shared_ptr<Video>> videos;
struct Buffer {
  OH_NativeBuffer* native = nullptr;
  std::shared_ptr<Video> video;
  uint64_t content = 0;
  ~Buffer() {
    if (native)
      OH_NativeBuffer_Unreference(native);
  }
};
struct Host;
std::map<std::string, Host*>
    hosts;  // UI-owned objects; lookup/connect under mutex.
struct Host {
  std::string id;
  void* content_owner = nullptr;
  void (*release_owner)(void*) = nullptr;
  ~Host() {
    if (release_owner)
      release_owner(content_owner);
  }

  ArkUI_NodeContentHandle content = nullptr;
  ArkUI_NodeHandle node = nullptr;
  ArkUI_RenderNodeHandle render = nullptr;
  ArkUI_NativeNodeAPI_1* api = nullptr;
  uv_timer_t timer{};
  OhosOverlayHost* session = nullptr;
  int pending_socket = -1;  // protected by mutex
  bool detached = false;    // protected by mutex
  bool connected = false;   // protected by mutex
  int socket = -1;
  bool closing = false;
  bool processed = true;
  uint64_t sequence = 0;
  std::unique_ptr<WireMessage> pending;
  std::deque<std::unique_ptr<WireMessage>> outgoing;
  std::shared_ptr<Buffer> current[2];
  uint64_t video_token = 0;
  struct Lease {
    Host* host;
    uint64_t sequence;
    unsigned plane;
    std::shared_ptr<Buffer> buffer;
  };
  void Reply(WireOp op, uint64_t seq, unsigned plane = 0, uint32_t result = 0) {
    if (socket < 0)
      return;
    if (outgoing.size() >= 64) {
      Disconnect();
      return;
    }
    auto msg = std::make_unique<WireMessage>();
    msg->packet.operation = op;
    msg->packet.sequence = seq;
    msg->packet.plane = plane;
    msg->packet.result = result;
    outgoing.push_back(std::move(msg));
  }
  static void Release(void* data) {
    std::unique_ptr<Lease> lease(static_cast<Lease*>(data));
    lease->host->Reply(WireOp::kReleased, lease->sequence, lease->plane);
  }
  void Disconnect() {
    if (socket >= 0)
      close(socket);
    socket = -1;
    closing = true;
    outgoing.clear();
    pending.reset();
    current[0].reset();
    current[1].reset();
    // Close is performed by Tick, outside Session::Poll's lease destructor.
  }
  std::shared_ptr<Buffer> ImportPrimary(const WireMessage& msg) {
    const auto& p = msg.packet;
    if (p.primary.reuse)
      return current[0] && current[0]->content == p.primary.content ? current[0]
                                                                    : nullptr;
    using Reader = int32_t (*)(OHIPCParcel*, OH_NativeBuffer**);
    auto reader = reinterpret_cast<Reader>(
        dlsym(RTLD_DEFAULT, "OH_NativeBuffer_ReadFromParcel"));
    using Parcel =
        std::unique_ptr<OHIPCParcel, decltype(&OH_IPCParcel_Destroy)>;
    Parcel parcel(OH_IPCParcel_Create(), &OH_IPCParcel_Destroy);
    if (!reader || !parcel ||
        OH_IPCParcel_WriteBuffer(
            parcel.get(), reinterpret_cast<const uint8_t*>(p.prefix.data()),
            56))
      return nullptr;
    for (unsigned i = 0; i < p.fd_count; ++i)
      if (OH_IPCParcel_WriteFileDescriptor(parcel.get(), msg.fds[i]))
        return nullptr;
    if (p.tail_bytes &&
        OH_IPCParcel_WriteBuffer(parcel.get(), p.tail.data(), p.tail_bytes))
      return nullptr;
    auto result = std::make_shared<Buffer>();
    if (reader(parcel.get(), &result->native) || !result->native)
      return nullptr;
    OH_NativeBuffer_Config config{};
    OH_NativeBuffer_GetConfig(result->native, &config);
    if (config.width != static_cast<int>(p.prefix[4]) ||
        config.height != static_cast<int>(p.prefix[6]) ||
        config.format != static_cast<int>(p.prefix[8]) ||
        OH_NativeBuffer_SetColorSpace(result->native, OH_COLORSPACE_SRGB_FULL))
      return nullptr;
    result->content = p.primary.content;
    return result;
  }
  std::shared_ptr<Buffer> GetVideo(const WirePacket& p) {
    if (p.video.reuse)
      return video_token == p.video_token && current[1] &&
                     current[1]->content == p.video.content
                 ? current[1]
                 : nullptr;
    std::shared_ptr<Video> video;
    {
      std::lock_guard<std::mutex> lock(mutex);
      const auto it = videos.find(p.video_token);
      if (it != videos.end())
        video = it->second;
    }
    if (!video)
      return nullptr;
    auto buffer = std::make_shared<Buffer>();
    auto* native = static_cast<OH_NativeBuffer*>(video->native);
    if (OH_NativeBuffer_Reference(native))
      return nullptr;
    buffer->native = native;
    buffer->video = std::move(video);
    buffer->content = p.video.content;
    return buffer;
  }
  void Submit() {
    const auto& p = pending->packet;
    auto primary = ImportPrimary(*pending);
    auto video = p.has_video ? GetVideo(p) : nullptr;
    OhosOverlayResult result = OHOS_OVERLAY_UNSUPPORTED;
    if (primary && (!p.has_video || video)) {
      OhosOverlayFrame frame{};
      frame.version = OHOS_OVERLAY_BRIDGE_VERSION;
      frame.has_video = frame.primary_has_video_hole = p.has_video;
      auto first = std::make_unique<Lease>(Lease{this, p.sequence, 0, primary});
      auto second =
          video ? std::make_unique<Lease>(Lease{this, p.sequence, 1, video})
                : nullptr;
      unsigned fd = p.fd_count;
      const auto convert = [&](OhosOverlayPlane& dst, const WirePlane& src,
                               Lease* lease) {
        dst.native_buffer = lease->buffer->native;
        dst.acquire_fence = src.has_fence ? pending->fds[fd++] : -1;
        dst.flags = (src.reuse ? OHOS_OVERLAY_REUSE_CONTENT : 0) |
                    (lease->buffer->video ? lease->buffer->video->flags
                                          : OHOS_OVERLAY_SDR_TAGS_PRISTINE);
        dst.source = src.source;
        dst.destination = src.destination;
        dst.lease_context = lease;
        dst.release_lease = &Release;
      };
      convert(frame.primary, p.primary, first.get());
      if (video)
        convert(frame.video, p.video, second.get());
      // Parent geometry is viewport-relative, never window-global. ArkUI's
      // ContentSlot owns its position, clip, visibility and shell stacking.
      OH_ArkUI_RenderNodeUtils_SetSize(
          render,
          static_cast<int32_t>(p.primary.destination.x +
                               p.primary.destination.width),
          static_cast<int32_t>(p.primary.destination.y +
                               p.primary.destination.height));
      result = OhosOverlaySubmit(session, &frame);
      if (result == OHOS_OVERLAY_ACCEPTED) {
        first.release();
        second.release();
        current[0] = std::move(primary);
        current[1] = std::move(video);
        video_token = p.video_token;
        processed = false;
        Reply(WireOp::kAccepted, p.sequence);
      }
    }
    if (result == OHOS_OVERLAY_BUSY)
      return;  // Retain descriptors; retry after releases.
    if (result != OHOS_OVERLAY_ACCEPTED)
      Reply(WireOp::kRejected, p.sequence, 0, result);
    pending.reset();
  }
  void Tick() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (detached)
        closing = true;
      if (pending_socket >= 0) {
        socket = pending_socket;
        pending_socket = -1;
      }
    }
    if (socket >= 0 && !closing) {
      pollfd peer{socket, POLLIN, 0};
      if (poll(&peer, 1, 0) > 0 &&
          (peer.revents & (POLLHUP | POLLERR | POLLNVAL)))
        Disconnect();
    }
    if (closing) {
      current[0].reset();
      current[1].reset();
      pending.reset();
      OhosOverlayClose(session);
    }
    OhosOverlayStatus status{};
    OhosOverlayPoll(session, &status);
    if (!processed && status.commands_processed) {
      Reply(WireOp::kProcessed, sequence);
      processed = true;
    }
    if (closing && status.state == 3) {
      if (socket >= 0)
        close(socket);
      socket = -1;
      OhosOverlayDestroy(session);
      session = nullptr;
      {
        std::lock_guard<std::mutex> lock(mutex);
        connected = false;
        // A failed GPU channel is not automatically retried: the next output
        // device falls back to the existing XComponent presenter.
        hosts.erase(id);
      }
      OH_ArkUI_NodeContent_RemoveNode(content, node);
      OH_ArkUI_RenderNodeUtils_RemoveRenderNode(node, render);
      OH_ArkUI_RenderNodeUtils_DisposeNode(render);
      api->disposeNode(node);
      uv_timer_stop(&timer);
      uv_close(reinterpret_cast<uv_handle_t*>(&timer),
               [](uv_handle_t* h) { delete static_cast<Host*>(h->data); });
      return;
    }
    if (socket < 0 || closing)
      return;
    while (!outgoing.empty()) {
      if (!SendWireMessage(socket, *outgoing.front())) {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
          Disconnect();
        return;
      }
      outgoing.pop_front();
    }
    if (!processed)
      return;
    if (!pending) {
      auto msg = std::make_unique<WireMessage>();
      if (!ReceiveWireMessage(socket, msg.get())) {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
          Disconnect();
        return;
      }
      if (msg->packet.sequence <= sequence) {
        Disconnect();
        return;
      }
      sequence = msg->packet.sequence;
      if (msg->packet.operation == WireOp::kClose) {
        closing = true;
        return;
      }
      if (msg->packet.operation != WireOp::kFrame) {
        Disconnect();
        return;
      }
      pending = std::move(msg);
    }
    Submit();
  }
};
}  // namespace
}  // namespace ui::ohos_overlay
using namespace ui::ohos_overlay;
extern "C" int OhosOverlayRegisterVideo(uint64_t token,
                                        void* native,
                                        uint32_t flags,
                                        void* context,
                                        void (*release)(void*)) {
  if (!token || !native || !release ||
      (flags & ~OHOS_OVERLAY_SDR_TAGS_PRISTINE))
    return 0;
  std::lock_guard<std::mutex> lock(mutex);
  if (hosts.empty() || videos.size() >= 64 || videos.count(token))
    return 0;
  auto video =
      std::shared_ptr<Video>(new Video{native, flags, context, release});
  videos.emplace(token, std::move(video));
  return 1;
}
extern "C" void OhosOverlayUnregisterVideo(uint64_t token) {
  std::shared_ptr<Video> retired;
  {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = videos.find(token);
    if (it == videos.end())
      return;
    retired = std::move(it->second);
    videos.erase(it);
  }
}
extern "C" int OhosOverlayConnect(const char* id) {
  if (!id)
    return -1;
  std::lock_guard<std::mutex> lock(mutex);
  auto it = hosts.find(id);
  if (it == hosts.end() || it->second->connected || it->second->detached)
    return -1;
  int fds[2];
  if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0,
                 fds))
    return -1;
  it->second->pending_socket = fds[0];
  it->second->connected = true;
  return fds[1];
}
extern "C" int OhosOverlayAttachContent(uint32_t version,
                                        const char* id,
                                        void* content,
                                        void* loop,
                                        void* owner,
                                        void (*release_owner)(void*)) {
  if (version != OHOS_OVERLAY_BRIDGE_VERSION || !id || !*id || !content ||
      !loop)
    return 0;
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (hosts.count(id))
      return 0;
  }
  auto host = std::make_unique<Host>();
  host->id = id;
  host->content = static_cast<ArkUI_NodeContentHandle>(content);
  OH_ArkUI_GetModuleInterface(ARKUI_NATIVE_NODE, ArkUI_NativeNodeAPI_1,
                              host->api);
  if (!host->api)
    return 0;
  host->node = host->api->createNode(ARKUI_NODE_CUSTOM);
  host->render = OH_ArkUI_RenderNodeUtils_CreateNode();
  bool attached = false;
  const auto cleanup = [&] {
    if (attached)
      OH_ArkUI_NodeContent_RemoveNode(host->content, host->node);
    if (host->node && host->render)
      OH_ArkUI_RenderNodeUtils_RemoveRenderNode(host->node, host->render);
    if (host->render)
      OH_ArkUI_RenderNodeUtils_DisposeNode(host->render);
    if (host->node)
      host->api->disposeNode(host->node);
  };
  if (!host->node || !host->render ||
      OH_ArkUI_RenderNodeUtils_AddRenderNode(host->node, host->render) ||
      OH_ArkUI_NodeContent_AddNode(host->content, host->node)) {
    cleanup();
    return 0;
  }
  attached = true;
  ArkUI_NumberValue percent{.f32 = 1.f};
  ArkUI_AttributeItem item{&percent, 1, nullptr, nullptr};
  host->api->setAttribute(host->node, NODE_WIDTH_PERCENT, &item);
  host->api->setAttribute(host->node, NODE_HEIGHT_PERCENT, &item);
  ArkUI_NumberValue clip{.i32 = 1};
  item.value = &clip;
  host->api->setAttribute(host->node, NODE_CLIP, &item);
  OH_ArkUI_RenderNodeUtils_SetClipToBounds(host->render, 1);
  // Establish the polling owner before creating asynchronous RS resources.
  if (uv_timer_init(static_cast<uv_loop_t*>(loop), &host->timer)) {
    cleanup();
    return 0;
  }
  host->timer.data = host.get();
  const uint64_t token = (static_cast<uint64_t>(gettid()) << 32) | 1;
  const uint64_t parent = ResolveOwnedRenderNode(token, host->render);
  host->session = parent ? OhosOverlayCreate(version, token, parent) : nullptr;
  if (!host->session) {
    cleanup();
    uv_close(reinterpret_cast<uv_handle_t*>(&host->timer),
             [](uv_handle_t* h) { delete static_cast<Host*>(h->data); });
    host.release();
    return 0;
  }
  host->timer.data = host.get();
  uv_timer_start(
      &host->timer, [](uv_timer_t* t) { static_cast<Host*>(t->data)->Tick(); },
      0, 4);
  {
    std::lock_guard<std::mutex> lock(mutex);
    hosts.emplace(id, host.get());
  }
  host->content_owner = owner;
  host->release_owner = release_owner;
  host.release();
  return 1;
}
extern "C" void OhosOverlayDetachContent(const char* id) {
  if (!id)
    return;
  std::lock_guard<std::mutex> lock(mutex);
  auto it = hosts.find(id);
  if (it != hosts.end())
    it->second->detached = true;
}
